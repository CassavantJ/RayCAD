#include "scene_controller.h"

#include "app_context.h"
#include "model/preview.h"

#include <QtCore/QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace ray::ui {

namespace {

constexpr const char* kEngineKey = "_ray_ui_scene";
constexpr double kEps = 1e-9;

// Body colours (T-016). The importer gives every body a colour and uses this grey when the file has none
// (src/kernel/io_import.cpp); it is a value to recognise, not one drawn: such bodies get the theme's ModelFace.
constexpr float kImporterNoColourGrey = 0.75f;
constexpr float kColourTolerance = 1e-3f;

// Body colours are the file's values, which STEP and IGES store sRGB-encoded (the importer reads them back with
// Quantity_TOC_sRGB); the viewport takes linear RGB.
float srgbToLinear(float encoded)
{
    const float e = std::clamp(encoded, 0.0f, 1.0f);
    return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}

// Faces in the body's own colour when the file gave one, else in the theme's; edges and highlights always come
// from the theme.
render::MeshStyle modelStyle(const api::viewer::Body* body)
{
    render::MeshStyle style{render::Token::ModelFace, render::Token::ModelEdge, 1.0f, true, false};
    if (body == nullptr)
        return style;
    const std::array<float, 4>& c = body->color;
    const bool finite = std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
    const bool noColour = std::abs(c[0] - kImporterNoColourGrey) < kColourTolerance &&
                          std::abs(c[1] - kImporterNoColourGrey) < kColourTolerance &&
                          std::abs(c[2] - kImporterNoColourGrey) < kColourTolerance;
    if (finite && !noColour)
        style.faceColor = render::Color{srgbToLinear(c[0]), srgbToLinear(c[1]), srgbToLinear(c[2]), 1.0f};
    return style;
}

struct BoxBuilder {
    Box3 box{{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
              std::numeric_limits<double>::infinity()},
             {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
              -std::numeric_limits<double>::infinity()}};
    bool any = false;
    void add(double x, double y, double z)
    {
        box.min.x = std::min(box.min.x, x);
        box.min.y = std::min(box.min.y, y);
        box.min.z = std::min(box.min.z, z);
        box.max.x = std::max(box.max.x, x);
        box.max.y = std::max(box.max.y, y);
        box.max.z = std::max(box.max.z, z);
        any = true;
    }
    void addVertex(const std::vector<float>& xyz, std::size_t vertex)
    {
        if (vertex * 3 + 2 < xyz.size())
            add(xyz[vertex * 3], xyz[vertex * 3 + 1], xyz[vertex * 3 + 2]);
    }
};

QString tierLabel(render::Tier tier)
{
    switch (tier) {
    case render::Tier::Low:
        return QObject::tr("Low");
    case render::Tier::Medium:
        return QObject::tr("Medium");
    case render::Tier::High:
        return QObject::tr("High");
    }
    return {};
}

QString statusName(doc::FeatureStatus status)
{
    switch (status) {
    case doc::FeatureStatus::Ok:
        return {};
    case doc::FeatureStatus::Warning:
        return QObject::tr("warning");
    case doc::FeatureStatus::Failed:
        return QObject::tr("failed");
    case doc::FeatureStatus::Suppressed:
        return QObject::tr("suppressed");
    case doc::FeatureStatus::NotBuilt:
        return QObject::tr("not built");
    }
    return {};
}

} // namespace

// ---------- SceneTreeModel ----------
SceneTreeModel::SceneTreeModel(SceneController* scene) : QAbstractListModel(scene), scene_(scene) {}

int SceneTreeModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SceneTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size()))
        return {};
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    const bool header = row.kind == QLatin1String("header");
    switch (role) {
    case KindRole:
        return row.kind;
    case SectionRole:
        return row.section;
    case NameRole:
        if (header)
            return row.section == QLatin1String("features") ? tr("Features") : tr("Bodies");
        if (row.feature >= 0)
            return scene_->features()[static_cast<std::size_t>(row.feature)].name;
        return scene_->bodyName(row.body);
    case DepthRole:
        return header ? 0 : 1;
    case ExpandableRole:
        return header;
    case ExpandedRole:
        return row.section == QLatin1String("features") ? featuresExpanded_ : bodiesExpanded_;
    case VisibleRole:
        return row.body >= 0 ? scene_->bodyVisible(row.body) : true;
    case SelectedRole:
        return row.body >= 0 && scene_->bodySelected(row.body);
    case BodyIndexRole:
        return row.body;
    case FeatureIndexRole:
        return row.feature;
    case DetailRole: {
        if (header) {
            const int n = row.section == QLatin1String("features") ? static_cast<int>(scene_->features().size())
                                                                   : scene_->bodyCount();
            return QString::number(n);
        }
        if (row.feature >= 0) {
            const SceneController::Feature& f = scene_->features()[static_cast<std::size_t>(row.feature)];
            return f.status.isEmpty() ? f.type : f.type + QStringLiteral(", ") + f.status;
        }
        if (const auto mesh = scene_->meshOf(row.body))
            return mesh->faces.size() == 1 ? tr("1 face") : tr("%1 faces").arg(mesh->faces.size());
        return QString();
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> SceneTreeModel::roleNames() const
{
    // Prefixed so that delegates (TreeRow has depth, expandable, expanded, ...) do not shadow their own properties.
    return {{KindRole, "rowKind"},
            {NameRole, "rowName"},
            {DepthRole, "rowDepth"},
            {ExpandableRole, "rowExpandable"},
            {ExpandedRole, "rowExpanded"},
            {VisibleRole, "rowShown"},
            {SelectedRole, "rowSelected"},
            {BodyIndexRole, "rowBody"},
            {FeatureIndexRole, "rowFeature"},
            {DetailRole, "rowDetail"},
            {SectionRole, "rowSection"}};
}

void SceneTreeModel::toggleSection(const QString& section)
{
    if (section == QLatin1String("features"))
        featuresExpanded_ = !featuresExpanded_;
    else
        bodiesExpanded_ = !bodiesExpanded_;
    rebuild();
}

int SceneTreeModel::rowOfBody(int bodyIndex) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].body == bodyIndex && rows_[i].kind == QLatin1String("body"))
            return static_cast<int>(i);
    }
    return -1;
}

void SceneTreeModel::rebuild()
{
    beginResetModel();
    rows_.clear();
    const auto& features = scene_->features();
    if (!features.empty()) {
        rows_.push_back(Row{QStringLiteral("header"), QStringLiteral("features"), -1, -1});
        if (featuresExpanded_) {
            for (std::size_t i = 0; i < features.size(); ++i)
                rows_.push_back(Row{QStringLiteral("feature"), QStringLiteral("features"), -1, static_cast<int>(i)});
        }
    }
    if (scene_->bodyCount() > 0) {
        rows_.push_back(Row{QStringLiteral("header"), QStringLiteral("bodies"), -1, -1});
        if (bodiesExpanded_) {
            for (int i = 0; i < scene_->bodyCount(); ++i)
                rows_.push_back(Row{QStringLiteral("body"), QStringLiteral("bodies"), i, -1});
        }
    }
    endResetModel();
}

void SceneTreeModel::refreshBodies()
{
    if (rows_.empty())
        return;
    emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1), {VisibleRole, SelectedRole, DetailRole});
}

// ---------- SceneController ----------
SceneController::SceneController(JobsProvider jobs, AppContext* context, QObject* parent)
    : QObject(parent), jobs_(std::move(jobs)), context_(context), snapshot_(std::make_shared<SceneSnapshot>()),
      preset_(render::presetFor(render::Tier::Low))
{
    tree_ = new SceneTreeModel(this);
    preview_ = std::make_unique<BodyOverlay>(this);
    if (context_ != nullptr)
        connect(context_, &AppContext::jobCancelRequested, this, [this] { cancelJobs(); });
}

