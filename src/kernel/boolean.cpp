// boolean() and transformed() (task T-094).
//
// BOOLEAN. One BRepAlgoAPI_BooleanOperation (cut, fuse or common) with OCCT parallelism off (determinism,
// PLAN.md section 7 rule 4) and non-destructive mode on (OCCT must not touch the tolerances of the caller's
// immutable shapes), then SimplifyResult() so coplanar and co-cylindrical pieces are unified and the history of
// that unification is folded into the maker's history (spike S4). OCCT's own status is not trusted (section 7
// rule 6), so the result is checked:
//   1. not empty; every face inside a solid; BRepCheck finds no fault; every shell closed; every volume positive;
//   2. not a no-op: the volume changed (beyond 1e-12 relative), and some face of the result comes from the tool
//      (removing or adding material always leaves tool faces on the boundary);
//   3. the volume moved the expected way: a cut shrinks the target, a union grows it by at most the tool, an
//      intersection is no larger than either input;
//   4. a union is one solid. A cut or intersection may leave several solids; they are returned as a compound and
//      `warnings` says so.
// If the unified result fails check 1 but the plain boolean passes, the plain result is returned with a warning
// (ShapeUpgrade_UnifySameDomain turned 10 of 496 valid results into invalid solids in spike S9).
// Cancel: OCCT polls JobIndicator::UserBreak() during the boolean; a cancelled call returns Error{Cancelled}.
//
// TRANSFORMED. A rigid move only (core.h Xform). The geometry is copied and moved (BRepBuilderAPI_Transform with
// copied geometry), so the result shares no TShape with the input and carries no location; every sub-shape is
// reported Modified to its moved copy. The identity is a no-op and fails (section 7 rule 6).
#include "boolean_internal.h"

#include <BOPAlgo_Alerts.hxx>
#include <BOPAlgo_Operation.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <NCollection_List.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace ray::kernel {

