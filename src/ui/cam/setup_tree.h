// The `CamTreeModel` QML singleton (task T-044): the CAM tree of the CAM workspace (qml/cam/CamTree.qml). One row per
// setup, in program order, and under each (while it is expanded) a row per operation, in its order: name, type and a
// status badge.
//
//   Badges     operations: ICamEngine::status (up to date, stale, generating with its progress, failed with the
//              message, broken reference, suppressed); setups: their own resolution first (a broken reference of the
//              WCS or the stock, from api::cam_setup::setupStates), then the worst of their operations. Statuses are
//              worked out on the job system (a superseded run is cancelled) whenever the document changes (an edit in
//              either workspace, undo, redo) or publishes a build, and when a generation the tree started ends; the
//              progress of those comes from their jobs. T-045: the engine's own statuses come from
//              ICamEngine::statusAsync (a job of the engine's, so no worker holds a pointer to it), and its
//              notifications (ICamEngine::subscribe: a generation started, progressed or ended anywhere, a preview was
//              filed) start a run too, coalesced to one in kNoticeMs; the progress of any generation is in its status.
//              Nothing polls.
//   Actions    rename, remove, move up and down (cam.setup.* and cam.operation.*), suppress (operations), edit (a
//              setup opens the setup panel, CamSetup.edit; an operation is announced for the operation panel of
//              T-045, operationEditRequested) and generate (ICamEngine::generate of an operation, or of every stale
//              operation of a setup), each one action and, where the document changes, one undo step. Every action
//              is also a command (cam.tree.*, Cam context) on the current row.
#pragma once

#include <ray/api.h>
#include <ray_api/cam_cycletime.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class SceneController;
class Session;
class SetupToolModel;

class SetupTreeModel : public QAbstractListModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamTreeModel) // the view is qml/cam/CamTree.qml
    QML_SINGLETON

    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int setupCount READ setupCount NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged)
    // Why the last tree action was refused or failed; "" when none.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    // A status run is under way.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

public:
    enum Role {
        KindRole = Qt::UserRole + 1, // "setup" or "operation"
        IdRole,
        NameRole,
        TypeRole,       // operation type ("drill"); "" for a setup
        DetailRole,     // setup: work offset and machine; operation: its type
        StatusRole,     // camui::statusKey, or "" when there is nothing to show
        StatusTextRole, // camui::statusText
        MessageRole,    // why (failed, broken reference)
        ProgressRole,   // 0..1 while generating (when known), else -1
        SuppressedRole,
        DepthRole,
        ExpandedRole,
        OperationCountRole,
        CanMoveUpRole,
        CanMoveDownRole,
        // T-059: the cycle-time estimate. A setup: camui::cycleTimeShort of its total while every operation that is
        // not suppressed is up to date, else ""; an operation: "" (its time is in the tip). The tip: camui::
        // cycleTimeDetails of the row's part (the operation's, or the setup's), "" while there is none.
        CycleTimeRole,
        CycleTimeTipRole,
    };

    SetupTreeModel(SceneController* scene, Session* session, AppContext* context, QObject* parent = nullptr);
    ~SetupTreeModel() override;
    static SetupTreeModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    // The setup panel: it shows the WCS of the current row's setup while it is closed (SetupToolModel::showWcsOf).
    void setSetupTool(SetupToolModel* tool);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(rows_.size()); }
    int setupCount() const { return setupCount_; }
    int revision() const { return revision_; }
    int currentIndex() const { return current_; }
    void setCurrentIndex(int index);
    QString message() const { return message_; }
    bool busy() const { return (statusJob_.valid() && !statusJob_.done()) || (setupJob_.valid() && !setupJob_.done()); }

    // ---- QML ----
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int rowOf(const QString& id) const;
    Q_INVOKABLE void toggleExpanded(int row);
    Q_INVOKABLE bool rename(int row, const QString& name);
    Q_INVOKABLE bool remove(int row);
    Q_INVOKABLE bool moveUp(int row);
    Q_INVOKABLE bool moveDown(int row);
    Q_INVOKABLE bool setSuppressed(int row, bool suppressed);
    Q_INVOKABLE bool edit(int row);
    Q_INVOKABLE bool generate(int row);
    Q_INVOKABLE void dismissMessage();

    // ---- the command catalog's hooks (cam.tree.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    // Status runs finished (each one ICamEngine::status over the whole document).
    int statusRuns() const { return statusRuns_; }
    std::optional<cam::OpStatus> statusOf(const OperationId& id) const;
    // T-045: the engine's whole status of an operation as last read (null when not known), and the current row's
    // setup (its own, or the one an operation row is in) and operation.
    const cam::OpState* opState(const OperationId& id) const;
    std::optional<SetupId> currentSetup() const;
    std::optional<OperationId> currentOperation() const;
    // Makes the row of `id` current as soon as the tree lists it (an operation just added).
    void selectWhenListed(const QString& id);
    // Engine notifications heard (tests).
    int notices() const { return notices_; }
    // Re-reads the statuses now (also done by every document notification).
    void refresh();
    // T-059: the cycle-time estimate of each setup, worked out on the job system after every status run that changed
    // what it is made of (api::cam_cycletime: the up-to-date toolpaths, their tool numbers, the machine), so a
    // toolpath generated, edited or gone changes it. Null while there is none for the setup. CycleTime::operations
    // parallels input.operations (the setup's operations that are up to date and not suppressed).
    struct CycleTimeEstimate {
        api::cam_cycletime::SetupInput input;
        cam::CycleTime time;
    };
    const CycleTimeEstimate* cycleTimeOf(const SetupId& id) const;
    // An operation's part of its setup's estimate; null while there is none.
    const cam::CycleTimePart* cycleTimeOf(const OperationId& id) const;
    // Estimates finished (tests).
    int cycleTimeRuns() const { return cycleTimeRuns_; }

