// The viewport renderer's own entry points (task T-014): what other code needs beyond the IViewport contract in
// <ray/render.h> to create a viewport and drive it. No Qt header here, so tests, tools and src/ui code that is not
// Qt-specific can include it. The Qt Quick seam (for T-020) is in viewport_rhi.h next to this file.
// Not one of the contract headers in include/ray/.
#pragma once

#include <ray/render.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ray::render {

// ---------- What the renderer is doing ----------
struct ViewportInfo {
    std::string backend;           // "D3D11", "OpenGL", "Metal", ... (QRhi's backend name); "" before the first frame
    std::string device;            // adapter name as the graphics API reports it
    bool softwareRenderer = false; // WARP, llvmpipe, SwiftShader
    int sampleCount = 1;           // MSAA samples in use: the preset's value clamped to what the device supports
    bool smoothEdges = false;      // mesh edges drawn as anti-aliased quads (preset) instead of 1 px lines
    int width = 0, height = 0;     // render target in pixels
    std::uint64_t framesRendered = 0;
    std::int64_t liveRhiResources = 0; // QRhi resources this viewport's renderer currently owns (leak checks)
    // Times the picking ID buffer was drawn. Picks reuse it until the camera or the scene changes (task T-015).
    std::uint64_t pickBufferRenders = 0;
    // Times the glass backdrop was blurred (task T-016): at most once per frame, only while the backdrop is enabled
    // and only when the viewport image changed (or the backdrop has no blur of the current image yet).
    std::uint64_t backdropBlurs = 0;
    // Times an item's geometry was uploaded to the GPU (task T-023): moving a section plane or switching draft
    // analysis changes only uniforms, so it never moves this.
    std::uint64_t meshUploads = 0;
    // Bodies that got a section cap in the last drawn frame (task T-023): closed, visible, opaque meshes the plane
    // cuts, while the preset allows caps and the section asks for them.
    std::uint32_t sectionCaps = 0;
    // Closed-mesh checks for section caps still running on a worker (task T-023); a body is capped once its check
    // is done (the viewport then draws again by itself).
    std::uint32_t closedChecksPending = 0;
    // Backplot segments drawn in the last drawn frame (task T-038): the ranges setBackplotRange shows, after display
    // decimation (TierPreset::maxBackplotSegments) and level of detail (src/render/lines.h); highlights not counted.
    std::uint64_t pathSegments = 0;
    // Bytes of GPU buffers that hold backplot vertices (task T-038), all backplots and their levels of detail.
    std::uint64_t pathGpuBytes = 0;
    // Backplots whose levels of detail are still being built on a worker (task T-038); the full path is drawn until
    // then, and the viewport draws again by itself when they are ready.
    std::uint32_t pathLodsPending = 0;
    // Tiled meshes (task T-073: simulation stock, and any mesh item updated with a dirty box; "Simulation stock"
    // below). stockUploadBytes counts every byte handed to the GPU for their face ranges since the viewport was made
    // (vertex and index data; nothing else is uploaded for them), stockTileUploads the face ranges uploaded, and
    // stockRepacks the times the renderer uploaded every range again because its buffers held more than twice what
    // is in use (a new graphics device uploads everything too, counted in the bytes but not here). stockGpuBytes is
    // the size of the GPU buffers that hold them now, stockDrawCalls the indexed draws their triangles took in the
    // last drawn frame (main pass only).
    std::uint64_t stockUploadBytes = 0;
    std::uint64_t stockTileUploads = 0;
    std::uint64_t stockRepacks = 0;
    std::uint64_t stockGpuBytes = 0;
    std::uint64_t stockDrawCalls = 0;
};

