// The application object: owns the QML engine, the Theme and AppContext singletons and the main window.
// runApp() (app.cpp) wraps it with the command-line options; tests create one directly.
//
// Start-up rules from spike S2 (PLAN.md 9, T-018):
//   - Qt Quick keeps the platform's default graphics API (Direct3D 11 on Windows, Metal on macOS); nothing here
//     forces OpenGL.
//   - The QML disk cache and the RHI pipeline cache live in a per-user cache folder from QStandardPaths, set
//     explicitly before the engine and the window exist.
//   - Nothing from the kernel runs before the first frame; the shell creates only what the first screen shows.
//
// The application session (T-019): startSession() makes the real one, ray::api::makeApp with this host's Qt
// dispatcher and its one job system (the viewer shares it: one job system per process), and runs the start-up
// crash-recovery check. runApp calls it right after the first frame. Tests may instead set a session of their own.
#pragma once

#include "app_options.h"

#include <QtCore/QObject>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>
#include <optional>
#include <string>

class QQmlApplicationEngine;
class QQuickWindow;

namespace ray {
class IJobSystem;
class IUiDispatcher;
} // namespace ray
namespace ray::api {
class IApp;
}

namespace ray::ui {

class AppContext;
class CommandCatalog;
class Files;
class IRecoverySource;
class SceneController;
class Session;
class SelectionModel;
class SettingsObject;
class ShortcutManager;
class ThemeObject;
class UnitsObject;
class ViewportItem;

class AppHost : public QObject {
    Q_OBJECT
public:
    // A QGuiApplication must exist. `cacheDir` empty means the per-user cache folder (cacheDirectory()).
    explicit AppHost(const AppOptions& options, QString cacheDir = {}, QObject* parent = nullptr);
    ~AppHost() override;

    // Problems found while applying the options (an unknown --theme); empty if none.
    QString optionError() const { return optionError_; }

    // T-024: reads the user settings (cheap: one small file) and applies them: theme and glass (unless the command
    // line set them), units, tier preset and overrides, and, in load(), the window geometry (clamped to the screens)
    // and the navigation preset. Call before load(). Without it nothing from the user's profile is read or applied
    // (tests build their windows that way). `userDataDir` as for startSession.
    void loadSettings(std::optional<std::string> userDataDir = std::nullopt);
    // Saves the main window's geometry into the settings (also done when the window closes).
    void saveWindowGeometry();

    // Creates the main window (hidden). False if the QML failed to load; errorString() says why.
    bool load();
    // Shows the window; the first frame follows.
    void show();

    QQuickWindow* window() const { return window_; }
    QQmlApplicationEngine* engine() const { return engine_.get(); }
    ThemeObject* theme() const { return theme_.get(); }
    AppContext* context() const { return context_.get(); }
    QString errorString() const { return error_; }
    // QML warnings seen since load() started (tests assert there are none).
    QStringList warnings() const { return warnings_; }
    QString cacheDir() const { return cacheDir_; }

    // The application session; may be null (no session) and may be set later. Not owned. startSession() sets the
    // one it makes.
    void setApp(api::IApp* app);
    api::IApp* app() const { return app_; }
    // Makes the real session (see above) unless one is set, then runs the start-up recovery check. `userDataDir`:
    // AppOptions::userDataDir of the api (nullopt: $RAYCAD_USER_DATA_DIR, else the platform's per-user folder).
    // False (and a message in the banner) if it could not be made.
    bool startSession(std::optional<std::string> userDataDir = std::nullopt);
    // The crash-recovery source for the start-up check; before startSession. Default: the session's own journals
    // (IApp::pendingRecoveries / recover / discardRecovery). Tests set a double.
    void setRecoverySource(std::unique_ptr<IRecoverySource> source);

    // T-020: the viewer. Scene and Selection are QML singletons; viewport() is the window's ViewportItem
    // (null before load()). The viewer and the session share one job system, made on first use.
    SceneController* scene() const { return scene_.get(); }
    SelectionModel* selection() const { return selection_.get(); }
    ViewportItem* viewport() const;
    IJobSystem* jobs();
    // T-019: the `Session` and `Files` QML singletons.
    Session* session() const { return session_.get(); }
    Files* files() const { return files_.get(); }

    // Closing the main window asks about unsaved changes first (Files::requestQuit).
    bool eventFilter(QObject* watched, QEvent* event) override;
    // T-021: the Commands and Shortcuts singletons (palette_commands.h, shortcuts.h).
    CommandCatalog* commands() const { return commands_.get(); }
    ShortcutManager* shortcuts() const { return shortcuts_.get(); }
    // T-024: the AppSettings and Units singletons (settings.h, units.h).
    SettingsObject* settings() const { return settings_.get(); }
    UnitsObject* units() const { return units_.get(); }

    // <per-user cache location>/: QStandardPaths::CacheLocation for organisation and application "RayCAD".
    static QString cacheDirectory();
    // A saved window rectangle made to fit the screens there are now: on the screen showing most of it, no larger
    // than its available area and wholly inside it; centred on the primary screen when it is on none.
    static QRect clampToScreens(const QRect& wanted, const QSize& minimum);
    // Sets the organisation and application names. Call before constructing QGuiApplication.
    static void configureProcess();

private:
    AppOptions options_;
    QString cacheDir_;
    QString optionError_;
    QString error_;
    QStringList warnings_;
    api::IApp* app_ = nullptr;
    // Declared before engine_ so they outlive it: the engine holds them as singletons with C++ ownership.
    // The dispatcher and the job system outlive the objects that hold job handles, and the session outlives
    // everything that observes it.
    std::unique_ptr<IUiDispatcher> dispatcher_;
    std::unique_ptr<IJobSystem> ownJobs_;
    std::unique_ptr<api::IApp> ownApp_;
    std::unique_ptr<IRecoverySource> recovery_;
    std::unique_ptr<ThemeObject> theme_;
    std::unique_ptr<AppContext> context_;
    std::unique_ptr<SceneController> scene_;
    std::unique_ptr<SelectionModel> selection_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<Files> files_;
    std::unique_ptr<CommandCatalog> commands_;
    std::unique_ptr<ShortcutManager> shortcuts_;
    std::unique_ptr<UnitsObject> units_;
    std::unique_ptr<SettingsObject> settings_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QQuickWindow* window_ = nullptr;
    bool quitApproved_ = false;
    bool settingsLoaded_ = false;
    bool restoreMaximized_ = false;
    // The geometry is saved for a normal run with the user's settings; not for --size, --smoke, --bench-start or
    // --screenshot runs, which would otherwise replace the user's window with theirs.
    void saveWindowGeometry(bool now);
    bool persistGeometry() const
    {
        return settingsLoaded_ && !options_.size.isValid() && !options_.smoke && !options_.benchStart &&
               options_.screenshot.isEmpty();
    }
};

} // namespace ray::ui
