#include "setup_tool.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../selection_model.h"
#include "../session.h"
#include "../ui_commands.h"
#include "../units.h"
#include "../viewport_item.h"
#include "../viewport_navigation.h"

#include <ray_api/cam_operation.h>
#include <ray_api/feature_edit.h>
#include <ray_api/viewer.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace ray::ui {

using namespace modelui;

namespace {
// T-044: the setup tool. Its key works in the CAM workspace only (CommandContext::Cam).
// clang-format off
constexpr UiCommand kCamSetupCommands[] = {
    {"cam.tool.newSetup", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "New Setup…"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Opens the setup panel: pick the top face, the stock and the origin"), "Shift+N", CommandContext::Cam, "cam.setup.add"},
};
// clang-format on

constexpr const char* kTrContext = "ray::ui::SetupToolModel";

QString trs(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

QString latin1(std::string_view text)
{
    return QString::fromLatin1(text.data(), static_cast<qsizetype>(text.size()));
}

bool isLengthKey(const QString& key)
{
    static const QStringList keys = {QStringLiteral("xNeg"),     QStringLiteral("xPos"),   QStringLiteral("yNeg"),
                                     QStringLiteral("yPos"),     QStringLiteral("zNeg"),   QStringLiteral("zPos"),
                                     QStringLiteral("diameter"), QStringLiteral("length"), QStringLiteral("endOffset")};
    return keys.contains(key);
}

const cam::Setup* findSetup(const doc::DocState& state, const SetupId& id)
{
    if (!state.cam)
        return nullptr;
    const auto it = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                 [&](const cam::Setup& s) { return s.id == id; });
    return it == state.cam->setups.end() ? nullptr : &*it;
}

// Which field a command's message is about, from its JSON Pointer or member name.
QString fieldOf(const QString& message)
{
    static const std::pair<const char*, const char*> fields[] = {
        {"stock.xNeg", "xNeg"},
        {"stock.xPos", "xPos"},
        {"stock.yNeg", "yNeg"},
        {"stock.yPos", "yPos"},
        {"stock.zNeg", "zNeg"},
        {"stock.zPos", "zPos"},
        {"stock.diameter", "diameter"},
        {"stock.length", "length"},
        {"stock.endOffset", "endOffset"},
        {"stock.body", "solidBody"},
        {"programNumber", "programNumber"},
        {"partBodies", "bodies"},
        {"fixtureBodies", "bodies"},
        {"name", "name"},
    };
    for (const auto& [needle, key] : fields) {
        if (message.contains(QLatin1String(needle)))
            return QLatin1String(key);
    }
    return {};
}

} // namespace

std::span<const UiCommand> camSetupUiCommands()
{
    return kCamSetupCommands;
}

// ---------- construction ----------
SetupToolModel::SetupToolModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                               QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session), context_(context),
      lines_(std::make_unique<LineOverlay>(scene))
{
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(0);
    connect(&previewTimer_, &QTimer::timeout, this, [this] { startPreview(); });
    if (scene_) {
        connect(&scene_->preview(), &BodyOverlay::shown, this, [this] {
            if (scene_ && scene_->preview().owner() == this)
                emit statusChanged();
        });
        connect(&scene_->preview(), &BodyOverlay::released, this, [this](const QObject* owner) {
            if (owner == this) { // another tool took the slot: the stock is no longer shown
                preview_.reset();
                lines_->clear();
                emit markersChanged();
            }
        });
        connect(scene_, &SceneController::bodiesChanged, this, [this] {
            if (active())
                emit optionsChanged();
        });
    }
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            const std::string id = session_->documentId().toStdString();
            if (id != docId_) {
                if (active())
                    close(); // the document closed or another became active: nothing of this panel applies
                subscribe();
            }
        });
    }
    if (selection_)
        connect(selection_, &SelectionModel::changed, this, [this] { onSelectionChanged(); });
    if (context_) {
        // Leaving the CAM workspace mid-edit cancels the panel: the document stays as it was.
        connect(context_, &AppContext::workspaceChanged, this, [this] {
            if (active() && context_ && context_->workspace() != kCamWorkspace)
                cancel();
        });
    }
}

SetupToolModel::~SetupToolModel()
{
    if (commands_) {
        commands_->setUiHandler(QStringLiteral("cam.tool."), {});
        commands_->setUiHandler(QStringLiteral("cam.setup.add"), {});
    }
    resolveJob_.cancel();
    if (doc::Document* d = liveDoc(); d != nullptr && active())
        d->cancelPreview();
    if (scene_)
        scene_->preview().clear(this);
    if (filterSaved_ && selection_)
        selection_->setFilterMask(savedFilter_);
    lines_.reset();
    unsubscribe();
}

SetupToolModel* SetupToolModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    UnitsObject* units = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
    }
    auto* model = new SetupToolModel(scene, selection, session, context);
    model->setCommands(commands);
    model->setUnits(units);
    model->subscribe();
    return model; // owned by the engine
}

void SetupToolModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<SetupToolModel> self(this);
    const CommandCatalog::UiHandler handler{
        [self](const QString& id) { return self ? self->commandReason(id) : trs("The setup tool is not available"); },
        [self](const QString& id) { return self && self->runCommand(id); }};
    commands_->setUiHandler(QStringLiteral("cam.tool."), handler);
    commands_->setUiHandler(QStringLiteral("cam.setup.add"), handler);
}

void SetupToolModel::setUnits(UnitsObject* units)
{
    units_ = units;
}

// ---------- document access ----------
api::IApp* SetupToolModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* SetupToolModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void SetupToolModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<SetupToolModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->onDocumentChanged();
    });
    previewListener_ = doc_->subscribePreview([self, generation](doc::DocStatePtr state, doc::BuildResultPtr build) {
        if (self && self->docGeneration_ == generation && self->previewBusy_)
            self->onPreviewBuilt(std::move(state), std::move(build));
    });
}

