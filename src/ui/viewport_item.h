// The 3D view inside Qt Quick (task T-020): a QQuickRhiItem that drives src/render's hosted viewport.
//
//   UI thread      the item owns `front` (render::makeHostedViewport). The scene pushes meshes into it, the
//                  selection its highlight, the Theme its colours, the scene's tier preset its quality. Every
//                  change calls update(), so the item draws only on demand: an idle view renders no frames.
//   Render thread  ViewportRenderer (viewport_item.cpp) owns one render::HostedViewportRenderer and forwards
//                  initialize / synchronize / render to it. The item keeps sample count 1 and no render target
//                  of its own: the renderer brings its MSAA and depth buffers and resolves into colorTexture().
//                  A frame that only answers a pick draws nothing; the texture keeps its last image.
//
// Input: the mouse mapping comes from the navigation preset (viewport_navigation.h); the wheel zooms about the
// point under the cursor; touchpads pan (two-finger scroll), zoom (pinch, Ctrl+wheel) and orbit (Shift+scroll,
// rotate gesture); orbit turns about the model point under the cursor when there is one. Keyboard (when the view
// has focus): F fit all, Shift+F fit selection, 1 / 3 / 7 front / right / top (Ctrl: back / left / bottom),
// 0 iso, 5 or P perspective/orthographic, arrows orbit, Shift+arrows pan, + and - zoom. Viewport.qml adds Esc.
//
// Glass backdrop (task T-016, PLAN.md 6.6). The item makes itself AppContext.backdropSource. Every glass panel
// (PanelBase.qml) holds a GlassBackdrop, which registers with the item while it is visible; the item enables the
// renderer's backdrop while at least one is registered (IViewport::setBackdropEnabled), so flat panels, or glass
// panels that are all hidden, cost nothing. How the blurred texture reaches the panels, all on the render thread:
//   1. ViewportRenderer::render() (QQuickRhiItemRenderer, called from the window's beforeRendering, after every
//      item's sync) draws the frame; the renderer blurs it into ONE texture it owns, at most once per frame;
//      render() then publishes HostedViewportRenderer::backdrop() into the item's BackdropLink.
//   2. Each GlassBackdrop's node re-reads the link in QSGNode::preprocess(), which the scene graph runs after
//      beforeRendering and before it draws, and points its non-owning QSGTexture at the current QRhiTexture. The
//      panel geometry (where the panel lies in the viewport, normalised) was taken in updatePaintNode, while the
//      UI thread was blocked.
//   So the handle never crosses to the UI thread, nothing but the renderer owns it (no createTextureFromRhiTexture,
//   which takes ownership: the double free of spike S1), and a texture replaced on resize is retired, not freed,
//   until the next frame's render() (viewport_rhi.h), so a node never samples freed memory even if the scene graph
//   prepared it before the viewport rendered. The link is cleared before the renderer frees anything.
#pragma once

#include "scene_controller.h"
#include "selection_model.h"
#include "theme_object.h"

#include <ray_render/viewport.h>

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QRectF>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QVector3D>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickRhiItem>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

class QRhiTexture;

namespace ray::ui {

class AppContext;
class ViewportRenderer;

// Render thread to render thread: the viewport's current blurred backdrop, for the glass panels' nodes. Written by
// ViewportRenderer::render(), read by GlassBackdrop's node in preprocess(). The mutex only guards against a
// threaded render loop that renders two windows on two threads.
struct BackdropLink {
    std::mutex mutex;
    QRhiTexture* texture = nullptr; // owned by the viewport's renderer; null: no backdrop
    QSize size;                     // texels
    bool yUp = false;               // the texture's first row is the bottom of the picture
    std::uint64_t generation = 0;
};

class ViewportItem : public QQuickRhiItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(ViewportItem)

