# Toolpath invariant checker

An independent checker for every toolpath an operation type produces (task T-058; PLAN.md section 11, "Toolpath
invariants" and "Property tests"). It shares no code with the generators: nothing of src/cam's geometry (geom2d,
link, the operations' internals), not Clipper2, not src/sim or src/post. It reads cam.h's types, meshes the part and
fixture shapes with `kernel::triangulateExact` (the only kernel call it makes) and does all its geometry on its own
grids. The property harness additionally builds its random parts with the kernel and finds operation types through
`registerBuiltinOperationTypes`.

| File | What |
|-|-|
| `checker.h`, `checker.cpp` | The one entry point `check(CheckInput)`: claims, findings, the move rules and the geometric pass |
| `cycle_invariants.h`, `toolpath_invariants.h` | The M2 rules (T-035), unchanged; `check` runs them |
| `tool_profile.*` | The tool as a solid of revolution: radius at each height, in zones (flutes, shank, holder) |
| `part_model.*` | The part mesh on a grid with conservative Z-sets per square, refined on demand |
| `stock_grid.*` | The in-process stock as Z intervals on point samples, cut move by move |
| `motion.*` | The toolpath as pieces (lines, XY arcs, chords of XZ/YZ arcs, expanded canned cycles) |
| `property.*`, `mini_json.h` | The seeded property harness |
| `reference_paths.*` | Hand-written reference generators (zig-zag facing, contour, pocket) used by the tests |

## Invariants

Each finding carries the invariant's id, the move index (`moves.size()` for the toolpath as a whole), a WCS position,
the measured value and the allowed one, and a one-line message. The bounds are for the default `CheckOptions`
(part grid 0.25 mm refined down to 0.001 mm, mesh chord 0.001 mm, stock grid max(0.1 mm, the stock's longer side /
1500)).

| Id | What breaks it | Error bound |
|-|-|-|
| `pad`, `feed`, `levels`, `return`, `peck`, `peckClear`, `dwell`, `leftHand`, `pitch`, `tapFeed`, `runStart` | The canned-cycle fields (cycle_invariants.h) | Exact (integer IR) |
| `rapidFeed`, `feedMove` | A rapid with a feed; a linear move or arc without a positive feed | Exact |
| `rapidLow` | A rapid over the stock box below `Claims::rapidFloorZ` (default the stock top); a retract straight up is not one | Exact |
| `belowFloor` | Motion below `Claims::floorZ` | Exact |
| `travel` | An end point, or the bulge of an XY arc, outside the machine's X, Y, Z travel (tip position + `wcsInMachine`) | Exact |
| `enum` | A field outside its enumeration | Exact |
| `spindle` | Cutting with rpm 0, outside the machine's rpm range, or counter-clockwise (every library tool is right hand; skipped for toolpaths with tap cycles, whose direction is the cycle's) | Exact |
| `arc` | Start and end radius differ by more than `arcRadiusTolNm` (2 nm); radius under 1 um; ends closer than 1 um but not equal | Exact (nm) |
| `compensation` | Compensation switched on or off on anything but a linear move; on during a cycle; still on at the end; on the side opposite to the requested climb/conventional | Exact |
| `continuity` | A Dwell or CycleCancel that does not carry the tool's position | Exact |
| `leadTangent` | The last lead-in move meets the cut, or the cut meets the first lead-out, at more than 0.1 degree in XY | Exact (double) |
| `heights` | Claimed heights out of order (clearance >= retract >= feed plane >= top >= bottom); the toolpath starting or ending below the clearance | Exact |
| `containment` | The tool's centre on a feed move outside `Claims::containment` grown by its tolerance | Sampled every min(tolerance, 0.05 mm) along the path, exact at ends |
| `gouge` | The flutes inside the part grown by the stock to leave (XY `stockToLeaveWall`, Z `stockToLeaveFloor`) deeper than `tolerance` | See "Part contacts" |
| `shank` | The tool above its flutes inside the part deeper than `tolerance` | See "Part contacts" |
| `holder` | The holder inside the part or the stock grown by `holderClearance` | Part: see "Part contacts"; stock: see "Stock contacts" |
| `rapidPart` | A rapid's tool (any zone) inside the part grown by `rapidClearance` deeper than `tolerance` | See "Part contacts" |
| `rapidStock` | A rapid's tool inside remaining stock | See "Stock contacts" |
| `fluteLength` | Remaining stock inside the tool above its flutes during a cut (engagement deeper than the flutes) | See "Stock contacts" |
| `plunge` | A straight plunge into stock (stock within `deadZoneFraction` of the radius, 0.25, of the axis) with a tool that does not cut at its centre (`Claims::centreCutting`, default by type: drills, spot drills, ball, chamfer and engraving tools do) | Stock samples |
| `rampAngle` | A ramp or helix into stock steeper than `maxRampAngleDeg` | Exact angle; "into stock" by samples |
| `helix` | A helix into stock narrower than `helixMinDiameter` | Exact diameter; "into stock" by samples |
| `direction` | A cut along a part wall (within cutting radius + stock to leave + 0.5 mm, one side only) climb where conventional was asked, or the reverse (clockwise spindle: climb keeps the wall on the right) | Checked at each cut piece's middle on the 0.25 mm part grid |
| `engagement` | The arc of the cutter's circumference in contact with stock above `maxEngagementDeg` (adaptive) | About +-(1 degree + 2 asin(stock cell / (sqrt 2 R))): 3.7 degrees for a 6 mm tool |
| `coverage` | A reachable sample of a claimed region (`Claims::clears`) keeping stock more than `allowedAbove` above the floor (+ stock to leave) | Per stock sample; see "Coverage" |
| `stockLeft` | A reachable sample farther than stock to leave + bound (at least 2 stock cells) from the claimed walls (`Claims::walls`) keeping stock between their Z limits; also a ClearClaim given this id (a finished floor) | Per stock sample |
| `mesh` | The part does not mesh | - |
| `determinism` (harness) | The same input generated twice gives different toolpath bytes | Exact |

