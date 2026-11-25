#include "fixtures_tool.h"

// Before operation_tool.h, on its own: its QPointer<ToolpathDisplay> needs the whole class.
#include "operation_view.h"

#include "../app_context.h"
#include "../model/model_util.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../selection_model.h"
#include "../session.h"
#include "../ui_commands.h"
#include "../units.h"
#include "operation_tool.h"
#include "setup_tool.h"
#include "setup_tree.h"

#include <ray_api/cam_setup.h>
#include <ray_api/feature_edit.h>
#include <ray_api/viewer.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include <algorithm>
#include <map>
#include <utility>
#include <variant>

namespace ray::ui {

using namespace modelui;

namespace {
// T-077: the fixtures panel. Its key works in the CAM workspace only.
// clang-format off
constexpr UiCommand kCamFixturesCommands[] = {
    {"cam.fixtures.open", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Fixtures…"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "CAM"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Opens the fixtures panel: add a vise, parallels or your own STEP as a fixture and place it against the part"), "Shift+F", CommandContext::Cam, ""},
};
// clang-format on

constexpr const char* kTrContext = "ray::ui::FixturesToolModel";
constexpr std::string_view kTransformBody = "transform_body";

QString trs(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

QJsonObject propsOf(const doc::Feature& f)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(f.propsJson)).object();
}

// The last transform_body feature that moves `body`.
const doc::Feature* placementOf(const doc::DocState& state, const QString& body)
{
    const doc::Feature* found = nullptr;
    for (const doc::Feature& f : state.features) {
        if (f.type == kTransformBody && propsOf(f).value(QStringLiteral("body")).toString() == body)
            found = &f;
    }
    return found;
}

const cam::Setup* findSetup(const doc::DocState& state, const SetupId& id)
{
    if (!state.cam)
        return nullptr;
    const auto it = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                 [&](const cam::Setup& s) { return s.id == id; });
    return it == state.cam->setups.end() ? nullptr : &*it;
}

QString fieldOf(const QString& message)
{
    for (const char* key : {"gap", "angle", "tiltX", "tiltY", "mate.face", "mate.target"}) {
        if (message.contains(QLatin1Char('\'') + QLatin1String(key) + QLatin1Char('\'')))
            return QString::fromLatin1(key).replace(QStringLiteral("mate."), QString());
    }
    for (const char* key : {"x", "y", "z"}) {
        if (message.contains(QLatin1Char('\'') + QLatin1String(key) + QLatin1Char('\'')))
            return QLatin1String(key);
    }
    if (message.contains(QStringLiteral("the face to mate")))
        return QStringLiteral("face");
    if (message.contains(QStringLiteral("the face it mates to")))
        return QStringLiteral("target");
    return {};
}

} // namespace

std::span<const UiCommand> camFixturesUiCommands()
{
    return kCamFixturesCommands;
}

// ---------- construction ----------
FixturesToolModel::FixturesToolModel(SceneController* scene, SelectionModel* selection, Session* session,
                                     AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session), context_(context)
{
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(0);
    connect(&previewTimer_, &QTimer::timeout, this, [this] { startPreview(); });
    if (scene_) {
        connect(&scene_->preview(), &BodyOverlay::shown, this, [this] {
            if (scene_ && scene_->preview().owner() == this)
                emit statusChanged();
        });
    }
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            if (session_->documentId().toStdString() != docId_) {
                closePlacement();
                subscribe();
                emit fixturesChanged();
            }
        });
    }
    if (selection_)
        connect(selection_, &SelectionModel::changed, this, [this] { onSelectionChanged(); });
    if (context_) {
        // Leaving the CAM workspace closes the panel; a placement under way is cancelled.
        connect(context_, &AppContext::workspaceChanged, this, [this] {
            if (shown_ && context_ && context_->workspace() != kCamWorkspace)
                close();
        });
    }
}

FixturesToolModel::~FixturesToolModel()
{
    ++stockSerial_;
    if (stockJob_.valid())
        stockJob_.cancel();
    if (commands_)
        commands_->setUiHandler(QStringLiteral("cam.fixtures."), {});
    if (doc::Document* d = liveDoc(); d != nullptr && !body_.isEmpty())
        d->cancelPreview();
    if (scene_)
        scene_->preview().clear(this);
    if (filterSaved_ && selection_)
        selection_->setFilterMask(savedFilter_);
    unsubscribe();
}

FixturesToolModel* FixturesToolModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    UnitsObject* units = nullptr;
    SetupToolModel* setup = nullptr;
    OperationToolModel* operation = nullptr;
    SetupTreeModel* tree = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
        setup = qml->singletonInstance<SetupToolModel*>(uri, QStringLiteral("CamSetup"));
        operation = qml->singletonInstance<OperationToolModel*>(uri, QStringLiteral("CamOperation"));
        tree = qml->singletonInstance<SetupTreeModel*>(uri, QStringLiteral("CamTreeModel"));
    }
    auto* model = new FixturesToolModel(scene, selection, session, context);
    model->setCommands(commands);
    model->setUnits(units);
    model->setPanels(setup, operation, tree);
    model->subscribe();
    return model; // owned by the engine
}

void FixturesToolModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<FixturesToolModel> self(this);
    commands_->setUiHandler(QStringLiteral("cam.fixtures."),
                            CommandCatalog::UiHandler{
                                [self](const QString& id) {
                                    return self ? self->commandReason(id) : trs("The fixtures panel is not available");
                                },
                                [self](const QString& id) { return self && self->runCommand(id); }});
}

