#include "setup_tree.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../session.h"
#include "../ui_commands.h"
#include "cycletime_logic.h"
#include "setup_logic.h"
#include "setup_tool.h"

#include <ray_api/cam_setup.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <utility>

namespace ray::ui {

using namespace modelui;

namespace {
// T-044: the CAM tree's commands, on its current row. Their keys work in the CAM workspace only.
// clang-format off
constexpr UiCommand kCamTreeCommands[] = {
    {"cam.tree.edit", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Edit Setup or Operation"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Opens the setup or operation selected in the CAM tree in its panel"), "", CommandContext::Cam, "cam.setup.edit cam.operation.edit"},
    {"cam.tree.rename", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Rename"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Renames the setup or operation selected in the CAM tree"), "", CommandContext::Cam, ""},
    {"cam.tree.delete", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Delete Setup or Operation"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Deletes the setup (with its operations) or the operation selected in the CAM tree"), "", CommandContext::Cam, "cam.setup.remove cam.operation.remove"},
    {"cam.tree.moveUp", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Move Up"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Moves the setup or operation selected in the CAM tree one place earlier"), "", CommandContext::Cam, "cam.setup.reorder cam.operation.reorder"},
    {"cam.tree.moveDown", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Move Down"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Moves the setup or operation selected in the CAM tree one place later"), "", CommandContext::Cam, ""},
    {"cam.tree.suppress", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Suppress or Unsuppress Operation"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Switches the operation selected in the CAM tree off or on again"), "", CommandContext::Cam, "cam.operation.suppress"},
    {"cam.tree.generate", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Generate Toolpaths"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Generates the toolpath of the selected operation, or of every stale operation of the selected setup"), "G", CommandContext::Cam, ""},
};
// clang-format on

QString trt(const char* text)
{
    return QCoreApplication::translate("ray::ui::SetupTreeModel", text);
}

QString workOffsetText(int n)
{
    if (n >= 1 && n <= 6)
        return QStringLiteral("G%1").arg(53 + n);
    return QStringLiteral("G54.1 P%1").arg(n - 6);
}

} // namespace

std::span<const UiCommand> camTreeUiCommands()
{
    return kCamTreeCommands;
}

SetupTreeModel::SetupTreeModel(SceneController* scene, Session* session, AppContext* context, QObject* parent)
    : QAbstractListModel(parent), scene_(scene), session_(session), context_(context)
{
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(0);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { refresh(); });
    // T-045: the engine's notices (a generation's progress comes at up to 30 Hz) make one status run per kNoticeMs.
    constexpr int kNoticeMs = 100;
    noticeTimer_.setSingleShot(true);
    noticeTimer_.setInterval(kNoticeMs);
    connect(&noticeTimer_, &QTimer::timeout, this, [this] { refresh(); });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            if (session_->documentId().toStdString() != docId_) {
                subscribe();
                // A document opened with setups shows its first one (its toolpaths and WCS in the view).
                selectFirst_ = true;
                sync();
            }
        });
    }
}

SetupTreeModel::~SetupTreeModel()
{
    if (commands_)
        commands_->setUiHandler(QStringLiteral("cam.tree."), {});
    statusJob_.cancel();
    setupJob_.cancel();
    cycleTimeJob_.cancel();
    unsubscribe();
    if (engineListener_ != 0 && app() != nullptr && app()->cam() == engine_)
        engine_->unsubscribe(engineListener_);
}

void SetupTreeModel::subscribeEngine()
{
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (engine == engine_)
        return;
    engine_ = engine;
    engineListener_ = 0;
    if (engine_ == nullptr)
        return;
    QPointer<SetupTreeModel> self(this);
    engineListener_ = engine_->subscribe([self](const OperationId&) {
        if (!self)
            return;
        ++self->notices_;
        if (!self->noticeTimer_.isActive())
            self->noticeTimer_.start();
    });
}

