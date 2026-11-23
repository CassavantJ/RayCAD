// The slot operation's plan (slot.h, task T-055): parameters, checks, slot recognition and its proof, depths and the
// passes linking joins. Every rule is stated in slot.h; this file follows them in its order. Deterministic: integer
// nanometres from the face boundary on, geom2d for every offset, sweep and boolean, dmath for trigonometry.
#include "op_params.h"
#include "slot.h"
#include "slot_common.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <set>
#include <utility>

// Returns the error of a Status or Result expression.
#define RAY_SLOT_TRY(expr)                                                                                             \
    do {                                                                                                               \
        if (auto ray_slot_status_ = (expr); !ray_slot_status_) {                                                       \
            return tl::unexpected(std::move(ray_slot_status_.error()));                                                \
        }                                                                                                              \
    } while (false)

namespace ray::cam::ops {

namespace g = geom2d;
using g::Coord;
using g::Pt;
using mill::failure;
using mill::invalid;
using mill::mmText;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

// ---------- Small geometry ----------

Pt spanStart(const g::Path& p, std::size_t k)
{
    return k == 0 ? p.start : p.spans[k - 1].end;
}

double spanLength(const g::Path& p, std::size_t k)
{
    g::Path one;
    one.start = spanStart(p, k);
    one.spans = {p.spans[k]};
    return g::length(one);
}

double dist(Pt a, Pt b)
{
    const double dx = static_cast<double>(b.x - a.x), dy = static_cast<double>(b.y - a.y);
    return std::sqrt(dx * dx + dy * dy);
}

Pt midpoint(Pt a, Pt b)
{
    return Pt{g::roundNm((static_cast<double>(a.x) + static_cast<double>(b.x)) / 2),
              g::roundNm((static_cast<double>(a.y) + static_cast<double>(b.y)) / 2)};
}

// ---------- Recognition (slot.h GEOMETRY) ----------

// A run of the floor's boundary: spans [first, first + count) of the rotated path, one kind of thing.
struct Run {
    std::size_t first = 0, count = 0;
    bool wall = true;
    bool arc = false;    // one circle: same direction, centre and radius within kWidthMatch
    bool convex = false; // counter-clockwise (the floor's boundary runs counter-clockwise)
    Pt centre;
    Coord radius = 0;
    double sweep = 0; // radians
    Pt a, b;          // its ends
};

bool sameCircle(const g::Span& s, const g::Span& t)
{
    return s.kind != g::SpanKind::Line && s.kind == t.kind &&
           std::abs(static_cast<double>(s.centre.x - t.centre.x)) <= static_cast<double>(kWidthMatch) &&
           std::abs(static_cast<double>(s.centre.y - t.centre.y)) <= static_cast<double>(kWidthMatch) &&
           std::abs(static_cast<double>(s.radius - t.radius)) <= static_cast<double>(kWidthMatch);
}

std::string floorWords(Pt at, Nm z)
{
    return "the slot floor at " + mill::atXYZ(at, z);
}

} // namespace

