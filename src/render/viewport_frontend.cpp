// The UI-thread half of every viewport (task T-014). See viewport_frontend.h.
#include "viewport_frontend.h"

#include "lines.h"
#include "section.h"

#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QThreadPool>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace ray::render::detail {

namespace {

constexpr int kMaxCaptureSide = 16384;

void grow(Box3& box, double x, double y, double z)
{
    box.min.x = std::min(box.min.x, x);
    box.min.y = std::min(box.min.y, y);
    box.min.z = std::min(box.min.z, z);
    box.max.x = std::max(box.max.x, x);
    box.max.y = std::max(box.max.y, y);
    box.max.z = std::max(box.max.z, z);
}

// A face colour from data is clamped to 0..1; one that is not a number is dropped (the face token is used).
void sanitize(MeshStyle& style)
{
    if (!style.faceColor)
        return;
    Color& c = *style.faceColor;
    if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b)) {
        style.faceColor.reset();
        return;
    }
    c.r = std::clamp(c.r, 0.0f, 1.0f);
    c.g = std::clamp(c.g, 0.0f, 1.0f);
    c.b = std::clamp(c.b, 0.0f, 1.0f);
    c.a = 1.0f; // MeshStyle::opacity is what applies
}

// `v` scaled to unit length; nullopt for a zero, tiny or non-finite vector.
std::optional<Vec3> unitVector(Vec3 v)
{
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!std::isfinite(len) || len < 1e-12)
        return std::nullopt;
    return Vec3{v.x / len, v.y / len, v.z / len};
}

Box3 emptyBox()
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    return Box3{Vec3{inf, inf, inf}, Vec3{-inf, -inf, -inf}};
}

std::optional<Box3> pointsBounds(const std::vector<float>& xyz)
{
    if (xyz.size() < 3)
        return std::nullopt;
    Box3 box = emptyBox();
    for (std::size_t i = 0; i + 2 < xyz.size(); i += 3)
        grow(box, xyz[i], xyz[i + 1], xyz[i + 2]);
    return box;
}

} // namespace

// ---------- Outbox ----------
void Outbox::post(std::function<void()> onUiThread)
{
    std::lock_guard lock(mutex_);
    if (!wake_)
        return; // the front is gone
    queue_.push_back(std::move(onUiThread));
    if (!wakePending_) {
        wakePending_ = true;
        wake_();
    }
}

void Outbox::attach(std::function<void()> wake)
{
    std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
}

void Outbox::detach()
{
    std::lock_guard lock(mutex_);
    wake_ = nullptr;
    queue_.clear();
}

void Outbox::drain()
{
    std::vector<std::function<void()>> work;
    {
        std::lock_guard lock(mutex_);
        work.swap(queue_);
        wakePending_ = false;
    }
    for (auto& fn : work) {
        if (fn)
            fn();
    }
}

// ---------- Mesh helpers ----------
Status validateMesh(const kernel::Mesh& mesh)
{
    const auto bad = [](std::string what) {
        return Status(tl::unexpected(Error{ErrorCode::InvalidArgument, "Mesh cannot be displayed: " + what, {}}));
    };
    if (mesh.positions.size() % 3 != 0)
        return bad("positions is not a list of xyz triples");
    if (mesh.normals.size() != mesh.positions.size())
        return bad("normals and positions differ in size");
    if (mesh.indices.size() % 3 != 0)
        return bad("indices is not a list of triangles");
    if (mesh.edgePositions.size() % 3 != 0)
        return bad("edgePositions is not a list of xyz triples");
    const std::size_t vertexCount = mesh.positions.size() / 3;
    for (const std::uint32_t index : mesh.indices) {
        if (index >= vertexCount)
            return bad("an index is out of range");
    }
    for (const kernel::FaceRange& face : mesh.faces) {
        if (std::size_t{face.firstIndex} + face.indexCount > mesh.indices.size() || face.indexCount % 3 != 0)
            return bad("a face range is out of bounds");
    }
    const std::size_t edgeVertexCount = mesh.edgePositions.size() / 3;
    for (const kernel::EdgeRange& edge : mesh.edges) {
        if (std::size_t{edge.firstVertex} + edge.vertexCount > edgeVertexCount)
            return bad("an edge range is out of bounds");
    }
    return {};
}

