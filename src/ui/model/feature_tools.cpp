#include "feature_tools.h"

#include "../app_context.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../selection_model.h"
#include "../session.h"
#include "../sketch/sketch_geom.h"
#include "../sketch/sketch_mode.h"
#include "../units.h"
#include "../viewport_item.h"
#include "../viewport_navigation.h"
#include "feature_tree.h"
#include "model_util.h"

#include <ray_api/sketch_edit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>
#include <QtCore/QLocale>
#include <QtGui/QMouseEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ray::ui {

using namespace modelui;

namespace {

constexpr const char* kTrContext = "ray::ui::FeatureToolModel";
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kToolPrefix = "model.tool.";
constexpr const char* kModelPrefix = "model.";

QString trf(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

struct ToolType {
    const char* type;  // feature type
    const char* tool;  // model.tool.<tool>
    const char* title; // panel title
};
constexpr ToolType kTools[] = {
    {"extrude", "extrude", QT_TRANSLATE_NOOP("ray::ui::FeatureToolModel", "Extrude")},
    {"revolve", "revolve", QT_TRANSLATE_NOOP("ray::ui::FeatureToolModel", "Revolve")},
    {"fillet", "fillet", QT_TRANSLATE_NOOP("ray::ui::FeatureToolModel", "Fillet")},
    {"chamfer", "chamfer", QT_TRANSLATE_NOOP("ray::ui::FeatureToolModel", "Chamfer")},
    {"hole", "hole", QT_TRANSLATE_NOOP("ray::ui::FeatureToolModel", "Hole")},
    {"datum_plane", "datumPlane", QT_TRANSLATE_NOOP("ray::ui::FeatureToolModel", "Datum Plane")},
};

const ToolType* toolOfType(const QString& type)
{
    for (const ToolType& t : kTools) {
        if (type == QLatin1String(t.type))
            return &t;
    }
    return nullptr;
}

const doc::Feature* findFeature(const doc::DocState& state, const FeatureId& id)
{
    const auto it =
        std::find_if(state.features.begin(), state.features.end(), [&](const doc::Feature& f) { return f.id == id; });
    return it == state.features.end() ? nullptr : &*it;
}

std::ptrdiff_t featureIndex(const doc::DocState& state, const FeatureId& id)
{
    const auto it =
        std::find_if(state.features.begin(), state.features.end(), [&](const doc::Feature& f) { return f.id == id; });
    return it == state.features.end() ? -1 : it - state.features.begin();
}

FeatureId featureIdOf(const QString& text)
{
    return FeatureId{Uuid::fromString(text.toStdString())};
}

bool sweepType(const QString& t)
{
    return t == QLatin1String("extrude") || t == QLatin1String("revolve");
}

bool blendType(const QString& t)
{
    return t == QLatin1String("fillet") || t == QLatin1String("chamfer");
}

bool usesSketch(const QString& t)
{
    return sweepType(t) || t == QLatin1String("hole");
}

// Text for a stored numeric prop: an expression as it is, a number (mm or rad) with its unit.
QString storedText(const QJsonValue& v, bool angle)
{
    if (v.isString())
        return v.toString();
    if (v.isDouble()) {
        const double x = angle ? v.toDouble() * 180.0 / kPi : v.toDouble();
        return QLocale::c().toString(x, 'g', 10) + (angle ? QStringLiteral(" deg") : QStringLiteral(" mm"));
    }
    return {};
}

Vec3 add3(Vec3 a, Vec3 b)
{
    return modelui::add(a, b);
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

} // namespace

// ---------- construction ----------
FeatureToolModel::FeatureToolModel(SceneController* scene, SelectionModel* selection, Session* session,
                                   AppContext* context, QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session), context_(context),
      overlay_(std::make_unique<BodyOverlay>(scene)), lines_(std::make_unique<LineOverlay>(scene))
{
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(0);
    connect(&previewTimer_, &QTimer::timeout, this, [this] { startPreview(); });
    connect(overlay_.get(), &BodyOverlay::shown, this, [this] { emit statusChanged(); });
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            const std::string id = session_->documentId().toStdString();
            if (id != docId_) {
                if (active())
                    close();
                offer_.clear();
                emit offerChanged();
                subscribe();
            }
        });
    }
    if (selection_) {
        connect(selection_, &SelectionModel::changed, this, [this] {
            if (!active())
                return;
            if (blendType(type_) && picking_)
                applyEdges();
            if (type_ == QLatin1String("datum_plane") &&
                values_.value(QStringLiteral("base")).toString() == QLatin1String("face"))
                applyEdges(); // the face, through the same reference path
            emit optionsChanged();
        });
    }
}

FeatureToolModel::~FeatureToolModel()
{
    if (commands_) {
        commands_->setUiHandler(QString::fromLatin1(kModelPrefix), {});
        for (const ToolType& t : kTools)
            commands_->setUiHandler(QStringLiteral("feature.%1.add").arg(QLatin1String(t.type)), {});
    }
    sideJob_.cancel();
    if (doc::Document* d = liveDoc(); d != nullptr && active())
        d->cancelPreview();
    overlay_.reset();
    lines_.reset();
    unsubscribe();
}

FeatureToolModel* FeatureToolModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    UnitsObject* units = nullptr;
    FeatureTreeModel* tree = nullptr;
    SketchModel* sketch = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
        tree = qml->singletonInstance<FeatureTreeModel*>(uri, QStringLiteral("FeatureTree"));
        sketch = qml->singletonInstance<SketchModel*>(uri, QStringLiteral("SketchTool"));
    }
    auto* model = new FeatureToolModel(scene, selection, session, context);
    model->setCommands(commands);
    model->setUnits(units);
    model->setTree(tree);
    model->setSketchTool(sketch);
    if (tree)
        tree->setFeatureTool(model);
    model->subscribe();
    return model; // owned by the engine
}

void FeatureToolModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<FeatureToolModel> self(this);
    const CommandCatalog::UiHandler handler{[self](const QString& id) {
                                                return self ? self->commandReason(id)
                                                            : trf("The feature tools are not available");
                                            },
                                            [self](const QString& id) { return self && self->runCommand(id); }};
    commands_->setUiHandler(QString::fromLatin1(kModelPrefix), handler);
    for (const ToolType& t : kTools)
        commands_->setUiHandler(QStringLiteral("feature.%1.add").arg(QLatin1String(t.type)), handler);
}

void FeatureToolModel::setUnits(UnitsObject* units)
{
    units_ = units;
}

void FeatureToolModel::setSketchTool(SketchModel* sketch)
{
    sketchTool_ = sketch;
    if (!sketch)
        return;
    // Remember the sketch being edited; when the sketch mode closes on one with closed regions, offer Extrude.
    connect(sketch, &SketchModel::modelChanged, this, [this] {
        if (sketchTool_ && sketchTool_->active() && sketchTool_->featureInDocument())
            lastSketch_ = sketchTool_->featureId();
    });
    connect(sketch, &SketchModel::activeChanged, this, [this] {
        if (!sketchTool_)
            return;
        if (sketchTool_->active()) {
            if (active())
                cancel();
            lastSketch_ = sketchTool_->featureInDocument() ? sketchTool_->featureId() : std::nullopt;
            offer_.clear();
            emit offerChanged();
            return;
        }
        doc::Document* d = liveDoc();
        if (!lastSketch_ || d == nullptr)
            return;
        const doc::DocStatePtr state = d->state();
        const doc::Feature* f = findFeature(*state, *lastSketch_);
        auto regions =
            f ? api::feature_edit::regionsOf(*state, *lastSketch_) : Result<std::vector<api::feature_edit::Region>>();
        const QString text = idText(lastSketch_->value);
        const bool used = std::any_of(state->features.begin(), state->features.end(), [&](const doc::Feature& g) {
            return g.type != "sketch" && jsonObject(g.propsJson).value(QStringLiteral("sketch")).toString() == text;
        });
        if (f != nullptr && !used && regions && !regions->empty()) {
            offer_ =
                QVariantMap{{QStringLiteral("id"), idText(lastSketch_->value)}, {QStringLiteral("name"), qs(f->name)}};
            emit offerChanged();
        }
        lastSketch_.reset();
    });
}

// ---------- document access ----------
api::IApp* FeatureToolModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

api::OpenDocument* FeatureToolModel::document() const
{
    api::IApp* a = app();
    if (a == nullptr)
        return nullptr;
    return docId_.empty() ? a->active() : a->findDocument(docId_);
}

doc::Document* FeatureToolModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void FeatureToolModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<FeatureToolModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->onDocumentChanged();
    });
    previewListener_ = doc_->subscribePreview([self, generation](doc::DocStatePtr state, doc::BuildResultPtr build) {
        if (self && self->docGeneration_ == generation && self->previewBusy_ && !self->previewSide_)
            self->onPreviewBuilt(std::move(state), std::move(build));
    });
}

void FeatureToolModel::unsubscribe()
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
    sideRebuilder_.reset();
    sideRebuilderDoc_.clear();
}

void FeatureToolModel::onDocumentChanged()
{
    if (!active() || committing_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr) {
        close();
        return;
    }
    // An edited feature that is gone (undone): the tool closes.
    if (editId_ && findFeature(*d->state(), *editId_) == nullptr) {
        close();
        return;
    }
    // The document's state changed under the tool (an undo, a command from elsewhere): that ended the running
    // preview, which is built again on the new state. A rebuild of the same state finishing changes nothing here.
    if (d->state() == previewBase_)
        return;
    if (previewBusy_ && !previewSide_)
        previewBusy_ = false;
    emit optionsChanged();
    schedulePreview();
}

// ---------- open and close ----------
QString FeatureToolModel::title() const
{
    const ToolType* t = toolOfType(type_);
    if (t == nullptr)
        return {};
    if (!editId_)
        return trf(t->title);
    doc::Document* d = liveDoc();
    const doc::Feature* f = d ? findFeature(*d->state(), *editId_) : nullptr;
    return f ? trf("Edit %1").arg(qs(f->name)) : trf(t->title);
}