void SetupToolModel::unsubscribe()
{
    if (doc::Document* d = liveDoc(); d != nullptr) {
        if (docListener_ != 0)
            d->unsubscribe(docListener_);
        if (previewListener_ != 0)
            d->unsubscribePreview(previewListener_);
    }
    doc_ = nullptr;
    docListener_ = previewListener_ = 0;
    docId_.clear();
    ++docGeneration_;
}

void SetupToolModel::onDocumentChanged()
{
    if (!active() || committing_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr) {
        close();
        return;
    }
    // The edited setup is gone (an undo of its add, a delete from elsewhere): the panel closes.
    if (editing_ && findSetup(*d->state(), *id_) == nullptr) {
        close();
        return;
    }
    emit optionsChanged();
    // The document's state changed under the panel (an undo, a model edit): that ended the running preview, which is
    // made again on the new state. A rebuild of the same state finishing changes nothing here.
    if (d->state() == previewBase_)
        return;
    previewBusy_ = false;
    schedulePreview();
}

// ---------- commands ----------
QString SetupToolModel::commandReason(const QString& id) const
{
    (void)id;
    api::IApp* a = app();
    if (a == nullptr)
        return trs("There is no session");
    api::OpenDocument* od = a->active();
    if (od == nullptr || !od->document)
        return trs("Open or import a part first");
    if (od->readOnly)
        return trs("The document is read-only");
    const doc::BuildResultPtr build = od->document->build();
    if (!build || build->bodies.empty())
        return trs("There is no body to machine yet");
    return {};
}

bool SetupToolModel::runCommand(const QString& id)
{
    if (id == QLatin1String("cam.tool.newSetup") || id == QLatin1String("cam.setup.add"))
        return start();
    return false;
}

// ---------- open and close ----------
QString SetupToolModel::title() const
{
    if (!active())
        return {};
    if (!editing_)
        return trs("New setup");
    doc::Document* d = liveDoc();
    const cam::Setup* s = d ? findSetup(*d->state(), *id_) : nullptr;
    return s ? trs("Edit %1").arg(qs(s->name)) : trs("Edit setup");
}

bool SetupToolModel::start()
{
    if (const QString reason = commandReason(QStringLiteral("cam.tool.newSetup")); !reason.isEmpty()) {
        if (context_)
            context_->showMessage(reason, true);
        return false;
    }
    if (context_ && context_->workspace() != kCamWorkspace)
        context_->setWorkspace(kCamWorkspace);
    open(std::nullopt);
    return active();
}

bool SetupToolModel::edit(const QString& setupId)
{
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return false;
    const SetupId id{Uuid::fromString(setupId.toStdString())};
    if (findSetup(*d->state(), id) == nullptr)
        return false;
    if (context_ && context_->workspace() != kCamWorkspace)
        context_->setWorkspace(kCamWorkspace);
    open(id);
    return active();
}

void SetupToolModel::open(const std::optional<SetupId>& existing)
{
    if (active())
        close();
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return;
    const doc::DocStatePtr state = d->state();
    id_ = existing ? *existing : SetupId{Uuid::generate()};
    editing_ = existing.has_value();
    values_.clear();
    roles_.clear();
    zFace_ = xEdge_ = vertex_ = QJsonObject();
    zFaceBody_.clear();
    xEdgeBody_.clear();
    vertexBody_.clear();
    message_.clear();
    messageField_.clear();
    previewStatus_.clear();
    committing_ = false;
    rejected_ = false;
    previewBusy_ = previewPending_ = false;
    previewBase_.reset();
    preview_.reset();
    ++pickSerial_;
    pickPending_ = false;
    loadOptions();
    if (editing_) {
        if (const cam::Setup* s = findSetup(*state, *id_))
            loadSetup(*state, *s);
    } else {
        defaults(*state);
    }
    pickMode_ = editing_ ? QString() : QStringLiteral("face");
    if (selection_) {
        savedFilter_ = selection_->filterMask();
        filterSaved_ = true;
        selection_->clear();
    }
    applyPickMode();
    emit activeChanged();
    emit valuesChanged();
    emit optionsChanged();
    emit picksChanged();
    emit statusChanged();
    schedulePreview();
}

void SetupToolModel::close()
{
    const bool was = active();
    previewTimer_.stop();
    ++previewSerial_;
    ++pickSerial_;
    pickPending_ = false;
    resolveJob_.cancel();
    resolveJob_ = {};
    if (doc::Document* d = liveDoc(); d != nullptr && was)
        d->cancelPreview();
    clearPreview();
    if (filterSaved_ && selection_)
        selection_->setFilterMask(savedFilter_);
    filterSaved_ = false;
    id_.reset();
    editing_ = false;
    values_.clear();
    roles_.clear();
    zFace_ = xEdge_ = vertex_ = QJsonObject();
    pickMode_.clear();
    message_.clear();
    messageField_.clear();
    previewStatus_.clear();
    committing_ = false;
    previewBusy_ = previewPending_ = false;
    previewBase_.reset();
    if (was) {
        emit activeChanged();
        emit valuesChanged();
        emit statusChanged();
        emit picksChanged();
        emit markersChanged();
    }
}

void SetupToolModel::cancel()
{
    close();
}

