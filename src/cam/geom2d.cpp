// The 2D geometry core (task T-050): types, conversion from and to kernel profiles, measures, nesting, canonical
// form, positions along paths, moves and the golden-file text. Offsets and booleans are in geom2d_clip.cpp, arc fitting
// in geom2d_fit.cpp, boundaries from the model in geom2d_model.cpp.
//
// TOLERANCES (every one in geom2d.h; why each value):
//   kMaxCoord 1 km        far beyond any machine; keeps every product of coordinate differences inside 128 bits and
//                         every coordinate exact in a double (2^53 nm is 9000 km).
//   kJoinTolerance 1 um   the kernel's curve ends meet within 1e-6 mm (1 nm); a sketch's within 1e-6 mm; 1 um joins
//                         ends that rounding or a STEP file's vertex tolerance left apart, and is ten times below the
//                         finest tolerance a shop works to, so no two ends meant to be apart are joined.
//   kOffsetSag 0.1 um     the sag of the chords Clipper2 sees. It decides topology only (which pieces of the exact
//                         offset survive and where they meet); the geometry is rebuilt exactly. At 0.1 um a circle of
//                         10 mm radius is about 700 points, so a 500-loop part stays well under a million points, and
//                         a narrow neck of the exact result is decided correctly unless it is narrower than 0.2 um.
//   kFitTolerance 1 um    default for fitArcs: the slices' own approximation is 0.5 um (kernel), so a fitted span is
//                         within 1.5 um of the model; ten times below a shop's finest tolerance.
//   kMinArcChord 10 um    a post rounds to 1 um (metric, 3 decimals) or 2.54 um (inch, 4 decimals): an arc whose start
//                         and end round to the same point is read by every control as a whole circle; 10 um is four
//                         times the coarsest rounding.
//   kMaxArcRadius 10 m    an arc flatter than that is a line within any tolerance this code is given, and some controls
//                         reject radii with more digits than their format.
//   kMaxFitSweep 180 deg  an R-form arc of more than 180 degrees needs a negative R that some posts and controls get
//                         wrong; at most 180 the I/J and R forms agree.
//   kMaxMoveSweep 180 deg the same for moves: appendMoves splits longer arcs (and whole circles) into equal parts.
//
// ROUNDING RULE. A double nanometre value becomes a coordinate by round-half-even (roundNm), a millimetre value by
// ray::toNm (also round-half-even). No other conversion reaches an output.
#include "geom2d_internal.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>

namespace ray::cam::geom2d {

using namespace detail;

namespace {

constexpr std::string_view kWhere = "cam::geom2d";

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {std::string(kWhere)}};
}

bool inRange(Coord v)
{
    return v >= -kMaxCoord && v <= kMaxCoord;
}
bool inRange(Pt p)
{
    return inRange(p.x) && inRange(p.y);
}

// ---------- y-monotone pieces of an arc (locate, interiorPoint) ----------
struct ArcPiece {
    Pt from, to; // exact: arc endpoints or the circle's top/bottom points
    bool right;  // on the right half of the circle (x >= cx)
};
// Splits the arc from `start` along `s` at the circle's top and bottom points.
std::vector<ArcPiece> monotonePieces(Pt start, const Span& s)
{
    const bool ccw = isCcw(s);
    const D2 c = d2(s.centre);
    const double a0 = angleOf(d2(start), c);
    const double sweep = sweepOf(start, s);
    struct Break {
        double at;
        Pt p;
    };
    std::vector<Break> breaks;
    const Pt top{s.centre.x, s.centre.y + s.radius};
    const Pt bottom{s.centre.x, s.centre.y - s.radius};
    for (const auto& [angle, p] : {std::pair<double, Pt>{kPi / 2, top}, std::pair<double, Pt>{-kPi / 2, bottom}}) {
        const double at = sweepBetween(a0, angle, ccw);
        if (at < sweep && at > 0.0 && p != start)
            breaks.push_back(Break{at, p});
    }
    std::sort(breaks.begin(), breaks.end(), [](const Break& a, const Break& b) { return a.at < b.at; });
    std::vector<ArcPiece> pieces;
    Pt from = start;
    double fromAt = 0.0;
    const auto addPiece = [&](Pt to, double toAt) {
        const double mid = a0 + (ccw ? 1.0 : -1.0) * 0.5 * (fromAt + toAt);
        pieces.push_back(ArcPiece{from, to, dmath::cos(mid) >= 0.0});
        from = to;
        fromAt = toAt;
    };
    for (const Break& b : breaks)
        addPiece(b.p, b.at);
    addPiece(s.end, sweep);
    return pieces;
}

