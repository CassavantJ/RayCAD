// Linking (link.h, task T-051): leads, entries (helix, ramp, pre-drill, plunge), retracts and stay-down links, the
// material model they are checked against, and cam.h's public linkPasses. The order is made in link_order.cpp.
//
// TOLERANCES (the values are in link.h; why each):
//   kFreeTolerance 1 um   cut area is the exact sweep of the tool radius along each cut; the disc of a link is shrunk
//   by
//                         1 um before its free test, so a link along the very edge of a cut (the next ring of a pocket,
//                         the stroke beside the last) is not refused for the 0.1 um to which the region booleans
//                         flatten arcs (geom2d kOffsetSag). The cost: a link may graze uncut material by up to 1 um,
//                         ten times below a shop's finest tolerance.
//   kRoomProbe 0.5 um, kRoomGrow 1 um   a lead or helix is inside its room when a disc of 0.5 um swept along it lies in
//                         the room grown by 1 um. A lead that runs exactly along the room's edge (a pocket wall offset)
//                         passes (the 0.5 um margin is five times the flattening); one that leaves it by more than
//                         1.5 um fails.
//   kRampSlack 1e-4       ramps and helices are made 0.01 % longer than their angle needs, room for appendSloped (with
//                         the angle) to round each piece's end Z so that no piece is steeper than the angle (plain
//                         rounding was up to 1 nm steeper per piece: 1.3e-5 relative on a whole piece, more on short
//                         ones). Where many short pieces need more than that, the ramp is made longer and planned
//                         again.
//   kAreaEps 1 um^2       a boolean remainder smaller than this is a rounding sliver, not material or a way out of a
//                         room: a real overlap of 1 um over 1 mm is 1000 um^2.
//   kMinLinkMove 1 um     a post writes metric coordinates to 1 um, so a shorter link move comes out as no move or as a
//                         zero move; merged into its neighbour it bends that move by under 1 um, within kFreeTolerance.
//                         The same bound keeps a rotatable pass from starting on a line under 1 um from its vertex; on
//                         an arc the bound is geom2d::kMinArcChord (a shorter arc reads as a whole circle).
#include "link_internal.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <variant>

namespace ray::cam::link {

using namespace detail;

namespace {

constexpr double kAreaEps = 1e6; // nm^2

geom2d::Span lineTo(Pt end)
{
    return geom2d::Span{geom2d::SpanKind::Line, end, {}, 0};
}

D2 onCircle(Pt c, Coord r, double a)
{
    return D2{static_cast<double>(c.x) + static_cast<double>(r) * dmath::cos(a),
              static_cast<double>(c.y) + static_cast<double>(r) * dmath::sin(a)};
}

D2 startDir(Pt prev, const Move& m)
{
    if (isArcMove(m)) {
        const D2 radial = d2(prev) - d2(Pt{m.i, m.j});
        return unit(m.kind == MoveKind::ArcCCW ? leftOf(radial) : rightOf(radial));
    }
    return unit(d2(xyOf(m)) - d2(prev));
}

D2 endDir(Pt prev, const Move& m)
{
    if (isArcMove(m)) {
        const D2 radial = d2(xyOf(m)) - d2(Pt{m.i, m.j});
        return unit(m.kind == MoveKind::ArcCCW ? leftOf(radial) : rightOf(radial));
    }
    return unit(d2(xyOf(m)) - d2(prev));
}

bool movesXY(Pt prev, const Move& m)
{
    return isArcMove(m) || m.x != prev.x || m.y != prev.y;
}

std::optional<Path> planarOf(Pt3 start, const std::vector<Move>& moves)
{
    Path p;
    p.start = xyOf(start);
    Pt prev = p.start;
    for (const Move& m : moves) {
        if (m.z != start.z)
            return std::nullopt;
        if (m.kind == MoveKind::Linear) {
            if (xyOf(m) != prev)
                p.spans.push_back(lineTo(xyOf(m)));
        } else if (isArcMove(m) && m.plane == ArcPlane::XY) {
            const Pt c{m.i, m.j};
            p.spans.push_back(
                geom2d::Span{m.kind == MoveKind::ArcCCW ? geom2d::SpanKind::ArcCCW : geom2d::SpanKind::ArcCW, xyOf(m),
                             c, geom2d::roundNm(distXY(prev, c))});
        } else {
            return std::nullopt;
        }
        prev = xyOf(m);
    }
    if (p.spans.empty())
        return std::nullopt;
    p.closed = prev == p.start;
    return p;
}

Shape shapeFrom(Pt3 start, std::vector<Move> moves)
{
    Shape s;
    s.start = start;
    s.zMax = s.zMin = start.z;
    Pt prev = xyOf(start);
    for (const Move& m : moves) {
        if (movesXY(prev, m)) {
            if (!s.hasTangent) {
                s.t0 = startDir(prev, m);
                s.hasTangent = true;
            }
            s.t1 = endDir(prev, m);
        }
        s.zMax = std::max(s.zMax, m.z);
        s.zMin = std::min(s.zMin, m.z);
        prev = xyOf(m);
    }
    s.end = moves.empty() ? start : Pt3{moves.back().x, moves.back().y, moves.back().z};
    s.comp = moves.empty() ? std::uint8_t{0} : moves.front().compensation;
    s.path = planarOf(start, moves);
    s.moves = std::move(moves);
    return s;
}

Coord leadSize(const Lead& lead)
{
    return lead.kind == LeadKind::Line ? lead.length : lead.radius;
}

Vec2 vec(D2 d)
{
    return Vec2{d.x, d.y};
}

// Exact decimal text of an integer number of `scale` units, `decimals` digits, trailing zeros dropped.
std::string fixed(std::int64_t v, std::int64_t scale, int decimals)
{
    const bool negative = v < 0;
    const std::uint64_t a = negative ? static_cast<std::uint64_t>(-(v + 1)) + 1 : static_cast<std::uint64_t>(v);
    std::string out = (negative ? "-" : "") + std::to_string(a / static_cast<std::uint64_t>(scale));
    std::string frac = std::to_string(a % static_cast<std::uint64_t>(scale));
    frac.insert(0, static_cast<std::size_t>(decimals) - frac.size(), '0');
    while (!frac.empty() && frac.back() == '0')
        frac.pop_back();
    return frac.empty() ? out : out + "." + frac;
}

std::string at2(Pt p)
{
    return "(" + mm(p.x) + ", " + mm(p.y) + ")";
}

// ---------- The material ----------
class Material {
public:
    Material(const Stock& stock, Coord toolRadius, Nm tipHeight)
        : stock_(stock), probe_(toolRadius > 2 * kFreeTolerance ? toolRadius - kFreeTolerance : toolRadius / 2),
          tip_(tipHeight)
    {
        for (const geom2d::Region& r : stock_.region) {
            const geom2d::Bounds b = geom2d::bounds(r);
            if (b.empty)
                continue;
            if (sb_.empty) {
                sb_ = b;
            } else {
                sb_.min = Pt{std::min(sb_.min.x, b.min.x), std::min(sb_.min.y, b.min.y)};
                sb_.max = Pt{std::max(sb_.max.x, b.max.x), std::max(sb_.max.y, b.max.y)};
            }
        }
        for (const Cleared& c : stock_.cleared)
            addLevel(c.z);
        addLevel(stock_.top);
    }

    Coord probeRadius() const { return probe_; }
    bool noMaterial() const { return sb_.empty; }

    Regions disc(Pt at) const { return Regions{geom2d::Region{Path::circle(at, probe_, true), {}}}; }

    Result<Regions> sweep(const Path& p, JobContext& ctx) const
    {
        if (p.spans.empty())
            return disc(p.start);
        return geom2d::sweptArea(std::span<const Path>(&p, 1), probe_, ctx);
    }

    // The probe (a disc or a sweep, already shrunk) touches no material at height z and above (outside `besides`).
    Result<bool> freeAt(const Regions& probe, Nm z, JobContext& ctx, const Regions* besides = nullptr)
    {
        if (sb_.empty || z >= stock_.top)
            return true;
        if (!overlapsStock(probe))
            return true;
        auto cleared = clearedAt(z, ctx);
        if (!cleared)
            return tl::unexpected(cleared.error());
        auto rest = geom2d::subtract(probe, **cleared, ctx);
        if (!rest)
            return tl::unexpected(rest.error());
        if (geom2d::area(*rest) <= kAreaEps)
            return true;
        if (besides != nullptr) {
            auto r2 = geom2d::subtract(*rest, *besides, ctx);
            if (!r2)
                return tl::unexpected(r2.error());
            rest = std::move(r2);
            if (geom2d::area(*rest) <= kAreaEps)
                return true;
        }
        auto inside = geom2d::intersect(*rest, stock_.region, ctx);
        if (!inside)
            return tl::unexpected(inside.error());
        return geom2d::area(*inside) <= kAreaEps;
    }

