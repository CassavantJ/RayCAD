#include "directedit_tool.h"

#include "../app_context.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../selection_model.h"
#include "../session.h"
#include "../sketch/sketch_mode.h"
#include "../ui_commands.h"
#include "../units.h"
#include "../viewport_item.h"
#include "../viewport_navigation.h"
#include "feature_tools.h"
#include "feature_tree.h" // feature_tools.h holds a QPointer to it
#include "model_util.h"

#include <ray_api/feature_edit.h>
#include <ray_api/viewer.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QLocale>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace ray::ui {

using namespace modelui;
namespace de = directedit;

namespace {
// T-083: the direct-edit tools. Their keys work in the Design workspace while no sketch is edited (Model); Delete and
// Shift+P are free there (the sketch's Delete is its own, and Shift+P is a sketch constraint: never both at once).
// clang-format off
constexpr UiCommand kDirectEditCommands[] = {
    {"directEdit.deleteFaces", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Delete Faces"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Model"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Removes the selected faces and closes the gap: a hole (by any face of it), a pocket, a fillet or a whole blend run"), "Del", CommandContext::Model, "feature.delete_face.add"},
    {"directEdit.pushPull", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Push/Pull"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Model"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Moves the selected flat faces along their normals: a positive distance adds material, a negative one removes it"), "Shift+P", CommandContext::Model, "feature.offset_face.add"},
    {"directEdit.resizeHole", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Resize Hole"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Model"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Gives the selected hole a new diameter, by any face of its bore"), "", CommandContext::Model, "feature.resize_hole.add"},
    {"directEdit.selectBlendRun", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Select the Whole Blend Run"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Model"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Delete Faces: adds every fillet joined to the picked one by tangent blends"), "", CommandContext::Model, ""},
};
// clang-format on

constexpr const char* kTrContext = "ray::ui::DirectEdit";
constexpr const char* kPrefix = "directEdit.";

QString trd(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

// The closest point of the line p + t d (d unit) to the ray: its t.
std::optional<double> closestOnLine(Vec3 p, Vec3 d, const render::Ray& ray)
{
    const Vec3 w0 = sub(p, ray.origin);
    const double b = dot(d, ray.dir);
    const double denom = 1.0 - b * b;
    if (std::abs(denom) < 1e-9)
        return std::nullopt;
    const double d1 = dot(d, w0);
    const double e = dot(ray.dir, w0);
    return (b * e - d1) / denom;
}

// The area-weighted centre and the mean (outward, shading) normal of face `face` in a display mesh.
std::optional<std::pair<Vec3, Vec3>> faceFrame(const kernel::Mesh& mesh, std::uint32_t face)
{
    for (const kernel::FaceRange& r : mesh.faces) {
        if (r.face.index != face || r.face.kind != kernel::SubKind::Face)
            continue;
        Vec3 centre{0, 0, 0}, normal{0, 0, 0};
        double area = 0;
        const auto at = [&](std::uint32_t i) {
            return Vec3{mesh.positions[3 * i], mesh.positions[3 * i + 1], mesh.positions[3 * i + 2]};
        };
        for (std::uint32_t k = r.firstIndex; k + 2 < r.firstIndex + r.indexCount && k + 2 < mesh.indices.size();
             k += 3) {
            const std::uint32_t ia = mesh.indices[k], ib = mesh.indices[k + 1], ic = mesh.indices[k + 2];
            if (3 * std::max({ia, ib, ic}) + 2 >= mesh.positions.size())
                continue;
            const Vec3 a = at(ia), b = at(ib), c = at(ic);
            const double tri = 0.5 * length(cross(sub(b, a), sub(c, a)));
            centre = add(centre, mul(add(add(a, b), c), tri / 3.0));
            area += tri;
            for (const std::uint32_t i : {ia, ib, ic}) {
                if (3 * i + 2 < mesh.normals.size())
                    normal = add(normal,
                                 mul(Vec3{mesh.normals[3 * i], mesh.normals[3 * i + 1], mesh.normals[3 * i + 2]}, tri));
            }
        }
        if (area <= 0 || length(normal) < 1e-12)
            return std::nullopt;
        // Flat faces only (push/pull's): every shading normal of the face along the mean. A curved face gets no
        // arrow; the kernel says why it cannot be pushed.
        const Vec3 mean = unit(normal);
        for (std::uint32_t k = r.firstIndex; k < r.firstIndex + r.indexCount && k < mesh.indices.size(); ++k) {
            const std::uint32_t i = mesh.indices[k];
            if (3 * i + 2 >= mesh.normals.size())
                continue;
            const Vec3 n = unit(Vec3{mesh.normals[3 * i], mesh.normals[3 * i + 1], mesh.normals[3 * i + 2]});
            if (dot(n, mean) < 0.9999)
                return std::nullopt;
        }
        return std::pair<Vec3, Vec3>{mul(centre, 1.0 / area), mean};
    }
    return std::nullopt;
}

} // namespace

std::span<const UiCommand> directEditUiCommands()
{
    return kDirectEditCommands;
}

// ---------- construction ----------
DirectEditModel::DirectEditModel(SceneController* scene, SelectionModel* selection, Session* session,
                                 AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session), context_(context)
{
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(0);
    connect(&previewTimer_, &QTimer::timeout, this, [this] { startPreview(); });
    busyTick_.setInterval(250);
    connect(&busyTick_, &QTimer::timeout, this, [this] {
        const int s = static_cast<int>(busyClock_.elapsed() / 1000);
        if (s != busySeconds_) {
            busySeconds_ = s;
            emit busyChanged();
        }
    });
    if (scene_) {
        connect(&scene_->preview(), &BodyOverlay::shown, this, [this] {
            if (scene_ && scene_->preview().owner() == this)
                emit statusChanged();
        });
        // The arrow follows the face once its mesh is there.
        connect(scene_, &SceneController::stateChanged, this, [this] {
            if (active() && tool_ == de::Tool::PushPull && !arrowBase_)
                updateArrow();
        });
    }
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
    if (selection_) {
        connect(selection_, &SelectionModel::changed, this, [this] {
            if (active() && !committing_)
                applySelection();
        });
    }
    if (context_) {
        // The direct edits are the Design workspace's: another workspace closes the tool (nothing changes).
        connect(context_, &AppContext::workspaceChanged, this, [this] {
            if (active() && context_ && context_->workspace() != 0)
                cancel();
        });
    }
}

DirectEditModel::~DirectEditModel()
{
    if (commands_) {
        commands_->setUiHandler(QString::fromLatin1(kPrefix), {});
        for (const de::Tool t : {de::Tool::Delete, de::Tool::PushPull, de::Tool::Resize})
            commands_->setUiHandler(QStringLiteral("feature.%1.add").arg(de::featureType(t)), {});
    }
    if (doc::Document* d = liveDoc(); d != nullptr && active())
        d->cancelPreview();
    if (scene_)
        scene_->preview().clear(this);
    unsubscribe();
}

DirectEditModel* DirectEditModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    UnitsObject* units = nullptr;
    FeatureToolModel* featureTool = nullptr;
    SketchModel* sketch = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
        featureTool = qml->singletonInstance<FeatureToolModel*>(uri, QStringLiteral("FeatureTool"));
        sketch = qml->singletonInstance<SketchModel*>(uri, QStringLiteral("SketchTool"));
    }
    auto* model = new DirectEditModel(scene, selection, session, context);
    model->setCommands(commands);
    model->setUnits(units);
    model->setFeatureTool(featureTool);
    model->setSketchTool(sketch);
    model->subscribe();
    return model; // owned by the engine
}

void DirectEditModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<DirectEditModel> self(this);
    const CommandCatalog::UiHandler handler{[self](const QString& id) {
                                                return self ? self->commandReason(id)
                                                            : trd("The direct edits are not available");
                                            },
                                            [self](const QString& id) { return self && self->runCommand(id); }};
    commands_->setUiHandler(QString::fromLatin1(kPrefix), handler);
    for (const de::Tool t : {de::Tool::Delete, de::Tool::PushPull, de::Tool::Resize})
        commands_->setUiHandler(QStringLiteral("feature.%1.add").arg(de::featureType(t)), handler);
}

void DirectEditModel::setUnits(UnitsObject* units)
{
    units_ = units;
}

void DirectEditModel::setFeatureTool(FeatureToolModel* tool)
{
    if (featureTool_)
        disconnect(featureTool_, nullptr, this, nullptr);
    featureTool_ = tool;
    if (!featureTool_)
        return;
    // One tool at a time: a feature tool opening closes this one (their Esc and Enter would clash).
    connect(featureTool_, &FeatureToolModel::activeChanged, this, [this] {
        if (featureTool_ && featureTool_->active() && active())
            cancel();
    });
}

void DirectEditModel::setSketchTool(SketchModel* sketch)
{
    if (sketchTool_)
        disconnect(sketchTool_, nullptr, this, nullptr);
    sketchTool_ = sketch;
    if (!sketchTool_)
        return;
    const auto closeForSketch = [this] {
        if (sketchTool_ && (sketchTool_->active() || sketchTool_->placing()) && active())
            cancel();
    };
    connect(sketchTool_, &SketchModel::activeChanged, this, closeForSketch);
    connect(sketchTool_, &SketchModel::placingChanged, this, closeForSketch);
}

// ---------- document ----------
api::IApp* DirectEditModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

doc::Document* DirectEditModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void DirectEditModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<DirectEditModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->onDocumentChanged();
    });
    previewListener_ = doc_->subscribePreview([self, generation](doc::DocStatePtr state, doc::BuildResultPtr build) {
        if (self && self->docGeneration_ == generation && self->previewBusy_)
            self->onPreviewBuilt(std::move(state), std::move(build));
    });
}

