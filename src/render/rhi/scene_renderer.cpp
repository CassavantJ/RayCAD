// The viewport renderer on Qt RHI (task T-014). See scene_renderer.h. Design from spike S1 (spikes/s1-viewport):
// one vertex and index buffer per body, per-face ranges kept in the vertex data (face id attribute) for T-015,
// edges in their own line buffer, faces pushed back with a depth bias so their edges stay visible.
#include "scene_renderer.h"

#include "backdrop_pass.h"
#include "pick_pass.h"

#include "../highlight.h"
#include "../section.h"

#include "../tier_internal.h"

#include <QtCore/QByteArray>
#include <QtGui/QColor>
#include <QtGui/QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace ray::render::rhi {

namespace {

// std140 blocks; must match src/render/shaders/frame.glsl.
struct FrameBlock {
    float viewProj[16];
    float view[16];
    float lightDir[4];
    float viewport[4];
    float bgTop[4];
    float bgBottom[4];
    float misc[4];
    float section[4];
    // T-023
    float analysis[4];
    float draftPull[4];
    float draftPositive[4];
    float draftNegative[4];
    float draftVertical[4];
};
struct ItemBlock {
    float model[16];
    float normalMatrix[16];
    float faceColor[4];
    float edgeColor[4];
    float line[4];
    float ids[4];
};
static_assert(sizeof(FrameBlock) == 304);
static_assert(sizeof(ItemBlock) == 192);

constexpr quint32 kMeshStride = 28; // position, normal, face id
constexpr quint32 kThinStride = 16; // position, edge id
constexpr quint32 kWideStride = 28; // p0, p1, edge id
constexpr quint32 kCapStride = 12;  // position (T-023)
constexpr quint32 kCapVertices = 6; // the cap quad as two triangles
constexpr double kPi = 3.14159265358979323846;
constexpr double kHatchPeriodPx = 10.0; // section caps' hatch (T-023), logical pixels before snapping
constexpr int kFaceDepthBias = 2;       // faces sit behind their edges (S1)
constexpr float kFaceSlopeDepthBias = 1.5f;
// Theme metrics used here, with the value used when a theme has none.
constexpr const char* kMetricThinLine = "line.thin.px";
constexpr double kDefaultThinLinePx = 1.5;
constexpr const char* kMetricDash = "line.dash.px";
constexpr double kDefaultDashPx = 8.0;
// Highlight sizes (T-015). Without the metric, a highlighted edge is this many thin lines wide, and a vertex marker
// this many highlighted edges.
constexpr const char* kMetricHighlightLine = "line.highlight.px";
constexpr double kHighlightLineScale = 2.0;
constexpr const char* kMetricVertexMarker = "point.highlight.px";
constexpr double kVertexMarkerScale = 2.5;

QRhiGraphicsPipeline::TargetBlend alphaBlend()
{
    QRhiGraphicsPipeline::TargetBlend b;
    b.enable = true;
    b.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    b.srcAlpha = QRhiGraphicsPipeline::One;
    b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return b;
}

double metric(const Theme& theme, const char* name, double fallback)
{
    const auto it = theme.metrics.find(name);
    return it != theme.metrics.end() && it->second > 0 ? it->second : fallback;
}

void putU32(char* dst, std::uint32_t v)
{
    std::memcpy(dst, &v, 4);
}

void putFloats(char* dst, const float* src, int count)
{
    std::memcpy(dst, src, static_cast<std::size_t>(count) * sizeof(float));
}

void copyMatrix(float dst[16], const QMatrix4x4& m)
{
    std::memcpy(dst, m.constData(), 16 * sizeof(float));
}

// Loaded once per process; a QShader holds no GPU resource.
struct Shaders {
    QShader backgroundVs, backgroundFs, meshVs, meshFs, lineVs, lineFs, wideVs, wideFs;
    QShader capVs, capFs; // T-023: section caps (optional: without them nothing is capped)
    bool ok = false, capOk = false;
};

const Shaders& shaders()
{
    static const Shaders s = [] {
        Shaders out;
        out.backgroundVs = loadShader("background.vert");
        out.backgroundFs = loadShader("background.frag");
        out.meshVs = loadShader("mesh.vert");
        out.meshFs = loadShader("mesh.frag");
        out.lineVs = loadShader("line.vert");
        out.lineFs = loadShader("line.frag");
        out.wideVs = loadShader("wideline.vert");
        out.wideFs = loadShader("wideline.frag");
        out.capVs = loadShader("cap.vert");
        out.capFs = loadShader("cap.frag");
        out.ok = out.backgroundVs.isValid() && out.backgroundFs.isValid() && out.meshVs.isValid() &&
                 out.meshFs.isValid() && out.lineVs.isValid() && out.lineFs.isValid() && out.wideVs.isValid() &&
                 out.wideFs.isValid();
        out.capOk = out.capVs.isValid() && out.capFs.isValid();
        return out;
    }();
    return s;
}

} // namespace

QRhiVertexInputLayout capVertexLayout()
{
    QRhiVertexInputLayout cap;
    cap.setBindings({QRhiVertexInputBinding(kCapStride)});
    cap.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0)});
    return cap;
}

void setCapStencilMarking(QRhiGraphicsPipeline& p, int colourTargets)
{
    // Section caps (T-023, section.h), step 1: both sides of the body's clipped faces flip stencil bit 0; nothing
    // else is written and nothing is depth tested.
    using P = QRhiGraphicsPipeline;
    p.setDepthTest(false);
    p.setDepthWrite(false);
    p.setCullMode(P::None);
    P::TargetBlend none;
    none.colorWrite = {};
    // (The iterator overload of setTargetBlends trips MSVC's C4702 inside Qt's overflow helpers.)
    if (colourTargets >= 3)
        p.setTargetBlends({none, none, none}); // the picking ID pass
    else
        p.setTargetBlends({none});
    P::StencilOpState op;
    op.failOp = P::Keep;
    op.depthFailOp = P::Keep;
    op.passOp = P::Invert;
    op.compareOp = P::Always;
    p.setStencilTest(true);
    p.setStencilFront(op);
    p.setStencilBack(op);
    p.setStencilReadMask(1);
    p.setStencilWriteMask(1);
}

void setCapStencilFill(QRhiGraphicsPipeline& p)
{
    // Step 2: the cap quad where bit 0 is set (reference 0, NotEqual), depth tested and written; the bit is cleared
    // wherever the quad lands, visible or not, so the next body starts from a clear stencil.
    using P = QRhiGraphicsPipeline;
    P::StencilOpState op;
    op.failOp = P::Keep;
    op.depthFailOp = P::StencilZero;
    op.passOp = P::StencilZero;
    op.compareOp = P::NotEqual;
    p.setStencilTest(true);
    p.setStencilFront(op);
    p.setStencilBack(op);
    p.setStencilReadMask(1);
    p.setStencilWriteMask(1);
    p.setFlags(p.flags() | P::UsesStencilRef);
}

QRhiVertexInputLayout meshVertexLayout()
{
    QRhiVertexInputLayout mesh;
    mesh.setBindings({QRhiVertexInputBinding(kMeshStride)});
    mesh.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
                        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 12),
                        QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::UNormByte4, 24)});
    return mesh;
}

QRhiVertexInputLayout thinEdgeVertexLayout()
{
    QRhiVertexInputLayout thin;
    thin.setBindings({QRhiVertexInputBinding(kThinStride)});
    thin.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
                        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::UNormByte4, 12)});
    return thin;
}

QRhiVertexInputLayout wideLineVertexLayout()
{
    QRhiVertexInputLayout wide;
    wide.setBindings({QRhiVertexInputBinding(2 * sizeof(float)),
                      QRhiVertexInputBinding(kWideStride, QRhiVertexInputBinding::PerInstance)});
    wide.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                        QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float3, 0),
                        QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float3, 12),
                        QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::UNormByte4, 24)});
    return wide;
}

struct SceneRenderer::PipelineSet {
    Owned<QRhiRenderPassDescriptor> rp;
    int samples = 1;
    Owned<QRhiGraphicsPipeline> background, meshOpaque, meshBlend, lineThin, lineWide;
    Owned<QRhiGraphicsPipeline> highlightFaces;      // T-015: mesh shaders, blended, on the faces' own depth
    Owned<QRhiGraphicsPipeline> capStencil, capFill; // T-023: section caps (section.h); null: no caps
    bool ok = false;
};

