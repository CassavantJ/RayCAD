// Private to src/kernel and its tests (task T-004). Not a public interface. Includes no OCCT header, so
// tests/unit/kernel can use it: these functions build shapes "through OCCT directly" for the tests, which
// may not include OCCT headers themselves (PLAN.md 0.4 rule 1).
#pragma once
#include <ray/kernel.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ray::kernel::testing {

// A 100 x 60 x 20 box at the origin with a radius 8 cylinder through it at (50, 30), cut with
// BRepAlgoAPI_Cut directly (not through kernel::boolean).
Result<Shape> makeCutBody();

// A 30 x 50.8 x 20 box with a pocket 8 deep cut by BRepAlgoAPI_Cut directly (not through kernel::boolean) from
// x = 10 out through the x = 30 side, z = 5 to 15, whose ceiling lies `skin` below the top face y = 50.8.
Result<Shape> makeSkinnedPocket(double skin);

// A compound holding the same 10 x 20 x 30 box twice: as built, and moved by (200, 0, 0) through a
// TopLoc_Location on the shared TShape.
Result<Shape> makeLocatedCompound();

// A lone planar 10 x 20 face at z = 5 (no solid).
Result<Shape> makePlanarFace();

// Builders for the tessellation tests (task T-006).
// An 80 x 50 x 20 box with its four vertical edges filleted (radius 5), two radius 6 through holes at
// (20, 25) and (60, 25), and the top rims of both holes filleted (radius 1.5).
Result<Shape> makeFilletedBody();
// A sphere of the given radius centred at `centre`.
Result<Shape> makeSphere(Vec3 centre, double radius);
// A torus about the z axis through the origin.
Result<Shape> makeTorus(double majorRadius, double minorRadius);
// A 10 * cols x 10 * rows x 5 plate with a radius 3 through hole in the middle of every 10 x 10 cell.
Result<Shape> makeHolePlate(int rows, int cols);
// A compound of a 10 x 10 x 10 box and a planar "bow tie" face at z = 50 whose outer wire crosses itself at
// (5, 5, 50). BRepMesh leaves that face without triangles. Its face SubId is the last one (index 6).
Result<Shape> makeBodyWithUnmeshableFace();

// A plate from (0, 0, 0) to `size` with a vertical through hole of `radius` at each (x, y) of `centres`, all cut
// in one BRepAlgoAPI_Cut (task T-007).
Result<Shape> makePlateWithHoles(Vec3 size, double radius, std::span<const Vec2> centres);

// A prism of height `height` over a star polygon with `points` outer corners at radius `outer` alternating with
// inner corners at radius `inner`: 2 * points side faces plus two caps (task T-007, a body with many faces).
Result<Shape> makeStarPrism(int points, double outer, double inner, double height);

// The same solid with the faces of every shell listed in a different order (a permutation chosen by `seed`).
// The faces, edges and vertices are the very same TShapes, so the geometry is bit-identical, but the
// canonical SubId order differs. The input must be a solid (task T-007).
Result<Shape> reorderFaces(const Shape& solid, std::uint64_t seed);

// Does to the shape's TShapes, in place, what later operations do to shared TShapes: meshes it (adds
// triangulations and polygons, sets the Modified flag) and adds it into a compound (clears its Free flag).
// For the canonical-serialisation test only; never call it on a shape another thread can see.
Status meshAndShare(const Shape& shape);

// What OCCT keeps on the shape's TShapes beyond its canonical bytes, one line per TShape in walk order: the
// bookkeeping flags (Free, Modified, Checked, Orientable, Closed, Infinite, Convex, Locked), tolerances, the curve
// representations of every edge in stored order (3D curve, pcurves and the surfaces they lie on, regularity,
// polygons) and the triangulations of every face. Not canonical (it names surfaces by address): it changes when
// only a cache or a flag changes, which serialize() deliberately ignores. For the immutability tests, which
// require that no kernel call changes even that. Empty for a null shape.
std::string rawState(const Shape& shape);

