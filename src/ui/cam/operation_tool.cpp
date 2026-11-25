#include "operation_tool.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../model/preview.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../selection_model.h"
#include "../session.h"
#include "../ui_commands.h"
#include "../units.h"
#include "operation_view.h"
#include "setup_tool.h"
#include "setup_tree.h"
#include "tools_model.h"

#include <ray_api/cam_setup.h>
#include <ray_api/feature_edit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace ray::ui {

using namespace modelui;

namespace {
// T-045: the operation panel. Its key works in the CAM workspace only (CommandContext::Cam).
// clang-format off
constexpr UiCommand kCamOperationCommands[] = {
    {"cam.opTool.new", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "New Operation…"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Opens the operation panel for a new operation in the selected setup"), "Shift+O", CommandContext::Cam, "cam.operation.add"},
};
// clang-format on

constexpr const char* kTr = "ray::ui::OperationToolModel";

QString trs(const char* text)
{
    return QCoreApplication::translate(kTr, text);
}

QString cText(double v)
{
    return QLocale::c().toString(v, 'g', 10);
}

const cam::Setup* findSetup(const doc::DocState& state, const SetupId& id)
{
    if (!state.cam)
        return nullptr;
    const auto it = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                 [&](const cam::Setup& s) { return s.id == id; });
    return it == state.cam->setups.end() ? nullptr : &*it;
}

const cam::Setup* setupListing(const doc::DocState& state, const OperationId& op)
{
    if (!state.cam)
        return nullptr;
    for (const cam::Setup& s : state.cam->setups) {
        if (std::find(s.operations.begin(), s.operations.end(), op) != s.operations.end())
            return &s;
    }
    return nullptr;
}

QString heightLabel(const QString& key)
{
    if (key == QLatin1String("clearance"))
        return trs("Clearance");
    if (key == QLatin1String("retract"))
        return trs("Retract");
    if (key == QLatin1String("feedPlane"))
        return trs("Feed plane");
    if (key == QLatin1String("top"))
        return trs("Top");
    return trs("Bottom");
}

QString heightHint(const QString& key)
{
    if (key == QLatin1String("clearance"))
        return trs("Rapid-safe height between operations (the initial level of a cycle)");
    if (key == QLatin1String("retract"))
        return trs("Rapid-safe height within the operation (the R plane of a cycle)");
    if (key == QLatin1String("feedPlane"))
        return trs("Where the tool changes from rapid to feed");
    if (key == QLatin1String("top"))
        return trs("Top of the machined range");
    return trs("Bottom of the machined range");
}

bool isCuttingKey(const QString& key)
{
    return camui::cuttingKeys().contains(key);
}

std::optional<double> recordedValue(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

bool sameRecord(const std::optional<cam::CalculatedFeeds>& a, const std::optional<cam::CalculatedFeeds>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    if (!a)
        return true;
    return camui::calculatedJson(*a) == camui::calculatedJson(*b);
}

} // namespace

std::span<const UiCommand> camOperationUiCommands()
{
    return kCamOperationCommands;
}

// ---------- construction ----------
OperationToolModel::OperationToolModel(SceneController* scene, SelectionModel* selection, Session* session,
                                       AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session), context_(context),
      form_(new SchemaFormModel(this))
{
    form_->setExprFn(exprFn());
    connect(form_, &SchemaFormModel::edited, this, [this](const QString&) {
        if (!active())
            return;
        emit toolsChanged(); // the tools a type offers can follow its params (x-toolTypes)
        schedulePreview();
    });
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(0);
    connect(&previewTimer_, &QTimer::timeout, this, [this] { startPreview(); });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            if (session_->documentId().toStdString() != docId_) {
                if (active())
                    close();
                subscribe();
            }
        });
    }
    if (selection_)
        connect(selection_, &SelectionModel::changed, this, [this] { onSelectionChanged(); });
    if (context_) {
        connect(context_, &AppContext::workspaceChanged, this, [this] {
            if (active() && context_ && context_->workspace() != kCamWorkspace)
                cancel();
        });
    }
    // The picks stay marked in the view as the list changes, and when the bodies are shown anew.
    connect(this, &OperationToolModel::picksChanged, this, &OperationToolModel::updatePickHighlight);
    if (scene_)
        connect(scene_, &SceneController::bodiesChanged, this, &OperationToolModel::updatePickHighlight);
    // The points of points parameters are drawn as they change.
    if (scene_)
        pointLines_ = std::make_unique<LineOverlay>(scene_);
    connect(form_, &SchemaFormModel::changed, this, &OperationToolModel::updatePointMarks);
}

OperationToolModel::~OperationToolModel()
{
    *alive_ = false;
    if (api::IApp* a = app(); a != nullptr && a->libraries() == stackSubscribed_ && stackListener_ != 0)
        stackSubscribed_->unsubscribe(stackListener_);
    if (commands_) {
        commands_->setUiHandler(QStringLiteral("cam.opTool."), {});
        commands_->setUiHandler(QStringLiteral("cam.operation.add"), {});
    }
    cancelJobs();
    holesJob_.cancel();
    if (doc::Document* d = liveDoc(); d != nullptr && active())
        d->cancelPreview();
    if (filterSaved_ && selection_)
        selection_->setFilterMask(savedFilter_);
    if (selection_)
        selection_->setToolHighlight({});
    unsubscribe();
}

OperationToolModel* OperationToolModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    UnitsObject* units = nullptr;
    SetupTreeModel* tree = nullptr;
    SetupToolModel* setupTool = nullptr;
    ToolpathDisplay* display = nullptr;
    ToolLibraryModel* tools = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
        tree = qml->singletonInstance<SetupTreeModel*>(uri, QStringLiteral("CamTreeModel"));
        setupTool = qml->singletonInstance<SetupToolModel*>(uri, QStringLiteral("CamSetup"));
        display = qml->singletonInstance<ToolpathDisplay*>(uri, QStringLiteral("CamToolpaths"));
        tools = qml->singletonInstance<ToolLibraryModel*>(uri, QStringLiteral("CamTools"));
    }
    auto* model = new OperationToolModel(scene, selection, session, context);
    model->setCommands(commands);
    model->setUnits(units);
    model->setTree(tree);
    model->setSetupTool(setupTool);
    model->setDisplay(display);
    model->setTools(tools);
    model->subscribe();
    return model; // owned by the engine
}

void OperationToolModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<OperationToolModel> self(this);
    const CommandCatalog::UiHandler handler{[self](const QString& id) {
                                                return self ? self->commandReason(id)
                                                            : trs("The operation panel is not available");
                                            },
                                            [self](const QString& id) { return self && self->runCommand(id); }};
    commands_->setUiHandler(QStringLiteral("cam.opTool."), handler);
    commands_->setUiHandler(QStringLiteral("cam.operation.add"), handler);
}

void OperationToolModel::setTools(ToolLibraryModel* tools)
{
    tools_ = tools;
}

void OperationToolModel::setUnits(UnitsObject* units)
{
    units_ = units;
    form_->setLengthUnit(inch() ? QStringLiteral("in") : QStringLiteral("mm"));
}

void OperationToolModel::setTree(SetupTreeModel* tree)
{
    if (tree_)
        disconnect(tree_, nullptr, this, nullptr);
    tree_ = tree;
    if (tree_)
        connect(tree_, &SetupTreeModel::operationEditRequested, this, [this](const QString& id) { edit(id); });
}

// ---------- document access ----------
api::IApp* OperationToolModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* OperationToolModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void OperationToolModel::subscribe()
{
    unsubscribe();
    subscribeLibrary();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<OperationToolModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->onDocumentChanged();
    });
    previewListener_ = doc_->subscribePreview([self, generation](doc::DocStatePtr state, doc::BuildResultPtr build) {
        if (self && self->docGeneration_ == generation && self->previewBusy_)
            self->onPreviewBuilt(std::move(state), std::move(build));
    });
}

void OperationToolModel::unsubscribe()
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

void OperationToolModel::subscribeLibrary()
{
    api::IApp* a = app();
    lib::LibraryStack* s = a != nullptr ? a->libraries() : nullptr;
    if (s == stackSubscribed_)
        return;
    // The old stack went with its app (a listener left in it is never called again); a new one is listened to.
    stackSubscribed_ = s;
    stackListener_ = 0;
    if (s != nullptr) {
        std::weak_ptr<bool> alive = alive_;
        stackListener_ = s->subscribe([this, alive] {
            if (auto live = alive.lock(); live && *live)
                onLibraryChanged();
        });
    }
}

// The library changed (its watch, a refresh, a save in the manager): the holders and materials on offer, and the chosen
// tool's feeds, follow. The picker's list follows by itself (CamTools listens too).
void OperationToolModel::onLibraryChanged()
{
    if (!active() || committing_)
        return;
    loadOptions();
    emit optionsChanged();
    emit toolChanged();
    updateFeeds();
    schedulePreview(); // a library tool or holder not embedded yet goes into the preview as the library has it now
}

const cam::Setup* OperationToolModel::setupOf(const doc::DocState& state) const
{
    return setup_ ? findSetup(state, *setup_) : nullptr;
}

void OperationToolModel::onDocumentChanged()
{
    if (!active() || committing_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr) {
        close();
        return;
    }
    const doc::DocStatePtr state = d->state();
    // The operation or its setup is gone (an undo of its add, a delete from the tree): the panel closes.
    if (setupOf(*state) == nullptr || (editing_ && (!state->cam || state->cam->operations.count(*id_) == 0))) {
        close();
        return;
    }
    if (state == previewBase_)
        return; // a rebuild of the same state finished
    loadOptions();
    emit optionsChanged();
    emit toolsChanged();
    previewBusy_ = false;
    schedulePreview();
}

// ---------- commands ----------
QString OperationToolModel::commandReason(const QString&) const
{
    api::IApp* a = app();
    if (a == nullptr)
        return trs("There is no session");
    api::OpenDocument* od = a->active();
    if (od == nullptr || !od->document)
        return trs("Open or import a part first");
    if (od->readOnly)
        return trs("The document is read-only");
    const doc::DocStatePtr state = od->document->state();
    if (!state->cam || state->cam->setups.empty())
        return trs("Add a setup first");
    if (a->operationTypes() == nullptr || a->operationTypes()->types().empty())
        return trs("No operation types are installed");
    return {};
}

