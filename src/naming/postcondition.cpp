// The resolver's post-condition and the pick-time samples it checks (postcondition.h; T-081b round 8).
#include "postcondition.h"

#include "naming_testing.h"
#include "propagate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace ray::naming {

namespace testing {
namespace {
std::atomic<int> g_postcondition{-1}; // -1: not switched (the environment decides), 0 off, 1 on
}
void setPostconditionEnabled(bool on) noexcept
{
    g_postcondition.store(on ? 1 : 0);
}
bool postconditionEnabled() noexcept
{
    const int v = g_postcondition.load();
    if (v >= 0)
        return v == 1;
    static const bool off = std::getenv("RAYCAD_NAMING_NO_POSTCONDITION") != nullptr;
    return !off;
}
} // namespace testing

namespace detail {

namespace {

using kernel::SubId;
using kernel::SubKind;
using Type = SampleCarrier::Type;

Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 mul(const Vec3& a, double k)
{
    return {a.x * k, a.y * k, a.z * k};
}
double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double norm(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}
std::optional<Vec3> unit(const Vec3& a)
{
    const double n = norm(a);
    if (!(n > 1e-12))
        return std::nullopt;
    return mul(a, 1.0 / n);
}

// RAYCAD_NAMING_POST_TRACE=1 (decisions), 2 (and the answers that pass), 3 (and the samples and carriers): read once,
// for diagnosis (lines on stderr).
int traceLevel()
{
    static const int level = [] {
        const char* v = std::getenv("RAYCAD_NAMING_POST_TRACE");
        return v == nullptr ? 0 : std::max(1, std::atoi(v));
    }();
    return level;
}
Resolved brokenBecause(std::string reason)
{
    return Resolved{ResolveState::Broken, {}, std::move(reason)};
}

// ---------------------------------------------------------------- placements as moves
struct Rigid {
    std::array<double, 9> r{1, 0, 0, 0, 1, 0, 0, 0, 1}; // frame to model, row-major
    Vec3 t{};
};
Rigid rigidOf(const FeaturePlacement& p)
{
    Rigid out;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j)
            out.r[i * 3 + j] = static_cast<double>(p.q[i * 4 + j]) / 1e9;
    out.t = {static_cast<double>(p.q[3]) / kLengthQuantaPerMm, static_cast<double>(p.q[7]) / kLengthQuantaPerMm,
             static_cast<double>(p.q[11]) / kLengthQuantaPerMm};
    return out;
}
// Direction d of a feature placed at `from`, where it points when the feature is placed at `to`.
Vec3 turn(const Rigid& from, const Rigid& to, const Vec3& d)
{
    const std::array<double, 3> local{from.r[0] * d.x + from.r[3] * d.y + from.r[6] * d.z,
                                      from.r[1] * d.x + from.r[4] * d.y + from.r[7] * d.z,
                                      from.r[2] * d.x + from.r[5] * d.y + from.r[8] * d.z};
    return {to.r[0] * local[0] + to.r[1] * local[1] + to.r[2] * local[2],
            to.r[3] * local[0] + to.r[4] * local[1] + to.r[5] * local[2],
            to.r[6] * local[0] + to.r[7] * local[1] + to.r[8] * local[2]};
}
// Model point x of a feature placed at `from`, where that point of the feature is when it is placed at `to`.
Vec3 carry(const Rigid& from, const Rigid& to, const Vec3& x)
{
    return add(turn(from, to, sub(x, from.t)), to.t);
}

// ---------------------------------------------------------------- surfaces
struct Surface {
    Type type = Type::Other;
    Vec3 o{}, a{}; // plane: a point of it, its outward normal; cylinder: a point of its axis, its axis (unit)
    double r = 0;  // cylinder
    bool inward = false;
};

Surface surfaceOf(const kernel::SurfaceInfo& info)
{
    Surface s;
    const auto a = unit(info.axis);
    if (!a)
        return s;
    s.o = info.origin;
    s.a = *a;
    if (info.type == kernel::SurfaceType::Plane) {
        s.type = Type::Plane;
        if (info.reversed)
            s.a = mul(s.a, -1.0);
    } else if (info.type == kernel::SurfaceType::Cylinder && info.radius > 0) {
        s.type = Type::Cylinder;
        s.r = info.radius;
        s.inward = info.reversed;
    }
    return s;
}

bool analytic(const Surface& s);
Vec3 radialOf(const Surface& s, const Vec3& p);

// A face's surface, its outward side from the kernel's face sample (dP/du x dP/dv, reversed for a reversed face, so
// right for left-handed surface frames too: SurfaceInfo::reversed alone is not). Other when unreadable.
Surface faceSurfaceOf(const TerritoryCache& t, SubId face)
{
    const std::optional<kernel::SurfaceInfo>& info = t.surface(face);
    if (!info)
        return {};
    Surface s = surfaceOf(*info);
    const std::optional<kernel::FaceSample>& smp = t.faceSample(face);
    if (!smp || !analytic(s))
        return s;
    if (s.type == Type::Plane) {
        const double d = dot(smp->normal, s.a);
        if (d < 0)
            s.a = mul(s.a, -1.0);
    } else {
        const Vec3 radial = radialOf(s, smp->point);
        const double d = dot(smp->normal, radial);
        if (d != 0)
            s.inward = d < 0;
    }
    return s;
}

Surface surfaceOf(const SampleCarrier& c)
{
    Surface s;
    s.type = c.type;
    s.o = fromQuanta(c.qOrigin);
    const Vec3 a{static_cast<double>(c.qAxis[0]) / kAxisQuanta, static_cast<double>(c.qAxis[1]) / kAxisQuanta,
                 static_cast<double>(c.qAxis[2]) / kAxisQuanta};
    if (const auto u = unit(a))
        s.a = *u;
    else
        s.type = Type::Other;
    s.r = std::abs(static_cast<double>(c.qRadius)) / kLengthQuantaPerMm;
    s.inward = c.qRadius < 0;
    return s;
}

SampleCarrier carrierOf(const Surface& s)
{
    SampleCarrier c;
    c.type = s.type;
    if (s.type == Type::Other)
        return c;
    c.qOrigin = toQuanta(s.o);
    c.qAxis = {quantise(s.a.x, kAxisQuanta), quantise(s.a.y, kAxisQuanta), quantise(s.a.z, kAxisQuanta)};
    if (s.type == Type::Cylinder) {
        c.qRadius = quantise(s.r, kLengthQuantaPerMm);
        if (s.inward)
            c.qRadius = -c.qRadius;
    }
    return c;
}

bool analytic(const Surface& s)
{
    return s.type == Type::Plane || s.type == Type::Cylinder;
}

Vec3 radialOf(const Surface& s, const Vec3& p)
{
    const Vec3 w = sub(p, s.o);
    return sub(w, mul(s.a, dot(w, s.a)));
}

// The outward normal of an analytic surface at p (zero on a cylinder's axis or for another surface).
Vec3 normalAt(const Surface& s, const Vec3& p)
{
    if (s.type == Type::Plane)
        return s.a;
    if (s.type == Type::Cylinder)
        if (const auto u = unit(radialOf(s, p)))
            return s.inward ? mul(*u, -1.0) : *u;
    return {};
}

// p moved onto the surface by the shortest way (a plane along its normal, a cylinder straight out from its axis).
Vec3 onto(const Surface& s, const Vec3& p)
{
    if (s.type == Type::Plane)
        return sub(p, mul(s.a, dot(sub(p, s.o), s.a)));
    if (s.type == Type::Cylinder) {
        const Vec3 radial = radialOf(s, p);
        const double l = norm(radial);
        if (l > 1e-12)
            return add(sub(p, radial), mul(radial, s.r / l));
    }
    return p;
}

// p moved along direction u onto the surface (the nearer crossing), or by the shortest way when u does not reach it.
Vec3 slide(const Surface& s, const Vec3& p, const Vec3& u)
{
    if (s.type == Type::Plane) {
        const double un = dot(u, s.a);
        if (std::abs(un) < 1e-9)
            return onto(s, p);
        return sub(p, mul(u, dot(sub(p, s.o), s.a) / un));
    }
    if (s.type == Type::Cylinder) {
        const Vec3 w = radialOf(s, p);
        const Vec3 uu = sub(u, mul(s.a, dot(u, s.a)));
        const double A = dot(uu, uu), B = 2 * dot(w, uu), C = dot(w, w) - s.r * s.r;
        const double disc = B * B - 4 * A * C;
        if (A < 1e-12 || disc < 0)
            return onto(s, p);
        const double root = std::sqrt(disc);
        const double s1 = (-B + root) / (2 * A), s2 = (-B - root) / (2 * A);
        return add(p, mul(u, std::abs(s1) < std::abs(s2) ? s1 : s2));
    }
    return p;
}

// Does a face's surface `f` lie on the mapped surface `s` (the same plane on the same side, or the same cylinder)?
bool sameSurface(const Surface& f, const Surface& s, double tol)
{
    if (f.type != s.type || !analytic(s))
        return false;
    if (s.type == Type::Plane)
        return dot(f.a, s.a) >= 1.0 - 1e-5 && std::abs(dot(sub(f.o, s.o), s.a)) <= tol;
    return std::abs(dot(f.a, s.a)) >= 1.0 - 1e-5 && std::abs(f.r - s.r) <= tol && f.inward == s.inward &&
           norm(radialOf(s, f.o)) <= tol;
}

double offSurface(const Surface& s, const Vec3& p)
{
    if (s.type == Type::Plane)
        return std::abs(dot(sub(p, s.o), s.a));
    if (s.type == Type::Cylinder)
        return std::abs(norm(radialOf(s, p)) - s.r);
    return 0;
}

// ---------------------------------------------------------------- owners
// The feature whose placement carries an element with these names: the one origin with a placement among its names,
// a split piece's name standing for its parents' (a piece lies on the surface of the element it was split from, whose
// owner the piece's own origin, the feature that split it, does not say: an imported face a cut runs through stays
// where it is), when no such name has an origin without one (a face unified from an imported face and a sweep's cap
// stays where the imported one is). nullopt: none; `unknownSplit` set when a split piece's parents have no record in
// the map (no element holds their names any more), so whose surface it lies on is not known.
std::optional<FeatureId> ownerOf(const ElementMap& map, std::span<const Hash256> names, bool& unknownSplit)
{
    std::optional<FeatureId> out;
    for (const Hash256& name : names) {
        const TopoName* record = map.record(name);
        if (record == nullptr)
            continue;
        std::vector<const TopoName*> roots;
        if (record->role == kSplitRole) {
            // Up the chain of split parents (a piece of a piece), as far as the map keeps their records.
            std::vector<const TopoName*> level{record};
            for (int depth = 0; depth < 4 && !level.empty(); ++depth) {
                std::vector<const TopoName*> next;
                for (const TopoName* r : level)
                    for (const Hash256& parent : r->parents)
                        if (const TopoName* p = map.record(parent))
                            (p->role == kSplitRole ? next : roots).push_back(p);
                level = std::move(next);
            }
            if (roots.empty()) {
                unknownSplit = true;
                continue;
            }
        } else {
            roots.push_back(record);
        }
        for (const TopoName* root : roots) {
            if (map.placement(root->origin) == nullptr)
                return std::nullopt;
            if (out && !(*out == root->origin))
                return std::nullopt;
            out = root->origin;
        }
    }
    return out;
}

std::int32_t ownerIndex(const ElementMap& map, std::vector<FeaturePlacement>& placements,
                        std::span<const Hash256> names, bool* unknownSplit = nullptr)
{
    bool unknown = false;
    const auto owner = ownerOf(map, names, unknown);
    if (unknownSplit != nullptr)
        *unknownSplit = unknown && !owner;
    if (!owner)
        return -1;
    const FeaturePlacement* p = map.placement(*owner);
    if (p == nullptr)
        return -1;
    for (std::size_t i = 0; i < placements.size(); ++i)
        if (placements[i].feature == *owner)
            return static_cast<std::int32_t>(i);
    placements.push_back(*p);
    return static_cast<std::int32_t>(placements.size() - 1);
}

// A face whose names say no owner (ownerOf) and that is a split piece whose parents are not known, split off by a
// feature with a placement (its split names' one origin): that feature may own its surface (its own face, split as it
// was made) or may only have split it (an imported face a cut runs through). -2 - (index into `placements`), or -1
// when there is no such feature.
std::int32_t splitOwnerIndex(const ElementMap& map, std::vector<FeaturePlacement>& placements,
                             std::span<const Hash256> names)
{
    std::optional<FeatureId> origin;
    for (const Hash256& name : names) {
        const TopoName* record = map.record(name);
        if (record == nullptr || record->role != kSplitRole || map.placement(record->origin) == nullptr)
            continue;
        if (origin && !(*origin == record->origin))
            return -1;
        origin = record->origin;
    }
    if (!origin)
        return -1;
    for (std::size_t i = 0; i < placements.size(); ++i)
        if (placements[i].feature == *origin)
            return -2 - static_cast<std::int32_t>(i);
    placements.push_back(*map.placement(*origin));
    return -2 - static_cast<std::int32_t>(placements.size() - 1);
}

std::array<std::int64_t, 3> axisQuanta(const Vec3& d)
{
    return {quantise(d.x, kAxisQuanta), quantise(d.y, kAxisQuanta), quantise(d.z, kAxisQuanta)};
}

Vec3 fromAxisQuanta(const std::int64_t* q)
{
    const Vec3 d{static_cast<double>(q[0]) / kAxisQuanta, static_cast<double>(q[1]) / kAxisQuanta,
                 static_cast<double>(q[2]) / kAxisQuanta};
    return unit(d).value_or(Vec3{});
}

// ---------------------------------------------------------------- the check
struct Sample {
    Vec3 p{};    // where it is (pick time, then mapped)
    Vec3 into{}; // an edge's end: the direction into the edge
    bool end = false;
};

std::vector<Sample> samplesOf(const TopoRef& ref)
{
    std::vector<Sample> out;
    const PickSamples& s = ref.samples;
    if (s.known()) {
        const bool ends = ref.kind == SubKind::Edge &&
                          std::any_of(s.ends.begin(), s.ends.end(), [](std::int64_t v) { return v != 0; });
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            Sample x;
            x.p = fromQuanta(s.points[i]);
            if (ends && i < 2) {
                x.end = true;
                x.into = fromAxisQuanta(s.ends.data() + 3 * i);
            }
            out.push_back(x);
        }
        return out;
    }
    for (const auto& q : ref.territory.points)
        out.push_back(Sample{fromQuanta(q), {}, false});
    return out;
}

