// boolean() and transformed() (task T-094).
//
// BOOLEAN. One BRepAlgoAPI_BooleanOperation (cut, fuse or common) with OCCT parallelism off (determinism,
// PLAN.md section 7 rule 4) and non-destructive mode on (OCCT must not touch the tolerances of the caller's
// immutable shapes), then SimplifyResult() so coplanar and co-cylindrical pieces are unified and the history of
// that unification is folded into the maker's history (spike S4). OCCT's own status is not trusted (section 7
// rule 6), so the result is checked:
//   1. not empty; every face inside a solid; BRepCheck finds no fault; every shell closed; every volume positive
//      (when both inputs passed these checks, the faces of theirs the result keeps untouched are not given to
//      BRepCheck again: boolean_internal.h INCREMENTAL CHECK; an input's volume is measured once per Shape);
//   2. not a no-op: the volume changed (beyond 1e-12 relative), and some face of the result comes from the tool
//      (removing or adding material always leaves tool faces on the boundary);
//   3. the volume moved the expected way: a cut shrinks the target, a union grows it by at most the tool, an
//      intersection is no larger than either input (T-080: a cut's or union's change below 1e-4 of the body is
//      measured inside the tool's box, boolean_internal.h LOCAL VOLUME CHANGE, and a cut then removes at most the
//      tool);
//   4. a union is one solid. A cut or intersection may leave several solids; they are returned as a compound and
//      `warnings` says so.
// If the unified result fails check 1 but the plain boolean passes, the plain result is returned with a warning
// (ShapeUpgrade_UnifySameDomain turned 10 of 496 valid results into invalid solids in spike S9).
// Cancel: OCCT polls JobIndicator::UserBreak() during the boolean; a cancelled call returns Error{Cancelled}.
//
// SLIVERS (boolean_internal.h). A sketch placed through a single-precision point picked on a face lies up to about
// 1e-6 mm off it, and a cut from there leaves a skin that thin over the pocket (the naming test harness hit one
// 7.6e-7 mm thick: y = 50.7999992 against a face at y = 50.8). OCCT is right to keep it: 7.6e-7 is more than its
// Precision::Confusion (1e-7). So when the result has a sliver that involves a face from the tool, the boolean is run
// again with OCCT's fuzzy option at kSliverWidth (1e-4 mm), which takes elements that close to be coincident, and
// that result is returned, with a warning, if it passes every check above and has no sliver. Otherwise the first
// result is returned with a warning naming the sliver faces. A boolean without slivers runs exactly as before, so
// its result and history do not change.
//
// IMMUTABLE INPUTS (PLAN.md section 7 rule 1). Non-destructive mode is not enough: on OCCT 8.0.1 Build() clears
// the Free flag of both argument solids and SimplifyResult() adds pcurves to edges the result shares with the
// arguments (their canonical bytes change), and UnifySameDomain can make a B-spline surface periodic in place. With
// the caller's shapes shared between features and threads, that made a later operation on the same body give a
// different result (the naming random test: 54 of 1,000 sequences differed between two identical runs, and one
// reference resolved to a wrong face). So every attempt runs on fresh private copies of target and tool, geometry
// included (boolean_internal.h PrivateCopy), and the history is mapped back to the caller's SubIds. Cost, measured
// on the largest corpus body (nist_ctc_04, 517 faces): 9 to 11 ms and about 1.9 MB per copy against 280 to 340 ms
// for the cut; the result keeps the copy's untouched faces instead of sharing the input's.
//
// TRANSFORMED. A rigid move only (core.h Xform). The geometry is copied and moved (BRepBuilderAPI_Transform with
// copied geometry), so the result shares no TShape with the input and carries no location; every sub-shape is
// reported Modified to its moved copy. The identity returns the input shape itself, every sub-shape Modified to
// itself: section 7 rule 6 (detect no-ops) is about OCCT reporting success while doing nothing when a change was
// asked for, and the identity asks for none, so callers (setup frames that are often the identity) need not
// special-case it.
#include "boolean_internal.h"

#include <BOPAlgo_Alerts.hxx>
#include <BOPAlgo_Operation.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Bnd_Box.hxx>
#include <NCollection_List.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ray::kernel {