SetupTreeModel* SetupTreeModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    SetupToolModel* tool = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        tool = qml->singletonInstance<SetupToolModel*>(uri, QStringLiteral("CamSetup"));
    }
    auto* model = new SetupTreeModel(scene, session, context);
    model->setCommands(commands);
    model->setSetupTool(tool);
    model->subscribe();
    model->selectFirst_ = true;
    model->sync();
    return model; // owned by the engine
}

void SetupTreeModel::setSetupTool(SetupToolModel* tool)
{
    if (tool_) {
        disconnect(this, nullptr, tool_, nullptr);
        disconnect(tool_, nullptr, this, nullptr);
    }
    tool_ = tool;
    if (!tool_)
        return;
    const auto show = [this] {
        if (tool_)
            tool_->showWcsOf(currentSetup());
    };
    connect(this, &SetupTreeModel::currentChanged, tool_, show);
    connect(this, &SetupTreeModel::changed, tool_, show);
    // A setup just added or changed by the panel becomes the current row (its WCS and toolpaths in the view).
    connect(tool_, &SetupToolModel::committed, this, [this](const QString& id) { selectWhenListed(id); });
    show();
}

void SetupTreeModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<SetupTreeModel> self(this);
    commands_->setUiHandler(
        QStringLiteral("cam.tree."),
        CommandCatalog::UiHandler{
            [self](const QString& id) { return self ? self->commandReason(id) : trt("The CAM tree is not available"); },
            [self](const QString& id) { return self && self->runCommand(id); }});
}

// ---------- document ----------
api::IApp* SetupTreeModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* SetupTreeModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void SetupTreeModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<SetupTreeModel> self(this);
    // Every change of the state (an edit in either workspace, undo, redo) and every published build: the rows at once,
    // the statuses on a worker.
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->sync();
    });
}

void SetupTreeModel::unsubscribe()
{
    if (doc::Document* d = liveDoc(); d != nullptr && docListener_ != 0)
        d->unsubscribe(docListener_);
    doc_ = nullptr;
    docListener_ = 0;
    docId_.clear();
    ++docGeneration_;
    ++statusSerial_;
    statusJob_.cancel();
    statusJob_ = {};
    setupJob_.cancel();
    setupJob_ = {};
    if (carried_)
        carried_->cancel(); // T-074
    carriedJob_.cancel();
    carriedJob_ = {};
    statuses_ = {};
    noticeTimer_.stop();
    generating_.clear();
    progress_.clear();
    ++cycleTimeSerial_;
    cycleTimeJob_.cancel();
    cycleTimeJob_ = {};
    if (!cycleTimes_.empty()) {
        cycleTimes_.clear();
        emit cycleTimesChanged();
    }
}