// x of the circle (c, r) at height y on the given half.
double circleX(Pt c, Coord r, Coord y, bool right)
{
    const double dy = static_cast<double>(y - c.y);
    const double rr = static_cast<double>(r);
    const double h = dmath::sqrt(std::max(0.0, rr * rr - dy * dy));
    return static_cast<double>(c.x) + (right ? h : -h);
}

bool onArc(Pt p, Pt start, const Span& s)
{
    const double d = distance(d2(p), d2(s.centre));
    if (std::abs(d - static_cast<double>(s.radius)) > 0.5)
        return false;
    if (s.end == start)
        return true;
    const double a0 = angleOf(d2(start), d2(s.centre));
    const double ap = angleOf(d2(p), d2(s.centre));
    return sweepBetween(a0, ap, isCcw(s)) <= sweepOf(start, s) || p == start;
}

// Winding number of a closed path about p; `boundary` set when p lies on it.
int winding(Pt p, const Path& path, bool& boundary)
{
    int w = 0;
    Pt cur = path.start;
    for (const Span& s : path.spans) {
        if (!isArc(s)) {
            const Pt a = cur, b = s.end;
            const int o = orient(a, b, p);
            if (o == 0 && p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) &&
                p.y <= std::max(a.y, b.y))
                boundary = true;
            if (a.y <= p.y && p.y < b.y && o > 0)
                ++w;
            else if (b.y <= p.y && p.y < a.y && o < 0)
                --w;
        } else {
            if (onArc(p, cur, s))
                boundary = true;
            for (const ArcPiece& piece : monotonePieces(cur, s)) {
                const bool up = piece.from.y < piece.to.y;
                const Coord y0 = std::min(piece.from.y, piece.to.y), y1 = std::max(piece.from.y, piece.to.y);
                if (y0 == y1 || p.y < y0 || p.y >= y1)
                    continue;
                const double x = circleX(s.centre, s.radius, p.y, piece.right);
                if (x > static_cast<double>(p.x))
                    w += up ? 1 : -1;
            }
        }
        cur = s.end;
    }
    return w;
}

// Every x where the boundary of `path` crosses the horizontal line y (half-open in y, as winding counts).
void crossingsAt(const Path& path, Coord y, std::vector<double>& xs)
{
    Pt cur = path.start;
    for (const Span& s : path.spans) {
        if (!isArc(s)) {
            const Pt a = cur, b = s.end;
            if ((a.y <= y && y < b.y) || (b.y <= y && y < a.y)) {
                const double t = static_cast<double>(y - a.y) / static_cast<double>(b.y - a.y);
                xs.push_back(static_cast<double>(a.x) + t * static_cast<double>(b.x - a.x));
            }
        } else {
            for (const ArcPiece& piece : monotonePieces(cur, s)) {
                const Coord y0 = std::min(piece.from.y, piece.to.y), y1 = std::max(piece.from.y, piece.to.y);
                if (y0 != y1 && y >= y0 && y < y1)
                    xs.push_back(circleX(s.centre, s.radius, y, piece.right));
            }
        }
        cur = s.end;
    }
}

struct SortKey {
    Pt boxMin;
    Pt lowest;
    double area;
    auto operator<=>(const SortKey&) const = default;
};
SortKey sortKey(const Path& p)
{
    const Bounds b = bounds(p);
    const std::vector<Pt> v = vertices(p);
    return SortKey{b.min, v.empty() ? Pt{} : *std::min_element(v.begin(), v.end()), area(p)};
}

// Span pieces: splitting a span at arc length t from its start.
Pt spanPointAt(Pt start, const Span& s, double t)
{
    const double len = spanLength(start, s);
    if (len <= 0.0)
        return start;
    if (!isArc(s)) {
        const double f = std::clamp(t / len, 0.0, 1.0);
        return roundPt(d2(start) + (d2(s.end) - d2(start)) * f);
    }
    if (t <= 0.0)
        return start;
    if (t >= len)
        return s.end;
    const double a0 = angleOf(d2(start), d2(s.centre));
    const double a = a0 + (isCcw(s) ? 1.0 : -1.0) * t / static_cast<double>(s.radius);
    return roundPt(onCircle(d2(s.centre), static_cast<double>(s.radius), a));
}

Span withEnd(const Span& s, Pt end)
{
    Span out = s;
    out.end = end;
    return out;
}

// Merges span b into a (a runs from aStart); false if they do not merge.
bool merge(Pt aStart, Span& a, const Span& b)
{
    if (a.kind != b.kind)
        return false;
    if (!isArc(a)) {
        const Pt mid = a.end;
        if (orient(aStart, mid, b.end) != 0)
            return false;
        if (dotSign(mid.x - aStart.x, mid.y - aStart.y, b.end.x - mid.x, b.end.y - mid.y) <= 0)
            return false;
        a.end = b.end;
        return true;
    }
    if (a.centre != b.centre || a.radius != b.radius)
        return false;
    const double total = sweepOf(aStart, a) + sweepOf(a.end, b);
    if (total >= kTwoPi - 1e-9)
        return false;
    a.end = b.end;
    return true;
}

} // namespace