// One item's highlighted elements in one style (T-015).
struct SceneRenderer::HighlightGroup {
    std::size_t draw = 0; // index into draws_
    bool hover = false;
    std::vector<PickHit> hits;
    quint32 blockOffset = 0;       // ItemBlock with the highlight colours and edge width
    quint32 markerBlockOffset = 0; // ItemBlock with the vertex marker size
    quint32 firstMarker = 0, markerCount = 0;
};

struct SceneRenderer::Target {
    QSize size;
    int samples = 1;
    QRhiTexture* colour = nullptr;
    Owned<QRhiTexture> ownedColour; // capture targets own their texture; the main target does not
    Owned<QRhiRenderBuffer> msaa, depth;
    Owned<QRhiTextureRenderTarget> rt;
    PipelineSet* set = nullptr;
};

struct SceneRenderer::Capture {
    Target target;
    QRhiReadbackResult result;
    std::function<void(std::vector<std::byte>)> done;
    quint32 frameOffset = 0; // its FrameBlock in the uniform buffer
    std::atomic<bool> finished{false};
};

SceneRenderer::SceneRenderer()
{
    auto pick = std::make_unique<PickPass>(counter_);
    pick_ = pick.get();
    passes_.push_back(std::move(pick));
    auto backdrop = std::make_unique<BackdropPass>(counter_);
    backdrop_ = backdrop.get();
    passes_.push_back(std::move(backdrop));
}

std::uint64_t SceneRenderer::pickBufferRenders() const
{
    return pick_ ? pick_->idRenders() : 0;
}

std::uint64_t SceneRenderer::backdropBlurs() const
{
    return backdrop_ ? backdrop_->blurs() : 0;
}

IViewport::Backdrop SceneRenderer::backdrop() const
{
    return backdrop_ ? backdrop_->backdrop() : IViewport::Backdrop{nullptr, 0, 0, false, 0};
}

SceneRenderer::~SceneRenderer()
{
    releaseResources();
}

void SceneRenderer::setRhi(QRhi* rhi)
{
    if (rhi == rhi_)
        return;
    releaseResources();
    rhi_ = rhi;
}

void SceneRenderer::setTarget(QRhiTexture* colour)
{
    // The same texture object again (a host that calls initialize every frame) keeps the target. A new object is
    // recognised by its resource id, never by its address, which a freed texture's successor may reuse.
    if (colour && colour == colour_ && colour->globalResourceId() == colourId_ && main_ &&
        main_->size == colour->pixelSize())
        return;
    colour_ = colour;
    colourId_ = colour ? colour->globalResourceId() : 0;
    main_.reset(); // its render target refers to the old texture
    targetDirty_ = true;
}

void SceneRenderer::setSnapshot(detail::FrameSnapshot&& snapshot)
{
    // Captures and picks not yet answered stay queued; a picture change not yet drawn stays pending.
    std::vector<detail::CaptureRequest> pending = std::move(snap_.captures);
    std::vector<detail::PickRequest> pendingPicks = std::move(snap_.picks);
    needRedraw_ = needRedraw_ || snapshot.redraw;
    snap_ = std::move(snapshot);
    if (!pending.empty()) {
        pending.insert(pending.end(), std::make_move_iterator(snap_.captures.begin()),
                       std::make_move_iterator(snap_.captures.end()));
        snap_.captures = std::move(pending);
    }
    if (!pendingPicks.empty()) {
        pendingPicks.insert(pendingPicks.end(), std::make_move_iterator(snap_.picks.begin()),
                            std::make_move_iterator(snap_.picks.end()));
        snap_.picks = std::move(pendingPicks);
    }
}

void SceneRenderer::setSinks(std::shared_ptr<detail::Outbox> outbox, std::shared_ptr<detail::SharedStatus> status)
{
    outbox_ = std::move(outbox);
    status_ = std::move(status);
}

void SceneRenderer::addPass(std::unique_ptr<IFramePass> pass)
{
    if (!pass)
        return;
    const auto last = std::find_if(passes_.begin(), passes_.end(),
                                   [&](const std::unique_ptr<IFramePass>& p) { return p.get() == backdrop_; });
    passes_.insert(last, std::move(pass));
}

bool SceneRenderer::busy() const
{
    for (const auto& c : captures_) {
        if (!c->finished.load())
            return true;
    }
    for (const auto& p : passes_) {
        if (p->busy())
            return true;
    }
    return false;
}

void SceneRenderer::releaseResources()
{
    if (rhi_ && !rhi_->isRecordingFrame()) {
        bool waiting = false;
        for (const auto& c : captures_)
            waiting = waiting || !c->finished.load();
        if (waiting)
            rhi_->finish(); // completes the read-backs, so nothing calls into a destroyed Capture
    }
    for (auto& p : passes_)
        p->releaseResources();
    captures_.clear();
    main_.reset();
    pipelineSets_.clear();
    gpu_.clear();
    draws_.clear();
    highlights_.clear();
    markers_.reset();
    markerCapacity_ = 0;
    capQuad_.reset();
    capped_.clear();
    needRedraw_ = true;
    bindings_.reset();
    uniforms_.reset();
    uniformCapacity_ = 0;
    quad_.reset();
    corners_.reset();
    targetDirty_ = true;
}

int SceneRenderer::effectiveSamples(int wanted) const
{
    if (wanted <= 1 || !rhi_ || !rhi_->isFeatureSupported(QRhi::MultisampleRenderBuffer))
        return 1;
    int best = 1;
    for (const int s : rhi_->supportedSampleCounts()) {
        if (s <= wanted)
            best = std::max(best, s);
    }
    return best;
}

void SceneRenderer::ensureStatic()
{
    frameBlockSize_ = sizeof(FrameBlock);
    itemBlockSize_ = sizeof(ItemBlock);
}