QString FeatureToolModel::commandReason(const QString& id) const
{
    api::IApp* a = app();
    if (a == nullptr)
        return trf("There is no session");
    api::OpenDocument* od = a->active();
    if (od == nullptr)
        return trf("Open or create a document first");
    if (od->readOnly)
        return trf("The document is read-only");
    if (id == QLatin1String("model.parameters"))
        return {};
    if (id == QLatin1String("model.rollback.end"))
        return tree_ && tree_->rollbackIndex() >= 0 ? QString() : trf("The model is not rolled back");
    if (id.startsWith(QLatin1String("model.feature."))) {
        if (!tree_ || !tree_->currentFeatureId())
            return trf("Select a feature in the model tree first");
        return {};
    }
    if (sketchTool_ && (sketchTool_->active() || sketchTool_->placing()))
        return trf("Finish the sketch first");
    QString type;
    if (id.startsWith(QLatin1String(kToolPrefix))) {
        const QString tool = id.mid(int(std::char_traits<char>::length(kToolPrefix)));
        for (const ToolType& t : kTools) {
            if (tool == QLatin1String(t.tool))
                type = QLatin1String(t.type);
        }
    } else if (id.startsWith(QLatin1String("feature."))) {
        type = id.section(QLatin1Char('.'), 1, 1);
    }
    if (type.isEmpty())
        return trf("Unknown model command");
    const doc::DocStatePtr state = od->document->state();
    const bool hasSketch = std::any_of(state->features.begin(), state->features.end(),
                                       [](const doc::Feature& f) { return f.type == "sketch"; });
    if (usesSketch(type) && !hasSketch)
        return trf("Draw a sketch first");
    if ((blendType(type) || type == QLatin1String("hole")) && state->bodies.empty())
        return trf("There is no body yet");
    return {};
}

bool FeatureToolModel::runCommand(const QString& id)
{
    if (id == QLatin1String("model.parameters")) {
        if (tree_)
            emit tree_->parametersRequested();
        return true;
    }
    if (id == QLatin1String("model.rollback.end")) {
        if (tree_)
            tree_->setRollback(-1);
        return true;
    }
    if (id.startsWith(QLatin1String("model.feature.")) && tree_) {
        const int row = tree_->currentIndex();
        if (id == QLatin1String("model.feature.edit"))
            return tree_->edit(row);
        if (id == QLatin1String("model.feature.rename")) {
            emit tree_->renameRequested(row);
            return true;
        }
        if (id == QLatin1String("model.feature.suppress"))
            return tree_->setSuppressed(row, !tree_->get(row).value(QStringLiteral("suppressed")).toBool());
        if (id == QLatin1String("model.feature.delete"))
            return tree_->remove(row);
        return false;
    }
    if (id.startsWith(QLatin1String(kToolPrefix))) {
        const QString tool = id.mid(int(std::char_traits<char>::length(kToolPrefix)));
        for (const ToolType& t : kTools) {
            if (tool == QLatin1String(t.tool))
                return start(QLatin1String(t.type));
        }
        return false;
    }
    return start(id.section(QLatin1Char('.'), 1, 1));
}

bool FeatureToolModel::start(const QString& type)
{
    if (toolOfType(type) == nullptr)
        return false;
    if (const QString reason = commandReason(QStringLiteral("feature.%1.add").arg(type)); !reason.isEmpty()) {
        if (context_)
            context_->showMessage(reason, true);
        return false;
    }
    open(type, std::nullopt);
    return active();
}

bool FeatureToolModel::edit(const QString& featureId)
{
    if (!docId_.empty() && document() == nullptr)
        subscribe();
    if (doc_ == nullptr)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return false;
    const FeatureId id = featureIdOf(featureId);
    const doc::Feature* f = findFeature(*d->state(), id);
    if (f == nullptr || toolOfType(qs(f->type)) == nullptr)
        return false;
    if (sketchTool_ && (sketchTool_->active() || sketchTool_->placing()))
        return false;
    open(qs(f->type), id);
    return active();
}

void FeatureToolModel::open(const QString& type, std::optional<FeatureId> editId)
{
    if (active())
        close();
    if (doc_ == nullptr || document() == nullptr || document()->document.get() != doc_)
        subscribe();
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return;
    type_ = type;
    editId_ = editId;
    id_ = editId ? *editId : FeatureId{Uuid::generate()};
    newBody_ = BodyId{Uuid::generate()};
    values_.clear();
    regionPicks_.clear();
    pointPicks_.clear();
    edges_ = QJsonArray();
    edgePoints_.clear();
    faceRef_ = QJsonObject();
    plane_.reset();
    regions_.clear();
    freePoints_.clear();
    sketchLines_.clear();
    flipTouched_ = false;
    message_.clear();
    messageField_.clear();
    previewStatus_.clear();
    committing_ = false;
    rejected_ = false;
    picking_ = false;
    previewBusy_ = previewPending_ = false;
    previewBuilds_ = 0;
    offer_.clear();
    defaults();
    if (editId_) {
        if (const doc::Feature* f = findFeature(*d->state(), *editId_))
            loadFeature(*f);
    } else {
        prefillFromSelection();
    }
    if (type_ == QLatin1String("hole"))
        loadThreads();
    if (blendType(type_)) {
        if (selection_) {
            savedFilter_ = selection_->filterMask();
            filterSaved_ = true;
            selection_->setFilterMask(static_cast<std::uint8_t>(render::PickFilter::Edges));
        }
        // Edges selected before the tool opened are its edges; without any, the view's clicks pick them.
        bool selectedEdges = false;
        if (selection_) {
            for (const SelectionEntry& e : selection_->entries())
                selectedEdges = selectedEdges || (e.sub && e.sub->kind == kernel::SubKind::Edge);
        }
        picking_ = edges_.isEmpty() && !selectedEdges;
        if (!editId_ && selectedEdges)
            applyEdges();
    } else if (type_ == QLatin1String("datum_plane") && selection_) {
        savedFilter_ = selection_->filterMask();
        filterSaved_ = true;
        selection_->setFilterMask(static_cast<std::uint8_t>(render::PickFilter::Faces));
    }
    if (tree_ && !editId_ && tree_->rollbackIndex() >= 0)
        tree_->setRollbackOverlaySuspended(true);
    emit activeChanged();
    emit valuesChanged();
    emit optionsChanged();
    emit picksChanged();
    emit offerChanged();
    loadSketch();
    schedulePreview();
}

void FeatureToolModel::close()
{
    const bool was = active();
    previewTimer_.stop();
    sideJob_.cancel();
    sideJob_ = {};
    ++previewSerial_;
    if (doc::Document* d = liveDoc(); d != nullptr && was && !previewSide_)
        d->cancelPreview();
    overlay_->clear();
    lines_->clear();
    if (filterSaved_ && selection_)
        selection_->setFilterMask(savedFilter_);
    filterSaved_ = false;
    type_.clear();
    editId_.reset();
    id_.reset();
    values_.clear();
    message_.clear();
    messageField_.clear();
    previewStatus_.clear();
    committing_ = false;
    picking_ = false;
    previewBusy_ = previewPending_ = previewSide_ = false;
    dragKey_.clear();
    if (tree_) {
        tree_->setToolSketch(std::nullopt);
        tree_->setRollbackOverlaySuspended(false);
    }
    if (was) {
        emit activeChanged();
        emit valuesChanged();
        emit statusChanged();
        emit picksChanged();
        emit handlesChanged();
    }
}

void FeatureToolModel::cancel()
{
    close();
}

void FeatureToolModel::defaults()
{
    doc::Document* d = liveDoc();
    const doc::DocStatePtr state = d->state();
    const bool inch = units_ ? units_->system() == UnitSystem::Inch : state->settings.units == UnitSystem::Inch;
    const QString len = [&](double mm, double in) {
        return inch ? QLocale::c().toString(in) + QStringLiteral(" in")
                    : QLocale::c().toString(mm) + QStringLiteral(" mm");
    }(10, 0.5);
    // The sketch: the one the tree has current, else the offered one, else the last before the insertion point.
    QString sketch;
    const std::size_t limit = insertIndex().value_or(state->features.size());
    for (std::size_t i = 0; i < state->features.size() && i < limit; ++i) {
        if (state->features[i].type == "sketch" && !state->features[i].suppressed)
            sketch = idText(state->features[i].id.value);
    }
    if (preferredSketch_) {
        if (findFeature(*state, *preferredSketch_) != nullptr)
            sketch = idText(preferredSketch_->value);
        preferredSketch_.reset();
    } else if (tree_) {
        if (const auto cur = tree_->currentFeatureId()) {
            const doc::Feature* f = findFeature(*state, *cur);
            if (f != nullptr && f->type == "sketch")
                sketch = idText(cur->value);
        }
    }
    const QString firstBody = state->bodies.empty() ? QString() : idText(state->bodies.front().id.value);
    values_[QStringLiteral("sketch")] = sketch;
    values_[QStringLiteral("operation")] = state->bodies.empty() ? QStringLiteral("new") : QStringLiteral("join");
    values_[QStringLiteral("target")] = firstBody;
    values_[QStringLiteral("flip")] = false;
    if (type_ == QLatin1String("extrude")) {
        values_[QStringLiteral("distance")] = len;
        values_[QStringLiteral("symmetric")] = false;
        values_[QStringLiteral("draft")] = QString();
    } else if (type_ == QLatin1String("revolve")) {
        values_[QStringLiteral("axis")] = QStringLiteral("x");
        values_[QStringLiteral("angle")] = QStringLiteral("360 deg");
    } else if (type_ == QLatin1String("fillet")) {
        values_[QStringLiteral("radius")] = inch ? QStringLiteral("0.0625 in") : QStringLiteral("1 mm");
    } else if (type_ == QLatin1String("chamfer")) {
        values_[QStringLiteral("distance")] = inch ? QStringLiteral("0.03 in") : QStringLiteral("0.5 mm");
    } else if (type_ == QLatin1String("hole")) {
        values_[QStringLiteral("kind")] = QStringLiteral("simple");
        values_[QStringLiteral("tapped")] = false;
        values_[QStringLiteral("diameter")] = inch ? QStringLiteral("0.25 in") : QStringLiteral("5 mm");
        values_[QStringLiteral("end")] = QStringLiteral("through");
        values_[QStringLiteral("depth")] = inch ? QStringLiteral("0.5 in") : QStringLiteral("10 mm");
        values_[QStringLiteral("bottom")] = QStringLiteral("angle");
        values_[QStringLiteral("tipAngle")] = QStringLiteral("118 deg");
        values_[QStringLiteral("cboreDiameter")] = inch ? QStringLiteral("0.4375 in") : QStringLiteral("11 mm");
        values_[QStringLiteral("cboreDepth")] = inch ? QStringLiteral("0.25 in") : QStringLiteral("6 mm");
        values_[QStringLiteral("csinkDiameter")] = inch ? QStringLiteral("0.5 in") : QStringLiteral("12 mm");
        values_[QStringLiteral("csinkAngle")] = QStringLiteral("90 deg");
        values_[QStringLiteral("series")] = inch ? QStringLiteral("unc") : QStringLiteral("iso_metric_coarse");
        values_[QStringLiteral("size")] = inch ? QStringLiteral("1/4-20 UNC") : QStringLiteral("M6x1");
        values_[QStringLiteral("threadClass")] = QString();
        values_[QStringLiteral("threadDepth")] = QString();
    } else if (type_ == QLatin1String("datum_plane")) {
        values_[QStringLiteral("base")] = QStringLiteral("xy");
        values_[QStringLiteral("offset")] = len;
    }
    // A sweep on a sketch that lies on a face of a body joins that body by default.
    if (sweepType(type_) || type_ == QLatin1String("hole")) {
        auto planeJson = sketch.isEmpty() ? Result<std::string>(tl::unexpected(Error{ErrorCode::NotFound, "", {}}))
                                          : api::sketch_edit::planeJsonOf(*state, featureIdOf(sketch));
        if (planeJson) {
            const QJsonObject plane = jsonObject(*planeJson);
            const QJsonObject face = plane.value(QStringLiteral("face")).toObject();
            const QString body = face.value(QStringLiteral("body")).toString();
            if (!body.isEmpty()) {
                values_[QStringLiteral("target")] = body;
                values_[QStringLiteral("operation")] = QStringLiteral("join");
            } else if (type_ != QLatin1String("hole")) {
                values_[QStringLiteral("operation")] = QStringLiteral("new");
            }
        }
    }
}

