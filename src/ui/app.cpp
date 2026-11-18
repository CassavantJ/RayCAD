#include <ray_ui/app.h>

#include "app_clock.h"
#include "app_host.h"
#include "app_options.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>

#include <atomic>
#include <cstdio>

namespace ray::ui {
namespace {

const char* graphicsApiName(QQuickWindow* window)
{
    const QSGRendererInterface* ri = window->rendererInterface();
    switch (ri != nullptr ? ri->graphicsApi() : QSGRendererInterface::Unknown) {
    case QSGRendererInterface::Direct3D11:
        return "Direct3D 11";
    case QSGRendererInterface::Direct3D12:
        return "Direct3D 12";
    case QSGRendererInterface::Metal:
        return "Metal";
    case QSGRendererInterface::OpenGL:
        return "OpenGL";
    case QSGRendererInterface::Vulkan:
        return "Vulkan";
    case QSGRendererInterface::Software:
        return "software";
    default:
        return "unknown";
    }
}

constexpr int kExitUsage = 1;
constexpr int kExitLoad = 2;
constexpr int kExitNoFrame = 3;
constexpr int kExitScreenshot = 4;
constexpr int kNoFrameTimeoutMs = 20000;
// After the first frame: let delayed bindings, fonts and the second layout pass settle before grabbing.
constexpr int kScreenshotSettleMs = 400;

} // namespace

int runApp(int argc, char** argv)
{
    const double tMain = msSinceProcessStart();
    AppHost::configureProcess();
    QGuiApplication app(argc, argv);
    const double tApp = msSinceProcessStart();

    const AppOptions options = parseOptions(QGuiApplication::arguments());
    if (!options.error.isEmpty()) {
        std::fprintf(stderr, "raycad: %s\n%s", qPrintable(options.error), qPrintable(usageText()));
        return kExitUsage;
    }
    if (options.help) {
        std::printf("%s", qPrintable(usageText()));
        return 0;
    }
    if (options.version) {
        std::printf("raycad %s\n", RAY_VERSION_STRING);
        return 0;
    }

    AppHost host(options);
    if (!host.optionError().isEmpty()) {
        std::fprintf(stderr, "raycad: %s\n", qPrintable(host.optionError()));
        return kExitUsage;
    }
    if (!host.load()) {
        std::fprintf(stderr, "raycad: %s\n", qPrintable(host.errorString()));
        return kExitLoad;
    }
    const double tQml = msSinceProcessStart();
    QQuickWindow* window = host.window();

    const bool exitAfterFrame = options.smoke || options.benchStart || !options.screenshot.isEmpty();
    std::atomic<bool> seen{false};
    std::atomic<double> tFrame{0};
    auto onFirstFrame = [&] {
        if (options.benchStart) {
            std::printf("raycad: bench-start first_frame_ms=%.1f main_ms=%.1f app_ms=%.1f qml_ms=%.1f backend=%s\n",
                        tFrame.load(), tMain, tApp, tQml, graphicsApiName(window));
        }
        if (options.smoke) {
            std::printf("raycad: smoke ok, first frame rendered (%s)\n", graphicsApiName(window));
        }
        std::fflush(stdout);
        if (options.screenshot.isEmpty()) {
            if (exitAfterFrame) {
                QCoreApplication::exit(0);
            }
            return;
        }
        QTimer::singleShot(kScreenshotSettleMs, window, [&] {
            const QImage image = window->grabWindow();
            const QString path = QFileInfo(options.screenshot).absoluteFilePath();
            QDir().mkpath(QFileInfo(path).absolutePath());
            if (image.isNull() || !image.save(path, "PNG")) {
                std::fprintf(stderr, "raycad: could not save the screenshot to %s\n", qPrintable(path));
                QCoreApplication::exit(kExitScreenshot);
                return;
            }
            std::printf("raycad: screenshot %dx%d saved to %s\n", image.width(), image.height(), qPrintable(path));
            std::fflush(stdout);
            QCoreApplication::exit(0);
        });
    };
    // frameSwapped comes from the render thread: take the time there, continue on the GUI thread.
    QObject::connect(
        window, &QQuickWindow::frameSwapped, window,
        [&] {
            if (!seen.exchange(true)) {
                tFrame = msSinceProcessStart();
                QMetaObject::invokeMethod(&app, onFirstFrame, Qt::QueuedConnection);
            }
        },
        Qt::DirectConnection);
    if (exitAfterFrame) {
        QTimer::singleShot(kNoFrameTimeoutMs, &app, [] {
            std::fprintf(stderr, "raycad: no frame was rendered within 20 seconds\n");
            QCoreApplication::exit(kExitNoFrame);
        });
    }

    host.show();
    return QGuiApplication::exec();
}

} // namespace ray::ui