namespace {

using detail::kernelFailure;

// Volume changes at or below this fraction of the larger input count as no change at all.
constexpr double kNoOpRelative = 1e-12;
// A volume that is "unchanged" by topology (no tool face in the result) may still differ by integration noise.
constexpr double kNoiseRelative = 1e-6;
// Slack on the upper and lower volume bounds of check 3.
constexpr double kBoundRelative = 1e-7;

struct OpWords {
    const char* subject; // "The cut"
    const char* stage;   // progress stage
};

OpWords wordsFor(BoolOp op)
{
    switch (op) {
    case BoolOp::Union:
        return {"The union", "Joining bodies"};
    case BoolOp::Cut:
        return {"The cut", "Cutting"};
    case BoolOp::Intersect:
        return {"The intersection", "Intersecting bodies"};
    }
    return {"The boolean", "Boolean"};
}

BOPAlgo_Operation occtOperation(BoolOp op)
{
    switch (op) {
    case BoolOp::Union:
        return BOPAlgo_FUSE;
    case BoolOp::Cut:
        return BOPAlgo_CUT;
    case BoolOp::Intersect:
        return BOPAlgo_COMMON;
    }
    return BOPAlgo_UNKNOWN;
}

std::string invalidMessage(BoolOp op)
{
    return std::string(wordsFor(op).subject) +
           " would give an invalid solid. This usually happens where the tool exactly touches a face, edge or "
           "tangent surface of the body: move or resize the tool slightly so it clearly overlaps or clearly misses.";
}

std::string emptyMessage(BoolOp op)
{
    switch (op) {
    case BoolOp::Cut:
        return "The cut removes the whole body: the tool encloses all of it.";
    case BoolOp::Intersect:
        return "The intersection is empty: the two bodies have no volume in common.";
    case BoolOp::Union:
        break;
    }
    return "The union produced no solid.";
}

std::string noOpMessage(BoolOp op)
{
    switch (op) {
    case BoolOp::Cut:
        return "The cut does not remove any material: the tool does not overlap the body (it may only touch it). "
               "Move or extend the tool into the body.";
    case BoolOp::Union:
        return "The union adds no material: the new material lies inside the body or only touches it. Move or "
               "extend it out of the body.";
    case BoolOp::Intersect:
        return "The intersection changes nothing: the tool encloses the whole body.";
    }
    return "The operation changes nothing.";
}

struct Attempt {
    Result<OpResult> result;
    bool invalid = false; // failed check 1 (retrying without unification may help)
};

Attempt attempt(BoolOp op, const Shape& target, const Shape& tool, bool unify, double targetVolume, double toolVolume,
                JobContext& ctx, std::string_view where)
{
    const OpWords words = wordsFor(op);
    BRepAlgoAPI_BooleanOperation maker;
    NCollection_List<TopoDS_Shape> arguments;
    arguments.Append(detail::occt(target));
    NCollection_List<TopoDS_Shape> tools;
    tools.Append(detail::occt(tool));
    maker.SetArguments(arguments);
    maker.SetTools(tools);
    maker.SetOperation(occtOperation(op));
    maker.SetRunParallel(false);
    maker.SetNonDestructive(true);
    occ::handle<detail::JobIndicator> indicator = new detail::JobIndicator(ctx, 0.0, 0.8, words.stage);
    maker.Build(indicator->Start());
    if (ctx.cancel.cancelled() || maker.HasError(STANDARD_TYPE(BOPAlgo_AlertUserBreak)))
        return {tl::unexpected(detail::cancelledError(where))};
    if (!maker.IsDone() || maker.HasErrors()) {
        std::ostringstream dump;
        maker.DumpErrors(dump);
        Error error = kernelFailure(std::string(words.subject) +
                                        " could not be computed: the geometry kernel could not intersect the body "
                                        "with the tool. This usually happens where faces only touch or are tangent; "
                                        "move or resize the tool slightly.",
                                    where);
        if (!dump.str().empty())
            error.context.insert(error.context.begin(), "OCCT: " + dump.str());
        return {tl::unexpected(std::move(error))};
    }
    if (unify)
        maker.SimplifyResult();
    ctx.progress.set(0.85, "Checking the result");

    const TopoDS_Shape solids = detail::normaliseSolids(maker.Shape());
    if (solids.IsNull())
        return {tl::unexpected(kernelFailure(emptyMessage(op), where))};
    auto checked = detail::validateSolids(solids, words.subject, where);
    if (!checked) {
        Error error = kernelFailure(invalidMessage(op), where);
        error.context.insert(error.context.begin(), checked.error().message);
        return {tl::unexpected(std::move(error)), true};
    }
    const double volume = *checked;

    OpResult out;
    out.shape = detail::wrap(solids);
    const std::array<const Shape*, 2> inputs{&target, &tool};
    out.history = detail::buildHistory(maker, inputs, out.shape);

    bool toolContributes = false;
    for (const HistoryEntry& entry : out.history.entries)
        if (entry.inputIndex == 1 && entry.source.kind == SubKind::Face &&
            entry.relation == HistoryEntry::Relation::Modified && !entry.results.empty())
            toolContributes = true;

    const double scale = std::max({targetVolume, toolVolume, 1.0});
    const double change = std::abs(volume - targetVolume);
    if (change <= kNoOpRelative * scale || (!toolContributes && change <= kNoiseRelative * scale))
        return {tl::unexpected(kernelFailure(noOpMessage(op), where))};
    const double slack = kBoundRelative * scale;
    bool expected = true;
    switch (op) {
    case BoolOp::Cut:
        expected = volume < targetVolume;
        break;
    case BoolOp::Union:
        expected = volume > targetVolume && volume <= targetVolume + toolVolume + slack;
        break;
    case BoolOp::Intersect:
        expected = volume <= std::min(targetVolume, toolVolume) + slack;
        break;
    }
    if (!expected) {
        Error error = kernelFailure(invalidMessage(op), where);
        error.context.insert(error.context.begin(), "volume " + std::to_string(targetVolume) + " -> " +
                                                        std::to_string(volume) + " is not the expected change");
        return {tl::unexpected(std::move(error)), true};
    }

    const std::uint32_t solidCount = out.shape.count(SubKind::Solid);
    if (solidCount > 1) {
        if (op == BoolOp::Union)
            return {tl::unexpected(kernelFailure(
                "The added material does not overlap the body, or touches it only along an edge or at a point, so "
                "the result would be separate solids. Move it so it overlaps the body or shares a face with it.",
                where))};
        out.warnings.push_back(std::string(words.subject) + " leaves " + std::to_string(solidCount) +
                               " separate solids.");
    }
    const std::vector<SubId> orphans = detail::orphanFaces(out.history, out.shape);
    if (!orphans.empty())
        out.warnings.push_back(detail::orphanWarning(orphans));
    return {std::move(out)};
}

} // namespace

