// Profile -> planar face for extrude and revolve (task T-094). The convention is in sweep_internal.h.
//
// HOW THE LOOPS ARE BUILT. Every joint of a loop becomes ONE vertex, shared by the two edges that meet there, at
// the start point of the curve that leaves it. Lines run exactly between their two joint vertices; a B-spline's
// end control points are moved onto them; an arc keeps its centre and the radius to its start point, and its
// vertices get a tolerance that covers the (at most 1e-6 mm) gap to its end point. So a loop is closed by
// construction and no gap is healed by guesswork.
#include "sweep_internal.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <ElCLib.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <NCollection_Array1.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace ray::kernel::detail {

namespace {

constexpr double kJoinTolerance = 1e-6; // mm: joints closer than this are the same point
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr int kSamplesPerEdge = 32;

using Kind = ProfileCurve::Kind;

Error badProfile(const std::string& message, std::string_view where)
{
    return makeError(ErrorCode::InvalidArgument, message, where);
}

std::string at(std::size_t loop, std::size_t curve)
{
    return "Profile loop " + std::to_string(loop + 1) + ", curve " + std::to_string(curve + 1) + ": ";
}

std::string mm(double value)
{
    std::ostringstream text;
    text.precision(3);
    text << value << " mm";
    return text.str();
}

bool finite2(const Vec2& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y);
}
double dist2(const Vec2& a, const Vec2& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

// One profile curve, oriented along its loop.
struct Segment {
    Kind kind{};
    Vec2 start, end, centre;
    double radius = 0.0;
    bool ccw = true;
    std::vector<Vec2> poles;
    std::uint64_t tag = 0;
    std::size_t index = 0; // position in the input loop

    void reverse()
    {
        std::swap(start, end);
        ccw = !ccw;
        std::reverse(poles.begin(), poles.end());
    }
};

Result<Segment> readCurve(const ProfileCurve& curve, std::size_t loop, std::size_t index, std::string_view where)
{
    Segment s;
    s.kind = curve.kind;
    s.tag = curve.tag;
    s.index = index;
    s.ccw = curve.ccw;
    for (const Vec2& p : curve.pts)
        if (!finite2(p))
            return tl::unexpected(badProfile(at(loop, index) + "a point is not finite.", where));
    switch (curve.kind) {
    case Kind::Line:
        if (curve.pts.size() != 2)
            return tl::unexpected(badProfile(at(loop, index) + "a line needs exactly 2 points (start, end).", where));
        s.start = curve.pts[0];
        s.end = curve.pts[1];
        if (dist2(s.start, s.end) <= kJoinTolerance)
            return tl::unexpected(badProfile(at(loop, index) + "the line has no length.", where));
        return s;
    case Kind::Arc: {
        if (curve.pts.size() != 3)
            return tl::unexpected(
                badProfile(at(loop, index) + "an arc needs exactly 3 points (start, end, centre).", where));
        s.start = curve.pts[0];
        s.end = curve.pts[1];
        s.centre = curve.pts[2];
        s.radius = dist2(s.start, s.centre);
        if (!(s.radius > kJoinTolerance))
            return tl::unexpected(badProfile(at(loop, index) + "the arc has no radius.", where));
        if (std::abs(dist2(s.end, s.centre) - s.radius) > kJoinTolerance)
            return tl::unexpected(badProfile(at(loop, index) + "the arc's end point is " +
                                                 mm(std::abs(dist2(s.end, s.centre) - s.radius)) + " off its circle.",
                                             where));
        if (curve.radius != 0.0 && !(std::abs(curve.radius - s.radius) <= kJoinTolerance))
            return tl::unexpected(
                badProfile(at(loop, index) + "the arc's radius does not match its start point and centre.", where));
        if (dist2(s.start, s.end) <= kJoinTolerance)
            return tl::unexpected(
                badProfile(at(loop, index) + "the arc starts where it ends; use a circle for a full turn.", where));
        return s;
    }
    case Kind::Circle:
        if (curve.pts.size() != 1)
            return tl::unexpected(badProfile(at(loop, index) + "a circle needs exactly 1 point (its centre).", where));
        if (!std::isfinite(curve.radius) || !(curve.radius > kJoinTolerance))
            return tl::unexpected(badProfile(at(loop, index) + "the circle needs a positive radius.", where));
        s.centre = curve.pts[0];
        s.radius = curve.radius;
        return s;
    case Kind::Bspline:
        if (curve.pts.size() < 2)
            return tl::unexpected(badProfile(at(loop, index) + "a B-spline needs at least 2 control points.", where));
        s.poles = curve.pts;
        s.start = curve.pts.front();
        s.end = curve.pts.back();
        return s;
    }
    return tl::unexpected(badProfile(at(loop, index) + "unknown curve kind.", where));
}

// Orders the curves of one loop head to tail. Returns them oriented; the loop is closed within kJoinTolerance.
Result<std::vector<Segment>> chainLoop(const ProfileLoop& loop, std::size_t loopIndex, std::string_view where)
{
    if (loop.curves.empty())
        return tl::unexpected(badProfile("Profile loop " + std::to_string(loopIndex + 1) + " has no curves.", where));
    std::vector<Segment> segments;
    for (std::size_t i = 0; i < loop.curves.size(); ++i) {
        auto segment = readCurve(loop.curves[i], loopIndex, i, where);
        if (!segment)
            return tl::unexpected(segment.error());
        segments.push_back(std::move(*segment));
    }
    const bool hasCircle =
        std::any_of(segments.begin(), segments.end(), [](const Segment& s) { return s.kind == Kind::Circle; });
    if (hasCircle) {
        if (segments.size() != 1)
            return tl::unexpected(
                badProfile("Profile loop " + std::to_string(loopIndex + 1) +
                               ": a circle is a loop on its own and cannot be joined to other curves.",
                           where));
        return segments;
    }
    if (segments.size() == 1) {
        if (segments[0].kind == Kind::Bspline && dist2(segments[0].start, segments[0].end) <= kJoinTolerance &&
            segments[0].poles.size() >= 3)
            return segments;
        return tl::unexpected(badProfile(
            "Profile loop " + std::to_string(loopIndex + 1) + " is open: one curve does not close a loop.", where));
    }
    // The first curve keeps its direction unless only its start touches the second curve.
    const Segment& second = segments[1];
    const auto touches = [&](const Vec2& p) {
        return dist2(p, second.start) <= kJoinTolerance || dist2(p, second.end) <= kJoinTolerance;
    };
    if (!touches(segments[0].end) && touches(segments[0].start))
        segments[0].reverse();
    for (std::size_t i = 1; i < segments.size(); ++i) {
        const Vec2 joint = segments[i - 1].end;
        if (dist2(segments[i].start, joint) <= kJoinTolerance)
            continue;
        if (dist2(segments[i].end, joint) <= kJoinTolerance) {
            segments[i].reverse();
            continue;
        }
        const double gap = std::min(dist2(segments[i].start, joint), dist2(segments[i].end, joint));
        return tl::unexpected(badProfile(at(loopIndex, i) + "does not start where curve " + std::to_string(i) +
                                             " ends (gap " + mm(gap) + ").",
                                         where));
    }
    const double closing = dist2(segments.back().end, segments.front().start);
    if (closing > kJoinTolerance)
        return tl::unexpected(badProfile("Profile loop " + std::to_string(loopIndex + 1) +
                                             " is open: its last curve ends " + mm(closing) +
                                             " away from where its first curve starts.",
                                         where));
    return segments;
}

struct Frame {
    gp_Pnt origin;
    gp_Dir normal, xDir, yDir;
    gp_Pnt at(const Vec2& p) const { return gp_Pnt(origin.XYZ() + xDir.XYZ() * p.x + yDir.XYZ() * p.y); }
    gp_Pnt2d local(const gp_Pnt& p) const
    {
        const gp_XYZ d = p.XYZ() - origin.XYZ();
        return gp_Pnt2d(d.Dot(xDir.XYZ()), d.Dot(yDir.XYZ()));
    }
};

Result<Frame> makeFrame(const Plane& plane, std::string_view where)
{
    const auto finite3 = [](const Vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finite3(plane.origin) || !finite3(plane.normal) || !finite3(plane.xDir))
        return tl::unexpected(badProfile("The profile plane must be finite.", where));
    const gp_XYZ n(plane.normal.x, plane.normal.y, plane.normal.z);
    if (!(n.Modulus() > Precision::Confusion()))
        return tl::unexpected(badProfile("The profile plane's normal must not be a zero vector.", where));
    const gp_XYZ unitN = n / n.Modulus();
    gp_XYZ x(plane.xDir.x, plane.xDir.y, plane.xDir.z);
    x -= unitN * x.Dot(unitN);
    if (!(x.Modulus() > 1e-9 * std::max(1.0, std::hypot(plane.xDir.x, plane.xDir.y, plane.xDir.z))))
        return tl::unexpected(
            badProfile("The profile plane's x direction must not be zero or parallel to its normal.", where));
    Frame frame;
    frame.origin = gp_Pnt(plane.origin.x, plane.origin.y, plane.origin.z);
    frame.normal = gp_Dir(unitN);
    frame.xDir = gp_Dir(x);
    frame.yDir = frame.normal.Crossed(frame.xDir);
    return frame;
}

// Shoelace area of a closed polyline (positive counter-clockwise).
double signedArea(const std::vector<gp_Pnt2d>& poly)
{
    double twice = 0.0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const gp_Pnt2d& a = poly[i];
        const gp_Pnt2d& b = poly[(i + 1) % poly.size()];
        twice += a.X() * b.Y() - b.X() * a.Y();
    }
    return 0.5 * twice;
}