std::optional<Box3> meshBounds(const kernel::Mesh& mesh)
{
    std::optional<Box3> faces = mesh.indices.empty() ? std::nullopt : pointsBounds(mesh.positions);
    std::optional<Box3> edges = pointsBounds(mesh.edgePositions);
    if (!faces)
        return edges;
    if (edges) {
        grow(*faces, edges->min.x, edges->min.y, edges->min.z);
        grow(*faces, edges->max.x, edges->max.y, edges->max.z);
    }
    return faces;
}

Box3 transformBox(const Box3& box, const Xform& xf)
{
    Box3 out = emptyBox();
    for (int corner = 0; corner < 8; ++corner) {
        const double x = (corner & 1) ? box.max.x : box.min.x;
        const double y = (corner & 2) ? box.max.y : box.min.y;
        const double z = (corner & 4) ? box.max.z : box.min.z;
        const double* m = xf.m;
        grow(out, m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
             m[8] * x + m[9] * y + m[10] * z + m[11]);
    }
    return out;
}

// ---------- ViewportFrontend ----------
ViewportFrontend::ViewportFrontend(std::function<void()> requestUpdate)
    : requestUpdate_(std::move(requestUpdate)), outbox_(std::make_shared<Outbox>()),
      status_(std::make_shared<SharedStatus>()), uiContext_(std::make_unique<QObject>())
{
    state_.camera = defaultCamera();
    state_.preset = presetFor(Tier::Low);
    QObject* context = uiContext_.get();
    std::weak_ptr<Outbox> weak = outbox_;
    outbox_->attach([context, weak] {
        QMetaObject::invokeMethod(
            context,
            [weak] {
                if (auto box = weak.lock())
                    box->drain();
            },
            Qt::QueuedConnection);
    });
}

ViewportFrontend::~ViewportFrontend()
{
    outbox_->detach();  // after this no render-thread post reaches uiContext_
    uiContext_.reset(); // drops wake-ups still queued for it
}

