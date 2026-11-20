#include "section_model.h"

#include "scene_controller.h"
#include "selection_model.h"
#include "ui_commands.h"
#include "units.h"
#include "viewport_item.h"

#include <QtCore/QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ray::ui {

namespace {
// T-023: section view and draft analysis (SectionModel::runCommand runs these).
// clang-format off
constexpr UiCommand kSectionCommands[] = {
    {"view.section.toggle", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Section View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Cuts the model with a plane to show its inside"), "", CommandContext::Global, ""},
    {"view.section.fromFace", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Section Parallel to Face"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Cuts the model parallel to the selected planar face"), "", CommandContext::Global, ""},
    {"view.draft.toggle", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Draft Analysis"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Colours faces by their draft angle to the pull direction"), "", CommandContext::Global, ""},
    {"view.draft.fromFace", QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Pull Direction from Face"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "View"),
     QT_TRANSLATE_NOOP("ray::ui::CommandCatalog", "Draft analysis with the selected planar face's normal as the pull"), "", CommandContext::Global, ""},
};
// clang-format on

} // namespace

std::span<const UiCommand> sectionUiCommands()
{
    return kSectionCommands;
}

namespace {

constexpr const char* kEngineKey = "_ray_ui_section";
constexpr double kMaxThresholdDeg = 89.0;
// A face is planar when every vertex normal is within this angle of the mean and every vertex within this fraction
// of the face's size of the mean plane.
constexpr double kPlanarCos = 0.99996; // 0.5 degree
constexpr double kPlanarFlatness = 1e-4;

constexpr double kMmPerInch = 25.4;

Vec3 axisNormal(const QString& plane)
{
    if (plane == QLatin1String("yz"))
        return Vec3{1, 0, 0};
    if (plane == QLatin1String("zx"))
        return Vec3{0, 1, 0};
    return Vec3{0, 0, 1};
}

std::optional<Vec3> axisPull(const QString& pull)
{
    static const std::array<std::pair<const char*, Vec3>, 6> kPulls = {{{"+x", {1, 0, 0}},
                                                                        {"-x", {-1, 0, 0}},
                                                                        {"+y", {0, 1, 0}},
                                                                        {"-y", {0, -1, 0}},
                                                                        {"+z", {0, 0, 1}},
                                                                        {"-z", {0, 0, -1}}}};
    for (const auto& [name, v] : kPulls) {
        if (pull == QLatin1String(name))
            return v;
    }
    return std::nullopt;
}

double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

SectionModel::SectionModel(SceneController* scene, SelectionModel* selection, ViewportProvider viewport,
                           QObject* parent)
    : QObject(parent), scene_(scene), selection_(selection), viewportProvider_(std::move(viewport))
{
    if (scene_) {
        // The range follows what is shown; a new body set puts an unplaced plane back in the middle.
        connect(scene_, &SceneController::bodiesChanged, this, [this] {
            offsetSet_ = false;
            updateRange(true);
        });
        connect(scene_, &SceneController::firstMeshesShown, this, [this] { updateRange(!offsetSet_); });
        connect(scene_, &SceneController::stateChanged, this, [this] { updateRange(!offsetSet_); });
        connect(scene_, &SceneController::bodyVisibilityChanged, this, [this] { updateRange(false); });
        connect(scene_, &SceneController::presetChanged, this, [this] {
            emit availabilityChanged();
            apply();
        });
    }
    if (selection_)
        connect(selection_, &SelectionModel::changed, this, &SectionModel::availabilityChanged);
    connect(this, &SectionModel::changed, this, &SectionModel::offsetTextChanged);
    connect(this, &SectionModel::unitChanged, this, &SectionModel::offsetTextChanged);
}

SectionModel::~SectionModel() = default;

SectionModel* SectionModel::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* section = qobject_cast<SectionModel*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(section, QJSEngine::CppOwnership);
            return section;
        }
    }
    return new SectionModel(nullptr, nullptr, {}); // a bare engine: nothing to cut
}

void SectionModel::provideFor(QQmlEngine* engine, SectionModel* section)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(section));
}

// ---------- state ----------
void SectionModel::setPanelOpen(bool open)
{
    if (open == panelOpen_)
        return;
    panelOpen_ = open;
    emit panelOpenChanged();
}