void FixturesToolModel::setUnits(UnitsObject* units)
{
    units_ = units;
}

// One CAM panel at a time: the setup or operation panel opening closes this one.
void FixturesToolModel::setPanels(SetupToolModel* setup, OperationToolModel* operation, SetupTreeModel* tree)
{
    setupTool_ = setup;
    operationTool_ = operation;
    tree_ = tree;
    if (setupTool_) {
        connect(setupTool_, &SetupToolModel::activeChanged, this, [this] {
            if (setupTool_ && setupTool_->active() && shown_)
                close();
        });
    }
    if (operationTool_) {
        connect(operationTool_, &OperationToolModel::activeChanged, this, [this] {
            if (operationTool_ && operationTool_->active() && shown_)
                close();
        });
    }
}

// ---------- document access ----------
api::IApp* FixturesToolModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* FixturesToolModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void FixturesToolModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<FixturesToolModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->onDocumentChanged();
    });
    previewListener_ = doc_->subscribePreview([self, generation](doc::DocStatePtr state, doc::BuildResultPtr build) {
        if (self && self->docGeneration_ == generation && self->previewBusy_)
            self->onPreviewBuilt(std::move(state), std::move(build));
    });
}

void FixturesToolModel::unsubscribe()
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

void FixturesToolModel::onDocumentChanged()
{
    emit fixturesChanged();
    if (body_.isEmpty() || committing_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr) {
        closePlacement();
        return;
    }
    // The fixture is gone (an undo of its import), or the edited feature is: the placement closes.
    const doc::DocStatePtr state = d->state();
    const bool bodyThere = std::any_of(state->bodies.begin(), state->bodies.end(),
                                       [&](const doc::Body& b) { return idText(b.id.value) == body_; });
    const bool featureThere =
        !existing_ || std::any_of(state->features.begin(), state->features.end(),
                                  [&](const doc::Feature& f) { return feature_ && f.id == *feature_; });
    if (!bodyThere || !featureThere) {
        closePlacement();
        return;
    }
    if (state == previewBase_)
        return;
    previewBusy_ = false;
    schedulePreview();
}

// ---------- commands ----------
QString FixturesToolModel::commandReason(const QString& id) const
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
    return {};
}

bool FixturesToolModel::runCommand(const QString& id)
{
    if (id == QLatin1String("cam.fixtures.open"))
        return open();
    return false;
}

// ---------- the panel ----------
bool FixturesToolModel::open()
{
    if (const QString reason = commandReason(QStringLiteral("cam.fixtures.open")); !reason.isEmpty()) {
        if (context_)
            context_->showMessage(reason, true);
        return false;
    }
    if (context_ && context_->workspace() != kCamWorkspace)
        context_->setWorkspace(kCamWorkspace);
    if (setupTool_ && setupTool_->active())
        setupTool_->cancel();
    if (operationTool_ && operationTool_->active())
        operationTool_->cancel();
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    loadLibrary();
    if (!shown_) {
        shown_ = true;
        emit shownChanged();
    }
    emit fixturesChanged();
    return true;
}

void FixturesToolModel::close()
{
    closePlacement();
    if (shown_) {
        shown_ = false;
        emit shownChanged();
    }
}

void FixturesToolModel::escape()
{
    if (!body_.isEmpty())
        cancel();
    else
        close();
}

void FixturesToolModel::loadLibrary()
{
    library_.clear();
    api::IApp* a = app();
    lib::LibraryStack* stack = a != nullptr ? a->libraries() : nullptr;
    std::vector<std::pair<std::string, std::string>> roots;
    if (stack != nullptr) {
        for (const lib::LibraryStack::RootInfo& r : stack->roots())
            roots.emplace_back(r.dir, r.name);
    }
    models_.clear();
    for (const camui::LibraryFixture& f : camui::libraryFixtures(roots)) {
        library_.push_back(QVariantMap{
            {QStringLiteral("name"), f.name}, {QStringLiteral("path"), f.path}, {QStringLiteral("root"), f.root}});
        // Its description beside it (ray_api/cam_fixtures.h), for Place on stock; a file that does not read is skipped.
        const QFileInfo model(f.path);
        QFile description(model.dir().filePath(model.completeBaseName() + QStringLiteral(".json")));
        if (description.open(QIODevice::ReadOnly)) {
            if (auto parsed = api::cam_fixtures::parseFixtureModel(description.readAll().toStdString()))
                models_.push_back(std::move(*parsed));
        }
    }
    emit libraryChanged();
    emit fixturesChanged();
}

std::optional<SetupId> FixturesToolModel::targetSetup() const
{
    doc::Document* d = liveDoc();
    if (d == nullptr || !d->state()->cam || d->state()->cam->setups.empty())
        return std::nullopt;
    if (tree_) {
        if (const std::optional<SetupId> current = tree_->currentSetup();
            current && findSetup(*d->state(), *current) != nullptr)
            return current;
    }
    return d->state()->cam->setups.front().id;
}

QString FixturesToolModel::setupName() const
{
    doc::Document* d = liveDoc();
    const std::optional<SetupId> id = targetSetup();
    const cam::Setup* s = d != nullptr && id ? findSetup(*d->state(), *id) : nullptr;
    return s != nullptr ? qs(s->name) : QString();
}