class Checker {
public:
    // inPlace: the samples where they were picked, the surfaces as moved (decide(): a feature moved along its own
    // surfaces).
    // carrySplit: a surface split off by a feature with a placement (SampleCarrier::owner <= -2) moves with it; else it
    // stays.
    // noSlide: a crossing of a moved surface with one that stayed is settled from the point where it was picked by the
    // shortest way onto both, not carried and slid along the moved feature's direction (decide()).
    Checker(const ElementMap& map, const TerritoryCache& t, const TopoRef& ref, bool inPlace = false,
            bool carrySplit = false, bool noSlide = false)
        : map_(map), t_(t), ref_(ref), inPlace_(inPlace), carrySplit_(carrySplit), noSlide_(noSlide)
    {
        picked_ = samplesOf(ref);
        const std::size_t own = std::min<std::size_t>(ref.samples.ownCarriers, ref.samples.carriers.size());
        for (std::size_t i = 0; i < ref.samples.carriers.size(); ++i)
            (i < own ? then_ : ends_).push_back(surfaceOf(ref.samples.carriers[i]));
        moveOf();
        now_ = then_;
        if (moved_ && !inPlace_)
            for (std::size_t i = 0; i < now_.size(); ++i)
                if (carried_[i]) {
                    now_[i].o = carry(from_, to_, then_[i].o);
                    now_[i].a = turn(from_, to_, then_[i].a);
                }
        tol_ = moved_ ? 1e-2 : kOnTolerance;
        length_ = static_cast<double>(ref.hint.qArea) / kLengthQuantaPerMm;
        // An edge or vertex whose surfaces all lie where they did (those that moved moved along themselves: a boss's
        // cap moved within its plane, a wall moved along itself, where it crosses a face that stayed) lies on the same
        // crossing, and moves along it only with what bounds it, as the oracle maps such crossings: a vertex not at
        // all; an edge on a cap or on an imported face not at all (what lies in the imported base stays: block 4 19:40
        // edge 191, T-081a random 4:5 edges 4 and 8, review1 9:13 edge 9); another only when a face at one of its ends
        // was moved off itself (directedit "an edge moved along its line with its feature": the pocket's end wall, the
        // pocket moved along its side walls); else it lies where it did, whatever slid along it.
        // (A cap of the moved feature, moved within its plane, carries no crossing with it whatever bounds it: block 4
        // 19:40 edge 191, the boss's start cap where it crosses the plane x 400, between the moved boss's walls.)
        bool caps = moved_;
        const Vec3 z{to_.r[2], to_.r[5], to_.r[8]};
        for (std::size_t i = 0; i < now_.size(); ++i)
            caps = caps && (!carried_[i] || (now_[i].type == Type::Plane && std::abs(dot(now_[i].a, z)) >= 1.0 - 1e-6));
        const bool crossingStays = ref.kind != SubKind::Face && movedWithinItself() &&
                                   (ref.kind == SubKind::Vertex || caps || onUnownedSurface() || !endsMovedOff());
        for (const Sample& s : picked_)
            mapped_.push_back(inPlace_ || crossingStays ? s : mapSample(s));
    }

    // Was a face at an end vertex of an edge (PickSamples::ownCarriers) moved off itself by its feature's move?
    bool endsMovedOff() const
    {
        const PickSamples& s = ref_.samples;
        const std::size_t own = std::min<std::size_t>(s.ownCarriers, s.carriers.size());
        return movedOff(own, s.carriers.size());
    }

    // Does the pick lie on a surface of material that stays whatever moves along it: an own surface without an owner
    // (an imported face; the oracle keeps what lies in the imported base where it was: T-081a random 4:5, review1
    // 9:13)?
    bool onUnownedSurface() const
    {
        const PickSamples& s = ref_.samples;
        for (std::size_t i = 0; i < then_.size(); ++i)
            if (s.carriers[i].owner == -1 && analytic(then_[i]))
                return true;
        return false;
    }