void DirectEditModel::unsubscribe()
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

void DirectEditModel::onDocumentChanged()
{
    if (!active() || committing_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr) {
        close();
        return;
    }
    // An edited feature that is gone (undone): the tool closes.
    if (editId_ && std::none_of(d->state()->features.begin(), d->state()->features.end(),
                                [&](const doc::Feature& f) { return f.id == *editId_; })) {
        close();
        return;
    }
    // The state changed under the tool (an undo, a command from elsewhere): that ended the preview; it is built again
    // on the new state. A rebuild of the same state finishing changes nothing here.
    if (d->state() == previewBase_)
        return;
    previewBusy_ = false;
    schedulePreview();
}

// ---------- commands ----------
QString DirectEditModel::commandReason(const QString& id) const
{
    api::IApp* a = app();
    if (a == nullptr)
        return trd("There is no session");
    api::OpenDocument* od = a->active();
    if (od == nullptr)
        return trd("Open or import a part first");
    if (od->readOnly)
        return trd("The document is read-only");
    if (context_ && context_->workspace() != 0)
        return trd("Direct edits are made in the Design workspace");
    if (sketchTool_ && (sketchTool_->active() || sketchTool_->placing()))
        return trd("Finish the sketch first");
    if (od->document->state()->bodies.empty())
        return trd("There is no body yet");
    if (id == QLatin1String("directEdit.selectBlendRun")) {
        if (!active() || tool_ != de::Tool::Delete)
            return trd("Open Delete Faces on a fillet first");
        return blendRunCount() > 0 ? QString() : trd("The picked face is not a fillet that joins other blends");
    }
    de::Tool tool = de::Tool::None;
    if (id == QLatin1String("directEdit.deleteFaces") || id == QLatin1String("feature.delete_face.add"))
        tool = de::Tool::Delete;
    else if (id == QLatin1String("directEdit.pushPull") || id == QLatin1String("feature.offset_face.add"))
        tool = de::Tool::PushPull;
    else if (id == QLatin1String("directEdit.resizeHole") || id == QLatin1String("feature.resize_hole.add"))
        tool = de::Tool::Resize;
    if (tool == de::Tool::None)
        return trd("Unknown direct-edit command");
    // Delete is the Delete key: it acts on a selection, never opens on nothing.
    if (tool == de::Tool::Delete && !(active() && tool_ == de::Tool::Delete)) {
        const bool faces = selection_ && std::any_of(selection_->entries().begin(), selection_->entries().end(),
                                                     [](const SelectionEntry& e) {
                                                         return e.sub && e.sub->kind == kernel::SubKind::Face;
                                                     });
        if (!faces)
            return trd("Select the faces to delete first");
    }
    return {};
}

