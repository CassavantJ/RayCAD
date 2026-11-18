// Tooltip popup. Controls create one lazily through HoverToolTip, so none exists until the pointer rests.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.ToolTip {
    id: control

    x: parent ? Math.round((parent.width - implicitWidth) / 2) : 0
    y: parent ? parent.height + Theme.spacingXs : 0
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    // Long tips wrap instead of growing without bound.
    width: Math.min(implicitWidth, Theme.controlHeight * 12 + leftPadding + rightPadding)

    margins: Theme.spacingS
    topPadding: Theme.spacingXs
    bottomPadding: Theme.spacingXs
    leftPadding: Theme.spacingS
    rightPadding: Theme.spacingS
    delay: Theme.toolTipDelay
    font: Theme.uiFont
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        objectName: "toolTipText"
        text: control.text
        font: control.font
        color: Theme.textPrimary
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }

    background: Rectangle {
        color: Theme.tooltipBg
        radius: Theme.radiusSmall
        border.width: Theme.borderWidth
        border.color: Theme.panelBorder
    }
}
