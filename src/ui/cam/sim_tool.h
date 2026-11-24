// The `CamSim` QML singleton (task T-074): the Simulate workspace (qml/cam/SimPanel.qml and qml/cam/Timeline.qml).
//
//   Setup      the setup to simulate: chosen in the panel (setSetup); on first entry the CAM tree's current setup, else
//              the first. Changing it drops the result shown.
//   Gate       like posting (ray_api/cam_post.h blockers over ICamEngine::statusAsync and cam_setup::setupStates): a
//              stale, failed, generating or missing toolpath blocks Simulate, each with its reason, and Generate all
//              generates the ones generating fixes. Checked whenever the document or the engine changes.
//   Run        run() (the Simulate button, the command cam.sim.run): for a setup whose stock comes from an earlier
//   setup,
//              first ICamEngine::carriedStock (an app job, with progress); then one job on a SimSession of its own
//              (ray_api/cam_sim_session.h: the program, the run at the tier preset's cell, the timeline, the first
//              mesh), with progress in the panel and cancel (the panel's Cancel, Esc). A run clears the result shown
//              before it; a cancelled or failed run leaves nothing shown.
//   Result     belongs to the toolpaths it was run on (their input hashes): when the setup's operations or their
//              toolpaths change, outOfDate says so (the stock is drawn in the stale colour, the panel offers to run
//              again); it is never shown as current. Events (sim.h) are listed with a machinist's title (camui::
//              eventTitle), their operation, move and time, and the event's own message; they sit on the timeline as
//              markers (clusters, camui::clusterMarkers).
//   Timeline   estimated machine time (cam_sim::Timeline): operations marked and named. play()/pause() (Space) at a
//              speed (camui::playbackSpeeds; "[" and "]"), stepForward()/stepBack() one move (Right, Left),
//              toStart()/toEnd() (Home, End), nextEvent()/previousEvent() (Ctrl+Right, Ctrl+Left), scrubTo() on the
//              track. Every move of the stock and the tool is a seek on the session's serial queue: one job at a time;
//              requests that arrive meanwhile replace each other (the latest wins), so a slow seek never blocks the UI
//              thread and the stock catches up with where the user is; targetSec is the position being sought.
//              During playback the clock runs at its speed, a seek starts at most every kMinFrameMs, and each frame's
//              dirty region is asked for with at most camui::playbackCap triangles; when playback stops the whole stock
//              is asked for again at the full budget.
//   Event      selectEvent(i) (a marker, a row): the stock and the tool go to where the event happened (EventPlace),
//   and
//              when the program posted for the setup is current the G-code viewer opens on the event's move (its first
//              line current, every line of it marked: GcodeModel::showMove, through PostOutput::lineMap).
//   G-code     gcodeState: "current" (the viewer's program is this setup's, from exactly these toolpaths), "stale"
//              (it is this setup's, from other toolpaths), "none" (no program of this setup). When it is not current
//              the panel says so and offers Post..., which opens the post dialog (its gate applies); nothing is posted
//              without the user. The other way (added after T-074): in the Simulate workspace, a line the user makes
//              current in the viewer (a click, the arrow keys, a find; GcodeModel::lineChosen) seeks the stock and the
//              tool to the end of the move that line writes (its first move when it writes several; PostOutput::
//              lineMap); a line that writes no move (a comment, the header, a tool change) does nothing, and so does a
//              line while the program is not current. The viewer keeps the line chosen (a frame does not move it to
//              its move's first line). The CAM workspace's viewer is unchanged.
//   Display    the stock (IRenderViewport::addStock, stockStyle, marks of the selected events), the tool glyph
//              (cutting part and shank) and its holder (addToolGlyph, one pair per operation, the current one shown at
//              ISimulator::toolPose), shown only in the Simulate workspace. The toolpaths of the run (added after
//              T-074): RunResult::backplots drawn by the renderer's backplot item (IViewport::addBackplot) in
//              ToolpathDisplay::pathStyle, as the CAM workspace draws them; two items share each backplot, the moves
//              done (up to the playhead, the move under way included) at full strength, the moves still to come dimmed,
//              split with setBackplotRange on every frame (no upload); the stale colour while the result is out of
//              date. Visibility toggles: stock, tool, holder, fixtures, toolpath. In the Simulate workspace the setup's
//              part bodies are hidden while the stock is shown (the stock is the part as cut), and fixture bodies while
//              the fixture toggle is off; the model tree's own visibility is not touched, and leaving the workspace
//              shows them as they were.
//   Budget     the display triangle budget from the tier preset's cell (camui::stockTriangleBudget).
#pragma once

