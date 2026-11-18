// Private to src/kernel and its tests (task T-004). Not a public interface. Includes no OCCT header, so
// tests/unit/kernel can use it: these functions build shapes "through OCCT directly" for the tests, which
// may not include OCCT headers themselves (PLAN.md 0.4 rule 1).
#pragma once
#include <ray/kernel.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ray::kernel::testing {

// A 100 x 60 x 20 box at the origin with a radius 8 cylinder through it at (50, 30), cut with
// BRepAlgoAPI_Cut directly (not through kernel::boolean).
Result<Shape> makeCutBody();

// A compound holding the same 10 x 20 x 30 box twice: as built, and moved by (200, 0, 0) through a
// TopLoc_Location on the shared TShape.
Result<Shape> makeLocatedCompound();

// A lone planar 10 x 20 face at z = 5 (no solid).
Result<Shape> makePlanarFace();

// Does to the shape's TShapes, in place, what later operations do to shared TShapes: meshes it (adds
// triangulations and polygons, sets the Modified flag) and adds it into a compound (clears its Free flag).
// For the canonical-serialisation test only; never call it on a shape another thread can see.
Status meshAndShare(const Shape& shape);

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