### Part contacts

Every square of the part grid gets a lower and an upper bound of the largest penetration of the protected material
into the tool zone (part_model.h explains why the square's Z-set is a superset of its material and why every height in
it holds material; checker.h how penetration is measured). Upper bound at most the allowed value: no contact. Lower
bound above it: a contact. Otherwise the square is split in four, down to 0.001 mm; a square still undecided there is
reported. So, against the real part:

- a **pass** means the true penetration is at most allowed + 0.0011 mm (mesh chord 0.001, chords of XZ/YZ arcs 0.0001);
- a **fail** means it is more than allowed - 0.0025 mm (0.001 sqrt 2 + 0.001 + 0.0001).

`checker_geometry_test.cpp` tries both sides at a 0.010 mm tolerance: 7 um into a flat wall, a floor and a meshed
curved wall pass; 11.5 um into each fails. Penetration is the smaller of the horizontal and the vertical depth inside
the tool: the normal depth on a floor or a vertical wall, up to 1/sin(angle) times it on a ball or chamfer flank (so a
sloped contact reads deeper than it is, never shallower).

### Stock contacts

The stock is point samples on a square grid (0.1 mm for parts up to 150 mm). Each sample is exact for its vertical
line; what lies between samples is not seen. Every report is real at a sample point. A contact deeper than the cell
times sqrt 2 (0.141 mm) cannot be missed (`checker_geometry_test.cpp` checks 0.146 mm into the shank is found); a
shallower one may be. Stock removal never removes what the tool did not cut (each sample loses what one real tool
position, or the union over a vertical move, sweeps through it), so remaining stock is never under-stated.

### Coverage

A sample counts when a disc of the cutting radius can reach it: its centre at least cutting radius + stock to leave +
one cell from every sample holding part material above the floor, the sample within cutting radius - one cell of such
a centre. So the samples within about two cells of a wall or an inside corner the tool cannot enter are not
required; the rest must be clear. An uncut sliver narrower than a cell can lie between samples.

## What the checker cannot see

Said plainly:

- **Between stock samples.** Contacts with remaining stock shallower than 0.141 mm (at the default 0.1 mm cell) can be
  missed; uncut slivers narrower than 0.1 mm can be missed; part features thinner than a cell can be misjudged when
  deciding what the tool can reach.
- **The true surface beyond the mesh.** The part is its mesh at 1 um chord; up to 1 um of contact on a curved convex
  face is invisible.
- **In-process stock from earlier operations.** The stock is the stock box minus the part (or, `InitialStock::Part`,
  only the allowed cuts); `ResolvedGeometry::stockSlices` is not read. An operation that machines only the rest left
  by an earlier one shows contacts with stock that is already gone.
