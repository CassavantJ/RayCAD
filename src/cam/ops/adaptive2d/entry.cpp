// Adaptive clearing with every region entered (task T-063; entry.h): the order of entry kinds per region, the ramp
// along a region's longest straight run, pre-drilled holes, the engine's runs on the regions still to enter, and the
// output made ready for the operation (ramp paths, hole links, links checked, reduced-feed flags).
#include "entry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace ray::cam::adaptive2d {
namespace {

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {}};
}
Error failed(std::string message)
{
    return Error{ErrorCode::ToolpathFailed, std::move(message), {}};
}

// A length in mm for messages, from nanometres: rounded to 1 um (half to even), trailing zeros dropped, no locale.
std::string mmText(Coord nm)
{
    Coord um = nm / 1000;
    const Coord rem = nm % 1000;
    const Coord twice = 2 * (rem < 0 ? -rem : rem);
    if (twice > 1000 || (twice == 1000 && um % 2 != 0)) {
        um += nm < 0 ? -1 : 1;
    }
    const bool negative = um < 0;
    const std::uint64_t a = negative ? std::uint64_t(-(um + 1)) + 1 : std::uint64_t(um);
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

std::string whereText(const Regions& r)
{
    geom2d::Bounds b;
    for (const geom2d::Region& region : r) {
        geom2d::Bounds rb = geom2d::bounds(region);
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
    return "(" + mmText((b.min.x + b.max.x) / 2) + ", " + mmText((b.min.y + b.max.y) / 2) + ") mm";
}

// ---------- the order of attempts ----------
enum class Attempt : std::uint8_t { HelixFull, HelixAny, Ramp, PreDrill };

bool isHelix(Attempt a)
{
    return a == Attempt::HelixFull || a == Attempt::HelixAny;
}

// The order's kinds as attempts: a helix followed by other kinds is tried at full size first and at any size (down
// to the minimum) after the rest, when a smaller helix exists.
std::vector<Attempt> attemptsFor(const std::vector<EntryKind>& order, bool smallerHelix)
{
    std::vector<Attempt> out;
    std::optional<std::size_t> helixAt;
    bool ramp = false, drill = false;
    for (EntryKind k : order) {
        if (k == EntryKind::Helix && !helixAt) {
            helixAt = out.size();
            out.push_back(Attempt::HelixAny);
        } else if (k == EntryKind::Ramp && !ramp) {
            ramp = true;
            out.push_back(Attempt::Ramp);
        } else if (k == EntryKind::PreDrill && !drill) {
            drill = true;
            out.push_back(Attempt::PreDrill);
        }
    }
    if (helixAt && *helixAt + 1 < out.size()) {
        out[*helixAt] = Attempt::HelixFull;
        if (smallerHelix) {
            out.push_back(Attempt::HelixAny);
        }
    }
    return out;
}

// ---------- the ramp: the longest straight run of a region ----------
struct D2 {
    double x = 0, y = 0;
};
struct Chord {
    double length = -1;
    double centred = -1; // distance across from its middle to the area's edge (the smaller side)
    D2 a, b;
};

// The crossings of the line {p : p . n = c} with the loops' edges, as positions along `u` (half-open rule at
// vertices), sorted.
std::vector<double> crossings(const std::vector<std::vector<D2>>& loops, D2 u, D2 n, double c)
{
    std::vector<double> out;
    for (const std::vector<D2>& loop : loops) {
        const std::size_t m = loop.size();
        for (std::size_t i = 0; i < m; i++) {
            const D2 p = loop[i], q = loop[(i + 1) % m];
            const double bp = p.x * n.x + p.y * n.y, bq = q.x * n.x + q.y * n.y;
            if ((bp <= c) == (bq <= c)) {
                continue;
            }
            const double t = (c - bp) / (bq - bp);
            const double ap = p.x * u.x + p.y * u.y, aq = q.x * u.x + q.y * u.y;
            out.push_back(ap + t * (aq - ap));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The best chord of the loops along direction `angle`: the longest interval over scanlines across the extent (64
// evenly spaced, then 32 about the best), its centredness for the tie-break.
Chord bestChord(const std::vector<std::vector<D2>>& loops, double angle, double tolLength)
{
    const D2 u{dmath::cos(angle), dmath::sin(angle)};
    const D2 n{-u.y, u.x};
    double lo = 1e300, hi = -1e300;
    for (const std::vector<D2>& loop : loops) {
        for (const D2& p : loop) {
            const double b = p.x * n.x + p.y * n.y;
            lo = std::min(lo, b);
            hi = std::max(hi, b);
        }
    }
    Chord best;
    if (!(hi > lo)) {
        return best;
    }
    auto consider = [&](double c) {
        const std::vector<double> xs = crossings(loops, u, n, c);
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            const double len = xs[i + 1] - xs[i];
            if (len < best.length - tolLength) {
                continue;
            }
            // centredness: the crossing interval of the perpendicular line through the chord's middle that holds c
            const double mid = 0.5 * (xs[i] + xs[i + 1]);
            const std::vector<double> ys = crossings(loops, n, u, mid);
            double centred = 0;
            for (std::size_t j = 0; j + 1 < ys.size(); j += 2) {
                if (ys[j] <= c && c <= ys[j + 1]) {
                    centred = std::min(c - ys[j], ys[j + 1] - c);
                }
            }
            if (len > best.length + tolLength || centred > best.centred) {
                best.length = len;
                best.centred = centred;
                best.a = {xs[i] * u.x + c * n.x, xs[i] * u.y + c * n.y};
                best.b = {xs[i + 1] * u.x + c * n.x, xs[i + 1] * u.y + c * n.y};
            }
        }
    };
    const int coarse = 64, fine = 32;
    const double step = (hi - lo) / coarse;
    for (int k = 1; k < coarse; k++) {
        consider(lo + step * k);
    }
    if (best.length > 0) {
        const double c0 = best.a.x * n.x + best.a.y * n.y;
        for (int k = -fine / 2; k <= fine / 2; k++) {
            const double c = c0 + step * k / (fine / 2);
            if (k != 0 && c > lo && c < hi) {
                consider(c);
            }
        }
    }
    return best;
}

struct RampPlan {
    Pt a, b; // the ramp runs from a to b; the passes start at b
};

// The ramp of a region: its tool-centre area's best chord (entry.h, Ramp), ends `margin` inside, at least `minRun`
// long, cut to a centred piece of maxRun (0: no limit). nullopt when none fits.
std::optional<RampPlan> planRamp(const Regions& area, Coord minRun, Coord maxRun, double margin)
{
    std::vector<std::vector<D2>> loops;
    for (const geom2d::Region& r : area) {
        auto add = [&](const geom2d::Path& p) {
            std::vector<D2> loop;
            for (const Pt& q : geom2d::polygonize(p, kInputSag)) {
                loop.push_back({double(q.x), double(q.y)});
            }
            if (loop.size() >= 3) {
                loops.push_back(std::move(loop));
            }
        };
        add(r.outer);
        for (const geom2d::Path& h : r.holes) {
            add(h);
        }
    }
    if (loops.empty()) {
        return std::nullopt;
    }
    // Directions: the axes and diagonals, then those of the 16 longest edges (distinct by half a degree).
    struct Edge {
        double length, angle;
    };
    std::vector<Edge> edges;
    for (const std::vector<D2>& loop : loops) {
        for (std::size_t i = 0; i < loop.size(); i++) {
            const D2 p = loop[i], q = loop[(i + 1) % loop.size()];
            const double dx = q.x - p.x, dy = q.y - p.y;
            double a = dmath::atan2(dy, dx);
            if (a < 0) {
                a += std::numbers::pi;
            }
            if (a >= std::numbers::pi) {
                a -= std::numbers::pi;
            }
            edges.push_back({std::sqrt(dx * dx + dy * dy), a});
        }
    }
    std::stable_sort(edges.begin(), edges.end(), [](const Edge& l, const Edge& r) { return l.length > r.length; });
    std::vector<double> angles{0, std::numbers::pi / 2, std::numbers::pi / 4, 3 * std::numbers::pi / 4};
    const double distinct = 0.5 * std::numbers::pi / 180;
    for (const Edge& e : edges) {
        if (angles.size() >= 20) {
            break;
        }
        bool same = false;
        for (double a : angles) {
            double d = std::fabs(a - e.angle);
            d = std::min(d, std::numbers::pi - d);
            same = same || d < distinct;
        }
        if (!same) {
            angles.push_back(e.angle);
        }
    }
    const double tolLength = 1'000; // 1 um: chords this close in length are equally long
    Chord best;
    for (double angle : angles) {
        Chord c = bestChord(loops, angle, tolLength);
        if (c.length > best.length + tolLength || (c.length >= best.length - tolLength && c.centred > best.centred)) {
            best = c;
        }
    }
    if (best.length <= 2 * margin || best.centred < margin / 5) {
        return std::nullopt;
    }
    // ends `margin` inside, at most maxRun long (centred)
    const double len = best.length;
    double from = margin, to = len - margin;
    if (maxRun > 0 && to - from > double(maxRun)) {
        const double mid = 0.5 * len;
        from = mid - 0.5 * double(maxRun);
        to = mid + 0.5 * double(maxRun);
    }
    if (to - from < double(minRun)) {
        return std::nullopt;
    }
    const D2 d{(best.b.x - best.a.x) / len, (best.b.y - best.a.y) / len};
    Pt a{geom2d::roundNm(best.a.x + d.x * from), geom2d::roundNm(best.a.y + d.y * from)};
    Pt b{geom2d::roundNm(best.a.x + d.x * to), geom2d::roundNm(best.a.y + d.y * to)};
    if (b < a) {
        std::swap(a, b); // deterministic: the ramp starts at the lower end (x, then y)
    }
    return RampPlan{a, b};
}

// ---------- one region's plan ----------
struct Pending {
    std::size_t index = 0;         // in the output's regions
    Pt point;                      // a point inside its tool-centre area (Input::regionsAt)
    std::vector<Attempt> attempts; // from the order
    std::size_t next = 0;          // the attempt to make
    std::optional<RampPlan> ramp;  // planned for the next run
    std::optional<std::size_t> hole;
    bool helix = false;
    bool done = false;
    std::vector<std::string> notFitting; // what did not fit, for the fallback warning or the error
};

Result<Regions> uniteAll(Regions a, const Regions& b, JobContext& ctx)
{
    if (b.empty()) {
        return a;
    }
    if (a.empty()) {
        return b;
    }
    return geom2d::unite(a, b, ctx);
}

void addStats(Stats& to, const Stats& s)
{
    to.steps += s.steps;
    to.cutAreaCalls += s.cutAreaCalls;
    to.passes += s.passes;
    to.linkSearches += s.linkSearches;
    to.linkSearchSteps += s.linkSearchSteps;
    to.linkSearchLimitHits += s.linkSearchLimitHits;
    to.linkSideProbes += s.linkSideProbes;
    to.engageSearches += s.engageSearches;
    to.failedPassesInARow = std::max(to.failedPassesInARow, s.failedPassesInARow);
    to.engineRuns += s.engineRuns;
}

const char* attemptName(Attempt a)
{
    switch (a) {
    case Attempt::HelixFull:
    case Attempt::HelixAny:
        return "helix";
    case Attempt::Ramp:
        return "ramp";
    case Attempt::PreDrill:
        return "pre-drilled hole";
    }
    return "";
}

} // namespace

// ---------- the output made ready (entry.h, HOW: per region) ----------
std::int64_t cutLinksThatCut(std::vector<Path>& paths)
{
    std::int64_t made = 0;
    for (Path& p : paths) {
        if (p.motion == Motion::LinkClear && p.role == Role::Link) {
            bool cuts = false;
            for (double e : p.engagementDeg) {
                cuts = cuts || e > kLinkCutDeg;
            }
            if (cuts) {
                p.motion = Motion::Cut;
                made++;
            }
        }
    }
    return made;
}

void flagReducedFeed(Path& p, double targetDeg)
{
    const double limitDeg = kReducedFeedFactor * targetDeg;
    const std::size_t n = p.points.size();
    p.reducedFeed.assign(n, 0);
    if (p.motion != Motion::Cut || n < 2) {
        return;
    }
    if (p.role == Role::Ramp) {
        std::fill(p.reducedFeed.begin() + 1, p.reducedFeed.end(), std::uint8_t(1));
        return;
    }
    // arc length to each point (nm)
    std::vector<double> s(n, 0.0);
    for (std::size_t i = 1; i < n; i++) {
        const double dx = double(p.points[i].x - p.points[i - 1].x), dy = double(p.points[i].y - p.points[i - 1].y);
        s[i] = s[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    const double reach = kReducedFeedReach * 1e6;
    for (std::size_t i = 1; i < n; i++) {
        const double e = i < p.engagementDeg.size() ? p.engagementDeg[i] : -1.0;
        // the last segment of a lead-in ends where the pass and its regulation begin: above the target it counts
        const bool high = e > limitDeg || (p.role == Role::LeadIn && i + 1 == n && e > targetDeg);
        if (!high) {
            continue;
        }
        // segment i spans s[i - 1]..s[i]; flag every segment overlapping s[i - 1] - reach .. s[i] + reach
        for (std::size_t j = i; j >= 1 && s[j] > s[i - 1] - reach; j--) {
            p.reducedFeed[j] = 1;
        }
        for (std::size_t j = i; j < n && s[j - 1] < s[i] + reach; j++) {
            p.reducedFeed[j] = 1;
        }
    }
}

Result<Output> planAdaptive(const Input& in, const EntryOptions& opt, JobContext& ctx)
{
    // ---------- checks ----------
    if (!in.seeds.empty() || !in.regionsAt.empty() || !in.helixEntry) {
        return tl::unexpected(invalid("adaptive: entry seeds, region points and the helix switch are set by the "
                                      "entry planner, not by its caller"));
    }
    if (opt.order.empty()) {
        return tl::unexpected(invalid("adaptive: no entry kind is allowed"));
    }
    for (EntryKind k : opt.order) {
        if (k != EntryKind::Helix && k != EntryKind::Ramp && k != EntryKind::PreDrill) {
            return tl::unexpected(invalid("adaptive: entry by plunge is not supported; use a helix, ramp or "
                                          "pre-drilled hole"));
        }
    }
    for (const PreDrillHole& h : opt.preDrills) {
        if (h.diameter <= 0 || h.diameter > geom2d::kMaxCoord || std::llabs(h.centre.x) > geom2d::kMaxCoord ||
            std::llabs(h.centre.y) > geom2d::kMaxCoord) {
            return tl::unexpected(invalid("adaptive: a pre-drilled hole needs a positive diameter and a centre "
                                          "within the coordinate range"));
        }
    }
    if (opt.minRampRun < 0 || opt.maxRampRun < 0) {
        return tl::unexpected(invalid("adaptive: ramp lengths must not be negative"));
    }
    if (auto s = ctx.cancel.check(); !s) {
        return tl::unexpected(s.error());
    }

    // ---------- sizes ----------
    // The helix diameters as the engine takes them (Adaptive2d::Execute): full = helixDiameter (0: the tool
    // diameter), at most the tool diameter; smallest = helixMinDiameter (0: tool / 8), at least tool / 8.
    const Coord tool = in.toolDiameter;
    const Coord radius = tool / 2;
    const Coord eighth = tool / 8;
    Coord helixFull = in.helixDiameter > 0 ? std::min(in.helixDiameter, tool) : tool;
    const Coord helixMin = std::max(in.helixMinDiameter > 0 ? in.helixMinDiameter : eighth, eighth);
    helixFull = std::max(helixFull, helixMin);
    const std::vector<Attempt> attempts = attemptsFor(opt.order, helixMin < helixFull);
    const double target = angleForStepover(in.stepover);
    const Coord minRun = opt.minRampRun > 0 ? opt.minRampRun : tool;
    const Coord allowance = finishingAllowance(in);
    // A hole gives the passes room to start as a helix does: at least as wide as the smallest helix cuts.
    const Coord minHole = tool + helixMin;

    // ---------- the first run ----------
    // With a helix first: a full helix only when other kinds follow; else no helix, to learn the regions.
    Input first = in;
    first.allowUnentered = true;
    if (isHelix(attempts[0])) {
        first.helixDiameter = helixFull;
        first.helixMinDiameter = attempts[0] == Attempt::HelixFull ? helixFull : helixMin;
    } else {
        first.helixEntry = false;
    }
    JobContext firstCtx{ctx.cancel, ctx.progress.sub(0.9)};
    auto ran = runEngine(first, firstCtx);
    if (!ran) {
        return tl::unexpected(ran.error());
    }
    Output out = std::move(*ran);
    const std::int64_t grid = out.stats.gridUnitsPerMm;
    const double margin = double(kRampEndMargin) * 1e6 / double(grid);
    // the engine's warnings except "not entered" (those regions are planned below)
    std::vector<Warning> warnings;
    for (const Warning& w : out.warnings) {
        if (w.code != Warning::Code::NotEntered) {
            warnings.push_back(w);
        }
    }
    std::vector<Pending> pending;
    for (std::size_t i = 0; i < out.regions.size(); i++) {
        const Region& r = out.regions[i];
        if (r.entry.kind != EntryMode::None || r.toolBounds.empty()) {
            continue;
        }
        Pending p;
        p.index = i;
        p.point = geom2d::interiorPoint(r.toolBounds.front()).value_or(Pt{});
        p.attempts = attempts;
        p.next = isHelix(attempts[0]) ? 1 : 0;
        if (isHelix(attempts[0])) {
            p.notFitting.push_back("a helix of " + mmText(helixFull) + " mm");
        }
        pending.push_back(std::move(p));
    }

    // ---------- further runs, one per attempt still to make ----------
    // Every run ends at least one region's attempt, so there are at most (attempts x regions) runs.
    const int maxRuns = int(attempts.size() * pending.size()) + 1;
    for (int run = 1; run <= maxRuns; run++) {
        if (auto s = ctx.cancel.check(); !s) {
            return tl::unexpected(s.error());
        }
        Regions entryCleared;
        Input next = in;
        next.allowUnentered = true;
        next.helixEntry = false;
        next.helixDiameter = helixFull;
        next.helixMinDiameter = helixMin;
        std::vector<std::size_t> planned; // indices into pending
        std::optional<Attempt> helixMode; // one helix size range per run: a region wanting the other waits
        for (std::size_t k = 0; k < pending.size(); k++) {
            Pending& p = pending[k];
            if (p.done) {
                continue;
            }
            if (helixMode && p.next < p.attempts.size() && isHelix(p.attempts[p.next]) &&
                p.attempts[p.next] != *helixMode) {
                continue;
            }
            p.ramp.reset();
            p.hole.reset();
            p.helix = false;
            const Region& r = out.regions[p.index];
            while (p.next < p.attempts.size()) {
                const Attempt a = p.attempts[p.next];
                if (isHelix(a)) {
                    p.helix = true;
                    break;
                }
                if (a == Attempt::Ramp) {
                    p.ramp = planRamp(r.toolBounds, minRun, opt.maxRampRun, margin);
                    if (p.ramp) {
                        p.ramp->a = onGrid(p.ramp->a, grid);
                        p.ramp->b = onGrid(p.ramp->b, grid);
                        break;
                    }
                    p.notFitting.push_back("a ramp of " + mmText(minRun) + " mm");
                } else {
                    auto inner = geom2d::offset(r.toolBounds, -Coord(margin), ctx);
                    if (!inner) {
                        return tl::unexpected(inner.error());
                    }
                    for (std::size_t h = 0; h < opt.preDrills.size(); h++) {
                        if (opt.preDrills[h].diameter >= minHole &&
                            geom2d::locate(opt.preDrills[h].centre, *inner) == geom2d::Where::Inside) {
                            p.hole = h;
                            break;
                        }
                    }
                    if (p.hole) {
                        break;
                    }
                    p.notFitting.push_back("a pre-drilled hole of " + mmText(minHole) + " mm");
                }
                p.next++;
            }
            if (p.next >= p.attempts.size()) {
                p.done = true; // nothing left to try
                continue;
            }
            if (p.helix && helixMode && p.attempts[p.next] != *helixMode) {
                continue; // reached a helix of the other size range while walking the order: the next run
            }
            planned.push_back(k);
            next.regionsAt.push_back(p.point);
            if (p.helix) {
                next.helixEntry = true;
                helixMode = p.attempts[p.next];
                next.helixMinDiameter = *helixMode == Attempt::HelixFull ? helixFull : helixMin;
            }
            if (p.ramp) {
                geom2d::Path line;
                line.start = p.ramp->a;
                line.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, p.ramp->b, {}, 0});
                auto swept = geom2d::sweptArea(std::span<const geom2d::Path>(&line, 1), radius, ctx);
                if (!swept) {
                    return tl::unexpected(swept.error());
                }
                auto u = uniteAll(std::move(entryCleared), *swept, ctx);
                if (!u) {
                    return tl::unexpected(u.error());
                }
                entryCleared = std::move(*u);
                next.seeds.push_back(Input::Seed{p.ramp->b, p.ramp->a, EntryMode::Ramp, p.ramp->a, 0});
            }
            if (p.hole) {
                const PreDrillHole& h = opt.preDrills[*p.hole];
                geom2d::Region disc;
                disc.outer = geom2d::Path::circle(h.centre, h.diameter / 2, true);
                auto u = uniteAll(std::move(entryCleared), Regions{disc}, ctx);
                if (!u) {
                    return tl::unexpected(u.error());
                }
                entryCleared = std::move(*u);
                // As after a helix, the first pass starts with the tool against the hole's side: the hole's centre
                // moved by (hole radius - tool radius) less a grid unit, the first of -y, +x, +y, -x that stays in
                // the tool-centre area (else the centre), moving along +x rotated likewise.
                const Coord unit = Coord(1e6 / double(grid)) + 1;
                const Coord shift = std::max<Coord>(0, h.diameter / 2 - radius - unit);
                Pt at = onGrid(h.centre, grid), toward{at.x + 1'000'000, at.y};
                const Pt dirs[] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
                for (const Pt& d : dirs) {
                    const Pt c = onGrid({h.centre.x + d.x * shift, h.centre.y + d.y * shift}, grid);
                    const double off = std::sqrt(double(c.x - h.centre.x) * double(c.x - h.centre.x) +
                                                 double(c.y - h.centre.y) * double(c.y - h.centre.y));
                    if (shift > 0 && off + double(radius) <= double(h.diameter / 2) &&
                        geom2d::locate(c, r.toolBounds) == geom2d::Where::Inside) {
                        at = c;
                        toward = {c.x - d.y * 1'000'000, c.y + d.x * 1'000'000};
                        break;
                    }
                }
                next.seeds.push_back(Input::Seed{at, toward, EntryMode::PreDrill, h.centre, h.diameter / 2});
            }
        }
        if (planned.empty()) {
            break;
        }
        auto cleared = uniteAll(in.cleared, entryCleared, ctx);
        if (!cleared) {
            return tl::unexpected(cleared.error());
        }
        next.cleared = std::move(*cleared);
        JobContext nextCtx{ctx.cancel, ctx.progress.sub(0.1 / double(1 << std::min(run, 20)))};
        auto again = runEngine(next, nextCtx);
        if (!again) {
            return tl::unexpected(again.error());
        }
        addStats(out.stats, again->stats);
        // Each planned region's result: entered, and with a pass (from a hole hardly wider than the tool the engine
        // finds no first cut). When some fail and others do not, the run's cleared area and warnings mix both, so it
        // is left out and the regions that worked run again by themselves (the same attempt) in the next run.
        std::vector<const Region*> results;
        std::size_t failures = 0;
        for (std::size_t k : planned) {
            const Region* got = nullptr;
            for (const Region& r : again->regions) {
                if (geom2d::locate(pending[k].point, r.toolBounds) == geom2d::Where::Inside) {
                    got = &r;
                    break;
                }
            }
            bool cuts = false;
            if (got != nullptr && got->entry.kind != EntryMode::None) {
                for (const Path& path : got->paths) {
                    cuts = cuts || path.role == Role::Pass;
                }
            }
            results.push_back(cuts ? got : nullptr);
            failures += cuts ? 0 : 1;
        }
        const bool mixed = failures > 0 && failures < planned.size();
        if (failures == 0) {
            for (const Warning& w : again->warnings) {
                if (w.code != Warning::Code::NotEntered) {
                    warnings.push_back(w);
                }
            }
            auto allCleared = uniteAll(std::move(out.cleared), again->cleared, ctx);
            if (!allCleared) {
                return tl::unexpected(allCleared.error());
            }
            out.cleared = std::move(*allCleared);
        }
        for (std::size_t j = 0; j < planned.size(); j++) {
            Pending& p = pending[planned[j]];
            const Region* got = results[j];
            if (got != nullptr && mixed) {
                continue; // again, alone
            }
            if (got != nullptr) {
                Region region = *got;
                region.toolBounds = out.regions[p.index].toolBounds;
                if (region.entry.kind == EntryMode::PreDrill && region.entry.start != region.entry.centre &&
                    !region.paths.empty()) {
                    // from the hole's centre, where the tool plunged, to the first pass's start against its side
                    const Pt c = region.entry.centre, s = region.paths.front().points.front();
                    const double dist =
                        std::sqrt(double(s.x - c.x) * double(s.x - c.x) + double(s.y - c.y) * double(s.y - c.y));
                    Path link;
                    link.role = Role::Link;
                    link.motion =
                        dist + double(radius) <= double(region.entry.radius) ? Motion::LinkClear : Motion::Cut;
                    link.points = {c, s};
                    link.engagementDeg = {-1.0, -1.0};
                    region.paths.insert(region.paths.begin(), std::move(link));
                    region.entry.start = c;
                }
                if (region.entry.kind == EntryMode::Ramp && !region.paths.empty()) {
                    Path ramp;
                    ramp.role = Role::Ramp;
                    ramp.motion = Motion::Cut;
                    ramp.points = {region.entry.centre, region.paths.front().points.front()};
                    ramp.engagementDeg = {-1.0, 180.0};
                    region.entry.start = ramp.points.back();
                    region.paths.insert(region.paths.begin(), std::move(ramp));
                }
                out.regions[p.index] = std::move(region);
                p.done = true;
                if (!p.notFitting.empty()) {
                    std::string list;
                    for (std::size_t i = 0; i < p.notFitting.size(); i++) {
                        list += (i == 0 ? "" : i + 1 == p.notFitting.size() ? " or " : ", ") + p.notFitting[i];
                    }
                    warnings.push_back({Warning::Code::EntryFallback,
                                        "adaptive: the region around " + whereText(out.regions[p.index].toolBounds) +
                                            " is entered by a " + attemptName(p.attempts[p.next]) + ": " + list +
                                            " does not fit"});
                }
            } else {
                if (isHelix(p.attempts[p.next])) {
                    p.notFitting.push_back("a helix of " +
                                           mmText(p.attempts[p.next] == Attempt::HelixFull ? helixFull : helixMin) +
                                           " mm");
                } else if (p.ramp || p.hole) {
                    p.notFitting.push_back(std::string("a ") + attemptName(p.attempts[p.next]) +
                                           " (no first cut from it)");
                }
                p.next++;
            }
        }
    }

    // ---------- regions no kind can enter ----------
    for (const Pending& p : pending) {
        if (out.regions[p.index].entry.kind != EntryMode::None) {
            continue;
        }
        std::string needs;
        for (Attempt a : attempts) {
            if (!needs.empty()) {
                needs += "; ";
            }
            switch (a) {
            case Attempt::HelixFull:
            case Attempt::HelixAny:
                if (needs.find("helix") == std::string::npos) {
                    needs += "a helix needs the area to be at least " + mmText(tool + helixMin + 2 * allowance) +
                             " mm wide (tool diameter, smallest helix " + mmText(helixMin) +
                             " mm and twice the finishing allowance)";
                }
                break;
            case Attempt::Ramp:
                needs += "a ramp needs a straight run of at least " + mmText(minRun) +
                         " mm for the tool centre in an area at least " + mmText(tool + 2 * allowance) + " mm wide";
                break;
            case Attempt::PreDrill:
                needs += "a pre-drilled hole needs to be at least " + mmText(minHole) +
                         " mm wide (tool diameter and smallest helix) with its centre where the tool centre may go";
                break;
            }
        }
        std::string message =
            "adaptive: no entry into the region around " + whereText(out.regions[p.index].toolBounds) + ": " + needs;
        if (!in.allowUnentered) {
            return tl::unexpected(failed(message));
        }
        warnings.push_back({Warning::Code::NotEntered, message});
    }

    // ---------- links, reduced feed ----------
    for (Region& r : out.regions) {
        out.stats.linksMadeCuts += cutLinksThatCut(r.paths);
        for (Path& p : r.paths) {
            flagReducedFeed(p, target);
        }
    }
    out.warnings = std::move(warnings);
    if (auto s = ctx.cancel.check(); !s) {
        return tl::unexpected(s.error());
    }
    ctx.progress.set(1.0, "adaptive clearing");
    return out;
}

} // namespace ray::cam::adaptive2d
