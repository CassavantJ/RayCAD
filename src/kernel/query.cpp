// Geometry queries on a Shape (task T-004): surfaceInfo, curveInfo, vertexPoint, adjacent, bounds,
// massProps, minDistance. Every query reads the exact geometry, never a mesh, and runs single-threaded.
#include "shape_impl.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Elips.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

#include <algorithm>
#include <cmath>

namespace ray::kernel {

namespace {

using detail::guarded;
using detail::makeError;
using detail::toVec3;

Result<TopoDS_Shape> resolveKind(const Shape& shape, SubId id, SubKind expected, const char* what,
                                 std::string_view where)
{
    if (id.kind != expected && detail::validKind(id.kind) && !shape.isNull())
        return tl::unexpected(
            makeError(ErrorCode::InvalidArgument, std::string("The sub-shape is not ") + what + ".", where));
    return detail::resolve(shape, id, where);
}

Box3 toBox(const Bnd_Box& box)
{
    if (box.IsVoid())
        return Box3{};
    double xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    return Box3{Vec3{xmin, ymin, zmin}, Vec3{xmax, ymax, zmax}};
}

// Tight box from the exact geometry: no triangulation, no tolerance enlargement.
Box3 tightBounds(const TopoDS_Shape& shape)
{
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, false, false);
    return toBox(box);
}

// The kind whose elements connect two elements of the same kind in adjacent(): faces meet at edges, edges at
// vertices, solids at faces, and two vertices are neighbours when an edge joins them.
SubKind bridgeKind(SubKind kind)
{
    switch (kind) {
    case SubKind::Vertex:
        return SubKind::Edge;
    case SubKind::Edge:
        return SubKind::Vertex;
    case SubKind::Face:
        return SubKind::Edge;
    case SubKind::Solid:
        return SubKind::Face;
    }
    return SubKind::Edge;
}

} // namespace

// ---------- surfaceInfo ----------
// origin/axis per type: Plane: a point of the plane and its normal; Cylinder: a point of the axis and its
// direction; Cone: the centre of the reference circle, the axis, radius = reference radius, semiAngle;
// Sphere: centre, polar axis; Torus: centre, axis, radius = major radius. Axes are those of the underlying
// surface. `reversed` is true when the face is used reversed in the stored shape, i.e. its material-side
// normal is the opposite of the surface normal. Bspline and Other leave origin, axis and radius zero.
Result<SurfaceInfo> surfaceInfo(const Shape& shape, SubId face)
{
    constexpr std::string_view where = "kernel::surfaceInfo";
    return guarded(where, [&]() -> Result<SurfaceInfo> {
        auto sub = resolveKind(shape, face, SubKind::Face, "a face", where);
        if (!sub)
            return tl::unexpected(sub.error());
        const TopoDS_Face& topoFace = TopoDS::Face(*sub);

        SurfaceInfo info{};
        const BRepAdaptor_Surface surface(topoFace, true);
        switch (surface.GetType()) {
        case GeomAbs_Plane: {
            const gp_Pln plane = surface.Plane();
            info.type = SurfaceType::Plane;
            info.origin = toVec3(plane.Location());
            info.axis = toVec3(plane.Axis().Direction());
            break;
        }
        case GeomAbs_Cylinder: {
            const gp_Cylinder cylinder = surface.Cylinder();
            info.type = SurfaceType::Cylinder;
            info.origin = toVec3(cylinder.Location());
            info.axis = toVec3(cylinder.Axis().Direction());
            info.radius = cylinder.Radius();
            break;
        }
        case GeomAbs_Cone: {
            const gp_Cone cone = surface.Cone();
            info.type = SurfaceType::Cone;
            info.origin = toVec3(cone.Location());
            info.axis = toVec3(cone.Axis().Direction());
            info.radius = cone.RefRadius();
            info.semiAngle = cone.SemiAngle();
            break;
        }
        case GeomAbs_Sphere: {
            const gp_Sphere sphere = surface.Sphere();
            info.type = SurfaceType::Sphere;
            info.origin = toVec3(sphere.Location());
            info.axis = toVec3(sphere.Position().Direction());
            info.radius = sphere.Radius();
            break;
        }
        case GeomAbs_Torus: {
            const gp_Torus torus = surface.Torus();
            info.type = SurfaceType::Torus;
            info.origin = toVec3(torus.Location());
            info.axis = toVec3(torus.Axis().Direction());
            info.radius = torus.MajorRadius();
            break;
        }
        case GeomAbs_BSplineSurface:
            info.type = SurfaceType::Bspline;
            break;
        default:
            info.type = SurfaceType::Other;
            break;
        }

        GProp_GProps props;
        BRepGProp::SurfaceProperties(topoFace, props); // exact geometry: UseTriangulation is off
        info.area = props.Mass();
        info.centroid = toVec3(props.CentreOfMass());
        info.bounds = tightBounds(topoFace);
        info.reversed = topoFace.Orientation() == TopAbs_REVERSED;
        return info;
    });
}

