// The glyphs of the sketch's geometric constraints over the view (task T-093), in SketchLayer.qml beside the drawing
// glyphs. One SketchGlyph per place of a constraint (SketchConstraints.glyphs: a parallel shows by both lines),
// stacked when several sit at one place. Hovering a glyph highlights its entities, a click selects the constraint
// (Delete removes it); conflicting or redundant ones take the conflict colour.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "constraintsLayer"
    visible: SketchTool.active

    Repeater {
        model: SketchConstraints.glyphs
        delegate: Item {
            id: g
            required property string cid
            required property int slot
            required property string kind
            required property string title
            required property real px
            required property real py
            required property int stack
            required property bool bad
            required property bool hovered
            required property bool selected
            readonly property color ink: selected ? Theme.selection
                                         : hovered ? Theme.hover
                                         : bad ? Theme.sketchConflict
                                         : Theme.accentText

            objectName: "constraintGlyph." + cid + "." + slot
            x: px + Theme.spacingXs + stack * (chip.width + Theme.spacingXs / 2)
            y: py + Theme.spacingXs
            width: chip.width
            height: chip.height

            SketchGlyph {
                id: chip
                kind: g.kind
                tint: g.ink
                edge: g.selected || g.hovered || g.bad ? g.ink : Theme.accentThinLine
                border.width: g.selected ? Theme.focusRingWidth : Theme.borderWidth
                Accessible.name: g.title
            }
            MouseArea {
                id: area
                anchors.fill: parent
                enabled: !SketchTool.dragging // a drag of the geometry passes over them
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.PointingHandCursor
                onEntered: SketchConstraints.hoverConstraint(g.cid)
                onExited: if (SketchConstraints.hovered === g.cid) SketchConstraints.hoverConstraint("")
                onClicked: SketchConstraints.selectConstraint(g.cid)
            }
            HoverToolTip {
                target: g // g.hovered: this constraint is hovered
                text: area.containsMouse ? g.title : ""
            }
        }
    }
}
