#include "viewport_item.h"

#include "app_context.h"
#include "scene_controller.h"
#include "selection_model.h"
#include "theme_object.h"
#include "viewport_navigation.h"

#include <ray_render/viewport_rhi.h>

#include <QtCore/QLineF>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QNativeGestureEvent>
#include <QtGui/QStyleHints>
#include <QtGui/QWheelEvent>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGNode>
#include <QtQuick/QSGRendererInterface>
#include <QtQuick/QSGTexture>
#include <QtQuick/QSGTextureMaterial>

#include <algorithm>
#include <cmath>

namespace ray::ui {

namespace {

constexpr double kOrbitDegPerPx = 0.4;
constexpr double kKeyOrbitDeg = 15.0;
constexpr double kKeyPanFraction = 0.1;  // of the view height
constexpr double kWheelStep = 1.15;      // zoom factor per wheel notch (120 units)
constexpr double kPinchPerUnit = 1.0025; // Ctrl+wheel (Windows sends touchpad pinch this way): per angle unit
constexpr double kDragZoomPerPx = 0.01;
constexpr double kTouchpadOrbitDegPerPx = 0.25;
constexpr int kTransitionMs = 220;
constexpr int kTickMs = 16;
constexpr double kHoverAnchorPx = 24.0; // a hover hit this close to the cursor gives the zoom depth

QVector3D toQ(const Vec3& v)
{
    return QVector3D(float(v.x), float(v.y), float(v.z));
}

double smoothstep(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Box3 unitBox()
{
    return Box3{Vec3{-50, -50, -50}, Vec3{50, 50, 50}};
}

} // namespace

// ---------- render thread ----------
class ViewportRenderer final : public QQuickRhiItemRenderer {
public:
    ~ViewportRenderer() override
    {
        publish(nullptr); // before anything is freed: glass panels stop sampling (T-016)
        renderer_.releaseResources();
    }

protected:
    void initialize(QRhiCommandBuffer*) override { renderer_.initialize(rhi(), colorTexture()); }
    void synchronize(QQuickRhiItem* item) override
    {
        auto* view = static_cast<ViewportItem*>(item);
        if (view->front_)
            renderer_.synchronize(*view->front_);
        if (view->backdropLink_ != link_) {
            publish(nullptr);
            link_ = view->backdropLink_;
        }
    }
    void render(QRhiCommandBuffer* cb) override
    {
        const bool more = renderer_.render(cb);
        // The blur of this frame, for the glass panels' nodes, which the scene graph prepares next (T-016).
        const render::IViewport::Backdrop b = renderer_.backdrop();
        publish(&b);
        if (more)
            update(); // a read-back waits for the GPU: one more frame soon
    }

private:
    void publish(const render::IViewport::Backdrop* b)
    {
        if (!link_)
            return;
        std::lock_guard lock(link_->mutex);
        link_->texture = b ? static_cast<QRhiTexture*>(b->rhiTexture) : nullptr;
        link_->size = link_->texture ? QSize(b->width, b->height) : QSize();
        link_->yUp = b && b->yUp;
        link_->generation = b ? b->generation : 0;
    }

    render::HostedViewportRenderer renderer_;
    std::shared_ptr<BackdropLink> link_;
};

// A QRhiTexture shown in the scene graph WITHOUT owning it (T-016): the viewport's renderer owns the backdrop.
// QQuickWindow::createTextureFromRhiTexture would take ownership (render.h).
class BorrowedTexture final : public QSGTexture {
public:
    void set(QRhiTexture* texture, QSize size)
    {
        texture_ = texture;
        size_ = size;
    }
    qint64 comparisonKey() const override { return qint64(reinterpret_cast<quintptr>(texture_)); }
    QRhiTexture* rhiTexture() const override { return texture_; }
    QSize textureSize() const override { return size_; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

private:
    QRhiTexture* texture_ = nullptr;
    QSize size_;
};

// GlassBackdrop's node: preprocess() fetches the current backdrop from the link right before the scene graph
// draws; without one, its subtree is blocked (opacity 0), so nothing samples a missing texture.
class GlassBackdropNode final : public QSGNode {
public:
    GlassBackdropNode()
    {
        setFlag(UsePreprocess);
        texture_.setFiltering(QSGTexture::Linear);
        texture_.setHorizontalWrapMode(QSGTexture::ClampToEdge);
        texture_.setVerticalWrapMode(QSGTexture::ClampToEdge);
        material_.setTexture(&texture_);
        material_.setFiltering(QSGTexture::Linear);
        material_.setHorizontalWrapMode(QSGTexture::ClampToEdge);
        material_.setVerticalWrapMode(QSGTexture::ClampToEdge);
        geometry_.setDrawingMode(QSGGeometry::DrawTriangles);
        shape_.setGeometry(&geometry_);
        shape_.setMaterial(&material_);
        shape_.setOpaqueMaterial(&opaqueMaterial_);
        opaqueMaterial_.setTexture(&texture_);
        opaqueMaterial_.setFiltering(QSGTexture::Linear);
        opaqueMaterial_.setHorizontalWrapMode(QSGTexture::ClampToEdge);
        opaqueMaterial_.setVerticalWrapMode(QSGTexture::ClampToEdge);
        opacity_.setOpacity(0);
        opacity_.appendChildNode(&shape_);
        appendChildNode(&opacity_);
        // The children are members: they must not be deleted by the parent.
        opacity_.setFlag(OwnedByParent, false);
        shape_.setFlag(OwnedByParent, false);
    }
    ~GlassBackdropNode() override
    {
        opacity_.removeChildNode(&shape_);
        removeChildNode(&opacity_);
    }

