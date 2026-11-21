// Private to src/kernel (task T-094): helpers shared by the modifying operations (sweeps, booleans, and the
// blends and direct edits that come later): progress and cancel for OCCT algorithms, history normalisation and
// result validation. Includes OCCT headers; nothing outside src/kernel may include it (PLAN.md 0.4 rule 1).
//
// HISTORY NORMALISATION (kernel.h ShapeHistory, spike S4). buildHistory() walks every vertex, edge, face and solid
// of every input Shape, in canonical SubId order, and asks the OCCT maker about it:
//   - Modified(s) that are in the result               -> one Modified entry, results sorted ascending;
//   - else, s itself is in the result (unchanged)      -> one Modified entry whose only result is s's SubId in the
//                                                         RESULT shape. SubIds are not kept across operations
//                                                         (the canonical order is a function of the whole shape),
//                                                         so an unchanged element is always listed explicitly;
//   - else                                             -> one Deleted entry;
//   - and in addition, Generated(s) that are in the result -> one Generated entry.
// Every input sub-shape therefore has exactly one Modified or Deleted entry, and results that are not in the
// final shape are dropped. Entries are ordered by input, then kind (vertex, edge, face, solid), then index, so the
// history is a function of the inputs and the result alone.
#pragma once
#include "shape_impl.h"

#include <BRepBuilderAPI_MakeShape.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>
#include <Message_ProgressScope.hxx>
#include <TopoDS_Shape.hxx>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ray::kernel::detail {

// Answers OCCT's cancel polls from a JobContext and reports OCCT's progress into [from, to] of ctx.progress.
// OCCT parallelism is off in every geometry-producing call (PLAN.md section 7 rule 4), so Show() runs on the
// calling thread; Progress::set is thread-safe in any case.
class JobIndicator final : public Message_ProgressIndicator {
public:
    JobIndicator(const JobContext& ctx, double from, double to, std::string_view stage)
        : cancel_(ctx.cancel), progress_(ctx.progress), from_(from), to_(to), stage_(stage)
    {
    }
    bool UserBreak() override { return cancel_.cancelled(); }
    void Show(const Message_ProgressScope&, const bool) override
    {
        progress_.set(from_ + (to_ - from_) * GetPosition(), stage_);
    }

private:
    CancelToken cancel_;
    Progress progress_;
    double from_, to_;
    std::string stage_;
};

// Error{Cancelled} with `where` as context.
Error cancelledError(std::string_view where);
// Error{KernelFailure} with a user-presentable message.
Error kernelFailure(std::string message, std::string_view where);

// A private copy of an input Shape, for the OCCT algorithms that write into the TShapes they are given (PLAN.md
// section 7 rule 1: a Shape never changes). Measured on OCCT 8.0.1 (tests/unit/kernel/immutability_test.cpp):
// BRepAlgoAPI_BooleanOperation::Build clears the Free flag of its argument solids even in non-destructive mode, and
// SimplifyResult (ShapeUpgrade_UnifySameDomain, safe input mode) adds pcurves to edges that the result shares with
// the arguments, which changes the arguments' canonical bytes; UnifySameDomain also makes a B-spline surface periodic
// in place when it merges faces across its seam. Fillets and chamfers write pcurves and tolerances into the shape too.
// The copy has the same canonical sub-shape order as the input in practice (BRepBuilderAPI_Copy rebuilds every
// container with its children in stored order), but the map is built and checked, never assumed.
struct PrivateCopy {
    Shape shape;                                                     // the copy
    std::array<std::vector<std::uint32_t>, kSubKindCount> toInput{}; // copy SubId index -> input SubId index, per kind
    bool identity = true;                                            // toInput maps every index to itself
};
inline constexpr std::uint32_t kNoInputIndex = 0xFFFFFFFFu;

// Copies `input` with BRepBuilderAPI_Copy (new TShapes; with `copyGeometry` also new curves and surfaces) and builds
// the SubId map back. KernelFailure if the copy's topology differs. May throw (call inside guarded()).
Result<PrivateCopy> privateCopy(const Shape& input, bool copyGeometry, std::string_view where);

// The input SubId of a SubId of the copy; empty if it has none.
std::optional<SubId> inputSubId(const PrivateCopy& copy, SubId id);

// Rewrites the sources of the entries of `history` whose inputIndex is `input` from the copy's SubIds to the input's,
// then restores the canonical entry order (input, kind, index; buildHistory's Modified/Deleted before Generated is
// kept by a stable sort). KernelFailure if a source has no input SubId.
Status remapSources(ShapeHistory& history, int input, const PrivateCopy& copy, std::string_view where);

// See "HISTORY NORMALISATION" above. `inputs[i]` must be the shapes the maker was given, in kernel.h input order.
// May throw (call inside guarded()).
ShapeHistory buildHistory(BRepBuilderAPI_MakeShape& maker, std::span<const Shape* const> inputs, const Shape& result);

// The result faces (in SubId order) that no Modified or Generated entry of `history` lists. Never throws.
std::vector<SubId> orphanFaces(const ShapeHistory& history, const Shape& result);

// A warning naming the orphan faces, for OpResult::warnings; empty if there are none.
std::string orphanWarning(std::span<const SubId> orphans);

// The solids of `shape` in explorer order.
std::vector<TopoDS_Shape> solidsOf(const TopoDS_Shape& shape);

// Checks that `shape` is made of valid closed solids: BRepCheck finds no fault, every shell of every solid is
// closed, every solid has a positive volume, and no face lies outside a solid. On success returns the summed
// volume. `what` names the operation in the message ("The extrusion", "The cut"). May throw.
Result<double> validateSolids(const TopoDS_Shape& shape, std::string_view what, std::string_view where);

// Volume of the solids of `shape` (0 if it has none). May throw.
double solidVolume(const TopoDS_Shape& shape);

// SLIVERS. Material or a gap thinner than kSliverWidth (mm) is never meant: it comes from two faces that were meant
// to lie on one plane but are a rounding error apart, such as a sketch placed through a single-precision point
// picked on a face (7.6e-7 mm off at y = 50.8). 1e-4 mm is 1000 times OCCT's Precision::Confusion, about the
// single-precision rounding of a coordinate of 2 m, and ten times below the finest tolerance a machine shop works
// to, so nothing anyone meant is that thin. A shape has a sliver where
//   - a face is narrower than kSliverWidth: twice its area over its perimeter, which is the width of a long thin
//     strip and no more than the width of any convex face (a step a rounding error high), or
//   - two planar faces face opposite ways on parallel planes less than kSliverWidth apart and their boxes overlap
//     (a skin of material, or a crack, a rounding error thick).
inline constexpr double kSliverWidth = 1e-4;

// How many testing::SliverMergeOff guards are alive; while it is above 0, kernel::boolean does not retry with the fuzzy
// option (tests only, shape_testing.h).
std::atomic<int>& sliverMergeOffCount() noexcept;

// The faces of `shape` that make a sliver, in SubId order. With `involved` (one flag per face of `shape`), only
// slivers with at least one involved face count (a boolean asks about the faces that came from its tool, so a sliver
// the body already had is not blamed on the tool). May throw (call inside guarded()).
std::vector<SubId> sliverFaces(const Shape& shape, const std::vector<bool>& involved = {});

// The one solid of `shape` if it has exactly one, else a compound of its solids in explorer order (null if none).
TopoDS_Shape normaliseSolids(const TopoDS_Shape& shape);

} // namespace ray::kernel::detail
