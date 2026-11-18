// The Qt Quick seam (task T-014): the viewport drawn with the scene graph's QRhi. See viewport_rhi.h for how the
// QQuickRhiItem of T-020 drives it.
#include "rhi/scene_renderer.h"
#include "viewport_frontend.h"

#include <ray_render/viewport_rhi.h>

#include <utility>

namespace ray::render {

std::unique_ptr<IRenderViewport> makeHostedViewport(std::function<void()> requestUpdate)
{
    return std::make_unique<detail::ViewportFrontend>(std::move(requestUpdate));
}

HostedViewportRenderer::HostedViewportRenderer() : renderer_(std::make_unique<rhi::SceneRenderer>()) {}

HostedViewportRenderer::~HostedViewportRenderer()
{
    renderer_->releaseResources();
}

void HostedViewportRenderer::initialize(QRhi* rhi, QRhiTexture* colourTexture)
{
    renderer_->setRhi(rhi);
    renderer_->setTarget(colourTexture);
}

void HostedViewportRenderer::synchronize(IRenderViewport& front)
{
    auto* frontend = dynamic_cast<detail::ViewportFrontend*>(&front);
    if (!frontend)
        return;
    renderer_->setSinks(frontend->outbox(), frontend->status());
    renderer_->setSnapshot(frontend->takeSnapshot());
}

bool HostedViewportRenderer::render(QRhiCommandBuffer* cb)
{
    // Frees capture targets whose read-back completed in an earlier frame and records the GPU time of the last
    // completed frame (only filled when the scene graph's QRhi has timestamps on).
    renderer_->frameDone(cb);
    return renderer_->render(cb);
}

void HostedViewportRenderer::releaseResources()
{
    renderer_->releaseResources();
}

void HostedViewportRenderer::frameSubmitted(QRhiCommandBuffer* cb, double wallMs)
{
    renderer_->frameDone(cb, wallMs);
}

bool HostedViewportRenderer::busy() const
{
    return renderer_->busy();
}

std::int64_t HostedViewportRenderer::liveResources() const
{
    return renderer_->liveResources();
}

IViewport::Backdrop HostedViewportRenderer::backdrop() const
{
    return renderer_->backdrop();
}

} // namespace ray::render