    // Sync phase (UI thread blocked): what to draw.
    void setShape(QSizeF size, qreal radius, QRectF uv, std::shared_ptr<BackdropLink> link)
    {
        if (size != size_ || radius != radius_ || uv != uv_) {
            size_ = size;
            radius_ = radius;
            uv_ = uv;
            shapeDirty_ = true;
        }
        link_ = std::move(link);
    }

    void preprocess() override
    {
        QRhiTexture* texture = nullptr;
        QSize size;
        bool yUp = false;
        if (link_) {
            std::lock_guard lock(link_->mutex);
            texture = link_->texture;
            size = link_->size;
            yUp = link_->yUp;
        }
        if (texture != texture_.rhiTexture() || size != texture_.textureSize()) {
            texture_.set(texture, size);
            shape_.markDirty(DirtyMaterial);
        }
        if (yUp != yUp_ || shapeDirty_) {
            yUp_ = yUp;
            build();
        }
        const qreal opacity = texture != nullptr && size_.width() > 0 && size_.height() > 0 ? 1.0 : 0.0;
        if (opacity != opacity_.opacity())
            opacity_.setOpacity(opacity); // 0 blocks the subtree
    }

private:
    // A rounded rectangle as a triangle fan around its centre, inset half a pixel so its hard edge stays under the
    // panel's anti-aliased outline; texture coordinates follow the panel's place in the viewport.
    void build()
    {
        shapeDirty_ = false;
        constexpr int kArc = 8; // segments per corner
        constexpr qreal kInset = 0.5;
        const qreal w = std::max<qreal>(0, size_.width() - 2 * kInset);
        const qreal h = std::max<qreal>(0, size_.height() - 2 * kInset);
        const qreal r = std::clamp<qreal>(radius_ - kInset, 0, std::min(w, h) / 2);
        const int ring = 4 * (kArc + 1);
        geometry_.allocate(ring + 1, ring * 3);
        auto* v = geometry_.vertexDataAsTexturedPoint2D();
        const auto put = [&](int i, qreal x, qreal y) {
            const qreal px = kInset + x, py = kInset + y;
            const qreal u = uv_.left() + px / std::max<qreal>(size_.width(), 1) * uv_.width();
            qreal t = uv_.top() + py / std::max<qreal>(size_.height(), 1) * uv_.height();
            if (yUp_)
                t = 1 - t;
            v[i].set(float(px), float(py), float(u), float(t));
        };
        put(0, w / 2, h / 2);
        const qreal centres[4][2] = {{w - r, r}, {w - r, h - r}, {r, h - r}, {r, r}};
        constexpr qreal kPi = 3.14159265358979323846;
        int i = 1;
        for (int c = 0; c < 4; ++c) {
            for (int k = 0; k <= kArc; ++k, ++i) {
                const qreal a = (c - 1) * kPi / 2 + k * (kPi / 2) / kArc; // clockwise from the top right
                put(i, centres[c][0] + r * std::cos(a), centres[c][1] + r * std::sin(a));
            }
        }
        quint16* idx = geometry_.indexDataAsUShort();
        for (int k = 0; k < ring; ++k) {
            idx[k * 3] = 0;
            idx[k * 3 + 1] = quint16(1 + k);
            idx[k * 3 + 2] = quint16(1 + (k + 1) % ring);
        }
        shape_.markDirty(DirtyGeometry);
    }

