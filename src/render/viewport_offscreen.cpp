// The offscreen viewport (task T-014): its own QRhi, its own colour texture, no window. It is built on the Qt Quick
// seam of viewport_rhi.h and calls it exactly as T-020's QQuickRhiItem will (initialize when the texture changes,
// synchronize, render), only on the UI thread and with frames it submits itself. So every offscreen test also
// exercises the seam.
#include "rhi/rhi_device.h"
#include "rhi/rhi_util.h"

#include <ray_render/viewport.h>
#include <ray_render/viewport_rhi.h>

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <cstring>
#include <memory>
#include <utility>

namespace ray::render {

namespace {

constexpr int kMaxSide = 16384;

class OffscreenViewport final : public IOffscreenViewport {
public:
    OffscreenViewport(rhi::OffscreenDevice device, int width, int height)
        : device_(std::move(device)), width_(width), height_(height)
    {
        front_ = makeHostedViewport([this] { schedule(); });
        renderer_ = std::make_unique<HostedViewportRenderer>();
    }

    ~OffscreenViewport() override
    {
        scheduler_.reset(); // no queued frame may run during teardown
        renderer_.reset();  // releases its resources while the QRhi is alive
        target_.reset();
        front_.reset();
        device_.rhi.reset(); // the QRhi, then its surface
        device_.surface.reset();
    }

    // ----- IOffscreenViewport -----
    void resize(int width, int height) override
    {
        if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide)
            return;
        if (width == width_ && height == height_)
            return;
        width_ = width;
        height_ = height;
        schedule();
    }

