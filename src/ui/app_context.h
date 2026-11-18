// The `AppContext` QML singleton: what the shell knows about the application session.
//
// It wraps an api::IApp* that MAY BE NULL (no session: the shell still starts and is fully usable for layout and
// theme work). Actions go through invoke(), which does nothing and returns false without a session.
// Later tasks extend it: T-016's ViewportItem sets backdropSource; T-020 reports display meshing through the job
// slot (setJob / clearJob, jobCancelRequested; T-019's Session merges it into the jobs list), T-019 routes file
// actions through the action handler, T-021 lists and invokes actions, T-024 persists glass and the theme mode.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtQml/QQmlEngine>

#include <functional>

namespace ray::api {
class IApp;
}

namespace ray::ui {

class AppContext : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(AppContext)
    QML_SINGLETON

    Q_PROPERTY(bool hasSession READ hasSession NOTIFY sessionChanged)
    // Liquid Glass panels (PLAN.md 6.6). Off means flat opaque panels; the layout is the same either way.
    Q_PROPERTY(bool glass READ glass WRITE setGlass NOTIFY glassChanged)
    // The blurred viewport backdrop glass panels sample (T-016): the ViewportItem, which sets itself here. Each glass
    // panel's GlassBackdrop draws the part of its one shared blurred texture behind the panel (viewport_item.h).
    // Null (no viewport, or no QRhi): glass panels show the tinted translucent fill alone.
    Q_PROPERTY(QObject* backdropSource READ backdropSource WRITE setBackdropSource NOTIFY backdropSourceChanged)
    // Index into workspaceNames.
    Q_PROPERTY(int workspace READ workspace WRITE setWorkspace NOTIFY workspaceChanged)
    Q_PROPERTY(QStringList workspaceNames READ workspaceNames CONSTANT)
    Q_PROPERTY(QString statusText READ statusText WRITE setStatusText NOTIFY statusTextChanged)
    // The status bar's job slot: visible while jobActive; jobProgress in 0..1, or below 0 for "indeterminate".
    Q_PROPERTY(bool jobActive READ jobActive NOTIFY jobChanged)
    Q_PROPERTY(QString jobTitle READ jobTitle NOTIFY jobChanged)
    Q_PROPERTY(qreal jobProgress READ jobProgress NOTIFY jobChanged)
    // "shell" or "gallery" (--gallery): which page Main.qml shows.
    Q_PROPERTY(QString page READ page CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    // True when the process was started to take a screenshot: pages open their demo popups.
    Q_PROPERTY(bool screenshotMode READ screenshotMode CONSTANT)
    // T-020: a dismissible message over the viewport (errors opening a file, and the like). Empty: none shown.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)

public:
    // Deliberately not default-constructible: QML must get the instance through create() (see provideFor).
    explicit AppContext(QObject* parent);
    ~AppContext() override;

    static AppContext* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, AppContext* context);

    // C++ side. The IApp must outlive this object or be reset to null first.
    void setApp(api::IApp* app);
    api::IApp* app() const { return app_; }
    void setPage(const QString& page) { page_ = page; }
    void setScreenshotMode(bool on) { screenshotMode_ = on; }
    void setJob(const QString& title, qreal progress);
    void clearJob();

    bool hasSession() const { return app_ != nullptr; }
    bool glass() const { return glass_; }
    void setGlass(bool on);
    QObject* backdropSource() const { return backdropSource_; }
    void setBackdropSource(QObject* source);
    int workspace() const { return workspace_; }
    void setWorkspace(int index);
    QStringList workspaceNames() const;
    QString statusText() const { return statusText_; }
    void setStatusText(const QString& text);
    bool jobActive() const { return jobActive_; }
    QString jobTitle() const { return jobTitle_; }
    qreal jobProgress() const { return jobProgress_; }
    QString page() const { return page_; }
    QString version() const;
    bool screenshotMode() const { return screenshotMode_; }
    QString message() const { return message_; }
    bool messageIsError() const { return messageIsError_; }
    Q_INVOKABLE void showMessage(const QString& text, bool isError = false);
    Q_INVOKABLE void dismissMessage();

    // Runs an action as Caller::User through IActionRegistry. False when there is no session (nothing runs).
    // The result arrives later: long-running actions show in the jobs list (Session), and a failure shows its
    // message in the banner. T-019: the action handler sees every call first; file actions that need a dialog or
    // the unsaved-changes prompt (Files::handleAction) are handled there and never reach the registry directly.
    Q_INVOKABLE bool invoke(const QString& actionId, const QString& argsJson = QStringLiteral("{}"));
    using ActionHandler = std::function<bool(const QString& actionId, const QString& argsJson)>;
    void setActionHandler(ActionHandler handler) { actionHandler_ = std::move(handler); }
    // The handler alone (the command palette runs registry actions itself): true if it took the action.
    bool handleAction(const QString& actionId, const QString& argsJson)
    {
        return app_ != nullptr && actionHandler_ && actionHandler_(actionId, argsJson);
    }
    // The user pressed Cancel in the job slot (or Esc, T-019).
    Q_INVOKABLE void cancelJob();

signals:
    void sessionChanged();
    void glassChanged();
    void backdropSourceChanged();
    void workspaceChanged();
    void statusTextChanged();
    void jobChanged();
    void jobCancelRequested();
    void messageChanged();

private:
    api::IApp* app_ = nullptr;
    bool glass_ = false;
    QPointer<QObject> backdropSource_;
    int workspace_ = 0;
    QString statusText_;
    bool jobActive_ = false;
    QString jobTitle_;
    qreal jobProgress_ = -1;
    QString page_ = QStringLiteral("shell");
    bool screenshotMode_ = false;
    QString message_;
    bool messageIsError_ = false;
    ActionHandler actionHandler_;
};

} // namespace ray::ui
