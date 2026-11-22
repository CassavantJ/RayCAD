// Edges and faces -> ProfileCurves and ordered loops in a frame's XY (task T-050). The rules are in slice_internal.h.
#include "slice_internal.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace ray::kernel::detail {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kJoin = 1e-6; // mm: curve ends closer than this meet

using Kind = ProfileCurve::Kind;

double dist(const Vec2& a, const Vec2& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

Vec2 xy(const gp_Pnt& p)
{
    return Vec2{p.X(), p.Y()};
}

ProfileCurve makeLine(Vec2 a, Vec2 b)
{
    return ProfileCurve{Kind::Line, {a, b}, 0.0, false, 0};
}
ProfileCurve makeArc(Vec2 a, Vec2 b, Vec2 c, double r, bool ccw)
{
    return ProfileCurve{Kind::Arc, {a, b, c}, r, ccw, 0};
}
ProfileCurve makeCircle(Vec2 c, double r, bool ccw)
{
    return ProfileCurve{Kind::Circle, {c}, r, ccw, 0};
}

// Distance from p to the segment ab.
double segmentDistance(const Vec2& p, const Vec2& a, const Vec2& b)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 0.0)
        return dist(p, a);
    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// The circle through three points; false when they are (nearly) collinear.
bool circleThrough(const Vec2& a, const Vec2& b, const Vec2& c, Vec2& centre, double& radius)
{
    const double bx = b.x - a.x, by = b.y - a.y, cx = c.x - a.x, cy = c.y - a.y;
    const double d = 2.0 * (bx * cy - by * cx);
    const double scale = std::max({std::abs(bx), std::abs(by), std::abs(cx), std::abs(cy), 1e-300});
    if (std::abs(d) <= 1e-12 * scale * scale)
        return false;
    const double b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
    const double ux = (cy * b2 - by * c2) / d, uy = (bx * c2 - cx * b2) / d;
    centre = Vec2{a.x + ux, a.y + uy};
    radius = std::hypot(ux, uy);
    return std::isfinite(radius);
}

// Signed angle swept from a to b about c going counter-clockwise (ccw) or clockwise, in (0, 2 pi].
double sweepOf(const Vec2& a, const Vec2& b, const Vec2& c, bool ccw)
{
    const double a0 = std::atan2(a.y - c.y, a.x - c.x);
    const double a1 = std::atan2(b.y - c.y, b.x - c.x);
    double s = ccw ? a1 - a0 : a0 - a1;
    while (s <= 0.0)
        s += 2.0 * kPi;
    while (s > 2.0 * kPi)
        s -= 2.0 * kPi;
    return s;
}

// A curve that is not a line or a circle about Z: recognised as a line or an arc, or approximated by lines.
class Sampler {
public:
    Sampler(const BRepAdaptor_Curve& curve, double t0, double t1, bool reversed, Vec2 start, Vec2 end)
        : curve_(curve), t0_(t0), t1_(t1), reversed_(reversed), start_(start), end_(end)
    {
    }

    void append(std::vector<ProfileCurve>& out) const
    {
        if (recognise(out))
            return;
        Vec2 last = start_;
        split(0.0, 1.0, start_, end_, 0, last, out);
    }

private:
    // Parameter of fraction f along the curve in the requested direction.
    double param(double f) const { return reversed_ ? t1_ + (t0_ - t1_) * f : t0_ + (t1_ - t0_) * f; }
    Vec2 at(double f) const
    {
        if (f <= 0.0)
            return start_;
        if (f >= 1.0)
            return end_;
        return xy(curve_.Value(param(f)));
    }

    bool recognise(std::vector<ProfileCurve>& out) const
    {
        std::vector<Vec2> samples;
        samples.reserve(kRecogniseSamples + 1);
        for (int i = 0; i <= kRecogniseSamples; ++i)
            samples.push_back(at(static_cast<double>(i) / kRecogniseSamples));
        const bool closed = dist(start_, end_) <= kJoin;
        if (!closed) {
            bool straight = true;
            for (const Vec2& p : samples)
                straight = straight && segmentDistance(p, start_, end_) <= kCurveTolerance;
            if (straight) {
                if (dist(start_, end_) > kDegenerateLength)
                    out.push_back(makeLine(start_, end_));
                return true;
            }
        }
        // Circle through three points spread along the curve.
        const Vec2 p1 = closed ? samples[kRecogniseSamples / 3] : samples[kRecogniseSamples / 2];
        const Vec2 p2 = closed ? samples[2 * kRecogniseSamples / 3] : end_;
        Vec2 centre;
        double radius = 0.0;
        if (!circleThrough(start_, p1, p2, centre, radius))
            return false;
        for (const Vec2& p : samples)
            if (std::abs(dist(p, centre) - radius) > kCurveTolerance)
                return false;
        // Direction: the turn of the samples about the centre must be one way all along.
        double turn = 0.0;
        for (int i = 0; i < kRecogniseSamples; ++i) {
            const Vec2& a = samples[i];
            const Vec2& b = samples[i + 1];
            const double cross = (a.x - centre.x) * (b.y - centre.y) - (a.y - centre.y) * (b.x - centre.x);
            if (i > 0 && (cross > 0.0) != (turn > 0.0) && cross != 0.0)
                return false;
            if (cross != 0.0)
                turn = cross;
        }
        const bool ccw = turn > 0.0;
        if (closed)
            out.push_back(makeCircle(centre, radius, ccw));
        else
            out.push_back(makeArc(start_, end_, centre, radius, ccw));
        return true;
    }

