// Private to src/render: tiled meshes, the simulation stock's display (task T-073). No Qt header here, so the rules
// are unit-tested without a GPU. ray_render/viewport.h ("Simulation stock") says what callers see.
//
// FRONT (UI thread). A tiled mesh item holds a PieceTable: one MeshPiece per face range, sorted by face number. A
// piece refers to the mesh it came from and the range in it; it computes nothing but its bounds and counts on the UI
// thread. updateMesh with a dirty box makes a new table: the old one with the new mesh's ranges put in by face number
// (patchPieces). Tables are immutable once made and shared with the renderer's snapshots, so a frame snapshot costs
// one pointer.
//
// RENDERER (render thread, rhi/stock_pool.h). The renderer keeps, per tiled item, which piece (by serial number) each
// face number has on the GPU and compares that with the table of the snapshot: only pieces it has not seen are
// packed and uploaded. Packing (MeshPiece::packed) builds the GPU vertex and index data once, on the render thread,
// and then lets go of the source mesh, so an old dirty mesh is freed as soon as its pieces are packed (a piece of a
// mesh that is still shown keeps only its own bytes). The packed bytes stay on the CPU for as long as the piece is
// current: the renderer uploads them again when it repacks its buffers or gets a new graphics device.
//
// GPU STORAGE. Pieces are sub-allocated in pages (one vertex buffer and one index buffer per part of PackedPiece) with
// first-fit RangeAllocators; a piece's indices are rebased to its place in the page's vertex buffer when it is uploaded
// (no base-vertex draws, which not every backend has). A page draws a part of its pieces in runs: pieces that lie one
// after the other in that part's index buffer are one draw (the 1536 tiles of the benchmark stock at 0.05 mm: 15 to 23
// draws). Buffers are QRhiBuffer::Static, updated by uploadStaticBuffer with an offset; nothing here needs a feature
// that one of Direct3D 11/12, Metal, OpenGL 3.2 or Vulkan lacks (32-bit indices, as every mesh already uses). When the
// pages hold more than twice what is in use (and over 8 MB) the renderer repacks every piece into new pages.
#pragma once

#include <ray/kernel.h>
#include <ray/render.h>
#include <ray_render/viewport.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ray::render::detail {

constexpr std::uint32_t kPieceVertexBytes = 28; // position, normal, face id: the layout of mesh.vert (scene_renderer)

// The GPU data of one piece: vertices in the mesh layout (face id = face number + 1 in the low three bytes), and
// indices into them from 0 in three parts. The triangles lying in one of the vertical planes of the piece's bounds (x
// or y at the bounds' minimum or maximum, all three corners exactly) go to the second part when that plane is a side
// of the outer box the renderer gives (the stock's own sides) and to the third otherwise; those of the third part are
// the walls that close a stock's tile inside the material, with the real walls that happen to fall on a tile edge,
// and their vertices carry 1 in the face id's high byte. The renderer draws every piece's first part, then every
// piece's second, then every piece's third, each part in draws of its own: an exact depth tie between a wall inside
// the material and the outer face it meets goes to the outer face, and mesh.vert pushes the marked vertices half a
// pixel's depth further back ("DEPTH" below). (Walls and the faces they meet in ONE draw were seen to let the walls
// through at single MSAA samples, varying from run to run, on an NVIDIA adapter under Direct3D 11; in separate draws
// they never were.) The real walls in the third part lose only to what lies that close behind them.
struct PackedPiece {
    std::vector<std::byte> vertices;
    std::vector<std::uint32_t> indices;
    // Indices of each part, in this order: inside the piece; in an edge plane that is also a side of the outer box
    // (the stock's own sides, real outer walls); in an edge plane inside the stock (a tile's walls, which only the
    // last part's vertices carry the mark for).
    std::uint32_t partIndices[3] = {0, 0, 0};
};

// One face range of a tiled mesh item.
class MeshPiece {
public:
    // `range` is an index into source->faces; the source must be valid (validateMesh) and the range non-empty.
    MeshPiece(std::shared_ptr<const kernel::Mesh> source, std::uint32_t range);
    MeshPiece(const MeshPiece&) = delete;
    MeshPiece& operator=(const MeshPiece&) = delete;

    std::uint64_t serial() const { return serial_; } // unique in the process: tells pieces apart for the renderer
    std::uint32_t face() const { return face_; }     // the face number (FaceRange::face.index)
    std::uint32_t triangles() const { return triangles_; }
    std::uint32_t vertexCount() const { return vertices_; } // vertices it uploads
    std::uint64_t vertexBytes() const { return std::uint64_t(vertices_) * kPieceVertexBytes; }
    std::uint64_t indexBytes() const { return std::uint64_t(triangles_) * 12; }
    const Box3& bounds() const { return bounds_; }
    // Packs on first use (render thread) and releases the source mesh. Thread-safe. `outerBox`: the stock's own box
    // (PackedPiece); read on the first call only.
    const PackedPiece& packed(const std::optional<Box3>& outerBox = std::nullopt) const;

private:
    std::uint64_t serial_ = 0;
    std::uint32_t face_ = 0, triangles_ = 0, vertices_ = 0;
    std::uint32_t firstVertex_ = 0; // dense pieces: their vertices are the source's [firstVertex_, + vertices_)
    bool dense_ = true;
    Box3 bounds_{};
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const kernel::Mesh> source_;
    mutable std::uint32_t range_ = 0;
    mutable PackedPiece packed_;
};

