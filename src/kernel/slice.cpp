// sliceAt, silhouette, faceLoops and edgeCurves (task T-050). kernel.h states the rules; slice_internal.h the curve
// and loop rules; this file how the sections are made.
//
// HOW A SLICE IS MADE. The shape (a topology copy, moved into the frame as a location) is intersected with a box that
// starts at height z and reaches past the shape on every other side (BRepAlgoAPI_Common, OCCT parallelism off). The
// faces of the result that lie on the box's bottom plane, with their outward normal pointing down, are exactly the
// closure of the material just above z: kernel.h's rule. This is why a section (BRepAlgoAPI_Section) is not used: a
// section returns edges, and at the height of a face (a pocket floor) it returns the floor's boundary together with
// the walls, so which side is material would have to be guessed; the common solid says it. The faces' wires give the
// loops directly, outer and holes, so no edge chaining or containment test is needed.
// Rules for the cases a plane through a solid can hit:
//   - z within kHeightSnap of a planar face perpendicular to Z is moved onto that face's height exactly (the nearest
//     such face; ties to the lower height), so a floor computed from a measured value still counts as "at the floor";
//   - a face tangent to the plane (a cylinder lying on its side, a sphere's pole, a fillet's tangent edge on a floor)
//     contributes no area: the boolean gives no face there, or a face of zero width that orderRegions drops;
//   - if OCCT cannot compute the boolean at z, it is computed once more at z + kRetryNudge (still the material just
//     above z, to within kRetryNudge times the steepest wall's slope); if that fails too, KernelFailure.
// For a slice from below (silhouette) the box ends at z and the faces looking up are taken.
//
// SILHOUETTE. The shadow of a closed solid along Z is the union of the shadows of the faces that look up. It is
// built as the union (OCCT general fuse of planar faces at z = 0, then ShapeUpgrade_UnifySameDomain) of
//   - every planar face perpendicular to Z looking up (its own loops: exact), and
//   - the slices from above and from below at the critical heights of every face that is neither such a plane nor
//     vertical (a plane parallel to Z, a cylinder or cone... see verticalFace): the ends of its height range, plus a
//     sphere's centre, the centre of a torus about Z and the axis height of a cylinder or cone lying across Z, where
//     their outline is widest. Between those heights a plane, a cone about Z, a sphere or a torus about Z only
//     shrinks or grows, so the slices at the ends cover it. Any other face (a B-spline, a surface of revolution or a
//     tilted cylinder) is also sliced at kSilhouetteSamples heights across its range; there the silhouette is exact
//     only at those heights (kernel.h says so).
// Faces parallel to Z (vertical) have no shadow area and are skipped.
#include "boolean_internal.h"
#include "slice_internal.h"

#include <BOPAlgo_Alerts.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_BuilderAlgo.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace ray::kernel {

namespace {

using detail::guarded;
using detail::makeError;

constexpr double kHeightSnap = 1e-6;    // mm: a z this close to a horizontal face is that face's height
constexpr double kRetryNudge = 1e-6;    // mm: the second try of a failed slice boolean
constexpr double kOnPlane = 1e-6;       // mm: a result face this close to the cut plane lies on it
constexpr double kHorizontalSin = 1e-9; // a normal is along Z when its XY part is at most this
constexpr double kVerticalCos = 1e-9;   // a plane is parallel to Z when |normal.z| is at most this
constexpr double kHeightMerge = 1e-6;   // mm: critical heights closer than this are one height
constexpr int kSilhouetteSamples = 16;  // heights across a face whose outline is not monotone (silhouette)

enum class From { Above, Below };

// The outward normal of a face at the middle of its parameter range (kernel.h faceSample's rule) and the point there.
bool faceNormal(const TopoDS_Face& face, gp_Pnt& point, gp_Vec& normal)
{
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const BRepAdaptor_Surface surface(face, true);
    gp_Vec du, dv;
    surface.D1(0.5 * (u0 + u1), 0.5 * (v0 + v1), point, du, dv);
    normal = du.Crossed(dv);
    if (normal.Magnitude() <= 1e-300)
        return false;
    normal.Normalize();
    if (face.Orientation() == TopAbs_REVERSED)
        normal.Reverse();
    return true;
}

// A planar face perpendicular to Z: its height and whether it looks up.
struct Horizontal {
    double z = 0;
    bool up = true;
};
std::optional<Horizontal> horizontalFace(const TopoDS_Face& face)
{
    const BRepAdaptor_Surface surface(face, true);
    if (surface.GetType() != GeomAbs_Plane)
        return std::nullopt;
    gp_Pnt p;
    gp_Vec n;
    if (!faceNormal(face, p, n) || std::hypot(n.X(), n.Y()) > kHorizontalSin)
        return std::nullopt;
    return Horizontal{surface.Plane().Location().Z(), n.Z() > 0.0};
}

Box3 tightBox(const TopoDS_Shape& shape)
{
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, false, false);
    if (box.IsVoid())
        return Box3{};
    double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    box.Get(x0, y0, z0, x1, y1, z1);
    return Box3{Vec3{x0, y0, z0}, Vec3{x1, y1, z1}};
}