void SectionModel::setActive(bool on)
{
    if (on == active_)
        return;
    active_ = on;
    if (on)
        updateRange(!offsetSet_);
    emit changed();
    apply();
}

void SectionModel::setPlane(const QString& plane)
{
    const QString p = plane.toLower();
    if (p != QLatin1String("xy") && p != QLatin1String("yz") && p != QLatin1String("zx") && p != QLatin1String("face"))
        return;
    if (p == plane_)
        return;
    plane_ = p;
    offsetSet_ = false; // a new orientation starts in the middle of the model
    updateRange(true);
    emit changed();
    apply();
}

void SectionModel::setOffset(double mm)
{
    if (!std::isfinite(mm))
        return;
    if (hasRange_)
        mm = std::clamp(mm, rangeMin_, rangeMax_);
    offsetSet_ = true;
    if (mm == offset_)
        return;
    offset_ = mm;
    emit changed();
    apply();
}

void SectionModel::setFlipped(bool on)
{
    if (on == flipped_)
        return;
    flipped_ = on;
    emit changed();
    apply();
}

void SectionModel::setCap(bool on)
{
    if (on == cap_)
        return;
    cap_ = on;
    emit changed();
    apply();
}

bool SectionModel::capAvailable() const
{
    return scene_ && scene_->preset().sectionCaps;
}

QString SectionModel::capReason() const
{
    if (capAvailable())
        return {};
    // The effective preset (T-024: the tier with the user's overrides) has no caps: say which of the two decided.
    if (scene_ && render::presetFor(scene_->preset().tier).sectionCaps)
        return tr("Section caps are turned off in Settings (Graphics)");
    const QString tier = scene_ ? scene_->tierName() : QString();
    return tier.isEmpty() ? tr("Section caps need the Medium or High graphics tier")
                          : tr("Section caps need the Medium or High graphics tier (this computer uses %1)").arg(tier);
}

void SectionModel::setDraft(bool on)
{
    if (on == draft_)
        return;
    draft_ = on;
    emit changed();
    apply();
}

void SectionModel::setPull(const QString& pull)
{
    const QString p = pull.toLower();
    if (!axisPull(p) && p != QLatin1String("face"))
        return;
    if (p == pull_)
        return;
    pull_ = p;
    emit changed();
    apply();
}

void SectionModel::setDraftThreshold(double degrees)
{
    if (!std::isfinite(degrees))
        return;
    degrees = std::clamp(degrees, 0.0, kMaxThresholdDeg);
    if (degrees == threshold_)
        return;
    threshold_ = degrees;
    emit changed();
    apply();
}

// ---------- display units: the Units singleton (units.h, T-024), live ----------
void SectionModel::setUnits(UnitsObject* units)
{
    if (units_ == units)
        return;
    if (units_)
        disconnect(units_, nullptr, this, nullptr);
    units_ = units;
    if (units_)
        connect(units_, &UnitsObject::changed, this, &SectionModel::unitChanged);
    emit unitChanged();
}

bool SectionModel::inch() const
{
    return units_ && units_->system() == UnitSystem::Inch;
}

QString SectionModel::unit() const
{
    return units_ ? units_->unitLabel() : QStringLiteral("mm");
}

int SectionModel::unitDecimals() const
{
    return units_ ? units_->decimals() : 3;
}

double SectionModel::toUnits(double mm) const
{
    return inch() ? mm / kMmPerInch : mm;
}

double SectionModel::fromUnits(double value) const
{
    return inch() ? value * kMmPerInch : value;
}

QString SectionModel::offsetText() const
{
    return units_ ? units_->formatLength(offset_) : QString::number(offset_, 'f', 3) + QStringLiteral(" mm");
}

