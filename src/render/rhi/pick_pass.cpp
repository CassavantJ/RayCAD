// The picking pass (task T-015). See pick_pass.h.
#include "pick_pass.h"
#include "scene_renderer.h" // GpuItem

#include "../viewport_frontend.h"

#include <ray_render/viewport.h>

#include <QtGui/QColor>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace ray::render::rhi {

namespace {

constexpr int kFaceDepthBias = 2; // as the main pass (scene_renderer.cpp), so edges win over their faces
constexpr float kFaceSlopeDepthBias = 1.5f;
// Read-back margin around a cursor beyond the tolerance: the vertex visibility test looks this far (pixels).
constexpr int kVertexMarginPx = 4;

struct Shaders {
    QShader meshVs, lineVs, wideVs, capVs, pathVs, fs;
    bool ok = false;
};

const Shaders& shaders()
{
    static const Shaders s = [] {
        Shaders out;
        out.meshVs = loadShader("pick_mesh.vert");
        out.lineVs = loadShader("pick_line.vert");
        out.wideVs = loadShader("pick_wideline.vert");
        out.capVs = loadShader("pick_cap.vert");   // T-023
        out.pathVs = loadShader("pick_path.vert"); // T-038
        out.fs = loadShader("pick.frag");
        out.ok = out.meshVs.isValid() && out.lineVs.isValid() && out.wideVs.isValid() && out.fs.isValid();
        return out;
    }();
    return s;
}

// Physical-pixel rectangle a request needs, inclusive.
struct PixelRect {
    int x0, y0, x1, y1;
};

int toPhysical(double logical, double dpr)
{
    return int(std::floor(logical * dpr));
}

PixelRect needed(const detail::PickRequest& r, double dpr)
{
    if (r.rect) {
        const int m = kVertexMarginPx;
        return {toPhysical(r.x0, dpr) - m, toPhysical(r.y0, dpr) - m, int(std::ceil((r.x1 + 1) * dpr)) - 1 + m,
                int(std::ceil((r.y1 + 1) * dpr)) - 1 + m};
    }
    const int cx = toPhysical(r.x0 + 0.5, dpr);
    const int cy = toPhysical(r.y0 + 0.5, dpr);
    const int m = int(std::ceil(kPickTolerancePx * dpr)) + kVertexMarginPx;
    return {cx - m, cy - m, cx + m, cy + m};
}

} // namespace

struct PickPass::Job {
    std::shared_ptr<const detail::PickView> view;
    std::vector<detail::PickRequest> requests;
    double dpr = 1.0;
    QRect rect; // top-left origin
    bool bottomUp = false;
    detail::Outbox* outbox = nullptr;
    QRhiReadbackResult results[3];
    int remaining = 3;
    std::atomic<bool> finished{false};

    void partDone()
    {
        if (--remaining > 0)
            return;
        const std::size_t bytes = std::size_t(rect.width()) * std::size_t(rect.height()) * 4;
        bool ok = true;
        for (const QRhiReadbackResult& r : results)
            ok = ok && std::size_t(r.data.size()) >= bytes;
        detail::IdWindow win;
        if (ok) {
            win = detail::decodeIdWindow(rect.x(), rect.y(), rect.width(), rect.height(),
                                         reinterpret_cast<const std::uint8_t*>(results[0].data.constData()),
                                         reinterpret_cast<const std::uint8_t*>(results[1].data.constData()),
                                         reinterpret_cast<const std::uint8_t*>(results[2].data.constData()), bottomUp);
        }
        for (QRhiReadbackResult& r : results)
            r.data.clear();
        for (detail::PickRequest& req : requests) {
            if (req.rect) {
                std::vector<PickHit> hits;
                if (ok) {
                    hits = detail::resolvePickRect(*view, win, toPhysical(req.x0, dpr), toPhysical(req.y0, dpr),
                                                   int(std::ceil((req.x1 + 1) * dpr)) - 1,
                                                   int(std::ceil((req.y1 + 1) * dpr)) - 1, req.filterMask);
                }
                if (outbox)
                    outbox->post(
                        [done = std::move(req.many), hits = std::move(hits)]() mutable { done(std::move(hits)); });
            } else {
                std::optional<PickHit> hit;
                if (ok) {
                    hit = detail::resolvePick(*view, win, toPhysical(req.x0 + 0.5, dpr), toPhysical(req.y0 + 0.5, dpr),
                                              kPickTolerancePx * dpr, req.filterMask);
                }
                if (outbox)
                    outbox->post([done = std::move(req.one), hit] { done(hit); });
            }
        }
        requests.clear();
        finished.store(true);
    }
};

