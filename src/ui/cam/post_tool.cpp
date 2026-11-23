#include "post_tool.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../session.h"
#include "../ui_commands.h"
#include "cycletime_logic.h"
#include "setup_tool.h" // setup_tree.h's inline setSetupTool needs the whole type
#include "setup_tree.h"

#include <ray_api/cam_setup.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDate>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrl>

#include <algorithm>
#include <set>

namespace ray::ui {

using namespace modelui;
namespace cp = api::cam_post;

// One post run's results: the program and the viewer's index of it, both made on the worker.
struct PostRun {
    std::shared_ptr<const cp::Posted> posted;
    std::shared_ptr<const camui::GcodeIndex> index;
};

namespace {
// T-047: posting. Its key works in the CAM workspace only (CommandContext::Cam).
// clang-format off
constexpr UiCommand kCamPostCommands[] = {
    {"cam.post.open", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Post Setup…"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Posts the setup selected in the CAM tree: choose the post and its properties, then write the program file"), "Ctrl+P", CommandContext::Cam, "cam.post"},
    {"cam.post.gcode", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Show G-code"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Shows the last program posted in the G-code viewer"), "", CommandContext::Cam, ""},
};
// clang-format on

QString trp(const char* text)
{
    return QCoreApplication::translate("ray::ui::PostToolModel", text);
}

const cam::Setup* findSetup(const doc::DocState& state, const SetupId& id)
{
    if (!state.cam)
        return nullptr;
    const auto it = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                 [&](const cam::Setup& s) { return s.id == id; });
    return it == state.cam->setups.end() ? nullptr : &*it;
}

QString reasonText(cp::BlockReason r)
{
    switch (r) {
    case cp::BlockReason::NotBuilt:
        return trp("Not built");
    case cp::BlockReason::NoSetup:
        return trp("No setup");
    case cp::BlockReason::SetupBroken:
        return trp("Setup does not resolve");
    case cp::BlockReason::NoMachine:
        return trp("No machine");
    case cp::BlockReason::NoOperations:
        return trp("Nothing to post");
    case cp::BlockReason::Stale:
        return trp("Stale");
    case cp::BlockReason::Generating:
        return trp("Still generating");
    case cp::BlockReason::Failed:
        return trp("Failed");
    case cp::BlockReason::ReferenceBroken:
        return trp("Broken reference");
    case cp::BlockReason::NoToolpath:
        return trp("No toolpath");
    case cp::BlockReason::NoTool:
        return trp("No tool");
    }
    return {};
}

} // namespace

std::span<const UiCommand> camPostUiCommands()
{
    return kCamPostCommands;
}

// ---------- construction ----------
PostToolModel::PostToolModel(SceneController* scene, Session* session, AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), session_(session), context_(context), form_(new SchemaFormModel(this))
{
    checkTimer_.setSingleShot(true);
    checkTimer_.setInterval(0);
    connect(&checkTimer_, &QTimer::timeout, this, [this] { check(); });
    connect(form_, &SchemaFormModel::changed, this, [this] {
        updateFileName();
        emit canPostChanged();
    });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (session_ && session_->documentId().toStdString() != docId_ && active())
                close(); // the document closed or another became active
        });
    }
    connect(this, &PostToolModel::activeChanged, this, &PostToolModel::cycleTimeChanged); // T-059
}

// T-059: the cycle-time estimate is the CAM tree's (worked out there after every status run).
void PostToolModel::setTree(SetupTreeModel* tree)
{
    if (tree_)
        disconnect(tree_.data(), &SetupTreeModel::cycleTimesChanged, this, &PostToolModel::cycleTimeChanged);
    tree_ = tree;
    if (tree_)
        connect(tree_.data(), &SetupTreeModel::cycleTimesChanged, this, &PostToolModel::cycleTimeChanged);
}

QString PostToolModel::cycleTime() const
{
    const SetupTreeModel::CycleTimeEstimate* e = tree_ && setup_ ? tree_->cycleTimeOf(*setup_) : nullptr;
    if (e == nullptr || e->input.operations.empty())
        return {};
    return camui::cycleTimeDetails(e->time, e->time.assumptions, e->input.complete);
}

PostToolModel::~PostToolModel()
{
    if (commands_)
        commands_->setUiHandler(QStringLiteral("cam.post"), {});
    postJob_.cancel();
    writeJob_.cancel();
    statusJob_.cancel();
    setupJob_.cancel();
    recheckJob_.cancel();
    unsubscribe();
}

PostToolModel* PostToolModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    SetupTreeModel* tree = nullptr;
    GcodeModel* viewer = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        tree = qml->singletonInstance<SetupTreeModel*>(uri, QStringLiteral("CamTreeModel"));
        viewer = qml->singletonInstance<GcodeModel*>(uri, QStringLiteral("CamGcode"));
    }
    auto* model = new PostToolModel(scene, session, context);
    model->setTree(tree);
    model->setViewer(viewer);
    model->setCommands(commands);
    return model; // owned by the engine
}

void PostToolModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<PostToolModel> self(this);
    // "cam.post" covers the UI commands and the registry's cam.post action they front: from the palette, the dialog.
    commands_->setUiHandler(
        QStringLiteral("cam.post"),
        CommandCatalog::UiHandler{
            [self](const QString& id) { return self ? self->commandReason(id) : trp("Posting is not available"); },
            [self](const QString& id) { return self && self->runCommand(id); }});
}

api::IApp* PostToolModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* PostToolModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void PostToolModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<PostToolModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation && self->active()) {
            self->scheduleCheck();
            self->updateFileName();
        }
    });
    engine_ = a->cam();
    if (engine_ != nullptr) {
        engineListener_ = engine_->subscribe([self](const OperationId&) {
            // A generation started, progressed or ended: the gate again, coalesced (progress comes at 30 Hz).
            if (self && self->active() && !self->checkTimer_.isActive()) {
                self->checkTimer_.setInterval(100);
                self->checkTimer_.start();
            }
        });
    }
}

void PostToolModel::unsubscribe()
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
}

std::vector<std::string> PostToolModel::libraryRoots(bool shippedOnly) const
{
    std::vector<std::string> roots;
    api::IApp* a = app();
    if (lib::LibraryStack* stack = a ? a->libraries() : nullptr) {
        for (const lib::LibraryStack::RootInfo& r : stack->roots()) {
            // The library shipped with RayCAD is the read-only root (LibraryStack::setRoots): open, not writable.
            if (!shippedOnly || (r.open && !r.writable))
                roots.push_back(r.dir);
        }
    }
    return roots;
}

QString PostToolModel::proofOf(bool shipped, const post::PostInfo& info)
{
    if (!shipped)
        return {}; // a post of the user's own library: not labelled either way
    return info.verifiedOn.empty() ? QStringLiteral("unproven") : QStringLiteral("proven");
}

QString PostToolModel::documentName() const
{
    api::IApp* a = app();
    api::OpenDocument* od = a ? a->findDocument(docId_) : nullptr;
    QString title = od ? qs(od->title) : QString();
    if (title.endsWith(QLatin1String(".raycad"), Qt::CaseInsensitive))
        title.chop(7);
    return title;
}

// ---------- open / close ----------
QString PostToolModel::setupIdText() const
{
    return setup_ ? idText(setup_->value) : QString();
}

QString PostToolModel::title() const
{
    return setup_ ? trp("Post %1").arg(setupName_) : trp("Post");
}

bool PostToolModel::open(const QString& setupId)
{
    if (busy() || app() == nullptr)
        return false;
    if (active())
        close();
    subscribe();
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    const SetupId id{Uuid::fromString(setupId.toStdString())};
    const cam::Setup* setup = state ? findSetup(*state, id) : nullptr;
    if (setup == nullptr) {
        unsubscribe();
        return false;
    }
    setup_ = id;
    setupName_ = qs(setup->name);
    const lib::MachineDef* machine = cp::machineOf(*state, id);
    machineName_ = machine ? qs(machine->name) : QString();
    machineVerified_ = machine != nullptr && machine->verified;
    maxNameLength_ = machine ? machine->maxProgramNameLength : 0;

    const std::optional<cp::Settings> remembered = cp::rememberedSettings(app()->userDataDir(), id);
    const cp::Settings defaults;
    const cp::Settings& r = remembered ? *remembered : defaults;
    values_.clear();
    values_[QStringLiteral("programNumber")] = QString::number(setup->programNumber);
    values_[QStringLiteral("programComment")] = qs(setup->programComment);
    values_[QStringLiteral("units")] = (r.units ? *r.units : state->settings.units) == UnitSystem::Inch
                                           ? QStringLiteral("inch")
                                           : QStringLiteral("metric");
    values_[QStringLiteral("directory")] = qs(r.directory);
    values_[QStringLiteral("pattern")] = qs(r.fileNamePattern);
    values_[QStringLiteral("overwrite")] = qs(std::string(cp::overwriteKey(r.overwrite)));
    values_[QStringLiteral("limitNameLength")] = r.limitNameLength;
    rememberedPost_ = qs(r.postId);
    rememberedProps_ = r.properties;
    QString post = qs(r.postId);
    if (post.isEmpty())
        post = qs(setup->postId);
    if (post.isEmpty() && machine != nullptr)
        post = qs(machine->defaultPost);
    machine_ = machine ? std::optional<lib::MachineDef>(*machine) : std::nullopt;
    postId_ = post;
    info_.reset();
    postTitle_.clear();
    form_->setSchemaJson(QStringLiteral("{\"type\":\"object\",\"properties\":{}}"));

    state_ = QStringLiteral("idle");
    progress_ = -1;
    statusText_.clear();
    messages_.clear();
    resultPath_.clear();
    existingName_.clear();
    posted_.reset();
    blockers_.clear();
    checking_ = true;
    emit activeChanged();
    emit valuesChanged();
    emit stateChanged();
    emit gateChanged();
    loadPosts();
    inspect();
    scheduleCheck();
    updateFileName();
    emit canPostChanged();
    return true;
}

