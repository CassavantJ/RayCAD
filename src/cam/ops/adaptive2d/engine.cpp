// The boundary of the constant-engagement engine (task T-060; engine.h): input checks, the conversion between
// nanometres and the engine's grid (exact integer arithmetic, rounded half to even), the run of the fork
// (engine_adaptive.cpp), and its flags turned into errors and warnings.
#include "engine.h"

#include "engine_adaptive.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

namespace ray::cam::adaptive2d {
namespace {

using Clipper2Lib::Path64;
using Clipper2Lib::Paths64;
using Clipper2Lib::Point64;

constexpr std::int64_t kNmPerMmInt = 1'000'000;

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {}};
}
Error failed(std::string message)
{
    return Error{ErrorCode::ToolpathFailed, std::move(message), {}};
}

// n / d rounded half to even, d > 0, exact.
std::int64_t divRoundHalfEven(std::int64_t n, std::int64_t d)
{
    std::int64_t q = n / d;
    std::int64_t r = n % d; // same sign as n
    std::int64_t twice = 2 * (r < 0 ? -r : r);
    if (twice > d || (twice == d && (q % 2 != 0))) {
        q += n < 0 ? -1 : 1;
    }
    return q;
}

// x rounded half to even (a finite double well inside the int64 range).
std::int64_t roundHalfEven(double x)
{
    double f = std::floor(x);
    double diff = x - f;
    std::int64_t i = static_cast<std::int64_t>(f);
    if (diff > 0.5 || (diff == 0.5 && (i % 2 != 0))) {
        ++i;
    }
    return i;
}

// Nanometres to grid units (g per mm) and back, rounded half to even, exact: n = q * 1e6 + r with |r| < 1e6, so no
// product exceeds 1e6 * g.
std::int64_t toGrid(Coord nm, std::int64_t g)
{
    std::int64_t q = nm / kNmPerMmInt;
    std::int64_t r = nm % kNmPerMmInt;
    return q * g + divRoundHalfEven(r * g, kNmPerMmInt);
}
Coord toNmFromGrid(std::int64_t u, std::int64_t g)
{
    std::int64_t q = u / g;
    std::int64_t r = u % g;
    return q * kNmPerMmInt + divRoundHalfEven(r * kNmPerMmInt, g);
}

double mm(Coord nm)
{
    return double(nm) / double(kNmPerMmInt);
}

// A length in mm for messages, from nanometres: rounded to 1 um (half to even), trailing zeros dropped, no locale.
std::string mmText(Coord nm)
{
    std::int64_t um = divRoundHalfEven(nm, 1000);
    const bool negative = um < 0;
    std::uint64_t a = negative ? std::uint64_t(-(um + 1)) + 1 : std::uint64_t(um);
    std::string out = (negative ? "-" : "") + std::to_string(a / 1000);
    if (std::uint64_t frac = a % 1000; frac != 0) {
        std::string digits = std::to_string(frac);
        digits.insert(0, 3 - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0') {
            digits.pop_back();
        }
        out += "." + digits;
    }
    return out;
}

// The closed loops of regions as grid polygons (arcs flattened with kInputSag).
Paths64 toGridPaths(const Regions& regions, std::int64_t g)
{
    Paths64 out;
    for (const geom2d::Region& region : regions) {
        auto add = [&](const geom2d::Path& p) {
            Path64 path;
            for (const Pt& pt : geom2d::polygonize(p, kInputSag)) {
                path.emplace_back(toGrid(pt.x, g), toGrid(pt.y, g));
            }
            if (path.size() >= 3) {
                out.push_back(std::move(path));
            }
        };
        add(region.outer);
        for (const geom2d::Path& hole : region.holes) {
            add(hole);
        }
    }
    return out;
}

Status checkRegions(const Regions& regions, const char* what)
{
    for (const geom2d::Region& r : regions) {
        if (auto s = geom2d::checkRange(r.outer); !s) {
            return tl::unexpected(invalid(std::string(what) + ": " + s.error().message));
        }
        for (const geom2d::Path& h : r.holes) {
            if (auto s = geom2d::checkRange(h); !s) {
                return tl::unexpected(invalid(std::string(what) + ": " + s.error().message));
            }
        }
    }
    return {};
}

