import QtQuick
import QtQuick.Controls

GlassPanel {
    id: root
    property string message
    implicitWidth: label.implicitWidth + 24
    implicitHeight: 30
    visible: message.length > 0
    Label { id: label; anchors.centerIn: parent; text: root.message; color: Theme.text; font.pixelSize: Theme.fontSize }
}