bool OperationToolModel::runCommand(const QString& id)
{
    if (id == QLatin1String("cam.opTool.new") || id == QLatin1String("cam.operation.add"))
        return start();
    return false;
}

// ---------- open and close ----------
QString OperationToolModel::title() const
{
    if (!active())
        return {};
    if (!editing_)
        return trs("New operation");
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    if (state && state->cam) {
        if (const auto it = state->cam->operations.find(*id_); it != state->cam->operations.end())
            return trs("Edit %1").arg(qs(it->second.name));
    }
    return trs("Edit operation");
}

QString OperationToolModel::operationId() const
{
    return id_ ? idText(id_->value) : QString();
}

QVariantList OperationToolModel::types() const
{
    QVariantList out;
    api::IApp* a = app();
    if (a == nullptr)
        return out;
    // In the panel's fixed order (camui::orderedTypeNames), not the registry's (by name).
    std::map<QString, QString> titles;
    QStringList names;
    for (const api::cam_operation::TypeInfo& t : api::cam_operation::operationTypes(*a)) {
        titles[qs(t.name)] = qs(t.title);
        names << qs(t.name);
    }
    for (const QString& name : camui::orderedTypeNames(names))
        out.push_back(QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("title"), titles[name]}});
    return out;
}

QString OperationToolModel::typeTitle() const
{
    const QString t = form_->title();
    return t.isEmpty() ? type_ : t;
}

bool OperationToolModel::start()
{
    if (const QString reason = commandReason(QStringLiteral("cam.opTool.new")); !reason.isEmpty()) {
        if (context_)
            context_->showMessage(reason, true);
        return false;
    }
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return false;
    // The setup of the tree's current row, else the last setup.
    std::optional<SetupId> setup = tree_ ? tree_->currentSetup() : std::nullopt;
    const doc::DocStatePtr state = d->state();
    if (!setup || findSetup(*state, *setup) == nullptr)
        setup = state->cam->setups.back().id;
    return startFor(idText(setup->value));
}

bool OperationToolModel::startFor(const QString& setupId)
{
    if (const QString reason = commandReason(QStringLiteral("cam.opTool.new")); !reason.isEmpty()) {
        if (context_)
            context_->showMessage(reason, true);
        return false;
    }
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return false;
    const SetupId setup{Uuid::fromString(setupId.toStdString())};
    if (findSetup(*d->state(), setup) == nullptr)
        return false;
    if (context_ && context_->workspace() != kCamWorkspace)
        context_->setWorkspace(kCamWorkspace);
    // The type of the last operation added or changed with this panel in this session, else drill, else the first in
    // the panel's order (camui::defaultTypeName).
    QStringList names;
    for (const QVariant& v : types())
        names << v.toMap().value(QStringLiteral("name")).toString();
    open(setup, std::nullopt, camui::defaultTypeName(names, lastUsedType_));
    return active();
}

bool OperationToolModel::edit(const QString& operationId)
{
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return false;
    const OperationId id{Uuid::fromString(operationId.toStdString())};
    const doc::DocStatePtr state = d->state();
    const cam::Setup* setup = setupListing(*state, id);
    if (setup == nullptr || !state->cam || state->cam->operations.count(id) == 0)
        return false;
    if (context_ && context_->workspace() != kCamWorkspace)
        context_->setWorkspace(kCamWorkspace);
    open(setup->id, id, qs(state->cam->operations.at(id).type));
    return active();
}

void OperationToolModel::open(const SetupId& setup, const std::optional<OperationId>& existing, const QString& type)
{
    if (active())
        close();
    if (setupTool_ && setupTool_->active())
        setupTool_->cancel(); // one panel at a time
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return;
    const doc::DocStatePtr state = d->state();
    id_ = existing ? *existing : OperationId{Uuid::generate()};
    setup_ = setup;
    editing_ = existing.has_value();
    values_.clear();
    fieldErrors_.clear();
    picks_.clear();
    hoveredPick_ = -1;
    holes_.clear();
    holesReady_ = false;
    queuedPick_.reset();
    toolId_.clear();
    holderId_.clear();
    showAllTools_ = false;
    record_.reset();
    loadedRecord_.reset();
    materialId_.clear();
    materialSource_.clear();
    message_.clear();
    messageField_.clear();
    previewStatus_.clear();
    errors_.clear();
    warnings_.clear();
    generatedWarnings_.clear();
    progress_ = -1;
    committing_ = rejected_ = checked_ = false;
    previewBusy_ = previewPending_ = false;
    previewBase_.reset();
    previewPath_.reset();
    previewBackplot_.reset();
    ++pickSerial_;
    pickPending_ = false;
    exact_.clear();
    fullText_.clear();
    form_->setLengthUnit(inch() ? QStringLiteral("in") : QStringLiteral("mm"));
    form_->setExprFn(exprFn());
    if (units_) {
        QPointer<UnitsObject> units = units_;
        form_->setLengthFormat([units](double mm) { return units ? units->formatLength(mm) : QString(); });
    }
    applyType(type, false);
    subscribeLibrary();
    loadOptions();
    // The library folders may have changed since the watch last looked: read them again on a worker (the picker and
    // this panel follow when the stack's listeners are called).
    if (tools_)
        tools_->refresh();
    std::vector<QJsonObject> refs;
    if (editing_) {
        const cam::Operation& op = state->cam->operations.at(*id_);
        loadOperation(*state, op);
        for (const naming::TopoRef& r : op.geometry)
            refs.push_back(jsonObject(api::cam_setup::referenceJson(r)));
    } else {
        defaults(*state);
    }
    picking_ = geometry_.kind != QLatin1String("none") && refs.empty();
    pointPickKey_.clear();
    if (selection_) {
        savedFilter_ = selection_->filterMask();
        filterSaved_ = true;
        selection_->clear();
    }
    applyPickMode();
    loadHoles(std::move(refs));
    updateFeeds();
    emit activeChanged();
    emit typeChanged();
    emit valuesChanged();
    emit toolChanged();
    emit toolsChanged();
    emit optionsChanged();
    emit picksChanged();
    emit statusChanged();
    schedulePreview();
}

void OperationToolModel::close()
{
    const bool was = active();
    previewTimer_.stop();
    ++previewSerial_;
    ++pickSerial_;
    pickPending_ = false;
    cancelJobs();
    holesJob_.cancel();
    holesJob_ = {};
    if (doc::Document* d = liveDoc(); d != nullptr && was)
        d->cancelPreview();
    if (display_)
        display_->clearPreview();
    if (filterSaved_ && selection_)
        selection_->setFilterMask(savedFilter_);
    filterSaved_ = false;
    id_.reset();
    setup_.reset();
    editing_ = false;
    picks_.clear();
    hoveredPick_ = -1;
    holes_.clear();
    picking_ = false;
    pointPickKey_.clear();
    frame_.reset();
    frameError_.clear();
    updatePointMarks();
    if (selection_)
        selection_->setToolHighlight({});
    values_.clear();
    message_.clear();
    messageField_.clear();
    previewStatus_.clear();
    errors_.clear();
    warnings_.clear();
    generatedWarnings_.clear();
    progress_ = -1;
    committing_ = false;
    previewBusy_ = previewPending_ = false;
    previewBase_.reset();
    previewPath_.reset();
    previewBackplot_.reset();
    if (was) {
        emit activeChanged();
        emit valuesChanged();
        emit picksChanged();
        emit statusChanged();
        emit feedsChanged();
    }
}

void OperationToolModel::cancel()
{
    close();
}

void OperationToolModel::cancelJobs()
{
    for (auto* job : {&validateJob_}) {
        if (job->valid() && !job->done())
            job->cancel();
        *job = {};
    }
    if (previewJob_.valid() && !previewJob_.done() && previewJob_.cancel())
        ++previewsCancelled_;
    previewJob_ = {};
    if (backplotJob_.valid() && !backplotJob_.done())
        backplotJob_.cancel();
    backplotJob_ = {};
}

// ---------- type, defaults, loading ----------
void OperationToolModel::applyType(const QString& type, bool keepParams)
{
    type_ = type;
    std::string schemaJson;
    if (api::IApp* a = app()) {
        for (const api::cam_operation::TypeInfo& t : api::cam_operation::operationTypes(*a)) {
            if (qs(t.name) == type)
                schemaJson = t.schemaJson;
        }
    }
    const QJsonObject before = keepParams ? form_->params() : QJsonObject();
    form_->setSchemaJson(qs(schemaJson));
    if (keepParams)
        form_->load(before);
    schema_ = form_->schema().top;
    geometry_ = camui::geometrySpec(schema_);
    heights_ = camui::heightsUsed(schema_);
    cutting_ = camui::cuttingUsed(schema_);
}

void OperationToolModel::setType(const QString& type)
{
    if (!active() || editing_ || type == type_)
        return;
    applyType(type, false);
    // Geometry of another kind does not carry over; heights and feeds do.
    picks_.clear();
    picking_ = geometry_.kind != QLatin1String("none");
    pointPickKey_.clear();
    applyPickMode();
    loadHoles({});
    doc::Document* d = liveDoc();
    if (d != nullptr) {
        // The name follows the type while it is the default one.
        const QString name = values_.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || name.endsWith(QStringLiteral(" %1").arg(nameSerial_)))
            values_[QStringLiteral("name")] = QStringLiteral("%1 %2").arg(typeTitle()).arg(nameSerial_);
    }
    emit typeChanged();
    emit valuesChanged();
    emit toolsChanged();
    emit picksChanged();
    updateFeeds();
    schedulePreview();
}

bool OperationToolModel::isExactText(const QString& key, const QString& text) const
{
    const auto it = exact_.find(key);
    if (it != exact_.end() && it->second.first == text)
        return true;
    const auto full = fullText_.find(key);
    return full != fullText_.end() && full->second == text;
}

void OperationToolModel::setStoredLength(const QString& key, double mm)
{
    values_[key] = lengthText(mm);
    exact_[key] = {values_.value(key).toString(), mm};
    fullText_[key] = camui::numberText(mm, camui::Unit::Length, inch() ? QStringLiteral("in") : QStringLiteral("mm"));
}