// ---------- Small helpers ----------
Coord roundNm(double v)
{
    constexpr double kLimit = 4.0e12;
    if (std::isnan(v))
        return 0;
    if (v >= kLimit)
        return static_cast<Coord>(kLimit);
    if (v <= -kLimit)
        return -static_cast<Coord>(kLimit);
    const double f = std::floor(v);
    const double d = v - f; // exact for |v| < 2^52
    Coord r = static_cast<Coord>(f);
    if (d > 0.5 || (d == 0.5 && (r & 1) != 0))
        ++r;
    return r;
}

Pt toPt(Vec2 mm)
{
    return Pt{toNm(mm.x), toNm(mm.y)};
}
Vec2 toVec2(Pt p)
{
    return Vec2{toMm(p.x), toMm(p.y)};
}
Coord toCoord(double mm)
{
    return toNm(mm);
}

Status checkRange(const Path& p)
{
    if (!inRange(p.start))
        return tl::unexpected(invalid("A point lies more than 1 km from the origin."));
    for (const Span& s : p.spans)
        if (!inRange(s.end) || !inRange(s.centre) || !inRange(s.radius) || s.radius < 0)
            return tl::unexpected(invalid("A point or radius lies beyond 1 km."));
    return {};
}

double sweepOf(Pt start, const Span& s)
{
    if (!isArc(s))
        return 0.0;
    if (s.end == start)
        return kTwoPi;
    return sweepBetween(angleOf(d2(start), d2(s.centre)), angleOf(d2(s.end), d2(s.centre)), isCcw(s));
}

Path Path::circle(Pt centre, Coord radius, bool ccw)
{
    Path p;
    p.start = Pt{centre.x + radius, centre.y};
    p.spans.push_back(Span{ccw ? SpanKind::ArcCCW : SpanKind::ArcCW, p.start, centre, radius});
    p.closed = true;
    return p;
}

std::vector<Pt> detail::vertices(const Path& p)
{
    std::vector<Pt> v;
    v.reserve(p.spans.size() + 1);
    v.push_back(p.start);
    for (const Span& s : p.spans)
        v.push_back(s.end);
    if (p.closed && v.size() > 1)
        v.pop_back();
    return v;
}

// ---------- Conversion ----------
Result<Path> fromLoop(const kernel::ProfileLoop& loop)
{
    using Kind = kernel::ProfileCurve::Kind;
    struct Seg {
        bool arc = false;
        Pt a, b, c;
        Coord r = 0;
        bool ccw = true;
    };
    if (loop.curves.empty())
        return tl::unexpected(invalid("A loop has no curves."));
    if (loop.curves.size() == 1 && loop.curves[0].kind == Kind::Circle) {
        const kernel::ProfileCurve& c = loop.curves[0];
        if (c.pts.empty() || !(c.radius > 0.0))
            return tl::unexpected(invalid("A circle needs a centre and a positive radius."));
        Path p = Path::circle(toPt(c.pts[0]), toCoord(c.radius), c.ccw);
        if (auto st = checkRange(p); !st)
            return tl::unexpected(st.error());
        return p;
    }
    std::vector<Seg> segs;
    for (const kernel::ProfileCurve& c : loop.curves) {
        Seg s;
        switch (c.kind) {
        case Kind::Line:
            if (c.pts.size() != 2)
                return tl::unexpected(invalid("A line needs two points."));
            s.a = toPt(c.pts[0]);
            s.b = toPt(c.pts[1]);
            break;
        case Kind::Arc:
            if (c.pts.size() != 3)
                return tl::unexpected(invalid("An arc needs a start, an end and a centre."));
            s.arc = true;
            s.a = toPt(c.pts[0]);
            s.b = toPt(c.pts[1]);
            s.c = toPt(c.pts[2]);
            s.r = c.radius > 0.0 ? toCoord(c.radius) : roundNm(distance(d2(s.a), d2(s.c)));
            s.ccw = c.ccw;
            if (s.r <= 0)
                return tl::unexpected(invalid("An arc has no radius."));
            break;
        case Kind::Circle:
            return tl::unexpected(invalid("A circle must be a loop of its own."));
        case Kind::Bspline:
            return tl::unexpected(invalid("B-spline curves must be approximated by lines and arcs first."));
        }
        if (!inRange(s.a) || !inRange(s.b) || !inRange(s.c) || !inRange(s.r))
            return tl::unexpected(invalid("A point lies more than 1 km from the origin."));
        if (s.a == s.b)
            continue; // no length after rounding
        segs.push_back(s);
    }
    if (segs.empty())
        return tl::unexpected(invalid("A loop has no length."));
    const auto toSpan = [](const Seg& s, Pt end) {
        if (!s.arc)
            return Span{SpanKind::Line, end, {}, 0};
        return Span{s.ccw ? SpanKind::ArcCCW : SpanKind::ArcCW, end, s.c, s.r};
    };
    Path path;
    path.closed = true;
    path.start = segs[0].a;
    std::vector<bool> used(segs.size(), false);
    used[0] = true;
    Seg current = segs[0];
    for (std::size_t n = 1; n < segs.size(); ++n) {
        const D2 end = d2(current.b);
        std::size_t best = segs.size();
        double bestDistance = 0.0;
        bool bestReversed = false;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (used[i])
                continue;
            const double da = distance(end, d2(segs[i].a)), db = distance(end, d2(segs[i].b));
            const double d = std::min(da, db);
            if (best == segs.size() || d < bestDistance) {
                best = i;
                bestDistance = d;
                bestReversed = db < da;
            }
        }
        if (bestDistance > static_cast<double>(kJoinTolerance))
            return tl::unexpected(invalid("The curves of a loop do not join up (a gap wider than 1 um)."));
        path.spans.push_back(toSpan(current, current.b));
        used[best] = true;
        Seg next = segs[best];
        if (bestReversed) {
            std::swap(next.a, next.b);
            next.ccw = !next.ccw;
        }
        next.a = current.b;
        current = next;
    }
    if (distance(d2(current.b), d2(path.start)) > static_cast<double>(kJoinTolerance))
        return tl::unexpected(invalid("A loop is not closed (its ends are more than 1 um apart)."));
    path.spans.push_back(toSpan(current, path.start));
    return path;
}

