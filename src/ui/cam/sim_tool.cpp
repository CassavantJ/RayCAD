#include "sim_tool.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../session.h"
#include "../ui_commands.h"
#include "operation_view.h" // ToolpathDisplay::pathStyle: the toolpaths drawn as the CAM workspace draws them
#include "post_tool.h"
#include "post_view.h"
#include "setup_tool.h" // setup_tree.h's inline setSetupTool needs the whole type
#include "setup_tree.h"

#include <ray_render/viewport.h>

#include <QtCore/QCoreApplication>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ray::ui {

using namespace modelui;
namespace cs = api::cam_sim;
namespace cp = api::cam_post;

namespace {
// T-074: the Simulate workspace. cam.sim.open works everywhere (it shows the workspace); the rest only there.
// clang-format off
constexpr UiCommand kCamSimCommands[] = {
    {"cam.sim.open", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate workspace"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows the Simulate workspace: cut the stock move by move, find collisions and gouges, and go to their G-code lines"), "", CommandContext::Global, ""},
    {"cam.sim.run", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Run the simulation"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulates the chosen setup: material removal, collision checks and gouges, with a timeline"), "", CommandContext::Sim, ""},
    {"cam.sim.play", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Play or pause"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Plays the simulation at the chosen speed, or pauses it"), "Space", CommandContext::Sim, ""},
    {"cam.sim.stepForward", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Step one move forward"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Moves the simulation to the end of the next move"), "Right", CommandContext::Sim, ""},
    {"cam.sim.stepBack", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Step one move back"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Moves the simulation back to the start of the move"), "Left", CommandContext::Sim, ""},
    {"cam.sim.start", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Go to the start"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows the stock before the first move"), "Home", CommandContext::Sim, ""},
    {"cam.sim.end", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Go to the end"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows the stock after the last move"), "End", CommandContext::Sim, ""},
    {"cam.sim.nextEvent", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Next problem"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Goes to the next collision, gouge or warning on the timeline"), "Ctrl+Right", CommandContext::Sim, ""},
    {"cam.sim.previousEvent", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Previous problem"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Goes to the previous collision, gouge or warning on the timeline"), "Ctrl+Left", CommandContext::Sim, ""},
    {"cam.sim.faster", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Play faster"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "The next faster playback speed"), "]", CommandContext::Sim, ""},
    {"cam.sim.slower", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Play slower"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "The next slower playback speed"), "[", CommandContext::Sim, ""},
    {"cam.sim.toggleStock", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Show or hide the stock"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows or hides the simulated stock (the part shows while it is hidden)"), "", CommandContext::Sim, ""},
    {"cam.sim.toggleTool", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Show or hide the tool"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows or hides the cutting tool"), "", CommandContext::Sim, ""},
    {"cam.sim.toggleHolder", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Show or hide the holder"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows or hides the tool holder"), "", CommandContext::Sim, ""},
    {"cam.sim.toggleFixtures", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Show or hide the fixtures"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows or hides the vise and other fixtures in the Simulate workspace"), "", CommandContext::Sim, ""},
    {"cam.sim.toggleToolpath", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Show or hide the toolpath"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Simulate"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows or hides the simulated toolpaths: the moves done in full, the moves to come dimmed"), "", CommandContext::Sim, ""},
};
// clang-format on

constexpr const char* kTrContext = "ray::ui::SimToolModel";
// During playback a seek starts at most this often (the render thread packs and uploads what each one changed).
constexpr int kMinFrameMs = 30;
constexpr int kTickMs = 16;
// The mark of a setup's event reaches this far past the overlap's depth (a fixture in the stock: the coloured patch of
// stock around where it goes in).
constexpr double kSetupMarkMm = 3.0;

QString trs(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

const cam::Setup* findSetup(const doc::DocState& state, const SetupId& id)
{
    if (!state.cam)
        return nullptr;
    const auto it = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                 [&](const cam::Setup& s) { return s.id == id; });
    return it == state.cam->setups.end() ? nullptr : &*it;
}

camui::SimSeverity severityOf(sim::Severity s)
{
    switch (s) {
    case sim::Severity::Error:
        return camui::SimSeverity::Error;
    case sim::Severity::Warning:
        return camui::SimSeverity::Warning;
    case sim::Severity::Info:
        break;
    }
    return camui::SimSeverity::Info;
}

QString tierLabel(render::Tier tier)
{
    switch (tier) {
    case render::Tier::Low:
        return trs("Low");
    case render::Tier::Medium:
        return trs("Medium");
    case render::Tier::High:
        return trs("High");
    }
    return {};
}

std::vector<Vec2> joined(const std::vector<Vec2>& a, const std::vector<Vec2>& b)
{
    std::vector<Vec2> out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

double radiusOf(const std::vector<Vec2>& profile)
{
    double r = 0;
    for (const Vec2& v : profile)
        r = std::max(r, v.x);
    return r;
}

} // namespace

std::span<const UiCommand> camSimUiCommands()
{
    return kCamSimCommands;
}

// ---------- construction ----------
SimToolModel::SimToolModel(SceneController* scene, Session* session, AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), session_(session), context_(context)
{
    checkTimer_.setSingleShot(true);
    connect(&checkTimer_, &QTimer::timeout, this, [this] { check(); });
    playTimer_.setInterval(kTickMs);
    playTimer_.setTimerType(Qt::PreciseTimer);
    connect(&playTimer_, &QTimer::timeout, this, [this] { onTick(); });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (session_ && session_->documentId().toStdString() != docId_) {
                cancel();
                clearResult();
                setup_.reset();
                setupChosen_ = false;
                subscribe();
                refreshSetups();
                scheduleCheck();
            }
        });
    }
    if (context_)
        connect(context_, &AppContext::workspaceChanged, this, [this] { onWorkspaceChanged(); });
    if (scene_) {
        // A new body set makes new viewport items: the workspace's hiding applies to them too.
        connect(scene_, &SceneController::bodiesChanged, this, [this] {
            bodyItems_.clear(); // the scene set its items' visibility again
            if (inWorkspace())
                applyBodies();
        });
        connect(scene_, &SceneController::stateChanged, this, [this] {
            if (inWorkspace())
                applyBodies();
        });
        connect(scene_, &SceneController::bodyVisibilityChanged, this, [this] {
            if (inWorkspace())
                applyBodies();
        });
    }
}

SimToolModel::~SimToolModel()
{
    if (commands_)
        commands_->setUiHandler(QStringLiteral("cam.sim."), {});
    playTimer_.stop();
    carriedJob_.cancel();
    runJob_.cancel();
    restoreBodies();
    removeItems();
    unsubscribe();
}

SimToolModel* SimToolModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
    }
    auto* model = new SimToolModel(scene, session, context);
    model->qml_ = qml; // the CAM tree, posting and the G-code viewer are looked up when first needed (ensureLinks)
    model->setCommands(commands);
    model->subscribe();
    model->refreshSetups();
    return model; // owned by the engine
}

// The CAM workspace's singletons this one works with, made when the Simulate workspace is first used rather than at
// start-up (the shell makes this singleton at start for its commands).
void SimToolModel::ensureLinks()
{
    if (!qml_ || linked_)
        return;
    linked_ = true;
    const QString uri = QStringLiteral("RayCad.Ui");
    if (!tree_)
        setTree(qml_->singletonInstance<SetupTreeModel*>(uri, QStringLiteral("CamTreeModel")));
    if (!post_)
        setPost(qml_->singletonInstance<PostToolModel*>(uri, QStringLiteral("CamPost")));
    if (!viewer_)
        setViewer(qml_->singletonInstance<GcodeModel*>(uri, QStringLiteral("CamGcode")));
}

void SimToolModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<SimToolModel> self(this);
    commands_->setUiHandler(
        QStringLiteral("cam.sim."),
        CommandCatalog::UiHandler{
            [self](const QString& id) { return self ? self->commandReason(id) : trs("Simulation is not available"); },
            [self](const QString& id) { return self && self->runCommand(id); }});
}