void PostToolModel::close()
{
    if (!active())
        return;
    postJob_.cancel();
    writeJob_.cancel();
    recheckJob_.cancel();
    statusJob_.cancel();
    setupJob_.cancel();
    ++postsSerial_;
    ++inspectSerial_;
    ++checkSerial_;
    ++runSerial_;
    checkTimer_.stop();
    setup_.reset();
    blockers_.clear();
    state_ = QStringLiteral("idle");
    unsubscribe();
    emit activeChanged();
    emit stateChanged();
    emit gateChanged();
    emit canPostChanged();
}

// ---------- the posts and their properties ----------
void PostToolModel::loadPosts()
{
    std::set<QString> posts;
    if (!postId_.isEmpty())
        posts.insert(postId_);
    posts_ = QStringList(posts.begin(), posts.end());
    emit optionsChanged();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return;
    const std::uint64_t serial = ++postsSerial_;
    const std::vector<std::string> roots = libraryRoots();
    // The roots may be network shares: read on a worker.
    JobHandle<std::vector<std::string>> job = jobs->submit<std::vector<std::string>>(
        JobInfo{"Post scripts", JobPriority::Interactive, false},
        [roots](JobContext&) -> Result<std::vector<std::string>> { return api::cam_setup::postStems(roots); });
    QPointer<PostToolModel> self(this);
    job.then([self, serial](Result<std::vector<std::string>> r) {
        if (!self || serial != self->postsSerial_ || !r)
            return;
        std::set<QString> all(self->posts_.begin(), self->posts_.end());
        for (const std::string& s : *r)
            all.insert(qs(s));
        self->posts_ = QStringList(all.begin(), all.end());
        emit self->optionsChanged();
    });
}

void PostToolModel::setPost(const QString& postId)
{
    if (!active() || postId == postId_ || busy())
        return;
    postId_ = postId;
    inspect();
}

void PostToolModel::inspect()
{
    info_.reset();
    postTitle_.clear();
    postMessage_.clear();
    postProof_.clear();
    postState_ = postId_.isEmpty() ? QString() : QStringLiteral("loading");
    if (postId_.isEmpty())
        postMessage_ = trp("Choose a post: neither the setup nor its machine names one");
    emit postChanged();
    emit canPostChanged();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr || postId_.isEmpty())
        return;
    const std::uint64_t serial = ++inspectSerial_;
    const std::vector<std::string> roots = libraryRoots();
    const std::vector<std::string> shippedRoots = libraryRoots(true);
    const std::string id = ss(postId_);
    struct Read {
        post::PostInfo info;
        bool shipped = false; // the script is the one shipped with RayCAD (no other root overrides it)
    };
    JobHandle<Read> job = jobs->submit<Read>(
        JobInfo{"Read post", JobPriority::Interactive, false}, [roots, shippedRoots, id](JobContext&) -> Result<Read> {
            const std::string path = cp::findPost(roots, id);
            if (path.empty())
                return tl::unexpected(Error{
                    ErrorCode::NotFound, "The post '" + id + "' is not in the library (posts/" + id + ".post.js)", {}});
            auto info = cp::inspectPost(path);
            if (!info)
                return tl::unexpected(info.error());
            const std::string shippedPath = cp::findPost(shippedRoots, id);
            return Read{std::move(*info), !shippedPath.empty() && shippedPath == path};
        });
    QPointer<PostToolModel> self(this);
    job.then([self, serial](Result<Read> read) {
        if (!self || serial != self->inspectSerial_ || !self->active())
            return;
        if (!read) {
            self->postState_ = QStringLiteral("failed");
            self->postMessage_ = qs(read.error().message);
            emit self->postChanged();
            emit self->canPostChanged();
            return;
        }
        const post::PostInfo* r = &read->info;
        self->info_ = *r;
        self->postProof_ = proofOf(read->shipped, *r);
        self->postTitle_ = qs(r->title);
        self->postState_ = QStringLiteral("ok");
        const lib::MachineDef* machine = self->machine_ ? &*self->machine_ : nullptr;
        self->form_->setSchemaJson(qs(cp::propertiesSchemaJson(*r, machine)));
        if (self->postId_ == self->rememberedPost_ || self->rememberedPost_.isEmpty())
            self->form_->load(jsonObject(cp::propertyValuesJson(*r, self->rememberedProps_)));
        else
            self->form_->load(QJsonObject());
        emit self->postChanged();
        self->updateFileName();
        emit self->canPostChanged();
    });
}

