#include "app_host.h"

#include "app_context.h"
#include "files.h"
#include "palette_commands.h"
#include "scene_controller.h"
#include "scene_jobs.h"
#include "section_model.h"
#include "selection_model.h"
#include "session.h"
#include "session_recovery.h"
#include "settings.h"
#include "shortcuts.h"
#include "theme_object.h"
#include "units.h"
#include "viewport_item.h"

#include <ray_api/viewer.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickGraphicsConfiguration>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <vector>

namespace ray::ui {

AppHost::AppHost(const AppOptions& options, QString cacheDir, QObject* parent)
    : QObject(parent), options_(options), cacheDir_(cacheDir.isEmpty() ? cacheDirectory() : std::move(cacheDir)),
      dispatcher_(std::make_unique<QtUiDispatcher>()), theme_(std::make_unique<ThemeObject>(QStringList())),
      context_(std::make_unique<AppContext>(nullptr))
{
    // The viewer (T-020). Its job system is made on first use, so nothing heavy happens before the first frame.
    scene_ = std::make_unique<SceneController>([this] { return jobs(); }, context_.get());
    selection_ = std::make_unique<SelectionModel>(scene_.get());
    // The session (T-019): observed by Session, driven by Files. Both work without one (start screen only).
    session_ = std::make_unique<Session>(scene_.get(), context_.get());
    files_ = std::make_unique<Files>(session_.get(), context_.get());
    context_->setActionHandler(
        [files = files_.get()](const QString& id, const QString& args) { return files->handleAction(id, args); });
    connect(files_.get(), &Files::quitApproved, this, [this] {
        quitApproved_ = true;
        if (window_ != nullptr) {
            window_->close();
        }
    });
    // T-021: commands and shortcuts. Cheap: nothing is read until the first key press or the palette needs it.
    commands_ =
        std::make_unique<CommandCatalog>(context_.get(), scene_.get(), selection_.get(), [this] { return viewport(); });
    shortcuts_ = std::make_unique<ShortcutManager>(commands_.get());
    commands_->setShortcuts(shortcuts_.get());
    // T-023: section view and draft analysis, also run as view.section.* / view.draft.* commands.
    section_ = std::make_unique<SectionModel>(scene_.get(), selection_.get(), [this] { return viewport(); });
    commands_->setSectionView(section_.get());
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
    // T-024: units and settings, after the command-line theme and glass (those are not saved as settings).
    units_ = std::make_unique<UnitsObject>(session_.get());
    settings_ = std::make_unique<SettingsObject>(theme_.get(), context_.get(), scene_.get(), units_.get(),
                                                 [this] { return jobs(); });
    connect(commands_.get(), &CommandCatalog::settingsRequested, settings_.get(), &SettingsObject::open);
    section_->setUnits(units_.get()); // T-023: the section offset in the display units

    // Text without an explicit font gets the theme's UI font.
    QGuiApplication::setFont(theme_->uiFont());
    connect(theme_.get(), &ThemeObject::themeChanged, this, [this] { QGuiApplication::setFont(theme_->uiFont()); });
}

AppHost::~AppHost()
{
    // The window belongs to the engine; destroy both before the singletons they reference.
    if (window_ != nullptr) {
        if (persistGeometry()) {
            saveWindowGeometry(); // only while it is shown; else what was tracked while it was
        }
        settings_->flush();
        window_->removeEventFilter(this);
    }
    engine_.reset();
    window_ = nullptr;
    settings_->attachView(nullptr);
    // Then what observes the session, then the viewer's objects (their jobs are cancelled), then the session (it
    // cancels and waits for its jobs and closes its documents), then the job system, which waits for the rest.
    shortcuts_.reset();
    commands_.reset();
    section_.reset();
    context_->setActionHandler({});
    files_.reset();
    session_.reset();
    context_->setApp(nullptr);
    selection_.reset();
    scene_.reset();
    app_ = nullptr;
    ownApp_.reset();
    ownJobs_.reset();
}

IJobSystem* AppHost::jobs()
{
    if (app_ != nullptr)
        return &app_->jobs();
    if (!ownJobs_)
        ownJobs_ = api::viewer::makeViewerJobs(*dispatcher_);
    return ownJobs_.get();
}

bool AppHost::startSession(std::optional<std::string> userDataDir)
{
    if (app_ == nullptr) {
        // One job system per process: the one the viewer uses (made now if nothing needed it yet).
        if (!ownJobs_) {
            ownJobs_ = api::viewer::makeViewerJobs(*dispatcher_);
        }
        api::AppOptions options;
        options.dispatcher = dispatcher_.get();
        options.jobs = ownJobs_.get();
        options.userDataDir = std::move(userDataDir);
        ownApp_ = api::makeApp(std::move(options));
        if (!ownApp_) {
            context_->showMessage(tr("The application session could not start; files cannot be opened."), true);
            return false;
        }
        setApp(ownApp_.get());
    }
    if (!recovery_) {
        recovery_ = makeAppRecoverySource(*app_);
    }
    session_->setRecoverySource(std::move(recovery_));
    session_->checkRecovery();
    // T-024: hardware detection, on a worker and only now (after the first frame): the tier and its reason.
    if (settingsLoaded_) {
        settings_->requestHardware();
    }
    return true;
}

void AppHost::loadSettings(std::optional<std::string> userDataDir)
{
    settings_->load(userDataDir, !options_.theme.isEmpty(), options_.glass.has_value());
    settingsLoaded_ = true;
}

void AppHost::saveWindowGeometry()
{
    saveWindowGeometry(true);
}

void AppHost::saveWindowGeometry(bool now)
{
    // A hidden or minimized window has no geometry worth keeping (and a closed one may report a stale one).
    if (window_ == nullptr || !window_->isVisible() || window_->visibility() == QWindow::Minimized ||
        window_->visibility() == QWindow::FullScreen) {
        return;
    }
    const QRect g = window_->geometry();
    settings_->saveWindow(g.x(), g.y(), g.width(), g.height(), window_->visibility() == QWindow::Maximized, now);
}

void AppHost::setRecoverySource(std::unique_ptr<IRecoverySource> source)
{
    recovery_ = std::move(source);
}

bool AppHost::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == window_ && event->type() == QEvent::Close && !quitApproved_ && files_ &&
        files_->hasUnsavedChanges()) {
        // Ask first (Save / Discard / Cancel); Files::quitApproved closes the window again once answered.
        event->ignore();
        QTimer::singleShot(0, files_.get(), [files = files_.get()] { files->requestQuit(); });
        return true;
    }
    if (watched == window_ && event->type() == QEvent::Close && persistGeometry()) {
        saveWindowGeometry();
    }
    return QObject::eventFilter(watched, event);
}