SceneRenderer::PipelineSet* SceneRenderer::pipelinesFor(Target& target)
{
    std::unique_ptr<QRhiRenderPassDescriptor> candidate(target.rt->newCompatibleRenderPassDescriptor());
    for (auto& set : pipelineSets_) {
        if (set->samples == target.samples && set->rp->isCompatible(candidate.get()))
            return set.get();
    }
    auto set = std::make_unique<PipelineSet>();
    set->samples = target.samples;
    set->rp = own(counter_, candidate.release());

    const Shaders& sh = shaders();
    if (!sh.ok) {
        pipelineSets_.push_back(std::move(set));
        return pipelineSets_.back().get();
    }
    const auto make = [&](const QShader& vs, const QShader& fs, const QRhiVertexInputLayout& layout,
                          QRhiGraphicsPipeline::Topology topology, bool depthTest, bool depthWrite,
                          QRhiGraphicsPipeline::CompareOp depthOp, bool blend, bool bias) {
        Owned<QRhiGraphicsPipeline> p = own(counter_, rhi_->newGraphicsPipeline());
        p->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
        p->setVertexInputLayout(layout);
        p->setTopology(topology);
        p->setDepthTest(depthTest);
        p->setDepthWrite(depthWrite);
        p->setDepthOp(depthOp);
        p->setCullMode(QRhiGraphicsPipeline::None);
        if (blend)
            p->setTargetBlends({alphaBlend()});
        if (bias) {
            p->setDepthBias(kFaceDepthBias);
            p->setSlopeScaledDepthBias(kFaceSlopeDepthBias);
        }
        p->setSampleCount(set->samples);
        p->setShaderResourceBindings(bindings_.get());
        p->setRenderPassDescriptor(set->rp.get());
        if (!p->create())
            p.reset();
        return p;
    };

    QRhiVertexInputLayout background;
    background.setBindings({QRhiVertexInputBinding(2 * sizeof(float))});
    background.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0)});

    const QRhiVertexInputLayout mesh = meshVertexLayout();
    const QRhiVertexInputLayout thin = thinEdgeVertexLayout();
    const QRhiVertexInputLayout wide = wideLineVertexLayout();

    using P = QRhiGraphicsPipeline;
    set->background =
        make(sh.backgroundVs, sh.backgroundFs, background, P::Triangles, false, false, P::Always, false, false);
    set->meshOpaque = make(sh.meshVs, sh.meshFs, mesh, P::Triangles, true, true, P::Less, false, true);
    set->meshBlend = make(sh.meshVs, sh.meshFs, mesh, P::Triangles, true, false, P::Less, true, true);
    set->lineThin = make(sh.lineVs, sh.lineFs, thin, P::Lines, true, false, P::LessOrEqual, false, false);
    if (rhi_->isFeatureSupported(QRhi::Instancing))
        set->lineWide = make(sh.wideVs, sh.wideFs, wide, P::Triangles, true, false, P::LessOrEqual, true, false);
    // Same shaders and depth bias as the faces, so a highlight lands on exactly their depth and passes LessOrEqual.
    set->highlightFaces = make(sh.meshVs, sh.meshFs, mesh, P::Triangles, true, false, P::LessOrEqual, true, true);
    if (sh.capOk) {
        const auto finish = [&](Owned<QRhiGraphicsPipeline>& p) {
            p->setSampleCount(set->samples);
            p->setShaderResourceBindings(bindings_.get());
            p->setRenderPassDescriptor(set->rp.get());
            if (!p->create())
                p.reset();
        };
        set->capStencil = own(counter_, rhi_->newGraphicsPipeline());
        set->capStencil->setShaderStages(
            {{QRhiShaderStage::Vertex, sh.meshVs}, {QRhiShaderStage::Fragment, sh.meshFs}});
        set->capStencil->setVertexInputLayout(mesh);
        set->capStencil->setTopology(P::Triangles);
        setCapStencilMarking(*set->capStencil, 1);
        finish(set->capStencil);
        // Same depth bias as the faces, so edges lying in the plane stay on top of a cap.
        set->capFill = own(counter_, rhi_->newGraphicsPipeline());
        set->capFill->setShaderStages({{QRhiShaderStage::Vertex, sh.capVs}, {QRhiShaderStage::Fragment, sh.capFs}});
        set->capFill->setVertexInputLayout(capVertexLayout());
        set->capFill->setTopology(P::Triangles);
        set->capFill->setDepthTest(true);
        set->capFill->setDepthWrite(true);
        set->capFill->setDepthOp(P::Less);
        set->capFill->setCullMode(P::None);
        set->capFill->setDepthBias(kFaceDepthBias);
        set->capFill->setSlopeScaledDepthBias(kFaceSlopeDepthBias);
        setCapStencilFill(*set->capFill);
        finish(set->capFill);
        if (!set->capStencil || !set->capFill) {
            set->capStencil.reset();
            set->capFill.reset();
        }
    }
    set->ok = set->background && set->meshOpaque && set->meshBlend && set->lineThin;
    pipelineSets_.push_back(std::move(set));
    return pipelineSets_.back().get();
}

bool SceneRenderer::buildTarget(Target& target, QRhiTexture* colour, QSize size, int samples)
{
    target.rt.reset();
    target.msaa.reset();
    target.depth.reset();
    target.set = nullptr;
    target.colour = colour;
    target.size = size;
    target.samples = samples;
    QRhiColorAttachment attachment(colour);
    if (samples > 1) {
        target.msaa =
            own(counter_, rhi_->newRenderBuffer(QRhiRenderBuffer::Color, size, samples, {}, colour->format()));
        if (!target.msaa->create())
            return false;
        attachment = QRhiColorAttachment(target.msaa.get());
        attachment.setResolveTexture(colour);
    }
    target.depth = own(counter_, rhi_->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, samples));
    if (!target.depth->create())
        return false;
    QRhiTextureRenderTargetDescription desc(attachment, target.depth.get());
    target.rt = own(counter_, rhi_->newTextureRenderTarget(desc));
    target.set = pipelinesFor(target);
    target.rt->setRenderPassDescriptor(target.set->rp.get());
    return target.rt->create();
}

bool SceneRenderer::ensureUniformCapacity(quint32 bytes)
{
    if (uniforms_ && bindings_ && bytes <= uniformCapacity_)
        return true;
    quint32 capacity = std::max<quint32>(uniformCapacity_, 16 * 1024);
    while (capacity < bytes)
        capacity *= 2;
    uniforms_ = own(counter_, rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, capacity));
    if (!uniforms_->create()) {
        uniforms_.reset();
        uniformCapacity_ = 0;
        return false;
    }
    uniformCapacity_ = capacity;
    // The pipelines were created against this bindings object, so it is rebuilt in place, never replaced.
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    if (!bindings_)
        bindings_ = own(counter_, rhi_->newShaderResourceBindings());
    bindings_->setBindings(
        {QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, uniforms_.get(), frameBlockSize_),
         QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(1, stages, uniforms_.get(), itemBlockSize_)});
    return bindings_->create();
}

void SceneRenderer::uploadItem(const detail::SceneItem& item, GpuItem& gpu, bool wantWide, QRhiResourceUpdateBatch* u)
{
    gpu.generation = item.geometryGeneration;
    ++uploads_;
    if (item.kind == detail::ItemKind::Lines) {
        ensureWide(item, gpu, u);
        if (!rhi_->isFeatureSupported(QRhi::Instancing)) {
            // No wide-line pipeline on this device: the lines are drawn 1 px wide instead, never left out.
            const std::vector<float>& p = item.lines->points;
            const std::size_t vertices = p.size() / 6 * 2;
            QByteArray data(static_cast<qsizetype>(vertices * kThinStride), Qt::Uninitialized);
            char* out = data.data();
            for (std::size_t v = 0; v < vertices; ++v, out += kThinStride) {
                putFloats(out, &p[v * 3], 3);
                putU32(out + 12, static_cast<std::uint32_t>(v / 2 + 1));
            }
            gpu.thinEdges = own(counter_, rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                                          static_cast<quint32>(std::max<qsizetype>(data.size(), 4))));
            if (vertices > 0 && gpu.thinEdges->create()) {
                u->uploadStaticBuffer(gpu.thinEdges.get(), std::move(data));
                gpu.thinEdgeVertexCount = static_cast<std::uint32_t>(vertices);
            } else {
                gpu.thinEdges.reset();
            }
        }
        return;
    }
    const kernel::Mesh& mesh = *item.mesh;
    gpu.index = detail::buildMeshIndex(mesh); // picking and highlights (T-015)
    const std::size_t vertexCount = mesh.positions.size() / 3;
    if (!mesh.indices.empty() && vertexCount > 0) {
        // Face id per vertex. The kernel gives every face its own vertices, so each vertex belongs to one face.
        std::vector<std::uint32_t> faceOf(vertexCount, 0);
        for (const kernel::FaceRange& face : mesh.faces) {
            for (std::uint32_t i = 0; i < face.indexCount; ++i)
                faceOf[mesh.indices[face.firstIndex + i]] = face.face.index + 1;
        }
        QByteArray data(static_cast<qsizetype>(vertexCount * kMeshStride), Qt::Uninitialized);
        char* out = data.data();
        for (std::size_t v = 0; v < vertexCount; ++v, out += kMeshStride) {
            putFloats(out, &mesh.positions[v * 3], 3);
            putFloats(out + 12, &mesh.normals[v * 3], 3);
            putU32(out + 24, faceOf[v]);
        }
        gpu.vertices = own(counter_, rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                                     static_cast<quint32>(data.size())));
        gpu.indices = own(counter_, rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
                                                    static_cast<quint32>(mesh.indices.size() * 4)));
        if (gpu.vertices->create() && gpu.indices->create()) {
            u->uploadStaticBuffer(gpu.vertices.get(), std::move(data));
            u->uploadStaticBuffer(gpu.indices.get(), QByteArray(reinterpret_cast<const char*>(mesh.indices.data()),
                                                                static_cast<qsizetype>(mesh.indices.size() * 4)));
            gpu.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
        } else {
            gpu.vertices.reset();
            gpu.indices.reset();
        }
    }
    // Edge polylines become a line list.
    std::size_t segments = 0;
    for (const kernel::EdgeRange& edge : mesh.edges)
        segments += edge.vertexCount > 1 ? edge.vertexCount - 1 : 0;
    if (segments > 0) {
        QByteArray data(static_cast<qsizetype>(segments * 2 * kThinStride), Qt::Uninitialized);
        char* out = data.data();
        for (const kernel::EdgeRange& edge : mesh.edges) {
            for (std::uint32_t k = 0; k + 1 < edge.vertexCount; ++k) {
                for (std::uint32_t end = 0; end < 2; ++end, out += kThinStride) {
                    putFloats(out, &mesh.edgePositions[(std::size_t{edge.firstVertex} + k + end) * 3], 3);
                    putU32(out + 12, edge.edge.index + 1);
                }
            }
        }
        gpu.thinEdges = own(counter_, rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                                      static_cast<quint32>(data.size())));
        if (gpu.thinEdges->create()) {
            u->uploadStaticBuffer(gpu.thinEdges.get(), std::move(data));
            gpu.thinEdgeVertexCount = static_cast<std::uint32_t>(segments * 2);
        } else {
            gpu.thinEdges.reset();
        }
    }
    gpu.segmentCount = static_cast<std::uint32_t>(segments);
    if (wantWide)
        ensureWide(item, gpu, u);
}