QVariantMap OperationToolModel::fullValues() const
{
    QVariantMap out;
    for (const auto& [key, text] : fullText_) {
        // Only while the field still shows the stored value (not once it is typed over).
        if (const auto it = exact_.find(key); it != exact_.end() && it->second.first == values_.value(key).toString())
            out.insert(key, text);
    }
    return out;
}

QString OperationToolModel::lengthText(double mm) const
{
    // At rest a length shows in the app's one display format (Units); being edited, in full (fullValues).
    if (units_)
        return units_->formatLength(mm);
    return camui::numberText(mm, camui::Unit::Length, inch() ? QStringLiteral("in") : QStringLiteral("mm"));
}

void OperationToolModel::defaults(const doc::DocState& state)
{
    // A name not used yet: "<type title> N".
    std::set<QString> names;
    if (state.cam) {
        for (const auto& [id, op] : state.cam->operations)
            names.insert(qs(op.name));
    }
    int n = 1;
    while (names.count(QStringLiteral("%1 %2").arg(typeTitle()).arg(n)) != 0)
        ++n;
    nameSerial_ = n;
    values_[QStringLiteral("name")] = QStringLiteral("%1 %2").arg(typeTitle()).arg(n);
    const bool in = inch();
    const std::pair<const char*, double> heights[] = {{"clearance", in ? 15.24 : 15.0},
                                                      {"retract", in ? 5.08 : 5.0},
                                                      {"feedPlane", in ? 2.54 : 2.0},
                                                      {"top", 0.0},
                                                      {"bottom", 0.0}};
    for (const auto& [key, mm] : heights)
        setStoredLength(QLatin1String(key), mm);
    for (const char* key : {"rpm", "feedCut", "feedPlunge", "feedRamp", "feedLeadIn", "feedLeadOut"})
        values_[QLatin1String(key)] = QString();
    values_[QStringLiteral("coolant")] = QStringLiteral("flood");
    values_[QStringLiteral("clockwise")] = true;
    values_[QStringLiteral("stickOut")] = QString();
    values_[QStringLiteral("toolNumber")] = QString();
    form_->load({});
}

void OperationToolModel::loadOperation(const doc::DocState& state, const cam::Operation& op)
{
    (void)state;
    values_[QStringLiteral("name")] = qs(op.name);
    const std::pair<const char*, double> heights[] = {{"clearance", op.heights.clearance},
                                                      {"retract", op.heights.retract},
                                                      {"feedPlane", op.heights.feedPlane},
                                                      {"top", op.heights.top},
                                                      {"bottom", op.heights.bottom}};
    for (const auto& [key, mm] : heights)
        setStoredLength(QLatin1String(key), mm);
    const bool in = inch();
    values_[QStringLiteral("rpm")] = cText(op.cutting.spindleRpm);
    const std::pair<const char*, double> feeds[] = {{"feedCut", op.cutting.feedCut},
                                                    {"feedPlunge", op.cutting.feedPlunge},
                                                    {"feedRamp", op.cutting.feedRamp},
                                                    {"feedLeadIn", op.cutting.feedLeadIn},
                                                    {"feedLeadOut", op.cutting.feedLeadOut}};
    for (const auto& [key, mm] : feeds)
        values_[QLatin1String(key)] = camui::feedText(mm, in);
    static const char* const coolants[] = {"off", "flood", "mist", "throughTool", "air"};
    values_[QStringLiteral("coolant")] = QLatin1String(coolants[static_cast<int>(op.cutting.coolant)]);
    values_[QStringLiteral("clockwise")] = op.cutting.clockwise;
    setStoredLength(QStringLiteral("stickOut"), op.tool.stickOut);
    values_[QStringLiteral("toolNumber")] = QString::number(op.tool.toolNumber);
    toolId_ = idText(op.tool.tool.value);
    holderId_ = op.tool.holder.value == Uuid{} ? QString() : idText(op.tool.holder.value);
    form_->load(jsonObject(op.paramsJson));
    record_ = op.calculated;
    loadedRecord_ = op.calculated;
    if (op.calculated && op.calculated->material.value != Uuid{}) {
        materialId_ = idText(op.calculated->material.value);
        materialSource_ = QStringLiteral("chosen");
    }
    // What is shown is what is stored: these texts read back as the stored values exactly (see length()).
    for (const auto& [key, mm] : feeds)
        exact_[QLatin1String(key)] = {values_.value(QLatin1String(key)).toString(), mm};
    exact_[QStringLiteral("rpm")] = {values_.value(QStringLiteral("rpm")).toString(), op.cutting.spindleRpm};
}

// The holders and materials on offer (the document's and the library's); the tools are the picker's (CamTools).
void OperationToolModel::loadOptions()
{
    holderList_.clear();
    materialItems_.clear();
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    api::IApp* a = app();
    lib::LibraryStack* stack = a ? a->libraries() : nullptr;
    holderList_.push_back(QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("name"), trs("None")}});
    std::set<QString> holders;
    if (state && state->cam) {
        for (const auto& [id, h] : state->cam->holders) {
            if (holders.insert(idText(id.value)).second)
                holderList_.push_back(
                    QVariantMap{{QStringLiteral("id"), idText(id.value)}, {QStringLiteral("name"), qs(h.item.name)}});
        }
    }
    if (stack != nullptr) {
        for (const lib::Holder& h : stack->holders()) {
            const QString id = idText(h.id.value);
            if (holders.insert(id).second)
                holderList_.push_back(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), qs(h.name)}});
        }
    }
    std::set<QString> materials;
    if (state && state->cam) {
        for (const auto& [id, m] : state->cam->materials) {
            if (materials.insert(idText(id.value)).second)
                materialItems_.push_back(m.item);
        }
    }
    if (stack != nullptr) {
        for (const lib::Material& m : stack->materials()) {
            if (materials.insert(idText(m.id.value)).second)
                materialItems_.push_back(m);
        }
    }
    // The material: the setup's, else the document's, else one chosen here earlier.
    if (state && materialSource_ != QLatin1String("chosen")) {
        materialId_.clear();
        materialSource_.clear();
        const cam::Setup* s = setupOf(*state);
        if (s != nullptr && s->material.value != Uuid{}) {
            materialId_ = idText(s->material.value);
            materialSource_ = QStringLiteral("setup");
        } else if (state->settings.material.value != Uuid{}) {
            materialId_ = idText(state->settings.material.value);
            materialSource_ = QStringLiteral("document");
        }
    }
}

// The holes of the setup (for a hole picker), and the names of the picks an edited operation has.
void OperationToolModel::loadHoles(std::vector<QJsonObject> refs)
{
    holesJob_.cancel();
    holesJob_ = {};
    holes_.clear();
    holesReady_ = false;
    frame_.reset();
    frameError_.clear();
    updatePointMarks();
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (d == nullptr || jobs == nullptr || !setup_)
        return;
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    if (!build)
        return;
    const SetupId setup = *setup_;
    const bool holes = geometry_.accepts(QStringLiteral("hole"));
    // Points parameters snap to hole centres and are placed in the setup's frame.
    const bool points = !form_->pointKeys().isEmpty();
    std::vector<std::string> refTexts;
    for (const QJsonObject& r : refs)
        refTexts.push_back(jsonText(r));
    using Out = PickSupport;
    // The located element of each reference (its body, element and kind), and for holes the hole it is in.
    holesJob_ = jobs->submit<std::shared_ptr<const Out>>(
        JobInfo{"Holes of the setup", JobPriority::Interactive, false},
        [state, build, setup, holes, points, refTexts](JobContext& ctx) -> Result<std::shared_ptr<const Out>> {
            auto out = std::make_shared<Out>();
            if (points) {
                auto frame = api::cam_setup::previewSetup(state, build, setup, ctx);
                if (frame)
                    out->frame = frame->resolved;
                else if (frame.error().code == ErrorCode::Cancelled)
                    return tl::unexpected(frame.error());
                else
                    out->frameError = qs(frame.error().message);
            }
            if (holes || (points && out->frame)) {
                auto found = api::cam_operation::holesOf(state, build, setup, ctx);
                if (found)
                    out->holes = std::move(*found);
                else if (holes || found.error().code == ErrorCode::Cancelled)
                    return tl::unexpected(found.error());
                // (for points only, no holes: no snapping to their centres)
            }
            for (const std::string& r : refTexts) {
                const auto at = api::feature_edit::locate(*build, r);
                int index = -1;
                kernel::SubKind kind = kernel::SubKind::Face;
                if (at && !at->elements.empty()) {
                    kind = at->elements.front().kind;
                    if (holes) {
                        for (std::size_t i = 0; i < out->holes.size() && index < 0; ++i) {
                            const auto& h = out->holes[i];
                            if (h.body == at->body &&
                                std::binary_search(h.faces.begin(), h.faces.end(), at->elements.front().index))
                                index = static_cast<int>(i);
                        }
                    } else {
                        index = static_cast<int>(at->elements.front().index);
                    }
                }
                out->located.emplace_back(index, kind);
            }
            return std::shared_ptr<const Out>(std::move(out));
        });
    QPointer<OperationToolModel> self(this);
    const std::uint64_t serial = pickSerial_;
    holesJob_.then([self, serial, refs, holes](Result<std::shared_ptr<const Out>> r) {
        if (!self || !self->active() || serial != self->pickSerial_)
            return;
        self->holesJob_ = {};
        self->holesReady_ = true;
        if (!r) {
            if (r.error().code != ErrorCode::Cancelled)
                self->setStatus(self->previewStatus_, qs(r.error().message), QStringLiteral("pick"));
            return;
        }
        self->holes_ = (*r)->holes;
        self->frame_ = (*r)->frame;
        self->frameError_ = (*r)->frameError;
        self->updatePointMarks();
        // The edited operation's picks, named.
        for (std::size_t i = 0; i < refs.size() && i < (*r)->located.size(); ++i) {
            Pick p;
            p.ref = refs[i];
            p.body = refs[i].value(QStringLiteral("body")).toString();
            const auto [index, kind] = (*r)->located[i];
            p.resolved = index >= 0;
            if (holes && index >= 0) {
                const auto& h = self->holes_[static_cast<std::size_t>(index)];
                p.element = h.boreFace;
                p.kind = kernel::SubKind::Face;
                p.text = self->holeText(h);
            } else {
                p.element = index >= 0 ? static_cast<std::uint32_t>(index) : 0;
                // Where it does not resolve, the kind the reference says it is.
                const bool edge = index >= 0
                                      ? kind == kernel::SubKind::Edge
                                      : refs[i].value(QStringLiteral("kind")).toString() == QLatin1String("edge");
                p.kind = edge ? kernel::SubKind::Edge : kernel::SubKind::Face;
                p.text = index >= 0 ? trs("%1 %2").arg(edge ? trs("Edge") : trs("Face")).arg(index)
                                    : trs("A reference that does not resolve");
            }
            self->picks_.push_back(std::move(p));
        }
        emit self->picksChanged();
        if (self->queuedPick_) {
            const auto [body, sub] = *self->queuedPick_;
            self->queuedPick_.reset();
            self->pickPending_ = false;
            self->takePick(body, sub);
        }
        emit self->statusChanged();
    });
}

