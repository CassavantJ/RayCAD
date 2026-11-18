// A monochrome vector icon from icons.js, drawn in `color` at `size` logical pixels (default Theme.iconSize).
import QtQuick
import QtQuick.Shapes
import RayCad.Ui
import "icons.js" as Icons

Item {
    id: root

    property string name
    property color color: Theme.textPrimary
    property real size: Theme.iconSize
    // The icons are drawn on a 16 x 16 grid (icons.js); the stroke scales with the icon.
    readonly property real gridSize: 16
    readonly property bool solid: Icons.filled(name)

    implicitWidth: size
    implicitHeight: size
    Accessible.ignored: true

    Shape {
        width: root.gridSize
        height: root.gridSize
        scale: root.size / root.gridSize
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        visible: root.name !== ""

        ShapePath {
            strokeColor: root.solid ? "transparent" : root.color
            strokeWidth: root.solid ? -1 : 1.5
            fillColor: root.solid ? root.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg {
                path: Icons.path(root.name)
            }
        }
    }
}
