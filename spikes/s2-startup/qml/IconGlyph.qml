import QtQuick

Item {
    id: root
    property color color: Theme.text
    property int kind: 0
    implicitWidth: 16
    implicitHeight: 16
    Rectangle {
        anchors.centerIn: parent
        width: root.kind % 2 ? 12 : 10
        height: root.kind % 3 ? 10 : 12
        radius: root.kind % 4 === 0 ? 6 : 2
        color: "transparent"
        border.color: root.color
        border.width: 1.5
    }
    Rectangle {
        visible: root.kind % 5 === 1
        anchors.centerIn: parent
        width: 4; height: 4; radius: 2
        color: root.color
    }
}