SceneController::~SceneController()
{
    preview_.reset();
    coarse_.cancel();
    fine_.cancel();
}

SceneController* SceneController::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* scene = qobject_cast<SceneController*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(scene, QJSEngine::CppOwnership);
            return scene;
        }
    }
    return new SceneController(JobsProvider{}, nullptr, nullptr); // no jobs: nothing can be shown
}

void SceneController::provideFor(QQmlEngine* engine, SceneController* scene)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(scene));
}

void SceneController::setBodies(std::vector<api::viewer::Body> bodies, const QString& sourceName)
{
    // Abandon whatever was meshing for the old set; results that still arrive are dropped by serial.
    coarse_.cancel();
    fine_.cancel();
    coarse_ = {};
    fine_ = {};
    coarseSerial_ = fineSerial_ = 0;

    // A body that is still the same shape object keeps its mesh and viewport item (see the header).
    std::map<BodyId, std::size_t> previous;
    for (std::size_t j = 0; j < snapshot_->bodies.size(); ++j)
        previous.emplace(snapshot_->bodies[j].id, j);
    std::vector<Slot> next(bodies.size());
    std::vector<char> kept(slots_.size(), 0);
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const api::viewer::Body& body = bodies[i];
        const auto it = previous.find(body.id);
        if (it != previous.end() && !kept[it->second]) {
            const std::size_t j = it->second;
            const api::viewer::Body& old = snapshot_->bodies[j];
            if (slots_[j].mesh && !body.shape.isNull() && old.shape.impl == body.shape.impl) {
                next[i] = slots_[j];
                kept[j] = 1;
                if (viewport_ != nullptr && next[i].item != 0 && old.color != body.color)
                    viewport_->setStyle(next[i].item, modelStyle(&body));
            }
        }
        next[i].visible = body.visible;
        if (viewport_ != nullptr && next[i].item != 0)
            viewport_->setVisible(next[i].item, body.visible);
    }
    if (viewport_ != nullptr) {
        for (std::size_t j = 0; j < slots_.size(); ++j) {
            if (!kept[j] && slots_[j].item != 0)
                viewport_->remove(slots_[j].item);
        }
    }
    ++generation_;
    auto snap = std::make_shared<SceneSnapshot>();
    snap->generation = generation_;
    snap->bodies = std::move(bodies);
    snap->resolver = std::make_shared<const api::viewer::SelectionResolver>(snap->bodies);
    slots_ = std::move(next);
    snapshot_ = std::move(snap);
    selectedBodies_.clear();
    sourceName_ = sourceName;
    fineWanted_ = false;
    framePending_ = true;
    if (snapshot_->bodies.empty())
        framedBodies_.clear(); // nothing shown: whatever comes next is framed
    emit bodiesChanged();
    tree_->rebuild();
    if (!slots_.empty()) {
        ensurePreset();
        if (std::any_of(slots_.begin(), slots_.end(), [](const Slot& s) { return s.mesh == nullptr; })) {
            startCoarse();
        } else {
            // Every body kept its mesh: nothing to tessellate; refine any that are still coarse.
            frameIfNew();
            fineWanted_ = true;
            startFine();
        }
    }
    updateJobSlot();
    emit stateChanged();
}

void SceneController::syncFromDocument(const doc::DocStatePtr& state, const doc::BuildResultPtr& build)
{
    // T-017/T-019 call this from IDocument::subscribe (UI thread). Same entry point as the import path.
    hasDocument_ = state != nullptr;
    std::vector<Feature> features;
    if (state) {
        for (const api::viewer::FeatureRow& row : api::viewer::featuresOf(*state, build.get())) {
            Feature f;
            f.name = QString::fromStdString(row.name);
            f.type = QString::fromStdString(row.type);
            f.suppressed = row.suppressed;
            if (row.suppressed)
                f.status = statusName(doc::FeatureStatus::Suppressed);
            else if (row.status)
                f.status = statusName(*row.status);
            features.push_back(std::move(f));
        }
    }
    features_ = std::move(features);
    if (state && build)
        setBodies(api::viewer::bodiesFromDocument(*state, *build), sourceName_);
    else
        setBodies({}, sourceName_);
}

