// minInternalCornerRadius (task T-050): the smallest concave radius a cutter coming down the tool axis meets, which
// limits the tool's diameter. Rules (kernel.h):
//   - a cylinder whose axis is parallel to the tool axis (within kAxisCos) and whose material lies outside it (the
//     outward normal points at the axis): its radius. Holes count: a hole limits the tool that machines it.
//   - a cone about the tool axis that is steeper than 45 degrees (semi-angle below kSteepSemiAngle: a drafted wall or
//     corner, not a drill point, chamfer or countersink) with its material outside: its smallest radius on the face.
//   - a B-spline or other free-form face that OCCT recognises as such a cylinder within kRecogniseTolerance (STEP
//     files often carry fillets as B-splines): its radius.
//   - a sharp internal corner: a straight edge within 45 degrees of the tool axis between two faces that are both
//     walls (normals within 45 degrees of perpendicular to the axis) meeting concavely (ChFi3d::DefineConnectType,
//     tangent within kTangentSin counts as not sharp): 0.
//   Tori and spheres (blends to a floor, ball ends) are not corners seen along the axis and do not count.
// The result is +infinity when nothing concave limits the tool.
#include "boolean_internal.h"
#include "slice_internal.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <ChFi3d.hxx>
#include <ShapeAnalysis_CanonicalRecognition.hxx>
#include <TopoDS.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <limits>

namespace ray::kernel {

namespace {

using detail::guarded;
using detail::makeError;

constexpr double kAxisCos = 1.0 - 1e-9;              // an axis is parallel to the tool axis when |cos| >= this
constexpr double kSteepSemiAngle = 0.7853981633974483; // 45 degrees
constexpr double kWallCos = 0.7071067811865476;       // cos 45: wall normals and steep edges
constexpr double kTangentSin = 0.008726535498373935;  // sin 0.5 degree
constexpr double kRecogniseTolerance = 1e-4;          // mm

gp_Vec outwardNormal(const TopoDS_Face& face, gp_Pnt& point)
{
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const BRepAdaptor_Surface surface(face, true);
    gp_Vec du, dv;
    surface.D1(0.5 * (u0 + u1), 0.5 * (v0 + v1), point, du, dv);
    gp_Vec n = du.Crossed(dv);
    if (n.Magnitude() > 1e-300)
        n.Normalize();
    if (face.Orientation() == TopAbs_REVERSED)
        n.Reverse();
    return n;
}

// Material outside the surface of revolution about the axis (A, D): the outward normal points towards the axis.
bool concaveAbout(const gp_Pnt& p, const gp_Vec& n, const gp_Pnt& a, const gp_Dir& d)
{
    gp_Vec radial(a, p);
    radial -= gp_Vec(d) * radial.Dot(gp_Vec(d));
    return radial.Magnitude() > 1e-12 && n.Dot(radial) < 0.0;
}

// The radius a face limits the tool to, or +infinity.
double faceLimit(const TopoDS_Face& face, const gp_Dir& axis)
{
    constexpr double kNone = std::numeric_limits<double>::infinity();
    const BRepAdaptor_Surface surface(face, true);
    gp_Pnt p;
    const gp_Vec n = outwardNormal(face, p);
    switch (surface.GetType()) {
    case GeomAbs_Cylinder: {
        const gp_Cylinder c = surface.Cylinder();
        if (std::abs(c.Axis().Direction().Dot(axis)) < kAxisCos)
            return kNone;
        return concaveAbout(p, n, c.Location(), c.Axis().Direction()) ? c.Radius() : kNone;
    }
    case GeomAbs_Cone: {
        const gp_Cone c = surface.Cone();
        if (std::abs(c.Axis().Direction().Dot(axis)) < kAxisCos || std::abs(c.SemiAngle()) >= kSteepSemiAngle)
            return kNone;
        if (!concaveAbout(p, n, c.Location(), c.Axis().Direction()))
            return kNone;
        double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        const double s = std::sin(c.SemiAngle());
        const double r0 = c.RefRadius() + v0 * s, r1 = c.RefRadius() + v1 * s;
        return std::max(0.0, std::min(r0, r1));
    }
    case GeomAbs_Plane:
    case GeomAbs_Sphere:
    case GeomAbs_Torus:
    case GeomAbs_SurfaceOfRevolution:
    case GeomAbs_SurfaceOfExtrusion:
        return kNone;
    default: {
        ShapeAnalysis_CanonicalRecognition recognise(face);
        gp_Cylinder c;
        if (!recognise.IsCylinder(kRecogniseTolerance, c))
            return kNone;
        if (std::abs(c.Axis().Direction().Dot(axis)) < kAxisCos)
            return kNone;
        return concaveAbout(p, n, c.Location(), c.Axis().Direction()) ? c.Radius() : kNone;
    }
    }
}

bool isWall(const TopoDS_Face& face, const gp_Dir& axis)
{
    gp_Pnt p;
    const gp_Vec n = outwardNormal(face, p);
    return std::abs(n.Dot(gp_Vec(axis))) <= kWallCos;
}

// A sharp internal corner along the tool axis.
bool sharpCorner(const TopoDS_Edge& edge, const TopoDS_Face& f1, const TopoDS_Face& f2, const gp_Dir& axis)
{
    if (BRep_Tool::Degenerated(edge) || f1.IsSame(f2))
        return false;
    const BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Line)
        return false;
    if (std::abs(curve.Line().Direction().Dot(axis)) < kWallCos)
        return false;
    if (!isWall(f1, axis) || !isWall(f2, axis))
        return false;
    return ChFi3d::DefineConnectType(edge, f1, f2, kTangentSin, false) == ChFiDS_Concave;
}

} // namespace