void SimToolModel::setViewer(GcodeModel* viewer)
{
    if (viewer_)
        disconnect(viewer_, nullptr, this, nullptr);
    viewer_ = viewer;
    if (viewer_) {
        connect(viewer_, &GcodeModel::programChanged, this, [this] {
            emit gcodeChanged();
            // A program posted for this run: the viewer, when open, follows the stock again.
            followGcode();
        });
        connect(viewer_, &GcodeModel::lineChosen, this, [this](int line) { seekToLine(line); });
    }
}

// ---------- document access ----------
api::IApp* SimToolModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* SimToolModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void SimToolModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<SimToolModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->onDocumentChanged();
    });
    engine_ = a->cam();
    if (engine_ != nullptr) {
        engineListener_ = engine_->subscribe([self](const OperationId&) {
            // A generation started, progressed or ended: the gate again, coalesced (progress comes at 30 Hz).
            if (self && (self->inWorkspace() || self->result_) && !self->checkTimer_.isActive()) {
                self->checkTimer_.setInterval(100);
                self->checkTimer_.start();
            }
        });
    }
}

void SimToolModel::unsubscribe()
{
    if (doc::Document* d = liveDoc(); d != nullptr && docListener_ != 0)
        d->unsubscribe(docListener_);
    if (engine_ != nullptr && engineListener_ != 0 && app() != nullptr && app()->cam() == engine_)
        engine_->unsubscribe(engineListener_);
    engine_ = nullptr;
    engineListener_ = 0;
    doc_ = nullptr;
    docListener_ = 0;
    docId_.clear();
    ++docGeneration_;
    ++checkSerial_;
    statusJob_.cancel();
    setupJob_.cancel();
    statusesKnown_ = false;
}

void SimToolModel::onDocumentChanged()
{
    refreshSetups();
    scheduleCheck();
    if (inWorkspace())
        applyBodies();
}

bool SimToolModel::inWorkspace() const
{
    return context_ && context_->workspace() == kSimWorkspace;
}

void SimToolModel::onWorkspaceChanged()
{
    if (inWorkspace()) {
        ensureLinks();
        if (doc_ == nullptr || liveDoc() == nullptr)
            subscribe();
        refreshSetups();
        if (!setupChosen_) {
            // The CAM tree's setup, else the first.
            std::optional<SetupId> pick = tree_ ? tree_->currentSetup() : std::nullopt;
            if (!pick && !setupList_.empty())
                pick = setupList_.front().first;
            if (pick && (!setup_ || *setup_ != *pick)) {
                setup_ = pick;
                emit setupChanged();
            }
        }
        scheduleCheck();
    } else {
        pause();
    }
    applyVisibility();
}

void SimToolModel::refreshSetups()
{
    std::vector<std::pair<SetupId, QString>> list;
    if (doc::Document* d = liveDoc()) {
        const doc::DocStatePtr state = d->state();
        if (state && state->cam) {
            for (const cam::Setup& s : state->cam->setups)
                list.emplace_back(s.id, qs(s.name));
        }
    }
    const bool same = list == setupList_;
    setupList_ = std::move(list);
    if (setup_ &&
        std::none_of(setupList_.begin(), setupList_.end(), [&](const auto& p) { return p.first == *setup_; })) {
        setup_.reset();
        setupChosen_ = false;
        emit setupChanged();
    }
    if (!setup_ && !setupList_.empty() && inWorkspace()) {
        setup_ = setupList_.front().first;
        emit setupChanged();
    }
    if (!same) {
        emit setupsChanged();
        emit setupChanged(); // the name may have changed
    }
}

QVariantList SimToolModel::setups() const
{
    QVariantList out;
    for (const auto& [id, name] : setupList_)
        out.push_back(QVariantMap{{QStringLiteral("id"), idText(id.value)}, {QStringLiteral("name"), name}});
    return out;
}

QString SimToolModel::setupIdText() const
{
    return setup_ ? idText(setup_->value) : QString();
}

QString SimToolModel::setupName() const
{
    for (const auto& [id, name] : setupList_) {
        if (setup_ && id == *setup_)
            return name;
    }
    return {};
}

int SimToolModel::setupIndex() const
{
    for (std::size_t i = 0; i < setupList_.size(); ++i) {
        if (setup_ && setupList_[i].first == *setup_)
            return static_cast<int>(i);
    }
    return -1;
}

void SimToolModel::setSetup(const QString& setupId)
{
    const SetupId id{Uuid::fromString(setupId.toStdString())};
    setupChosen_ = true;
    if (setup_ && *setup_ == id)
        return;
    if (running())
        cancel();
    clearResult();
    setup_ = id;
    emit setupChanged();
    scheduleCheck();
}

void SimToolModel::open()
{
    if (context_ && context_->workspace() != kSimWorkspace)
        context_->setWorkspace(kSimWorkspace);
}

// ---------- the gate ----------
void SimToolModel::scheduleCheck()
{
    if (!inWorkspace() && !result_)
        return;
    checkTimer_.setInterval(0);
    checkTimer_.start();
}

void SimToolModel::check()
{
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (d == nullptr || jobs == nullptr || engine == nullptr)
        return;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    statusJob_.cancel();
    setupJob_.cancel();
    const std::uint64_t serial = ++checkSerial_;
    checking_ = true;
    emit gateChanged();
    statusJob_ = engine->statusAsync(state, build);
    setupJob_ = jobs->submit<std::shared_ptr<const std::map<SetupId, Status>>>(
        JobInfo{"Simulation check", JobPriority::Interactive, false},
        [state, build](JobContext& ctx) -> Result<std::shared_ptr<const std::map<SetupId, Status>>> {
            return std::make_shared<const std::map<SetupId, Status>>(api::cam_setup::setupStates(state, build, ctx));
        });
    auto statuses = std::make_shared<std::optional<std::map<OperationId, cam::OpState>>>();
    auto setups = std::make_shared<std::optional<std::shared_ptr<const std::map<SetupId, Status>>>>();
    QPointer<SimToolModel> self(this);
    const auto both = [self, serial, state, build, statuses, setups] {
        if (!self || serial != self->checkSerial_ || !statuses->has_value() || !setups->has_value())
            return;
        self->statuses_ = **statuses;
        self->statusesKnown_ = true;
        self->blockers_.clear();
        if (self->setup_) {
            Status own = std::monostate{};
            if (const auto it = (**setups)->find(*self->setup_); it != (**setups)->end())
                own = it->second;
            self->blockers_ = cp::blockers(*state, build != nullptr, *self->setup_, self->statuses_, own);
        }
        self->checking_ = false;
        self->updateOutOfDate();
        emit self->gateChanged();
    };
    statusJob_.then([self, serial, statuses, both](Result<std::map<OperationId, cam::OpState>> r) {
        if (!self || serial != self->checkSerial_ || !r)
            return;
        *statuses = std::move(*r);
        both();
    });
    setupJob_.then([self, serial, setups, both](Result<std::shared_ptr<const std::map<SetupId, Status>>> r) {
        if (!self || serial != self->checkSerial_ || !r)
            return;
        *setups = *r;
        both();
    });
}

QVariantList SimToolModel::blockers() const
{
    QVariantList out;
    for (const cp::Blocker& b : blockers_) {
        out.push_back(QVariantMap{{QStringLiteral("opId"), (b.op.value == Uuid{}) ? QString() : idText(b.op.value)},
                                  {QStringLiteral("name"), b.name.empty() ? trs("Setup") : qs(b.name)},
                                  {QStringLiteral("reason"), qs(std::string(cp::reasonKey(b.reason)))},
                                  {QStringLiteral("message"), qs(b.message)},
                                  {QStringLiteral("canGenerate"), b.generateFixes}});
    }
    return out;
}