    Q_PROPERTY(ray::ui::SceneController* scene READ scene WRITE setScene NOTIFY sceneChanged)
    Q_PROPERTY(ray::ui::SelectionModel* selection READ selection WRITE setSelection NOTIFY selectionChanged)
    Q_PROPERTY(ray::ui::ThemeObject* theme READ theme WRITE setTheme NOTIFY themeChanged)
    // Index into navigationPresetNames (0 = the RayCAD default).
    Q_PROPERTY(int navigationPreset READ navigationPreset WRITE setNavigationPreset NOTIFY navigationPresetChanged)
    Q_PROPERTY(QStringList navigationPresetNames READ navigationPresetNames CONSTANT)
    Q_PROPERTY(QStringList navigationPresetDescriptions READ navigationPresetDescriptions CONSTANT)
    Q_PROPERTY(bool perspective READ perspective WRITE setPerspective NOTIFY cameraChanged)
    // The camera's frame in world coordinates, for the view cube.
    Q_PROPERTY(QVector3D viewRight READ viewRight NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D viewUp READ viewUp NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D viewForward READ viewForward NOTIFY cameraChanged)
    // Fit and standard views glide for a short moment; off: they jump (tests).
    Q_PROPERTY(bool animationsEnabled READ animationsEnabled WRITE setAnimationsEnabled NOTIFY animatingChanged)
    Q_PROPERTY(bool animating READ animating NOTIFY animatingChanged)
    // Box selection in progress: the rectangle in item coordinates.
    Q_PROPERTY(bool boxActive READ boxActive NOTIFY boxChanged)
    Q_PROPERTY(QRectF boxRect READ boxRect NOTIFY boxChanged)
    // False under a scene graph without QRhi (QT_QUICK_BACKEND=software): Viewport.qml then draws a plain backdrop.
    Q_PROPERTY(bool rhiAvailable READ rhiAvailable CONSTANT)
    // Anything visible to frame.
    Q_PROPERTY(bool hasContent READ hasContent NOTIFY contentChanged)
    // The part of the item no panel covers (item coordinates): fit and the standard views frame the model there.
    // Empty: the whole item.
    Q_PROPERTY(QRectF frameRect READ frameRect WRITE setFrameRect NOTIFY frameRectChanged)

public:
    explicit ViewportItem(QQuickItem* parent = nullptr);
    ~ViewportItem() override;

    SceneController* scene() const { return scene_; }
    void setScene(SceneController* scene);
    SelectionModel* selection() const { return selection_; }
    void setSelection(SelectionModel* selection);
    ThemeObject* theme() const { return theme_; }
    void setTheme(ThemeObject* theme);
    int navigationPreset() const { return preset_; }
    void setNavigationPreset(int index);
    QStringList navigationPresetNames() const;
    QStringList navigationPresetDescriptions() const;
    bool perspective() const;
    void setPerspective(bool on);
    QVector3D viewRight() const;
    QVector3D viewUp() const;
    QVector3D viewForward() const;
    bool animationsEnabled() const { return animationsEnabled_; }
    void setAnimationsEnabled(bool on);
    bool animating() const { return animTimer_.isActive(); }
    bool boxActive() const { return boxActive_; }
    QRectF boxRect() const { return boxRect_; }
    bool rhiAvailable() const { return rhiAvailable_; }
    bool hasContent() const;
    QRectF frameRect() const { return frameRect_; }
    void setFrameRect(const QRectF& rect);
    // `camera` turned to frame `box` in frameRect, keeping its view direction.
    render::Camera framed(const render::Camera& camera, const Box3& box) const;

    // ---- navigation (QML, the view cube, keyboard and tests) ----
    Q_INVOKABLE void fitAll();
    Q_INVOKABLE void fitSelection();
    // "front", "back", "left", "right", "top", "bottom", "iso".
    Q_INVOKABLE bool setStandardView(const QString& name);
    // Look along a direction (eye to target), framing everything: view cube edges and corners.
    Q_INVOKABLE void viewAlong(qreal dx, qreal dy, qreal dz);
    Q_INVOKABLE void toggleProjection() { setPerspective(!perspective()); }
    Q_INVOKABLE void orbitBy(qreal yawDeg, qreal pitchDeg);
    Q_INVOKABLE void panBy(qreal dxPx, qreal dyPx);
    // factor > 1 zooms in, about the model point under (x, y) (item coordinates).
    Q_INVOKABLE void zoomAt(qreal factor, qreal x, qreal y);
    Q_INVOKABLE QVariantMap cameraState() const;
    Q_INVOKABLE void cancelBox();
    // A click-select at (x, y) in item coordinates, as the left button would do it (the answer is asynchronous).
    Q_INVOKABLE void selectAt(qreal x, qreal y, int modifiers = 0);

    // ---- C++ (tests, the scene) ----
    render::IRenderViewport* viewport() const { return front_.get(); }
    render::Camera camera() const;
    void setCamera(const render::Camera& camera); // immediate; stops a transition
    // The world point a zoom at (x, y) keeps fixed: on the cursor ray, at the depth of the last hover hit when it
    // is near, else at the depth of the camera target.
    Vec3 zoomAnchor(QPointF itemPos) const;