bool DirectEditModel::runCommand(const QString& id)
{
    if (id == QLatin1String("directEdit.selectBlendRun")) {
        selectBlendRun();
        return true;
    }
    if (id == QLatin1String("directEdit.deleteFaces") || id == QLatin1String("feature.delete_face.add"))
        return start(QStringLiteral("delete"));
    if (id == QLatin1String("directEdit.pushPull") || id == QLatin1String("feature.offset_face.add"))
        return start(QStringLiteral("pushPull"));
    if (id == QLatin1String("directEdit.resizeHole") || id == QLatin1String("feature.resize_hole.add"))
        return start(QStringLiteral("resize"));
    return false;
}

bool DirectEditModel::start(const QString& toolName)
{
    const de::Tool tool = de::toolOfKey(toolName);
    if (tool == de::Tool::None)
        return false;
    if (active() && tool_ == tool)
        return true; // already open: the key again changes nothing
    const QString id = tool == de::Tool::Delete     ? QStringLiteral("directEdit.deleteFaces")
                       : tool == de::Tool::PushPull ? QStringLiteral("directEdit.pushPull")
                                                    : QStringLiteral("directEdit.resizeHole");
    if (const QString reason = commandReason(id); !reason.isEmpty()) {
        if (context_)
            context_->showMessage(reason, true);
        return false;
    }
    open(tool);
    return active();
}

QString DirectEditModel::title() const
{
    if (editId_)
        return trd("Edit %1").arg(editName_);
    return de::toolTitle(tool_);
}

