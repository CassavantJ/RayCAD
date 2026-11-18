// Menu row: optional icon or check mark, label, shortcut text on the right, arrow for a submenu.
// `marked` shows the check mark without making the item checkable (radio-like choices bound to a setting).
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.MenuItem {
    id: control

    property string iconName: ""
    property string shortcutText: ""
    property bool marked: false
    readonly property bool showCheck: checked || marked

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    topPadding: Theme.spacingXs
    bottomPadding: Theme.spacingXs
    leftPadding: Theme.spacingS + Theme.iconSize + Theme.spacingS
    rightPadding: Theme.spacingM + (subMenu ? Theme.iconSize : 0)
    spacing: Theme.spacingL
    font: Theme.uiFont
    hoverEnabled: true

    Accessible.name: text
    Accessible.checkable: checkable || marked
    Accessible.checked: showCheck

    indicator: Icon {
        x: Theme.spacingS
        y: control.topPadding + Math.round((control.availableHeight - height) / 2)
        visible: control.showCheck || control.iconName !== ""
        name: control.showCheck ? "check" : control.iconName
        color: !control.enabled ? Theme.textDisabled : control.showCheck ? Theme.accentText : Theme.textSecondary
    }

    arrow: Icon {
        x: control.width - width - Theme.spacingS
        y: control.topPadding + Math.round((control.availableHeight - height) / 2)
        visible: control.subMenu !== null
        name: "chevron-right"
        color: control.enabled ? Theme.textSecondary : Theme.textDisabled
    }

    contentItem: Item {
        implicitWidth: label.implicitWidth + (shortcut.visible ? control.spacing + shortcut.implicitWidth : 0)
        implicitHeight: Math.max(label.implicitHeight, shortcut.implicitHeight)

        Text {
            id: label
            objectName: "label"
            anchors.left: parent.left
            anchors.right: shortcut.visible ? shortcut.left : parent.right
            anchors.rightMargin: shortcut.visible ? control.spacing : 0
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.font
            color: control.enabled ? Theme.textPrimary : Theme.textDisabled
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
        Text {
            id: shortcut
            objectName: "shortcut"
            visible: control.shortcutText !== ""
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: control.shortcutText
            font: control.font
            color: control.enabled ? Theme.textSecondary : Theme.textDisabled
            textFormat: Text.PlainText
        }
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight * 6
        implicitHeight: Theme.rowHeight
        radius: Theme.radiusSmall
        color: control.highlighted && control.enabled ? Theme.controlBgHover : "transparent"
    }
}
