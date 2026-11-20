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

// The one solid of `shape` if it has exactly one, else a compound of its solids in explorer order (null if none).
TopoDS_Shape normaliseSolids(const TopoDS_Shape& shape);

} // namespace ray::kernel::detail