// ---------- values ----------
void OperationToolModel::setValue(const QString& key, const QVariant& value)
{
    if (!active() || values_.value(key) == value)
        return;
    values_[key] = value;
    fieldErrors_.erase(key);
    emit valuesChanged();
    if (isCuttingKey(key))
        updateFeeds();
    schedulePreview();
}

QVariantMap OperationToolModel::fieldMessages() const
{
    QVariantMap out;
    if (!active())
        return out;
    // What does not read, then what the API said.
    for (const QString& key : heights_) {
        QString error;
        if (!length(key, &error))
            out.insert(key, error);
    }
    const auto check = [&](const QString& key, auto reader) {
        if (!cutting_.contains(key))
            return;
        QString error;
        if (!reader(values_.value(key).toString(), &error))
            out.insert(key, error);
    };
    const camui::ExprFn expr = exprFn();
    const bool in = inch();
    const auto exact = [&](const QString& key, const QString& text) -> std::optional<double> {
        const auto it = exact_.find(key);
        return it != exact_.end() && isExactText(key, text) ? std::optional<double>(it->second.second) : std::nullopt;
    };
    check(QStringLiteral("rpm"), [&](const QString& t, QString* e) {
        return exact(QStringLiteral("rpm"), t) ? std::optional<double>(1) : camui::readRpm(t, expr, e);
    });
    for (const char* key : {"feedCut", "feedPlunge", "feedRamp", "feedLeadIn", "feedLeadOut"}) {
        const QString k = QLatin1String(key);
        check(k, [&](const QString& t, QString* e) {
            if (exact(k, t))
                return std::optional<double>(1);
            if (k != QLatin1String("feedCut") && t.trimmed().isEmpty())
                return std::optional<double>(1); // the cut feed then
            return camui::readFeed(t, in, expr, e);
        });
    }
    for (const auto& [key, m] : fieldErrors_) {
        if (!out.contains(key))
            out.insert(key, m);
    }
    return out;
}

QVariantList OperationToolModel::heights() const
{
    QVariantList out;
    for (const QString& key : heights_) {
        // What the type measures it from (x-heightsUsed), shown under the field and in its tooltip.
        const QString from =
            camui::heightFromText(camui::heightFrom(schema_, key),
                                  geometry_.kinds.isEmpty() ? geometry_.kind : geometry_.kinds.join(QLatin1Char(',')));
        out.push_back(QVariantMap{
            {QStringLiteral("key"), key},
            {QStringLiteral("label"), heightLabel(key)},
            {QStringLiteral("hint"), from.isEmpty() ? heightHint(key) : trs("%1, %2").arg(heightHint(key), from)},
            {QStringLiteral("from"), from}});
    }
    return out;
}

bool OperationToolModel::inch() const
{
    return units_ && units_->system() == UnitSystem::Inch;
}

camui::ExprFn OperationToolModel::exprFn() const
{
    QPointer<const OperationToolModel> self(this);
    return [self](const QString& expression, int kind, QString* error) -> std::optional<double> {
        doc::Document* d = self ? self->liveDoc() : nullptr;
        if (d == nullptr)
            return camui::plainExprFn()(expression, kind, error);
        const doc::Parameter::Kind k = kind == 0   ? doc::Parameter::Kind::Length
                                       : kind == 1 ? doc::Parameter::Kind::Angle
                                                   : doc::Parameter::Kind::Unitless;
        auto v = api::cam_operation::evaluate(*d->state(), expression.toStdString(), k);
        if (!v) {
            *error = qs(v.error().message);
            return std::nullopt;
        }
        return *v;
    };
}

std::optional<double> OperationToolModel::length(const QString& key, QString* error) const
{
    const QString text = values_.value(key).toString();
    if (const auto it = exact_.find(key); it != exact_.end() && isExactText(key, text))
        return it->second.second;
    return camui::readNumber(text, camui::Unit::Length, inch() ? QStringLiteral("in") : QStringLiteral("mm"), exprFn(),
                             error);
}

// ---------- tools ----------
std::optional<lib::Tool> OperationToolModel::chosenTool() const
{
    if (toolId_.isEmpty())
        return std::nullopt;
    const ToolId id{Uuid::fromString(toolId_.toStdString())};
    if (doc::Document* d = liveDoc()) {
        const doc::DocStatePtr state = d->state();
        if (state->cam) {
            if (const auto it = state->cam->tools.find(id); it != state->cam->tools.end())
                return it->second.item;
        }
    }
    api::IApp* a = app();
    lib::LibraryStack* stack = a ? a->libraries() : nullptr;
    if (const lib::Tool* t = stack ? stack->tool(id) : nullptr)
        return *t;
    return std::nullopt;
}

QString OperationToolModel::toolText() const
{
    const std::optional<lib::Tool> t = chosenTool();
    if (!t)
        return toolId_.isEmpty() ? trs("No tool chosen") : trs("A tool that is not in the library");
    return qs(t->name);
}

QStringList OperationToolModel::toolTypes() const
{
    return active() ? camui::toolTypesFor(schema_, form_->params()) : QStringList();
}

void OperationToolModel::setShowAllTools(bool all)
{
    if (all == showAllTools_)
        return;
    showAllTools_ = all;
    emit toolsChanged();
}

QVariantList OperationToolModel::holders() const
{
    return holderList_;
}

void OperationToolModel::chooseTool(const QString& toolId)
{
    if (!active() || toolId == toolId_)
        return;
    toolId_ = toolId;
    fieldErrors_.erase(QStringLiteral("tool"));
    // The new tool's own stick-out and number, unless typed.
    values_[QStringLiteral("stickOut")] = QString();
    values_[QStringLiteral("toolNumber")] = QString();
    emit toolChanged();
    emit toolsChanged();
    emit valuesChanged();
    updateFeeds();
    schedulePreview();
}

void OperationToolModel::chooseHolder(const QString& holderId)
{
    if (!active() || holderId == holderId_)
        return;
    holderId_ = holderId;
    emit toolChanged();
    schedulePreview();
}

QVariantList OperationToolModel::materials() const
{
    QVariantList out;
    out.push_back(QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("name"), trs("None")}});
    for (const lib::Material& m : materialItems_)
        out.push_back(QVariantMap{{QStringLiteral("id"), idText(m.id.value)}, {QStringLiteral("name"), qs(m.name)}});
    return out;
}

void OperationToolModel::setMaterial(const QString& materialId)
{
    if (!active() || materialId == materialId_)
        return;
    materialId_ = materialId;
    materialSource_ = materialId.isEmpty() ? QString() : QStringLiteral("chosen");
    updateFeeds();
}

