// The `DevOpen` QML singleton (task T-020): the developer path that opens a STEP or IGES file straight into the
// viewer, so the viewport can be used and tested before the application session (T-017) exists.
//
// `raycad <file>`, dropping a file on the window and the Open button of the empty state all come here. The file is
// imported on the job system (api::viewer::importModel: the kernel's staged progress in the status bar's job slot,
// Esc or Cancel abandons it), its bodies get persistent names, and they go to the scene's one entry point
// (SceneController::setBodies), which meshes them and fits the view. Errors show as a dismissible message
// (AppContext::showMessage), never a crash.
//
// T-019 replaces this class with IApp::open: the document then feeds the scene through
// SceneController::syncFromDocument, and nothing else in the viewer changes.
#pragma once

#include "scene_controller.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtQml/QQmlEngine>

namespace ray::ui {

class AppContext;

class DevOpen : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(DevOpen)
    QML_SINGLETON

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY busyChanged)
    // Name filters for a file dialog.
    Q_PROPERTY(QStringList nameFilters READ nameFilters CONSTANT)

public:
    DevOpen(SceneController* scene, AppContext* context, QObject* parent = nullptr);
    ~DevOpen() override;

    static DevOpen* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, DevOpen* opener);

    bool busy() const { return job_.valid(); }
    QString fileName() const { return fileName_; }
    QStringList nameFilters() const;

    // A local path or a file:// URL. False (and a message) if it is not a STEP/IGES file or cannot be read; the
    // import itself reports later through opened() / failed().
    Q_INVOKABLE bool open(const QString& pathOrUrl);
    Q_INVOKABLE bool openUrl(const QUrl& url) { return open(url.toString()); }
    // True for a local .step/.stp/.igs/.iges file (drag-and-drop asks before accepting).
    Q_INVOKABLE bool canOpen(const QUrl& url) const;
    // Abandons the running import (the kernel's model-load stage cannot be interrupted; its result is dropped).
    Q_INVOKABLE void cancel();

signals:
    void busyChanged();
    void opened(const QString& fileName, int bodyCount);
    void failed(const QString& message);

private:
    static QString localPath(const QString& pathOrUrl);

    QPointer<SceneController> scene_;
    QPointer<AppContext> context_;
    JobHandle<std::shared_ptr<api::viewer::ImportedModel>> job_;
    std::uint64_t serial_ = 0;
    QString fileName_;
};

} // namespace ray::ui
