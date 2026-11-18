// Numeric input with a unit suffix ("mm", "deg", "rpm"), in the mono font, right-aligned.
// Up/Down step by stepSize (Shift: 10x). Enter or leaving the field commits; valueModified(value) fires on a user
// change. Text that does not parse keeps the old value and shows the error outline.
// Unit conversion and expressions ("1/2 in") come with the settings and units task (T-024).
import QtQuick
import RayCad.Ui

TextField {
    id: control

    property real value: 0
    property real from: -1e9
    property real to: 1e9
    property real stepSize: 1
    property int decimals: 3
    property string unit: ""
    property string label: ""

    signal valueModified(real value)

    function format(v) {
        return Number(v).toFixed(control.decimals)
    }
    function clamp(v) {
        return Math.min(control.to, Math.max(control.from, v))
    }
    function commit() {
        const parsed = Number(control.text.trim())
        if (control.text.trim() === "" || !isFinite(parsed)) {
            control.invalid = true
            return
        }
        control.invalid = false
        const next = control.clamp(parsed)
        control.text = control.format(next)
        if (next !== control.value) {
            control.value = next
            control.valueModified(next)
        }
    }
    function step(direction, big) {
        const next = control.clamp(control.value + direction * control.stepSize * (big ? 10 : 1))
        control.invalid = false
        control.text = control.format(next)
        if (next !== control.value) {
            control.value = next
            control.valueModified(next)
        }
    }

    font: Theme.monoFont
    horizontalAlignment: TextInput.AlignRight
    rightPadding: Theme.spacingM + (unit !== "" ? suffix.implicitWidth + Theme.spacingXs : 0)
    inputMethodHints: Qt.ImhFormattedNumbersOnly
    implicitWidth: Theme.controlHeight * 4

    Accessible.role: Accessible.SpinBox
    Accessible.name: (label !== "" ? label + " " : "") + text + (unit !== "" ? " " + unit : "")

    Component.onCompleted: text = format(value)
    onValueChanged: if (!activeFocus) text = format(value)
    onEditingFinished: commit()
    onActiveFocusChanged: if (!activeFocus) commit()
    Keys.onUpPressed: event => control.step(1, event.modifiers & Qt.ShiftModifier)
    Keys.onDownPressed: event => control.step(-1, event.modifiers & Qt.ShiftModifier)

    Text {
        id: suffix
        objectName: "unitSuffix"
        visible: control.unit !== ""
        text: control.unit
        font: Theme.monoFont
        color: control.enabled ? Theme.textSecondary : Theme.textDisabled
        textFormat: Text.PlainText
        anchors.right: parent.right
        anchors.rightMargin: Theme.spacingM
        // Same font and size as the number, centred the same way, so both sit on one baseline.
        anchors.verticalCenter: parent.verticalCenter
    }
}
