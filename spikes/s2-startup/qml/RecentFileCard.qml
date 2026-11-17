import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: root
    property string fileName
    property string detail
    implicitWidth: 150
    implicitHeight: 110
    background: GlassPanel { color: root.hovered ? Theme.panelAlt : Theme.panel }
    contentItem: ColumnLayout {
        spacing: 4
        Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.window; radius: 3 }
        Label { text: root.fileName; color: Theme.text; font.pixelSize: Theme.fontSize; elide: Text.ElideMiddle; Layout.fillWidth: true }
        Label { text: root.detail; color: Theme.textDim; font.pixelSize: 10 }
    }
}
