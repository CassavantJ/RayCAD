// Title row of a panel: optional icon, title, and tool buttons on the right (children of the header go there).
import QtQuick
import RayCad.Ui

Item {
    id: root

    property string title
    property string iconName: ""
    property bool showSeparator: true
    default property alias tools: toolRow.data

    implicitWidth: Theme.spacingM + heading.implicitWidth + Theme.spacingS + toolRow.implicitWidth + Theme.spacingXs
    implicitHeight: Theme.panelHeaderHeight

    Accessible.role: Accessible.StaticText
    Accessible.name: title

    IconText {
        id: heading
        objectName: "panelTitle"
        anchors.left: parent.left
        anchors.leftMargin: Theme.spacingM
        anchors.right: toolRow.left
        anchors.rightMargin: Theme.spacingS
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height
        alignment: Qt.AlignLeft
        iconName: root.iconName
        iconColor: Theme.textSecondary
        text: root.title
        font: Theme.strongFont
        color: Theme.textPrimary
    }

    Row {
        id: toolRow
        anchors.right: parent.right
        anchors.rightMargin: Theme.spacingXs
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacingXs / 2
    }

    Rectangle {
        visible: root.showSeparator
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.borderWidth
        color: Theme.panelBorder
    }
}