signals:
    // T-059: a setup's cycle-time estimate arrived or went.
    void cycleTimesChanged();
    void changed();
    void currentChanged();
    void messageChanged();
    // T-045: an operation row was opened for editing (double-click, Enter, cam.tree.edit).
    void operationEditRequested(const QString& operationId);
    void renameRequested(int row);

private:
    struct Row {
        bool setup = true;
        SetupId setupId;
        OperationId opId;
        QString name, type, detail;
        bool suppressed = false;
        int operations = 0;
        int index = 0;    // within its list (setups, or the setup's operations)
        int siblings = 0; // the list's length
    };
    struct Statuses {
        std::map<OperationId, cam::OpState> ops;
        std::map<SetupId, Status> setups;
    };
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void sync();
    void scheduleRefresh();
    void setMessage(const QString& message);
    bool run(const char* actionId, const std::string& argsJson, const QString& label);
    const Row* rowAt(int row) const;

    QPointer<SceneController> scene_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<SetupToolModel> tool_;

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    std::vector<Row> rows_;
    int setupCount_ = 0;
    std::set<SetupId> collapsed_;
    int revision_ = 0;
    int current_ = -1;
    QString message_;

    // Statuses.
    void subscribeEngine();
    Statuses statuses_;
    JobHandle<std::map<OperationId, cam::OpState>> statusJob_;             // ICamEngine::statusAsync
    JobHandle<std::shared_ptr<const std::map<SetupId, Status>>> setupJob_; // api::cam_setup::setupStates
    std::uint64_t statusSerial_ = 0;
    int statusRuns_ = 0;
    QTimer refreshTimer_; // coalesces the notifications of one change into one run
    QTimer noticeTimer_;  // coalesces the engine's notifications (progress comes at up to 30 Hz)
    std::map<OperationId, JobHandle<std::shared_ptr<const cam::Toolpath>>> generating_; // started here
    std::map<OperationId, double> progress_;
    cam::ICamEngine* engine_ = nullptr; // the engine subscribed to (the app's, which outlives this model's use)
    int engineListener_ = 0;
    int notices_ = 0;
    QString pendingSelect_;
    bool selectFirst_ = false; // a document was opened: its first setup becomes current once its rows are there

    // T-059: cycle-time estimates.
    void estimateCycleTimes();
    void showCycleTimes();
    std::map<SetupId, std::shared_ptr<const CycleTimeEstimate>> cycleTimes_;
    JobHandle<std::shared_ptr<const std::vector<CycleTimeEstimate>>> cycleTimeJob_;
    std::uint64_t cycleTimeSerial_ = 0;
    int cycleTimeRuns_ = 0;
};

} // namespace ray::ui