kernel::ProfileLoop toLoop(const Path& p)
{
    using Kind = kernel::ProfileCurve::Kind;
    kernel::ProfileLoop loop;
    if (p.spans.size() == 1 && isArc(p.spans[0]) && p.spans[0].end == p.start) {
        const Span& s = p.spans[0];
        loop.curves.push_back(kernel::ProfileCurve{Kind::Circle, {toVec2(s.centre)}, toMm(s.radius), isCcw(s), 0});
        return loop;
    }
    Pt cur = p.start;
    for (const Span& s : p.spans) {
        if (!isArc(s))
            loop.curves.push_back(kernel::ProfileCurve{Kind::Line, {toVec2(cur), toVec2(s.end)}, 0.0, false, 0});
        else
            loop.curves.push_back(kernel::ProfileCurve{
                Kind::Arc, {toVec2(cur), toVec2(s.end), toVec2(s.centre)}, toMm(s.radius), isCcw(s), 0});
        cur = s.end;
    }
    return loop;
}

Result<Regions> regionsFromLoops(std::span<const kernel::ProfileLoop> loops, JobContext& ctx)
{
    std::vector<Path> paths;
    paths.reserve(loops.size());
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (auto st = poll(ctx, i, 64); !st)
            return tl::unexpected(st.error());
        auto p = fromLoop(loops[i]);
        if (!p)
            return tl::unexpected(p.error());
        if (std::abs(area(*p)) <= 0.0)
            continue;
        paths.push_back(std::move(*p));
    }
    auto nesting = nest(paths, ctx);
    if (!nesting)
        return tl::unexpected(nesting.error());
    return regionsByNesting(paths, *nesting);
}

std::vector<kernel::ProfileLoop> toLoops(const Regions& regions)
{
    std::vector<kernel::ProfileLoop> loops;
    for (const Region& r : regions) {
        loops.push_back(toLoop(r.outer));
        for (const Path& h : r.holes)
            loops.push_back(toLoop(h));
    }
    return loops;
}

// ---------- Direction and side ----------
Side toolSideFor(CutMode mode, bool spindleClockwise)
{
    const bool left = (mode == CutMode::Climb) == spindleClockwise;
    return left ? Side::Left : Side::Right;
}

Path orientForTool(Path p, Side current, Side want)
{
    return current == want ? p : reverse(p);
}

Path reverse(const Path& p)
{
    Path r;
    r.closed = p.closed;
    const std::vector<Pt> v = [&] {
        std::vector<Pt> all{p.start};
        for (const Span& s : p.spans)
            all.push_back(s.end);
        return all;
    }();
    r.start = v.back();
    for (std::size_t i = p.spans.size(); i-- > 0;) {
        Span s = p.spans[i];
        s.end = v[i];
        if (s.kind == SpanKind::ArcCCW)
            s.kind = SpanKind::ArcCW;
        else if (s.kind == SpanKind::ArcCW)
            s.kind = SpanKind::ArcCCW;
        r.spans.push_back(s);
    }
    return r;
}