PickPass::PickPass(ResourceCounter& counter) : counter_(counter) {}

PickPass::~PickPass()
{
    releaseResources();
}

bool PickPass::busy() const
{
    return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& j) { return !j->finished.load(); });
}

void PickPass::releaseResources()
{
    if (rhi_ && !rhi_->isRecordingFrame() && busy())
        rhi_->finish(); // completes the read-backs, so nothing calls into a destroyed Job
    jobs_.clear();
    faces_.reset();
    thinEdges_.reset();
    segments_.reset();
    paths_.reset();
    capStencil_.reset();
    capFill_.reset();
    rt_.reset();
    rp_.reset();
    depthBuffer_.reset();
    ids_.reset();
    items_.reset();
    depths_.reset();
    pipelineBindings_ = nullptr;
    pipelinesTried_ = false;
    view_.reset();
    size_ = QSize();
    rhi_ = nullptr;
}

void PickPass::answerMisses(std::vector<detail::PickRequest>& requests, detail::Outbox* outbox)
{
    if (!outbox)
        return;
    for (detail::PickRequest& r : requests) {
        if (r.rect)
            outbox->post([done = std::move(r.many)] { done({}); });
        else
            outbox->post([done = std::move(r.one)] { done(std::nullopt); });
    }
    requests.clear();
}

bool PickPass::ensureTarget(const FrameContext& ctx)
{
    if (rt_ && size_ == ctx.pixelSize)
        return true;
    // The pipelines stay: their render pass descriptor is compatible with the new target's.
    rt_.reset();
    depthBuffer_.reset();
    ids_.reset();
    items_.reset();
    depths_.reset();
    view_.reset();
    size_ = ctx.pixelSize;
    const auto flags = QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource;
    ids_ = own(counter_, rhi_->newTexture(QRhiTexture::RGBA8, size_, 1, flags));
    items_ = own(counter_, rhi_->newTexture(QRhiTexture::RGBA8, size_, 1, flags));
    depths_ = own(counter_, rhi_->newTexture(QRhiTexture::RGBA8, size_, 1, flags));
    depthBuffer_ = own(counter_, rhi_->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size_, 1));
    if (!ids_->create() || !items_->create() || !depths_->create() || !depthBuffer_->create()) {
        rt_.reset();
        return false;
    }
    QRhiTextureRenderTargetDescription desc;
    desc.setColorAttachments(
        {QRhiColorAttachment(ids_.get()), QRhiColorAttachment(items_.get()), QRhiColorAttachment(depths_.get())});
    desc.setDepthStencilBuffer(depthBuffer_.get());
    rt_ = own(counter_, rhi_->newTextureRenderTarget(desc));
    if (!rp_)
        rp_ = own(counter_, rt_->newCompatibleRenderPassDescriptor());
    rt_->setRenderPassDescriptor(rp_.get());
    if (!rt_->create()) {
        rt_.reset();
        return false;
    }
    return true;
}