bool DirectEditModel::edit(const QString& featureId)
{
    if (doc_ == nullptr || app() == nullptr || app()->findDocument(docId_) == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return false;
    const FeatureId id{Uuid::fromString(featureId.toStdString())};
    const doc::DocStatePtr state = d->state();
    const auto f =
        std::find_if(state->features.begin(), state->features.end(), [&](const doc::Feature& x) { return x.id == id; });
    if (f == state->features.end())
        return false;
    const de::Tool tool = de::toolOfFeatureType(qs(f->type));
    if (tool != de::Tool::PushPull && tool != de::Tool::Resize)
        return false;
    if (const QString reason = commandReason(QStringLiteral("directEdit.resizeHole")); !reason.isEmpty()) {
        if (context_)
            context_->showMessage(reason, true);
        return false;
    }
    open(tool, id);
    return active();
}

// ---------- open and close ----------
void DirectEditModel::open(de::Tool tool, std::optional<FeatureId> editId)
{
    if (active())
        close();
    if (featureTool_ && featureTool_->active())
        featureTool_->cancel();
    if (doc_ == nullptr || app() == nullptr || app()->findDocument(docId_) == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return;
    tool_ = tool;
    editId_ = editId;
    id_ = editId ? *editId : FeatureId{Uuid::generate()};
    editRefs_ = QJsonArray();
    editName_.clear();
    flow_.open();
    values_.clear();
    message_.clear();
    hint_.clear();
    picks_.clear();
    refsPending_ = 0;
    offerRun_.clear();
    currentDiameter_.reset();
    diameterTouched_ = false;
    arrowBase_.reset();
    arrowDir_.reset();
    dragKey_.clear();
    previewBusy_ = previewPending_ = false;
    previewBuilds_ = 0;
    committing_ = false;
    if (tool_ == de::Tool::PushPull)
        values_[QStringLiteral("distance")] = inch() ? QStringLiteral("0.05 in") : QStringLiteral("1 mm");
    else if (tool_ == de::Tool::Resize)
        values_[QStringLiteral("diameter")] = QString();
    if (editId_) {
        // The feature's own faces and value (an expression as it is, a number in mm).
        const doc::DocStatePtr state = d->state();
        for (const doc::Feature& f : state->features) {
            if (f.id != *editId_)
                continue;
            editName_ = qs(f.name);
            const QJsonObject p = jsonObject(f.propsJson);
            if (p.contains(QStringLiteral("faces")))
                editRefs_ = p.value(QStringLiteral("faces")).toArray();
            else if (p.contains(QStringLiteral("face")))
                editRefs_.append(p.value(QStringLiteral("face")));
            const QString key = tool_ == de::Tool::PushPull ? QStringLiteral("distance") : QStringLiteral("diameter");
            const QJsonValue v = p.value(key);
            values_[key] =
                v.isString() ? v.toString() : QLocale::c().toString(v.toDouble(), 'g', 10) + QStringLiteral(" mm");
        }
        diameterTouched_ = true;
        emit activeChanged();
        emit valuesChanged();
        emit picksChanged();
        schedulePreview();
        emit statusChanged();
        return;
    }
    if (selection_) {
        savedFilter_ = selection_->filterMask();
        filterSaved_ = true;
        selection_->setFilterMask(static_cast<std::uint8_t>(render::PickFilter::Faces));
    }
    emit activeChanged();
    emit valuesChanged();
    applySelection();
    emit statusChanged();
}

void DirectEditModel::close()
{
    const bool was = active();
    previewTimer_.stop();
    if (doc::Document* d = liveDoc(); d != nullptr && was)
        d->cancelPreview();
    if (scene_)
        scene_->preview().clear(this);
    if (filterSaved_ && selection_)
        selection_->setFilterMask(savedFilter_);
    filterSaved_ = false;
    flow_.close();
    tool_ = de::Tool::None;
    id_.reset();
    editId_.reset();
    editRefs_ = QJsonArray();
    editName_.clear();
    values_.clear();
    message_.clear();
    hint_.clear();
    picks_.clear();
    ++pickSerial_; // references still on their way are dropped
    refsPending_ = 0;
    offerRun_.clear();
    ++offerSerial_;
    ++diameterSerial_;
    currentDiameter_.reset();
    arrowBase_.reset();
    arrowDir_.reset();
    dragKey_.clear();
    previewBusy_ = previewPending_ = false;
    committing_ = false;
    setBusy(false);
    if (was) {
        emit activeChanged();
        emit valuesChanged();
        emit statusChanged();
        emit picksChanged();
        emit offerChanged();
        emit handlesChanged();
    }
}

void DirectEditModel::cancel()
{
    close();
}

// ---------- picks ----------
int DirectEditModel::sceneIndexOf(const BodyId& body) const
{
    if (!scene_)
        return -1;
    for (int i = 0; i < scene_->bodyCount(); ++i) {
        const api::viewer::Body* b = scene_->body(i);
        if (b != nullptr && b->id == body)
            return i;
    }
    return -1;
}

void DirectEditModel::applySelection()
{
    api::IApp* a = app();
    if (!active() || a == nullptr || !selection_ || !scene_ || editId_)
        return; // editing: the feature's own faces stay
    // The cache of references is for the body set shown.
    const void* shown = scene_->snapshot().get();
    if (shown != refCacheFor_) {
        refCache_.clear();
        runCache_.clear();
        refCacheFor_ = shown;
    }
    std::vector<Pick> picks;
    for (const SelectionEntry& e : selection_->entries()) {
        if (!e.sub || e.sub->kind != kernel::SubKind::Face)
            continue;
        const api::viewer::Body* body = scene_->body(e.body);
        if (body == nullptr)
            continue;
        // Push/pull and delete edit one body (the first picked); resize one face.
        if (!picks.empty() && body->id != picks.front().body)
            continue;
        Pick p;
        p.body = body->id;
        p.index = e.sub->index;
        p.point = e.hit.point;
        if (const auto it = refCache_.find({p.body, p.index}); it != refCache_.end())
            p.ref = it->second;
        picks.push_back(std::move(p));
        if (tool_ == de::Tool::Resize)
            break;
    }
    const bool facesChanged = picks.size() != picks_.size() ||
                              !std::equal(picks.begin(), picks.end(), picks_.begin(), [](const Pick& x, const Pick& y) {
                                  return x.body == y.body && x.index == y.index;
                              });
    picks_ = std::move(picks);
    const std::uint64_t serial = ++pickSerial_;
    refsPending_ = 0;
    for (std::size_t i = 0; i < picks_.size(); ++i) {
        if (!picks_[i].ref.isEmpty())
            continue;
        const QJsonObject args{{QStringLiteral("body"), idText(picks_[i].body.value)},
                               {QStringLiteral("kind"), QStringLiteral("face")},
                               {QStringLiteral("index"), static_cast<int>(picks_[i].index)}};
        JobHandle<api::ActionResult> job = a->actions().invoke("model.reference", jsonText(args), api::Caller::User);
        if (!job.valid())
            continue;
        ++refsPending_;
        QPointer<DirectEditModel> self(this);
        const BodyId body = picks_[i].body;
        const std::uint32_t index = picks_[i].index;
        job.then([self, serial, body, index](Result<api::ActionResult> r) {
            if (!self || serial != self->pickSerial_)
                return;
            --self->refsPending_;
            if (r) {
                const QJsonObject ref = jsonObject(r->resultJson).value(QStringLiteral("ref")).toObject();
                self->refCache_[{body, index}] = ref;
                for (Pick& p : self->picks_) {
                    if (p.body == body && p.index == index)
                        p.ref = ref;
                }
            } else {
                self->setMessage(qs(r.error().message));
            }
            if (self->refsPending_ == 0)
                self->refsArrived(serial);
        });
    }
    emit picksChanged();
    if (facesChanged) {
        updateArrow();
        if (tool_ == de::Tool::Resize)
            loadDiameter();
        updateBlendOffer();
    }
    if (refsPending_ == 0)
        refsArrived(serial);
}

void DirectEditModel::refsArrived(std::uint64_t serial)
{
    if (serial != pickSerial_ || !active())
        return;
    // Picks whose reference failed are left out.
    picks_.erase(std::remove_if(picks_.begin(), picks_.end(), [](const Pick& p) { return p.ref.isEmpty(); }),
                 picks_.end());
    emit picksChanged();
    schedulePreview();
}

QJsonArray DirectEditModel::faceRefs() const
{
    if (editId_)
        return editRefs_;
    QJsonArray out;
    for (const Pick& p : picks_) {
        if (!p.ref.isEmpty())
            out.append(p.ref);
    }
    return out;
}

QString DirectEditModel::pickText() const
{
    if (editId_)
        return trd("The faces it was made with");
    if (picks_.empty())
        return trd("No face picked");
    if (tool_ == de::Tool::Resize)
        return trd("A face of the hole");
    return picks_.size() == 1 ? trd("1 face") : trd("%1 faces").arg(picks_.size());
}

QString DirectEditModel::pickHint() const
{
    if (editId_)
        return trd("Change the value; the faces stay as they are.");
    switch (tool_) {
    case de::Tool::Delete:
        return trd("Click faces in the view (Shift adds, Ctrl toggles). Any face of a hole removes the whole hole.");
    case de::Tool::PushPull:
        return trd("Click flat faces in the view (Shift adds, Ctrl toggles), then drag the arrow or type a distance.");
    case de::Tool::Resize:
        return trd("Click any face of the hole's bore in the view.");
    case de::Tool::None:
        break;
    }
    return {};
}

// ---------- blend runs ----------
void DirectEditModel::updateBlendOffer()
{
    const bool had = !offerRun_.empty();
    offerRun_.clear();
    const std::uint64_t serial = ++offerSerial_;
    api::IApp* a = app();
    if (tool_ != de::Tool::Delete || picks_.empty() || a == nullptr) {
        if (had)
            emit offerChanged();
        return;
    }
    // The first picked face whose run is not wholly picked.
    for (const Pick& p : picks_) {
        const auto cached = runCache_.find({p.body, p.index});
        if (cached == runCache_.end())
            continue;
        const auto& run = cached->second;
        const bool all = std::all_of(run.begin(), run.end(), [&](std::uint32_t f) {
            return std::any_of(picks_.begin(), picks_.end(), [&](const Pick& q) { return q.index == f; });
        });
        if (run.size() > 1 && !all) {
            offerRun_ = run;
            offerBody_ = p.body;
            break;
        }
    }
    if (had || !offerRun_.empty())
        emit offerChanged();
    // Faces not asked about yet: one model.blendRun each.
    for (const Pick& p : picks_) {
        if (runCache_.count({p.body, p.index}) != 0)
            continue;
        const QJsonObject args{{QStringLiteral("body"), idText(p.body.value)},
                               {QStringLiteral("index"), static_cast<int>(p.index)}};
        JobHandle<api::ActionResult> job = a->actions().invoke("model.blendRun", jsonText(args), api::Caller::User);
        if (!job.valid())
            continue;
        QPointer<DirectEditModel> self(this);
        const auto key = std::make_pair(p.body, p.index);
        const void* forScene = refCacheFor_;
        job.then([self, serial, key, forScene](Result<api::ActionResult> r) {
            if (!self || !self->active() || forScene != self->refCacheFor_)
                return;
            std::vector<std::uint32_t> run;
            if (r) {
                for (const QJsonValue& v : jsonObject(r->resultJson).value(QStringLiteral("faces")).toArray())
                    run.push_back(static_cast<std::uint32_t>(v.toInt()));
            }
            self->runCache_[key] = std::move(run);
            if (serial == self->offerSerial_)
                self->updateBlendOffer();
        });
        break; // one at a time: the answer asks for the next
    }
}

void DirectEditModel::selectBlendRun()
{
    if (!active() || tool_ != de::Tool::Delete || offerRun_.size() < 2 || !selection_ || !scene_)
        return;
    const int index = sceneIndexOf(offerBody_);
    const render::ItemId item = scene_->itemForBody(index);
    if (item == 0)
        return;
    std::vector<render::PickHit> hits;
    for (const std::uint32_t f : offerRun_) {
        const bool picked = std::any_of(picks_.begin(), picks_.end(), [&](const Pick& p) { return p.index == f; });
        if (!picked)
            hits.push_back(render::PickHit{item, kernel::SubKind::Face, f, Vec3{}, 0});
    }
    selection_->applyBox(hits, Qt::ShiftModifier); // added to the selection; its change makes the picks
}

// ---------- resize: the hole's present diameter ----------
void DirectEditModel::loadDiameter()
{
    currentDiameter_.reset();
    const std::uint64_t serial = ++diameterSerial_;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    const int index = picks_.empty() ? -1 : sceneIndexOf(picks_.front().body);
    const api::viewer::Body* body = scene_ && index >= 0 ? scene_->body(index) : nullptr;
    if (jobs == nullptr || body == nullptr) {
        hint_.clear();
        emit statusChanged();
        return;
    }
    const kernel::Shape shape = body->shape;
    const kernel::SubId face{kernel::SubKind::Face, picks_.front().index};
    JobHandle<double> job = jobs->submit<double>(
        JobInfo{"Hole size", JobPriority::Interactive, false}, [shape, face](JobContext& ctx) -> Result<double> {
            auto info = api::viewer::describeElement(shape, face, ctx);
            if (!info)
                return tl::unexpected(info.error());
            if (!info->surface || info->surface->type != kernel::SurfaceType::Cylinder)
                return -1.0;
            return 2.0 * info->surface->radius;
        });
    QPointer<DirectEditModel> self(this);
    job.then([self, serial](Result<double> d) {
        if (!self || serial != self->diameterSerial_ || !self->active())
            return;
        if (d && *d > 0) {
            self->currentDiameter_ = *d;
            const QString text = de::lengthText(*d, self->inch(), true);
            self->hint_ = trd("The hole is %1 across now").arg(text);
            if (!self->diameterTouched_) {
                self->values_[QStringLiteral("diameter")] = text;
                emit self->valuesChanged();
            }
        } else {
            self->hint_ = trd("Type the new diameter (this face does not show the hole's size)");
        }
        emit self->statusChanged();
        self->schedulePreview();
    });
}

// ---------- push/pull: the arrow ----------
void DirectEditModel::updateArrow()
{
    arrowBase_.reset();
    arrowDir_.reset();
    if (tool_ == de::Tool::PushPull && !picks_.empty() && scene_) {
        const int index = sceneIndexOf(picks_.front().body);
        if (const auto mesh = index >= 0 ? scene_->meshOf(index) : nullptr) {
            if (const auto frame = faceFrame(*mesh, picks_.front().index)) {
                arrowBase_ = frame->first;
                arrowDir_ = frame->second;
            }
        }
    }
    emit handlesChanged();
}

std::optional<std::pair<Vec3, Vec3>> DirectEditModel::arrow() const
{
    if (!arrowBase_ || !arrowDir_)
        return std::nullopt;
    return std::pair<Vec3, Vec3>{*arrowBase_, *arrowDir_};
}

QVariantList DirectEditModel::handles() const
{
    QVariantList out;
    if (!active() || tool_ != de::Tool::PushPull || !arrowBase_ || !arrowDir_ || !view_)
        return out;
    const double d = numberOf(QStringLiteral("distance")).value_or(0);
    const auto b = toView(*arrowBase_);
    auto t = toView(add(*arrowBase_, mul(*arrowDir_, d)));
    const auto unitTip = toView(add(*arrowBase_, *arrowDir_));
    if (!b || !t || !unitTip)
        return out;
    // At least 40 pixels long on screen, so there is something to grab when the distance is small.
    const double px = std::hypot(t->x() - b->x(), t->y() - b->y());
    constexpr double kMinPx = 40;
    if (px < kMinPx) {
        QPointF dir = *unitTip - *b;
        const double len = std::hypot(dir.x(), dir.y());
        if (len > 1e-9) {
            dir /= len;
            const double sign = d < 0 ? -1.0 : 1.0;
            t = *b + dir * (kMinPx * sign);
        }
    }
    out.append(QVariantMap{{QStringLiteral("key"), QStringLiteral("distance")},
                           {QStringLiteral("kind"), QStringLiteral("arrow")},
                           {QStringLiteral("x"), b->x()},
                           {QStringLiteral("y"), b->y()},
                           {QStringLiteral("tipX"), t->x()},
                           {QStringLiteral("tipY"), t->y()}});
    return out;
}

void DirectEditModel::dragHandle(const QString& key, qreal x, qreal y, bool finished, bool fine)
{
    if (!active() || tool_ != de::Tool::PushPull || key != QLatin1String("distance") || !arrowBase_ || !arrowDir_)
        return;
    if (const auto ray = rayAt(QPointF(x, y))) {
        if (const auto t = closestOnLine(*arrowBase_, *arrowDir_, *ray)) {
            if (dragKey_.isEmpty()) {
                // The drag moves the value by how far the pointer moves along the arrow, from where it took hold.
                dragKey_ = key;
                dragT0_ = *t;
                dragV0_ = numberOf(QStringLiteral("distance")).value_or(0);
            }
            const double v = de::snapLength(dragV0_ + (*t - dragT0_), inch(), fine);
            const QString text = de::lengthText(v, inch(), fine);
            if (values_.value(QStringLiteral("distance")).toString() != text) {
                values_[QStringLiteral("distance")] = text;
                emit valuesChanged();
                schedulePreview();
            }
            emit handlesChanged();
        }
    }
    if (finished) {
        dragKey_.clear();
        emit handlesChanged();
    }
}

QObject* DirectEditModel::view() const
{
    return view_;
}

void DirectEditModel::setView(QObject* view)
{
    auto* item = qobject_cast<ViewportItem*>(view);
    if (item == view_)
        return;
    for (const auto& c : viewConnections_)
        disconnect(c);
    viewConnections_.clear();
    view_ = item;
    if (view_) {
        viewConnections_ << connect(view_, &ViewportItem::cameraChanged, this, [this] {
            if (active())
                emit handlesChanged();
        });
        viewConnections_ << connect(view_, &QQuickItem::widthChanged, this, [this] {
            if (active())
                emit handlesChanged();
        });
        viewConnections_ << connect(view_, &QQuickItem::heightChanged, this, [this] {
            if (active())
                emit handlesChanged();
        });
    }
    emit viewChanged();
}

std::optional<QPointF> DirectEditModel::toView(Vec3 world) const
{
    if (!view_)
        return std::nullopt;
    return nav::project(view_->camera(), world, view_->width(), view_->height());
}

std::optional<render::Ray> DirectEditModel::rayAt(QPointF viewPos) const
{
    if (!view_ || view_->width() <= 0 || view_->height() <= 0)
        return std::nullopt;
    return render::cameraRay(view_->camera(), viewPos.x(), viewPos.y(), static_cast<int>(view_->width()),
                             static_cast<int>(view_->height()));
}

// ---------- values ----------
bool DirectEditModel::inch() const
{
    if (units_)
        return units_->system() == UnitSystem::Inch;
    doc::Document* d = liveDoc();
    return d != nullptr && d->state()->settings.units == UnitSystem::Inch;
}

void DirectEditModel::setValue(const QString& key, const QString& text)
{
    if (!active() || values_.value(key).toString() == text)
        return;
    values_[key] = text;
    if (key == QLatin1String("diameter"))
        diameterTouched_ = true;
    emit valuesChanged();
    emit handlesChanged();
    schedulePreview();
}

QString DirectEditModel::exprOf(const QString& key) const
{
    const QString text = values_.value(key).toString().trimmed();
    if (text.isEmpty())
        return {};
    bool ok = false;
    QLocale::c().toDouble(text, &ok);
    if (!ok)
        return text; // an expression, a number with a unit, a parameter name
    return text + QLatin1Char(' ') + (inch() ? QStringLiteral("in") : QStringLiteral("mm"));
}

std::optional<double> DirectEditModel::numberOf(const QString& key) const
{
    doc::Document* d = liveDoc();
    const QString e = exprOf(key);
    if (d == nullptr || e.isEmpty())
        return std::nullopt;
    auto v = api::feature_edit::evaluate(*d->state(), e.toStdString(), false);
    return v ? std::optional<double>(*v) : std::nullopt;
}

Result<std::string> DirectEditModel::propsJson() const
{
    const auto fail = [](const QString& m) {
        return Result<std::string>(tl::unexpected(Error{ErrorCode::InvalidArgument, m.toStdString(), {}}));
    };
    const QJsonArray faces = faceRefs();
    if (faces.isEmpty())
        return fail(tool_ == de::Tool::Resize ? trd("Pick a face of the hole in the view")
                                              : trd("Pick faces in the view"));
    QJsonObject p;
    switch (tool_) {
    case de::Tool::Delete:
        p.insert(QStringLiteral("faces"), faces);
        break;
    case de::Tool::PushPull: {
        const QString e = exprOf(QStringLiteral("distance"));
        if (e.isEmpty())
            return fail(trd("Give a distance: positive adds material, negative removes it"));
        p.insert(QStringLiteral("faces"), faces);
        p.insert(QStringLiteral("distance"), e);
        break;
    }
    case de::Tool::Resize: {
        const QString e = exprOf(QStringLiteral("diameter"));
        if (e.isEmpty() || !diameterTouched_)
            return fail(trd("Type the new diameter"));
        p.insert(QStringLiteral("face"), faces.first());
        p.insert(QStringLiteral("diameter"), e);
        break;
    }
    case de::Tool::None:
        return fail(trd("No direct edit is open"));
    }
    return jsonText(p);
}

Result<doc::Command> DirectEditModel::command() const
{
    auto props = propsJson();
    if (!props)
        return tl::unexpected(props.error());
    doc::Document* d = liveDoc();
    if (d == nullptr || !id_)
        return tl::unexpected(Error{ErrorCode::NotFound, trd("The document was closed").toStdString(), {}});
    const std::string type = de::featureType(tool_).toStdString();
    if (editId_) {
        // An edit names the value only: the faces stay as stored.
        QJsonObject patch = jsonObject(*props);
        patch.remove(QStringLiteral("faces"));
        patch.remove(QStringLiteral("face"));
        doc::Command c;
        c.name = "feature." + type + ".edit";
        c.argsJson =
            jsonText(QJsonObject{{QStringLiteral("id"), idText(editId_->value)}, {QStringLiteral("props"), patch}});
        c.label = title().toStdString();
        return c;
    }
    // The feature's name: the tool's title and the next number of its type ("Push/Pull 2").
    const doc::DocStatePtr state = d->state();
    const auto count = std::count_if(state->features.begin(), state->features.end(),
                                     [&](const doc::Feature& f) { return f.type == type; });
    const QString name = QStringLiteral("%1 %2").arg(title()).arg(count + 1);
    QJsonObject args{{QStringLiteral("id"), idText(id_->value)},
                     {QStringLiteral("name"), name},
                     {QStringLiteral("props"), jsonObject(*props)}};
    doc::Command c;
    c.name = "feature." + type + ".add";
    c.argsJson = jsonText(args);
    c.label = name.toStdString();
    return c;
}

// ---------- preview ----------
bool DirectEditModel::previewBusy() const
{
    const bool meshing = scene_ && scene_->preview().owner() == this && scene_->preview().meshing();
    return refsPending_ > 0 || previewTimer_.isActive() || previewBusy_ || previewPending_ || meshing;
}

const BodyOverlay& DirectEditModel::overlay() const
{
    static BodyOverlay* const none = new BodyOverlay(nullptr); // without a scene: an empty slot (never freed)
    return scene_ ? scene_->preview() : *none;
}

void DirectEditModel::setMessage(const QString& message)
{
    if (message == message_)
        return;
    message_ = message;
    emit statusChanged();
}

void DirectEditModel::setBusy(bool busy)
{
    if (busy) {
        if (!busyTick_.isActive()) {
            busyClock_.start();
            busyTick_.start();
        }
        return;
    }
    busyTick_.stop();
    if (busySeconds_ != 0) {
        busySeconds_ = 0;
        emit busyChanged();
    }
}

void DirectEditModel::schedulePreview()
{
    if (!active() || committing_)
        return;
    if (refsPending_ > 0)
        return; // refsArrived schedules it
    auto cmd = command();
    if (!cmd) {
        // Nothing to preview yet (no face, no value): the model as it is shows.
        flow_.incomplete();
        previewTimer_.stop();
        if (scene_)
            scene_->preview().clear(this);
        if (doc::Document* d = liveDoc())
            d->cancelPreview();
        previewBusy_ = previewPending_ = false;
        setBusy(false);
        message_ = qs(cmd.error().message); // what is missing (shown plainly: the phase is Picking)
        emit statusChanged();
        return;
    }
    flow_.request();
    emit statusChanged();
    previewTimer_.start();
}

void DirectEditModel::startPreview()
{
    if (!active() || committing_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return;
    if (previewBusy_) {
        previewPending_ = true; // the newest values follow when this one is in
        return;
    }
    previewPending_ = false;
    auto cmd = command();
    if (!cmd) {
        schedulePreview();
        return;
    }
    previewSerial_ = flow_.serial();
    previewBase_ = d->state();
    previewBusy_ = true;
    setBusy(true);
    d->beginPreview(*cmd);
    if (const auto error = d->previewError()) {
        previewBusy_ = false;
        setBusy(false);
        flow_.refuse();
        if (scene_)
            scene_->preview().clear(this);
        message_ = qs(error->message);
        emit statusChanged();
    }
}

void DirectEditModel::onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build)
{
    previewBusy_ = false;
    ++previewBuilds_;
    if (!active())
        return;
    de::Outcome outcome = de::Outcome::Refused;
    QString message;
    if (!state || !build) {
        doc::Document* d = liveDoc();
        const auto error = d ? d->previewError() : std::nullopt;
        message = error ? qs(error->message) : trd("The preview could not be built");
    } else {
        const auto report = std::find_if(build->reports.begin(), build->reports.end(),
                                         [&](const doc::FeatureReport& r) { return id_ && r.id == *id_; });
        if (report == build->reports.end()) {
            message = trd("The edit was not built");
        } else if (report->status == doc::FeatureStatus::Failed || report->status == doc::FeatureStatus::NotBuilt) {
            message = qs(report->message);
            if (message.isEmpty())
                message = trd("The edit could not be made");
        } else {
            outcome = report->status == doc::FeatureStatus::Warning ? de::Outcome::Warning : de::Outcome::Ok;
            message = outcome == de::Outcome::Warning ? qs(report->message) : QString();
        }
    }
    const bool current = flow_.result(previewSerial_, outcome);
    if (outcome != de::Outcome::Refused) {
        showPreview(*state, *build); // also an older one: the view follows the drag
    } else if (current && scene_) {
        scene_->preview().clear(this); // a refusal shows the model as it is: nothing changes
    }
    if (current) {
        message_ = message;
        setBusy(false);
    }
    emit statusChanged();
    if (previewPending_)
        startPreview();
    else if (!current)
        setBusy(false);
}

void DirectEditModel::showPreview(const doc::DocState& state, const doc::BuildResult& build)
{
    doc::Document* d = liveDoc();
    if (d == nullptr || !scene_)
        return;
    std::vector<api::viewer::Body> bodies = api::viewer::bodiesFromDocument(state, build);
    std::set<BodyId> hide;
    const doc::BuildResultPtr current = d->build();
    std::vector<api::viewer::Body> changed;
    for (api::viewer::Body& b : bodies) {
        bool same = false;
        if (current) {
            const auto it = current->bodies.find(b.id);
            same = it != current->bodies.end() && it->second.shape.impl == b.shape.impl;
        }
        if (!same) {
            hide.insert(b.id);
            changed.push_back(std::move(b));
        }
    }
    render::MeshStyle style{render::Token::ModelFace, render::Token::Accent, 1.0f, true, false};
    scene_->preview().show(this, std::move(changed), std::move(hide), false, style);
}

// ---------- commit ----------
bool DirectEditModel::canCommit() const
{
    return active() && !committing_ && flow_.canCommit() && refsPending_ == 0 && !previewTimer_.isActive() &&
           !previewPending_;
}

bool DirectEditModel::commit()
{
    if (!canCommit())
        return false;
    api::IApp* a = app();
    auto cmd = command();
    if (a == nullptr || !cmd)
        return false;
    committing_ = true;
    flow_.commitStarted();
    emit statusChanged();
    previewTimer_.stop();
    JobHandle<api::ActionResult> job = a->actions().invoke(cmd->name, cmd->argsJson, api::Caller::User);
    if (!job.valid()) {
        committing_ = false;
        flow_.commitFailed();
        setMessage(trd("The edit could not be added"));
        return false;
    }
    const FeatureId id = *id_;
    QPointer<DirectEditModel> self(this);
    job.then([self, id](Result<api::ActionResult> r) {
        if (!self)
            return;
        self->committing_ = false;
        if (!r) {
            self->flow_.commitFailed();
            self->message_ = qs(r.error().message);
            emit self->statusChanged();
            return;
        }
        self->close(); // the flow ends with it
        emit self->committed(idText(id.value));
    });
    return true;
}

} // namespace ray::ui
