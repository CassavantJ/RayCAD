// Horizontal or vertical slider (stepover, transparency, timeline scrubbing).
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.Slider {
    id: control

    readonly property real grooveThickness: Theme.spacingXs

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitHandleWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitHandleHeight + topPadding + bottomPadding)

    padding: Theme.spacingXs
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    handle: Rectangle {
        x: control.leftPadding + (control.horizontal ? control.visualPosition * (control.availableWidth - width)
                                                     : (control.availableWidth - width) / 2)
        y: control.topPadding + (control.horizontal ? (control.availableHeight - height) / 2
                                                    : control.visualPosition * (control.availableHeight - height))
        implicitWidth: Theme.indicatorSize
        implicitHeight: Theme.indicatorSize
        radius: width / 2
        color: control.pressed ? Theme.controlBgPressed : control.hovered ? Theme.controlBgHover : Theme.panelBg
        border.width: Theme.focusRingWidth
        border.color: control.enabled ? Theme.accentThinLine : Theme.textDisabled

        FocusRing {
            visible: control.visualFocus
        }
    }

    background: Rectangle {
        x: control.leftPadding + (control.horizontal ? 0 : (control.availableWidth - width) / 2)
        y: control.topPadding + (control.horizontal ? (control.availableHeight - height) / 2 : 0)
        implicitWidth: control.horizontal ? Theme.controlHeight * 5 : control.grooveThickness
        implicitHeight: control.horizontal ? control.grooveThickness : Theme.controlHeight * 5
        width: control.horizontal ? control.availableWidth : implicitWidth
        height: control.horizontal ? implicitHeight : control.availableHeight
        radius: control.grooveThickness / 2
        color: Theme.controlBgPressed

        Rectangle {
            y: control.horizontal ? 0 : control.visualPosition * parent.height
            width: control.horizontal ? control.position * parent.width : parent.width
            height: control.horizontal ? parent.height : control.position * parent.height
            radius: parent.radius
            color: control.enabled ? Theme.accentThinLine : Theme.textDisabled
        }
    }
}