void SetupTreeModel::sync()
{
    if (doc_ == nullptr && app() != nullptr && app()->active() != nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    std::vector<Row> rows;
    int setups = 0;
    if (state && state->cam) {
        const cam::CamState& cam = *state->cam;
        for (std::size_t i = 0; i < cam.setups.size(); ++i) {
            const cam::Setup& s = cam.setups[i];
            Row r;
            r.setup = true;
            r.setupId = s.id;
            r.name = qs(s.name);
            QString machine;
            if (const auto m = cam.machines.find(s.machine); m != cam.machines.end())
                machine = qs(m->second.item.name);
            r.detail = machine.isEmpty() ? workOffsetText(s.wcs.workOffset)
                                         : trt("%1 on %2").arg(workOffsetText(s.wcs.workOffset), machine);
            r.operations = static_cast<int>(s.operations.size());
            r.index = static_cast<int>(i);
            r.siblings = static_cast<int>(cam.setups.size());
            rows.push_back(r);
            ++setups;
            if (collapsed_.count(s.id) != 0)
                continue;
            for (std::size_t j = 0; j < s.operations.size(); ++j) {
                const auto op = cam.operations.find(s.operations[j]);
                if (op == cam.operations.end())
                    continue;
                Row o;
                o.setup = false;
                o.setupId = s.id;
                o.opId = op->first;
                o.name = qs(op->second.name);
                o.type = qs(op->second.type);
                o.detail = o.type;
                o.suppressed = op->second.suppressed;
                o.templateId = op->second.templateId;
                o.index = static_cast<int>(j);
                o.siblings = static_cast<int>(s.operations.size());
                rows.push_back(o);
            }
        }
    }
    const bool same = rows.size() == rows_.size() &&
                      std::equal(rows.begin(), rows.end(), rows_.begin(), [](const Row& a, const Row& b) {
                          return a.setup == b.setup && a.setupId == b.setupId && a.opId == b.opId;
                      });
    if (same) {
        rows_ = std::move(rows);
        if (!rows_.empty())
            emit dataChanged(index(0), index(count() - 1));
    } else {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }
    setupCount_ = setups;
    ++revision_;
    if (current_ >= count()) {
        current_ = -1;
        emit currentChanged();
    }
    if (!pendingSelect_.isEmpty()) {
        if (const int row = rowOf(pendingSelect_); row >= 0) {
            pendingSelect_.clear();
            setCurrentIndex(row);
        }
    }
    if (selectFirst_ && state) {
        selectFirst_ = false;
        if (current_ < 0 && setups > 0)
            setCurrentIndex(0);
    }
    emit changed();
    scheduleRefresh();
}

// ---------- statuses ----------
void SetupTreeModel::scheduleRefresh()
{
    refreshTimer_.start();
}

void SetupTreeModel::refresh()
{
    api::IApp* a = app();
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    cam::ICamEngine* engine = a ? a->cam() : nullptr;
    if (d == nullptr || jobs == nullptr || engine == nullptr)
        return;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    if (!state || !state->cam) {
        statuses_ = {};
        return;
    }
    subscribeEngine();
    // The runs before are superseded. The operations' statuses are the engine's own job (statusAsync: it holds
    // what it needs, not the engine); the setups' resolution is a job here (it reads the snapshot only).
    statusJob_.cancel();
    setupJob_.cancel();
    noticeTimer_.stop();
    const std::uint64_t serial = ++statusSerial_;
    statusJob_ = engine->statusAsync(state, build);
    setupJob_ = jobs->submit<std::shared_ptr<const std::map<SetupId, Status>>>(
        JobInfo{"CAM setup status", JobPriority::Interactive, false},
        [state, build](JobContext& ctx) -> Result<std::shared_ptr<const std::map<SetupId, Status>>> {
            if (auto ok = ctx.cancel.check(); !ok)
                return tl::unexpected(ok.error());
            auto out = std::make_shared<std::map<SetupId, Status>>(api::cam_setup::setupStates(state, build, ctx));
            if (auto ok = ctx.cancel.check(); !ok)
                return tl::unexpected(ok.error());
            return std::shared_ptr<const std::map<SetupId, Status>>(std::move(out));
        });
    QPointer<SetupTreeModel> self(this);
    const auto shown = [self] {
        if (!self->rows_.empty())
            emit self->dataChanged(self->index(0), self->index(self->count() - 1));
        ++self->revision_;
        emit self->changed();
    };
    statusJob_.then([self, serial, shown, state, build](Result<std::map<OperationId, cam::OpState>> r) {
        if (!self || serial != self->statusSerial_)
            return;
        self->statusJob_ = {};
        if (!r)
            return;
        self->statuses_.ops = std::move(*r);
        ++self->statusRuns_;
        shown();
        self->estimateCycleTimes();                 // T-059
        self->refreshCarried(state, build, serial); // T-074
    });
    setupJob_.then([self, serial, shown](Result<std::shared_ptr<const std::map<SetupId, Status>>> r) {
        if (!self || serial != self->statusSerial_)
            return;
        self->setupJob_ = {};
        if (!r)
            return;
        self->statuses_.setups = **r;
        shown();
    });
    emit changed();
}

// T-074: the setups whose stock comes from an earlier setup, from that stock, once the earlier setups are up to date.
void SetupTreeModel::refreshCarried(const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                                    std::uint64_t serial)
{
    api::IApp* a = app();
    cam::ICamEngine* engine = a ? a->cam() : nullptr;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (!state || !build || engine == nullptr || jobs == nullptr)
        return;
    std::vector<SetupId> ready;
    for (const SetupId& id : api::cam_setup::carriedSetups(*state)) {
        if (camui::chainReady(*state, id, statuses_.ops))
            ready.push_back(id);
    }
    if (!carried_)
        carried_ = std::make_unique<camui::CarriedStockRequest>();
    carriedJob_.cancel();
    if (ready.empty()) {
        carried_->cancel();
        return;
    }
    QPointer<SetupTreeModel> self(this);
    carried_->start(
        *engine, state, build, ready,
        [self, serial, state, build](api::cam_setup::CarriedStocks stocks, std::map<SetupId, Error> errors) {
            if (!self || serial != self->statusSerial_)
                return;
            IJobSystem* jobs = self->scene_ ? self->scene_->jobs() : nullptr;
            if (jobs == nullptr)
                return;
            auto shared = std::make_shared<const api::cam_setup::CarriedStocks>(std::move(stocks));
            self->carriedJob_ = jobs->submit<std::shared_ptr<const std::map<SetupId, Status>>>(
                JobInfo{"CAM setup status", JobPriority::Interactive, false},
                [state, build, shared](JobContext& ctx) -> Result<std::shared_ptr<const std::map<SetupId, Status>>> {
                    return std::make_shared<const std::map<SetupId, Status>>(
                        api::cam_setup::setupStates(state, build, ctx, shared.get()));
                });
            self->carriedJob_.then(
                [self, serial, shared, errors](Result<std::shared_ptr<const std::map<SetupId, Status>>> r) {
                    if (!self || serial != self->statusSerial_ || !r)
                        return;
                    self->carriedJob_ = {};
                    for (const auto& [id, stock] : *shared) {
                        if (const auto it = (*r)->find(id); it != (*r)->end())
                            self->statuses_.setups[id] = it->second;
                    }
                    for (const auto& [id, error] : errors) {
                        if (error.code != ErrorCode::Cancelled)
                            self->statuses_.setups[id] = Status(tl::unexpected(error));
                    }
                    if (!self->rows_.empty())
                        emit self->dataChanged(self->index(0), self->index(self->count() - 1));
                    ++self->revision_;
                    emit self->changed();
                });
        });
}

// ---------- cycle time (T-059) ----------
void SetupTreeModel::estimateCycleTimes()
{
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    std::vector<api::cam_cycletime::SetupInput> inputs;
    if (state && state->cam && jobs != nullptr)
        inputs = api::cam_cycletime::setupInputs(*state->cam, statuses_.ops);
    // Estimates whose inputs did not change are kept; the others are worked out again on a worker (one job for all).
    std::map<SetupId, std::shared_ptr<const CycleTimeEstimate>> kept;
    std::vector<api::cam_cycletime::SetupInput> todo;
    for (api::cam_cycletime::SetupInput& in : inputs) {
        const auto it = cycleTimes_.find(in.setup);
        if (it != cycleTimes_.end() && it->second->input.same(in))
            kept.emplace(in.setup, it->second);
        else
            todo.push_back(std::move(in));
    }
    const bool dropped = kept.size() != cycleTimes_.size();
    cycleTimes_ = std::move(kept);
    ++cycleTimeSerial_;
    cycleTimeJob_.cancel();
    cycleTimeJob_ = {};
    if (dropped)
        showCycleTimes();
    if (todo.empty())
        return;
    const std::uint64_t serial = cycleTimeSerial_;
    cycleTimeJob_ = jobs->submit<std::shared_ptr<const std::vector<CycleTimeEstimate>>>(
        JobInfo{"CAM cycle time", JobPriority::Background, false},
        [todo = std::move(todo)](JobContext& ctx) -> Result<std::shared_ptr<const std::vector<CycleTimeEstimate>>> {
            auto out = std::make_shared<std::vector<CycleTimeEstimate>>();
            out->reserve(todo.size());
            for (const api::cam_cycletime::SetupInput& in : todo) {
                if (auto ok = ctx.cancel.check(); !ok)
                    return tl::unexpected(ok.error());
                out->push_back({in, api::cam_cycletime::estimate(in)});
            }
            return std::shared_ptr<const std::vector<CycleTimeEstimate>>(std::move(out));
        });
    QPointer<SetupTreeModel> self(this);
    cycleTimeJob_.then([self, serial](Result<std::shared_ptr<const std::vector<CycleTimeEstimate>>> r) {
        if (!self || serial != self->cycleTimeSerial_)
            return;
        self->cycleTimeJob_ = {};
        if (!r)
            return;
        for (const CycleTimeEstimate& e : **r)
            self->cycleTimes_[e.input.setup] = std::make_shared<const CycleTimeEstimate>(e);
        ++self->cycleTimeRuns_;
        self->showCycleTimes();
    });
}

void SetupTreeModel::showCycleTimes()
{
    if (!rows_.empty())
        emit dataChanged(index(0), index(count() - 1), {CycleTimeRole, CycleTimeTipRole});
    emit cycleTimesChanged();
}

const SetupTreeModel::CycleTimeEstimate* SetupTreeModel::cycleTimeOf(const SetupId& id) const
{
    const auto it = cycleTimes_.find(id);
    return it == cycleTimes_.end() ? nullptr : it->second.get();
}

const cam::CycleTimePart* SetupTreeModel::cycleTimeOf(const OperationId& id) const
{
    for (const auto& [setup, e] : cycleTimes_) {
        const std::vector<OperationId>& ops = e->input.operations;
        const auto it = std::find(ops.begin(), ops.end(), id);
        if (it != ops.end()) {
            const auto k = static_cast<std::size_t>(it - ops.begin());
            return k < e->time.operations.size() ? &e->time.operations[k] : nullptr;
        }
    }
    return nullptr;
}

const cam::OpState* SetupTreeModel::opState(const OperationId& id) const
{
    const auto it = statuses_.ops.find(id);
    return it == statuses_.ops.end() ? nullptr : &it->second;
}

std::optional<SetupId> SetupTreeModel::currentSetup() const
{
    const Row* r = rowAt(current_);
    return r ? std::optional<SetupId>(r->setupId) : std::nullopt;
}

std::optional<OperationId> SetupTreeModel::currentOperation() const
{
    const Row* r = rowAt(current_);
    return r && !r->setup ? std::optional<OperationId>(r->opId) : std::nullopt;
}

void SetupTreeModel::selectWhenListed(const QString& id)
{
    const int row = rowOf(id);
    if (row >= 0) {
        pendingSelect_.clear();
        setCurrentIndex(row);
        return;
    }
    pendingSelect_ = id;
}

std::optional<cam::OpStatus> SetupTreeModel::statusOf(const OperationId& id) const
{
    const auto it = statuses_.ops.find(id);
    return it == statuses_.ops.end() ? std::nullopt : std::optional<cam::OpStatus>(it->second.status);
}

// ---------- rows ----------
int SetupTreeModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

const SetupTreeModel::Row* SetupTreeModel::rowAt(int row) const
{
    return row >= 0 && row < count() ? &rows_[static_cast<std::size_t>(row)] : nullptr;
}

QVariant SetupTreeModel::data(const QModelIndex& index, int role) const
{
    const Row* r = index.isValid() ? rowAt(index.row()) : nullptr;
    if (r == nullptr)
        return {};
    // The badge.
    std::optional<camui::Badge> badge;
    double progress = -1;
    if (r->setup) {
        std::vector<const cam::OpState*> ops;
        doc::Document* d = liveDoc();
        const doc::DocStatePtr state = d ? d->state() : nullptr;
        if (state && state->cam) {
            for (const cam::Setup& s : state->cam->setups) {
                if (s.id != r->setupId)
                    continue;
                for (const OperationId& op : s.operations) {
                    const auto it = statuses_.ops.find(op);
                    if (it != statuses_.ops.end())
                        ops.push_back(&it->second);
                }
            }
        }
        const auto own = statuses_.setups.find(r->setupId);
        badge = camui::setupBadge(own == statuses_.setups.end() ? std::optional<Status>() : own->second, ops);
    } else if (const auto it = statuses_.ops.find(r->opId); it != statuses_.ops.end()) {
        badge = camui::badgeOf(it->second);
        if (it->second.status == cam::OpStatus::Generating) {
            // A generation the tree started reports through its job; any other's progress is in the status (T-045).
            const auto p = progress_.find(r->opId);
            progress = p != progress_.end() ? std::max(p->second, it->second.progress) : it->second.progress;
        }
    }
    switch (role) {
    case KindRole:
        return r->setup ? QStringLiteral("setup") : QStringLiteral("operation");
    case IdRole:
        return r->setup ? idText(r->setupId.value) : idText(r->opId.value);
    case NameRole:
        return r->name;
    case TypeRole:
        return r->type;
    case DetailRole:
        return r->detail;
    case StatusRole:
        return badge ? camui::statusKey(*badge) : QString();
    case StatusTextRole:
        return badge ? camui::statusText(*badge, progress) : QString();
    case MessageRole:
        return badge && (badge->status == cam::OpStatus::Failed || badge->status == cam::OpStatus::ReferenceBroken)
                   ? badge->message
                   : QString();
    case ProgressRole:
        return progress;
    case SuppressedRole:
        return r->suppressed;
    case DepthRole:
        return r->setup ? 0 : 1;
    case ExpandedRole:
        return r->setup && collapsed_.count(r->setupId) == 0;
    case OperationCountRole:
        return r->operations;
    case CanMoveUpRole:
        return r->index > 0;
    case CanMoveDownRole:
        return r->index + 1 < r->siblings;
    case TemplateRole: {
        // T-057: the template's name now (a renamed template shows its new name).
        if (r->templateId.empty())
            return QString();
        lib::LibraryStack* stack = app() != nullptr ? app()->libraries() : nullptr;
        const lib::OpTemplate* t = stack != nullptr ? stack->opTemplate(Uuid::fromString(r->templateId)) : nullptr;
        return t != nullptr ? qs(t->name) : trt("a template not in the libraries");
    }
    case CycleTimeRole: {
        const CycleTimeEstimate* e = r->setup ? cycleTimeOf(r->setupId) : nullptr;
        return e != nullptr && e->input.complete && !e->input.operations.empty()
                   ? camui::cycleTimeShort(e->time.totalSec)
                   : QString();
    }
    case CycleTimeTipRole: {
        if (r->setup) {
            const CycleTimeEstimate* e = cycleTimeOf(r->setupId);
            return e != nullptr && !e->input.operations.empty()
                       ? camui::cycleTimeDetails(e->time, e->time.assumptions, e->input.complete)
                       : QString();
        }
        const cam::CycleTimePart* p = cycleTimeOf(r->opId);
        const CycleTimeEstimate* e = p != nullptr ? cycleTimeOf(r->setupId) : nullptr;
        return e != nullptr ? camui::cycleTimeDetails(*p, e->time.assumptions, true) : QString();
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> SetupTreeModel::roleNames() const
{
    return {{KindRole, "rowKind"},
            {IdRole, "rowId"},
            {NameRole, "rowName"},
            {TypeRole, "rowType"},
            {DetailRole, "rowDetail"},
            {StatusRole, "status"},
            {StatusTextRole, "statusText"},
            {MessageRole, "message"},
            {ProgressRole, "progress"},
            {SuppressedRole, "suppressed"},
            {DepthRole, "rowDepth"},
            {ExpandedRole, "expanded"},
            {OperationCountRole, "operationCount"},
            {CanMoveUpRole, "canMoveUp"},
            {CanMoveDownRole, "canMoveDown"},
            {TemplateRole, "rowTemplate"},
            {CycleTimeRole, "cycleTime"},
            {CycleTimeTipRole, "cycleTimeTip"}};
}

QVariantMap SetupTreeModel::get(int row) const
{
    QVariantMap out;
    if (rowAt(row) == nullptr)
        return out;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it)
        out.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    return out;
}

int SetupTreeModel::rowOf(const QString& id) const
{
    for (int i = 0; i < count(); ++i) {
        const Row& r = rows_[static_cast<std::size_t>(i)];
        if ((r.setup ? idText(r.setupId.value) : idText(r.opId.value)) == id)
            return i;
    }
    return -1;
}

void SetupTreeModel::setCurrentIndex(int index)
{
    if (index < -1 || index >= count())
        index = -1;
    if (index == current_)
        return;
    current_ = index;
    emit currentChanged();
}

void SetupTreeModel::toggleExpanded(int row)
{
    const Row* r = rowAt(row);
    if (r == nullptr || !r->setup)
        return;
    const SetupId id = r->setupId;
    if (!collapsed_.erase(id))
        collapsed_.insert(id);
    sync();
    setCurrentIndex(rowOf(idText(id.value)));
}

void SetupTreeModel::setMessage(const QString& message)
{
    if (message == message_)
        return;
    message_ = message;
    emit messageChanged();
}

void SetupTreeModel::dismissMessage()
{
    setMessage(QString());
}

// ---------- actions ----------
bool SetupTreeModel::run(const char* actionId, const std::string& argsJson, const QString& label)
{
    api::IApp* a = app();
    api::OpenDocument* od = a ? a->findDocument(docId_) : nullptr;
    if (a == nullptr || od == nullptr)
        return false;
    if (od->readOnly) {
        setMessage(trt("The document is read-only"));
        return false;
    }
    // As the user, like every tree action of the model workspace: no confirmation is asked for the user's own click.
    JobHandle<api::ActionResult> job = a->actions().invoke(actionId, argsJson, api::Caller::User);
    if (!job.valid())
        return false;
    QPointer<SetupTreeModel> self(this);
    job.then([self, label](Result<api::ActionResult> r) {
        if (self && !r)
            self->setMessage(trt("%1 failed: %2").arg(label, qs(r.error().message)));
    });
    setMessage(QString());
    return true;
}

bool SetupTreeModel::rename(int row, const QString& name)
{
    const Row* r = rowAt(row);
    const QString trimmed = name.trimmed();
    if (r == nullptr || trimmed.isEmpty() || trimmed == r->name)
        return false;
    const QJsonObject args{{QStringLiteral("id"), r->setup ? idText(r->setupId.value) : idText(r->opId.value)},
                           {QStringLiteral("name"), trimmed}};
    return run(r->setup ? "cam.setup.edit" : "cam.operation.edit", jsonText(args), trt("Rename"));
}

bool SetupTreeModel::remove(int row)
{
    const Row* r = rowAt(row);
    if (r == nullptr)
        return false;
    if (r->setup && tool_ && tool_->active() && tool_->setupId() == r->setupId) {
        setMessage(trt("Close the setup panel first"));
        return false;
    }
    const QJsonObject args{{QStringLiteral("id"), r->setup ? idText(r->setupId.value) : idText(r->opId.value)}};
    return run(r->setup ? "cam.setup.remove" : "cam.operation.remove", jsonText(args), trt("Delete"));
}

bool SetupTreeModel::moveUp(int row)
{
    const Row* r = rowAt(row);
    if (r == nullptr || r->index == 0)
        return false;
    const QJsonObject args{{QStringLiteral("id"), r->setup ? idText(r->setupId.value) : idText(r->opId.value)},
                           {QStringLiteral("index"), r->index - 1}};
    return run(r->setup ? "cam.setup.reorder" : "cam.operation.reorder", jsonText(args), trt("Move"));
}

bool SetupTreeModel::moveDown(int row)
{
    const Row* r = rowAt(row);
    if (r == nullptr || r->index + 1 >= r->siblings)
        return false;
    const QJsonObject args{{QStringLiteral("id"), r->setup ? idText(r->setupId.value) : idText(r->opId.value)},
                           {QStringLiteral("index"), r->index + 1}};
    return run(r->setup ? "cam.setup.reorder" : "cam.operation.reorder", jsonText(args), trt("Move"));
}

bool SetupTreeModel::setSuppressed(int row, bool suppressed)
{
    const Row* r = rowAt(row);
    if (r == nullptr || r->setup || r->suppressed == suppressed)
        return false;
    const QJsonObject args{{QStringLiteral("id"), idText(r->opId.value)}, {QStringLiteral("suppressed"), suppressed}};
    return run("cam.operation.suppress", jsonText(args), suppressed ? trt("Suppress") : trt("Unsuppress"));
}

bool SetupTreeModel::edit(int row)
{
    const Row* r = rowAt(row);
    if (r == nullptr)
        return false;
    if (!r->setup) {
        emit operationEditRequested(idText(r->opId.value)); // the operation panel (T-045)
        return true;
    }
    return tool_ && tool_->edit(idText(r->setupId.value));
}

bool SetupTreeModel::generate(int row)
{
    const Row* r = rowAt(row);
    api::IApp* a = app();
    doc::Document* d = liveDoc();
    cam::ICamEngine* engine = a ? a->cam() : nullptr;
    if (r == nullptr || d == nullptr || engine == nullptr)
        return false;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    if (!build) {
        setMessage(trt("The model is not built yet"));
        return false;
    }
    std::vector<OperationId> ops;
    if (r->setup) {
        if (state && state->cam) {
            for (const cam::Setup& s : state->cam->setups) {
                if (s.id != r->setupId)
                    continue;
                for (const OperationId& op : s.operations) {
                    const auto st = statuses_.ops.find(op);
                    if (st == statuses_.ops.end() || st->second.status == cam::OpStatus::Stale)
                        ops.push_back(op);
                }
            }
        }
    } else {
        ops.push_back(r->opId);
    }
    if (ops.empty()) {
        setMessage(trt("Every toolpath of the setup is up to date"));
        return false;
    }
    QPointer<SetupTreeModel> self(this);
    for (const OperationId& op : ops) {
        JobHandle<std::shared_ptr<const cam::Toolpath>> job = engine->generate(state, build, op);
        generating_[op] = job;
        progress_[op] = 0;
        job.onProgress([self, op](double fraction, std::string) {
            if (!self || self->generating_.count(op) == 0)
                return;
            self->progress_[op] = fraction;
            const int row = self->rowOf(idText(op.value));
            if (row >= 0)
                emit self->dataChanged(self->index(row), self->index(row), {StatusTextRole, ProgressRole});
        });
        job.then([self, op](Result<std::shared_ptr<const cam::Toolpath>>) {
            if (!self)
                return;
            self->generating_.erase(op);
            self->progress_.erase(op);
            self->scheduleRefresh(); // the engine filed the result before this runs
        });
    }
    setMessage(QString());
    scheduleRefresh(); // shows Generating
    return true;
}

QString SetupTreeModel::commandReason(const QString& id) const
{
    api::IApp* a = app();
    if (a == nullptr)
        return trt("There is no session");
    if (liveDoc() == nullptr)
        return trt("Open or import a part first");
    const Row* r = rowAt(current_);
    if (r == nullptr)
        return trt("Select a setup or an operation in the CAM tree first");
    if (id == QLatin1String("cam.tree.suppress") && r->setup)
        return trt("Select an operation: setups are not suppressed");
    if (id == QLatin1String("cam.tree.moveUp") && r->index == 0)
        return trt("It is the first one already");
    if (id == QLatin1String("cam.tree.moveDown") && r->index + 1 >= r->siblings)
        return trt("It is the last one already");
    return {};
}

bool SetupTreeModel::runCommand(const QString& id)
{
    const int row = current_;
    const Row* r = rowAt(row);
    if (r == nullptr)
        return false;
    if (id == QLatin1String("cam.tree.edit"))
        return edit(row);
    if (id == QLatin1String("cam.tree.rename")) {
        emit renameRequested(row);
        return true;
    }
    if (id == QLatin1String("cam.tree.delete"))
        return remove(row);
    if (id == QLatin1String("cam.tree.moveUp"))
        return moveUp(row);
    if (id == QLatin1String("cam.tree.moveDown"))
        return moveDown(row);
    if (id == QLatin1String("cam.tree.suppress"))
        return setSuppressed(row, !r->suppressed);
    if (id == QLatin1String("cam.tree.generate"))
        return generate(row);
    return false;
}

} // namespace ray::ui