void SceneController::clear()
{
    features_.clear();
    hasDocument_ = false;
    setBodies({}, {});
}

void SceneController::attachViewport(render::IRenderViewport* viewport)
{
    if (viewport == viewport_)
        return;
    if (viewport_ != nullptr)
        detachViewport(viewport_);
    viewport_ = viewport;
    if (viewport_ == nullptr)
        return;
    viewport_->setPreset(preset_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].item = 0;
        pushToViewport(static_cast<int>(i));
    }
}

void SceneController::detachViewport(render::IRenderViewport* viewport)
{
    if (viewport != viewport_ || viewport_ == nullptr)
        return;
    for (Slot& slot : slots_)
        slot.item = 0;
    viewport_ = nullptr;
}

void SceneController::setPreset(const render::TierPreset& preset)
{
    const bool sameTolerance = presetKnown_ && preset.meshChordTolMm == preset_.meshChordTolMm &&
                               preset.meshAngleTolDeg == preset_.meshAngleTolDeg;
    preset_ = preset;
    presetKnown_ = true;
    presetPending_ = false;
    ++presetSerial_; // a detection still running no longer applies
    if (viewport_ != nullptr)
        viewport_->setPreset(preset_);
    emit presetChanged();
    // A new tolerance: mesh again (only once the coarse view is up; the coarse job's end starts it otherwise).
    if (!sameTolerance) {
        for (Slot& slot : slots_)
            slot.fine = false; // meshed at the old tolerance (still shown until the new ones arrive)
    }
    if (!sameTolerance && !slots_.empty() && !coarse_.valid()) {
        fine_.cancel();
        fine_ = {};
        fineWanted_ = true;
        startFine();
    }
}

QString SceneController::tierName() const
{
    return presetKnown_ ? tierLabel(preset_.tier) : QString();
}

void SceneController::ensurePreset()
{
    if (presetKnown_ || presetPending_)
        return;
    IJobSystem* jobs = this->jobs();
    if (jobs == nullptr)
        return;
    presetPending_ = true;
    const std::uint64_t serial = ++presetSerial_;
    // detectHardware creates a throw-away device on Windows (up to a few hundred ms): never on the UI thread.
    JobHandle<render::TierPreset> job =
        jobs->submit<render::TierPreset>(JobInfo{"Detecting graphics hardware", JobPriority::Normal, false},
                                         [](JobContext&) -> Result<render::TierPreset> {
                                             const render::HardwareInfo hw = render::detectHardware();
                                             return render::presetFor(render::chooseTier(hw));
                                         });
    QPointer<SceneController> self(this);
    job.then([self, serial](Result<render::TierPreset> preset) {
        if (!self || serial != self->presetSerial_)
            return;
        self->presetPending_ = false;
        self->setPreset(preset ? *preset : render::presetFor(render::Tier::Low));
    });
}

