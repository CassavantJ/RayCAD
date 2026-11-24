// Private to src/render/rhi: the GPU side of tiled meshes (task T-073, the simulation stock; ../stock.h says how the
// pieces, the pages and the runs fit together). One TilePool per tiled mesh item, owned by its GpuItem.
#pragma once

#include "rhi_util.h"

#include "../stock.h"

#include <rhi/qrhi.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ray::render::rhi {

class TilePool {
public:
    explicit TilePool(ResourceCounter& counter) : counter_(&counter) {}
    TilePool(const TilePool&) = delete;
    TilePool& operator=(const TilePool&) = delete;

    struct SyncStats {
        std::uint64_t bytes = 0;  // vertex and index bytes handed to the GPU
        std::uint64_t pieces = 0; // face ranges uploaded
        bool repacked = false;    // every piece was uploaded again into new pages
    };
    // Brings the GPU copy to `table`: uploads the pieces it does not hold yet (by serial number), frees the slots of
    // those that left. Nothing is done when `table` is the one synced last. A buffer that cannot be created leaves the
    // pool empty (logged); the next sync tries again. `outerBox`: the stock's own box, for packing new pieces
    // (MeshPiece::packed).
    SyncStats sync(QRhi* rhi, const std::shared_ptr<const detail::PieceTable>& table,
                   const std::optional<Box3>& outerBox, QRhiResourceUpdateBatch* u);

    bool empty() const { return slots_.empty(); }
    std::uint64_t triangles() const { return triangles_; }
    std::uint64_t gpuBytes() const;
    // With a pipeline that uses meshVertexLayout() and its shader resources set: draws every triangle, part after part
    // (PackedPiece: every piece's first part, then every piece's second, then every piece's third), each page bound
    // once per part and each run of pieces that lie one after the other one draw. Returns the triangles drawn and adds
    // the draw calls.
    std::uint64_t drawAll(QRhiCommandBuffer* cb, std::uint64_t& drawCalls) const;
    // The same for the piece of one face number (highlights); 0 when there is none.
    std::uint64_t drawFace(QRhiCommandBuffer* cb, std::uint32_t face, std::uint64_t& drawCalls) const;

private:
    static constexpr int kParts = 3; // PackedPiece's parts: inside, on the outer box, the walls inside the stock
    struct Run {
        std::uint32_t firstIndex, indexCount;
    };
    struct Page {
        Owned<QRhiBuffer> vertices;
        Owned<QRhiBuffer> indices[kParts];
        detail::RangeAllocator vertexSpace;        // in vertices
        detail::RangeAllocator indexSpace[kParts]; // in indices
        mutable std::vector<Run> runs[kParts];     // what drawAll draws, rebuilt when runsDirty
        mutable bool runsDirty = true;
    };
    struct Slot {
        std::shared_ptr<const detail::MeshPiece> piece;
        std::size_t page = 0;
        std::uint64_t firstVertex = 0;
        std::uint32_t vertexCount = 0;
        std::uint64_t firstIndex[kParts] = {0, 0, 0};
        std::uint32_t indexCount[kParts] = {0, 0, 0};
    };

    std::uint64_t drawPart(QRhiCommandBuffer* cb, int part, std::uint64_t& drawCalls) const;
    bool place(QRhi* rhi, Slot& slot, const std::uint64_t pending[1 + kParts]);
    void release(const Slot& slot);
    std::uint64_t upload(const Slot& slot, QRhiResourceUpdateBatch* u);
    void rebuildRuns(const Page& page, std::size_t index) const;
    void clear();

    ResourceCounter* counter_;
    std::map<std::uint32_t, Slot> slots_; // by face number
    std::vector<std::unique_ptr<Page>> pages_;
    std::shared_ptr<const detail::PieceTable> synced_;
    std::optional<Box3> outerBox_;
    std::uint64_t triangles_ = 0;
};

} // namespace ray::render::rhi