geom2d::Bounds boundsOf(const Regions& regions, geom2d::Bounds b)
{
    for (const geom2d::Region& r : regions) {
        geom2d::Bounds rb = geom2d::bounds(r);
        if (rb.empty) {
            continue;
        }
        if (b.empty) {
            b = rb;
            continue;
        }
        b.min.x = std::min(b.min.x, rb.min.x);
        b.min.y = std::min(b.min.y, rb.min.y);
        b.max.x = std::max(b.max.x, rb.max.x);
        b.max.y = std::max(b.max.y, rb.max.y);
    }
    return b;
}

// Grid polygons (Clipper's output: outer loops and holes, any orientation) to canonical regions in nanometres.
Result<Regions> toRegions(const Paths64& paths, std::int64_t g, JobContext& ctx)
{
    std::vector<geom2d::Path> loops;
    for (const Path64& p : paths) {
        if (p.size() < 3) {
            continue;
        }
        geom2d::Path path;
        path.closed = true;
        path.start = {toNmFromGrid(p[0].x, g), toNmFromGrid(p[0].y, g)};
        for (std::size_t i = 1; i <= p.size(); i++) {
            const Point64& q = p[i % p.size()];
            path.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, {toNmFromGrid(q.x, g), toNmFromGrid(q.y, g)}});
        }
        path = geom2d::canonicalize(std::move(path));
        if (path.spans.size() >= 3) {
            loops.push_back(std::move(path));
        }
    }
    auto nesting = geom2d::nest(loops, ctx);
    if (!nesting) {
        return tl::unexpected(nesting.error());
    }
    return geom2d::regionsByNesting(loops, *nesting);
}

Motion motionOf(int m)
{
    switch (m) {
    case fork::mtLinkClear:
        return Motion::LinkClear;
    case fork::mtLinkNotClear:
        return Motion::LinkRetract;
    default:
        return Motion::Cut;
    }
}
Role roleOf(int r)
{
    switch (r) {
    case fork::prLeadIn:
        return Role::LeadIn;
    case fork::prLeadOut:
        return Role::LeadOut;
    case fork::prLink:
        return Role::Link;
    case fork::prFinish:
        return Role::Finish;
    default:
        return Role::Pass;
    }
}

// The engine's radial depth estimate (grid units) as an engagement angle, degrees; -1 stays -1 (no estimate).
double engagementDeg(double depth, double radius)
{
    if (depth < -0.5) {
        return -1;
    }
    double t = std::max(0.0, std::min(2.0, depth / radius));
    return dmath::acos(1 - t) * 180.0 / std::numbers::pi;
}

std::string pointText(Pt p)
{
    return std::to_string(p.x) + " " + std::to_string(p.y);
}

double pathLength(const Path& p)
{
    double len = 0;
    for (std::size_t i = 1; i < p.points.size(); i++) {
        const double dx = double(p.points[i].x - p.points[i - 1].x), dy = double(p.points[i].y - p.points[i - 1].y);
        len += std::sqrt(dx * dx + dy * dy);
    }
    return len;
}

// T-063: a region's paths tidied. A run of tool-down moves (consecutive LinkClear paths: a lead-out's stretch through
// cleared area, the link, the next lead-in's) shorter than minRun is not worth being one: its leads' stretches stay
// cuts. Then the pieces of one engine path with the same motion are joined again, and paths of no length (upstream's
// links from a point to itself) are left out.
void tidyPaths(std::vector<Path>& paths, std::vector<std::size_t>& origin, Coord minRun)
{
    for (std::size_t i = 0; i < paths.size();) {
        if (paths[i].motion != Motion::LinkClear) {
            i++;
            continue;
        }
        std::size_t j = i;
        double run = 0;
        while (j < paths.size() && paths[j].motion == Motion::LinkClear) {
            run += pathLength(paths[j]);
            j++;
        }
        if (run < double(minRun)) {
            for (std::size_t k = i; k < j; k++) {
                if (paths[k].role == Role::LeadIn || paths[k].role == Role::LeadOut) {
                    paths[k].motion = Motion::Cut;
                }
            }
        }
        i = j;
    }
    std::vector<Path> kept;
    std::vector<std::size_t> keptOrigin;
    for (std::size_t i = 0; i < paths.size(); i++) {
        Path& p = paths[i];
        bool still = true;
        for (const Pt& q : p.points) {
            still = still && q == p.points.front();
        }
        if (still) {
            continue;
        }
        if (!kept.empty() && keptOrigin.back() == origin[i] && kept.back().motion == p.motion &&
            kept.back().points.back() == p.points.front()) {
            kept.back().points.insert(kept.back().points.end(), p.points.begin() + 1, p.points.end());
            kept.back().engagementDeg.insert(kept.back().engagementDeg.end(), p.engagementDeg.begin() + 1,
                                             p.engagementDeg.end());
            continue;
        }
        kept.push_back(std::move(p));
        keptOrigin.push_back(origin[i]);
    }
    paths = std::move(kept);
    origin = std::move(keptOrigin);
}

} // namespace