void SceneRenderer::ensureWide(const detail::SceneItem& item, GpuItem& gpu, QRhiResourceUpdateBatch* u)
{
    if (gpu.wideEdges)
        return;
    QByteArray data;
    std::uint32_t segments = 0;
    if (item.kind == detail::ItemKind::Lines) {
        const std::vector<float>& p = item.lines->points;
        segments = static_cast<std::uint32_t>(p.size() / 6);
        data = QByteArray(static_cast<qsizetype>(std::size_t{segments} * kWideStride), Qt::Uninitialized);
        char* out = data.data();
        for (std::uint32_t s = 0; s < segments; ++s, out += kWideStride) {
            putFloats(out, &p[std::size_t{s} * 6], 6);
            putU32(out + 24, s + 1);
        }
    } else {
        const kernel::Mesh& mesh = *item.mesh;
        for (const kernel::EdgeRange& edge : mesh.edges)
            segments += edge.vertexCount > 1 ? edge.vertexCount - 1 : 0;
        data = QByteArray(static_cast<qsizetype>(std::size_t{segments} * kWideStride), Qt::Uninitialized);
        char* out = data.data();
        for (const kernel::EdgeRange& edge : mesh.edges) {
            for (std::uint32_t k = 0; k + 1 < edge.vertexCount; ++k, out += kWideStride) {
                putFloats(out, &mesh.edgePositions[(std::size_t{edge.firstVertex} + k) * 3], 6);
                putU32(out + 24, edge.edge.index + 1);
            }
        }
    }
    if (segments == 0)
        return;
    gpu.wideEdges = own(
        counter_, rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(data.size())));
    if (gpu.wideEdges->create()) {
        u->uploadStaticBuffer(gpu.wideEdges.get(), std::move(data));
        gpu.segmentCount = segments;
    } else {
        gpu.wideEdges.reset();
    }
}

void SceneRenderer::syncItems(QRhiResourceUpdateBatch* u)
{
    // Drop GPU data of removed items. Both sides are sorted by id.
    for (auto it = gpu_.begin(); it != gpu_.end();) {
        const auto pos = std::lower_bound(snap_.items.begin(), snap_.items.end(), it->first,
                                          [](const detail::SceneItem& a, ItemId id) { return a.id < id; });
        const bool present = pos != snap_.items.end() && pos->id == it->first;
        it = present ? std::next(it) : gpu_.erase(it);
    }
    for (const detail::SceneItem& item : snap_.items) {
        if (!item.visible)
            continue; // uploaded when it becomes visible
        GpuItem& gpu = gpu_[item.id];
        const bool wantWide = item.kind == detail::ItemKind::Lines || (smoothEdges_ && item.style.showEdges) ||
                              std::binary_search(highlightWide_.begin(), highlightWide_.end(), item.id);
        if (gpu.generation != item.geometryGeneration) {
            gpu = GpuItem{};
            uploadItem(item, gpu, wantWide, u);
        } else if (wantWide) {
            ensureWide(item, gpu, u);
        }
    }
}

void SceneRenderer::writeFrameBlock(QRhiResourceUpdateBatch* u, quint32 offset, QSize size, QMatrix4x4& view,
                                    QMatrix4x4& projection)
{
    const Camera& cam = snap_.camera;
    const QVector3D eye(float(cam.eye.x), float(cam.eye.y), float(cam.eye.z));
    const QVector3D target(float(cam.target.x), float(cam.target.y), float(cam.target.z));
    QVector3D up(float(cam.up.x), float(cam.up.y), float(cam.up.z));
    QVector3D forward = target - eye;
    if (forward.lengthSquared() < 1e-12f)
        forward = QVector3D(0, 1, 0);
    forward.normalize();
    if (QVector3D::crossProduct(forward, up).lengthSquared() < 1e-12f)
        up = std::abs(forward.z()) < 0.9f ? QVector3D(0, 0, 1) : QVector3D(0, 1, 0);
    view.setToIdentity();
    view.lookAt(eye, eye + forward, up);

    // Clip planes from the bounding sphere of what is drawn.
    const std::optional<Box3> bounds = visibleBounds();
    double nearPlane = 0.1;
    double farPlane = 10000.0;
    if (bounds) {
        const QVector3D centre(float((bounds->min.x + bounds->max.x) * 0.5),
                               float((bounds->min.y + bounds->max.y) * 0.5),
                               float((bounds->min.z + bounds->max.z) * 0.5));
        const double dx = bounds->max.x - bounds->min.x;
        const double dy = bounds->max.y - bounds->min.y;
        const double dz = bounds->max.z - bounds->min.z;
        const double radius = std::max(0.5 * std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3) * 1.02;
        const double depth = QVector3D::dotProduct(centre - eye, forward);
        if (cam.perspective) {
            farPlane = std::max(depth + radius, 1e-2);
            nearPlane = std::max({depth - radius, farPlane / 10000.0, 1e-4});
        } else {
            nearPlane = depth - radius;
            farPlane = depth + radius;
        }
    }
    const float aspect = float(size.width()) / float(std::max(1, size.height()));
    projection = rhi_->clipSpaceCorrMatrix();
    if (cam.perspective) {
        projection.perspective(float(std::clamp(cam.fovDeg, 1.0, 150.0)), aspect, float(nearPlane), float(farPlane));
    } else {
        const float h = float(std::max(cam.orthoHeight, 1e-6)) * 0.5f;
        projection.ortho(-h * aspect, h * aspect, -h, h, float(nearPlane), float(farPlane));
    }

    FrameBlock block{};
    copyMatrix(block.viewProj, projection * view);
    copyMatrix(block.view, view);
    const QVector3D light = QVector3D(-0.3f, 0.5f, 0.8f).normalized();
    block.lightDir[0] = light.x();
    block.lightDir[1] = light.y();
    block.lightDir[2] = light.z();
    block.viewport[0] = float(size.width());
    block.viewport[1] = float(size.height());
    block.viewport[2] = 1.0f / float(std::max(1, size.width()));
    block.viewport[3] = 1.0f / float(std::max(1, size.height()));
    if (const Theme* theme = snap_.theme.get()) {
        toFloat4(theme->colors[static_cast<std::size_t>(Token::ViewportBgTop)], block.bgTop);
        toFloat4(theme->colors[static_cast<std::size_t>(Token::ViewportBgBottom)], block.bgBottom);
        block.misc[3] = float(metric(*theme, kMetricDash, kDefaultDashPx) * snap_.devicePixelRatio);
    } else {
        block.misc[3] = float(kDefaultDashPx);
    }
    block.misc[0] = rhi_->isYUpInNDC() ? 1.0f : -1.0f;
    block.misc[1] = cam.perspective ? 0.0f : 1.0f;
    // T-023: the section plane (the outline of the cut a thin line wide) and draft analysis.
    if (const std::optional<detail::PlaneEquation> plane =
            snap_.section ? detail::planeEquation(*snap_.section) : std::nullopt) {
        block.misc[2] = 1.0f;
        block.section[0] = float(plane->nx);
        block.section[1] = float(plane->ny);
        block.section[2] = float(plane->nz);
        block.section[3] = float(plane->d);
        const double thin =
            snap_.theme ? metric(*snap_.theme, kMetricThinLine, kDefaultThinLinePx) : kDefaultThinLinePx;
        block.analysis[2] = float(thin * snap_.devicePixelRatio);
        // The caps' hatch: about kHatchPeriodPx logical pixels at the camera target, snapped to a power of two (mm).
        const double dx = cam.target.x - cam.eye.x, dy = cam.target.y - cam.eye.y, dz = cam.target.z - cam.eye.z;
        const double viewHeightMm = cam.perspective ? 2.0 * std::sqrt(dx * dx + dy * dy + dz * dz) *
                                                          std::tan(std::clamp(cam.fovDeg, 1.0, 150.0) * kPi / 360.0)
                                                    : std::max(cam.orthoHeight, 1e-6);
        const double wanted =
            viewHeightMm / double(std::max(1, size.height())) * kHatchPeriodPx * snap_.devicePixelRatio;
        if (std::isfinite(wanted) && wanted > 0)
            block.analysis[3] = float(std::exp2(std::round(std::log2(wanted))));
    }
    if (snap_.draft && snap_.theme) {
        const DraftAnalysis& d = *snap_.draft;
        const QVector3D pull =
            view.mapVector(QVector3D(float(d.pull.x), float(d.pull.y), float(d.pull.z))).normalized();
        block.analysis[0] = 1.0f;
        block.analysis[1] = float(std::sin(std::clamp(d.thresholdDeg, 0.0, 89.0) * kPi / 180.0));
        block.draftPull[0] = pull.x();
        block.draftPull[1] = pull.y();
        block.draftPull[2] = pull.z();
        const Theme& theme = *snap_.theme;
        toFloat4(theme.colors[std::size_t(draftToken(DraftClass::Positive))], block.draftPositive);
        toFloat4(theme.colors[std::size_t(draftToken(DraftClass::Negative))], block.draftNegative);
        toFloat4(theme.colors[std::size_t(draftToken(DraftClass::Vertical))], block.draftVertical);
    }
    u->updateDynamicBuffer(uniforms_.get(), offset, sizeof(block), &block);
}

