// Private to src/render: the UI-thread half of every viewport (task T-014). It keeps the scene, camera, theme and
// preset that the IViewport setters change and hands a snapshot of them to the renderer at each sync. It never
// touches the GPU. No Qt header here.
#pragma once

#include <ray_render/viewport.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QObject;

namespace ray::render::detail {

// Backplot (T-038): a toolpath's line strip, drawn by move type (lines.h). A tool glyph is a Mesh item.
enum class ItemKind : std::uint8_t { Mesh, Lines, Backplot };
struct PathLodSet; // lines.h
struct PieceTable; // stock.h (T-073)

// Segments of an addLines item: xyz of both ends, segment after segment.
struct LineSet {
    std::vector<float> points;
    float widthPx = 1;
    bool dashed = false;
};

// Whether a mesh is closed (watertight), for section caps (T-023, section.h). Worked out once per geometry on Qt's
// global thread pool, never on the UI or render thread; state is -1 until then, 0 open, 1 closed.
struct ClosedCheck {
    std::atomic<int> state{-1};
    std::atomic<bool> started{false};
};

struct SceneItem {
    ItemId id = 0;
    ItemKind kind = ItemKind::Mesh;
    std::shared_ptr<const kernel::Mesh> mesh;      // Mesh items
    std::shared_ptr<const LineSet> lines;          // Lines items
    std::shared_ptr<const sim::Backplot> backplot; // Backplot items (T-038)
    std::shared_ptr<const PathLodSet> lods;        // Backplot items: levels of detail, made on a worker (lines.h)
    // Backplot items: the moves drawn, inclusive (setBackplotRange). Changing them never changes geometryGeneration.
    std::uint32_t firstMove = 0, lastMove = 0xFFFFFFFFu;
    Xform xform = Xform::identity();
    // Lines items: style.face and style.edge are the line colour. Backplot items: edge PathCut (the default) draws by
    // move type, any other edge token draws the whole path in that colour; opacity applies (render.h).
    MeshStyle style{};
    bool visible = true;
    PickMode pickMode = PickMode::WhenVisible;
    std::uint64_t geometryGeneration = 0; // changes whenever mesh or lines are replaced
    std::optional<Box3> localBounds;
    std::shared_ptr<ClosedCheck> closed; // Mesh items, new with every geometry (T-023)
    // T-073 (stock.h): a tiled mesh item keeps its triangles per face range here, and `mesh` is null. `stock` is set
    // for addStock items: the stock look, a cap in every section, see-through by its front surface only.
    std::shared_ptr<const PieceTable> pieces;
    std::shared_ptr<const StockShading> stock;
};

struct CaptureRequest {
    int width = 0, height = 0;
    std::function<void(std::vector<std::byte>)> done; // run on the UI thread
    std::shared_ptr<const Theme> theme;               // T-076: drawn in this theme's colours; null: the viewport's
};

// pick (rect false: x0, y0 is the cursor) or pickRect, in logical pixels as the caller gave them (task T-015).
struct PickRequest {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool rect = false;
    std::uint8_t filterMask = 0;
    std::function<void(std::optional<PickHit>)> one; // run on the UI thread
    std::function<void(std::vector<PickHit>)> many;  // run on the UI thread
};

// Everything the renderer needs for one frame. Items are sorted by id.
struct FrameSnapshot {
    std::vector<SceneItem> items;
    Camera camera{};
    std::shared_ptr<const Theme> theme;
    TierPreset preset{};
    double devicePixelRatio = 1.0;
    // Section and draft analysis (T-023): validated by the front (unit normal / pull, finite values).
    std::optional<SectionPlane> section;
    std::optional<DraftAnalysis> draft;
    std::vector<PickHit> selected; // T-015
    std::optional<PickHit> hover;  // T-015
    bool backdropEnabled = false;  // T-016: rhi/backdrop_pass.h blurs the frame
    // Bumped by any change to items, transforms, visibility (sceneGeneration) or the camera (cameraGeneration);
    // T-015 re-renders its ID buffer only when one of them moved.
    std::uint64_t sceneGeneration = 0;
    std::uint64_t cameraGeneration = 0;
    std::vector<CaptureRequest> captures; // moved out of the front at each sync
    std::vector<PickRequest> picks;       // moved out of the front at each sync (T-015)
    // False when only picks asked for this frame: the picture is unchanged, so the renderer may keep the image it
    // drew last instead of drawing it again (T-015).
    bool redraw = true;
};

// Render thread to UI thread: closures queued here run on the UI thread, in order. post() is thread-safe; the
// front detaches the wake-up hook before it dies, so a late post from the render thread is simply dropped.
class Outbox {
public:
    void post(std::function<void()> onUiThread);
    void attach(std::function<void()> wake);
    void detach();
    void drain(); // UI thread

private:
    std::mutex mutex_;
    std::vector<std::function<void()>> queue_;
    std::function<void()> wake_;
    bool wakePending_ = false;
};

// Written by the renderer after each frame, read by the front at any time.
struct SharedStatus {
    mutable std::mutex mutex;
    IViewport::FrameStats stats{0, 0, 0, 0};
    ViewportInfo info;
    IViewport::Backdrop backdrop{nullptr, 0, 0, false, 0}; // T-016: the latest blur; rhiTexture is always null here
};

class ViewportFrontend final : public IRenderViewport {
public:
    // `requestUpdate` runs on the UI thread whenever the picture changed or a frame was requested.
    explicit ViewportFrontend(std::function<void()> requestUpdate);
    ~ViewportFrontend() override;
    ViewportFrontend(const ViewportFrontend&) = delete;
    ViewportFrontend& operator=(const ViewportFrontend&) = delete;