SceneItem* ViewportFrontend::find(ItemId id)
{
    const auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

void ViewportFrontend::changed(bool scene, bool camera)
{
    if (scene)
        ++state_.sceneGeneration;
    if (camera)
        ++state_.cameraGeneration;
    pictureDirty_ = true;
    if (requestUpdate_)
        requestUpdate_();
}

ItemId ViewportFrontend::addMesh(std::shared_ptr<const kernel::Mesh> mesh, const Xform& xf, MeshStyle style)
{
    if (!mesh) {
        ray::log(LogLevel::Warn, "render", "addMesh: null mesh ignored");
        return 0;
    }
    if (Status ok = validateMesh(*mesh); !ok) {
        ray::log(LogLevel::Warn, "render", "addMesh: " + ok.error().message);
        return 0;
    }
    SceneItem item;
    item.id = nextId_++;
    item.kind = ItemKind::Mesh;
    item.localBounds = meshBounds(*mesh);
    item.mesh = std::move(mesh);
    item.xform = xf;
    sanitize(style);
    item.style = style;
    item.geometryGeneration = nextGeometryGeneration_++;
    item.closed = std::make_shared<ClosedCheck>();
    const ItemId id = item.id;
    items_.emplace(id, std::move(item));
    startClosedChecks();
    changed(true, false);
    return id;
}

void ViewportFrontend::updateMesh(ItemId id, std::shared_ptr<const kernel::Mesh> mesh, std::optional<Box3> dirty)
{
    // `dirty` would let the renderer patch only part of the buffers (simulation stock, T-07x). T-014 uploads the
    // whole mesh again.
    (void)dirty;
    SceneItem* item = find(id);
    if (!item || item->kind != ItemKind::Mesh || !mesh)
        return;
    if (Status ok = validateMesh(*mesh); !ok) {
        ray::log(LogLevel::Warn, "render", "updateMesh: " + ok.error().message);
        return;
    }
    item->localBounds = meshBounds(*mesh);
    item->mesh = std::move(mesh);
    item->geometryGeneration = nextGeometryGeneration_++;
    item->closed = std::make_shared<ClosedCheck>();
    startClosedChecks();
    changed(true, false);
}

ItemId ViewportFrontend::addBackplot(std::shared_ptr<const sim::Backplot> backplot)
{
    // T-038 (lines.h): checked in one pass over the moves and vertices, then shared with the renderer as it is.
    if (!backplot) {
        ray::log(LogLevel::Warn, "render", "addBackplot: null backplot ignored");
        return 0;
    }
    if (Status ok = validateBackplot(*backplot); !ok) {
        ray::log(LogLevel::Warn, "render", "addBackplot: " + ok.error().message);
        return 0;
    }
    SceneItem item;
    item.id = nextId_++;
    item.kind = ItemKind::Backplot;
    item.localBounds = backplotBounds(*backplot);
    item.style = MeshStyle{Token::PathCut, Token::PathCut, 1.0f, true, false};
    item.geometryGeneration = nextGeometryGeneration_++;
    if (backplot->lineVertices.size() >= kLodMinVertices) {
        // Levels of detail on Qt's thread pool (about 0.1 s for 2 M segments); until they are ready the full path
        // is drawn. The answer asks for a new frame (and a new ID buffer, which draws the same level).
        auto lods = std::make_shared<PathLodSet>();
        item.lods = lods;
        pathLodsPending_->fetch_add(1);
        std::weak_ptr<Outbox> outbox = outbox_;
        QThreadPool::globalInstance()->start([bp = backplot, lods, pending = pathLodsPending_, outbox, this] {
            lods->levels = buildPathLods(*bp);
            lods->ready.store(true, std::memory_order_release);
            pending->fetch_sub(1);
            if (auto box = outbox.lock())
                box->post([this] { changed(true, false); }); // dropped once this front is gone (Outbox::detach)
        });
    }
    item.backplot = std::move(backplot);
    const ItemId id = item.id;
    items_.emplace(id, std::move(item));
    changed(true, false);
    return id;
}

void ViewportFrontend::setBackplotRange(ItemId id, std::uint32_t firstMove, std::uint32_t lastMove)
{
    // Scrubbing: only the instance range of the draw changes (no upload). The picking ID buffer holds the drawn range,
    // so it counts as a scene change.
    SceneItem* item = find(id);
    if (!item || item->kind != ItemKind::Backplot || (item->firstMove == firstMove && item->lastMove == lastMove))
        return;
    item->firstMove = firstMove;
    item->lastMove = lastMove;
    changed(true, false);
}

ItemId ViewportFrontend::addLines(std::span<const Vec3> segments, Token colour, float widthPx, bool dashed)
{
    if (segments.size() < 2 || !(widthPx > 0) || colour >= Token::Count)
        return 0;
    auto lines = std::make_shared<LineSet>();
    const std::size_t count = segments.size() / 2 * 2; // an odd last point has no partner
    lines->points.reserve(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        lines->points.push_back(static_cast<float>(segments[i].x));
        lines->points.push_back(static_cast<float>(segments[i].y));
        lines->points.push_back(static_cast<float>(segments[i].z));
    }
    lines->widthPx = widthPx;
    lines->dashed = dashed;
    SceneItem item;
    item.id = nextId_++;
    item.kind = ItemKind::Lines;
    item.localBounds = pointsBounds(lines->points);
    item.lines = std::move(lines);
    item.style = MeshStyle{colour, colour, 1.0f, true, false};
    item.geometryGeneration = nextGeometryGeneration_++;
    const ItemId id = item.id;
    items_.emplace(id, std::move(item));
    changed(true, false);
    return id;
}

ItemId ViewportFrontend::addToolGlyph(std::span<const Vec2> revolvedProfile, Token colour)
{
    // T-038: the revolved profile becomes an ordinary shaded mesh (lines.h, revolveProfile), uploaded once; moving the
    // tool is setTransform, which changes a uniform only. It does not answer picks (PickMode::Never), so it never
    // stands between the cursor and the toolpath under it.
    if (colour >= Token::Count)
        return 0;
    Result<kernel::Mesh> mesh = revolveProfile(revolvedProfile);
    if (!mesh) {
        ray::log(LogLevel::Warn, "render", "addToolGlyph: " + mesh.error().message);
        return 0;
    }
    const ItemId id = addMesh(std::make_shared<const kernel::Mesh>(std::move(*mesh)), Xform::identity(),
                              MeshStyle{colour, colour, 1.0f, false, false});
    if (SceneItem* item = find(id))
        item->pickMode = PickMode::Never;
    return id;
}

void ViewportFrontend::setTransform(ItemId id, const Xform& xf)
{
    if (SceneItem* item = find(id)) {
        item->xform = xf;
        changed(true, false);
    }
}

void ViewportFrontend::setStyle(ItemId id, MeshStyle style)
{
    sanitize(style);
    if (SceneItem* item = find(id); item && style.face < Token::Count && style.edge < Token::Count) {
        // Whether edges are drawn changes what the picking ID buffer holds, so a style change counts as a scene change.
        item->style = style;
        changed(true, false);
    }
}

void ViewportFrontend::setVisible(ItemId id, bool visible)
{
    if (SceneItem* item = find(id); item && item->visible != visible) {
        item->visible = visible;
        changed(true, false);
    }
}

void ViewportFrontend::setPickMode(ItemId id, PickMode mode)
{
    if (SceneItem* item = find(id); item && item->pickMode != mode) {
        item->pickMode = mode;
        changed(true, false); // the ID buffer holds other items now (and a hidden item's highlights may show)
    }
}

PickMode ViewportFrontend::pickMode(ItemId id) const
{
    const auto it = items_.find(id);
    return it == items_.end() ? PickMode::WhenVisible : it->second.pickMode;
}

void ViewportFrontend::remove(ItemId id)
{
    if (items_.erase(id) > 0)
        changed(true, false);
}

void ViewportFrontend::setHighlight(std::span<const PickHit> selected, std::optional<PickHit> hover)
{
    // Drawn in the main pass (SceneRenderer::drawHighlights); the ID buffer does not depend on it.
    state_.selected.assign(selected.begin(), selected.end());
    state_.hover = hover;
    changed(false, false);
}

void ViewportFrontend::setCamera(const Camera& camera)
{
    state_.camera = camera;
    changed(false, true);
}

Camera ViewportFrontend::camera() const
{
    return state_.camera;
}

void ViewportFrontend::setSection(std::optional<SectionPlane> section)
{
    // T-023: the renderer clips faces, edges, lines and the ID buffer at the plane and caps closed bodies. Only
    // uniforms change, so dragging the plane re-uploads nothing; the picture and the ID buffer must be drawn again.
    if (section) {
        const std::optional<Vec3> n = unitVector(section->normal);
        const Vec3& o = section->origin;
        if (!n || !std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.z))
            section.reset();
        else
            section->normal = *n;
    }
    const auto same = [](const std::optional<SectionPlane>& a, const std::optional<SectionPlane>& b) {
        if (a.has_value() != b.has_value())
            return false;
        return !a || (a->origin.x == b->origin.x && a->origin.y == b->origin.y && a->origin.z == b->origin.z &&
                      a->normal.x == b->normal.x && a->normal.y == b->normal.y && a->normal.z == b->normal.z &&
                      a->cap == b->cap);
    };
    if (same(section, state_.section))
        return;
    state_.section = section;
    startClosedChecks();
    changed(true, false); // what the picking ID buffer holds changes with it
}

