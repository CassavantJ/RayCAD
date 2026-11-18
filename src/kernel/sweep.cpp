// extrude() and revolve() (task T-094).
//
// Both build the profile face (sweep_profile.cpp, convention in sweep_internal.h) and sweep it with
// BRepPrimAPI_MakePrism / BRepPrimAPI_MakeRevol: Generated(edge) is the side face of each profile curve and
// FirstShape / LastShape of the face are the caps. Prism history is complete (spike S4); revolve history has two
// gaps, worked around in finishOrigins ("OCCT 8.0.1 GAPS"). Each result face is reported in
// SweepResult::origins with the tag of its profile curve (Side) or of the outer loop (StartCap at the profile
// plane, EndCap at the far end), in face SubId order. A full revolution has no caps. A profile curve that lies on
// the revolve axis sweeps no face and has no origin.
//
// A sweep has no input Shape, so OpResult::history is empty: `origins` is the history.
//
// DRAFT (extrude only). The prism is drafted with BRepOffsetAPI_DraftAngle, neutral plane = the profile plane,
// pull direction = the extrusion direction. A positive angle takes material away as the extrusion goes on: outer
// walls lean in and hole walls lean out, so the end cap is smaller than the start cap (a boss that leaves a mould).
// OCCT drafts planar, cylindrical and conical faces only, so a drafted profile may hold lines, arcs and circles but
// no B-spline. Each prism face maps to one drafted face (ModifiedShape), so origins carry over.
//
// Validation (PLAN.md section 7 rule 6): the result is one valid closed solid of positive volume. A zero distance
// or angle is rejected up front, so a sweep is never a no-op.
#include "boolean_internal.h"
#include "sweep_internal.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepSweep_Revol.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ray::kernel {

