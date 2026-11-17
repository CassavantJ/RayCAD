import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: root
    property string name
    property string tool
    property real progress: 1
    implicitHeight: 34
    padding: 2
    background: Rectangle { color: root.hovered ? Theme.panelAlt : "transparent" }
    contentItem: RowLayout {
        spacing: 6
        IconGlyph { kind: 7; color: root.progress >= 1 ? Theme.good : Theme.warn }
        ColumnLayout {
            spacing: 0
            Layout.fillWidth: true
            Label { text: root.name; color: Theme.text; font.pixelSize: Theme.fontSize; elide: Text.ElideRight; Layout.fillWidth: true }
            Label { text: root.tool; color: Theme.textDim; font.pixelSize: 10; elide: Text.ElideRight; Layout.fillWidth: true }
        }
        ProgressBar { value: root.progress; visible: root.progress < 1; Layout.preferredWidth: 50 }
    }
}