ViewportItem* AppHost::viewport() const
{
    if (window_ == nullptr)
        return nullptr;
    // Items a Loader creates are not QObject children of the window's content: walk the visual tree.
    std::vector<QQuickItem*> todo{window_->contentItem()};
    while (!todo.empty()) {
        QQuickItem* item = todo.back();
        todo.pop_back();
        if (auto* view = qobject_cast<ViewportItem*>(item))
            return view;
        const QList<QQuickItem*> children = item->childItems();
        todo.insert(todo.end(), children.begin(), children.end());
    }
    return nullptr;
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
    SceneController::provideFor(engine_.get(), scene_.get());
    SelectionModel::provideFor(engine_.get(), selection_.get());
    Session::provideFor(engine_.get(), session_.get());
    Files::provideFor(engine_.get(), files_.get());
    CommandCatalog::provideFor(engine_.get(), commands_.get());
    ShortcutManager::provideFor(engine_.get(), shortcuts_.get());
    UnitsObject::provideFor(engine_.get(), units_.get());
    SettingsObject::provideFor(engine_.get(), settings_.get());
    SectionModel::provideFor(engine_.get(), section_.get());
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
    shortcuts_->attach(window_);
    commands_->attachWindow(window_);

    // Before the window is exposed for the first time (Main.qml starts hidden).
    QQuickGraphicsConfiguration config = window_->graphicsConfiguration();
    const QString pipelineCache = QDir(cacheDir_).filePath(QStringLiteral("rhi-pipeline.bin"));
    config.setPipelineCacheSaveFile(pipelineCache);
    config.setPipelineCacheLoadFile(pipelineCache);
    // GPU timestamps for the viewport's IViewport::lastFrameStats (T-020).
    config.setTimestamps(true);
    window_->setGraphicsConfiguration(config);
    window_->installEventFilter(this);

    QSize size = options_.size;
    if (!size.isValid()) {
        const QScreen* screen = window_->screen();
        const QSize available = screen != nullptr ? screen->availableSize() : QSize(1600, 1000);
        size = QSize(std::min(1600, available.width() * 9 / 10), std::min(1000, available.height() * 9 / 10));
    }
    // T-024: the geometry saved when the window last closed, kept on the screens there are now.
    std::optional<api::WindowGeometry> saved;
    if (settingsLoaded_ && !options_.size.isValid()) {
        saved = settings_->current().window;
    }
    if (saved) {
        window_->setGeometry(clampToScreens(QRect(saved->x, saved->y, saved->width, saved->height),
                                            QSize(window_->minimumWidth(), window_->minimumHeight())));
        restoreMaximized_ = saved->maximized;
    } else {
        window_->resize(size);
        if (const QScreen* screen = window_->screen()) {
            const QRect area = screen->availableGeometry();
            window_->setPosition(area.center() - QPoint(size.width() / 2, size.height() / 2));
        }
    }
    settings_->attachView(viewport());
    // Kept up to date as the window moves (saved after a pause), so a crash does not lose it either.
    if (persistGeometry()) {
        const auto track = [this] {
            saveWindowGeometry(false);
        };
        connect(window_, &QWindow::xChanged, this, track);
        connect(window_, &QWindow::yChanged, this, track);
        connect(window_, &QWindow::widthChanged, this, track);
        connect(window_, &QWindow::heightChanged, this, track);
        connect(window_, &QWindow::visibilityChanged, this, track);
    }
    return true;
}