QVariantList FixturesToolModel::fixtures() const
{
    QVariantList out;
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return out;
    const doc::DocStatePtr state = d->state();
    const std::optional<SetupId> setupId = targetSetup();
    const cam::Setup* setup = setupId ? findSetup(*state, *setupId) : nullptr;
    for (const doc::Body& b : state->bodies) {
        if (b.role != doc::Body::Role::Fixture)
            continue;
        const QString id = idText(b.id.value);
        const doc::Feature* placement = placementOf(*state, id);
        const bool inSetup = setup != nullptr && std::find(setup->fixtureBodies.begin(), setup->fixtureBodies.end(),
                                                           b.id) != setup->fixtureBodies.end();
        out.push_back(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), qs(b.name)},
            {QStringLiteral("placed"), placement != nullptr},
            {QStringLiteral("mated"), placement != nullptr && propsOf(*placement).contains(QStringLiteral("mate"))},
            {QStringLiteral("inSetup"), inSetup}});
    }
    return out;
}

QString FixturesToolModel::bodyName(const QString& id) const
{
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return {};
    for (const doc::Body& b : d->state()->bodies) {
        if (idText(b.id.value) == id)
            return qs(b.name);
    }
    return {};
}

QString FixturesToolModel::placingName() const
{
    return body_.isEmpty() ? QString() : bodyName(body_);
}

// ---------- adding fixtures ----------
void FixturesToolModel::requestImport()
{
    if (!commandReason(QString()).isEmpty())
        return;
    emit importDialogRequested();
}

