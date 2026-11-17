import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

TextField {
    id: root
    property real value: 0
    property string unit: "mm"
    text: value.toFixed(3) + " " + unit
    color: Theme.text
    font.pixelSize: Theme.fontSize
    implicitHeight: 24
    Layout.fillWidth: true
    selectByMouse: true
    validator: RegularExpressionValidator { regularExpression: /[-+0-9., a-z]*/ }
    background: Rectangle {
        color: Theme.window
        border.color: root.activeFocus ? Theme.accent : Theme.border
        radius: 3
    }
}