QString SimToolModel::runBlockedReason() const
{
    if (app() == nullptr || liveDoc() == nullptr)
        return trs("Open a part first");
    if (!setup_)
        return trs("Add a setup in the CAM workspace first");
    if (running())
        return trs("A simulation is running");
    if (!statusesKnown_ || checking_)
        return trs("Checking the toolpaths…");
    if (!blockers_.empty())
        return qs(cp::blockersText(blockers_));
    return {};
}

bool SimToolModel::canGenerate() const
{
    return std::any_of(blockers_.begin(), blockers_.end(),
                       [](const cp::Blocker& b) { return b.generateFixes && !(b.op.value == Uuid{}); });
}

bool SimToolModel::generateAll()
{
    doc::Document* d = liveDoc();
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (d == nullptr || engine == nullptr || !d->build())
        return false;
    bool any = false;
    for (const cp::Blocker& b : blockers_) {
        if (!b.generateFixes || b.op.value == Uuid{})
            continue;
        JobHandle<std::shared_ptr<const cam::Toolpath>> job = engine->generate(d->state(), d->build(), b.op);
        QPointer<SimToolModel> self(this);
        job.then([self](Result<std::shared_ptr<const cam::Toolpath>>) {
            if (self)
                self->scheduleCheck();
        });
        any = true;
    }
    scheduleCheck(); // shows Generating
    return any;
}

// The result belongs to the toolpaths it was run on.
void SimToolModel::updateOutOfDate()
{
    QString reason;
    if (result_) {
        doc::Document* d = liveDoc();
        const doc::DocStatePtr state = d ? d->state() : nullptr;
        const cam::Setup* setup = state ? findSetup(*state, result_->setup) : nullptr;
        if (setup == nullptr) {
            reason = trs("The setup was deleted.");
        } else if (statusesKnown_) {
            std::vector<OperationId> ops;
            for (const OperationId& id : setup->operations) {
                const auto op = state->cam->operations.find(id);
                if (op != state->cam->operations.end() && !op->second.suppressed)
                    ops.push_back(id);
            }
            if (ops != result_->ops) {
                reason = trs("The setup's operations changed since this simulation ran.");
            } else {
                for (std::size_t i = 0; i < ops.size() && reason.isEmpty(); ++i) {
                    const auto st = statuses_.find(ops[i]);
                    const bool same = st != statuses_.end() && st->second.status == cam::OpStatus::UpToDate &&
                                      st->second.toolpath && st->second.toolpath->inputHash == result_->inputs[i];
                    if (!same)
                        reason = trs("%1 changed since this simulation ran.").arg(qs(result_->opNames[i]));
                }
            }
        }
    }
    if (reason == outOfDateReason_)
        return;
    outOfDateReason_ = reason;
    applyVisibility(); // the stale colour
    emit outOfDateChanged();
    emit gcodeChanged();
}

// ---------- the run ----------
void SimToolModel::setState(const QString& state, const QString& message)
{
    state_ = state;
    message_ = message;
    emit stateChanged();
    emit gateChanged(); // canRun follows running
}

bool SimToolModel::run()
{
    ensureLinks();
    if (running())
        return false;
    if (const QString why = runBlockedReason(); !why.isEmpty()) {
        message_ = why;
        emit stateChanged();
        return false;
    }
    doc::Document* d = liveDoc();
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    const doc::BuildResultPtr build = d ? d->build() : nullptr;
    const cam::Setup* setup = state ? findSetup(*state, *setup_) : nullptr;
    if (setup == nullptr || !build || engine == nullptr || jobs == nullptr)
        return false;
    // The setup's operations as the gate saw them: each up to date.
    cs::RunRequest request;
    request.state = state;
    request.build = build;
    request.setup = setup->id;
    for (const OperationId& id : setup->operations) {
        const auto op = state->cam->operations.find(id);
        if (op == state->cam->operations.end() || op->second.suppressed)
            continue;
        const auto st = statuses_.find(id);
        if (st == statuses_.end() || st->second.status != cam::OpStatus::UpToDate || !st->second.toolpath) {
            scheduleCheck();
            return false;
        }
        request.toolpaths.push_back(st->second.toolpath);
        request.toolNumbers.push_back(op->second.tool.toolNumber);
    }
    const render::TierPreset preset = scene_ ? scene_->preset() : render::presetFor(render::Tier::Medium);
    request.cellMm = preset.simCellMm;
    budget_ = camui::stockTriangleBudget(preset.simCellMm);
    tierName_ = tierLabel(preset.tier);
    request.mesh.budget = budget_;
    request.mesh.full = true;
    request.addedEvents = addedEvents_; // tests only
    request.seekReport = seekReport_;   // tests only

    pause();
    clearResult();
    const std::uint64_t serial = ++runSerial_;
    ++runsStarted_;
    progress_ = 0;
    stage_ = trs("Starting");
    setState(QStringLiteral("running"));
    emit progressChanged();
    if (std::holds_alternative<cam::FromSetupStock>(setup->stock)) {
        // The stock it starts from: the earlier setup simulated by the CAM engine (cached), as an app job.
        stage_ = trs("Simulating the earlier setup for the stock");
        emit progressChanged();
        carriedJob_ = engine->carriedStock(state, build, setup->id);
        QPointer<SimToolModel> self(this);
        carriedJob_.onProgress([self, serial](double f, std::string) {
            if (!self || serial != self->runSerial_)
                return;
            self->progress_ = 0.3 * f;
            emit self->progressChanged();
        });
        auto req = std::make_shared<cs::RunRequest>(std::move(request));
        carriedJob_.then([self, serial, req](Result<std::shared_ptr<const cam::CarriedStock>> r) {
            if (!self || serial != self->runSerial_)
                return;
            self->carriedJob_ = {};
            if (!r) {
                if (r.error().code == ErrorCode::Cancelled)
                    self->setState(QStringLiteral("cancelled"), trs("Simulation cancelled"));
                else
                    self->setState(QStringLiteral("failed"),
                                   trs("The stock from the earlier setup: %1").arg(qs(r.error().message)));
                return;
            }
            req->startStock = *r;
            self->progress_ = 0.3;
            emit self->progressChanged();
            // startRun reads the request from here.
            IJobSystem* jobs = self->scene_ ? self->scene_->jobs() : nullptr;
            if (jobs == nullptr)
                return;
            auto session = std::make_shared<cs::SimSession>(*jobs);
            self->running_ = session;
            const double base = 0.3;
            self->runJob_ = jobs->submit<std::shared_ptr<cs::RunResult>>(
                JobInfo{"Simulation", JobPriority::Normal, false},
                [session, req](JobContext& ctx) -> Result<std::shared_ptr<cs::RunResult>> {
                    return session->run(*req, ctx);
                });
            self->runJob_.onProgress([self, serial, base](double f, std::string stage) {
                if (!self || serial != self->runSerial_)
                    return;
                self->progress_ = base + (1 - base) * f;
                if (!stage.empty())
                    self->stage_ = qs(stage);
                emit self->progressChanged();
            });
            self->runJob_.then([self, serial](Result<std::shared_ptr<cs::RunResult>> res) {
                if (!self || serial != self->runSerial_)
                    return;
                self->runJob_ = {};
                std::shared_ptr<cs::SimSession> owned = std::move(self->running_);
                if (!res) {
                    self->release(std::move(owned));
                    if (res.error().code == ErrorCode::Cancelled)
                        self->setState(QStringLiteral("cancelled"), trs("Simulation cancelled"));
                    else
                        self->setState(QStringLiteral("failed"), qs(res.error().message));
                    return;
                }
                self->install(std::move(owned), *res);
            });
        });
        return true;
    }
    auto req = std::make_shared<cs::RunRequest>(std::move(request));
    auto session = std::make_shared<cs::SimSession>(*jobs);
    running_ = session;
    runJob_ = jobs->submit<std::shared_ptr<cs::RunResult>>(
        JobInfo{"Simulation", JobPriority::Normal, false},
        [session, req](JobContext& ctx) -> Result<std::shared_ptr<cs::RunResult>> { return session->run(*req, ctx); });
    QPointer<SimToolModel> self(this);
    runJob_.onProgress([self, serial](double f, std::string stage) {
        if (!self || serial != self->runSerial_)
            return;
        self->progress_ = f;
        if (!stage.empty())
            self->stage_ = qs(stage);
        emit self->progressChanged();
    });
    runJob_.then([self, serial](Result<std::shared_ptr<cs::RunResult>> res) {
        if (!self || serial != self->runSerial_)
            return;
        self->runJob_ = {};
        std::shared_ptr<cs::SimSession> owned = std::move(self->running_);
        if (!res) {
            self->release(std::move(owned));
            if (res.error().code == ErrorCode::Cancelled)
                self->setState(QStringLiteral("cancelled"), trs("Simulation cancelled"));
            else
                self->setState(QStringLiteral("failed"), qs(res.error().message));
            return;
        }
        self->install(std::move(owned), *res);
    });
    return true;
}