void FeatureToolModel::prefillFromSelection()
{
    if (!selection_ || !scene_)
        return;
    if (type_ == QLatin1String("datum_plane")) {
        for (const SelectionEntry& e : selection_->entries()) {
            if (e.sub && e.sub->kind == kernel::SubKind::Face) {
                values_[QStringLiteral("base")] = QStringLiteral("face");
                values_[QStringLiteral("offset")] = QStringLiteral("0 mm");
                applyEdges();
                break;
            }
        }
    }
}

void FeatureToolModel::loadFeature(const doc::Feature& f)
{
    const QJsonObject p = jsonObject(f.propsJson);
    oldPropsJson_ = f.propsJson;
    auto str = [&](const char* key) {
        return p.value(QLatin1String(key)).toString();
    };
    if (p.contains(QStringLiteral("sketch")))
        values_[QStringLiteral("sketch")] = str("sketch");
    if (p.contains(QStringLiteral("operation")))
        values_[QStringLiteral("operation")] = str("operation");
    if (p.contains(QStringLiteral("target")))
        values_[QStringLiteral("target")] = str("target");
    if (sweepType(type_)) {
        values_[QStringLiteral("flip")] = str("direction") == QLatin1String("reverse");
        for (const QJsonValue& r : p.value(QStringLiteral("regions")).toArray()) {
            std::vector<SketchEntId> ids;
            for (const QJsonValue& e : r.toArray())
                ids.push_back(SketchEntId{Uuid::fromString(e.toString().toStdString())});
            std::sort(ids.begin(), ids.end());
            regionPicks_.push_back(std::move(ids));
        }
        if (str("operation") == QLatin1String("new"))
            newBody_ = BodyId{Uuid::fromString(str("target").toStdString())};
    }
    if (type_ == QLatin1String("extrude")) {
        values_[QStringLiteral("distance")] = storedText(p.value(QStringLiteral("distance")), false);
        values_[QStringLiteral("symmetric")] = p.value(QStringLiteral("symmetric")).toBool();
        values_[QStringLiteral("draft")] =
            p.contains(QStringLiteral("draft")) ? storedText(p.value(QStringLiteral("draft")), true) : QString();
    } else if (type_ == QLatin1String("revolve")) {
        const QJsonObject axis = p.value(QStringLiteral("axis")).toObject();
        values_[QStringLiteral("axis")] =
            axis.contains(QStringLiteral("entity"))
                ? QStringLiteral("line:") + axis.value(QStringLiteral("entity")).toString()
                : axis.value(QStringLiteral("principal")).toString();
        values_[QStringLiteral("angle")] = p.contains(QStringLiteral("angle"))
                                               ? storedText(p.value(QStringLiteral("angle")), true)
                                               : QStringLiteral("360 deg");
    } else if (blendType(type_)) {
        edges_ = p.value(QStringLiteral("edges")).toArray();
        const char* size = type_ == QLatin1String("fillet") ? "radius" : "distance";
        values_[QLatin1String(size)] = storedText(p.value(QLatin1String(size)), false);
    } else if (type_ == QLatin1String("hole")) {
        values_[QStringLiteral("flip")] = p.value(QStringLiteral("flip")).toBool();
        for (const QJsonValue& v : p.value(QStringLiteral("points")).toArray())
            pointPicks_.push_back(SketchEntId{Uuid::fromString(v.toString().toStdString())});
        const QString kind = str("kind");
        values_[QStringLiteral("tapped")] = kind.contains(QLatin1String("tapped"));
        values_[QStringLiteral("kind")] = kind.startsWith(QLatin1String("counterbore")) ? QStringLiteral("counterbore")
                                          : kind.startsWith(QLatin1String("countersink"))
                                              ? QStringLiteral("countersink")
                                              : QStringLiteral("simple");
        if (p.contains(QStringLiteral("diameter")))
            values_[QStringLiteral("diameter")] = storedText(p.value(QStringLiteral("diameter")), false);
        values_[QStringLiteral("end")] = str("end");
        if (p.contains(QStringLiteral("depth")))
            values_[QStringLiteral("depth")] = storedText(p.value(QStringLiteral("depth")), false);
        if (p.contains(QStringLiteral("bottom")))
            values_[QStringLiteral("bottom")] = str("bottom");
        if (p.contains(QStringLiteral("tipAngle")))
            values_[QStringLiteral("tipAngle")] = storedText(p.value(QStringLiteral("tipAngle")), true);
        const QJsonObject cb = p.value(QStringLiteral("counterbore")).toObject();
        if (!cb.isEmpty()) {
            values_[QStringLiteral("cboreDiameter")] = storedText(cb.value(QStringLiteral("diameter")), false);
            values_[QStringLiteral("cboreDepth")] = storedText(cb.value(QStringLiteral("depth")), false);
        }
        const QJsonObject cs = p.value(QStringLiteral("countersink")).toObject();
        if (!cs.isEmpty()) {
            values_[QStringLiteral("csinkDiameter")] = storedText(cs.value(QStringLiteral("diameter")), false);
            if (cs.contains(QStringLiteral("angle")))
                values_[QStringLiteral("csinkAngle")] = storedText(cs.value(QStringLiteral("angle")), true);
        }
        const QJsonObject th = p.value(QStringLiteral("thread")).toObject();
        if (!th.isEmpty()) {
            values_[QStringLiteral("series")] = th.value(QStringLiteral("standard")).toString();
            values_[QStringLiteral("size")] = th.value(QStringLiteral("designation")).toString();
            values_[QStringLiteral("threadClass")] = th.value(QStringLiteral("class")).toString();
            values_[QStringLiteral("threadDepth")] =
                th.contains(QStringLiteral("depth")) ? storedText(th.value(QStringLiteral("depth")), false) : QString();
        }
    } else if (type_ == QLatin1String("datum_plane")) {
        const QJsonObject base = p.value(QStringLiteral("base")).toObject();
        if (base.contains(QStringLiteral("principal")))
            values_[QStringLiteral("base")] = base.value(QStringLiteral("principal")).toString();
        else if (base.contains(QStringLiteral("datum")))
            values_[QStringLiteral("base")] = QStringLiteral("datum:") + base.value(QStringLiteral("datum")).toString();
        else {
            values_[QStringLiteral("base")] = QStringLiteral("face");
            faceRef_ = base.value(QStringLiteral("face")).toObject();
        }
        values_[QStringLiteral("offset")] =
            p.contains(QStringLiteral("offset")) ? storedText(p.value(QStringLiteral("offset")), false) : QString();
        values_[QStringLiteral("flip")] = p.value(QStringLiteral("flip")).toBool();
    }
}

