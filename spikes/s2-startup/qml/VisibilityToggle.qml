import QtQuick
import QtQuick.Controls

AbstractButton {
    id: root
    checkable: true
    checked: true
    implicitWidth: 20
    implicitHeight: 20
    contentItem: IconGlyph { kind: 4; color: root.checked ? Theme.text : Theme.textDim }
}