void SimToolModel::cancel()
{
    if (!running())
        return;
    ++runSerial_; // whatever arrives late is dropped
    carriedJob_.cancel();
    runJob_.cancel();
    carriedJob_ = {};
    runJob_ = {};
    release(std::move(running_));
    setState(QStringLiteral("cancelled"), trs("Simulation cancelled"));
}

bool SimToolModel::escape()
{
    if (running()) {
        cancel();
        return true;
    }
    if (playing_) {
        pause();
        return true;
    }
    return false;
}

void SimToolModel::install(std::shared_ptr<cs::SimSession> session, std::shared_ptr<cs::RunResult> result)
{
    cs::Frame first = result->first;
    result->first.mesh.reset(); // the result keeps its numbers, not a second copy of the mesh
    sim_ = std::move(session);
    result_ = std::move(result);
    outOfDateReason_.clear();
    currentEvent_ = -1;
    shownVersion_ = 0;
    shownOp_ = -1;
    needFull_ = false;
    coarseShown_ = false;
    pending_.reset();
    ++seekSerial_;
    position_ = first.position;
    requested_ = position_;
    positionSec_ = targetSec_ = result_->timeline.secondsAt(position_);
    progress_ = 1;
    emit progressChanged();
    applyFrame(first);
    setState(QStringLiteral("done"));
    updateOutOfDate();
    applyMarks();
    emit resultChanged();
    emit currentEventChanged();
    emit gcodeChanged();
    emit positionChanged();
    scheduleCheck();
}

// A simulator holds its keyframes (hundreds of megabytes at a fine cell): freeing them is a worker's job, never the UI
// thread's.
void SimToolModel::release(std::shared_ptr<cs::SimSession> session)
{
    if (!session)
        return;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return; // freed here, at the end of this function
    (void)jobs->submit<int>(JobInfo{"Simulation release", JobPriority::Background, false},
                            [owned = std::move(session)](JobContext&) mutable -> Result<int> {
                                owned.reset();
                                return 0;
                            });
}

void SimToolModel::clearResult()
{
    ++seekSerial_;
    pending_.reset();
    seekJob_ = {};
    playing_ = false;
    playTimer_.stop();
    removeItems();
    release(std::move(sim_));
    const bool had = result_ != nullptr;
    result_.reset();
    outOfDateReason_.clear();
    currentEvent_ = -1;
    position_ = requested_ = {0, 0, 0};
    positionSec_ = targetSec_ = 0;
    shownOp_ = -1;
    gcodeLine_ = -1;
    marks_.clear();
    applyVisibility();
    if (had) {
        emit resultChanged();
        emit outOfDateChanged();
        emit positionChanged();
        emit currentEventChanged();
        emit gcodeChanged();
        emit playingChanged();
    }
}

// ---------- display ----------
render::IRenderViewport* SimToolModel::viewport() const
{
    return scene_ ? scene_->viewport() : nullptr;
}

void SimToolModel::removeItems()
{
    render::IRenderViewport* v = viewport();
    if (v != nullptr && v == itemsViewport_) {
        if (stockItem_ != 0)
            v->remove(stockItem_);
        for (const Glyph& g : glyphs_) {
            if (g.tool != 0)
                v->remove(g.tool);
            if (g.holder != 0)
                v->remove(g.holder);
        }
        for (const Path& p : paths_) {
            if (p.done != 0)
                v->remove(p.done);
            if (p.ahead != 0)
                v->remove(p.ahead);
        }
    }
    stockItem_ = 0;
    glyphs_.clear();
    paths_.clear();
    itemsViewport_ = nullptr;
    stockVisible_ = toolVisible_ = holderVisible_ = toolpathVisible_ = false;
    pathStale_ = false;
    stockShownKnown_ = false;
    shownVersion_ = 0;
}

void SimToolModel::ensureItems()
{
    render::IRenderViewport* v = viewport();
    if (v == nullptr || !result_)
        return;
    if (v != itemsViewport_) {
        // A new viewport (or none yet): the items are made there; the stock needs a whole mesh.
        stockItem_ = 0;
        glyphs_.clear();
        paths_.clear();
        stockShownKnown_ = false;
        itemsViewport_ = v;
        shownVersion_ = 0;
        needFull_ = true;
    }
    if (glyphs_.size() != result_->tools.size()) {
        glyphs_.assign(result_->tools.size(), Glyph{});
        for (std::size_t i = 0; i < result_->tools.size(); ++i) {
            const sim::ToolShape& s = result_->tools[i];
            const std::vector<Vec2> tool = joined(s.cuttingProfile, s.shankProfile);
            if (!tool.empty()) {
                glyphs_[i].tool = v->addToolGlyph(tool, render::Token::Accent);
                v->setVisible(glyphs_[i].tool, false);
            }
            if (!s.holderProfile.empty()) {
                glyphs_[i].holder = v->addToolGlyph(s.holderProfile, render::Token::TextSecondary);
                v->setVisible(glyphs_[i].holder, false);
            }
        }
    }
    if (paths_.size() != result_->backplots.size()) {
        // The toolpaths, as the CAM workspace draws them; both items share the backplot (shown by applyVisibility).
        paths_.assign(result_->backplots.size(), Path{});
        for (std::size_t i = 0; i < result_->backplots.size(); ++i) {
            const std::shared_ptr<const sim::Backplot>& bp = result_->backplots[i];
            if (!bp || bp->moveFirstVertex.size() < 2)
                continue;
            Path& p = paths_[i];
            p.moves = static_cast<std::uint32_t>(bp->moveFirstVertex.size() - 1);
            p.done = v->addBackplot(bp);
            p.ahead = v->addBackplot(bp);
            if (p.done != 0) {
                v->setStyle(p.done, ToolpathDisplay::pathStyle(pathStale_, false));
                v->setVisible(p.done, false);
            }
            if (p.ahead != 0) {
                v->setStyle(p.ahead, ToolpathDisplay::pathStyle(pathStale_, true));
                v->setVisible(p.ahead, false);
            }
        }
    }
}

// The moves done (to the playhead, the move under way included) at full strength, the rest dimmed: a draw range each.
void SimToolModel::applyPathRanges()
{
    render::IRenderViewport* v = viewport();
    if (v == nullptr || v != itemsViewport_ || paths_.empty())
        return;
    constexpr std::pair<std::uint32_t, std::uint32_t> none{1, 0};
    const sim::TimelinePosition p = position_;
    for (std::size_t i = 0; i < paths_.size(); ++i) {
        Path& path = paths_[i];
        if (path.moves == 0)
            continue;
        // How many of its moves are begun: all before the current operation, none after it.
        std::uint32_t begun = 0;
        if (i < p.opIndex)
            begun = path.moves;
        else if (i == p.opIndex)
            begun = std::min(path.moves, p.moveIndex + (p.t01 > 0 ? 1u : 0u));
        const auto done = begun == 0 ? none : std::make_pair(0u, begun - 1);
        const auto ahead = begun >= path.moves ? none : std::make_pair(begun, path.moves - 1);
        if (path.done != 0 && done != path.doneRange)
            v->setBackplotRange(path.done, done.first, done.second);
        if (path.ahead != 0 && ahead != path.aheadRange)
            v->setBackplotRange(path.ahead, ahead.first, ahead.second);
        path.doneRange = done;
        path.aheadRange = ahead;
    }
}

