// Icon (or short text) button for toolbars and panel headers. Flat until hovered; `checkable` for toggles.
// Icon-only buttons must set `toolTipText`: it is their accessible name.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.ToolButton {
    id: control

    property string iconName: ""
    property string toolTipText: ""
    // Smaller variant for panel headers and the status bar.
    property bool compact: false
    readonly property real side: compact ? Theme.controlHeightSmall : Theme.controlHeight

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: Math.max(0, (side - Theme.iconSize) / 2)
    leftPadding: text !== "" ? Theme.spacingS : padding
    rightPadding: text !== "" ? Theme.spacingS : padding
    spacing: Theme.spacingXs
    font: Theme.uiFont
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: text !== "" ? text : toolTipText
    Accessible.description: toolTipText

    contentItem: IconText {
        iconName: control.iconName
        text: control.text
        font: control.font
        spacing: control.spacing
        color: !control.enabled ? Theme.textDisabled
               : control.checked ? Theme.accentText
               : control.hovered || control.down ? Theme.textPrimary
               : Theme.textSecondary
    }

    background: Rectangle {
        implicitWidth: control.side
        implicitHeight: control.side
        radius: Math.min(Theme.radiusControl, height / 2)
        color: control.down ? Theme.controlBgPressed
               : control.checked ? Theme.controlBgPressed
               : control.hovered && control.enabled ? Theme.controlBgHover
               : "transparent"

        FocusRing {
            visible: control.visualFocus
        }
    }

    HoverToolTip {
        text: control.toolTipText
    }
}
