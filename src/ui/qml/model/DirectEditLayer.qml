// The open direct-edit tool over the 3D view (task T-083): Push/Pull's arrow along the face's outward normal, from the
// middle of the first picked face. Viewport.qml's sibling in the shell's viewportHost, with the view's geometry.
// Dragging the knob moves the distance by how far the pointer moves along the arrow, in steps of the display unit
// (Shift: small steps), and rebuilds the preview; the number also goes into the panel's field. The layer takes no
// input outside its knob and draws nothing while nothing changes.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

Item {
    id: root
    objectName: "directEditLayer"

    // The ViewportItem (Viewport.qml's `view`).
    property var view: null

    Binding {
        target: DirectEdit
        property: "view"
        value: root.view
    }

    Repeater {
        // By count: a drag moves the arrow, it does not change how many there are, so the knob being dragged stays.
        model: DirectEdit.active ? DirectEdit.handles.length : 0
        delegate: Item {
            id: handle
            required property int index
            readonly property var modelData: DirectEdit.handles[index]
                                             || { key: "", kind: "arrow", x: 0, y: 0, tipX: 0, tipY: 0 }
            readonly property real dx: modelData.tipX - modelData.x
            readonly property real dy: modelData.tipY - modelData.y
            readonly property real len: Math.max(1, Math.sqrt(dx * dx + dy * dy))
            readonly property bool active: knobArea.pressed || knobArea.containsMouse
            anchors.fill: parent

            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: Theme.accentThinLine
                    strokeWidth: Theme.focusRingWidth
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    startX: handle.modelData.x
                    startY: handle.modelData.y
                    PathLine {
                        x: handle.modelData.tipX
                        y: handle.modelData.tipY
                    }
                }
                ShapePath {
                    id: head
                    strokeColor: "transparent"
                    fillColor: Theme.accent
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

            Rectangle {
                id: knob
                objectName: "directEditHandle." + handle.modelData.key
                width: Theme.spacingM + (handle.active ? Theme.spacingXs : 0)
                height: width
                radius: width / 2
                x: handle.modelData.tipX - width / 2
                y: handle.modelData.tipY - height / 2
                color: handle.active ? Theme.accent : Theme.panelBg
                border.width: Theme.focusRingWidth
                border.color: Theme.accent
                Accessible.role: Accessible.Slider
                Accessible.name: qsTr("Push/Pull distance")

                MouseArea {
                    id: knobArea
                    anchors.fill: parent
                    anchors.margins: -Theme.spacingXs
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: Qt.SizeAllCursor
                    onPositionChanged: mouse => {
                        if (!pressed)
                            return
                        const p = mapToItem(root, mouse.x, mouse.y)
                        DirectEdit.dragHandle(handle.modelData.key, p.x, p.y, false,
                                              (mouse.modifiers & Qt.ShiftModifier) !== 0)
                    }
                    onReleased: mouse => {
                        const p = mapToItem(root, mouse.x, mouse.y)
                        DirectEdit.dragHandle(handle.modelData.key, p.x, p.y, true,
                                              (mouse.modifiers & Qt.ShiftModifier) !== 0)
                    }
                }
            }
        }
    }
}
