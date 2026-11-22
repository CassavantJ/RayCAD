#include "selection_model.h"

#include "app_context.h"

#include <ray_render/viewport.h>

#include <QtCore/QVariantMap>
#include <QtGui/QGuiApplication>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ray::ui {

namespace {

constexpr const char* kEngineKey = "_ray_ui_selection";

bool sameHit(const render::PickHit& a, const render::PickHit& b)
{
    return a.item == b.item && a.kind == b.kind && a.subIndex == b.subIndex;
}

QString kindName(kernel::SubKind kind)
{
    switch (kind) {
    case kernel::SubKind::Face:
        return QObject::tr("Face");
    case kernel::SubKind::Edge:
        return QObject::tr("Edge");
    case kernel::SubKind::Vertex:
        return QObject::tr("Vertex");
    case kernel::SubKind::Solid:
        return QObject::tr("Body");
    }
    return {};
}

QString mm(double v)
{
    return QObject::tr("%1 mm").arg(QString::number(v, 'f', 3));
}

QString point(const Vec3& p)
{
    return QStringLiteral("%1, %2, %3")
        .arg(QString::number(p.x, 'f', 3), QString::number(p.y, 'f', 3), QString::number(p.z, 'f', 3));
}

QVariantMap row(const QString& label, const QString& value)
{
    return QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("value"), value}};
}

// T-024: a measured row also carries its quantity and raw value in mm, so the read-out formats it in the display
// units (Units.formatRow) and switches live; `value` keeps the mm text for anything that shows rows as they are.
QVariantMap measured(const QString& label, const QString& value, const char* quantity, const QVariant& mm)
{
    QVariantMap r = row(label, value);
    r.insert(QStringLiteral("quantity"), QString::fromLatin1(quantity));
    r.insert(QStringLiteral("mm"), mm);
    return r;
}

QVariant pointValue(const Vec3& p)
{
    return QVariantList{p.x, p.y, p.z};
}

// A unit direction as a person reads it: "+Z", "−X" along a model axis, else its components ("0.707, 0, 0.707").
QString directionText(const Vec3& d)
{
    const double n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (n < 1e-12)
        return QObject::tr("none");
    const Vec3 u{d.x / n, d.y / n, d.z / n};
    const std::pair<double, const char*> axes[] = {{u.x, "X"}, {u.y, "Y"}, {u.z, "Z"}};
    for (const auto& [c, name] : axes) {
        if (std::abs(std::abs(c) - 1.0) < 1e-9)
            return (c > 0 ? QStringLiteral("+") : QStringLiteral("−")) + QLatin1String(name);
    }
    const auto num = [](double v) {
        QString s = QString::number(std::abs(v) < 5e-4 ? 0.0 : v, 'f', 3);
        while (s.contains(QLatin1Char('.')) && (s.endsWith(QLatin1Char('0')) || s.endsWith(QLatin1Char('.'))))
            s.chop(1);
        return s == QLatin1String("-0") ? QStringLiteral("0") : s;
    };
    return QStringLiteral("%1, %2, %3").arg(num(u.x), num(u.y), num(u.z));
}

// What a sub/ref job finds out about one entry.
struct Resolution {
    std::optional<kernel::SubId> sub;
    std::optional<naming::TopoRef> ref;
};

struct InfoResult {
    std::optional<kernel::SubId> sub;
    api::viewer::ElementInfo info;
};

std::optional<kernel::SubId> subOf(const api::viewer::Body& body, const render::PickHit& hit)
{
    switch (hit.kind) {
    case kernel::SubKind::Face:
    case kernel::SubKind::Edge:
        return kernel::SubId{hit.kind, hit.subIndex};
    case kernel::SubKind::Solid:
        return kernel::SubId{kernel::SubKind::Solid, 0};
    case kernel::SubKind::Vertex:
        return api::viewer::vertexOfEdgeEnd(body.shape, render::vertexEdge(hit.subIndex), hit.point);
    }
    return std::nullopt;
}

} // namespace

SelectionModel::SelectionModel(SceneController* scene, QObject* parent) : QObject(parent), scene_(scene)
{
    if (scene_)
        connect(scene_, &SceneController::bodiesChanged, this, &SelectionModel::resetForNewScene);
}

SelectionModel::~SelectionModel() = default;

SelectionModel* SelectionModel::create(QQmlEngine* qml, QJSEngine*)
{
    if (qml != nullptr) {
        if (auto* selection = qobject_cast<SelectionModel*>(qml->property(kEngineKey).value<QObject*>())) {
            QJSEngine::setObjectOwnership(selection, QJSEngine::CppOwnership);
            return selection;
        }
    }
    return new SelectionModel(nullptr, nullptr);
}

void SelectionModel::provideFor(QQmlEngine* engine, SelectionModel* selection)
{
    engine->setProperty(kEngineKey, QVariant::fromValue<QObject*>(selection));
}