// ---------- the chosen sketch ----------
void FeatureToolModel::loadSketch()
{
    plane_.reset();
    regions_.clear();
    freePoints_.clear();
    sketchLines_.clear();
    const QString sketchText = values_.value(QStringLiteral("sketch")).toString();
    if (tree_)
        tree_->setToolSketch(usesSketch(type_) && !sketchText.isEmpty()
                                 ? std::optional<FeatureId>(featureIdOf(sketchText))
                                 : std::nullopt);
    emit optionsChanged();
    updateHighlights();
    updateHandles();
    doc::Document* d = liveDoc();
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (!usesSketch(type_) || sketchText.isEmpty() || d == nullptr || jobs == nullptr)
        return;
    const FeatureId id = featureIdOf(sketchText);
    const doc::DocStatePtr state = d->state();
    const doc::BuildResultPtr build = d->build();
    const std::uint64_t serial = ++sketchSerial_;
    JobHandle<std::shared_ptr<SketchInfo>> job = jobs->submit<std::shared_ptr<SketchInfo>>(
        JobInfo{"Sketch regions", JobPriority::Interactive, false},
        [state, build, id](JobContext& ctx) -> Result<std::shared_ptr<SketchInfo>> {
            auto info = std::make_shared<SketchInfo>();
            info->id = id;
            auto planeJson = api::sketch_edit::planeJsonOf(*state, id);
            if (planeJson) {
                auto plane = api::sketch_edit::resolvePlane(*state, build.get(), *planeJson, ctx);
                if (plane)
                    info->plane = *plane;
                else
                    info->error = qs(plane.error().message);
            }
            if (auto regions = api::feature_edit::regionsOf(*state, id))
                info->regions = std::move(*regions);
            if (auto model = api::sketch_edit::modelOf(*state, id)) {
                const sketchui::SketchView view(*model);
                for (const sketchui::ViewPoint& p : view.points()) {
                    if (!p.construction && (!p.used || p.center))
                        info->freePoints.emplace_back(p.id, p.p);
                    if (!p.construction && p.used && p.center)
                        info->centres.insert(p.id);
                }
                for (const sketchui::ViewCurve& c : view.curves()) {
                    if (c.kind == sketchui::ViewCurve::Kind::Line)
                        info->lines.push_back({c.id, {c.a, c.b}});
                }
            }
            return info;
        });
    QPointer<FeatureToolModel> self(this);
    job.then([self, serial](Result<std::shared_ptr<SketchInfo>> info) {
        if (!self || serial != self->sketchSerial_ || !info)
            return;
        self->plane_ = (*info)->plane;
        self->regions_ = std::move((*info)->regions);
        self->freePoints_ = std::move((*info)->freePoints);
        self->centres_ = std::move((*info)->centres);
        self->sketchLines_ = std::move((*info)->lines);
        // Regions picked before that are no longer in the sketch are dropped.
        auto& picks = self->regionPicks_;
        picks.erase(std::remove_if(picks.begin(), picks.end(),
                                   [&](const std::vector<SketchEntId>& p) {
                                       return std::none_of(
                                           self->regions_.begin(), self->regions_.end(),
                                           [&](const api::feature_edit::Region& r) { return r.entities == p; });
                                   }),
                    picks.end());
        emit self->optionsChanged();
        emit self->picksChanged();
        self->updateHighlights();
        self->updateHandles();
        self->schedulePreview();
    });
}

void FeatureToolModel::loadThreads()
{
    if (!threadTable_.isEmpty() || threadsLoading_)
        return;
    api::IApp* a = app();
    if (a == nullptr)
        return;
    threadsLoading_ = true;
    JobHandle<api::ActionResult> job = a->actions().invoke("standards.threads.list", "{}", api::Caller::User);
    QPointer<FeatureToolModel> self(this);
    job.then([self](Result<api::ActionResult> r) {
        if (!self)
            return;
        self->threadsLoading_ = false;
        if (!r)
            return;
        self->threadTable_ = jsonObject(r->resultJson).value(QStringLiteral("series")).toArray();
        emit self->optionsChanged();
        self->schedulePreview();
    });
}

// Fillet and chamfer edges, or the datum plane's face, from the view's selection (references through
// model.reference, which reads the document's latest build: the build the view shows).
void FeatureToolModel::applyEdges()
{
    api::IApp* a = app();
    if (a == nullptr || !selection_ || !scene_)
        return;
    const bool face = type_ == QLatin1String("datum_plane");
    struct Want {
        BodyId body;
        std::uint32_t index;
        Vec3 point;
    };
    std::vector<Want> wanted;
    for (const SelectionEntry& e : selection_->entries()) {
        if (!e.sub) // a pending entry knows its face or edge already (only a vertex waits for it)
            continue;
        const kernel::SubKind kind = face ? kernel::SubKind::Face : kernel::SubKind::Edge;
        if (e.sub->kind != kind)
            continue;
        const api::viewer::Body* body = scene_->body(e.body);
        if (body == nullptr)
            continue;
        wanted.push_back(Want{body->id, e.sub->index, e.hit.point});
        if (face)
            break;
    }
    const std::uint64_t serial = ++edgeSerial_;
    if (wanted.empty()) {
        if (!face) {
            edges_ = QJsonArray();
            edgePoints_.clear();
            emit picksChanged();
            updateHandles();
            schedulePreview();
        }
        return;
    }
    auto refs = std::make_shared<std::vector<QJsonObject>>(wanted.size());
    auto left = std::make_shared<int>(static_cast<int>(wanted.size()));
    auto failedMessage = std::make_shared<QString>();
    std::vector<Vec3> points;
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        points.push_back(wanted[i].point);
        QJsonObject args{{QStringLiteral("body"), idText(wanted[i].body.value)},
                         {QStringLiteral("kind"), face ? QStringLiteral("face") : QStringLiteral("edge")},
                         {QStringLiteral("index"), static_cast<int>(wanted[i].index)}};
        JobHandle<api::ActionResult> job = a->actions().invoke("model.reference", jsonText(args), api::Caller::User);
        QPointer<FeatureToolModel> self(this);
        job.then([self, serial, refs, left, failedMessage, i, face, points](Result<api::ActionResult> r) {
            if (!self || serial != self->edgeSerial_)
                return;
            if (r)
                (*refs)[i] = jsonObject(r->resultJson).value(QStringLiteral("ref")).toObject();
            else
                *failedMessage = qs(r.error().message);
            if (--*left > 0)
                return;
            if (face) {
                self->faceRef_ = (*refs)[0];
            } else {
                QJsonArray edges;
                std::vector<Vec3> at;
                for (std::size_t k = 0; k < refs->size(); ++k) {
                    if (!(*refs)[k].isEmpty()) {
                        edges.append((*refs)[k]);
                        at.push_back(points[k]);
                    }
                }
                self->edges_ = edges;
                self->edgePoints_ = std::move(at);
            }
            if (!failedMessage->isEmpty())
                self->setStatus(QStringLiteral("failed"), *failedMessage);
            emit self->picksChanged();
            self->updateHandles();
            self->schedulePreview();
        });
    }
}

// ---------- values ----------
void FeatureToolModel::setValue(const QString& key, const QVariant& value)
{
    if (!active() || values_.value(key) == value)
        return;
    values_[key] = value;
    if (key == QLatin1String("flip"))
        flipTouched_ = true;
    // Giving the size ends the picking of edges (the preview shows again).
    if (blendType(type_) && picking_ && pickCount() > 0 &&
        (key == QLatin1String("radius") || key == QLatin1String("distance"))) {
        picking_ = false;
        emit picksChanged();
    }
    if (key == QLatin1String("operation")) {
        // A cut from a sketch on a face goes into the body: against the face's outward normal.
        if (value.toString() == QLatin1String("cut") && !flipTouched_ && type_ == QLatin1String("extrude")) {
            doc::Document* d = liveDoc();
            const QString sketch = values_.value(QStringLiteral("sketch")).toString();
            auto planeJson = d && !sketch.isEmpty() ? api::sketch_edit::planeJsonOf(*d->state(), featureIdOf(sketch))
                                                    : Result<std::string>(tl::unexpected(Error{}));
            if (planeJson && jsonObject(*planeJson).contains(QStringLiteral("face")))
                values_[QStringLiteral("flip")] = true;
        }
        if (value.toString() != QLatin1String("new") && values_.value(QStringLiteral("target")).toString().isEmpty()) {
            doc::Document* d = liveDoc();
            if (d && !d->state()->bodies.empty())
                values_[QStringLiteral("target")] = idText(d->state()->bodies.front().id.value);
        }
    }
    if (key == QLatin1String("series")) {
        // The first size of the new series.
        for (const QJsonValue& s : threadTable_) {
            if (s.toObject().value(QStringLiteral("id")).toString() == value.toString()) {
                const QJsonArray sizes = s.toObject().value(QStringLiteral("sizes")).toArray();
                if (!sizes.isEmpty())
                    values_[QStringLiteral("size")] =
                        sizes.first().toObject().value(QStringLiteral("designation")).toString();
            }
        }
        emit optionsChanged();
    }
    emit valuesChanged();
    if (key == QLatin1String("sketch")) {
        regionPicks_.clear();
        pointPicks_.clear();
        emit picksChanged();
        loadSketch();
    } else if (key == QLatin1String("base") && value.toString() == QLatin1String("face")) {
        applyEdges();
    }
    updateHighlights();
    updateHandles();
    schedulePreview();
}

void FeatureToolModel::clearPicks()
{
    regionPicks_.clear();
    pointPicks_.clear();
    if (blendType(type_)) {
        edges_ = QJsonArray();
        edgePoints_.clear();
        if (selection_)
            selection_->clear();
        setPicking(true);
    }
    emit picksChanged();
    updateHighlights();
    updateHandles();
    schedulePreview();
}

void FeatureToolModel::setPicking(bool on)
{
    if (picking_ == on || !blendType(type_))
        return;
    picking_ = on;
    if (picking_) {
        overlay_->clear(); // the original edges must be clickable
        previewStatus_.clear();
        emit statusChanged();
    } else {
        schedulePreview();
    }
    emit picksChanged();
}

QString FeatureToolModel::exprOf(const QString& key, bool angle) const
{
    const QString text = values_.value(key).toString().trimmed();
    if (text.isEmpty())
        return {};
    bool ok = false;
    QLocale::c().toDouble(text, &ok);
    if (!ok)
        return text; // an expression, a number with a unit, a parameter name
    if (angle)
        return text + QStringLiteral(" deg");
    const QString unit = units_ ? units_->unitLabel() : QStringLiteral("mm");
    return text + QLatin1Char(' ') + unit;
}

std::optional<double> FeatureToolModel::numberOf(const QString& key, bool angle) const
{
    doc::Document* d = liveDoc();
    const QString e = exprOf(key, angle);
    if (d == nullptr || e.isEmpty())
        return std::nullopt;
    auto v = api::feature_edit::evaluate(*d->state(), e.toStdString(), angle);
    return v ? std::optional<double>(*v) : std::nullopt;
}

QString FeatureToolModel::lengthText(double mm) const
{
    if (units_ && units_->system() == UnitSystem::Inch)
        return QLocale::c().toString(mm / 25.4, 'f', 4) + QStringLiteral(" in");
    return QLocale::c().toString(mm, 'f', 3) + QStringLiteral(" mm");
}