Side insideSide(const Path& closed)
{
    return area(closed) >= 0.0 ? Side::Left : Side::Right;
}

// ---------- Measures ----------
double area(const Path& p)
{
    if (p.spans.empty())
        return 0.0;
    const D2 o = d2(p.start);
    double twice = 0.0;
    Pt cur = p.start;
    for (const Span& s : p.spans) {
        twice += cross(d2(cur) - o, d2(s.end) - o);
        if (isArc(s)) {
            const double sweep = sweepOf(cur, s);
            const double r = static_cast<double>(s.radius);
            const double segment = r * r * (sweep - dmath::sin(sweep));
            twice += isCcw(s) ? segment : -segment;
        }
        cur = s.end;
    }
    return 0.5 * twice;
}

double area(const Region& r)
{
    double a = std::abs(area(r.outer));
    for (const Path& h : r.holes)
        a -= std::abs(area(h));
    return std::max(0.0, a);
}

double area(const Regions& rs)
{
    double a = 0.0;
    for (const Region& r : rs)
        a += area(r);
    return a;
}

double length(const Path& p)
{
    double total = 0.0;
    Pt cur = p.start;
    for (const Span& s : p.spans) {
        total += spanLength(cur, s);
        cur = s.end;
    }
    return total;
}

Bounds bounds(const Path& p)
{
    Bounds b;
    const auto add = [&](Pt q) {
        if (b.empty) {
            b.min = b.max = q;
            b.empty = false;
            return;
        }
        b.min = Pt{std::min(b.min.x, q.x), std::min(b.min.y, q.y)};
        b.max = Pt{std::max(b.max.x, q.x), std::max(b.max.y, q.y)};
    };
    add(p.start);
    Pt cur = p.start;
    for (const Span& s : p.spans) {
        add(s.end);
        if (isArc(s)) {
            const double a0 = angleOf(d2(cur), d2(s.centre));
            const double sweep = sweepOf(cur, s);
            const std::pair<double, Pt> extremes[] = {
                {0.0, Pt{s.centre.x + s.radius, s.centre.y}},
                {kPi / 2, Pt{s.centre.x, s.centre.y + s.radius}},
                {kPi, Pt{s.centre.x - s.radius, s.centre.y}},
                {-kPi / 2, Pt{s.centre.x, s.centre.y - s.radius}},
            };
            for (const auto& [angle, q] : extremes)
                if (sweep >= kTwoPi || sweepBetween(a0, angle, isCcw(s)) < sweep)
                    add(q);
        }
        cur = s.end;
    }
    return b;
}

Bounds bounds(const Region& r)
{
    return bounds(r.outer);
}

Where locate(Pt p, const Path& closed)
{
    bool boundary = false;
    const int w = winding(p, closed, boundary);
    if (boundary)
        return Where::OnBoundary;
    return w != 0 ? Where::Inside : Where::Outside;
}

Where locate(Pt p, const Region& r)
{
    const Where outer = locate(p, r.outer);
    if (outer != Where::Inside)
        return outer;
    for (const Path& h : r.holes) {
        const Where w = locate(p, h);
        if (w == Where::OnBoundary)
            return Where::OnBoundary;
        if (w == Where::Inside)
            return Where::Outside;
    }
    return Where::Inside;
}

Where locate(Pt p, const Regions& rs)
{
    for (const Region& r : rs) {
        const Where w = locate(p, r);
        if (w != Where::Outside)
            return w;
    }
    return Where::Outside;
}

std::optional<Pt> interiorPoint(const Region& r)
{
    const Bounds b = bounds(r.outer);
    if (b.empty || b.max.y - b.min.y < 2)
        return std::nullopt;
    const Coord h = b.max.y - b.min.y;
    // Lines tried, in order: the middle, then thirds, quarters, ... of the height.
    for (int denominator = 2; denominator <= 64; ++denominator) {
        for (int numerator = 1; numerator < denominator; ++numerator) {
            if (std::gcd(numerator, denominator) != 1)
                continue;
            const Coord y = b.min.y + h * numerator / denominator;
            std::vector<double> xs;
            crossingsAt(r.outer, y, xs);
            for (const Path& hole : r.holes)
                crossingsAt(hole, y, xs);
            std::sort(xs.begin(), xs.end());
            double bestWidth = 0.0;
            std::optional<Pt> best;
            for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
                const double width = xs[i + 1] - xs[i];
                if (width > bestWidth) {
                    const Pt candidate{roundNm(0.5 * (xs[i] + xs[i + 1])), y};
                    if (locate(candidate, r) == Where::Inside) {
                        bestWidth = width;
                        best = candidate;
                    }
                }
            }
            if (best)
                return best;
        }
    }
    return std::nullopt;
}