void SelectionModel::setMaskBit(std::uint8_t bit, bool on)
{
    setFilterMask(on ? std::uint8_t(mask_ | bit) : std::uint8_t(mask_ & ~bit));
}

void SelectionModel::setFilterMask(std::uint8_t mask)
{
    mask &= kFaces | kEdges | kVertices | kBodies;
    if (mask == mask_)
        return;
    mask_ = mask;
    // A hover of a kind no longer picked goes.
    setHover(std::nullopt);
    emit filterChanged();
}

bool SelectionModel::pending() const
{
    return std::any_of(entries_.begin(), entries_.end(), [](const SelectionEntry& e) { return e.pending; });
}

QString SelectionModel::summary() const
{
    if (entries_.empty())
        return tr("Nothing selected");
    if (entries_.size() == 1)
        return kindName(entries_.front().hit.kind);
    return tr("%1 items").arg(count());
}

std::optional<SelectionEntry> SelectionModel::entryFor(const render::PickHit& hit)
{
    if (!scene_)
        return std::nullopt;
    const int body = scene_->bodyIndexForItem(hit.item);
    if (body < 0)
        return std::nullopt;
    SelectionEntry e;
    e.hit = hit;
    e.body = body;
    e.serial = nextSerial_++;
    if (hit.kind == kernel::SubKind::Face || hit.kind == kernel::SubKind::Edge)
        e.sub = kernel::SubId{hit.kind, hit.subIndex};
    else if (hit.kind == kernel::SubKind::Solid)
        e.sub = kernel::SubId{kernel::SubKind::Solid, 0};
    return e;
}

int SelectionModel::find(const render::PickHit& hit) const
{
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (sameHit(entries_[i].hit, hit))
            return static_cast<int>(i);
    }
    return -1;
}

void SelectionModel::add(const render::PickHit& hit)
{
    if (find(hit) >= 0)
        return;
    std::optional<SelectionEntry> e = entryFor(hit);
    if (!e)
        return;
    resolveLater(*e);
    entries_.push_back(std::move(*e));
}

void SelectionModel::toggle(const render::PickHit& hit)
{
    const int at = find(hit);
    if (at >= 0)
        entries_.erase(entries_.begin() + at);
    else
        add(hit);
}

void SelectionModel::resolveLater(SelectionEntry& entry)
{
    const api::viewer::Body* body = scene_ ? scene_->body(entry.body) : nullptr;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    const bool needsRef = body != nullptr && body->map.has_value() && entry.hit.kind != kernel::SubKind::Solid;
    const bool needsSub = !entry.sub.has_value();
    if (body == nullptr || jobs == nullptr || (!needsRef && !needsSub))
        return;
    entry.pending = true;
    const std::shared_ptr<const SceneSnapshot> snap = scene_->snapshot();
    const std::size_t index = static_cast<std::size_t>(entry.body);
    const render::PickHit hit = entry.hit;
    const std::uint64_t serial = entry.serial;
    // adjacent() and the element map are read on a worker: on a large body they are not "trivial" (PLAN.md 7).
    JobHandle<Resolution> job = jobs->submit<Resolution>(
        JobInfo{"Selection reference", JobPriority::Interactive, false}, [snap, index, hit](JobContext&) {
            Resolution r;
            const api::viewer::Body& b = snap->bodies[index];
            r.sub = subOf(b, hit);
            if (r.sub && snap->resolver)
                r.ref = snap->resolver->makeRef(b.id, *r.sub);
            return Result<Resolution>(std::move(r));
        });
    QPointer<SelectionModel> self(this);
    const std::uint64_t generation = snap->generation;
    job.then([self, serial, generation](Result<Resolution> r) {
        if (!self || !self->scene_ || self->scene_->snapshot()->generation != generation)
            return;
        for (SelectionEntry& e : self->entries_) {
            if (e.serial != serial)
                continue;
            e.pending = false;
            if (r) {
                e.sub = r->sub;
                e.ref = std::move(r->ref);
            }
            emit self->changed();
            if (self->infoFor_ == serial && self->infoJob_ == 0)
                self->infoFor_ = 0; // the read-out shows the reference too: redo it
            self->updateInfo();
            return;
        }
    });
}

void SelectionModel::setPathPicking(bool on)
{
    if (on == pathPicking_)
        return;
    pathPicking_ = on;
    setHover(std::nullopt); // a hover of a kind no longer picked goes
    emit filterChanged();
}

void SelectionModel::setPathHighlight(std::optional<render::PickHit> hit)
{
    if (hit.has_value() == pathHighlight_.has_value() &&
        (!hit || (hit->item == pathHighlight_->item && hit->moveIndex == pathHighlight_->moveIndex)))
        return;
    pathHighlight_ = hit;
    emit highlightChanged();
}