void ViewportFrontend::startClosedChecks()
{
    // Caps need to know which bodies are watertight (section.h): O(n log n) per mesh, about 60 ms for 500k
    // triangles, so it runs on Qt's thread pool. Until it is known the body is drawn uncapped; the answer asks for a
    // new frame (and a new ID buffer, which holds the caps).
    if (!state_.section || !state_.section->cap)
        return;
    for (auto& [id, item] : items_) {
        if (item.kind != ItemKind::Mesh || !item.mesh || !item.closed || item.closed->started.exchange(true))
            continue;
        closedChecksPending_->fetch_add(1);
        std::weak_ptr<Outbox> outbox = outbox_;
        QThreadPool::globalInstance()->start(
            [mesh = item.mesh, check = item.closed, pending = closedChecksPending_, outbox, this] {
                check->state.store(isClosedMesh(*mesh) ? 1 : 0);
                pending->fetch_sub(1);
                if (auto box = outbox.lock())
                    box->post([this] { changed(true, false); }); // dropped once this front is gone (Outbox::detach)
            });
    }
}

void ViewportFrontend::setDraftAnalysis(std::optional<DraftAnalysis> draft)
{
    if (draft) {
        const std::optional<Vec3> p = unitVector(draft->pull);
        if (!p || !std::isfinite(draft->thresholdDeg)) {
            draft.reset();
        } else {
            draft->pull = *p;
            draft->thresholdDeg = std::clamp(draft->thresholdDeg, 0.0, 89.0);
        }
    }
    const auto same = [](const std::optional<DraftAnalysis>& a, const std::optional<DraftAnalysis>& b) {
        if (a.has_value() != b.has_value())
            return false;
        return !a || (a->pull.x == b->pull.x && a->pull.y == b->pull.y && a->pull.z == b->pull.z &&
                      a->thresholdDeg == b->thresholdDeg);
    };
    if (same(draft, state_.draft))
        return;
    state_.draft = draft;
    changed(false, false); // colours only: the ID buffer stays valid
}