Result<SlotShape> recognizeSlot(const kernel::Shape& shape, kernel::SubId face, const Xform& frame, JobContext& ctx)
{
    auto fb = g::faceBoundary(shape, face, frame);
    if (!fb) {
        return invalid("The face is not a flat face square to the setup's Z axis, so it cannot be a slot floor: pick "
                       "the floor of the slot");
    }
    const g::Region& floor = fb->region;
    const g::Bounds bb = g::bounds(floor);
    const Pt centreOfBox{(bb.min.x + bb.max.x) / 2, (bb.min.y + bb.max.y) / 2};
    const std::string here = floorWords(centreOfBox, fb->z);
    if (!fb->facesUp) {
        return invalid("The face at " + mill::atXYZ(centreOfBox, fb->z) +
                       " looks down the setup's Z axis: pick the floor of a slot, which looks up");
    }
    if (!floor.holes.empty()) {
        return failure(ErrorCode::ToolpathFailed, "The floor at " + mill::atXYZ(centreOfBox, fb->z) +
                                                      " has an island in it, so it is not a slot: machine it as a 2D "
                                                      "pocket (pocket2d)");
    }
    const g::Path& outer = floor.outer;
    if (outer.spans.size() < 3) {
        return failure(ErrorCode::ToolpathFailed,
                       "The outline of " + here + " is not a slot (a round floor is a hole or a round pocket)");
    }
    auto slice = g::sliceRegions(shape, frame, fb->z, ctx);
    if (!slice) {
        return tl::unexpected(slice.error());
    }
    // Wall or open, per span (the floor is on the left, the material outside it on the right).
    const std::size_t n = outer.spans.size();
    std::vector<bool> wall(n);
    {
        double s = 0;
        for (std::size_t k = 0; k < n; ++k) {
            RAY_SLOT_TRY(ctx.cancel.check());
            const double len = spanLength(outer, k);
            const double mid = s + len / 2;
            const Pt at = g::pointAt(outer, mid);
            const Vec2 t = g::tangentAt(outer, mid);
            const Pt probe{at.x + g::roundNm(t.y * static_cast<double>(kWallProbe)),
                           at.y - g::roundNm(t.x * static_cast<double>(kWallProbe))};
            wall[k] = g::locate(probe, *slice) != g::Where::Outside;
            s += len;
        }
    }
    // Runs, starting where two spans do not belong together (a closed end may be split at the path's start).
    const auto together = [&](std::size_t i, std::size_t j) {
        if (wall[i] != wall[j]) {
            return false;
        }
        if (!wall[i]) {
            return true; // open spans form one run
        }
        return sameCircle(outer.spans[i], outer.spans[j]);
    };
    std::size_t rot = n;
    for (std::size_t k = 0; k < n; ++k) {
        if (!together((k + n - 1) % n, k)) {
            rot = k;
            break;
        }
    }
    if (rot == n) {
        return failure(ErrorCode::ToolpathFailed,
                       "The outline of " + here + " is one circle or has no wall along it, so it is not a slot");
    }
    g::Path path;
    path.closed = true;
    path.start = spanStart(outer, rot);
    for (std::size_t k = 0; k < n; ++k) {
        path.spans.push_back(outer.spans[(rot + k) % n]);
    }
    std::vector<bool> w(n);
    for (std::size_t k = 0; k < n; ++k) {
        w[k] = wall[(rot + k) % n];
    }
    std::vector<Run> runs;
    for (std::size_t k = 0; k < n;) {
        Run r;
        r.first = k;
        r.wall = w[k];
        std::size_t e = k + 1;
        while (e < n && w[e] == w[k] && (!w[k] || sameCircle(path.spans[e - 1], path.spans[e]))) {
            ++e;
        }
        r.count = e - k;
        r.a = spanStart(path, k);
        r.b = path.spans[e - 1].end;
        const g::Span& s0 = path.spans[k];
        r.arc = s0.kind != g::SpanKind::Line;
        if (r.arc) {
            r.convex = s0.kind == g::SpanKind::ArcCCW;
            r.centre = s0.centre;
            r.radius = s0.radius;
            for (std::size_t j = k; j < e; ++j) {
                r.sweep += g::sweepOf(spanStart(path, j), path.spans[j]);
            }
        }
        runs.push_back(r);
        k = e;
    }
    // The ends: open runs (one straight side each) and convex half circles of the smallest radius.
    std::vector<std::size_t> opens, caps;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const Run& r = runs[i];
        if (!r.wall) {
            if (r.count != 1 || r.arc) {
                return failure(ErrorCode::ToolpathFailed,
                               "An open side of " + here +
                                   " (where no wall rises) is not one straight line, so the "
                                   "face is not a slot: machine it as a 2D pocket (pocket2d)");
            }
            opens.push_back(i);
        } else if (r.arc && r.convex && std::abs(r.sweep - kPi) <= kHalfTurnTol) {
            caps.push_back(i);
        }
    }
    if (opens.size() > 2) {
        return failure(ErrorCode::ToolpathFailed, "The outline of " + here + " has " + std::to_string(opens.size()) +
                                                      " open sides; a slot has at most two (its ends)");
    }
    const std::size_t needCaps = 2 - opens.size();
    if (caps.size() < needCaps) {
        return failure(ErrorCode::ToolpathFailed,
                       "The outline of " + here +
                           " is not a slot: a slot's ends are half circles of its width (what a centre-line cut "
                           "leaves) or open sides; machine this face as a 2D pocket (pocket2d)");
    }
    std::stable_sort(caps.begin(), caps.end(),
                     [&](std::size_t i, std::size_t j) { return runs[i].radius < runs[j].radius; });
    std::vector<std::size_t> ends = opens;
    ends.insert(ends.end(), caps.begin(), caps.begin() + static_cast<std::ptrdiff_t>(needCaps));
    std::sort(ends.begin(), ends.end());
    const Run& e0 = runs[ends[0]];
    const Run& e1 = runs[ends[1]];
    // Rails: everything between the ends, walls only, on both sides.
    const std::size_t between = ends[1] - ends[0] - 1;
    const std::size_t around = runs.size() - ends[1] - 1 + ends[0];
    if (between == 0 || around == 0) {
        return failure(ErrorCode::ToolpathFailed, "The outline of " + here + " has no side wall between its ends");
    }
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (i != ends[0] && i != ends[1] && !runs[i].wall) {
            return failure(ErrorCode::ToolpathFailed, "A side of " + here + " has no wall: it is not a slot");
        }
    }
    const auto widthOf = [](const Run& r) {
        return r.wall ? 2 * r.radius : g::roundNm(dist(r.a, r.b));
    };
    const Coord w0 = widthOf(e0), w1 = widthOf(e1);
    if (std::abs(static_cast<double>(w0 - w1)) > static_cast<double>(kWidthMatch)) {
        return failure(ErrorCode::ToolpathFailed,
                       "The two ends of " + here + " differ in width (" + mmText(w0) + " and " + mmText(w1) +
                           " mm), so it is not a slot: machine it as a 2D pocket (pocket2d)");
    }
    SlotShape out;
    out.width = std::max(w0, w1);
    out.floor = floor;
    out.floorZ = fb->z;
    const Coord r = g::roundNm(static_cast<double>(out.width) / 2);
    const Pt c0 = e0.wall ? e0.centre : midpoint(e0.a, e0.b);
    const Pt c1 = e1.wall ? e1.centre : midpoint(e1.a, e1.b);
    // The left rail runs from end 0 to end 1 (the boundary is counter-clockwise, the floor on its left).
    g::Path rail;
    rail.start = e0.b;
    for (std::size_t k = runs[ends[0] + 1].first; k < e1.first; ++k) {
        rail.spans.push_back(path.spans[k]);
    }
    g::Path centre;
    if (rail.spans.size() == 1 && rail.spans[0].kind == g::SpanKind::Line) {
        centre.start = c0;
        centre.spans = {g::Span{g::SpanKind::Line, c1, {}, 0}};
    } else {
        auto pieces = g::offsetOpen(rail, r, g::Side::Left, ctx);
        if (!pieces) {
            return tl::unexpected(pieces.error());
        }
        if (pieces->size() != 1) {
            return failure(ErrorCode::ToolpathFailed,
                           "The side walls of " + here + " come closer than its width somewhere: it is not a slot");
        }
        centre = pieces->front();
        const auto end = centre.spans.empty() ? centre.start : centre.spans.back().end;
        if (dist(centre.start, c0) > static_cast<double>(kEndMatch) || dist(end, c1) > static_cast<double>(kEndMatch)) {
            return failure(ErrorCode::ToolpathFailed,
                           "The side walls of " + here + " are not one slot width apart along the slot");
        }
    }
    if (centre.spans.empty() || g::length(centre) < 1000) {
        return failure(ErrorCode::ToolpathFailed, "The slot at " + mill::atXYZ(c0, fb->z) +
                                                      " has no length (a round floor is a hole or a round pocket)");
    }
    out.centre = centre;
    out.open[0] = !e0.wall;
    out.open[1] = !e1.wall;
    return out;
}