void SetupToolModel::defaults(const doc::DocState& state)
{
    // A name not used yet: "Setup N".
    std::set<std::string> names;
    int programNumber = 1000;
    QString machine, material;
    if (state.cam) {
        for (const cam::Setup& s : state.cam->setups) {
            names.insert(s.name);
            programNumber = std::max(programNumber, s.programNumber);
            if (s.machine.value != Uuid{})
                machine = idText(s.machine.value); // the document's last setup's machine
            if (s.material.value != Uuid{})
                material = idText(s.material.value); // and material
        }
    }
    int n = static_cast<int>(state.cam ? state.cam->setups.size() : 0) + 1;
    while (names.count(trs("Setup %1").arg(n).toStdString()) != 0)
        ++n;
    values_[QStringLiteral("name")] = trs("Setup %1").arg(n);
    // Every built body: the document's part bodies are parts, its fixture bodies fixtures.
    doc::Document* d = liveDoc();
    const doc::BuildResultPtr build = d ? d->build() : nullptr;
    for (const doc::Body& b : state.bodies) {
        if (build && build->bodies.count(b.id) == 0)
            continue;
        roles_[idText(b.id.value)] = b.role == doc::Body::Role::Fixture ? QStringLiteral("fixture")
                                     : b.role == doc::Body::Role::Part  ? QStringLiteral("part")
                                                                        : QStringLiteral("none");
    }
    const QString zero =
        units_ && units_->system() == UnitSystem::Inch ? QStringLiteral("0 in") : QStringLiteral("0 mm");
    values_[QStringLiteral("stock")] = QStringLiteral("box");
    for (const char* key : {"xNeg", "xPos", "yNeg", "yPos", "zNeg", "zPos", "endOffset"})
        values_[QLatin1String(key)] = zero;
    values_[QStringLiteral("diameter")] = QString();
    values_[QStringLiteral("length")] = QString();
    values_[QStringLiteral("axis")] = QStringLiteral("z");
    values_[QStringLiteral("solidBody")] = QString();
    values_[QStringLiteral("origin")] = QStringLiteral("corner0");
    values_[QStringLiteral("workOffset")] = 1;
    values_[QStringLiteral("programNumber")] = QString::number(programNumber + 1);
    values_[QStringLiteral("flipX")] = false;
    values_[QStringLiteral("flipZ")] = false;
    // The machine: this session's last, else the document's last setup's, else the first there is.
    const auto known = [&](const QString& id) {
        return std::any_of(machines_.begin(), machines_.end(),
                           [&](const QVariant& m) { return m.toMap().value(QStringLiteral("id")).toString() == id; });
    };
    if (!lastMachine_.isEmpty() && known(lastMachine_))
        machine = lastMachine_;
    if (machine.isEmpty() || !known(machine))
        machine = machines_.isEmpty() ? QString() : machines_.front().toMap().value(QStringLiteral("id")).toString();
    values_[QStringLiteral("machine")] = machine;
    values_[QStringLiteral("post")] = machinePost_.count(machine) ? machinePost_.at(machine) : QString();
    // The material: this session's last, else the document's last setup's, else none.
    const auto knownMaterial = [&](const QString& id) {
        return std::any_of(materials_.begin(), materials_.end(),
                           [&](const QVariant& m) { return m.toMap().value(QStringLiteral("id")).toString() == id; });
    };
    if (!lastMaterial_.isEmpty() && knownMaterial(lastMaterial_))
        material = lastMaterial_;
    values_[QStringLiteral("material")] = knownMaterial(material) ? material : QString();
}

void SetupToolModel::loadSetup(const doc::DocState& state, const cam::Setup& s)
{
    values_[QStringLiteral("name")] = qs(s.name);
    for (const doc::Body& b : state.bodies)
        roles_[idText(b.id.value)] = QStringLiteral("none");
    for (const BodyId& b : s.partBodies)
        roles_[idText(b.value)] = QStringLiteral("part");
    for (const BodyId& b : s.fixtureBodies)
        roles_[idText(b.value)] = QStringLiteral("fixture");
    const auto mm = [&](double v) {
        if (units_ && units_->system() == UnitSystem::Inch)
            return QLocale::c().toString(v / 25.4, 'g', 10) + QStringLiteral(" in");
        return QLocale::c().toString(v, 'g', 10) + QStringLiteral(" mm");
    };
    const QString zero = mm(0);
    for (const char* key : {"xNeg", "xPos", "yNeg", "yPos", "zNeg", "zPos", "endOffset"})
        values_[QLatin1String(key)] = zero;
    values_[QStringLiteral("diameter")] = QString();
    values_[QStringLiteral("length")] = QString();
    values_[QStringLiteral("axis")] = QStringLiteral("z");
    values_[QStringLiteral("solidBody")] = QString();
    if (const auto* box = std::get_if<cam::BoxStock>(&s.stock)) {
        values_[QStringLiteral("stock")] = QStringLiteral("box");
        values_[QStringLiteral("xNeg")] = mm(box->xNeg);
        values_[QStringLiteral("xPos")] = mm(box->xPos);
        values_[QStringLiteral("yNeg")] = mm(box->yNeg);
        values_[QStringLiteral("yPos")] = mm(box->yPos);
        values_[QStringLiteral("zNeg")] = mm(box->zNeg);
        values_[QStringLiteral("zPos")] = mm(box->zPos);
    } else if (const auto* cyl = std::get_if<cam::CylinderStock>(&s.stock)) {
        values_[QStringLiteral("stock")] = QStringLiteral("cylinder");
        values_[QStringLiteral("diameter")] = mm(cyl->diameter);
        values_[QStringLiteral("length")] = mm(cyl->length);
        values_[QStringLiteral("endOffset")] = mm(cyl->endOffset);
        values_[QStringLiteral("axis")] = cyl->axis == 0   ? QStringLiteral("x")
                                          : cyl->axis == 1 ? QStringLiteral("y")
                                                           : QStringLiteral("z");
    } else if (const auto* solid = std::get_if<cam::SolidStock>(&s.stock)) {
        values_[QStringLiteral("stock")] = QStringLiteral("solid");
        values_[QStringLiteral("solidBody")] = idText(solid->body.value);
    } else {
        values_[QStringLiteral("stock")] = QStringLiteral("box"); // stock from an earlier setup: not offered yet
    }
    values_[QStringLiteral("origin")] = camui::originKey(s.wcs);
    values_[QStringLiteral("workOffset")] = s.wcs.workOffset;
    values_[QStringLiteral("programNumber")] = QString::number(s.programNumber);
    values_[QStringLiteral("flipX")] = s.wcs.flipX;
    values_[QStringLiteral("flipZ")] = s.wcs.flipZ;
    values_[QStringLiteral("machine")] = s.machine.value == Uuid{} ? QString() : idText(s.machine.value);
    values_[QStringLiteral("post")] = qs(s.postId);
    values_[QStringLiteral("material")] = s.material.value == Uuid{} ? QString() : idText(s.material.value);
    if (s.wcs.zFace) {
        zFace_ = jsonObject(api::cam_setup::referenceJson(*s.wcs.zFace));
        zFaceBody_ = idText(s.wcs.zFace->body.value);
    }
    if (s.wcs.xEdge) {
        xEdge_ = jsonObject(api::cam_setup::referenceJson(*s.wcs.xEdge));
        xEdgeBody_ = idText(s.wcs.xEdge->body.value);
    }
    if (s.wcs.originVertex) {
        vertex_ = jsonObject(api::cam_setup::referenceJson(*s.wcs.originVertex));
        vertexBody_ = idText(s.wcs.originVertex->body.value);
    }
}