    // The lowest height >= from at which the probe is free (the stock top at worst).
    Result<Nm> lowestFree(const Regions& probe, Nm from, JobContext& ctx)
    {
        auto ok = freeAt(probe, from, ctx);
        if (!ok)
            return tl::unexpected(ok.error());
        if (*ok)
            return from;
        for (const Nm level : levels_) {
            if (level <= from)
                continue;
            auto f = freeAt(probe, level, ctx);
            if (!f)
                return tl::unexpected(f.error());
            if (*f)
                return level;
        }
        return std::max(from, stock_.top);
    }

    // The area swept by the tool radius along `paths`, cut at zMax: free from zMax plus the tool's tip height up
    // (link.h THE TOOL'S END).
    Status addCut(const std::vector<Path>& paths, Coord toolRadius, Nm zMax, JobContext& ctx)
    {
        if (sb_.empty || zMax + tip_ >= stock_.top)
            return {};
        auto area = swept(paths, toolRadius, ctx);
        if (!area)
            return tl::unexpected(area.error());
        addArea(std::move(*area), zMax);
        return {};
    }
    void addArea(Regions area, Nm zMax)
    {
        const Nm z = zMax + tip_;
        if (sb_.empty || z >= stock_.top)
            return;
        cuts_.push_back(Cut{std::move(area), z});
        addLevel(z);
    }
    // The area a disc of radius r covers along `paths` (a path without spans: a disc at its start).
    static Result<Regions> swept(const std::vector<Path>& paths, Coord toolRadius, JobContext& ctx)
    {
        std::vector<Path> open;
        Regions discs;
        for (const Path& p : paths) {
            if (p.spans.empty())
                discs.push_back(geom2d::Region{Path::circle(p.start, toolRadius, true), {}});
            else
                open.push_back(p);
        }
        Regions area = discs;
        if (!open.empty()) {
            auto s = geom2d::sweptArea(open, toolRadius, ctx);
            if (!s)
                return tl::unexpected(s.error());
            area.insert(area.end(), s->begin(), s->end());
        }
        return area;
    }

private:
    struct Cut {
        Regions area;
        Nm z = 0;
    };
    struct Level {
        Regions area;
        std::size_t done = 0;
        bool started = false;
    };

    void addLevel(Nm z)
    {
        if (z > stock_.top)
            return;
        const auto it = std::lower_bound(levels_.begin(), levels_.end(), z);
        if (it == levels_.end() || *it != z)
            levels_.insert(it, z);
    }

    bool overlapsStock(const Regions& probe) const
    {
        for (const geom2d::Region& r : probe) {
            const geom2d::Bounds b = geom2d::bounds(r);
            if (!b.empty && b.max.x > sb_.min.x && b.min.x < sb_.max.x && b.max.y > sb_.min.y && b.min.y < sb_.max.y)
                return true;
        }
        return false;
    }

    // Everything free at z: the cleared areas at or below z and the cuts whose highest point is at or below z.
    Result<const Regions*> clearedAt(Nm z, JobContext& ctx)
    {
        Level& level = cache_[z];
        Regions more;
        if (!level.started) {
            level.started = true;
            for (const Cleared& c : stock_.cleared)
                if (c.z <= z)
                    more.insert(more.end(), c.area.begin(), c.area.end());
        }
        for (; level.done < cuts_.size(); ++level.done)
            if (cuts_[level.done].z <= z)
                more.insert(more.end(), cuts_[level.done].area.begin(), cuts_[level.done].area.end());
        if (!more.empty()) {
            auto u = geom2d::unite(level.area, more, ctx);
            if (!u)
                return tl::unexpected(u.error());
            level.area = std::move(*u);
        }
        return &level.area;
    }

    const Stock& stock_;
    Coord probe_;
    Nm tip_ = 0;
    geom2d::Bounds sb_;
    std::vector<Cut> cuts_;
    std::map<Nm, Level> cache_;
    std::vector<Nm> levels_; // sorted, at most the stock top
};

Result<bool> insideRoom(const Path& path, const Regions& grownRoom, JobContext& ctx)
{
    auto probe = geom2d::sweptArea(std::span<const Path>(&path, 1), kRoomProbe, ctx);
    if (!probe)
        return tl::unexpected(probe.error());
    auto rest = geom2d::subtract(*probe, grownRoom, ctx);
    if (!rest)
        return tl::unexpected(rest.error());
    return geom2d::area(*rest) <= kAreaEps;
}

// ---------- Emitting ----------
struct Emitter {
    std::vector<Move> out;
    std::vector<bool> linking; // per move: made here (a link, retract or approach), not a pass's, lead's or entry's
    Pt3 cur;

    void put(const Move& m, bool link = true)
    {
        out.push_back(m);
        linking.push_back(link);
        cur = Pt3{m.x, m.y, m.z};
    }
    void rapid(Pt3 p, MoveRole role)
    {
        if (!out.empty() && p == cur)
            return;
        Move m{};
        m.kind = MoveKind::Rapid;
        m.role = role;
        m.x = p.x;
        m.y = p.y;
        m.z = p.z;
        put(m);
    }
    void feed(Pt3 p, MoveRole role, std::int32_t f, std::uint8_t comp = 0)
    {
        if (p == cur)
            return;
        Move m{};
        m.kind = MoveKind::Linear;
        m.role = role;
        m.feedMicronPerMin = f;
        m.compensation = comp;
        m.x = p.x;
        m.y = p.y;
        m.z = p.z;
        put(m);
    }
    void append(const std::vector<Move>& ms)
    {
        for (const Move& m : ms)
            put(m, false);
    }

    // link.h SHORT MOVES: every link, retract or approach move (straight or rapid) shorter than kMinLinkMove dropped
    // before a straight move (a short rapid: or a rapid), which then starts where it started, or else merged into the
    // link move before it. A feed is never taken over by a rapid. moves[0] (the approach to Linked::start) stays.
    // `passMove` indices follow (they point at moves that are never merged).
    void mergeShortMoves(std::vector<std::size_t>& passMove)
    {
        const auto mergeable = [](const Move& m) {
            return (m.kind == MoveKind::Linear || m.kind == MoveKind::Rapid) &&
                   (m.role == MoveRole::Link || m.role == MoveRole::Retract || m.role == MoveRole::Approach);
        };
        const auto at = [](const Move& m) {
            return Pt3{m.x, m.y, m.z};
        };
        const auto length = [](Pt3 a, Pt3 b) {
            return dmath::hypot(dmath::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y)),
                                static_cast<double>(b.z - a.z));
        };
        std::vector<Move> kept;
        std::vector<bool> keptLinking;
        std::vector<Pt3> from; // where each kept move starts (kept[0]: its own end)
        std::vector<std::size_t> index(out.size(), 0);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const Move& m = out[i];
            index[i] = kept.size();
            if (i > 0 && linking[i] && mergeable(m) &&
                length(at(kept.back()), at(m)) < static_cast<double>(kMinLinkMove)) {
                // A feed is never taken over by a rapid (a rapid then could end, or start, lower than planned).
                const bool rapid = m.kind == MoveKind::Rapid;
                const MoveKind next = i + 1 < out.size() ? out[i + 1].kind : MoveKind::Dwell;
                if (next == MoveKind::Linear || (rapid && next == MoveKind::Rapid))
                    continue; // dropped: the next move starts where this one started
                if (kept.size() >= 2 && keptLinking.back() && mergeable(kept.back()) &&
                    (rapid || kept.back().kind == MoveKind::Linear)) {
                    // Into the link move before: it ends where this one ended (or vanishes if that is its start).
                    kept.back().x = m.x;
                    kept.back().y = m.y;
                    kept.back().z = m.z;
                    if (at(kept.back()) == from.back()) {
                        kept.pop_back();
                        keptLinking.pop_back();
                        from.pop_back();
                    }
                    continue;
                }
            }
            from.push_back(kept.empty() ? at(m) : at(kept.back()));
            kept.push_back(m);
            keptLinking.push_back(linking[i]);
        }
        for (std::size_t& k : passMove)
            k = index[k];
        out = std::move(kept);
        linking = std::move(keptLinking);
        if (!out.empty())
            cur = at(out.back());
    }
};