g::Path extendOpenEnds(const SlotShape& slot, Coord runOut)
{
    g::Path p = slot.centre;
    const double len = g::length(p);
    if (slot.open[0]) {
        const Vec2 t = g::tangentAt(p, 0);
        const Pt s{p.start.x - g::roundNm(t.x * static_cast<double>(runOut)),
                   p.start.y - g::roundNm(t.y * static_cast<double>(runOut))};
        p.spans.insert(p.spans.begin(), g::Span{g::SpanKind::Line, p.start, {}, 0});
        p.start = s;
    }
    if (slot.open[1]) {
        const Vec2 t = g::tangentAt(slot.centre, len);
        const Pt e = slot.centre.spans.back().end;
        p.spans.push_back(g::Span{g::SpanKind::Line,
                                  Pt{e.x + g::roundNm(t.x * static_cast<double>(runOut)),
                                     e.y + g::roundNm(t.y * static_cast<double>(runOut))},
                                  {},
                                  0});
    }
    return g::canonicalize(p);
}

std::vector<Nm> slotDepths(Nm z0, Nm f, Nm stepDown)
{
    std::vector<Nm> out;
    if (f >= z0 || stepDown <= 0) {
        return out;
    }
    const Nm depth = z0 - f;
    const Nm n = std::max<Nm>(1, (depth + stepDown - 1) / stepDown);
    for (Nm k = 1; k < n; ++k) {
        out.push_back(z0 - g::roundNm(static_cast<double>(k) * static_cast<double>(depth) / static_cast<double>(n)));
    }
    out.push_back(f);
    return out;
}