void SimToolModel::applyFrame(const cs::Frame& frame)
{
    ensureItems();
    render::IRenderViewport* v = viewport();
    if (v != nullptr && result_ && frame.mesh) {
        if (stockItem_ == 0) {
            if (frame.full) {
                render::StockShading shading;
                shading.uncut = result_->stockBounds;
                stockItem_ = v->addStock(frame.mesh, result_->modelFromWcs, render::stockStyle(), shading);
                stockVisible_ = false;
                if (stockItem_ != 0)
                    v->setVisible(stockItem_, false); // applyVisibility shows it
            } else {
                needFull_ = true; // a region without the whole: ask for the whole
            }
        } else if (frame.full) {
            v->updateMesh(stockItem_, frame.mesh, std::nullopt);
        } else {
            v->updateMesh(stockItem_, frame.mesh, frame.dirty);
        }
    }
    if (stockItem_ != 0)
        shownVersion_ = frame.version;
    coarseShown_ = coarseShown_ || frame.coarse;
    if (v != nullptr && stockItem_ != 0 && v->triangleCount(stockItem_) > static_cast<std::uint64_t>(budget_) * 3 / 2)
        needFull_ = true; // finer than the display needs: the whole stock again at the budget
    position_ = frame.position;
    toolPose_ = frame.toolPose;
    if (result_)
        positionSec_ = result_->timeline.secondsAt(position_);
    if (!pending_ && !playing_)
        targetSec_ = positionSec_;
    shownOp_ = static_cast<int>(position_.opIndex);
    if (v != nullptr && shownOp_ >= 0 && static_cast<std::size_t>(shownOp_) < glyphs_.size()) {
        const Glyph& g = glyphs_[static_cast<std::size_t>(shownOp_)];
        if (g.tool != 0)
            v->setTransform(g.tool, toolPose_);
        if (g.holder != 0)
            v->setTransform(g.holder, toolPose_);
    }
    applyPathRanges();
    ++frames_;
    lastFrame_ = frame;
    lastFrame_.mesh.reset();
    mostFrameTriangles_ = std::max(mostFrameTriangles_, frame.triangles);
    worstSeekMs_ = std::max(worstSeekMs_, frame.seekMs);
    applyVisibility(false);
    followGcode();
    emit positionChanged();
}

// What the workspace shows of its own items, and (unless `bodies` is false: a frame changes none of them) which of the
// scene's bodies it hides. Only what changed reaches the viewport.
void SimToolModel::applyVisibility(bool bodies)
{
    render::IRenderViewport* v = viewport();
    const bool here = inWorkspace() && result_ != nullptr && v != nullptr && v == itemsViewport_;
    const bool stock = here && showStock_ && stockItem_ != 0;
    const bool stale = outOfDate();
    if (v != nullptr && v == itemsViewport_ && stockItem_ != 0) {
        if (stock != stockVisible_ || !stockShownKnown_)
            v->setVisible(stockItem_, stock);
        if (stale != stockStale_ || !stockShownKnown_) {
            render::MeshStyle style = render::stockStyle();
            if (stale) {
                // Not current: the stale colour, never the stock's own.
                style.face = render::Token::PathStale;
                style.edge = render::Token::PathStale;
            }
            v->setStyle(stockItem_, style);
        }
        stockShownKnown_ = true;
        stockStale_ = stale;
    }
    const bool bodiesChange = bodies || stock != stockVisible_;
    stockVisible_ = stock;
    bool toolShown = false, holderShown = false;
    for (std::size_t i = 0; i < glyphs_.size(); ++i) {
        Glyph& g = glyphs_[i];
        const bool current = here && static_cast<int>(i) == shownOp_;
        const bool tool = current && showTool_ && g.tool != 0;
        const bool holder = current && showHolder_ && g.holder != 0;
        if (v != nullptr && v == itemsViewport_) {
            if (g.tool != 0 && tool != g.toolShown)
                v->setVisible(g.tool, tool);
            if (g.holder != 0 && holder != g.holderShown)
                v->setVisible(g.holder, holder);
        }
        g.toolShown = tool;
        g.holderShown = holder;
        toolShown = toolShown || tool;
        holderShown = holderShown || holder;
    }
    toolVisible_ = toolShown;
    holderVisible_ = holderShown;
    // The toolpaths: in the stale colour while the result is out of date, as the stock.
    bool pathShown = false;
    if (v != nullptr && v == itemsViewport_) {
        const bool path = here && showToolpath_;
        const bool restyle = stale != pathStale_;
        for (Path& p : paths_) {
            for (const auto& [item, dim] : {std::make_pair(p.done, false), std::make_pair(p.ahead, true)}) {
                if (item == 0)
                    continue;
                if (restyle)
                    v->setStyle(item, ToolpathDisplay::pathStyle(stale, dim));
                if (path != p.shown)
                    v->setVisible(item, path);
            }
            p.shown = path;
            pathShown = pathShown || (path && (p.done != 0 || p.ahead != 0));
        }
        pathStale_ = stale;
    }
    toolpathVisible_ = pathShown;
    if (bodiesChange)
        applyBodies();
}

void SimToolModel::applyBodies()
{
    if (!inWorkspace()) {
        restoreBodies();
        return;
    }
    render::IRenderViewport* v = viewport();
    if (v == nullptr || !scene_)
        return;
    std::set<BodyId> parts, fixtures;
    if (doc::Document* d = liveDoc()) {
        const doc::DocStatePtr state = d->state();
        const SetupId* which = result_ ? &result_->setup : (setup_ ? &*setup_ : nullptr);
        if (const cam::Setup* s = state && which ? findSetup(*state, *which) : nullptr) {
            parts.insert(s->partBodies.begin(), s->partBodies.end());
            fixtures.insert(s->fixtureBodies.begin(), s->fixtureBodies.end());
        }
    }
    std::set<int> hidden;
    for (int i = 0; i < scene_->bodyCount(); ++i) {
        const api::viewer::Body* body = scene_->body(i);
        const render::ItemId item = scene_->itemForBody(i);
        if (body == nullptr || item == 0)
            continue;
        const bool fixture = body->fixture || fixtures.count(body->id) != 0;
        const bool hide = (fixture && !showFixtures_) || (parts.count(body->id) != 0 && stockVisible_);
        if (hide)
            hidden.insert(i);
        // Only what changes (a new body set makes new items: those get their state whatever it was).
        const bool visible = scene_->bodyVisible(i);
        const auto was = bodyItems_.find(i);
        if (was != bodyItems_.end() && was->second == std::make_tuple(item, hide, visible))
            continue;
        bodyItems_[i] = std::make_tuple(item, hide, visible);
        v->setVisible(item, visible && !hide);
        v->setPickMode(item, hide ? render::PickMode::Never : render::PickMode::WhenVisible);
    }
    bodiesTouched_ = true;
    hidden_ = std::move(hidden);
}

void SimToolModel::restoreBodies()
{
    if (!bodiesTouched_)
        return;
    bodiesTouched_ = false;
    hidden_.clear();
    bodyItems_.clear();
    render::IRenderViewport* v = viewport();
    if (v == nullptr || !scene_)
        return;
    for (int i = 0; i < scene_->bodyCount(); ++i) {
        const render::ItemId item = scene_->itemForBody(i);
        if (item == 0)
            continue;
        v->setVisible(item, scene_->bodyVisible(i));
        v->setPickMode(item, render::PickMode::WhenVisible);
    }
}