bool PickPass::ensurePipelines(const FrameContext& ctx)
{
    if (pipelinesTried_ && pipelineBindings_ == ctx.bindings)
        return faces_ && thinEdges_;
    pipelinesTried_ = true;
    pipelineBindings_ = ctx.bindings;
    faces_.reset();
    thinEdges_.reset();
    segments_.reset();
    paths_.reset();
    capStencil_.reset();
    capFill_.reset();
    const Shaders& sh = shaders();
    if (!sh.ok || !ctx.bindings || rhi_->resourceLimit(QRhi::MaxColorAttachments) < 3)
        return false;
    using P = QRhiGraphicsPipeline;
    const auto make = [&](const QShader& vs, const QRhiVertexInputLayout& layout, P::Topology topology,
                          P::CompareOp depthOp, bool depthWrite, bool bias) {
        Owned<QRhiGraphicsPipeline> p = own(counter_, rhi_->newGraphicsPipeline());
        p->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, sh.fs}});
        p->setVertexInputLayout(layout);
        p->setTopology(topology);
        p->setDepthTest(true);
        p->setDepthWrite(depthWrite);
        p->setDepthOp(depthOp);
        p->setCullMode(P::None);
        p->setTargetBlends({P::TargetBlend(), P::TargetBlend(), P::TargetBlend()});
        if (bias) {
            p->setDepthBias(kFaceDepthBias);
            p->setSlopeScaledDepthBias(kFaceSlopeDepthBias);
        }
        p->setSampleCount(1);
        p->setShaderResourceBindings(ctx.bindings);
        p->setRenderPassDescriptor(rp_.get());
        if (!p->create())
            p.reset();
        return p;
    };
    faces_ = make(sh.meshVs, meshVertexLayout(), P::Triangles, P::Less, true, true);
    thinEdges_ = make(sh.lineVs, thinEdgeVertexLayout(), P::Lines, P::LessOrEqual, false, false);
    if (rhi_->isFeatureSupported(QRhi::Instancing))
        segments_ = make(sh.wideVs, wideLineVertexLayout(), P::Triangles, P::LessOrEqual, false, false);
    // T-038: backplot segments. They write depth, so where segments cross the nearer one holds the pixel.
    if (sh.pathVs.isValid() && rhi_->isFeatureSupported(QRhi::Instancing))
        paths_ = make(sh.pathVs, pickPathVertexLayout(), P::TriangleStrip, P::LessOrEqual, true, false);
    // T-023: section caps, as in the main pass (section.h), so a cap hides what lies behind it here too.
    if (sh.capVs.isValid()) {
        const auto finish = [&](Owned<QRhiGraphicsPipeline>& p) {
            p->setSampleCount(1);
            p->setShaderResourceBindings(ctx.bindings);
            p->setRenderPassDescriptor(rp_.get());
            if (!p->create())
                p.reset();
        };
        capStencil_ = own(counter_, rhi_->newGraphicsPipeline());
        capStencil_->setShaderStages({{QRhiShaderStage::Vertex, sh.meshVs}, {QRhiShaderStage::Fragment, sh.fs}});
        capStencil_->setVertexInputLayout(meshVertexLayout());
        capStencil_->setTopology(P::Triangles);
        setCapStencilMarking(*capStencil_, 3);
        finish(capStencil_);
        capFill_ = own(counter_, rhi_->newGraphicsPipeline());
        capFill_->setShaderStages({{QRhiShaderStage::Vertex, sh.capVs}, {QRhiShaderStage::Fragment, sh.fs}});
        capFill_->setVertexInputLayout(capVertexLayout());
        capFill_->setTopology(P::Triangles);
        capFill_->setDepthTest(true);
        capFill_->setDepthWrite(true);
        capFill_->setDepthOp(P::Less);
        capFill_->setCullMode(P::None);
        capFill_->setTargetBlends({P::TargetBlend(), P::TargetBlend(), P::TargetBlend()});
        capFill_->setDepthBias(kFaceDepthBias);
        capFill_->setSlopeScaledDepthBias(kFaceSlopeDepthBias);
        setCapStencilFill(*capFill_);
        finish(capFill_);
        if (!capStencil_ || !capFill_) {
            capStencil_.reset();
            capFill_.reset();
        }
    }
    return faces_ && thinEdges_;
}