// The z of the horizontal face nearest to z within kHeightSnap, else z.
double snapHeight(const TopoDS_Shape& moved, double z)
{
    double best = z;
    double bestDistance = 0.0;
    bool found = false;
    for (TopExp_Explorer ex(moved, TopAbs_FACE); ex.More(); ex.Next()) {
        const auto h = horizontalFace(TopoDS::Face(ex.Current()));
        if (!h)
            continue;
        const double d = std::abs(h->z - z);
        if (d > kHeightSnap)
            continue;
        if (!found || d < bestDistance || (d == bestDistance && h->z < best)) {
            best = h->z;
            bestDistance = d;
            found = true;
        }
    }
    return best;
}

// The faces of the material just above (or below) z, lying on the plane z. `moved` is a private copy in the frame.
// Empty when z is outside the shape's height range.
Result<std::vector<TopoDS_Face>> sectionFaces(const TopoDS_Shape& moved, const Box3& bounds, double z, From from,
                                              JobContext& ctx, std::string_view where)
{
    std::vector<TopoDS_Face> faces;
    if (from == From::Above ? z >= bounds.max.z : z <= bounds.min.z)
        return faces;
    if (from == From::Above ? z < bounds.min.z - kHeightSnap : z > bounds.max.z + kHeightSnap)
        return faces;
    const double size =
        std::max({bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y, bounds.max.z - bounds.min.z, 1.0});
    const double margin = 1.0 + 0.01 * size;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const double zc = attempt == 0 ? z : (from == From::Above ? z + kRetryNudge : z - kRetryNudge);
        const gp_Pnt lo(bounds.min.x - margin, bounds.min.y - margin, from == From::Above ? zc : bounds.min.z - margin);
        const gp_Pnt hi(bounds.max.x + margin, bounds.max.y + margin, from == From::Above ? bounds.max.z + margin : zc);
        if (!(hi.Z() > lo.Z()))
            return faces;
        const TopoDS_Shape box = BRepPrimAPI_MakeBox(lo, hi).Shape();
        BRepAlgoAPI_Common common;
        NCollection_List<TopoDS_Shape> arguments, tools;
        arguments.Append(moved);
        tools.Append(box);
        common.SetArguments(arguments);
        common.SetTools(tools);
        common.SetRunParallel(false);
        common.SetNonDestructive(true);
        occ::handle<detail::JobIndicator> indicator = new detail::JobIndicator(ctx, 0.0, 0.0, "Slicing");
        common.Build(indicator->Start());
        if (ctx.cancel.cancelled() || common.HasError(STANDARD_TYPE(BOPAlgo_AlertUserBreak)))
            return tl::unexpected(detail::cancelledError(where));
        if (!common.IsDone() || common.HasErrors())
            continue;
        for (TopExp_Explorer ex(common.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
            const TopoDS_Face& face = TopoDS::Face(ex.Current());
            const auto h = horizontalFace(face);
            if (!h || std::abs(h->z - zc) > kOnPlane || h->up != (from == From::Below))
                continue;
            faces.push_back(face);
        }
        return faces;
    }
    return tl::unexpected(
        makeError(ErrorCode::KernelFailure, "The geometry kernel could not cut the part at this height.", where));
}