// Even-odd point in polygon.
bool inside(const std::vector<gp_Pnt2d>& poly, const gp_Pnt2d& p)
{
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const gp_Pnt2d& a = poly[i];
        const gp_Pnt2d& b = poly[j];
        if ((a.Y() > p.Y()) != (b.Y() > p.Y()) && p.X() < (b.X() - a.X()) * (p.Y() - a.Y()) / (b.Y() - a.Y()) + a.X())
            in = !in;
    }
    return in;
}

struct BuiltLoop {
    TopoDS_Wire wire;
    std::vector<ProfileEdge> edges;
    std::vector<gp_Pnt2d> polyline; // along the wire as built
};

TopoDS_Vertex makeVertex(const gp_Pnt& point, double tolerance)
{
    TopoDS_Vertex vertex;
    BRep_Builder().MakeVertex(vertex, point, std::max(tolerance, Precision::Confusion()));
    return vertex;
}

Result<BuiltLoop> buildLoop(const std::vector<Segment>& segments, const ProfileLoop& loop, std::size_t loopIndex,
                            const Frame& frame, std::string_view where)
{
    BuiltLoop out;
    BRepBuilderAPI_MakeWire wire;
    const auto edgeFailed = [&](const Segment& s) {
        return tl::unexpected(badProfile(at(loopIndex, s.index) + "the curve could not be built.", where));
    };
    if (segments.size() == 1 && segments[0].kind == Kind::Circle) {
        const Segment& s = segments[0];
        const gp_Circ circle(gp_Ax2(frame.at(s.centre), frame.normal, frame.xDir), s.radius);
        BRepBuilderAPI_MakeEdge edge(circle);
        if (!edge.IsDone())
            return edgeFailed(s);
        out.edges.push_back({edge.Edge(), s.tag, loop.tag});
        wire.Add(edge.Edge());
    } else {
        // Joint k is where segment k starts; segment k runs from joint k to joint k + 1 (mod n).
        const std::size_t n = segments.size();
        std::vector<double> tolerance(n, Precision::Confusion());
        std::vector<occ::handle<Geom_Circle>> circles(n);
        for (std::size_t k = 0; k < n; ++k) {
            const Segment& s = segments[k];
            if (s.kind != Kind::Arc)
                continue;
            const gp_Dir axis = s.ccw ? frame.normal : frame.normal.Reversed();
            circles[k] = new Geom_Circle(gp_Ax2(frame.at(s.centre), axis, frame.xDir), s.radius);
            const gp_Pnt next = frame.at(segments[(k + 1) % n].start);
            const double u = ElCLib::Parameter(circles[k]->Circ(), next);
            const double gap = circles[k]->Value(u).Distance(next);
            tolerance[(k + 1) % n] = std::max(tolerance[(k + 1) % n], 2.0 * gap);
        }
        std::vector<TopoDS_Vertex> joints(n);
        for (std::size_t k = 0; k < n; ++k)
            joints[k] = makeVertex(frame.at(segments[k].start), tolerance[k]);
        for (std::size_t k = 0; k < n; ++k) {
            const Segment& s = segments[k];
            const TopoDS_Vertex& v0 = joints[k];
            const TopoDS_Vertex& v1 = joints[(k + 1) % n];
            TopoDS_Edge built;
            if (s.kind == Kind::Line) {
                BRepBuilderAPI_MakeEdge edge(v0, v1);
                if (!edge.IsDone())
                    return edgeFailed(s);
                built = edge.Edge();
            } else if (s.kind == Kind::Arc) {
                const gp_Circ& circ = circles[k]->Circ();
                const double u0 = ElCLib::Parameter(circ, BRep_Tool::Pnt(v0));
                double u1 = ElCLib::Parameter(circ, BRep_Tool::Pnt(v1));
                if (u1 <= u0)
                    u1 += kTwoPi;
                BRepBuilderAPI_MakeEdge edge(circles[k], v0, v1, u0, u1);
                if (!edge.IsDone())
                    return edgeFailed(s);
                built = edge.Edge();
            } else { // Bspline
                std::vector<Vec2> poles = s.poles;
                const int count = static_cast<int>(poles.size());
                const int degree = std::min(3, count - 1);
                NCollection_Array1<gp_Pnt> points(1, count);
                for (int i = 0; i < count; ++i)
                    points.SetValue(i + 1, frame.at(poles[static_cast<std::size_t>(i)]));
                points.SetValue(1, BRep_Tool::Pnt(v0));
                points.SetValue(count, BRep_Tool::Pnt(v1));
                const int knotCount = count - degree + 1;
                NCollection_Array1<double> knots(1, knotCount);
                NCollection_Array1<int> mults(1, knotCount);
                for (int i = 1; i <= knotCount; ++i) {
                    knots.SetValue(i, static_cast<double>(i - 1) / static_cast<double>(knotCount - 1));
                    mults.SetValue(i, (i == 1 || i == knotCount) ? degree + 1 : 1);
                }
                occ::handle<Geom_BSplineCurve> curve = new Geom_BSplineCurve(points, knots, mults, degree);
                BRepBuilderAPI_MakeEdge edge(curve, v0, v1, curve->FirstParameter(), curve->LastParameter());
                if (!edge.IsDone())
                    return edgeFailed(s);
                built = edge.Edge();
            }
            out.edges.push_back({built, s.tag, loop.tag});
            wire.Add(built);
            if (!wire.IsDone())
                return tl::unexpected(
                    badProfile(at(loopIndex, s.index) + "the curve does not join the previous one.", where));
        }
    }
    if (!wire.IsDone())
        return tl::unexpected(
            badProfile("Profile loop " + std::to_string(loopIndex + 1) + " could not be joined into a loop.", where));
    out.wire = wire.Wire();
    if (!BRep_Tool::IsClosed(out.wire))
        return tl::unexpected(badProfile("Profile loop " + std::to_string(loopIndex + 1) + " is open.", where));
    for (const ProfileEdge& e : out.edges) {
        const BRepAdaptor_Curve curve(e.edge);
        const double first = curve.FirstParameter();
        const double last = curve.LastParameter();
        for (int i = 0; i < kSamplesPerEdge; ++i)
            out.polyline.push_back(frame.local(curve.Value(first + (last - first) * i / kSamplesPerEdge)));
    }
    return out;
}

} // namespace

