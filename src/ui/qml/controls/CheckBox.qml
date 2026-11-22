// Check box with label. tristate is supported (PartiallyChecked draws a dash). The label wraps when the check box is
// given a width narrower than it, never elided; the box stays beside its first line.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.CheckBox {
    id: control

    FontMetrics {
        id: labelMetrics
        font: control.font
    }

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
        readonly property bool marked: control.checkState !== Qt.Unchecked
        implicitWidth: Theme.indicatorSize
        implicitHeight: Theme.indicatorSize
        x: control.leftPadding
        // Centred on the label's first line (on the whole label while it is one line).
        y: control.topPadding
           + Math.round((Math.min(control.availableHeight, Math.max(height, labelMetrics.height)) - height) / 2)
        radius: Math.min(Theme.radiusSmall, Theme.spacingXs)
        color: marked ? (control.enabled ? Theme.accentThinLine : Theme.controlBgPressed)
               : control.hovered && control.enabled ? Theme.controlBgHover
               : Theme.fieldBg
        border.width: marked ? 0 : Theme.borderWidth
        border.color: control.enabled ? Theme.controlBorder : Theme.panelBorder

        Icon {
            anchors.centerIn: parent
            size: parent.width
            visible: parent.marked
            name: control.checkState === Qt.PartiallyChecked ? "dash" : "check"
            color: control.enabled ? Theme.onAccentLine : Theme.textDisabled
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
        wrapMode: Text.Wrap
        elide: Text.ElideNone
        textFormat: Text.PlainText
        // From the top, the first line level with the box (centred on it when the box is the taller).
        verticalAlignment: Text.AlignTop
        topPadding: Math.max(0, Math.round((control.indicator.height - labelMetrics.height) / 2))
    }
}
