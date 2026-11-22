// The GPU side of toolpath backplots (task T-038). See path_lines.h and ../lines.h.
#include "path_lines.h"

#include "scene_renderer.h" // GpuItem

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ray::render::rhi {

namespace {

constexpr quint32 kVertexBytes = sizeof(sim::BackplotVertex); // 24
// Byte offsets inside a BackplotVertex (sim.h).
constexpr quint32 kRoleKindOffset = 12; // role, kind, pad (u16)
constexpr quint32 kMoveIndexOffset = 20;
static_assert(offsetof(sim::BackplotVertex, role) == kRoleKindOffset);
static_assert(offsetof(sim::BackplotVertex, kind) == kRoleKindOffset + 1);
static_assert(offsetof(sim::BackplotVertex, moveIndex) == kMoveIndexOffset);
constexpr double kPi = 3.14159265358979323846;

QRhiVertexInputLayout layout(bool pick)
{
    QRhiVertexInputLayout l;
    l.setBindings({QRhiVertexInputBinding(2 * sizeof(float)),
                   QRhiVertexInputBinding(kVertexBytes, QRhiVertexInputBinding::PerInstance),
                   QRhiVertexInputBinding(kVertexBytes, QRhiVertexInputBinding::PerInstance)});
    if (pick) {
        l.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                         QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float3, 0),
                         QRhiVertexInputAttribute(2, 2, QRhiVertexInputAttribute::Float3, 0),
                         QRhiVertexInputAttribute(2, 4, QRhiVertexInputAttribute::UNormByte4, kMoveIndexOffset)});
    } else {
        l.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                         QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float3, 0),
                         QRhiVertexInputAttribute(2, 2, QRhiVertexInputAttribute::Float3, 0),
                         QRhiVertexInputAttribute(2, 3, QRhiVertexInputAttribute::UNormByte4, kRoleKindOffset)});
    }
    return l;
}

// A strip of `vertexCount` vertices as chunks of at most kPathChunkSegments segments, each repeating the last vertex
// of the one before. Returns false (and leaves `out` empty) when a buffer could not be created.
bool uploadStrip(QRhi* rhi, ResourceCounter& counter, const sim::BackplotVertex* vertices, std::uint64_t vertexCount,
                 std::vector<PathChunk>& out, QRhiResourceUpdateBatch* u)
{
    out.clear();
    const std::uint64_t segments = vertexCount > 1 ? vertexCount - 1 : 0;
    for (std::uint64_t first = 0; first < segments; first += detail::kPathChunkSegments) {
        PathChunk chunk;
        chunk.firstSegment = first;
        chunk.segments = std::uint32_t(std::min(detail::kPathChunkSegments, segments - first));
        const quint32 bytes = (chunk.segments + 1) * kVertexBytes;
        chunk.buffer = own(counter, rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, bytes));
        if (!chunk.buffer->create()) {
            out.clear();
            return false;
        }
        u->uploadStaticBuffer(chunk.buffer.get(), 0, bytes, vertices + first);
        out.push_back(std::move(chunk));
    }
    return true;
}

// Draws segments [range) of a chunked strip.
std::uint64_t drawChunks(QRhiCommandBuffer* cb, QRhiBuffer* corners, const std::vector<PathChunk>& chunks,
                         detail::SegmentRange range, std::uint64_t& drawCalls)
{
    std::uint64_t drawn = 0;
    if (!corners || range.empty())
        return 0;
    for (const PathChunk& chunk : chunks) {
        const std::uint64_t lo = std::max(range.first, chunk.firstSegment);
        const std::uint64_t hi = std::min(range.end, chunk.firstSegment + chunk.segments);
        if (lo >= hi)
            continue;
        const quint32 local = quint32(lo - chunk.firstSegment);
        const QRhiCommandBuffer::VertexInput inputs[3] = {
            {corners, 0}, {chunk.buffer.get(), local * kVertexBytes}, {chunk.buffer.get(), (local + 1) * kVertexBytes}};
        cb->setVertexInput(0, 3, inputs);
        cb->draw(4, quint32(hi - lo));
        drawn += hi - lo;
        ++drawCalls;
    }
    return drawn;
}

// Original segments [range) from the full path (or its display decimation).
std::uint64_t drawFull(QRhiCommandBuffer* cb, QRhiBuffer* corners, const GpuItem& gpu, detail::SegmentRange range,
                       std::uint64_t& drawCalls)
{
    return drawChunks(cb, corners, gpu.pathChunks, detail::strideRange(range, gpu.pathStride, gpu.pathSegments),
                      drawCalls);
}

} // namespace

const float kPathCorners[8] = {0, -1, 0, 1, 1, -1, 1, 1};

QRhiVertexInputLayout pathVertexLayout()
{
    return layout(false);
}

QRhiVertexInputLayout pickPathVertexLayout()
{
    return layout(true);
}

