import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: root
    property int depth: 0
    property string label
    property int glyph: 0
    property bool hasChildren: false
    property bool suppressed: false
    implicitHeight: Theme.rowHeight
    padding: 0
    background: Rectangle {
        color: root.highlighted ? Theme.accentDim : (root.hovered ? Theme.panelAlt : "transparent")
    }
    contentItem: RowLayout {
        spacing: 4
        Item { implicitWidth: 6 + root.depth * 14 }
        Label {
            text: root.hasChildren ? "▾" : ""
            color: Theme.textDim
            font.pixelSize: 10
            Layout.preferredWidth: 10
        }
        IconGlyph { kind: root.glyph; color: root.suppressed ? Theme.textDim : Theme.accent }
        Label {
            text: root.label
            color: root.suppressed ? Theme.textDim : Theme.text
            font.pixelSize: Theme.fontSize
            font.strikeout: root.suppressed
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        VisibilityToggle { }
    }
}