// ---------- Nesting ----------
Result<Nesting> nest(std::span<const Path> closed, JobContext& ctx)
{
    const std::size_t n = closed.size();
    Nesting out;
    out.parent.assign(n, -1);
    out.depth.assign(n, 0);
    if (n == 0)
        return out;
    std::vector<Bounds> box(n);
    std::vector<double> absArea(n);
    Bounds all;
    for (std::size_t i = 0; i < n; ++i) {
        box[i] = bounds(closed[i]);
        absArea[i] = std::abs(area(closed[i]));
        if (all.empty) {
            all = box[i];
        } else {
            all.min = Pt{std::min(all.min.x, box[i].min.x), std::min(all.min.y, box[i].min.y)};
            all.max = Pt{std::max(all.max.x, box[i].max.x), std::max(all.max.y, box[i].max.y)};
        }
        all.empty = false;
    }
    // A uniform grid of about n cells; each path is listed in the cells its box covers. The paths enclosing a point
    // all cover the cell of that point, so they are the only candidates.
    const std::size_t side = std::max<std::size_t>(1, static_cast<std::size_t>(dmath::sqrt(static_cast<double>(n))));
    const double w = std::max(1.0, static_cast<double>(all.max.x - all.min.x) + 1.0);
    const double hgt = std::max(1.0, static_cast<double>(all.max.y - all.min.y) + 1.0);
    const auto cellX = [&](Coord x) {
        return std::min(side - 1, static_cast<std::size_t>(static_cast<double>(x - all.min.x) / w * side));
    };
    const auto cellY = [&](Coord y) {
        return std::min(side - 1, static_cast<std::size_t>(static_cast<double>(y - all.min.y) / hgt * side));
    };
    std::vector<std::vector<int>> cells(side * side);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t cy = cellY(box[i].min.y); cy <= cellY(box[i].max.y); ++cy)
            for (std::size_t cx = cellX(box[i].min.x); cx <= cellX(box[i].max.x); ++cx)
                cells[cy * side + cx].push_back(static_cast<int>(i));
    for (std::size_t i = 0; i < n; ++i) {
        if (auto st = poll(ctx, i, 64); !st)
            return tl::unexpected(st.error());
        // A test point of path i that is not on another path's boundary: its vertices in order (paths do not cross,
        // so one that lies on another path is a touch point; the next vertex decides).
        const std::vector<Pt> pts = vertices(closed[i]);
        int parent = -1;
        for (const int j : cells[cellY(pts[0].y) * side + cellX(pts[0].x)]) {
            if (static_cast<std::size_t>(j) == i || absArea[j] <= absArea[i])
                continue;
            if (box[j].min.x > box[i].min.x || box[j].min.y > box[i].min.y || box[j].max.x < box[i].max.x ||
                box[j].max.y < box[i].max.y)
                continue;
            Where where = Where::OnBoundary;
            for (const Pt& q : pts) {
                where = locate(q, closed[j]);
                if (where != Where::OnBoundary)
                    break;
            }
            if (where != Where::Inside)
                continue;
            if (parent < 0 || absArea[j] < absArea[parent] || (absArea[j] == absArea[parent] && j < parent))
                parent = j;
        }
        out.parent[i] = parent;
    }
    // Depth by following parents (each parent's area is larger, so this ends).
    for (std::size_t i = 0; i < n; ++i) {
        int d = 0;
        for (int p = out.parent[i]; p >= 0; p = out.parent[static_cast<std::size_t>(p)])
            ++d;
        out.depth[i] = d;
    }
    return out;
}

Regions regionsByNesting(std::span<const Path> closed, const Nesting& nesting)
{
    Regions regions;
    std::vector<int> regionOf(closed.size(), -1);
    for (std::size_t i = 0; i < closed.size(); ++i) {
        if (nesting.depth[i] % 2 != 0)
            continue;
        regionOf[i] = static_cast<int>(regions.size());
        regions.push_back(Region{closed[i], {}});
    }
    for (std::size_t i = 0; i < closed.size(); ++i) {
        if (nesting.depth[i] % 2 == 0)
            continue;
        const int parent = nesting.parent[i];
        if (parent < 0 || regionOf[static_cast<std::size_t>(parent)] < 0)
            continue;
        regions[static_cast<std::size_t>(regionOf[static_cast<std::size_t>(parent)])].holes.push_back(closed[i]);
    }
    return canonicalize(std::move(regions));
}