bool uploadBackplot(QRhi* rhi, ResourceCounter& counter, const sim::Backplot& bp, int maxSegments, GpuItem& gpu,
                    QRhiResourceUpdateBatch* u)
{
    gpu.pathChunks.clear();
    const std::uint64_t original = detail::backplotSegmentCount(bp);
    gpu.pathStride = detail::backplotStride(original, maxSegments);
    gpu.pathSegments = 0;
    if (original == 0)
        return true;
    std::vector<sim::BackplotVertex> decimated;
    const sim::BackplotVertex* vertices = bp.lineVertices.data();
    std::uint64_t count = bp.lineVertices.size();
    if (gpu.pathStride > 1) {
        decimated = detail::decimateBackplot(bp, gpu.pathStride);
        vertices = decimated.data();
        count = decimated.size();
    }
    if (!uploadStrip(rhi, counter, vertices, count, gpu.pathChunks, u))
        return false;
    gpu.pathSegments = count - 1;
    return true;
}

bool uploadPathLods(QRhi* rhi, ResourceCounter& counter, const detail::SceneItem& item, GpuItem& gpu,
                    QRhiResourceUpdateBatch* u)
{
    if (gpu.lodsUploaded || !item.lods || !item.lods->ready.load(std::memory_order_acquire))
        return false;
    gpu.lodsUploaded = true;
    gpu.lodChunks.clear();
    for (const detail::PathLod& lod : item.lods->levels) {
        std::vector<PathChunk> chunks;
        if (!uploadStrip(rhi, counter, lod.vertices.data(), lod.vertices.size(), chunks, u)) {
            gpu.lodChunks.clear(); // drawn at full detail
            return true;
        }
        gpu.lodChunks.push_back(std::move(chunks));
    }
    return !gpu.lodChunks.empty();
}

int pathLevelFor(const detail::SceneItem& item, const GpuItem& gpu, const Camera& camera, int heightPx)
{
    if (gpu.lodChunks.empty() || !item.lods || !item.localBounds || heightPx <= 0)
        return 0;
    double viewHeightMm = camera.orthoHeight;
    if (camera.perspective) {
        // The point of the bounds nearest the eye along the view direction.
        const Vec3 f{camera.target.x - camera.eye.x, camera.target.y - camera.eye.y, camera.target.z - camera.eye.z};
        const double len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
        if (!(len > 0))
            return 0;
        const Box3 b = detail::transformBox(*item.localBounds, item.xform);
        double nearest = 1e300;
        for (int corner = 0; corner < 8; ++corner) {
            const double x = (corner & 1) ? b.max.x : b.min.x, y = (corner & 2) ? b.max.y : b.min.y,
                         z = (corner & 4) ? b.max.z : b.min.z;
            nearest = std::min(nearest,
                               ((x - camera.eye.x) * f.x + (y - camera.eye.y) * f.y + (z - camera.eye.z) * f.z) / len);
        }
        if (!(nearest > 0))
            return 0; // the eye is inside or beside the path: full detail
        viewHeightMm = 2.0 * nearest * std::tan(std::clamp(camera.fovDeg, 1.0, 150.0) * kPi / 360.0);
    }
    if (!(viewHeightMm > 0))
        return 0;
    int level = std::min<int>(detail::pathLodLevel(item.lods->levels, double(heightPx) / viewHeightMm),
                              int(gpu.lodChunks.size()));
    // Never more segments than the display limit allows (gpu.pathSegments, after decimation): a coarser level then,
    // or the decimated path when even the coarsest is larger.
    while (level > 0 && item.lods->levels[std::size_t(level) - 1].vertices.size() - 1 > gpu.pathSegments)
        level = level < int(gpu.lodChunks.size()) ? level + 1 : 0;
    return level;
}

std::uint64_t drawBackplot(QRhiCommandBuffer* cb, QRhiBuffer* corners, const detail::SceneItem& item,
                           const GpuItem& gpu, int level, std::uint64_t& drawCalls)
{
    if (!item.backplot)
        return 0;
    const detail::SegmentRange range = detail::backplotMoveSegments(*item.backplot, item.firstMove, item.lastMove);
    if (level <= 0 || std::size_t(level) > gpu.lodChunks.size() || !item.lods)
        return drawFull(cb, corners, gpu, range, drawCalls);
    const detail::LodPieces pieces = detail::lodPieces(item.lods->levels[std::size_t(level) - 1], range);
    std::uint64_t drawn = drawFull(cb, corners, gpu, pieces.head, drawCalls);
    drawn += drawChunks(cb, corners, gpu.lodChunks[std::size_t(level) - 1], pieces.level, drawCalls);
    drawn += drawFull(cb, corners, gpu, pieces.tail, drawCalls);
    return drawn;
}

std::uint64_t drawBackplotMove(QRhiCommandBuffer* cb, QRhiBuffer* corners, const detail::SceneItem& item,
                               const GpuItem& gpu, std::uint32_t moveIndex, std::uint64_t& drawCalls)
{
    if (!item.backplot)
        return 0;
    return drawFull(cb, corners, gpu, detail::backplotMoveSegments(*item.backplot, moveIndex, moveIndex), drawCalls);
}

} // namespace ray::render::rhi