Result<std::string> FeatureToolModel::propsJson() const
{
    const auto fail = [](const QString& m) {
        return Result<std::string>(tl::unexpected(Error{ErrorCode::InvalidArgument, m.toStdString(), {}}));
    };
    QJsonObject p;
    const auto setExpr = [&](QJsonObject& o, const char* prop, const QString& key, bool angle) {
        const QString e = exprOf(key, angle);
        if (!e.isEmpty())
            o.insert(QLatin1String(prop), e);
        return !e.isEmpty();
    };
    const QString sketch = values_.value(QStringLiteral("sketch")).toString();
    const QString operation = values_.value(QStringLiteral("operation")).toString();
    const QString target = values_.value(QStringLiteral("target")).toString();
    if (sweepType(type_)) {
        if (sketch.isEmpty())
            return fail(trf("Pick a sketch"));
        p.insert(QStringLiteral("sketch"), sketch);
        if (!regionPicks_.empty()) {
            QJsonArray regions;
            for (const auto& r : regionPicks_) {
                QJsonArray ids;
                for (const SketchEntId& e : r)
                    ids.append(idText(e.value));
                regions.append(ids);
            }
            p.insert(QStringLiteral("regions"), regions);
        }
        p.insert(QStringLiteral("operation"), operation);
        if (operation == QLatin1String("new")) {
            p.insert(QStringLiteral("target"), idText(newBody_.value));
        } else {
            if (target.isEmpty())
                return fail(trf("Pick the body to %1").arg(operation));
            p.insert(QStringLiteral("target"), target);
        }
        p.insert(QStringLiteral("direction"),
                 values_.value(QStringLiteral("flip")).toBool() ? QStringLiteral("reverse") : QStringLiteral("normal"));
        if (type_ == QLatin1String("extrude")) {
            if (!setExpr(p, "distance", QStringLiteral("distance"), false))
                return fail(trf("'distance' is empty: give a length"));
            p.insert(QStringLiteral("symmetric"), values_.value(QStringLiteral("symmetric")).toBool());
            setExpr(p, "draft", QStringLiteral("draft"), true);
        } else {
            const QString axis = values_.value(QStringLiteral("axis")).toString();
            if (axis.startsWith(QLatin1String("line:")))
                p.insert(QStringLiteral("axis"), QJsonObject{{QStringLiteral("entity"), axis.mid(5)}});
            else
                p.insert(QStringLiteral("axis"), QJsonObject{{QStringLiteral("principal"), axis}});
            setExpr(p, "angle", QStringLiteral("angle"), true);
        }
    } else if (blendType(type_)) {
        if (edges_.isEmpty())
            return fail(trf("Pick the edges in the view"));
        p.insert(QStringLiteral("edges"), edges_);
        const char* size = type_ == QLatin1String("fillet") ? "radius" : "distance";
        if (!setExpr(p, size, QLatin1String(size), false))
            return fail(trf("'%1' is empty: give a length").arg(QLatin1String(size)));
    } else if (type_ == QLatin1String("hole")) {
        if (sketch.isEmpty())
            return fail(trf("Pick a sketch with points"));
        if (target.isEmpty())
            return fail(trf("Pick the body to drill"));
        p.insert(QStringLiteral("sketch"), sketch);
        p.insert(QStringLiteral("target"), target);
        p.insert(QStringLiteral("flip"), values_.value(QStringLiteral("flip")).toBool());
        std::vector<SketchEntId> points = pointPicks_;
        // No free-standing point but circle centres: drill at the centres.
        const bool anyFree = std::any_of(freePoints_.begin(), freePoints_.end(),
                                         [&](const auto& fp) { return centres_.count(fp.first) == 0; });
        if (points.empty() && !anyFree) {
            for (const auto& fp : freePoints_)
                points.push_back(fp.first);
        }
        if (!points.empty()) {
            QJsonArray list;
            for (const SketchEntId& id : points)
                list.append(idText(id.value));
            p.insert(QStringLiteral("points"), list);
        }
        const QString kindBase = values_.value(QStringLiteral("kind")).toString();
        const bool tapped = values_.value(QStringLiteral("tapped")).toBool();
        QString kind = kindBase;
        if (tapped)
            kind =
                kindBase == QLatin1String("simple") ? QStringLiteral("tapped") : kindBase + QStringLiteral("_tapped");
        p.insert(QStringLiteral("kind"), kind);
        if (!tapped && !setExpr(p, "diameter", QStringLiteral("diameter"), false))
            return fail(trf("'diameter' is empty: give a length"));
        const QString end = values_.value(QStringLiteral("end")).toString();
        p.insert(QStringLiteral("end"), end);
        if (end == QLatin1String("blind")) {
            if (!setExpr(p, "depth", QStringLiteral("depth"), false))
                return fail(trf("'depth' is empty: give a length"));
            p.insert(QStringLiteral("bottom"), values_.value(QStringLiteral("bottom")).toString());
            if (values_.value(QStringLiteral("bottom")).toString() == QLatin1String("angle"))
                setExpr(p, "tipAngle", QStringLiteral("tipAngle"), true);
        }
        if (kindBase == QLatin1String("counterbore")) {
            QJsonObject cb;
            setExpr(cb, "diameter", QStringLiteral("cboreDiameter"), false);
            setExpr(cb, "depth", QStringLiteral("cboreDepth"), false);
            p.insert(QStringLiteral("counterbore"), cb);
        } else if (kindBase == QLatin1String("countersink")) {
            QJsonObject cs;
            setExpr(cs, "diameter", QStringLiteral("csinkDiameter"), false);
            setExpr(cs, "angle", QStringLiteral("csinkAngle"), true);
            p.insert(QStringLiteral("countersink"), cs);
        }
        if (tapped) {
            const QString series = values_.value(QStringLiteral("series")).toString();
            const QString size = values_.value(QStringLiteral("size")).toString();
            QJsonObject entry;
            for (const QJsonValue& s : threadTable_) {
                if (s.toObject().value(QStringLiteral("id")).toString() != series)
                    continue;
                for (const QJsonValue& z : s.toObject().value(QStringLiteral("sizes")).toArray()) {
                    if (z.toObject().value(QStringLiteral("designation")).toString() == size)
                        entry = z.toObject();
                }
            }
            if (entry.isEmpty())
                return fail(threadTable_.isEmpty() ? trf("Reading the thread table…")
                                                   : trf("Pick a thread size of the series"));
            QJsonObject th{{QStringLiteral("designation"), size},
                           {QStringLiteral("standard"), series},
                           {QStringLiteral("nominalDiameter"), entry.value(QStringLiteral("majorDiameter"))},
                           {QStringLiteral("tapDrill"),
                            entry.value(QStringLiteral("tapDrill")).toObject().value(QStringLiteral("diameter"))}};
            if (entry.contains(QStringLiteral("tpi")))
                th.insert(QStringLiteral("tpi"), entry.value(QStringLiteral("tpi")));
            else
                th.insert(QStringLiteral("pitch"), entry.value(QStringLiteral("pitch")));
            const QString cls = values_.value(QStringLiteral("threadClass")).toString().trimmed();
            if (!cls.isEmpty())
                th.insert(QStringLiteral("class"), cls);
            setExpr(th, "depth", QStringLiteral("threadDepth"), false);
            p.insert(QStringLiteral("thread"), th);
        }
    } else if (type_ == QLatin1String("datum_plane")) {
        const QString base = values_.value(QStringLiteral("base")).toString();
        if (base == QLatin1String("face")) {
            if (faceRef_.isEmpty())
                return fail(trf("Select a planar face in the view"));
            p.insert(QStringLiteral("base"), QJsonObject{{QStringLiteral("face"), faceRef_}});
        } else if (base.startsWith(QLatin1String("datum:"))) {
            p.insert(QStringLiteral("base"), QJsonObject{{QStringLiteral("datum"), base.mid(6)}});
        } else {
            p.insert(QStringLiteral("base"), QJsonObject{{QStringLiteral("principal"), base}});
        }
        setExpr(p, "offset", QStringLiteral("offset"), false);
        p.insert(QStringLiteral("flip"), values_.value(QStringLiteral("flip")).toBool());
    }
    return jsonText(p);
}

std::optional<std::size_t> FeatureToolModel::insertIndex() const
{
    if (editId_ || !tree_ || tree_->rollbackIndex() < 0)
        return std::nullopt;
    return static_cast<std::size_t>(tree_->rollbackIndex() + 1);
}

Result<doc::Command> FeatureToolModel::command() const
{
    auto props = propsJson();
    if (!props)
        return tl::unexpected(props.error());
    const std::string type = type_.toStdString();
    QJsonObject args{{QStringLiteral("id"), idText(id_->value)}};
    QJsonObject propsObject = jsonObject(*props);
    doc::Command c;
    if (editId_) {
        // The edit replaces the keys it names; a key the feature had and the panel no longer gives is removed.
        const QJsonObject old = jsonObject(oldPropsJson_);
        for (auto it = old.begin(); it != old.end(); ++it) {
            if (!propsObject.contains(it.key()))
                propsObject.insert(it.key(), QJsonValue::Null);
        }
        args.insert(QStringLiteral("props"), propsObject);
        c.name = "feature." + type + ".edit";
        c.label = title().toStdString();
    } else {
        args.insert(QStringLiteral("props"), propsObject);
        if (const auto at = insertIndex())
            args.insert(QStringLiteral("index"), static_cast<int>(*at));
        c.name = "feature." + type + ".add";
        c.label = title().toStdString();
    }
    c.argsJson = jsonText(args);
    return c;
}

// ---------- preview ----------
void FeatureToolModel::schedulePreview()
{
    if (!active())
        return;
    previewTimer_.start();
}

void FeatureToolModel::setStatus(const QString& status, const QString& message, const QString& field)
{
    previewStatus_ = status;
    message_ = message;
    messageField_ = field.isEmpty() && !message.isEmpty() ? fieldOfMessage(message) : field;
    emit statusChanged();
}

