// Private to src/cam (task T-051): linking. Orders the cut passes of a milling operation and joins them into one
// toolpath: lead-ins and lead-outs, the entry into material (helix, ramp, pre-drilled point, plunge), retracts, and
// stay-down links through area already cut. cam.h linkPasses is the public form of linkPasses below (no regions).
//
// WHO USES WHAT (the operation tasks of PLAN.md M3/M4):
//   T-052 face      passFromPath per stroke, keepOrder (the stepover order is the op's), Pass::reversible = false;
//                   strokes start outside the stock (overhang), so the entry is a feed descent in air (Entered::Free)
//                   and a stroke-to-stroke link outside the stock stays down (it crosses no stock). One-way facing:
//                   the return link crosses uncut stock, so it retracts by itself.
//   T-053 contour   Lead::Arc (tangent arc lead-in/out, `length` for a straight part), Lead::Line for compensation in
//                   the control (Move::compensation on the pass's moves; the straight part of the lead-in switches it
//                   on, the straight end of the lead-out switches it off), Pass::room = where the tool centre may go
//                   (the part grown by the tool radius, complemented, clipped to the stock grown), one pass per depth
//                   with cluster/level so depths stay in order; tabs are z moves inside the pass (non-planar passes
//                   are fine, they enter by Free, PreDrill or Plunge).
//   T-054 pocket    rings as rotatable closed passes, cluster = pocket, level = depth index, maxStayDown with stepIn
//                   for the step from one ring to the next (Entered::Step: the tool centre stays in cut area, or with
//                   stepInSlot, added by T-054 for stepovers above the tool radius, in what the next ring cuts), plain
//                   stay-down links where the whole link lies in cut area, entryOrder {Helix, Ramp, PreDrill}, room =
//                   the pocket shrunk by the tool radius plus stock to leave; Options::preDrills for pre-drilled
//                   points.
//   T-055 slot      the centre line as an open pass per depth, Ramp (zigzag along it; the run must be at least
//                   minRampRun), helixDiameter 0 so no helix is tried; appendSloped for a ramp the op makes itself;
//                   maxStayDown with stayDownEntry (added by T-055) so each depth ramps in from the one above.
//   T-055 chamfer   one pass per centre path at the tip's depth, closed ones rotatable, entryOrder {Plunge} beside
//                   the edge (centreCutting), maxStayDown 0.
//   T-056 engrave   entryOrder {Plunge} with a centre-cutting engraver, tipHeight its point length; threadmill: the
//                   helical pass as moves, a
//                   Lead::Arc of radius (thread radius / 2) and sweep 180 from the hole centre (a radial lead-in), the
//                   drilled hole given as Stock::cleared (the linker does not know an earlier drilling operation).
//   T-061 adaptive  passes from the engine as moves (fitArcs, then passFromPath), maxStayDown with stayDownLift (lifted
//                   feed links), Stock::cleared from the engine's cleared area if it has one, Entered for the stats.
//
// HOW (linkPasses):
//   1. Check heights (clearance >= retract >= feed plane, retract >= stock top), the tool and every pass.
//   2. Order (orderPasses) unless keepOrder: nearest neighbour inside each sequence batch, respecting cluster levels,
//      then Or-opt and 2-opt in a window, then the orientation of every reversible pass by dynamic programming.
//   3. For each pass in order: its leads (shrunk towards Lead::minRadius when they do not fit the room), then the link
//      from where the tool is: stay down when the pass is within maxStayDown and the whole link provably lies in area
//      already cut at that depth (or outside the stock); else, with stepIn, step in at depth (Options::stepIn says
//      when); otherwise retract (feed up to the feed plane, or rapid with
//      rapidRetract), rapid across at the retract plane, rapid down to the feed plane (never lower, and never into
//      stock that is not cut); then the entry: a feed descent through cut area as far as it reaches, then helix, ramp,
//      pre-drill or plunge in the order asked for, the first that fits; none fits: the pass is refused, naming why each
//      did not fit.
//   4. After the last pass, up to the clearance.
//
// THE MATERIAL MODEL. Material is the stock region (XY) up to Stock::top. "Free at z" (the tool may be anywhere at
// height z and above over that area without touching material) holds outside the stock region, above the stock top,
// inside Stock::cleared areas at or above their z, and inside the area swept by the tool radius along every cut this
// call made before, at or above that cut's highest Z plus Options::tipHeight. Cut area is the sweep of a pass with its
// leads (planar passes: at their Z; others: at their highest Z) and the flat circle at the bottom of a helix; sloped
// entry moves are not counted (conservative). A free test of a move grows nothing: the tool's disc shrunk by
// kFreeTolerance must lie in free area, so a link may graze uncut material by at most kFreeTolerance (1 um) where its
// path runs along the edge of a cut.
// THE TOOL'S END. The tool is a cylinder of toolRadius above the height tipHeight over its tip, and inside that
// cylinder below it (a flat end mill: tipHeight 0; a ball end mill: its radius; a bull nose: its corner radius; a
// pointed tool (engraver, spot drill, chamfer mill): the height of its point, where it reaches its diameter). A cut
// clears the full tool radius along its path only from tipHeight above the cut up; below that it leaves a groove
// narrower than the tool (a V or a round bottom). So a cut counts as free area from its highest Z plus tipHeight up,
// and the rule above is the whole rule: every free test (stay-down links, the descent of an approach, the rapid down to
// the feed plane, the floor of a feed descent before an entry) uses the full tool radius at the tested height, which is
// conservative for the narrower tip below it. Stock::cleared areas are empty space and need no such margin. Moving
// straight up out of a cut is always free (the tool only leaves its own volume), so retracts are not tested. With
// tipHeight 0 (the default) every tool is taken as a cylinder, as before.
//
// SHORT MOVES. No link, retract or approach move linking makes is shorter than kMinLinkMove (a control writes such a
// move as nothing, or as a zero move): a shorter one is dropped when the move after it is a straight move, or a rapid
// after a short rapid (that move then starts where the short one started), or else merged into the link move before it
// (which then ends where the short one ended); a feed is never taken over by a rapid, so a short feed between rapids,
// or one before an arc of a pass with no link feed before it, stays. Rounded lead ends a nanometre or two apart (a
// radial lead-out and the next radial lead-in at a hole's centre) are the case it is for.
//
// REFUSALS (Error ToolpathFailed, or InvalidArgument for options the user must change), each naming the cause: heights
// out of order; the retract plane under the stock top; a pass above the feed plane, without moves, with a rapid or a
// cycle, an XZ/YZ arc, a move without feed; a lead that does not fit its room even at its minimum radius, or a lead the
// pass's compensation cannot use; an entry that no allowed kind can make (helix does not fit the room down to its
// minimum diameter, ramp run shorter than minRampRun, no pre-drilled point within reach, a plunge the tool cannot do),
// with the room width a helix would need.
//
// DETERMINISM. Integer nanometre input and output; every new point is rounded by geom2d::roundNm (half to even);
// trigonometry through ray::dmath; costs compared as doubles from integer differences (src/cam is built with strict
// floating point); ties broken by pass index. No clock, no unordered containers. Polls ctx.cancel per pass and inside
// the ordering loops; one Clipper2 call (a region test) cannot be interrupted, so the latency is that of the largest
// region test (about a millisecond for rings of a few thousand vertices).
#pragma once
#include "geom2d.h"

