// One inferred-constraint glyph next to the pointer while drawing (task T-092): a small chip with the constraint's
// symbol (icons.js "glyph-<kind>", drawn for RayCAD). T-093 reuses it for the glyphs of existing constraints.
import QtQuick
import RayCad.Ui

Rectangle {
    id: root
    property string kind: ""

    objectName: "sketchGlyph." + kind
    width: Theme.iconSize + Theme.spacingXs
    height: width
    radius: Theme.radiusSmall
    color: Theme.panelBg
    border.width: Theme.borderWidth
    border.color: Theme.accentThinLine
    Accessible.role: Accessible.Graphic
    Accessible.name: kind

    Icon {
        anchors.centerIn: parent
        name: "glyph-" + root.kind
        color: Theme.accentText
        size: Theme.iconSize * 0.75
    }
}
