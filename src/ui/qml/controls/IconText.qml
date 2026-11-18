// Icon and label side by side: the content of buttons, tabs, menu items and rows. The label elides when the item
// is narrower than its implicit width (tests check `label.truncated`).
import QtQuick
import RayCad.Ui

Item {
    id: root

    property string iconName
    property string text
    property font font: Theme.uiFont
    property color color: Theme.textPrimary
    property color iconColor: color
    property real spacing: Theme.spacingS
    property real iconSize: Theme.iconSize
    property int alignment: Qt.AlignHCenter // or Qt.AlignLeft
    readonly property alias label: label
    readonly property bool hasIcon: iconName !== ""
    readonly property bool hasText: text !== ""
    readonly property real gap: hasIcon && hasText ? spacing : 0

    implicitWidth: (hasIcon ? iconSize : 0) + gap + (hasText ? label.implicitWidth : 0)
    implicitHeight: Math.max(hasIcon ? iconSize : 0, hasText ? label.implicitHeight : 0)

    Item {
        id: group
        height: parent.height
        width: Math.min(parent.width, root.implicitWidth)
        x: root.alignment === Qt.AlignLeft ? 0 : Math.round((parent.width - width) / 2)

        Icon {
            id: icon
            visible: root.hasIcon
            name: root.iconName
            color: root.iconColor
            size: root.iconSize
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            id: label
            objectName: "label"
            visible: root.hasText
            anchors.left: root.hasIcon ? icon.right : parent.left
            anchors.leftMargin: root.gap
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            font: root.font
            color: root.color
            elide: Text.ElideRight
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignVCenter
        }
    }
}
