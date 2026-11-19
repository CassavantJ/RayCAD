#include "sketch_mode.h"

#include "../app_context.h"
#include "../palette_commands.h"
#include "../scene_controller.h"
#include "../selection_model.h"
#include "../session.h"
#include "../theme_object.h"
#include "../units.h"
#include "../viewport_item.h"
#include "../viewport_navigation.h"

#include <ray_api/sketch_edit.h>
#include <ray_render/viewport.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QVariantMap>
#include <QtGui/QKeyEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ray::ui {

namespace su = sketchui;

namespace {

constexpr const char* kTrContext = "ray::ui::SketchModel";

QString trs(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

std::string_view sv(const QByteArray& b)
{
    return {b.constData(), static_cast<std::size_t>(b.size())};
}

Vec3 v3add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 v3sub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 v3mul(Vec3 a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
double v3dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 v3cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double v3len(Vec3 a)
{
    return std::sqrt(v3dot(a, a));
}

Vec3 yDirOf(const kernel::Plane& p)
{
    return v3cross(p.normal, p.xDir);
}

Vec3 worldOf(const kernel::Plane& p, Vec2 uv)
{
    return v3add(p.origin, v3add(v3mul(p.xDir, uv.x), v3mul(yDirOf(p), uv.y)));
}

std::string principalJson(const QString& key)
{
    return "{\"principal\":\"" + key.toStdString() + "\"}";
}

bool isPrintable(const QKeyEvent* e)
{
    const QString t = e->text();
    if (t.isEmpty())
        return false;
    const QChar c = t.at(0);
    return c.isPrint() && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
}

bool startsNumber(const QKeyEvent* e)
{
    const QString t = e->text();
    if (t.isEmpty() || (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        return false;
    const QChar c = t.at(0);
    return c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char(',');
}

} // namespace

SketchModel::SketchModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                         QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), session_(session), context_(context)
{
    if (session_) {
        connect(session_, &Session::documentChanged, this, [this] {
            if (!session_)
                return;
            const std::string id = session_->documentId().toStdString();
            if (active_ && id != docId_)
                leave();
            if (placing_ && id.empty()) {
                placing_ = false;
                emit placingChanged();
            }
        });
    }
    if (selection_) {
        connect(selection_, &SelectionModel::changed, this, [this] {
            if (placing_)
                emit placingChanged(); // the "selected face" entry
        });
    }
}

SketchModel::~SketchModel()
{
    if (commands_) {
        for (const char* prefix : {"sketch.tool.", "sketch.mode.", "feature.sketch.add"})
            commands_->setUiHandler(QString::fromLatin1(prefix), {});
    }
    drag_.reset();
    unsubscribe();
}

SketchModel* SketchModel::create(QQmlEngine* qml, QJSEngine*)
{
    SceneController* scene = nullptr;
    SelectionModel* selection = nullptr;
    Session* session = nullptr;
    AppContext* context = nullptr;
    CommandCatalog* commands = nullptr;
    UnitsObject* units = nullptr;
    ThemeObject* theme = nullptr;
    if (qml != nullptr) {
        const QString uri = QStringLiteral("RayCad.Ui");
        scene = qml->singletonInstance<SceneController*>(uri, QStringLiteral("Scene"));
        selection = qml->singletonInstance<SelectionModel*>(uri, QStringLiteral("Selection"));
        session = qml->singletonInstance<Session*>(uri, QStringLiteral("Session"));
        context = qml->singletonInstance<AppContext*>(uri, QStringLiteral("AppContext"));
        commands = qml->singletonInstance<CommandCatalog*>(uri, QStringLiteral("Commands"));
        units = qml->singletonInstance<UnitsObject*>(uri, QStringLiteral("Units"));
        theme = qml->singletonInstance<ThemeObject*>(uri, QStringLiteral("Theme"));
    }
    auto* model = new SketchModel(scene, selection, session, context);
    model->setCommands(commands);
    model->setUnits(units);
    model->setTheme(theme);
    return model; // owned by the engine
}

void SketchModel::setCommands(CommandCatalog* commands)
{
    commands_ = commands;
    if (!commands_)
        return;
    QPointer<SketchModel> self(this);
    const CommandCatalog::UiHandler handler{
        [self](const QString& id) { return self ? self->commandReason(id) : trs("The sketch tools are not available"); },
        [self](const QString& id) { return self && self->runCommand(id); }};
    for (const char* prefix : {"sketch.tool.", "sketch.mode.", "feature.sketch.add"})
        commands_->setUiHandler(QString::fromLatin1(prefix), handler);
}

// ---------- document access ----------
api::IApp* SketchModel::app() const
{
    return context_ ? context_->app() : nullptr;
}

api::OpenDocument* SketchModel::document() const
{
    api::IApp* a = app();
    if (a == nullptr)
        return nullptr;
    return docId_.empty() ? a->active() : a->findDocument(docId_);
}

doc::Document* SketchModel::liveDoc() const
{
    api::IApp* a = app();
    if (a == nullptr || docId_.empty() || doc_ == nullptr)
        return nullptr;
    api::OpenDocument* od = a->findDocument(docId_);
    return od != nullptr && od->document.get() == doc_ ? doc_ : nullptr;
}

void SketchModel::subscribe()
{
    unsubscribe();
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document)
        return;
    docId_ = od->id;
    doc_ = od->document.get();
    const std::uint64_t generation = ++docGeneration_;
    QPointer<SketchModel> self(this);
    docListener_ = doc_->subscribe([self, generation](doc::DocStatePtr, doc::BuildResultPtr) {
        if (self && self->docGeneration_ == generation)
            self->syncFromDocument();
    });
}

void SketchModel::unsubscribe()
{
    if (doc::Document* d = liveDoc(); d != nullptr && docListener_ != 0)
        d->unsubscribe(docListener_);
    doc_ = nullptr;
    docListener_ = 0;
    docId_.clear();
    ++docGeneration_;
}

void SketchModel::syncFromDocument()
{
    if (!active_ || inFlight_ > 0 || dragging_)
        return;
    doc::Document* d = liveDoc();
    if (d == nullptr) {
        leave();
        return;
    }
    const doc::DocStatePtr state = d->state();
    auto model = api::sketch_edit::modelOf(*state, feature_);
    if (model) {
        pending_ = false;
        setModel(std::move(*model));
        return;
    }
    if (created_) {
        // The step that made the feature was undone: the mode stays on the same plane, empty.
        pending_ = true;
        setModel({});
        return;
    }
    if (context_)
        context_->showMessage(trs("The sketch is no longer in the document"), false);
    leave();
}

// ---------- entering and leaving ----------
QVariantList SketchModel::planes() const
{
    QVariantList out;
    const auto add = [&out](const QString& key, const QString& title, const QString& kind) {
        QVariantMap m;
        m.insert(QStringLiteral("key"), key);
        m.insert(QStringLiteral("title"), title);
        m.insert(QStringLiteral("kind"), kind);
        out << m;
    };
    add(QStringLiteral("xy"), trs("XY plane (top)"), QStringLiteral("principal"));
    add(QStringLiteral("xz"), trs("XZ plane (front)"), QStringLiteral("principal"));
    add(QStringLiteral("yz"), trs("YZ plane (right)"), QStringLiteral("principal"));
    if (api::OpenDocument* od = document(); od != nullptr && od->document) {
        for (const doc::Feature& f : od->document->state()->features) {
            if (f.type == "datum_plane" && !f.suppressed)
                add(QStringLiteral("datum:") + qs(f.id.value.toString()), qs(f.name.empty() ? f.type : f.name),
                    QStringLiteral("datum"));
        }
    }
    if (selection_) {
        int faces = 0;
        for (const SelectionEntry& e : selection_->entries())
            faces += e.hit.kind == kernel::SubKind::Face ? 1 : 0;
        if (faces == 1 && selection_->entries().size() == 1)
            add(QStringLiteral("face"), trs("Selected face"), QStringLiteral("face"));
    }
    return out;
}

void SketchModel::newSketch()
{
    api::OpenDocument* od = app() != nullptr ? app()->active() : nullptr;
    if (od == nullptr) {
        if (context_)
            context_->showMessage(trs("Open or create a document first"), true);
        return;
    }
    if (active_)
        return;
    placing_ = true;
    emit placingChanged();
    updateHint();
}

void SketchModel::cancelPlacing()
{
    if (!placing_)
        return;
    placing_ = false;
    ++placeSerial_;
    emit placingChanged();
    updateHint();
}

QString SketchModel::nextName(const char* prefix, const char* type) const
{
    int n = 1;
    if (api::OpenDocument* od = document(); od != nullptr && od->document) {
        for (const doc::Feature& f : od->document->state()->features)
            n += f.type == type ? 1 : 0;
    }
    return QStringLiteral("%1 %2").arg(QString::fromLatin1(prefix)).arg(n);
}

void SketchModel::placeOn(const QString& key, const QString& offsetText)
{
    api::IApp* a = app();
    api::OpenDocument* od = a != nullptr ? a->active() : nullptr;
    if (od == nullptr || !od->document) {
        if (context_)
            context_->showMessage(trs("Open or create a document first"), true);
        return;
    }
    if (od->readOnly) {
        if (context_)
            context_->showMessage(trs("The document is read-only"), true);
        return;
    }
    double offset = 0;
    const QString trimmed = offsetText.trimmed();
    if (!trimmed.isEmpty()) {
        offset = units_ ? units_->parseLength(trimmed) : trimmed.toDouble();
        if (!std::isfinite(offset)) {
            if (context_)
                context_->showMessage(trs("The offset '%1' is not a length").arg(trimmed), true);
            return;
        }
    }
    const std::uint64_t serial = ++placeSerial_;
    QPointer<SketchModel> self(this);
    // Step 2: an offset makes a datum plane on the chosen base first.
    auto withOffset = [self, serial, offset](std::string planeJson) {
        if (!self || serial != self->placeSerial_)
            return;
        if (offset == 0) {
            self->placeResolved(std::move(planeJson));
            return;
        }
        api::IApp* app = self->app();
        if (app == nullptr)
            return;
        const FeatureId datum{Uuid::generate()};
        const QByteArray args = QByteArray::fromStdString(api::sketch_edit::datumAddArgs(
            datum, self->nextName("Plane", "datum_plane").toStdString(), planeJson, offset));
        JobHandle<api::ActionResult> job = app->actions().invoke("feature.datum_plane.add", sv(args), api::Caller::User);
        job.then([self, serial, datum](Result<api::ActionResult> r) {
            if (!self || serial != self->placeSerial_)
                return;
            if (!r) {
                if (self->context_)
                    self->context_->showMessage(trs("The datum plane could not be added: %1").arg(qs(r.error().message)),
                                                true);
                return;
            }
            self->placeResolved("{\"datum\":\"" + datum.value.toString() + "\"}");
        });
    };
    if (key == QLatin1String("xy") || key == QLatin1String("xz") || key == QLatin1String("yz")) {
        withOffset(principalJson(key));
        return;
    }
    if (key.startsWith(QLatin1String("datum:"))) {
        withOffset("{\"datum\":\"" + key.mid(6).toStdString() + "\"}");
        return;
    }
    if (key == QLatin1String("face")) {
        const SelectionEntry* face = nullptr;
        if (selection_) {
            for (const SelectionEntry& e : selection_->entries()) {
                if (e.hit.kind == kernel::SubKind::Face)
                    face = &e;
            }
        }
        const api::viewer::Body* body = face != nullptr && scene_ ? scene_->body(face->body) : nullptr;
        if (body == nullptr) {
            if (context_)
                context_->showMessage(trs("Select a planar face first"), true);
            return;
        }
        const std::uint32_t index = face->sub ? face->sub->index : face->hit.subIndex;
        const std::string args = "{\"body\":\"" + body->id.value.toString() + "\",\"kind\":\"face\",\"index\":" +
                                 std::to_string(index) + "}";
        JobHandle<api::ActionResult> job = a->actions().invoke("model.reference", args, api::Caller::User);
        job.then([self, serial, withOffset](Result<api::ActionResult> r) {
            if (!self || serial != self->placeSerial_)
                return;
            if (!r) {
                if (self->context_)
                    self->context_->showMessage(trs("That face cannot hold a sketch: %1").arg(qs(r.error().message)),
                                                true);
                return;
            }
            // {"ref":<TopoRef>} (canonical): the reference is everything after "{\"ref\":" up to the last brace.
            const std::string& text = r->resultJson;
            const std::string prefix = "{\"ref\":";
            if (text.rfind(prefix, 0) != 0 || text.size() < prefix.size() + 1) {
                if (self->context_)
                    self->context_->showMessage(trs("That face cannot hold a sketch"), true);
                return;
            }
            withOffset("{\"face\":" + text.substr(prefix.size(), text.size() - prefix.size() - 1) + "}");
        });
        return;
    }
    if (context_)
        context_->showMessage(trs("Unknown sketch plane %1").arg(key), true);
}

void SketchModel::placeResolved(std::string planeJson)
{
    api::OpenDocument* od = document();
    if (od == nullptr || !od->document)
        return;
    const doc::DocStatePtr state = od->document->state();
    const doc::BuildResultPtr build = od->document->build();
    const FeatureId id{Uuid::generate()};
    const QString name = nextName("Sketch", "sketch");
    if (planeJson.find("\"face\"") == std::string::npos) {
        JobContext ctx;
        auto plane = api::sketch_edit::resolvePlane(*state, build.get(), planeJson, ctx);
        if (!plane) {
            if (context_)
                context_->showMessage(trs("The sketch plane cannot be used: %1").arg(qs(plane.error().message)), true);
            return;
        }
        enter(id, name, std::move(planeJson), *plane, true);
        return;
    }
    // A face: kernel queries, on a worker.
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return;
    const std::uint64_t serial = placeSerial_;
    JobHandle<kernel::Plane> job = jobs->submit<kernel::Plane>(
        JobInfo{"Sketch plane", JobPriority::Interactive, false},
        [state, build, planeJson](JobContext& ctx) { return api::sketch_edit::resolvePlane(*state, build.get(), planeJson, ctx); });
    QPointer<SketchModel> self(this);
    job.then([self, serial, id, name, planeJson](Result<kernel::Plane> plane) {
        if (!self || serial != self->placeSerial_)
            return;
        if (!plane) {
            if (self->context_)
                self->context_->showMessage(trs("The sketch plane cannot be used: %1").arg(qs(plane.error().message)),
                                            true);
            return;
        }
        self->enter(id, name, planeJson, *plane, true);
    });
}

bool SketchModel::editFeature(const QString& featureId)
{
    api::OpenDocument* od = app() != nullptr ? app()->active() : nullptr;
    if (od == nullptr || !od->document)
        return false;
    const FeatureId id{Uuid::fromString(featureId.toStdString())};
    const doc::DocStatePtr state = od->document->state();
    auto model = api::sketch_edit::modelOf(*state, id);
    auto planeJson = api::sketch_edit::planeJsonOf(*state, id);
    if (!model || !planeJson)
        return false;
    if (active_)
        leave();
    QString name;
    for (const doc::Feature& f : state->features) {
        if (f.id == id)
            name = qs(f.name.empty() ? f.type : f.name);
    }
    ++placeSerial_;
    if (planeJson->find("\"face\"") == std::string::npos) {
        JobContext ctx;
        auto plane = api::sketch_edit::resolvePlane(*state, od->document->build().get(), *planeJson, ctx);
        if (!plane) {
            if (context_)
                context_->showMessage(trs("The sketch plane cannot be used: %1").arg(qs(plane.error().message)), true);
            return false;
        }
        enter(id, name, *planeJson, *plane, false);
        return true;
    }
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return false;
    const doc::BuildResultPtr build = od->document->build();
    const std::string pj = *planeJson;
    const std::uint64_t serial = placeSerial_;
    JobHandle<kernel::Plane> job =
        jobs->submit<kernel::Plane>(JobInfo{"Sketch plane", JobPriority::Interactive, false},
                                    [state, build, pj](JobContext& ctx) {
                                        return api::sketch_edit::resolvePlane(*state, build.get(), pj, ctx);
                                    });
    QPointer<SketchModel> self(this);
    job.then([self, serial, id, name, pj](Result<kernel::Plane> plane) {
        if (!self || serial != self->placeSerial_)
            return;
        if (!plane) {
            if (self->context_)
                self->context_->showMessage(trs("The sketch plane cannot be used: %1").arg(qs(plane.error().message)),
                                            true);
            return;
        }
        self->enter(id, name, pj, *plane, false);
    });
    return true;
}

bool SketchModel::editFeatureAt(int index)
{
    api::OpenDocument* od = app() != nullptr ? app()->active() : nullptr;
    if (od == nullptr || !od->document || index < 0)
        return false;
    const doc::DocStatePtr state = od->document->state();
    if (static_cast<std::size_t>(index) >= state->features.size())
        return false;
    const doc::Feature& f = state->features[static_cast<std::size_t>(index)];
    if (f.type != "sketch")
        return false;
    return editFeature(qs(f.id.value.toString()));
}

void SketchModel::enter(FeatureId id, QString name, std::string planeJson, kernel::Plane plane, bool pending)
{
    placing_ = false;
    active_ = true;
    pending_ = pending;
    created_ = pending;
    feature_ = id;
    name_ = std::move(name);
    planeJson_ = std::move(planeJson);
    plane_ = plane;
    selected_.clear();
    hover_ = {};
    typedText_.clear();
    activeField_ = -1;
    subscribe();
    sketch::Sketch model;
    if (!pending) {
        if (doc::Document* d = liveDoc()) {
            if (auto m = api::sketch_edit::modelOf(*d->state(), feature_))
                model = std::move(*m);
        }
    }
    engine_.setKind(pending ? su::ToolKind::Line : su::ToolKind::Select);
    setModel(std::move(model));
    turnView();
    emit placingChanged();
    emit activeChanged();
    emit toolChanged();
    emit selectionChanged();
    updateHint();
    if (context_)
        context_->setStatusText(trs("Editing %1").arg(name_));
}

void SketchModel::leave()
{
    const bool was = active_;
    drag_.reset();
    dragging_ = false;
    dragSketch_.reset();
    unsubscribe();
    active_ = false;
    pending_ = false;
    created_ = false;
    model_ = {};
    solved_.reset();
    display_ = {};
    selected_.clear();
    hover_ = {};
    typedText_.clear();
    activeField_ = -1;
    engine_.setKind(su::ToolKind::Select);
    status_.clear();
    freeDof_ = 0;
    conflictEntities_.clear();
    overlay_ = {};
    glyphs_.clear();
    if (was) {
        emit activeChanged();
        emit toolChanged();
        emit statusChanged();
        emit modelChanged();
        emit selectionChanged();
        emit draggingChanged();
        emit overlayChanged();
        emit fieldsChanged();
        updateHint();
        if (context_)
            context_->setStatusText(QString());
    }
}

void SketchModel::finish()
{
    if (placing_) {
        cancelPlacing();
        return;
    }
    leave();
}

void SketchModel::turnView()
{
    if (!view_)
        return;
    render::Camera cam = view_->camera();
    const Vec3 n = plane_.normal;
    const Vec3 target = v3sub(cam.target, v3mul(n, v3dot(v3sub(cam.target, plane_.origin), n)));
    double dist = v3len(v3sub(cam.eye, cam.target));
    if (!(dist > 0))
        dist = 100;
    cam.target = target;
    cam.eye = v3add(target, v3mul(n, dist));
    cam.up = yDirOf(plane_);
    if (const auto b = display_.bounds()) {
        Box3 box{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
        for (const Vec2 c : {b->first, b->second, Vec2{b->first.x, b->second.y}, Vec2{b->second.x, b->first.y}}) {
            const Vec3 w = worldOf(plane_, c);
            box.min = {std::min(box.min.x, w.x), std::min(box.min.y, w.y), std::min(box.min.z, w.z)};
            box.max = {std::max(box.max.x, w.x), std::max(box.max.y, w.y), std::max(box.max.z, w.z)};
        }
        cam = view_->framed(cam, box);
    }
    view_->setCamera(cam);
}

// ---------- the model ----------
void SketchModel::setModel(sketch::Sketch model)
{
    model_ = std::move(model);
    ++modelSerial_;
    const su::SketchView view(model_);
    engine_.modelChanged(view);
    for (auto it = selected_.begin(); it != selected_.end();) {
        if (view.pointEntry(*it) == nullptr && view.curve(*it) == nullptr)
            it = selected_.erase(it);
        else
            ++it;
    }
    requestSolve();
    refreshDisplay();
    emit modelChanged();
}

std::optional<sketch::Sketch> SketchModel::solved() const
{
    if (solved_ && solvedSerial_ == modelSerial_)
        return solved_->solved;
    return std::nullopt;
}

void SketchModel::refreshDisplay()
{
    if (dragSketch_)
        display_ = su::SketchView(*dragSketch_);
    else if (solved_ && solvedSerial_ == modelSerial_ && solved_->status != sketch::SolveStatus::Failed &&
             solved_->status != sketch::SolveStatus::Conflicting)
        display_ = su::SketchView(solved_->solved);
    else
        display_ = su::SketchView(model_);
    updateOverlay();
}

void SketchModel::setStatus(const QString& status, int dof)
{
    if (status == status_ && dof == freeDof_)
        return;
    status_ = status;
    freeDof_ = dof;
    emit statusChanged();
}

void SketchModel::requestSolve()
{
    const std::uint64_t serial = ++solveSerial_;
    const std::uint64_t modelSerial = modelSerial_;
    if (model_.entities.empty()) {
        solved_.reset();
        solvedSerial_ = modelSerial;
        solveSerial_ = serial;
        conflictEntities_.clear();
        setStatus(active_ ? QStringLiteral("under") : QString(), 0);
        return;
    }
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (jobs == nullptr)
        return;
    setStatus(QStringLiteral("solving"), freeDof_);
    JobHandle<sketch::SolveResult> job =
        jobs->submit<sketch::SolveResult>(JobInfo{"Sketch solve", JobPriority::Interactive, false},
                                          [model = model_](JobContext& ctx) -> Result<sketch::SolveResult> {
                                              auto solver = api::sketch_edit::makeSolver();
                                              if (!solver)
                                                  return tl::unexpected(Error{ErrorCode::Internal, "No solver", {}});
                                              return solver->solve(model, ctx);
                                          });
    QPointer<SketchModel> self(this);
    job.then([self, serial, modelSerial](Result<sketch::SolveResult> r) {
        if (!self || serial != self->solveSerial_ || modelSerial != self->modelSerial_)
            return;
        self->solvedSerial_ = modelSerial;
        self->conflictEntities_.clear();
        if (!r) {
            self->solved_.reset();
            self->setStatus(QStringLiteral("failed"), 0);
            self->refreshDisplay();
            return;
        }
        self->solved_ = std::move(*r);
        QString status;
        switch (self->solved_->status) {
        case sketch::SolveStatus::FullyConstrained:
            status = QStringLiteral("full");
            break;
        case sketch::SolveStatus::UnderConstrained:
            status = QStringLiteral("under");
            break;
        case sketch::SolveStatus::OverConstrained:
            status = QStringLiteral("over");
            break;
        case sketch::SolveStatus::Conflicting:
            status = QStringLiteral("conflict");
            break;
        case sketch::SolveStatus::Failed:
            status = QStringLiteral("failed");
            break;
        }
        std::set<Uuid> bad(self->solved_->conflicting.begin(), self->solved_->conflicting.end());
        bad.insert(self->solved_->redundant.begin(), self->solved_->redundant.end());
        for (const sketch::Constraint& c : self->model_.constraints) {
            if (bad.count(c.id))
                self->conflictEntities_.insert(c.refs.begin(), c.refs.end());
        }
        self->setStatus(status, self->solved_->freeDof);
        self->refreshDisplay();
    });
}

QString SketchModel::statusText() const
{
    if (status_ == QLatin1String("full"))
        return trs("Fully constrained");
    if (status_ == QLatin1String("under")) {
        if (model_.entities.empty())
            return trs("Empty: pick a tool and draw");
        return freeDof_ == 1 ? trs("Under-constrained: 1 degree of freedom")
                             : trs("Under-constrained: %1 degrees of freedom").arg(freeDof_);
    }
    if (status_ == QLatin1String("over"))
        return trs("Over-constrained: redundant constraints");
    if (status_ == QLatin1String("conflict"))
        return trs("Conflicting constraints");
    if (status_ == QLatin1String("failed"))
        return trs("The sketch does not solve");
    if (status_ == QLatin1String("solving"))
        return trs("Solving…");
    return {};
}

// ---------- running steps ----------
void SketchModel::run(const QString& actionId, std::string argsJson, const QString& label)
{
    api::IApp* a = app();
    if (a == nullptr)
        return;
    const QByteArray id = actionId.toUtf8();
    JobHandle<api::ActionResult> job = a->actions().invoke(sv(id), argsJson, api::Caller::User);
    if (!job.valid()) {
        if (context_)
            context_->showMessage(trs("%1 could not be started").arg(label), true);
        return;
    }
    ++inFlight_;
    QPointer<SketchModel> self(this);
    job.then([self, label](Result<api::ActionResult> r) {
        if (!self)
            return;
        --self->inFlight_;
        if (!r && self->context_)
            self->context_->showMessage(trs("%1 failed: %2").arg(label, qs(r.error().message)), true);
        if (self->inFlight_ == 0)
            self->syncFromDocument();
    });
}

void SketchModel::apply(const su::Action& action)
{
    if (action.kind == su::Action::Kind::None || !active_)
        return;
    api::OpenDocument* od = document();
    if (od != nullptr && od->readOnly) {
        if (context_)
            context_->showMessage(trs("The document is read-only"), true);
        return;
    }
    const QString label = qs(action.label);
    if (action.kind == su::Action::Kind::Add) {
        sketch::Sketch next = model_;
        next.entities.insert(next.entities.end(), action.batch.entities.begin(), action.batch.entities.end());
        next.constraints.insert(next.constraints.end(), action.batch.constraints.begin(),
                                action.batch.constraints.end());
        std::sort(next.entities.begin(), next.entities.end(),
                  [](const sketch::Entity& x, const sketch::Entity& y) { return x.id < y.id; });
        if (pending_) {
            run(QStringLiteral("feature.sketch.add"),
                api::sketch_edit::sketchAddArgs(feature_, name_.toStdString(), planeJson_, next), trs("New sketch"));
            pending_ = false;
        } else {
            run(QStringLiteral("sketch.entity.add"),
                api::sketch_edit::entityAddArgs(feature_, action.batch.entities, action.batch.constraints), label);
        }
        setModel(std::move(next));
        return;
    }
    if (pending_)
        return; // nothing to act on yet
    std::string args = action.argsJson;
    if (args.size() < 2 || args.front() != '{')
        return;
    args = "{\"id\":\"" + feature_.value.toString() + "\"" + (args.size() > 2 ? "," : "") + args.substr(1);
    run(qs(action.command), std::move(args), label);
}

void SketchModel::deleteSelection()
{
    if (!active_ || selected_.empty() || pending_)
        return;
    std::string ids;
    for (const SketchEntId& id : selected_)
        ids += (ids.empty() ? "\"" : ",\"") + id.value.toString() + "\"";
    run(QStringLiteral("sketch.entity.remove"),
        "{\"entities\":[" + ids + "],\"id\":\"" + feature_.value.toString() + "\"}", trs("Delete"));
    selected_.clear();
    emit selectionChanged();
    updateOverlay();
}

void SketchModel::toggleConstruction()
{
    if (!active_)
        return;
    std::vector<SketchEntId> curves;
    bool allConstruction = true;
    for (const SketchEntId& id : selected_) {
        if (const su::ViewCurve* c = display_.curve(id)) {
            curves.push_back(id);
            allConstruction = allConstruction && c->construction;
        }
    }
    if (curves.empty() || pending_) {
        setConstruction(!engine_.construction());
        return;
    }
    std::string ids;
    for (const SketchEntId& id : curves)
        ids += (ids.empty() ? "\"" : ",\"") + id.value.toString() + "\"";
    run(QStringLiteral("sketch.construction"),
        "{\"construction\":" + std::string(allConstruction ? "false" : "true") + ",\"entities\":[" + ids +
            "],\"id\":\"" + feature_.value.toString() + "\"}",
        trs("Construction"));
}

void SketchModel::clearSelection()
{
    if (selected_.empty())
        return;
    selected_.clear();
    emit selectionChanged();
    updateOverlay();
}

QStringList SketchModel::selection() const
{
    QStringList out;
    for (const SketchEntId& id : selected_)
        out << qs(id.value.toString());
    return out;
}

// ---------- tools ----------
QString SketchModel::tool() const
{
    return qs(su::toolName(engine_.kind()));
}

void SketchModel::setTool(const QString& name)
{
    const su::ToolKind kind = su::toolFromName(name.toStdString());
    if (kind == engine_.kind() && !engine_.busy())
        return;
    engine_.setKind(kind);
    typedText_.clear();
    activeField_ = -1;
    hover_ = {};
    if (pointerIn_) {
        if (const auto uv = toPlane(pointer_))
            engine_.hover(display_, *uv, snapTolerance(pointer_), true);
    }
    emit toolChanged();
    emit fieldsChanged();
    updateOverlay();
    updateHint();
}

void SketchModel::setConstruction(bool on)
{
    if (on == engine_.construction())
        return;
    engine_.setConstruction(on);
    emit toolChanged();
}

QString SketchModel::hint() const
{
    if (placing_)
        return trs("Pick the plane to sketch on, or select a planar face in the view");
    if (!active_)
        return {};
    const bool started = engine_.busy();
    switch (engine_.kind()) {
    case su::ToolKind::Select:
        return trs("Click to select, drag a point or curve to move it; Delete removes the selection");
    case su::ToolKind::Line:
        return started ? trs("Click the next point, or type a length (Tab: angle, Enter: place). Esc ends the line")
                       : trs("Click where the line starts");
    case su::ToolKind::Rectangle:
        return started ? trs("Click the opposite corner, or type the width (Tab: height)")
                       : trs("Click the first corner");
    case su::ToolKind::Circle:
        return started ? trs("Click a point on the circle, or type the diameter") : trs("Click the centre");
    case su::ToolKind::Arc3:
        return trs("Click the start, the end, then a point on the arc");
    case su::ToolKind::ArcCenter:
        return trs("Click the centre, the start, then the end (move round the way the arc should go)");
    case su::ToolKind::ArcTangent:
        return started ? trs("Click where the arc ends") : trs("Click the end of a line or arc to continue from");
    case su::ToolKind::Slot:
        return trs("Click the two centres, then the width");
    case su::ToolKind::Spline:
        return trs("Click the control points; Enter or a click on the last point finishes");
    case su::ToolKind::Trim:
        return trs("Click the piece of a curve to remove");
    case su::ToolKind::Extend:
        return trs("Click near the end of a line or arc to extend it to the next curve");
    case su::ToolKind::Offset:
        return engine_.offsetChain().empty() ? trs("Click a curve: its connected chain is offset")
                                             : trs("Click on the side to offset to, or type the distance");
    case su::ToolKind::Fillet:
        return trs("Click the corner of two lines; type the radius first to change it");
    }
    return {};
}

void SketchModel::updateHint()
{
    emit hintChanged();
}

QVariantList SketchModel::fields() const
{
    QVariantList out;
    if (!active_)
        return out;
    const std::vector<su::FieldSpec> specs = engine_.fields();
    for (int i = 0; i < static_cast<int>(specs.size()); ++i) {
        const su::FieldSpec& f = specs[static_cast<std::size_t>(i)];
        QVariantMap m;
        m.insert(QStringLiteral("key"), qs(f.key));
        m.insert(QStringLiteral("label"), trs(f.label.c_str()));
        const auto typed = typedText_.find(f.key);
        QString text;
        if (typed != typedText_.end())
            text = typed->second;
        else if (f.angle)
            text = QString::number(f.value * 180 / su::kPi, 'f', 1) + QStringLiteral("°");
        else
            text = units_ ? units_->formatLength(f.value) : QString::number(f.value, 'f', 3);
        m.insert(QStringLiteral("text"), text);
        m.insert(QStringLiteral("typed"), typed != typedText_.end());
        m.insert(QStringLiteral("current"), i == activeField_);
        out << m;
    }
    return out;
}

std::optional<double> SketchModel::parseField(const su::FieldSpec& spec, const QString& text) const
{
    QString t = text.trimmed();
    if (t.isEmpty())
        return std::nullopt;
    if (spec.angle) {
        t.remove(QStringLiteral("°")).remove(QStringLiteral("deg"));
        t.replace(QLatin1Char(','), QLatin1Char('.'));
        bool ok = false;
        const double deg = t.trimmed().toDouble(&ok);
        return ok && std::isfinite(deg) ? std::optional<double>(deg * su::kPi / 180) : std::nullopt;
    }
    t.replace(QLatin1Char(','), QLatin1Char('.'));
    const double mm = units_ ? units_->parseLength(t) : t.toDouble();
    if (!std::isfinite(mm) || !(mm > 0))
        return std::nullopt;
    return mm;
}

void SketchModel::applyTyped()
{
    for (const su::FieldSpec& f : engine_.fields()) {
        const auto it = typedText_.find(f.key);
        engine_.setTyped(f.key, it != typedText_.end() ? parseField(f, it->second) : std::nullopt);
    }
    if (pointerIn_) {
        if (const auto uv = toPlane(pointer_))
            engine_.hover(display_, *uv, snapTolerance(pointer_), true);
    }
}

void SketchModel::afterToolInput()
{
    if (engine_.typed().empty()) {
        typedText_.clear();
        activeField_ = -1;
    }
    updateOverlay();
    emit fieldsChanged();
    updateHint();
}

// ---------- pointer ----------
double SketchModel::snapPixels() const
{
    return theme_ ? theme_->spacingS() : 8.0;
}

double SketchModel::snapTolerance(QPointF pos) const
{
    const auto a = toPlane(pos);
    const auto b = toPlane(pos + QPointF(snapPixels(), 0));
    if (!a || !b)
        return 1.0;
    return std::max(1e-9, su::distance(*a, *b));
}

std::optional<Vec2> SketchModel::toPlane(QPointF viewPos) const
{
    if (!view_)
        return std::nullopt;
    const render::Camera cam = view_->camera();
    const int w = std::max(1, static_cast<int>(std::lround(view_->width())));
    const int h = std::max(1, static_cast<int>(std::lround(view_->height())));
    const render::Ray ray = render::cameraRay(cam, viewPos.x(), viewPos.y(), w, h);
    const auto hit = nav::rayPlane(ray, plane_.origin, plane_.normal);
    if (!hit)
        return std::nullopt;
    const Vec3 d = v3sub(*hit, plane_.origin);
    return Vec2{v3dot(d, plane_.xDir), v3dot(d, yDirOf(plane_))};
}

std::optional<QPointF> SketchModel::toView(Vec2 planePoint) const
{
    if (!view_)
        return std::nullopt;
    return nav::project(view_->camera(), worldOf(plane_, planePoint), view_->width(), view_->height());
}

QPointF SketchModel::mapToView(double u, double v) const
{
    return toView(Vec2{u, v}).value_or(QPointF());
}

void SketchModel::pointerMove(QPointF pos, Qt::KeyboardModifiers mods)
{
    if (!active_)
        return;
    // Qt Quick repeats a hover at the same place after frames: answering it would ask for the next frame.
    if (pointerIn_ && pos == pointer_ && !press_.down && !dragging_)
        return;
    pointer_ = pos;
    pointerIn_ = true;
    const auto uv = toPlane(pos);
    if (!uv)
        return;
    if (dragging_) {
        if (drag_ && !dragFinishing_)
            drag_->move(su::add(*uv, dragGrabOffset_));
        return;
    }
    const double tol = snapTolerance(pos);
    if (press_.down && engine_.kind() == su::ToolKind::Select) {
        if (!press_.moved) {
            const QPointF d = pos - press_.at;
            if (std::hypot(d.x(), d.y()) > snapPixels() / 2) {
                press_.moved = true;
                if (press_.hit.kind != su::SketchView::Hit::Kind::None && !pending_) {
                    const auto start = toPlane(press_.at);
                    startDrag(press_.hit, start.value_or(*uv));
                    if (drag_)
                        drag_->move(su::add(*uv, dragGrabOffset_));
                }
            }
        }
        return;
    }
    if (engine_.kind() == su::ToolKind::Select) {
        const su::SketchView::Hit h = display_.hit(*uv, tol);
        if (h.kind != hover_.kind || h.id != hover_.id) {
            hover_ = h;
            updateOverlay();
        }
        return;
    }
    engine_.hover(display_, *uv, tol, !(mods & Qt::AltModifier));
    updateOverlay();
    emit fieldsChanged();
}

void SketchModel::pointerPress(QPointF pos, Qt::KeyboardModifiers mods)
{
    if (!active_)
        return;
    pointer_ = pos;
    press_ = {};
    press_.down = true;
    press_.at = pos;
    press_.mods = mods;
    if (const auto uv = toPlane(pos))
        press_.hit = display_.hit(*uv, snapTolerance(pos));
}

void SketchModel::pointerRelease(QPointF pos, Qt::KeyboardModifiers mods)
{
    if (!active_)
        return;
    const Pending press = press_;
    press_ = {};
    if (dragging_) {
        if (drag_ && !dragFinishing_) {
            if (const auto uv = toPlane(pos))
                drag_->move(su::add(*uv, dragGrabOffset_));
            dragFinishing_ = true;
            drag_->finish();
        }
        return;
    }
    if (!press.down)
        return;
    const auto uv = toPlane(pos);
    if (!uv)
        return;
    if (engine_.kind() == su::ToolKind::Select) {
        if (press.moved)
            return;
        const su::SketchView::Hit h = display_.hit(*uv, snapTolerance(pos));
        const bool toggle = mods & Qt::ControlModifier;
        const bool addTo = mods & Qt::ShiftModifier;
        if (h.kind == su::SketchView::Hit::Kind::None) {
            if (!toggle && !addTo)
                selected_.clear();
        } else if (toggle) {
            if (!selected_.erase(h.id))
                selected_.insert(h.id);
        } else if (addTo) {
            selected_.insert(h.id);
        } else {
            selected_ = {h.id};
        }
        emit selectionChanged();
        updateOverlay();
        return;
    }
    engine_.hover(display_, *uv, snapTolerance(pos), !(mods & Qt::AltModifier));
    apply(engine_.click(display_));
    afterToolInput();
}

void SketchModel::pointerDoubleClick(QPointF pos, Qt::KeyboardModifiers)
{
    if (!active_ || engine_.kind() != su::ToolKind::Select)
        return;
    const auto uv = toPlane(pos);
    if (!uv)
        return;
    const su::SketchView::Hit h = display_.hit(*uv, snapTolerance(pos));
    if (h.kind == su::SketchView::Hit::Kind::None)
        return;
    for (const sketch::Constraint& c : model_.constraints) {
        const bool dimension = c.type == sketch::ConstraintType::Distance || c.type == sketch::ConstraintType::DistanceX ||
                               c.type == sketch::ConstraintType::DistanceY || c.type == sketch::ConstraintType::Radius ||
                               c.type == sketch::ConstraintType::Diameter || c.type == sketch::ConstraintType::Angle;
        if (dimension && std::find(c.refs.begin(), c.refs.end(), h.id) != c.refs.end()) {
            emit constraintEditRequested(qs(c.id.toString()));
            return;
        }
    }
}

void SketchModel::pointerLeave()
{
    pointerIn_ = false;
    if (hover_.kind != su::SketchView::Hit::Kind::None) {
        hover_ = {};
        updateOverlay();
    }
}

// ---------- dragging ----------
void SketchModel::startDrag(const su::SketchView::Hit& hit, Vec2 at)
{
    if (model_.entities.empty())
        return;
    sketch::Sketch model = model_;
    SketchEntId point;
    dragTempPoint_.reset();
    dragTempConstraint_.reset();
    if (hit.kind == su::SketchView::Hit::Kind::Point) {
        point = hit.id;
        dragGrabOffset_ = su::sub(hit.at, at);
    } else {
        const su::ViewCurve* c = display_.curve(hit.id);
        if (c == nullptr || (c->kind != su::ViewCurve::Kind::Line && c->kind != su::ViewCurve::Kind::Arc &&
                             c->kind != su::ViewCurve::Kind::Circle))
            return;
        point = SketchEntId{Uuid::generate()};
        model.entities.push_back(sketch::Entity{point, sketch::Point{hit.at}, false});
        sketch::Constraint on;
        on.id = Uuid::generate();
        on.type = sketch::ConstraintType::PointOnCurve;
        on.refs = {point, hit.id};
        model.constraints.push_back(on);
        dragTempPoint_ = point;
        dragTempConstraint_ = on.id;
        dragGrabOffset_ = su::sub(hit.at, at);
        std::sort(model.entities.begin(), model.entities.end(),
                  [](const sketch::Entity& x, const sketch::Entity& y) { return x.id < y.id; });
    }
    // Start from the geometry shown (the solved one), so the drag does not jump.
    if (const auto s = solved()) {
        for (sketch::Entity& e : model.entities) {
            if (auto* p = std::get_if<sketch::Point>(&e.geom)) {
                if (const auto q = display_.point(e.id))
                    p->p = *q;
            }
        }
        (void)s;
    }
    dragSteps_ = 0;
    slowestStepMs_ = 0;
    dragFinishing_ = false;
    dragging_ = true;
    selected_.clear();
    emit selectionChanged();
    drag_ = std::make_unique<su::SketchDrag>(this, ++dragSerial_, std::move(model), point,
                                             [this](const su::SketchDrag::Update& u) { onDrag(u); });
    emit draggingChanged();
}

void SketchModel::onDrag(const su::SketchDrag::Update& update)
{
    if (!drag_ || update.serial != drag_->serial())
        return;
    const auto strip = [this](sketch::Sketch s) {
        if (dragTempPoint_) {
            s.entities.erase(std::remove_if(s.entities.begin(), s.entities.end(),
                                            [&](const sketch::Entity& e) { return e.id == *dragTempPoint_; }),
                             s.entities.end());
        }
        if (dragTempConstraint_) {
            s.constraints.erase(std::remove_if(s.constraints.begin(), s.constraints.end(),
                                               [&](const sketch::Constraint& c) { return c.id == *dragTempConstraint_; }),
                                s.constraints.end());
        }
        return s;
    };
    switch (update.kind) {
    case su::SketchDrag::Update::Kind::Started:
        return;
    case su::SketchDrag::Update::Kind::Step:
        ++dragSteps_;
        slowestStepMs_ = std::max(slowestStepMs_, update.stepMs);
        dragSketch_ = strip(update.sketch);
        refreshDisplay();
        return;
    case su::SketchDrag::Update::Kind::Final: {
        sketch::Sketch result = strip(update.sketch);
        drag_.reset();
        dragging_ = false;
        dragFinishing_ = false;
        dragSketch_.reset();
        emit draggingChanged();
        if (!result.entities.empty() && !pending_) {
            run(QStringLiteral("sketch.set"), api::sketch_edit::sketchSetArgs(feature_, result), trs("Drag"));
            setModel(std::move(result));
        } else {
            refreshDisplay();
        }
        return;
    }
    case su::SketchDrag::Update::Kind::Failed:
        drag_.reset();
        dragging_ = false;
        dragFinishing_ = false;
        dragSketch_.reset();
        emit draggingChanged();
        if (context_)
            context_->showMessage(trs("The geometry cannot be dragged: %1").arg(qs(update.error)), true);
        refreshDisplay();
        return;
    }
}

// ---------- keyboard ----------
bool SketchModel::wantsKey(const QKeyEvent* e) const
{
    if (!active_)
        return false;
    const int key = e->key();
    const bool typing = activeField_ >= 0;
    const bool hasFields = !engine_.fields().empty();
    if (typing && (isPrintable(e) || key == Qt::Key_Backspace || key == Qt::Key_Tab || key == Qt::Key_Backtab))
        return true;
    if (hasFields && startsNumber(e))
        return true;
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && (typing || engine_.busy()))
        return true;
    if (key == Qt::Key_Escape)
        return typing || engine_.busy() || engine_.kind() != su::ToolKind::Select || !selected_.empty();
    if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && !selected_.empty())
        return true;
    return false;
}

bool SketchModel::keyPress(QKeyEvent* e)
{
    if (!active_)
        return false;
    const int key = e->key();
    const std::vector<su::FieldSpec> specs = engine_.fields();
    const bool typing = activeField_ >= 0;
    if (key == Qt::Key_Escape) {
        if (typing || !typedText_.empty()) {
            typedText_.clear();
            activeField_ = -1;
            applyTyped();
        } else if (engine_.busy()) {
            engine_.cancel();
        } else if (engine_.kind() != su::ToolKind::Select) {
            setTool(QStringLiteral("select"));
            return true;
        } else if (!selected_.empty()) {
            clearSelection();
            return true;
        } else {
            return false;
        }
        afterToolInput();
        return true;
    }
    if ((key == Qt::Key_Tab || key == Qt::Key_Backtab) && !specs.empty() && typing) {
        const int n = static_cast<int>(specs.size());
        activeField_ = (activeField_ + (key == Qt::Key_Tab ? 1 : n - 1)) % n;
        applyTyped();
        emit fieldsChanged();
        return true;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (!typing && !engine_.busy())
            return false;
        applyTyped();
        apply(engine_.enter(display_));
        typedText_.clear();
        activeField_ = -1;
        for (const su::FieldSpec& f : engine_.fields())
            engine_.setTyped(f.key, std::nullopt);
        if (pointerIn_) {
            if (const auto uv = toPlane(pointer_))
                engine_.hover(display_, *uv, snapTolerance(pointer_), true);
        }
        afterToolInput();
        return true;
    }
    if (key == Qt::Key_Backspace && typing) {
        if (activeField_ < static_cast<int>(specs.size())) {
            const std::string k = specs[static_cast<std::size_t>(activeField_)].key;
            QString& t = typedText_[k];
            t.chop(1);
            if (t.isEmpty())
                typedText_.erase(k);
        }
        applyTyped();
        updateOverlay();
        emit fieldsChanged();
        return true;
    }
    if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && !selected_.empty()) {
        deleteSelection();
        return true;
    }
    if (!specs.empty() && ((typing && isPrintable(e)) || startsNumber(e))) {
        if (activeField_ < 0 || activeField_ >= static_cast<int>(specs.size()))
            activeField_ = 0;
        typedText_[specs[static_cast<std::size_t>(activeField_)].key] += e->text();
        applyTyped();
        updateOverlay();
        emit fieldsChanged();
        return true;
    }
    return false;
}