std::optional<Box3> SceneRenderer::visibleBounds() const
{
    std::optional<Box3> bounds;
    for (const detail::SceneItem& item : snap_.items) {
        if (!item.visible || !item.localBounds)
            continue;
        const Box3 b = detail::transformBox(*item.localBounds, item.xform);
        if (!bounds) {
            bounds = b;
        } else {
            bounds->min = {std::min(bounds->min.x, b.min.x), std::min(bounds->min.y, b.min.y),
                           std::min(bounds->min.z, b.min.z)};
            bounds->max = {std::max(bounds->max.x, b.max.x), std::max(bounds->max.y, b.max.y),
                           std::max(bounds->max.z, b.max.z)};
        }
    }
    return bounds;
}

void SceneRenderer::prepareCaps(QRhiResourceUpdateBatch* u)
{
    // Caps only when the section asks for them AND the preset allows them (PLAN.md 6.2: Medium and High).
    capped_.clear();
    if (!snap_.section || !snap_.section->cap || !snap_.preset.sectionCaps || !snap_.theme)
        return;
    const std::optional<detail::PlaneEquation> plane = detail::planeEquation(*snap_.section);
    const std::optional<Box3> bounds = visibleBounds();
    if (!plane || !bounds)
        return;
    const std::optional<std::array<Vec3, 4>> quad = detail::capQuad(*plane, *bounds);
    if (!quad)
        return; // the plane misses everything
    const bool transparency = snap_.preset.transparency;
    for (std::size_t i = 0; i < draws_.size(); ++i) {
        const detail::SceneItem& item = *draws_[i].item;
        const GpuItem& drawn = *draws_[i].gpu;
        if (item.kind != detail::ItemKind::Mesh || !item.mesh || !item.localBounds || !drawn.indices ||
            drawn.indexCount == 0)
            continue;
        if (transparency && item.style.opacity < 1.0f)
            continue; // see-through bodies get no cap
        // Only bodies the plane really cuts.
        const Box3 b = detail::transformBox(*item.localBounds, item.xform);
        bool below = false, above = false;
        for (int corner = 0; corner < 8; ++corner) {
            const double dist = plane->distance(Vec3{(corner & 1) ? b.max.x : b.min.x, (corner & 2) ? b.max.y : b.min.y,
                                                     (corner & 4) ? b.max.z : b.min.z});
            below = below || dist < 0;
            above = above || dist > 0;
        }
        if (!below || !above)
            continue;
        // Worked out on a worker by the front (viewport_frontend.cpp); not known yet: no cap this frame.
        if (item.closed && item.closed->state.load() == 1)
            capped_.push_back(i);
    }
    if (capped_.empty())
        return;
    const std::array<Vec3, 4>& q = *quad;
    const Vec3 corners[kCapVertices] = {q[0], q[1], q[2], q[0], q[2], q[3]};
    float data[kCapVertices * 3];
    for (quint32 v = 0; v < kCapVertices; ++v) {
        data[v * 3 + 0] = float(corners[v].x);
        data[v * 3 + 1] = float(corners[v].y);
        data[v * 3 + 2] = float(corners[v].z);
    }
    if (!capQuad_) {
        capQuad_ = own(counter_, rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sizeof(data)));
        if (!capQuad_->create()) {
            capQuad_.reset();
            capped_.clear();
            return;
        }
    }
    u->updateDynamicBuffer(capQuad_.get(), 0, sizeof(data), data);
}

void SceneRenderer::drawScene(QRhiCommandBuffer* cb, PipelineSet& set, QSize size, quint32 frameOffset)
{
    const QRhiViewport viewport(0, 0, float(size.width()), float(size.height()));
    const auto use = [&](QRhiGraphicsPipeline* pipeline, quint32 itemOffset) {
        cb->setGraphicsPipeline(pipeline);
        cb->setViewport(viewport);
        const QRhiCommandBuffer::DynamicOffset offsets[2] = {{0, frameOffset}, {1, itemOffset}};
        cb->setShaderResources(bindings_.get(), 2, offsets);
    };
    std::uint64_t triangles = 0;
    std::uint64_t drawCalls = 0;

    const QRhiCommandBuffer::VertexInput quad(quad_.get(), 0);
    use(set.background.get(), 0);
    cb->setVertexInput(0, 1, &quad);
    cb->draw(3);
    ++drawCalls;

    const bool transparency = snap_.preset.transparency;
    const auto isBlended = [&](const DrawItem& d) {
        return transparency && d.item->style.opacity < 1.0f;
    };
    const auto drawFaces = [&](const DrawItem& d, QRhiGraphicsPipeline* pipeline) {
        if (!d.gpu->indices || d.gpu->indexCount == 0)
            return;
        use(pipeline, d.itemUniformOffset);
        const QRhiCommandBuffer::VertexInput vin(d.gpu->vertices.get(), 0);
        cb->setVertexInput(0, 1, &vin, d.gpu->indices.get(), 0, QRhiCommandBuffer::IndexUInt32);
        cb->drawIndexed(d.gpu->indexCount);
        triangles += d.gpu->indexCount / 3;
        ++drawCalls;
    };
    const auto drawWide = [&](const DrawItem& d) {
        if (!set.lineWide || !d.gpu->wideEdges || d.gpu->segmentCount == 0)
            return;
        use(set.lineWide.get(), d.itemUniformOffset);
        const QRhiCommandBuffer::VertexInput ins[2] = {{corners_.get(), 0}, {d.gpu->wideEdges.get(), 0}};
        cb->setVertexInput(0, 2, ins);
        cb->draw(6, d.gpu->segmentCount);
        ++drawCalls;
    };

    // 1. opaque faces
    for (const DrawItem& d : draws_) {
        if (d.item->kind == detail::ItemKind::Mesh && !isBlended(d))
            drawFaces(d, set.meshOpaque.get());
    }
    // 1b. section caps (T-023, section.h): per closed body, mark its inside in the stencil, then fill the cap quad
    // there. The stencil marking adds the body's triangles once more (no colour, no depth), the fill one quad.
    if (!capped_.empty() && capQuad_ && set.capStencil && set.capFill) {
        const QRhiCommandBuffer::VertexInput quadInput(capQuad_.get(), 0);
        for (const std::size_t index : capped_) {
            const DrawItem& d = draws_[index];
            use(set.capStencil.get(), d.itemUniformOffset);
            const QRhiCommandBuffer::VertexInput vin(d.gpu->vertices.get(), 0);
            cb->setVertexInput(0, 1, &vin, d.gpu->indices.get(), 0, QRhiCommandBuffer::IndexUInt32);
            cb->drawIndexed(d.gpu->indexCount);
            use(set.capFill.get(), d.itemUniformOffset);
            cb->setStencilRef(0);
            cb->setVertexInput(0, 1, &quadInput);
            cb->draw(kCapVertices);
            drawCalls += 2;
        }
    }
    // 2. edges and lines
    for (const DrawItem& d : draws_) {
        if (d.item->kind == detail::ItemKind::Lines && set.lineWide) {
            drawWide(d);
        } else if (d.item->kind == detail::ItemKind::Lines || d.item->style.showEdges) {
            if (smoothEdges_ && d.gpu->wideEdges && set.lineWide) {
                drawWide(d);
            } else if (d.gpu->thinEdges && d.gpu->thinEdgeVertexCount > 0) {
                use(set.lineThin.get(), d.itemUniformOffset);
                const QRhiCommandBuffer::VertexInput vin(d.gpu->thinEdges.get(), 0);
                cb->setVertexInput(0, 1, &vin);
                cb->draw(d.gpu->thinEdgeVertexCount);
                ++drawCalls;
            }
        }
    }
    // 3. transparent faces, far to near (draws_ is already in that order for them)
    for (const DrawItem& d : draws_) {
        if (d.item->kind == detail::ItemKind::Mesh && isBlended(d))
            drawFaces(d, set.meshBlend.get());
    }
    // 4. selection, then hover (T-015)
    drawHighlights(cb, set, viewport, frameOffset, drawCalls);
    stats_.triangles = triangles;
    stats_.drawCalls = drawCalls;
}