void SimToolModel::applyMarks()
{
    render::IRenderViewport* v = viewport();
    if (v == nullptr || v != itemsViewport_ || stockItem_ == 0 || !result_)
        return;
    render::StockShading shading;
    shading.uncut = result_->stockBounds;
    const int n = static_cast<int>(result_->events.size());
    // The event chosen, then the ones after it, up to the renderer's limit.
    const int from = currentEvent_ >= 0 ? currentEvent_ : 0;
    for (int k = 0; k < n && static_cast<int>(shading.marks.size()) < render::kMaxStockMarks; ++k) {
        const int e = (from + k) % n;
        const sim::SimEvent& ev = result_->events[static_cast<std::size_t>(e)];
        const cs::EventPlace& pl = result_->places[static_cast<std::size_t>(e)];
        if (ev.moveIndex == sim::kSetupEvent) {
            // The setup's (a fixture in the part or the stock): no tool; a box about the point of the overlap, reaching
            // the stock's surface (the point lies amountMm from the solid it is in) and a little more.
            const double r = kSetupMarkMm + (std::isfinite(ev.amountMm) ? std::max(0.0, ev.amountMm) : 0.0);
            render::StockMark mark;
            mark.box = Box3{{ev.position.x - r, ev.position.y - r, ev.position.z - r},
                            {ev.position.x + r, ev.position.y + r, ev.position.z + r}};
            mark.colour = render::Token::SimCollision;
            shading.marks.push_back(mark);
            continue;
        }
        if (pl.opIndex >= result_->tools.size())
            continue;
        const sim::ToolShape& tool = result_->tools[pl.opIndex];
        // The part of the tool that met something: the holder, the shank, else the cutting part (the tip's position is
        // the event's; the profiles' heights are above the tip).
        const bool holder = ev.kind == sim::EventKind::HolderHitsStock ||
                            ev.kind == sim::EventKind::HolderHitsFixture || ev.kind == sim::EventKind::HolderHitsPart;
        const bool shank = ev.kind == sim::EventKind::ShankHitsStock || ev.kind == sim::EventKind::ShankHitsPart;
        const std::vector<Vec2>& zone = holder && !tool.holderProfile.empty() ? tool.holderProfile
                                        : shank && !tool.shankProfile.empty() ? tool.shankProfile
                                                                              : tool.cuttingProfile;
        const double r = zone.empty() ? 1.0 : radiusOf(zone);
        const double low = zone.empty() ? 0.0 : zone.front().y;
        const double high = zone.empty() ? 1.0 : std::max(zone.back().y, low + 0.5);
        render::StockMark mark;
        mark.box = Box3{{ev.position.x - r - 0.5, ev.position.y - r - 0.5, ev.position.z + low - 0.5},
                        {ev.position.x + r + 0.5, ev.position.y + r + 0.5, ev.position.z + high}};
        mark.colour = ev.kind == sim::EventKind::Gouge ? render::Token::SimGouge : render::Token::SimCollision;
        shading.marks.push_back(mark);
    }
    marks_ = shading.marks;
    v->setStockShading(stockItem_, shading);
}

// ---------- seeks ----------
void SimToolModel::requestSeek(const sim::TimelinePosition& to)
{
    if (!sim_ || !result_)
        return;
    requested_ = result_->timeline.normalised(to);
    targetSec_ = result_->timeline.secondsAt(requested_);
    pending_ = requested_;
    emit positionChanged();
    startNext();
}

void SimToolModel::startNext()
{
    if (seekJob_.valid() || !sim_ || !result_)
        return;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return;
    const bool refresh = !pending_ && !playing_ && (needFull_ || coarseShown_);
    if (!pending_ && !refresh)
        return;
    if (pending_ && playing_ && lastSeekStart_.isValid() && lastSeekStart_.elapsed() < kMinFrameMs) {
        // At most one frame every kMinFrameMs while playing: the next tick asks again.
        return;
    }
    cs::MeshAsk ask;
    ask.shownVersion = shownVersion_;
    ask.budget = budget_;
    ask.full = needFull_ || stockItem_ == 0 || itemsViewport_ != viewport();
    ask.cap = playing_ ? camui::playbackCap(budget_) : 0;
    needFull_ = false;
    if (refresh)
        coarseShown_ = false;
    const std::uint64_t serial = ++seekSerial_;
    std::shared_ptr<cs::SimSession> session = sim_;
    lastSeekStart_.start();
    if (pending_) {
        const sim::TimelinePosition to = *pending_;
        pending_.reset();
        seekJob_ = jobs->submit<cs::Frame>(JobInfo{"Simulation seek", JobPriority::Interactive, false},
                                           [session, to, ask](JobContext&) { return session->seek(to, ask); });
    } else {
        ask.full = true;
        seekJob_ = jobs->submit<cs::Frame>(JobInfo{"Simulation display", JobPriority::Interactive, false},
                                           [session, ask](JobContext&) { return session->refresh(ask); });
    }
    QPointer<SimToolModel> self(this);
    const bool full = ask.full;
    seekJob_.then([self, serial, full](Result<cs::Frame> r) {
        if (!self || serial != self->seekSerial_)
            return;
        self->seekJob_ = {};
        if (r) {
            self->applyFrame(*r);
        } else if (full) {
            self->needFull_ = true;
        }
        self->startNext();
        emit self->positionChanged();
    });
}

// ---------- playback ----------
void SimToolModel::play()
{
    if (!result_ || playing_ || running())
        return;
    const double total = result_->timeline.totalSec;
    playSec_ = targetSec_ >= total - 1e-9 ? 0.0 : targetSec_;
    if (playSec_ <= 0)
        requestSeek(result_->timeline.start());
    playing_ = true;
    playClock_.start();
    playTimer_.start();
    emit playingChanged();
}

void SimToolModel::pause()
{
    if (!playing_)
        return;
    playing_ = false;
    playTimer_.stop();
    emit playingChanged();
    startNext(); // the whole display again if playback left coarse tiles
}

void SimToolModel::togglePlay()
{
    playing_ ? pause() : play();
}

void SimToolModel::onTick()
{
    if (!playing_ || !result_) {
        pause();
        return;
    }
    const double dt = static_cast<double>(playClock_.restart()) / 1000.0;
    const std::span<const double> speeds = camui::playbackSpeeds();
    const double speed = speeds[static_cast<std::size_t>(std::clamp<int>(speedIndex_, 0, int(speeds.size()) - 1))];
    playSec_ += dt * speed;
    const double total = result_->timeline.totalSec;
    if (playSec_ >= total) {
        playSec_ = total;
        requestSeek(result_->timeline.end());
        pause();
        return;
    }
    requestSeek(result_->timeline.positionAt(playSec_));
    targetSec_ = playSec_;
}

void SimToolModel::setSpeedIndex(int index)
{
    index = std::clamp<int>(index, 0, static_cast<int>(camui::playbackSpeeds().size()) - 1);
    if (index == speedIndex_)
        return;
    speedIndex_ = index;
    emit speedChanged();
}

QStringList SimToolModel::speeds() const
{
    QStringList out;
    for (const double s : camui::playbackSpeeds())
        out << camui::speedText(s);
    return out;
}

void SimToolModel::faster()
{
    setSpeedIndex(speedIndex_ + 1);
}

void SimToolModel::slower()
{
    setSpeedIndex(speedIndex_ - 1);
}

void SimToolModel::stepForward()
{
    if (!result_)
        return;
    pause();
    requestSeek(result_->timeline.step(requested_, +1));
}

void SimToolModel::stepBack()
{
    if (!result_)
        return;
    pause();
    requestSeek(result_->timeline.step(requested_, -1));
}

void SimToolModel::toStart()
{
    if (!result_)
        return;
    pause();
    requestSeek(result_->timeline.start());
}

void SimToolModel::toEnd()
{
    if (!result_)
        return;
    pause();
    requestSeek(result_->timeline.end());
}

void SimToolModel::scrubTo(double fraction)
{
    if (!result_)
        return;
    pause();
    const double f = std::isfinite(fraction) ? std::clamp(fraction, 0.0, 1.0) : 0.0;
    requestSeek(result_->timeline.positionAt(f * result_->timeline.totalSec));
}

