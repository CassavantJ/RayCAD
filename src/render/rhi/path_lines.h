// Private to src/render/rhi: the GPU side of toolpath backplots (task T-038); the rules are in ../lines.h.
//
// GPU memory: the backplot's own 24-byte vertices (or the decimated copy's), in buffers of at most
// detail::kPathChunkSegments + 1 vertices, and nothing per segment besides, plus its levels of detail (24 bytes per
// vertex kept); one 32-byte corner template is shared by every backplot. A range change (scrubbing) moves the buffer
// offsets and the instance counts of the draws, never a buffer or a pipeline.
#pragma once

#include "rhi_util.h"

#include "../lines.h"
#include "../viewport_frontend.h"

#include <cstdint>
#include <vector>

namespace ray::render::rhi {

struct GpuItem;

// path.vert: corner (binding 0, the 4-vertex strip template), then per instance p0 (binding 1, vertex s) and p1 and
// role / kind bytes (binding 2, vertex s + 1). pick_path.vert adds the move index bytes from binding 2.
QRhiVertexInputLayout pathVertexLayout();
QRhiVertexInputLayout pickPathVertexLayout();
// The corner template: 4 float2, as a triangle strip.
constexpr quint32 kPathCornerBytes = 4 * 2 * sizeof(float);
extern const float kPathCorners[8];

struct PathChunk {
    Owned<QRhiBuffer> buffer;
    std::uint64_t firstSegment = 0; // in the GPU copy
    std::uint32_t segments = 0;
};

// Uploads the backplot (decimated to `maxSegments`, TierPreset::maxBackplotSegments) into gpu.pathChunks and sets
// gpu.pathStride and gpu.pathSegments. Returns false when a buffer could not be created (nothing is drawn then).
bool uploadBackplot(QRhi* rhi, ResourceCounter& counter, const sim::Backplot& bp, int maxSegments, GpuItem& gpu,
                    QRhiResourceUpdateBatch* u);
// Uploads the item's levels of detail into gpu.lodChunks once they are ready (lines.h); sets gpu.lodsUploaded.
// Returns true when it uploaded something.
bool uploadPathLods(QRhi* rhi, ResourceCounter& counter, const detail::SceneItem& item, GpuItem& gpu,
                    QRhiResourceUpdateBatch* u);

// The level of detail to draw the item at in a target `heightPx` pixels high (0: the full path): from its largest
// scale on screen, pixels per mm at the point of its bounds nearest the eye.
int pathLevelFor(const detail::SceneItem& item, const GpuItem& gpu, const Camera& camera, int heightPx);

// Records the draws of the item's range (setBackplotRange) at `level`, with the pipeline and shader resources
// already set: the level between full-path pieces at both ends. Returns the segments drawn; adds the draw calls to
// `drawCalls`.
std::uint64_t drawBackplot(QRhiCommandBuffer* cb, QRhiBuffer* corners, const detail::SceneItem& item,
                           const GpuItem& gpu, int level, std::uint64_t& drawCalls);
// The same for the segments of one move (highlights), always from the full path.
std::uint64_t drawBackplotMove(QRhiCommandBuffer* cb, QRhiBuffer* corners, const detail::SceneItem& item,
                               const GpuItem& gpu, std::uint32_t moveIndex, std::uint64_t& drawCalls);

} // namespace ray::render::rhi
