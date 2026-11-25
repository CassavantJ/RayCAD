// Private to src/kernel and its tests (task T-080). Not a public interface. Includes no OCCT header, so
// tests/unit/kernel can use it: the S9 corpus measurement of the direct edits needs targets found and results judged
// "through OCCT directly", which the tests may not do themselves (PLAN.md 0.4 rule 1).
//
// THE S9 METHOD (spikes/s9-directedit/REPORT.md sections 3.3 to 3.5, main.cpp `analyse`, `prepare`, `verify`), kept so
// the rates measured here can be set beside the spike's:
//   - targets: holes (bore plus bottom), holes with a counterbore, countersink or chamfer (the whole stack), closed
//     pockets, fillets, blend runs of two or more faces, planar faces largest first (directedit_internal.h has the
//     rules); up to five of each kind per solid, spread evenly over the detected list;
//   - edits: delete each kind; resize each hole to 1.25 and 0.8 times its radius; push/pull each planar face out and
//     in by 1% of the bounding-box diagonal, clamped to 0.25..2 mm;
//   - a result is "ok" only if it is one valid closed solid (BRepCheck, no free or badly oriented edges, positive
//     volume), the volume changed (more than 1e-9 relative) in the expected direction by a plausible amount, and
//     points on the targets now lie inside, outside or on the result as the edit intends (on: within 1e-5 mm).
// The one change: a bore picked out of a stack is judged as deleting the whole stack, because deleteFaces removes
// the stack it belongs to (kernel.h; the spike deleted the bore alone).
#pragma once
#include <ray/kernel.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ray::kernel::testing {

enum class DirectEdit : std::uint8_t {
    DeleteFillet,   // one fillet face
    DeleteBlendRun, // every face of a blend run
    DeleteHole,     // one bore (with its bottom); the note says "plain" or "in_stack"
    DeleteHoleStack,
    DeletePocket,
    ResizeLarger,  // 1.25 times the radius
    ResizeSmaller, // 0.8 times the radius
    PushOut,
    PushIn,
};
inline constexpr int kDirectEditCount = 9;
// "del_fillet", "del_fillet_chain", "del_hole", "del_hole_stack", "del_pocket", "rs_grow", "rs_shrink", "pp_out",
// "pp_in" (the spike's names).
const char* directEditName(DirectEdit edit);

enum class PointWant : std::uint8_t { In, Out, On };
struct PointCheck {
    Vec3 point;
    PointWant want;
};

struct DirectEditCase {
    DirectEdit edit = DirectEdit::DeleteFillet;
    int k = 0;                // which of the (up to five) targets of this kind
    int available = 0;        // how many targets of this kind the solid has
    std::vector<SubId> faces; // deleteFaces / offsetFaces argument; resizeHole gets faces.front()
    double newDiameter = 0.0; // resize
    double distance = 0.0;    // push/pull, signed
    // DeleteFillet: "isolated" (touches no other blend face) or "chain_member"; DeleteHole: "plain" or "in_stack";
    // push/pull: "square" (every neighbour square to the face) or "angled".
    std::string note;
    Box3 region{};             // where the edit may change the body (the volume change is measured there)
    int sign = 0;              // expected sign of the volume change (0: either)
    double lo = 0.0, hi = 0.0; // plausible |volume change|
    std::vector<PointCheck> checks;
};

// Every case of THE S9 METHOD on `solid` (one solid), in the spike's order: edits as listed in DirectEdit, then k.
Result<std::vector<DirectEditCase>> directEditCases(const Shape& solid);

// category: "ok", "invalid" (not one valid closed solid), "no_op", or "wrong" (not the intended change); detail says
// why. The volume change is measured inside 'region' when the whole-body change is under 1e-4 of the body (whole-body
// integration is off by tens of mm3 on large imported bodies), and then must agree with the whole-body change within
// 2e-5 of the body (else "wrong": the body changed away from the edit).
struct DirectEditVerdict {
    std::string category;
    std::string detail;
};
DirectEditVerdict judgeDirectEdit(const Shape& input, const DirectEditCase& edit, const Shape& result);

// Why the request of a push/pull or a hole enlargement cannot be met by any body, decided from the input alone by a
// geometric test independent of the kernel's edit (empty if it can): pushed out, the moved face's sample point lies
// inside the body's material; pulled in, it lies outside the body, or the body minus the swept face falls apart;
// enlarged, the body minus the new hole's cylinder falls apart. For reporting rates both ways (T-080 round 2).
std::string infeasibleRequest(const Shape& input, const DirectEditCase& edit);

// What BRepCheck_Analyzer finds wrong with 'shape', one line per faulty sub-shape (kind, SubId index, statuses), at
// most 'limit' lines; empty if it is valid. For diagnosing imported files.
std::vector<std::string> validityFaults(const Shape& shape, int limit);

// While one lives, the next `count` direct edits fault inside their guarded OCCT work (directedit_internal.h): on
// Windows a structured exception, which the guard must turn into KernelFailure with the process still healthy.
class DirectEditFault {
public:
    explicit DirectEditFault(int count = 1);
    ~DirectEditFault();
    DirectEditFault(const DirectEditFault&) = delete;
    DirectEditFault& operator=(const DirectEditFault&) = delete;
};

// While one lives, direct edits report the face history OpenCASCADE gave, without demoting the images that look the
// other way or share no ground with their old face (directedit.cpp demoteReversedImages): to show that a test of
// that demotion meets a case of it.
class RawDirectEditHistory {
public:
    RawDirectEditHistory();
    ~RawDirectEditHistory();
    RawDirectEditHistory(const RawDirectEditHistory&) = delete;
    RawDirectEditHistory& operator=(const RawDirectEditHistory&) = delete;
};

// Joins the kernel threads that abandoned (cancelled) direct edits are still finishing on, and how many are running
// (io_internal.h: direct edits share the import threads).
void waitForAbandonedEdits();
int runningEditThreads();

} // namespace ray::kernel::testing