bool FixturesToolModel::importFile(const QString& pathOrUrl)
{
    api::IApp* a = app();
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (a == nullptr || d == nullptr || importing_)
        return false;
    const QUrl url(pathOrUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
    if (path.isEmpty())
        return false;
    closePlacement();
    std::set<BodyId> before;
    for (const doc::Body& b : d->state()->bodies)
        before.insert(b.id);
    const QJsonObject args{{QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
                           {QStringLiteral("role"), QStringLiteral("fixture")},
                           {QStringLiteral("document"), QString::fromStdString(docId_)}};
    JobHandle<api::ActionResult> job = a->actions().invoke("file.import", jsonText(args), api::Caller::User);
    if (!job.valid())
        return false;
    importing_ = true;
    setStatus(previewStatus_, QString());
    QPointer<FixturesToolModel> self(this);
    job.then([self, before](Result<api::ActionResult> r) {
        if (!self)
            return;
        self->importing_ = false;
        doc::Document* d = self->liveDoc();
        if (!r || d == nullptr) {
            self->setStatus(QString(), r ? QString() : qs(r.error().message));
            return;
        }
        // The new fixture bodies, in document order.
        QStringList added;
        std::vector<BodyId> addedIds;
        for (const doc::Body& b : d->state()->bodies) {
            if (before.count(b.id) == 0 && b.role == doc::Body::Role::Fixture) {
                added << idText(b.id.value);
                addedIds.push_back(b.id);
            }
        }
        emit self->fixturesChanged();
        if (added.isEmpty()) {
            self->setStatus(QString(), QString());
            return;
        }
        // Into the setup's fixture bodies, so the safety checks see them.
        const std::optional<SetupId> setupId = self->targetSetup();
        const cam::Setup* setup = setupId ? findSetup(*d->state(), *setupId) : nullptr;
        api::IApp* app = self->app();
        if (setup != nullptr && app != nullptr) {
            QJsonArray list;
            for (const BodyId& b : setup->fixtureBodies)
                list.append(idText(b.value));
            for (const QString& b : added)
                list.append(b);
            const QJsonObject edit{{QStringLiteral("id"), idText(setup->id.value)},
                                   {QStringLiteral("fixtureBodies"), list},
                                   {QStringLiteral("document"), QString::fromStdString(self->docId_)}};
            const QString first = added.front();
            JobHandle<api::ActionResult> join =
                app->actions().invoke("cam.setup.edit", jsonText(edit), api::Caller::User);
            join.then([self, first](Result<api::ActionResult> res) {
                if (!self)
                    return;
                if (!res)
                    self->setStatus(QString(), qs(res.error().message));
                emit self->fixturesChanged();
                self->place(first);
            });
            return;
        }
        self->place(added.front());
    });
    emit statusChanged();
    return true;
}

bool FixturesToolModel::addFromLibrary(int index)
{
    if (index < 0 || index >= library_.size())
        return false;
    return importFile(library_.at(index).toMap().value(QStringLiteral("path")).toString());
}

bool FixturesToolModel::setInSetup(const QString& bodyId, bool in)
{
    api::IApp* a = app();
    doc::Document* d = liveDoc();
    const std::optional<SetupId> setupId = targetSetup();
    const cam::Setup* setup = d != nullptr && setupId ? findSetup(*d->state(), *setupId) : nullptr;
    if (a == nullptr || setup == nullptr)
        return false;
    const BodyId body{Uuid::fromString(bodyId.toStdString())};
    QJsonArray list;
    bool there = false;
    for (const BodyId& b : setup->fixtureBodies) {
        if (b == body) {
            there = true;
            if (!in)
                continue;
        }
        list.append(idText(b.value));
    }
    if (in == there)
        return true;
    if (in)
        list.append(bodyId);
    const QJsonObject edit{{QStringLiteral("id"), idText(setup->id.value)},
                           {QStringLiteral("fixtureBodies"), list},
                           {QStringLiteral("document"), QString::fromStdString(docId_)}};
    QPointer<FixturesToolModel> self(this);
    a->actions().invoke("cam.setup.edit", jsonText(edit), api::Caller::User).then([self](Result<api::ActionResult> r) {
        if (!self)
            return;
        if (!r)
            self->setStatus(self->previewStatus_, qs(r.error().message));
        emit self->fixturesChanged();
    });
    return true;
}

// ---------- the placement ----------
bool FixturesToolModel::place(const QString& bodyId)
{
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return false;
    const doc::DocStatePtr state = d->state();
    const auto body = std::find_if(state->bodies.begin(), state->bodies.end(),
                                   [&](const doc::Body& b) { return idText(b.id.value) == bodyId; });
    if (body == state->bodies.end())
        return false;
    if (!shown_)
        open();
    closePlacement();
    body_ = bodyId;
    values_.clear();
    face_ = target_ = QJsonObject();
    targetBody_.clear();
    const QString unit = units_ ? units_->unitLabel() : QStringLiteral("mm");
    if (const doc::Feature* f = placementOf(*state, bodyId)) {
        feature_ = f->id;
        existing_ = true;
        const QJsonObject props = propsOf(*f);
        for (const char* key : {"gap", "x", "y", "z", "angle", "tiltX", "tiltY"}) {
            const QLatin1String k(key);
            values_[k] = camui::valueText(props.value(k),
                                          k == QLatin1String("angle") || k.startsWith(QLatin1String("tilt")), unit);
        }
        const QJsonObject mate = props.value(QStringLiteral("mate")).toObject();
        face_ = mate.value(QStringLiteral("face")).toObject();
        target_ = mate.value(QStringLiteral("target")).toObject();
        targetBody_ = target_.value(QStringLiteral("body")).toString();
    } else {
        feature_ = FeatureId{Uuid::generate()};
        existing_ = false;
    }
    pickMode_ = face_.isEmpty() ? QStringLiteral("face") : QString();
    if (selection_) {
        savedFilter_ = selection_->filterMask();
        filterSaved_ = true;
        selection_->clear();
    }
    applyPickMode();
    emit placingChanged();
    emit valuesChanged();
    emit picksChanged();
    emit statusChanged();
    schedulePreview();
    return true;
}

void FixturesToolModel::closePlacement()
{
    const bool was = !body_.isEmpty();
    previewTimer_.stop();
    ++pickSerial_;
    pickPending_ = false;
    if (doc::Document* d = liveDoc(); d != nullptr && was)
        d->cancelPreview();
    if (scene_)
        scene_->preview().clear(this);
    if (filterSaved_ && selection_)
        selection_->setFilterMask(savedFilter_);
    filterSaved_ = false;
    body_.clear();
    feature_.reset();
    existing_ = false;
    values_.clear();
    face_ = target_ = QJsonObject();
    targetBody_.clear();
    pickMode_.clear();
    message_.clear();
    messageField_.clear();
    previewStatus_.clear();
    committing_ = false;
    rejected_ = false;
    previewBusy_ = previewPending_ = false;
    previewBase_.reset();
    if (was) {
        emit placingChanged();
        emit valuesChanged();
        emit picksChanged();
        emit statusChanged();
    }
}

void FixturesToolModel::setValue(const QString& key, const QVariant& value)
{
    if (body_.isEmpty())
        return;
    static const QStringList keys = {QStringLiteral("gap"),  QStringLiteral("x"),     QStringLiteral("y"),
                                     QStringLiteral("z"),    QStringLiteral("angle"), QStringLiteral("tiltX"),
                                     QStringLiteral("tiltY")};
    if (!keys.contains(key))
        return;
    values_[key] = value.toString();
    emit valuesChanged();
    schedulePreview();
}

QString FixturesToolModel::faceText() const
{
    if (face_.isEmpty())
        return trs("None: click the face that goes against the part");
    return trs("Face of %1").arg(placingName());
}

QString FixturesToolModel::targetText() const
{
    if (target_.isEmpty())
        return trs("None: click a face of the part");
    const QString name = bodyName(targetBody_);
    return name.isEmpty() ? trs("A face of a body that is gone") : trs("Face of %1").arg(name);
}

void FixturesToolModel::setPickMode(const QString& mode)
{
    if (body_.isEmpty())
        return;
    const QString m = mode == QLatin1String("face") || mode == QLatin1String("target") ? mode : QString();
    if (m == pickMode_)
        return;
    pickMode_ = m;
    ++pickSerial_;
    pickPending_ = false;
    if (selection_)
        selection_->clear();
    applyPickMode();
    emit picksChanged();
    emit statusChanged();
}

void FixturesToolModel::applyPickMode()
{
    if (!selection_ || body_.isEmpty())
        return;
    const std::uint8_t mask = !pickMode_.isEmpty() ? static_cast<std::uint8_t>(render::PickFilter::Faces)
                              : filterSaved_       ? savedFilter_
                                                   : selection_->filterMask();
    selection_->setFilterMask(mask);
}

void FixturesToolModel::clearMate()
{
    if (body_.isEmpty())
        return;
    face_ = target_ = QJsonObject();
    targetBody_.clear();
    values_.remove(QStringLiteral("gap"));
    if (selection_)
        selection_->clear();
    emit valuesChanged();
    emit picksChanged();
    schedulePreview();
}

void FixturesToolModel::onSelectionChanged()
{
    if (body_.isEmpty() || pickMode_.isEmpty() || !selection_ || !scene_)
        return;
    const std::vector<SelectionEntry>& entries = selection_->entries();
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->pending || !it->sub || it->sub->kind != kernel::SubKind::Face)
            continue;
        takePick(it->body, *it->sub);
        return;
    }
}

