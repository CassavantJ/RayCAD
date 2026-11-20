// The open feature tool over the 3D view (task T-098): its drag handles. Viewport.qml's sibling in the shell's
// viewportHost, with the view's geometry. FeatureTool.handles gives each handle's base and tip in view coordinates:
// an arrow (extrude distance, hole depth), an angle knob (revolve) or a radius knob (fillet, chamfer). Dragging a
// knob sets the field and rebuilds the preview. The layer takes no input outside its knobs, and draws nothing while
// nothing changes.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

Item {
    id: root
    objectName: "featureLayer"

    // The ViewportItem (Viewport.qml's `view`).
    property var view: null

    Binding {
        target: FeatureTool
        property: "view"
        value: root.view
    }
    // Clicks on the datum planes drawn in the view, where a plane is asked for.
    Binding {
        target: FeatureTree
        property: "view"
        value: root.view
    }

    Repeater {
        // By count: a drag changes the handles' places, not their number, so the knob being dragged stays.
        model: FeatureTool.active ? FeatureTool.handles.length : 0
        delegate: Item {
            id: handle
            required property int index
            readonly property var modelData: FeatureTool.handles[index]
                                             || { key: "", kind: "arrow", x: 0, y: 0, tipX: 0, tipY: 0 }
            readonly property real dx: modelData.tipX - modelData.x
            readonly property real dy: modelData.tipY - modelData.y
            readonly property real len: Math.max(1, Math.sqrt(dx * dx + dy * dy))
            readonly property bool active: knobArea.pressed || knobArea.containsMouse
            anchors.fill: parent

            // The shaft from base to tip (dashed for the radius and the angle).
            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: Theme.accentThinLine
                    strokeWidth: Theme.focusRingWidth
                    strokeStyle: handle.modelData.kind === "arrow" ? ShapePath.SolidLine : ShapePath.DashLine
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    startX: handle.modelData.x
                    startY: handle.modelData.y
                    PathLine {
                        x: handle.modelData.tipX
                        y: handle.modelData.tipY
                    }
                }
                // An arrow head at the tip.
                ShapePath {
                    id: head
                    strokeColor: "transparent"
                    fillColor: handle.modelData.kind === "arrow" ? Theme.accent : "transparent"
                    readonly property real ux: handle.dx / handle.len
                    readonly property real uy: handle.dy / handle.len
                    readonly property real s: Theme.spacingS
                    startX: handle.modelData.tipX + ux * s * 1.5
                    startY: handle.modelData.tipY + uy * s * 1.5
                    PathLine {
                        x: handle.modelData.tipX - head.uy * head.s * 0.8
                        y: handle.modelData.tipY + head.ux * head.s * 0.8
                    }
                    PathLine {
                        x: handle.modelData.tipX + head.uy * head.s * 0.8
                        y: handle.modelData.tipY - head.ux * head.s * 0.8
                    }
                    PathLine {
                        x: handle.modelData.tipX + head.ux * head.s * 1.5
                        y: handle.modelData.tipY + head.uy * head.s * 1.5
                    }
                }
            }

            // The knob: drag it.
            Rectangle {
                id: knob
                objectName: "featureHandle." + handle.modelData.key
                width: Theme.spacingM + (handle.active ? Theme.spacingXs : 0)
                height: width
                radius: width / 2
                x: handle.modelData.tipX - width / 2
                y: handle.modelData.tipY - height / 2
                color: handle.active ? Theme.accent : Theme.panelBg
                border.width: Theme.focusRingWidth
                border.color: Theme.accent
                Accessible.role: Accessible.Slider
                Accessible.name: handle.modelData.key

                MouseArea {
                    id: knobArea
                    anchors.fill: parent
                    anchors.margins: -Theme.spacingXs
                    // Never over a sketch drag (a hover-enabled area would take its release).
                    enabled: !SketchTool.active
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: Qt.SizeAllCursor
                    onPositionChanged: mouse => {
                        if (!pressed)
                            return
                        const p = mapToItem(root, mouse.x, mouse.y)
                        FeatureTool.dragHandle(handle.modelData.key, p.x, p.y, false)
                    }
                    onReleased: mouse => {
                        const p = mapToItem(root, mouse.x, mouse.y)
                        FeatureTool.dragHandle(handle.modelData.key, p.x, p.y, true)
                    }
                }
            }
        }
    }
}