namespace {

using detail::kernelFailure;

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kHalfPi = 1.5707963267948966192313216916398;

Error invalidArgument(std::string message, std::string_view where)
{
    return detail::makeError(ErrorCode::InvalidArgument, std::move(message), where);
}

// A face of the sweep before the final shape exists, with where it came from.
struct PendingOrigin {
    TopoDS_Shape face;
    std::uint64_t curveTag;
    std::uint64_t loopTag;
    SweepFaceOrigin::Role role;
};

// The tagged profile edge that is the same edge as `edge`, if any.
const detail::ProfileEdge* findEdge(const detail::BuiltProfile& profile, const TopoDS_Shape& edge)
{
    for (const detail::ProfileEdge& e : profile.edges)
        if (e.edge.IsSame(edge))
            return &e;
    return nullptr;
}

// Side faces: Generated(edge) of every edge of the profile face, or sweptFace(edge) where OCCT reports none; caps:
// First/Last shape of the face (if present).
template <class Maker>
std::vector<PendingOrigin> collectOrigins(Maker& maker, const detail::BuiltProfile& profile, bool caps,
                                          const std::function<TopoDS_Shape(const TopoDS_Shape&)>& sweptFace = {})
{
    std::vector<PendingOrigin> out;
    for (TopExp_Explorer it(profile.face, TopAbs_EDGE); it.More(); it.Next()) {
        const detail::ProfileEdge* source = findEdge(profile, it.Current());
        if (source == nullptr)
            continue;
        bool any = false;
        for (const TopoDS_Shape& made : maker.Generated(it.Current()))
            if (made.ShapeType() == TopAbs_FACE) {
                out.push_back({made, source->curveTag, source->loopTag, SweepFaceOrigin::Role::Side});
                any = true;
            }
        if (any || !sweptFace)
            continue;
        const TopoDS_Shape swept = sweptFace(it.Current());
        if (!swept.IsNull() && swept.ShapeType() == TopAbs_FACE)
            out.push_back({swept, source->curveTag, source->loopTag, SweepFaceOrigin::Role::Side});
    }
    if (caps) {
        out.push_back({maker.FirstShape(profile.face), 0, profile.outerTag, SweepFaceOrigin::Role::StartCap});
        out.push_back({maker.LastShape(profile.face), 0, profile.outerTag, SweepFaceOrigin::Role::EndCap});
    }
    return out;
}

// The one face of `result` not yet taken that lies on the same surface (same Geom_Surface object, same location)
// as `face`; empty if there is none or more than one. Identity, not address order, decides, so it is repeatable.
std::optional<SubId> sameSurface(const Shape& result, const TopoDS_Shape& face, const std::vector<char>& taken)
{
    TopLoc_Location location;
    const occ::handle<Geom_Surface>& surface = BRep_Tool::Surface(TopoDS::Face(face), location);
    if (surface.IsNull())
        return std::nullopt;
    const detail::ShapeIndex& faces = result.impl->subs[detail::kindSlot(SubKind::Face)];
    std::optional<SubId> found;
    for (int i = 1; i <= faces.Extent(); ++i) {
        if (taken[static_cast<std::size_t>(i - 1)] != 0)
            continue;
        TopLoc_Location other;
        if (BRep_Tool::Surface(TopoDS::Face(faces.FindKey(i)), other) != surface || !other.IsEqual(location))
            continue;
        if (found)
            return std::nullopt;
        found = SubId{SubKind::Face, static_cast<std::uint32_t>(i - 1)};
    }
    return found;
}

// Turns pending origins into SweepFaceOrigin on the result, sorted by face, and warns about faces without one.
//
// OCCT 8.0.1 GAPS, both in BRepPrimAPI_MakeRevol:
//  1. In a FULL revolution Generated(edge) is empty for profile edges square to the axis (their faces are planar
//     rings and discs): BRepSweep marks them unused. collectOrigins asks the sweep itself (BRepSweep_Revol::Shape)
//     for those.
//  2. A face that shares a degenerated edge at the axis with another face is replaced by a copy, and Generated()
//     may still name the face before the copy, which is not in the result. The copy keeps the surface, so such a
//     face is matched here by its surface.
// A face matched by neither stays an orphan and `warnings` names it.
void finishOrigins(SweepResult& sweep, const std::vector<PendingOrigin>& pending)
{
    const Shape& result = sweep.op.shape;
    std::vector<char> taken(result.count(SubKind::Face), 0);
    std::vector<const PendingOrigin*> lost;
    for (const PendingOrigin& p : pending) {
        if (p.face.IsNull() || p.face.ShapeType() != TopAbs_FACE)
            continue;
        const auto id = detail::subIdOf(result, p.face);
        if (!id) {
            lost.push_back(&p);
            continue;
        }
        if (taken[id->index] != 0)
            continue;
        taken[id->index] = 1;
        sweep.origins.push_back({*id, p.curveTag, p.loopTag, p.role});
    }
    for (const PendingOrigin* p : lost) {
        const auto id = sameSurface(result, p->face, taken);
        if (!id)
            continue;
        taken[id->index] = 1;
        sweep.origins.push_back({*id, p->curveTag, p->loopTag, p->role});
    }
    std::sort(sweep.origins.begin(), sweep.origins.end(),
              [](const SweepFaceOrigin& a, const SweepFaceOrigin& b) { return a.face < b.face; });
    std::vector<SubId> orphans;
    for (std::uint32_t i = 0; i < taken.size(); ++i)
        if (taken[i] == 0)
            orphans.push_back(SubId{SubKind::Face, i});
    if (!orphans.empty())
        sweep.op.warnings.push_back(detail::orphanWarning(orphans));
}

Result<TopoDS_Shape> applyDraft(const TopoDS_Shape& prism, std::vector<PendingOrigin>& pending, const gp_Dir& pull,
                                double angle, const gp_Pln& neutral, std::string_view where)
{
    for (const PendingOrigin& p : pending) {
        if (p.role != SweepFaceOrigin::Role::Side || p.face.IsNull())
            continue;
        const GeomAbs_SurfaceType type = BRepAdaptor_Surface(TopoDS::Face(p.face)).GetType();
        if (type != GeomAbs_Plane && type != GeomAbs_Cylinder)
            return tl::unexpected(invalidArgument(
                "A drafted extrusion can only have lines, arcs and circles in its profile, not B-splines.", where));
    }
    // A face already drafted through tangent propagation from an earlier one is accepted again as a no-op.
    BRepOffsetAPI_DraftAngle draft(prism);
    for (const PendingOrigin& p : pending) {
        if (p.role != SweepFaceOrigin::Role::Side || p.face.IsNull())
            continue;
        draft.Add(TopoDS::Face(p.face), pull, angle, neutral, true);
        if (!draft.AddDone())
            return tl::unexpected(kernelFailure(
                "The draft angle cannot be applied to this profile: the drafted walls would cut into each other. "
                "Use a smaller angle or a shorter extrusion.",
                where));
    }
    draft.Build();
    if (!draft.IsDone() || draft.Shape().IsNull())
        return tl::unexpected(kernelFailure(
            "The draft angle is too large for this extrusion: the walls would meet or the end face would vanish. "
            "Use a smaller angle or a shorter extrusion.",
            where));
    // OCCT reports a face that got a new surface as Generated, not Modified, and an untouched face as neither;
    // ModifiedShape() gives the result face in every case.
    for (PendingOrigin& p : pending)
        if (!p.face.IsNull())
            p.face = draft.ModifiedShape(p.face);
    return draft.Shape();
}

Result<SweepResult> finish(const TopoDS_Shape& built, const std::vector<PendingOrigin>& pending, const char* what,
                           JobContext& ctx, std::string_view where)
{
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(detail::cancelledError(where));
    const TopoDS_Shape solid = detail::normaliseSolids(built);
    auto checked = detail::validateSolids(solid, what, where);
    if (!checked)
        return tl::unexpected(checked.error());
    if (detail::solidsOf(solid).size() != 1)
        return tl::unexpected(kernelFailure(std::string(what) + " produced more than one solid.", where));
    SweepResult sweep;
    sweep.op.shape = detail::wrap(solid);
    finishOrigins(sweep, pending);
    ctx.progress.set(1.0, "Done");
    return sweep;
}

} // namespace