struct PieceTable {
    // Sorted by face number, one piece per face number.
    std::vector<std::shared_ptr<const MeshPiece>> pieces;
    std::uint64_t triangles = 0;
    std::optional<Box3> bounds; // of every piece; nullopt when empty
};

// A table of every non-empty face range of `mesh` (a later range with the same face number replaces an earlier one).
std::shared_ptr<const PieceTable> piecesOf(const std::shared_ptr<const kernel::Mesh>& mesh);
// `old` with the face ranges of `mesh` put in by face number (an empty range removes the face) and, with a dirty box,
// the faces `mesh` does not name whose bounds lie inside `dirty` removed. `old` may be null (an empty table).
std::shared_ptr<const PieceTable> patchPieces(const std::shared_ptr<const PieceTable>& old,
                                              const std::shared_ptr<const kernel::Mesh>& mesh,
                                              const std::optional<Box3>& dirty);

// First-fit allocation of [offset, offset + size) ranges in a buffer of fixed capacity, merging neighbours when
// freed. Units are whatever the caller counts in (vertices, indices).
class RangeAllocator {
public:
    explicit RangeAllocator(std::uint64_t capacity = 0);
    std::optional<std::uint64_t> allocate(std::uint64_t size); // nullopt when no free range is large enough
    void free(std::uint64_t offset, std::uint64_t size);
    std::uint64_t capacity() const { return capacity_; }
    std::uint64_t used() const { return used_; }
    std::uint64_t largestFree() const;
    std::size_t freeRanges() const { return free_.size(); }

private:
    std::uint64_t capacity_ = 0, used_ = 0;
    std::map<std::uint64_t, std::uint64_t> free_; // offset -> size, disjoint and never adjacent
};

// How the stock look reaches the shaders (mesh.vert, mesh.frag): the uncut box with its tolerance, the marks, and the
// depth push. Pure data, made per frame from StockShading and the item's bounds.
//
// DEPTH. Ordinary faces are drawn with the pipeline's depth bias (2 units plus 1.5 times the face's own depth slope),
// so their edges stay on top. On the stock that bias would differ between the walls that close a tile inside the
// material and the top or side face they meet (their slopes differ), and a wall would win the depth test along the
// line where they meet: a line along every tile seam. So the stock is drawn WITHOUT pipeline bias, and mesh.vert
// pushes each of its vertices back by the same amount whichever face it belongs to: 1.5 times the steepest depth
// slope of the three axis planes through the point (every face of the column mesh lies in one), each at most four
// times that of a plane at 45 degrees to the view, plus 2 units, and never more than uncutMax.w mm along the view ray
// (the renderer's far plane leaves room for it). Faces that meet share the push along their common line, so the
// depth test there compares the true geometry, and the inside walls, which lie behind, lose. What rounding leaves of
// ties is settled by the walls inside the stock (PackedPiece's third part): drawn last, and pushed back by half the
// depth slope of a plane at 45 degrees more (the depth buffer's own precision, which is poor close up, is in that
// unit too). A line
// drawn on the stock is at least as far in front of it as of any other face (the push is at least the face's own
// slope bias).
struct StockUniforms {
    float stock[4] = {0, 0, 0, 0};    // x: 1 for the stock colours; y: number of marks; z: tolerance in mm; w: 1 for
                                      // a stock's surface (depth push, section tolerance), also in its highlights
    float uncutMin[4] = {0, 0, 0, 0}; // xyz: the uncut box, else the bounds; w: 1 when there is an uncut box
    float uncutMax[4] = {0, 0, 0, 0}; // xyz; w: the largest depth push, mm along the view ray
    float markMin[kMaxStockMarks][4] = {}; // xyz; w: colour slot (StockMarkSlot)
    float markMax[kMaxStockMarks][4] = {};
};
// The simulation tokens a mark can take, in the order of FrameBlock::simColor (scene_renderer.cpp).
enum class StockMarkSlot : std::uint8_t { Collision, Gouge, Leftover, Count };
StockMarkSlot stockMarkSlot(Token);
Token stockMarkToken(StockMarkSlot);
// `bounds`: the item's bounds (for the largest push, and the tolerance when there is no uncut box).
StockUniforms stockUniforms(const StockShading&, const std::optional<Box3>& bounds);
// The largest depth push of a stock with these bounds, in mm: a tenth of its diagonal (so at most a fifth of the radius
// of anything that contains it; SceneRenderer moves its far plane back by a quarter of the scene's radius).
double stockMaxPushMm(const std::optional<Box3>& bounds);

} // namespace ray::render::detail
