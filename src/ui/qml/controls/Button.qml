// Push button. `primary` for the one main action of a view (accent fill); `flat` for low-emphasis buttons.
// Optional leading icon (`iconName`) and tooltip (`toolTipText`, also its accessible description).
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.Button {
    id: control

    property bool primary: false
    property string iconName: ""
    property string toolTipText: ""

    readonly property color foreground: !enabled ? Theme.textDisabled
                                       : primary ? Theme.onAccent
                                       : checked ? Theme.accentText
                                       : Theme.textPrimary

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    topPadding: Theme.spacingXs
    bottomPadding: Theme.spacingXs
    leftPadding: text !== "" ? Theme.spacingM : Theme.spacingXs
    rightPadding: text !== "" ? Theme.spacingM : Theme.spacingXs
    spacing: Theme.spacingS
    font: Theme.uiFont
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: text !== "" ? text : toolTipText
    Accessible.description: toolTipText

    contentItem: IconText {
        iconName: control.iconName
        text: control.text
        font: control.font
        color: control.foreground
        spacing: control.spacing
    }

    background: Rectangle {
        implicitWidth: control.text !== "" ? Theme.controlHeight * 2.5 : Theme.controlHeight
        implicitHeight: Theme.controlHeight
        radius: Math.min(Theme.radiusControl, height / 2)
        color: {
            if (control.primary) {
                if (!control.enabled)
                    return Theme.controlBg
                return control.down ? Theme.accentPressed : control.hovered ? Theme.accentHover : Theme.accent
            }
            if (control.down)
                return Theme.controlBgPressed
            if (control.checked)
                return Theme.accentFill
            if (control.hovered && control.enabled)
                return Theme.controlBgHover
            return control.flat ? "transparent" : Theme.controlBg
        }
        border.width: control.flat || (control.primary && control.enabled) ? 0 : Theme.borderWidth
        border.color: Theme.panelBorder

        FocusRing {
            visible: control.visualFocus
        }
    }

    HoverToolTip {
        text: control.toolTipText
    }
}