void SceneRenderer::collectHighlightItems()
{
    highlightWide_.clear();
    const auto needsWide = [](const PickHit& h) {
        return h.kind == kernel::SubKind::Edge || h.kind == kernel::SubKind::Solid;
    };
    for (const PickHit& h : snap_.selected) {
        if (needsWide(h))
            highlightWide_.push_back(h.item);
    }
    if (snap_.hover && needsWide(*snap_.hover))
        highlightWide_.push_back(snap_.hover->item);
    std::sort(highlightWide_.begin(), highlightWide_.end());
    highlightWide_.erase(std::unique(highlightWide_.begin(), highlightWide_.end()), highlightWide_.end());
}

void SceneRenderer::prepareHighlights(QRhiResourceUpdateBatch* u, const Theme& theme, quint32 base, quint32 slot)
{
    highlights_.clear();
    if (snap_.selected.empty() && !snap_.hover)
        return;
    std::map<ItemId, std::size_t> drawOf;
    for (std::size_t i = 0; i < draws_.size(); ++i)
        drawOf.emplace(draws_[i].item->id, i);
    std::map<ItemId, std::vector<PickHit>> selected;
    for (const PickHit& h : snap_.selected) {
        if (drawOf.count(h.item))
            selected[h.item].push_back(h);
    }
    const auto group = [&](ItemId id, bool hover, std::vector<PickHit> hits) {
        HighlightGroup g;
        g.draw = drawOf[id];
        g.hover = hover;
        g.hits = std::move(hits);
        highlights_.push_back(std::move(g));
    };
    for (auto& [id, hits] : selected)
        group(id, false, std::move(hits));
    if (snap_.hover && drawOf.count(snap_.hover->item))
        group(snap_.hover->item, true, {*snap_.hover});

    // Vertex markers: one zero-length wide-line instance per highlighted vertex.
    QByteArray markers;
    std::uint32_t markerCount = 0;
    for (HighlightGroup& g : highlights_) {
        g.firstMarker = markerCount;
        const detail::MeshIndex* index = draws_[g.draw].gpu->index.get();
        for (const PickHit& h : g.hits) {
            if (h.kind != kernel::SubKind::Vertex || !index)
                continue;
            const detail::MeshIndex::Vertex* v = index->vertexOfCode(h.subIndex);
            if (!v)
                continue;
            char entry[kWideStride];
            putFloats(entry, v->pos, 3);
            putFloats(entry + 12, v->pos, 3);
            putU32(entry + 24, 0);
            markers.append(entry, kWideStride);
            ++markerCount;
        }
        g.markerCount = markerCount - g.firstMarker;
    }
    if (markerCount > 0) {
        const quint32 bytes = markerCount * kWideStride;
        if (!markers_ || markerCapacity_ < bytes) {
            markerCapacity_ = std::max<quint32>(bytes, 64 * kWideStride);
            markers_ = own(counter_, rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, markerCapacity_));
            if (!markers_->create()) {
                markers_.reset();
                markerCapacity_ = 0;
            }
        }
        if (markers_)
            u->updateDynamicBuffer(markers_.get(), 0, bytes, markers.constData());
    }

    // Colours come from the theme: Selection / Hover for lines and markers, SelectionFill's colour (selection) or
    // Hover's colour (hover) at SelectionFill's opacity for faces. On a body drawn in its own colour (T-016) they
    // are strengthened so that they stay visible on any colour (highlight.h).
    const double dpr = snap_.devicePixelRatio;
    const double thin = metric(theme, kMetricThinLine, kDefaultThinLinePx);
    const double line = metric(theme, kMetricHighlightLine, thin * kHighlightLineScale);
    const double marker = metric(theme, kMetricVertexMarker, line * kVertexMarkerScale);
    const Color fill = theme.colors[std::size_t(Token::SelectionFill)];
    for (std::size_t k = 0; k < highlights_.size(); ++k) {
        HighlightGroup& g = highlights_[k];
        g.blockOffset = base + quint32(2 * k) * slot;
        g.markerBlockOffset = g.blockOffset + slot;
        const detail::SceneItem& item = *draws_[g.draw].item;
        ItemBlock block{};
        const QMatrix4x4 model = toMatrix(item.xform);
        copyMatrix(block.model, model);
        copyMatrix(block.normalMatrix, model.inverted().transposed());
        const Color themeBody = theme.colors[std::size_t(item.style.face)];
        const Color lineColour = theme.colors[std::size_t(g.hover ? Token::Hover : Token::Selection)];
        const Color fillColour = g.hover ? theme.colors[std::size_t(Token::Hover)] : fill;
        const detail::HighlightPaint paint =
            detail::highlightPaint(lineColour, fillColour, fill.a, item.style.faceColor.value_or(themeBody), themeBody,
                                   item.kind == detail::ItemKind::Mesh && item.style.faceColor.has_value());
        toFloat4(paint.fill, block.faceColor);
        block.faceColor[3] = paint.fillAlpha;
        toFloat4(paint.line, block.edgeColor);
        block.edgeColor[3] = 1.0f;
        block.line[0] = float(line * dpr);
        block.line[2] = 1.0f; // anti-aliased
        block.ids[0] = float(item.id);
        u->updateDynamicBuffer(uniforms_.get(), g.blockOffset, sizeof(block), &block);
        block.line[0] = float(marker * dpr);
        u->updateDynamicBuffer(uniforms_.get(), g.markerBlockOffset, sizeof(block), &block);
    }
}