#include <ray/cam.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ray::cam::link {

using geom2d::Coord;
using geom2d::Path;
using geom2d::Pt;
using geom2d::Regions;
using geom2d::Side;

// ---------- Tolerances (link.cpp, "TOLERANCES", says why each value) ----------
inline constexpr Coord kFreeTolerance = 1'000; // 1 um: a link may graze uncut material by this much
inline constexpr Coord kRoomProbe = 500;       // 0.5 um: the probe radius of a room test
inline constexpr Coord kRoomGrow = 1'000;      // 1 um: the room is grown by this for its test (so 1.5 um tolerance)
inline constexpr double kRampSlack = 1e-4;     // ramps and helices are this much (relative) longer than the angle needs
inline constexpr int kMaxLeadTries = 8;        // lead radii tried: radius, halved each time, down to minRadius
inline constexpr Coord kMinLinkMove = 1'000;   // 1 um: shorter link moves are merged (SHORT MOVES above); a rotatable
                                               // pass's start is never closer than this to a vertex on a line span

struct Pt3 {
    Nm x = 0, y = 0, z = 0;
    auto operator<=>(const Pt3&) const = default;
};

// ---------- Leads ----------
// A lead joins the tool to the pass tangentially at the pass's depth (the lead-in ends at the pass start in the pass's
// start direction; the lead-out leaves the pass end in its end direction). `side`: the side of the direction of travel
// the lead lies on (the side away from the material: Left for a climb contour with an M3 spindle, the tool keeping the
// material on its right).
//   Arc   a tangent arc of `radius` and `sweepDeg` (0, 180], centre on `side` (Left: counter-clockwise); `length` > 0
//         adds a straight part tangent to the arc at its far end (before the arc for a lead-in, after it for a
//         lead-out). A radial lead-in to a circle (thread milling): radius = circle radius / 2, sweep 180, side towards
//         the centre: it starts at the circle's centre.
//   Line  a straight tangent extension of `length`.
// A lead that does not fit the pass's room is tried with its radius halved (Arc; Line: its length halved) down to
// minRadius (0: the radius only), at most kMaxLeadTries times; then the pass is refused.
enum class LeadKind : std::uint8_t { None, Arc, Line };
struct Lead {
    LeadKind kind = LeadKind::None;
    Coord radius = 0;
    double sweepDeg = 90;
    Coord length = 0;
    Side side = Side::Left;
    Coord minRadius = 0;
};