Result<OpResult> boolean(BoolOp op, const Shape& target, const Shape& tool, JobContext& ctx)
{
    constexpr std::string_view where = "kernel::boolean";
    if (op != BoolOp::Union && op != BoolOp::Cut && op != BoolOp::Intersect)
        return tl::unexpected(detail::makeError(ErrorCode::InvalidArgument, "Unknown boolean operation.", where));
    if (target.isNull() || target.count(SubKind::Solid) == 0)
        return tl::unexpected(
            detail::makeError(ErrorCode::InvalidArgument, "The body to modify must be a solid.", where));
    if (tool.isNull() || tool.count(SubKind::Solid) == 0)
        return tl::unexpected(detail::makeError(ErrorCode::InvalidArgument, "The tool body must be a solid.", where));
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(detail::cancelledError(where));
    return detail::guarded(where, [&]() -> Result<OpResult> {
        const double targetVolume = detail::solidVolume(detail::occt(target));
        const double toolVolume = detail::solidVolume(detail::occt(tool));
        if (!(targetVolume > 0.0) || !(toolVolume > 0.0))
            return tl::unexpected(kernelFailure("A body without volume cannot take part in a boolean.", where));
        Attempt unified = attempt(op, target, tool, true, targetVolume, toolVolume, ctx, where);
        if (unified.result || !unified.invalid) {
            if (unified.result)
                ctx.progress.set(1.0, "Done");
            return std::move(unified.result);
        }
        if (auto ok = ctx.cancel.check(); !ok)
            return tl::unexpected(detail::cancelledError(where));
        Attempt plain = attempt(op, target, tool, false, targetVolume, toolVolume, ctx, where);
        if (!plain.result)
            return std::move(unified.result);
        plain.result->warnings.insert(plain.result->warnings.begin(),
                                      "Faces that lie on one surface were left as separate faces: merging them "
                                      "made the solid invalid.");
        ctx.progress.set(1.0, "Done");
        return std::move(plain.result);
    });
}

Result<OpResult> transformed(const Shape& shape, const Xform& xform)
{
    constexpr std::string_view where = "kernel::transformed";
    if (auto ok = detail::requireShape(shape, where); !ok)
        return tl::unexpected(ok.error());
    for (const double value : xform.m)
        if (!std::isfinite(value))
            return tl::unexpected(
                detail::makeError(ErrorCode::InvalidArgument, "The transform must be finite.", where));
    // Rows of the rotation part must be orthonormal and right-handed: a rigid move, no scale, no mirror.
    const auto r = [&](int i, int j) {
        return xform.m[i * 4 + j];
    };
    constexpr double kRigidTolerance = 1e-9;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double dot = r(i, 0) * r(j, 0) + r(i, 1) * r(j, 1) + r(i, 2) * r(j, 2);
            if (std::abs(dot - (i == j ? 1.0 : 0.0)) > kRigidTolerance)
                return tl::unexpected(detail::makeError(
                    ErrorCode::InvalidArgument, "Only rigid moves are allowed: the transform scales or skews.", where));
        }
    const double det = r(0, 0) * (r(1, 1) * r(2, 2) - r(1, 2) * r(2, 1)) -
                       r(0, 1) * (r(1, 0) * r(2, 2) - r(1, 2) * r(2, 0)) +
                       r(0, 2) * (r(1, 0) * r(2, 1) - r(1, 1) * r(2, 0));
    if (!(det > 0.0))
        return tl::unexpected(detail::makeError(
            ErrorCode::InvalidArgument, "Only rigid moves are allowed: the transform mirrors the body.", where));
    bool identity = true;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            if (std::abs(r(i, j) - (i == j ? 1.0 : 0.0)) > 1e-12)
                identity = false;
    if (identity)
        return tl::unexpected(kernelFailure(
            "The move does nothing: the transform is the identity, so the body would not change.", where));

    return detail::guarded(where, [&]() -> Result<OpResult> {
        gp_Trsf trsf;
        trsf.SetValues(r(0, 0), r(0, 1), r(0, 2), r(0, 3), r(1, 0), r(1, 1), r(1, 2), r(1, 3), r(2, 0), r(2, 1),
                       r(2, 2), r(2, 3));
        BRepBuilderAPI_Transform maker(detail::occt(shape), trsf, true);
        if (!maker.IsDone() || maker.Shape().IsNull())
            return tl::unexpected(kernelFailure("The body could not be moved.", where));
        OpResult out;
        out.shape = detail::wrap(maker.Shape());
        for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face, SubKind::Solid})
            if (out.shape.count(kind) != shape.count(kind))
                return tl::unexpected(kernelFailure("Moving the body changed its topology.", where));
        if (!BRepCheck_Analyzer(maker.Shape()).IsValid()) {
            if (BRepCheck_Analyzer(detail::occt(shape)).IsValid())
                return tl::unexpected(kernelFailure("Moving the body made it invalid.", where));
            out.warnings.push_back("The body was already invalid before the move.");
        }
        if (shape.count(SubKind::Solid) > 0) {
            const double before = detail::solidVolume(detail::occt(shape));
            const double after = detail::solidVolume(maker.Shape());
            if (std::abs(after - before) > 1e-9 * std::max(1.0, std::abs(before)))
                return tl::unexpected(kernelFailure("Moving the body changed its volume.", where));
        }
        const std::array<const Shape*, 1> inputs{&shape};
        out.history = detail::buildHistory(maker, inputs, out.shape);
        const std::vector<SubId> orphans = detail::orphanFaces(out.history, out.shape);
        if (!orphans.empty())
            out.warnings.push_back(detail::orphanWarning(orphans));
        return out;
    });
}

} // namespace ray::kernel
