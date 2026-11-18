// Private to src/kernel and its tests (task T-004). Not a public interface. Includes no OCCT header, so
// tests/unit/kernel can use it: these functions build shapes "through OCCT directly" for the tests, which
// may not include OCCT headers themselves (PLAN.md 0.4 rule 1).
#pragma once
#include <ray/kernel.h>

#include <cstddef>
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
