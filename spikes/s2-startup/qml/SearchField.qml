import QtQuick
import QtQuick.Controls

TextField {
    id: root
    placeholderText: "Search commands (Ctrl+K)"
    color: Theme.text
    font.pixelSize: Theme.fontSize
    selectByMouse: true
    background: Rectangle {
        color: Theme.window
        border.color: root.activeFocus ? Theme.accent : Theme.border
        radius: Theme.radius
    }
}
