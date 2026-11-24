// RayCAD interface contract: hardware tiers, theme tokens, viewport renderer
// Module: src/render   Depends on: core, kernel (Mesh), sim (Backplot); Qt RHI is PRIVATE to src/render/rhi
// Status: PLAN DRAFT. Backend choice (own renderer on Qt RHI) is gated by spike S1; this header does not
// change if the fallback (OCCT viewer on forced OpenGL) is chosen, because nothing here names a graphics API.
//
// Hard rule: nothing in this module can influence geometry, toolpaths or G-code. src/cam, src/post and
// src/kernel do not link src/render (enforced by a CMake dependency check in CI).
// T-076: IViewport::captureImageInTheme (additive, with a default that captures as captureImage does).
// T-073: the comment of IViewport::updateMesh says what a dirty box means (comment only; no signature changed).
#pragma once
#include "core.h"
#include "kernel.h"
#include <map>
#include <optional>

namespace ray::sim { struct Backplot; }

namespace ray::render {

// ---------- Hardware detection and tiers ----------
// Unknown means detection could not tell; chooseTier treats an Unknown GPU as Low.
enum class GpuType : std::uint8_t { Unknown, Integrated, Discrete, Software };
// Every field has an "unknown" value: "" for strings, 0 for numbers, GpuType::Unknown, false for computeShaders.
struct HardwareInfo {
    std::string gpuVendor, gpuName, driverVersion, graphicsApi;   // "Direct3D 11", "Metal", "OpenGL 4.6"
    GpuType gpuType = GpuType::Unknown;                            // Software: WARP, llvmpipe, SwiftShader
    std::uint64_t vramBytes = 0, ramBytes = 0; int physicalCores = 0;   // vramBytes: dedicated video memory
    bool computeShaders = false; int maxTextureSize = 0;
    bool discreteGpu() const { return gpuType == GpuType::Discrete; }
    bool softwareRasterizer() const { return gpuType == GpuType::Software; }
};
// Never fails and needs no window and no GPU; may take up to a few hundred ms (it creates a throw-away device on
// Windows). Cached in settings with the driver version. On Windows gpuType, vramBytes and driverVersion come from
// DXGI and D3D11: QRhi reports device type Unknown and no VRAM for real adapters (spike S1). Linux reads
// /sys/class/drm, macOS sysctl; what they cannot see is reported as unknown (details: src/render/tier_detect.cpp).
HardwareInfo detectHardware();

enum class Tier { Low, Medium, High };
struct TierPreset {
    Tier tier;
    // Display only
    double meshChordTolMm, meshAngleTolDeg;   // Low 0.2/20, Medium 0.05/12, High 0.01/6 (starting values).
                                              // An imported model is FIRST meshed at 0.2/20 on every tier, then refined (PLAN 9).
    int msaaSamples;                          // 0, 4, 4
    bool ambientOcclusion, shadows, smoothEdges, transparency, sectionCaps;
    bool liquidGlass;                         // default per tier; user override stored separately
    int maxBackplotSegments;                  // beyond this the backplot is decimated FOR DISPLAY only
    // Simulation resolution and backend
    double simCellMm;                         // 0.2, 0.1, 0.05 (upper bound, snapped per axis; PLAN 6.2)
    bool gpuSimulation;                       // High only, v2
    int workerThreads;                        // 0 = physical cores - 1
};
Tier chooseTier(const HardwareInfo&);                 // rules in PLAN.md 6.1; unknown GPU type gives Low, other
                                                      // unknown values never give High
TierPreset presetFor(Tier);
// The user may override the tier and any individual field; overrides persist in user settings, not in documents.

// ---------- Theme tokens ----------
struct Color { float r, g, b, a; };                   // linear RGB
enum class Token : std::uint16_t {
    // surfaces and text
    WindowBg, PanelBg, PanelBgGlassTint, PanelBorder, PanelHighlightTop, TextPrimary, TextSecondary, TextDisabled,
    Accent, AccentThinLine, AccentText, Selection, SelectionFill, Hover, Error, Warning, Success,
    // viewport
    ViewportBgTop, ViewportBgBottom, ModelFace, ModelEdge, ModelSilhouette, FixtureFace, StockFace, StockCutFace,
    GridMajor, GridMinor, AxisX, AxisY, AxisZ, SketchGeometry, SketchConstruction, SketchConstrained, SketchConflict,
    // toolpaths
    PathCut, PathRapid, PathPlunge, PathRamp, PathLead, PathLink, PathSelected, PathStale,
    SimGouge, SimLeftover, SimCollision,
    // draft analysis (T-023): positive draft, negative draft (undercut), near-vertical
    DraftPositive, DraftNegative, DraftVertical,
    // G-code editor
    GcodeText, GcodeComment, GcodeGWord, GcodeMWord, GcodeAxis, GcodeFeedSpeed, GcodeLineNumber, GcodeCurrentLine,
    Count
};
struct Theme {
    std::string id, title; bool dark;
    std::array<Color, static_cast<std::size_t>(Token::Count)> colors;
    std::map<std::string, double> metrics;            // "radius.panel", "radius.control", "font.ui.px", "blur.radius", ...
    std::map<std::string, std::string> fonts;         // "ui" -> "Inter", "mono" -> "JetBrains Mono"
};
// The JSON key of a token in a theme file: its enumerator name, e.g. "PanelBg". Empty for Token::Count.
std::string_view tokenName(Token);
// Themes are JSON files in resources/themes/ (schemas/theme.schema.json). Loading validates: every token present;
// text tokens meet 4.5:1 contrast against PanelBg and against the worst-case glass backdrop; thin-line tokens meet
// 3:1 against ViewportBgTop and ViewportBgBottom. A failure is FormatError naming the offending token.
Result<Theme> loadTheme(const std::string& jsonPath);   // UTF-8 path
double contrastRatio(Color fg, Color bg);             // WCAG 2 ratio; fg alpha is blended over bg, bg is taken opaque

// ---------- Scene ----------
using ItemId = std::uint32_t;
// faceColor (T-016): when set, faces are drawn in this colour (linear RGB; alpha is ignored, `opacity` applies)
// instead of the `face` token. It is DATA, e.g. a body colour from a STEP/IGES file, never a literal in code; edges,
// selection and hover keep their theme tokens (highlights on such faces are strengthened so they stay visible).
struct MeshStyle {
    Token face; Token edge; float opacity = 1; bool showEdges = true; bool silhouetteOnly = false;
    std::optional<Color> faceColor;
};
struct Camera { Vec3 eye, target, up; bool perspective; double fovDeg, orthoHeight; };
struct SectionPlane { Vec3 origin, normal; bool cap; };

// `point` needs a depth read-back at the picked pixel in addition to the ID buffer (spike S1).
// Toolpath (T-038): only PickFilter::Toolpath finds a backplot. A hit on one of its segments has kind Edge and both
// subIndex and moveIndex equal to that segment's sim::BackplotVertex::moveIndex (all 32 bits); moveIndex is 0 for
// every other hit. setHighlight with a hit on a backplot item highlights the move `moveIndex` (kind and subIndex are
// not read), so a hit can be made from a G-code line through PostOutput::lineMap.
struct PickHit { ItemId item; kernel::SubKind kind; std::uint32_t subIndex; Vec3 point; std::uint32_t moveIndex; };
enum class PickFilter : std::uint8_t { Faces = 1, Edges = 2, Vertices = 4, Bodies = 8, Toolpath = 16 };

class IViewport {
public:
    virtual ~IViewport() = default;
    // All calls on the UI thread. Uploads are queued and applied on the render thread at the next sync.
    virtual ItemId addMesh(std::shared_ptr<const kernel::Mesh>, const Xform&, MeshStyle) = 0;
    // Incremental sim stock (T-073; details in src/render/include/ray_render/viewport.h, "Simulation stock"). Without
    // `dirty` the mesh replaces the item's. With it, the mesh's face ranges replace the item's ranges of the same face
    // number (IStockModel::toMesh: one per tile; an empty range removes one), ranges it does not name stay, except those
    // lying wholly inside `dirty` (item coordinates), which go; only the new ranges are uploaded.
    virtual void updateMesh(ItemId, std::shared_ptr<const kernel::Mesh>, std::optional<Box3> dirty) = 0;
    // Backplot (T-038): drawn by move type in the tokens PathCut, PathRapid (dashed), PathPlunge, PathRamp, PathLead
    // (LeadIn, LeadOut) and PathLink (Link, Retract, Approach); a rapid is PathRapid whatever its role. Width: theme
    // metric "line.path.px", else "line.thin.px", in device-independent pixels. The backplot is shared, never copied,
    // and must not change afterwards. setStyle: `edge` PathCut (the default) keeps the colours by move type, any
    // other token draws the whole path in it (e.g. PathStale); `opacity` applies; the other fields are ignored.
    // Over TierPreset::maxBackplotSegments segments it is decimated for display (src/render/lines.h).
    virtual ItemId addBackplot(std::shared_ptr<const sim::Backplot>) = 0;
    // Moves firstMove to lastMove INCLUSIVE are drawn and pickable, the others hidden; values past the last move are
    // clamped, firstMove > lastMove shows nothing. A new backplot shows all its moves. Costs a draw-range change only
    // (no upload). Timeline scrubbing.
    virtual void setBackplotRange(ItemId, std::uint32_t firstMove, std::uint32_t lastMove) = 0;
    // `segments`: pairs of end points, one segment per pair (an odd last point is dropped).
    virtual ItemId addLines(std::span<const Vec3> segments, Token colour, float widthPx, bool dashed) = 0;
    // `revolvedProfile`: (radius, height) points in mm from the tip up (sim::ToolShape's profiles, e.g. cutting,
    // shank and holder profiles joined), revolved about +Z with the tip at the item's origin; shaded like a mesh in
    // `colour`. Place and move it with setTransform (a uniform change, cheap every frame). It answers no picks
    // (PickMode::Never in ray_render/viewport.h), so it never hides the toolpath under the cursor.
    virtual ItemId addToolGlyph(std::span<const Vec2> revolvedProfile, Token colour) = 0;
    virtual void setTransform(ItemId, const Xform&) = 0;
    virtual void setStyle(ItemId, MeshStyle) = 0;
    virtual void setVisible(ItemId, bool) = 0;
    virtual void remove(ItemId) = 0;