// ---------- Passes ----------
// One continuous cut. The tool is at `start` before moves[0]; the moves are feed moves (Linear, ArcCW/ArcCCW in the XY
// plane, helical ones included) with a positive feed and normally role Cut. Every point lies at or below the feed
// plane. A pass is planar when every move is an XY line or arc at start.z: only planar passes may be reversible,
// rotatable or entered by a ramp.
struct Pass {
    Pt3 start;
    std::vector<Move> moves;
    // Order. Passes are cut in non-decreasing `sequence`; inside one sequence, the passes of one cluster (>= 0) in
    // non-decreasing `level` (a pocket: cluster = the pocket, level = the depth index). cluster -1: no level
    // constraint.
    int sequence = 0;
    int cluster = -1;
    int level = 0;
    bool reversible = false; // planar, every move the same role, feed and no compensation: may be cut end to start
    // Planar and closed (ends at start), uniform as above: may start anywhere on it, except that a start that is not a
    // vertex keeps at least geom2d::kMinArcChord (on an arc; kMinLinkMove on a line) from both ends of the span it
    // splits; a nearer one is moved onto that vertex (the nearer, when the span is too short for both). So a rotated
    // pass never has an arc shorter than an arc fitted or offset by geom2d (a control reads a shorter one as a whole
    // circle or as nothing).
    bool rotatable = false;
    Lead leadIn, leadOut;
    // Where the tool centre may go at this pass's depth without gouging: leads and helices must lie inside it. nullopt:
    // nowhere off the pass itself (leads and helices are refused). unbounded() for a pass with no walls within reach.
    std::optional<Regions> room;
    std::vector<EntryKind> entryOrder; // empty: Options::entryOrder
};
// A pass along a path at height z: geom2d::appendMoves with that role and feed.
Pass passFromPath(const Path& path, Nm z, std::int32_t feedMicronPerMin, MoveRole role = MoveRole::Cut);
// A room with no walls (a square 2 km wide).
Regions unbounded();
// The path of a planar pass (nullopt when it is not planar), and whether its moves are uniform (one role, one feed,
// compensation off): what reversing and rotating need.
std::optional<Path> planarPath(const Pass&);
bool uniformMoves(const Pass&);

// ---------- Material ----------
struct Cleared {
    Nm z = 0;     // free of material at this height and above,
    Regions area; // over this area
};
struct Stock {
    Regions region;               // the material's footprint (empty: no material anywhere, every move is free)
    Nm top = 0;                   // its top
    std::vector<Cleared> cleared; // known free areas (an earlier operation, a drilled hole)
};
// The stock box of the setup: its XY rectangle up to its top (ResolvedGeometry::stockBounds, rounded to nm).
Stock stockOf(const ResolvedGeometry&);