void SelectionModel::applyClick(std::optional<render::PickHit> hit, Qt::KeyboardModifiers modifiers)
{
    // T-047: a toolpath move under the click while the G-code viewer picks them.
    if (hit && pathPicking_ && scene_ && scene_->bodyIndexForItem(hit->item) < 0) {
        setPathHighlight(*hit);
        emit pathPicked(*hit);
        return;
    }
    const bool ctrl = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    if (!hit) {
        if (!ctrl && !shift && !entries_.empty()) {
            entries_.clear();
            selectionChanged();
        }
        return;
    }
    if (ctrl) {
        toggle(*hit);
    } else if (shift) {
        add(*hit);
    } else {
        if (entries_.size() == 1 && sameHit(entries_.front().hit, *hit))
            return;
        entries_.clear();
        add(*hit);
    }
    selectionChanged();
}

void SelectionModel::applyBox(const std::vector<render::PickHit>& hits, Qt::KeyboardModifiers modifiers)
{
    const bool ctrl = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    if (!ctrl && !shift)
        entries_.clear();
    for (const render::PickHit& hit : hits) {
        if (ctrl)
            toggle(hit);
        else
            add(hit);
    }
    selectionChanged();
}

void SelectionModel::setHover(std::optional<render::PickHit> hit)
{
    if (hit.has_value() == hover_.has_value() && (!hit || sameHit(*hit, *hover_)))
        return;
    hover_ = hit;
    emit highlightChanged();
}

void SelectionModel::clear()
{
    if (entries_.empty())
        return;
    entries_.clear();
    selectionChanged();
}

void SelectionModel::selectBody(int bodyIndex, int modifiers)
{
    if (!scene_)
        return;
    const Qt::KeyboardModifiers mods =
        modifiers < 0 ? QGuiApplication::keyboardModifiers() : Qt::KeyboardModifiers(modifiers);
    const render::ItemId item = scene_->itemForBody(bodyIndex);
    if (item == 0)
        return; // not shown yet
    applyClick(render::PickHit{item, kernel::SubKind::Solid, 0, Vec3{}, 0}, mods);
}

bool SelectionModel::isBodySelected(int bodyIndex) const
{
    return std::any_of(entries_.begin(), entries_.end(), [&](const SelectionEntry& e) { return e.body == bodyIndex; });
}

std::vector<render::PickHit> SelectionModel::hits() const
{
    std::vector<render::PickHit> out;
    out.reserve(entries_.size() + 1);
    for (const SelectionEntry& e : entries_)
        out.push_back(e.hit);
    if (pathHighlight_)
        out.push_back(*pathHighlight_);
    out.insert(out.end(), toolHighlight_.begin(), toolHighlight_.end());
    return out;
}

void SelectionModel::setToolHighlight(std::vector<render::PickHit> hits, std::optional<render::PickHit> emphasis)
{
    const auto same = [](const std::vector<render::PickHit>& a, const std::vector<render::PickHit>& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), sameHit);
    };
    const bool sameEmphasis =
        emphasis.has_value() == toolEmphasis_.has_value() && (!emphasis || sameHit(*emphasis, *toolEmphasis_));
    if (same(hits, toolHighlight_) && sameEmphasis)
        return;
    toolHighlight_ = std::move(hits);
    toolEmphasis_ = emphasis;
    emit highlightChanged();
}

void SelectionModel::selectionChanged()
{
    if (scene_) {
        std::vector<int> bodies;
        for (const SelectionEntry& e : entries_)
            bodies.push_back(e.body);
        scene_->setSelectedBodies(std::move(bodies));
    }
    emit changed();
    emit highlightChanged();
    updateInfo();
}

void SelectionModel::resetForNewScene()
{
    entries_.clear();
    hover_.reset();
    infoJob_ = 0;
    infoFor_ = 0;
    infoTitle_.clear();
    info_.clear();
    emit changed();
    emit highlightChanged();
    emit infoChanged();
}