    // Is one of carriers [from, to) (end carriers) moved off itself by its feature's move?
    bool movedOff(std::size_t from, std::size_t to) const
    {
        const PickSamples& s = ref_.samples;
        const std::size_t own = std::min<std::size_t>(s.ownCarriers, s.carriers.size());
        for (std::size_t i = from; i < to; ++i) {
            // (Only a face its feature owns: a piece split off by a feature may lie on another's surface, which bounds
            // the edge where it was: T-081a random 1:0 edge 31.)
            const SampleCarrier& c = s.carriers[i];
            const std::int32_t index = c.owner;
            if (index < 0 || static_cast<std::size_t>(index) >= s.placements.size())
                continue;
            const FeaturePlacement& then = s.placements[static_cast<std::size_t>(index)];
            const FeaturePlacement* now = map_.placement(then.feature);
            if (now == nullptr || samePlacement(then, *now))
                continue;
            const Surface& was = ends_[i - own];
            if (!analytic(was))
                return true;
            const Rigid a = rigidOf(then), b = rigidOf(*now);
            Surface moved = was;
            moved.o = carry(a, b, was.o);
            moved.a = turn(a, b, was.a);
            if (!sameSurface(moved, was, tol_))
                return true;
        }
        return false;
    }

    // Are the samples slid (mapSample): a crossing of a moved surface with one that stayed?
    bool slides() const
    {
        return moved_ && std::find(carried_.begin(), carried_.end(), true) != carried_.end() &&
               std::find(carried_.begin(), carried_.end(), false) != carried_.end();
    }

    // Did every surface that moved move along itself (a plane within its plane, a cylinder about or along its axis)?
    bool movedWithinItself() const
    {
        if (!moved_)
            return false;
        for (std::size_t i = 0; i < now_.size(); ++i)
            if (carried_[i] && !(analytic(then_[i]) && sameSurface(now_[i], then_[i], tol_)))
                return false;
        return true;
    }

    bool any() const { return !picked_.empty(); }
    // Is the move of the pick's own surfaces known (moveOf)?
    bool known() const { return known_; }
    // Did a feature that split off one of the pick's surfaces move (moveOf)?
    bool splitterMoved() const { return splitterMoved_; }
    // How many mapped samples the answer last decided held.
    std::size_t held() const { return held_; }

    Resolved decide(Resolved r) const;

private:
    // RAYCAD_NAMING_POST_TRACE (read once): one line per decision on stderr, for diagnosis.
    void trace(const char* step, const std::vector<SubId>& answer, std::size_t on, std::size_t elsewhere,
               const char* what) const
    {
        const int level = traceLevel();
        if (level == 0 || (level < 2 && std::string(step) == "ok"))
            return;
        std::string a;
        for (const SubId& e : answer)
            a += " " + std::to_string(e.index);
        std::fprintf(stderr,
                     "POST %s kind %d hint (%.4f %.4f %.4f) %.4f answer {%s } samples %zu on answer %zu elsewhere %zu "
                     "known %d moved %d: %s\n",
                     step, static_cast<int>(ref_.kind), ref_.hint.qCx * 1e-4, ref_.hint.qCy * 1e-4,
                     ref_.hint.qCz * 1e-4, ref_.hint.qArea * 1e-4, a.c_str(), mapped_.size(), on, elsewhere,
                     known_ ? 1 : 0, moved_ ? 1 : 0, what);
        if (level >= 3) {
            for (std::size_t i = 0; i < now_.size(); ++i)
                std::fprintf(stderr,
                             "POST   carrier %zu type %d carried %d o (%.4f %.4f %.4f) a (%.6f %.6f %.6f) r %.4f\n", i,
                             static_cast<int>(now_[i].type), carried_[i] ? 1 : 0, now_[i].o.x, now_[i].o.y, now_[i].o.z,
                             now_[i].a.x, now_[i].a.y, now_[i].a.z, now_[i].r);
            for (std::size_t i = 0; i < ends_.size(); ++i)
                std::fprintf(stderr,
                             "POST   end carrier %zu%s type %d owner %d o (%.4f %.4f %.4f) a (%.6f %.6f %.6f)\n", i,
                             i < ref_.samples.firstEndCarriers ? " (first end)" : "", static_cast<int>(ends_[i].type),
                             ref_.samples.carriers[ref_.samples.ownCarriers + i].owner, ends_[i].o.x, ends_[i].o.y,
                             ends_[i].o.z, ends_[i].a.x, ends_[i].a.y, ends_[i].a.z);
            for (std::size_t i = 0; i < mapped_.size(); ++i)
                std::fprintf(stderr, "POST   sample %zu (%.4f %.4f %.4f) -> (%.4f %.4f %.4f)%s\n", i, picked_[i].p.x,
                             picked_[i].p.y, picked_[i].p.z, mapped_[i].p.x, mapped_[i].p.y, mapped_[i].p.z,
                             mapped_[i].end ? " end" : "");
        }
    }

    // The move of the pick's surfaces (postcondition.h).
    void moveOf()
    {
        carried_.assign(then_.size(), false);
        const PickSamples& s = ref_.samples;
        if (!s.known())
            return;
        bool owned = false;
        std::vector<std::pair<Rigid, Rigid>> moves;
        for (std::size_t i = 0; i < then_.size(); ++i) {
            const SampleCarrier& c = s.carriers[i];
            const bool split = c.owner <= -2;
            const std::int32_t index = split ? -2 - c.owner : c.owner;
            if (index < 0 || static_cast<std::size_t>(index) >= s.placements.size())
                continue;
            if (split) {
                const FeaturePlacement* now = map_.placement(s.placements[static_cast<std::size_t>(index)].feature);
                splitterMoved_ =
                    splitterMoved_ ||
                    (now != nullptr && !samePlacement(s.placements[static_cast<std::size_t>(index)], *now));
                if (!carrySplit_)
                    continue; // taken to stay where it was (and its move not known)
            }
            owned = true;
            const FeaturePlacement& then = s.placements[static_cast<std::size_t>(index)];
            const FeaturePlacement* now = map_.placement(then.feature);
            if (now == nullptr) {
                carried_.assign(then_.size(), false);
                return; // the owner is gone: its move is not known
            }
            if (samePlacement(then, *now))
                continue;
            carried_[i] = true;
            moves.emplace_back(rigidOf(then), rigidOf(*now));
        }
        if (!owned)
            return;
        for (std::size_t i = 1; i < moves.size(); ++i)
            for (const Sample& x : picked_)
                if (norm(sub(carry(moves[0].first, moves[0].second, x.p),
                             carry(moves[i].first, moves[i].second, x.p))) > 1e-3) {
                    carried_.assign(then_.size(), false);
                    return; // moved differently: not known
                }
        known_ = true;
        if (!moves.empty()) {
            moved_ = true;
            from_ = moves[0].first;
            to_ = moves[0].second;
        }
    }

    Sample mapSample(const Sample& s) const
    {
        if (!moved_)
            return s;
        Sample out = s;
        out.p = carry(from_, to_, s.p);
        out.into = turn(from_, to_, s.into);
        const bool stays = std::find(carried_.begin(), carried_.end(), false) != carried_.end();
        if (ref_.kind == SubKind::Face || !stays)
            return out;
        // (noSlide: the crossing nearest the point where it was picked, on the narrow face that stayed.)
        if (noSlide_)
            out.p = s.p;
        // A crossing of a moved surface with one that stayed: slide onto the one that stayed along the moved feature's
        // direction (or within the moved surface towards it), then settle onto all of them.
        const Vec3 z{to_.r[2], to_.r[5], to_.r[8]};
        for (std::size_t i = 0; i < now_.size() && !noSlide_; ++i) {
            if (carried_[i] || !analytic(now_[i]))
                continue;
            const Vec3 n = normalAt(now_[i], out.p);
            Vec3 u = z;
            if (std::abs(dot(u, n)) < 0.2) {
                bool found = false;
                for (std::size_t j = 0; j < now_.size() && !found; ++j) {
                    if (!carried_[j] || !analytic(now_[j]))
                        continue;
                    const Vec3 w = normalAt(now_[j], out.p);
                    if (const auto t = unit(sub(n, mul(w, dot(n, w))))) {
                        u = *t;
                        found = true;
                    }
                }
                if (!found)
                    continue;
            }
            out.p = slide(now_[i], out.p, u);
        }
        out.p = settle(out.p);
        if (s.end)
            if (const auto t = tangentAt(out.p))
                out.into = dot(*t, out.into) < 0 ? mul(*t, -1.0) : *t;
        return out;
    }

