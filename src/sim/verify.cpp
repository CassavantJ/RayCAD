// The safety checks of a setup's program (task T-071): the analytic layer, events, ToolTooShort and travel. What is
// checked and why: verify.h. The cut-stock layer: verify_cutstock.cpp; the motions: verify_motion.cpp; the exact
// geometry: verify_geom.h.
#include "verify_impl.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ray::sim::detail {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// ---------- Tool zones ----------

// The top of a profile: its last height and the largest radius it has there.
Vec2 topOf(const std::vector<Vec2>& p)
{
    Vec2 t{0, 0};
    if (p.empty()) {
        return t;
    }
    t = {0, p.back().y};
    for (std::size_t i = p.size(); i-- > 0 && p[i].y == p.back().y;) {
        t.x = std::max(t.x, p[i].x);
    }
    return t;
}

double maxRadius(const std::vector<Vec2>& p)
{
    double r = 0;
    for (const Vec2& v : p) {
        r = std::max(r, v.x);
    }
    return r;
}

// The part of a profile at and above height `from` (cut by interpolation).
std::vector<Vec2> above(const std::vector<Vec2>& p, double from)
{
    std::vector<Vec2> out;
    if (p.size() < 2 || from >= p.back().y) {
        return out;
    }
    for (std::size_t i = 0; i + 1 < p.size(); ++i) {
        const Vec2 a = p[i], b = p[i + 1];
        if (b.y < from) {
            continue;
        }
        if (a.y >= from) {
            if (out.empty() || out.back().x != a.x || out.back().y != a.y) {
                out.push_back(a);
            }
        } else if (b.y > a.y) {
            const double t = (from - a.y) / (b.y - a.y);
            out.push_back({a.x + (b.x - a.x) * t, from});
        }
    }
    out.push_back(p.back());
    return out;
}

// A zero-radius stub: as the last profile of a ToolShape it stops the stock model's endless top.
std::vector<Vec2> stub(double h)
{
    return {{0.0, h}, {0.0, h + 1.0}};
}

// The holder zone's profile: the holder, or without one the spindle side above the tool's top.
std::vector<Vec2> holderProfileOf(const ToolShape& s)
{
    if (!s.holderProfile.empty()) {
        return s.holderProfile;
    }
    const Vec2 t = !s.shankProfile.empty() ? topOf(s.shankProfile) : topOf(s.cuttingProfile);
    if (!(t.x > 0)) {
        return {};
    }
    return {{t.x, t.y}, {t.x, t.y + 1.0}};
}

// The zone of profile p grown by e (contact, and the sagitta along arcs).
CutterShape grownZone(const std::vector<Vec2>& p, double e, bool endless)
{
    return grownShape(compileProfile(p, endless), e);
}
// The zone of profile p shrunk by the part skin (less the chords' sagitta, which the real tool exceeds the profile by),
// grown again by the arc sagitta for chords.
CutterShape skinZone(const std::vector<Vec2>& p, double arcGrow)
{
    const CutterShape s = compileProfile(shrunkProfile(p, kPartSkinMm - kArcChordTolMm), false);
    return arcGrow > 0 ? grownShape(s, arcGrow) : s;
}
double growOf(int variant)
{
    return kVerifyGrowMm + (variant == 1 ? kVerifyArcSagittaMm : 0.0);
}