// The events in time order (their order in events() breaks ties); within one move they need not be.
std::vector<int> SimToolModel::timeOrder() const
{
    std::vector<int> order;
    if (!result_)
        return order;
    for (std::size_t i = 0; i < result_->places.size(); ++i)
        order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return result_->places[static_cast<std::size_t>(a)].sec < result_->places[static_cast<std::size_t>(b)].sec;
    });
    return order;
}

void SimToolModel::nextEvent()
{
    const std::vector<int> order = timeOrder();
    if (order.empty())
        return;
    const auto at = std::find(order.begin(), order.end(), currentEvent_);
    if (currentEvent_ >= 0 && at != order.end() &&
        std::abs(result_->places[static_cast<std::size_t>(currentEvent_)].sec - targetSec_) <= 1e-6) {
        // On the event chosen: the one after it.
        if (std::next(at) != order.end())
            selectEvent(*std::next(at));
        return;
    }
    for (const int i : order) {
        if (result_->places[static_cast<std::size_t>(i)].sec > targetSec_ + 1e-6) {
            selectEvent(i);
            return;
        }
    }
}

void SimToolModel::previousEvent()
{
    const std::vector<int> order = timeOrder();
    if (order.empty())
        return;
    const auto at = std::find(order.begin(), order.end(), currentEvent_);
    if (currentEvent_ >= 0 && at != order.end() &&
        std::abs(result_->places[static_cast<std::size_t>(currentEvent_)].sec - targetSec_) <= 1e-6) {
        if (at != order.begin())
            selectEvent(*std::prev(at));
        return;
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (result_->places[static_cast<std::size_t>(*it)].sec < targetSec_ - 1e-6) {
            selectEvent(*it);
            return;
        }
    }
}

void SimToolModel::selectEvent(int index)
{
    if (!result_ || index < 0 || index >= static_cast<int>(result_->events.size()))
        return;
    pause();
    currentEvent_ = index;
    const cs::EventPlace& pl = result_->places[static_cast<std::size_t>(index)];
    requestSeek(sim::TimelinePosition{pl.opIndex, pl.moveIndex, pl.t01});
    applyMarks();
    emit currentEventChanged();
    // The G-code line at once (the seek may take a moment); a setup's event has no move, so no line.
    if (result_->events[static_cast<std::size_t>(index)].moveIndex != sim::kSetupEvent &&
        gcodeState() == QLatin1String("current") && viewer_) {
        if (const auto section = sectionOf(pl.opIndex)) {
            viewer_->showMove(*section, pl.moveIndex);
            gcodeLine_ = viewer_->currentLine();
        }
    }
}

// ---------- G-code ----------
std::optional<std::uint32_t> SimToolModel::sectionOf(std::uint32_t opIndex) const
{
    const PostedProgram* p = viewer_ ? viewer_->program() : nullptr;
    if (p == nullptr || !result_ || opIndex >= result_->ops.size())
        return std::nullopt;
    for (std::size_t i = 0; i < p->sectionOps.size(); ++i) {
        if (p->sectionOps[i] == result_->ops[opIndex])
            return static_cast<std::uint32_t>(i);
    }
    return std::nullopt;
}

QString SimToolModel::gcodeState() const
{
    if (!result_)
        return {};
    const PostedProgram* p = viewer_ ? viewer_->program() : nullptr;
    if (p == nullptr || p->setup != result_->setup)
        return QStringLiteral("none");
    if (p->sectionOps != result_->ops || p->sectionInputs != result_->inputs)
        return QStringLiteral("stale");
    return QStringLiteral("current");
}

QString SimToolModel::gcodeText() const
{
    const QString s = gcodeState();
    const QString name = result_ ? qs(result_->setupName) : QString();
    if (s == QLatin1String("current"))
        return trs("The G-code viewer follows the simulation: a problem's move shows its lines.");
    if (s == QLatin1String("stale"))
        return trs("The program posted for %1 was made from other toolpaths: post it again to see the G-code line of "
                   "each move.")
            .arg(name);
    if (s == QLatin1String("none"))
        return trs("No program of %1 has been posted in this session: post it to see the G-code line of each move.")
            .arg(name);
    return {};
}

bool SimToolModel::post()
{
    ensureLinks();
    if (!post_ || !setup_)
        return false;
    pause();
    return post_->open(setupIdText());
}

void SimToolModel::showGcode()
{
    if (viewer_ && viewer_->hasProgram())
        viewer_->show();
    followGcode();
}

void SimToolModel::followGcode()
{
    if (!viewer_ || !result_ || gcodeState() != QLatin1String("current"))
        return;
    std::uint32_t op = position_.opIndex, move = position_.moveIndex;
    // Where the stock is on its way to (an event chosen, a line chosen) rather than a frame it passes on the way; while
    // playing, where it is.
    if (!playing_ && (pending_ || seekJob_.valid())) {
        op = requested_.opIndex;
        move = requested_.moveIndex;
    }
    if (!viewer_->isOpen())
        return; // closed by the user: an event chosen opens it (selectEvent), a frame does not
    if (const auto section = sectionOf(op)) {
        // A line of that move current already (one the user chose, or the move shown before): it stays.
        const PostedProgram* p = viewer_->program();
        if (p != nullptr && viewer_->currentLine() >= 0) {
            const auto now = p->index->moveOf(static_cast<std::size_t>(viewer_->currentLine()));
            if (now && now->section == *section && now->move == move)
                return;
        }
        viewer_->showMove(*section, move);
        gcodeLine_ = viewer_->currentLine();
    }
}

// A line made current in the viewer (GcodeModel::lineChosen): in the Simulate workspace, the end of its move.
void SimToolModel::seekToLine(int line)
{
    if (!inWorkspace() || !result_ || !viewer_ || line < 0 || gcodeState() != QLatin1String("current"))
        return;
    const PostedProgram* p = viewer_->program();
    const auto move = p != nullptr ? p->index->moveOf(static_cast<std::size_t>(line)) : std::nullopt;
    if (!move || move->section >= p->sectionOps.size())
        return; // a comment, the header, a tool change: no move
    for (std::size_t i = 0; i < result_->ops.size() && i < result_->timeline.ops.size(); ++i) {
        if (result_->ops[i] != p->sectionOps[move->section])
            continue;
        if (move->move >= result_->timeline.ops[i].moves())
            return;
        pause();
        if (currentEvent_ >= 0) {
            // No problem chosen any more: the viewer follows the stock, not the event.
            currentEvent_ = -1;
            applyMarks();
            emit currentEventChanged();
        }
        requestSeek(sim::TimelinePosition{static_cast<std::uint32_t>(i), move->move, 1.0});
        return;
    }
}

// ---------- visibility ----------
void SimToolModel::setShowStock(bool on)
{
    if (on == showStock_)
        return;
    showStock_ = on;
    applyVisibility();
    emit visibilityChanged();
}

void SimToolModel::setShowTool(bool on)
{
    if (on == showTool_)
        return;
    showTool_ = on;
    applyVisibility();
    emit visibilityChanged();
}

void SimToolModel::setShowHolder(bool on)
{
    if (on == showHolder_)
        return;
    showHolder_ = on;
    applyVisibility();
    emit visibilityChanged();
}

void SimToolModel::setShowFixtures(bool on)
{
    if (on == showFixtures_)
        return;
    showFixtures_ = on;
    applyVisibility();
    emit visibilityChanged();
}

void SimToolModel::setShowToolpath(bool on)
{
    if (on == showToolpath_)
        return;
    showToolpath_ = on;
    applyVisibility(false);
    emit visibilityChanged();
}

// ---------- what the panel shows ----------
QString SimToolModel::resolutionText() const
{
    if (!result_)
        return {};
    const QString cell = QString::number(result_->cellMm, 'f', 2);
    return tierName_.isEmpty() ? trs("Stock cells of %1 mm").arg(cell)
                               : trs("Stock cells of %1 mm (%2 tier)").arg(cell, tierName_);
}

QString SimToolModel::slowSeekText() const
{
    if (!result_)
        return {};
    return camui::slowSeekText(result_->seek.thinned, result_->seek.worstSeekMs, result_->cellMm);
}

