// A QRhi with no window for the offscreen viewport (task T-014).
#include "rhi_device.h"

#include "../tier_internal.h"

#include <rhi/qrhi_platform.h>

#include <QtCore/QByteArray>
#include <QtCore/QtGlobal>
#include <QtGui/QGuiApplication>
#include <QtGui/QSurfaceFormat>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace ray::render::rhi {

namespace {

std::string lowerEnv(const char* name)
{
    std::string value = qEnvironmentVariable(name).toStdString();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

std::vector<RhiBackend> candidates(RhiBackend wanted)
{
    if (wanted == RhiBackend::Auto) {
        const std::string env = lowerEnv("RAY_RHI_BACKEND");
        if (env == "d3d11")
            wanted = RhiBackend::D3D11;
        else if (env == "d3d12")
            wanted = RhiBackend::D3D12;
        else if (env == "metal")
            wanted = RhiBackend::Metal;
        else if (env == "opengl" || env == "gl")
            wanted = RhiBackend::OpenGL;
        else if (env == "null")
            wanted = RhiBackend::Null;
    }
    if (wanted != RhiBackend::Auto)
        return {wanted};
#if defined(Q_OS_WIN)
    return {RhiBackend::D3D11, RhiBackend::D3D12, RhiBackend::OpenGL};
#elif defined(Q_OS_MACOS)
    return {RhiBackend::Metal, RhiBackend::OpenGL};
#else
    return {RhiBackend::OpenGL};
#endif
}

const char* nameOf(RhiBackend b)
{
    switch (b) {
    case RhiBackend::D3D11:
        return "Direct3D 11";
    case RhiBackend::D3D12:
        return "Direct3D 12";
    case RhiBackend::Metal:
        return "Metal";
    case RhiBackend::OpenGL:
        return "OpenGL";
    case RhiBackend::Null:
        return "Null";
    case RhiBackend::Auto:
        break;
    }
    return "Auto";
}

} // namespace

OffscreenDevice::~OffscreenDevice()
{
    rhi.reset(); // before the surface it may use
    surface.reset();
}

Result<OffscreenDevice> createOffscreenDevice(RhiBackend backend, bool preferSoftware)
{
    preferSoftware = preferSoftware || detail::envValueIsTrue(qgetenv("RAY_RHI_SOFTWARE").constData());
    QRhi::Flags flags = QRhi::EnableTimestamps;
    if (preferSoftware)
        flags |= QRhi::PreferSoftwareRenderer;
    std::string failures;
    for (const RhiBackend b : candidates(backend)) {
        OffscreenDevice dev;
        switch (b) {
#if defined(Q_OS_WIN)
        case RhiBackend::D3D11: {
            QRhiD3D11InitParams params;
            dev.rhi.reset(QRhi::create(QRhi::D3D11, &params, flags));
            break;
        }
        case RhiBackend::D3D12: {
            QRhiD3D12InitParams params;
            dev.rhi.reset(QRhi::create(QRhi::D3D12, &params, flags));
            break;
        }
#endif
#if QT_CONFIG(metal)
        case RhiBackend::Metal: {
            QRhiMetalInitParams params;
            dev.rhi.reset(QRhi::create(QRhi::Metal, &params, flags));
            break;
        }
#endif
#if QT_CONFIG(opengl)
        case RhiBackend::OpenGL: {
            if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
                failures += " OpenGL: no QGuiApplication;";
                continue;
            }
            QRhiGles2InitParams params;
            params.format = QSurfaceFormat::defaultFormat();
            dev.surface.reset(QRhiGles2InitParams::newFallbackSurface(params.format));
            params.fallbackSurface = dev.surface.get();
            dev.rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, flags));
            break;
        }
#endif
        case RhiBackend::Null: {
            QRhiNullInitParams params;
            dev.rhi.reset(QRhi::create(QRhi::Null, &params, flags));
            break;
        }
        default:
            failures += std::string(" ") + nameOf(b) + ": not available on this platform;";
            continue;
        }
        if (dev.rhi)
            return dev;
        failures += std::string(" ") + nameOf(b) + ": could not be initialised;";
    }
    return tl::unexpected(Error{ErrorCode::Internal, "No graphics API could be initialised:" + failures, {}});
}

} // namespace ray::render::rhi