void SceneController::startCoarse()
{
    IJobSystem* jobs = this->jobs();
    if (jobs == nullptr)
        return;
    const std::shared_ptr<const SceneSnapshot> snap = snapshot_;
    const std::uint64_t serial = ++jobSerial_;
    coarseSerial_ = serial;
    jobTitle_ = sourceName_.isEmpty() ? tr("Meshing") : tr("Meshing %1").arg(sourceName_);
    jobProgress_ = 0;
    std::vector<std::size_t> todo; // the bodies that have no mesh yet
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].mesh)
            todo.push_back(i);
    }
    coarse_ = jobs->submit<Meshes>(
        JobInfo{"Display mesh (coarse)", JobPriority::Interactive, true},
        [snap, todo, meshed = meshed_](JobContext& ctx) -> Result<Meshes> {
            auto out = std::make_shared<SceneMeshBatch>();
            const std::size_t n = todo.size();
            for (std::size_t i : todo) {
                if (auto ok = ctx.cancel.check(); !ok)
                    return tl::unexpected(ok.error());
                JobContext sub{ctx.cancel, ctx.progress.sub(1.0 / double(n))};
                ++*meshed;
                Result<kernel::Mesh> mesh = api::viewer::meshBody(snap->bodies[i].shape, api::viewer::kCoarseChordMm,
                                                                  api::viewer::kCoarseAngleDeg, sub);
                if (!mesh && mesh.error().code == ErrorCode::Cancelled)
                    return tl::unexpected(mesh.error());
                out->indices.push_back(i);
                out->meshes.push_back(mesh ? std::make_shared<const kernel::Mesh>(std::move(*mesh)) : nullptr);
            }
            return Meshes(std::move(out));
        });
    QPointer<SceneController> self(this);
    coarse_.onProgress([self, serial](double fraction, const std::string&) {
        if (!self || serial != self->coarseSerial_ || !self->coarse_.valid())
            return;
        self->jobProgress_ = fraction;
        self->updateJobSlot();
    });
    coarse_.then([self, serial](Result<Meshes> meshes) {
        if (!self || serial != self->coarseSerial_)
            return; // superseded or abandoned
        self->coarse_ = {};
        if (meshes) {
            self->showMeshes(*meshes, false);
            self->fineWanted_ = true;
            self->startFine();
        } else if (meshes.error().code != ErrorCode::Cancelled && self->context_) {
            self->context_->showMessage(
                tr("The model could not be meshed: %1").arg(QString::fromStdString(meshes.error().message)), true);
        }
        self->updateJobSlot();
        emit self->stateChanged();
    });
}

void SceneController::startFine()
{
    if (!fineWanted_ || coarse_.valid() || fine_.valid() || slots_.empty() || !presetKnown_)
        return;
    const double chord = preset_.meshChordTolMm;
    const double angle = preset_.meshAngleTolDeg;
    if (chord >= api::viewer::kCoarseChordMm - kEps && angle >= api::viewer::kCoarseAngleDeg - kEps) {
        fineWanted_ = false; // the tier's tolerance is the coarse one (Low): nothing to refine
        return;
    }
    IJobSystem* jobs = this->jobs();
    if (jobs == nullptr)
        return;
    fineWanted_ = false;
    std::vector<std::size_t> todo; // the bodies not yet meshed at this tolerance
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].fine)
            todo.push_back(i);
    }
    if (todo.empty())
        return;
    const std::shared_ptr<const SceneSnapshot> snap = snapshot_;
    const std::uint64_t serial = ++jobSerial_;
    fineSerial_ = serial;
    jobTitle_ = tr("Refining the display mesh");
    jobProgress_ = 0;
    fine_ = jobs->submit<Meshes>(
        JobInfo{"Display mesh (fine)", JobPriority::Background, true},
        [snap, todo, chord, angle, meshed = meshed_](JobContext& ctx) -> Result<Meshes> {
            auto out = std::make_shared<SceneMeshBatch>();
            const std::size_t n = todo.size();
            for (std::size_t i : todo) {
                if (auto ok = ctx.cancel.check(); !ok)
                    return tl::unexpected(ok.error());
                JobContext sub{ctx.cancel, ctx.progress.sub(1.0 / double(n))};
                ++*meshed;
                Result<kernel::Mesh> mesh = api::viewer::meshBody(snap->bodies[i].shape, chord, angle, sub);
                if (!mesh && mesh.error().code == ErrorCode::Cancelled)
                    return tl::unexpected(mesh.error());
                out->indices.push_back(i);
                out->meshes.push_back(mesh ? std::make_shared<const kernel::Mesh>(std::move(*mesh)) : nullptr);
            }
            return Meshes(std::move(out));
        });
    QPointer<SceneController> self(this);
    fine_.onProgress([self, serial](double fraction, const std::string&) {
        if (!self || serial != self->fineSerial_ || !self->fine_.valid())
            return;
        self->jobProgress_ = fraction;
        self->updateJobSlot();
    });
    fine_.then([self, serial](Result<Meshes> meshes) {
        if (!self || serial != self->fineSerial_)
            return;
        self->fine_ = {};
        if (meshes)
            self->showMeshes(*meshes, true);
        self->updateJobSlot();
        emit self->stateChanged();
    });
    emit stateChanged();
}