    virtual void setHighlight(std::span<const PickHit> selected, std::optional<PickHit> hover) = 0;
    virtual void setCamera(const Camera&) = 0;
    virtual Camera camera() const = 0;
    virtual void setSection(std::optional<SectionPlane>) = 0;
    virtual void setTheme(std::shared_ptr<const Theme>) = 0;       // runtime switch, no restart
    virtual void setPreset(const TierPreset&) = 0;                 // runtime switch, no restart

    // Picking uses an ID buffer rendered on demand (only when the mouse moves and the scene or camera is unchanged
    // since the last ID render, the cached buffer is reused). Async: result arrives on the UI thread.
    virtual void pick(int x, int y, std::uint8_t filterMask, std::function<void(std::optional<PickHit>)>) = 0;
    virtual void pickRect(int x0, int y0, int x1, int y1, std::uint8_t filterMask, std::function<void(std::vector<PickHit>)>) = 0;

    virtual void requestFrame() = 0;                               // render on demand; no continuous redraw when idle
    virtual void captureImage(int w, int h, std::function<void(std::vector<std::byte> png)>) = 0;   // thumbnails, setup sheets
    // Added by T-076: the same capture drawn in the given theme's colours instead of the viewport's (a setup sheet's picture
    // prints on white whatever the app's theme), without selection or hover highlights. Only the capture changes: what
    // is on screen and the frames after it are drawn as before. Null theme: as captureImage.
    // The default, for an implementation without it, captures in the viewport's own theme.
    virtual void captureImageInTheme(int w, int h, std::shared_ptr<const Theme> /*theme*/,
                                     std::function<void(std::vector<std::byte> png)> done) { captureImage(w, h, std::move(done)); }
    struct FrameStats { double cpuMs, gpuMs; std::uint64_t triangles, drawCalls; };
    virtual FrameStats lastFrameStats() const = 0;

