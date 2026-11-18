// The viewport renderer on Qt RHI (task T-014). See scene_renderer.h. Design from spike S1 (spikes/s1-viewport):
// one vertex and index buffer per body, per-face ranges kept in the vertex data (face id attribute) for T-015,
// edges in their own line buffer, faces pushed back with a depth bias so their edges stay visible.
#include "scene_renderer.h"

#include "../tier_internal.h"

#include <QtCore/QByteArray>
#include <QtGui/QColor>
#include <QtGui/QVector3D>

#include <algorithm>
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
};
struct ItemBlock {
    float model[16];
    float normalMatrix[16];
    float faceColor[4];
    float edgeColor[4];
    float line[4];
    float ids[4];
};
static_assert(sizeof(FrameBlock) == 224);
static_assert(sizeof(ItemBlock) == 192);

constexpr quint32 kMeshStride = 28; // position, normal, face id
constexpr quint32 kThinStride = 16; // position, edge id
constexpr quint32 kWideStride = 28; // p0, p1, edge id
constexpr int kFaceDepthBias = 2;   // faces sit behind their edges (S1)
constexpr float kFaceSlopeDepthBias = 1.5f;
// Theme metrics used here, with the value used when a theme has none.
constexpr const char* kMetricThinLine = "line.thin.px";
constexpr double kDefaultThinLinePx = 1.5;
constexpr const char* kMetricDash = "line.dash.px";
constexpr double kDefaultDashPx = 8.0;

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
    bool ok = false;
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
        out.ok = out.backgroundVs.isValid() && out.backgroundFs.isValid() && out.meshVs.isValid() &&
                 out.meshFs.isValid() && out.lineVs.isValid() && out.lineFs.isValid() && out.wideVs.isValid() &&
                 out.wideFs.isValid();
        return out;
    }();
    return s;
}

} // namespace

struct SceneRenderer::PipelineSet {
    Owned<QRhiRenderPassDescriptor> rp;
    int samples = 1;
    Owned<QRhiGraphicsPipeline> background, meshOpaque, meshBlend, lineThin, lineWide;
    bool ok = false;
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

SceneRenderer::SceneRenderer() = default;

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
    // Captures not yet drawn stay queued.
    std::vector<detail::CaptureRequest> pending = std::move(snap_.captures);
    snap_ = std::move(snapshot);
    if (!pending.empty()) {
        pending.insert(pending.end(), std::make_move_iterator(snap_.captures.begin()),
                       std::make_move_iterator(snap_.captures.end()));
        snap_.captures = std::move(pending);
    }
}

void SceneRenderer::setSinks(std::shared_ptr<detail::Outbox> outbox, std::shared_ptr<detail::SharedStatus> status)
{
    outbox_ = std::move(outbox);
    status_ = std::move(status);
}

void SceneRenderer::addPass(std::unique_ptr<IFramePass> pass)
{
    if (pass)
        passes_.push_back(std::move(pass));
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

    QRhiVertexInputLayout mesh;
    mesh.setBindings({QRhiVertexInputBinding(kMeshStride)});
    mesh.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
                        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 12),
                        QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::UNormByte4, 24)});

    QRhiVertexInputLayout thin;
    thin.setBindings({QRhiVertexInputBinding(kThinStride)});
    thin.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
                        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::UNormByte4, 12)});

    QRhiVertexInputLayout wide;
    wide.setBindings({QRhiVertexInputBinding(2 * sizeof(float)),
                      QRhiVertexInputBinding(kWideStride, QRhiVertexInputBinding::PerInstance)});
    wide.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                        QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float3, 0),
                        QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float3, 12),
                        QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::UNormByte4, 24)});

    using P = QRhiGraphicsPipeline;
    set->background =
        make(sh.backgroundVs, sh.backgroundFs, background, P::Triangles, false, false, P::Always, false, false);
    set->meshOpaque = make(sh.meshVs, sh.meshFs, mesh, P::Triangles, true, true, P::Less, false, true);
    set->meshBlend = make(sh.meshVs, sh.meshFs, mesh, P::Triangles, true, false, P::Less, true, true);
    set->lineThin = make(sh.lineVs, sh.lineFs, thin, P::Lines, true, false, P::LessOrEqual, false, false);
    if (rhi_->isFeatureSupported(QRhi::Instancing))
        set->lineWide = make(sh.wideVs, sh.wideFs, wide, P::Triangles, true, false, P::LessOrEqual, true, false);
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
    if (item.kind == detail::ItemKind::Lines) {
        ensureWide(item, gpu, u);
        return;
    }
    const kernel::Mesh& mesh = *item.mesh;
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
        const bool wantWide = item.kind == detail::ItemKind::Lines || (smoothEdges_ && item.style.showEdges);
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
    block.misc[2] = 0.0f; // section off until T-023
    u->updateDynamicBuffer(uniforms_.get(), offset, sizeof(block), &block);
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
    // 2. edges and lines
    for (const DrawItem& d : draws_) {
        if (d.item->kind == detail::ItemKind::Lines) {
            drawWide(d);
        } else if (d.item->style.showEdges) {
            if (smoothEdges_ && d.gpu->wideEdges) {
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
    stats_.triangles = triangles;
    stats_.drawCalls = drawCalls;
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
    if (!ensureUniformCapacity(frameSlots * frameSlot + std::max<quint32>(1, maxItems) * itemSlot)) {
        u->release();
        return busy();
    }

    const int samples = effectiveSamples(snap_.preset.msaaSamples);
    const bool smooth = snap_.preset.smoothEdges && rhi_->isFeatureSupported(QRhi::Instancing);
    const QSize size = colour_->pixelSize();
    if (!main_ || targetDirty_ || main_->size != size || main_->samples != samples) {
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
            toFloat4(theme->colors[static_cast<std::size_t>(d.item->style.face)], block.faceColor);
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

    // Main pass.
    cb->beginPass(main_->rt.get(), QColor(Qt::transparent), {1.0f, 0}, u);
    if (theme && set.ok)
        drawScene(cb, set, size, 0);
    else
        stats_.triangles = stats_.drawCalls = 0;
    cb->endPass();
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
        for (auto& pass : passes_)
            pass->record(ctx);
    }

    ++frames_;
    publishStats(double(timer.nsecsElapsed()) * 1e-6);
    return busy();
}

void SceneRenderer::frameDone(QRhiCommandBuffer* cb, double wallMs)
{
    if (cb) {
        const double gpu = cb->lastCompletedGpuTime();
        if (gpu > 0)
            stats_.gpuMs = gpu * 1000.0;
    }
    std::erase_if(captures_, [](const std::unique_ptr<Capture>& c) { return c->finished.load(); });
    publishStats(wallMs > 0 ? wallMs : stats_.cpuMs);
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
    info.liveRhiResources = counter_.live.load();
}

} // namespace ray::render::rhi