// ---------- Entries ----------
struct Plan {
    Entered kind = Entered::Free;
    Pt start;                // where the tool descends
    Nm level = 0;            // the column over the entry's footprint is free from here up
    Nm top = 0;              // the entry moves start at (start, top)
    std::vector<Move> moves; // from (start, top) to the pass's approach point at its depth
    Coord helixDiameter = 0;
    std::optional<Path> flat; // the helix's flat circle at depth (cut area)
};

struct EntryContext {
    const Options& opts;
    const Pass& pass;
    const Shape& shape;
    std::size_t number; // 1-based, for messages
    Pt3 a;              // where the pass (its lead-in) starts, at its depth
    D2 tangent;         // direction of travel there
    bool hasTangent = false;
    const std::optional<Path>& entryPath; // lead-in + planar pass, or the planar pass, for ramps
    bool ring = false;                    // the entry path is the closed pass itself
    const Regions* room = nullptr;        // grown by kRoomGrow
    Nm freeLevel = 0;                     // the disc at `a` is free from here up
    std::int32_t rampFeed = 0, plungeFeed = 0;
    Material& material;
    JobContext& ctx;

    Nm topFor(Nm level) const { return std::min(std::max(opts.feedPlane, level), level + opts.entryClearance); }
};

using Planned = std::variant<Plan, std::string, Error>;

// "in the control" or "for wear" (Options::compensation), as the refusals name the compensation a pass has on.
const char* compensationWords(const Options& o)
{
    return o.compensation == Options::Compensation::Wear ? "for wear" : "in the control";
}

Planned planHelix(EntryContext& e)
{
    const Options& o = e.opts;
    if (e.shape.comp != 0)
        return std::string("cutter compensation ") + compensationWords(o) + " is on for this pass";
    if (o.helixDiameter <= 0)
        return std::string("no helix diameter is set");
    if (!(o.rampAngleDeg > 0))
        return std::string("no ramp angle is set");
    if (e.room == nullptr)
        return std::string("no room (where the tool centre may go) is given for the pass");
    if (!e.hasTangent)
        return std::string("the pass has no direction at its start");
    const Coord dMin = o.helixMinDiameter > 0 ? o.helixMinDiameter : o.helixDiameter;
    std::vector<Coord> diameters;
    for (double d = static_cast<double>(o.helixDiameter); diameters.size() < 16; d *= 0.75) {
        const Coord di = geom2d::roundNm(d);
        if (di <= dMin)
            break;
        diameters.push_back(di);
    }
    diameters.push_back(dMin);
    const double tanA = dmath::tan(o.rampAngleDeg * kPi / 180.0);
    const bool firstCcw = o.spindleClockwise;
    const Pt a = xyOf(e.a);
    for (const Coord d : diameters) {
        // Rounded up, and at the minimum 2 nm more: the centre and the points on the circle are rounded to the
        // nanometre, and no point may lie nearer the centre than half the minimum diameter.
        const Coord r = std::max<Coord>((d + 1) / 2, (dMin + 1) / 2 + 2);
        if (r < 1000)
            continue;
        for (const bool ccw : {firstCcw, !firstCcw}) {
            const D2 n = ccw ? leftOf(e.tangent) : rightOf(e.tangent);
            const Pt c = roundPt(d2(a) + n * static_cast<double>(r));
            const Coord rr = geom2d::roundNm(distXY(a, c));
            const geom2d::SpanKind kind = ccw ? geom2d::SpanKind::ArcCCW : geom2d::SpanKind::ArcCW;
            Path circle;
            circle.start = a;
            circle.spans.push_back(geom2d::Span{kind, a, c, rr});
            circle.closed = true;
            auto in = insideRoom(circle, *e.room, e.ctx);
            if (!in)
                return in.error();
            if (!*in)
                continue;
            auto probe = e.material.sweep(circle, e.ctx);
            if (!probe)
                return probe.error();
            auto level = e.material.lowestFree(*probe, e.freeLevel, e.ctx);
            if (!level)
                return level.error();
            Plan p;
            p.kind = Entered::Helix;
            p.level = *level;
            p.top = e.topFor(*level);
            const double dz = static_cast<double>(p.top - e.a.z);
            double length = dz / tanA * (1.0 + kRampSlack);
            Path helix;
            for (int attempt = 0;; ++attempt) {
                const double theta = length / static_cast<double>(rr);
                const int parts = std::max(1, static_cast<int>(std::ceil(theta / kPi - 1e-12)));
                const double dir = ccw ? 1.0 : -1.0;
                const double aEnd = dmath::atan2(static_cast<double>(a.y - c.y), static_cast<double>(a.x - c.x));
                const double a0 = aEnd - dir * theta;
                helix = Path{};
                helix.start = roundPt(onCircle(c, rr, a0));
                for (int k = 1; k <= parts; ++k) {
                    const Pt end = k == parts ? a : roundPt(onCircle(c, rr, a0 + dir * theta * k / parts));
                    helix.spans.push_back(geom2d::Span{kind, end, c, rr});
                }
                // Never steeper than the angle (link.h appendSloped): each piece may lose under 1 nm to rounding.
                if (appendSloped(helix, p.top, e.a.z, tanA, MoveRole::Ramp, e.rampFeed, p.moves))
                    break;
                if (attempt == 7) {
                    appendSloped(helix, p.top, e.a.z, MoveRole::Ramp, e.rampFeed, p.moves);
                    break;
                }
                length += static_cast<double>(2 * parts + 2) / tanA;
            }
            p.start = helix.start;
            geom2d::appendMoves(circle, e.a.z, MoveRole::Ramp, e.rampFeed, p.moves);
            p.helixDiameter = d;
            p.flat = circle;
            return p;
        }
    }
    return "a helix of " + mm(o.helixDiameter) + " mm" +
           (dMin < o.helixDiameter ? " down to " + mm(dMin) + " mm" : std::string()) + " does not fit the room at " +
           at2(a);
}