// The machines and materials the panel offers (the document's embedded ones and the library's, by id) and the posts.
void SetupToolModel::loadOptions()
{
    machines_.clear();
    machineJson_.clear();
    machinePost_.clear();
    materials_.clear();
    materialJson_.clear();
    materials_.push_back(QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("name"), trs("None")}});
    std::set<QString> seen, seenMaterials;
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    if (state && state->cam) {
        for (const auto& [id, m] : state->cam->machines) {
            const QString key = idText(id.value);
            machines_.push_back(QVariantMap{{QStringLiteral("id"), key}, {QStringLiteral("name"), qs(m.item.name)}});
            machinePost_[key] = qs(m.item.defaultPost);
            seen.insert(key);
        }
        for (const auto& [id, m] : state->cam->materials) {
            const QString key = idText(id.value);
            materials_.push_back(QVariantMap{{QStringLiteral("id"), key}, {QStringLiteral("name"), qs(m.item.name)}});
            seenMaterials.insert(key);
        }
    }
    api::IApp* a = app();
    lib::LibraryStack* stack = a ? a->libraries() : nullptr;
    std::vector<std::string> roots;
    if (stack != nullptr) {
        for (const lib::MachineDef& m : stack->machines()) {
            const QString key = idText(m.id.value);
            machineJson_[key] = api::cam_setup::machineJson(m);
            if (seen.insert(key).second) {
                machines_.push_back(QVariantMap{{QStringLiteral("id"), key}, {QStringLiteral("name"), qs(m.name)}});
                machinePost_[key] = qs(m.defaultPost);
            }
        }
        for (const lib::Material& m : stack->materials()) {
            const QString key = idText(m.id.value);
            materialJson_[key] = api::cam_operation::materialJson(m);
            if (seenMaterials.insert(key).second)
                materials_.push_back(QVariantMap{{QStringLiteral("id"), key}, {QStringLiteral("name"), qs(m.name)}});
        }
        // By name after "None" (ties by id), as the tool library lists them.
        std::stable_sort(materials_.begin() + 1, materials_.end(), [](const QVariant& a, const QVariant& b) {
            const QVariantMap ma = a.toMap(), mb = b.toMap();
            const int c = ma.value(QStringLiteral("name"))
                              .toString()
                              .compare(mb.value(QStringLiteral("name")).toString(), Qt::CaseInsensitive);
            return c != 0 ? c < 0
                          : ma.value(QStringLiteral("id")).toString() < mb.value(QStringLiteral("id")).toString();
        });
        for (const lib::LibraryStack::RootInfo& r : stack->roots())
            roots.push_back(r.dir);
    }
    // The posts of the library roots are listed on a worker (a root can be a slow network share); the machines'
    // default posts are offered at once.
    std::set<QString> posts;
    for (const auto& [id, post] : machinePost_) {
        if (!post.isEmpty())
            posts.insert(post);
    }
    posts_ = QStringList(posts.begin(), posts.end());
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr || roots.empty())
        return;
    const std::uint64_t serial = ++postsSerial_;
    JobHandle<std::vector<std::string>> job = jobs->submit<std::vector<std::string>>(
        JobInfo{"Post scripts", JobPriority::Background, false},
        [roots](JobContext&) -> Result<std::vector<std::string>> { return api::cam_setup::postStems(roots); });
    QPointer<SetupToolModel> self(this);
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