namespace {

using detail::kernelFailure;

// Volume changes at or below this fraction of the larger input count as no change at all.
constexpr double kNoOpRelative = 1e-12;
// A volume that is "unchanged" by topology (no tool face in the result) may still differ by integration noise.
constexpr double kNoiseRelative = 1e-6;
// Slack on the upper and lower volume bounds of check 3.
constexpr double kBoundRelative = 1e-7;
// Slack on "a cut removes, a union adds, at most the tool" when both are measured locally: the volumes of tiny pieces
// are integrated less precisely (a 0.3 mm3 prism and the 0.3 mm3 it cut measured 1e-4 apart, a 0.0016 mm3
// wedge 2.5e-3). The check is there for gross errors.
constexpr double kLocalToolSlack = 1e-2;
// Whether the private copies of the inputs get their own curves and surfaces too (see "IMMUTABLE INPUTS").
constexpr bool kCopyGeometry = true;

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
    bool invalid = false;       // failed check 1 (retrying without unification may help)
    std::vector<SubId> slivers; // of a successful result: its sliver faces that involve the tool (SLIVERS above)
};

Attempt failed(Error error, bool invalid = false)
{
    return {tl::unexpected(std::move(error)), invalid, {}};
}

// The result faces that come from a face of the tool (a Modified or Generated entry of input 1), by SubId index.
std::vector<bool> toolFaces(const OpResult& out)
{
    std::vector<bool> fromTool(out.shape.count(SubKind::Face), false);
    for (const HistoryEntry& entry : out.history.entries)
        if (entry.inputIndex == 1 && entry.source.kind == SubKind::Face &&
            entry.relation != HistoryEntry::Relation::Deleted)
            for (const SubId& r : entry.results)
                if (r.kind == SubKind::Face && r.index < fromTool.size())
                    fromTool[r.index] = true;
    return fromTool;
}

std::string faceList(std::span<const SubId> faces)
{
    std::string text;
    for (std::size_t i = 0; i < faces.size(); ++i)
        text += (i == 0 ? "" : ", ") + std::to_string(faces[i].index);
    return text;
}