    // p moved onto every mapped analytic surface at once (where they meet): Gauss-Newton steps of least length, each
    // solving the surfaces' offsets linearised at p (planes exactly; a cylinder by its radial offset), up to 3
    // surfaces. Alternating projections converge too slowly where the surfaces meet at a shallow angle.
    Vec3 settle(Vec3 p) const
    {
        for (int iter = 0; iter < 20; ++iter) {
            std::array<Vec3, 3> n{};
            std::array<double, 3> f{};
            std::size_t k = 0;
            double worst = 0;
            for (const Surface& s : now_) {
                if (!analytic(s) || k == 3)
                    continue;
                if (s.type == Type::Plane) {
                    n[k] = s.a;
                    f[k] = dot(sub(p, s.o), s.a);
                } else {
                    const Vec3 radial = radialOf(s, p);
                    const auto u = unit(radial);
                    if (!u)
                        continue;
                    n[k] = *u;
                    f[k] = norm(radial) - s.r;
                }
                worst = std::max(worst, std::abs(f[k]));
                ++k;
            }
            if (k == 0 || worst < 1e-10)
                return p;
            // G lambda = f, G[i][j] = n_i . n_j; the step is sum lambda_j n_j.
            std::array<std::array<double, 4>, 3> m{};
            for (std::size_t i = 0; i < k; ++i) {
                for (std::size_t j = 0; j < k; ++j)
                    m[i][j] = dot(n[i], n[j]);
                m[i][3] = f[i];
            }
            bool singular = false;
            for (std::size_t c = 0; c < k && !singular; ++c) {
                std::size_t pivot = c;
                for (std::size_t r = c + 1; r < k; ++r)
                    if (std::abs(m[r][c]) > std::abs(m[pivot][c]))
                        pivot = r;
                if (std::abs(m[pivot][c]) < 1e-12) {
                    singular = true;
                    break;
                }
                std::swap(m[c], m[pivot]);
                for (std::size_t r = 0; r < k; ++r) {
                    if (r == c)
                        continue;
                    const double q = m[r][c] / m[c][c];
                    for (std::size_t j = c; j < 4; ++j)
                        m[r][j] -= q * m[c][j];
                }
            }
            if (singular) {
                // Surfaces parallel there: one at a time.
                for (const Surface& s : now_)
                    if (analytic(s))
                        p = onto(s, p);
                continue;
            }
            Vec3 step{};
            for (std::size_t i = 0; i < k; ++i)
                step = add(step, mul(n[i], m[i][3] / m[i][i]));
            p = sub(p, step);
        }
        return p;
    }

    // The direction of an edge's mapped crossing at p (two analytic surfaces), if known.
    std::optional<Vec3> tangentAt(const Vec3& p) const
    {
        if (ref_.kind != SubKind::Edge)
            return std::nullopt;
        std::vector<Vec3> normals;
        for (const Surface& s : now_)
            if (analytic(s))
                normals.push_back(normalAt(s, p));
        if (normals.size() != 2)
            return std::nullopt;
        return unit(cross(normals[0], normals[1]));
    }

    bool inBox(SubId e, const Vec3& p, double tol) const
    {
        const std::optional<Box3>& b = t_.box(e);
        if (!b)
            return true;
        return p.x >= b->min.x - tol && p.x <= b->max.x + tol && p.y >= b->min.y - tol && p.y <= b->max.y + tol &&
               p.z >= b->min.z - tol && p.z <= b->max.z + tol;
    }

    // Does element e hold mapped sample s on the pick's mapped surfaces (postcondition.h)?
    bool holds(SubId e, const Sample& s, double within = -1) const
    {
        const double tol = within >= 0 ? within : tol_;
        const double reach = s.end ? endStep() : 0.0;
        if (!inBox(e, s.p, tol + reach))
            return false;
        if (ref_.kind == SubKind::Face) {
            // The surface first (no kernel call), then the face.
            const std::optional<kernel::SurfaceInfo>& info = t_.surface(e);
            if (!info)
                return false;
            if (!now_.empty() && analytic(now_.front()) ? !sameSurface(faceSurfaceOf(t_, e), now_.front(), tol)
                                                        : info->type != ref_.hint.surface)
                return false;
        }
        const Probe& probe = t_.probe(e);
        if (!probe.on(s.p, tol))
            return false;
        switch (ref_.kind) {
        case SubKind::Face:
            return true;
        case SubKind::Edge: {
            const auto t = s.end ? std::nullopt : tangentAt(s.p);
            const std::optional<kernel::CurveInfo>& c = t_.curve(e);
            if (s.end) {
                // It runs from there the way the pick did.
                const double step = endStep();
                if (step >= 3 * tol && !probe.on(add(s.p, mul(s.into, step)), std::max(tol, 0.3 * step)))
                    return false;
            } else if (t && c) {
                std::optional<Vec3> d;
                if (c->type == kernel::CurveType::Line)
                    d = unit(sub(c->end, c->start));
                else if (c->type == kernel::CurveType::Circle)
                    d = unit(cross(c->axis, sub(s.p, c->center)));
                if (d && std::abs(dot(*d, *t)) < 0.999)
                    return false;
            }
            // Between the pick's surfaces as mapped: a face on each (the same plane on the same side, the same
            // cylinder), not another meeting along the same line (a cap of the moved feature on the plane, the other
            // way out).
            return carriersMet(e, tol) >= analyticCarriers();
        }
        case SubKind::Vertex: {
            // Where the pick's surfaces meet: a face on each of them, or on each but one that a face parallel to it
            // replaced at the very point (a bore opened up about its seam vertex); two vertices are never at one point,
            // but another meeting can be where the pick was (a chamfer rebuilt otherwise: T-081a random 10:5 vertex 2).
            const std::size_t n = analyticCarriers();
            return n == 0 || carriersMet(e, tol) == n || (n >= 2 && vertexMovedAlong(e));
        }
        case SubKind::Solid:
            break;
        }
        return false;
    }

    std::size_t analyticCarriers() const
    {
        return static_cast<std::size_t>(std::count_if(now_.begin(), now_.end(), analytic));
    }

    // How many of the pick's mapped analytic surfaces a face of edge or vertex e lies on.
    std::size_t carriersMet(SubId e, double tol = -1) const
    {
        tol = tol >= 0 ? tol : tol_;
        const std::vector<SubId> faces = kernel::adjacent(t_.shape(), e, SubKind::Face);
        std::size_t met = 0;
        for (const Surface& m : now_) {
            if (!analytic(m))
                continue;
            bool found = false;
            for (const SubId& f : faces)
                if (const std::optional<kernel::SurfaceInfo>& info = t_.surface(f); info && !found)
                    found = sameSurface(faceSurfaceOf(t_, f), m, tol);
            met += found ? 1 : 0;
        }
        return met;
    }

    // How far along an edge from its end the sense of the edge is tested.
    double endStep() const { return std::min(0.05, 0.25 * length_); }

    // Does a recorded neighbour's territory, where it was or mapped by its own feature's move, land on e?
    bool holdsANeighbour(SubId e) const
    {
        const Probe& probe = t_.probe(e);
        const PickSamples& s = ref_.samples;
        for (std::size_t i = 0; i < ref_.pickNeighbours.size(); ++i) {
            const Territory& n = ref_.pickNeighbours[i].territory;
            // (A neighbour of no measure, such as the degenerate edge at a sphere's pole, holds no ground.)
            if (!n.known() || (ref_.kind != SubKind::Vertex && n.qMeasure == 0))
                continue;
            const std::int32_t owner = i < s.neighbourOwners.size() ? s.neighbourOwners[i] : -1;
            std::optional<std::pair<Rigid, Rigid>> move;
            if (owner >= 0 && static_cast<std::size_t>(owner) < s.placements.size()) {
                const FeaturePlacement& then = s.placements[static_cast<std::size_t>(owner)];
                // (Its feature gone, its ground is taken where it was: a face that took it in holds that.)
                const FeaturePlacement* now = map_.placement(then.feature);
                if (now != nullptr && !samePlacement(then, *now))
                    move = std::pair{rigidOf(then), rigidOf(*now)};
            }
            // Its ground as its feature moved it, and where it was at pick time: an element now on either holds what
            // was another element's then (a face grown over the place a moved boss's cap left is that and more).
            for (const auto& q : n.points) {
                if (probe.on(fromQuanta(q), kOnTolerance))
                    return true;
                if (move && probe.on(carry(move->first, move->second, fromQuanta(q)), 5e-3))
                    return true;
            }
        }
        return false;
    }

    // Does edge m lie wholly past the pick as its feature moved it: every point of its territory, carried back by the
    // move, outside the picked box (beyond the extent tolerance)? False without a known move.
    bool pastThePick(SubId m) const
    {
        if (!moved_ || inPlace_ || ref_.kind != SubKind::Edge || !ref_.territory.known())
            return false;
        const Territory& own = t_.of(m);
        if (!own.known())
            return false;
        const Box3 was = boxOf(ref_.territory);
        const double size = std::max({was.max.x - was.min.x, was.max.y - was.min.y, was.max.z - was.min.z, 0.0});
        const double tol = 2e-3 + 5e-4 * size;
        for (const auto& q : own.points) {
            const Vec3 back = carry(to_, from_, fromQuanta(q));
            if (back.x >= was.min.x - tol && back.x <= was.max.x + tol && back.y >= was.min.y - tol &&
                back.y <= was.max.y + tol && back.z >= was.min.z - tol && back.z <= was.max.z + tol)
                return false;
        }
        return true;
    }

    // Does member m hold part of the mapped pick by its own territory (postcondition.h)?
    bool coversPart(SubId m) const
    {
        const double cover = static_cast<double>(ref_.samples.qSpacing) / kLengthQuantaPerMm;
        if (!(cover > 0) || mapped_.empty())
            return false;
        const Territory& own = t_.of(m);
        for (const auto& q : own.points) {
            const Vec3 x = fromQuanta(q);
            if (ref_.kind == SubKind::Face) {
                if (!now_.empty() && analytic(now_.front()) && offSurface(now_.front(), x) > tol_)
                    continue;
                if (ref_.outline.known() && !now_.empty() && now_.front().type == Type::Plane) {
                    const Vec3 back = moved_ && carried_.front() ? carry(to_, from_, x) : x;
                    if (insidePickOutline(ref_.outline, back))
                        return true;
                    continue;
                }
            } else if (ref_.kind == SubKind::Edge) {
                bool past = false;
                for (const Sample& s : mapped_)
                    past = past || (s.end && dot(sub(x, s.p), s.into) < -tol_);
                if (past)
                    continue;
            } else {
                return false;
            }
            for (const Sample& s : mapped_)
                if (norm(sub(x, s.p)) <= cover + tol_)
                    return true;
        }
        return false;
    }