// ---------- values ----------
void SetupToolModel::setValue(const QString& key, const QVariant& value)
{
    if (!active() || values_.value(key) == value)
        return;
    values_[key] = value;
    if (key == QLatin1String("machine")) {
        // The machine's own post, if it names one.
        const QString m = value.toString();
        if (machinePost_.count(m) && !machinePost_.at(m).isEmpty())
            values_[QStringLiteral("post")] = machinePost_.at(m);
    }
    if (key == QLatin1String("origin")) {
        // A model point is a vertex picked in the view.
        if (value.toString() == QLatin1String("modelPoint")) {
            pickMode_ = QStringLiteral("vertex");
            applyPickMode();
            emit picksChanged();
        } else if (pickMode_ == QLatin1String("vertex")) {
            pickMode_.clear();
            applyPickMode();
            emit picksChanged();
        }
        emit markersChanged();
    }
    if (key == QLatin1String("stock") && value.toString() == QLatin1String("cylinder") && preview_) {
        // A first cylinder fits round the part: its diameter and length from the stock box shown.
        const Box3& b = preview_->resolved.partBounds;
        const QString axis = values_.value(QStringLiteral("axis")).toString();
        const double ex = b.max.x - b.min.x, ey = b.max.y - b.min.y, ez = b.max.z - b.min.z;
        const double across = axis == QLatin1String("x")   ? std::hypot(ey, ez)
                              : axis == QLatin1String("y") ? std::hypot(ex, ez)
                                                           : std::hypot(ex, ey);
        const double along = axis == QLatin1String("x") ? ex : axis == QLatin1String("y") ? ey : ez;
        const bool inch = units_ && units_->system() == UnitSystem::Inch;
        const auto text = [&](double mm) {
            return inch ? QLocale::c().toString(std::ceil(mm / 25.4 * 100) / 100, 'g', 10) + QStringLiteral(" in")
                        : QLocale::c().toString(std::ceil(mm), 'g', 10) + QStringLiteral(" mm");
        };
        if (values_.value(QStringLiteral("diameter")).toString().trimmed().isEmpty())
            values_[QStringLiteral("diameter")] = text(across);
        if (values_.value(QStringLiteral("length")).toString().trimmed().isEmpty())
            values_[QStringLiteral("length")] = text(along);
    }
    if (key == QLatin1String("stock") && value.toString() == QLatin1String("solid") &&
        values_.value(QStringLiteral("solidBody")).toString().isEmpty()) {
        // The first body that is no part of the setup.
        for (const auto& [id, role] : roles_) {
            if (role == QLatin1String("none")) {
                values_[QStringLiteral("solidBody")] = id;
                break;
            }
        }
    }
    emit valuesChanged();
    if (key == QLatin1String("flipZ"))
        emit picksChanged(); // "None: model -Z"
    schedulePreview();
}

void SetupToolModel::setBodyRole(const QString& bodyId, const QString& role)
{
    if (!active() ||
        (role != QLatin1String("part") && role != QLatin1String("fixture") && role != QLatin1String("none")))
        return;
    if (roles_[bodyId] == role)
        return;
    roles_[bodyId] = role;
    emit optionsChanged();
    schedulePreview();
}

QVariantList SetupToolModel::bodies() const
{
    QVariantList out;
    doc::Document* d = liveDoc();
    if (!active() || d == nullptr)
        return out;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    for (const doc::Body& b : state->bodies) {
        if (build && build->bodies.count(b.id) == 0)
            continue;
        const QString id = idText(b.id.value);
        const auto it = roles_.find(id);
        out.push_back(QVariantMap{{QStringLiteral("id"), id},
                                  {QStringLiteral("name"), qs(b.name)},
                                  {QStringLiteral("role"), it == roles_.end() ? QStringLiteral("none") : it->second}});
    }
    return out;
}

QString SetupToolModel::bodyName(const QString& id) const
{
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return {};
    for (const doc::Body& b : d->state()->bodies) {
        if (idText(b.id.value) == id)
            return qs(b.name);
    }
    return trs("a body");
}

QString SetupToolModel::zFaceText() const
{
    if (zFace_.isEmpty())
        return values_.value(QStringLiteral("flipZ")).toBool() ? trs("None: model -Z") : trs("None: model +Z");
    return trs("Face of %1").arg(bodyName(zFaceBody_));
}

QString SetupToolModel::xEdgeText() const
{
    if (xEdge_.isEmpty())
        return trs("None: from Z");
    return trs("Edge of %1").arg(bodyName(xEdgeBody_));
}

QString SetupToolModel::vertexText() const
{
    if (vertex_.isEmpty())
        return trs("Pick a vertex in the view");
    return trs("Vertex of %1").arg(bodyName(vertexBody_));
}

QString SetupToolModel::exprOf(const QString& raw) const
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return {};
    bool ok = false;
    QLocale::c().toDouble(text, &ok);
    if (!ok)
        return text; // an expression, a number with a unit, a parameter name
    return text + QLatin1Char(' ') + (units_ ? units_->unitLabel() : QStringLiteral("mm"));
}

camui::SetupInput SetupToolModel::input() const
{
    camui::SetupInput in;
    in.id = id_ ? idText(id_->value) : QString();
    in.name = values_.value(QStringLiteral("name")).toString();
    // Bodies in the document's order.
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    if (state) {
        for (const doc::Body& b : state->bodies) {
            const QString id = idText(b.id.value);
            const auto it = roles_.find(id);
            if (it == roles_.end())
                continue;
            if (it->second == QLatin1String("part"))
                in.partBodies << id;
            else if (it->second == QLatin1String("fixture"))
                in.fixtureBodies << id;
        }
    }
    in.stock = values_.value(QStringLiteral("stock")).toString();
    for (auto it = values_.begin(); it != values_.end(); ++it) {
        if (isLengthKey(it.key()))
            in.lengths[it.key()] = it.value().toString();
    }
    in.axis = values_.value(QStringLiteral("axis")).toString();
    in.solidBody = values_.value(QStringLiteral("solidBody")).toString();
    in.origin = values_.value(QStringLiteral("origin")).toString();
    in.workOffset = values_.value(QStringLiteral("workOffset")).toInt();
    in.programNumber = values_.value(QStringLiteral("programNumber")).toString();
    in.zFace = zFace_;
    in.xEdge = xEdge_;
    if (in.origin == QLatin1String("modelPoint"))
        in.originVertex = vertex_;
    in.flipX = values_.value(QStringLiteral("flipX")).toBool();
    in.flipZ = values_.value(QStringLiteral("flipZ")).toBool();
    in.machine = values_.value(QStringLiteral("machine")).toString();
    in.post = values_.value(QStringLiteral("post")).toString();
    // A library machine the document does not have yet is embedded in the same step.
    if (!in.machine.isEmpty() && state) {
        const MachineId mid{Uuid::fromString(in.machine.toStdString())};
        const bool embedded = state->cam && state->cam->machines.count(mid) != 0;
        if (!embedded && machineJson_.count(in.machine))
            in.libraryMachine = jsonObject(machineJson_.at(in.machine));
    }
    // So is a library material.
    in.material = values_.value(QStringLiteral("material")).toString();
    if (!in.material.isEmpty() && state) {
        const MaterialId mid{Uuid::fromString(in.material.toStdString())};
        const bool embedded = state->cam && state->cam->materials.count(mid) != 0;
        if (!embedded && materialJson_.count(in.material))
            in.libraryMaterial = jsonObject(materialJson_.at(in.material));
    }
    return in;
}