// Heights at which a face's outline is extreme (silhouette rule above), in the frame; empty for faces without a
// shadow (vertical) and for horizontal planes (taken whole).
std::vector<double> criticalHeights(const TopoDS_Face& face)
{
    std::vector<double> heights;
    const BRepAdaptor_Surface surface(face, true);
    const Box3 box = tightBox(face);
    const auto axisAlong = [](const gp_Dir& d) {
        return std::hypot(d.X(), d.Y()) <= 1e-9;
    };
    const auto axisAcross = [](const gp_Dir& d) {
        return std::abs(d.Z()) <= 1e-9;
    };
    bool sample = false;
    switch (surface.GetType()) {
    case GeomAbs_Plane: {
        gp_Pnt p;
        gp_Vec n;
        if (!faceNormal(face, p, n) || std::abs(n.Z()) <= kVerticalCos)
            return heights; // vertical: no shadow
        if (std::hypot(n.X(), n.Y()) <= kHorizontalSin)
            return heights; // horizontal: taken whole
        break;
    }
    case GeomAbs_Cylinder: {
        const gp_Cylinder c = surface.Cylinder();
        if (axisAlong(c.Axis().Direction()))
            return heights; // vertical
        if (axisAcross(c.Axis().Direction()))
            heights.push_back(c.Location().Z());
        else
            sample = true;
        break;
    }
    case GeomAbs_Cone: {
        const gp_Cone c = surface.Cone();
        if (axisAcross(c.Axis().Direction()))
            heights.push_back(c.Location().Z());
        else if (!axisAlong(c.Axis().Direction()))
            sample = true;
        break;
    }
    case GeomAbs_Sphere:
        heights.push_back(surface.Sphere().Location().Z());
        break;
    case GeomAbs_Torus: {
        const gp_Torus t = surface.Torus();
        if (axisAlong(t.Axis().Direction()))
            heights.push_back(t.Location().Z());
        else
            sample = true;
        break;
    }
    default:
        sample = true;
        break;
    }
    heights.push_back(box.min.z);
    heights.push_back(box.max.z);
    if (sample)
        for (int i = 1; i < kSilhouetteSamples; ++i)
            heights.push_back(box.min.z + (box.max.z - box.min.z) * i / kSilhouetteSamples);
    std::erase_if(heights, [&](double h) { return h < box.min.z - kHeightMerge || h > box.max.z + kHeightMerge; });
    return heights;
}

TopoDS_Face atZeroLookingUp(const TopoDS_Face& face, double z, bool up)
{
    gp_Trsf down;
    down.SetTranslation(gp_Vec(0.0, 0.0, -z));
    TopoDS_Face moved = TopoDS::Face(face.Moved(TopLoc_Location(down)));
    return up ? moved : TopoDS::Face(moved.Reversed());
}

Result<std::vector<ProfileLoop>> sliceLoops(const Shape& shape, const Xform& frame, double z, JobContext& ctx,
                                            std::string_view where)
{
    if (auto ok = detail::requireShape(shape, where); !ok)
        return tl::unexpected(ok.error());
    if (!std::isfinite(z))
        return tl::unexpected(makeError(ErrorCode::InvalidArgument, "The slice height must be finite.", where));
    auto trsf = detail::rigidTrsf(frame, where);
    if (!trsf)
        return tl::unexpected(trsf.error());
    if (auto st = ctx.cancel.check(); !st)
        return tl::unexpected(st.error());
    return guarded(where, [&]() -> Result<std::vector<ProfileLoop>> {
        const TopoDS_Shape moved = detail::topologyCopy(detail::occt(shape)).Moved(TopLoc_Location(*trsf));
        const Box3 bounds = tightBox(moved);
        const double zs = snapHeight(moved, z);
        auto faces = sectionFaces(moved, bounds, zs, From::Above, ctx, where);
        if (!faces)
            return tl::unexpected(faces.error());
        return detail::orderRegions(detail::regionsOfFaces(*faces));
    });
}

} // namespace

Result<std::vector<ProfileLoop>> sliceAt(const Shape& shape, const Xform& frame, double z, JobContext& ctx)
{
    return sliceLoops(shape, frame, z, ctx, "kernel::sliceAt");
}

