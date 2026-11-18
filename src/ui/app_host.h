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

namespace ray::api {
class IApp;
}

namespace ray::ui {

class AppContext;
class ThemeObject;

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
    // Declared before engine_ so they outlive it: the engine holds them as singletons with C++ ownership.
    std::unique_ptr<ThemeObject> theme_;
    std::unique_ptr<AppContext> context_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QQuickWindow* window_ = nullptr;
};

} // namespace ray::ui
