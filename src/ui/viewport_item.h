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

#include <memory>
#include <optional>

namespace ray::ui {

class ViewportRenderer;

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

protected:
    QQuickRhiItemRenderer* createRenderer() override;
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
    // Camera transition.
    QTimer animTimer_;
    QElapsedTimer animClock_;
    render::Camera animFrom_{}, animTo_{};
};

} // namespace ray::ui
