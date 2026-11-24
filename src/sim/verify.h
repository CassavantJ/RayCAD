// Safety checks of a setup's program (task T-071): ISimulator::verifyFast (include/ray/sim.h) as free functions, so the
// simulator (T-072) calls them from its verifyFast and its run without rewriting them.
//
// WHAT IS CHECKED, AND AGAINST WHAT. The toolpath IR of every operation is expanded into straight sub-motions of the
// tool tip, in doubles (verify_motion.cpp; canned cycles as cam.h "Canned cycles" and buildBackplot expand them, arcs
// as chords whose sagitta is at most kVerifyArcSagittaMm, the tool inflated by that much along them). Each sub-motion
// sweeps the tool's three zones (ToolShape: cutting, shank, holder; the holder zone carries the endless spindle side,
// and without a holder the endless part above the shank stands for it). Two layers:
//   ANALYTIC (verify_geom.cpp): exact sweeps of the tool's convex pieces against the exact triangles of the initial
//     stock, the part and each fixture; no resolution, no tier. Every zone is grown by kVerifyGrowMm first, so a
//     contact within kVerifyContactMm of the real tool is a hit (a case too close to call is raised).
//       - fixtures: cutting or shank zone -> ToolHitsFixture; holder zone -> HolderHitsFixture (every motion);
//       - part: a rapid's cutting or shank zone deeper than kPartSkinMm into the part -> RapidIntoStock ("into the
//         part"); a feed's shank zone -> ShankHitsPart; the holder zone -> HolderHitsPart (every motion);
//       - initial stock: a rapid (any zone) -> RapidIntoStock; a feed's shank zone -> ShankHitsStock, holder zone ->
//         HolderHitsStock; a plunge of a tool that does not cut at its centre (its dead zone) ->
//         PlungeWithNonCentreCutting. The initial stock is what was there before the program; the program removes
//         material as it goes, which no analytic test knows. So a stock contact is CERTAIN only where no cutting
//         sweep before it (any operation, rapids too; for shank and holder the motion's own as well) comes near:
//         when the box of the contact region (the zone's swept box clipped to the stock's box) overlaps no earlier
//         cutting sweep's box. Certain contacts are reported here. The others are DEFERRED to the second layer.
//   CUT STOCK (verify_cutstock.cpp): only when something was deferred, the program is cut into the checker's OWN
//     tri-dexel model (makeTriDexelCpu) at a FIXED cell, kCutStockCellMm (doubled until the stock fits in
//     kCutStockMaxDexels), never the tier preset's. Deferred motions are asked against the material as it is at that
//     point (a rapid and a plunge before the motion cuts; shank and holder as cutBatch's nonCuttingHits: before the
//     motion cuts unless it goes down). Hits become RapidIntoCutStock, ShankHitsStock, HolderHitsStock and
//     PlungeWithNonCentreCutting with the cell in the message. Resolution-dependent as IStockModel::intersects is:
//     material thinner than a cell between the model's Z rays can be missed (spike S6), which the message says.
//   Results depend on neither the tier (nothing reads a preset) nor the thread count (the model is bit-identical for
//   any worker count; everything else runs in program order on the calling thread).
//
// THE START. Each toolpath's first motion is the tool standing at its start point, checked as a zero-length rapid of
// move 0. The moves that bring it there from the tool change are the post's and are not in the IR: not checked.
//
// MOTIONS SKIPPED. A sub-motion straight up along the tool axis (X and Y unchanged) of a tool whose radius never
// narrows going up (cutting, shank and holder joined) sweeps only space the tool already held at its start, so it can
// meet nothing new: it is not checked (it is still cut into the cut-stock model). Every other motion is checked.
//
// PLUNGES. A feed sub-motion going down more steeply than kPlungeMinAngleDeg from horizontal (straight down included:
// canned cycles' feeds). Tools that cut at their centre (cam invariants' convention, centreCuttingByDefault): Drill,
// SpotDrill, BallEndMill, EngraveCutter, and ChamferMill without a flat at its tip. The others (FlatEndMill,
// BullEndMill, FaceMill, Tap, Reamer, BoringBar, ThreadMill, a chamfer mill with a tip flat, and an operation whose
// tool is not known) have a dead zone: a disc at the tip of radius kDeadZoneFraction of the cutting radius (at most the
// flat of the tip). A plunge whose dead zone meets material is the event; a tap into a drilled hole is not one.
//
// TOOL TOO SHORT. Per operation, from its FEED motions (a rapid that hits is the rapid's fault, not the tool's length):
//   - the holder zone met the part, a fixture or the stock: one ToolTooShort with amountMm the extra stick-out that
//     clears every one of those motions (the holder lifted by it; what comes out of the holder is the tool's flute,
//     which cuts, up to its flute length, then its shank, which must clear too), plus kStickOutMarginMm, rounded up to
//     0.1 mm; the message gives the minimum safe stick-out, and says when it is more than the tool allows (its overall
//     length less kMinGripShankDiameters shank diameters held in the holder) or when no stick-out clears (material
//     within the shank's radius above the flutes). Each later hit is first tried at the stick-out found so far;
//     only a hit that it does not clear is searched (bisection to 0.1 um);
//   - the shank zone met the stock or the part: one ToolTooShort with amountMm the extra flute length the cut needs
//     (the message gives the length from the tip the shank must clear).
//
// TRAVEL. The machine's X, Y and Z axes whose travel is known (max > min). Positions: X and Y of the tool axis, Z of
// the gauge line (tip Z plus the height of the tool's top above its tip: the top of the holder profile, or of the shank
// or cutting profile without a holder). With VerifyInput::wcsInMachine (the work offset as the control holds it, so
// that machine Z = wcsInMachine.z + tip Z + tool length) every point is checked against the travel; without it, which
// is the case of SimProgram (no setup stores it), only the program's extent along each axis against the travel's
// length: an extent longer than the axis fits no work offset (amountMm the excess). That catches a program too large
// for the machine, not one placed wrongly on it. A position exactly on a limit is within the travel; along an arc the
// extent is widened by kVerifyArcSagittaMm.
//
// EVENTS. One per run of consecutive IR moves of one operation with the same kind, obstacle and layer (moveIndex the
// first; the message counts the rest), each at the tool tip of the first contact found along that run's first motion.
// Sorted by operation (program order), move index, kind. All Severity::Error. amountMm: ToolTooShort the extra length,
// TravelLimit the excess, collisions 0.
//
// FRAME. Every solid and the toolpaths are in the setup WCS, mm (sim.h SimProgram). Solids are closed triangle soups,
// either orientation (inside = nonzero winding).
#pragma once