// ---------- Options ----------
struct Feeds {
    // um/min; 0: the cut feed of the pass being entered (its first move's feed).
    std::int32_t plunge = 0; // plunges, pre-drill plunges and feed descents through cut area
    std::int32_t ramp = 0;   // ramps and helices
    std::int32_t leadIn = 0, leadOut = 0;
    std::int32_t link = 0; // stay-down links and feed retracts
};
struct PreDrill {
    Pt at;
    Coord diameter = 0; // the hole; the tool enters when its disc fits the hole
    Nm bottom = 0;      // the hole is at least this deep
};
struct Options {
    // Absolute WCS Z, nm. clearance >= retract >= feedPlane; retract >= Stock::top.
    Nm clearance = 0, retract = 0, feedPlane = 0;
    Coord toolRadius = 0;       // cutting radius, > 0
    bool centreCutting = false; // the tool may plunge straight into material (drills, ball, engraving; not most mills)
    Feeds feeds;
    // Stay down: a link of at most this XY length stays at depth when it lies in free area; 0: always retract.
    Coord maxStayDown = 0;
    Nm stayDownLift = 0; // a stay-down link runs this much above the higher of its two ends (lifted feed link)
    // Step in (offset pocketing): when a link cannot stay down only because the next pass starts beside the cut area
    // (the next ring, one stepover out), the tool may move straight to the pass start at depth, as a Cut move, if
    // that move is no longer than maxStayDown, its centre line lies in cut area, and every bit of material it removes
    // lies in what the pass (with its leads) cuts anyway. So the tool centre never enters material, only its flank
    // does (a radial depth under the tool radius), and nothing outside the pass's own swept area is cut. Entered::Step.
    bool stepIn = false;
    // Added by T-055 (multi-depth slots and other passes cut depth after depth in place): a link that cannot stay down
    // only because the next pass lies deeper in material not cut yet still need not leave the cut. With
    // stayDownEntry, when the next pass's entry (ramp, helix, pre-drill or plunge) starts within maxStayDown of the
    // tool and the straight move there, at the higher of the tool's height and the entry's top, lies in free area
    // (the same test as a stay-down link) and not above the feed plane, the tool feeds up to that height if it must,
    // across, and down to the entry's top (Link moves, the link feed) and enters from there instead of retracting.
    // Off (the default): such a link retracts.
    bool stayDownEntry = false;
    // Added by T-054: with stepIn, the step's centre line may also lie in what the pass itself cuts (with its leads),
    // not only in area already cut: the tool moves sideways into material the pass removes anyway, a cut at most as
    // wide as the tool (a slot, as the first ring of a pocket is). For offset pocketing with a stepover above the tool
    // radius, where the next ring lies farther out than the cut area reaches. The removed-material rule is unchanged.
    bool stepInSlot = false;
    bool rapidRetract = false; // retract from the cut by a rapid straight up; false: feed up to the feed plane first
    // Entry, in this order, the first that fits (a pass's own entryOrder replaces it).
    std::vector<EntryKind> entryOrder{EntryKind::Helix, EntryKind::Ramp, EntryKind::PreDrill};
    // Of the tool centre's circle. helixDiameter 0: no helix. helixMinDiameter 0: helixDiameter is also the minimum.
    // A helix is tried at helixDiameter, then 0.75 times smaller down to the minimum; no helix, and no ramp along an
    // arc, is ever tighter than the minimum.
    Coord helixDiameter = 0, helixMinDiameter = 0;
    double rampAngleDeg = 0; // ramps and helices descend at most this steeply, (0, 90); 0: neither
    Coord minRampRun = 0;    // a zigzag ramp's run (or a closed pass's length) is at least this; 0: the tool diameter
    Nm entryClearance = 0;   // a helix or ramp starts this far above the material it enters
    std::vector<PreDrill> preDrills;
    bool spindleClockwise = true; // the helix turns counter-clockwise (climb) with an M3 spindle, else clockwise first
    // Order.
    bool keepOrder = false; // cut the passes as given (reversible and rotatable are then ignored)
    std::optional<Pt> from; // where the tool comes from (XY), for the first link of the order; nullopt: anywhere
    // The tool's end (added after T-056): how far above its tip the tool reaches toolRadius (THE TOOL'S END above): 0
    // for a flat end, a ball's radius, a bull nose's corner radius, a pointed tool's point length. >= 0.
    Nm tipHeight = 0;
    // Added after M3: what the passes' Move::compensation asks of the control, for the refusals to name it: Control
    // (the control offsets the programmed contour by the tool radius) or Wear (the moves are the tool centre's and the
    // control offsets by the wear only). Linking treats both alike.
    enum class Compensation : std::uint8_t { Control, Wear };
    Compensation compensation = Compensation::Control;
};