int SimToolModel::errorCount() const
{
    if (!result_)
        return 0;
    return static_cast<int>(std::count_if(result_->events.begin(), result_->events.end(),
                                          [](const sim::SimEvent& e) { return e.severity == sim::Severity::Error; }));
}

int SimToolModel::warningCount() const
{
    if (!result_)
        return 0;
    return static_cast<int>(std::count_if(result_->events.begin(), result_->events.end(),
                                          [](const sim::SimEvent& e) { return e.severity == sim::Severity::Warning; }));
}

QString SimToolModel::summary() const
{
    if (!result_)
        return {};
    const int errors = errorCount(), warnings = warningCount();
    const int others = static_cast<int>(result_->events.size()) - errors - warnings;
    QStringList parts;
    if (errors > 0)
        parts << (errors == 1 ? trs("1 error") : trs("%1 errors").arg(errors));
    if (warnings > 0)
        parts << (warnings == 1 ? trs("1 warning") : trs("%1 warnings").arg(warnings));
    if (others > 0)
        parts << (others == 1 ? trs("1 note") : trs("%1 notes").arg(others));
    const QString found =
        parts.isEmpty() ? trs("No collision, gouge or other problem found") : parts.join(QStringLiteral(", "));
    return trs("%1. %2 moves, %3 of machine time (estimated).")
        .arg(found)
        .arg(result_->timeline.moveCount)
        .arg(camui::formatClock(result_->timeline.totalSec));
}

QVariantList SimToolModel::events() const
{
    QVariantList out;
    if (!result_)
        return out;
    const double total = result_->timeline.totalSec;
    for (std::size_t i = 0; i < result_->events.size(); ++i) {
        const sim::SimEvent& e = result_->events[i];
        const cs::EventPlace& pl = result_->places[i];
        const int kind = static_cast<int>(e.kind);
        const bool setup = e.moveIndex == sim::kSetupEvent;
        const QString operation =
            !setup && pl.opIndex < result_->opNames.size() ? qs(result_->opNames[pl.opIndex]) : QString();
        const int move = setup ? 0 : static_cast<int>(pl.moveIndex) + 1;
        out.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                                  {QStringLiteral("kind"), kind},
                                  {QStringLiteral("kindName"), QString::fromLatin1(camui::eventKindName(kind))},
                                  {QStringLiteral("severity"), camui::severityKey(severityOf(e.severity))},
                                  {QStringLiteral("title"), camui::eventTitle(kind)},
                                  {QStringLiteral("message"), qs(e.message)},
                                  {QStringLiteral("operation"), operation},
                                  {QStringLiteral("opIndex"), static_cast<int>(pl.opIndex)},
                                  {QStringLiteral("move"), move},
                                  {QStringLiteral("setup"), setup},
                                  {QStringLiteral("where"),
                                   setup ? trs("Before the first move") : trs("%1 · move %2").arg(operation).arg(move)},
                                  {QStringLiteral("sec"), pl.sec},
                                  {QStringLiteral("time"), camui::formatClock(pl.sec)},
                                  {QStringLiteral("x"), total > 0 ? pl.sec / total : 0.0}});
    }
    return out;
}

QVariantList SimToolModel::operations() const
{
    QVariantList out;
    if (!result_)
        return out;
    const double total = result_->timeline.totalSec;
    for (const cs::TimelineOperation& op : result_->timeline.ops) {
        out.push_back(QVariantMap{{QStringLiteral("name"), qs(op.name)},
                                  {QStringLiteral("start"), total > 0 ? op.startSec / total : 0.0},
                                  {QStringLiteral("end"), total > 0 ? op.endSec / total : 0.0},
                                  {QStringLiteral("startSec"), op.startSec},
                                  {QStringLiteral("endSec"), op.endSec},
                                  {QStringLiteral("moves"), static_cast<qulonglong>(op.moves())}});
    }
    return out;
}

QVariantList SimToolModel::markers(double widthPx) const
{
    QVariantList out;
    if (!result_ || !(widthPx > 0))
        return out;
    const double total = result_->timeline.totalSec;
    std::vector<camui::MarkerInput> in;
    for (std::size_t i = 0; i < result_->events.size(); ++i) {
        const double x = total > 0 ? result_->places[i].sec / total * widthPx : 0.0;
        in.push_back({x, severityOf(result_->events[i].severity), static_cast<int>(i)});
    }
    std::stable_sort(in.begin(), in.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    for (const camui::MarkerCluster& c : camui::clusterMarkers(in)) {
        QVariantList events;
        for (const int e : c.events)
            events.push_back(e);
        out.push_back(QVariantMap{{QStringLiteral("x"), c.x},
                                  {QStringLiteral("severity"), camui::severityKey(c.severity)},
                                  {QStringLiteral("count"), static_cast<int>(c.events.size())},
                                  {QStringLiteral("first"), c.events.front()},
                                  {QStringLiteral("events"), events}});
    }
    return out;
}

QString SimToolModel::positionText() const
{
    if (!result_ || result_->timeline.ops.empty())
        return {};
    const sim::TimelinePosition p = result_->timeline.normalised(position_);
    const cs::TimelineOperation& op = result_->timeline.ops[p.opIndex];
    if (op.moves() == 0)
        return qs(op.name);
    return trs("%1 · move %2 of %3").arg(qs(op.name)).arg(p.moveIndex + 1).arg(op.moves());
}

QString SimToolModel::timeText() const
{
    if (!result_)
        return {};
    return trs("%1 / %2").arg(camui::formatClock(positionSec_), camui::formatClock(result_->timeline.totalSec));
}

bool SimToolModel::busy() const
{
    return running() || seekJob_.valid() || pending_.has_value() || checking_ || checkTimer_.isActive();
}

// ---------- commands ----------
QString SimToolModel::commandReason(const QString& id) const
{
    api::IApp* a = app();
    if (a == nullptr)
        return trs("There is no session");
    api::OpenDocument* od = a->active();
    if (od == nullptr || !od->document)
        return trs("Open or import a part first");
    if (id == QLatin1String("cam.sim.open"))
        return {};
    if (id == QLatin1String("cam.sim.run"))
        return runBlockedReason();
    if (id.startsWith(QLatin1String("cam.sim.toggle")))
        return {};
    if (!result_)
        return trs("Run the simulation first");
    return {};
}

bool SimToolModel::runCommand(const QString& id)
{
    if (const QString why = commandReason(id); !why.isEmpty()) {
        if (context_ && id == QLatin1String("cam.sim.run"))
            context_->showMessage(why, true);
        return false;
    }
    if (id == QLatin1String("cam.sim.open")) {
        open();
        return true;
    }
    if (id == QLatin1String("cam.sim.run"))
        return run();
    if (id == QLatin1String("cam.sim.play"))
        togglePlay();
    else if (id == QLatin1String("cam.sim.stepForward"))
        stepForward();
    else if (id == QLatin1String("cam.sim.stepBack"))
        stepBack();
    else if (id == QLatin1String("cam.sim.start"))
        toStart();
    else if (id == QLatin1String("cam.sim.end"))
        toEnd();
    else if (id == QLatin1String("cam.sim.nextEvent"))
        nextEvent();
    else if (id == QLatin1String("cam.sim.previousEvent"))
        previousEvent();
    else if (id == QLatin1String("cam.sim.faster"))
        faster();
    else if (id == QLatin1String("cam.sim.slower"))
        slower();
    else if (id == QLatin1String("cam.sim.toggleStock"))
        setShowStock(!showStock_);
    else if (id == QLatin1String("cam.sim.toggleTool"))
        setShowTool(!showTool_);
    else if (id == QLatin1String("cam.sim.toggleHolder"))
        setShowHolder(!showHolder_);
    else if (id == QLatin1String("cam.sim.toggleFixtures"))
        setShowFixtures(!showFixtures_);
    else if (id == QLatin1String("cam.sim.toggleToolpath"))
        setShowToolpath(!showToolpath_);
    else
        return false;
    return true;
}

} // namespace ray::ui