    void split(double f0, double f1, Vec2 a, Vec2 b, int depth, Vec2& last, std::vector<ProfileCurve>& out) const
    {
        const Vec2 q1 = at(f0 + 0.25 * (f1 - f0));
        const Vec2 q2 = at(f0 + 0.5 * (f1 - f0));
        const Vec2 q3 = at(f0 + 0.75 * (f1 - f0));
        const double deviation =
            std::max({segmentDistance(q1, a, b), segmentDistance(q2, a, b), segmentDistance(q3, a, b)});
        if (depth >= kMaxPolylineSplits || (depth >= kMinPolylineSplits && deviation <= kCurveTolerance)) {
            if (dist(last, b) > kDegenerateLength) {
                out.push_back(makeLine(last, b));
                last = b;
            }
            return;
        }
        const double fm = f0 + 0.5 * (f1 - f0);
        split(f0, fm, a, q2, depth + 1, last, out);
        split(fm, f1, q2, b, depth + 1, last, out);
    }

    const BRepAdaptor_Curve& curve_;
    double t0_, t1_;
    bool reversed_;
    Vec2 start_, end_;
};

Vec2 curveStart(const ProfileCurve& c)
{
    return c.kind == Kind::Circle ? Vec2{c.pts[0].x + c.radius, c.pts[0].y} : c.pts[0];
}
Vec2 curveEnd(const ProfileCurve& c)
{
    return c.kind == Kind::Circle ? curveStart(c) : c.pts[1];
}

// The lowest vertex of a loop, rounded to nanometres: its start-point key.
std::pair<Nm, Nm> vertexKey(const Vec2& p)
{
    return {toNm(p.x), toNm(p.y)};
}
std::pair<Nm, Nm> loopKey(const ProfileLoop& loop)
{
    std::pair<Nm, Nm> best{std::numeric_limits<Nm>::max(), std::numeric_limits<Nm>::max()};
    for (const ProfileCurve& c : loop.curves) {
        const Vec2 p = c.kind == Kind::Circle ? Vec2{c.pts[0].x - c.radius, c.pts[0].y} : c.pts[0];
        best = std::min(best, vertexKey(p));
    }
    return best;
}

// Puts the curves of a wire in chain order when the wire explorer could not (start of each = end of the previous).
void chainCurves(std::vector<ProfileCurve>& curves)
{
    if (curves.size() < 2)
        return;
    std::vector<ProfileCurve> chained;
    chained.reserve(curves.size());
    std::vector<bool> used(curves.size(), false);
    chained.push_back(curves[0]);
    used[0] = true;
    for (std::size_t n = 1; n < curves.size(); ++n) {
        const Vec2 end = curveEnd(chained.back());
        std::size_t best = curves.size();
        double bestDistance = 0.0;
        for (std::size_t i = 0; i < curves.size(); ++i) {
            if (used[i])
                continue;
            const double d = dist(curveStart(curves[i]), end);
            if (best == curves.size() || d < bestDistance) {
                best = i;
                bestDistance = d;
            }
        }
        used[best] = true;
        chained.push_back(curves[best]);
    }
    curves = std::move(chained);
}

bool chainedInOrder(const std::vector<ProfileCurve>& curves)
{
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const ProfileCurve& next = curves[(i + 1) % curves.size()];
        if (dist(curveEnd(curves[i]), curveStart(next)) > 1e-4)
            return false;
    }
    return true;
}

} // namespace

