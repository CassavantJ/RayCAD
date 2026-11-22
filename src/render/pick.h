// Private to src/render: the CPU half of picking (task T-015). The RHI pass (rhi/pick_pass.cpp) renders an ID buffer
// and reads back a small window of it around the cursor; this file turns that window into PickHits. No Qt header
// here, so the search rules are unit-tested without a GPU.
//
// ID buffer, one pixel: which item (ItemId), what kind of element (face, mesh edge, segment of an addLines item), its
// index, and the depth there. The renderer draws faces pushed back with the same depth bias as the main pass, so a
// visible edge wins over the faces it borders.
//
// Search rules for pick(x, y): inside a tolerance of 6 logical pixels (scaled by the device pixel ratio) the priority
// is vertex > edge > face > body among the kinds the filter enables; faces and bodies need the cursor pixel itself
// (a face hidden under a 1 px edge line is taken from the nearest face pixel within 1.5 px). Vertices are the end
// points of the edge polylines, welded per item; kernel::Mesh carries no vertex ids, so a vertex hit names the end of
// an edge (vertexCode in ray_render/viewport.h).
#pragma once

#include "section.h"

#include <ray/kernel.h>
#include <ray/render.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ray::render::detail {

// Lookup tables of one displayed mesh, built when it is uploaded.
struct MeshIndex {
    // Position in Mesh::faces / Mesh::edges of a kernel face / edge index; -1 when the mesh has none.
    std::vector<std::int32_t> faceByKernel, edgeByKernel;
    // Per position in Mesh::edges: first segment in the renderer's segment buffers, and how many.
    std::vector<std::uint32_t> edgeFirstSegment, edgeSegmentCount;
    // Edge end points welded into vertices. `code` is vertexCode of the smallest (edge, end) that lands on it, so
    // every edge that ends there names the vertex the same way.
    struct Vertex {
        std::uint32_t code = 0;
        float pos[3] = {0, 0, 0};
        std::vector<std::uint32_t> edges; // kernel indices of the edges that end here, sorted
    };
    std::vector<Vertex> vertices;
    std::vector<std::int32_t> endVertex; // per (position in Mesh::edges) * 2 + end: index into vertices, or -1

    // Position in Mesh::faces / Mesh::edges of a kernel index, or -1.
    std::int32_t facePosition(std::uint32_t kernelIndex) const
    {
        return kernelIndex < faceByKernel.size() ? faceByKernel[kernelIndex] : -1;
    }
    std::int32_t edgePosition(std::uint32_t kernelIndex) const
    {
        return kernelIndex < edgeByKernel.size() ? edgeByKernel[kernelIndex] : -1;
    }
    // The vertex a vertexCode names (any end of any edge that meets there), or null.
    const Vertex* vertexOfCode(std::uint32_t code) const;
};
std::shared_ptr<const MeshIndex> buildMeshIndex(const kernel::Mesh&);

// What the ID buffer stores per pixel.
// Cap (T-023): a section cap, which belongs to its body but is no kernel element. It answers only PickFilter::Bodies
// and hides the faces behind it.
// Path (T-038): a backplot segment; `index` is its move index (all 32 bits). It answers only PickFilter::Toolpath, as
// kind Edge with subIndex and moveIndex the move index; for faces under it, it counts as a line drawn over them.
enum class IdKind : std::uint8_t { None = 0, Face = 1, Edge = 2, Segment = 3, Cap = 4, Path = 5 };
struct IdPixel {
    ItemId item = 0;
    IdKind kind = IdKind::None;
    std::uint32_t index = 0; // kernel face or edge index, segment index of an addLines item, or move index (Path)
    double depth = 1.0;      // 0..1: 0.5 * NDC z + 0.5 of the backend's clip space
};

// A window of the ID buffer in top-left-origin pixels of the render target.
struct IdWindow {
    int x0 = 0, y0 = 0, width = 0, height = 0;
    std::vector<IdPixel> pixels; // rows top to bottom
    const IdPixel* at(int x, int y) const
    {
        if (x < x0 || y < y0 || x >= x0 + width || y >= y0 + height)
            return nullptr;
        return &pixels[std::size_t(y - y0) * std::size_t(width) + std::size_t(x - x0)];
    }
};
// Decodes the three RGBA8 attachments of the ID pass (ids, item, depth), each `width * height * 4` bytes with rows
// in the order given; `bottomUp` when the first row is the bottom one (OpenGL).
IdWindow decodeIdWindow(int x0, int y0, int width, int height, const std::uint8_t* ids, const std::uint8_t* items,
                        const std::uint8_t* depths, bool bottomUp);

// The scene as the ID buffer was rendered: camera matrices and the items in it. Immutable, shared by read-backs.
struct PickItem {
    ItemId id = 0;
    bool lines = false; // an addLines item: segments, no faces, no vertices
    Xform xform = Xform::identity();
    std::shared_ptr<const kernel::Mesh> mesh;
    std::shared_ptr<const MeshIndex> index;
};
struct PickView {
    int width = 0, height = 0; // render target, physical pixels
    double viewProj[16] = {};  // column-major, includes the backend's clip-space correction
    double invViewProj[16] = {};
    bool yUpInNdc = true;
    std::vector<PickItem> items; // sorted by id
    // T-023: vertices beyond the section plane (positive distance) are not pickable.
    std::optional<PlaneEquation> section;
    const PickItem* item(ItemId) const;
    // World point at the centre of pixel (x, y) with ID-buffer depth `depth`.
    Vec3 unproject(int x, int y, double depth) const;
    // Pixel position (pixel centres at +0.5) of a world point; nullopt behind the eye.
    std::optional<Vec2> project(Vec3 world) const;
};
// m is column-major; false when it is singular.
bool invert4x4(const double m[16], double out[16]);

// Requests in physical pixels. `radius` is the edge and vertex tolerance.
std::optional<PickHit> resolvePick(const PickView&, const IdWindow&, int x, int y, double radius,
                                   std::uint8_t filterMask);
// Every element of an enabled kind visible in the inclusive rectangle, sorted by item, kind and index.
std::vector<PickHit> resolvePickRect(const PickView&, const IdWindow&, int x0, int y0, int x1, int y1,
                                     std::uint8_t filterMask);

// World position of a mesh point (float xyz) under the item's transform.
Vec3 transformPoint(const Xform&, const float* xyz);

} // namespace ray::render::detail