#include "toolshape.h"

#include <ray/sim.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ray::sim::detail {

// ---------- Judgment constants (each a shop-practice decision; the report of T-071 lists them) ----------
// The tool counts as touching a solid when it comes within this distance (above float noise, below any shop clearance).
inline constexpr double kVerifyContactMm = 1e-3;
// Every zone is grown by this (grownShape: a superset of the zone grown by a ball): the contact distance plus the
// sagitta of the chords makeToolShape puts on a ball or corner radius, which lie inside the real tool by up to
// kArcChordTolMm. So a contact within kVerifyContactMm of the real tool is always a hit, and one farther than
// kVerifyGrowMm (plus kVerifyArcSagittaMm along an arc) from the swept tool never is.
inline constexpr double kVerifyGrowMm = kVerifyContactMm + kArcChordTolMm;
// Arcs become chords whose sagitta is at most this; the tool is inflated by it along them.
inline constexpr double kVerifyArcSagittaMm = 5e-3;
// A rapid's cutting or shank zone may lie this far inside the part's triangles without an event: a retract along a
// finished wall or out of a drilled hole rubs the part, and a part soup's chords lie inside its curved faces. The part
// soup must be triangulated within this (T-072). Contacts this shallow are still found against the stock.
inline constexpr double kPartSkinMm = 0.01;
// A feed motion going down more steeply than this from horizontal is a plunge (shipped ramp presets reach 3 degrees).
inline constexpr double kPlungeMinAngleDeg = 5.0;
// A tool that does not cut at its centre cannot cut within this fraction of its radius (the cam invariant checker's
// deadZoneFraction).
inline constexpr double kDeadZoneFraction = 0.25;
// The dead zone is a disc of this height at the tip.
inline constexpr double kDeadZoneHeightMm = 0.01;
// Holder clearance added to the stick-out a collision needs.
inline constexpr double kStickOutMarginMm = 2.0;
// At least this many shank diameters stay inside the holder, which bounds the stick-out a tool allows.
inline constexpr double kMinGripShankDiameters = 2.5;
// The cut-stock model's cell (not a tier's: Low 0.2, Medium 0.1 and High 0.05 are display and simulation settings).
inline constexpr double kCutStockCellMm = 0.1;
// Above this many dexels in all three grids the cut-stock cell doubles (about 160 MB of dexels).
inline constexpr std::uint64_t kCutStockMaxDexels = 8'000'000;
// Sub-motions per cutBatch call of the cut-stock layer.
inline constexpr std::size_t kCutStockBatch = 2048;

// One operation of the program.
struct VerifyOperation {
    OperationId id;
    const cam::Toolpath* path = nullptr; // null or empty: nothing to check
    const ToolShape* shape = nullptr;    // the tool as makeToolShape builds it (with its holder and stick-out)
    const lib::Tool* tool = nullptr;     // null: not known (taken as not centre-cutting, no stick-out limit)
};

struct VerifyInput {
    std::vector<VerifyOperation> ops;             // program order
    const kernel::TriSoup* stock = nullptr;       // the INITIAL stock (closed); null or empty: no stock checks
    const kernel::TriSoup* part = nullptr;        // null or empty: no part checks
    std::vector<const kernel::TriSoup*> fixtures; // each closed; nulls are skipped
    const lib::MachineDef* machine = nullptr;     // null: no travel checks
    std::optional<Vec3> wcsInMachine;             // see TRAVEL above
};

struct VerifyOptions {
    bool cutStock = true; // false: the analytic layer only (deferred contacts are then dropped; tests)
};

// What a run did (tests and the report).
struct VerifyStats {
    std::uint64_t motions = 0;        // straight sub-motions checked or skipped
    std::uint64_t skippedMotions = 0; // straight up with a tool that never narrows upward
    std::uint64_t deferred = 0;       // contacts with the initial stock handed to the cut-stock layer
    bool cutStockRan = false;
    double cutStockCellMm = 0; // the cell it used (0 when it did not run)
};

// The checks of a whole setup. Cancellable (polls ctx.cancel); reports progress. `jobs` runs the cut-stock model.
Result<std::vector<SimEvent>> verifyProgram(const VerifyInput&, IJobSystem& jobs, JobContext& ctx,
                                            const VerifyOptions& = {}, VerifyStats* stats = nullptr);

// The input of a SimProgram (pointers into it: keep it alive). SimProgram::toolDefs may be empty or shorter than ops.
VerifyInput verifyInputFrom(const SimProgram&);

// The cut-stock cell for a stock box: kCutStockCellMm, doubled until the three grids hold at most kCutStockMaxDexels.
double cutStockCell(const Box3& stockBounds);

// Whether a tool cuts at its centre (the cam invariant checker's default by type).
bool centreCutting(const lib::Tool* tool);

} // namespace ray::sim::detail