    ItemId addMesh(std::shared_ptr<const kernel::Mesh>, const Xform&, MeshStyle) override;
    void updateMesh(ItemId, std::shared_ptr<const kernel::Mesh>, std::optional<Box3> dirty) override;
    ItemId addBackplot(std::shared_ptr<const sim::Backplot>) override;
    void setBackplotRange(ItemId, std::uint32_t firstMove, std::uint32_t lastMove) override;
    ItemId addLines(std::span<const Vec3> segments, Token colour, float widthPx, bool dashed) override;
    ItemId addToolGlyph(std::span<const Vec2> revolvedProfile, Token colour) override;
    void setTransform(ItemId, const Xform&) override;
    void setStyle(ItemId, MeshStyle) override;
    void setVisible(ItemId, bool) override;
    void remove(ItemId) override;

    void setHighlight(std::span<const PickHit> selected, std::optional<PickHit> hover) override;
    void setCamera(const Camera&) override;
    Camera camera() const override;
    void setSection(std::optional<SectionPlane>) override;
    void setTheme(std::shared_ptr<const Theme>) override;
    void setPreset(const TierPreset&) override;

    void pick(int x, int y, std::uint8_t filterMask, std::function<void(std::optional<PickHit>)>) override;
    void pickRect(int x0, int y0, int x1, int y1, std::uint8_t filterMask,
                  std::function<void(std::vector<PickHit>)>) override;

    void requestFrame() override;
    void captureImage(int w, int h, std::function<void(std::vector<std::byte> png)>) override;
    void captureImageInTheme(int w, int h, std::shared_ptr<const Theme> theme,
                             std::function<void(std::vector<std::byte> png)>) override;
    FrameStats lastFrameStats() const override;

    Backdrop blurredBackdropTexture() override;
    void setBackdropEnabled(bool) override;

    std::optional<Box3> sceneBounds() const override;
    ViewportInfo info() const override;
    void setDevicePixelRatio(double) override;
    std::optional<SectionPlane> section() const override { return state_.section; }
    void setDraftAnalysis(std::optional<DraftAnalysis>) override;
    std::optional<DraftAnalysis> draftAnalysis() const override { return state_.draft; }
    void setPickMode(ItemId, PickMode) override;
    PickMode pickMode(ItemId) const override;
    ItemId addStock(std::shared_ptr<const kernel::Mesh>, const Xform&, MeshStyle, StockShading) override;
    void setStockShading(ItemId, StockShading) override;
    std::uint64_t triangleCount(ItemId) const override;

    // For the hosts. takeSnapshot runs on the render thread while the UI thread is blocked (Qt Quick), or on the
    // UI thread (offscreen).
    FrameSnapshot takeSnapshot();
    const std::shared_ptr<Outbox>& outbox() const { return outbox_; }
    const std::shared_ptr<SharedStatus>& status() const { return status_; }

private:
    SceneItem* find(ItemId);
    void changed(bool scene, bool camera);
    // T-023: starts the closed-mesh checks a capped section needs (no-op without one).
    void startClosedChecks();

    std::function<void()> requestUpdate_;
    std::map<ItemId, SceneItem> items_;
    ItemId nextId_ = 1;
    std::uint64_t nextGeometryGeneration_ = 1;
    FrameSnapshot state_; // everything except items and captures
    std::vector<CaptureRequest> captures_;
    std::vector<PickRequest> picks_;
    bool pictureDirty_ = true; // something other than a pick changed since the last snapshot
    std::shared_ptr<Outbox> outbox_;
    std::shared_ptr<SharedStatus> status_;
    std::unique_ptr<QObject> uiContext_; // receives the outbox's queued wake-ups on the UI thread
    std::shared_ptr<std::atomic<int>> closedChecksPending_ = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> pathLodsPending_ = std::make_shared<std::atomic<int>>(0); // T-038
};

// Validates a mesh for display: sizes of the arrays agree and every index and range is in bounds.
Status validateMesh(const kernel::Mesh&);
// Bounds of a mesh's triangles and edge polylines; nullopt when it has neither.
std::optional<Box3> meshBounds(const kernel::Mesh&);
// Bounds of `box` after the rigid transform.
Box3 transformBox(const Box3& box, const Xform&);
// Whether an item is drawn into the picking ID buffer, and whether the renderer needs it at all this frame (drawn,
// or hidden but pickable).
inline bool itemPickable(const SceneItem& item)
{
    return item.pickMode == PickMode::Always || (item.visible && item.pickMode == PickMode::WhenVisible);
}
inline bool itemNeeded(const SceneItem& item)
{
    return item.visible || item.pickMode == PickMode::Always;
}

} // namespace ray::render::detail
