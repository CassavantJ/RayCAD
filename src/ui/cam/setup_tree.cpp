#include "setup_tree.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../session.h"
#include "../ui_commands.h"
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
    {"cam.tree.edit", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Edit Setup"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Opens the setup selected in the CAM tree in the setup panel"), "", CommandContext::Cam, "cam.setup.edit"},
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
    generatingPoll_.setSingleShot(true);
    generatingPoll_.setInterval(250);
    connect(&generatingPoll_, &QTimer::timeout, this, [this] { refresh(); });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            if (session_->documentId().toStdString() != docId_) {
                subscribe();
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
    unsubscribe();
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
    model->sync();
    return model; // owned by the engine
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
    statuses_ = {};
    generatingPoll_.stop();
    generating_.clear();
    progress_.clear();
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
    // The run before is superseded.
    statusJob_.cancel();
    const std::uint64_t serial = ++statusSerial_;
    statusJob_ = jobs->submit<std::shared_ptr<const Statuses>>(
        JobInfo{"CAM status", JobPriority::Interactive, false},
        [engine, state, build](JobContext& ctx) -> Result<std::shared_ptr<const Statuses>> {
            if (auto ok = ctx.cancel.check(); !ok)
                return tl::unexpected(ok.error());
            auto out = std::make_shared<Statuses>();
            out->ops = engine->status(state, build);
            out->setups = api::cam_setup::setupStates(state, build, ctx);
            if (auto ok = ctx.cancel.check(); !ok)
                return tl::unexpected(ok.error());
            return std::shared_ptr<const Statuses>(std::move(out));
        });
    QPointer<SetupTreeModel> self(this);
    statusJob_.then([self, serial](Result<std::shared_ptr<const Statuses>> r) {
        if (!self || serial != self->statusSerial_)
            return;
        self->statusJob_ = {};
        if (!r)
            return;
        self->statuses_ = **r;
        ++self->statusRuns_;
        // A generation the tree did not start (the engine says Generating, and no job of ours) ends without notice:
        // look again shortly, until it is over.
        bool foreign = false;
        for (const auto& [id, st] : self->statuses_.ops) {
            if (st.status == cam::OpStatus::Generating && self->generating_.count(id) == 0)
                foreign = true;
        }
        if (foreign)
            self->generatingPoll_.start();
        if (!self->rows_.empty())
            emit self->dataChanged(self->index(0), self->index(self->count() - 1));
        ++self->revision_;
        emit self->changed();
    });
    emit changed();
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
        badge = camui::Badge{it->second.status, qs(it->second.message)};
        if (const auto p = progress_.find(r->opId);
            p != progress_.end() && it->second.status == cam::OpStatus::Generating)
            progress = p->second;
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
        return badge ? camui::statusKey(badge->status) : QString();
    case StatusTextRole:
        return badge ? camui::statusText(badge->status, progress) : QString();
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
            {CanMoveDownRole, "canMoveDown"}};
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
    if (id == QLatin1String("cam.tree.edit") && !r->setup)
        return trt("Operations are edited in the operation panel");
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