Result<gp_Trsf> rigidTrsf(const Xform& frame, std::string_view where)
{
    const auto r = [&](int i, int j) {
        return frame.m[i * 4 + j];
    };
    for (const double value : frame.m)
        if (!std::isfinite(value))
            return tl::unexpected(makeError(ErrorCode::InvalidArgument, "The frame must be finite.", where));
    constexpr double kRigidTolerance = 1e-9;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double dot = r(i, 0) * r(j, 0) + r(i, 1) * r(j, 1) + r(i, 2) * r(j, 2);
            if (std::abs(dot - (i == j ? 1.0 : 0.0)) > kRigidTolerance)
                return tl::unexpected(makeError(ErrorCode::InvalidArgument,
                                                "Only rigid frames are allowed: the frame scales or skews.", where));
        }
    const double det = r(0, 0) * (r(1, 1) * r(2, 2) - r(1, 2) * r(2, 1)) -
                       r(0, 1) * (r(1, 0) * r(2, 2) - r(1, 2) * r(2, 0)) +
                       r(0, 2) * (r(1, 0) * r(2, 1) - r(1, 1) * r(2, 0));
    if (!(det > 0.0))
        return tl::unexpected(
            makeError(ErrorCode::InvalidArgument, "Only rigid frames are allowed: the frame mirrors.", where));
    gp_Trsf trsf;
    trsf.SetValues(r(0, 0), r(0, 1), r(0, 2), r(0, 3), r(1, 0), r(1, 1), r(1, 2), r(1, 3), r(2, 0), r(2, 1), r(2, 2),
                   r(2, 3));
    return trsf;
}

TopoDS_Shape topologyCopy(const TopoDS_Shape& shape)
{
    BRepBuilderAPI_Copy copier(shape, false, false);
    return copier.Shape();
}

void appendProjectedEdge(const TopoDS_Edge& edge, bool reversed, std::vector<ProfileCurve>& out)
{
    if (BRep_Tool::Degenerated(edge))
        return;
    const BRepAdaptor_Curve curve(edge);
    const double t0 = curve.FirstParameter(), t1 = curve.LastParameter();
    TopoDS_Vertex v0, v1;
    TopExp::Vertices(edge, v0, v1); // v0 at the first parameter, v1 at the last, whatever the edge's orientation
    Vec2 start = v0.IsNull() ? xy(curve.Value(t0)) : xy(BRep_Tool::Pnt(v0));
    Vec2 end = v1.IsNull() ? xy(curve.Value(t1)) : xy(BRep_Tool::Pnt(v1));
    if (reversed)
        std::swap(start, end);

    switch (curve.GetType()) {
    case GeomAbs_Line:
        if (dist(start, end) > kDegenerateLength)
            out.push_back(makeLine(start, end));
        return;
    case GeomAbs_Circle: {
        const gp_Circ circle = curve.Circle();
        const gp_Dir axis = circle.Axis().Direction();
        if (std::hypot(axis.X(), axis.Y()) <= kParallelSin) {
            const Vec2 centre = xy(circle.Location());
            const bool ccw = (axis.Z() > 0.0) != reversed;
            const bool whole = std::abs((t1 - t0) - 2.0 * kPi) <= 1e-9 || BRep_Tool::IsClosed(edge);
            if (whole && dist(start, end) <= kJoin) {
                out.push_back(makeCircle(centre, circle.Radius(), ccw));
            } else if (dist(start, end) <= kJoin) {
                // A closed edge that is not a whole turn by its parameters: two halves keep it unambiguous.
                const double tm = 0.5 * (t0 + t1);
                const Vec2 mid = xy(curve.Value(tm));
                out.push_back(makeArc(start, mid, centre, circle.Radius(), ccw));
                out.push_back(makeArc(mid, end, centre, circle.Radius(), ccw));
            } else {
                out.push_back(makeArc(start, end, centre, circle.Radius(), ccw));
            }
            return;
        }
        break;
    }
    default:
        break;
    }
    Sampler(curve, t0, t1, reversed, start, end).append(out);
}

std::vector<ProfileLoop> faceWireLoops(const TopoDS_Face& face)
{
    std::vector<ProfileLoop> loops;
    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    std::vector<TopoDS_Wire> wires;
    if (!outer.IsNull())
        wires.push_back(outer);
    for (TopoDS_Iterator it(face); it.More(); it.Next())
        if (it.Value().ShapeType() == TopAbs_WIRE && !it.Value().IsSame(outer))
            wires.push_back(TopoDS::Wire(it.Value()));
    for (const TopoDS_Wire& wire : wires) {
        ProfileLoop loop;
        int edgeCount = 0;
        for (TopExp_Explorer ex(wire, TopAbs_EDGE); ex.More(); ex.Next())
            ++edgeCount;
        int explored = 0;
        for (BRepTools_WireExplorer we(wire, face); we.More(); we.Next()) {
            appendProjectedEdge(we.Current(), we.Orientation() == TopAbs_REVERSED, loop.curves);
            ++explored;
        }
        if (explored != edgeCount || !chainedInOrder(loop.curves)) {
            // The wire explorer stops at a vertex with more than two edges: take the edges as stored and chain them.
            loop.curves.clear();
            for (TopoDS_Iterator it(wire); it.More(); it.Next())
                if (it.Value().ShapeType() == TopAbs_EDGE)
                    appendProjectedEdge(TopoDS::Edge(it.Value()), it.Value().Orientation() == TopAbs_REVERSED,
                                        loop.curves);
            chainCurves(loop.curves);
        }
        if (!loop.curves.empty())
            loops.push_back(std::move(loop));
    }
    return loops;
}