ToolZones buildZones(const VerifyOperation& op)
{
    ToolZones z;
    if (op.shape == nullptr) {
        return z;
    }
    const ToolShape& s = *op.shape;
    const std::vector<Vec2> hp = holderProfileOf(s);
    z.hasHolder = !s.holderProfile.empty();
    z.cuttingRadius = maxRadius(s.cuttingProfile);
    z.fluteTop = s.cuttingProfile.empty() ? 0.0 : s.cuttingProfile.back().y;
    const Vec2 shankTop = !s.shankProfile.empty() ? topOf(s.shankProfile) : topOf(s.cuttingProfile);
    z.shankTop = shankTop.y;
    z.shankTopRadius = shankTop.x;
    z.gaugeHeight = !s.holderProfile.empty() ? s.holderProfile.back().y : z.shankTop;
    // With more stick-out: the tool's flutes (when the holder now covers some of them), then its shank.
    z.toolFluteTop = z.fluteTop;
    z.bodyRadius = z.shankTopRadius;
    if (s.shankProfile.empty() && op.tool != nullptr) {
        const double lf = op.tool->fluteLength > 0 ? op.tool->fluteLength : op.tool->overallLength;
        z.toolFluteTop = std::max(z.fluteTop, lf);
        if (op.tool->shankDiameter > 0) {
            z.bodyRadius = op.tool->shankDiameter / 2;
        }
    }
    if (!centreCutting(op.tool) && z.cuttingRadius > 0 && !s.cuttingProfile.empty()) {
        // The flat of the tip: the largest radius at the profile's first height.
        double flat = 0;
        for (const Vec2& v : s.cuttingProfile) {
            if (v.y != s.cuttingProfile.front().y) {
                break;
            }
            flat = std::max(flat, v.x);
        }
        z.deadRadius = kDeadZoneFraction * z.cuttingRadius;
        if (flat > 0) {
            z.deadRadius = std::min(z.deadRadius, flat);
        }
    }
    // The radius never narrows going up (the three profiles joined; the endless top keeps the last radius).
    z.monotone = true;
    double r = -1;
    for (const std::vector<Vec2>* p : {&s.cuttingProfile, &s.shankProfile, &s.holderProfile}) {
        for (const Vec2& v : *p) {
            if (v.x < r) {
                z.monotone = false;
            }
            r = std::max(r, v.x);
        }
    }
    const std::vector<Vec2> deadProfile =
        z.deadRadius > 0 ? std::vector<Vec2>{{0, 0}, {z.deadRadius, 0}, {z.deadRadius, kDeadZoneHeightMm}}
                         : std::vector<Vec2>{};
    for (int v = 0; v < 2; ++v) {
        const double grow = growOf(v);
        const double arcGrow = v == 1 ? kVerifyArcSagittaMm : 0.0;
        z.cutting[v] = grownZone(s.cuttingProfile, grow, false);
        z.shank[v] = grownZone(s.shankProfile, grow, false);
        z.holder[v] = grownZone(hp, grow, true);
        z.cuttingSkin[v] = skinZone(s.cuttingProfile, arcGrow);
        z.shankSkin[v] = skinZone(s.shankProfile, arcGrow);
        z.dead[v] = grownZone(deadProfile, grow, false);
    }
    z.full = s;
    if (!s.shankProfile.empty()) {
        z.shankOnly.shankProfile = s.shankProfile;
        z.shankOnly.holderProfile = stub(z.shankTop);
    }
    z.holderOnly.holderProfile = hp;
    if (!deadProfile.empty()) {
        z.deadOnly.cuttingProfile = deadProfile;
        z.deadOnly.shankProfile = stub(kDeadZoneHeightMm);
    }
    return z;
}

// ---------- Text ----------

std::string runText(std::uint32_t moves)
{
    if (moves <= 1) {
        return "";
    }
    return " (this move and the " + std::to_string(moves - 1) + " after it)";
}

std::string cellText(double cell)
{
    return "in-process stock checked at " + mmText(cell) +
           " mm cells; material thinner than a cell between its rays can be missed";
}

std::string what(EventKind k)
{
    switch (k) {
    case EventKind::RapidIntoStock:
    case EventKind::RapidIntoCutStock:
        return "A rapid runs into ";
    case EventKind::HolderHitsStock:
    case EventKind::HolderHitsPart:
    case EventKind::HolderHitsFixture:
        return "The holder runs into ";
    case EventKind::ShankHitsStock:
    case EventKind::ShankHitsPart:
        return "The tool above its flutes (shank) runs into ";
    case EventKind::ToolHitsFixture:
        return "The tool runs into ";
    case EventKind::PlungeWithNonCentreCutting:
        return "A plunge with a tool that does not cut at its centre goes into ";
    default:
        return "";
    }
}

std::string message(const RawHit& h, std::uint32_t moves, double cell)
{
    std::string target;
    if (h.obstacle == kPart) {
        target = "the part";
    } else if (h.obstacle >= kFixture0) {
        target = "fixture " + std::to_string(h.obstacle - kFixture0 + 1);
    } else if (h.layer == Layer::Analytic) {
        target = "the stock where no earlier move has cut";
    } else {
        target = "material left by earlier moves (" + cellText(cell) + ")";
    }
    return what(h.kind) + target + runText(moves) + ".";
}

// ---------- Travel ----------

struct AxisTravel {
    char letter;
    int index; // 0 x, 1 y, 2 z
    double min, max;
};

