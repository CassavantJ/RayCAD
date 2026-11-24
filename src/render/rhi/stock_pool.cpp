// The GPU side of tiled meshes (task T-073). See stock_pool.h and ../stock.h.
#include "stock_pool.h"

#include <algorithm>
#include <utility>

namespace ray::render::rhi {

namespace {

using detail::kPieceVertexBytes;

// Page sizes, in vertices and in indices of each part. A page holds at least the piece that asks for it; otherwise it
// is sized for what is still waiting to be placed (or a quarter of what the pool holds), within these bounds: 28 MiB
// of vertices and 12 MiB of indices at most, well inside every backend's buffer limits.
constexpr std::uint64_t kMinPage[4] = {16 * 1024, 48 * 1024, 4 * 1024, 4 * 1024};
constexpr std::uint64_t kMaxPage[4] = {1024 * 1024, 3 * 1024 * 1024, 3 * 1024 * 1024, 3 * 1024 * 1024};
// Repack when the pages hold more than twice what is in use and more than this many bytes.
constexpr std::uint64_t kRepackMinBytes = 8ull * 1024 * 1024;

std::uint64_t pageSize(int kind, std::uint64_t wanted, std::uint64_t need)
{
    return std::max(need, std::clamp(wanted, kMinPage[kind], kMaxPage[kind]));
}

} // namespace

TilePool::SyncStats TilePool::sync(QRhi* rhi, const std::shared_ptr<const detail::PieceTable>& table,
                                   const std::optional<Box3>& outerBox, QRhiResourceUpdateBatch* u)
{
    SyncStats stats;
    if (!rhi || table == synced_)
        return stats;
    outerBox_ = outerBox;
    // 1. Compare by face number: slots of faces that left are freed; a face with a new piece keeps its place when the
    //    piece has the same sizes, else it is placed anew. New pieces are packed here (render thread).
    std::vector<std::uint32_t> toPlace, toUpload;
    std::uint64_t pending[1 + kParts] = {0, 0, 0, 0}; // vertices, indices of each part
    static const std::vector<std::shared_ptr<const detail::MeshPiece>> kNone;
    const std::vector<std::shared_ptr<const detail::MeshPiece>>& pieces = table ? table->pieces : kNone;
    const auto sizesOf = [this](const detail::MeshPiece& piece, std::uint32_t out[1 + kParts]) {
        const detail::PackedPiece& data = piece.packed(outerBox_);
        out[0] = piece.vertexCount();
        for (int k = 0; k < kParts; ++k)
            out[1 + k] = data.partIndices[k];
    };
    std::size_t next = 0;
    for (auto it = slots_.begin(); it != slots_.end() || next < pieces.size();) {
        const bool haveSlot = it != slots_.end();
        const bool havePiece = next < pieces.size();
        if (haveSlot && (!havePiece || it->first < pieces[next]->face())) {
            release(it->second);
            it = slots_.erase(it);
            continue;
        }
        const std::shared_ptr<const detail::MeshPiece>& piece = pieces[next++];
        std::uint32_t sizes[1 + kParts];
        if (haveSlot && it->first == piece->face()) {
            Slot& slot = it->second;
            ++it;
            if (slot.piece->serial() == piece->serial())
                continue;
            sizesOf(*piece, sizes);
            bool sameSize = slot.vertexCount == sizes[0];
            for (int k = 0; k < kParts; ++k)
                sameSize = sameSize && slot.indexCount[k] == sizes[1 + k];
            slot.piece = piece;
            toUpload.push_back(piece->face());
            if (sameSize)
                continue;
            release(slot);
        } else {
            sizesOf(*piece, sizes);
            slots_[piece->face()].piece = piece;
            toUpload.push_back(piece->face());
        }
        Slot& slot = slots_[piece->face()];
        slot.vertexCount = sizes[0];
        for (int k = 0; k < kParts; ++k)
            slot.indexCount[k] = sizes[1 + k];
        slot.page = pages_.size(); // not placed yet
        toPlace.push_back(piece->face());
        for (int k = 0; k < 1 + kParts; ++k)
            pending[k] += sizes[k];
    }
    synced_ = table;
    triangles_ = table ? table->triangles : 0;

    // 2. Too much free space (old places of pieces that changed size): every piece into new pages.
    std::uint64_t capacityBytes = 0, used[1 + kParts] = {0, 0, 0, 0}, capacity[1 + kParts] = {0, 0, 0, 0};
    for (const auto& page : pages_) {
        capacityBytes += page->vertices->size();
        for (int k = 0; k < kParts; ++k)
            capacityBytes += page->indices[k]->size();
        used[0] += page->vertexSpace.used();
        capacity[0] += page->vertexSpace.capacity();
        for (int k = 0; k < kParts; ++k) {
            used[1 + k] += page->indexSpace[k].used();
            capacity[1 + k] += page->indexSpace[k].capacity();
        }
    }
    const auto bytesOf = [](const std::uint64_t n[1 + kParts]) {
        std::uint64_t indices = 0;
        for (int k = 0; k < kParts; ++k)
            indices += n[1 + k];
        return n[0] * kPieceVertexBytes + indices * 4;
    };
    std::uint64_t needed[1 + kParts];
    for (int k = 0; k < 1 + kParts; ++k)
        needed[k] = used[k] + pending[k];
    if (capacityBytes > kRepackMinBytes && bytesOf(capacity) > 2 * bytesOf(needed)) {
        clear();
        toPlace.clear();
        toUpload.clear();
        std::fill(std::begin(pending), std::end(pending), 0);
        for (auto& [face, slot] : slots_) {
            slot.page = pages_.size();
            toPlace.push_back(face);
            toUpload.push_back(face);
            pending[0] += slot.vertexCount;
            for (int k = 0; k < kParts; ++k)
                pending[1 + k] += slot.indexCount[k];
        }
        stats.repacked = true;
    }

    // 3. Place and upload.
    for (const std::uint32_t face : toPlace) {
        Slot& slot = slots_[face];
        if (!place(rhi, slot, pending)) {
            ray::log(LogLevel::Error, "render", "a tiled mesh could not get GPU buffers; it is not drawn");
            clear();
            slots_.clear();
            synced_.reset(); // try again at the next sync
            triangles_ = 0;
            return stats;
        }
        pending[0] -= std::min<std::uint64_t>(pending[0], slot.vertexCount);
        for (int k = 0; k < kParts; ++k)
            pending[1 + k] -= std::min<std::uint64_t>(pending[1 + k], slot.indexCount[k]);
    }
    for (const std::uint32_t face : toUpload) {
        stats.bytes += upload(slots_[face], u);
        ++stats.pieces;
    }
    return stats;
}

bool TilePool::place(QRhi* rhi, Slot& slot, const std::uint64_t pending[1 + kParts])
{
    const auto fits = [&slot](const Page& page) {
        if (page.vertexSpace.largestFree() < slot.vertexCount)
            return false;
        for (int k = 0; k < kParts; ++k) {
            if (slot.indexCount[k] > 0 && page.indexSpace[k].largestFree() < slot.indexCount[k])
                return false;
        }
        return true;
    };
    for (std::size_t p = 0; p < pages_.size(); ++p) {
        Page& page = *pages_[p];
        if (!fits(page))
            continue;
        slot.page = p;
        slot.firstVertex = *page.vertexSpace.allocate(slot.vertexCount);
        for (int k = 0; k < kParts; ++k)
            slot.firstIndex[k] = slot.indexCount[k] > 0 ? *page.indexSpace[k].allocate(slot.indexCount[k]) : 0;
        page.runsDirty = true;
        return true;
    }
    std::uint64_t live[1 + kParts] = {0, 0, 0, 0};
    for (const auto& page : pages_) {
        live[0] += page->vertexSpace.used();
        for (int k = 0; k < kParts; ++k)
            live[1 + k] += page->indexSpace[k].used();
    }
    std::uint64_t need[1 + kParts] = {slot.vertexCount, 0, 0, 0};
    for (int k = 0; k < kParts; ++k)
        need[1 + k] = slot.indexCount[k];
    std::uint64_t size[1 + kParts];
    for (int k = 0; k < 1 + kParts; ++k)
        size[k] = pageSize(k, std::max(pending[k], live[k] / 4), need[k]);
    auto page = std::make_unique<Page>();
    page->vertices = own(
        *counter_, rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, quint32(size[0] * kPieceVertexBytes)));
    if (!page->vertices->create())
        return false;
    for (int k = 0; k < kParts; ++k) {
        page->indices[k] =
            own(*counter_, rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::IndexBuffer, quint32(size[1 + k] * 4)));
        if (!page->indices[k]->create())
            return false;
        page->indexSpace[k] = detail::RangeAllocator(size[1 + k]);
    }
    page->vertexSpace = detail::RangeAllocator(size[0]);
    pages_.push_back(std::move(page));
    return place(rhi, slot, pending);
}