// A face of the fixture for "face", of another body for "target", referenced through model.reference (the fixture's
// face as the body is just before its feature, so the mate is resolved where it was picked).
void FixturesToolModel::takePick(int bodyIndex, kernel::SubId sub)
{
    api::IApp* a = app();
    const api::viewer::Body* body = scene_ ? scene_->body(bodyIndex) : nullptr;
    if (a == nullptr || body == nullptr)
        return;
    const QString picked = idText(body->id.value);
    const bool facePick = pickMode_ == QLatin1String("face");
    const auto refuse = [&](const QString& reason) {
        setStatus(previewStatus_, reason, facePick ? QStringLiteral("face") : QStringLiteral("target"));
        if (selection_)
            selection_->clear();
    };
    if (facePick && picked != body_) {
        refuse(trs("That is a face of %1: click a face of %2, the fixture being placed")
                   .arg(qs(body->name), placingName()));
        return;
    }
    if (!facePick && picked == body_) {
        refuse(trs("Click a face of another body: the part, or the fixture this one stands on"));
        return;
    }
    const std::uint64_t serial = ++pickSerial_;
    pickPending_ = true;
    emit statusChanged();
    QJsonObject args{{QStringLiteral("body"), picked},
                     {QStringLiteral("kind"), QStringLiteral("face")},
                     {QStringLiteral("index"), static_cast<int>(sub.index)},
                     {QStringLiteral("document"), QString::fromStdString(docId_)}};
    if (facePick && existing_ && feature_)
        args.insert(QStringLiteral("before"), idText(feature_->value));
    QPointer<FixturesToolModel> self(this);
    a->actions()
        .invoke("model.reference", jsonText(args), api::Caller::User)
        .then([self, serial, facePick, picked](Result<api::ActionResult> res) {
            if (!self || serial != self->pickSerial_ || self->body_.isEmpty())
                return;
            self->pickPending_ = false;
            if (!res) {
                self->setStatus(self->previewStatus_, qs(res.error().message));
                return;
            }
            const QJsonObject ref = jsonObject(res->resultJson).value(QStringLiteral("ref")).toObject();
            if (facePick) {
                self->face_ = ref;
                self->pickMode_ = self->target_.isEmpty() ? QStringLiteral("target") : QString();
                // A mate sets the orientation: the tilts of a move by numbers go.
                if (self->values_.remove(QStringLiteral("tiltX")) + self->values_.remove(QStringLiteral("tiltY")) > 0)
                    emit self->valuesChanged();
            } else {
                self->target_ = ref;
                self->targetBody_ = picked;
                self->pickMode_.clear();
            }
            if (self->selection_)
                self->selection_->clear();
            self->applyPickMode();
            self->setStatus(self->previewStatus_, QString());
            emit self->picksChanged();
            self->schedulePreview();
        });
}

// ---------- preview ----------
camui::PlacementArgs FixturesToolModel::arguments() const
{
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d ? d->state() : nullptr;
    camui::PlacementInput in;
    in.body = body_;
    in.face = face_;
    in.target = target_;
    in.gap = values_.value(QStringLiteral("gap")).toString();
    in.x = values_.value(QStringLiteral("x")).toString();
    in.y = values_.value(QStringLiteral("y")).toString();
    in.z = values_.value(QStringLiteral("z")).toString();
    in.angle = values_.value(QStringLiteral("angle")).toString();
    in.tiltX = values_.value(QStringLiteral("tiltX")).toString();
    in.tiltY = values_.value(QStringLiteral("tiltY")).toString();
    in.unitLabel = units_ ? units_->unitLabel() : QStringLiteral("mm");
    const camui::CheckFn check = [&](const QString& expression, bool angle, QString* error) {
        if (!state)
            return false;
        auto v = api::feature_edit::evaluate(*state, expression.toStdString(), angle);
        if (!v)
            *error = qs(v.error().message);
        return v.has_value();
    };
    return camui::placementProps(in, check);
}

doc::Command FixturesToolModel::command() const
{
    const camui::PlacementArgs args = arguments();
    QJsonObject cmd{{QStringLiteral("id"), feature_ ? idText(feature_->value) : QString()}};
    if (existing_) {
        cmd.insert(QStringLiteral("props"), camui::editPatch(args.props));
        return doc::Command{"feature.transform_body.edit", jsonText(cmd),
                            trs("Place %1").arg(placingName()).toStdString()};
    }
    cmd.insert(QStringLiteral("props"), args.props);
    cmd.insert(QStringLiteral("name"), trs("Place %1").arg(placingName()));
    if (doc::Document* d = liveDoc()) {
        std::vector<std::pair<std::string, std::string>> features;
        for (const doc::Feature& f : d->state()->features)
            features.emplace_back(f.type, f.propsJson);
        if (const std::optional<int> at = camui::placementIndex(features, body_))
            cmd.insert(QStringLiteral("index"), *at);
    }
    return doc::Command{"feature.transform_body.add", jsonText(cmd), trs("Place %1").arg(placingName()).toStdString()};
}

void FixturesToolModel::schedulePreview()
{
    if (!body_.isEmpty())
        previewTimer_.start();
}

void FixturesToolModel::setStatus(const QString& status, const QString& message, const QString& field)
{
    previewStatus_ = status;
    message_ = message;
    messageField_ = field.isEmpty() && !message.isEmpty() ? fieldOf(message) : field;
    emit statusChanged();
}