    // Does the pick, moved onto e's own surface or curve by the shortest way, land on it (at least half of it)? All:
    // yes, and e's surface is the picked one moved along its normal (a plane parallel to it on the same side, a
    // cylinder about the same axis), or e (an edge) still lies on one of the picked edge's surfaces; Most: yes, a face
    // on another surface; None: no (an edge off every picked surface included).
    enum class Guess { None, Most, All };
    Guess guess(SubId e) const
    {
        if (ref_.kind == SubKind::Vertex)
            return vertexMovedAlong(e) ? Guess::All : Guess::None;
        std::size_t n = 0, landed = 0;
        const bool counted = countGuess(e, n, landed);
        if (traceLevel() > 0) {
            int curve = -1;
            if (ref_.kind == SubKind::Edge)
                if (const std::optional<kernel::CurveInfo>& c = t_.curve(e); c)
                    curve = static_cast<int>(c->type);
            std::fprintf(stderr, "POST   guess counted %d n %zu landed %zu parallel %d curve %d hint type %d\n",
                         counted ? 1 : 0, n, landed, parallelTo(e) ? 1 : 0, curve, static_cast<int>(ref_.hint.surface));
            if (ref_.kind == SubKind::Face && !now_.empty())
                if (const std::optional<kernel::SurfaceInfo>& info = t_.surface(e); info) {
                    const Surface f = faceSurfaceOf(t_, e);
                    std::fprintf(stderr,
                                 "POST   face type %d axis (%.6f %.6f %.6f) r %.4f in %d; pick type %d axis (%.6f %.6f "
                                 "%.6f) r %.4f in %d\n",
                                 static_cast<int>(f.type), f.a.x, f.a.y, f.a.z, f.r, f.inward ? 1 : 0,
                                 static_cast<int>(now_[0].type), now_[0].a.x, now_[0].a.y, now_[0].a.z, now_[0].r,
                                 now_[0].inward ? 1 : 0);
                }
        }
        if (!counted || n == 0 || landed == 0)
            return Guess::None;
        // An edge still on a picked surface slid along it and may have been shortened as well (a resized cut's edge on
        // the face it crosses): some of it is enough. An edge off every surface the pick lay on is not guessed at all.
        if (ref_.kind == SubKind::Edge)
            return parallelTo(e) ? Guess::All : Guess::None;
        if (2 * landed < n)
            return Guess::None;
        return parallelTo(e) ? Guess::All : Guess::Most;
    }

    // Does e lie on the pick's surfaces as mapped (a face on its surface, an edge or vertex on a face on each of its
    // analytic surfaces, a vertex on two or more)?
    bool onMappedCarriers(SubId e) const
    {
        if (ref_.kind == SubKind::Face) {
            const std::optional<kernel::SurfaceInfo>& info = t_.surface(e);
            return info && !now_.empty() && analytic(now_.front()) &&
                   sameSurface(faceSurfaceOf(t_, e), now_.front(), tol_);
        }
        const std::size_t n = analyticCarriers();
        return n >= (ref_.kind == SubKind::Vertex ? 2u : 1u) && carriersMet(e) == n;
    }

    // An answer on the pick's mapped surfaces that holds none of the mapped samples (nothing of them lands anywhere):
    // is it the pick, the part (not its feature) bounding it elsewhere on those surfaces? A face: only when its feature
    // moved and it is a rigid copy of the pick (its area and sorted extents within a hundredth: a cut's wall through
    // the part, moved along itself, holdout1 8:23); else it is another patch of the surface (a boss's side whose
    // visible patch slid as a join moved over it, holdout1 7:33; the window of a moved cap, holdout2 7:44). An edge:
    // when a point of its own lies among the mapped pick's (coversPart: a stretch of the same crossing slid over the
    // pick's place, holdout2 17:24); one beside it is another stretch (holdout1 4:33, review2 2:8). A vertex: never
    // (another meeting point).
    bool whereThePickWent(SubId e) const
    {
        if (ref_.kind == SubKind::Vertex)
            return false;
        const std::optional<Box3>& now = t_.box(e);
        if (!now)
            return false;
        if (ref_.kind == SubKind::Face)
            return moved_ && rigidCopy(e);
        // An edge: a point of its own among the mapped pick's (within the samples' spacing of one, not past its ends).
        return coversPart(e);
    }

    // Is face e a rigid copy of the pick: the same area and sorted box extents, each within a hundredth?
    bool rigidCopy(SubId e) const
    {
        const std::optional<Box3>& now = t_.box(e);
        if (!now || !ref_.territory.known() || ref_.kind != SubKind::Face)
            return false;
        const double then = static_cast<double>(ref_.hint.qArea);
        const double area = static_cast<double>(map_.fingerprintOf(e).qArea);
        if (std::abs(area - then) > 0.01 * then + kGrowthSlackQuanta)
            return false;
        const Box3 was = boxOf(ref_.territory);
        std::array<double, 3> da{was.max.x - was.min.x, was.max.y - was.min.y, was.max.z - was.min.z};
        std::array<double, 3> db{now->max.x - now->min.x, now->max.y - now->min.y, now->max.z - now->min.z};
        std::sort(da.begin(), da.end());
        std::sort(db.begin(), db.end());
        for (std::size_t i = 0; i < 3; ++i)
            if (std::abs(da[i] - db[i]) > 0.01 * std::max(da[i], db[i]) + 2e-3)
                return false;
        return true;
    }

    // Is vertex e where the pick's surfaces meet but one, moved along its normal (on a face of every analytic surface
    // of the pick, two or more, but one; and on a face parallel to that one: a plane with the same normal, a cylinder
    // about the same axis)? The corner of a pushed face, the seam vertex of a bore opened up.
    bool vertexMovedAlong(SubId e) const
    {
        const std::size_t n = analyticCarriers();
        if (n < 2)
            return false;
        const std::vector<SubId> faces = kernel::adjacent(t_.shape(), e, SubKind::Face);
        std::size_t met = 0, moved = 0;
        for (const Surface& m : now_) {
            if (!analytic(m))
                continue;
            bool same = false, parallel = false;
            for (const SubId& f : faces) {
                const std::optional<kernel::SurfaceInfo>& info = t_.surface(f);
                if (!info)
                    continue;
                const Surface s = faceSurfaceOf(t_, f);
                same = same || sameSurface(s, m, tol_);
                parallel = parallel || parallelSurface(s, m);
            }
            met += same ? 1 : 0;
            moved += !same && parallel ? 1 : 0;
        }
        return met + 1 == n && moved == 1;
    }

    // Is e's surface (curve) the picked one moved along its normal: a parallel plane, the same side out; a cylinder
    // about the same axis, the same way round; a line in the picked direction; a circle about the same axis?
    bool parallelTo(SubId e) const
    {
        if (ref_.kind == SubKind::Face) {
            const std::optional<kernel::SurfaceInfo>& info = t_.surface(e);
            if (!info || then_.empty() || !analytic(then_.front()))
                return false;
            const Surface f = faceSurfaceOf(t_, e);
            const Surface& p = now_.front();
            if (f.type != p.type)
                return false;
            return parallelSurface(f, p);
        }
        if (ref_.kind == SubKind::Edge) {
            // Where the picked edge's surfaces, or those of them that moved along their normals, meet: each analytic
            // surface of the pick has a face of the edge on it or on a parallel one (the rim of a resized bore on the
            // face it opens on, the edge of a pushed face along its side, the seam of a hole its sketch moved), and
            // each analytic face of the edge is so on a surface of the pick. A surface other than a plane or a cylinder
            // is told by name: a face that held a name of one of the picked edge's faces. Not another meeting along the
            // same line (the other wall of a moved feature, standing where its trailing wall was: the other way out,
            // not parallel).
            std::vector<Surface> faces;
            bool named = false;
            for (const SubId& f : kernel::adjacent(t_.shape(), e, SubKind::Face)) {
                const Surface fs = faceSurfaceOf(t_, f);
                if (!analytic(fs)) {
                    for (const Hash256& name : map_.namesOf(f))
                        named = named || std::find(ref_.neighbourNames.begin(), ref_.neighbourNames.end(), name) !=
                                             ref_.neighbourNames.end();
                    continue;
                }
                bool matched = false;
                for (const Surface& s : now_)
                    matched = matched || (analytic(s) && (sameSurface(fs, s, tol_) || parallelSurface(fs, s)));
                if (!matched)
                    return false;
                faces.push_back(fs);
            }
            for (const Surface& s : now_) {
                if (!analytic(s))
                    continue;
                bool matched = false;
                for (const Surface& fs : faces)
                    matched = matched || sameSurface(fs, s, tol_) || parallelSurface(fs, s);
                if (!matched)
                    return false;
            }
            return named || !faces.empty();
        }
        return false;
    }