void TilePool::release(const Slot& slot)
{
    if (slot.page >= pages_.size())
        return;
    Page& page = *pages_[slot.page];
    page.vertexSpace.free(slot.firstVertex, slot.vertexCount);
    for (int k = 0; k < kParts; ++k)
        page.indexSpace[k].free(slot.firstIndex[k], slot.indexCount[k]);
    page.runsDirty = true;
}

std::uint64_t TilePool::upload(const Slot& slot, QRhiResourceUpdateBatch* u)
{
    const detail::PackedPiece& data = slot.piece->packed(outerBox_);
    Page& page = *pages_[slot.page];
    const quint32 vertexBytes = quint32(data.vertices.size());
    u->uploadStaticBuffer(page.vertices.get(), quint32(slot.firstVertex * kPieceVertexBytes), vertexBytes,
                          data.vertices.data());
    std::uint64_t bytes = vertexBytes;
    // Indices rebased to the piece's place in the page's vertex buffer, each part into its own index buffer.
    const std::uint32_t base = std::uint32_t(slot.firstVertex);
    std::uint32_t from = 0;
    for (int k = 0; k < kParts; ++k) {
        if (slot.indexCount[k] == 0)
            continue;
        std::vector<std::uint32_t> rebased(data.indices.begin() + from,
                                           data.indices.begin() + from + slot.indexCount[k]);
        for (std::uint32_t& i : rebased)
            i += base;
        const quint32 indexBytes = quint32(rebased.size() * 4);
        u->uploadStaticBuffer(page.indices[k].get(), quint32(slot.firstIndex[k] * 4), indexBytes, rebased.data());
        bytes += indexBytes;
        from += slot.indexCount[k];
    }
    return bytes;
}