Planned planRamp(EntryContext& e)
{
    const Options& o = e.opts;
    if (e.shape.comp != 0)
        return std::string("cutter compensation ") + compensationWords(o) + " is on for this pass";
    if (!(o.rampAngleDeg > 0))
        return std::string("no ramp angle is set");
    if (!e.entryPath)
        return std::string("the pass is not flat (a ramp runs along a flat pass)");
    const Path& ep = *e.entryPath;
    const double len = geom2d::length(ep);
    const double minRun = static_cast<double>(o.minRampRun > 0 ? o.minRampRun : 2 * o.toolRadius);
    if (len < minRun) {
        if (e.ring)
            return "the closed pass is " + mm(len) + " mm long, shorter than the ramp's minimum run of " + mm(minRun) +
                   " mm";
        return "the cut offers " + mm(len) + " mm for the ramp, which needs a run of at least " + mm(minRun) + " mm";
    }
    const double tanA = dmath::tan(o.rampAngleDeg * kPi / 180.0);
    // Extra length when the rounding of many short pieces needs it (appendSloped with the angle, below).
    double extra = 0;
    for (int attempt = 0;; ++attempt) {
        Nm level = e.freeLevel;
        double rampLength = 0, leg = 0;
        Path footprint;
        Nm top = 0;
        for (int iteration = 0; iteration < 64; ++iteration) {
            top = e.topFor(level);
            rampLength = static_cast<double>(top - e.a.z) / tanA * (1.0 + kRampSlack) + extra;
            if (e.ring) {
                footprint = rampLength >= len ? ep : geom2d::subPath(ep, len - rampLength, len);
            } else {
                leg = std::min(len, std::max(rampLength / 2, minRun));
                footprint = geom2d::subPath(ep, 0, leg);
            }
            // A ramp along an arc is a helix of that arc's diameter: never tighter than the helix minimum.
            const Coord dMin = o.helixMinDiameter > 0 ? o.helixMinDiameter : o.helixDiameter;
            if (dMin > 0) {
                for (const geom2d::Span& s : footprint.spans)
                    if (s.kind != geom2d::SpanKind::Line && 2 * s.radius < dMin + 4)
                        return "the ramp would follow an arc of " + mm(2 * s.radius) +
                               " mm diameter, below the helix minimum of " + mm(dMin) + " mm";
            }
            auto probe = e.material.sweep(footprint, e.ctx);
            if (!probe)
                return probe.error();
            auto next = e.material.lowestFree(*probe, level, e.ctx);
            if (!next)
                return next.error();
            if (*next == level)
                break;
            level = *next;
        }
        Path ramp;
        if (e.ring) {
            const double laps = std::floor(rampLength / len);
            const double rest = rampLength - laps * len;
            ramp.start = ep.start;
            if (rest > 0) {
                const Path piece = geom2d::subPath(ep, len - rest, len);
                ramp.start = piece.start;
                ramp.spans = piece.spans;
            }
            for (int k = 0; k < static_cast<int>(laps); ++k)
                ramp.spans.insert(ramp.spans.end(), ep.spans.begin(), ep.spans.end());
        } else {
            int legs = std::max(1, static_cast<int>(std::ceil(rampLength / leg - 1e-12)));
            if (legs % 2 != 0)
                ++legs;
            const Path back = geom2d::reverse(footprint);
            ramp.start = footprint.start;
            for (int k = 0; k < legs; ++k) {
                const Path& part = k % 2 == 0 ? footprint : back;
                ramp.spans.insert(ramp.spans.end(), part.spans.begin(), part.spans.end());
            }
        }
        Plan p;
        p.kind = Entered::Ramp;
        p.level = level;
        p.top = top;
        p.start = ramp.start;
        // Never steeper than the angle (link.h appendSloped): each piece may lose under 1 nm to rounding, so a ramp of
        // many short pieces is made longer by that and planned again (its footprint checked again).
        if (appendSloped(ramp, top, e.a.z, tanA, MoveRole::Ramp, e.rampFeed, p.moves))
            return p;
        if (attempt == 7) {
            appendSloped(ramp, top, e.a.z, MoveRole::Ramp, e.rampFeed, p.moves);
            return p;
        }
        std::vector<Move> pieces;
        geom2d::appendMoves(ramp, e.a.z, MoveRole::Ramp, e.rampFeed, pieces);
        extra += static_cast<double>(pieces.size() + 2) / tanA;
    }
}

Plan straightPlunge(EntryContext& e, Entered kind)
{
    Plan p;
    p.kind = kind;
    p.start = xyOf(e.a);
    p.level = e.freeLevel;
    p.top = e.topFor(e.freeLevel);
    Move m{};
    m.kind = MoveKind::Linear;
    m.role = MoveRole::Plunge;
    m.feedMicronPerMin = e.plungeFeed;
    m.x = e.a.x;
    m.y = e.a.y;
    m.z = e.a.z;
    p.moves.push_back(m);
    return p;
}

Planned planPreDrill(EntryContext& e)
{
    const Options& o = e.opts;
    if (o.preDrills.empty())
        return std::string("no pre-drilled points are given");
    const Pt a = xyOf(e.a);
    for (const PreDrill& h : o.preDrills) {
        if (h.diameter < 2 * o.toolRadius || h.bottom > e.a.z)
            continue;
        const double reach = static_cast<double>(h.diameter) / 2 - static_cast<double>(o.toolRadius);
        if (distXY(a, h.at) <= reach + static_cast<double>(kFreeTolerance))
            return straightPlunge(e, Entered::PreDrill);
    }
    return "no pre-drilled point at least the tool's diameter and down to " + mm(e.a.z) + " mm lies under " + at2(a);
}

Planned planPlunge(EntryContext& e)
{
    if (!e.opts.centreCutting)
        return std::string("the tool does not cut at its centre, so it cannot plunge");
    return straightPlunge(e, Entered::Plunge);
}

const char* entryName(EntryKind k)
{
    switch (k) {
    case EntryKind::Helix:
        return "helix";
    case EntryKind::Ramp:
        return "ramp";
    case EntryKind::PreDrill:
        return "pre-drilled point";
    case EntryKind::Plunge:
        return "plunge";
    }
    return "?";
}

Result<Plan> planEntry(EntryContext& e)
{
    const std::vector<EntryKind>& order = e.pass.entryOrder.empty() ? e.opts.entryOrder : e.pass.entryOrder;
    std::string why;
    bool helixAsked = false;
    for (const EntryKind k : order) {
        Planned r = std::string("not a known kind of entry");
        switch (k) {
        case EntryKind::Helix:
            helixAsked = true;
            r = planHelix(e);
            break;
        case EntryKind::Ramp:
            r = planRamp(e);
            break;
        case EntryKind::PreDrill:
            r = planPreDrill(e);
            break;
        case EntryKind::Plunge:
            r = planPlunge(e);
            break;
        }
        if (auto* p = std::get_if<Plan>(&r))
            return std::move(*p);
        if (auto* err = std::get_if<Error>(&r))
            return tl::unexpected(std::move(*err));
        why += std::string(why.empty() ? "" : "; ") + entryName(k) + ": " + std::get<std::string>(r);
    }
    std::string message = "Pass " + std::to_string(e.number) + " cannot be entered at " + at2(xyOf(e.a)) + ", Z " +
                          mm(e.a.z) + " mm (material from Z " + mm(e.freeLevel) + " mm down): ";
    message += order.empty() ? std::string("no entry kind is allowed") : why;
    if (helixAsked && e.opts.helixDiameter > 0) {
        const Coord dMin = e.opts.helixMinDiameter > 0 ? e.opts.helixMinDiameter : e.opts.helixDiameter;
        message += ". A helix needs a room at least " + mm(dMin + 2 * e.opts.toolRadius) + " mm wide";
    }
    return tl::unexpected(failure(message + "."));
}

// ---------- Leads ----------
Result<std::optional<Path>> fitLead(const Lead& lead, const Shape& s, bool in, const Regions* room, std::size_t no,
                                    JobContext& ctx, std::vector<std::string>& warnings)
{
    if (lead.kind == LeadKind::None)
        return std::optional<Path>{};
    const std::string what = std::string("pass ") + std::to_string(no) + ": the lead-" + (in ? "in" : "out");
    const Coord size = leadSize(lead);
    if (size < 1000)
        return tl::unexpected(invalid(std::string("The lead-") + (in ? "in" : "out") + " of pass " +
                                      std::to_string(no) + " needs a " +
                                      (lead.kind == LeadKind::Arc ? "radius" : "length") + " of at least 0.001 mm."));
    if (lead.kind == LeadKind::Arc && !(lead.sweepDeg > 0 && lead.sweepDeg <= 180))
        return tl::unexpected(invalid("The lead arc of pass " + std::to_string(no) + " sweeps " +
                                      fixed(geom2d::roundNm(lead.sweepDeg * 1000), 1000, 3) +
                                      " degrees; a lead arc sweeps more than 0 and at most 180 degrees."));
    if (!s.hasTangent)
        return tl::unexpected(failure("Pass " + std::to_string(no) + " has a lead but no direction at its ends."));
    if (room == nullptr)
        return tl::unexpected(failure("Pass " + std::to_string(no) +
                                      " has a lead but no room (where the tool centre may go) to check it in."));
    const Pt at = in ? xyOf(s.start) : xyOf(s.end);
    const D2 t = in ? s.t0 : s.t1;
    const Coord least = std::max<Coord>(1000, lead.minRadius > 0 ? lead.minRadius : size);
    Coord tried = size;
    for (int k = 0; k < kMaxLeadTries; ++k) {
        const Coord cur = k == 0 ? size : std::max(least, size >> k);
        if (k > 0 && cur >= tried)
            break;
        tried = cur;
        const Path p = leadPath(at, vec(t), lead, cur, in);
        auto inside = insideRoom(p, *room, ctx);
        if (!inside)
            return tl::unexpected(inside.error());
        if (*inside) {
            if (cur != size)
                warnings.push_back(what +
                                   " was made smaller to fit: " + (lead.kind == LeadKind::Arc ? "radius " : "length ") +
                                   mm(cur) + " mm instead of " + mm(size) + " mm");
            return std::optional<Path>{p};
        }
    }
    return tl::unexpected(failure("Pass " + std::to_string(no) + ": the lead-" + (in ? "in" : "out") + " (" +
                                  (lead.kind == LeadKind::Arc ? "arc" : "line") + ", " + mm(size) + " mm) at " +
                                  at2(at) + " does not fit the room even at " + mm(tried) + " mm."));
}

