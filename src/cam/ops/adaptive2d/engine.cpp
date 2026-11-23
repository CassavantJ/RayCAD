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
    if (auto s = ctx.cancel.check(); !s) {
        return tl::unexpected(s.error());
    }

    // ---------- the grid ----------
    const double toolMm = mm(in.toolDiameter);
    const double tolMm = mm(in.tolerance);
    const double gridDouble = fork::Adaptive2d::MIN_STEP_CLIPPER / tolMm / std::min(1.0, in.stepover * toolMm);
    const std::int64_t g = std::max<std::int64_t>(1, roundHalfEven(gridDouble));
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

    std::list<fork::AdaptiveOutput> results = engine.Execute(stock, regions, cleared);
    if (auto s = ctx.cancel.check(); !s) {
        return tl::unexpected(s.error());
    }

    // ---------- results ----------
    Output out;
    const fork::Stats& fs = engine.GetStats();
    out.stats.gridUnitsPerMm = g;
    out.stats.steps = fs.steps;
    out.stats.cutAreaCalls = fs.cutAreaCalls;
    out.stats.passes = fs.passes;
    out.stats.linkSearches = fs.linkSearches;
    out.stats.linkSearchSteps = fs.linkSearchSteps;
    out.stats.linkSearchLimitHits = fs.linkSearchLimitHits;
    out.stats.engageSearches = fs.engageSearches;
    out.stats.linkSideProbes = fs.linkSideProbes;
    out.stats.failedPassesInARow = fs.failedPassesInARow;
    if (fs.regions == 0) {
        return tl::unexpected(failed("adaptive: the tool (diameter " + mmText(in.toolDiameter) +
                                     " mm) does not fit anywhere in the area" +
                                     (in.stockToLeave > 0 ? " less the stock to leave" : "")));
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
        for (const fork::TPath& tp : r.AdaptivePaths) {
            if (tp.points.empty()) {
                continue;
            }
            Path path;
            path.role = roleOf(tp.role);
            path.motion = motionOf(tp.motion);
            path.points.reserve(tp.points.size());
            path.engagementDeg.reserve(tp.points.size());
            for (std::size_t i = 0; i < tp.points.size(); i++) {
                if (!inBox(tp.points[i])) {
                    return tl::unexpected(failed("adaptive: internal fault: a path point off the area"));
                }
                path.points.push_back(pt(tp.points[i]));
                double d = i > 0 && i < tp.depth.size() ? tp.depth[i] : -1.0;
                path.engagementDeg.push_back(i == 0 ? -1.0 : engagementDeg(d, radius));
            }
            region.paths.push_back(std::move(path));
        }
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
                s += pointText(p.points[k]) + " " + mdeg(k < p.engagementDeg.size() ? p.engagementDeg[k] : -1) + "\n";
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
