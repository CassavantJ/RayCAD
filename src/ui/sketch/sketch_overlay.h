// SketchOverlay (task T-092): the sketch over the 3D view, and the pointer and keys of the sketch mode.
//
// Drawing: SketchTool's overlay scene (sketch_mode.h), already in this item's coordinates, as triangles in the scene
// graph: curves as screen-space strips (construction and guide lines dashed), points as small squares. One node per
// colour; every colour is a theme token (Theme: sketchGeometry, sketchConstrained, sketchConflict,
// sketchConstruction, selection, hover, accent). It repaints only when SketchTool's overlay changes (geometry, tool,
// camera) or the theme does, so an idle sketch draws nothing.
//
// Input: the left button (without Alt) and hover go to SketchTool; other buttons, Alt + left and the wheel pass to
// the view below, so navigation works as everywhere. Keys go to SketchTool first (it claims what it uses before the
// shortcuts, through ShortcutOverride); keys it does not use go on to the view (F fits, 1 / 3 / 7 standard views).
// The item must have the view's geometry.
#pragma once

#include <QtCore/QPointer>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

namespace ray::ui {

class SketchModel;
class ThemeObject;

class SketchOverlay : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(SketchOverlay)
    Q_PROPERTY(QObject* tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QObject* theme READ theme WRITE setTheme NOTIFY themeChanged)

public:
    explicit SketchOverlay(QQuickItem* parent = nullptr);
    ~SketchOverlay() override;

    QObject* tool() const;
    void setTool(QObject* tool);
    QObject* theme() const;
    void setTheme(QObject* theme);
    // Scene graph updates made (tests: an idle sketch makes none).
    int paints() const { return paints_; }

signals:
    void toolChanged();
    void themeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    QPointer<SketchModel> tool_;
    QPointer<ThemeObject> theme_;
    QMetaObject::Connection toolConnection_, themeConnection_;
    bool pressed_ = false;
    int paints_ = 0;
};

} // namespace ray::ui