std::vector<AxisTravel> knownAxes(const lib::MachineDef* m)
{
    std::vector<AxisTravel> out;
    if (m == nullptr) {
        return out;
    }
    for (const char letter : {'X', 'Y', 'Z'}) {
        for (const lib::AxisDef& a : m->axes) {
            if (a.letter == letter && std::isfinite(a.minTravel) && std::isfinite(a.maxTravel) &&
                a.maxTravel > a.minTravel) {
                out.push_back({letter, letter == 'X' ? 0 : letter == 'Y' ? 1 : 2, a.minTravel, a.maxTravel});
                break;
            }
        }
    }
    return out;
}

double coord(const Vec3& p, int i)
{
    return i == 0 ? p.x : i == 1 ? p.y : p.z;
}

double roundUpTenth(double mm)
{
    return std::ceil(mm * 10.0 - 1e-9) / 10.0;
}

// The order of events at one move: collisions (the shank's two kinds side by side), then plunges, then travel, then
// ToolTooShort, which explains the collisions before it.
int kindRank(EventKind k)
{
    switch (k) {
    case EventKind::RapidIntoStock:
        return 0;
    case EventKind::RapidIntoCutStock:
        return 1;
    case EventKind::HolderHitsStock:
        return 2;
    case EventKind::ShankHitsStock:
        return 3;
    case EventKind::ShankHitsPart:
        return 4;
    case EventKind::ToolHitsFixture:
        return 5;
    case EventKind::HolderHitsFixture:
        return 6;
    case EventKind::HolderHitsPart:
        return 7;
    case EventKind::PlungeWithNonCentreCutting:
        return 8;
    case EventKind::TravelLimit:
        return 9;
    case EventKind::ToolTooShort:
        return 10;
    default:
        return 11 + static_cast<int>(k);
    }
}

} // namespace

// ---------- Shared helpers ----------

std::string mmText(double mm)
{
    if (!std::isfinite(mm) || std::abs(mm) >= 1e12) {
        return "?";
    }
    const Nm nm = toNm(mm);
    std::int64_t um = nm / 1000;
    std::int64_t rest = nm % 1000;
    if (rest < 0) {
        rest += 1000;
        --um;
    }
    if (rest > 500 || (rest == 500 && (um % 2) != 0)) {
        ++um;
    }
    const bool negative = um < 0;
    const std::uint64_t a = negative ? static_cast<std::uint64_t>(-(um + 1)) + 1 : static_cast<std::uint64_t>(um);
    std::string out = (negative ? "-" : "") + std::to_string(a / 1000);
    if (const std::uint64_t frac = a % 1000; frac != 0) {
        std::string digits = std::to_string(frac);
        digits.insert(0, 3 - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0') {
            digits.pop_back();
        }
        out += "." + digits;
    }
    return out;
}

bool centreCutting(const lib::Tool* tool)
{
    if (tool == nullptr) {
        return false;
    }
    using T = lib::ToolType;
    switch (tool->type) {
    case T::Drill:
    case T::SpotDrill:
    case T::BallEndMill:
    case T::EngraveCutter:
        return true;
    case T::ChamferMill:
        return !(tool->tipDiameter > 0);
    default:
        return false;
    }
}

double cutStockCell(const Box3& b)
{
    const double ex = std::max(b.max.x - b.min.x, 0.0), ey = std::max(b.max.y - b.min.y, 0.0),
                 ez = std::max(b.max.z - b.min.z, 0.0);
    double cell = kCutStockCellMm;
    for (int i = 0; i < 60; ++i) {
        const double nx = std::max(1.0, std::ceil(ex / cell)), ny = std::max(1.0, std::ceil(ey / cell)),
                     nz = std::max(1.0, std::ceil(ez / cell));
        if (nx * ny + ny * nz + nx * nz <= static_cast<double>(kCutStockMaxDexels)) {
            break;
        }
        cell *= 2;
    }
    return cell;
}

ToolShape liftedHolderShape(const ToolZones& z, double s)
{
    ToolShape out;
    const std::vector<Vec2>& hp = z.holderOnly.holderProfile;
    if (hp.empty()) {
        return out;
    }
    std::vector<Vec2> p;
    // The body the extra stick-out brings out, above any flute it brings out (flutes cut: they are not a collision).
    const double bodyFrom = std::max(z.shankTop, z.toolFluteTop);
    if (z.shankTop + s > bodyFrom && z.bodyRadius > 0) {
        p.push_back({z.bodyRadius, bodyFrom});
        p.push_back({z.bodyRadius, z.shankTop + s});
    }
    for (const Vec2& v : hp) {
        p.push_back({v.x, v.y + s});
    }
    out.holderProfile = std::move(p);
    return out;
}