void SceneRenderer::drawHighlights(QRhiCommandBuffer* cb, PipelineSet& set, const QRhiViewport& viewport,
                                   quint32 frameOffset, std::uint64_t& drawCalls)
{
    const auto use = [&](QRhiGraphicsPipeline* pipeline, quint32 itemOffset) {
        cb->setGraphicsPipeline(pipeline);
        cb->setViewport(viewport);
        const QRhiCommandBuffer::DynamicOffset offsets[2] = {{0, frameOffset}, {1, itemOffset}};
        cb->setShaderResources(bindings_.get(), 2, offsets);
    };
    for (const HighlightGroup& g : highlights_) {
        const DrawItem& d = draws_[g.draw];
        const GpuItem& gpu = *d.gpu;
        const detail::MeshIndex* index = gpu.index.get();
        const kernel::Mesh* mesh = d.item->kind == detail::ItemKind::Mesh ? d.item->mesh.get() : nullptr;

        // Faces: the face's index range, or every triangle for a body.
        if (set.highlightFaces && gpu.indices && gpu.indexCount > 0) {
            bool bound = false;
            for (const PickHit& h : g.hits) {
                std::uint32_t first = 0, count = 0;
                if (h.kind == kernel::SubKind::Solid) {
                    count = gpu.indexCount;
                } else if (h.kind == kernel::SubKind::Face && index && mesh) {
                    const std::int32_t pos = index->facePosition(h.subIndex);
                    if (pos >= 0) {
                        first = mesh->faces[std::size_t(pos)].firstIndex;
                        count = mesh->faces[std::size_t(pos)].indexCount;
                    }
                }
                if (count == 0)
                    continue;
                if (!bound) {
                    use(set.highlightFaces.get(), g.blockOffset);
                    const QRhiCommandBuffer::VertexInput vin(gpu.vertices.get(), 0);
                    cb->setVertexInput(0, 1, &vin, gpu.indices.get(), 0, QRhiCommandBuffer::IndexUInt32);
                    bound = true;
                }
                cb->drawIndexed(count, 1, first);
                ++drawCalls;
            }
        }
        if (!set.lineWide)
            continue;
        // Edges: the edge's segments as wide lines, or all of them for a body.
        if (gpu.wideEdges && gpu.segmentCount > 0) {
            for (const PickHit& h : g.hits) {
                std::uint32_t first = 0, count = 0;
                if (h.kind == kernel::SubKind::Solid) {
                    count = gpu.segmentCount;
                } else if (h.kind == kernel::SubKind::Edge && !mesh) {
                    if (h.subIndex < gpu.segmentCount) { // a segment of an addLines item
                        first = h.subIndex;
                        count = 1;
                    }
                } else if (h.kind == kernel::SubKind::Edge && index) {
                    const std::int32_t pos = index->edgePosition(h.subIndex);
                    if (pos >= 0) {
                        first = index->edgeFirstSegment[std::size_t(pos)];
                        count = index->edgeSegmentCount[std::size_t(pos)];
                    }
                }
                if (count == 0 || first + count > gpu.segmentCount)
                    continue;
                use(set.lineWide.get(), g.blockOffset);
                const QRhiCommandBuffer::VertexInput ins[2] = {{corners_.get(), 0},
                                                               {gpu.wideEdges.get(), first * kWideStride}};
                cb->setVertexInput(0, 2, ins);
                cb->draw(6, count);
                ++drawCalls;
            }
        }
        // Vertices: square markers.
        if (g.markerCount > 0 && markers_) {
            use(set.lineWide.get(), g.markerBlockOffset);
            const QRhiCommandBuffer::VertexInput ins[2] = {{corners_.get(), 0},
                                                           {markers_.get(), g.firstMarker * kWideStride}};
            cb->setVertexInput(0, 2, ins);
            cb->draw(6, g.markerCount);
            ++drawCalls;
        }
    }
}

