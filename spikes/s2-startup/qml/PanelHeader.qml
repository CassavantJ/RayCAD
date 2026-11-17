import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    property string title
    property bool collapsible: false
    property bool collapsed: false
    implicitHeight: 28
    color: Theme.panelAlt
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 4
        Label {
            text: root.title
            color: Theme.text
            font.pixelSize: Theme.headerSize
            font.bold: true
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        ToolButton {
            visible: root.collapsible
            implicitWidth: 22; implicitHeight: 22
            text: root.collapsed ? "+" : "-"
            onClicked: root.collapsed = !root.collapsed
        }
    }
}