double angleForStepover(double stepover)
{
    double c = std::max(-1.0, std::min(1.0, 1 - 2 * stepover));
    return dmath::acos(c) * 180.0 / std::numbers::pi;
}

double stepoverForAngle(double degrees)
{
    return (1 - dmath::cos(degrees * std::numbers::pi / 180.0)) / 2;
}

std::int64_t gridUnitsPerMm(const Input& in)
{
    const double gridDouble =
        fork::Adaptive2d::MIN_STEP_CLIPPER / mm(in.tolerance) / std::min(1.0, in.stepover * mm(in.toolDiameter));
    return std::max<std::int64_t>(1, roundHalfEven(gridDouble));
}

Pt onGrid(Pt p, std::int64_t g)
{
    return {toNmFromGrid(toGrid(p.x, g), g), toNmFromGrid(toGrid(p.y, g), g)};
}

Coord finishingAllowance(const Input& in)
{
    if (!in.finishingPass) {
        return 0;
    }
    // As Adaptive2d::Execute: toolRadiusScaled = Rnd(D g / 2), finishPassOffsetScaled = Rnd(radius x stepover / 10)
    const std::int64_t g = gridUnitsPerMm(in);
    const double radius = std::round(mm(in.toolDiameter) * double(g) / 2); // positive: Rnd is std::round
    return toNmFromGrid(std::int64_t(std::round(radius * in.stepover * (1 / 10.))), g);
}