    Status renderNow() override
    {
        QRhi* rhi = device_.rhi.get();
        if (!target_ || target_->pixelSize() != QSize(width_, height_)) {
            // A new texture, as Qt Quick makes one on resize: the renderer is told before it draws again.
            renderer_->initialize(rhi, nullptr);
            target_.reset();
            target_ =
                rhi::own(counter_, rhi->newTexture(QRhiTexture::RGBA8, QSize(width_, height_), 1,
                                                   QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
            if (!target_->create()) {
                target_.reset();
                return tl::unexpected(Error{ErrorCode::Internal, "Could not create the viewport's colour texture", {}});
            }
            renderer_->initialize(rhi, target_.get());
        }
        QElapsedTimer timer;
        timer.start();
        renderer_->synchronize(*front_);
        QRhiCommandBuffer* cb = nullptr;
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb)
            return tl::unexpected(Error{ErrorCode::Internal, "Could not begin an offscreen frame", {}});
        renderer_->render(cb);
        if (rhi->endOffscreenFrame() != QRhi::FrameOpSuccess)
            return tl::unexpected(Error{ErrorCode::Internal, "Could not submit an offscreen frame", {}});
        // Offscreen frames wait for the GPU, so read-backs are complete here.
        renderer_->frameSubmitted(cb, double(timer.nsecsElapsed()) * 1e-6);
        if (renderer_->busy())
            schedule();
        return {};
    }

    // ----- IRenderViewport -----
    std::optional<Box3> sceneBounds() const override { return front_->sceneBounds(); }
    ViewportInfo info() const override
    {
        ViewportInfo i = front_->info();
        if (i.backend.empty() && device_.rhi) {
            i.backend = device_.rhi->backendName();
            i.device = device_.rhi->driverInfo().deviceName.toStdString();
        }
        i.liveRhiResources = renderer_->liveResources() + counter_.live.load();
        return i;
    }
    void setDevicePixelRatio(double r) override { front_->setDevicePixelRatio(r); }
    std::optional<SectionPlane> section() const override { return front_->section(); }
    void setDraftAnalysis(std::optional<DraftAnalysis> d) override { front_->setDraftAnalysis(d); }
    std::optional<DraftAnalysis> draftAnalysis() const override { return front_->draftAnalysis(); }
    void setPickMode(ItemId id, PickMode m) override { front_->setPickMode(id, m); }
    PickMode pickMode(ItemId id) const override { return front_->pickMode(id); }

    // ----- IViewport -----
    ItemId addMesh(std::shared_ptr<const kernel::Mesh> m, const Xform& x, MeshStyle s) override
    {
        return front_->addMesh(std::move(m), x, s);
    }
    void updateMesh(ItemId id, std::shared_ptr<const kernel::Mesh> m, std::optional<Box3> dirty) override
    {
        front_->updateMesh(id, std::move(m), dirty);
    }
    ItemId addBackplot(std::shared_ptr<const sim::Backplot> b) override { return front_->addBackplot(std::move(b)); }
    void setBackplotRange(ItemId id, std::uint32_t a, std::uint32_t b) override { front_->setBackplotRange(id, a, b); }
    ItemId addLines(std::span<const Vec3> s, Token c, float w, bool d) override { return front_->addLines(s, c, w, d); }
    ItemId addToolGlyph(std::span<const Vec2> p, Token c) override { return front_->addToolGlyph(p, c); }
    void setTransform(ItemId id, const Xform& x) override { front_->setTransform(id, x); }
    void setStyle(ItemId id, MeshStyle s) override { front_->setStyle(id, s); }
    void setVisible(ItemId id, bool v) override { front_->setVisible(id, v); }
    void remove(ItemId id) override { front_->remove(id); }
    void setHighlight(std::span<const PickHit> s, std::optional<PickHit> h) override { front_->setHighlight(s, h); }
    void setCamera(const Camera& c) override { front_->setCamera(c); }
    Camera camera() const override { return front_->camera(); }
    void setSection(std::optional<SectionPlane> s) override { front_->setSection(s); }
    void setTheme(std::shared_ptr<const Theme> t) override { front_->setTheme(std::move(t)); }
    void setPreset(const TierPreset& p) override { front_->setPreset(p); }
    void pick(int x, int y, std::uint8_t f, std::function<void(std::optional<PickHit>)> cb) override
    {
        front_->pick(x, y, f, std::move(cb));
    }
    void pickRect(int x0, int y0, int x1, int y1, std::uint8_t f, std::function<void(std::vector<PickHit>)> cb) override
    {
        front_->pickRect(x0, y0, x1, y1, f, std::move(cb));
    }
    void requestFrame() override { front_->requestFrame(); }
    void captureImage(int w, int h, std::function<void(std::vector<std::byte> png)> cb) override
    {
        front_->captureImage(w, h, std::move(cb));
    }
    FrameStats lastFrameStats() const override { return front_->lastFrameStats(); }
    // This viewport's UI thread is its render thread, so the handle itself is handed out (render.h).
    Backdrop blurredBackdropTexture() override { return renderer_->backdrop(); }
    void setBackdropEnabled(bool on) override { front_->setBackdropEnabled(on); }

    Result<Image> readBackdrop() override
    {
        const Backdrop b = renderer_->backdrop();
        auto* texture = static_cast<QRhiTexture*>(b.rhiTexture);
        if (!texture)
            return tl::unexpected(Error{ErrorCode::NotFound, "The glass backdrop is disabled or not drawn yet", {}});
        QRhi* rhi = device_.rhi.get();
        QRhiCommandBuffer* cb = nullptr;
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb)
            return tl::unexpected(Error{ErrorCode::Internal, "Could not begin an offscreen frame", {}});
        QRhiReadbackResult result;
        QRhiResourceUpdateBatch* u = rhi->nextResourceUpdateBatch();
        u->readBackTexture(QRhiReadbackDescription(texture), &result);
        cb->resourceUpdate(u);
        if (rhi->endOffscreenFrame() != QRhi::FrameOpSuccess)
            return tl::unexpected(Error{ErrorCode::Internal, "Could not submit an offscreen frame", {}});
        // endOffscreenFrame waited for the GPU: the data is here.
        Image image;
        image.width = result.pixelSize.width();
        image.height = result.pixelSize.height();
        const std::size_t row = std::size_t(image.width) * 4;
        if (image.width != b.width || image.height != b.height ||
            std::size_t(result.data.size()) < row * std::size_t(image.height))
            return tl::unexpected(Error{ErrorCode::Internal, "The glass backdrop read-back is incomplete", {}});
        image.rgba.resize(row * std::size_t(image.height));
        const auto* src = reinterpret_cast<const std::uint8_t*>(result.data.constData());
        for (int y = 0; y < image.height; ++y) {
            const int from = b.yUp ? image.height - 1 - y : y;
            std::memcpy(&image.rgba[std::size_t(y) * row], src + std::size_t(from) * row, row);
        }
        return image;
    }

private:
    // One queued frame at most; it runs from the event loop.
    void schedule()
    {
        if (framePending_ || !scheduler_)
            return;
        framePending_ = true;
        QTimer::singleShot(0, scheduler_.get(), [this] {
            framePending_ = false;
            if (Status ok = renderNow(); !ok)
                ray::log(LogLevel::Error, "render", ok.error().message);
        });
    }

    rhi::OffscreenDevice device_;
    rhi::ResourceCounter counter_; // the colour texture, which this host owns as Qt Quick owns the item's
    std::unique_ptr<IRenderViewport> front_;
    std::unique_ptr<HostedViewportRenderer> renderer_;
    rhi::Owned<QRhiTexture> target_;
    std::unique_ptr<QObject> scheduler_ = std::make_unique<QObject>();
    bool framePending_ = false;
    int width_, height_;
};

} // namespace

Result<std::unique_ptr<IOffscreenViewport>> makeOffscreenViewport(const OffscreenOptions& options)
{
    if (options.width <= 0 || options.height <= 0 || options.width > kMaxSide || options.height > kMaxSide)
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "Viewport size out of range", {}});
    auto device = rhi::createOffscreenDevice(options.backend, options.preferSoftware);
    if (!device)
        return tl::unexpected(device.error());
    return std::unique_ptr<IOffscreenViewport>(
        std::make_unique<OffscreenViewport>(std::move(*device), options.width, options.height));
}

} // namespace ray::render
