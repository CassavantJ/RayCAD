// On/off switch with label, for settings that apply at once (glass panels, follow system theme).
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.Switch {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: Theme.spacingXs
    spacing: Theme.spacingS
    font: Theme.uiFont
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    indicator: Rectangle {
        id: track
        readonly property real inset: Theme.spacingXs / 2
        implicitWidth: Theme.indicatorSize * 2
        implicitHeight: Theme.indicatorSize + inset
        x: control.leftPadding
        y: control.topPadding + Math.round((control.availableHeight - height) / 2)
        radius: height / 2
        color: control.checked ? (control.enabled ? Theme.accentThinLine : Theme.controlBgPressed)
               : control.hovered && control.enabled ? Theme.controlBgPressed
               : Theme.controlBg
        border.width: control.checked ? 0 : Theme.borderWidth
        border.color: control.enabled ? Theme.controlBorder : Theme.panelBorder

        Rectangle {
            id: thumb
            width: track.height - 2 * track.inset
            height: width
            radius: width / 2
            y: track.inset
            x: control.checked ? track.width - width - track.inset : track.inset
            color: !control.enabled ? Theme.textDisabled
                   : control.checked ? Theme.onAccentLine
                   : Theme.textSecondary
            Behavior on x {
                enabled: !control.down
                NumberAnimation {
                    duration: Theme.durationFast
                    easing.type: Easing.OutCubic
                }
            }
        }

        FocusRing {
            visible: control.visualFocus
        }
    }

    contentItem: Text {
        objectName: "label"
        leftPadding: control.indicator.width + (control.text !== "" ? control.spacing : 0)
        text: control.text
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        elide: Text.ElideRight
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignVCenter
    }
}