// ---------- feeds ----------
void OperationToolModel::updateFeeds()
{
    calculated_.clear();
    feeds_.reset();
    if (!active()) {
        emit feedsChanged();
        return;
    }
    const std::optional<lib::Tool> tool = chosenTool();
    const lib::Material* material = nullptr;
    for (const lib::Material& m : materialItems_) {
        if (idText(m.id.value) == materialId_)
            material = &m;
    }
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    lib::MachineDef machine;
    if (state && state->cam) {
        if (const cam::Setup* s = setupOf(*state)) {
            if (const auto m = state->cam->machines.find(s->machine); m != state->cam->machines.end())
                machine = m->second.item;
        }
    }
    const bool in = inch();
    calculated_.insert(QStringLiteral("available"), false);
    if (!tool || material == nullptr) {
        calculated_.insert(QStringLiteral("error"), !tool ? trs("Choose a tool to calculate the feeds")
                                                          : trs("Choose a material to calculate the feeds"));
    } else {
        api::cam_operation::FeedsRequest req;
        req.tool = *tool;
        req.material = *material;
        req.machine = machine;
        req.opType = type_.toStdString();
        auto r = api::cam_operation::computeFeeds(req);
        if (!r) {
            calculated_.insert(QStringLiteral("error"), qs(r.error().message));
        } else {
            feeds_ = *r;
            const cam::FeedsResult& f = *r;
            calculated_.insert(QStringLiteral("available"), true);
            calculated_.insert(QStringLiteral("rpm"), camui::rpmText(f.rpm));
            calculated_.insert(QStringLiteral("feed"), camui::feedText(f.feedMmMin, in));
            // A tap advances by its pitch per revolution; every other tool takes a chip load per tooth.
            const bool tapping = f.mode == cam::FeedsMode::Tapping;
            calculated_.insert(QStringLiteral("chipLoadLabel"), tapping ? trs("Pitch") : trs("Chip load"));
            calculated_.insert(QStringLiteral("chipLoad"), tapping ? camui::feedPerRevText(f.feedPerRevMm, in)
                                                                   : camui::chipLoadText(f.chipLoadMm, in));
            calculated_.insert(QStringLiteral("surfaceSpeed"), camui::surfaceSpeedText(f.surfaceSpeedMMin, in));
            calculated_.insert(QStringLiteral("actualSurfaceSpeed"),
                               camui::surfaceSpeedText(f.actualSurfaceSpeedMMin, in));
            calculated_.insert(QStringLiteral("power"), f.powerKw > 0 ? camui::powerText(f.powerKw) : trs("not known"));
            calculated_.insert(QStringLiteral("opClass"), qs(f.opClass));
            calculated_.insert(QStringLiteral("mode"), f.mode == cam::FeedsMode::Tapping    ? trs("Tapping")
                                                       : f.mode == cam::FeedsMode::Drilling ? trs("Drilling")
                                                                                            : trs("Milling"));
            QVariantList limits;
            QStringList limited;
            const std::pair<const char*, const cam::FeedsLimit*> named[] = {
                {QT_TRANSLATE_NOOP("ray::ui::OperationToolModel", "Maximum rpm"), &f.limits.maxRpm},
                {QT_TRANSLATE_NOOP("ray::ui::OperationToolModel", "Minimum rpm"), &f.limits.minRpm},
                {QT_TRANSLATE_NOOP("ray::ui::OperationToolModel", "Maximum feed"), &f.limits.maxFeed},
                {QT_TRANSLATE_NOOP("ray::ui::OperationToolModel", "Power"), &f.limits.power},
                {QT_TRANSLATE_NOOP("ray::ui::OperationToolModel", "Torque"), &f.limits.torque}};
            for (const auto& [name, limit] : named) {
                limits.push_back(
                    QVariantMap{{QStringLiteral("name"), trs(name)},
                                {QStringLiteral("state"), camui::limitStateText(limit->state)},
                                {QStringLiteral("applied"), limit->state == cam::LimitState::Applied ||
                                                                limit->state == cam::LimitState::Exceeded}});
                if (limit->state == cam::LimitState::Applied || limit->state == cam::LimitState::Exceeded)
                    limited << trs(name);
            }
            calculated_.insert(QStringLiteral("limits"), limits);
            calculated_.insert(QStringLiteral("limited"),
                               limited.isEmpty() ? trs("No machine limit applied")
                                                 : trs("Limited by: %1").arg(limited.join(QStringLiteral(", "))));
            QStringList notes;
            for (const std::string& n : f.notes)
                notes << qs(n);
            calculated_.insert(QStringLiteral("notes"), notes);
        }
    }
    // Calculated or the user's own?
    QString state2 = QStringLiteral("none");
    QString error;
    const camui::ExprFn expr = exprFn();
    const auto value = [&](const char* key, bool feed) -> std::optional<double> {
        const QString k = QLatin1String(key);
        const QString text = values_.value(k).toString();
        if (const auto it = exact_.find(k); it != exact_.end() && isExactText(k, text))
            return it->second.second;
        QString e;
        return feed ? camui::readFeed(text, in, expr, &e) : camui::readRpm(text, expr, &e);
    };
    const auto rpm = value("rpm", false);
    const auto cut = value("feedCut", true);
    if (rpm || cut) {
        state2 = QStringLiteral("user");
        if (record_ && rpm && cut &&
            camui::matchesRecord(*record_, *rpm, *cut, value("feedPlunge", true).value_or(*cut),
                                 value("feedRamp", true).value_or(*cut), cutting_)) {
            const bool sameInputs =
                idText(record_->tool.value) == toolId_ &&
                (record_->material.value == Uuid{} ? QString() : idText(record_->material.value)) == materialId_;
            state2 = sameInputs || !feeds_ ? QStringLiteral("calculated") : QStringLiteral("offer");
        }
    }
    feedsState_ = state2;
    emit feedsChanged();
}

QString OperationToolModel::feedsNote() const
{
    if (feedsState_ == QLatin1String("calculated"))
        return trs("These are the calculated values");
    if (feedsState_ == QLatin1String("offer"))
        return trs("The tool or material changed since these were calculated");
    if (feedsState_ == QLatin1String("user"))
        return trs("These are your own values, not the calculated ones");
    return trs("Type the speed and feeds, or use the calculated ones");
}

void OperationToolModel::useCalculated()
{
    if (!active() || !feeds_)
        return;
    const cam::FeedsResult& f = *feeds_;
    const double cut = f.feedMmMin;
    // computeFeeds has no rule for these: fractions of the cut feed, which the panel says (docs/operation_types.md).
    const double plunge = cut * camui::kPlungeFraction;
    const double ramp = cut * camui::kRampFraction;
    const bool in = inch();
    const std::pair<const char*, double> written[] = {
        {"feedCut", cut}, {"feedPlunge", plunge}, {"feedRamp", ramp}, {"feedLeadIn", cut}, {"feedLeadOut", cut}};
    values_[QStringLiteral("rpm")] = cText(f.rpm);
    exact_[QStringLiteral("rpm")] = {values_.value(QStringLiteral("rpm")).toString(), f.rpm};
    for (const auto& [key, mm] : written) {
        values_[QLatin1String(key)] = camui::feedText(mm, in);
        exact_[QLatin1String(key)] = {values_.value(QLatin1String(key)).toString(), mm};
        fieldErrors_.erase(QLatin1String(key));
    }
    cam::CalculatedFeeds r;
    r.tool = ToolId{Uuid::fromString(toolId_.toStdString())};
    r.material = MaterialId{materialId_.isEmpty() ? Uuid{} : Uuid::fromString(materialId_.toStdString())};
    if (doc::Document* d = liveDoc()) {
        if (const cam::Setup* s = setupOf(*d->state()))
            r.machine = s->machine;
    }
    r.opClass = f.opClass;
    r.spindleRpm = f.rpm;
    r.feedCut = cut;
    r.feedPlunge = plunge;
    r.feedRamp = ramp;
    record_ = r;
    emit valuesChanged();
    updateFeeds();
    schedulePreview();
}

// ---------- geometry ----------
QString OperationToolModel::geometryPrompt() const
{
    QString prompt = geometry_.description;
    if (prompt.isEmpty()) {
        if (geometry_.accepts(QStringLiteral("hole")))
            prompt = trs("Click a face of each hole");
        else if (geometry_.several())
            prompt = trs("Click the edges or faces to machine");
        else if (geometry_.accepts(QStringLiteral("edge")))
            prompt = trs("Click the edges to machine");
        else if (geometry_.accepts(QStringLiteral("face")))
            prompt = trs("Click the faces to machine");
    }
    // What the schema asks that the panel cannot do is said, not dropped.
    if (!geometry_.problem.isEmpty())
        prompt = prompt.isEmpty() ? geometry_.problem : trs("%1 (%2)").arg(prompt, geometry_.problem);
    return prompt;
}

QString OperationToolModel::pickKind(const Pick& p) const
{
    if (geometry_.accepts(QStringLiteral("hole")))
        return QStringLiteral("hole");
    return p.kind == kernel::SubKind::Edge ? QStringLiteral("edge") : QStringLiteral("face");
}

QVariantList OperationToolModel::picks() const
{
    // Grouped by kind in x-geometry's order when the type takes several kinds (each group in pick order), with a
    // heading on its first row; else in pick order.
    QVariantList out;
    const QStringList order = geometry_.several() ? geometry_.kinds : QStringList{QString()};
    for (const QString& kind : order) {
        bool first = true;
        for (std::size_t i = 0; i < picks_.size(); ++i) {
            const QString k = pickKind(picks_[i]);
            if (!kind.isEmpty() && k != kind)
                continue;
            QString heading;
            if (!kind.isEmpty() && first)
                heading = kind == QLatin1String("edge") ? trs("Edges") : trs("Faces");
            first = false;
            out.push_back(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                                      {QStringLiteral("text"), picks_[i].text},
                                      {QStringLiteral("kind"), k},
                                      {QStringLiteral("heading"), heading}});
        }
    }
    return out;
}

QString OperationToolModel::pickCountText() const
{
    if (!active() || geometry_.kinds.isEmpty())
        return {};
    const auto countOf = [&](const QString& kind) {
        return static_cast<int>(
            std::count_if(picks_.begin(), picks_.end(), [&](const Pick& p) { return pickKind(p) == kind; }));
    };
    const auto words = [](const QString& kind, int n) {
        if (kind == QLatin1String("hole"))
            return n == 1 ? trs("1 hole") : trs("%1 holes").arg(n);
        if (kind == QLatin1String("edge"))
            return n == 1 ? trs("1 edge") : trs("%1 edges").arg(n);
        return n == 1 ? trs("1 face") : trs("%1 faces").arg(n);
    };
    if (picks_.empty()) {
        if (geometry_.several())
            return trs("No edges or faces picked");
        if (geometry_.kind == QLatin1String("hole"))
            return trs("No holes picked");
        return geometry_.kind == QLatin1String("edge") ? trs("No edges picked") : trs("No faces picked");
    }
    // By kind, in x-geometry's order: "2 edges, 1 face".
    QStringList parts;
    for (const QString& kind : geometry_.kinds) {
        if (const int n = countOf(kind); n > 0)
            parts << words(kind, n);
    }
    return parts.join(QStringLiteral(", "));
}

void OperationToolModel::setHoveredPick(int index)
{
    if (index < 0 || index >= static_cast<int>(picks_.size()))
        index = -1;
    if (index == hoveredPick_)
        return;
    hoveredPick_ = index;
    emit picksChanged();
}

std::vector<render::PickHit> OperationToolModel::pickHits(int index) const
{
    std::vector<render::PickHit> out;
    if (!scene_ || index < 0 || index >= static_cast<int>(picks_.size()))
        return out;
    const Pick& p = picks_[static_cast<std::size_t>(index)];
    if (!p.resolved)
        return out;
    render::ItemId item = 0;
    BodyId bodyId;
    for (int b = 0; b < scene_->bodyCount() && item == 0; ++b) {
        const api::viewer::Body* body = scene_->body(b);
        if (body != nullptr && idText(body->id.value) == p.body) {
            item = scene_->itemForBody(b);
            bodyId = body->id;
        }
    }
    if (item == 0)
        return out;
    const auto hit = [&](kernel::SubKind kind, std::uint32_t sub) {
        out.push_back(render::PickHit{item, kind, sub, Vec3{}, 0});
    };
    if (geometry_.accepts(QStringLiteral("hole"))) {
        // Every face of the hole (bore, countersink, floor), its bore first; the bore alone until the holes are found.
        hit(kernel::SubKind::Face, p.element);
        const auto h = std::find_if(holes_.begin(), holes_.end(), [&](const api::cam_operation::Hole& x) {
            return x.body == bodyId && x.boreFace == p.element;
        });
        if (h != holes_.end()) {
            for (const std::uint32_t f : h->faces) {
                if (f != p.element)
                    hit(kernel::SubKind::Face, f);
            }
        }
        return out;
    }
    hit(p.kind, p.element);
    return out;
}

