// Seam for drawing a viewport with someone else's QRhi: Qt Quick's scene graph (task T-020 wraps this in a
// QQuickRhiItem). Qt types are only forward-declared, but the functions take Qt RHI objects, so only src/ui and
// src/render may use this header. Not one of the contract headers in include/ray/.
//
// How T-020 uses it (the S1 spike's QQuickRhiItem pattern):
//   UI thread    the QQuickRhiItem owns `front = makeHostedViewport([item] { item->update(); })` and hands it to the
//                rest of the app as IViewport / IRenderViewport. Every setter that changes the picture calls the
//                callback, so the item draws only on demand. It calls front->setDevicePixelRatio when the window's
//                ratio changes.
//   Render thread  createRenderer() returns a QQuickRhiItemRenderer that owns one HostedViewportRenderer r and
//                forwards: initialize(cb) -> r.initialize(rhi(), colorTexture());
//                          synchronize(item) -> r.synchronize(*front);   (UI thread blocked, as Qt guarantees)
//                          render(cb) -> if (r.render(cb)) update();     (true while a read-back is in flight)
//                and calls r.releaseResources() from its destructor. The item keeps sampleCount 1 and its default
//                colour format: the renderer brings its own MSAA and depth buffers and resolves into
//                colorTexture(); the item's own render target is not used.
// Results that come back from the GPU (captureImage, later picks) reach the UI thread through queued calls on an
// object owned by `front`, so they are dropped safely if the item is destroyed first.
#pragma once

#include "viewport.h"

#include <cstdint>
#include <functional>
#include <memory>

class QRhi;
class QRhiCommandBuffer;
class QRhiTexture;

namespace ray::render {

// UI-thread half. `requestUpdate` is called on the UI thread whenever a new frame is needed.
std::unique_ptr<IRenderViewport> makeHostedViewport(std::function<void()> requestUpdate);

namespace rhi {
class SceneRenderer;
}

// Render-thread half. Create, use and destroy it on the render thread.
class HostedViewportRenderer {
public:
    HostedViewportRenderer();
    ~HostedViewportRenderer(); // releases every RHI resource it owns
    HostedViewportRenderer(const HostedViewportRenderer&) = delete;
    HostedViewportRenderer& operator=(const HostedViewportRenderer&) = delete;

    // Whenever the QRhi or the colour texture changed (first frame, resize, scene graph re-initialised).
    void initialize(QRhi* rhi, QRhiTexture* colourTexture);
    // Copies the front's state. `front` must come from makeHostedViewport; anything else is ignored.
    void synchronize(IRenderViewport& front);
    // Records the frame into the command buffer. Returns true when another frame is needed soon (a read-back is
    // waiting for the GPU); the caller then schedules one (QQuickRhiItemRenderer::update()).
    bool render(QRhiCommandBuffer* cb);
    // Drops every RHI resource now (scene graph invalidated). initialize() brings them back.
    void releaseResources();

    // Optional, for hosts that submit frames themselves (the offscreen viewport, which is built on this seam):
    // after the frame of `cb` was submitted and waited for. Frees finished read-back targets and stores the GPU
    // time and the wall time of the frame. Qt Quick hosts do not call it; render() does the same one frame later.
    void frameSubmitted(QRhiCommandBuffer* cb, double wallMs);
    bool busy() const;                  // read-backs or later passes still wait for the GPU
    std::int64_t liveResources() const; // QRhi resources owned now (leak checks)

private:
    std::unique_ptr<rhi::SceneRenderer> renderer_;
};

} // namespace ray::render
