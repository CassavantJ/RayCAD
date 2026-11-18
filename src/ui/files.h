// The `Files` QML singleton (task T-019): the user's file operations, all through the application API.
//
//   open      a .raycad document, or a STEP/IGES file (a new document with one import feature): IApp::open. From
//             the Open dialog, the start screen's recent list, the command line and drag-and-drop. Several files at
//             once (a drop, the command line): the first opens, every further STEP/IGES file is imported into it.
//   new       IApp::newDocument.  save / save as   IApp::save.  import   IApp::importFile into the current document.
//   export    the file.exportStep / file.exportIges / file.exportStl actions with the path from a save dialog.
//   close     IApp::close.  quit   closing the main window (AppHost forwards it here).
//
// The shell shows one document at a time: opening or creating one replaces the current one, which is closed once
// the new one is there. Whenever that, closing or quitting would lose unsaved changes, the unsaved-changes prompt
// (dialogs/UnsavedChangesDialog.qml) asks first: Save, Discard or Cancel.
//
// Menus, shortcuts and the command palette (T-021) run these through AppContext::invoke: handleAction() takes the
// file actions that need a dialog or the prompt, so `invoke("file.open")` with no path shows the Open dialog.
// Errors show in the banner with the API's message (AppContext::showMessage).
#pragma once

#include <ray/api.h>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtQml/QQmlEngine>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ray::ui {

class AppContext;
class Session;
struct PendingRecovery;

class Files : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Files)
    QML_SINGLETON

    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // An open is running (until it finishes or is cancelled).
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString openingName READ openingName NOTIFY busyChanged)
    // Name filters for the file dialogs.
    Q_PROPERTY(QStringList openFilters READ openFilters CONSTANT)
    Q_PROPERTY(QStringList importFilters READ importFilters CONSTANT)
    Q_PROPERTY(QStringList documentFilters READ documentFilters CONSTANT)
    // False in tests: the *DialogRequested signals are still emitted, but no native dialog opens.
    Q_PROPERTY(bool useNativeDialogs READ useNativeDialogs WRITE setUseNativeDialogs NOTIFY useNativeDialogsChanged)

    // The unsaved-changes prompt.
    Q_PROPERTY(bool promptVisible READ promptVisible NOTIFY promptChanged)
    Q_PROPERTY(QString promptDocument READ promptDocument NOTIFY promptChanged)
    // "close", "quit", "open" or "new": what happens after the answer.
    Q_PROPERTY(QString promptReason READ promptReason NOTIFY promptChanged)

public:
    Files(Session* session, AppContext* context, QObject* parent = nullptr);
    ~Files() override;

    static Files* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, Files* files);

    bool available() const;
    bool busy() const { return openJob_.valid(); }
    QString openingName() const { return openingName_; }
    QStringList openFilters() const;
    QStringList importFilters() const;
    QStringList documentFilters() const;
    Q_INVOKABLE QStringList exportFilters(const QString& kind) const; // kind: "step", "iges" or "stl"
    bool useNativeDialogs() const { return nativeDialogs_; }
    void setUseNativeDialogs(bool on);
    bool promptVisible() const { return prompt_.has_value(); }
    QString promptDocument() const;
    QString promptReason() const;

    // The AppContext::invoke hook. True when the action was handled here.
    bool handleAction(const QString& actionId, const QString& argsJson);
    // Any open document has unsaved changes (closing the window asks first).
    bool hasUnsavedChanges() const;

    // ---- open ----
    // A local path or a file:// URL. False (and a message) when it is not a RayCAD, STEP or IGES file; the open
    // itself reports later (opened / openFailed).
    Q_INVOKABLE bool open(const QString& pathOrUrl);
    Q_INVOKABLE bool openUrls(const QList<QUrl>& urls);
    bool openPaths(const QStringList& paths);
    // Drag-and-drop asks before accepting: local files of a kind that opens.
    Q_INVOKABLE bool canOpen(const QUrl& url) const;
    Q_INVOKABLE bool canOpenAny(const QList<QUrl>& urls) const;
    Q_INVOKABLE void requestOpen(); // shows the Open dialog

    // ---- the current document ----
    Q_INVOKABLE void newPart();
    Q_INVOKABLE void save(); // Save As for a document never saved or read-only
    Q_INVOKABLE void requestSaveAs();
    // `confirmedOverwrite`: the native save dialog already asked about replacing an existing file.
    Q_INVOKABLE void saveAs(const QString& pathOrUrl, bool confirmedOverwrite = false);
    Q_INVOKABLE void saveAsCancelled(); // the Save As dialog was dismissed
    Q_INVOKABLE void requestImport();
    Q_INVOKABLE void importFile(const QString& pathOrUrl);
    Q_INVOKABLE void requestExport(const QString& kind);
    Q_INVOKABLE void exportTo(const QString& kind, const QString& pathOrUrl);
    Q_INVOKABLE void close();
    Q_INVOKABLE void requestQuit();
    // "save", "discard" or "cancel".
    Q_INVOKABLE void answerPrompt(const QString& choice);

    // ---- crash recovery ----
    // Recovers a pending recovery (IRecoverySource::recover, i.e. IApp::recover): the document reopens with its
    // journalled changes and becomes the active one; replay warnings show in the banner; a document that was never
    // saved is offered Save As.
    Q_INVOKABLE void restoreRecovery(const QString& id);

signals:
    void availableChanged();
    void busyChanged();
    void useNativeDialogsChanged();
    void promptChanged();
    void openDialogRequested();
    void saveAsDialogRequested(const QString& suggestedName, const QUrl& folder);
    void importDialogRequested();
    void exportDialogRequested(const QString& kind, const QString& suggestedName, const QUrl& folder);
    void opened(const QString& title);
    void openFailed(const QString& message);
    void saved(const QString& title);
    void exported(const QString& path);
    void recoveryRestored(const QString& title);
    // Every unsaved change was saved or discarded: the window may close.
    void quitApproved();

private:
    enum class Reason { Close, Quit, Open, New };
    struct Prompt {
        Reason reason;
        std::string documentId;
        QString title;
        std::function<void()> then; // after Save (once saved) or Discard
    };
    using Opened = std::function<void(api::OpenDocument*)>;

    api::IApp* app() const;
    static QString localPath(const QString& pathOrUrl);
    void showError(const QString& message);
    void setStatus(const QString& text);
    // Runs `then` now if the active document has no unsaved changes, else after the prompt's Save or Discard.
    void guard(Reason reason, std::function<void()> then);
    // `make`: the job to run instead of IApp::open(path) (a recovery); `path` is then only its name.
    using MakeJob = std::function<JobHandle<api::OpenDocument*>()>;
    void startOpen(const QString& path, QStringList importAfter, std::string replaceId, Opened opened = {},
                   MakeJob make = {});
    void openFinished();
    void importChain(std::string documentId, QStringList paths);
    void startSave(api::OpenDocument& od, std::optional<std::string> path, bool overwrite, std::function<void()> then);
    bool closeDocument(const std::string& id);
    void onJobAbandoned(qint64 id);

    QPointer<Session> session_;
    QPointer<AppContext> context_;
    bool nativeDialogs_ = true;
    std::optional<Prompt> prompt_;
    std::function<void()> afterSaveAs_; // a prompt's Save of a document that needed Save As
    JobHandle<api::OpenDocument*> openJob_;
    qint64 openJobId_ = 0;
    std::uint64_t openSerial_ = 0;
    QString openingName_;
};

} // namespace ray::ui