// `fuzzy` > 0: OCCT treats elements closer than that as coincident (BOPAlgo fuzzy option).
Attempt attempt(BoolOp op, const Shape& target, const Shape& tool, bool unify, double fuzzy, double targetVolume,
                double toolVolume, JobContext& ctx, std::string_view where)
{
    // OCCT works on private copies (boolean_internal.h PrivateCopy): Build and SimplifyResult write into the TShapes
    // of their arguments. Fresh copies for every attempt, so the retry without unification does not see what the
    // first attempt wrote.
    auto targetCopy = detail::privateCopy(target, kCopyGeometry, where);
    if (!targetCopy)
        return failed(targetCopy.error());
    auto toolCopy = detail::privateCopy(tool, kCopyGeometry, where);
    if (!toolCopy)
        return failed(toolCopy.error());
    const OpWords words = wordsFor(op);
    BRepAlgoAPI_BooleanOperation maker;
    NCollection_List<TopoDS_Shape> arguments;
    arguments.Append(detail::occt(targetCopy->shape));
    NCollection_List<TopoDS_Shape> tools;
    tools.Append(detail::occt(toolCopy->shape));
    maker.SetArguments(arguments);
    maker.SetTools(tools);
    maker.SetOperation(occtOperation(op));
    maker.SetRunParallel(false);
    maker.SetNonDestructive(true);
    if (fuzzy > 0.0)
        maker.SetFuzzyValue(fuzzy);
    occ::handle<detail::JobIndicator> indicator = new detail::JobIndicator(ctx, 0.0, 0.8, words.stage);
    maker.Build(indicator->Start());
    if (ctx.cancel.cancelled() || maker.HasError(STANDARD_TYPE(BOPAlgo_AlertUserBreak)))
        return failed(detail::cancelledError(where));
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
        return failed(std::move(error));
    }
    if (unify)
        maker.SimplifyResult();
    ctx.progress.set(0.85, "Checking the result");

    const TopoDS_Shape solids = detail::normaliseSolids(maker.Shape());
    if (solids.IsNull())
        return failed(kernelFailure(emptyMessage(op), where));
    // Faces of the inputs the result keeps untouched are not checked again when both inputs passed validation
    // (boolean_internal.h INCREMENTAL CHECK).
    const std::array<const Shape*, 2> originals{&target, &tool};
    const std::array<TopoDS_Shape, 2> copies{detail::occt(targetCopy->shape), detail::occt(toolCopy->shape)};
    const std::span<const TopoDS_Shape> unchangedFrom =
        detail::allValidated(originals) ? std::span<const TopoDS_Shape>(copies) : std::span<const TopoDS_Shape>();
    auto checked = detail::validateSolids(solids, words.subject, where, unchangedFrom);
    if (!checked) {
        Error error = kernelFailure(invalidMessage(op), where);
        error.context.insert(error.context.begin(), checked.error().message);
        return failed(std::move(error), true);
    }
    const double volume = *checked;

    OpResult out;
    out.shape = detail::wrap(solids);
    detail::markValidated(out.shape, volume);
    const std::array<const Shape*, 2> inputs{&targetCopy->shape, &toolCopy->shape};
    out.history = detail::buildHistory(maker, inputs, out.shape);
    for (int input = 0; input < 2; ++input)
        if (auto mapped = detail::remapSources(out.history, input, input == 0 ? *targetCopy : *toolCopy, where);
            !mapped)
            return failed(mapped.error());

    bool toolContributes = false;
    for (const HistoryEntry& entry : out.history.entries)
        if (entry.inputIndex == 1 && entry.source.kind == SubKind::Face &&
            entry.relation == HistoryEntry::Relation::Modified && !entry.results.empty())
            toolContributes = true;

    const double scale = std::max({targetVolume, toolVolume, 1.0});
    // When the whole-body change is small next to the body, whole-body integration noise can hide it or turn its sign
    // (boolean_internal.h LOCAL VOLUME CHANGE): it is then measured inside the tool's box, and the whole-body
    // change must agree with it within that noise.
    std::optional<double> local;
    if (op != BoolOp::Intersect && std::abs(volume - targetVolume) < detail::kLocalVolumeRelative * targetVolume) {
        Bnd_Box region;
        BRepBndLib::Add(detail::occt(toolCopy->shape), region, false);
        region.Enlarge(1e-3 + 0.05 * std::sqrt(region.SquareExtent()));
        local = detail::localVolumeChange(detail::occt(targetCopy->shape), solids, region);
    }
    const double change = local ? std::abs(*local) : std::abs(volume - targetVolume);
    const double changeScale = local ? std::max(toolVolume, 1.0) : scale;
    if (change <= kNoOpRelative * changeScale || (!toolContributes && change <= kNoiseRelative * changeScale))
        return failed(kernelFailure(noOpMessage(op), where));
    const double slack = kBoundRelative * scale;
    // A union adds at most the tool, judged on the whole bodies with their integration noise (T-080: NIST CTC 02 and
    // the NIST/MTC cover came out 34 and 4.7 mm3 over for correct unions; 1e-7 of the body refused them).
    const double unionSlack = std::max(slack, detail::kVolumeNoiseRelative * targetVolume + 1e-3 * toolVolume);
    bool expected = true;
    switch (op) {
    case BoolOp::Cut:
        expected =
            local ? (*local < 0.0 && -*local <= toolVolume * (1.0 + kLocalToolSlack) + 1e-6) : volume < targetVolume;
        break;
    case BoolOp::Union:
        expected = local ? (*local > 0.0 && *local <= toolVolume * (1.0 + kLocalToolSlack) + 1e-6)
                         : volume > targetVolume && volume <= targetVolume + toolVolume + unionSlack;
        break;
    case BoolOp::Intersect:
        expected = volume <= std::min(targetVolume, toolVolume) + slack;
        break;
    }
    if (local && std::abs((volume - targetVolume) - *local) > detail::kVolumeNoiseRelative * targetVolume)
        expected = false; // the body changed away from the tool
    if (!expected) {
        Error error = kernelFailure(invalidMessage(op), where);
        error.context.insert(error.context.begin(),
                             "volume " + std::to_string(targetVolume) + " -> " + std::to_string(volume) +
                                 (local ? " (" + std::to_string(*local) + " inside the tool's box, tool " +
                                              std::to_string(toolVolume) + ")"
                                        : std::string()) +
                                 " is not the expected change");
        return failed(std::move(error), true);
    }

    const std::uint32_t solidCount = out.shape.count(SubKind::Solid);
    if (solidCount > 1) {
        if (op == BoolOp::Union)
            return failed(kernelFailure(
                "The added material does not overlap the body, or touches it only along an edge or at a point, so "
                "the result would be separate solids. Move it so it overlaps the body or shares a face with it.",
                where));
        out.warnings.push_back(std::string(words.subject) + " leaves " + std::to_string(solidCount) +
                               " separate solids.");
    }
    const std::vector<SubId> orphans = detail::orphanFaces(out.history, out.shape);
    if (!orphans.empty())
        out.warnings.push_back(detail::orphanWarning(orphans));
    std::vector<SubId> slivers = detail::sliverFaces(out.shape, toolFaces(out));
    return {std::move(out), false, std::move(slivers)};
}