#include "sim_logic.h"

#include <ray/api.h>
#include <ray/render.h>
#include <ray_api/cam_post.h>
#include <ray_api/cam_setup.h>
#include <ray_api/cam_sim_session.h>

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace ray::render {
class IRenderViewport;
}

namespace ray::ui {

class AppContext;
class CommandCatalog;
class GcodeModel;
class PostToolModel;
class SceneController;
class Session;
class SetupTreeModel;

class SimToolModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamSim)
    QML_SINGLETON

    // The document's setups [{id, name}] and the one to simulate.
    Q_PROPERTY(QVariantList setups READ setups NOTIFY setupsChanged)
    Q_PROPERTY(QString setupId READ setupIdText NOTIFY setupChanged)
    Q_PROPERTY(QString setupName READ setupName NOTIFY setupChanged)
    Q_PROPERTY(int setupIndex READ setupIndex NOTIFY setupChanged)
    // The gate: [{opId, name, reason, message, canGenerate}]; checking while it is worked out.
    Q_PROPERTY(QVariantList blockers READ blockers NOTIFY gateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY gateChanged)
    Q_PROPERTY(bool canRun READ canRun NOTIFY gateChanged)
    Q_PROPERTY(QString runBlockedReason READ runBlockedReason NOTIFY gateChanged)
    Q_PROPERTY(bool canGenerate READ canGenerate NOTIFY gateChanged)
    // The run: "idle", "running", "done", "failed", "cancelled"; message for a failure or a cancel.
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged) // 0..1, -1 while not known
    Q_PROPERTY(QString stage READ stage NOTIFY progressChanged)
    // The result shown.
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    Q_PROPERTY(bool outOfDate READ outOfDate NOTIFY outOfDateChanged)
    Q_PROPERTY(QString outOfDateReason READ outOfDateReason NOTIFY outOfDateChanged)
    Q_PROPERTY(QString resolutionText READ resolutionText NOTIFY resultChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY resultChanged)
    Q_PROPERTY(int errorCount READ errorCount NOTIFY resultChanged)
    Q_PROPERTY(int warningCount READ warningCount NOTIFY resultChanged)
    // [{index, kind, kindName, severity, title, message, operation, opIndex, move (1-based), sec, time, x (0..1)}]
    Q_PROPERTY(QVariantList events READ events NOTIFY resultChanged)
    // [{name, start, end (0..1 of the timeline), startSec, endSec}]
    Q_PROPERTY(QVariantList operations READ operations NOTIFY resultChanged)
    Q_PROPERTY(double totalSec READ totalSec NOTIFY resultChanged)
    // Where the stock and the tool are (positionSec), and where they are going (targetSec, while seeking).
    Q_PROPERTY(double positionSec READ positionSec NOTIFY positionChanged)
    Q_PROPERTY(double targetSec READ targetSec NOTIFY positionChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY positionChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionChanged)
    Q_PROPERTY(QString timeText READ timeText NOTIFY positionChanged)
    Q_PROPERTY(int currentEvent READ currentEvent NOTIFY currentEventChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(int speedIndex READ speedIndex WRITE setSpeedIndex NOTIFY speedChanged)
    Q_PROPERTY(QStringList speeds READ speeds CONSTANT)
    // Visibility.
    Q_PROPERTY(bool showStock READ showStock WRITE setShowStock NOTIFY visibilityChanged)
    Q_PROPERTY(bool showTool READ showTool WRITE setShowTool NOTIFY visibilityChanged)
    Q_PROPERTY(bool showHolder READ showHolder WRITE setShowHolder NOTIFY visibilityChanged)
    Q_PROPERTY(bool showFixtures READ showFixtures WRITE setShowFixtures NOTIFY visibilityChanged)
    Q_PROPERTY(bool showToolpath READ showToolpath WRITE setShowToolpath NOTIFY visibilityChanged)
    // The G-code link.
    Q_PROPERTY(QString gcodeState READ gcodeState NOTIFY gcodeChanged)
    Q_PROPERTY(QString gcodeText READ gcodeText NOTIFY gcodeChanged)

public:
    SimToolModel(SceneController* scene, Session* session, AppContext* context, QObject* parent = nullptr);
    ~SimToolModel() override;
    static SimToolModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    void setTree(SetupTreeModel* tree) { tree_ = tree; }
    void setPost(PostToolModel* post) { post_ = post; }
    void setViewer(GcodeModel* viewer);
    void ensureLinks();

    QVariantList setups() const;
    QString setupIdText() const;
    QString setupName() const;
    int setupIndex() const;
    QVariantList blockers() const;
    bool checking() const { return checking_; }
    bool canRun() const { return runBlockedReason().isEmpty(); }
    QString runBlockedReason() const;
    bool canGenerate() const;
    QString state() const { return state_; }
    bool running() const { return state_ == QLatin1String("running"); }
    QString message() const { return message_; }
    double progress() const { return progress_; }
    QString stage() const { return stage_; }
    bool hasResult() const { return result_ != nullptr; }
    bool outOfDate() const { return result_ != nullptr && !outOfDateReason_.isEmpty(); }
    QString outOfDateReason() const { return result_ ? outOfDateReason_ : QString(); }
    QString resolutionText() const;
    QString summary() const;
    int errorCount() const;
    int warningCount() const;
    QVariantList events() const;
    QVariantList operations() const;
    double totalSec() const { return result_ ? result_->timeline.totalSec : 0; }
    double positionSec() const { return positionSec_; }
    double targetSec() const { return targetSec_; }
    bool seeking() const { return seekJob_.valid() || pending_.has_value(); }
    QString positionText() const;
    QString timeText() const;
    int currentEvent() const { return currentEvent_; }
    bool playing() const { return playing_; }
    int speedIndex() const { return speedIndex_; }
    void setSpeedIndex(int index);
    QStringList speeds() const;
    bool showStock() const { return showStock_; }
    void setShowStock(bool on);
    bool showTool() const { return showTool_; }
    void setShowTool(bool on);
    bool showHolder() const { return showHolder_; }
    void setShowHolder(bool on);
    bool showFixtures() const { return showFixtures_; }
    void setShowFixtures(bool on);
    bool showToolpath() const { return showToolpath_; }
    void setShowToolpath(bool on);
    QString gcodeState() const;
    QString gcodeText() const;

    // ---- QML ----
    Q_INVOKABLE void open(); // shows the Simulate workspace
    Q_INVOKABLE void setSetup(const QString& setupId);
    Q_INVOKABLE bool run();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool generateAll();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void stepForward();
    Q_INVOKABLE void stepBack();
    Q_INVOKABLE void toStart();
    Q_INVOKABLE void toEnd();
    Q_INVOKABLE void nextEvent();
    Q_INVOKABLE void previousEvent();
    Q_INVOKABLE void faster();
    Q_INVOKABLE void slower();
    // A place on the track (0..1 of the timeline): the stock and the tool go there (coalesced with the requests
    // before).
    Q_INVOKABLE void scrubTo(double fraction);
    Q_INVOKABLE void selectEvent(int index);
    // The timeline's markers for a track `widthPx` wide: [{x (px), severity, count, first (event index), events}].
    Q_INVOKABLE QVariantList markers(double widthPx) const;
    Q_INVOKABLE bool post(); // the post dialog for the setup
    Q_INVOKABLE void showGcode();
    // Esc: cancels a run, else pauses playback. False when it did nothing (the key belongs to someone else then).
    Q_INVOKABLE bool escape();

    // ---- the command catalog's hooks (cam.sim.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    std::shared_ptr<const api::cam_sim::RunResult> result() const { return result_; }
    std::optional<SetupId> setupIdValue() const { return setup_; }
    sim::TimelinePosition position() const { return position_; }
    Xform toolPose() const { return toolPose_; }
    render::ItemId stockItem() const { return stockItem_; }
    render::ItemId toolItem(std::size_t op) const { return op < glyphs_.size() ? glyphs_[op].tool : 0; }
    render::ItemId holderItem(std::size_t op) const { return op < glyphs_.size() ? glyphs_[op].holder : 0; }
    // The glyph shown now (its operation's), or -1.
    int shownOp() const { return shownOp_; }
    bool stockVisible() const { return stockVisible_; }
    bool toolVisible() const { return toolVisible_; }
    bool holderVisible() const { return holderVisible_; }
    // The toolpath items of operation `op`: the moves done, or (ahead) those still to come; and the moves each shows
    // (first, last; first > last: none).
    render::ItemId pathItem(std::size_t op, bool ahead) const
    {
        return op < paths_.size() ? (ahead ? paths_[op].ahead : paths_[op].done) : 0;
    }
    std::pair<std::uint32_t, std::uint32_t> pathRange(std::size_t op, bool ahead) const
    {
        if (op >= paths_.size())
            return {1, 0};
        return ahead ? paths_[op].aheadRange : paths_[op].doneRange;
    }
    bool toolpathVisible() const { return toolpathVisible_; }
    // Tests only: events every later run adds to the simulator's (cam_sim::RunRequest::addedEvents).
    void setAddedEvents(std::vector<sim::SimEvent> events) { addedEvents_ = std::move(events); }
    // Bodies the workspace hides now (by scene index).
    std::vector<int> hiddenBodies() const { return std::vector<int>(hidden_.begin(), hidden_.end()); }
    std::uint32_t triangleBudget() const { return budget_; }
    // Frames applied (seeks and refreshes), the last one's numbers, and the most triangles one frame brought.
    int framesApplied() const { return frames_; }
    const api::cam_sim::Frame& lastFrame() const { return lastFrame_; }
    std::uint64_t mostFrameTriangles() const { return mostFrameTriangles_; }
    double worstSeekMs() const { return worstSeekMs_; }
    void resetStats()
    {
        mostFrameTriangles_ = 0;
        worstSeekMs_ = 0;
    }
    int runsStarted() const { return runsStarted_; }
    bool busy() const; // a run, a seek, a gate check or a carried-stock request under way
    // The line the viewer was last moved to for an event (-1 none).
    int gcodeLine() const { return gcodeLine_; }

signals:
    void setupsChanged();
    void setupChanged();
    void gateChanged();
    void stateChanged();
    void progressChanged();
    void resultChanged();
    void outOfDateChanged();
    void positionChanged();
    void currentEventChanged();
    void playingChanged();
    void speedChanged();
    void visibilityChanged();
    void gcodeChanged();

private:
    struct Glyph {
        render::ItemId tool = 0, holder = 0;
        bool toolShown = false, holderShown = false;
    };
    // One operation's toolpath: two items on its backplot (moves done, moves ahead), the ranges last set, shown.
    struct Path {
        render::ItemId done = 0, ahead = 0;
        std::uint32_t moves = 0;
        std::pair<std::uint32_t, std::uint32_t> doneRange{0, ~0u}, aheadRange{0, ~0u}; // as addBackplot leaves them
        bool shown = false;
    };
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void onDocumentChanged();
    void onWorkspaceChanged();
    bool inWorkspace() const;
    void refreshSetups();
    void scheduleCheck();
    void check();
    void updateOutOfDate();
    void setState(const QString& state, const QString& message = QString());
    void install(std::shared_ptr<api::cam_sim::SimSession> session, std::shared_ptr<api::cam_sim::RunResult> result);
    void clearResult();
    void release(std::shared_ptr<api::cam_sim::SimSession> session);
    void removeItems();
    render::IRenderViewport* viewport() const;
    void ensureItems();
    void applyFrame(const api::cam_sim::Frame& frame);
    void requestSeek(const sim::TimelinePosition& to);
    void startNext();
    void onTick();
    void applyVisibility(bool bodies = true);
    void applyBodies();
    void restoreBodies();
    void applyMarks();
    void applyPathRanges();
    void followGcode();
    void seekToLine(int line);
    std::optional<std::uint32_t> sectionOf(std::uint32_t opIndex) const;
    std::vector<int> timeOrder() const;

    QPointer<SceneController> scene_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<SetupTreeModel> tree_;
    QPointer<PostToolModel> post_;
    QPointer<GcodeModel> viewer_;
    QPointer<QQmlEngine> qml_;
    bool linked_ = false;

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;
    cam::ICamEngine* engine_ = nullptr;
    int engineListener_ = 0;

    std::optional<SetupId> setup_;
    bool setupChosen_ = false;
    std::vector<std::pair<SetupId, QString>> setupList_;

    // Gate.
    QTimer checkTimer_;
    bool checking_ = false;
    std::uint64_t checkSerial_ = 0;
    std::vector<api::cam_post::Blocker> blockers_;
    std::map<OperationId, cam::OpState> statuses_;
    bool statusesKnown_ = false;
    JobHandle<std::map<OperationId, cam::OpState>> statusJob_;
    JobHandle<std::shared_ptr<const std::map<SetupId, Status>>> setupJob_;

    // Run.
    QString state_ = QStringLiteral("idle");
    QString message_;
    double progress_ = -1;
    QString stage_;
    std::uint64_t runSerial_ = 0;
    int runsStarted_ = 0;
    JobHandle<std::shared_ptr<const cam::CarriedStock>> carriedJob_;
    JobHandle<std::shared_ptr<api::cam_sim::RunResult>> runJob_;
    std::shared_ptr<api::cam_sim::SimSession> running_;

    // Result.
    std::shared_ptr<api::cam_sim::SimSession> sim_;
    std::shared_ptr<const api::cam_sim::RunResult> result_;
    QString outOfDateReason_;
    QString tierName_;
    std::uint32_t budget_ = 400'000;

    // Display.
    render::IRenderViewport* itemsViewport_ = nullptr;
    render::ItemId stockItem_ = 0;
    std::vector<Glyph> glyphs_;
    std::vector<Path> paths_;
    bool toolpathVisible_ = false, pathStale_ = false;
    int shownOp_ = -1;
    std::uint32_t shownVersion_ = 0;
    bool needFull_ = false;
    bool coarseShown_ = false;
    bool stockVisible_ = false, toolVisible_ = false, holderVisible_ = false;
    std::set<int> hidden_;
    // By scene index: the item, whether the workspace hides it and whether the scene shows it, as last applied.
    std::map<int, std::tuple<render::ItemId, bool, bool>> bodyItems_;
    bool stockShownKnown_ = false, stockStale_ = false;
    bool bodiesTouched_ = false;
    bool showStock_ = true, showTool_ = true, showHolder_ = true, showFixtures_ = true, showToolpath_ = true;
    std::vector<sim::SimEvent> addedEvents_; // tests only

    // Position, seeks and playback.
    sim::TimelinePosition position_{0, 0, 0};
    sim::TimelinePosition requested_{0, 0, 0};
    Xform toolPose_ = Xform::identity();
    double positionSec_ = 0, targetSec_ = 0;
    std::optional<sim::TimelinePosition> pending_;
    JobHandle<api::cam_sim::Frame> seekJob_;
    std::uint64_t seekSerial_ = 0;
    QElapsedTimer lastSeekStart_;
    bool playing_ = false;
    int speedIndex_ = camui::kDefaultSpeedIndex;
    double playSec_ = 0;
    QTimer playTimer_;
    QElapsedTimer playClock_;
    int currentEvent_ = -1;

    // Diagnostics.
    int frames_ = 0;
    api::cam_sim::Frame lastFrame_;
    std::uint64_t mostFrameTriangles_ = 0;
    double worstSeekMs_ = 0;
    int gcodeLine_ = -1;
};

} // namespace ray::ui