// The faces of `shape` the kernel counts as slivers: narrower than 1e-4 mm (boolean_internal.h kSliverWidth).
Result<std::vector<SubId>> sliverFaces(const Shape& shape);

// Switches off, for the whole process while it lives, the second try with OCCT's fuzzy option that kernel::boolean
// makes when its result has a sliver (boolean.cpp SLIVERS): booleans then keep a skin a rounding error thick, as before
// that rule. For the naming tests that are built on such a skin (a 7.6e-7 mm slab) and check how naming treats it.
// Not for concurrent use with tests that expect the rule; guards may nest.
class SliverMergeOff {
public:
    SliverMergeOff();
    ~SliverMergeOff();
    SliverMergeOff(const SliverMergeOff&) = delete;
    SliverMergeOff& operator=(const SliverMergeOff&) = delete;
};

// How often, with RAYCAD_KERNEL_VALIDATE_BOTH=1 set, the incremental result check (boolean_internal.h INCREMENTAL
// CHECK) and the full BRepCheck disagreed in this process.
std::uint64_t validationDisagreements() noexcept;

// Marks `shape` as having passed validation, as the kernel does for its own results, so that operations on it check
// only the faces they change. For tests that need that path on a body the kernel did not make (an import).
void markValidated(const Shape& shape) noexcept;

// How many faults (access violations and the like) were raised in this process on any thread, whether the kernel's
// guard or OCCT itself (its thread pool's catch (...)) swallowed them (detail::structuredExceptionCount). Always 0
// off Windows: there nothing catches them and the process dies.
std::uint64_t structuredExceptions() noexcept;

// ---------- Import / export tests (task T-005) ----------

// Number of parts makeTestPart() builds.
inline constexpr int kTestPartCount = 20;
// Twenty different valid solids for round-trip tests: primitives, booleans, fillets, chamfers, sweeps,
// B-spline lofts and revolutions, and a transformed body. `index` in [0, kTestPartCount).
Result<Shape> makeTestPart(int index);

// A compound of the six faces of a 10 x 20 x 30 box, each face with its own edges, so that only sewing turns
// them into a closed shell.
Result<Shape> makeLooseBoxFaces();

// Writes an AP214 STEP assembly through XCAF, in inches if `inches` (else millimetres):
//   "Fixture" with components, in this order:
//     "Bracket" (box 0..40 x 0..20 x 0..10 mm, part colour sRGB 0.8 0.1 0.1) at the identity,
//     "Bracket" again, rotated 90 degrees about +Z and moved by (100, 0, 0) mm,
//     "Pin" (cylinder radius 3, height 30 along +Z from the origin, colour sRGB 0.1 0.2 0.9) moved by (10, 10, 10) mm,
//     "Plate" (box 0..100 x 0..50 x 0..5 mm, no part colour, its first face sRGB 0.1 0.7 0.2) moved by (0, 0, -5) mm.
// `path` is UTF-8.
Status writeStepAssembly(const std::string& path, bool inches);

// Writes an IGES file (faces mode, as most CAD systems write) through XCAF with one box 0..40 x 0..20 x 0..10 mm,
// coloured sRGB 0.8 0.1 0.1, in inches if `inches`. `path` is UTF-8.
Status writeIgesColoredBox(const std::string& path, bool inches);

// True if BRepCheck_Analyzer finds the shape valid. KernelFailure for a null shape or if OCCT throws.
Result<bool> isValidShape(const Shape& shape);

// The OCCT payload inside serialize() output; empty if the envelope is not well formed.
std::vector<std::byte> payloadOf(std::span<const std::byte> serialized);
// Wraps a payload in a valid envelope (correct length and checksum), to feed damaged payloads past the
// envelope checks.
std::vector<std::byte> seal(std::span<const std::byte> payload);

} // namespace ray::kernel::testing