    // Is surface f the picked surface p moved along its normal: a plane with the same normal; a cylinder about a
    // parallel axis, the same way round, overlapping the picked one across its axis (a bore resized, or moved and
    // resized by an edit of its sketch, whose placement, the centre of the box of the whole profile or of all a hole
    // feature's points, does not tell its own move: doc sketch_stability, hole)?
    bool parallelSurface(const Surface& f, const Surface& p) const
    {
        if (f.type != p.type || !analytic(p))
            return false;
        if (f.type == Type::Plane)
            return dot(f.a, p.a) >= 1.0 - 1e-5;
        return std::abs(dot(f.a, p.a)) >= 1.0 - 1e-5 && f.inward == p.inward &&
               norm(radialOf(p, f.o)) <= f.r + p.r + tol_;
    }

    // The point of edge e nearest p, and whether it lies inside e (not at one of its ends).
    std::optional<Vec3> footInside(SubId e, const Vec3& p) const
    {
        const Vec3 h{1e-6, 1e-6, 1e-6};
        auto box = kernel::makeBox(sub(p, h), add(p, h));
        if (!box)
            return std::nullopt;
        const auto d = kernel::minDistance(t_.shape(), e, *box, SubId{SubKind::Solid, 0});
        const std::optional<kernel::CurveInfo>& c = t_.curve(e);
        if (!d || !c)
            return std::nullopt;
        if (!c->closed && (norm(sub(d->p1, c->start)) <= tol_ || norm(sub(d->p1, c->end)) <= tol_))
            return std::nullopt;
        return d->p1;
    }

    bool countGuess(SubId e, std::size_t& n, std::size_t& landed) const
    {
        const Probe& probe = t_.probe(e);
        if (ref_.kind == SubKind::Face) {
            const std::optional<kernel::SurfaceInfo>& info = t_.surface(e);
            if (!info || info->type != ref_.hint.surface)
                return false;
            const Surface s = faceSurfaceOf(t_, e);
            if (!analytic(s))
                return false;
            for (const Sample& x : mapped_) {
                ++n;
                landed += probe.on(onto(s, x.p), tol_) ? 1 : 0;
            }
        } else if (ref_.kind == SubKind::Edge) {
            const std::optional<kernel::CurveInfo>& c = t_.curve(e);
            if (!c || static_cast<std::int64_t>(c->type) != static_cast<std::int64_t>(ref_.hint.surface))
                return false;
            for (const Sample& x : mapped_) {
                if (x.end)
                    continue;
                Vec3 foot = x.p;
                if (c->type == kernel::CurveType::Line) {
                    const auto d = unit(sub(c->end, c->start));
                    if (!d)
                        return false;
                    foot = add(c->start, mul(*d, dot(sub(x.p, c->start), *d)));
                } else if (c->type == kernel::CurveType::Circle) {
                    const auto a = unit(c->axis);
                    if (!a)
                        return false;
                    const Vec3 w = sub(x.p, c->center);
                    const auto radial = unit(sub(w, mul(*a, dot(w, *a))));
                    if (!radial)
                        continue;
                    foot = add(c->center, mul(*radial, c->radius));
                } else {
                    // Another curve: its nearest point, inside it.
                    ++n;
                    landed += footInside(e, x.p) ? 1 : 0;
                    continue;
                }
                ++n;
                landed += probe.on(foot, tol_) ? 1 : 0;
            }
        }
        return true;
    }

    // Has the pick, as its features moved it, left the part: every mapped sample outside the box of the shape's faces
    // (by more than the tolerance)? A dimension edit moves a feature's placement by a fraction of its size, near the
    // part; a move that takes the pick's ground out of the part leaves nothing of it anywhere.
public:
    bool imageLeftThePart() const
    {
        if (mapped_.empty())
            return false;
        bool first = true;
        Vec3 lo{}, hi{};
        const std::uint32_t faces = t_.shape().count(SubKind::Face);
        for (std::uint32_t i = 0; i < faces; ++i) {
            const std::optional<Box3>& b = t_.box(SubId{SubKind::Face, i});
            if (!b)
                return false; // unreadable: not known
            lo = first ? b->min : Vec3{std::min(lo.x, b->min.x), std::min(lo.y, b->min.y), std::min(lo.z, b->min.z)};
            hi = first ? b->max : Vec3{std::max(hi.x, b->max.x), std::max(hi.y, b->max.y), std::max(hi.z, b->max.z)};
            first = false;
        }
        if (first)
            return false;
        for (const Sample& s : mapped_)
            if (s.p.x >= lo.x - tol_ && s.p.x <= hi.x + tol_ && s.p.y >= lo.y - tol_ && s.p.y <= hi.y + tol_ &&
                s.p.z >= lo.z - tol_ && s.p.z <= hi.z + tol_)
                return false;
        return true;
    }

private:
    const ElementMap& map_;
    const TerritoryCache& t_;
    const TopoRef& ref_;
    bool inPlace_ = false, carrySplit_ = false, noSlide_ = false, splitterMoved_ = false;
    mutable std::size_t held_ = 0;
    std::vector<Sample> picked_, mapped_;
    std::vector<Surface> then_, now_, ends_; // ends_: the surfaces at an edge's end vertices, where picked
    std::vector<bool> carried_;
    bool known_ = false, moved_ = false;
    Rigid from_, to_;
    double tol_ = kOnTolerance, length_ = 0;
};

