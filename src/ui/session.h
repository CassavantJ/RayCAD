// The `Session` QML singleton (task T-019): what the shell shows about the application session.
//
//   - The active document: title, dirty marker, read-only state with the lock holder, undo/redo availability, the
//     window title. It subscribes to IApp's events and to the active document's state and build changes, and feeds
//     the viewer through its one document entry point, SceneController::syncFromDocument (bodies, model tree).
//   - Jobs: IApp::activeJobs() (open, save, import, export and long-running actions) plus the document's rebuild and
//     the viewer's display meshing, as one list (panels/JobsPanel.qml) and a foreground job for the status bar's
//     slot. cancelForegroundJob() is Esc. A cancelled job leaves the list at once; when its work is in a stage that
//     cannot be interrupted (the STEP model load, PLAN.md 7) its late result is dropped.
//   - The recent-files list for the start screen. Whether each file still exists is checked on a worker, because a
//     path on a network share that is gone can block for seconds.
//   - Pending crash recoveries found at start-up (session_recovery.h). Recovering is Files::restoreRecovery.
//   - Warning events of the app (crash recovery off for a document, a partial recovery) in the banner.
//
// Files (files.h) does the user's file operations; Session only observes.
#pragma once

#include "session_recovery.h"

#include <ray/api.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ray::ui {

class AppContext;
class SceneController;

// Recent files for the start screen, most recent first.
class RecentFilesModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        PathRole = Qt::UserRole + 1, // as stored (UTF-8 path)
        NameRole,                    // file name with extension
        FolderRole,                  // the folder, in native separators
        OpenedRole,                  // when it was last opened, as text
        OpenedMsRole,
        MissingRole,   // the file is not there any more (checked on a worker; false until known)
        ThumbnailRole, // image URL; "" until thumbnails are stored (M1): the start screen shows a placeholder
    };
    struct Item {
        QString path;
        qint64 openedMs = 0;
        bool missing = false;
    };
    explicit RecentFilesModel(QObject* parent);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(std::vector<Item> items);
    void setMissing(const QString& path, bool missing);
    const std::vector<Item>& items() const { return items_; }

private:
    std::vector<Item> items_;
};

// Running jobs, oldest first.
class JobsModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        JobIdRole = Qt::UserRole + 1, // pass to Session.cancelJob
        TitleRole,
        StageRole,    // the job's current stage ("Reading file", "Transferring", ...), may be empty
        ProgressRole, // 0..1, or below 0 while indeterminate
        KindRole,     // "app" (an IApp job), "rebuild" or "display"
    };
    struct Row {
        qint64 id = 0;
        QString title, stage, kind;
        double progress = -1;
    };
    explicit JobsModel(QObject* parent);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRows(std::vector<Row> rows);
    const std::vector<Row>& rows() const { return rows_; }

private:
    std::vector<Row> rows_;
};

class Session : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Session)
    QML_SINGLETON

    // ---- the active document ----
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY documentChanged)
    Q_PROPERTY(QString documentId READ documentId NOTIFY documentChanged)
    Q_PROPERTY(QString documentTitle READ documentTitle NOTIFY documentChanged)
    Q_PROPERTY(QString documentPath READ documentPath NOTIFY documentChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY documentChanged)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY documentChanged)
    // Read-only documents: who holds the write lock ("user on host") and since when (local date and time).
    Q_PROPERTY(QString lockHolder READ lockHolder NOTIFY documentChanged)
    Q_PROPERTY(QString lockSince READ lockSince NOTIFY documentChanged)
    Q_PROPERTY(bool lockStale READ lockStale NOTIFY documentChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY documentChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY documentChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY documentChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY documentChanged)
    // "<name>* - RayCAD" while dirty, "<name> (read-only) - RayCAD", or "RayCAD" with no document.
    Q_PROPERTY(QString windowTitle READ windowTitle NOTIFY documentChanged)

    // ---- recent files ----
    Q_PROPERTY(QAbstractItemModel* recentFiles READ recentFiles CONSTANT)
    Q_PROPERTY(int recentCount READ recentCount NOTIFY recentChanged)

    // ---- jobs ----
    Q_PROPERTY(QAbstractItemModel* jobs READ jobs CONSTANT)
    Q_PROPERTY(int jobCount READ jobCount NOTIFY jobsChanged)
    // The status bar's slot: the foreground job (the newest app job, else the rebuild, else the display meshing).
    Q_PROPERTY(bool jobActive READ jobActive NOTIFY jobsChanged)
    Q_PROPERTY(QString jobTitle READ jobTitle NOTIFY jobsChanged)
    Q_PROPERTY(qreal jobProgress READ jobProgress NOTIFY jobsChanged)

    // ---- crash recovery ----
    // [{id, title, folder, time, changes, damaged}] for dialogs/RecoveryDialog.qml.
    Q_PROPERTY(QVariantList recoveries READ recoveries NOTIFY recoveriesChanged)
    // The recovery prompt is up: after the start-up check found some, or an open needs one first. "Decide later"
    // (postponeRecovery) hides it; the journals stay for the next start.
    Q_PROPERTY(bool recoveryPromptVisible READ recoveryPromptVisible NOTIFY recoveriesChanged)