void OperationToolModel::updatePickHighlight()
{
    if (!selection_)
        return;
    if (!active()) {
        selection_->setToolHighlight({});
        return;
    }
    if (hoveredPick_ >= static_cast<int>(picks_.size()))
        hoveredPick_ = -1;
    std::vector<render::PickHit> hits;
    for (int i = 0; i < static_cast<int>(picks_.size()); ++i) {
        const std::vector<render::PickHit> h = pickHits(i);
        hits.insert(hits.end(), h.begin(), h.end());
    }
    std::optional<render::PickHit> emphasis;
    if (hoveredPick_ >= 0) {
        const std::vector<render::PickHit> h = pickHits(hoveredPick_);
        if (!h.empty())
            emphasis = h.front(); // a hole's bore, the face or edge itself
    }
    selection_->setToolHighlight(std::move(hits), emphasis);
}

QString OperationToolModel::holeText(const api::cam_operation::Hole& h) const
{
    const bool in = inch();
    const auto len = [&](double mm) {
        return in ? QLocale::c().toString(mm / 25.4, 'f', 4) + QStringLiteral(" in")
                  : QLocale::c().toString(mm, 'g', 6) + QStringLiteral(" mm");
    };
    // The position in the display unit too (the WCS values are mm).
    const auto coord = [&](double mm) {
        return in ? QLocale::c().toString(mm / 25.4, 'f', 4) : QLocale::c().toString(mm, 'g', 6);
    };
    const QString at = trs("X%1 Y%2").arg(coord(h.x), coord(h.y));
    const QString what = !h.threadSpec.empty() ? qs(h.threadSpec) : trs("⌀%1").arg(len(h.diameter));
    return h.through ? trs("%1 through at %2").arg(what, at) : trs("%1, %2 deep, at %3").arg(what, len(h.depth), at);
}

void OperationToolModel::setPicking(bool on)
{
    if (!active() || geometry_.kinds.isEmpty() || on == picking_)
        return;
    picking_ = on;
    if (on)
        pointPickKey_.clear(); // one kind of click at a time
    ++pickSerial_;
    pickPending_ = false;
    if (selection_)
        selection_->clear();
    applyPickMode();
    emit picksChanged();
    emit statusChanged();
}

void OperationToolModel::applyPickMode()
{
    if (!selection_ || !active())
        return;
    using render::PickFilter;
    std::uint8_t mask = filterSaved_ ? savedFilter_ : selection_->filterMask();
    if (!pointPickKey_.isEmpty()) {
        // A point: on a face (a hole's centre, or where it is clicked) or at a vertex.
        mask = static_cast<std::uint8_t>(static_cast<std::uint8_t>(PickFilter::Faces) |
                                         static_cast<std::uint8_t>(PickFilter::Vertices));
    } else if (picking_) {
        // Every kind the type takes (a hole is picked by a face of it).
        mask = 0;
        if (geometry_.accepts(QStringLiteral("edge")))
            mask |= static_cast<std::uint8_t>(PickFilter::Edges);
        if (geometry_.accepts(QStringLiteral("face")) || geometry_.accepts(QStringLiteral("hole")))
            mask |= static_cast<std::uint8_t>(PickFilter::Faces);
    }
    selection_->setFilterMask(mask);
}

void OperationToolModel::removePick(int index)
{
    if (!active() || index < 0 || index >= static_cast<int>(picks_.size()))
        return;
    picks_.erase(picks_.begin() + index);
    emit picksChanged();
    schedulePreview();
}

void OperationToolModel::clearPicks()
{
    if (!active() || picks_.empty())
        return;
    picks_.clear();
    emit picksChanged();
    schedulePreview();
}

void OperationToolModel::setPointPicking(const QString& key)
{
    if (!active() || key == pointPickKey_ || (!key.isEmpty() && !form_->pointKeys().contains(key)))
        return;
    pointPickKey_ = key;
    if (!key.isEmpty())
        picking_ = false; // one kind of click at a time
    ++pickSerial_;
    pickPending_ = false;
    if (selection_)
        selection_->clear();
    applyPickMode();
    emit picksChanged();
    emit statusChanged();
}

void OperationToolModel::takePoint(int bodyIndex, kernel::SubId sub, Vec3 at)
{
    const api::viewer::Body* body = scene_ ? scene_->body(bodyIndex) : nullptr;
    if (body == nullptr)
        return;
    const auto refuse = [&](const QString& reason) {
        setStatus(previewStatus_, reason, QStringLiteral("pick"));
        if (selection_)
            selection_->clear();
    };
    // T-077: as for edges and faces, a point is picked on the part, never on a fixture or another body.
    {
        doc::Document* d = liveDoc();
        const cam::Setup* setup = d != nullptr ? setupOf(*d->state()) : nullptr;
        if (setup == nullptr ||
            std::find(setup->partBodies.begin(), setup->partBodies.end(), body->id) == setup->partBodies.end()) {
            refuse(trs("%1 is not a part body of the setup: pick on the part").arg(qs(body->name)));
            return;
        }
    }
    if (!holesReady_) {
        refuse(trs("The setup is still being worked out: click again in a moment"));
        return;
    }
    if (!frame_) {
        refuse(frameError_.isEmpty() ? trs("The setup does not resolve, so no point can be placed in it")
                                     : trs("The setup does not resolve: %1").arg(frameError_));
        return;
    }
    // On the setup's XY plane: a hole's centre (any face of the hole snaps to it), else the vertex or the point
    // clicked on the face.
    double x = 0, y = 0;
    const auto hole = std::find_if(holes_.begin(), holes_.end(), [&](const api::cam_operation::Hole& h) {
        return sub.kind == kernel::SubKind::Face && h.body == body->id &&
               std::binary_search(h.faces.begin(), h.faces.end(), sub.index);
    });
    if (hole != holes_.end()) {
        x = hole->x;
        y = hole->y;
    } else {
        const double* m = frame_->wcsFromModel.m;
        x = m[0] * at.x + m[1] * at.y + m[2] * at.z + m[3];
        y = m[4] * at.x + m[5] * at.y + m[6] * at.z + m[7];
    }
    if (selection_)
        selection_->clear();
    setStatus(previewStatus_, QString()); // a message about an earlier refused click goes
    form_->addPoint(pointPickKey_, x, y);
}

void OperationToolModel::updatePointMarks()
{
    pointMarks_.clear();
    std::vector<Vec3> stems;
    if (active() && frame_) {
        const Box3& stock = frame_->stockBounds;
        const double s = std::clamp(0.02 * std::hypot(stock.max.x - stock.min.x, stock.max.y - stock.min.y), 1.0, 10.0);
        const double* m = frame_->modelFromWcs.m;
        const auto model = [m](double x, double y, double z) {
            return Vec3{m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
                        m[8] * x + m[9] * y + m[10] * z + m[11]};
        };
        for (const QString& key : form_->pointKeys()) {
            if (!form_->isRelevant(key))
                continue;
            for (const auto& [x, y] : form_->pointsOf(key)) {
                // A cross on the stock's top, and a line down through the stock (the point is on every level).
                const double top = stock.max.z, bottom = stock.min.z;
                for (const Vec3& v :
                     {model(x - s, y, top), model(x + s, y, top), model(x, y - s, top), model(x, y + s, top)})
                    pointMarks_.push_back(v);
                stems.push_back(model(x, y, top));
                stems.push_back(model(x, y, bottom));
            }
        }
    }
    if (!pointLines_)
        return;
    pointLines_->set(QStringLiteral("points.cross"), pointMarks_, render::Token::Accent, 2.5f);
    pointLines_->set(QStringLiteral("points.stem"), std::move(stems), render::Token::AccentThinLine, 1.5f, true);
}

void OperationToolModel::onSelectionChanged()
{
    if (!active() || !selection_ || !scene_)
        return;
    if (!pointPickKey_.isEmpty()) {
        const std::vector<SelectionEntry>& entries = selection_->entries();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->pending || !it->sub ||
                (it->sub->kind != kernel::SubKind::Face && it->sub->kind != kernel::SubKind::Vertex))
                continue;
            takePoint(it->body, *it->sub, it->hit.point);
            return;
        }
        return;
    }
    if (!picking_)
        return;
    const bool edges = geometry_.accepts(QStringLiteral("edge"));
    const bool faces = geometry_.accepts(QStringLiteral("face")) || geometry_.accepts(QStringLiteral("hole"));
    const std::vector<SelectionEntry>& entries = selection_->entries();
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->pending || !it->sub ||
            !((edges && it->sub->kind == kernel::SubKind::Edge) || (faces && it->sub->kind == kernel::SubKind::Face)))
            continue;
        takePick(it->body, *it->sub);
        return;
    }
}

void OperationToolModel::takePick(int bodyIndex, kernel::SubId sub)
{
    const api::viewer::Body* body = scene_ ? scene_->body(bodyIndex) : nullptr;
    doc::Document* d = liveDoc();
    if (body == nullptr || d == nullptr)
        return;
    const QString bodyId = idText(body->id.value);
    const auto refuse = [&](const QString& reason) {
        setStatus(previewStatus_, reason, QStringLiteral("pick"));
        if (selection_)
            selection_->clear(); // the refused element is not shown as picked
    };
    const cam::Setup* setup = setupOf(*d->state());
    if (setup == nullptr ||
        std::find(setup->partBodies.begin(), setup->partBodies.end(), body->id) == setup->partBodies.end()) {
        refuse(trs("%1 is not a part body of the setup: pick on the part").arg(qs(body->name)));
        return;
    }
    if (geometry_.accepts(QStringLiteral("hole"))) {
        if (!holesReady_) {
            // The holes are still being found: this click is taken when they are.
            queuedPick_ = std::make_pair(bodyIndex, sub);
            pickPending_ = true;
            emit statusChanged();
            return;
        }
        const auto hole = std::find_if(holes_.begin(), holes_.end(), [&](const api::cam_operation::Hole& h) {
            return h.body == body->id && std::binary_search(h.faces.begin(), h.faces.end(), sub.index);
        });
        if (hole == holes_.end()) {
            refuse(trs("That face is not part of a hole along the setup's Z: click a face of a hole"));
            return;
        }
        const bool picked = std::any_of(picks_.begin(), picks_.end(),
                                        [&](const Pick& p) { return p.body == bodyId && p.element == hole->boreFace; });
        if (picked) {
            refuse(trs("That hole is picked already"));
            return;
        }
        addPick(bodyId, hole->boreFace, holeText(*hole), kernel::SubKind::Face);
        return;
    }
    const bool edge = sub.kind == kernel::SubKind::Edge;
    if (!geometry_.accepts(edge ? QStringLiteral("edge") : QStringLiteral("face"))) {
        refuse(edge ? trs("This operation does not take edges: click a face")
                    : trs("This operation does not take faces: click an edge"));
        return;
    }
    const bool picked = std::any_of(picks_.begin(), picks_.end(), [&](const Pick& p) {
        return p.body == bodyId && p.kind == sub.kind && p.element == sub.index;
    });
    if (picked) {
        refuse(edge ? trs("That edge is picked already") : trs("That face is picked already"));
        return;
    }
    const QString text = edge ? trs("Edge of %1").arg(qs(body->name)) : trs("Face of %1").arg(qs(body->name));
    addPick(bodyId, sub.index, text, sub.kind);
}