QRect AppHost::clampToScreens(const QRect& wanted, const QSize& minimum)
{
    // The screen that shows most of it; none (a monitor was unplugged): the primary screen.
    const QScreen* best = nullptr;
    qint64 bestArea = 0;
    for (const QScreen* screen : QGuiApplication::screens()) {
        const QRect part = screen->availableGeometry().intersected(wanted);
        const qint64 area = qint64(part.width()) * part.height();
        if (area > bestArea) {
            bestArea = area;
            best = screen;
        }
    }
    const bool offScreen = best == nullptr;
    if (offScreen) {
        best = QGuiApplication::primaryScreen();
    }
    if (best == nullptr) {
        return wanted;
    }
    const QRect area = best->availableGeometry();
    const int w = std::clamp(wanted.width(), std::min(minimum.width(), area.width()), area.width());
    const int h = std::clamp(wanted.height(), std::min(minimum.height(), area.height()), area.height());
    if (offScreen) {
        // Centred from the edges, not from QRect::center(): that rounds down ((left + right) / 2 with right =
        // left + width - 1), which put a window as wide as an even-sized screen one pixel off its left edge.
        return QRect(area.left() + (area.width() - w) / 2, area.top() + (area.height() - h) / 2, w, h);
    }
    // Wholly on that screen: the title bar can always be reached.
    const int x = std::clamp(wanted.x(), area.left(), area.left() + area.width() - w);
    const int y = std::clamp(wanted.y(), area.top(), area.top() + area.height() - h);
    return QRect(x, y, w, h);
}

void AppHost::show()
{
    if (window_ != nullptr) {
        if (restoreMaximized_) {
            window_->showMaximized();
        } else {
            window_->show();
        }
    }
}

void AppHost::setApp(api::IApp* app)
{
    if (app == app_) {
        return;
    }
    // The scene's jobs belong to the job system of the session they came from.
    scene_->cancelJobs(true);
    app_ = app;
    context_->setApp(app);
    session_->setApp(app);
    settings_->setApp(app);
}

} // namespace ray::ui
