// The sketch over the 3D view (task T-092): Viewport.qml's sibling in the shell's viewportHost, with the view's
// geometry. SketchOverlay (C++, sketch_overlay.h) draws the sketch and takes the pointer and the keys while a sketch
// is open; this file adds what is easier in QML: the inferred-constraint glyphs next to the pointer and the value
// chips (length, angle, diameter, ...) that typed numbers go into. T-093 puts its constraint glyphs and dimension
// labels here too (SketchTool.mapToView places them).
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "sketchLayer"

    // The ViewportItem (Viewport.qml's `view`).
    property var view: null

    Binding {
        target: SketchTool
        property: "view"
        value: root.view
    }

    SketchOverlay {
        id: overlay
        objectName: "sketchOverlay"
        anchors.fill: parent
        visible: SketchTool.active
        enabled: SketchTool.active
        tool: SketchTool
        theme: Theme
        Accessible.role: Accessible.Canvas
        Accessible.name: qsTr("Sketch %1").arg(SketchTool.sketchName)
    }

    Connections {
        target: SketchTool
        function onActiveChanged() {
            if (SketchTool.active)
                overlay.forceActiveFocus()
            else if (root.view)
                root.view.forceActiveFocus()
        }
    }

    // Inferred constraints of the step being drawn.
    Repeater {
        model: SketchTool.active ? SketchTool.glyphs : []
        delegate: SketchGlyph {
            required property var modelData
            required property int index
            kind: modelData.kind
            x: modelData.px + Theme.spacingXs + index * (width + Theme.spacingXs / 2)
            y: modelData.py - height - Theme.spacingXs
        }
    }

    // The values of the step: live from the pointer, or as typed (typing a number starts it; Tab, Enter, Esc).
    Row {
        id: chips
        objectName: "sketchFields"
        visible: SketchTool.active && SketchTool.fields.length > 0 && SketchTool.tool !== "select"
        spacing: Theme.spacingXs
        x: Math.max(0, Math.min(root.width - width, SketchTool.fieldsAt.x + Theme.spacingM))
        y: Math.max(0, Math.min(root.height - height, SketchTool.fieldsAt.y + Theme.spacingM))

        Repeater {
            model: SketchTool.fields
            delegate: Rectangle {
                id: chip
                required property var modelData
                objectName: "sketchField." + modelData.key
                width: chipRow.implicitWidth + 2 * Theme.spacingS
                height: chipRow.implicitHeight + Theme.spacingXs
                radius: Theme.radiusSmall
                color: Theme.panelBg
                border.width: modelData.current ? Theme.focusRingWidth : Theme.borderWidth
                border.color: modelData.current ? Theme.accentThinLine : Theme.panelBorder

                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: Theme.spacingXs
                    Label {
                        secondary: true
                        font: Theme.smallFont
                        text: chip.modelData.label
                    }
                    Label {
                        objectName: "sketchFieldText"
                        mono: true
                        color: chip.modelData.typed ? Theme.accentText : Theme.textPrimary
                        text: chip.modelData.text + (chip.modelData.current ? "▏" : "")
                    }
                }
            }
        }
    }
}