// Begins the ID pass and draws every item; the caller ends the pass with its read-back batch.
void PickPass::drawIds(const FrameContext& ctx)
{
    QRhiCommandBuffer* cb = ctx.cb;
    cb->beginPass(rt_.get(), QColor(0, 0, 0, 0), {1.0f, 0});
    const QRhiViewport viewport(0, 0, float(size_.width()), float(size_.height()));
    const auto use = [&](QRhiGraphicsPipeline* pipeline, quint32 itemOffset) {
        cb->setGraphicsPipeline(pipeline);
        cb->setViewport(viewport);
        const QRhiCommandBuffer::DynamicOffset offsets[2] = {{0, ctx.frameUniformOffset}, {1, itemOffset}};
        cb->setShaderResources(ctx.bindings, 2, offsets);
    };
    // Items that do not answer picks (PickMode::Never) are left out, so picks go through them.
    for (const DrawItem& d : ctx.draws) {
        if (!d.pickable || d.item->kind != detail::ItemKind::Mesh || !d.gpu->indices || d.gpu->indexCount == 0)
            continue;
        use(faces_.get(), d.itemUniformOffset);
        const QRhiCommandBuffer::VertexInput vin(d.gpu->vertices.get(), 0);
        cb->setVertexInput(0, 1, &vin, d.gpu->indices.get(), 0, QRhiCommandBuffer::IndexUInt32);
        cb->drawIndexed(d.gpu->indexCount);
    }
    // T-023: the caps of the capped bodies (IdKind::Cap).
    if (capStencil_ && capFill_ && ctx.capQuad) {
        const QRhiCommandBuffer::VertexInput quadInput(ctx.capQuad, 0);
        for (const std::size_t index : ctx.capped) {
            if (index >= ctx.draws.size())
                continue;
            const DrawItem& d = ctx.draws[index];
            if (!d.pickable)
                continue;
            use(capStencil_.get(), d.itemUniformOffset);
            const QRhiCommandBuffer::VertexInput vin(d.gpu->vertices.get(), 0);
            cb->setVertexInput(0, 1, &vin, d.gpu->indices.get(), 0, QRhiCommandBuffer::IndexUInt32);
            cb->drawIndexed(d.gpu->indexCount);
            use(capFill_.get(), d.itemUniformOffset);
            cb->setStencilRef(0);
            cb->setVertexInput(0, 1, &quadInput);
            cb->draw(6);
        }
    }
    for (const DrawItem& d : ctx.draws) {
        if (!d.pickable)
            continue;
        if (d.item->kind == detail::ItemKind::Backplot) {
            // T-038: the range on screen, as the main pass draws it (the picture and the ID buffer agree).
            if (!paths_ || !ctx.pathCorners)
                continue;
            use(paths_.get(), d.itemUniformOffset);
            std::uint64_t drawCalls = 0;
            const int level = pathLevelFor(*d.item, *d.gpu, ctx.snapshot->camera, size_.height());
            drawBackplot(cb, ctx.pathCorners, *d.item, *d.gpu, level, drawCalls);
        } else if (d.item->kind == detail::ItemKind::Lines) {
            if (!segments_ || !ctx.lineCorners || !d.gpu->wideEdges || d.gpu->segmentCount == 0)
                continue;
            use(segments_.get(), d.itemUniformOffset);
            const QRhiCommandBuffer::VertexInput ins[2] = {{ctx.lineCorners, 0}, {d.gpu->wideEdges.get(), 0}};
            cb->setVertexInput(0, 2, ins);
            cb->draw(6, d.gpu->segmentCount);
        } else if (d.item->style.showEdges && d.gpu->thinEdges && d.gpu->thinEdgeVertexCount > 0) {
            use(thinEdges_.get(), d.itemUniformOffset);
            const QRhiCommandBuffer::VertexInput vin(d.gpu->thinEdges.get(), 0);
            cb->setVertexInput(0, 1, &vin);
            cb->draw(d.gpu->thinEdgeVertexCount);
        }
    }
}