double SectionModel::parseLength(const QString& text) const
{
    if (units_)
        return units_->parseLength(text);
    bool ok = false;
    const double v = text.trimmed().toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

// ---------- the selected face ----------
std::optional<SectionModel::Face> SectionModel::selectedFace() const
{
    if (!selection_ || !scene_ || selection_->count() != 1)
        return std::nullopt;
    const SelectionEntry& e = selection_->entries().front();
    if (e.hit.kind != kernel::SubKind::Face)
        return std::nullopt;
    const std::shared_ptr<const kernel::Mesh> mesh = scene_->meshOf(scene_->bodyIndexForItem(e.hit.item));
    if (!mesh)
        return std::nullopt;
    const kernel::FaceRange* range = nullptr;
    for (const kernel::FaceRange& f : mesh->faces) {
        if (f.face.index == e.hit.subIndex)
            range = &f;
    }
    if (!range || range->indexCount == 0)
        return std::nullopt;
    // The display mesh's vertices of the face (the scene shows bodies untransformed).
    Vec3 normal{0, 0, 0}, centroid{0, 0, 0};
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    std::size_t n = 0;
    const auto vertexAt = [&](std::uint32_t i) -> std::optional<std::uint32_t> {
        const std::size_t at = std::size_t{range->firstIndex} + i;
        if (at >= mesh->indices.size() || std::size_t{mesh->indices[at]} * 3 + 2 >= mesh->positions.size())
            return std::nullopt;
        return mesh->indices[at];
    };
    for (std::uint32_t i = 0; i < range->indexCount; ++i) {
        const auto v = vertexAt(i);
        if (!v)
            return std::nullopt;
        const float* p = &mesh->positions[std::size_t{*v} * 3];
        const float* q = &mesh->normals[std::size_t{*v} * 3];
        normal = Vec3{normal.x + q[0], normal.y + q[1], normal.z + q[2]};
        centroid = Vec3{centroid.x + p[0], centroid.y + p[1], centroid.z + p[2]};
        for (int c = 0; c < 3; ++c) {
            lo = std::min(lo, double(p[c]));
            hi = std::max(hi, double(p[c]));
        }
        ++n;
    }
    const double len = std::sqrt(dot(normal, normal));
    if (n == 0 || !(len > 0))
        return std::nullopt;
    normal = Vec3{normal.x / len, normal.y / len, normal.z / len};
    centroid = Vec3{centroid.x / double(n), centroid.y / double(n), centroid.z / double(n)};
    const double flatness = kPlanarFlatness * std::max(hi - lo, 1e-3);
    for (std::uint32_t i = 0; i < range->indexCount; ++i) {
        const std::uint32_t v = *vertexAt(i);
        const float* p = &mesh->positions[std::size_t{v} * 3];
        const float* q = &mesh->normals[std::size_t{v} * 3];
        const double ql = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2]);
        if (!(ql > 0) || dot(Vec3{q[0] / ql, q[1] / ql, q[2] / ql}, normal) < kPlanarCos)
            return std::nullopt;
        const Vec3 d{p[0] - centroid.x, p[1] - centroid.y, p[2] - centroid.z};
        if (std::abs(dot(d, normal)) > flatness)
            return std::nullopt;
    }
    return Face{normal, centroid};
}

QString SectionModel::faceReason() const
{
    if (selectedFace())
        return {};
    if (!selection_ || selection_->count() == 0)
        return tr("Select a planar face first");
    if (selection_->count() > 1)
        return tr("Select only one planar face");
    if (selection_->entries().front().hit.kind != kernel::SubKind::Face)
        return tr("Select a face, not an edge, vertex or body");
    return tr("The selected face is not planar");
}

bool SectionModel::planeFromSelectedFace()
{
    const std::optional<Face> face = selectedFace();
    if (!face)
        return false;
    faceNormal_ = face->normal;
    plane_ = QStringLiteral("face");
    offsetSet_ = false;
    updateRange(true);
    emit changed();
    apply();
    return true;
}

bool SectionModel::pullFromSelectedFace()
{
    const std::optional<Face> face = selectedFace();
    if (!face)
        return false;
    pullFace_ = face->normal;
    pull_ = QStringLiteral("face");
    emit changed();
    apply();
    return true;
}

// ---------- what the renderer gets ----------
Vec3 SectionModel::planeNormal() const
{
    return plane_ == QLatin1String("face") ? faceNormal_ : axisNormal(plane_);
}