ToolShape shankAboveShape(const ToolZones& z, double from)
{
    ToolShape out;
    std::vector<Vec2> p = above(z.shankOnly.shankProfile, from);
    if (p.size() < 2) {
        return out;
    }
    out.shankProfile = std::move(p);
    out.holderProfile = stub(z.shankTop);
    return out;
}

VerifyInput verifyInputFrom(const SimProgram& p)
{
    VerifyInput in;
    for (std::size_t i = 0; i < p.ops.size(); ++i) {
        VerifyOperation op;
        if (p.ops[i] != nullptr) {
            op.id = p.ops[i]->id;
        }
        op.path = i < p.toolpaths.size() ? p.toolpaths[i].get() : nullptr;
        op.shape = i < p.tools.size() ? &p.tools[i] : nullptr;
        op.tool = i < p.toolDefs.size() ? p.toolDefs[i] : nullptr;
        if (op.id == OperationId{} && op.path != nullptr) {
            op.id = op.path->operation;
        }
        in.ops.push_back(op);
    }
    in.stock = &p.stockSolid;
    in.part = &p.partSolid;
    for (const kernel::TriSoup& f : p.fixtureSolids) {
        in.fixtures.push_back(&f);
    }
    in.machine = p.machine;
    return in;
}

// ---------- The checks ----------