// ---------- Simulation stock (task T-073) ----------
// The in-process stock of the simulation is a TILED mesh: Mesh::faces holds one triangle range per tile (sim.h,
// IStockModel::toMesh: SubId{Face, tile number}, each tile a closed surface of its own). The viewport keeps it per
// face range, and IViewport::updateMesh(item, mesh, dirty) with a dirty box REPLACES BY FACE NUMBER: every face range
// of `mesh` replaces the range with the same face number (an empty range removes it), ranges not in `mesh` stay as
// they were, except ranges whose bounds lie entirely inside `dirty` (item coordinates), which are removed (a producer
// may leave out a tile it has emptied; IStockModel lists them all). Triangles in no face range are not drawn. Only
// the new ranges are uploaded to the GPU (ViewportInfo::stockUploadBytes); the rest stay where they are. Without a
// dirty box updateMesh replaces every range (still per range, without re-creating the buffers). Any mesh item can be
// updated this way; the first update with a dirty box moves it to the tiled storage (one full upload).
// A face hit (pick, setHighlight) on a tiled mesh names the face number, here the tile.
//
// addStock adds a tiled mesh with the look of the stock: StockShading below, MeshStyle::face for its uncut faces and
// MeshStyle::edge for the faces cutting made (stockStyle() gives the tokens StockFace and StockCutFace; showEdges is
// ignored: a stock has no edge lines). It is treated as the closed solid IStockModel guarantees: a section always caps
// it (on every preset, whatever SectionPlane::cap says), so the walls that close each tile inside the material never
// show, and drawn see-through (opacity < 1 with a preset that allows transparency) only its front surface is drawn.
// For T-074 (simulation panel):
//   show      id = addStock(std::make_shared<const kernel::Mesh>(stock.toMesh(0, &box, budget)), Xform, stockStyle(),
//                           StockShading{bounds of the stock at reset, {}});  shown = stock.version();
//   update    mesh = stock.toMesh(shown, &dirty, budget); updateMesh(id, mesh, dirty); shown = stock.version();
//             (nothing changed: an empty mesh and an empty box, and updateMesh does nothing)
//   seek      the same as update: restore() marks the tiles it changes dirty
//   hide      setVisible(id, false) (the tiles stay on the GPU; updates while hidden are uploaded when shown)
//   recolour  setStyle(id, MeshStyle{uncut token, cut token, opacity}); setStockShading for marks
//   budget    each toMesh call honours its own budget, so a dirty region asked for with the whole display's budget
//             comes back finer than its neighbours (measured, 0.05 mm benchmark stock: one 20 mm move gave 887k
//             triangles in 55 tiles, 60 MB to upload, where the whole display held 661k in 1536 tiles, 43 MB). To keep
//             the display's level of detail, learn the box first (toMesh(shown, &box, 1) returns the coarsest mesh;
//             its cost was not measured) and ask again with the budget times the box's share of the stock's area;
//             that move then uploads about 3 % of the display's bytes. triangleCount(id) says what the item holds, to
//             decide when to ask for toMesh(0, ...) at a coarser budget again. Tiles of different detail meet without
//             gaps (each is closed); only their heights step at the seam where the coarser one merged its columns.
// Marks colour the stock's surface inside up to kMaxStockMarks boxes (item coordinates) in a simulation token:
// SimCollision (the default; where a holder or shank met material), SimGouge or SimLeftover; any other token draws
// as SimCollision. Marks after the first kMaxStockMarks are ignored. Colours change per pixel in the shader, so
// marks, the uncut box and the style change no buffer. (The uncut box also tells the stock's own sides from the walls
// inside it when a tile is first uploaded; give it with addStock: a later change does not re-sort tiles already shown.)
constexpr int kMaxStockMarks = 8;
struct StockMark {
    Box3 box;
    Token colour = Token::SimCollision;
};
struct StockShading {
    // Faces lying on this box's surface (the stock before any cut, in item coordinates) are uncut and drawn in
    // MeshStyle::face; every other face in MeshStyle::edge. nullopt: every face in MeshStyle::face. Only a box stock is
    // told apart exactly; for stock from a solid, faces on its bounding box count as uncut and all others as cut.
    std::optional<Box3> uncut;
    std::vector<StockMark> marks;
};
inline MeshStyle stockStyle()
{
    return MeshStyle{Token::StockFace, Token::StockCutFace, 1.0f, false, false, std::nullopt};
}

// ---------- Section view and draft analysis (task T-023) ----------
// IViewport::setSection(SectionPlane{origin, normal, cap}): everything on the side the normal points to,
// dot(p - origin, normal) > 0, is cut away (faces, edges, lines and the picking ID buffer alike); the viewer usually
// looks along -normal into the cut. A zero or non-finite normal turns the section off. With `cap` and a preset whose
// sectionCaps is set, every closed (watertight) visible opaque mesh gets a solid cap on the cut, in a darker shade of
// its own face colour; open shells, lone faces, see-through and hidden bodies get none. The cut line is outlined on
// the faces in the edge colour. Caps answer picks as their body (PickFilter::Bodies) and hide what lies behind them.
// Draft analysis colours every mesh face per pixel by the angle between its outward normal n and the unit pull
// direction p: draft angle = asin(dot(n, p)). Above +threshold the face has positive draft (it releases), below
// -threshold negative draft (an undercut), in between it is near-vertical. While on it replaces MeshStyle::faceColor
// and the face token. Colours: the theme tokens DraftPositive, DraftNegative and DraftVertical.
struct DraftAnalysis {
    Vec3 pull{0, 0, 1};        // normalised by the renderer; a zero or non-finite vector turns the analysis off
    double thresholdDeg = 1.0; // clamped to 0..89
};
enum class DraftClass : std::uint8_t { Positive, Vertical, Negative };
// The class draft analysis paints for a surface normal (CPU reference of the shader; `normal` need not be unit).
DraftClass draftClassOf(Vec3 normal, const DraftAnalysis&);
// The colour token of a class.
Token draftToken(DraftClass);