// ---------- Canonical form ----------
Path canonicalize(Path p)
{
    // Zero-length spans: a line to where it starts, an arc of no radius, or (in a path of several spans) an arc to
    // where it starts.
    {
        std::vector<Span> kept;
        Pt cur = p.start;
        for (const Span& s : p.spans) {
            const bool whole = isArc(s) && s.end == cur && p.spans.size() == 1;
            if (s.end == cur && !whole)
                continue;
            if (isArc(s) && s.radius <= 0)
                kept.push_back(Span{SpanKind::Line, s.end, {}, 0});
            else
                kept.push_back(s);
            cur = s.end;
        }
        p.spans = std::move(kept);
    }
    if (p.spans.empty())
        return p;
    if (p.closed && p.spans.size() == 1 && isArc(p.spans[0]))
        return Path::circle(p.spans[0].centre, p.spans[0].radius, isCcw(p.spans[0]));
    {
        std::vector<Span> merged;
        Pt backStart = p.start; // where merged.back() starts
        Pt cur = p.start;
        for (const Span& s : p.spans) {
            if (!merged.empty() && merge(backStart, merged.back(), s)) {
                cur = s.end;
                continue;
            }
            backStart = cur;
            merged.push_back(s);
            cur = s.end;
        }
        p.spans = std::move(merged);
    }
    if (!p.closed)
        return p;
    // Closed: merge across the start while the last and first spans merge (the start vertex then disappears and the
    // merged span starts where the last one did), then start at the lowest vertex.
    while (p.spans.size() >= 2) {
        const Pt lastStart = p.spans[p.spans.size() - 2].end;
        Span last = p.spans.back();
        const Span first = p.spans.front();
        if (p.spans.size() == 2 && isArc(last) && last.kind == first.kind && last.centre == first.centre &&
            last.radius == first.radius && sweepOf(lastStart, last) + sweepOf(p.start, first) >= kTwoPi - 1e-9)
            return Path::circle(first.centre, first.radius, isCcw(first));
        if (!merge(lastStart, last, first))
            break;
        p.spans.pop_back();
        p.spans.front() = last;
        p.start = lastStart;
    }
    const std::vector<Pt> v = vertices(p);
    const std::size_t k = static_cast<std::size_t>(std::min_element(v.begin(), v.end()) - v.begin());
    if (k != 0) {
        std::rotate(p.spans.begin(), p.spans.begin() + static_cast<std::ptrdiff_t>(k), p.spans.end());
        p.start = v[k];
    }
    return p;
}

Region canonicalize(Region r)
{
    if (area(r.outer) < 0.0)
        r.outer = reverse(r.outer);
    r.outer = canonicalize(std::move(r.outer));
    for (Path& h : r.holes) {
        if (area(h) > 0.0)
            h = reverse(h);
        h = canonicalize(std::move(h));
    }
    std::stable_sort(r.holes.begin(), r.holes.end(),
                     [](const Path& a, const Path& b) { return sortKey(a) < sortKey(b); });
    return r;
}

Regions canonicalize(Regions rs)
{
    for (Region& r : rs)
        r = canonicalize(std::move(r));
    std::stable_sort(rs.begin(), rs.end(),
                     [](const Region& a, const Region& b) { return sortKey(a.outer) < sortKey(b.outer); });
    return rs;
}

// ---------- Paths along their length ----------
std::vector<Pt> polygonize(const Path& p, Coord sag)
{
    std::vector<Pt> out{p.start};
    Pt cur = p.start;
    for (const Span& s : p.spans) {
        if (isArc(s) && s.radius > 0) {
            const double r = static_cast<double>(s.radius);
            const double sweep = sweepOf(cur, s);
            const double ratio = std::clamp(1.0 - static_cast<double>(std::max<Coord>(sag, 1)) / r, -1.0, 1.0);
            const double step = std::max(1e-6, std::min(kPi / 2, 2.0 * dmath::acos(ratio)));
            const int n = std::max(1, static_cast<int>(std::ceil(sweep / step)));
            const double a0 = angleOf(d2(cur), d2(s.centre));
            const double dir = isCcw(s) ? 1.0 : -1.0;
            for (int k = 1; k < n; ++k)
                out.push_back(roundPt(onCircle(d2(s.centre), r, a0 + dir * sweep * k / n)));
        }
        out.push_back(s.end);
        cur = s.end;
    }
    if (p.closed && out.size() > 1 && out.back() == out.front())
        out.pop_back();
    return out;
}

Pt pointAt(const Path& p, double s)
{
    Pt cur = p.start;
    double at = 0.0;
    for (const Span& span : p.spans) {
        const double len = spanLength(cur, span);
        if (s <= at + len)
            return spanPointAt(cur, span, s - at);
        at += len;
        cur = span.end;
    }
    return cur;
}