// ---------- Result ----------
enum class Entered : std::uint8_t { Free, StayDown, Step, Helix, Ramp, PreDrill, Plunge };
struct Placement {
    std::size_t pass = 0;
    bool reversed = false;
    double startS = 0; // rotatable: the arc length along the pass's path where it starts (0: as given; Pass::rotatable)
};
struct Linked {
    Pt3 start;               // (the first approach point, clearance): Toolpath::startX/Y/Z
    std::vector<Move> moves; // the whole operation; moves[0] is a rapid to `start`
    std::vector<Placement> order;
    std::vector<Entered> entered;      // per placement
    std::vector<Coord> helixDiameter;  // per placement: the helix used (0 if none)
    std::vector<std::size_t> passMove; // per placement: index in `moves` of its lead-in's first move (or its own)
    double rapidLength = 0;            // nm, every rapid
    double linkFeedLength = 0;         // nm, stay-down links and feed retracts
    std::vector<std::string> warnings; // leads made smaller, in order
};
Result<Linked> linkPasses(std::span<const Pass> passes, const Options&, const Stock&, JobContext&);

// ---------- Ordering (linkPasses uses these; exposed for the tests and for operations that order by themselves) ----
// The cost of an order: the rapid length the linked toolpath has when no link stays down (vertical rapids of every
// retract and approach, horizontal rapids at the retract plane, the final retract), with a link that can stay down
// (Options::maxStayDown, same Z, same cluster) costing its XY length instead; plus the distance from Options::from.
// Exactly the linked toolpath's rapid length (less that distance) when no link stays down, the feed plane is at or
// above the stock top, leads keep their nominal size, and every entry starts at the pass's approach point (free,
// plunge, pre-drill, zigzag ramp; a helix or a ramp along a closed pass starts elsewhere).
double travelCost(std::span<const Pass> passes, std::span<const Placement> order, const Options&);
// The best orientation of each reversible pass (and start of each rotatable one) for a fixed order of passes.
std::vector<Placement> orient(std::span<const Pass> passes, std::span<const std::size_t> order, const Options&);
// The order linkPasses uses. Valid: sequences non-decreasing, cluster levels non-decreasing. Polls ctx.cancel.
Result<std::vector<Placement>> orderPasses(std::span<const Pass> passes, const Options&, JobContext&);
// True when `order` respects the sequence and cluster-level constraints.
bool validOrder(std::span<const Pass> passes, std::span<const std::size_t> order);

// ---------- Building blocks (also for operations) ----------
// The tool-centre path of a lead (in: ending at `at` in direction `tangent`; out: starting there), as Lead says.
Path leadPath(Pt at, Vec2 tangent, const Lead& lead, Coord radius, bool in);
// Moves along `path` with Z going linearly with arc length from z0 to z1 (ramps, helices): lines, and XY arcs split as
// geom2d::appendMoves splits them, each end's Z rounded half to even, the last exactly z1.
void appendSloped(const Path& path, Nm z0, Nm z1, MoveRole role, std::int32_t feedMicronPerMin, std::vector<Move>& out);
// The same with no piece steeper than maxSlope (|dz| over its XY length; an arc's length taken at the smaller of its
// end radii): each end's Z is the proportional one rounded half to even unless that would make its piece steeper or
// leave the pieces after it unable to reach z1, then the nearest Z that does neither. Rounding never steepens a piece.
// False, and nothing appended, when the path is too short for it (fewer whole nanometres allowed than |z1 - z0|).
// Linking's ramps and helices use it with the ramp angle (added after T-054 found them up to 1.3e-5 steeper).
bool appendSloped(const Path& path, Nm z0, Nm z1, double maxSlope, MoveRole role, std::int32_t feedMicronPerMin,
                  std::vector<Move>& out);
// The rapid and feed lengths of moves from `from` (nm), for stats.
struct Lengths {
    double rapid = 0, feed = 0;
};
Lengths lengthsOf(std::span<const Move> moves, Pt3 from);

} // namespace ray::cam::link
