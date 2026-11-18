// The application object: owns the QML engine, the Theme and AppContext singletons and the main window.
// runApp() (app.cpp) wraps it with the command-line options; tests create one directly.
//
// Start-up rules from spike S2 (PLAN.md 9, T-018):
//   - Qt Quick keeps the platform's default graphics API (Direct3D 11 on Windows, Metal on macOS); nothing here
//     forces OpenGL.
//   - The QML disk cache and the RHI pipeline cache live in a per-user cache folder from QStandardPaths, set
//     explicitly before the engine and the window exist.
//   - Nothing from the kernel runs before the first frame; the shell creates only what the first screen shows.
#pragma once

#include "app_options.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>

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
class DevOpen;
class SceneController;
class SelectionModel;
class ShortcutManager;
class ThemeObject;
class ViewportItem;

class AppHost : public QObject {
    Q_OBJECT
public:
    // A QGuiApplication must exist. `cacheDir` empty means the per-user cache folder (cacheDirectory()).
    explicit AppHost(const AppOptions& options, QString cacheDir = {}, QObject* parent = nullptr);
    ~AppHost() override;

    // Problems found while applying the options (an unknown --theme); empty if none.
    QString optionError() const { return optionError_; }

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

    // The application session; may be null (no session) and may be set later. Not owned.
    void setApp(api::IApp* app);

    // T-020: the viewer. Scene, Selection and DevOpen are QML singletons; viewport() is the window's ViewportItem
    // (null before load()). Jobs come from the session (IApp::jobs) or, without one, from a job system owned here.
    SceneController* scene() const { return scene_.get(); }
    SelectionModel* selection() const { return selection_.get(); }
    DevOpen* devOpen() const { return devOpen_.get(); }
    ViewportItem* viewport() const;
    IJobSystem* jobs();
    // Lets the empty state's Open button, drag-and-drop and command-line files open STEP/IGES files without a
    // session (the developer path until T-019). runApp turns it on; tests that need it do too.
    void enableDevOpen(bool on = true);
    // T-021: the Commands and Shortcuts singletons (palette_commands.h, shortcuts.h).
    CommandCatalog* commands() const { return commands_.get(); }
    ShortcutManager* shortcuts() const { return shortcuts_.get(); }

    // <per-user cache location>/: QStandardPaths::CacheLocation for organisation and application "RayCAD".
    static QString cacheDirectory();
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
    // The dispatcher and the job system outlive the objects that hold job handles.
    std::unique_ptr<IUiDispatcher> dispatcher_;
    std::unique_ptr<IJobSystem> ownJobs_;
    std::unique_ptr<ThemeObject> theme_;
    std::unique_ptr<AppContext> context_;
    std::unique_ptr<SceneController> scene_;
    std::unique_ptr<SelectionModel> selection_;
    std::unique_ptr<DevOpen> devOpen_;
    std::unique_ptr<CommandCatalog> commands_;
    std::unique_ptr<ShortcutManager> shortcuts_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QQuickWindow* window_ = nullptr;
};

} // namespace ray::ui