std::optional<render::SectionPlane> SectionModel::sectionPlane() const
{
    if (!active_)
        return std::nullopt;
    const Vec3 n = planeNormal();
    const Vec3 origin{n.x * offset_, n.y * offset_, n.z * offset_};
    const Vec3 normal = flipped_ ? Vec3{-n.x, -n.y, -n.z} : n;
    // The renderer draws caps only where the preset allows them; asking is harmless.
    return render::SectionPlane{origin, normal, cap_};
}

std::optional<render::DraftAnalysis> SectionModel::draftAnalysis() const
{
    if (!draft_)
        return std::nullopt;
    const Vec3 pull = pull_ == QLatin1String("face") ? pullFace_ : axisPull(pull_).value_or(Vec3{0, 0, 1});
    return render::DraftAnalysis{pull, threshold_};
}

ViewportItem* SectionModel::view() const
{
    if (!view_ && viewportProvider_)
        view_ = viewportProvider_();
    return view_;
}

void SectionModel::apply()
{
    const bool known = view_ != nullptr;
    ViewportItem* v = view();
    if (v == nullptr || v->viewport() == nullptr)
        return;
    if (!known && !active_ && !draft_)
        return; // a viewport that never saw a section needs nothing
    v->viewport()->setSection(sectionPlane());
    v->viewport()->setDraftAnalysis(draftAnalysis());
}

void SectionModel::updateRange(bool recentre)
{
    ViewportItem* v = view();
    const std::optional<Box3> box = v && v->viewport() ? v->viewport()->sceneBounds() : std::nullopt;
    const bool had = hasRange_;
    const double oldMin = rangeMin_, oldMax = rangeMax_;
    if (box) {
        const Vec3 n = planeNormal();
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 p{(corner & 1) ? box->max.x : box->min.x, (corner & 2) ? box->max.y : box->min.y,
                         (corner & 4) ? box->max.z : box->min.z};
            lo = std::min(lo, dot(p, n));
            hi = std::max(hi, dot(p, n));
        }
        rangeMin_ = lo;
        rangeMax_ = hi;
        hasRange_ = true;
    } else {
        hasRange_ = false;
    }
    if (had != hasRange_ || oldMin != rangeMin_ || oldMax != rangeMax_)
        emit rangeChanged();
    if (!hasRange_)
        return;
    const double before = offset_;
    if (recentre)
        offset_ = 0.5 * (rangeMin_ + rangeMax_);
    else
        offset_ = std::clamp(offset_, rangeMin_, rangeMax_);
    if (offset_ != before) {
        emit changed();
        apply();
    }
}

// ---------- commands ----------
bool SectionModel::handles(const QString& id)
{
    return id.startsWith(QLatin1String("view.section.")) || id.startsWith(QLatin1String("view.draft."));
}

QString SectionModel::commandReason(const QString& id) const
{
    ViewportItem* v = view();
    if (v == nullptr)
        return tr("There is no 3D view");
    const bool turningOff =
        (id == QLatin1String("view.section.toggle") && active_) || (id == QLatin1String("view.draft.toggle") && draft_);
    if (!v->hasContent() && !turningOff)
        return tr("Nothing is shown");
    if (id == QLatin1String("view.section.fromFace") || id == QLatin1String("view.draft.fromFace"))
        return faceReason();
    return {};
}

bool SectionModel::runCommand(const QString& id)
{
    if (!commandReason(id).isEmpty())
        return false;
    if (id == QLatin1String("view.section.toggle")) {
        setActive(!active_);
        if (active_)
            setPanelOpen(true);
        return true;
    }
    if (id == QLatin1String("view.section.fromFace")) {
        if (!planeFromSelectedFace())
            return false;
        setActive(true);
        setPanelOpen(true);
        return true;
    }
    if (id == QLatin1String("view.draft.toggle")) {
        setDraft(!draft_);
        if (draft_)
            setPanelOpen(true);
        return true;
    }
    if (id == QLatin1String("view.draft.fromFace")) {
        if (!pullFromSelectedFace())
            return false;
        setDraft(true);
        setPanelOpen(true);
        return true;
    }
    return false;
}

bool SectionModel::commandMarked(const QString& id) const
{
    if (id == QLatin1String("view.section.toggle"))
        return active_;
    if (id == QLatin1String("view.draft.toggle"))
        return draft_;
    return false;
}

} // namespace ray::ui