// ---------- the overlay ----------
void SketchModel::updateOverlay()
{
    SketchOverlayScene scene;
    scene.revision = overlay_.revision + 1;
    if (!active_ || !view_) {
        overlay_ = std::move(scene);
        glyphs_.clear();
        emit overlayChanged();
        return;
    }
    const render::Camera cam = view_->camera();
    const double w = view_->width(), h = view_->height();
    const auto project = [&](Vec2 p) { return nav::project(cam, worldOf(plane_, p), w, h); };
    const auto polyline = [&](const std::vector<Vec2>& pts, SketchOverlayScene::Style style, bool dashed) {
        SketchOverlayScene::Line line{style, {}, dashed};
        line.points.reserve(pts.size());
        for (const Vec2& p : pts) {
            const auto q = project(p);
            if (!q)
                return;
            line.points.push_back(*q);
        }
        scene.lines.push_back(std::move(line));
    };
    using S = SketchOverlayScene::Style;
    const S base = status_ == QLatin1String("full")                                         ? S::Constrained
                   : status_ == QLatin1String("over") || status_ == QLatin1String("conflict") ? S::Conflict
                                                                                              : S::Geometry;
    const su::Draft& draft = engine_.draft();
    const std::set<SketchEntId> highlight(draft.highlight.begin(), draft.highlight.end());
    const auto styleOf = [&](const SketchEntId& id, bool construction) {
        if (selected_.count(id))
            return S::Selected;
        if (hover_.kind != su::SketchView::Hit::Kind::None && hover_.id == id)
            return S::Hovered;
        if (highlight.count(id))
            return S::Highlight;
        if (conflictEntities_.count(id))
            return S::Conflict;
        return construction ? S::Construction : base;
    };
    for (const su::ViewCurve& c : display_.curves())
        polyline(c.poly, styleOf(c.id, c.construction), c.construction);
    for (const su::ViewPoint& p : display_.points()) {
        if (const auto q = project(p.p)) {
            S s = styleOf(p.id, p.construction);
            if (s == S::Construction)
                s = base;
            scene.points.push_back({s, *q});
        }
    }
    for (const auto& pts : draft.curves)
        polyline(pts, S::Preview, engine_.construction());
    for (const auto& [a, b] : draft.guides)
        polyline({a, b}, S::Guide, true);
    for (const Vec2& p : draft.points) {
        if (const auto q = project(p))
            scene.points.push_back({S::Preview, *q});
    }
    overlay_ = std::move(scene);
    // Glyphs and the chips' anchor.
    QVariantList glyphs;
    for (const su::Glyph& g : draft.glyphs) {
        if (const auto q = project(g.at)) {
            QVariantMap m;
            m.insert(QStringLiteral("px"), q->x());
            m.insert(QStringLiteral("py"), q->y());
            m.insert(QStringLiteral("kind"), QString::fromLatin1(su::glyphName(g.kind)));
            glyphs << m;
        }
    }
    glyphs_ = glyphs;
    if (draft.labelAt) {
        if (const auto q = project(*draft.labelAt))
            fieldsAt_ = *q;
    }
    emit overlayChanged();
}