std::shared_ptr<const detail::PickView> PickPass::makeView(const FrameContext& ctx) const
{
    auto view = std::make_shared<detail::PickView>();
    view->width = ctx.pixelSize.width();
    view->height = ctx.pixelSize.height();
    view->yUpInNdc = ctx.rhi->isYUpInNDC();
    if (ctx.snapshot && ctx.snapshot->section)
        view->section = detail::planeEquation(*ctx.snapshot->section);
    const QMatrix4x4 vp = ctx.projection * ctx.view;
    const float* m = vp.constData();
    for (int i = 0; i < 16; ++i)
        view->viewProj[i] = double(m[i]);
    if (!detail::invert4x4(view->viewProj, view->invViewProj))
        std::fill(std::begin(view->invViewProj), std::end(view->invViewProj), 0.0);
    view->items.reserve(ctx.draws.size());
    for (const DrawItem& d : ctx.draws) {
        if (!d.pickable)
            continue;
        detail::PickItem item;
        item.id = d.item->id;
        item.lines = d.item->kind == detail::ItemKind::Lines || d.item->kind == detail::ItemKind::Backplot;
        item.xform = d.item->xform;
        item.mesh = d.item->mesh;
        item.index = d.gpu->index;
        view->items.push_back(std::move(item));
    }
    std::sort(view->items.begin(), view->items.end(),
              [](const detail::PickItem& a, const detail::PickItem& b) { return a.id < b.id; });
    return view;
}

void PickPass::record(const FrameContext& ctx)
{
    std::erase_if(jobs_, [](const std::unique_ptr<Job>& j) { return j->finished.load(); });
    if (!ctx.picks || ctx.picks->empty())
        return;
    std::vector<detail::PickRequest> requests = std::move(*ctx.picks);
    ctx.picks->clear();
    if (!ctx.rhi || !ctx.cb || !ctx.snapshot || !ctx.snapshot->theme || ctx.pixelSize.isEmpty()) {
        answerMisses(requests, ctx.outbox); // nothing is drawn without a theme, so nothing can be hit
        return;
    }
    if (ctx.rhi != rhi_) {
        releaseResources();
        rhi_ = ctx.rhi;
    }
    const bool resized = !rt_ || size_ != ctx.pixelSize;
    if (!ensureTarget(ctx) || !ensurePipelines(ctx)) {
        if (!loggedFailure_) {
            ray::log(LogLevel::Error, "render", "picking ID buffer could not be created; picks miss");
            loggedFailure_ = true;
        }
        answerMisses(requests, ctx.outbox);
        return;
    }

    // The window to read: the union of what every request needs, inside the target.
    const double dpr = ctx.snapshot->devicePixelRatio > 0 ? ctx.snapshot->devicePixelRatio : 1.0;
    int x0 = size_.width(), y0 = size_.height(), x1 = -1, y1 = -1;
    for (const detail::PickRequest& r : requests) {
        const PixelRect p = needed(r, dpr);
        x0 = std::min(x0, p.x0);
        y0 = std::min(y0, p.y0);
        x1 = std::max(x1, p.x1);
        y1 = std::max(y1, p.y1);
    }
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, size_.width() - 1);
    y1 = std::min(y1, size_.height() - 1);
    if (x0 > x1 || y0 > y1) {
        answerMisses(requests, ctx.outbox); // every request is outside the viewport
        return;
    }

    const bool stale =
        resized || !view_ || sceneGeneration_ != ctx.sceneGeneration || cameraGeneration_ != ctx.cameraGeneration;
    if (stale) {
        drawIds(ctx);
        view_ = makeView(ctx);
        sceneGeneration_ = ctx.sceneGeneration;
        cameraGeneration_ = ctx.cameraGeneration;
        ++renders_;
    }

    auto job = std::make_unique<Job>();
    job->view = view_;
    job->requests = std::move(requests);
    job->dpr = dpr;
    job->rect = QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    job->bottomUp = rhi_->isYUpInFramebuffer();
    job->outbox = ctx.outbox;
    QRect source = job->rect;
    if (job->bottomUp)
        source.moveTop(size_.height() - (y1 + 1));
    QRhiResourceUpdateBatch* rb = rhi_->nextResourceUpdateBatch();
    QRhiTexture* textures[3] = {ids_.get(), items_.get(), depths_.get()};
    for (int i = 0; i < 3; ++i) {
        job->results[i].completed = [j = job.get()] {
            j->partDone();
        };
        QRhiReadbackDescription desc(textures[i]);
        desc.setRect(source);
        rb->readBackTexture(desc, &job->results[i]);
    }
    jobs_.push_back(std::move(job));
    if (stale)
        ctx.cb->endPass(rb);
    else
        ctx.cb->resourceUpdate(rb);
}

} // namespace ray::render::rhi