// ---------- Checks ----------
Status checkOptions(const Options& o, const Stock& stock)
{
    if (o.toolRadius <= 0)
        return tl::unexpected(invalid("Linking needs the tool's radius (above 0)."));
    if (!(o.clearance >= o.retract && o.retract >= o.feedPlane))
        return tl::unexpected(invalid("Heights out of order: clearance " + mm(o.clearance) + " mm, retract " +
                                      mm(o.retract) + " mm, feed plane " + mm(o.feedPlane) +
                                      " mm (each must be at or below the one before)."));
    if (!stock.region.empty() && o.retract < stock.top)
        return tl::unexpected(invalid("The retract plane (" + mm(o.retract) + " mm) is below the top of the stock (" +
                                      mm(stock.top) + " mm): links at the retract plane would cut the stock."));
    if (!(o.rampAngleDeg >= 0 && o.rampAngleDeg < 90))
        return tl::unexpected(invalid("The ramp angle must be at least 0 and below 90 degrees."));
    if (o.helixDiameter < 0 || o.helixMinDiameter < 0 || (o.helixDiameter > 0 && o.helixMinDiameter > o.helixDiameter))
        return tl::unexpected(invalid("The helix diameter (" + mm(o.helixDiameter) +
                                      " mm) must be at least its minimum (" + mm(o.helixMinDiameter) + " mm)."));
    if (o.stayDownLift < 0 || o.entryClearance < 0 || o.maxStayDown < 0 || o.minRampRun < 0)
        return tl::unexpected(invalid("Linking distances must not be negative."));
    if (o.tipHeight < 0 || o.tipHeight > geom2d::kMaxCoord)
        return tl::unexpected(invalid("The tool's tip height must be at least 0 (and below 1 km)."));
    return {};
}

Status checkPass(const Pass& p, std::size_t no, const Options& o)
{
    const std::string name = "Pass " + std::to_string(no);
    if (p.moves.empty())
        return tl::unexpected(invalid(name + " has no moves."));
    if (p.start.z > o.feedPlane)
        return tl::unexpected(invalid(name + " starts at Z " + mm(p.start.z) + " mm, above the feed plane (" +
                                      mm(o.feedPlane) + " mm)."));
    Pt prev = xyOf(p.start);
    for (std::size_t k = 0; k < p.moves.size(); ++k) {
        const Move& m = p.moves[k];
        const std::string mv = name + ", move " + std::to_string(k + 1);
        if (m.kind == MoveKind::Rapid)
            return tl::unexpected(invalid(mv + " is a rapid: passes are feed moves, linking adds the rapids."));
        if (m.kind != MoveKind::Linear && !isArcMove(m))
            return tl::unexpected(invalid(mv + " is a canned cycle or a dwell, which linking does not join."));
        if (isArcMove(m) && m.plane != ArcPlane::XY)
            return tl::unexpected(invalid(mv + " is an arc outside the XY plane, which linking does not join."));
        if (isArcMove(m) && distXY(prev, Pt{m.i, m.j}) < 1000)
            return tl::unexpected(invalid(mv + " is an arc of radius under 0.001 mm."));
        if (m.feedMicronPerMin <= 0)
            return tl::unexpected(invalid(mv + " has no feed."));
        if (m.z > o.feedPlane)
            return tl::unexpected(
                invalid(mv + " reaches Z " + mm(m.z) + " mm, above the feed plane (" + mm(o.feedPlane) + " mm)."));
        if (m.compensation > 2)
            return tl::unexpected(invalid(mv + " has a compensation value outside 0, 1, 2."));
        prev = xyOf(m);
    }
    if (p.reversible || p.rotatable) {
        const std::optional<Path> path = planarPath(p);
        if (!path || !uniformMoves(p))
            return tl::unexpected(invalid(name + " is marked " + (p.rotatable ? "rotatable" : "reversible") +
                                          " but is not flat with one role and feed and no compensation."));
        if (p.rotatable && !path->closed)
            return tl::unexpected(invalid(name + " is marked rotatable but does not end where it starts."));
    }
    if (p.room) {
        for (const geom2d::Region& r : *p.room) {
            if (auto st = geom2d::checkRange(r.outer); !st)
                return tl::unexpected(invalid(name + ": its room lies beyond 1 km."));
        }
    }
    return {};
}

} // namespace

// ---------- detail ----------
namespace detail {

std::string mm(Nm v)
{
    // Rounded half to even to the micrometre.
    const Nm q = v / 1000, r = v % 1000;
    Nm um = q;
    if (r > 500 || (r == 500 && (q % 2) != 0))
        ++um;
    else if (r < -500 || (r == -500 && (q % 2) != 0))
        --um;
    return fixed(um, 1000, 3);
}

std::string mm(double nm)
{
    return mm(geom2d::roundNm(nm));
}

Error failure(std::string message)
{
    return Error{ErrorCode::ToolpathFailed, std::move(message), {std::string(kWhere)}};
}

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {std::string(kWhere)}};
}

Shape placedShape(const Pass& pass, const Placement& pl)
{
    if (!pl.reversed && pl.startS == 0)
        return shapeFrom(pass.start, pass.moves);
    const std::optional<Path> path = planarOf(pass.start, pass.moves);
    if (!path)
        return shapeFrom(pass.start, pass.moves);
    Path q = pl.reversed ? geom2d::reverse(*path) : *path;
    if (pl.startS != 0 && q.closed)
        q = geom2d::startAt(q, pl.startS);
    const Move& m0 = pass.moves.front();
    std::vector<Move> moves;
    geom2d::appendMoves(q, pass.start.z, m0.role, m0.feedMicronPerMin, moves);
    return shapeFrom(Pt3{q.start.x, q.start.y, pass.start.z}, std::move(moves));
}

Path projection(Pt3 start, const std::vector<Move>& moves)
{
    Path p;
    p.start = xyOf(start);
    Pt prev = p.start;
    for (const Move& m : moves) {
        if (isArcMove(m)) {
            const Pt c{m.i, m.j};
            p.spans.push_back(
                geom2d::Span{m.kind == MoveKind::ArcCCW ? geom2d::SpanKind::ArcCCW : geom2d::SpanKind::ArcCW, xyOf(m),
                             c, geom2d::roundNm(distXY(prev, c))});
        } else if (xyOf(m) != prev) {
            p.spans.push_back(lineTo(xyOf(m)));
        }
        prev = xyOf(m);
    }
    return p;
}

Ends nominalEnds(const Pass& pass, const Shape& s)
{
    Ends e{s.start, s.end};
    if (pass.leadIn.kind != LeadKind::None && s.hasTangent && leadSize(pass.leadIn) > 0) {
        const Path p = leadPath(xyOf(s.start), vec(s.t0), pass.leadIn, leadSize(pass.leadIn), true);
        e.a = Pt3{p.start.x, p.start.y, s.start.z};
    }
    if (pass.leadOut.kind != LeadKind::None && s.hasTangent && leadSize(pass.leadOut) > 0) {
        const Path p = leadPath(xyOf(s.end), vec(s.t1), pass.leadOut, leadSize(pass.leadOut), false);
        const Pt end = p.spans.empty() ? p.start : p.spans.back().end;
        e.e = Pt3{end.x, end.y, s.end.z};
    }
    return e;
}

} // namespace detail

// ---------- Public helpers ----------
Pass passFromPath(const Path& path, Nm z, std::int32_t feedMicronPerMin, MoveRole role)
{
    Pass p;
    p.start = Pt3{path.start.x, path.start.y, z};
    geom2d::appendMoves(path, z, role, feedMicronPerMin, p.moves);
    return p;
}

Regions unbounded()
{
    constexpr Coord h = 1'000'000'000'000 / 2;
    Path square;
    square.start = Pt{-h, -h};
    square.spans = {lineTo(Pt{h, -h}), lineTo(Pt{h, h}), lineTo(Pt{-h, h}), lineTo(Pt{-h, -h})};
    square.closed = true;
    return Regions{geom2d::Region{square, {}}};
}

std::optional<Path> planarPath(const Pass& p)
{
    return planarOf(p.start, p.moves);
}