QString FeatureToolModel::fieldOfMessage(const QString& message) const
{
    static const std::pair<const char*, const char*> fields[] = {
        {"'distance'", "distance"},       {"'draft'", "draft"},       {"'angle'", "angle"},
        {"'radius'", "radius"},           {"'depth'", "depth"},       {"'diameter'", "diameter"},
        {"'offset'", "offset"},           {"'tipAngle'", "tipAngle"}, {"counterbore", "cboreDiameter"},
        {"countersink", "csinkDiameter"}, {"'thread", "size"},        {"/props/distance", "distance"},
        {"/props/radius", "radius"},      {"/props/angle", "angle"},  {"/props/depth", "depth"},
        {"/props/diameter", "diameter"},
    };
    for (const auto& [needle, key] : fields) {
        if (message.contains(QLatin1String(needle)))
            return QLatin1String(key);
    }
    if (type_ == QLatin1String("fillet") && message.contains(QLatin1String("radius")))
        return QStringLiteral("radius");
    if (type_ == QLatin1String("chamfer") && message.contains(QLatin1String("distance")))
        return QStringLiteral("distance");
    return {};
}

void FeatureToolModel::startPreview()
{
    if (!active() || committing_ || (blendType(type_) && picking_))
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
    auto cmd = command();
    if (!cmd) {
        rejected_ = true;
        setStatus(QString(), qs(cmd.error().message));
        return;
    }
    rejected_ = false;
    ++previewSerial_;
    previewBase_ = d->state();
    const auto at = insertIndex();
    if (!at) {
        previewSide_ = false;
        previewBusy_ = true;
        setStatus(QStringLiteral("building"), QString());
        d->beginPreview(*cmd);
        if (const auto error = d->previewError()) {
            previewBusy_ = false;
            rejected_ = true;
            setStatus(QStringLiteral("failed"), qs(error->message));
        }
        return;
    }
    // Rolled back: the history up to the new feature, rebuilt by a rebuilder of our own.
    const doc::ICommandHandler* handler = a->commandTypes().find(cmd->name);
    if (handler == nullptr)
        return;
    auto next = handler->apply(d->state(), cmd->argsJson);
    if (!next) {
        rejected_ = true;
        setStatus(QStringLiteral("failed"), qs(next.error().message));
        return;
    }
    auto truncated = std::make_shared<doc::DocState>(**next);
    if (truncated->features.size() > *at + 1)
        truncated->features.resize(*at + 1);
    api::OpenDocument* od = document();
    if (!sideRebuilder_ || sideRebuilderDoc_ != docId_) {
        sideRebuilder_ = od ? api::feature_edit::makeSideRebuilder(*a, *od) : nullptr;
        sideRebuilderDoc_ = docId_;
    }
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (!sideRebuilder_ || jobs == nullptr)
        return;
    previewSide_ = true;
    previewBusy_ = true;
    setStatus(QStringLiteral("building"), QString());
    const std::uint64_t serial = previewSerial_;
    const doc::DocStatePtr state = truncated;
    auto rebuilder = sideRebuilder_;
    sideJob_ = jobs->submit<doc::BuildResultPtr>(
        JobInfo{"Feature preview", JobPriority::Interactive, false},
        [rebuilder, state](JobContext& ctx) -> Result<doc::BuildResultPtr> { return rebuilder->rebuild(state, ctx); });
    QPointer<FeatureToolModel> self(this);
    sideJob_.then([self, serial, state](Result<doc::BuildResultPtr> build) {
        if (!self || serial != self->previewSerial_)
            return;
        self->sideJob_ = {};
        self->onPreviewBuilt(state, build ? *build : nullptr);
    });
}

void FeatureToolModel::onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build)
{
    previewBusy_ = false;
    ++previewBuilds_;
    if (!active())
        return;
    if (!state || !build) {
        doc::Document* d = liveDoc();
        const auto error = d && !previewSide_ ? d->previewError() : std::nullopt;
        setStatus(QStringLiteral("failed"), error ? qs(error->message) : trf("The preview could not be built"));
    } else {
        const auto report = std::find_if(build->reports.begin(), build->reports.end(),
                                         [&](const doc::FeatureReport& r) { return r.id == *id_; });
        if (report == build->reports.end()) {
            setStatus(QStringLiteral("failed"), trf("The feature was not built"));
        } else if (report->status == doc::FeatureStatus::Failed || report->status == doc::FeatureStatus::NotBuilt) {
            QString m = qs(report->message);
            if (m.isEmpty())
                m = report->status == doc::FeatureStatus::NotBuilt ? trf("An earlier feature it needs did not build")
                                                                   : trf("The feature failed to build");
            setStatus(QStringLiteral("failed"), m);
        } else {
            showPreview(*state, *build);
            setStatus(report->status == doc::FeatureStatus::Warning ? QStringLiteral("warning") : QStringLiteral("ok"),
                      report->status == doc::FeatureStatus::Warning ? qs(report->message) : QString());
        }
    }
    if (previewPending_)
        startPreview();
}

void FeatureToolModel::showPreview(const doc::DocState& state, const doc::BuildResult& build)
{
    doc::Document* d = liveDoc();
    if (d == nullptr || (blendType(type_) && picking_))
        return;
    std::vector<api::viewer::Body> bodies = api::viewer::bodiesFromDocument(state, build);
    std::set<BodyId> hide;
    const bool all = previewSide_;
    if (!all) {
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
        if (current) {
            for (const auto& [id, built] : current->bodies) {
                if (build.bodies.find(id) == build.bodies.end())
                    hide.insert(id);
            }
        }
        bodies = std::move(changed);
    }
    render::MeshStyle style{render::Token::ModelFace, render::Token::Accent, 1.0f, true, false};
    overlay_->show(std::move(bodies), std::move(hide), all, style);
    if (type_ == QLatin1String("datum_plane"))
        showDatumPlane(state, build);
}

// The new plane as a square around its origin, sized to the model (resolved on a worker: a face needs the kernel).
void FeatureToolModel::showDatumPlane(const doc::DocState& state, const doc::BuildResult& build)
{
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr || !id_)
        return;
    auto st = std::make_shared<const doc::DocState>(state);
    auto bd = std::make_shared<const doc::BuildResult>(build);
    const std::string planeJson = "{\"datum\":\"" + id_->value.toString() + "\"}";
    double size = 50;
    if (render::IRenderViewport* viewport = scene_->viewport()) {
        if (const auto box = viewport->sceneBounds())
            size = std::max(10.0, 0.6 * length(sub(box->max, box->min)));
    }
    const std::uint64_t serial = previewSerial_;
    JobHandle<kernel::Plane> job = jobs->submit<kernel::Plane>(
        JobInfo{"Datum plane preview", JobPriority::Interactive, false},
        [st, bd, planeJson](JobContext& ctx) { return api::sketch_edit::resolvePlane(*st, bd.get(), planeJson, ctx); });
    QPointer<FeatureToolModel> self(this);
    job.then([self, serial, size](Result<kernel::Plane> plane) {
        if (!self || serial != self->previewSerial_ || !self->active() || !plane)
            return;
        const double h = size / 2;
        std::vector<Vec3> square;
        for (const Vec2 c : {Vec2{-h, -h}, Vec2{h, -h}, Vec2{h, h}, Vec2{-h, h}, Vec2{-h, -h}})
            square.push_back(worldOf(*plane, c));
        std::vector<Vec3> segs;
        appendPolyline(segs, square);
        // The normal, from the origin.
        segs.push_back(plane->origin);
        segs.push_back(add3(plane->origin, mul(plane->normal, size / 6)));
        self->lines_->set(QStringLiteral("datum"), std::move(segs), render::Token::Accent, 2.0f);
    });
}

// ---------- commit ----------
bool FeatureToolModel::canCommit() const
{
    // A feature that fails to build may be added (the tree then shows why); one the document refuses may not.
    return active() && !committing_ && !rejected_ && propsJson().has_value();
}

bool FeatureToolModel::commit()
{
    if (!active() || committing_)
        return false;
    api::IApp* a = app();
    auto cmd = command();
    if (a == nullptr)
        return false;
    if (!cmd) {
        setStatus(QStringLiteral("failed"), qs(cmd.error().message));
        return false;
    }
    committing_ = true;
    emit statusChanged();
    previewTimer_.stop();
    sideJob_.cancel();
    JobHandle<api::ActionResult> job = a->actions().invoke(cmd->name, cmd->argsJson, api::Caller::User);
    if (!job.valid()) {
        committing_ = false;
        setStatus(QStringLiteral("failed"), trf("The feature could not be added"));
        return false;
    }
    const FeatureId id = *id_;
    QPointer<FeatureToolModel> self(this);
    job.then([self, id](Result<api::ActionResult> r) {
        if (!self)
            return;
        self->committing_ = false;
        if (!r) {
            self->setStatus(QStringLiteral("failed"), qs(r.error().message));
            self->schedulePreview();
            return;
        }
        // The new feature lands after the rollback bar, which moves past it.
        if (self->tree_ && !self->editId_ && self->tree_->rollbackIndex() >= 0)
            self->tree_->setRollback(self->tree_->rollbackIndex() + 1);
        self->close();
        emit self->committed(idText(id.value));
    });
    return true;
}

// ---------- offer after a sketch ----------
void FeatureToolModel::acceptOffer()
{
    if (offer_.isEmpty())
        return;
    preferredSketch_ = featureIdOf(offer_.value(QStringLiteral("id")).toString());
    offer_.clear();
    emit offerChanged();
    start(QStringLiteral("extrude"));
}

void FeatureToolModel::dismissOffer()
{
    offer_.clear();
    emit offerChanged();
}

// ---------- options ----------
QVariantList FeatureToolModel::sketches() const
{
    QVariantList out;
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return out;
    const doc::DocStatePtr state = d->state();
    std::size_t limit = insertIndex().value_or(state->features.size());
    if (editId_) {
        const std::ptrdiff_t at = featureIndex(*state, *editId_);
        if (at >= 0)
            limit = static_cast<std::size_t>(at);
    }
    for (std::size_t i = 0; i < state->features.size() && i < limit; ++i) {
        const doc::Feature& f = state->features[i];
        if (f.type == "sketch")
            out.append(QVariantMap{{QStringLiteral("id"), idText(f.id.value)}, {QStringLiteral("name"), qs(f.name)}});
    }
    return out;
}