void OperationToolModel::addPick(const QString& body, std::uint32_t element, const QString& text, kernel::SubKind kind)
{
    api::IApp* a = app();
    if (a == nullptr)
        return;
    const std::uint64_t serial = ++pickSerial_;
    pickPending_ = true;
    emit statusChanged();
    const QJsonObject args{
        {QStringLiteral("body"), body},
        {QStringLiteral("kind"), kind == kernel::SubKind::Edge ? QStringLiteral("edge") : QStringLiteral("face")},
        {QStringLiteral("index"), static_cast<int>(element)}};
    JobHandle<api::ActionResult> ref = a->actions().invoke("model.reference", jsonText(args), api::Caller::User);
    QPointer<OperationToolModel> self(this);
    ref.then([self, serial, body, element, text, kind](Result<api::ActionResult> res) {
        if (!self || serial != self->pickSerial_ || !self->active())
            return;
        self->pickPending_ = false;
        if (self->selection_)
            self->selection_->clear();
        if (!res) {
            self->setStatus(self->previewStatus_, qs(res.error().message), QStringLiteral("pick"));
            return;
        }
        Pick p;
        p.ref = jsonObject(res->resultJson).value(QStringLiteral("ref")).toObject();
        p.body = body;
        p.element = element;
        p.kind = kind;
        p.text = text;
        if (self->geometry_.atMostOne())
            self->picks_.clear();
        self->picks_.push_back(std::move(p));
        self->fieldErrors_.erase(QStringLiteral("geometry"));
        self->setStatus(self->previewStatus_, QString()); // the message about an earlier refused pick goes
        emit self->picksChanged();
        self->schedulePreview();
    });
}

// ---------- arguments ----------
std::optional<QJsonObject> OperationToolModel::arguments(QString* error, QString* field) const
{
    const auto fail = [&](const QString& why, const QString& where) -> std::optional<QJsonObject> {
        *error = why;
        *field = where;
        return std::nullopt;
    };
    doc::Document* d = liveDoc();
    if (!active() || d == nullptr)
        return fail(trs("The panel is closed"), {});
    const doc::DocStatePtr state = d->state();
    if (type_.isEmpty())
        return fail(trs("Choose the type of operation"), QStringLiteral("type"));
    camui::OperationInput in;
    in.id = idText(id_->value);
    in.setup = setup_ ? idText(setup_->value) : QString();
    in.type = type_;
    in.name = values_.value(QStringLiteral("name")).toString().trimmed();
    if (in.name.isEmpty())
        return fail(trs("Enter a name"), QStringLiteral("name"));
    if (toolId_.isEmpty())
        return fail(trs("Choose a tool"), QStringLiteral("tool"));
    in.tool = toolId_;
    in.holder = holderId_;
    // A library tool or holder the document does not have yet is embedded in the same step: the tool library's
    // "library" member (ToolLibraryModel::libraryArgsFor, as the picker's API says).
    const bool toolEmbedded =
        state->cam && state->cam->tools.count(ToolId{Uuid::fromString(toolId_.toStdString())}) != 0;
    const bool holderEmbedded =
        holderId_.isEmpty() ||
        (state->cam && state->cam->holders.count(HolderId{Uuid::fromString(holderId_.toStdString())}) != 0);
    if (!toolEmbedded || !holderEmbedded) {
        if (!tools_)
            return fail(trs("The tool library is not available"), QStringLiteral("tool"));
        const QJsonObject library = QJsonObject::fromVariantMap(tools_->libraryArgsFor(toolId_, holderId_));
        in.libraryTools = library.value(QStringLiteral("tools")).toArray();
        in.libraryHolders = library.value(QStringLiteral("holders")).toArray();
        if (!toolEmbedded && in.libraryTools.isEmpty())
            return fail(trs("The tool is in neither the document nor the library"), QStringLiteral("tool"));
        if (!holderEmbedded && in.libraryHolders.isEmpty())
            return fail(trs("The holder is in neither the document nor the library"), QStringLiteral("holder"));
    }
    const QString stickOut = values_.value(QStringLiteral("stickOut")).toString().trimmed();
    if (!stickOut.isEmpty()) {
        QString e;
        const auto v = length(QStringLiteral("stickOut"), &e);
        if (!v)
            return fail(e, QStringLiteral("stickOut"));
        in.stickOut = *v;
    }
    const QString number = values_.value(QStringLiteral("toolNumber")).toString().trimmed();
    if (!number.isEmpty()) {
        bool ok = false;
        const int n = number.toInt(&ok);
        if (!ok || n < 0 || n > 99999)
            return fail(trs("The tool number must be a whole number from 0 to 99999"), QStringLiteral("toolNumber"));
        in.toolNumber = n;
    } else if (!editing_) {
        // Left empty on a new operation: the document's number for this tool, else a number no other tool has (else
        // every library tool, whose own number is 0, would be T1, and the program would never change tools).
        const std::optional<lib::Tool> tool = chosenTool();
        in.toolNumber = camui::newToolNumber(state->cam.get(), ToolId{Uuid::fromString(toolId_.toStdString())},
                                             tool ? tool->defaultToolNumber : 0);
    }
    // Geometry.
    if (geometry_.needsOne() && picks_.empty())
        return fail(geometryPrompt().isEmpty() ? trs("Pick what to machine") : geometryPrompt(),
                    QStringLiteral("geometry"));
    for (const Pick& p : picks_)
        in.geometry.push_back(p.ref);
    // Heights: those the type uses, the others from them (docs/operation_types.md).
    std::map<QString, double> used;
    for (const QString& key : heights_) {
        QString e;
        const auto v = length(key, &e);
        if (!v)
            return fail(trs("%1: %2").arg(heightLabel(key), e), key);
        used[key] = *v;
    }
    in.heights = camui::completeHeights(used);
    // Only the order every operation of the type needs (heights measured from one reference); the rest is the type's
    // validation's, whose message the panel shows (face: a bottom from the part top above a top from the stock top).
    if (const auto bad = camui::heightsOutOfOrder(schema_, used))
        return fail(trs("%1 must not be below %2").arg(heightLabel(bad->first), heightLabel(bad->second)), bad->first);
    // Cutting data.
    const camui::ExprFn expr = exprFn();
    const bool inchUnits = inch();
    const auto exact = [&](const QString& key) -> std::optional<double> {
        const auto it = exact_.find(key);
        return it != exact_.end() && isExactText(key, values_.value(key).toString())
                   ? std::optional<double>(it->second.second)
                   : std::nullopt;
    };
    {
        QString e;
        auto rpm = exact(QStringLiteral("rpm"));
        if (!rpm)
            rpm = camui::readRpm(values_.value(QStringLiteral("rpm")).toString(), expr, &e);
        if (!rpm)
            return fail(values_.value(QStringLiteral("rpm")).toString().trimmed().isEmpty()
                            ? trs("Enter the spindle speed and feed, or use the calculated ones")
                            : trs("Spindle speed: %1").arg(e),
                        QStringLiteral("rpm"));
        in.rpm = *rpm;
        auto cut = exact(QStringLiteral("feedCut"));
        if (!cut)
            cut = camui::readFeed(values_.value(QStringLiteral("feedCut")).toString(), inchUnits, expr, &e);
        if (!cut)
            return fail(trs("Cut feed: %1").arg(e), QStringLiteral("feedCut"));
        in.feedCut = *cut;
    }
    const auto feed = [&](const char* key, double* slot) -> bool {
        const QString k = QLatin1String(key);
        const QString text = values_.value(k).toString();
        if (!cutting_.contains(k) || text.trimmed().isEmpty()) {
            *slot = in.feedCut; // the commands' own default too
            return true;
        }
        if (const auto v = exact(k)) {
            *slot = *v;
            return true;
        }
        QString e;
        const auto v = camui::readFeed(text, inchUnits, expr, &e);
        if (!v) {
            *error = e;
            *field = k;
            return false;
        }
        *slot = *v;
        return true;
    };
    if (!feed("feedPlunge", &in.feedPlunge) || !feed("feedRamp", &in.feedRamp) || !feed("feedLeadIn", &in.feedLeadIn) ||
        !feed("feedLeadOut", &in.feedLeadOut))
        return std::nullopt;
    in.coolant = values_.value(QStringLiteral("coolant")).toString();
    if (in.coolant.isEmpty())
        in.coolant = QStringLiteral("off");
    in.clockwise = values_.value(QStringLiteral("clockwise"), true).toBool();
    // Parameters.
    if (const auto bad = form_->firstError()) {
        const camui::FormField* f = nullptr;
        for (const camui::FormField& x : form_->schema().fields) {
            if (x.key == bad->first)
                f = &x;
        }
        return fail(trs("%1: %2").arg(f ? f->title : bad->first, bad->second), QStringLiteral("params/") + bad->first);
    }
    in.params = form_->params();
    // The record of "Use calculated", when it changed.
    if (!sameRecord(record_, loadedRecord_))
        in.calculated = record_ ? QJsonValue(camui::calculatedJson(*record_)) : QJsonValue(QJsonValue::Null);
    error->clear();
    field->clear();
    return camui::operationArgs(in, !editing_);
}

// ---------- status ----------
void OperationToolModel::setStatus(const QString& status, const QString& message, const QString& field)
{
    previewStatus_ = status;
    message_ = message;
    messageField_ = field;
    emit statusChanged();
}