void FixturesToolModel::startPreview()
{
    if (body_.isEmpty() || committing_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return;
    if (previewBusy_) {
        previewPending_ = true;
        return;
    }
    previewPending_ = false;
    const camui::PlacementArgs args = arguments();
    if (!args.error.isEmpty()) {
        rejected_ = true;
        // The next pick is a step to take, not a failure.
        setStatus(args.field == QLatin1String("face") || args.field == QLatin1String("target")
                      ? QString()
                      : QStringLiteral("failed"),
                  args.error, args.field);
        return;
    }
    rejected_ = false;
    previewBase_ = d->state();
    previewBusy_ = true;
    setStatus(QStringLiteral("building"), message_, messageField_);
    d->beginPreview(command());
    if (const auto error = d->previewError()) {
        previewBusy_ = false;
        rejected_ = true;
        setStatus(QStringLiteral("failed"), qs(error->message));
    }
}

void FixturesToolModel::onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build)
{
    previewBusy_ = false;
    if (body_.isEmpty())
        return;
    if (!state || !build) {
        doc::Document* d = liveDoc();
        const auto error = d ? d->previewError() : std::nullopt;
        setStatus(QStringLiteral("failed"), error ? qs(error->message) : trs("The preview could not be built"));
    } else {
        // The feature's own report: a mate that does not resolve or is not flat fails it.
        const auto report = std::find_if(build->reports.begin(), build->reports.end(),
                                         [&](const doc::FeatureReport& r) { return feature_ && r.id == *feature_; });
        const BodyId bodyId{Uuid::fromString(body_.toStdString())};
        const auto built = build->bodies.find(bodyId);
        if (report != build->reports.end() && report->status == doc::FeatureStatus::Failed) {
            rejected_ = true;
            setStatus(QStringLiteral("failed"), qs(report->message));
        } else if (built != build->bodies.end() && !built->second.shape.isNull() && scene_) {
            api::viewer::Body shown;
            shown.id = BodyId{Uuid::fromString("f1c70000-0000-4000-8000-000000000000")}; // display only, never stored
            shown.name = placingName().toStdString();
            shown.shape = built->second.shape;
            shown.fixture = true;
            std::vector<api::viewer::Body> bodies;
            bodies.push_back(std::move(shown));
            const render::MeshStyle style{render::Token::FixtureFace, render::Token::ModelEdge, 0.85f, true, false};
            scene_->preview().show(this, std::move(bodies), {bodyId}, false, style);
            ++previewsShown_;
            const bool warned = report != build->reports.end() && report->status == doc::FeatureStatus::Warning;
            setStatus(warned ? QStringLiteral("warning") : QStringLiteral("ok"),
                      warned ? qs(report->message) : QString());
        }
    }
    if (previewPending_)
        startPreview();
}

// ---------- commit ----------
bool FixturesToolModel::canCommit() const
{
    return !body_.isEmpty() && !committing_ && !rejected_ && !pickPending_ && arguments().error.isEmpty();
}

bool FixturesToolModel::busy() const
{
    return importing_ || pickPending_ || previewBusy_ || previewTimer_.isActive() || committing_ || placingOnStock_ ||
           (scene_ && scene_->preview().owner() == this && scene_->preview().meshing());
}

bool FixturesToolModel::commit()
{
    if (body_.isEmpty() || committing_)
        return false;
    api::IApp* a = app();
    if (a == nullptr)
        return false;
    const camui::PlacementArgs args = arguments();
    if (!args.error.isEmpty()) {
        setStatus(QStringLiteral("failed"), args.error, args.field);
        return false;
    }
    const doc::Command cmd = command();
    committing_ = true;
    previewTimer_.stop();
    emit statusChanged();
    QJsonObject withDocument = jsonObject(cmd.argsJson);
    withDocument.insert(QStringLiteral("document"), QString::fromStdString(docId_));
    JobHandle<api::ActionResult> job = a->actions().invoke(cmd.name, jsonText(withDocument), api::Caller::User);
    if (!job.valid()) {
        committing_ = false;
        setStatus(QStringLiteral("failed"), trs("The placement could not be saved"));
        return false;
    }
    const QString body = body_;
    QPointer<FixturesToolModel> self(this);
    job.then([self, body](Result<api::ActionResult> r) {
        if (!self)
            return;
        self->committing_ = false;
        if (!r) {
            self->setStatus(QStringLiteral("failed"), qs(r.error().message));
            self->schedulePreview();
            return;
        }
        self->closePlacement();
        emit self->fixturesChanged();
        emit self->committed(body);
    });
    return true;
}

void FixturesToolModel::cancel()
{
    closePlacement();
}

// ---------- on stock ----------
void FixturesToolModel::setGrip(const QString& grip)
{
    const QString g = grip == QLatin1String("x") || grip == QLatin1String("y") ? grip : QStringLiteral("shorter");
    if (g == grip_)
        return;
    grip_ = g;
    emit gripChanged();
}

void FixturesToolModel::setStockStatus(const QString& status, const QString& message)
{
    stockStatus_ = status;
    stockMessage_ = message;
    emit stockStatusChanged();
}