// ---------- curveInfo ----------
// start/end follow the curve's own parameter direction (first to last parameter), whatever the edge's
// orientation in any face. Line: center = midpoint, axis = unit direction start to end. Circle: center,
// axis (normal of the circle's plane), radius. Ellipse: center, axis, radius = major radius. Bspline and
// Other: center and axis zero. closed: the edge starts and ends at the same vertex. A degenerated edge (no 3D
// curve, e.g. at a cone apex) is reported as Other with length 0 at its vertex.
Result<CurveInfo> curveInfo(const Shape& shape, SubId edge)
{
    constexpr std::string_view where = "kernel::curveInfo";
    return guarded(where, [&]() -> Result<CurveInfo> {
        auto sub = resolveKind(shape, edge, SubKind::Edge, "an edge", where);
        if (!sub)
            return tl::unexpected(sub.error());
        const TopoDS_Edge topoEdge = TopoDS::Edge(sub->Oriented(TopAbs_FORWARD));

        CurveInfo info{};
        TopoDS_Vertex first;
        TopoDS_Vertex last;
        TopExp::Vertices(topoEdge, first, last);
        info.closed = !first.IsNull() && first.IsSame(last);

        if (BRep_Tool::Degenerated(topoEdge)) {
            info.type = CurveType::Other;
            if (!first.IsNull())
                info.start = info.end = info.center = toVec3(BRep_Tool::Pnt(first));
            return info;
        }

        const BRepAdaptor_Curve curve(topoEdge);
        const gp_Pnt start = curve.Value(curve.FirstParameter());
        const gp_Pnt end = curve.Value(curve.LastParameter());
        info.start = toVec3(start);
        info.end = toVec3(end);
        switch (curve.GetType()) {
        case GeomAbs_Line: {
            info.type = CurveType::Line;
            info.center = toVec3((start.XYZ() + end.XYZ()) * 0.5);
            info.axis = toVec3(curve.Line().Direction());
            break;
        }
        case GeomAbs_Circle: {
            const gp_Circ circle = curve.Circle();
            info.type = CurveType::Circle;
            info.center = toVec3(circle.Location());
            info.axis = toVec3(circle.Axis().Direction());
            info.radius = circle.Radius();
            break;
        }
        case GeomAbs_Ellipse: {
            const gp_Elips ellipse = curve.Ellipse();
            info.type = CurveType::Ellipse;
            info.center = toVec3(ellipse.Location());
            info.axis = toVec3(ellipse.Axis().Direction());
            info.radius = ellipse.MajorRadius();
            break;
        }
        case GeomAbs_BSplineCurve:
            info.type = CurveType::Bspline;
            break;
        default:
            info.type = CurveType::Other;
            break;
        }

        GProp_GProps props;
        BRepGProp::LinearProperties(topoEdge, props);
        info.length = props.Mass();
        return info;
    });
}