    // Liquid Glass support: a downsampled, blurred copy of the last viewport frame, regenerated only when the
    // viewport image changed. Panels sample this ONE texture.
    // Ownership (spike S1 hit a double free here): the viewport OWNS the texture (a QRhiTexture). Whoever wraps it
    // for a scene graph must not take ownership (QQuickWindow::createTextureFromRhiTexture does, so it is not used;
    // src/ui wraps it in a non-owning QSGTexture). The handle stays valid until the viewport is resized, the backdrop
    // is disabled or the renderer releases its resources; after that it must be fetched again (`generation` moves
    // with every new blur, `width`/`height` say when the texture was replaced).
    // Orientation: the texture has the native orientation of a render target; yUp true (OpenGL) means its first row
    // is the BOTTOM of the picture, so a sampler flips V. Width and height are about a quarter of the viewport's
    // pixels; the whole texture covers the whole viewport.
    // Threading (settled in T-016): the handle is only usable on the render thread, so on the UI thread this call
    // returns the description of the latest blur with rhiTexture null for a Qt Quick viewport; the render thread
    // gets the handle from HostedViewportRenderer::backdrop() (ray_render/viewport_rhi.h) right after rendering. The
    // offscreen viewport, whose UI thread is its render thread, returns the handle itself.
    struct Backdrop { void* rhiTexture; int width, height; bool yUp; std::uint64_t generation; };
    virtual Backdrop blurredBackdropTexture() = 0;
    // off => zero cost: no blur pass and no backdrop texture (flat panels, or no glass panel visible).
    virtual void setBackdropEnabled(bool) = 0;
};

// ---------- GPU compute plug-in point (High tier, v2) ----------
// The same algorithm interface (sim::IStockModel) gets a second implementation; callers do not branch on tier.
class IComputeBackend {
public:
    virtual ~IComputeBackend() = default;
    virtual bool available() const = 0;
    using BufferId = std::uint32_t; using PipelineId = std::uint32_t;
    virtual BufferId createBuffer(std::size_t bytes) = 0;
    virtual void upload(BufferId, std::span<const std::byte>, std::size_t offset) = 0;
    virtual Result<PipelineId> createPipeline(std::string_view shaderResourceName) = 0;   // precompiled .qsb per API
    virtual void dispatch(PipelineId, std::span<const BufferId> bindings, int gx, int gy, int gz) = 0;
    virtual void readback(BufferId, std::function<void(std::vector<std::byte>)>) = 0;
    virtual void destroy(BufferId) = 0;
};

} // namespace ray::render