// ---------- Picking (task T-015) ----------
// pick and pickRect take LOGICAL pixels, origin top-left (what Qt Quick's input events give); the renderer scales
// them by setDevicePixelRatio. The edge and vertex tolerance is kPickTolerancePx logical pixels. Inside it the most
// specific kind the filter enables wins: vertex > edge > face > body. A face or body needs the cursor pixel itself.
// Hits: Face and Edge carry the kernel index (FaceRange::face, EdgeRange::edge); an addLines item answers as Edge
// with the segment index; Bodies answers kind Solid, subIndex 0. `point` is the world point under the pixel, read
// from the ID buffer's depth (a vertex hit gives the vertex itself). moveIndex is 0 except for Toolpath hits.
// Toolpath (task T-038): a segment of a backplot within the tolerance answers kind Edge with subIndex and moveIndex
// both its BackplotVertex::moveIndex (the full 32 bits). Only PickFilter::Toolpath finds backplots; with Edges as well,
// the nearer of the two wins. Backplots never answer Faces or Bodies, and a face under a toolpath line is still found.
// Zoomed out, where a level of detail draws one chord for several sub-pixel segments (src/render/lines.h), the hit
// names the last move of that chord, which lies within half a pixel of the others.
// Every visible item is pickable, see-through ones included, unless its PickMode says otherwise (below).
constexpr int kPickTolerancePx = 6;

// Which items answer picks. WhenVisible (every new item): pickable while it is visible. Never: drawn but never in the
// ID buffer, so a pick goes through it to what lies under it (a feature preview, guides, picked-region outlines).
// Always: in the ID buffer even while hidden, and its selection and hover highlights are drawn while hidden (the
// body a preview stands in for: edges of the model as it is can still be picked while the preview shows the result).
enum class PickMode : std::uint8_t { WhenVisible, Never, Always };
// Vertex hits: kernel::Mesh has no vertex ids, so a Vertex PickHit's subIndex names one END of an edge polyline:
// vertexCode(edge, lastEnd) with the edge's kernel index and whether it is the polyline's last point. Of all the edge
// ends that meet at a vertex the smallest code is used, so a vertex always has the same code. The kernel vertex is
// kernel::adjacent(shape, {Edge, vertexEdge(code)}, Vertex), the one at `point`.
constexpr std::uint32_t vertexCode(std::uint32_t edgeIndex, bool lastEnd)
{
    return edgeIndex * 2 + (lastEnd ? 1u : 0u);
}
constexpr std::uint32_t vertexEdge(std::uint32_t code)
{
    return code / 2;
}
constexpr bool vertexIsLastEnd(std::uint32_t code)
{
    return (code & 1u) != 0;
}

// IViewport plus the queries every host needs. Same threading rule: UI thread only.
class IRenderViewport : public IViewport {
public:
    // Bounds of the visible items in world coordinates (transforms applied); nullopt when nothing is visible.
    virtual std::optional<Box3> sceneBounds() const = 0;
    virtual ViewportInfo info() const = 0;
    // Physical pixels per logical pixel; scales line widths taken from theme metrics. Default 1.
    virtual void setDevicePixelRatio(double) = 0;
    // Task T-023 (see "Section view and draft analysis" above). Both survive theme and preset changes.
    virtual std::optional<SectionPlane> section() const = 0;
    virtual void setDraftAnalysis(std::optional<DraftAnalysis>) = 0;
    virtual std::optional<DraftAnalysis> draftAnalysis() const = 0;
    // See PickMode above. Unknown ids are ignored. Changes what the ID buffer holds (it is drawn again).
    virtual void setPickMode(ItemId, PickMode) = 0;
    virtual PickMode pickMode(ItemId) const = 0; // WhenVisible for an unknown id
    // Task T-073 (see "Simulation stock" above). addStock returns 0 for a mesh that does not hold together;
    // setStockShading on an item that is no stock is ignored. triangleCount: the triangles a mesh item holds (a tiled
    // one: in all its face ranges), 0 for an unknown id or another kind of item.
    virtual ItemId addStock(std::shared_ptr<const kernel::Mesh>, const Xform&, MeshStyle, StockShading) = 0;
    virtual void setStockShading(ItemId, StockShading) = 0;
    virtual std::uint64_t triangleCount(ItemId) const = 0;
};

