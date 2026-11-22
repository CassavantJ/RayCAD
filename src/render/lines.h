// Private to src/render: the CPU side of toolpath backplots and of the tool glyph (task T-038). No Qt header here, so
// the rules below are unit-tested without a GPU; the RHI side is rhi/path_lines.h, the shaders path.vert, path.frag
// and pick_path.vert.
//
// Backplot on the GPU. The renderer uploads sim::Backplot::lineVertices as they are (24 bytes a vertex, sim.h) and
// draws every segment as one instance that reads the buffer twice, one vertex apart: the start from vertex s and the
// end, with the segment's attributes, from vertex s + 1. So the GPU holds 24 bytes per segment and nothing else, and
// the attributes never depend on a provoking-vertex convention. The buffer is cut into chunks of kPathChunkSegments
// segments (each chunk repeats the last vertex of the one before), because Direct3D 11 limits one buffer to 128 MB on
// adapters with little dedicated memory.
//
// Colour (pathSlot): the motion first, then the role. Rapid motion is PathRapid and dashed whatever its role; a feed
// is PathCut, PathPlunge, PathRamp, PathLead (LeadIn and LeadOut) or PathLink (Link, and Retract and Approach, which
// have no token of their own). The shader does the same lookup from a table of these tokens in the frame block, so no
// pass over the vertices is needed when the theme changes.
//
// Range (setBackplotRange, render.h): moves firstMove to lastMove INCLUSIVE are drawn, the rest is hidden. Their
// segments are [moveFirstVertex[a] - 1, moveFirstVertex[b + 1] - 1), found without a search (sim.h), so scrubbing
// changes the instance range of a draw and nothing else.
//
// Display limit (TierPreset::maxBackplotSegments): a backplot with more segments is decimated for display only: the
// GPU copy keeps every k-th vertex (and the last), k the smallest stride that brings it under the limit. A decimated
// segment spans k original segments and takes the attributes of the last of them.
#pragma once

#include <ray/core.h>
#include <ray/kernel.h>
#include <ray/render.h>
#include <ray/sim.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ray::render::detail {

static_assert(sizeof(sim::BackplotVertex) == 24, "the path shaders read 24-byte backplot vertices");

// Segments per GPU buffer of a backplot: 48 MiB of vertices.
constexpr std::uint64_t kPathChunkSegments = std::uint64_t{1} << 21;

// The colour of a segment, as path.vert picks it (CPU reference of the shader).
enum class PathSlot : std::uint8_t { Cut, Rapid, Plunge, Ramp, Lead, Link, Count };
PathSlot pathSlot(std::uint8_t role, std::uint8_t kind); // cam::MoveRole and cam::MoveKind values
Token pathToken(PathSlot);
bool pathDashed(std::uint8_t kind); // rapid motion

// Checks the parts of the layout the renderer relies on (sim.h): moveFirstVertex has moves + 1 non-decreasing
// entries, the first at least 1 when there are moves, the last lineVertices.size(); every coordinate is finite.
Status validateBackplot(const sim::Backplot&);
std::uint64_t backplotSegmentCount(const sim::Backplot&);
// Backplot::bounds, or the box of the vertices when that box is empty or inverted; nullopt without vertices.
std::optional<Box3> backplotBounds(const sim::Backplot&);

// Half-open range of segments.
struct SegmentRange {
    std::uint64_t first = 0, end = 0;
    bool empty() const { return end <= first; }
    std::uint64_t count() const { return empty() ? 0 : end - first; }
};
// The segments of moves firstMove to lastMove inclusive, clamped to the backplot; empty when firstMove > lastMove or
// firstMove is past the last move.
SegmentRange backplotMoveSegments(const sim::Backplot&, std::uint32_t firstMove, std::uint32_t lastMove);

// Display decimation: 1 when `segments` fits `maxSegments` (or the limit is 0 or less), else the smallest stride k
// with ceil(segments / k) <= maxSegments.
std::uint32_t backplotStride(std::uint64_t segments, int maxSegments);
// Segments of the decimated copy (stride k) that cover the original range.
SegmentRange strideRange(SegmentRange original, std::uint32_t stride, std::uint64_t strideSegments);
std::uint64_t strideSegmentCount(std::uint64_t segments, std::uint32_t stride);
// The decimated vertices: originals 0, k, 2k, ... and always the last one.
std::vector<sim::BackplotVertex> decimateBackplot(const sim::Backplot&, std::uint32_t stride);

// Level of detail. Zoomed out, most of a dense toolpath's segments are far below a pixel (an adaptive clearing's arc
// chords are 0.05 to 0.1 mm), and the cost of a frame is the number of segments, not the pixels they cover: on the
// software rasteriser 2 M segments are 4 M tiny triangles. So each backplot of at least kLodMinVertices vertices gets
// coarser copies, built once on a worker (buildPathLods): a vertex is dropped when the chord that replaces it and its
// neighbours stays within the level's tolerance of every dropped vertex, the merged segments all have the same colour
// slot, and the chord spans at most kLodMaxRun segments of the level below. The chord takes the attributes of its
// last vertex (its move index included). Each frame draws the coarsest level whose tolerance is at most kLodErrorPx
// pixels at the item's largest scale on screen (pathLodLevel), so the picture differs from the full path by less
// than half a pixel. The ends of a range are drawn from the full path (lodPieces): scrubbing stays exact.
constexpr std::size_t kLodMinVertices = 4096;
constexpr std::uint32_t kLodMaxRun = 64;
constexpr double kLodErrorPx = 0.5;
struct PathLod {
    double toleranceMm = 0;                    // to the full path: the sum of the tolerances of the levels below
    std::vector<sim::BackplotVertex> vertices; // a strip like lineVertices
    std::vector<std::uint32_t> source;         // per vertex: its index in lineVertices, ascending
};
// Coarser and coarser levels, finest first; empty for a short backplot or one that does not simplify.
std::vector<PathLod> buildPathLods(const sim::Backplot&);
// The levels of one backplot, filled by a worker (viewport_frontend.cpp); `levels` is read only once `ready`.
struct PathLodSet {
    std::atomic<bool> ready{false};
    std::vector<PathLod> levels;
};
// 0 for the full path, else 1 + the index of the coarsest level fine enough at `pxPerMm`.
int pathLodLevel(const std::vector<PathLod>& lods, double pxPerMm);
// A range of original segments as full-path pieces at both ends and level segments between.
struct LodPieces {
    SegmentRange head, level, tail; // head and tail in original segments, level in segments of the level
};
LodPieces lodPieces(const PathLod&, SegmentRange original);

// Tool glyph (addToolGlyph). `profile` is (radius, height) in mm from the tip up, as sim::ToolShape's profiles; it is
// revolved about the +Z axis of the item with `sides` facets around. Each profile segment is a band of its own;
// neighbouring bands share smooth normals where they meet at less than kGlyphCreaseDeg, so a corner radius shades
// round and the tip and shoulders stay crisp. An end whose radius is not 0 is closed by a flat disc. Points of zero
// length are dropped. InvalidArgument for fewer than two distinct points, a negative radius or a non-finite value.
constexpr int kToolGlyphSides = 48;
constexpr double kGlyphCreaseDeg = 35.0;
Result<kernel::Mesh> revolveProfile(std::span<const Vec2> profile, int sides = kToolGlyphSides);

} // namespace ray::render::detail
