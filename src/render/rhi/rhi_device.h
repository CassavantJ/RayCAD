// Private to src/render: a QRhi of our own, with no window, for the offscreen viewport (task T-014).
#pragma once

#include <ray_render/viewport.h>

#include <rhi/qrhi.h>

#include <QtGui/QOffscreenSurface>

#include <memory>
#include <string>

namespace ray::render::rhi {

struct OffscreenDevice {
    std::unique_ptr<QOffscreenSurface> surface; // OpenGL only; must outlive the QRhi
    std::unique_ptr<QRhi> rhi;
    ~OffscreenDevice();
    OffscreenDevice() = default;
    OffscreenDevice(OffscreenDevice&&) = default;
    OffscreenDevice& operator=(OffscreenDevice&&) = delete; // would free the surface before the QRhi
};

// Tries the backends in order (see OffscreenOptions); Internal with every backend's failure listed if none works.
// Needs a QGuiApplication for OpenGL. Timestamps are enabled, so FrameStats::gpuMs is filled where supported.
Result<OffscreenDevice> createOffscreenDevice(RhiBackend backend, bool preferSoftware);

} // namespace ray::render::rhi