namespace {

// ---------- Parameters ----------

struct Params {
    double stepDown = 1, floorStock = 0, rampAngle = 3, minRampRun = 0, widthTolerance = 0.05;
};

const OpSchema* slotSchema()
{
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("slot"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = slotSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The slot operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    out.stepDown = p->number("stepDown");
    out.floorStock = p->number("floorStock");
    out.rampAngle = p->number("rampAngle");
    out.minRampRun = p->number("minRampRun");
    out.widthTolerance = p->number("widthTolerance");
    return out;
}

// ---------- The plan ----------

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx) {}

    Result<SlotPlan> run()
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        RAY_SLOT_TRY(checkTool());
        RAY_SLOT_TRY(mill::checkSpindle(in_));
        RAY_SLOT_TRY(checkFeeds());
        RAY_SLOT_TRY(mill::checkHeights(in_.op.heights, true));
        RAY_SLOT_TRY(findSlots());
        RAY_SLOT_TRY(passes());
        return std::move(plan_);
    }

private:
    const lib::Tool& tool() const { return in_.tool; }

    Status checkTool()
    {
        const lib::Tool& t = tool();
        if (!(t.diameter > 0) || !std::isfinite(t.diameter) || t.diameter > 1000) {
            return invalid("The tool has no diameter");
        }
        if (t.type != lib::ToolType::FlatEndMill) {
            return invalid("The tool is a " + std::string(mill::toolWord(t.type)) +
                           "; a centre-line slot is cut with a flat end mill (a slot's floor is flat and its walls "
                           "meet it square)");
        }
        return {};
    }

    Status checkFeeds()
    {
        const CuttingData& c = in_.op.cutting;
        auto cut = mill::feedUm(c.feedCut, "cutting feed", false);
        if (!cut) {
            return tl::unexpected(cut.error());
        }
        auto plunge = mill::feedUm(c.feedPlunge, "plunge feed", true);
        if (!plunge) {
            return tl::unexpected(plunge.error());
        }
        auto ramp = mill::feedUm(c.feedRamp, "ramp feed", true);
        if (!ramp) {
            return tl::unexpected(ramp.error());
        }
        feedCut_ = *cut;
        plan_.options.feeds.plunge = *plunge;
        plan_.options.feeds.ramp = *ramp;
        return {};
    }

    std::string selectedWords(std::size_t i) const
    {
        const kernel::SubId sub = in_.geom.selected[i];
        const std::size_t part = static_cast<std::size_t>(in_.geom.selectedShapeIndex[i]);
        std::string text = "Selection " + std::to_string(i + 1);
        if (sub.kind == kernel::SubKind::Face) {
            if (auto s = kernel::surfaceInfo(in_.geom.partShapes[part], sub)) {
                const double* m = in_.geom.wcsFromModel.m;
                const Vec3 p = s->centroid;
                const Vec3 c{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                             m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
                text += " (the face centred at X" + mmText(c.x) + " Y" + mmText(c.y) + " Z" + mmText(c.z) + ")";
            }
        }
        return text;
    }

    Status findSlots()
    {
        const ResolvedGeometry& geo = in_.geom;
        if (geo.selected.empty()) {
            return invalid("Pick the slots to cut: select the floor face of each slot");
        }
        if (geo.selectedShapeIndex.size() != geo.selected.size()) {
            return failure(ErrorCode::Internal, "The selection and its part indices differ in length");
        }
        const Coord toolD = toNm(tool().diameter);
        const Coord toolR = toNm(tool().diameter / 2);
        const Coord widthTol = toNm(p_.widthTolerance);
        const Nm top = mill::stockTopNm(geo);
        const Nm z0 = top + toNm(in_.op.heights.top);
        const Nm step = toNm(p_.stepDown);
        const mill::ToolShape shape = mill::toolShape(tool());
        std::set<std::pair<std::size_t, kernel::SubId>> taken;
        for (std::size_t i = 0; i < geo.selected.size(); ++i) {
            RAY_SLOT_TRY(ctx_.cancel.check());
            const kernel::SubId sub = geo.selected[i];
            const int partIndex = geo.selectedShapeIndex[i];
            if (partIndex < 0 || static_cast<std::size_t>(partIndex) >= geo.partShapes.size()) {
                return failure(ErrorCode::Internal, "A selection lies on no part body");
            }
            const std::size_t part = static_cast<std::size_t>(partIndex);
            if (sub.kind != kernel::SubKind::Face) {
                return invalid(selectedWords(i) + " is not a face: pick the floor face of each slot");
            }
            if (!taken.insert({part, sub}).second) {
                continue;
            }
            auto slot = recognizeSlot(geo.partShapes[part], sub, geo.wcsFromModel, ctx_);
            if (!slot) {
                Error e = slot.error();
                if (e.code != ErrorCode::Cancelled && e.code != ErrorCode::KernelFailure) {
                    e.message = selectedWords(i) + ": " + e.message;
                }
                return tl::unexpected(std::move(e));
            }
            PlannedSlot ps;
            ps.part = part;
            ps.shape = std::move(*slot);
            const SlotShape& s = ps.shape;
            // A slot is named by the lower end of its centre line (x, then y).
            const Pt name = std::min(s.centre.start, s.centre.spans.back().end);
            const std::string at = "the " + mmText(s.width) + " mm slot at " + mill::atXY(name);
            // The tool against the slot's width.
            if (toolD > s.width + kWiderTol) {
                return invalid("The " + mmText(tool().diameter) + " mm " + mill::toolWord(tool().type) +
                               " is wider than " + at + ": it would cut into both walls");
            }
            if (toolD < s.width - widthTol) {
                return invalid(
                    "The " + mmText(tool().diameter) + " mm " + mill::toolWord(tool().type) + " is narrower than " +
                    at +
                    ": a centre-line slot is as wide as its tool; use a tool of the slot's width, or machine "
                    "the slot as a 2D pocket (pocket2d)");
            }
            if (toolD < s.width - kWiderTol) {
                plan_.warnings.push_back("The " + mmText(tool().diameter) + " mm tool cuts " + at + " " +
                                         mmText(s.width - toolD) + " mm narrower than the model");
            }
            ps.path = extendOpenEnds(s, toolR + kOpenEndRunOut);
            // (1) The floor lies inside what the slot's width sweeps along the centre line.
            {
                const std::vector<g::Path> one{ps.path};
                auto swept = g::sweptArea(one, g::roundNm(static_cast<double>(s.width) / 2) + kSweepTol, ctx_);
                if (!swept) {
                    return tl::unexpected(swept.error());
                }
                auto left = g::subtract(g::Regions{s.floor}, *swept, ctx_);
                if (!left) {
                    return tl::unexpected(left.error());
                }
                if (g::area(*left) >= mill::kContactMinArea) {
                    return failure(ErrorCode::ToolpathFailed,
                                   "The floor of " + at + " is not what its centre line and width sweep (near " +
                                       mill::atXY(mill::contactPoint(*left)) +
                                       "), so it is not a centre-line slot: machine it as a 2D pocket (pocket2d)");
                }
            }
            // Depths.
            const Nm f = s.floorZ + toNm(p_.floorStock);
            if (f >= z0) {
                return invalid("The floor of " + at + " (Z" + mmText(f) +
                               " with the floor stock) is not below the top "
                               "height (Z" +
                               mmText(z0) + "): lower the top height or the floor stock");
            }
            ps.depths = slotDepths(z0, f, step);
            const Nm deep = z0 - f;
            if (shape.fluteLength > 0 && deep > shape.fluteLength + kWallTol) {
                return invalid("The slot at " + mill::atXY(name) + " is " + mmText(deep) +
                               " mm deep from the top height, but the tool's flutes are " + mmText(tool().fluteLength) +
                               " mm long");
            }
            const double stickOut = in_.op.tool.stickOut;
            const Nm need = mill::highestNm(geo) - f + toNm(mill::kHolderClearanceMm);
            if (stickOut > 0 && toNm(stickOut) < need) {
                return invalid("The tool sticks out " + mmText(stickOut) + " mm, but the slot at " + mill::atXY(name) +
                               " needs " + mmText(need) + " mm for the holder to stay " +
                               mmText(mill::kHolderClearanceMm) + " mm above the stock and the part");
            }
            // (2) The tool clears the part at every height it passes.
            RAY_SLOT_TRY(clears(ps, z0, toolR));
            plan_.slots.push_back(std::move(ps));
        }
        return {};
    }

    Status clears(const PlannedSlot& ps, Nm z0, Coord toolR)
    {
        const ResolvedGeometry& geo = in_.geom;
        const SlotShape& s = ps.shape;
        std::vector<Nm> heights = {s.floorZ};
        heights.insert(heights.end(), ps.depths.begin(), ps.depths.end());
        for (Nm z = s.floorZ + kProbeStep; z < z0; z += kProbeStep) {
            heights.push_back(z);
        }
        auto faces = mill::faceHeights(geo.partShapes[ps.part], geo.wcsFromModel, s.floorZ, z0, ctx_);
        if (!faces) {
            return tl::unexpected(faces.error());
        }
        heights.insert(heights.end(), faces->begin(), faces->end());
        std::sort(heights.begin(), heights.end());
        heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
        const std::vector<g::Path> one{ps.path};
        auto swept = g::sweptArea(one, toolR - kWallTol, ctx_);
        if (!swept) {
            return tl::unexpected(swept.error());
        }
        for (const std::vector<kernel::Shape>* bodies : {&geo.partShapes, &geo.fixtureShapes}) {
            for (const kernel::Shape& body : *bodies) {
                if (body.isNull()) {
                    continue;
                }
                for (const Nm z : heights) {
                    RAY_SLOT_TRY(ctx_.cancel.check());
                    auto hit = mill::contactAt(body, geo.wcsFromModel, z, *swept, std::nullopt, ctx_);
                    if (!hit) {
                        return tl::unexpected(hit.error());
                    }
                    if (!hit->empty()) {
                        return failure(ErrorCode::ToolpathFailed,
                                       "Cutting the " + mmText(s.width) + " mm slot at " +
                                           mill::atXY(std::min(s.centre.start, s.centre.spans.back().end)) +
                                           ", the tool would cut into the part or a fixture at " +
                                           mill::atXYZ(mill::contactPoint(*hit), z) +
                                           ": the slot is narrower there than at its floor, or something stands over "
                                           "it");
                    }
                }
            }
        }
        return {};
    }

    Status passes()
    {
        const ResolvedGeometry& geo = in_.geom;
        const Heights& h = in_.op.heights;
        const Nm top = mill::stockTopNm(geo);
        link::Options& o = plan_.options;
        o.clearance = top + toNm(h.clearance);
        o.retract = top + toNm(h.retract);
        o.feedPlane = top + toNm(h.feedPlane);
        o.toolRadius = toNm(tool().diameter / 2);
        o.centreCutting = false;
        o.maxStayDown = toNm(tool().diameter);
        o.stayDownEntry = true; // the next depth ramps in from the cut, without a retract (link.h)
        o.entryOrder = {EntryKind::Ramp};
        o.helixDiameter = 0;
        o.helixMinDiameter = 0;
        o.rampAngleDeg = p_.rampAngle;
        o.minRampRun = toNm(p_.minRampRun);
        o.entryClearance = kRampClearance;
        o.spindleClockwise = in_.op.cutting.clockwise;
        o.from = Pt{0, 0};
        plan_.stock = link::stockOf(geo);
        for (std::size_t s = 0; s < plan_.slots.size(); ++s) {
            const PlannedSlot& ps = plan_.slots[s];
            for (std::size_t k = 0; k < ps.depths.size(); ++k) {
                link::Pass pass = link::passFromPath(ps.path, ps.depths[k], feedCut_);
                pass.reversible = true;
                pass.cluster = static_cast<int>(s);
                pass.level = static_cast<int>(k);
                plan_.passes.push_back(std::move(pass));
            }
        }
        return {};
    }

    const GenerateInput& in_;
    JobContext& ctx_;
    Params p_;
    SlotPlan plan_;
    std::int32_t feedCut_ = 0;
};

} // namespace

Result<SlotPlan> planSlot(const GenerateInput& in, JobContext& ctx)
{
    try {
        return Planner(in, ctx).run();
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the slot operation");
    }
}

#undef RAY_SLOT_TRY

} // namespace ray::cam::ops