bool uniformMoves(const Pass& p)
{
    for (const Move& m : p.moves)
        if (m.role != p.moves.front().role || m.feedMicronPerMin != p.moves.front().feedMicronPerMin ||
            m.compensation != 0)
            return false;
    return true;
}

Stock stockOf(const ResolvedGeometry& geom)
{
    const Box3& b = geom.stockBounds;
    Stock s;
    const Coord x0 = toNm(b.min.x), y0 = toNm(b.min.y), x1 = toNm(b.max.x), y1 = toNm(b.max.y);
    s.top = toNm(b.max.z);
    if (x1 > x0 && y1 > y0) {
        Path box;
        box.start = Pt{x0, y0};
        box.spans = {lineTo(Pt{x1, y0}), lineTo(Pt{x1, y1}), lineTo(Pt{x0, y1}), lineTo(Pt{x0, y0})};
        box.closed = true;
        s.region = Regions{geom2d::Region{box, {}}};
    }
    return s;
}

Path leadPath(Pt at, Vec2 tangent, const Lead& lead, Coord size, bool in)
{
    Path p;
    p.start = at;
    const D2 t = unit(D2{tangent.x, tangent.y});
    if (lead.kind == LeadKind::None || size <= 0)
        return p;
    if (lead.kind == LeadKind::Line) {
        const Pt far = roundPt(d2(at) + t * (in ? -static_cast<double>(size) : static_cast<double>(size)));
        if (in) {
            p.start = far;
            p.spans.push_back(lineTo(at));
        } else {
            p.spans.push_back(lineTo(far));
        }
        return p;
    }
    const bool ccw = lead.side == Side::Left;
    const D2 n = ccw ? leftOf(t) : rightOf(t);
    const Pt c = roundPt(d2(at) + n * static_cast<double>(size));
    const Coord r = geom2d::roundNm(distXY(at, c));
    const double dir = ccw ? 1.0 : -1.0;
    const double sweep = std::clamp(lead.sweepDeg, 0.0, 180.0) * kPi / 180.0;
    const double a = dmath::atan2(static_cast<double>(at.y - c.y), static_cast<double>(at.x - c.x));
    const geom2d::SpanKind kind = ccw ? geom2d::SpanKind::ArcCCW : geom2d::SpanKind::ArcCW;
    const auto tangentAt = [&](Pt q) {
        return unit(ccw ? leftOf(d2(q) - d2(c)) : rightOf(d2(q) - d2(c)));
    };
    if (in) {
        const Pt q = roundPt(onCircle(c, r, a - dir * sweep));
        if (lead.length > 0) {
            p.start = roundPt(d2(q) - tangentAt(q) * static_cast<double>(lead.length));
            p.spans.push_back(lineTo(q));
        } else {
            p.start = q;
        }
        p.spans.push_back(geom2d::Span{kind, at, c, r});
    } else {
        const Pt q = roundPt(onCircle(c, r, a + dir * sweep));
        p.spans.push_back(geom2d::Span{kind, q, c, r});
        if (lead.length > 0)
            p.spans.push_back(lineTo(roundPt(d2(q) + tangentAt(q) * static_cast<double>(lead.length))));
    }
    return p;
}

void appendSloped(const Path& path, Nm z0, Nm z1, MoveRole role, std::int32_t feedMicronPerMin, std::vector<Move>& out)
{
    // The pieces as geom2d::appendMoves makes them (at height z1), then Z by arc length.
    std::vector<Move> flat;
    geom2d::appendMoves(path, z1, role, feedMicronPerMin, flat);
    std::vector<double> lengths;
    lengths.reserve(flat.size());
    double total = 0;
    Pt prev = path.start;
    for (const Move& m : flat) {
        double len = 0;
        if (isArcMove(m)) {
            const Pt c{m.i, m.j};
            const double r = distXY(prev, c);
            const double a0 = dmath::atan2(static_cast<double>(prev.y - c.y), static_cast<double>(prev.x - c.x));
            const double a1 = dmath::atan2(static_cast<double>(m.y - c.y), static_cast<double>(m.x - c.x));
            double sweep = m.kind == MoveKind::ArcCCW ? a1 - a0 : a0 - a1;
            while (sweep <= 0)
                sweep += 2 * kPi;
            if (xyOf(m) == prev)
                sweep = 2 * kPi;
            len = r * sweep;
        } else {
            len = distXY(prev, xyOf(m));
        }
        lengths.push_back(len);
        total += len;
        prev = xyOf(m);
    }
    double at = 0;
    for (std::size_t k = 0; k < flat.size(); ++k) {
        Move m = flat[k];
        at += lengths[k];
        const Nm z =
            k + 1 == flat.size() || total <= 0 ? z1 : z0 + geom2d::roundNm(static_cast<double>(z1 - z0) * (at / total));
        m.z = z;
        if (isArcMove(m))
            m.k = z;
        out.push_back(m);
    }
}

bool appendSloped(const Path& path, Nm z0, Nm z1, double maxSlope, MoveRole role, std::int32_t feedMicronPerMin,
                  std::vector<Move>& out)
{
    std::vector<Move> flat;
    geom2d::appendMoves(path, z1, role, feedMicronPerMin, flat);
    if (flat.empty())
        return z0 == z1;
    // Each piece's XY length (an arc's at the smaller of its two radii, so no reading of it is shorter), and the
    // largest whole number of nanometres it may climb or descend.
    std::vector<double> lengths;
    std::vector<std::int64_t> caps;
    lengths.reserve(flat.size());
    caps.reserve(flat.size());
    double total = 0;
    std::int64_t capacity = 0;
    Pt prev = path.start;
    for (const Move& m : flat) {
        double len = 0;
        if (isArcMove(m)) {
            const Pt c{m.i, m.j};
            const double r = std::min(distXY(prev, c), distXY(xyOf(m), c));
            const double a0 = dmath::atan2(static_cast<double>(prev.y - c.y), static_cast<double>(prev.x - c.x));
            const double a1 = dmath::atan2(static_cast<double>(m.y - c.y), static_cast<double>(m.x - c.x));
            double sweep = m.kind == MoveKind::ArcCCW ? a1 - a0 : a0 - a1;
            while (sweep <= 0)
                sweep += 2 * kPi;
            if (xyOf(m) == prev)
                sweep = 2 * kPi;
            len = r * sweep;
        } else {
            len = distXY(prev, xyOf(m));
        }
        lengths.push_back(len);
        total += len;
        caps.push_back(std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(maxSlope * len * (1.0 - 1e-9)))));
        capacity += caps.back();
        prev = xyOf(m);
    }
    const std::int64_t span = z1 >= z0 ? z1 - z0 : z0 - z1;
    const std::int64_t sign = z1 >= z0 ? 1 : -1;
    if (capacity < span || total <= 0)
        return false;
    // The climb or descent done by the end of each piece: the proportional one rounded half to even, clamped so that
    // this piece stays within its cap and the pieces after it can still do the rest.
    std::int64_t done = 0, after = capacity;
    double at = 0;
    for (std::size_t k = 0; k < flat.size(); ++k) {
        at += lengths[k];
        after -= caps[k];
        const std::int64_t ideal = geom2d::roundNm(static_cast<double>(span) * (at / total));
        const std::int64_t lo = std::max(done, span - after);
        const std::int64_t hi = std::min(done + caps[k], span);
        done = k + 1 == flat.size() ? span : std::clamp(ideal, lo, hi);
        Move m = flat[k];
        m.z = z0 + sign * done;
        if (isArcMove(m))
            m.k = m.z;
        out.push_back(m);
    }
    return true;
}

Lengths lengthsOf(std::span<const Move> moves, Pt3 from)
{
    Lengths l;
    Pt3 cur = from;
    for (const Move& m : moves) {
        double len = 0;
        const double dz = static_cast<double>(m.z - cur.z);
        if (isArcMove(m)) {
            const Pt c{m.i, m.j};
            const double r = distXY(xyOf(cur), c);
            const double a0 = dmath::atan2(static_cast<double>(cur.y - c.y), static_cast<double>(cur.x - c.x));
            const double a1 = dmath::atan2(static_cast<double>(m.y - c.y), static_cast<double>(m.x - c.x));
            double sweep = m.kind == MoveKind::ArcCCW ? a1 - a0 : a0 - a1;
            while (sweep <= 0)
                sweep += 2 * kPi;
            if (xyOf(m) == xyOf(cur))
                sweep = 2 * kPi;
            len = dmath::hypot(r * sweep, dz);
        } else {
            len = dmath::hypot(distXY(xyOf(cur), xyOf(m)), dz);
        }
        (m.kind == MoveKind::Rapid ? l.rapid : l.feed) += len;
        cur = Pt3{m.x, m.y, m.z};
    }
    return l;
}