QObject* SketchModel::view() const
{
    return view_.data();
}

void SketchModel::setView(QObject* view)
{
    auto* v = qobject_cast<ViewportItem*>(view);
    if (v == view_)
        return;
    for (const auto& c : viewConnections_)
        disconnect(c);
    viewConnections_.clear();
    view_ = v;
    if (view_) {
        viewConnections_ << connect(view_, &ViewportItem::cameraChanged, this, [this] {
            if (active_)
                updateOverlay();
        });
        viewConnections_ << connect(view_, &QQuickItem::widthChanged, this, [this] {
            if (active_)
                updateOverlay();
        });
        viewConnections_ << connect(view_, &QQuickItem::heightChanged, this, [this] {
            if (active_)
                updateOverlay();
        });
    }
    emit viewChanged();
}

std::optional<FeatureId> SketchModel::featureId() const
{
    return active_ ? std::optional<FeatureId>(feature_) : std::nullopt;
}

// ---------- commands ----------
QString SketchModel::commandReason(const QString& id) const
{
    api::IApp* a = app();
    if (id == QLatin1String("feature.sketch.add") || id == QLatin1String("sketch.mode.new")) {
        if (a == nullptr)
            return trs("There is no session");
        api::OpenDocument* od = a->active();
        if (od == nullptr)
            return trs("Open or create a document first");
        if (od->readOnly)
            return trs("The document is read-only");
        if (active_)
            return trs("A sketch is open; finish it first");
        return {};
    }
    if (!active_)
        return trs("Open a sketch first (New Sketch)");
    if (id == QLatin1String("sketch.mode.finish"))
        return {};
    if (id.startsWith(QLatin1String("sketch.tool.")))
        return {};
    return trs("Unknown sketch command");
}

bool SketchModel::runCommand(const QString& id)
{
    if (id == QLatin1String("feature.sketch.add") || id == QLatin1String("sketch.mode.new")) {
        newSketch();
        return true;
    }
    if (id == QLatin1String("sketch.mode.finish")) {
        finish();
        return true;
    }
    if (id == QLatin1String("sketch.tool.construction")) {
        toggleConstruction();
        return true;
    }
    if (id.startsWith(QLatin1String("sketch.tool."))) {
        setTool(id.mid(12));
        return true;
    }
    return false;
}

} // namespace ray::ui