QVariantList FeatureToolModel::bodies() const
{
    QVariantList out;
    doc::Document* d = liveDoc();
    if (d == nullptr)
        return out;
    for (const doc::Body& b : d->state()->bodies)
        out.append(QVariantMap{{QStringLiteral("id"), idText(b.id.value)}, {QStringLiteral("name"), qs(b.name)}});
    return out;
}

QVariantList FeatureToolModel::axes() const
{
    QVariantList out{
        QVariantMap{{QStringLiteral("key"), QStringLiteral("x")}, {QStringLiteral("title"), trf("X axis")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("y")}, {QStringLiteral("title"), trf("Y axis")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("z")}, {QStringLiteral("title"), trf("Z axis")}}};
    int n = 0;
    for (const auto& [id, ends] : sketchLines_) {
        ++n;
        out.append(QVariantMap{{QStringLiteral("key"), QStringLiteral("line:") + idText(id.value)},
                               {QStringLiteral("title"), trf("Sketch line %1").arg(n)}});
    }
    return out;
}

QVariantList FeatureToolModel::planes() const
{
    QVariantList out{
        QVariantMap{{QStringLiteral("key"), QStringLiteral("xy")}, {QStringLiteral("title"), trf("XY plane")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("xz")}, {QStringLiteral("title"), trf("XZ plane")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("yz")}, {QStringLiteral("title"), trf("YZ plane")}}};
    doc::Document* d = liveDoc();
    if (d != nullptr) {
        for (const doc::Feature& f : d->state()->features) {
            if (f.type == "datum_plane" && (!editId_ || f.id != *editId_))
                out.append(QVariantMap{{QStringLiteral("key"), QStringLiteral("datum:") + idText(f.id.value)},
                                       {QStringLiteral("title"), qs(f.name)}});
        }
    }
    out.append(
        QVariantMap{{QStringLiteral("key"), QStringLiteral("face")}, {QStringLiteral("title"), trf("Selected face")}});
    return out;
}

QVariantList FeatureToolModel::threadSeries() const
{
    QVariantList out;
    for (const QJsonValue& s : threadTable_) {
        const QJsonObject o = s.toObject();
        out.append(QVariantMap{{QStringLiteral("id"), o.value(QStringLiteral("id")).toString()},
                               {QStringLiteral("name"), o.value(QStringLiteral("name")).toString()}});
    }
    return out;
}

QVariantList FeatureToolModel::threadSizes() const
{
    QVariantList out;
    const QString series = values_.value(QStringLiteral("series")).toString();
    for (const QJsonValue& s : threadTable_) {
        const QJsonObject o = s.toObject();
        if (o.value(QStringLiteral("id")).toString() != series)
            continue;
        for (const QJsonValue& z : o.value(QStringLiteral("sizes")).toArray()) {
            const QJsonObject size = z.toObject();
            out.append(QVariantMap{
                {QStringLiteral("designation"), size.value(QStringLiteral("designation")).toString()},
                {QStringLiteral("tapDrill"),
                 size.value(QStringLiteral("tapDrill")).toObject().value(QStringLiteral("size")).toString()}});
        }
    }
    return out;
}

// ---------- picks ----------
int FeatureToolModel::pickCount() const
{
    if (sweepType(type_))
        return static_cast<int>(regionPicks_.size());
    if (type_ == QLatin1String("hole"))
        return static_cast<int>(pointPicks_.size());
    if (blendType(type_))
        return edges_.size();
    if (type_ == QLatin1String("datum_plane"))
        return faceRef_.isEmpty() ? 0 : 1;
    return 0;
}

QString FeatureToolModel::pickText() const
{
    if (sweepType(type_)) {
        if (regions_.empty())
            return trf("No closed region in the sketch");
        if (regionPicks_.empty())
            return regions_.size() == 1 ? trf("The sketch's region") : trf("All %1 regions").arg(regions_.size());
        return trf("%1 of %2 regions").arg(regionPicks_.size()).arg(regions_.size());
    }
    if (type_ == QLatin1String("hole")) {
        if (freePoints_.empty())
            return trf("No point in the sketch");
        if (pointPicks_.empty())
            return trf("Every point (%1)").arg(freePoints_.size());
        return trf("%1 of %2 points").arg(pointPicks_.size()).arg(freePoints_.size());
    }
    if (blendType(type_))
        return edges_.size() == 1 ? trf("1 edge") : trf("%1 edges").arg(edges_.size());
    if (type_ == QLatin1String("datum_plane"))
        return faceRef_.isEmpty() ? trf("No face selected") : trf("A face");
    return {};
}

QString FeatureToolModel::pickHint() const
{
    if (sweepType(type_))
        return trf("Click regions of the sketch in the view to pick them");
    if (type_ == QLatin1String("hole"))
        return trf("Click points of the sketch in the view to pick them");
    if (blendType(type_))
        return picking_ ? trf("Click edges in the view (Shift adds, Ctrl toggles)") : trf("Pick edges to change them");
    if (type_ == QLatin1String("datum_plane"))
        return trf("Select a planar face in the view");
    return {};
}

bool FeatureToolModel::pickAt(qreal x, qreal y)
{
    if (!active() || !view_)
        return false;
    if (sweepType(type_)) {
        if (!plane_)
            return false;
        const auto ray = rayAt(QPointF(x, y));
        if (!ray)
            return false;
        const auto hit = nav::rayPlane(*ray, plane_->origin, plane_->normal);
        if (!hit)
            return false;
        const Vec2 q = planeOf(*plane_, *hit);
        const api::feature_edit::Region* best = nullptr;
        for (const api::feature_edit::Region& r : regions_) {
            if (!insidePolygon(r.outer, q))
                continue;
            if (std::any_of(r.holes.begin(), r.holes.end(), [&](const auto& h) { return insidePolygon(h, q); }))
                continue;
            if (best == nullptr || r.area < best->area)
                best = &r;
        }
        if (best == nullptr)
            return false;
        auto it = std::find(regionPicks_.begin(), regionPicks_.end(), best->entities);
        if (it != regionPicks_.end())
            regionPicks_.erase(it);
        else
            regionPicks_.push_back(best->entities);
    } else if (type_ == QLatin1String("hole")) {
        if (!plane_)
            return false;
        const double tol = 10;
        const std::pair<SketchEntId, Vec2>* best = nullptr;
        double bestD = tol;
        for (const auto& fp : freePoints_) {
            const auto v = toView(worldOf(*plane_, fp.second));
            if (!v)
                continue;
            const double dd = std::hypot(v->x() - x, v->y() - y);
            if (dd <= bestD) {
                bestD = dd;
                best = &fp;
            }
        }
        if (best == nullptr)
            return false;
        auto it = std::find(pointPicks_.begin(), pointPicks_.end(), best->first);
        if (it != pointPicks_.end())
            pointPicks_.erase(it);
        else
            pointPicks_.push_back(best->first);
    } else {
        return false;
    }
    emit picksChanged();
    updateHighlights();
    updateHandles();
    schedulePreview();
    return true;
}

bool FeatureToolModel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == view_ && active()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* m = static_cast<QMouseEvent*>(event);
            pressed_ = m->button() == Qt::LeftButton;
            pressAt_ = m->position();
        } else if (event->type() == QEvent::MouseButtonRelease && pressed_) {
            auto* m = static_cast<QMouseEvent*>(event);
            pressed_ = false;
            const QPointF d = m->position() - pressAt_;
            if (m->button() == Qt::LeftButton && std::hypot(d.x(), d.y()) < 4)
                pickAt(m->position().x(), m->position().y());
        }
    }
    return QObject::eventFilter(watched, event);
}

// What is picked, drawn in the view: the picked regions (all, when none is picked) and the hole points.
void FeatureToolModel::updateHighlights()
{
    lines_->clear();
    if (!active() || !plane_)
        return;
    if (sweepType(type_)) {
        std::vector<Vec3> segs;
        for (const api::feature_edit::Region& r : regions_) {
            const bool picked = regionPicks_.empty() ||
                                std::find(regionPicks_.begin(), regionPicks_.end(), r.entities) != regionPicks_.end();
            if (!picked)
                continue;
            auto loop = [&](const std::vector<Vec2>& poly) {
                std::vector<Vec3> w;
                for (const Vec2& p : poly)
                    w.push_back(worldOf(*plane_, p));
                if (!w.empty())
                    w.push_back(w.front());
                appendPolyline(segs, w);
            };
            loop(r.outer);
            for (const auto& h : r.holes)
                loop(h);
        }
        lines_->set(QStringLiteral("regions"), std::move(segs), render::Token::Accent, 2.5f);
    } else if (type_ == QLatin1String("hole")) {
        std::vector<Vec3> segs;
        // A small cross (about 6 pixels) at every drill point (picked, or all).
        for (const auto& fp : freePoints_) {
            double size = 1.0;
            const Vec3 at = worldOf(*plane_, fp.second);
            const auto pa = toView(at);
            const auto pb = toView(add3(at, plane_->xDir));
            if (pa && pb) {
                const double px = std::hypot(pa->x() - pb->x(), pa->y() - pb->y());
                if (px > 1e-6)
                    size = 6.0 / px;
            }
            const bool picked =
                pointPicks_.empty() || std::find(pointPicks_.begin(), pointPicks_.end(), fp.first) != pointPicks_.end();
            if (!picked)
                continue;
            const Vec2 c = fp.second;
            for (const auto& [a, b] : {std::pair<Vec2, Vec2>{{c.x - size, c.y - size}, {c.x + size, c.y + size}},
                                       std::pair<Vec2, Vec2>{{c.x - size, c.y + size}, {c.x + size, c.y - size}}}) {
                segs.push_back(worldOf(*plane_, a));
                segs.push_back(worldOf(*plane_, b));
            }
        }
        lines_->set(QStringLiteral("points"), std::move(segs), render::Token::Accent, 2.5f);
    }
}

// ---------- view and handles ----------
QObject* FeatureToolModel::view() const
{
    return view_;
}

