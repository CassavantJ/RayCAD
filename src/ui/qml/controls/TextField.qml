// Single-line text input. `iconName` adds a leading icon (search fields); `invalid` draws the error outline.
// Without a visible label, set Accessible.name (the placeholder is used otherwise).
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.TextField {
    id: control

    property string iconName: ""
    property bool invalid: false
    readonly property real iconSpace: iconName !== "" ? Theme.iconSize + Theme.spacingXs : 0

    implicitWidth: implicitBackgroundWidth + leftInset + rightInset
                   || Math.max(contentWidth, placeholder.implicitWidth) + leftPadding + rightPadding
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding,
                             placeholder.implicitHeight + topPadding + bottomPadding)

    topPadding: Theme.spacingXs
    bottomPadding: Theme.spacingXs
    leftPadding: Theme.spacingM + iconSpace
    rightPadding: Theme.spacingM
    font: Theme.uiFont
    color: enabled ? Theme.textPrimary : Theme.textDisabled
    selectionColor: Theme.selectionFill
    selectedTextColor: Theme.textPrimary
    placeholderTextColor: enabled ? Theme.textSecondary : Theme.textDisabled
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true
    hoverEnabled: true

    Accessible.name: placeholderText

    Text {
        id: placeholder
        objectName: "placeholder"
        x: control.leftPadding
        y: control.topPadding
        width: control.width - (control.leftPadding + control.rightPadding)
        height: control.height - (control.topPadding + control.bottomPadding)
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: control.verticalAlignment
        horizontalAlignment: control.horizontalAlignment === TextInput.AlignRight ? Text.AlignRight : Text.AlignLeft
        visible: !control.length && !control.preeditText
        elide: Text.ElideRight
        textFormat: Text.PlainText
    }

    Icon {
        visible: control.iconName !== ""
        name: control.iconName
        color: control.enabled ? Theme.textSecondary : Theme.textDisabled
        x: Theme.spacingM - Theme.spacingXs / 2
        anchors.verticalCenter: parent.verticalCenter
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight * 6
        implicitHeight: Theme.controlHeight
        radius: Math.min(Theme.radiusControl, height / 2)
        color: control.enabled ? Theme.fieldBg : Theme.controlBg
        border.width: control.activeFocus ? Theme.focusRingWidth : Theme.borderWidth
        border.color: control.invalid ? Theme.error
                      : control.activeFocus ? Theme.focusRing
                      : control.hovered && control.enabled ? Theme.controlBorder
                      : Theme.panelBorder
    }
}