void TilePool::clear()
{
    pages_.clear();
    for (auto& entry : slots_) {
        entry.second.page = 0; // not placed: pages_ is empty
        entry.second.firstVertex = 0;
        for (std::uint64_t& first : entry.second.firstIndex)
            first = 0;
    }
}

void TilePool::rebuildRuns(const Page& page, std::size_t index) const
{
    for (int k = 0; k < kParts; ++k) {
        std::vector<Run> ranges;
        for (const auto& entry : slots_) {
            const Slot& slot = entry.second;
            if (slot.page == index && slot.indexCount[k] > 0)
                ranges.push_back({std::uint32_t(slot.firstIndex[k]), slot.indexCount[k]});
        }
        std::sort(ranges.begin(), ranges.end(), [](const Run& a, const Run& b) { return a.firstIndex < b.firstIndex; });
        std::vector<Run>& runs = page.runs[k];
        runs.clear();
        for (const Run& r : ranges) {
            if (!runs.empty() && runs.back().firstIndex + runs.back().indexCount == r.firstIndex)
                runs.back().indexCount += r.indexCount;
            else
                runs.push_back(r);
        }
    }
    page.runsDirty = false;
}

std::uint64_t TilePool::gpuBytes() const
{
    std::uint64_t bytes = 0;
    for (const auto& page : pages_) {
        bytes += page->vertices->size();
        for (int k = 0; k < kParts; ++k)
            bytes += page->indices[k]->size();
    }
    return bytes;
}

std::uint64_t TilePool::drawAll(QRhiCommandBuffer* cb, std::uint64_t& drawCalls) const
{
    std::uint64_t triangles = 0;
    for (int k = 0; k < kParts; ++k)
        triangles += drawPart(cb, k, drawCalls);
    return triangles;
}

std::uint64_t TilePool::drawPart(QRhiCommandBuffer* cb, int part, std::uint64_t& drawCalls) const
{
    std::uint64_t triangles = 0;
    if (part < 0 || part >= kParts)
        return 0;
    for (std::size_t p = 0; p < pages_.size(); ++p) {
        const Page& page = *pages_[p];
        if (page.runsDirty)
            rebuildRuns(page, p);
        if (page.runs[part].empty())
            continue;
        const QRhiCommandBuffer::VertexInput vin(page.vertices.get(), 0);
        cb->setVertexInput(0, 1, &vin, page.indices[part].get(), 0, QRhiCommandBuffer::IndexUInt32);
        for (const Run& r : page.runs[part]) {
            cb->drawIndexed(r.indexCount, 1, r.firstIndex);
            triangles += r.indexCount / 3;
            ++drawCalls;
        }
    }
    return triangles;
}

std::uint64_t TilePool::drawFace(QRhiCommandBuffer* cb, std::uint32_t face, std::uint64_t& drawCalls) const
{
    const auto it = slots_.find(face);
    if (it == slots_.end() || it->second.page >= pages_.size())
        return 0;
    const Slot& slot = it->second;
    const Page& page = *pages_[slot.page];
    std::uint64_t triangles = 0;
    for (int k = 0; k < kParts; ++k) {
        if (slot.indexCount[k] == 0)
            continue;
        const QRhiCommandBuffer::VertexInput vin(page.vertices.get(), 0);
        cb->setVertexInput(0, 1, &vin, page.indices[k].get(), 0, QRhiCommandBuffer::IndexUInt32);
        cb->drawIndexed(slot.indexCount[k], 1, quint32(slot.firstIndex[k]));
        ++drawCalls;
        triangles += slot.indexCount[k] / 3;
    }
    return triangles;
}

} // namespace ray::render::rhi