Result<Output> runEngine(const Input& in, JobContext& ctx)
{
    // ---------- checks ----------
    if (in.regions.empty()) {
        return tl::unexpected(invalid("adaptive: no area to clear"));
    }
    if (in.toolDiameter <= 0) {
        return tl::unexpected(invalid("adaptive: the tool diameter must be positive"));
    }
    if (!(in.stepover >= kMinStepover && in.stepover <= kMaxStepover)) {
        return tl::unexpected(invalid("adaptive: the stepover must be between 1% and 100% of the tool diameter"));
    }
    if (in.tolerance < kMinTolerance || in.tolerance > kMaxTolerance) {
        return tl::unexpected(invalid("adaptive: the tolerance must be between 0.01 mm and 1 mm"));
    }
    if (in.stockToLeave < 0 || in.helixDiameter < 0 || in.helixMinDiameter < 0) {
        return tl::unexpected(invalid("adaptive: stock to leave and helix diameters must not be negative"));
    }
    if (!(in.keepToolDownRatio >= 0)) {
        return tl::unexpected(invalid("adaptive: the keep-tool-down ratio must not be negative"));
    }
    if (in.toolDiameter > geom2d::kMaxCoord || in.stockToLeave > geom2d::kMaxCoord ||
        in.helixDiameter > geom2d::kMaxCoord || in.helixMinDiameter > geom2d::kMaxCoord) {
        return tl::unexpected(invalid("adaptive: a size is beyond the coordinate range"));
    }
    for (auto [regions, what] :
         {std::pair{&in.regions, "area"}, std::pair{&in.cleared, "cleared area"}, std::pair{&in.stock, "stock"}}) {
        if (auto s = checkRegions(*regions, what); !s) {
            return tl::unexpected(s.error());
        }
    }
    // T-063: the seeds' and region points' coordinates (the regions' range bounds them, through the grid box below)
    auto inRange = [](Pt p) {
        return p.x >= -geom2d::kMaxCoord && p.x <= geom2d::kMaxCoord && p.y >= -geom2d::kMaxCoord &&
               p.y <= geom2d::kMaxCoord;
    };
    for (const Input::Seed& s : in.seeds) {
        if (!inRange(s.at) || !inRange(s.toward) || !inRange(s.from) || s.radius < 0 || s.radius > geom2d::kMaxCoord) {
            return tl::unexpected(invalid("adaptive: an entry seed is beyond the coordinate range"));
        }
    }
    for (const Pt& p : in.regionsAt) {
        if (!inRange(p)) {
            return tl::unexpected(invalid("adaptive: a region point is beyond the coordinate range"));
        }
    }
    if (auto s = ctx.cancel.check(); !s) {
        return tl::unexpected(s.error());
    }

    // ---------- the grid ----------
    const double toolMm = mm(in.toolDiameter);
    const double tolMm = mm(in.tolerance);
    const std::int64_t g = gridUnitsPerMm(in);
    // The extent of everything on the grid, the overshoot outside the stock and the tool included.
    geom2d::Bounds all = boundsOf(in.stock, boundsOf(in.cleared, boundsOf(in.regions, {})));
    if (all.empty) {
        return tl::unexpected(invalid("adaptive: no area to clear"));
    }
    const Coord margin = 4 * in.toolDiameter + in.stockToLeave + 1'000'000;
    const double extentGrid =
        (double(std::max(all.max.x - all.min.x, all.max.y - all.min.y)) + 2 * double(margin)) / 1e6 * double(g);
    if (!(extentGrid < double(kMaxGridExtent))) {
        return tl::unexpected(invalid("adaptive: the area is too large for the engine's grid at this tolerance (" +
                                      std::to_string(g) + " units per mm; at most " +
                                      mmText(kMaxGridExtent * kNmPerMmInt / g) + " mm across)"));
    }

    // ---------- the run ----------
    Paths64 regions = toGridPaths(in.regions, g);
    Paths64 cleared = toGridPaths(in.cleared, g);
    Paths64 stock = toGridPaths(in.stock, g);
    bool forceInsideOut = !in.enterFromOutside || stock.empty();
    if (stock.empty()) {
        // Upstream finishes only the boundary that lies in stock that is not cleared: without a stock outline, the
        // box of everything grown by the tool diameter stands in (the tool never leaves the regions: forceInsideOut).
        Coord grow = 2 * in.toolDiameter;
        Path64 box;
        box.emplace_back(toGrid(all.min.x - grow, g), toGrid(all.min.y - grow, g));
        box.emplace_back(toGrid(all.max.x + grow, g), toGrid(all.min.y - grow, g));
        box.emplace_back(toGrid(all.max.x + grow, g), toGrid(all.max.y + grow, g));
        box.emplace_back(toGrid(all.min.x - grow, g), toGrid(all.max.y + grow, g));
        stock.push_back(std::move(box));
    }

    fork::Adaptive2d engine;
    engine.toolDiameter = toolMm;
    engine.stepOverFactor = in.stepover;
    engine.tolerance = tolMm;
    engine.stockToLeave = mm(in.stockToLeave);
    engine.finishingProfile = in.finishingPass;
    engine.helixRampTargetDiameter = mm(in.helixDiameter);
    engine.helixRampMinDiameter = mm(in.helixMinDiameter);
    engine.keepToolDownDistRatio = in.keepToolDownRatio;
    engine.forceInsideOut = forceInsideOut;
    engine.opType = fork::otClearingInside;
    engine.scaleFactor = g;
    engine.limits.linkSearchSteps = in.limits.linkSearchSteps;
    engine.limits.linkSideSteps = in.limits.linkSideSteps;
    engine.limits.passes = in.limits.passes;
    engine.limits.pointsPerPass = in.limits.pointsPerPass;
    engine.limits.steps = in.limits.steps;
    engine.limits.failedEngagements = in.limits.failedEngagements;
    engine.limits.leadSteps = in.limits.leadSteps;
    engine.cancel = &ctx.cancel;
    engine.progress = [&ctx](double f) {
        ctx.progress.set(f, "adaptive clearing");
    };
    // T-063: entries made by the caller, regions to clear, helix or not
    engine.helixEntryAllowed = in.helixEntry;
    for (const Input::Seed& s : in.seeds) {
        Point64 at(toGrid(s.at.x, g), toGrid(s.at.y, g));
        double dx = double(s.toward.x - s.at.x), dy = double(s.toward.y - s.at.y);
        double len = std::sqrt(dx * dx + dy * dy);
        Clipper2Lib::PointD dir = len > 0 ? Clipper2Lib::PointD(dx / len, dy / len) : Clipper2Lib::PointD(1.0, 0.0);
        engine.entrySeeds.emplace_back(at, dir);
    }
    for (const Pt& p : in.regionsAt) {
        engine.onlyRegionsAt.emplace_back(toGrid(p.x, g), toGrid(p.y, g));
    }

    std::list<fork::AdaptiveOutput> results = engine.Execute(stock, regions, cleared);
    if (auto s = ctx.cancel.check(); !s) {
        return tl::unexpected(s.error());
    }

    // ---------- results ----------
    Output out;
    const fork::Stats& fs = engine.GetStats();
    out.stats.gridUnitsPerMm = g;
    out.stats.engineRuns = 1;
    out.stats.steps = fs.steps;
    out.stats.cutAreaCalls = fs.cutAreaCalls;
    out.stats.passes = fs.passes;
    out.stats.linkSearches = fs.linkSearches;
    out.stats.linkSearchSteps = fs.linkSearchSteps;
    out.stats.linkSearchLimitHits = fs.linkSearchLimitHits;
    out.stats.engageSearches = fs.engageSearches;
    out.stats.linkSideProbes = fs.linkSideProbes;
    out.stats.failedPassesInARow = fs.failedPassesInARow;
    auto fitsNowhere = [&]() {
        // T-063: with the narrowest slot the tool can clear (its tool-centre area is empty below that width)
        const Coord allowance = finishingAllowance(in);
        const Coord width = in.toolDiameter + 2 * allowance + 2 * in.stockToLeave;
        std::string why = "the tool diameter";
        if (allowance > 0) {
            why += (in.stockToLeave > 0 ? ", " : " and ") + std::string("twice the finishing allowance of ") +
                   mmText(allowance) + " mm";
        }
        if (in.stockToLeave > 0) {
            why += " and twice the stock to leave";
        }
        return failed("adaptive: the tool (diameter " + mmText(in.toolDiameter) +
                      " mm) does not fit anywhere in the area" +
                      (in.stockToLeave > 0 ? " less the stock to leave" : "") + "; a slot must be wider than " +
                      mmText(width) + " mm for it (" + why + ")");
    };
    if (fs.regions == 0) {
        return tl::unexpected(fitsNowhere());
    }
    const double radius = double(engine.GetToolRadiusScaled());
    // Every output point must lie in the grown box of the input; anything else is an internal fault.
    const std::int64_t loX = toGrid(all.min.x - margin, g), hiX = toGrid(all.max.x + margin, g);
    const std::int64_t loY = toGrid(all.min.y - margin, g), hiY = toGrid(all.max.y + margin, g);
    auto inBox = [&](const Point64& p) {
        return p.x >= loX && p.x <= hiX && p.y >= loY && p.y <= hiY;
    };
    auto pt = [&](const Point64& p) {
        return Pt{toNmFromGrid(p.x, g), toNmFromGrid(p.y, g)};
    };

    Paths64 clearedAll;
    for (const fork::AdaptiveOutput& r : results) {
        auto where = [&]() {
            geom2d::Bounds b;
            for (const Path64& p : r.ToolBoundPaths) {
                for (const Point64& q : p) {
                    Pt n = pt(q);
                    if (b.empty) {
                        b.min = b.max = n;
                        b.empty = false;
                    }
                    b.min.x = std::min(b.min.x, n.x);
                    b.min.y = std::min(b.min.y, n.y);
                    b.max.x = std::max(b.max.x, n.x);
                    b.max.y = std::max(b.max.y, n.y);
                }
            }
            return "(" + mmText((b.min.x + b.max.x) / 2) + ", " + mmText((b.min.y + b.max.y) / 2) + ") mm";
        };
        if (r.LimitReached) {
            return tl::unexpected(failed("adaptive: stopped at the limit on " + r.LimitName + " in the region around " +
                                         where() + "; the result would be incomplete"));
        }
        if (r.TooManyFailedEngagements) {
            return tl::unexpected(failed("adaptive: gave up in the region around " + where() + " after " +
                                         std::to_string(in.limits.failedEngagements) +
                                         " passes in a row that cut nothing"));
        }
        if (r.Stopped) {
            // Not cancelled (checked above) and no limit: cannot happen; refuse rather than return part of a region.
            return tl::unexpected(failed("adaptive: the region around " + where() + " stopped early"));
        }
        Region region;
        auto bounds = toRegions(r.ToolBoundPaths, g, ctx);
        if (!bounds) {
            return tl::unexpected(bounds.error());
        }
        region.toolBounds = std::move(*bounds);
        if (r.StartPointNotFound) {
            Coord minHelix = in.helixMinDiameter > 0 ? in.helixMinDiameter : in.toolDiameter / 8;
            std::string message = "adaptive: no entry into the region around " + where() +
                                  ": there is no cleared area to start from and the smallest helix (diameter " +
                                  mmText(minHelix) + " mm) does not fit; it needs a ramp or a pre-drilled hole";
            if (!in.allowUnentered) {
                return tl::unexpected(failed(message));
            }
            out.warnings.push_back({Warning::Code::NotEntered, message});
            out.regions.push_back(std::move(region));
            continue;
        }
        if (!inBox(r.HelixCenterPoint) || !inBox(r.StartPoint)) {
            return tl::unexpected(failed("adaptive: internal fault: an entry point off the area"));
        }
        region.entry.kind = r.HelixEntry ? EntryMode::Helix : EntryMode::FromCleared;
        region.entry.centre = pt(r.HelixCenterPoint);
        region.entry.start = pt(r.StartPoint);
        region.entry.radius = r.HelixEntry ? toNmFromGrid(r.HelixRadius, g) : 0;
        if (!r.HelixEntry) {
            region.entry.centre = region.entry.start;
        }
        if (r.SeedEntry >= 0 && std::size_t(r.SeedEntry) < in.seeds.size()) {
            // T-063: an entry the caller made; its own nanometre values (the grid point is the same within rounding)
            const Input::Seed& seed = in.seeds[std::size_t(r.SeedEntry)];
            region.entry = Entry{seed.kind, seed.from, seed.radius, seed.at};
        }
        // T-063: each path starts where the previous one ended (a gap is closed by the next path's first segment, no
        // estimate), and a lead's stretch through cleared area (TPath::airFrom..airTo) is a path of its own, LinkClear
        std::optional<Pt> last;
        std::vector<std::size_t> origin; // per output path: the engine path it came from
        for (const fork::TPath& tp : r.AdaptivePaths) {
            if (tp.points.empty()) {
                continue;
            }
            const std::size_t enginePath = std::size_t(&tp - r.AdaptivePaths.data());
            for (const Point64& q : tp.points) {
                if (!inBox(q)) {
                    return tl::unexpected(failed("adaptive: internal fault: a path point off the area"));
                }
            }
            auto emit = [&](std::size_t from, std::size_t to, Motion motion) {
                Path path;
                path.role = roleOf(tp.role);
                path.motion = motion;
                path.points.reserve(to - from + 2);
                path.engagementDeg.reserve(to - from + 2);
                const bool gap = last && *last != pt(tp.points[from]);
                if (gap && motion != Motion::LinkRetract) {
                    path.points.push_back(*last);
                    path.engagementDeg.push_back(-1.0);
                }
                for (std::size_t i = from; i <= to; i++) {
                    double d = i > from && i < tp.depth.size() ? tp.depth[i] : -1.0;
                    // a segment measured in pieces whose estimates differ keeps them apart (TPath::pieces)
                    if (i > from && i < tp.pieces.size() && tp.pieces[i].size() > 1) {
                        const auto& pieces = tp.pieces[i];
                        for (std::size_t k = 0; k + 1 < pieces.size(); k++) {
                            path.points.push_back(pt(pieces[k].first));
                            path.engagementDeg.push_back(engagementDeg(pieces[k].second, radius));
                        }
                        d = pieces.back().second;
                    }
                    // a retract keeps its two points: it leaves from where the tool is
                    path.points.push_back(gap && i == from && motion == Motion::LinkRetract ? *last : pt(tp.points[i]));
                    path.engagementDeg.push_back(path.points.size() == 1 ? -1.0 : engagementDeg(d, radius));
                }
                last = path.points.back();
                region.paths.push_back(std::move(path));
                origin.push_back(enginePath);
            };
            const std::size_t n = tp.points.size();
            const Motion motion = motionOf(tp.motion);
            if (motion == Motion::Cut && tp.airFrom >= 0 && tp.airTo > tp.airFrom && std::size_t(tp.airTo) < n) {
                const std::size_t a = std::size_t(tp.airFrom), b = std::size_t(tp.airTo);
                if (a > 0) {
                    emit(0, a, Motion::Cut);
                }
                emit(a, b, Motion::LinkClear);
                if (b + 1 < n) {
                    emit(b, n - 1, Motion::Cut);
                }
            } else {
                emit(0, n - 1, motion);
            }
        }
        tidyPaths(region.paths, origin, in.toolDiameter / 12);
        region.returnClear = r.ReturnMotionType == fork::mtLinkClear;
        region.clearedAreaMm2 = r.ClearedArea;
        if (r.LeadPathFailed) {
            out.warnings.push_back(
                {Warning::Code::LeadFailed, "adaptive: a lead-in or lead-out in the region around " + where() +
                                                " did not reach cleared area; a pass starts or ends without one"});
        }
        if (r.FinishingLeadInFailed) {
            out.warnings.push_back(
                {Warning::Code::FinishingLeadFailed, "adaptive: a finishing pass in the region around " + where() +
                                                         " was left out: no lead-in reached it"});
        }
        if (r.UnclearedAreaRemains) {
            out.warnings.push_back({Warning::Code::UnclearedArea, "adaptive: area is left uncut in the region around " +
                                                                      where() + ": no engagement point was found"});
        }
        if (r.FailedToSetUpFinishingPass) {
            out.warnings.push_back({Warning::Code::FinishingSetup,
                                    "adaptive: part of the region around " + where() +
                                        " was not cleared to within the finishing allowance; the finishing pass may "
                                        "cut more than the target there"});
        }
        clearedAll.insert(clearedAll.end(), r.ClearedPaths.begin(), r.ClearedPaths.end());
        out.regions.push_back(std::move(region));
    }
    // T-063: no region at all to clear, though there is material in the area (the only tool-centre area was the one
    // around the stock, which needs no clearing): the tool fits nowhere in the area
    if (out.regions.empty() && in.regionsAt.empty()) {
        auto material = in.stock.empty() ? Result<Regions>(in.regions) : geom2d::intersect(in.regions, in.stock, ctx);
        if (!material) {
            return tl::unexpected(material.error());
        }
        auto left = in.cleared.empty() ? std::move(material) : geom2d::subtract(*material, in.cleared, ctx);
        if (!left) {
            return tl::unexpected(left.error());
        }
        if (geom2d::area(*left) > 1e6) { // 1 um^2
            return tl::unexpected(fitsNowhere());
        }
    }
    auto clearedRegions = toRegions(Clipper2Lib::Union(clearedAll, Clipper2Lib::FillRule::NonZero), g, ctx);
    if (!clearedRegions) {
        return tl::unexpected(clearedRegions.error());
    }
    out.cleared = std::move(*clearedRegions);
    if (auto s = ctx.cancel.check(); !s) {
        return tl::unexpected(s.error());
    }
    return out;
}

