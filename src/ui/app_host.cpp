#include "app_host.h"

#include "app_context.h"
#include "theme_object.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickGraphicsConfiguration>
#include <QtQuick/QQuickWindow>

#include <algorithm>

namespace ray::ui {

AppHost::AppHost(const AppOptions& options, QString cacheDir, QObject* parent)
    : QObject(parent), options_(options), cacheDir_(cacheDir.isEmpty() ? cacheDirectory() : std::move(cacheDir)),
      theme_(std::make_unique<ThemeObject>(QStringList())), context_(std::make_unique<AppContext>(nullptr))
{
    if (!theme_->loadErrors().isEmpty()) {
        optionError_ = theme_->loadErrors().join(QLatin1Char('\n'));
    }
    if (!options_.theme.isEmpty() && !theme_->setMode(options_.theme)) {
        optionError_ = QStringLiteral("unknown theme '%1'; use system, dark, light or one of: %2")
                           .arg(options_.theme, theme_->themeIds().join(QStringLiteral(", ")));
    }
    if (options_.glass) {
        context_->setGlass(*options_.glass);
    }
    context_->setPage(options_.gallery ? QStringLiteral("gallery") : QStringLiteral("shell"));
    context_->setScreenshotMode(!options_.screenshot.isEmpty());

    // Text without an explicit font gets the theme's UI font.
    QGuiApplication::setFont(theme_->uiFont());
    connect(theme_.get(), &ThemeObject::themeChanged, this, [this] { QGuiApplication::setFont(theme_->uiFont()); });
}

AppHost::~AppHost()
{
    // The window belongs to the engine; destroy both before the singletons they reference.
    engine_.reset();
}

QString AppHost::cacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

void AppHost::configureProcess()
{
    QCoreApplication::setOrganizationName(QStringLiteral("RayCAD"));
    QCoreApplication::setApplicationName(QStringLiteral("RayCAD"));
    QCoreApplication::setApplicationVersion(QStringLiteral(RAY_VERSION_STRING));
}

bool AppHost::load()
{
    if (engine_ != nullptr) {
        return window_ != nullptr;
    }
    // Caches: QML byte code (QML_DISK_CACHE_PATH, read when the engine compiles) and the RHI pipeline cache.
    const QString qmlCache = QDir(cacheDir_).filePath(QStringLiteral("qml"));
    QDir().mkpath(qmlCache);
    if (!qEnvironmentVariableIsSet("QML_DISK_CACHE_PATH")) {
        qputenv("QML_DISK_CACHE_PATH", QDir::toNativeSeparators(qmlCache).toLocal8Bit());
    }
    // Crisper small text on desktop monitors than distance-field text.
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);

    engine_ = std::make_unique<QQmlApplicationEngine>();
    ThemeObject::provideFor(engine_.get(), theme_.get());
    AppContext::provideFor(engine_.get(), context_.get());
    connect(engine_.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError>& list) {
        for (const QQmlError& e : list) {
            warnings_ << e.toString();
        }
    });

    engine_->loadFromModule("RayCad.Ui", "Main");
    const QList<QObject*> roots = engine_->rootObjects();
    window_ = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(roots.constFirst());
    if (window_ == nullptr) {
        error_ = roots.isEmpty() ? QStringLiteral("the main window failed to load")
                                 : QStringLiteral("the root QML object is not a window");
        if (!warnings_.isEmpty()) {
            error_ += QStringLiteral(":\n") + warnings_.join(QLatin1Char('\n'));
        }
        return false;
    }

    // Before the window is exposed for the first time (Main.qml starts hidden).
    QQuickGraphicsConfiguration config = window_->graphicsConfiguration();
    const QString pipelineCache = QDir(cacheDir_).filePath(QStringLiteral("rhi-pipeline.bin"));
    config.setPipelineCacheSaveFile(pipelineCache);
    config.setPipelineCacheLoadFile(pipelineCache);
    window_->setGraphicsConfiguration(config);

    QSize size = options_.size;
    if (!size.isValid()) {
        const QScreen* screen = window_->screen();
        const QSize available = screen != nullptr ? screen->availableSize() : QSize(1600, 1000);
        size = QSize(std::min(1600, available.width() * 9 / 10), std::min(1000, available.height() * 9 / 10));
    }
    window_->resize(size);
    if (const QScreen* screen = window_->screen()) {
        const QRect area = screen->availableGeometry();
        window_->setPosition(area.center() - QPoint(size.width() / 2, size.height() / 2));
    }
    return true;
}

void AppHost::show()
{
    if (window_ != nullptr) {
        window_->show();
    }
}

void AppHost::setApp(api::IApp* app)
{
    context_->setApp(app);
}

} // namespace ray::ui