    std::shared_ptr<BackdropLink> link_;
    BorrowedTexture texture_;
    QSGTextureMaterial material_;
    QSGOpaqueTextureMaterial opaqueMaterial_;
    QSGGeometry geometry_{QSGGeometry::defaultAttributes_TexturedPoint2D(), 0, 0, QSGGeometry::UnsignedShortType};
    QSGGeometryNode shape_;
    QSGOpacityNode opacity_;
    QSizeF size_;
    qreal radius_ = 0;
    QRectF uv_;
    bool yUp_ = false;
    bool shapeDirty_ = true;
};

// ---------- UI thread ----------
ViewportItem::ViewportItem(QQuickItem* parent) : QQuickRhiItem(parent)
{
    // The renderer brings its own MSAA and depth; the item keeps one sample and no render target of its own.
    setSampleCount(1);
    setAutoRenderTarget(false);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setFlag(ItemAcceptsInputMethod, false);
    rhiAvailable_ = QSGRendererInterface::isApiRhiBased(QQuickWindow::graphicsApi());
    // Told when an ancestor moves or resizes it, so glass panels can map onto it again (T-016).
    setFlag(ItemObservesViewport);
    front_ = render::makeHostedViewport([this] {
        if (!ownRequest_)
            ++viewChanges_;
        update();
    });
    animTimer_.setInterval(kTickMs);
    animTimer_.setTimerType(Qt::PreciseTimer);
    connect(&animTimer_, &QTimer::timeout, this, &ViewportItem::animationTick);
}

ViewportItem::~ViewportItem()
{
    if (backdropContext_ && backdropContext_->backdropSource() == this)
        backdropContext_->setBackdropSource(nullptr);
    if (scene_)
        scene_->detachViewport(front_.get());
}

QQuickRhiItemRenderer* ViewportItem::createRenderer()
{
    return new ViewportRenderer;
}

void ViewportItem::componentComplete()
{
    QQuickRhiItem::componentComplete();
    // The backdrop glass panels sample (T-016). Without QRhi there is none: panels keep their tint.
    QQmlEngine* engine = qmlEngine(this);
    if (engine == nullptr || !rhiAvailable_)
        return;
    if (auto* context = engine->singletonInstance<AppContext*>("RayCad.Ui", "AppContext")) {
        backdropContext_ = context;
        context->setBackdropSource(this);
    }
}

void ViewportItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        ++viewChanges_; // the picture under a resting cursor moves
    emit scenePlacementChanged();
}

void ViewportItem::addBackdropUser()
{
    if (++backdropUsers_ == 1)
        front_->setBackdropEnabled(true);
}

void ViewportItem::removeBackdropUser()
{
    if (backdropUsers_ > 0 && --backdropUsers_ == 0)
        front_->setBackdropEnabled(false);
}

void ViewportItem::setScene(SceneController* scene)
{
    if (scene == scene_)
        return;
    for (const auto& c : sceneConnections_)
        disconnect(c);
    sceneConnections_.clear();
    if (scene_)
        scene_->detachViewport(front_.get());
    scene_ = scene;
    if (scene_) {
        scene_->attachViewport(front_.get());
        sceneConnections_ << connect(scene_, &SceneController::firstMeshesShown, this, [this] {
            // A new model: frame it at once (no transition from wherever the last one was).
            const Box3 box = contentBounds();
            const render::Camera cam =
                render::standardViewCamera(render::StandardView::Iso, box, aspect(), perspective(), camera().fovDeg);
            setCamera(framed(cam, box));
            emit contentChanged();
        });
        sceneConnections_ << connect(scene_, &SceneController::bodiesChanged, this, [this] {
            clearHover();
            emit contentChanged();
        });
        sceneConnections_ << connect(scene_, &SceneController::bodyVisibilityChanged, this,
                                     [this] { emit contentChanged(); });
    }
    emit sceneChanged();
}

void ViewportItem::setSelection(SelectionModel* selection)
{
    if (selection == selection_)
        return;
    for (const auto& c : selectionConnections_)
        disconnect(c);
    selectionConnections_.clear();
    selection_ = selection;
    if (selection_) {
        selectionConnections_ << connect(selection_, &SelectionModel::highlightChanged, this,
                                         &ViewportItem::updateHighlight);
    }
    updateHighlight();
    emit selectionChanged();
}

void ViewportItem::setTheme(ThemeObject* theme)
{
    if (theme == theme_)
        return;
    disconnect(themeConnection_);
    theme_ = theme;
    if (theme_)
        themeConnection_ = connect(theme_, &ThemeObject::themeChanged, this, &ViewportItem::applyTheme);
    applyTheme();
    emit themeChanged();
}

void ViewportItem::applyTheme()
{
    if (theme_ && theme_->valid())
        front_->setTheme(theme_->current());
}

void ViewportItem::setNavigationPreset(int index)
{
    if (index < 0 || index >= static_cast<int>(nav::presets().size()) || index == preset_)
        return;
    preset_ = index;
    emit navigationPresetChanged();
}

QStringList ViewportItem::navigationPresetNames() const
{
    QStringList out;
    for (const nav::Preset& p : nav::presets())
        out << tr(p.title);
    return out;
}

QStringList ViewportItem::navigationPresetDescriptions() const
{
    QStringList out;
    for (const nav::Preset& p : nav::presets())
        out << tr(p.description);
    return out;
}

render::Camera ViewportItem::camera() const
{
    return front_->camera();
}

void ViewportItem::setCamera(const render::Camera& camera)
{
    stopAnimation();
    applyCamera(camera);
}

void ViewportItem::applyCamera(const render::Camera& camera)
{
    front_->setCamera(camera);
    emit cameraChanged();
}

bool ViewportItem::perspective() const
{
    return front_->camera().perspective;
}

void ViewportItem::setPerspective(bool on)
{
    if (on == perspective())
        return;
    setCamera(nav::withProjection(camera(), on));
}

QVector3D ViewportItem::viewRight() const
{
    return toQ(nav::frameOf(camera()).right);
}

QVector3D ViewportItem::viewUp() const
{
    return toQ(nav::frameOf(camera()).up);
}

QVector3D ViewportItem::viewForward() const
{
    return toQ(nav::frameOf(camera()).forward);
}

void ViewportItem::setAnimationsEnabled(bool on)
{
    if (on == animationsEnabled_)
        return;
    animationsEnabled_ = on;
    if (!on && animating()) {
        const render::Camera end = animTo_;
        stopAnimation();
        applyCamera(end);
    }
    emit animatingChanged();
}

bool ViewportItem::hasContent() const
{
    return front_->sceneBounds().has_value();
}

double ViewportItem::aspect() const
{
    return height() > 0 ? width() / height() : 1.0;
}

int ViewportItem::heightPx() const
{
    return std::max(1, static_cast<int>(std::lround(height())));
}

void ViewportItem::setFrameRect(const QRectF& rect)
{
    if (rect == frameRect_)
        return;
    frameRect_ = rect;
    emit frameRectChanged();
}

render::Camera ViewportItem::framed(const render::Camera& cam, const Box3& box) const
{
    const double w = std::max(1.0, width());
    const double h = std::max(1.0, height());
    const QRectF whole(0, 0, w, h);
    QRectF frame = frameRect_.isEmpty() ? whole : frameRect_.intersected(whole);
    if (frame.width() < 16 || frame.height() < 16)
        frame = whole;
    // Fit into a window of the full view: a narrower field of view (perspective) or a taller view (orthographic)
    // whose visible height is the frame's share of the item, then move the frame's centre onto the box.
    const double share = frame.height() / h;
    render::Camera narrow = cam;
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    if (cam.perspective)
        narrow.fovDeg = std::max(1.0, 2.0 * std::atan(std::tan(cam.fovDeg / kRadToDeg * 0.5) * share) * kRadToDeg);
    render::Camera out = render::fitCamera(narrow, box, frame.width() / frame.height());
    out.fovDeg = cam.fovDeg;
    if (!cam.perspective)
        out.orthoHeight = out.orthoHeight / share;
    return render::panCamera(out, frame.center().x() - w / 2, frame.center().y() - h / 2, heightPx());
}

void ViewportItem::frameBox(const Box3& box)
{
    animateTo(framed(camera(), box));
}

bool ViewportItem::inFrame(const std::vector<Vec3>& points, double margin) const
{
    const QRectF whole(0, 0, std::max(1.0, width()), std::max(1.0, height()));
    QRectF frame = frameRect_.isEmpty() ? whole : frameRect_.intersected(whole);
    frame = frame.adjusted(margin, margin, -margin, -margin);
    const render::Camera cam = camera();
    for (const Vec3& p : points) {
        const auto v = nav::project(cam, p, width(), height());
        if (!v || !frame.contains(*v))
            return false;
    }
    return true;
}

Box3 ViewportItem::contentBounds() const
{
    const std::optional<Box3> box = front_->sceneBounds();
    return box ? *box : unitBox();
}

// ---------- transitions ----------
void ViewportItem::animateTo(const render::Camera& camera)
{
    if (!animationsEnabled_ || !isVisible()) {
        setCamera(camera);
        return;
    }
    animFrom_ = this->camera();
    animTo_ = camera;
    animClock_.start();
    if (!animTimer_.isActive()) {
        animTimer_.start();
        emit animatingChanged();
    }
}

void ViewportItem::stopAnimation()
{
    if (animTimer_.isActive()) {
        animTimer_.stop();
        emit animatingChanged();
    }
}

void ViewportItem::animationTick()
{
    const double t = double(animClock_.elapsed()) / double(kTransitionMs);
    if (t >= 1.0) {
        stopAnimation();
        applyCamera(animTo_);
        return;
    }
    applyCamera(nav::interpolate(animFrom_, animTo_, smoothstep(t)));
}

// ---------- navigation commands ----------
void ViewportItem::fitAll()
{
    const std::optional<Box3> box = front_->sceneBounds();
    if (!box)
        return;
    animateTo(framed(camera(), *box));
}

bool ViewportItem::fitIfCovered()
{
    // The bodies shown (not the overlays drawn with them: a triad, a toolpath, its labels).
    std::optional<Box3> box;
    if (scene_) {
        std::vector<render::PickHit> bodies;
        for (int i = 0; i < scene_->bodyCount(); ++i) {
            if (const render::ItemId item = scene_->itemForBody(i); item != 0)
                bodies.push_back(render::PickHit{item, kernel::SubKind::Solid, 0, Vec3{}, 0});
        }
        box = scene_->boundsOf(bodies);
    }
    if (!box)
        box = front_->sceneBounds();
    if (!box)
        return false;
    std::vector<Vec3> corners;
    for (int k = 0; k < 8; ++k)
        corners.push_back(Vec3{(k & 1) ? box->max.x : box->min.x, (k & 2) ? box->max.y : box->min.y,
                               (k & 4) ? box->max.z : box->min.z});
    if (inFrame(corners))
        return false;
    animateTo(framed(camera(), *box));
    return true;
}

void ViewportItem::fitSelection()
{
    std::optional<Box3> box;
    if (scene_ && selection_ && selection_->count() > 0) {
        const std::vector<render::PickHit> hits = selection_->hits();
        box = scene_->boundsOf(hits);
    }
    if (!box) {
        fitAll();
        return;
    }
    animateTo(framed(camera(), *box));
}

bool ViewportItem::setStandardView(const QString& name)
{
    static const std::pair<const char*, render::StandardView> views[] = {
        {"front", render::StandardView::Front}, {"back", render::StandardView::Back},
        {"left", render::StandardView::Left},   {"right", render::StandardView::Right},
        {"top", render::StandardView::Top},     {"bottom", render::StandardView::Bottom},
        {"iso", render::StandardView::Iso}};
    for (const auto& [key, view] : views) {
        if (name.compare(QLatin1String(key), Qt::CaseInsensitive) == 0) {
            const render::Camera cam = camera();
            const Box3 box = contentBounds();
            animateTo(framed(render::standardViewCamera(view, box, aspect(), cam.perspective, cam.fovDeg), box));
            return true;
        }
    }
    return false;
}

void ViewportItem::viewAlong(qreal dx, qreal dy, qreal dz)
{
    const Box3 box = contentBounds();
    animateTo(framed(nav::viewAlong(camera(), Vec3{dx, dy, dz}, box, aspect()), box));
}

void ViewportItem::orbitBy(qreal yawDeg, qreal pitchDeg)
{
    const render::Camera cam = camera();
    Vec3 pivot = cam.target;
    if (const std::optional<Box3> box = front_->sceneBounds())
        pivot = Vec3{(box->min.x + box->max.x) / 2, (box->min.y + box->max.y) / 2, (box->min.z + box->max.z) / 2};
    setCamera(render::orbitCamera(cam, yawDeg, pitchDeg, pivot));
}

void ViewportItem::panBy(qreal dxPx, qreal dyPx)
{
    setCamera(render::panCamera(camera(), dxPx, dyPx, heightPx()));
}

void ViewportItem::zoomAt(qreal factor, qreal x, qreal y)
{
    if (!(factor > 0))
        return;
    setCamera(render::zoomCamera(camera(), factor, zoomAnchor(QPointF(x, y))));
}

Vec3 ViewportItem::zoomAnchor(QPointF itemPos) const
{
    const render::Camera cam = camera();
    const render::Ray ray =
        render::cameraRay(cam, itemPos.x(), itemPos.y(), std::max(1, int(std::lround(width()))), heightPx());
    // The depth: of the model point under (or near) the cursor when the hover pick found one, else of the target.
    Vec3 depthPoint = cam.target;
    if (hoverPoint_ && QLineF(hoverPointAt_, itemPos).length() <= kHoverAnchorPx)
        depthPoint = *hoverPoint_;
    const nav::Frame f = nav::frameOf(cam);
    if (const std::optional<Vec3> p = nav::rayPlane(ray, depthPoint, f.forward))
        return *p;
    return cam.target;
}

QVariantMap ViewportItem::cameraState() const
{
    const render::Camera c = camera();
    return QVariantMap{{QStringLiteral("eye"), toQ(c.eye)},  {QStringLiteral("target"), toQ(c.target)},
                       {QStringLiteral("up"), toQ(c.up)},    {QStringLiteral("perspective"), c.perspective},
                       {QStringLiteral("fovDeg"), c.fovDeg}, {QStringLiteral("orthoHeight"), c.orthoHeight}};
}

// ---------- picking and highlight ----------
std::uint8_t ViewportItem::pickMask() const
{
    return selection_ ? selection_->filterMask() : std::uint8_t(render::PickFilter::Faces);
}

void ViewportItem::updateHighlight()
{
    if (!selection_) {
        front_->setHighlight({}, std::nullopt);
        return;
    }
    const std::vector<render::PickHit> hits = selection_->hits();
    // A highlight changes no pick answer (the picking ID buffer has none).
    ownRequest_ = true;
    front_->setHighlight(hits, selection_->hoverHighlight());
    ownRequest_ = false;
}

void ViewportItem::requestHover(QPointF pos)
{
    hoverPos_ = pos;
    if (hoverPicked_ && pos == hoverPickedPos_ && pickMask() == hoverPickedMask_ &&
        viewChanges_ == hoverPickedChanges_) {
        hoverWanted_ = false; // picked here already (or being picked), with the picture as it is now
        return;
    }
    hoverWanted_ = true;
    if (!hoverInFlight_)
        sendHover();
}

void ViewportItem::sendHover()
{
    if (!selection_ || pickMask() == 0) {
        hoverWanted_ = false;
        return;
    }
    hoverWanted_ = false;
    hoverInFlight_ = true;
    const QPointF pos = hoverPos_;
    hoverPicked_ = true;
    hoverPickedPos_ = pos;
    hoverPickedMask_ = pickMask();
    hoverPickedChanges_ = viewChanges_;
    QPointer<ViewportItem> self(this);
    ownRequest_ = true;
    front_->pick(int(std::floor(pos.x())), int(std::floor(pos.y())), pickMask(),
                 [self, pos](std::optional<render::PickHit> hit) {
                     if (!self)
                         return;
                     self->hoverInFlight_ = false;
                     if (hit) {
                         self->hoverPoint_ = hit->point;
                         self->hoverPointAt_ = pos;
                     } else {
                         self->hoverPoint_.reset();
                     }
                     if (self->drag_.action == DragAction::None && self->selection_)
                         self->selection_->setHover(hit);
                     else
                         self->hoverPicked_ = false; // the answer was not used: pick again at the next hover
                     if (self->hoverWanted_)
                         self->sendHover();
                 });
    ownRequest_ = false;
}

void ViewportItem::clearHover()
{
    hoverWanted_ = false;
    hoverPicked_ = false;
    hoverPoint_.reset();
    if (selection_)
        selection_->setHover(std::nullopt);
}

void ViewportItem::cancelBox()
{
    if (drag_.action == DragAction::Select)
        drag_ = {};
    if (boxActive_) {
        boxActive_ = false;
        boxRect_ = {};
        emit boxChanged();
    }
}

void ViewportItem::selectAt(qreal x, qreal y, int modifiers)
{
    if (!selection_)
        return;
    const Qt::KeyboardModifiers mods(modifiers);
    QPointer<ViewportItem> self(this);
    front_->pick(int(std::floor(x)), int(std::floor(y)), pickMask(), [self, mods](std::optional<render::PickHit> hit) {
        if (self && self->selection_)
            self->selection_->applyClick(hit, mods);
    });
}

// ---------- input ----------
void ViewportItem::mousePressEvent(QMouseEvent* event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    if (drag_.action != DragAction::None) {
        event->accept(); // a second button during a drag changes nothing
        return;
    }
    const nav::Action action =
        nav::actionFor(nav::presets()[std::size_t(preset_)], event->button(), event->modifiers());
    if (action == nav::Action::None) {
        event->ignore();
        return;
    }
    stopAnimation();
    drag_ = {};
    drag_.button = event->button();
    drag_.modifiers = event->modifiers();
    drag_.press = drag_.last = event->position();
    switch (action) {
    case nav::Action::Select:
        drag_.action = DragAction::Select;
        break;
    case nav::Action::Orbit: {
        drag_.action = DragAction::Orbit;
        // Without a model point under the cursor: the middle of what is shown (fit keeps it in the free area,
        // which need not be the camera target).
        drag_.pivot = camera().target;
        if (const std::optional<Box3> box = front_->sceneBounds())
            drag_.pivot =
                Vec3{(box->min.x + box->max.x) / 2, (box->min.y + box->max.y) / 2, (box->min.z + box->max.z) / 2};
        if (hoverPoint_ && QLineF(hoverPointAt_, drag_.press).length() <= 2.0)
            drag_.pivot = *hoverPoint_;
        // The model point under the cursor, if any, becomes the pivot as soon as the ID buffer answers.
        QPointer<ViewportItem> self(this);
        const QPointF press = drag_.press;
        front_->pick(int(std::floor(press.x())), int(std::floor(press.y())), std::uint8_t(render::PickFilter::Faces),
                     [self, press](std::optional<render::PickHit> hit) {
                         if (self && hit && self->drag_.action == DragAction::Orbit && self->drag_.press == press)
                             self->drag_.pivot = hit->point;
                     });
        break;
    }
    case nav::Action::Pan:
        drag_.action = DragAction::Pan;
        break;
    case nav::Action::Zoom:
        drag_.action = DragAction::Zoom;
        drag_.anchor = zoomAnchor(drag_.press);
        break;
    case nav::Action::None:
        break;
    }
    if (drag_.action != DragAction::Select)
        clearHover();
    event->accept();
}

void ViewportItem::mouseMoveEvent(QMouseEvent* event)
{
    if (drag_.action == DragAction::None) {
        event->ignore();
        return;
    }
    const QPointF pos = event->position();
    const QPointF delta = pos - drag_.last;
    drag_.last = pos;
    if (!drag_.moved && (pos - drag_.press).manhattanLength() >= QGuiApplication::styleHints()->startDragDistance())
        drag_.moved = true;
    switch (drag_.action) {
    case DragAction::Select:
        if (drag_.moved) {
            boxActive_ = true;
            boxRect_ = QRectF(drag_.press, pos).normalized();
            emit boxChanged();
        }
        break;
    case DragAction::Orbit:
        applyCamera(
            render::orbitCamera(camera(), -delta.x() * kOrbitDegPerPx, -delta.y() * kOrbitDegPerPx, drag_.pivot));
        break;
    case DragAction::Pan:
        applyCamera(render::panCamera(camera(), delta.x(), delta.y(), heightPx()));
        break;
    case DragAction::Zoom:
        applyCamera(render::zoomCamera(camera(), std::exp(-delta.y() * kDragZoomPerPx), drag_.anchor));
        break;
    case DragAction::None:
        break;
    }
    event->accept();
}

void ViewportItem::mouseReleaseEvent(QMouseEvent* event)
{
    if (drag_.action == DragAction::None || event->button() != drag_.button) {
        event->accept();
        return;
    }
    const Drag drag = drag_;
    drag_ = {};
    if (drag.action == DragAction::Select && selection_) {
        const Qt::KeyboardModifiers mods = drag.modifiers;
        QPointer<ViewportItem> self(this);
        if (boxActive_) {
            const QRect r = boxRect_.toAlignedRect();
            cancelBox();
            front_->pickRect(r.left(), r.top(), r.right(), r.bottom(), pickMask(),
                             [self, mods](std::vector<render::PickHit> hits) {
                                 if (self && self->selection_)
                                     self->selection_->applyBox(hits, mods);
                             });
        } else {
            front_->pick(int(std::floor(drag.press.x())), int(std::floor(drag.press.y())), pickMask(),
                         [self, mods](std::optional<render::PickHit> hit) {
                             if (self && self->selection_)
                                 self->selection_->applyClick(hit, mods);
                         });
        }
    }
    event->accept();
}

void ViewportItem::mouseDoubleClickEvent(QMouseEvent* event)
{
    // The second click of a double click selects like a single one.
    mousePressEvent(event);
}

void ViewportItem::mouseUngrabEvent()
{
    drag_ = {};
    cancelBox();
}

void ViewportItem::hoverMoveEvent(QHoverEvent* event)
{
    if (drag_.action == DragAction::None)
        requestHover(event->position());
    event->accept();
}

void ViewportItem::hoverLeaveEvent(QHoverEvent* event)
{
    clearHover();
    event->accept();
}

void ViewportItem::wheelEvent(QWheelEvent* event)
{
    stopAnimation();
    const QPointF pos = event->position();
    const QPoint angle = event->angleDelta();
    const QPoint pixels = event->pixelDelta();
    const Qt::KeyboardModifiers mods = event->modifiers();
    // Touchpads deliver scroll phases or pixel deltas (macOS, Wayland, X11 with libinput). Windows sends
    // two-finger scrolling as plain wheel steps (they zoom) and a pinch as Ctrl+wheel.
    const bool touchpad = event->phase() != Qt::NoScrollPhase || !pixels.isNull();
    if (mods.testFlag(Qt::ControlModifier)) {
        const double units = pixels.isNull() ? angle.y() : pixels.y() * 2.0;
        zoomAt(std::pow(kPinchPerUnit, units), pos.x(), pos.y());
    } else if (touchpad && mods.testFlag(Qt::ShiftModifier)) {
        const QPointF step = pixels.isNull() ? QPointF(angle) / 8.0 : QPointF(pixels);
        orbitBy(-step.x() * kTouchpadOrbitDegPerPx, -step.y() * kTouchpadOrbitDegPerPx);
    } else if (touchpad) {
        const QPointF step = pixels.isNull() ? QPointF(angle) / 8.0 : QPointF(pixels);
        panBy(step.x(), step.y());
    } else if (angle.y() != 0) {
        zoomAt(std::pow(kWheelStep, angle.y() / 120.0), pos.x(), pos.y());
    }
    event->accept();
}

void ViewportItem::keyPressEvent(QKeyEvent* event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const QPointF centre(width() / 2, height() / 2);
    switch (event->key()) {
    case Qt::Key_F:
        shift ? fitSelection() : fitAll();
        break;
    case Qt::Key_1:
        setStandardView(ctrl ? QStringLiteral("back") : QStringLiteral("front"));
        break;
    case Qt::Key_3:
        setStandardView(ctrl ? QStringLiteral("left") : QStringLiteral("right"));
        break;
    case Qt::Key_7:
        setStandardView(ctrl ? QStringLiteral("bottom") : QStringLiteral("top"));
        break;
    case Qt::Key_0:
        setStandardView(QStringLiteral("iso"));
        break;
    case Qt::Key_5:
    case Qt::Key_P:
        toggleProjection();
        break;
    case Qt::Key_Left:
        shift ? panBy(-height() * kKeyPanFraction, 0) : orbitBy(kKeyOrbitDeg, 0);
        break;
    case Qt::Key_Right:
        shift ? panBy(height() * kKeyPanFraction, 0) : orbitBy(-kKeyOrbitDeg, 0);
        break;
    case Qt::Key_Up:
        shift ? panBy(0, -height() * kKeyPanFraction) : orbitBy(0, kKeyOrbitDeg);
        break;
    case Qt::Key_Down:
        shift ? panBy(0, height() * kKeyPanFraction) : orbitBy(0, -kKeyOrbitDeg);
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomAt(kWheelStep, centre.x(), centre.y());
        break;
    case Qt::Key_Minus:
        zoomAt(1.0 / kWheelStep, centre.x(), centre.y());
        break;
    default:
        event->ignore();
        return;
    }
    event->accept();
}

bool ViewportItem::event(QEvent* event)
{
    if (event->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(event);
        const QPointF pos = g->position();
        switch (g->gestureType()) {
        case Qt::ZoomNativeGesture:
            zoomAt(1.0 + g->value(), pos.x(), pos.y());
            break;
        case Qt::PanNativeGesture:
            panBy(g->delta().x(), g->delta().y());
            break;
        case Qt::RotateNativeGesture:
            orbitBy(-g->value(), 0);
            break;
        case Qt::SmartZoomNativeGesture:
            fitAll();
            break;
        default:
            break;
        }
        event->accept();
        return true;
    }
    return QQuickRhiItem::event(event);
}

void ViewportItem::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickRhiItem::itemChange(change, value);
    if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged)
        updateDevicePixelRatio();
    if (change == ItemTransformHasChanged || change == ItemSceneChange)
        emit scenePlacementChanged();
}