camui::ArgsResult SetupToolModel::arguments() const
{
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    const camui::LengthFn length = [&](const QString& text, QString* error) -> std::optional<double> {
        if (!state)
            return std::nullopt;
        auto v = api::feature_edit::evaluate(*state, exprOf(text).toStdString(), false);
        if (!v) {
            *error = qs(v.error().message);
            return std::nullopt;
        }
        return *v;
    };
    return camui::setupArgs(input(), length, !editing_);
}

// ---------- picking ----------
void SetupToolModel::setPickMode(const QString& mode)
{
    if (!active())
        return;
    const QString m = mode == QLatin1String("face") || mode == QLatin1String("edge") || mode == QLatin1String("vertex")
                          ? mode
                          : QString();
    if (m == pickMode_)
        return;
    pickMode_ = m;
    ++pickSerial_;
    pickPending_ = false;
    if (selection_)
        selection_->clear();
    applyPickMode();
    emit picksChanged();
}

void SetupToolModel::applyPickMode()
{
    if (!selection_ || !active())
        return;
    using render::PickFilter;
    std::uint8_t mask = filterSaved_ ? savedFilter_ : selection_->filterMask();
    if (pickMode_ == QLatin1String("face"))
        mask = static_cast<std::uint8_t>(PickFilter::Faces);
    else if (pickMode_ == QLatin1String("edge"))
        mask = static_cast<std::uint8_t>(PickFilter::Edges);
    else if (pickMode_ == QLatin1String("vertex"))
        mask = static_cast<std::uint8_t>(PickFilter::Vertices);
    selection_->setFilterMask(mask);
}

void SetupToolModel::clearPick(const QString& which)
{
    if (!active())
        return;
    if (which == QLatin1String("face")) {
        zFace_ = QJsonObject();
        zFaceBody_.clear();
    } else if (which == QLatin1String("edge")) {
        xEdge_ = QJsonObject();
        xEdgeBody_.clear();
    } else if (which == QLatin1String("vertex")) {
        vertex_ = QJsonObject();
        vertexBody_.clear();
    } else {
        return;
    }
    if (selection_)
        selection_->clear();
    emit picksChanged();
    schedulePreview();
}

void SetupToolModel::pickOrigin(const QString& key)
{
    cam::Wcs::OriginMode mode{};
    int corner = 0;
    if (!active() || !camui::originFromKey(key, &mode, &corner) || mode == cam::Wcs::OriginMode::ModelPoint)
        return;
    setValue(QStringLiteral("origin"), key);
}

void SetupToolModel::onSelectionChanged()
{
    if (!active() || pickMode_.isEmpty() || !selection_ || !scene_)
        return;
    const kernel::SubKind kind = pickMode_ == QLatin1String("face")   ? kernel::SubKind::Face
                                 : pickMode_ == QLatin1String("edge") ? kernel::SubKind::Edge
                                                                      : kernel::SubKind::Vertex;
    const std::vector<SelectionEntry>& entries = selection_->entries();
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->pending || !it->sub || it->sub->kind != kind)
            continue;
        takePick(kind, it->body, *it->sub);
        return;
    }
}