void ViewportFrontend::setTheme(std::shared_ptr<const Theme> theme)
{
    state_.theme = std::move(theme);
    changed(false, false);
}

void ViewportFrontend::setPreset(const TierPreset& preset)
{
    // Section caps (T-023) depend on the preset and are in the picking ID buffer.
    const bool capsChanged =
        preset.sectionCaps != state_.preset.sectionCaps || preset.transparency != state_.preset.transparency;
    state_.preset = preset;
    changed(capsChanged && state_.section.has_value(), false);
}

void ViewportFrontend::pick(int x, int y, std::uint8_t filterMask, std::function<void(std::optional<PickHit>)> done)
{
    // Queued like a capture; the pick pass (rhi/pick_pass.cpp) answers through the outbox. A pick alone does not
    // make the picture dirty, so the renderer only reads back the ID buffer (and draws it if it is stale).
    if (!done)
        return;
    PickRequest request;
    request.x0 = request.x1 = x;
    request.y0 = request.y1 = y;
    request.filterMask = filterMask;
    request.one = std::move(done);
    picks_.push_back(std::move(request));
    if (requestUpdate_)
        requestUpdate_();
}

void ViewportFrontend::pickRect(int x0, int y0, int x1, int y1, std::uint8_t filterMask,
                                std::function<void(std::vector<PickHit>)> done)
{
    if (!done)
        return;
    PickRequest request;
    request.x0 = std::min(x0, x1);
    request.y0 = std::min(y0, y1);
    request.x1 = std::max(x0, x1);
    request.y1 = std::max(y0, y1);
    request.rect = true;
    request.filterMask = filterMask;
    request.many = std::move(done);
    picks_.push_back(std::move(request));
    if (requestUpdate_)
        requestUpdate_();
}