Result<double> minInternalCornerRadius(const Shape& shape, Vec3 toolAxis, JobContext& ctx)
{
    constexpr std::string_view where = "kernel::minInternalCornerRadius";
    if (auto ok = detail::requireShape(shape, where); !ok)
        return tl::unexpected(ok.error());
    const double length = std::sqrt(toolAxis.x * toolAxis.x + toolAxis.y * toolAxis.y + toolAxis.z * toolAxis.z);
    if (!std::isfinite(length) || length <= 1e-12)
        return tl::unexpected(makeError(ErrorCode::InvalidArgument, "The tool axis must be a non-zero direction.", where));
    return guarded(where, [&]() -> Result<double> {
        const gp_Dir axis(toolAxis.x / length, toolAxis.y / length, toolAxis.z / length);
        double best = std::numeric_limits<double>::infinity();
        const detail::ShapeIndex& faces = shape.impl->subs[detail::kindSlot(SubKind::Face)];
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (i % 64 == 0)
                if (auto st = ctx.cancel.check(); !st)
                    return tl::unexpected(st.error());
            best = std::min(best, faceLimit(TopoDS::Face(faces(i)), axis));
        }
        const detail::AncestorIndex& edgeFaces = shape.impl->ancestors(SubKind::Edge, SubKind::Face);
        for (int i = 1; i <= edgeFaces.Extent(); ++i) {
            if (i % 64 == 0)
                if (auto st = ctx.cancel.check(); !st)
                    return tl::unexpected(st.error());
            const NCollection_List<TopoDS_Shape>& adjacentFaces = edgeFaces(i);
            if (adjacentFaces.Extent() != 2)
                continue;
            const TopoDS_Edge& edge = TopoDS::Edge(edgeFaces.FindKey(i));
            if (sharpCorner(edge, TopoDS::Face(adjacentFaces.First()), TopoDS::Face(adjacentFaces.Last()), axis))
                return 0.0;
        }
        ctx.progress.set(1.0, "Corner radius");
        return best;
    });
}

} // namespace ray::kernel