// The element is checked on a worker (a planar face for Z, a straight edge for X), then referenced through
// model.reference; or refused with the reason in the status line.
void SetupToolModel::takePick(kernel::SubKind kind, int bodyIndex, kernel::SubId sub)
{
    api::IApp* a = app();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    const api::viewer::Body* body = scene_ ? scene_->body(bodyIndex) : nullptr;
    if (a == nullptr || jobs == nullptr || body == nullptr)
        return;
    const std::uint64_t serial = ++pickSerial_;
    pickPending_ = true;
    emit statusChanged();
    const kernel::Shape shape = body->shape;
    const BodyId bodyId = body->id;
    struct Check {
        bool ok = true;
        QString reason;
    };
    JobHandle<Check> job = jobs->submit<Check>(
        JobInfo{"Check the pick", JobPriority::Interactive, false},
        [shape, sub, kind](JobContext& ctx) -> Result<Check> {
            auto info = api::viewer::describeElement(shape, sub, ctx);
            if (!info)
                return tl::unexpected(info.error());
            Check c;
            if (kind == kernel::SubKind::Face && info->surface && info->surface->type != kernel::SurfaceType::Plane) {
                c.ok = false;
                c.reason = QCoreApplication::translate(kTrContext,
                                                       "That face is not flat (it is %1): pick a planar face for Z")
                               .arg(latin1(api::viewer::surfaceTypeName(info->surface->type)).toLower());
            } else if (kind == kernel::SubKind::Edge && info->curve && info->curve->type != kernel::CurveType::Line) {
                c.ok = false;
                c.reason = QCoreApplication::translate(
                               kTrContext, "That edge is not straight (it is %1): pick a straight edge for X")
                               .arg(latin1(api::viewer::curveTypeName(info->curve->type)).toLower());
            }
            return c;
        });
    QPointer<SetupToolModel> self(this);
    job.then([self, serial, kind, bodyId, sub](Result<Check> r) {
        if (!self || serial != self->pickSerial_ || !self->active())
            return;
        if (!r || !r->ok) {
            self->pickPending_ = false;
            self->setStatus(self->previewStatus_, r ? r->reason : qs(r.error().message));
            if (self->selection_)
                self->selection_->clear(); // the refused element is not shown as picked
            return;
        }
        api::IApp* app = self->app();
        if (app == nullptr) {
            self->pickPending_ = false;
            return;
        }
        const QString kindText = kind == kernel::SubKind::Face   ? QStringLiteral("face")
                                 : kind == kernel::SubKind::Edge ? QStringLiteral("edge")
                                                                 : QStringLiteral("vertex");
        const QJsonObject args{{QStringLiteral("body"), idText(bodyId.value)},
                               {QStringLiteral("kind"), kindText},
                               {QStringLiteral("index"), static_cast<int>(sub.index)}};
        JobHandle<api::ActionResult> ref = app->actions().invoke("model.reference", jsonText(args), api::Caller::User);
        ref.then([self, serial, kind, bodyId](Result<api::ActionResult> res) {
            if (!self || serial != self->pickSerial_ || !self->active())
                return;
            self->pickPending_ = false;
            if (!res) {
                self->setStatus(self->previewStatus_, qs(res.error().message));
                return;
            }
            const QJsonObject refJson = jsonObject(res->resultJson).value(QStringLiteral("ref")).toObject();
            const QString body = idText(bodyId.value);
            if (kind == kernel::SubKind::Face) {
                self->zFace_ = refJson;
                self->zFaceBody_ = body;
            } else if (kind == kernel::SubKind::Edge) {
                self->xEdge_ = refJson;
                self->xEdgeBody_ = body;
            } else {
                self->vertex_ = refJson;
                self->vertexBody_ = body;
            }
            // The message about an earlier refused pick goes.
            self->setStatus(self->previewStatus_, QString());
            emit self->picksChanged();
            self->schedulePreview();
        });
    });
}

// ---------- preview ----------
void SetupToolModel::schedulePreview()
{
    if (active())
        previewTimer_.start();
}

void SetupToolModel::setStatus(const QString& status, const QString& message, const QString& field)
{
    previewStatus_ = status;
    message_ = message;
    messageField_ = field.isEmpty() && !message.isEmpty() ? fieldOf(message) : field;
    emit statusChanged();
}

void SetupToolModel::startPreview()
{
    if (!active() || committing_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return;
    if (previewBusy_) {
        previewPending_ = true;
        return;
    }
    previewPending_ = false;
    const camui::ArgsResult args = arguments();
    if (!args.error.isEmpty()) {
        rejected_ = true;
        setStatus(QStringLiteral("failed"), args.error, args.field);
        return;
    }
    rejected_ = false;
    ++previewSerial_;
    previewBase_ = d->state();
    previewBusy_ = true;
    // The message about the last values stays while the new ones build, so the panel does not change size twice
    // for every change.
    setStatus(QStringLiteral("building"), message_, messageField_);
    const doc::Command cmd{editing_ ? "cam.setup.edit" : "cam.setup.add", jsonText(args.args), title().toStdString()};
    d->beginPreview(cmd);
    if (const auto error = d->previewError()) {
        previewBusy_ = false;
        rejected_ = true;
        setStatus(QStringLiteral("failed"), qs(error->message));
    }
}

void SetupToolModel::onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build)
{
    previewBusy_ = false;
    if (!active())
        return;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (!state || !build || jobs == nullptr) {
        doc::Document* d = liveDoc();
        const auto error = d ? d->previewError() : std::nullopt;
        setStatus(QStringLiteral("failed"), error ? qs(error->message) : trs("The preview could not be built"));
    } else {
        // The resolve of the values before is superseded.
        if (resolveJob_.valid() && !resolveJob_.done() && resolveJob_.cancel())
            ++resolvesCancelled_;
        const std::uint64_t serial = previewSerial_;
        const SetupId id = *id_;
        resolveJob_ = jobs->submit<std::shared_ptr<const api::cam_setup::Preview>>(
            JobInfo{"Setup preview", JobPriority::Interactive, false},
            [state, build, id](JobContext& ctx) -> Result<std::shared_ptr<const api::cam_setup::Preview>> {
                auto p = api::cam_setup::previewSetup(state, build, id, ctx);
                if (!p)
                    return tl::unexpected(p.error());
                return std::make_shared<const api::cam_setup::Preview>(std::move(*p));
            });
        QPointer<SetupToolModel> self(this);
        resolveJob_.then([self, serial](Result<std::shared_ptr<const api::cam_setup::Preview>> r) {
            if (!self || serial != self->previewSerial_ || !self->active())
                return; // superseded, or the panel closed
            self->resolveJob_ = {};
            if (!r) {
                if (r.error().code != ErrorCode::Cancelled)
                    self->setStatus(QStringLiteral("failed"), qs(r.error().message));
                return; // the last good preview stays
            }
            self->showPreview(**r);
        });
    }
    if (previewPending_)
        startPreview();
}