void ViewportFrontend::requestFrame()
{
    pictureDirty_ = true; // the caller wants the picture drawn, even if nothing it knows of changed
    if (requestUpdate_)
        requestUpdate_();
}

void ViewportFrontend::captureImage(int w, int h, std::function<void(std::vector<std::byte> png)> done)
{
    captureImageInTheme(w, h, nullptr, std::move(done));
}

void ViewportFrontend::captureImageInTheme(int w, int h, std::shared_ptr<const Theme> theme,
                                           std::function<void(std::vector<std::byte> png)> done)
{
    if (!done)
        return;
    if (w <= 0 || h <= 0 || w > kMaxCaptureSide || h > kMaxCaptureSide) {
        ray::log(LogLevel::Warn, "render", "captureImage: bad size " + std::to_string(w) + "x" + std::to_string(h));
        outbox_->post([done = std::move(done)] { done({}); });
        return;
    }
    captures_.push_back(CaptureRequest{w, h, std::move(done), std::move(theme)});
    changed(false, false);
}

IViewport::FrameStats ViewportFrontend::lastFrameStats() const
{
    std::lock_guard lock(status_->mutex);
    return status_->stats;
}

IViewport::Backdrop ViewportFrontend::blurredBackdropTexture()
{
    // The UI thread gets the description only; the handle is the render thread's (render.h, viewport_rhi.h).
    std::lock_guard lock(status_->mutex);
    Backdrop b = status_->backdrop;
    b.rhiTexture = nullptr;
    return b;
}

void ViewportFrontend::setBackdropEnabled(bool on)
{
    if (on == state_.backdropEnabled)
        return;
    state_.backdropEnabled = on;
    // The picture is the same: the next frame blurs the image the viewport already holds (or drops the blur), it
    // does not draw the scene again.
    if (requestUpdate_)
        requestUpdate_();
}

std::optional<Box3> ViewportFrontend::sceneBounds() const
{
    std::optional<Box3> all;
    for (const auto& [id, item] : items_) {
        if (!item.visible || !item.localBounds)
            continue;
        const Box3 box = transformBox(*item.localBounds, item.xform);
        if (!all) {
            all = box;
        } else {
            grow(*all, box.min.x, box.min.y, box.min.z);
            grow(*all, box.max.x, box.max.y, box.max.z);
        }
    }
    return all;
}

ViewportInfo ViewportFrontend::info() const
{
    std::lock_guard lock(status_->mutex);
    ViewportInfo info = status_->info;
    info.closedChecksPending = std::uint32_t(std::max(0, closedChecksPending_->load()));
    info.pathLodsPending = std::uint32_t(std::max(0, pathLodsPending_->load()));
    return info;
}

void ViewportFrontend::setDevicePixelRatio(double ratio)
{
    if (ratio > 0 && std::isfinite(ratio) && ratio != state_.devicePixelRatio) {
        state_.devicePixelRatio = ratio;
        changed(true, false); // line widths scale with it, and so does what the picking ID buffer holds
    }
}

FrameSnapshot ViewportFrontend::takeSnapshot()
{
    FrameSnapshot snap;
    snap.camera = state_.camera;
    snap.theme = state_.theme;
    snap.preset = state_.preset;
    snap.devicePixelRatio = state_.devicePixelRatio;
    snap.section = state_.section;
    snap.draft = state_.draft;
    snap.selected = state_.selected;
    snap.hover = state_.hover;
    snap.backdropEnabled = state_.backdropEnabled;
    snap.sceneGeneration = state_.sceneGeneration;
    snap.cameraGeneration = state_.cameraGeneration;
    snap.items.reserve(items_.size());
    for (const auto& [id, item] : items_)
        snap.items.push_back(item);
    snap.captures = std::move(captures_);
    captures_.clear();
    snap.picks = std::move(picks_);
    picks_.clear();
    snap.redraw = pictureDirty_;
    pictureDirty_ = false;
    return snap;
}

} // namespace ray::render::detail