// The unified attempt, and the plain one if unifying made the solid invalid (see the file comment).
Attempt attemptBoth(BoolOp op, const Shape& target, const Shape& tool, double fuzzy, double targetVolume,
                    double toolVolume, JobContext& ctx, std::string_view where)
{
    Attempt unified = attempt(op, target, tool, true, fuzzy, targetVolume, toolVolume, ctx, where);
    if (unified.result || !unified.invalid)
        return unified;
    if (auto ok = ctx.cancel.check(); !ok)
        return failed(detail::cancelledError(where));
    Attempt plain = attempt(op, target, tool, false, fuzzy, targetVolume, toolVolume, ctx, where);
    if (!plain.result)
        return unified;
    plain.result->warnings.insert(plain.result->warnings.begin(),
                                  "Faces that lie on one surface were left as separate faces: merging them made the "
                                  "solid invalid.");
    return plain;
}

} // namespace

namespace detail {
std::atomic<int>& sliverMergeOffCount() noexcept
{
    static std::atomic<int> count{0};
    return count;
}
} // namespace detail

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
        const double targetVolume = detail::volumeOf(target);
        const double toolVolume = detail::volumeOf(tool);
        if (!(targetVolume > 0.0) || !(toolVolume > 0.0))
            return tl::unexpected(kernelFailure("A body without volume cannot take part in a boolean.", where));
        Attempt exact = attemptBoth(op, target, tool, 0.0, targetVolume, toolVolume, ctx, where);
        // Tests only (testing::SliverMergeOff): the result as it was before this rule, slivers and all, no warning.
        const bool mergeSlivers = detail::sliverMergeOffCount().load(std::memory_order_relaxed) == 0;
        if (exact.result && mergeSlivers && !exact.slivers.empty()) {
            // SLIVERS: a tool face a rounding error off a face of the body. Again with OCCT's fuzzy option, so the
            // two count as one; kept only if that removes every sliver.
            if (auto ok = ctx.cancel.check(); !ok)
                return tl::unexpected(detail::cancelledError(where));
            Attempt fuzzy = attemptBoth(op, target, tool, detail::kSliverWidth, targetVolume, toolVolume, ctx, where);
            if (fuzzy.result && fuzzy.slivers.empty()) {
                fuzzy.result->warnings.push_back(
                    "Faces of the tool less than 0.0001 mm from faces of the body were taken to lie on them, so the "
                    "result has no skin, step or gap that thin.");
                exact = std::move(fuzzy);
            } else {
                exact.result->warnings.push_back(
                    "The result has material or a gap thinner than 0.0001 mm at face" +
                    std::string(exact.slivers.size() == 1 ? " " : "s ") + faceList(exact.slivers) +
                    ": a face of the tool lies a rounding error off a face of the body. Place the sketch on the face "
                    "itself.");
            }
        }
        if (exact.result)
            ctx.progress.set(1.0, "Done");
        return std::move(exact.result);
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
    if (identity) {
        // The input itself (shapes are immutable), every sub-shape Modified to itself.
        OpResult out;
        out.shape = shape;
        for (std::size_t slot = 0; slot < detail::kSubKindCount; ++slot) {
            const auto kind = static_cast<SubKind>(slot);
            for (std::uint32_t i = 0; i < shape.count(kind); ++i)
                out.history.entries.push_back({HistoryEntry::Relation::Modified, 0, SubId{kind, i}, {SubId{kind, i}}});
        }
        return out;
    }

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
        if (!detail::checkedValid(maker.Shape())) {
            if (detail::checkedValid(detail::occt(shape)))
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
