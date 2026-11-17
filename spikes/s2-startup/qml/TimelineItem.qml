import QtQuick
import QtQuick.Controls

AbstractButton {
    id: root
    property int glyph: 0
    property bool current: false
    implicitWidth: 28
    implicitHeight: 28
    background: Rectangle {
        radius: 4
        color: root.current ? Theme.accentDim : (root.hovered ? Theme.panelAlt : "transparent")
        border.color: root.current ? Theme.accent : "transparent"
    }
    contentItem: IconGlyph { kind: root.glyph }
}