Result<std::vector<ProfileLoop>> silhouette(const Shape& shape, const Xform& frame, JobContext& ctx)
{
    constexpr std::string_view where = "kernel::silhouette";
    if (auto ok = detail::requireShape(shape, where); !ok)
        return tl::unexpected(ok.error());
    auto trsf = detail::rigidTrsf(frame, where);
    if (!trsf)
        return tl::unexpected(trsf.error());
    if (auto st = ctx.cancel.check(); !st)
        return tl::unexpected(st.error());
    return guarded(where, [&]() -> Result<std::vector<ProfileLoop>> {
        const TopoDS_Shape moved = detail::topologyCopy(detail::occt(shape)).Moved(TopLoc_Location(*trsf));
        const Box3 bounds = tightBox(moved);
        std::vector<TopoDS_Face> flat; // looking up, at z = 0
        std::vector<double> heights;
        int polled = 0;
        for (TopExp_Explorer ex(moved, TopAbs_FACE); ex.More(); ex.Next()) {
            if (++polled % 64 == 0)
                if (auto st = ctx.cancel.check(); !st)
                    return tl::unexpected(st.error());
            const TopoDS_Face& face = TopoDS::Face(ex.Current());
            if (const auto h = horizontalFace(face)) {
                if (h->up)
                    flat.push_back(atZeroLookingUp(face, h->z, true));
                continue;
            }
            for (const double z : criticalHeights(face))
                heights.push_back(z);
        }
        std::sort(heights.begin(), heights.end());
        std::vector<double> unique;
        for (const double h : heights)
            if (unique.empty() || h - unique.back() > kHeightMerge)
                unique.push_back(h);
        for (std::size_t i = 0; i < unique.size(); ++i) {
            ctx.progress.set(0.8 * static_cast<double>(i) / static_cast<double>(unique.size()), "Silhouette");
            const double z = snapHeight(moved, unique[i]);
            for (const From from : {From::Above, From::Below}) {
                auto faces = sectionFaces(moved, bounds, z, from, ctx, where);
                if (!faces)
                    return tl::unexpected(faces.error());
                for (const TopoDS_Face& face : *faces)
                    flat.push_back(atZeroLookingUp(face, z, from == From::Below));
            }
        }
        if (flat.empty())
            return std::vector<ProfileLoop>{};
        if (auto st = ctx.cancel.check(); !st)
            return tl::unexpected(st.error());
        TopoDS_Shape united;
        if (flat.size() == 1) {
            united = flat.front();
        } else {
            BRepAlgoAPI_BuilderAlgo fuse;
            NCollection_List<TopoDS_Shape> arguments;
            for (const TopoDS_Face& face : flat)
                arguments.Append(face);
            fuse.SetArguments(arguments);
            fuse.SetRunParallel(false);
            fuse.SetNonDestructive(true);
            occ::handle<detail::JobIndicator> indicator = new detail::JobIndicator(ctx, 0.8, 0.95, "Silhouette");
            fuse.Build(indicator->Start());
            if (ctx.cancel.cancelled() || fuse.HasError(STANDARD_TYPE(BOPAlgo_AlertUserBreak)))
                return tl::unexpected(detail::cancelledError(where));
            if (!fuse.IsDone() || fuse.HasErrors())
                return tl::unexpected(makeError(ErrorCode::KernelFailure,
                                                "The geometry kernel could not unite the part's outline.", where));
            ShapeUpgrade_UnifySameDomain unify(fuse.Shape(), true, true, false);
            unify.Build();
            united = unify.Shape();
        }
        std::vector<TopoDS_Face> faces;
        for (TopExp_Explorer ex(united, TopAbs_FACE); ex.More(); ex.Next())
            faces.push_back(TopoDS::Face(ex.Current()));
        return detail::orderRegions(detail::regionsOfFaces(faces));
    });
}

Result<FaceLoops> faceLoops(const Shape& shape, SubId face, const Xform& frame)
{
    constexpr std::string_view where = "kernel::faceLoops";
    if (face.kind != SubKind::Face)
        return tl::unexpected(makeError(ErrorCode::InvalidArgument, "The sub-shape is not a face.", where));
    auto trsf = detail::rigidTrsf(frame, where);
    if (!trsf)
        return tl::unexpected(trsf.error());
    return guarded(where, [&]() -> Result<FaceLoops> {
        auto sub = detail::resolve(shape, face, where);
        if (!sub)
            return tl::unexpected(sub.error());
        const TopoDS_Face moved = TopoDS::Face(sub->Moved(TopLoc_Location(*trsf)));
        const auto h = horizontalFace(moved);
        if (!h)
            return tl::unexpected(makeError(ErrorCode::InvalidArgument,
                                            "The face is not a plane perpendicular to the setup's Z axis.", where));
        FaceLoops out;
        out.z = h->z;
        out.facesUp = h->up;
        out.loops = detail::orderRegions(detail::regionsOfFaces({moved}));
        return out;
    });
}

Result<EdgeCurves> edgeCurves(const Shape& shape, SubId edge, const Xform& frame)
{
    constexpr std::string_view where = "kernel::edgeCurves";
    if (edge.kind != SubKind::Edge)
        return tl::unexpected(makeError(ErrorCode::InvalidArgument, "The sub-shape is not an edge.", where));
    auto trsf = detail::rigidTrsf(frame, where);
    if (!trsf)
        return tl::unexpected(trsf.error());
    return guarded(where, [&]() -> Result<EdgeCurves> {
        auto sub = detail::resolve(shape, edge, where);
        if (!sub)
            return tl::unexpected(sub.error());
        const TopoDS_Edge moved = TopoDS::Edge(sub->Moved(TopLoc_Location(*trsf)).Oriented(TopAbs_FORWARD));
        EdgeCurves out;
        detail::appendProjectedEdge(moved, false, out.curves);
        const Box3 box = tightBox(moved);
        out.zMin = box.min.z;
        out.zMax = box.max.z;
        return out;
    });
}

} // namespace ray::kernel