Result<SweepResult> extrude(const Profile& profile, double distance, double draftAngle, JobContext& ctx)
{
    constexpr std::string_view where = "kernel::extrude";
    if (!std::isfinite(distance) || !(std::abs(distance) > Precision::Confusion()))
        return tl::unexpected(
            invalidArgument("The extrusion distance must be a finite length other than zero.", where));
    if (!std::isfinite(draftAngle) || !(std::abs(draftAngle) < kHalfPi))
        return tl::unexpected(invalidArgument("The draft angle must be finite and less than 90 degrees.", where));
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(detail::cancelledError(where));
    return detail::guarded(where, [&]() -> Result<SweepResult> {
        auto built = detail::buildProfile(profile, where);
        if (!built)
            return tl::unexpected(built.error());
        const gp_Dir normal = built->frame.Direction();
        const gp_Vec along = gp_Vec(normal) * distance;
        BRepPrimAPI_MakePrism prism(built->face, along, false, true);
        if (!prism.IsDone() || prism.Shape().IsNull())
            return tl::unexpected(kernelFailure("The profile could not be extruded.", where));
        std::vector<PendingOrigin> pending = collectOrigins(prism, *built, true);
        TopoDS_Shape solid = prism.Shape();
        ctx.progress.set(0.5, "Extruding");
        if (draftAngle != 0.0) {
            if (auto ok = ctx.cancel.check(); !ok)
                return tl::unexpected(detail::cancelledError(where));
            const gp_Pln neutral(built->frame);
            auto drafted = applyDraft(solid, pending, gp_Dir(along), draftAngle, neutral, where);
            if (!drafted)
                return tl::unexpected(drafted.error());
            solid = *drafted;
        }
        return finish(solid, pending, "The extrusion", ctx, where);
    });
}