void SceneController::showMeshes(const Meshes& meshes, bool fine)
{
    if (!meshes)
        return;
    int failed = 0;
    for (std::size_t k = 0; k < meshes->indices.size() && k < meshes->meshes.size(); ++k) {
        const std::size_t i = meshes->indices[k];
        const std::shared_ptr<const kernel::Mesh>& mesh = meshes->meshes[k];
        if (i >= slots_.size())
            continue;
        if (!mesh) {
            ++failed;
            continue;
        }
        slots_[i].mesh = mesh;
        slots_[i].fine = fine;
        pushToViewport(static_cast<int>(i));
    }
    tree_->refreshBodies();
    if (failed > 0 && !fine && context_)
        context_->showMessage(failed == 1 ? tr("1 body could not be meshed and is not shown.")
                                          : tr("%1 bodies could not be meshed and are not shown.").arg(failed),
                              true);
    frameIfNew();
}

void SceneController::frameIfNew()
{
    if (!framePending_)
        return;
    framePending_ = false;
    // T-092: a document edit feeds the same bodies again (every sketch step does); the view then stays where the user
    // put it. Only a different body set is framed anew.
    std::vector<BodyId> ids;
    for (const api::viewer::Body& b : snapshot_->bodies)
        ids.push_back(b.id);
    std::sort(ids.begin(), ids.end());
    if (!ids.empty() && ids == framedBodies_)
        return;
    framedBodies_ = std::move(ids);
    emit firstMeshesShown();
}

void SceneController::pushToViewport(int index)
{
    if (viewport_ == nullptr || index < 0 || index >= bodyCount())
        return;
    Slot& slot = slots_[static_cast<std::size_t>(index)];
    if (!slot.mesh)
        return;
    if (slot.item == 0) {
        slot.item = viewport_->addMesh(slot.mesh, Xform::identity(), modelStyle(body(index)));
        if (slot.item != 0 && !slot.visible)
            viewport_->setVisible(slot.item, false);
    } else {
        viewport_->updateMesh(slot.item, slot.mesh, std::nullopt);
    }
}

void SceneController::updateJobSlot()
{
    if (context_ == nullptr)
        return;
    if (coarse_.valid() || fine_.valid())
        context_->setJob(jobTitle_, jobProgress_);
    else
        context_->clearJob();
}

void SceneController::cancelJobs(bool quiet)
{
    if (!coarse_.valid() && !fine_.valid())
        return;
    const bool coarse = coarse_.valid();
    coarse_.cancel();
    fine_.cancel();
    coarse_ = {};
    fine_ = {};
    coarseSerial_ = fineSerial_ = 0; // abandon: late results are dropped
    fineWanted_ = false;
    updateJobSlot();
    if (context_ && !quiet)
        context_->setStatusText(coarse ? tr("Meshing cancelled") : tr("Mesh refinement cancelled"));
    emit stateChanged();
}

bool SceneController::ready() const
{
    return !slots_.empty() && !coarse_.valid() &&
           std::all_of(slots_.begin(), slots_.end(), [](const Slot& s) { return s.mesh != nullptr; });
}

const api::viewer::Body* SceneController::body(int index) const
{
    if (!snapshot_ || index < 0 || index >= static_cast<int>(snapshot_->bodies.size()))
        return nullptr;
    return &snapshot_->bodies[static_cast<std::size_t>(index)];
}

QString SceneController::bodyName(int index) const
{
    const api::viewer::Body* b = body(index);
    return b ? QString::fromStdString(b->name) : QString();
}

