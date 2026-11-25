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
// canned cycles' feeds). Whether the tool cuts at its centre is the tool's own answer (libraries.h Tool::cutsAtCentre:
// what its library entry states, else its type's default, centreCuttingByDefault). A tool that does not (by default
// FaceMill, Tap, Reamer, BoringBar, ThreadMill and a chamfer mill with a flat at its tip; any milling cutter whose
// entry says so; and an operation whose tool is not known) has a dead zone: a disc at the tip of radius
// kDeadZoneFraction of the cutting radius (at most the flat of the tip). A plunge whose dead zone meets material is the
// event; a tap into a drilled hole is not one.
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
// TravelLimit the excess, collisions 0. The setup's events (SETUP below) come before all of them.
//
// FRAME. Every solid and the toolpaths are in the setup WCS, mm (sim.h SimProgram). Solids are closed triangle soups,
// either orientation (inside = nonzero winding).
//
// SETUP (added after T-074; verify_setup.cpp). Before the moves, once per program: each fixture solid against the part
// and against the INITIAL stock. Contact is not interference: a vise jaw against the stock's side, the stock on the
// vise floor or on parallels ("Place on stock", ray_api/cam_fixtures.h) touch over whole faces. So two solids overlap
// only when the overlap is DEEPER than a tolerance: when a point of one solid's surface lies inside the other more than
// the tolerance from every one of its triangles (either way round: the fixture's surface in the part, or the part's in
// the fixture, which catches one solid wholly inside the other). For two blocks pushed together by d that depth is d.
//   The tolerance is kFixtureOverlapMm: it passes touching faces and the rounding of an import or a placement (a
//   STEP file's 1e-7 mm, a mate's rounded points, the soups' chords, 0.009 mm inside curved faces), and
//   catches any overlap a machinist would call wrong. Against a carried stock (SimProgram::stockCellMm) half its cell
//   more: its cut walls lie up to half a cell outside the material they stand for.
//   How, exactly on the triangles (verify_setup.cpp): every triangle of one solid that lies in the other's box is a
//   patch. A patch is settled when it lies within the tolerance of one triangle of the other (distance to a triangle is
//   convex, so its corners decide), or of a face of several triangles in one plane (its shadow on that plane inside the
//   face: so touching faces settle at once however they are triangulated), or of the other's surface as a whole (its
//   centre's distance plus its size), or when it is clear of that surface (then it is all outside, or all inside and an
//   overlap if clear by more than the tolerance: one point decides which, by the vote of three rays, as one ray running
//   along a face reads either way). Otherwise it is cut in two across its longest edge, down to patches
//   kFixturePatchFraction of the tolerance in radius, where its centre decides. So an overlap is reported only when one
//   is deeper than the tolerance, and always when one is deeper than the tolerance plus that radius. The work lies
//   along the lines where the faces that touch end, a few patches per tenth of a millimetre of them; at most
//   kFixturePatchesPerTriangle patches per piece (counted: none reached in the tests). The triangles are first cut
//   into pieces at most 2 mm across within the other's box, which run in parallel; the overlap reported is the first
//   found on the lowest-numbered piece, the same for any worker count.
//   Events (in fixture order, before every move's): a fixture overlapping the part is FixtureHitsPart (Error: the part
//   cannot be there); else one overlapping the initial stock is FixtureHitsStock (Warning: rough stock is often drawn
//   oversize, so it may stand as it is). moveIndex kSetupEvent, operation nil, position a point of the overlap (setup
//   WCS), amountMm how deep that point lies (an overlap is at least that deep).
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
// SETUP (added after T-074): a fixture overlaps the part or the stock when the overlap is deeper than this. Well above
// touching faces and placement rounding (micrometres), well below any misplacement that matters (a tenth and more).
inline constexpr double kFixtureOverlapMm = 0.05;
// Patches are cut down to this fraction of the tolerance across (the radius about their centre): an overlap deeper
// than the tolerance times (1 + this) is never missed.
inline constexpr double kFixturePatchFraction = 0.25;
// At most this many patches per piece of a triangle (the parallel search's units, at most 2 mm across: a bound on the
// time of a pathological pair, e.g. two curved soups lying on each other); a piece that reaches it is left as it is and
// counted (OverlapStats::capped).
inline constexpr std::uint32_t kFixturePatchesPerTriangle = 1u << 18;

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
    double stockCellMm = 0;                       // SimProgram::stockCellMm (SETUP above)
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
    double cutStockCellMm = 0;                       // the cell it used (0 when it did not run)
    std::uint64_t setupPatches = 0, setupCapped = 0; // the setup checks' OverlapStats (SETUP above)
};

// The checks of a whole setup. Cancellable (polls ctx.cancel); reports progress. `jobs` runs the cut-stock model.
Result<std::vector<SimEvent>> verifyProgram(const VerifyInput&, IJobSystem& jobs, JobContext& ctx,
                                            const VerifyOptions& = {}, VerifyStats* stats = nullptr);

// The input of a SimProgram (pointers into it: keep it alive). SimProgram::toolDefs may be empty or shorter than ops.
VerifyInput verifyInputFrom(const SimProgram&);

// ---------- Setup checks (verify_setup.cpp; SETUP above) ----------
struct OverlapStats {
    std::uint64_t triangles = 0; // triangles of either solid that lay in the other's box
    std::uint64_t patches = 0;   // patches looked at
    std::uint64_t capped = 0;    // triangles that reached kFixturePatchesPerTriangle
};
struct Overlap {
    Vec3 point;   // a point of one solid's surface inside the other
    double depth; // its distance from the other's surface (more than the tolerance)
};
// Whether solids a and b (closed soups) overlap by more than tolMm (SETUP above): a point of the overlap, or nothing.
// The same for any worker count. Cancellable.
Result<std::optional<Overlap>> overlapDeeperThan(const kernel::TriSoup& a, const kernel::TriSoup& b, double tolMm,
                                                 IJobSystem& jobs, JobContext& ctx, OverlapStats* stats = nullptr);
// The setup-level events of a program: each fixture against the part, then (when it does not overlap the part) against
// the initial stock. In fixture order. Cancellable; progress on ctx.progress.
Result<std::vector<SimEvent>> setupEvents(const VerifyInput&, IJobSystem& jobs, JobContext& ctx,
                                          OverlapStats* stats = nullptr);

// The cut-stock cell for a stock box: kCutStockCellMm, doubled until the three grids hold at most kCutStockMaxDexels.
double cutStockCell(const Box3& stockBounds);

// Whether a tool cuts at its centre: Tool::cutsAtCentre; false for an unknown tool (null).
bool centreCutting(const lib::Tool* tool);

} // namespace ray::sim::detail