QString PostToolModel::notice() const
{
    if (!active())
        return {};
    // PLAN.md risk 4. A shipped post says whether it has been run on a real control (its declaration's verifiedOn,
    // docs/post_review); none has yet. A post of the user's own library is not labelled either way. The machine
    // definition says whether it was checked against the real machine (MachineDef::verified).
    const QString dryRun =
        trp("Dry-run the program before cutting: single block, feed hold in reach, above the part (an air cut) or "
            "without a tool. Check the work offsets, tool lengths and tool changes.");
    const bool postUnproven = postProof_ == QLatin1String("unproven");
    const QString machine = machineName_.isEmpty() ? trp("none") : machineName_;
    if (postUnproven && !machineVerified_)
        return trp("This post has not been proven on a real machine, and the machine definition (%1) has not been "
                   "checked against one. ")
                   .arg(machine) +
               dryRun;
    if (postUnproven)
        return trp("This post has not been proven on a real machine. ") + dryRun;
    if (!machineVerified_)
        return trp("The machine definition (%1) has not been checked against the real machine. ").arg(machine) + dryRun;
    return {};
}

// ---------- values ----------
void PostToolModel::setValue(const QString& key, const QVariant& value)
{
    if (!active() || busy() || values_.value(key) == value)
        return;
    values_[key] = value;
    emit valuesChanged();
    updateFileName();
    emit canPostChanged();
}

QString PostToolModel::homeRelative(const QString& path, const QString& home)
{
    const QString p = QDir::toNativeSeparators(path.trimmed());
    QString h = QDir::toNativeSeparators(QDir::cleanPath(home));
    if (p.isEmpty() || h.isEmpty())
        return p;
    while (h.endsWith(QDir::separator()))
        h.chop(1);
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive; // C:\Users\x and c:\users\x are one folder
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    if (p.compare(h, cs) == 0)
        return QStringLiteral("~");
    if (p.startsWith(h + QDir::separator(), cs))
        return QStringLiteral("~") + p.mid(h.size());
    return p;
}

QString PostToolModel::directoryShown() const
{
    return homeRelative(values_.value(QStringLiteral("directory")).toString(), QDir::homePath());
}

void PostToolModel::setFolder(const QVariant& urlOrPath)
{
    QString path;
    if (urlOrPath.canConvert<QUrl>() && urlOrPath.toUrl().isLocalFile())
        path = urlOrPath.toUrl().toLocalFile();
    else
        path = urlOrPath.toString();
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    setValue(QStringLiteral("directory"), QDir::toNativeSeparators(path));
}

cp::Settings PostToolModel::settings() const
{
    cp::Settings s;
    s.postId = ss(postId_);
    if (info_) {
        const lib::MachineDef* machine = machine_ ? &*machine_ : nullptr;
        s.properties = cp::propertyOverrides(*info_, machine, jsonText(form_->params()));
        const QJsonValue date = form_->valueOf(QStringLiteral("includeDate"));
        if (date.isBool() && date.toBool())
            s.date = ss(QDate::currentDate().toString(Qt::ISODate));
    }
    bool ok = false;
    const int number = values_.value(QStringLiteral("programNumber")).toString().trimmed().toInt(&ok);
    if (ok)
        s.programNumber = number;
    s.programComment = ss(values_.value(QStringLiteral("programComment")).toString());
    s.units = values_.value(QStringLiteral("units")).toString() == QLatin1String("inch") ? UnitSystem::Inch
                                                                                         : UnitSystem::Metric;
    s.directory = ss(values_.value(QStringLiteral("directory")).toString().trimmed());
    s.fileNamePattern = ss(values_.value(QStringLiteral("pattern")).toString());
    s.overwrite =
        cp::overwriteFromKey(ss(values_.value(QStringLiteral("overwrite")).toString())).value_or(cp::Overwrite::Ask);
    s.limitNameLength = values_.value(QStringLiteral("limitNameLength")).toBool();
    return s;
}

void PostToolModel::updateFileName()
{
    QString name, note;
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    if (active() && info_ && state) {
        auto f = cp::fileNameFor(*state, *setup_, *info_, settings(), ss(documentName()));
        if (f) {
            name = qs(f->name);
            QStringList notes;
            for (const std::string& n : f->notes)
                notes.push_back(qs(n));
            note = notes.join(QLatin1Char(' '));
        } else {
            note = qs(f.error().message);
        }
    }
    if (name == fileName_ && note == fileNameNote_)
        return;
    fileName_ = name;
    fileNameNote_ = note;
    emit valuesChanged();
}