Resolved Checker::decide(Resolved r) const
{
    const SubKind kind = ref_.kind;
    std::vector<SubId> answer = r.elements;
    std::sort(answer.begin(), answer.end());
    auto inAnswer = [&](const SubId& e) {
        return std::binary_search(answer.begin(), answer.end(), e);
    };
    // Where every mapped sample lands.
    std::vector<std::size_t> held(answer.size(), 0);
    std::size_t onAnswer = 0, interior = 0;
    std::set<SubId> elsewhere;
    bool againstSense = false;
    const std::uint32_t count = t_.shape().count(kind);
    const bool interiors = std::any_of(mapped_.begin(), mapped_.end(), [](const Sample& s) { return !s.end; });
    for (const Sample& s : mapped_) {
        bool mine = false;
        for (std::size_t i = 0; i < answer.size(); ++i)
            if (holds(answer[i], s)) {
                ++held[i];
                mine = true;
            }
        if (mine) {
            ++onAnswer;
            interior += s.end ? 0 : 1;
            continue;
        }
        // A picked end on the answer, which runs from it the other way: it shares only that point with the pick.
        if (s.end)
            for (const SubId& a : answer)
                againstSense = againstSense || t_.probe(a).on(s.p, tol_);
        for (std::uint32_t i = 0; i < count; ++i) {
            const SubId e{kind, i};
            if (!inAnswer(e) && holds(e, s)) {
                elsewhere.insert(e);
                if (traceLevel() >= 3)
                    std::fprintf(stderr, "POST   (%.4f %.4f %.4f)%s held by %u\n", s.p.x, s.p.y, s.p.z,
                                 s.end ? " end" : "", i);
            }
        }
    }
    // An edge that holds only an end of the pick (none of the points along it) holds a point, not the pick: it may run
    // on from there over ground the pick never had (review2 2:8 edge 22: the rest of a rim, 0.07 mm of it over the
    // picked arc's end).
    if (kind == SubKind::Edge && interiors && interior == 0 && onAnswer > 0) {
        onAnswer = 0;
        std::fill(held.begin(), held.end(), 0);
    }
    held_ = onAnswer;
    if (againstSense && onAnswer == 0) {
        trace("P1", answer, onAnswer, elsewhere.size(), "an end, the other way");
        return brokenBecause("The element the picked edge's name leads to meets the picked edge, as its features moved "
                             "it, only at an end, and runs from there the other way.");
    }
    // P1 and P3: what lands on other elements.
    if (!elsewhere.empty()) {
        // (Faces only: an edge that runs on along the same crossing past the answer may be another picked piece's
        // continuation, where the pick's and that piece's mapped places meet, which the samples cannot tell apart:
        // flange 6:3 edge 10. An edge's fuller set is the completeness check's: completeAgainstEvidence.)
        if (onAnswer > 0 && known_ && kind == SubKind::Face) {
            std::vector<SubId> pieces = answer;
            pieces.insert(pieces.end(), elsewhere.begin(), elsewhere.end());
            std::sort(pieces.begin(), pieces.end());
            bool oneCarrier = true;
            for (const SubId& p : pieces)
                oneCarrier = oneCarrier && (p == pieces.front() || t_.sameCarrier(pieces.front(), p));
            bool fit = oneCarrier;
            for (std::size_t i = 0; i < answer.size() && fit; ++i)
                fit = held[i] > 0 || coversPart(answer[i]);
            trace("P3", answer, onAnswer, elsewhere.size(), fit ? "split" : "not one carrier");
            if (fit)
                return Resolved{ResolveState::Split, std::move(pieces), {}};
        }
        trace("P1", answer, onAnswer, elsewhere.size(), "broken");
        return brokenBecause(onAnswer > 0 ? "Part of the picked element, as its features moved it, is now another "
                                            "element; which elements hold all of it cannot be told."
                                          : "The picked element, as its features moved it, is now another element "
                                            "than the one its name leads to.");
    }
    // Every member holds part of the mapped pick.
    if (onAnswer == 0 && !inPlace_ && kind == SubKind::Face && answer.size() == 1 && movedWithinItself() &&
        whereThePickWent(answer.front())) {
        // The pick's feature moved along the pick's own surface, nothing of the pick so moved lands anywhere, and the
        // answer is a rigid copy of the pick: the part, not the feature, bounds it there (a cut's wall through the
        // part, moved along itself, stays where the part is). Where it was picked is where it is: the same checks in
        // place. (A face that is no copy of the pick, such as a cap that moved over the old place of a piece of it,
        // holdout2 4:6, is not.)
        trace("in place", answer, onAnswer, 0, "moved along itself");
        const Checker there(map_, t_, ref_, true, carrySplit_);
        Resolved out = there.decide(std::move(r));
        held_ = there.held();
        return out;
    }
    // P3, nothing landing anywhere: the pick, as its features moved it, left the part. Whatever now covers its old
    // place (its own feature's face moved over it included: the reviewer's v3 and the T-081a 4:6 class) is not the
    // pick, unless it is a rigid copy of the pick (a cut's wall through the part, the cut moved along it: the part, not
    // the feature, bounds it, as the oracle's copy exemption has it: holdout1 8:23 face 1).
    // (Faces only, as the oracle's rule: an edge's crossing slid along its line can land outside the part's box while
    // the crossing nearest where it was is still the pick: T-081a random 10:5, blend R3's edge.)
    if (onAnswer == 0 && moved_ && !inPlace_ && kind == SubKind::Face && imageLeftThePart() &&
        !(answer.size() == 1 && rigidCopy(answer.front()))) {
        trace("P3", answer, onAnswer, 0, "left the part");
        return brokenBecause("The picked element, as its features moved it, is outside the part now.");
    }
    if (onAnswer == 0 && moved_ && !inPlace_ && r.state == ResolveState::Exact && answer.size() == 1) {
        // A placement that changed is not always a move of the pick: a feature's placement is the centre of the box of
        // its profile or points, so a dimension edit of one curve, or one hole's point moved, places all of the feature
        // elsewhere (the walls and holes it did not touch included: doc sketch_stability, hole). When nothing of the
        // pick so moved lands anywhere, an Exact answer by name is checked in place too, and stands when it holds the
        // pick there.
        const Checker still(map_, t_, ref_, true, carrySplit_);
        Resolved there = still.decide(r);
        if (still.held() > 0) {
            held_ = still.held();
            trace("in place", answer, still.held(), 0, "held where picked");
            return there;
        }
    }
    if (onAnswer == 0 && moved_ && !inPlace_ && !noSlide_ && kind != SubKind::Face && slides()) {
        // A crossing slid along the moved feature's direction onto a surface that stayed can land far outside the face
        // it crosses there (a narrow chamfer strip the moved wall now cuts elsewhere along its line: T-081a random
        // 10:5, chamfer R3's edge): the crossing nearest where the pick was is tried too, and stands when the answer
        // holds it.
        const Checker nearest(map_, t_, ref_, false, carrySplit_, true);
        Resolved there = nearest.decide(r);
        if (nearest.held() > 0) {
            held_ = nearest.held();
            trace("nearest", answer, nearest.held(), 0, "the crossing nearest where the pick was");
            return there;
        }
    }
    if (onAnswer == 0) {
        // Nothing of the pick as moved lands on the answer, nor anywhere (the ground was cut away, or the pick moved
        // in a way its evidence does not say). The answer stands only on ground no other pick-time element held (P2),
        // and when it lies on the pick's surfaces as moved by a known move (P1 holds: nothing it should hold is
        // elsewhere), or when the pick moved onto its surface or curve by the shortest way lands on it, at least half
        // of it (a pushed face, a resized bore, a deeper cap: the surface moved along its normal; as it was when that
        // surface is the picked one so moved, else flagged: Merged, never Exact).
        // Not even near another element (within 0.05 mm: a mapped crossing a few hundredths off where the kernel put it
        // is still where the pick went).
        for (const Sample& s : mapped_)
            for (std::uint32_t i = 0; i < count; ++i)
                if (const SubId e{kind, i}; !inAnswer(e) && holds(e, s, 0.05)) {
                    trace("P1", answer, onAnswer, 1, "near another element");
                    return brokenBecause("The picked element, as its features moved it, lies by another element than "
                                         "the one its name leads to.");
                }
        const bool single = answer.size() == 1;
        const bool onCarriers = single && known_ && onMappedCarriers(answer.front());
        if (onCarriers && !whereThePickWent(answer.front())) {
            trace("guess", answer, onAnswer, 0, "on the mapped surfaces, elsewhere");
            return brokenBecause(kind == SubKind::Face
                                     ? "The picked face's place on its surface is gone, and the face its name leads to "
                                       "is another patch of that surface."
                                     : "The element the picked element's name leads to is another stretch (or meeting "
                                       "point) of the surfaces it lay on, holding none of the pick as its features "
                                       "moved it.");
        }
        Guess g = Guess::None;
        if (single && !onCarriers) {
            g = guess(answer.front());
        } else if (!single && kind != SubKind::Vertex) {
            // Pieces: each on the picked surface moved along its normal (or an edge still on a picked surface) and
            // holding some of the pick so moved onto it, together at least half of it.
            std::size_t total = 0, n = 0;
            g = Guess::All;
            for (const SubId& a : answer) {
                std::size_t na = 0, landed = 0;
                if (holdsANeighbour(a) || !countGuess(a, na, landed) || landed == 0 || !parallelTo(a))
                    g = Guess::None;
                n = std::max(n, na);
                total += landed;
            }
            if (n == 0 || 2 * total < n)
                g = Guess::None;
        }
        // (c) A face that is a rigid copy of the pick (its area and sorted extents kept) moved as a whole, turned or
        // not: a body moved onto a mate (doc transform_body), which moves every face of it and places none.
        if (g == Guess::None && !onCarriers && single && kind == SubKind::Face && rigidCopy(answer.front()))
            g = Guess::All;
        trace("guess", answer, onAnswer, 0,
              onCarriers         ? "on the mapped surfaces"
              : g == Guess::All  ? "all"
              : g == Guess::Most ? "most"
                                 : "none");
        if ((!onCarriers && g == Guess::None) || holdsANeighbour(answer.front()))
            return brokenBecause("Nothing of the picked element, as its features moved it, lies on the element its "
                                 "name leads to; whether that element is the one picked cannot be told.");
        if (!onCarriers && g == Guess::Most && r.state == ResolveState::Exact)
            return Resolved{ResolveState::Merged, std::move(r.elements), {}};
        return r;
    }
    std::vector<SubId> holding;
    bool past = true;
    for (std::size_t i = 0; i < answer.size(); ++i)
        if (held[i] > 0 || coversPart(answer[i]))
            holding.push_back(answer[i]);
        else
            past = past && pastThePick(answer[i]);
    if (holding.size() < answer.size()) {
        // An edge piece wholly past the pick as its feature moved it (another element's ground, or new ground past the
        // pick's old end: neither is ever part of the pick; T-081b round 4, review holdout 1:39 edge 83) is left out,
        // and the pieces that hold the moved pick are answered, flagged (Split, however many are left). Any other
        // member that holds none of the pick makes the answer Broken: a piece the samples miss may still be the pick's
        // (two incomplete Splits when such pieces were left out: directedit 11:40 face 14, 19:35 face 39).
        trace("member", answer, onAnswer, 0, past ? "a member past the moved pick" : "a member holds none");
        if (kind != SubKind::Edge || r.state != ResolveState::Split || holding.empty() || !past)
            return brokenBecause("The picked element came apart, and an element of the answer holds none of it as "
                                 "its features moved it.");
        return Resolved{ResolveState::Split, std::move(holding), {}};
    }
    // P2: another pick-time element's ground on an Exact answer.
    if (r.state == ResolveState::Exact && holdsANeighbour(answer.front())) {
        trace("P2", answer, onAnswer, 0, "merged");
        if (kind == SubKind::Vertex)
            return brokenBecause("The picked vertex now lies where another vertex was when it was picked.");
        return Resolved{ResolveState::Merged, std::move(r.elements), {}};
    }
    trace("ok", answer, onAnswer, 0, r.state == ResolveState::Exact ? "exact" : "flagged");
    return r;
}

} // namespace