// The vise with a library description the document has (one that is a fixture of the setup first), its moving jaw
// (one in the setup, else the first after it in the document, else any), and the setup's parallels (a described pair,
// both fixtures of the setup).
std::optional<FixturesToolModel::ViseChoice> FixturesToolModel::chooseVise(const doc::DocState& state) const
{
    const std::optional<SetupId> setupId = targetSetup();
    const cam::Setup* setup = setupId ? findSetup(state, *setupId) : nullptr;
    const auto inSetup = [&](const BodyId& id) {
        return setup != nullptr &&
               std::find(setup->fixtureBodies.begin(), setup->fixtureBodies.end(), id) != setup->fixtureBodies.end();
    };
    const auto named = [&](const std::string& name) {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < state.bodies.size(); ++i) {
            if (state.bodies[i].role == doc::Body::Role::Fixture && state.bodies[i].name == name)
                out.push_back(i);
        }
        return out;
    };
    std::optional<ViseChoice> best;
    bool bestInSetup = false;
    for (const api::cam_fixtures::FixtureModel& m : models_) {
        if (m.kind != api::cam_fixtures::FixtureModel::Kind::Vise)
            continue;
        const std::vector<std::size_t> jaws = named(m.vise.movingJaw);
        for (const std::size_t i : named(m.vise.body)) {
            const bool in = inSetup(state.bodies[i].id);
            if (best && (bestInSetup || !in))
                continue;
            std::optional<std::size_t> jaw;
            for (const std::size_t j : jaws) {
                if (!jaw && inSetup(state.bodies[j].id))
                    jaw = j;
            }
            for (const std::size_t j : jaws) {
                if (!jaw && j > i)
                    jaw = j;
            }
            if (!jaw && !jaws.empty())
                jaw = jaws.front();
            if (!jaw)
                continue;
            best = ViseChoice{m.vise, state.bodies[i].id,      state.bodies[*jaw].id, std::nullopt,
                              {},     qs(state.bodies[i].name)};
            bestInSetup = in;
        }
    }
    if (!best || setup == nullptr)
        return best;
    for (const api::cam_fixtures::FixtureModel& m : models_) {
        if (m.kind != api::cam_fixtures::FixtureModel::Kind::Parallels)
            continue;
        std::vector<BodyId> pair;
        for (const std::string& name : m.parallels.bodies) {
            for (const BodyId& b : setup->fixtureBodies) {
                const auto body = std::find_if(state.bodies.begin(), state.bodies.end(),
                                               [&](const doc::Body& x) { return x.id == b; });
                if (body != state.bodies.end() && body->name == name &&
                    std::find(pair.begin(), pair.end(), b) == pair.end()) {
                    pair.push_back(b);
                    break;
                }
            }
        }
        if (pair.size() == 2) {
            best->parallelsModel = m.parallels;
            best->parallels = pair;
            break;
        }
    }
    return best;
}

QString FixturesToolModel::viseName() const
{
    doc::Document* d = liveDoc();
    if (d == nullptr || models_.empty())
        return {};
    const std::optional<ViseChoice> choice = chooseVise(*d->state());
    return choice ? choice->name : QString();
}