void OperationToolModel::mapError(const QString& message)
{
    // "/params/<key>" from the API's check of the params; the command's own names a member.
    static const QRegularExpression pointer(QStringLiteral(R"(/params/([A-Za-z0-9_]+))"));
    if (const QRegularExpressionMatch m = pointer.match(message); m.hasMatch()) {
        const int colon = static_cast<int>(message.lastIndexOf(QStringLiteral(": ")));
        form_->setExternalMessage(m.captured(1), colon >= 0 ? message.mid(colon + 2) : message);
        setStatus(QStringLiteral("failed"), message, QStringLiteral("params/") + m.captured(1));
        return;
    }
    QString field;
    for (const QString& key : heights_) {
        if (message.contains(QStringLiteral("heights.") + key))
            field = key;
    }
    if (field.isEmpty() && message.contains(QLatin1String("heights")))
        field = heights_.isEmpty() ? QString() : heights_.front();
    for (const QString& key : cutting_) {
        if (message.contains(QStringLiteral("cutting.") + key))
            field = key;
    }
    if (message.contains(QLatin1String("tool.stickOut")))
        field = QStringLiteral("stickOut");
    else if (message.contains(QLatin1String("tool.")))
        field = QStringLiteral("tool");
    else if (message.contains(QLatin1String("geometry")))
        field = QStringLiteral("geometry");
    if (!field.isEmpty())
        fieldErrors_[field] = message;
    setStatus(QStringLiteral("failed"), message, field);
}

QString OperationToolModel::statusLine() const
{
    if (!active())
        return {};
    if (committing_)
        return trs("Saving…");
    if (messageField_ == QLatin1String("pick") && !message_.isEmpty())
        return message_;
    if (pickPending_)
        return trs("Taking the pick…");
    if (previewStatus_ == QLatin1String("incomplete"))
        return message_;
    if (previewStatus_ == QLatin1String("building") || previewBusy()) {
        if (progress_ >= 0)
            return trs("Generating the preview… %1%").arg(static_cast<int>(std::lround(progress_ * 100)));
        return trs("Building the preview…");
    }
    if (!errors_.isEmpty())
        return trs("Fix the error above first");
    if (previewStatus_ == QLatin1String("failed"))
        return trs("The preview shows the last values that worked");
    if (canCommit())
        return editing_ ? trs("Ready: Apply changes the operation") : trs("Ready: OK adds the operation");
    return {};
}

bool OperationToolModel::canCommit() const
{
    if (!active() || committing_ || rejected_ || pickPending_ || !checked_ || !errors_.isEmpty())
        return false;
    QString error, field;
    return arguments(&error, &field).has_value();
}

bool OperationToolModel::previewBusy() const
{
    const auto running = [](const auto& job) {
        return job.valid() && !job.done();
    };
    return pickPending_ || previewTimer_.isActive() || previewBusy_ || previewPending_ || running(validateJob_) ||
           running(previewJob_) || running(backplotJob_) || running(holesJob_) || committing_;
}

// ---------- preview ----------
void OperationToolModel::schedulePreview()
{
    if (active())
        previewTimer_.start();
    emit statusChanged();
}

void OperationToolModel::startPreview()
{
    if (!active() || committing_)
        return;
    doc::Document* d = liveDoc();
    api::IApp* a = app();
    if (d == nullptr || a == nullptr)
        return;
    if (previewBusy_) {
        previewPending_ = true;
        return;
    }
    previewPending_ = false;
    checked_ = false;
    QString error, field;
    const auto args = arguments(&error, &field);
    if (!args) {
        // The next step still missing: nothing to preview yet.
        rejected_ = true;
        cancelJobs();
        errors_.clear();
        warnings_.clear();
        generatedWarnings_.clear();
        progress_ = -1;
        setStatus(QStringLiteral("incomplete"), error, field);
        return;
    }
    rejected_ = false;
    form_->clearExternal();
    fieldErrors_.clear();
    const doc::Command given{editing_ ? "cam.operation.edit" : "cam.operation.add", jsonText(*args),
                             title().toStdString()};
    auto cmd = api::cam_operation::completeCommand(*a, *d->state(), given);
    if (!cmd) {
        rejected_ = true;
        cancelJobs();
        mapError(qs(cmd.error().message));
        return;
    }
    ++previewSerial_;
    previewBase_ = d->state();
    previewBusy_ = true;
    setStatus(QStringLiteral("building"), QString());
    d->beginPreview(*cmd);
    if (const auto e = d->previewError()) {
        previewBusy_ = false;
        rejected_ = true;
        mapError(qs(e->message));
    }
}

void OperationToolModel::onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build)
{
    previewBusy_ = false;
    if (!active())
        return;
    api::IApp* a = app();
    cam::ICamEngine* engine = a ? a->cam() : nullptr;
    if (!state || !build || engine == nullptr) {
        doc::Document* d = liveDoc();
        const auto error = d ? d->previewError() : std::nullopt;
        setStatus(QStringLiteral("failed"), error ? qs(error->message) : trs("The preview could not be built"));
    } else {
        // What ran for the values before is superseded.
        cancelJobs();
        generatedWarnings_.clear();
        const std::uint64_t serial = previewSerial_;
        const OperationId id = *id_;
        const SetupId setup = *setup_;
        QPointer<OperationToolModel> self(this);
        validateJob_ = engine->validate(state, build, id);
        validateJob_.then([self, serial](Result<cam::OpValidation> r) {
            if (!self || serial != self->previewSerial_ || !self->active())
                return;
            self->checked_ = true;
            self->errors_.clear();
            self->warnings_.clear();
            if (r) {
                if (r->error && r->status != cam::OpStatus::Stale)
                    self->errors_ << qs(r->error->message);
                for (const std::string& w : r->warnings)
                    self->warnings_ << qs(w);
            } else if (r.error().code != ErrorCode::Cancelled) {
                self->errors_ << qs(r.error().message);
            }
            emit self->statusChanged();
        });
        progress_ = 0;
        previewJob_ = engine->preview(state, build, id);
        previewJob_.onProgress([self, serial](double fraction, std::string) {
            if (!self || serial != self->previewSerial_)
                return;
            self->progress_ = fraction;
            emit self->statusChanged();
        });
        IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
        previewJob_.then([self, serial, state, build, setup, jobs](Result<std::shared_ptr<const cam::Toolpath>> r) {
            if (!self || serial != self->previewSerial_ || !self->active())
                return;
            self->progress_ = -1;
            if (!r) {
                if (r.error().code != ErrorCode::Cancelled) {
                    // A generator failure validation did not catch blocks OK as well.
                    if (!self->errors_.contains(qs(r.error().message)))
                        self->errors_ << qs(r.error().message);
                    self->setStatus(QStringLiteral("failed"), QString());
                }
                emit self->statusChanged();
                return;
            }
            self->previewPath_ = *r;
            // After M3: what the generation itself warned of. A type's check may stop short of what only generation
            // finds (adaptive2d does not run its engine there, slot does not link: a last-resort plunge, say).
            self->generatedWarnings_.clear();
            if (*r) {
                for (const std::string& w : (*r)->warnings)
                    self->generatedWarnings_ << qs(w);
            }
            if (jobs == nullptr)
                return;
            const std::shared_ptr<const cam::Toolpath> path = *r;
            self->backplotJob_ = jobs->submit<std::shared_ptr<const sim::Backplot>>(
                JobInfo{"Toolpath preview display", JobPriority::Interactive, false},
                [state, build, setup, path](JobContext& ctx) -> Result<std::shared_ptr<const sim::Backplot>> {
                    return api::cam_operation::backplot(state, build, setup, *path, ctx);
                });
            self->backplotJob_.then([self, serial](Result<std::shared_ptr<const sim::Backplot>> b) {
                if (!self || serial != self->previewSerial_ || !self->active())
                    return;
                if (b)
                    self->showBackplot(*b);
                else if (b.error().code != ErrorCode::Cancelled)
                    self->setStatus(QStringLiteral("failed"), qs(b.error().message));
                emit self->statusChanged();
            });
            emit self->statusChanged();
        });
        setStatus(QStringLiteral("building"), QString());
    }
    if (previewPending_)
        startPreview();
}

void OperationToolModel::showBackplot(std::shared_ptr<const sim::Backplot> backplot)
{
    previewBackplot_ = backplot;
    if (display_ && id_ && setup_)
        display_->setPreview(*id_, *setup_, std::move(backplot));
    ++previewsShown_;
    setStatus(QStringLiteral("ok"), QString());
}

// ---------- commit ----------
bool OperationToolModel::commit()
{
    if (!canCommit())
        return false;
    api::IApp* a = app();
    if (a == nullptr)
        return false;
    QString error, field;
    const auto args = arguments(&error, &field);
    if (!args) {
        setStatus(QStringLiteral("incomplete"), error, field);
        return false;
    }
    committing_ = true;
    previewTimer_.stop();
    emit statusChanged();
    // As the user: the confirmation rule (changesToolpathsOrGcode) is for scripts and agents.
    const char* name = editing_ ? "cam.operation.edit" : "cam.operation.add";
    JobHandle<api::ActionResult> job = a->actions().invoke(name, jsonText(*args), api::Caller::User);
    if (!job.valid()) {
        committing_ = false;
        setStatus(QStringLiteral("failed"), trs("The operation could not be saved"));
        return false;
    }
    const OperationId id = *id_;
    const QString idString = idText(id.value);
    const QString type = type_;
    QPointer<OperationToolModel> self(this);
    job.then([self, id, idString, type](Result<api::ActionResult> r) {
        if (!self)
            return;
        self->committing_ = false;
        if (!r) {
            self->mapError(qs(r.error().message));
            self->schedulePreview();
            return;
        }
        self->lastUsedType_ = type; // the next new operation starts with it
        // The committed operation's toolpath: found in the cache when the preview made it, else generated now.
        if (doc::Document* d = self->liveDoc(); d != nullptr && self->app() != nullptr && self->app()->cam() != nullptr)
            (void)self->app()->cam()->generate(d->state(), d->build(), id);
        if (self->tree_)
            self->tree_->selectWhenListed(idString);
        self->close();
        emit self->committed(idString);
    });
    return true;
}

} // namespace ray::ui