void SelectionModel::updateInfo()
{
    if (entries_.empty()) {
        infoJob_ = 0;
        infoFor_ = 0;
        if (!infoTitle_.isEmpty() || !info_.isEmpty()) {
            infoTitle_.clear();
            info_.clear();
            emit infoChanged();
        }
        return;
    }
    const SelectionEntry& last = entries_.back();
    if (last.serial == infoFor_ && infoJob_ == 0 && !info_.isEmpty()) {
        // Same element; only the title (count) may differ.
        const QString title = entries_.size() > 1 ? tr("%1 items selected").arg(count()) : kindName(last.hit.kind);
        if (title != infoTitle_) {
            infoTitle_ = title;
            emit infoChanged();
        }
        return;
    }
    const api::viewer::Body* body = scene_ ? scene_->body(last.body) : nullptr;
    IJobSystem* jobs = scene_ ? scene_->jobs() : nullptr;
    if (body == nullptr || jobs == nullptr)
        return;
    const std::uint64_t serial = last.serial;
    if (infoJob_ == serial)
        return; // already being worked out
    infoJob_ = serial;
    infoFor_ = serial;
    emit infoChanged();
    const std::shared_ptr<const SceneSnapshot> snap = scene_->snapshot();
    const std::size_t index = static_cast<std::size_t>(last.body);
    const render::PickHit hit = last.hit;
    const std::optional<kernel::SubId> known = last.sub;
    JobHandle<InfoResult> job = jobs->submit<InfoResult>(
        JobInfo{"Selection properties", JobPriority::Interactive, false},
        [snap, index, hit, known](JobContext& ctx) -> Result<InfoResult> {
            const api::viewer::Body& b = snap->bodies[index];
            InfoResult out;
            out.sub = known ? known : subOf(b, hit);
            if (!out.sub)
                return tl::unexpected(Error{ErrorCode::NotFound, "The picked vertex was not found.", {}});
            Result<api::viewer::ElementInfo> info = api::viewer::describeElement(b.shape, *out.sub, ctx);
            if (!info)
                return tl::unexpected(info.error());
            out.info = std::move(*info);
            return out;
        });
    QPointer<SelectionModel> self(this);
    const std::uint64_t generation = snap->generation;
    const QString bodyName = QString::fromStdString(body->name);
    job.then([self, serial, generation, bodyName](Result<InfoResult> r) {
        if (!self || self->infoJob_ != serial || !self->scene_ || self->scene_->snapshot()->generation != generation)
            return;
        self->infoJob_ = 0;
        QVariantList rows;
        const SelectionEntry* entry = nullptr;
        for (const SelectionEntry& e : self->entries_) {
            if (e.serial == serial)
                entry = &e;
        }
        const kernel::SubKind kind = entry ? entry->hit.kind : kernel::SubKind::Solid;
        rows << row(tr("Body"), bodyName);
        if (!r) {
            rows << row(tr("Error"), QString::fromStdString(r.error().message));
        } else {
            // What a person reads off the element; internal ids (the element's index, its reference's names) are not
            // shown: they stay in the selection's entries for tests and scripts.
            const api::viewer::ElementInfo& info = r->info;
            if (info.surface) {
                const kernel::SurfaceInfo& s = *info.surface;
                rows << row(tr("Surface"), QString::fromLatin1(api::viewer::surfaceTypeName(s.type).data()));
                rows << measured(tr("Area"), tr("%1 mm²").arg(QString::number(s.area, 'f', 3)), "area", s.area);
                if (s.type == kernel::SurfaceType::Cylinder) {
                    rows << measured(tr("Diameter"), mm(2 * s.radius), "length", 2 * s.radius);
                    rows << row(tr("Axis"), directionText(s.axis));
                } else if (s.type == kernel::SurfaceType::Plane) {
                    // Out of the material: the plane's normal, turned round where the face is reversed on it.
                    const Vec3 n = s.reversed ? Vec3{-s.axis.x, -s.axis.y, -s.axis.z} : s.axis;
                    rows << row(tr("Normal"), directionText(n));
                } else if (s.radius > 0) {
                    rows << measured(tr("Radius"), mm(s.radius), "length", s.radius);
                }
            }
            if (info.curve) {
                rows << row(tr("Curve"), QString::fromLatin1(api::viewer::curveTypeName(info.curve->type).data()));
                rows << measured(tr("Length"), mm(info.curve->length), "length", info.curve->length);
                if (info.curve->radius > 0)
                    rows << measured(tr("Radius"), mm(info.curve->radius), "length", info.curve->radius);
            }
            if (info.point)
                rows << measured(tr("Position"), point(*info.point), "point", pointValue(*info.point));
            if (info.mass) {
                rows << measured(tr("Volume"), tr("%1 mm³").arg(QString::number(info.mass->volume, 'f', 3)), "volume",
                                 info.mass->volume);
                rows << measured(tr("Area"), tr("%1 mm²").arg(QString::number(info.mass->area, 'f', 3)), "area",
                                 info.mass->area);
            }
            if (info.bounds) {
                const Box3& b = *info.bounds;
                const Vec3 size{b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z};
                rows << measured(tr("Size"), point(size), "point", pointValue(size));
                rows << measured(tr("Min"), point(b.min), "point", pointValue(b.min));
                rows << measured(tr("Max"), point(b.max), "point", pointValue(b.max));
            }
        }
        self->info_ = rows;
        self->infoTitle_ = self->entries_.size() > 1 ? tr("%1 items selected").arg(self->count()) : kindName(kind);
        emit self->infoChanged();
    });
}

} // namespace ray::ui