bool SceneController::bodyVisible(int index) const
{
    return index >= 0 && index < bodyCount() && slots_[static_cast<std::size_t>(index)].visible;
}

void SceneController::setBodyVisible(int index, bool visible)
{
    if (index < 0 || index >= bodyCount())
        return;
    Slot& slot = slots_[static_cast<std::size_t>(index)];
    if (slot.visible == visible)
        return;
    slot.visible = visible;
    if (viewport_ != nullptr && slot.item != 0)
        viewport_->setVisible(slot.item, visible);
    tree_->refreshBodies();
    emit bodyVisibilityChanged(index);
}

int SceneController::bodyIndexForItem(render::ItemId item) const
{
    if (item == 0)
        return -1;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].item == item)
            return static_cast<int>(i);
    }
    return -1;
}

render::ItemId SceneController::itemForBody(int index) const
{
    return index >= 0 && index < bodyCount() ? slots_[static_cast<std::size_t>(index)].item : 0;
}

std::shared_ptr<const kernel::Mesh> SceneController::meshOf(int index) const
{
    return index >= 0 && index < bodyCount() ? slots_[static_cast<std::size_t>(index)].mesh : nullptr;
}

bool SceneController::meshIsFine(int index) const
{
    return index >= 0 && index < bodyCount() && slots_[static_cast<std::size_t>(index)].fine;
}

void SceneController::setSelectedBodies(std::vector<int> bodies)
{
    std::sort(bodies.begin(), bodies.end());
    bodies.erase(std::unique(bodies.begin(), bodies.end()), bodies.end());
    if (bodies == selectedBodies_)
        return;
    selectedBodies_ = std::move(bodies);
    tree_->refreshBodies();
}

bool SceneController::bodySelected(int index) const
{
    return std::binary_search(selectedBodies_.begin(), selectedBodies_.end(), index);
}

std::optional<Box3> SceneController::boundsOf(std::span<const render::PickHit> hits) const
{
    BoxBuilder b;
    for (const render::PickHit& hit : hits) {
        const int index = bodyIndexForItem(hit.item);
        const std::shared_ptr<const kernel::Mesh> mesh = meshOf(index);
        if (!mesh)
            continue;
        switch (hit.kind) {
        case kernel::SubKind::Solid:
            for (std::size_t v = 0; v * 3 + 2 < mesh->positions.size(); ++v)
                b.addVertex(mesh->positions, v);
            break;
        case kernel::SubKind::Face:
            for (const kernel::FaceRange& face : mesh->faces) {
                if (face.face.index != hit.subIndex)
                    continue;
                for (std::uint32_t i = 0; i < face.indexCount; ++i) {
                    const std::size_t at = std::size_t{face.firstIndex} + i;
                    if (at < mesh->indices.size())
                        b.addVertex(mesh->positions, mesh->indices[at]);
                }
            }
            break;
        case kernel::SubKind::Edge:
            for (const kernel::EdgeRange& edge : mesh->edges) {
                if (edge.edge.index != hit.subIndex)
                    continue;
                for (std::uint32_t i = 0; i < edge.vertexCount; ++i)
                    b.addVertex(mesh->edgePositions, std::size_t{edge.firstVertex} + i);
            }
            break;
        case kernel::SubKind::Vertex:
            b.add(hit.point.x, hit.point.y, hit.point.z);
            break;
        }
    }
    if (!b.any)
        return std::nullopt;
    // A single point (a vertex) gets a small box so a fit has something to frame.
    const double pad = 1.0;
    if (b.box.max.x - b.box.min.x < kEps && b.box.max.y - b.box.min.y < kEps && b.box.max.z - b.box.min.z < kEps) {
        b.add(b.box.min.x - pad, b.box.min.y - pad, b.box.min.z - pad);
        b.add(b.box.max.x + pad, b.box.max.y + pad, b.box.max.z + pad);
    }
    return b.box;
}

} // namespace ray::ui