// ---------- vertexPoint ----------
Result<Vec3> vertexPoint(const Shape& shape, SubId vertex)
{
    constexpr std::string_view where = "kernel::vertexPoint";
    return guarded(where, [&]() -> Result<Vec3> {
        auto sub = resolveKind(shape, vertex, SubKind::Vertex, "a vertex", where);
        if (!sub)
            return tl::unexpected(sub.error());
        return toVec3(BRep_Tool::Pnt(TopoDS::Vertex(*sub)));
    });
}

// ---------- adjacent ----------
// wanted below `of` (e.g. the edges of a face): the sub-shapes of that kind it contains.
// wanted above `of` (e.g. the faces of an edge): the elements of that kind that contain it.
// wanted == of: neighbours through bridgeKind() (faces sharing an edge, edges sharing a vertex, vertices
// joined by an edge, solids sharing a face), the element itself excluded.
// Sorted ascending, no duplicates. Empty for a null shape, an invalid SubId or an invalid kind.
std::vector<SubId> adjacent(const Shape& shape, SubId of, SubKind wanted)
{
    constexpr std::string_view where = "kernel::adjacent";
    auto result = guarded(where, [&]() -> Result<std::vector<SubId>> {
        auto sub = detail::resolve(shape, of, where);
        if (!sub)
            return tl::unexpected(sub.error());
        if (!detail::validKind(wanted))
            return tl::unexpected(makeError(ErrorCode::InvalidArgument, "Unknown sub-shape kind.", where));

        const Shape::Impl& impl = *shape.impl;
        std::vector<SubId> out;
        auto add = [&](const TopoDS_Shape& element, SubKind kind) {
            const int index = impl.subs[detail::kindSlot(kind)].FindIndex(element);
            if (index > 0)
                out.push_back(SubId{kind, static_cast<std::uint32_t>(index - 1)});
        };
        auto addContained = [&](const TopoDS_Shape& from, SubKind kind) {
            detail::ShapeIndex local;
            TopExp::MapShapes(from, detail::occtKind(kind), local);
            for (int i = 1; i <= local.Extent(); ++i)
                add(local.FindKey(i), kind);
        };
        auto addContaining = [&](const TopoDS_Shape& element, SubKind elementKind, SubKind kind) {
            const NCollection_List<TopoDS_Shape>* owners = impl.ancestors(elementKind, kind).Seek(element);
            if (owners == nullptr)
                return;
            for (NCollection_List<TopoDS_Shape>::Iterator it(*owners); it.More(); it.Next())
                add(it.Value(), kind);
        };

        const SubKind kind = of.kind;
        if (wanted < kind) {
            addContained(*sub, wanted);
        } else if (wanted > kind) {
            addContaining(*sub, kind, wanted);
        } else {
            const SubKind bridge = bridgeKind(kind);
            detail::ShapeIndex bridges;
            if (bridge < kind) {
                TopExp::MapShapes(*sub, detail::occtKind(bridge), bridges);
                for (int i = 1; i <= bridges.Extent(); ++i)
                    addContaining(bridges.FindKey(i), bridge, kind);
            } else {
                const NCollection_List<TopoDS_Shape>* owners = impl.ancestors(kind, bridge).Seek(*sub);
                if (owners != nullptr)
                    for (NCollection_List<TopoDS_Shape>::Iterator it(*owners); it.More(); it.Next())
                        addContained(it.Value(), kind);
            }
            std::erase(out, of);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    });
    return result ? std::move(*result) : std::vector<SubId>{};
}

// ---------- bounds ----------
// Tight axis-aligned box of the exact geometry (no mesh, no tolerance added). Box3{} for a null shape or on
// failure.
Box3 bounds(const Shape& shape)
{
    constexpr std::string_view where = "kernel::bounds";
    auto result = guarded(where, [&]() -> Result<Box3> {
        if (auto ok = detail::requireShape(shape, where); !ok)
            return tl::unexpected(ok.error());
        return tightBounds(shape.impl->shape);
    });
    return result ? *result : Box3{};
}

// ---------- massProps ----------
// volume: of the solids (0 if the shape has none). area: of all faces. centroid: of the solids' volume; if
// there are no solids, of the faces' area; if no faces, of the edges' length; else of the vertices.
// `density` must be positive and finite; MassProps has no mass field, so it changes nothing else (see the
// T-004 report).
Result<MassProps> massProps(const Shape& shape, double density)
{
    constexpr std::string_view where = "kernel::massProps";
    if (!(density > 0.0) || !std::isfinite(density))
        return tl::unexpected(makeError(ErrorCode::InvalidArgument, "Density must be positive and finite.", where));
    return guarded(where, [&]() -> Result<MassProps> {
        if (auto ok = detail::requireShape(shape, where); !ok)
            return tl::unexpected(ok.error());
        const TopoDS_Shape& stored = shape.impl->shape;
        MassProps out{};

        GProp_GProps surface;
        if (shape.count(SubKind::Face) > 0) {
            BRepGProp::SurfaceProperties(stored, surface);
            out.area = surface.Mass();
        }
        if (shape.count(SubKind::Solid) > 0) {
            GProp_GProps volume;
            BRepGProp::VolumeProperties(stored, volume);
            out.volume = volume.Mass();
            out.centroid = toVec3(volume.CentreOfMass());
        } else if (shape.count(SubKind::Face) > 0) {
            out.centroid = toVec3(surface.CentreOfMass());
        } else if (shape.count(SubKind::Edge) > 0) {
            GProp_GProps linear;
            BRepGProp::LinearProperties(stored, linear);
            out.centroid = toVec3(linear.CentreOfMass());
        } else {
            const detail::ShapeIndex& vertices = shape.impl->subs[detail::kindSlot(SubKind::Vertex)];
            gp_XYZ sum(0, 0, 0);
            for (int i = 1; i <= vertices.Extent(); ++i)
                sum += BRep_Tool::Pnt(TopoDS::Vertex(vertices.FindKey(i))).XYZ();
            if (vertices.Extent() > 0)
                out.centroid = toVec3(sum / static_cast<double>(vertices.Extent()));
        }
        if (!std::isfinite(out.volume) || !std::isfinite(out.area))
            return tl::unexpected(
                makeError(ErrorCode::KernelFailure, "The mass properties could not be computed.", where));
        return out;
    });
}

// ---------- minDistance ----------
// Smallest distance between two sub-shapes (any kinds, possibly of different shapes), with a closest pair
// of points: p1 on `a`, p2 on `b`. Where several pairs are equally close (parallel faces) the first one
// BRepExtrema reports is returned; it runs single-threaded, so that choice is repeatable.
Result<Distance> minDistance(const Shape& shapeA, SubId a, const Shape& shapeB, SubId b)
{
    constexpr std::string_view where = "kernel::minDistance";
    return guarded(where, [&]() -> Result<Distance> {
        auto first = detail::resolve(shapeA, a, where);
        if (!first)
            return tl::unexpected(first.error());
        auto second = detail::resolve(shapeB, b, where);
        if (!second)
            return tl::unexpected(second.error());
        BRepExtrema_DistShapeShape extrema;
        extrema.SetMultiThread(false);
        extrema.LoadS1(*first);
        extrema.LoadS2(*second);
        if (!extrema.Perform() || !extrema.IsDone() || extrema.NbSolution() < 1)
            return tl::unexpected(makeError(ErrorCode::KernelFailure, "The distance could not be computed.", where));
        return Distance{extrema.Value(), toVec3(extrema.PointOnShape1(1)), toVec3(extrema.PointOnShape2(1))};
    });
}

} // namespace ray::kernel
