// Labels of the measurements over the 3D view (task T-022). MeasureTool draws the witness lines into the viewport
// itself (IViewport::addLines, depth-tested like the model); this overlay adds what must stay readable on top: the
// value labels, the witness points, and the dimension lines again as thin dashes, so a line that runs through the
// material (face to face across a part) still shows. Positions come from MeasureTool, projected with the camera into
// the view item's coordinates; this item must have the view's geometry. It takes no input.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

Item {
    id: root
    objectName: "measureOverlay"

    // The ViewportItem (Viewport.qml's `view`).
    property var view: null

    Binding {
        target: MeasureTool
        property: "view"
        value: root.view
    }

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: Theme.accentThinLine
            strokeWidth: Theme.lineThinPx
            strokeStyle: ShapePath.DashLine
            dashPattern: [Theme.spacingXs, Theme.spacingXs]
            fillColor: "transparent"
            PathMultiline {
                paths: MeasureTool.pinnedPaths
            }
        }
        ShapePath {
            strokeColor: Theme.accentThinLine
            strokeWidth: Theme.lineThinPx
            strokeStyle: ShapePath.DashLine
            dashPattern: [Theme.spacingXs, Theme.spacingXs]
            fillColor: "transparent"
            PathMultiline {
                paths: MeasureTool.currentPaths
            }
        }
    }

    Repeater {
        model: MeasureTool.pointsModel
        delegate: Rectangle {
            required property real px
            required property real py
            width: Theme.spacingS
            height: width
            radius: width / 2
            x: px - width / 2
            y: py - height / 2
            color: Theme.accentThinLine
            border.width: Theme.borderWidth
            border.color: Theme.panelBg
        }
    }

    Repeater {
        model: MeasureTool.labelsModel
        delegate: Rectangle {
            id: chip
            required property real px
            required property real py
            required property string text
            required property bool pinned
            required property int index
            objectName: "measureLabel." + index
            width: chipText.implicitWidth + 2 * Theme.spacingS
            height: chipText.implicitHeight + Theme.spacingXs
            // Just above and centred on its point, kept inside the view.
            x: Math.max(0, Math.min(root.width - width, px - width / 2))
            y: Math.max(0, Math.min(root.height - height, py - height - Theme.spacingXs))
            radius: Theme.radiusSmall
            color: Theme.panelBg
            border.width: pinned ? Theme.borderWidth : Theme.focusRingWidth
            border.color: pinned ? Theme.panelBorder : Theme.accentThinLine

            Label {
                id: chipText
                anchors.centerIn: parent
                mono: true
                color: Theme.textPrimary
                text: chip.text
            }
        }
    }
}