// ---------- GlassBackdrop (T-016) ----------
GlassBackdrop::GlassBackdrop(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    setFlag(ItemObservesViewport); // ItemTransformHasChanged when an ancestor moves it
}

GlassBackdrop::~GlassBackdrop()
{
    if (registered_ && view_)
        view_->removeBackdropUser();
}

void GlassBackdrop::setSource(QObject* source)
{
    auto* view = qobject_cast<ViewportItem*>(source);
    if (view == view_)
        return;
    if (registered_ && view_)
        view_->removeBackdropUser();
    if (registered_) {
        registered_ = false;
        emit samplingChanged();
    }
    for (const auto& c : viewConnections_)
        disconnect(c);
    viewConnections_.clear();
    view_ = view;
    if (view_) {
        viewConnections_ << connect(view_, &ViewportItem::scenePlacementChanged, this, [this] { update(); });
        viewConnections_ << connect(view_, &QQuickItem::windowChanged, this, &GlassBackdrop::updateRegistration);
        viewConnections_ << connect(view_, &QObject::destroyed, this, [this] {
            // Its count died with it.
            if (registered_) {
                registered_ = false;
                emit samplingChanged();
            }
            update();
        });
    }
    updateRegistration();
    update();
    emit sourceChanged();
}

void GlassBackdrop::setRadius(qreal radius)
{
    if (qFuzzyCompare(radius, radius_))
        return;
    radius_ = radius;
    update();
    emit radiusChanged();
}