    // ---- glass backdrop (T-016) ----
    // Visible glass panels that sample the backdrop; it is enabled while there is at least one.
    void addBackdropUser();
    void removeBackdropUser();
    int backdropUsers() const { return backdropUsers_; }
    const std::shared_ptr<BackdropLink>& backdropLink() const { return backdropLink_; }

signals:
    void sceneChanged();
    void selectionChanged();
    void themeChanged();
    void navigationPresetChanged();
    void cameraChanged();
    void animatingChanged();
    void boxChanged();
    void contentChanged();
    void frameRectChanged();
    // The item's place in the scene changed (an ancestor moved or resized it): glass panels map onto it again.
    void scenePlacementChanged();

protected:
    QQuickRhiItemRenderer* createRenderer() override;
    void componentComplete() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    friend class ViewportRenderer;
    enum class DragAction { None, Select, Orbit, Pan, Zoom };
    struct Drag {
        DragAction action = DragAction::None;
        Qt::MouseButton button = Qt::NoButton;
        Qt::KeyboardModifiers modifiers;
        QPointF press, last;
        bool moved = false;
        Vec3 pivot{};
        Vec3 anchor{};
    };

    void applyCamera(const render::Camera& camera);
    void animateTo(const render::Camera& camera);
    void stopAnimation();
    void animationTick();
    double aspect() const;
    int heightPx() const;
    void updateDevicePixelRatio();
    void applyTheme();
    void updateHighlight();
    void requestHover(QPointF pos);
    void sendHover();
    void clearHover();
    std::uint8_t pickMask() const;
    Box3 contentBounds() const;

    std::unique_ptr<render::IRenderViewport> front_;
    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<ThemeObject> theme_;
    QMetaObject::Connection themeConnection_;
    QList<QMetaObject::Connection> sceneConnections_;
    QList<QMetaObject::Connection> selectionConnections_;
    int preset_ = 0;
    bool rhiAvailable_ = true;
    bool animationsEnabled_ = true;
    Drag drag_;
    bool boxActive_ = false;
    QRectF boxRect_;
    QRectF frameRect_;
    // Hover picking: at most one pick in flight; the newest cursor position is picked next.
    bool hoverInFlight_ = false;
    bool hoverWanted_ = false;
    QPointF hoverPos_;
    std::optional<Vec3> hoverPoint_; // world point of the last hover hit
    QPointF hoverPointAt_;
    // Qt Quick sends a hover again, at the same place, after frames while the cursor rests on the window. Picking
    // again for it would ask for a frame and so for the next such hover: the view would never go idle. A hover is
    // picked again only where the last hover pick was not made, or after something other than this item's hover
    // picks and highlights asked the view for a frame (camera, scene, size: the picture under the cursor changed).
    std::uint64_t viewChanges_ = 0;
    bool ownRequest_ = false; // a frame request of this item's hover pick or highlight: not a view change
    bool hoverPicked_ = false;
    QPointF hoverPickedPos_;
    std::uint8_t hoverPickedMask_ = 0;
    std::uint64_t hoverPickedChanges_ = 0;
    // Camera transition.
    QTimer animTimer_;
    QElapsedTimer animClock_;
    render::Camera animFrom_{}, animTo_{};
    // Glass backdrop (T-016).
    std::shared_ptr<BackdropLink> backdropLink_ = std::make_shared<BackdropLink>();
    int backdropUsers_ = 0;
    QPointer<AppContext> backdropContext_; // where this item is the backdropSource
};

// The blurred viewport behind a glass panel (T-016): PanelBase.qml puts one under the glass tint. It draws the part
// of the viewport's ONE shared blurred backdrop that lies behind it, as a rounded rectangle of `radius`, and
// nothing when `source` (AppContext.backdropSource: the ViewportItem) is null, in another window, or has no
// backdrop yet. While visible it counts as a user of the backdrop (ViewportItem::addBackdropUser). See the top of
// this file for how the texture reaches it.
class GlassBackdrop : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(GlassBackdrop)
    Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    // Registered as a user of the source's backdrop (visible, in the source's window).
    Q_PROPERTY(bool sampling READ sampling NOTIFY samplingChanged)

public:
    explicit GlassBackdrop(QQuickItem* parent = nullptr);
    ~GlassBackdrop() override;

    QObject* source() const { return view_; }
    void setSource(QObject* source);
    qreal radius() const { return radius_; }
    void setRadius(qreal radius);
    bool sampling() const { return registered_; }
    // Where this item lies in the source viewport, normalised to its size (0..1 across the viewport); empty when
    // not sampling. The node samples exactly this part of the blurred texture.
    QRectF sourceRect() const;

signals:
    void sourceChanged();
    void radiusChanged();
    void samplingChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void updateRegistration();

    QPointer<ViewportItem> view_;
    QList<QMetaObject::Connection> viewConnections_;
    qreal radius_ = 0;
    bool registered_ = false;
};

} // namespace ray::ui