void SetupToolModel::showPreview(const api::cam_setup::Preview& p)
{
    preview_ = p;
    if (scene_) {
        api::viewer::Body stock;
        stock.id = BodyId{Uuid::fromString("5e7c0000-0000-4000-8000-000000000000")}; // display only, never stored
        stock.name = "Stock";
        stock.shape = p.stock;
        const render::MeshStyle style{render::Token::StockFace, render::Token::ModelEdge, 0.35f, true, false};
        std::vector<api::viewer::Body> bodies;
        bodies.push_back(std::move(stock));
        scene_->preview().show(this, std::move(bodies), {}, false, style);
    }
    drawTriad(p);
    ++previewsShown_;
    setStatus(QStringLiteral("ok"), QString());
    emit markersChanged();
}

void SetupToolModel::clearPreview()
{
    if (scene_)
        scene_->preview().clear(this);
    if (lines_)
        lines_->clear();
    preview_.reset();
}

// The WCS triad at the origin: X, Y and Z in the axis colours. Drawn as viewport line items through the line overlay;
// this is the one place that changes when the renderer's toolpath line primitive takes them over.
void SetupToolModel::drawTriad(const api::cam_setup::Preview& p)
{
    const camui::Triad triad =
        camui::triadGeometry(p.origin, p.x, p.y, p.z, camui::triadLength(p.resolved.stockBounds));
    const render::Token tokens[] = {render::Token::AxisX, render::Token::AxisY, render::Token::AxisZ};
    const char* keys[] = {"triad.x", "triad.y", "triad.z"};
    for (std::size_t i = 0; i < 3; ++i)
        lines_->set(QLatin1String(keys[i]), triad.segments[i], tokens[i], 2.5f);
}

// ---------- commit ----------
bool SetupToolModel::canCommit() const
{
    return active() && !committing_ && !rejected_ && !pickPending_ && arguments().error.isEmpty();
}

bool SetupToolModel::commit()
{
    if (!active() || committing_)
        return false;
    api::IApp* a = app();
    if (a == nullptr)
        return false;
    const camui::ArgsResult args = arguments();
    if (!args.error.isEmpty()) {
        setStatus(QStringLiteral("failed"), args.error, args.field);
        return false;
    }
    committing_ = true;
    previewTimer_.stop();
    emit statusChanged();
    // As the user: the command's confirmation rule (changesToolpathsOrGcode) is for scripts and agents, so the
    // user's own OK is not asked about again.
    const char* name = editing_ ? "cam.setup.edit" : "cam.setup.add";
    JobHandle<api::ActionResult> job = a->actions().invoke(name, jsonText(args.args), api::Caller::User);
    if (!job.valid()) {
        committing_ = false;
        setStatus(QStringLiteral("failed"), trs("The setup could not be saved"));
        return false;
    }
    const QString id = idText(id_->value);
    const QString machine = values_.value(QStringLiteral("machine")).toString();
    const QString material = values_.value(QStringLiteral("material")).toString();
    QPointer<SetupToolModel> self(this);
    job.then([self, id, machine, material](Result<api::ActionResult> r) {
        if (!self)
            return;
        self->committing_ = false;
        if (!r) {
            self->setStatus(QStringLiteral("failed"), qs(r.error().message));
            self->schedulePreview();
            return;
        }
        if (!machine.isEmpty())
            self->lastMachine_ = machine;
        if (!material.isEmpty())
            self->lastMaterial_ = material;
        self->close();
        emit self->committed(id);
    });
    return true;
}

// ---------- the view ----------
QObject* SetupToolModel::view() const
{
    return view_;
}

void SetupToolModel::setView(QObject* view)
{
    auto* item = qobject_cast<ViewportItem*>(view);
    if (item == view_)
        return;
    for (const auto& c : viewConnections_)
        disconnect(c);
    viewConnections_.clear();
    view_ = item;
    if (view_) {
        const auto moved = [this] {
            if (preview_)
                emit markersChanged();
        };
        viewConnections_ << connect(view_, &ViewportItem::cameraChanged, this, moved);
        viewConnections_ << connect(view_, &QQuickItem::widthChanged, this, moved);
        viewConnections_ << connect(view_, &QQuickItem::heightChanged, this, moved);
    }
    emit viewChanged();
}

std::optional<QPointF> SetupToolModel::toView(Vec3 world) const
{
    if (!view_)
        return std::nullopt;
    return nav::project(view_->camera(), world, view_->width(), view_->height());
}

QVariantList SetupToolModel::markers() const
{
    QVariantList out;
    if (!active() || !preview_)
        return out;
    const QString current = values_.value(QStringLiteral("origin")).toString();
    for (int i = 0; i < 9; ++i) {
        const auto v = toView(preview_->markers[static_cast<std::size_t>(i)]);
        if (!v)
            continue;
        const QString key = camui::markerKey(i);
        out.push_back(QVariantMap{{QStringLiteral("key"), key},
                                  {QStringLiteral("x"), v->x()},
                                  {QStringLiteral("y"), v->y()},
                                  {QStringLiteral("current"), key == current}});
    }
    return out;
}

QVariantList SetupToolModel::triadLabels() const
{
    QVariantList out;
    if (!active() || !preview_)
        return out;
    const double len = camui::triadLength(preview_->resolved.stockBounds);
    const Vec3 dirs[] = {preview_->x, preview_->y, preview_->z};
    const char* names[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        const auto v = toView(add(preview_->origin, mul(dirs[i], len * 1.12)));
        if (!v)
            continue;
        out.push_back(QVariantMap{{QStringLiteral("axis"), QLatin1String(names[i])},
                                  {QStringLiteral("x"), v->x()},
                                  {QStringLiteral("y"), v->y()}});
    }
    return out;
}

bool SetupToolModel::previewBusy() const
{
    const bool meshing = scene_ && scene_->preview().owner() == this && scene_->preview().meshing();
    const bool resolving = resolveJob_.valid() && !resolveJob_.done();
    return pickPending_ || previewTimer_.isActive() || previewBusy_ || previewPending_ || resolving || meshing ||
           committing_;
}

} // namespace ray::ui
