// Creates a ToolTip for `target` only while the pointer is over it (start-up cost: one inactive Loader).
import QtQuick
import RayCad.Ui

Loader {
    id: loader

    property Item target: parent
    property string text

    active: text !== "" && target !== null && target.hovered === true && target.pressed !== true
    sourceComponent: Component {
        Item {
            ToolTip {
                parent: loader.target
                text: loader.text
                visible: true
            }
        }
    }
}