Result<std::vector<SimEvent>> verifyProgram(const VerifyInput& in, IJobSystem& jobs, JobContext& ctx,
                                            const VerifyOptions& opts, VerifyStats* statsOut)
{
    VerifyStats stats;
    Progress analyticProgress = ctx.progress.sub(0.5);
    Progress cutProgress = ctx.progress.sub(0.5);
    const std::vector<Motion> motions = expandMotions(in);
    stats.motions = motions.size();
    std::vector<ToolZones> zones;
    zones.reserve(in.ops.size());
    for (const VerifyOperation& op : in.ops) {
        zones.push_back(buildZones(op));
    }
    const bool haveStock = in.stock != nullptr && !in.stock->indices.empty();
    const SoupIndex stock = haveStock ? SoupIndex(*in.stock) : SoupIndex();
    const SoupIndex part = in.part != nullptr ? SoupIndex(*in.part) : SoupIndex();
    std::vector<SoupIndex> fixtures;
    std::vector<std::int32_t> fixtureIds;
    for (std::size_t f = 0; f < in.fixtures.size(); ++f) {
        if (in.fixtures[f] != nullptr && !in.fixtures[f]->indices.empty()) {
            fixtures.emplace_back(*in.fixtures[f]);
            fixtureIds.push_back(kFixture0 + static_cast<std::int32_t>(f));
        }
    }
    // The boxes of every motion's cutting sweep, for "has anything cut near here before".
    OrderedBoxes cutBoxes;
    if (!stock.empty()) {
        std::vector<Box3> boxes;
        boxes.reserve(motions.size());
        for (const Motion& m : motions) {
            const ToolZones& z = zones[m.op];
            const CutterShape& c = z.cutting[m.chord ? 1 : 0];
            boxes.push_back(c.empty() ? Box3{{kInf, kInf, kInf}, {-kInf, -kInf, -kInf}} : sweepBox(c, m.a, m.b));
        }
        cutBoxes = OrderedBoxes(std::move(boxes));
    }
    const double plungeSlope = dmath::tan(kPlungeMinAngleDeg * 3.14159265358979323846 / 180.0);

    std::vector<RawHit> raw;
    std::vector<Deferred> deferred;
    std::vector<std::vector<LengthHit>> lengths(in.ops.size());
    std::vector<double> holderNeed(in.ops.size(), 0.0); // the stick-out found so far per operation
    const auto hit = [&](const Motion& m, std::uint32_t index, EventKind kind, std::int32_t obstacle) {
        RawHit h;
        h.op = m.op;
        h.move = m.move;
        h.motion = index;
        h.kind = kind;
        h.obstacle = obstacle;
        raw.push_back(h);
    };
    // ToolTooShort inputs of a feed motion whose holder or shank met `soup`.
    const auto holderLength = [&](const Motion& m, std::uint32_t index, const SoupIndex& soup) {
        const ToolZones& z = zones[m.op];
        const double grow = growOf(m.chord ? 1 : 0);
        const auto hits = [&](double s) {
            const ToolShape lifted = liftedHolderShape(z, s);
            return sweepHitsSoup(grownZone(lifted.holderProfile, grow, true), m.a, m.b, soup);
        };
        LengthHit L;
        L.motion = index;
        L.holder = true;
        if (holderNeed[m.op] > 0 && !hits(holderNeed[m.op])) {
            L.need = holderNeed[m.op];
        } else {
            L.need = smallestClear(kLiftCapMm, hits);
            L.clearable = L.need <= kLiftCapMm;
            if (L.clearable) {
                holderNeed[m.op] = std::max(holderNeed[m.op], L.need);
            }
        }
        lengths[m.op].push_back(L);
    };
    const auto shankLength = [&](const Motion& m, std::uint32_t index, const SoupIndex& soup) {
        const ToolZones& z = zones[m.op];
        const double grow = growOf(m.chord ? 1 : 0);
        const double span = std::max(0.0, z.shankTop - z.fluteTop);
        const auto hits = [&](double t) {
            const ToolShape s = shankAboveShape(z, z.fluteTop + t);
            return !s.shankProfile.empty() && sweepHitsSoup(grownZone(s.shankProfile, grow, false), m.a, m.b, soup);
        };
        LengthHit L;
        L.motion = index;
        L.holder = false;
        L.need = z.fluteTop + std::min(span, smallestClear(span, hits));
        lengths[m.op].push_back(L);
    };

    // Travel.
    const std::vector<AxisTravel> axes = knownAxes(in.machine);
    double lo[3] = {kInf, kInf, kInf}, hi[3] = {-kInf, -kInf, -kInf};
    std::uint32_t firstOver[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};

    for (std::uint32_t i = 0; i < motions.size(); ++i) {
        if ((i & 1023u) == 0) {
            if (Status s = ctx.cancel.check(); !s) {
                return tl::unexpected(s.error());
            }
            analyticProgress.set(static_cast<double>(i) / static_cast<double>(motions.size()), "Checking moves");
        }
        const Motion& m = motions[i];
        const ToolZones& z = zones[m.op];
        const int v = m.chord ? 1 : 0;

        // Travel of every motion (skipped ones included).
        for (const AxisTravel& ax : axes) {
            const double slack = m.chord ? kVerifyArcSagittaMm : 0.0;
            const double add = ax.index == 2 ? z.gaugeHeight : 0.0;
            const double ca = coord(m.a, ax.index) + add, cb = coord(m.b, ax.index) + add;
            const double mn = std::min(ca, cb) - slack, mx = std::max(ca, cb) + slack;
            if (in.wcsInMachine) {
                const double off = coord(*in.wcsInMachine, ax.index);
                const double over = std::max(ax.min - (mn + off), (mx + off) - ax.max);
                if (over > 0) {
                    RawHit h;
                    h.op = m.op;
                    h.move = m.move;
                    h.motion = i;
                    h.kind = EventKind::TravelLimit;
                    h.obstacle = ax.index;
                    h.amount = std::max(over, 0.0);
                    h.position = (mx + off) - ax.max >= ax.min - (mn + off) ? m.b : m.a;
                    h.havePosition = true;
                    raw.push_back(h);
                }
            } else {
                lo[ax.index] = std::min(lo[ax.index], mn);
                hi[ax.index] = std::max(hi[ax.index], mx);
                if (firstOver[ax.index] == UINT32_MAX && hi[ax.index] - lo[ax.index] > ax.max - ax.min) {
                    firstOver[ax.index] = i;
                }
            }
        }

        const bool up = m.a.x == m.b.x && m.a.y == m.b.y && m.b.z > m.a.z;
        if (up && z.monotone) {
            ++stats.skippedMotions;
            continue;
        }
        const auto meets = [&](const CutterShape& zone, const SoupIndex& soup) {
            return sweepHitsSoup(zone, m.a, m.b, soup);
        };

        // Fixtures.
        for (std::size_t f = 0; f < fixtures.size(); ++f) {
            if (meets(z.cutting[v], fixtures[f]) || meets(z.shank[v], fixtures[f])) {
                hit(m, i, EventKind::ToolHitsFixture, fixtureIds[f]);
            }
            if (meets(z.holder[v], fixtures[f])) {
                hit(m, i, EventKind::HolderHitsFixture, fixtureIds[f]);
                if (!m.rapid) {
                    holderLength(m, i, fixtures[f]);
                }
            }
        }

        // Part.
        bool rapidPart = false, shankPart = false, holderPart = false;
        if (!part.empty()) {
            if (m.rapid) {
                if (meets(z.cuttingSkin[v], part) || meets(z.shankSkin[v], part)) {
                    hit(m, i, EventKind::RapidIntoStock, kPart);
                    rapidPart = true;
                }
            } else if (meets(z.shank[v], part)) {
                hit(m, i, EventKind::ShankHitsPart, kPart);
                shankLength(m, i, part);
                shankPart = true;
            }
            if (meets(z.holder[v], part)) {
                hit(m, i, EventKind::HolderHitsPart, kPart);
                if (!m.rapid) {
                    holderLength(m, i, part);
                }
                holderPart = true;
            }
        }

        // Initial stock.
        if (!stock.empty()) {
            // Certain: the contact region (the zone's sweep box clipped to the stock) meets no earlier cutting sweep
            // (`limit`: motions before it).
            const auto certain = [&](const CutterShape& zone, std::uint32_t limit) {
                const Box3 box = grown(clipped(sweepBox(zone, m.a, m.b), stock.bounds()), kVerifyContactMm);
                return !cutBoxes.anyBefore(box, limit);
            };
            std::uint8_t bits = 0;
            if (m.rapid) {
                if (!rapidPart) {
                    // Certain when the contact of any one zone is.
                    bool any = false, sure = false;
                    for (const CutterShape* zone : {&z.cutting[v], &z.shank[v], &z.holder[v]}) {
                        if (meets(*zone, stock)) {
                            any = true;
                            sure = sure || certain(*zone, i);
                        }
                    }
                    if (any && sure) {
                        hit(m, i, EventKind::RapidIntoStock, kStock);
                    } else if (any) {
                        bits |= kDeferRapid;
                    }
                }
            } else {
                if (!shankPart && meets(z.shank[v], stock)) {
                    if (certain(z.shank[v], i + 1)) {
                        hit(m, i, EventKind::ShankHitsStock, kStock);
                        shankLength(m, i, stock);
                    } else {
                        bits |= kDeferShank;
                    }
                }
                if (!holderPart && meets(z.holder[v], stock)) {
                    if (certain(z.holder[v], i + 1)) {
                        hit(m, i, EventKind::HolderHitsStock, kStock);
                        holderLength(m, i, stock);
                    } else {
                        bits |= kDeferHolder;
                    }
                }
                const double dz = m.a.z - m.b.z;
                const double dx = m.b.x - m.a.x, dy = m.b.y - m.a.y;
                const bool plunge = dz > 0 && dz * dz > plungeSlope * plungeSlope * (dx * dx + dy * dy);
                if (plunge && z.deadRadius > 0 && meets(z.dead[v], stock)) {
                    if (certain(z.dead[v], i)) {
                        hit(m, i, EventKind::PlungeWithNonCentreCutting, kStock);
                    } else {
                        bits |= kDeferPlunge;
                    }
                }
            }
            if (bits != 0) {
                deferred.push_back({i, bits});
            }
        }
    }
    analyticProgress.set(1.0, "Checking moves");
    stats.deferred = deferred.size();

    // The cut-stock layer.
    double cell = 0;
    if (opts.cutStock && !deferred.empty()) {
        Result<CutStockResult> cs = runCutStock(in, motions, zones, deferred, jobs, ctx, cutProgress);
        if (!cs) {
            return tl::unexpected(cs.error());
        }
        cell = cs->cellMm;
        stats.cutStockRan = true;
        stats.cutStockCellMm = cell;
        for (RawHit& h : cs->hits) {
            h.layer = Layer::CutStock;
            raw.push_back(h);
        }
        for (const LengthHit& L : cs->lengths) {
            lengths[motions[L.motion].op].push_back(L);
        }
    }
    cutProgress.set(1.0, "Checking moves");

    // Runs: one event per run of consecutive moves of one operation with the same kind, obstacle and layer.
    std::sort(raw.begin(), raw.end(), [](const RawHit& a, const RawHit& b) {
        if (a.op != b.op) {
            return a.op < b.op;
        }
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        if (a.obstacle != b.obstacle) {
            return a.obstacle < b.obstacle;
        }
        if (a.layer != b.layer) {
            return a.layer < b.layer;
        }
        return a.motion < b.motion;
    });
    struct Run {
        RawHit first;
        std::uint32_t lastMove = 0;
        std::uint32_t moves = 0;
        double amount = 0;
    };
    std::vector<Run> runs;
    for (const RawHit& h : raw) {
        if (!runs.empty()) {
            Run& r = runs.back();
            if (r.first.op == h.op && r.first.kind == h.kind && r.first.obstacle == h.obstacle &&
                r.first.layer == h.layer && h.move <= r.lastMove + 1) {
                if (h.move != r.lastMove) {
                    ++r.moves;
                    r.lastMove = h.move;
                }
                r.amount = std::max(r.amount, h.amount);
                continue;
            }
        }
        runs.push_back({h, h.move, 1, h.amount});
    }

    std::vector<SimEvent> events;
    std::vector<std::size_t> eventOp; // the operation index of each event
    // The tip where a run's first motion first meets its obstacle (analytic hits): bisection along the motion.
    const auto firstContact = [&](const RawHit& h) -> Vec3 {
        const Motion& m = motions[h.motion];
        const ToolZones& z = zones[m.op];
        const int v = m.chord ? 1 : 0;
        std::vector<const CutterShape*> zs;
        const SoupIndex* soup = h.obstacle == kStock ? &stock : h.obstacle == kPart ? &part : nullptr;
        if (h.obstacle >= kFixture0) {
            for (std::size_t f = 0; f < fixtureIds.size(); ++f) {
                if (fixtureIds[f] == h.obstacle) {
                    soup = &fixtures[f];
                }
            }
        }
        switch (h.kind) {
        case EventKind::RapidIntoStock:
            if (h.obstacle == kPart) {
                zs = {&z.cuttingSkin[v], &z.shankSkin[v]};
            } else {
                zs = {&z.cutting[v], &z.shank[v], &z.holder[v]};
            }
            break;
        case EventKind::ToolHitsFixture:
            zs = {&z.cutting[v], &z.shank[v]};
            break;
        case EventKind::ShankHitsStock:
        case EventKind::ShankHitsPart:
            zs = {&z.shank[v]};
            break;
        case EventKind::PlungeWithNonCentreCutting:
            zs = {&z.dead[v]};
            break;
        default:
            zs = {&z.holder[v]};
            break;
        }
        if (soup == nullptr) {
            return m.a;
        }
        const auto hitsTo = [&](double t) {
            const Vec3 e{m.a.x + (m.b.x - m.a.x) * t, m.a.y + (m.b.y - m.a.y) * t, m.a.z + (m.b.z - m.a.z) * t};
            for (const CutterShape* zone : zs) {
                if (sweepHitsSoup(*zone, m.a, e, *soup)) {
                    return true;
                }
            }
            return false;
        };
        if (hitsTo(0.0)) {
            return m.a;
        }
        double t0 = 0, t1 = 1;
        for (int it = 0; it < 40; ++it) {
            const double mid = 0.5 * (t0 + t1);
            if (hitsTo(mid)) {
                t1 = mid;
            } else {
                t0 = mid;
            }
        }
        return {m.a.x + (m.b.x - m.a.x) * t1, m.a.y + (m.b.y - m.a.y) * t1, m.a.z + (m.b.z - m.a.z) * t1};
    };
    for (const Run& r : runs) {
        SimEvent e;
        e.kind = r.first.kind;
        e.severity = Severity::Error;
        e.operation = in.ops[r.first.op].id;
        e.moveIndex = r.first.move;
        e.amountMm = r.amount;
        if (r.first.kind == EventKind::TravelLimit) {
            const AxisTravel* ax = nullptr;
            for (const AxisTravel& a : axes) {
                if (a.index == r.first.obstacle) {
                    ax = &a;
                }
            }
            e.position = r.first.position;
            e.message = std::string("The tool goes ") + mmText(r.amount) + " mm beyond the machine's " +
                        (ax != nullptr ? std::string(1, ax->letter) : std::string("?")) + " travel" +
                        (ax != nullptr && ax->index == 2 ? " (the gauge line, with the tool's length)" : "") +
                        " with the work offset given" + runText(r.moves) + ".";
        } else {
            e.position = r.first.havePosition ? r.first.position : firstContact(r.first);
            e.message = message(r.first, r.moves, cell);
        }
        events.push_back(std::move(e));
        eventOp.push_back(r.first.op);
    }

    // Travel without a work offset: the program's extent against each axis's length.
    for (const AxisTravel& ax : axes) {
        const std::uint32_t i = firstOver[ax.index];
        if (in.wcsInMachine || i == UINT32_MAX) {
            continue;
        }
        const Motion& m = motions[i];
        SimEvent e;
        e.kind = EventKind::TravelLimit;
        e.severity = Severity::Error;
        e.operation = in.ops[m.op].id;
        e.moveIndex = m.move;
        e.position = m.b;
        const double extent = hi[ax.index] - lo[ax.index];
        e.amountMm = std::max(0.0, extent - (ax.max - ax.min));
        e.message = std::string("The program spans ") + mmText(extent) + " mm in " + ax.letter +
                    (ax.index == 2 ? " (the gauge line, with each tool's length)" : "") + ", more than the machine's " +
                    mmText(ax.max - ax.min) + " mm of travel: no work offset fits it (first exceeded here).";
        events.push_back(std::move(e));
        eventOp.push_back(m.op);
    }

    // Tool too short, per operation.
    for (std::size_t k = 0; k < lengths.size(); ++k) {
        std::vector<LengthHit>& L = lengths[k];
        if (L.empty()) {
            continue;
        }
        std::sort(L.begin(), L.end(), [](const LengthHit& a, const LengthHit& b) {
            return a.motion < b.motion || (a.motion == b.motion && a.holder > b.holder);
        });
        const ToolZones& z = zones[k];
        const lib::Tool* tool = in.ops[k].tool;
        for (const bool holder : {true, false}) {
            const LengthHit* first = nullptr;
            double need = 0;
            bool clearable = true;
            for (const LengthHit& h : L) {
                if (h.holder != holder) {
                    continue;
                }
                if (first == nullptr) {
                    first = &h;
                }
                if (h.clearable) {
                    need = std::max(need, h.need);
                } else {
                    clearable = false;
                }
            }
            if (first == nullptr) {
                continue;
            }
            const Motion& m = motions[first->motion];
            SimEvent e;
            e.kind = EventKind::ToolTooShort;
            e.severity = Severity::Error;
            e.operation = in.ops[k].id;
            e.moveIndex = m.move;
            e.position = m.a;
            if (holder) {
                const double extra = clearable ? roundUpTenth(need + kStickOutMarginMm) : 0.0;
                e.amountMm = extra;
                const double now = z.shankTop;
                if (!z.hasHolder) {
                    e.message = "The tool is too short: above its top the spindle meets material; a tool " +
                                mmText(extra) + " mm longer is needed.";
                } else if (!clearable) {
                    e.message = "The tool is too short and no stick-out clears the holder: material lies within the "
                                "shank's reach above the flutes. Use a longer or narrower tool or a slimmer holder.";
                } else {
                    e.message = "The tool sticks out too little: the holder needs " + mmText(extra) +
                                " mm more to clear (" + mmText(kStickOutMarginMm) +
                                " mm clearance included): minimum safe stick-out " + mmText(now + extra) + " mm, now " +
                                mmText(now) + " mm.";
                    if (tool != nullptr && tool->overallLength > 0) {
                        const double shankD = tool->shankDiameter > 0 ? tool->shankDiameter : tool->diameter;
                        const double allowed = tool->overallLength - kMinGripShankDiameters * shankD;
                        if (now + extra > allowed) {
                            e.message += " This tool allows at most " + mmText(std::max(allowed, 0.0)) +
                                         " mm (its overall length less " + mmText(kMinGripShankDiameters) +
                                         " shank diameters held): use a longer tool or holder.";
                        }
                    }
                }
            } else {
                const double extra = roundUpTenth(std::max(0.0, need - z.fluteTop));
                e.amountMm = extra;
                e.message = "The flutes are too short: the cut reaches " + mmText(need) +
                            " mm above the tool tip, the flutes " + mmText(z.fluteTop) + " mm (" + mmText(extra) +
                            " mm more needed).";
            }
            events.push_back(std::move(e));
            eventOp.push_back(k);
        }
    }

    // Operations in program order (by index: ids may repeat or be nil), then move, then kind; equal keys keep the
    // order they were made in (obstacles in order).
    std::vector<std::size_t> order(events.size());
    for (std::size_t e = 0; e < order.size(); ++e) {
        order[e] = e;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
        const SimEvent& a = events[x];
        const SimEvent& b = events[y];
        if (eventOp[x] != eventOp[y]) {
            return eventOp[x] < eventOp[y];
        }
        if (a.moveIndex != b.moveIndex) {
            return a.moveIndex < b.moveIndex;
        }
        return kindRank(a.kind) < kindRank(b.kind);
    });
    std::vector<SimEvent> sorted;
    sorted.reserve(events.size());
    for (std::size_t e : order) {
        sorted.push_back(std::move(events[e]));
    }
    if (statsOut != nullptr) {
        *statsOut = stats;
    }
    ctx.progress.set(1.0, "Checking moves");
    return sorted;
}

} // namespace ray::sim::detail