void FeatureToolModel::setView(QObject* view)
{
    auto* item = qobject_cast<ViewportItem*>(view);
    if (item == view_)
        return;
    for (const auto& c : viewConnections_)
        disconnect(c);
    viewConnections_.clear();
    if (view_)
        view_->removeEventFilter(this);
    view_ = item;
    if (view_) {
        view_->installEventFilter(this);
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

std::optional<QPointF> FeatureToolModel::toView(Vec3 world) const
{
    if (!view_)
        return std::nullopt;
    return nav::project(view_->camera(), world, view_->width(), view_->height());
}

std::optional<render::Ray> FeatureToolModel::rayAt(QPointF viewPos) const
{
    if (!view_ || view_->width() <= 0 || view_->height() <= 0)
        return std::nullopt;
    return render::cameraRay(view_->camera(), viewPos.x(), viewPos.y(), static_cast<int>(view_->width()),
                             static_cast<int>(view_->height()));
}

void FeatureToolModel::updateHandles()
{
    emit handlesChanged();
}

// The handle's base point and direction (unit) in the world; the handle's tip is base + dir * value.
std::optional<Vec3> FeatureToolModel::handleBase(const QString& key, Vec3* dir) const
{
    if (!active())
        return std::nullopt;
    if (type_ == QLatin1String("extrude") && key == QLatin1String("distance")) {
        if (!plane_ || regions_.empty())
            return std::nullopt;
        // The centre of the picked regions' outlines (all regions when none is picked).
        Vec2 c{0, 0};
        int n = 0;
        for (const api::feature_edit::Region& r : regions_) {
            if (!regionPicks_.empty() &&
                std::find(regionPicks_.begin(), regionPicks_.end(), r.entities) == regionPicks_.end())
                continue;
            for (const Vec2& p : r.outer) {
                c.x += p.x;
                c.y += p.y;
                ++n;
            }
        }
        if (n == 0)
            return std::nullopt;
        c = {c.x / n, c.y / n};
        const bool flip = values_.value(QStringLiteral("flip")).toBool();
        *dir = mul(plane_->normal, flip ? -1.0 : 1.0);
        Vec3 base = worldOf(*plane_, c);
        if (values_.value(QStringLiteral("symmetric")).toBool()) {
            const double d = numberOf(QStringLiteral("distance"), false).value_or(0);
            base = sub(base, mul(*dir, std::abs(d) / 2));
        }
        return base;
    }
    if (type_ == QLatin1String("hole") && key == QLatin1String("depth")) {
        if (!plane_ || freePoints_.empty() || values_.value(QStringLiteral("end")).toString() != QLatin1String("blind"))
            return std::nullopt;
        Vec2 c = freePoints_.front().second;
        if (!pointPicks_.empty()) {
            for (const auto& fp : freePoints_) {
                if (fp.first == pointPicks_.front())
                    c = fp.second;
            }
        }
        const bool flip = values_.value(QStringLiteral("flip")).toBool();
        *dir = mul(plane_->normal, flip ? 1.0 : -1.0);
        return worldOf(*plane_, c);
    }
    if (blendType(type_) && (key == QLatin1String("radius") || key == QLatin1String("distance"))) {
        if (edgePoints_.empty() || !scene_ || picking_)
            return std::nullopt;
        const Vec3 at = edgePoints_.front();
        // Towards the middle of the body: the side the blend takes material from.
        std::optional<Box3> box;
        if (selection_) {
            for (const SelectionEntry& e : selection_->entries()) {
                const render::PickHit hit{scene_->itemForBody(e.body), kernel::SubKind::Solid, 0, {}, 0};
                box = scene_->boundsOf(std::span<const render::PickHit>(&hit, 1));
                break;
            }
        }
        Vec3 centre =
            box ? Vec3{(box->min.x + box->max.x) / 2, (box->min.y + box->max.y) / 2, (box->min.z + box->max.z) / 2}
                : Vec3{0, 0, 0};
        Vec3 d = unit(sub(centre, at));
        if (length(d) < 0.5)
            d = {0, 0, -1};
        *dir = d;
        return at;
    }
    return std::nullopt;
}

QVariantList FeatureToolModel::handles() const
{
    QVariantList out;
    if (!active() || !view_)
        return out;
    const auto add = [&](const QString& key, const QString& kind, Vec3 base, Vec3 tip) {
        const auto b = toView(base);
        const auto t = toView(tip);
        if (!b || !t)
            return;
        out.append(QVariantMap{{QStringLiteral("key"), key},
                               {QStringLiteral("kind"), kind},
                               {QStringLiteral("x"), b->x()},
                               {QStringLiteral("y"), b->y()},
                               {QStringLiteral("tipX"), t->x()},
                               {QStringLiteral("tipY"), t->y()}});
    };
    Vec3 dir{};
    if (type_ == QLatin1String("extrude")) {
        if (const auto base = handleBase(QStringLiteral("distance"), &dir)) {
            const double d = std::abs(numberOf(QStringLiteral("distance"), false).value_or(0));
            add(QStringLiteral("distance"), QStringLiteral("arrow"), *base, add3(*base, mul(dir, d)));
        }
    } else if (type_ == QLatin1String("hole")) {
        if (const auto base = handleBase(QStringLiteral("depth"), &dir)) {
            const double d = std::abs(numberOf(QStringLiteral("depth"), false).value_or(0));
            add(QStringLiteral("depth"), QStringLiteral("arrow"), *base, add3(*base, mul(dir, d)));
        }
    } else if (blendType(type_)) {
        const QString key = type_ == QLatin1String("fillet") ? QStringLiteral("radius") : QStringLiteral("distance");
        if (const auto base = handleBase(key, &dir)) {
            const double r = std::abs(numberOf(key, false).value_or(0));
            add(key, QStringLiteral("radius"), *base, add3(*base, mul(dir, r)));
        }
    } else if (type_ == QLatin1String("revolve") && plane_ && !regions_.empty()) {
        // Axis, the regions' centre, and that centre turned by the angle.
        Vec3 axisOrigin{0, 0, 0}, axisDir{1, 0, 0};
        const QString axis = values_.value(QStringLiteral("axis")).toString();
        if (axis == QLatin1String("y"))
            axisDir = {0, 1, 0};
        else if (axis == QLatin1String("z"))
            axisDir = {0, 0, 1};
        else if (axis.startsWith(QLatin1String("line:"))) {
            for (const auto& [id, ends] : sketchLines_) {
                if (idText(id.value) == axis.mid(5)) {
                    axisOrigin = worldOf(*plane_, ends.first);
                    axisDir = unit(sub(worldOf(*plane_, ends.second), axisOrigin));
                }
            }
        }
        Vec2 c{0, 0};
        int n = 0;
        for (const auto& r : regions_) {
            for (const Vec2& p : r.outer) {
                c.x += p.x;
                c.y += p.y;
                ++n;
            }
        }
        if (n > 0 && length(axisDir) > 0.5) {
            const Vec3 R = worldOf(*plane_, Vec2{c.x / n, c.y / n});
            const Vec3 centre = add3(axisOrigin, mul(axisDir, dot(sub(R, axisOrigin), axisDir)));
            const Vec3 radial = sub(R, centre);
            double angle = numberOf(QStringLiteral("angle"), true).value_or(2 * kPi);
            if (values_.value(QStringLiteral("flip")).toBool())
                angle = -angle;
            const Vec3 tip = add3(centre, rotate(radial, axisDir, angle));
            add(QStringLiteral("angle"), QStringLiteral("angle"), centre, tip);
        }
    }
    return out;
}

void FeatureToolModel::dragHandle(const QString& key, qreal x, qreal y, bool finished)
{
    if (!active())
        return;
    const auto ray = rayAt(QPointF(x, y));
    if (ray) {
        Vec3 dir{};
        if (key == QLatin1String("angle") && type_ == QLatin1String("revolve")) {
            // The angle about the axis from the regions' centre to where the pointer meets the turning plane.
            Vec3 axisOrigin{0, 0, 0}, axisDir{1, 0, 0};
            const QString axis = values_.value(QStringLiteral("axis")).toString();
            if (axis == QLatin1String("y"))
                axisDir = {0, 1, 0};
            else if (axis == QLatin1String("z"))
                axisDir = {0, 0, 1};
            else if (axis.startsWith(QLatin1String("line:")) && plane_) {
                for (const auto& [id, ends] : sketchLines_) {
                    if (idText(id.value) == axis.mid(5)) {
                        axisOrigin = worldOf(*plane_, ends.first);
                        axisDir = unit(sub(worldOf(*plane_, ends.second), axisOrigin));
                    }
                }
            }
            if (plane_ && !regions_.empty()) {
                Vec2 c{0, 0};
                int n = 0;
                for (const auto& r : regions_) {
                    for (const Vec2& p : r.outer) {
                        c.x += p.x;
                        c.y += p.y;
                        ++n;
                    }
                }
                const Vec3 R = worldOf(*plane_, Vec2{c.x / std::max(n, 1), c.y / std::max(n, 1)});
                const Vec3 centre = add3(axisOrigin, mul(axisDir, dot(sub(R, axisOrigin), axisDir)));
                const Vec3 radial = sub(R, centre);
                if (const auto hit = nav::rayPlane(*ray, centre, axisDir)) {
                    const Vec3 w = sub(*hit, centre);
                    double a = std::atan2(dot(cross(radial, w), axisDir), dot(radial, w));
                    if (values_.value(QStringLiteral("flip")).toBool())
                        a = -a;
                    if (a <= 0)
                        a += 2 * kPi;
                    values_[QStringLiteral("angle")] =
                        QLocale::c().toString(a * 180 / kPi, 'f', 1) + QStringLiteral(" deg");
                }
            }
        } else if (const auto base = handleBase(key, &dir)) {
            if (const auto t = closestOnLine(*base, dir, *ray)) {
                double v = std::max(*t, 0.001);
                if (key == QLatin1String("distance") && type_ == QLatin1String("extrude") &&
                    values_.value(QStringLiteral("symmetric")).toBool()) {
                    // The base moved back by half the distance: the pointer gives the whole length.
                    v = std::max(*t, 0.001);
                }
                values_[key] = lengthText(v);
            }
        }
        dragKey_ = finished ? QString() : key;
        emit valuesChanged();
        emit handlesChanged();
        schedulePreview();
    }
    if (finished) {
        dragKey_.clear();
        emit handlesChanged();
    }
}

} // namespace ray::ui
