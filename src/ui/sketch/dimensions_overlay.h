// SketchDimensionOverlay (task T-093): the dimensions of the sketch being edited over the 3D view: extension lines,
// dimension lines and arcs, and arrowheads, from SketchConstraints' scene (constraints_model.h), already in this
// item's coordinates. Thin screen-space strips and filled arrowheads, one scene graph node per colour; every colour
// is a theme token (accentText driving, sketchConstruction driven, sketchConflict over-constrained, hover, selection,
// accent for the one being placed). It repaints only when the scene changes (geometry, camera, labels moved) or the
// theme does: an idle sketch draws nothing. It takes no input (the labels are QML items above it).
#pragma once

#include <QtCore/QPointer>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

namespace ray::ui {

class SketchConstraintsModel;
class ThemeObject;

class SketchDimensionOverlay : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(SketchDimensionOverlay)
    Q_PROPERTY(QObject* constraints READ constraints WRITE setConstraints NOTIFY constraintsChanged)
    Q_PROPERTY(QObject* theme READ theme WRITE setTheme NOTIFY themeChanged)

public:
    explicit SketchDimensionOverlay(QQuickItem* parent = nullptr);
    ~SketchDimensionOverlay() override;

    QObject* constraints() const;
    void setConstraints(QObject* constraints);
    QObject* theme() const;
    void setTheme(QObject* theme);
    int paints() const { return paints_; } // scene graph updates made (tests)

signals:
    void constraintsChanged();
    void themeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;

private:
    QPointer<SketchConstraintsModel> model_;
    QPointer<ThemeObject> theme_;
    QMetaObject::Connection modelConnection_, themeConnection_;
    int paints_ = 0;
};

} // namespace ray::ui
