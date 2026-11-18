// Private to src/kernel and its tests (task T-004). Not a public interface. Includes no OCCT header, so
// tests/unit/kernel can use it: these functions build shapes "through OCCT directly" for the tests, which
// may not include OCCT headers themselves (PLAN.md 0.4 rule 1).
#pragma once
#include <ray/kernel.h>

#include <cstddef>
#include <cstdint>
#include <span>
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

// The OCCT payload inside serialize() output; empty if the envelope is not well formed.
std::vector<std::byte> payloadOf(std::span<const std::byte> serialized);
// Wraps a payload in a valid envelope (correct length and checksum), to feed damaged payloads past the
// envelope checks.
std::vector<std::byte> seal(std::span<const std::byte> payload);

} // namespace ray::kernel::testing