// ---------- linkPasses ----------
Result<Linked> linkPasses(std::span<const Pass> passes, const Options& opts, const Stock& stock, JobContext& ctx)
{
    if (auto st = checkOptions(opts, stock); !st)
        return tl::unexpected(st.error());
    for (std::size_t i = 0; i < passes.size(); ++i)
        if (auto st = checkPass(passes[i], i + 1, opts); !st)
            return tl::unexpected(st.error());
    Linked result;
    if (passes.empty())
        return result;

    std::vector<Placement> order;
    if (opts.keepOrder) {
        for (std::size_t i = 0; i < passes.size(); ++i)
            order.push_back(Placement{i, false, 0});
    } else {
        auto o = orderPasses(passes, opts, ctx);
        if (!o)
            return tl::unexpected(o.error());
        order = std::move(*o);
    }

    Material material(stock, opts.toolRadius, opts.tipHeight);
    std::map<std::size_t, Regions> grownRooms;
    Emitter em;
    double linkFeed = 0;
    std::int32_t lastLinkFeed = 0;
    for (std::size_t k = 0; k < order.size(); ++k) {
        if (auto st = ctx.cancel.check(); !st)
            return tl::unexpected(st.error());
        ctx.progress.set(static_cast<double>(k) / static_cast<double>(order.size()));
        const Placement& pl = order[k];
        const Pass& pass = passes[pl.pass];
        const std::size_t no = pl.pass + 1;
        const Shape sh = placedShape(pass, pl);
        const std::int32_t cutFeed = sh.moves.front().feedMicronPerMin;
        const auto feedOr = [&](std::int32_t f) {
            return f > 0 ? f : cutFeed;
        };

        const Regions* room = nullptr;
        if (pass.room) {
            auto it = grownRooms.find(pl.pass);
            if (it == grownRooms.end()) {
                auto g = geom2d::offset(*pass.room, kRoomGrow, ctx);
                if (!g)
                    return tl::unexpected(g.error());
                it = grownRooms.emplace(pl.pass, std::move(*g)).first;
            }
            room = &it->second;
        }

        // Leads.
        if (sh.comp != 0) {
            const auto straightStart = [](const Lead& l) {
                return l.kind == LeadKind::Line || (l.kind == LeadKind::Arc && l.length > 0);
            };
            if (!straightStart(pass.leadIn) || !straightStart(pass.leadOut))
                return tl::unexpected(failure(
                    "Pass " + std::to_string(no) + " uses cutter compensation " + compensationWords(opts) +
                    ", which needs a straight lead-in to switch it on and a straight lead-out to switch it off (a "
                    "line lead, or an arc lead with a straight part)."));
        }
        auto lin = fitLead(pass.leadIn, sh, true, room, no, ctx, result.warnings);
        if (!lin)
            return tl::unexpected(lin.error());
        auto lout = fitLead(pass.leadOut, sh, false, room, no, ctx, result.warnings);
        if (!lout)
            return tl::unexpected(lout.error());
        std::vector<Move> inMoves, outMoves;
        if (*lin) {
            geom2d::appendMoves(**lin, sh.start.z, MoveRole::LeadIn, feedOr(opts.feeds.leadIn), inMoves);
            for (Move& m : inMoves)
                m.compensation = sh.comp;
        }
        if (*lout) {
            geom2d::appendMoves(**lout, sh.end.z, MoveRole::LeadOut, feedOr(opts.feeds.leadOut), outMoves);
            for (Move& m : outMoves)
                m.compensation = sh.comp;
            outMoves.back().compensation = 0;
        }
        const Pt3 a = *lin ? Pt3{(*lin)->start.x, (*lin)->start.y, sh.start.z} : sh.start;
        // What the pass cuts (with its leads), swept once when first needed.
        std::vector<Path> cut;
        if (*lin)
            cut.push_back(**lin);
        cut.push_back(sh.path ? *sh.path : projection(sh.start, sh.moves));
        if (*lout)
            cut.push_back(**lout);
        std::optional<Regions> cutArea;
        const auto passArea = [&]() -> Result<const Regions*> {
            if (!cutArea) {
                auto s = Material::swept(cut, opts.toolRadius, ctx);
                if (!s)
                    return tl::unexpected(s.error());
                cutArea = std::move(*s);
            }
            return &*cutArea;
        };

        // The link: stay down, or retract and enter.
        Entered entered = Entered::Free;
        Coord helixD = 0;
        bool stayed = false;
        const std::int32_t linkF = feedOr(opts.feeds.link);
        if (k > 0 && opts.maxStayDown > 0 && distXY(xyOf(em.cur), xyOf(a)) <= static_cast<double>(opts.maxStayDown)) {
            const Nm zl = std::max(em.cur.z, a.z) + opts.stayDownLift;
            if (zl <= opts.feedPlane) {
                Path link;
                link.start = xyOf(em.cur);
                if (xyOf(a) != link.start)
                    link.spans.push_back(lineTo(xyOf(a)));
                auto probe = material.sweep(link, ctx);
                if (!probe)
                    return tl::unexpected(probe.error());
                auto across = material.freeAt(*probe, zl, ctx);
                if (!across)
                    return tl::unexpected(across.error());
                bool down = true;
                if (*across && a.z < zl) {
                    auto d = material.freeAt(material.disc(xyOf(a)), a.z, ctx);
                    if (!d)
                        return tl::unexpected(d.error());
                    down = *d;
                }
                if (*across && down) {
                    em.feed(Pt3{em.cur.x, em.cur.y, zl}, MoveRole::Link, linkF);
                    em.feed(Pt3{a.x, a.y, zl}, MoveRole::Link, linkF);
                    em.feed(a, MoveRole::Link, linkF);
                    stayed = true;
                    entered = Entered::StayDown;
                }
            }
            if (!stayed && opts.stepIn && em.cur.z == a.z && xyOf(a) != xyOf(em.cur)) {
                // Step in: the centre line in cut area, the material removed inside what the pass cuts anyway.
                Path line;
                line.start = xyOf(em.cur);
                line.spans.push_back(lineTo(xyOf(a)));
                auto centre = geom2d::sweptArea(std::span<const Path>(&line, 1), kRoomProbe, ctx);
                if (!centre)
                    return tl::unexpected(centre.error());
                // stepInSlot (T-054): or in what the pass itself cuts.
                const Regions* besides = nullptr;
                if (opts.stepInSlot) {
                    auto own = passArea();
                    if (!own)
                        return tl::unexpected(own.error());
                    besides = *own;
                }
                auto centreFree = material.freeAt(*centre, a.z, ctx, besides);
                if (!centreFree)
                    return tl::unexpected(centreFree.error());
                if (*centreFree) {
                    auto probe = material.sweep(line, ctx);
                    if (!probe)
                        return tl::unexpected(probe.error());
                    auto own = passArea();
                    if (!own)
                        return tl::unexpected(own.error());
                    auto flank = material.freeAt(*probe, a.z, ctx, *own);
                    if (!flank)
                        return tl::unexpected(flank.error());
                    if (*flank) {
                        em.feed(a, MoveRole::Cut, cutFeed);
                        stayed = true;
                        entered = Entered::Step;
                    }
                }
            }
        }
        if (!stayed) {
            auto freeLevel = material.lowestFree(material.disc(xyOf(a)), a.z, ctx);
            if (!freeLevel)
                return tl::unexpected(freeLevel.error());
            Plan plan;
            if (*freeLevel <= a.z) {
                plan.kind = Entered::Free;
                plan.start = xyOf(a);
                plan.level = a.z;
                plan.top = a.z;
            } else {
                std::optional<Path> entryPath;
                bool ring = false;
                if (*lin) {
                    Path ep = **lin;
                    if (sh.path)
                        ep.spans.insert(ep.spans.end(), sh.path->spans.begin(), sh.path->spans.end());
                    entryPath = ep;
                } else if (sh.path) {
                    entryPath = sh.path;
                    ring = sh.path->closed;
                }
                D2 tangent = sh.t0;
                bool hasTangent = sh.hasTangent;
                if (*lin && !(*lin)->spans.empty()) {
                    const Vec2 t = geom2d::tangentAt(**lin, 0);
                    tangent = D2{t.x, t.y};
                    hasTangent = true;
                }
                EntryContext ec{opts,
                                pass,
                                sh,
                                no,
                                a,
                                tangent,
                                hasTangent,
                                entryPath,
                                ring,
                                room,
                                *freeLevel,
                                feedOr(opts.feeds.ramp),
                                feedOr(opts.feeds.plunge),
                                material,
                                ctx};
                auto p = planEntry(ec);
                if (!p)
                    return tl::unexpected(p.error());
                plan = std::move(*p);
            }
            // T-055 (Options::stayDownEntry): stay in the cut and enter from there.
            const Pt s = plan.start;
            bool kept = false;
            if (k > 0 && opts.stayDownEntry && opts.maxStayDown > 0 && plan.kind != Entered::Free &&
                distXY(xyOf(em.cur), s) <= static_cast<double>(opts.maxStayDown)) {
                const Nm zt = std::max(em.cur.z, plan.top);
                if (zt <= opts.feedPlane) {
                    Path link;
                    link.start = xyOf(em.cur);
                    if (s != link.start)
                        link.spans.push_back(lineTo(s));
                    auto probe = material.sweep(link, ctx);
                    if (!probe)
                        return tl::unexpected(probe.error());
                    auto across = material.freeAt(*probe, zt, ctx);
                    if (!across)
                        return tl::unexpected(across.error());
                    if (*across) {
                        if (zt > em.cur.z)
                            em.feed(Pt3{em.cur.x, em.cur.y, zt}, MoveRole::Link, linkF);
                        if (s != xyOf(em.cur))
                            em.feed(Pt3{s.x, s.y, zt}, MoveRole::Link, linkF);
                        if (zt > plan.top)
                            em.feed(Pt3{s.x, s.y, plan.top}, MoveRole::Link, linkF);
                        em.append(plan.moves);
                        if (plan.flat) {
                            if (auto st = material.addCut({*plan.flat}, opts.toolRadius, a.z, ctx); !st)
                                return tl::unexpected(st.error());
                        }
                        kept = true;
                    }
                }
            }
            // Out of the last cut, across at the retract plane, down to the feed plane (or the free column's floor).
            if (!kept) {
                if (k == 0) {
                    result.start = Pt3{s.x, s.y, opts.clearance};
                    em.rapid(result.start, MoveRole::Approach);
                } else {
                    if (em.cur.z < opts.retract) {
                        if (!opts.rapidRetract && em.cur.z < opts.feedPlane) {
                            em.feed(Pt3{em.cur.x, em.cur.y, opts.feedPlane}, MoveRole::Retract, linkF);
                        }
                        em.rapid(Pt3{em.cur.x, em.cur.y, opts.retract}, MoveRole::Retract);
                    }
                    em.rapid(Pt3{s.x, s.y, std::max(em.cur.z, opts.retract)}, MoveRole::Link);
                }
                const Nm zRapid = std::max(opts.feedPlane, plan.level);
                em.rapid(Pt3{s.x, s.y, std::min(em.cur.z, zRapid)}, MoveRole::Approach);
                const std::int32_t plungeF = feedOr(opts.feeds.plunge);
                if (plan.kind == Entered::Free) {
                    em.feed(a, MoveRole::Approach, plungeF);
                } else {
                    em.feed(Pt3{s.x, s.y, plan.top}, MoveRole::Approach, plungeF);
                    em.append(plan.moves);
                    if (plan.flat) {
                        if (auto st = material.addCut({*plan.flat}, opts.toolRadius, a.z, ctx); !st)
                            return tl::unexpected(st.error());
                    }
                }
            }
            entered = plan.kind;
            helixD = plan.helixDiameter;
        }
        lastLinkFeed = linkF;
        result.passMove.push_back(em.out.size());
        em.append(inMoves);
        em.append(sh.moves);
        em.append(outMoves);

        // What the pass cut.
        if (!material.noMaterial() && sh.zMax < stock.top) {
            auto area = passArea();
            if (!area)
                return tl::unexpected(area.error());
            material.addArea(std::move(*cutArea), sh.zMax);
        }

        result.order.push_back(pl);
        result.entered.push_back(entered);
        result.helixDiameter.push_back(helixD);
    }
    // Up to the clearance.
    if (em.cur.z < opts.clearance) {
        if (!opts.rapidRetract && em.cur.z < opts.feedPlane)
            em.feed(Pt3{em.cur.x, em.cur.y, opts.feedPlane}, MoveRole::Retract, lastLinkFeed);
        em.rapid(Pt3{em.cur.x, em.cur.y, opts.clearance}, MoveRole::Retract);
    }
    em.mergeShortMoves(result.passMove);
    // Stats: rapids, and the feed of links and retracts.
    Pt3 cur = result.start;
    for (const Move& m : em.out) {
        const Lengths l = lengthsOf(std::span<const Move>(&m, 1), cur);
        result.rapidLength += l.rapid;
        if (m.kind != MoveKind::Rapid && (m.role == MoveRole::Link || m.role == MoveRole::Retract))
            linkFeed += l.feed;
        cur = Pt3{m.x, m.y, m.z};
    }
    result.linkFeedLength = linkFeed;
    result.moves = std::move(em.out);
    ctx.progress.set(1.0);
    return result;
}

