// Private to src/render: the CPU half of the section view (task T-023). No Qt header here, so it is unit-tested
// without a GPU. The renderer (rhi/scene_renderer.cpp) uses it to decide which bodies get a cap and where the cap
// quad lies; the shaders do the clipping.
//
// Cap technique (stencil parity, per body). After the opaque faces are drawn (clipped at the plane), for every
// closed visible opaque body the renderer
//   1. draws the body's clipped faces again into the stencil buffer only: no colour, no depth test, no depth write,
//      both sides with the stencil op Invert on bit 0. Afterwards a pixel has bit 0 set exactly where the view ray
//      meets the plane inside the body: a ray crosses a closed surface an odd number of times between a point
//      inside and the outside, and only the surfaces on the kept side of the plane are counted.
//   2. draws one quad lying in the plane (capQuad, covering the whole scene) with the stencil test "bit 0 set", the
//      depth test and depth writes on, in the cap colour; the stencil op Zero on pass and on depth fail clears the
//      bit again for the next body.
// It needs nothing but a depth-stencil buffer (which every target already has), works per sample with MSAA and on
// every backend (GLSL 120 included), and costs per capped body one extra depth-less draw of its triangles into the
// stencil buffer plus one quad. The parity is only right for watertight meshes, hence isClosedMesh: open shells,
// lone faces and meshes with faces BRepMesh could not triangulate get no cap and so show no artefacts.
#pragma once

#include <ray/kernel.h>
#include <ray/render.h>

#include <array>
#include <optional>

namespace ray::render::detail {

// True when the mesh's triangles form closed surfaces: after welding coincident positions (tolerance 1e-5 of the
// mesh's extent) every triangle edge is shared by an even number of triangles, none by more than two of one face,
// and the kernel reported no face without triangles. O(n log n); the renderer caches it per geometry.
bool isClosedMesh(const kernel::Mesh&);

// The plane as n.x * x + n.y * y + n.z * z + d = 0 with a unit normal; points with a positive value are cut away.
struct PlaneEquation {
    double nx = 0, ny = 0, nz = 1, d = 0;
    double distance(Vec3 p) const { return nx * p.x + ny * p.y + nz * p.z + d; }
};
std::optional<PlaneEquation> planeEquation(const SectionPlane&);

// A square in the plane, centred on the projection of the bounding sphere's centre, whose half size is twice the
// sphere's radius: it covers every point of the plane inside `bounds`. Corners in order around the square. nullopt
// when the plane does not meet the bounding sphere (nothing to cap).
std::optional<std::array<Vec3, 4>> capQuad(const PlaneEquation&, const Box3& bounds);

} // namespace ray::render::detail
