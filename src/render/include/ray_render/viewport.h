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
};

// ---------- Picking (task T-015) ----------
// pick and pickRect take LOGICAL pixels, origin top-left (what Qt Quick's input events give); the renderer scales
// them by setDevicePixelRatio. The edge and vertex tolerance is kPickTolerancePx logical pixels. Inside it the most
// specific kind the filter enables wins: vertex > edge > face > body. A face or body needs the cursor pixel itself.
// Hits: Face and Edge carry the kernel index (FaceRange::face, EdgeRange::edge); an addLines item answers as Edge
// with the segment index; Bodies answers kind Solid, subIndex 0. `point` is the world point under the pixel, read
// from the ID buffer's depth (a vertex hit gives the vertex itself). moveIndex is 0 until backplots exist.
// Every visible item is pickable, see-through ones included.
constexpr int kPickTolerancePx = 6;
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
