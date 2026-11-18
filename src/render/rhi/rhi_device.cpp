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
#include <utility>
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
            // Ask for OpenGL 3.3 first. QRhi takes the GL version from the context's reported format, and Qt's
            // offscreen platform reports the version that was requested, not the one Mesa created (4.5 for
            // llvmpipe): with the default 2.0 request QRhi turned instancing off, and the wide-line pipeline (smooth
            // edges, addLines) with it. The default format remains the fallback for drivers without 3.3.
            QSurfaceFormat wanted = QSurfaceFormat::defaultFormat();
            std::vector<QSurfaceFormat> formats;
            if (wanted.renderableType() != QSurfaceFormat::OpenGLES &&
                std::make_pair(wanted.majorVersion(), wanted.minorVersion()) < std::make_pair(3, 3)) {
                QSurfaceFormat gl33 = wanted;
                gl33.setVersion(3, 3);
                formats.push_back(gl33);
            }
            formats.push_back(wanted);
            for (const QSurfaceFormat& format : formats) {
                QRhiGles2InitParams params;
                params.format = format;
                dev.surface.reset(QRhiGles2InitParams::newFallbackSurface(params.format));
                params.fallbackSurface = dev.surface.get();
                dev.rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, flags));
                if (dev.rhi)
                    break;
                dev.surface.reset();
            }
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
        if (dev.rhi) {
            detail::noteGraphicsDriverLoaded();
            return dev;
        }
        failures += std::string(" ") + nameOf(b) + ": could not be initialised;";
    }
    return tl::unexpected(Error{ErrorCode::Internal, "No graphics API could be initialised:" + failures, {}});
}

} // namespace ray::render::rhi