// ---------- the gate ----------
void PostToolModel::scheduleCheck()
{
    checkTimer_.setInterval(0);
    checkTimer_.start();
}

void PostToolModel::check()
{
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (!active() || d == nullptr || jobs == nullptr || engine == nullptr)
        return;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    statusJob_.cancel();
    setupJob_.cancel();
    const std::uint64_t serial = ++checkSerial_;
    checking_ = true;
    emit gateChanged();
    emit canPostChanged();
    statusJob_ = engine->statusAsync(state, build);
    setupJob_ = jobs->submit<std::shared_ptr<const std::map<SetupId, Status>>>(
        JobInfo{"Post check", JobPriority::Interactive, false},
        [state, build](JobContext& ctx) -> Result<std::shared_ptr<const std::map<SetupId, Status>>> {
            return std::make_shared<const std::map<SetupId, Status>>(api::cam_setup::setupStates(state, build, ctx));
        });
    auto statuses = std::make_shared<std::optional<std::map<OperationId, cam::OpState>>>();
    auto setups = std::make_shared<std::optional<std::shared_ptr<const std::map<SetupId, Status>>>>();
    QPointer<PostToolModel> self(this);
    const auto both = [self, serial, state, build, statuses, setups] {
        if (!self || serial != self->checkSerial_ || !statuses->has_value() || !setups->has_value() || !self->active())
            return;
        Status own = std::monostate{};
        if (const auto it = (**setups)->find(*self->setup_); it != (**setups)->end())
            own = it->second;
        self->blockers_ = cp::blockers(*state, build != nullptr, *self->setup_, **statuses, own);
        self->checking_ = false;
        emit self->gateChanged();
        emit self->canPostChanged();
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

QVariantList PostToolModel::blockers() const
{
    QVariantList out;
    for (const cp::Blocker& b : blockers_) {
        out.push_back(QVariantMap{{QStringLiteral("opId"), (b.op.value == Uuid{}) ? QString() : idText(b.op.value)},
                                  {QStringLiteral("name"), b.name.empty() ? trp("Setup") : qs(b.name)},
                                  {QStringLiteral("reason"), qs(std::string(cp::reasonKey(b.reason)))},
                                  {QStringLiteral("reasonText"), reasonText(b.reason)},
                                  {QStringLiteral("message"), qs(b.message)},
                                  {QStringLiteral("canGenerate"), b.generateFixes}});
    }
    return out;
}

bool PostToolModel::generate(const QString& opId)
{
    doc::Document* d = liveDoc();
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (d == nullptr || engine == nullptr || !d->build())
        return false;
    const OperationId op{Uuid::fromString(opId.toStdString())};
    JobHandle<std::shared_ptr<const cam::Toolpath>> job = engine->generate(d->state(), d->build(), op);
    QPointer<PostToolModel> self(this);
    job.then([self](Result<std::shared_ptr<const cam::Toolpath>>) {
        if (self && self->active())
            self->scheduleCheck();
    });
    scheduleCheck(); // shows Generating
    return true;
}

bool PostToolModel::generateAll()
{
    bool any = false;
    for (const cp::Blocker& b : blockers_) {
        if (b.generateFixes && !(b.op.value == Uuid{}))
            any = generate(idText(b.op.value)) || any;
    }
    return any;
}

// ---------- posting ----------
bool PostToolModel::busy() const
{
    return state_ == QLatin1String("posting") || state_ == QLatin1String("checking") ||
           state_ == QLatin1String("writing");
}

QString PostToolModel::postBlockedReason() const
{
    if (!active())
        return trp("Choose a setup to post");
    if (busy())
        return trp("Posting…");
    if (state_ == QLatin1String("asking"))
        return trp("Answer the question about the existing file first");
    if (!blockers_.empty())
        return blockers_.size() == 1 ? trp("Posting is blocked: an operation needs attention")
                                     : trp("Posting is blocked: %1 problems need attention").arg(blockers_.size());
    if (checking_)
        return trp("Checking the operations…");
    if (postState_ == QLatin1String("loading"))
        return trp("Reading the post…");
    if (postState_ != QLatin1String("ok"))
        return postMessage_.isEmpty() ? trp("Choose a post") : postMessage_;
    if (const auto e = form_->firstError())
        return trp("Post property %1: %2").arg(e->first, e->second);
    bool ok = false;
    const int number = values_.value(QStringLiteral("programNumber")).toString().trimmed().toInt(&ok);
    if (!ok || number < 0 || number > 99999999)
        return trp("The program number must be a whole number from 0 to 99999999");
    if (values_.value(QStringLiteral("directory")).toString().trimmed().isEmpty())
        return trp("Choose the folder to write the program to");
    if (fileName_.isEmpty())
        return fileNameNote_.isEmpty() ? trp("The file name pattern makes no name") : fileNameNote_;
    return {};
}

void PostToolModel::setState(const QString& state, const QString& status)
{
    state_ = state;
    statusText_ = status;
    if (state != QLatin1String("posting"))
        progress_ = -1;
    emit stateChanged();
    emit canPostChanged();
}

void PostToolModel::addMessages(const std::vector<std::string>& texts, const QString& kind)
{
    for (const std::string& t : texts) {
        const camui::MessagePlace place = camui::attachTo(t);
        // The script and its line are shown apart ("(haas-ngc.post.js, line 256)"); the post's own words in full.
        QVariantMap m{{QStringLiteral("kind"), kind},
                      {QStringLiteral("text"), qs(t.substr(std::min(place.textStart, t.size())))},
                      {QStringLiteral("full"), qs(t)},
                      {QStringLiteral("opId"), QString()},
                      {QStringLiteral("opName"), QString()},
                      {QStringLiteral("script"), qs(place.script)},
                      {QStringLiteral("scriptLine"), place.scriptLine ? *place.scriptLine : -1}};
        if (place.section && posted_ && *place.section < posted_->sectionOps.size()) {
            m[QStringLiteral("opId")] = idText(posted_->sectionOps[*place.section].value);
            m[QStringLiteral("opName")] = qs(posted_->sectionNames[*place.section]);
        } else if (place.section && setup_) {
            // An error comes before any Posted: the setup's own order (suppressed operations are not posted).
            doc::Document* d = liveDoc();
            const doc::DocStatePtr state = d ? d->state() : nullptr;
            const cam::Setup* s = state ? findSetup(*state, *setup_) : nullptr;
            std::uint32_t n = 0;
            for (const OperationId& id : s ? s->operations : std::vector<OperationId>{}) {
                const auto op = state->cam->operations.find(id);
                if (op == state->cam->operations.end() || op->second.suppressed)
                    continue;
                if (n++ == *place.section) {
                    m[QStringLiteral("opId")] = idText(id.value);
                    m[QStringLiteral("opName")] = qs(op->second.name);
                }
            }
        }
        messages_.push_back(m);
    }
}

void PostToolModel::fail(const Error& error)
{
    if (error.code == ErrorCode::Cancelled) {
        setState(QStringLiteral("cancelled"), trp("Posting was cancelled; no file was written"));
        return;
    }
    // Every line of the message (the gate lists one per operation).
    std::vector<std::string> lines;
    std::size_t from = 0;
    const std::string& text = error.message;
    while (from <= text.size()) {
        const std::size_t nl = text.find('\n', from);
        const std::string line = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        if (!line.empty())
            lines.push_back(line);
        if (nl == std::string::npos)
            break;
        from = nl + 1;
    }
    addMessages(lines, QStringLiteral("error"));
    const std::string problem = cp::problemKey(error);
    QString status;
    if (problem == "exists")
        status = trp("Nothing was written: the file exists and the overwrite rule refuses to replace it");
    else if (!problem.empty())
        status = trp("Nothing was written: the program could not be put in the folder");
    else
        status = trp("Nothing was written: the post stopped with an error");
    if (error.code == ErrorCode::PostFailed)
        scheduleCheck(); // the gate refused: the list shows why
    setState(QStringLiteral("failed"), status);
}

bool PostToolModel::post()
{
    if (!canPost()) {
        statusText_ = postBlockedReason();
        emit stateChanged();
        return false;
    }
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (d == nullptr || jobs == nullptr || engine == nullptr)
        return false;
    const cp::Settings s = settings();
    writeBack();
    rememberNow();
    messages_.clear();
    resultPath_.clear();
    existingName_.clear();
    posted_.reset();
    ++runs_;
    const std::uint64_t serial = ++runSerial_;
    progress_ = 0;
    setState(QStringLiteral("posting"), trp("Posting %1 with %2…").arg(setupName_, postTitle_));

    cp::Inputs in;
    in.setup = *setup_;
    in.state = d->state();
    in.build = d->build();
    in.libraryRoots = libraryRoots();
    in.documentName = ss(documentName());
    // The statuses of exactly this snapshot (the engine's own job), then the post on a worker.
    statusJob_.cancel();
    recheckJob_ = engine->statusAsync(in.state, in.build);
    QPointer<PostToolModel> self(this);
    recheckJob_.then([self, serial, in, s, jobs](Result<std::map<OperationId, cam::OpState>> r) mutable {
        if (!self || serial != self->runSerial_)
            return;
        if (!r) {
            self->fail(r.error());
            return;
        }
        in.statuses = std::move(*r);
        self->postJob_ = jobs->submit<std::shared_ptr<const PostRun>>(
            JobInfo{"Post", JobPriority::Interactive, false},
            [in, s](JobContext& ctx) -> Result<std::shared_ptr<const PostRun>> {
                auto posted = cp::run(in, s, ctx);
                if (!posted)
                    return tl::unexpected(posted.error());
                if (auto ok = ctx.cancel.check(); !ok)
                    return tl::unexpected(ok.error());
                auto run = std::make_shared<PostRun>();
                auto shared = std::make_shared<cp::Posted>(std::move(*posted));
                auto text = std::make_shared<const std::string>(shared->output.text);
                run->index = camui::buildIndex(text, shared->output.lineMap);
                run->posted = std::move(shared);
                return std::shared_ptr<const PostRun>(std::move(run));
            });
        self->postJob_.onProgress([self, serial](double fraction, std::string) {
            if (!self || serial != self->runSerial_ || self->state_ != QLatin1String("posting"))
                return;
            self->progress_ = fraction;
            emit self->stateChanged();
        });
        self->postJob_.then([self, serial](Result<std::shared_ptr<const PostRun>> run) {
            if (!self || serial != self->runSerial_)
                return;
            if (!run) {
                self->fail(run.error());
                return;
            }
            self->viewerIndex_ = (*run)->index;
            self->afterPost((*run)->posted);
        });
    });
    return true;
}

void PostToolModel::afterPost(std::shared_ptr<const cp::Posted> posted)
{
    posted_ = std::move(posted);
    addMessages(posted_->output.warnings, QStringLiteral("warning"));
    addMessages(posted_->notes, QStringLiteral("note"));
    recheckThenWrite(std::nullopt);
}

// The gate again on the document as it is now, then the file. `rule`: the answer to "the file exists".
void PostToolModel::recheckThenWrite(std::optional<cp::Overwrite> rule)
{
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    cam::ICamEngine* engine = app() ? app()->cam() : nullptr;
    if (d == nullptr || jobs == nullptr || engine == nullptr || !posted_) {
        fail(Error{ErrorCode::Cancelled, "", {}});
        return;
    }
    setState(QStringLiteral("checking"), trp("Checking that nothing changed while it was posted…"));
    const std::uint64_t serial = runSerial_;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    cp::Settings s = settings();
    if (rule)
        s.overwrite = *rule;
    const bool ask = s.overwrite == cp::Overwrite::Ask;
    recheckJob_ = engine->statusAsync(state, build);
    QPointer<PostToolModel> self(this);
    recheckJob_.then([self, serial, state, build, s, ask, jobs](Result<std::map<OperationId, cam::OpState>> r) {
        if (!self || serial != self->runSerial_)
            return;
        if (!r) {
            self->fail(r.error());
            return;
        }
        auto posted = self->posted_;
        post::IOutputFs* fs = self->fs_;
        self->setState(QStringLiteral("writing"), trp("Writing %1…").arg(qs(posted->fileName.name)));
        auto statuses = std::make_shared<std::map<OperationId, cam::OpState>>(std::move(*r));
        // Ask: an existing file is reported (an empty Written path) instead of written.
        self->writeJob_ = jobs->submit<cp::Written>(
            JobInfo{"Write program", JobPriority::Interactive, false},
            [posted, s, ask, state, build, statuses, fs](JobContext& ctx) -> Result<cp::Written> {
                Status own = std::monostate{};
                if (build) {
                    const auto states = api::cam_setup::setupStates(state, build, ctx);
                    if (const auto it = states.find(posted->setup); it != states.end())
                        own = it->second;
                }
                if (auto ok = cp::stillCurrent(*posted, *state, build != nullptr, *statuses, own); !ok)
                    return tl::unexpected(ok.error());
                if (ask) {
                    auto there = cp::targetExists(*posted, s, fs);
                    if (!there)
                        return tl::unexpected(there.error());
                    if (*there)
                        return cp::Written{}; // ask the user
                }
                return cp::write(*posted, s, fs, &ctx.cancel);
            });
        self->writeJob_.then([self, serial](Result<cp::Written> w) {
            if (!self || serial != self->runSerial_)
                return;
            if (!w) {
                self->fail(w.error());
                return;
            }
            if (w->path.empty()) {
                self->existingName_ = qs(self->posted_->fileName.name);
                self->setState(QStringLiteral("asking"),
                               trp("%1 already exists in the folder").arg(self->existingName_));
                return;
            }
            self->finish(*w);
        });
    });
}

void PostToolModel::answerOverwrite(const QString& answer)
{
    if (state_ != QLatin1String("asking"))
        return;
    if (answer == QLatin1String("replace"))
        recheckThenWrite(cp::Overwrite::Replace);
    else if (answer == QLatin1String("newName"))
        recheckThenWrite(cp::Overwrite::NewName);
    else
        setState(QStringLiteral("cancelled"), trp("Nothing was written: the existing file was kept"));
}

void PostToolModel::finish(const cp::Written& written)
{
    ++writes_;
    resultPath_ = qs(written.path);
    // The name here, the folder under it in the dialog (cut in the middle when long).
    const QString name = qs(written.fileName);
    QString status = written.replaced ? trp("Written %1, replacing the old file").arg(name)
                     : name != qs(posted_->fileName.name)
                         ? trp("Written %1 (%2 exists; it was kept)").arg(name, qs(posted_->fileName.name))
                         : trp("Written %1").arg(name);
    setState(QStringLiteral("done"), status);
    if (viewer_ && posted_ && viewerIndex_) {
        auto program = std::make_shared<PostedProgram>();
        program->index = viewerIndex_;
        program->setup = posted_->setup;
        program->sectionOps = posted_->sectionOps;
        for (const std::string& n : posted_->sectionNames)
            program->sectionNames.push_back(qs(n));
        program->sectionInputs = posted_->sectionInputs;
        program->fileName = qs(written.fileName);
        program->path = resultPath_;
        program->postTitle = qs(posted_->info.title);
        program->setupName = qs(posted_->setupName);
        viewer_->setProgram(std::move(program));
    }
    emit this->written(resultPath_);
}

void PostToolModel::cancel()
{
    if (state_ == QLatin1String("asking")) {
        setState(QStringLiteral("cancelled"), trp("Nothing was written: the existing file was kept"));
        return;
    }
    if (!busy()) {
        close();
        return;
    }
    // Each step ends Cancelled (and its continuation says so) unless the file is already being put in place.
    statusText_ = trp("Cancelling…");
    emit stateChanged();
    recheckJob_.cancel();
    postJob_.cancel();
    writeJob_.cancel();
}

void PostToolModel::showGcode()
{
    if (viewer_ && viewer_->hasProgram()) {
        close();
        viewer_->show();
    }
}

// The setup's own members (post, program number, comment) in the document: one undo step, only when changed.
void PostToolModel::writeBack()
{
    api::IApp* a = app();
    api::OpenDocument* od = a ? a->findDocument(docId_) : nullptr;
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    const cam::Setup* setup = state ? findSetup(*state, *setup_) : nullptr;
    if (od == nullptr || od->readOnly || setup == nullptr)
        return;
    const cp::Settings s = settings();
    QJsonObject args{{QStringLiteral("id"), idText(setup_->value)}};
    if (s.programNumber && *s.programNumber != setup->programNumber)
        args.insert(QStringLiteral("programNumber"), *s.programNumber);
    if (s.programComment && *s.programComment != setup->programComment)
        args.insert(QStringLiteral("programComment"), qs(*s.programComment));
    if (!s.postId.empty() && s.postId != setup->postId)
        args.insert(QStringLiteral("post"), qs(s.postId));
    if (args.size() == 1)
        return;
    (void)a->actions().invoke("cam.setup.edit", jsonText(args), api::Caller::User);
}

void PostToolModel::rememberNow()
{
    api::IApp* a = app();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (a == nullptr || jobs == nullptr || a->userDataDir().empty())
        return;
    const std::string dir = a->userDataDir();
    const SetupId setup = *setup_;
    const cp::Settings s = settings();
    // A small local file, but a write with a flush: never on the UI thread.
    (void)jobs->submit<std::monostate>(
        JobInfo{"Remember post settings", JobPriority::Background, false},
        [dir, setup, s](JobContext&) -> Result<std::monostate> { return cp::remember(dir, setup, s); });
}

// ---------- commands ----------
QString PostToolModel::commandReason(const QString& id) const
{
    api::IApp* a = app();
    if (a == nullptr)
        return trp("There is no session");
    if (a->active() == nullptr)
        return trp("Open or import a part first");
    if (id == QLatin1String("cam.post.gcode"))
        return viewer_ && viewer_->hasProgram() ? QString() : trp("Post a setup first");
    if (busy())
        return trp("A setup is being posted");
    if (!tree_ || !tree_->currentSetup())
        return trp("Select a setup in the CAM tree first");
    return {};
}

bool PostToolModel::runCommand(const QString& id)
{
    if (!commandReason(id).isEmpty())
        return false;
    if (id == QLatin1String("cam.post.gcode")) {
        viewer_->show();
        return true;
    }
    // cam.post.open, and the cam.post action from the palette: the dialog for the current row's setup.
    return open(idText(tree_->currentSetup()->value));
}

} // namespace ray::ui
