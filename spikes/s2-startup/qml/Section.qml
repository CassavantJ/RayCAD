import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    property alias title: header.title
    default property alias content: body.data
    spacing: 0
    PanelHeader { id: header; collapsible: true; Layout.fillWidth: true }
    ColumnLayout {
        id: body
        visible: !header.collapsed
        spacing: 2
        Layout.fillWidth: true
        Layout.margins: 6
    }
}