bool FixturesToolModel::placeOnStock()
{
    if (placingOnStock_)
        return false;
    if (doc_ == nullptr || liveDoc() == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    api::IApp* a = app();
    if (d == nullptr || a == nullptr)
        return false;
    if (const QString reason = commandReason(QString()); !reason.isEmpty()) {
        setStockStatus(QStringLiteral("failed"), reason);
        return false;
    }
    closePlacement();
    if (models_.empty())
        loadLibrary();
    const std::optional<SetupId> setupId = targetSetup();
    const doc::DocStatePtr state = d->state();
    const cam::Setup* setup = setupId ? findSetup(*state, *setupId) : nullptr;
    if (setup == nullptr) {
        setStockStatus(QStringLiteral("failed"), trs("Add a setup first: the vise goes on its stock"));
        return false;
    }
    std::optional<ViseChoice> choice = chooseVise(*state);
    if (!choice) {
        setStockStatus(QStringLiteral("failed"),
                       trs("There is no library vise in the document: add the generic vise from the library first"));
        return false;
    }
    const doc::BuildResultPtr build = d->build();
    if (!build || d->rebuildPending() || d->builtState() != state) {
        setStockStatus(QStringLiteral("failed"), trs("The model is still being rebuilt: try again when it is done"));
        return false;
    }
    placingOnStock_ = true;
    const std::uint64_t serial = ++stockSerial_;
    setStockStatus(QString(), trs("Placing %1 on the stock of %2…").arg(choice->name, qs(setup->name)));
    // A stock from an earlier setup is what the CAM engine simulates for it (an app job with progress).
    cam::ICamEngine* engine = a->cam();
    if (std::holds_alternative<cam::FromSetupStock>(setup->stock) && engine != nullptr) {
        if (!carried_)
            carried_ = std::make_unique<camui::CarriedStockRequest>();
        QPointer<FixturesToolModel> self(this);
        const SetupId id = *setupId;
        carried_->start(*engine, state, build, {id},
                        [self, serial, c = *choice, id, state, build](api::cam_setup::CarriedStocks stocks,
                                                                      std::map<SetupId, Error> errors) {
                            if (!self || serial != self->stockSerial_)
                                return;
                            if (const auto e = errors.find(id); e != errors.end()) {
                                self->placingOnStock_ = false;
                                self->setStockStatus(
                                    e->second.code == ErrorCode::Cancelled ? QString() : QStringLiteral("failed"),
                                    trs("The stock from the earlier setup: %1").arg(qs(e->second.message)));
                                return;
                            }
                            self->planOnStock(c, id, state, build, stocks[id]);
                        });
        return true;
    }
    planOnStock(std::move(*choice), *setupId, state, build, nullptr);
    return true;
}

void FixturesToolModel::planOnStock(ViseChoice choice, SetupId setupId, doc::DocStatePtr state,
                                    doc::BuildResultPtr build, std::shared_ptr<const cam::CarriedStock> carried)
{
    using api::cam_fixtures::PlacePlan;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    api::IApp* a = app();
    if (jobs == nullptr || a == nullptr) {
        placingOnStock_ = false;
        setStockStatus(QStringLiteral("failed"), trs("The fixtures panel is not available"));
        return;
    }
    api::cam_fixtures::PlaceRequest rq;
    rq.state = state;
    rq.build = build;
    rq.setup = setupId;
    rq.viseModel = choice.model;
    rq.vise = choice.vise;
    rq.jaw = choice.jaw;
    rq.parallelsModel = choice.parallelsModel;
    rq.parallels = choice.parallels;
    rq.grip = grip_ == QLatin1String("x")   ? api::cam_fixtures::Grip::AcrossX
              : grip_ == QLatin1String("y") ? api::cam_fixtures::Grip::AcrossY
                                            : api::cam_fixtures::Grip::Shorter;
    rq.unit = (units_ ? units_->unitLabel() : QStringLiteral("mm")).toStdString();
    rq.namePattern = trs("Place %1").toStdString();
    const std::string docId = docId_;
    const std::uint64_t serial = stockSerial_;
    stockJob_ = jobs->submit<std::shared_ptr<const PlacePlan>>(
        JobInfo{"Vise on the stock", JobPriority::Interactive, false},
        [a, docId, rq, carried](JobContext& ctx) mutable -> Result<std::shared_ptr<const PlacePlan>> {
            // The setup's frame and stock box, as the setup panel shows them.
            auto frame = api::cam_setup::previewSetup(rq.state, rq.build, rq.setup, ctx, carried.get());
            if (!frame)
                return tl::unexpected(frame.error());
            rq.modelFromWcs = frame->resolved.modelFromWcs;
            rq.stock = frame->resolved.stockBounds;
            const doc::DocStatePtr base = rq.state;
            rq.buildBefore = [a, docId, base, &ctx](const FeatureId& f) -> Result<doc::BuildResultPtr> {
                const doc::DocStatePtr snapshot = api::feature_edit::stateBefore(*base, f);
                if (!snapshot)
                    return tl::unexpected(Error{ErrorCode::NotFound, "The placement is not in the document", {}});
                return api::feature_edit::buildSnapshot(*a, docId, snapshot, ctx);
            };
            auto plan = api::cam_fixtures::planViseOnStock(rq);
            if (!plan)
                return tl::unexpected(plan.error());
            return std::make_shared<const PlacePlan>(std::move(*plan));
        });
    QPointer<FixturesToolModel> self(this);
    const QString setupTitle = setupName();
    stockJob_.then([self, serial, choice, setupTitle](Result<std::shared_ptr<const PlacePlan>> r) {
        if (!self || serial != self->stockSerial_)
            return;
        self->stockJob_ = {};
        if (!r) {
            self->placingOnStock_ = false;
            self->setStockStatus(r.error().code == ErrorCode::Cancelled ? QString() : QStringLiteral("failed"),
                                 qs(r.error().message));
            return;
        }
        self->applyOnStock(**r, choice, setupTitle);
    });
}

void FixturesToolModel::applyOnStock(const api::cam_fixtures::PlacePlan& plan, const ViseChoice& choice,
                                     const QString& setupTitle)
{
    api::IApp* a = app();
    QJsonObject args = jsonObject(plan.batchArgsJson);
    args.insert(QStringLiteral("document"), QString::fromStdString(docId_));
    JobHandle<api::ActionResult> job = a != nullptr
                                           ? a->actions().invoke("cam.batch", jsonText(args), api::Caller::User)
                                           : JobHandle<api::ActionResult>{};
    if (!job.valid()) {
        placingOnStock_ = false;
        setStockStatus(QStringLiteral("failed"), trs("The placement could not be saved"));
        return;
    }
    const auto len = [this](double mm) {
        return units_ ? units_->formatLength(mm) : QLocale::c().toString(mm, 'f', 3) + QStringLiteral(" mm");
    };
    const api::cam_fixtures::ViseFit& fit = plan.fit;
    QStringList lines;
    lines << trs("%1 is on the stock of %2, gripping it across %3 (%4).")
                 .arg(choice.name, setupTitle, fit.acrossX ? QStringLiteral("X") : QStringLiteral("Y"),
                      len(fit.across));
    lines << (choice.parallelsModel ? trs("The stock stands on the parallels; the jaws hold %1 of its %2 height.")
                                    : trs("The stock stands on the vise's floor; the jaws hold %1 of its %2 height."))
                 .arg(len(std::max(fit.gripDepth, 0.0)), len(fit.height));
    bool warn = false;
    if (fit.tooWide) {
        lines << trs("The stock is %1 across the jaws, but %2 opens to %3: the moving jaw is shown fully open and does "
                     "not hold it.")
                     .arg(len(fit.across), choice.name, len(choice.model.maxOpening));
        warn = true;
    }
    if (fit.gripDepth <= 0) {
        lines << trs("The jaws do not reach the stock.");
        warn = true;
    } else if (fit.jawAboveStock > 0.0005) {
        lines << trs("The jaws stand %1 above the stock's top: the tools must clear them.").arg(len(fit.jawAboveStock));
        warn = true;
    }
    if (!plan.addedToSetup.empty())
        lines << trs("It is now a fixture of %1.").arg(setupTitle);
    const QString done = lines.join(QLatin1Char(' '));
    const bool warned = warn;
    QPointer<FixturesToolModel> self(this);
    const std::uint64_t serial = stockSerial_;
    job.then([self, serial, done, warned](Result<api::ActionResult> r) {
        if (!self || serial != self->stockSerial_)
            return;
        self->placingOnStock_ = false;
        if (!r)
            self->setStockStatus(QStringLiteral("failed"), qs(r.error().message));
        else
            self->setStockStatus(warned ? QStringLiteral("warning") : QStringLiteral("ok"), done);
        emit self->fixturesChanged();
    });
}

} // namespace ray::ui
