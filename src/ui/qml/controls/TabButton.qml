// One segment of a TabBar. Takes its own implicit width, so a long label is never squeezed by its neighbours.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.TabButton {
    id: control

    property string iconName: ""

    width: implicitWidth
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    topPadding: Theme.spacingXs / 2
    bottomPadding: Theme.spacingXs / 2
    leftPadding: Theme.spacingM
    rightPadding: Theme.spacingM
    spacing: Theme.spacingXs
    font: Theme.uiFont
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.PageTab
    Accessible.name: text

    contentItem: IconText {
        iconName: control.iconName
        text: control.text
        font: control.font
        spacing: control.spacing
        color: !control.enabled ? Theme.textDisabled
               : control.checked || control.hovered ? Theme.textPrimary
               : Theme.textSecondary
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight - Theme.spacingXs
        radius: Math.min(Theme.radiusControl, height / 2)
        color: control.checked ? Theme.controlRaised
               : control.hovered && control.enabled ? Theme.controlBgHover
               : "transparent"
        border.width: control.checked && !Theme.dark ? Theme.borderWidth : 0
        border.color: Theme.panelBorder

        FocusRing {
            visible: control.visualFocus
        }
    }
}