// ---------- cam.h's public form ----------
namespace {
Result<std::vector<Move>> publicLink(std::span<const LinkPass> in, const Heights& h, const LinkOptions& o,
                                     const ResolvedGeometry& geom, JobContext& ctx)
{
    if (o.leadRadius != 0)
        return tl::unexpected(
            invalid("Leads need the room the tool centre may use; the operation types link through src/cam/link.h."));
    const auto feed = [](double mmPerMin) {
        return static_cast<std::int32_t>(std::clamp<Nm>(geom2d::roundNm(mmPerMin * 1000.0), 0, 2'147'483'647));
    };
    Options opts;
    opts.clearance = toNm(h.clearance);
    opts.retract = toNm(h.retract);
    opts.feedPlane = toNm(h.feedPlane);
    opts.toolRadius = toNm(o.toolDiameter) / 2;
    opts.centreCutting = o.centreCutting;
    opts.feeds.plunge = feed(o.feedPlunge);
    opts.feeds.ramp = feed(o.feedRamp);
    opts.feeds.link = feed(o.feedLink);
    opts.maxStayDown = toNm(o.maxStayDownDistance);
    opts.stayDownLift = toNm(o.stayDownLift);
    opts.rapidRetract = o.allowRapidRetract;
    if (!o.entryOrder.empty())
        opts.entryOrder = o.entryOrder;
    opts.helixDiameter = toNm(o.helixDiameter);
    opts.helixMinDiameter = toNm(o.helixMinDiameter);
    opts.rampAngleDeg = o.rampAngleDeg;
    opts.minRampRun = toNm(o.minRampRun);
    opts.entryClearance = toNm(o.entryClearance);
    for (const PreDrillPoint& p : o.preDrillPoints)
        opts.preDrills.push_back(PreDrill{Pt{toNm(p.x), toNm(p.y)}, toNm(p.diameter), toNm(p.bottom)});
    opts.keepOrder = o.keepOrder;
    std::vector<Pass> passes;
    passes.reserve(in.size());
    for (const LinkPass& lp : in) {
        Pass p;
        p.start = Pt3{lp.startX, lp.startY, lp.startZ};
        p.moves = lp.moves;
        p.reversible = lp.reversible;
        p.sequence = lp.sequence;
        p.cluster = lp.cluster;
        p.level = lp.level;
        passes.push_back(std::move(p));
    }
    auto linked = linkPasses(passes, opts, stockOf(geom), ctx);
    if (!linked)
        return tl::unexpected(linked.error());
    return std::move(linked->moves);
}
} // namespace

} // namespace ray::cam::link

namespace ray::cam {

Result<std::vector<Move>> linkPasses(std::span<const LinkPass> passes, const Heights& heights,
                                     const LinkOptions& options, const ResolvedGeometry& geom, JobContext& ctx)
{
    return link::publicLink(passes, heights, options, geom, ctx);
}

} // namespace ray::cam