double loopArea(const ProfileLoop& loop)
{
    double twice = 0.0;
    for (const ProfileCurve& c : loop.curves) {
        if (c.kind == Kind::Circle) {
            const double a = kPi * c.radius * c.radius;
            twice += 2.0 * (c.ccw ? a : -a);
            continue;
        }
        const Vec2& a = c.pts[0];
        const Vec2& b = c.pts[1];
        twice += a.x * b.y - a.y * b.x;
        if (c.kind == Kind::Arc) {
            const double r = c.radius > 0.0 ? c.radius : dist(a, c.pts[2]);
            const double sweep = sweepOf(a, b, c.pts[2], c.ccw);
            const double segment = 0.5 * r * r * (sweep - std::sin(sweep));
            twice += 2.0 * (c.ccw ? segment : -segment);
        }
    }
    return 0.5 * twice;
}

void reverseLoop(ProfileLoop& loop)
{
    std::reverse(loop.curves.begin(), loop.curves.end());
    for (ProfileCurve& c : loop.curves) {
        if (c.kind == Kind::Line || c.kind == Kind::Arc)
            std::swap(c.pts[0], c.pts[1]);
        else if (c.kind == Kind::Bspline)
            std::reverse(c.pts.begin(), c.pts.end());
        c.ccw = !c.ccw;
    }
}

bool orientLoop(ProfileLoop& loop, bool ccw)
{
    const double area = loopArea(loop);
    if (!(std::abs(area) >= kMinLoopArea))
        return false;
    if ((area > 0.0) != ccw)
        reverseLoop(loop);
    return true;
}

void startLoop(ProfileLoop& loop)
{
    if (loop.curves.size() < 2)
        return;
    std::size_t best = 0;
    for (std::size_t i = 1; i < loop.curves.size(); ++i)
        if (vertexKey(loop.curves[i].pts[0]) < vertexKey(loop.curves[best].pts[0]))
            best = i;
    std::rotate(loop.curves.begin(), loop.curves.begin() + static_cast<std::ptrdiff_t>(best), loop.curves.end());
}

std::vector<ProfileLoop> orderRegions(std::vector<LoopRegion> regions)
{
    struct Keyed {
        std::pair<Nm, Nm> key;
        double area;
        LoopRegion region;
    };
    std::vector<Keyed> keyed;
    for (LoopRegion& region : regions) {
        if (!orientLoop(region.outer, true))
            continue;
        startLoop(region.outer);
        std::vector<ProfileLoop> holes;
        for (ProfileLoop& hole : region.holes) {
            if (!orientLoop(hole, false))
                continue;
            startLoop(hole);
            holes.push_back(std::move(hole));
        }
        std::stable_sort(holes.begin(), holes.end(), [](const ProfileLoop& a, const ProfileLoop& b) {
            return std::make_pair(loopKey(a), loopArea(a)) < std::make_pair(loopKey(b), loopArea(b));
        });
        region.holes = std::move(holes);
        const auto key = loopKey(region.outer);
        const double area = loopArea(region.outer);
        keyed.push_back(Keyed{key, area, std::move(region)});
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const Keyed& a, const Keyed& b) { return std::tie(a.key, a.area) < std::tie(b.key, b.area); });
    std::vector<ProfileLoop> out;
    for (Keyed& k : keyed) {
        out.push_back(std::move(k.region.outer));
        for (ProfileLoop& hole : k.region.holes)
            out.push_back(std::move(hole));
    }
    return out;
}

std::vector<LoopRegion> regionsOfFaces(const std::vector<TopoDS_Face>& faces)
{
    std::vector<LoopRegion> regions;
    for (const TopoDS_Face& face : faces) {
        std::vector<ProfileLoop> loops = faceWireLoops(face);
        if (loops.empty())
            continue;
        // The outer wire encloses the others: the one of largest area (OuterWire is a guess OCCT makes from the
        // wires' boxes, which a hole touching the outline can fool).
        std::size_t outer = 0;
        for (std::size_t i = 1; i < loops.size(); ++i)
            if (std::abs(loopArea(loops[i])) > std::abs(loopArea(loops[outer])))
                outer = i;
        LoopRegion region;
        region.outer = std::move(loops[outer]);
        for (std::size_t i = 0; i < loops.size(); ++i)
            if (i != outer)
                region.holes.push_back(std::move(loops[i]));
        regions.push_back(std::move(region));
    }
    return regions;
}

} // namespace ray::kernel::detail