Result<SweepResult> revolve(const Profile& profile, Vec3 axisOrigin, Vec3 axisDir, double angle, JobContext& ctx)
{
    constexpr std::string_view where = "kernel::revolve";
    const auto finite3 = [](const Vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finite3(axisOrigin) || !finite3(axisDir))
        return tl::unexpected(invalidArgument("The revolve axis must be finite.", where));
    const double axisLength = std::sqrt(axisDir.x * axisDir.x + axisDir.y * axisDir.y + axisDir.z * axisDir.z);
    if (!(axisLength > Precision::Confusion()))
        return tl::unexpected(invalidArgument("The revolve axis direction must not be a zero vector.", where));
    if (!std::isfinite(angle) || !(std::abs(angle) > Precision::Angular()) || std::abs(angle) > kTwoPi + 1e-9)
        return tl::unexpected(
            invalidArgument("The revolve angle must be other than zero and at most one full turn.", where));
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(detail::cancelledError(where));
    return detail::guarded(where, [&]() -> Result<SweepResult> {
        auto built = detail::buildProfile(profile, where);
        if (!built)
            return tl::unexpected(built.error());
        const gp_Ax3& frame = built->frame;
        const gp_Dir dir(axisDir.x, axisDir.y, axisDir.z);
        const gp_Pnt origin(axisOrigin.x, axisOrigin.y, axisOrigin.z);
        const double offPlane = std::abs(gp_Vec(frame.Location(), origin).Dot(gp_Vec(frame.Direction())));
        if (std::abs(dir.Dot(frame.Direction())) > 1e-9 || offPlane > 1e-6)
            return tl::unexpected(invalidArgument("The revolve axis must lie in the profile's plane.", where));
        // Signed distance of every profile sample from the axis, in the plane. All must be on one side.
        const gp_XYZ rel = origin.XYZ() - frame.Location().XYZ();
        const double ox = rel.Dot(frame.XDirection().XYZ());
        const double oy = rel.Dot(frame.YDirection().XYZ());
        const double dx = dir.XYZ().Dot(frame.XDirection().XYZ());
        const double dy = dir.XYZ().Dot(frame.YDirection().XYZ());
        constexpr double kOnAxis = 1e-6;
        bool left = false;
        bool right = false;
        for (const gp_Pnt2d& p : built->samples) {
            const double side = dx * (p.Y() - oy) - dy * (p.X() - ox);
            left = left || side > kOnAxis;
            right = right || side < -kOnAxis;
        }
        if (left && right)
            return tl::unexpected(invalidArgument(
                "The profile crosses the revolve axis. A revolved profile must lie on one side of the axis (it may "
                "touch it).",
                where));
        const bool full = std::abs(angle) >= kTwoPi - 1e-9;
        const gp_Ax1 axis(origin, angle < 0.0 ? dir.Reversed() : dir);
        std::optional<BRepPrimAPI_MakeRevol> revol;
        if (full)
            revol.emplace(built->face, axis, false);
        else
            revol.emplace(built->face, axis, std::abs(angle), false);
        if (!revol->IsDone() || revol->Shape().IsNull())
            return tl::unexpected(kernelFailure(
                "The profile could not be revolved; its surface of revolution would cross itself.", where));
        // See "OCCT 8.0.1 GAPS" at finishOrigins. Revol() is const only by its return type: the sweep belongs to
        // `revol`, which is not const, and Shape(edge) of a built sweep only looks the face up.
        auto& sweep = const_cast<BRepSweep_Revol&>(revol->Revol());
        const std::vector<PendingOrigin> pending =
            collectOrigins(*revol, *built, !full, [&sweep](const TopoDS_Shape& edge) { return sweep.Shape(edge); });
        ctx.progress.set(0.5, "Revolving");
        return finish(revol->Shape(), pending, "The revolution", ctx, where);
    });
}

} // namespace ray::kernel