public:
    static constexpr qint64 kRebuildJobId = -1;
    static constexpr qint64 kDisplayJobId = -2;

    Session(SceneController* scene, AppContext* context, QObject* parent = nullptr);
    ~Session() override;

    static Session* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, Session* session);

    // The session to observe (null detaches). It must outlive this object or be reset to null first.
    void setApp(api::IApp* app);
    api::IApp* app() const { return app_; }
    // The start-up check (after the first frame): lists the pending recoveries and shows the prompt if there are
    // any. Before that, setRecoverySource (AppHost sets the session's own, makeAppRecoverySource).
    void setRecoverySource(std::unique_ptr<IRecoverySource> source);
    IRecoverySource* recoverySource() const { return recovery_.get(); }
    void checkRecovery();
    // An open was refused because `path` has changes to recover first: lists the recoveries again and shows the
    // prompt if one is for that file. False if none is.
    bool offerRecoveryFor(const QString& path);
    // Removes a pending recovery from the list and returns it (Files recovers it); nullopt for an unknown id.
    std::optional<PendingRecovery> takeRecovery(const QString& id);

    api::OpenDocument* activeDocument() const;
    std::vector<qint64> appJobIds() const; // the visible IApp jobs, before abandoning
    // Re-reads the jobs now (a job just started shows at once, before the app's JobsChanged event arrives).
    void refreshJobs();

    bool hasDocument() const { return !docId_.empty(); }
    QString documentId() const { return QString::fromStdString(docId_); }
    QString documentTitle() const { return title_; }
    QString documentPath() const { return path_; }
    bool dirty() const { return dirty_; }
    bool readOnly() const { return readOnly_; }
    QString lockHolder() const { return lockHolder_; }
    QString lockSince() const { return lockSince_; }
    bool lockStale() const { return lockStale_; }
    bool canUndo() const { return canUndo_; }
    bool canRedo() const { return canRedo_; }
    QString undoText() const { return undoText_; }
    QString redoText() const { return redoText_; }
    QString windowTitle() const;

    QAbstractItemModel* recentFiles() const { return recent_; }
    RecentFilesModel* recentModel() const { return recent_; }
    int recentCount() const { return recent_->rowCount(); }
    QAbstractItemModel* jobs() const { return jobs_; }
    JobsModel* jobsModel() const { return jobs_; }
    int jobCount() const { return jobs_->rowCount(); }
    bool jobActive() const { return foreground_ >= 0; }
    QString jobTitle() const;
    qreal jobProgress() const;
    QVariantList recoveries() const;
    bool recoveryPromptVisible() const { return recoveryPrompt_ && !pending_.empty(); }

    // What the scene shows instead of the active document's latest build, while a tool needs it (a feature tool edits
    // a feature: the model as it was just before it, built from feature_edit::stateBefore). `state` gives the bodies'
    // names, colours and visibility and the tree's features; `build` the shapes. One owner at a time: another
    // owner's call replaces it. Cleared (the document's build shown again) by clearShownModel(owner) or when the
    // active document changes. The document's notifications refresh the document's properties meanwhile, not the
    // scene.
    void showModel(const QObject* owner, doc::DocStatePtr state, doc::BuildResultPtr build);
    void clearShownModel(const QObject* owner);
    bool showingModel() const { return modelOwner_ != nullptr; }

    // The title of a registered action ("" if unknown), for menus (shortcut texts: the Shortcuts singleton).
    Q_INVOKABLE QString actionTitle(const QString& id) const;
    // Cancels a job of the list (its jobId) or the foreground job (Esc). No-op if nothing runs.
    Q_INVOKABLE void cancelJob(qint64 id);
    Q_INVOKABLE void cancelForegroundJob();
    // Removes a file from the recent list (the start screen's remove button, for a missing file).
    Q_INVOKABLE bool removeRecent(const QString& path);
    // Deletes the journal (IApp::discardRecovery); an error shows in the banner.
    Q_INVOKABLE void discardRecovery(const QString& id);
    Q_INVOKABLE void postponeRecovery();
    // Re-reads the recent list and checks which files exist (also done on every RecentFilesChanged event).
    Q_INVOKABLE void refreshRecent();

signals:
    void sessionChanged();
    void documentChanged();
    void recentChanged();
    void jobsChanged();
    void recoveriesChanged();
    // The scene was fed a newly published build of the active document.
    void buildShown();
    // An IApp job was cancelled from the UI and left the list (its late result is to be dropped).
    void jobAbandoned(qint64 id);

private:
    void onAppEvent(const api::AppEvent& event);
    // doc_ while it is still open; a document closed a moment ago (its DocumentClosed event not delivered yet) is
    // destroyed already, so nothing may touch it: then this re-attaches to the active document first.
    doc::Document* liveDoc();
    void attachActive();
    void detachDocument();
    void onDocumentChanged(std::uint64_t generation);
    void refreshDocument();
    void syncScene(bool force);
    void checkRecentExistence();
    void updateRebuildPoll();

    QPointer<SceneController> scene_;
    QPointer<AppContext> context_;
    api::IApp* app_ = nullptr;
    int appListener_ = 0;

    // The document the scene shows.
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;
    doc::BuildResultPtr shownBuild_;
    QTimer rebuildPoll_;
    // showModel(): the model shown instead of the document's build, and who asked for it.
    const QObject* modelOwner_ = nullptr;
    doc::DocStatePtr modelState_;
    doc::BuildResultPtr modelBuild_;

    QString title_, path_, lockHolder_, lockSince_, undoText_, redoText_;
    bool dirty_ = false, readOnly_ = false, lockStale_ = false, canUndo_ = false, canRedo_ = false;

    RecentFilesModel* recent_ = nullptr;
    std::uint64_t recentSerial_ = 0;

    JobsModel* jobs_ = nullptr;
    std::set<qint64> abandoned_;
    qint64 foreground_ = -1; // row index in jobs_, -1 when nothing runs

    std::unique_ptr<IRecoverySource> recovery_;
    std::vector<PendingRecovery> pending_;
    bool recoveryPrompt_ = false;
};

} // namespace ray::ui