std::string toText(const Output& out)
{
    std::string s = "ADAPTIVE2D 1\ngrid " + std::to_string(out.stats.gridUnitsPerMm) + "\n";
    auto mdeg = [](double deg) {
        return deg < 0 ? std::string("-1") : std::to_string(roundHalfEven(deg * 1000));
    };
    for (std::size_t i = 0; i < out.regions.size(); i++) {
        const Region& r = out.regions[i];
        s += "region " + std::to_string(i) + " entry " + std::to_string(int(r.entry.kind)) + " " +
             pointText(r.entry.centre) + " " + std::to_string(r.entry.radius) + " " + pointText(r.entry.start) +
             " return " + (r.returnClear ? "1" : "0") + " paths " + std::to_string(r.paths.size()) + "\n";
        for (const Path& p : r.paths) {
            s += "path " + std::to_string(int(p.role)) + " " + std::to_string(int(p.motion)) + " " +
                 std::to_string(p.points.size()) + "\n";
            for (std::size_t k = 0; k < p.points.size(); k++) {
                s += pointText(p.points[k]) + " " + mdeg(k < p.engagementDeg.size() ? p.engagementDeg[k] : -1);
                // T-063: planAdaptive's reduced-feed flag, when there is one
                if (!p.reducedFeed.empty()) {
                    s += k < p.reducedFeed.size() && p.reducedFeed[k] != 0 ? " r" : " -";
                }
                s += "\n";
            }
        }
        s += "toolbounds\n" + geom2d::toText(r.toolBounds);
    }
    for (const Warning& w : out.warnings) {
        s += "warning " + std::to_string(int(w.code)) + " " + w.message + "\n";
    }
    s += "cleared\n" + geom2d::toText(out.cleared);
    return s;
}

} // namespace ray::cam::adaptive2d