PickSamples pickSamplesOf(const ElementMap& map, const TerritoryCache& territories, SubId sub,
                          const std::vector<PickNeighbour>& neighbours)
{
    PickSamples out;
    try {
        const kernel::Shape& shape = territories.shape();
        std::vector<SubId> faces;
        if (sub.kind == SubKind::Face) {
            double cover = 0;
            for (const Vec3& p : facePointsOf(shape, sub, kMaxPickSamples, cover))
                out.points.push_back(toQuanta(p));
            out.qSpacing = out.points.empty() ? 0 : quantise(cover, kLengthQuantaPerMm);
            faces.push_back(sub);
        } else if (sub.kind == SubKind::Edge) {
            const EdgePoints e = edgePointsOf(shape, sub);
            if (e.known) {
                if (!e.closed) {
                    out.points.push_back(toQuanta(e.start));
                    out.points.push_back(toQuanta(e.end));
                    const auto a = axisQuanta(e.intoFromStart), b = axisQuanta(e.intoFromEnd);
                    out.ends = {a[0], a[1], a[2], b[0], b[1], b[2]};
                }
                for (const Vec3& p : e.along)
                    out.points.push_back(toQuanta(p));
                out.qSpacing = quantise(e.cover, kLengthQuantaPerMm);
            }
            faces = kernel::adjacent(shape, sub, SubKind::Face);
        } else if (sub.kind == SubKind::Vertex) {
            if (const auto p = kernel::vertexPoint(shape, sub))
                out.points.push_back(toQuanta(*p));
            faces = kernel::adjacent(shape, sub, SubKind::Face);
        }
        // Without points of its own (another surface or curve): its territory points stand in.
        if (out.points.empty())
            for (const auto& q : territories.of(sub).points)
                if (out.points.size() < kMaxPickSamples)
                    out.points.push_back(q);
        if (out.points.empty())
            return PickSamples{};
        auto carrierFor = [&](const SubId& f) {
            const std::optional<kernel::SurfaceInfo>& info = territories.surface(f);
            SampleCarrier c = info ? carrierOf(faceSurfaceOf(territories, f)) : SampleCarrier{};
            bool unknownSplit = false;
            c.owner = ownerIndex(map, out.placements, map.namesOf(f), &unknownSplit);
            if (c.owner < 0 && unknownSplit)
                c.owner = splitOwnerIndex(map, out.placements, map.namesOf(f));
            if (traceLevel() >= 4)
                for (const Hash256& n : map.namesOf(f))
                    if (const TopoName* r = map.record(n))
                        std::fprintf(stderr, "POST pick face %u name %s role %s origin %s placed %d -> owner %d\n",
                                     f.index, n.hex().substr(0, 6).c_str(), r->role.c_str(),
                                     r->origin.value.toString().substr(0, 8).c_str(),
                                     map.placement(r->origin) != nullptr ? 1 : 0, c.owner);
            return c;
        };
        for (const SubId& f : faces) {
            if (out.carriers.size() >= kMaxSampleCarriers)
                break;
            out.carriers.push_back(carrierFor(f));
        }
        out.ownCarriers = static_cast<std::uint32_t>(out.carriers.size());
        // An edge's ends: the other faces at its vertices, what bounds it (PickSamples::ownCarriers); not one tangent
        // to the edge's own faces there (an arc wall running on from a flat one bounds nothing).
        if (sub.kind == SubKind::Edge) {
            const std::vector<SubId> vertices = kernel::adjacent(shape, sub, SubKind::Vertex);
            for (std::size_t k = 0; k < vertices.size() && k < 2; ++k) {
                const auto at = kernel::vertexPoint(shape, vertices[k]);
                if (!at)
                    continue;
                std::vector<Vec3> ownNormals;
                for (const SubId& f : faces)
                    if (const Surface s = faceSurfaceOf(territories, f); analytic(s))
                        ownNormals.push_back(normalAt(s, *at));
                for (const SubId& f : kernel::adjacent(shape, vertices[k], SubKind::Face)) {
                    if (std::find(faces.begin(), faces.end(), f) != faces.end() ||
                        out.carriers.size() >= kMaxSampleCarriers)
                        continue;
                    bool tangent = false;
                    if (const Surface s = faceSurfaceOf(territories, f); analytic(s))
                        for (const Vec3& n : ownNormals)
                            tangent = tangent || std::abs(dot(normalAt(s, *at), n)) >= 1.0 - 1e-6;
                    if (!tangent)
                        out.carriers.push_back(carrierFor(f));
                }
                if (k == 0)
                    out.firstEndCarriers = static_cast<std::uint32_t>(out.carriers.size()) - out.ownCarriers;
            }
        }
        for (const PickNeighbour& n : neighbours)
            out.neighbourOwners.push_back(ownerIndex(map, out.placements, n.names));
        if (std::all_of(out.neighbourOwners.begin(), out.neighbourOwners.end(), [](std::int32_t o) { return o < 0; }))
            out.neighbourOwners.clear();
        // Sorted by feature, the owners' indexes with them.
        std::vector<std::size_t> order(out.placements.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) { return out.placements[a].feature < out.placements[b].feature; });
        std::vector<std::int32_t> rank(order.size());
        std::vector<FeaturePlacement> sorted;
        for (std::size_t i = 0; i < order.size(); ++i) {
            rank[order[i]] = static_cast<std::int32_t>(i);
            sorted.push_back(out.placements[order[i]]);
        }
        out.placements = std::move(sorted);
        for (SampleCarrier& c : out.carriers)
            if (c.owner >= 0)
                c.owner = rank[static_cast<std::size_t>(c.owner)];
            else if (c.owner <= -2)
                c.owner = -2 - rank[static_cast<std::size_t>(-2 - c.owner)];
        for (std::int32_t& o : out.neighbourOwners)
            if (o >= 0)
                o = rank[static_cast<std::size_t>(o)];
        out.version = kPickSamplesVersion;
    } catch (...) {
        return PickSamples{};
    }
    return out;
}

namespace {
// The move of the pick's own surfaces is not known (Checker::known: an imported face, a piece split off by what the map
// keeps no record of), yet a feature whose placement the reference recorded (an owner of a surface at its ends, of a
// neighbour, the feature that split it) moved, and that move takes every sample of the pick out of the part (the box
// of the shape's faces): whether the pick went with it cannot be told, so an answer over its old place is not certain
// (T-081b round 7: random 14:11 face 24, a piece of a boss's cap whose split parents the map no longer records, the
// boss moved by its width out of the part, its cap still over the piece's old place).
bool leftWithAMovedFeature(const ElementMap& map, const TerritoryCache& t, const TopoRef& ref)
{
    const PickSamples& s = ref.samples;
    const std::vector<Sample> picked = samplesOf(ref);
    if (picked.empty())
        return false;
    bool first = true;
    Vec3 lo{}, hi{};
    const std::uint32_t faces = t.shape().count(SubKind::Face);
    for (std::uint32_t i = 0; i < faces; ++i) {
        const std::optional<Box3>& b = t.box(SubId{SubKind::Face, i});
        if (!b)
            return false;
        lo = first ? b->min : Vec3{std::min(lo.x, b->min.x), std::min(lo.y, b->min.y), std::min(lo.z, b->min.z)};
        hi = first ? b->max : Vec3{std::max(hi.x, b->max.x), std::max(hi.y, b->max.y), std::max(hi.z, b->max.z)};
        first = false;
    }
    if (first)
        return false;
    for (const FeaturePlacement& then : s.placements) {
        const FeaturePlacement* now = map.placement(then.feature);
        if (now == nullptr || samePlacement(then, *now))
            continue;
        const Rigid a = rigidOf(then), b = rigidOf(*now);
        bool outside = true;
        for (const Sample& x : picked) {
            const Vec3 q = carry(a, b, x.p);
            if (q.x >= lo.x - kOnTolerance && q.x <= hi.x + kOnTolerance && q.y >= lo.y - kOnTolerance &&
                q.y <= hi.y + kOnTolerance && q.z >= lo.z - kOnTolerance && q.z <= hi.z + kOnTolerance) {
                outside = false;
                break;
            }
        }
        if (outside)
            return true;
    }
    return false;
}
} // namespace

Resolved postcondition(const ElementMap& map, const TerritoryCache& territories, const TopoRef& ref, Resolved r)
{
    try {
        if (!testing::postconditionEnabled() || r.elements.empty())
            return r;
        if (r.state != ResolveState::Exact && r.state != ResolveState::Split && r.state != ResolveState::Merged)
            return r;
        for (const SubId& e : r.elements)
            if (e.kind != ref.kind)
                return r;
        const Checker check(map, territories, ref);
        if (!check.any())
            return r; // no evidence: as before
        if (!check.splitterMoved()) {
            Resolved out = check.decide(std::move(r));
            if (ref.kind == SubKind::Face && !check.known() && check.held() > 0 && out.state != ResolveState::Broken &&
                leftWithAMovedFeature(map, territories, ref))
                return brokenBecause(
                    "A feature the picked element was recorded with moved, and the picked element may have "
                    "moved with it, out of the part; whether the element now over its old place is the one "
                    "picked cannot be told.");
            return out;
        }
        // A surface of the pick is a piece split off by a feature that has moved since, from a face whose owner is not
        // known (splitOwnerIndex: the map keeps no record of what it was split from): whether that feature carried it
        // cannot be told. A Merged answer (the names say the pick is now part of another element) is checked as moved
        // with that feature (T-081a holdout2 4:6 face 31: a moved cut's ceiling piece whose image left the part, the
        // ceiling moved over its old place); any other as staying where it was, and as moved when it holds none of it
        // there.
        const Checker carried(map, territories, ref, false, true);
        if (r.state == ResolveState::Merged) {
            // (Unless the pick so moved still lands on the answer, and where it was picked it lands there too: then it
            // stayed, a piece of a face split twice, review1 14:46 face 39.)
            Resolved moved = carried.decide(r);
            if (carried.held() == 0)
                return moved;
            Resolved stayed = check.decide(std::move(r));
            return check.held() > 0 ? stayed : moved;
        }
        Resolved inPlace = check.decide(r);
        if (check.held() > 0)
            return inPlace;
        return carried.decide(std::move(r));
    } catch (const std::exception& e) {
        if (traceLevel() > 0)
            std::fprintf(stderr, "POST exception: %s\n", e.what());
        return brokenBecause("The reference could not be checked because of an internal failure.");
    } catch (...) {
        if (traceLevel() > 0)
            std::fprintf(stderr, "POST exception (not std)\n");
        return brokenBecause("The reference could not be checked because of an internal failure.");
    }
}

} // namespace detail
} // namespace ray::naming