bool SceneRenderer::render(QRhiCommandBuffer* cb)
{
    QElapsedTimer timer;
    timer.start();
    if (!rhi_ || !colour_ || !cb)
        return busy();
    ensureStatic();
    QRhiResourceUpdateBatch* u = rhi_->nextResourceUpdateBatch();
    if (!quad_) {
        static const float quad[6] = {-1, -1, 3, -1, -1, 3};
        static const float corners[12] = {0, -1, 1, -1, 1, 1, 0, -1, 1, 1, 0, 1};
        quad_ = own(counter_, rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
        corners_ = own(counter_, rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(corners)));
        quad_->create();
        corners_->create();
        u->uploadStaticBuffer(quad_.get(), quad);
        u->uploadStaticBuffer(corners_.get(), corners);
    }

    // Draw list: visible items with geometry. Slots in the uniform buffer: FrameBlocks first (main pass, then one
    // per capture), then one ItemBlock per drawn item.
    std::vector<detail::CaptureRequest> requests = std::move(snap_.captures);
    snap_.captures.clear();
    const quint32 frameSlot = quint32(rhi_->ubufAligned(int(frameBlockSize_)));
    const quint32 itemSlot = quint32(rhi_->ubufAligned(int(itemBlockSize_)));
    const quint32 frameSlots = 1 + quint32(requests.size());
    const quint32 maxItems = quint32(snap_.items.size());
    // Two ItemBlocks per highlight group: one per item with a selection, one for the hover.
    std::vector<ItemId> selectedItems;
    for (const PickHit& h : snap_.selected)
        selectedItems.push_back(h.item);
    std::sort(selectedItems.begin(), selectedItems.end());
    selectedItems.erase(std::unique(selectedItems.begin(), selectedItems.end()), selectedItems.end());
    const quint32 highlightSlots = 2 * (quint32(selectedItems.size()) + (snap_.hover ? 1u : 0u));
    if (!ensureUniformCapacity(frameSlots * frameSlot + (std::max<quint32>(1, maxItems) + highlightSlots) * itemSlot)) {
        u->release();
        return busy();
    }

    const int samples = effectiveSamples(snap_.preset.msaaSamples);
    const bool smooth = snap_.preset.smoothEdges && rhi_->isFeatureSupported(QRhi::Instancing);
    const QSize size = colour_->pixelSize();
    if (!main_ || targetDirty_ || main_->size != size || main_->samples != samples) {
        needRedraw_ = true; // a new target holds no image yet
        main_ = std::make_unique<Target>();
        if (!buildTarget(*main_, colour_, size, samples)) {
            main_.reset();
            u->release();
            return busy();
        }
        targetDirty_ = false;
        // Pipeline sets no target uses any more (an old sample count) go.
        std::erase_if(pipelineSets_, [&](const std::unique_ptr<PipelineSet>& set) {
            if (set.get() == main_->set)
                return false;
            return std::none_of(captures_.begin(), captures_.end(),
                                [&](const auto& c) { return c->target.set == set.get(); });
        });
    }
    samples_ = samples;
    smoothEdges_ = smooth;
    PipelineSet& set = *main_->set;
    if (!set.ok) {
        if (!loggedPipelineFailure_) {
            ray::log(LogLevel::Error, "render", "viewport pipelines could not be created; drawing nothing");
            loggedPipelineFailure_ = true;
        }
    }

    collectHighlightItems();
    syncItems(u);

    QMatrix4x4 view, projection;
    writeFrameBlock(u, 0, size, view, projection);

    const Theme* theme = snap_.theme.get();
    if (!theme && !loggedNoTheme_) {
        ray::log(LogLevel::Warn, "render", "viewport has no theme; drawing nothing");
        loggedNoTheme_ = true;
    }
    draws_.clear();
    const quint32 itemBase = frameSlots * frameSlot;
    for (const detail::SceneItem& item : snap_.items) {
        if (!item.visible)
            continue;
        const auto it = gpu_.find(item.id);
        if (it == gpu_.end())
            continue;
        draws_.push_back(DrawItem{&item, &it->second, itemBase + quint32(draws_.size()) * itemSlot});
    }
    if (theme) {
        const double dpr = snap_.devicePixelRatio;
        const float edgeWidth = smooth ? float(metric(*theme, kMetricThinLine, kDefaultThinLinePx) * dpr) : 1.0f;
        for (const DrawItem& d : draws_) {
            ItemBlock block{};
            const QMatrix4x4 model = toMatrix(d.item->xform);
            copyMatrix(block.model, model);
            copyMatrix(block.normalMatrix, model.inverted().transposed());
            // A face colour from data (T-016: imported body colours) replaces the face token.
            const MeshStyle& style = d.item->style;
            toFloat4(style.faceColor && d.item->kind == detail::ItemKind::Mesh
                         ? *style.faceColor
                         : theme->colors[static_cast<std::size_t>(style.face)],
                     block.faceColor);
            toFloat4(theme->colors[static_cast<std::size_t>(d.item->style.edge)], block.edgeColor);
            block.faceColor[3] = snap_.preset.transparency ? std::clamp(d.item->style.opacity, 0.0f, 1.0f) : 1.0f;
            block.edgeColor[3] = 1.0f;
            if (d.item->kind == detail::ItemKind::Lines) {
                block.line[0] = float(d.item->lines->widthPx * dpr);
                block.line[1] = d.item->lines->dashed ? 1.0f : 0.0f;
            } else {
                block.line[0] = edgeWidth;
            }
            block.line[2] = smooth ? 1.0f : 0.0f;
            block.ids[0] = float(d.item->id);
            // T-023: draft analysis paints body faces (over their own colour too), never lines or highlights.
            block.ids[1] = d.item->kind == detail::ItemKind::Mesh ? 1.0f : 0.0f;
            u->updateDynamicBuffer(uniforms_.get(), d.itemUniformOffset, sizeof(block), &block);
        }
        // Transparent items far to near.
        if (snap_.preset.transparency) {
            const auto depthOf = [&](const DrawItem& d) {
                if (!d.item->localBounds)
                    return 0.0f;
                const Box3 b = detail::transformBox(*d.item->localBounds, d.item->xform);
                const QVector3D c(float((b.min.x + b.max.x) * 0.5), float((b.min.y + b.max.y) * 0.5),
                                  float((b.min.z + b.max.z) * 0.5));
                return (view.map(c)).z(); // view space looks down -z: more negative is farther
            };
            std::stable_sort(draws_.begin(), draws_.end(),
                             [&](const DrawItem& a, const DrawItem& b) { return depthOf(a) < depthOf(b); });
        }
        prepareHighlights(u, *theme, itemBase + quint32(draws_.size()) * itemSlot, itemSlot);
        prepareCaps(u); // after the sort: capped_ indexes draws_
    } else {
        highlights_.clear();
        capped_.clear();
    }

    // Capture targets and their FrameBlocks, written in the same batch as the main pass.
    std::vector<Capture*> started;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto capture = std::make_unique<Capture>();
        capture->done = std::move(requests[i].done);
        const QSize csize(requests[i].width, requests[i].height);
        capture->target.ownedColour =
            own(counter_, rhi_->newTexture(QRhiTexture::RGBA8, csize, 1,
                                           QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        bool ok = capture->target.ownedColour->create() &&
                  buildTarget(capture->target, capture->target.ownedColour.get(), csize, samples) &&
                  capture->target.set->ok;
        if (!ok) {
            if (outbox_)
                outbox_->post([done = std::move(capture->done)] { done({}); });
            continue;
        }
        QMatrix4x4 cview, cprojection;
        capture->frameOffset = quint32(i + 1) * frameSlot;
        writeFrameBlock(u, capture->frameOffset, csize, cview, cprojection);
        capture->result.completed = [c = capture.get(), outbox = outbox_, yUp = rhi_->isYUpInFramebuffer()] {
            Image image;
            image.width = c->result.pixelSize.width();
            image.height = c->result.pixelSize.height();
            const std::size_t row = std::size_t(image.width) * 4;
            const auto* src = reinterpret_cast<const std::uint8_t*>(c->result.data.constData());
            if (std::size_t(c->result.data.size()) >= row * std::size_t(image.height)) {
                image.rgba.resize(row * std::size_t(image.height));
                for (int y = 0; y < image.height; ++y) {
                    const int from = yUp ? image.height - 1 - y : y;
                    std::memcpy(&image.rgba[std::size_t(y) * row], src + std::size_t(from) * row, row);
                }
            }
            c->result.data.clear();
            if (outbox) {
                outbox->post([image = std::move(image), done = std::move(c->done)]() mutable {
                    std::vector<std::byte> png;
                    if (!image.rgba.empty()) {
                        if (auto encoded = encodePng(image))
                            png = std::move(*encoded);
                    }
                    done(std::move(png));
                });
            }
            c->finished.store(true);
        };
        started.push_back(capture.get());
        captures_.push_back(std::move(capture));
    }

    // Main pass. A frame that only answers picks keeps the image drawn last (T-015): the colour texture still holds
    // it, so only the resource updates are recorded.
    const bool drawMain = needRedraw_;
    if (drawMain) {
        cb->beginPass(main_->rt.get(), QColor(Qt::transparent), {1.0f, 0}, u);
        if (theme && set.ok)
            drawScene(cb, set, size, 0);
        else
            stats_.triangles = stats_.drawCalls = 0;
        cb->endPass();
        needRedraw_ = false;
        capsDrawn_ = theme && set.ok && set.capStencil && set.capFill ? std::uint32_t(capped_.size()) : 0;
    } else {
        cb->resourceUpdate(u);
    }
    const IViewport::FrameStats mainStats = stats_;

    // Captures: the same scene at the requested size, then a read-back.
    for (Capture* capture : started) {
        Capture& c = *capture;
        cb->beginPass(c.target.rt.get(), QColor(Qt::transparent), {1.0f, 0});
        if (theme)
            drawScene(cb, *c.target.set, c.target.size, c.frameOffset);
        QRhiResourceUpdateBatch* rb = rhi_->nextResourceUpdateBatch();
        rb->readBackTexture(QRhiReadbackDescription(c.target.ownedColour.get()), &c.result);
        cb->endPass(rb);
    }
    stats_ = mainStats;

    // Later passes (picking, backdrop, section caps).
    if (!passes_.empty()) {
        FrameContext ctx;
        ctx.rhi = rhi_;
        ctx.cb = cb;
        ctx.colour = colour_;
        ctx.pixelSize = size;
        ctx.snapshot = &snap_;
        ctx.view = view;
        ctx.projection = projection;
        ctx.draws = draws_;
        ctx.uniforms = uniforms_.get();
        ctx.bindings = bindings_.get();
        ctx.frameUniformOffset = 0;
        ctx.sceneGeneration = snap_.sceneGeneration;
        ctx.cameraGeneration = snap_.cameraGeneration;
        ctx.outbox = outbox_.get();
        ctx.colourRedrawn = drawMain;
        ctx.picks = &snap_.picks;
        ctx.lineCorners = corners_.get();
        ctx.capQuad = capQuad_.get();
        ctx.capped = capped_;
        for (auto& pass : passes_)
            pass->record(ctx);
    }

    ++frames_;
    // Frame statistics describe the last frame that drew the picture, not a pick-only frame.
    lastDrew_ = drawMain;
    publishStats(drawMain ? double(timer.nsecsElapsed()) * 1e-6 : stats_.cpuMs);
    return busy();
}

void SceneRenderer::frameDone(QRhiCommandBuffer* cb, double wallMs)
{
    if (cb && lastDrew_) {
        const double gpu = cb->lastCompletedGpuTime();
        if (gpu > 0)
            stats_.gpuMs = gpu * 1000.0;
    }
    std::erase_if(captures_, [](const std::unique_ptr<Capture>& c) { return c->finished.load(); });
    if (backdrop_)
        backdrop_->frameDone(); // textures it replaced before this frame are no longer referenced
    publishStats(wallMs > 0 && lastDrew_ ? wallMs : stats_.cpuMs);
}

void SceneRenderer::publishStats(double cpuMs)
{
    stats_.cpuMs = cpuMs;
    if (!status_)
        return;
    std::lock_guard lock(status_->mutex);
    status_->stats = stats_;
    ViewportInfo& info = status_->info;
    if (rhi_) {
        info.backend = rhi_->backendName();
        const QRhiDriverInfo driver = rhi_->driverInfo();
        info.device = driver.deviceName.toStdString();
        info.softwareRenderer =
            driver.deviceType == QRhiDriverInfo::CpuDevice || detail::isSoftwareRendererName(info.device);
    }
    info.sampleCount = samples_;
    info.smoothEdges = smoothEdges_;
    if (main_) {
        info.width = main_->size.width();
        info.height = main_->size.height();
    }
    info.framesRendered = frames_;
    info.pickBufferRenders = pickBufferRenders();
    info.backdropBlurs = backdropBlurs();
    info.meshUploads = uploads_;
    info.sectionCaps = capsDrawn_;
    info.liveRhiResources = counter_.live.load();
    // The UI thread learns size, orientation and generation; the handle itself stays on this thread (render.h).
    status_->backdrop = backdrop();
    status_->backdrop.rhiTexture = nullptr;
}

} // namespace ray::render::rhi