- **The tool beyond what libraries.h says.** The neck below the shoulder is min(cutting radius, shank radius) (as the
  tool library draws it); a thread mill's neck is taken as R - 0.6134 pitch; above the holder's gauge line the widest
  holder segment carries on upward (the spindle nose is not described anywhere); without a holder the shank carries on
  upward and no holder is checked; stick-out 0 means the overall length.
- **Cutter compensation in the control.** Coordinates are taken as the tool's centre (wear-mode compensation). A
  toolpath programmed on the part contour with the full radius in the control would read as gouging.
- **The machine beyond its axis limits.** Travel is the tip position plus `wcsInMachine`; the tool length is not added
  to Z. Acceleration, feed limits, rapid behaviour (dog-leg rapids) and the tool changer are not modelled.
- **Direction on stock.** Climb and conventional are judged against part walls only, at the middle of each cut piece;
  slotting (walls on both sides) and cuts along remaining stock are not judged.
- **Fixtures** are protected like the part: a fixture contact is reported as `gouge`, `shank`, `holder` or
  `rapidPart`.
- **Quality beyond these rules.** Feeds (beyond being positive), coolant, chip load, surface finish, cycle time and
  air cutting are not checked; nor 4- and 5-axis motion.
- **Determinism across processes and platforms.** The harness compares two generations in one process; the goldens
  (tests/golden/toolpaths) and the CI's three OS jobs cover the rest.

## Plugging in an operation type (T-052 to T-056, T-061)

Add `property_<type>_test.cpp` to this folder and to the `ray_cam_invariants_tests` list in `CMakeLists.txt`:

```cpp
#include "property.h"
using namespace ray::cam::invariants;
static PropertyPlugin plugin() { PropertyPlugin p; p.type = "pocket2d"; p.tools = {ray::lib::ToolType::FlatEndMill, ray::lib::ToolType::BullEndMill}; p.needs = {Feature::Kind::Pocket}; p.geometry = pick::pocketFloors;
    p.claims = [](const PropertyCase& c) { Claims k = defaultClaims(c); k.clears = coverFeatures(c, {Feature::Kind::Pocket}, 0.01); return k; }; return p; }
RAY_INVARIANT_PROPERTY_TESTS("pocket2d", plugin())
```

That defines the quick test (12 fixed seeds, quick tier) and the slow one (200 seeds, `[.slow]`, ctest
`ray_cam_invariants_slow` in the slow tier). What to set:

- `type`: the registered name. `tools`: the tool types the case may draw. `needs`: feature kinds every random part
  must have (`Pocket`, `Slot`, `Holes`, `Step` (an open edge), `Boss`). `geometry`: the faces the operation is given
  (`pick::holes`, `pocketFloors`, `topFaces`, `outerWalls`, `none`, or your own lambda over `PropertyCase`).
- Parameters are drawn from your schema (enums, booleans, numbers from minimum to max(2 x default, minimum + 1)).
  `p.ranges = {{"stepover", {0.2, 0.6}}}` narrows a number, `p.fixed = {{"direction", "\"climb\""}}` fixes a value.
- `p.adjust = [](PropertyCase& c) { ... }` sets what the schema cannot draw well: heights (`c.op.heights`, whose meaning
  is your type's), a tool that fits the feature (`randomTool(c.rng, type, diameter)`), `c.op.paramsJson`.
- `p.claims` says what your operation promises, starting from `defaultClaims(c)`: `clears` (coverage, with
  `coverFeatures`), `walls` (finished walls), `stockToLeaveWall/Floor`, `direction`, `maxRampAngleDeg`,
  `helixMinDiameter`, `maxEngagementDeg`, `heights` (absolute), `containment`, `allowedCuts` (where cutting the model
  is intended: threads, chamfers the model lacks), `initialStock`. An empty claim is not checked.
- A refusal (generate returning an error) is a valid outcome and is listed; at least `minGenerated` (0.5) of the cases
  must produce a toolpath. A failure prints the seed, the part, the tool, the parameters and every finding; rerun one
  case by putting its seed in a test of its own with `runProperty(plugin(), {seed})`.

`property_drill_test.cpp` is the drilling plug-in; `property_harness_test.cpp` plugs in a reference facing type and
shows broken types being reported.

## Resolution and time

Part grid 0.25 mm, refined to 0.001 mm only where a contact is undecided (along walls the tool finishes); stock grid
0.1 mm; mesh chord 0.001 mm. `RAY_INVARIANTS_TIMING=1` prints where the time goes. Measured times are in the task
report (T-058).