Vec2 tangentAt(const Path& p, double s)
{
    Pt cur = p.start;
    double at = 0.0;
    for (std::size_t i = 0; i < p.spans.size(); ++i) {
        const Span& span = p.spans[i];
        const double len = spanLength(cur, span);
        if (s < at + len || i + 1 == p.spans.size()) {
            const Pt q = spanPointAt(cur, span, s - at);
            D2 t;
            if (!isArc(span)) {
                t = unit(d2(span.end) - d2(cur));
            } else {
                const D2 radial = d2(q) - d2(span.centre);
                t = unit(isCcw(span) ? leftOf(radial) : rightOf(radial));
            }
            return Vec2{t.x, t.y};
        }
        at += len;
        cur = span.end;
    }
    return Vec2{1.0, 0.0};
}

Path subPath(const Path& p, double s0, double s1)
{
    Path out;
    const double total = length(p);
    if (p.spans.empty() || total <= 0.0)
        return out;
    if (!p.closed) {
        s0 = std::clamp(s0, 0.0, total);
        s1 = std::clamp(s1, s0, total);
    } else {
        s0 = std::clamp(s0, 0.0, total);
        s1 = std::clamp(s1, s0, s0 + total);
    }
    out.start = pointAt(p, s0);
    Pt cur = p.start;
    double at = 0.0;
    const std::size_t rounds = p.closed ? 2 : 1;
    for (std::size_t round = 0; round < rounds; ++round) {
        cur = p.start;
        for (const Span& span : p.spans) {
            const double len = spanLength(cur, span);
            const double a = at, b = at + len;
            at = b;
            const Pt spanStart = cur;
            cur = span.end;
            if (b <= s0 || a >= s1)
                continue;
            const Pt end = b <= s1 ? span.end : spanPointAt(spanStart, span, s1 - a);
            if (end == (out.spans.empty() ? out.start : out.spans.back().end))
                continue;
            Span piece = withEnd(span, end);
            out.spans.push_back(piece);
        }
    }
    return out;
}

Path startAt(const Path& closed, double s)
{
    const double total = length(closed);
    Path out = subPath(closed, s, s + total);
    out.closed = true;
    if (!out.spans.empty())
        out.spans.back().end = out.start;
    return out;
}

// ---------- Moves ----------
void appendMoves(const Path& p, Coord z, MoveRole role, std::int32_t feedMicronPerMin, std::vector<Move>& out)
{
    Pt cur = p.start;
    for (const Span& s : p.spans) {
        if (!isArc(s)) {
            Move m{};
            m.kind = MoveKind::Linear;
            m.role = role;
            m.plane = ArcPlane::XY;
            m.feedMicronPerMin = feedMicronPerMin;
            m.x = s.end.x;
            m.y = s.end.y;
            m.z = z;
            out.push_back(m);
            cur = s.end;
            continue;
        }
        const double sweep = sweepOf(cur, s);
        const int parts = std::max(1, static_cast<int>(std::ceil(sweep / kMaxMoveSweep - 1e-12)));
        const double a0 = angleOf(d2(cur), d2(s.centre));
        const double dir = isCcw(s) ? 1.0 : -1.0;
        for (int k = 1; k <= parts; ++k) {
            const Pt end =
                k == parts
                    ? s.end
                    : roundPt(onCircle(d2(s.centre), static_cast<double>(s.radius), a0 + dir * sweep * k / parts));
            Move m{};
            m.kind = isCcw(s) ? MoveKind::ArcCCW : MoveKind::ArcCW;
            m.role = role;
            m.plane = ArcPlane::XY;
            m.feedMicronPerMin = feedMicronPerMin;
            m.x = end.x;
            m.y = end.y;
            m.z = z;
            m.i = s.centre.x;
            m.j = s.centre.y;
            m.k = z;
            out.push_back(m);
        }
        cur = s.end;
    }
}

// ---------- Text ----------
std::string toText(const Path& p)
{
    std::string t = "P " + std::to_string(p.closed ? 1 : 0) + " " + std::to_string(p.start.x) + " " +
                    std::to_string(p.start.y) + "\n";
    for (const Span& s : p.spans) {
        if (!isArc(s)) {
            t += "L " + std::to_string(s.end.x) + " " + std::to_string(s.end.y) + "\n";
        } else {
            t += std::string(isCcw(s) ? "A+ " : "A- ") + std::to_string(s.end.x) + " " + std::to_string(s.end.y) + " " +
                 std::to_string(s.centre.x) + " " + std::to_string(s.centre.y) + " " + std::to_string(s.radius) + "\n";
        }
    }
    return t;
}

std::string toText(const Region& r)
{
    std::string t = "R\nO\n" + toText(r.outer);
    for (const Path& h : r.holes)
        t += "H\n" + toText(h);
    return t;
}

std::string toText(const Regions& rs)
{
    std::string t;
    for (const Region& r : rs)
        t += toText(r);
    return t;
}

} // namespace ray::cam::geom2d