// ---------- Camera helpers (pure math; the UI's navigation calls these, then setCamera) ----------
// World is Z-up. Distances in mm, angles in degrees, pixel coordinates with the origin top-left.
enum class StandardView : std::uint8_t { Front, Back, Left, Right, Top, Bottom, Iso };
Camera defaultCamera(); // iso view of the origin, perspective, 30 degree field
// Turns the camera about `pivot`: yaw about the world Z axis, pitch about the camera's right axis.
Camera orbitCamera(const Camera&, double yawDeg, double pitchDeg, Vec3 pivot);
// Moves eye and target so that the content under the cursor follows a drag of (dx, dy) pixels.
Camera panCamera(const Camera&, double dxPx, double dyPx, int viewportHeightPx);
// factor > 1 zooms in. With `towards`, that point keeps its place on screen (zoom to cursor).
Camera zoomCamera(const Camera&, double factor, std::optional<Vec3> towards = std::nullopt);
// Keeps the view direction and frames `box` with a small margin. aspect = width / height.
Camera fitCamera(const Camera&, const Box3& box, double aspect);
Camera standardViewCamera(StandardView, const Box3& box, double aspect, bool perspective, double fovDeg = 30.0);
// The world-space ray through pixel (x, y) of a w x h viewport (pixel centres at +0.5). dir is unit length.
struct Ray {
    Vec3 origin, dir;
};
Ray cameraRay(const Camera&, double xPx, double yPx, int widthPx, int heightPx);

// ---------- Offscreen viewport: its own QRhi, no window (tests, thumbnails, the headless CLI) ----------
struct Image; // below
enum class RhiBackend : std::uint8_t { Auto, D3D11, D3D12, Metal, OpenGL, Null };
struct OffscreenOptions {
    int width = 640, height = 480;
    // Auto tries the platform's native API first (D3D11, then D3D12 on Windows; Metal on macOS), then OpenGL.
    // The environment variable RAY_RHI_BACKEND (d3d11, d3d12, metal, opengl) overrides Auto.
    RhiBackend backend = RhiBackend::Auto;
    // Ask for the software rasteriser (WARP on D3D). RAY_RHI_SOFTWARE=1 sets it too.
    bool preferSoftware = false;
};
class IOffscreenViewport : public IRenderViewport {
public:
    virtual void resize(int width, int height) = 0; // takes effect at the next frame, which it requests
    // Draws a frame now, on the calling thread, whether or not one was requested. Frames that requestFrame and the
    // setters ask for are drawn from the Qt event loop, coalesced into one per loop iteration.
    virtual Status renderNow() = 0;
    // The blurred glass backdrop (blurredBackdropTexture) read back as an image, rows top to bottom whatever the
    // texture's orientation (tests and diagnostics). NotFound when the backdrop is disabled or not drawn yet.
    virtual Result<Image> readBackdrop() = 0;
};
// Needs ensureQtRuntime() first. Fails with InvalidArgument for a bad size and with Internal when no graphics API
// could be initialised (tests then skip).
Result<std::unique_ptr<IOffscreenViewport>> makeOffscreenViewport(const OffscreenOptions& = {});

// ---------- Qt runtime for programs that have no Qt application of their own ----------
// Creates the process's QGuiApplication if there is none (on Linux without a display it selects the offscreen
// platform). Idempotent; call on the thread that will run the event loop. The application lives until exit.
Status ensureQtRuntime();
// Runs the event loop for `ms` milliseconds. Returns how many 5 ms timer ticks the loop delivered, which shows the
// loop was alive.
int processEventsFor(int ms);
// Runs the event loop until `done` returns true or `timeoutMs` passes. Returns done().
bool processEventsUntil(const std::function<bool()>& done, int timeoutMs);
// Qt warnings and errors logged since the last call (ensureQtRuntime records them, with QRhi's leak check on, and
// still prints them). Tests use it to prove that no RHI resource leaked.
std::vector<std::string> takeQtWarnings();

// ---------- Images (captureImage output, reference images) ----------
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba; // 8-bit sRGB-encoded RGBA, rows top to bottom, straight alpha
};
Result<std::vector<std::byte>> encodePng(const Image&);
Result<Image> decodePng(std::span<const std::byte>);
Result<Image> readPng(const std::string& path); // UTF-8 path
Status writePng(const std::string& path, const Image&);

} // namespace ray::render