Result<BuiltProfile> buildProfile(const Profile& profile, std::string_view where)
{
    auto frame = makeFrame(profile.plane, where);
    if (!frame)
        return tl::unexpected(frame.error());
    if (profile.loops.empty())
        return tl::unexpected(badProfile("The profile has no loops.", where));

    std::vector<BuiltLoop> loops;
    for (std::size_t i = 0; i < profile.loops.size(); ++i) {
        auto segments = chainLoop(profile.loops[i], i, where);
        if (!segments)
            return tl::unexpected(segments.error());
        auto loop = buildLoop(*segments, profile.loops[i], i, *frame, where);
        if (!loop)
            return tl::unexpected(loop.error());
        const double area = signedArea(loop->polyline);
        if (!(std::abs(area) > kJoinTolerance * kJoinTolerance))
            return tl::unexpected(badProfile("Profile loop " + std::to_string(i + 1) + " encloses no area.", where));
        // Outer loop counter-clockwise about the normal, holes clockwise.
        if ((i == 0) != (area > 0.0)) {
            loop->wire = TopoDS::Wire(loop->wire.Reversed());
            std::reverse(loop->polyline.begin(), loop->polyline.end());
        }
        loops.push_back(std::move(*loop));
    }
    for (std::size_t i = 1; i < loops.size(); ++i) {
        const gp_Pnt2d probe = loops[i].polyline.front();
        if (!inside(loops[0].polyline, probe))
            return tl::unexpected(badProfile("Profile loop " + std::to_string(i + 1) +
                                                 " lies outside the first loop. The first loop is the outer boundary "
                                                 "and every other loop must be a hole inside it.",
                                             where));
        for (std::size_t j = 1; j < loops.size(); ++j)
            if (j != i && inside(loops[j].polyline, probe))
                return tl::unexpected(badProfile("Profile loop " + std::to_string(i + 1) + " lies inside loop " +
                                                     std::to_string(j + 1) +
                                                     ". An island inside a hole must be a separate profile.",
                                                 where));
    }

    BuiltProfile out;
    out.frame = gp_Ax3(frame->origin, frame->normal, frame->xDir);
    out.outerTag = profile.loops.front().tag;
    BRepBuilderAPI_MakeFace face(gp_Pln(out.frame), loops[0].wire, true);
    for (std::size_t i = 1; i < loops.size() && face.IsDone(); ++i)
        face.Add(loops[i].wire);
    if (!face.IsDone())
        return tl::unexpected(badProfile("The profile could not be made into a face.", where));
    out.face = face.Face();
    if (!BRepCheck_Analyzer(out.face).IsValid())
        return tl::unexpected(badProfile(
            "The profile crosses itself, or its loops touch or overlap. Every loop must be a simple closed curve and "
            "holes must lie apart from each other and from the boundary.",
            where));
    for (BuiltLoop& loop : loops) {
        out.edges.insert(out.edges.end(), loop.edges.begin(), loop.edges.end());
        out.samples.insert(out.samples.end(), loop.polyline.begin(), loop.polyline.end());
    }
    return out;
}

} // namespace ray::kernel::detail