void GlassBackdrop::updateRegistration()
{
    const bool want = view_ && isVisible() && window() != nullptr && view_->window() == window();
    if (want == registered_)
        return;
    registered_ = want;
    if (want)
        view_->addBackdropUser();
    else if (view_)
        view_->removeBackdropUser();
    update();
    emit samplingChanged();
}

void GlassBackdrop::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged || change == ItemSceneChange)
        updateRegistration();
    if (change == ItemTransformHasChanged || change == ItemDevicePixelRatioHasChanged)
        update();
}

void GlassBackdrop::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    update();
}

QRectF GlassBackdrop::sourceRect() const
{
    if (!registered_ || !view_ || view_->width() <= 0 || view_->height() <= 0)
        return {};
    const QRectF r = view_->mapRectFromItem(this, QRectF(0, 0, width(), height()));
    return QRectF(r.x() / view_->width(), r.y() / view_->height(), r.width() / view_->width(),
                  r.height() / view_->height());
}

QSGNode* GlassBackdrop::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    // Sync phase: the UI thread is blocked, so both items' geometry can be read here.
    const QRectF uv = sourceRect();
    if (uv.isEmpty() || width() <= 0 || height() <= 0) {
        delete old;
        return nullptr;
    }
    auto* node = static_cast<GlassBackdropNode*>(old);
    if (node == nullptr)
        node = new GlassBackdropNode;
    node->setShape(QSizeF(width(), height()), radius_, uv, view_->backdropLink());
    return node;
}

void ViewportItem::updateDevicePixelRatio()
{
    if (QQuickWindow* w = window())
        front_->setDevicePixelRatio(w->effectiveDevicePixelRatio());
}

} // namespace ray::ui
