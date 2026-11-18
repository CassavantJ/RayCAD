#include <ray_ui/app.h>

#include "app_clock.h"
#include "app_host.h"
#include "app_options.h"
#include "files.h"
#include "queued_call.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "viewport_item.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>

#include <atomic>
#include <cstdio>
#include <memory>

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
// --screenshot with a file: how long the import, meshing and refinement may take.
constexpr int kOpenTimeoutMs = 180000;
constexpr qint64 kSelectTimeoutMs = 3000;

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
    host.loadSettings(); // T-024: the user's settings ($RAYCAD_USER_DATA_DIR or the per-user folder); one small file
    if (!host.load()) {
        std::fprintf(stderr, "raycad: %s\n", qPrintable(host.errorString()));
        return kExitLoad;
    }
    const double tQml = msSinceProcessStart();
    QQuickWindow* window = host.window();
    // Files on the command line open once the session exists (after the first frame): the first one as the
    // document, further STEP/IGES files imported into it.
    const QStringList openFiles = options.files;
    const bool openFile = !openFiles.isEmpty();

    const bool exitAfterFrame = options.smoke || options.benchStart || !options.screenshot.isEmpty();
    std::atomic<bool> seen{false};
    std::atomic<double> tFrame{0};
    auto saveScreenshot = [&] {
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
    };
    // With a file: wait until it is shown at the tier's tolerance, optionally select, then grab.
    auto screenshotWhenShown = [&] {
        auto* poll = new QTimer(&app);
        auto started = std::make_shared<QElapsedTimer>();
        started->start();
        auto stage = std::make_shared<int>(0);
        auto selectedAt = std::make_shared<qint64>(0);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, &app, [&, poll, started, stage, selectedAt] {
            SceneController* scene = host.scene();
            SelectionModel* selection = host.selection();
            ViewportItem* view = host.viewport();
            const bool shown =
                !host.files()->busy() && scene->ready() && !scene->refining() && view != nullptr && !view->animating();
            if (started->elapsed() > kOpenTimeoutMs) {
                std::fprintf(stderr, "raycad: the file was not shown within %d seconds\n", kOpenTimeoutMs / 1000);
                poll->stop();
                QCoreApplication::exit(kExitNoFrame);
                return;
            }
            if (*stage == 0 && shown) {
                *stage = 1;
                *selectedAt = started->elapsed();
                if (options.selectAt && view != nullptr) {
                    const QPointF at = view->mapFromScene(*options.selectAt);
                    view->selectAt(at.x(), at.y());
                }
                return;
            }
            // The pick is answered by a later frame: wait for it (or give up after a while: nothing was there).
            const bool picked =
                !options.selectAt || selection->count() > 0 || started->elapsed() - *selectedAt > kSelectTimeoutMs;
            if (*stage == 1 && picked && !selection->pending() && !selection->infoBusy()) {
                *stage = 2;
                poll->stop();
                QTimer::singleShot(kScreenshotSettleMs, window, saveScreenshot);
            }
        });
        poll->start();
    };
    auto onFirstFrame = [&] {
        if (options.benchStart) {
            std::printf("raycad: bench-start first_frame_ms=%.1f main_ms=%.1f app_ms=%.1f qml_ms=%.1f backend=%s\n",
                        tFrame.load(), tMain, tApp, tQml, graphicsApiName(window));
        }
        if (options.smoke) {
            std::printf("raycad: smoke ok, first frame rendered (%s)\n", graphicsApiName(window));
        }
        std::fflush(stdout);
        if (options.smoke && options.screenshot.isEmpty()) {
            QCoreApplication::exit(0);
            return;
        }
        // The application session (T-019): nothing from the kernel before the first frame.
        const double tSession0 = msSinceProcessStart();
        const bool started = host.startSession();
        if (options.benchStart) {
            // Reported for PLAN.md 9 (the start screen fills in with the recent files once the session exists).
            std::printf("raycad: bench-start session_ms=%.1f\n", msSinceProcessStart() - tSession0);
            std::fflush(stdout);
            if (options.screenshot.isEmpty()) {
                QCoreApplication::exit(0);
                return;
            }
        }
        if (started && openFile) {
            if (exitAfterFrame) {
                QObject::connect(host.files(), &Files::openFailed, &app, [](const QString& message) {
                    std::fprintf(stderr, "raycad: %s\n", qPrintable(message));
                    QCoreApplication::exit(kExitLoad);
                });
            }
            host.files()->openPaths(openFiles);
        }
        if (options.screenshot.isEmpty()) {
            return;
        }
        if (openFile) {
            screenshotWhenShown();
            return;
        }
        QTimer::singleShot(kScreenshotSettleMs, window, saveScreenshot);
    };
    // frameSwapped comes from the render thread: take the time there, continue on the GUI thread.
    QObject::connect(
        window, &QQuickWindow::frameSwapped, window,
        [&] {
            if (!seen.exchange(true)) {
                tFrame = msSinceProcessStart();
                invokeQueued(&app, onFirstFrame);
            }
        },
        Qt::DirectConnection);
    if (exitAfterFrame) {
        QTimer::singleShot(kNoFrameTimeoutMs, &app, [&] {
            if (seen) {
                return;
            }
            std::fprintf(stderr, "raycad: no frame was rendered within 20 seconds\n");
            QCoreApplication::exit(kExitNoFrame);
        });
    }

    host.show();
    return QGuiApplication::exec();
}

} // namespace ray::ui
