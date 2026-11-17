import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    property string title
    property var tools: []
    spacing: 2
    RowLayout {
        spacing: 2
        Repeater {
            model: root.tools
            ToolButtonIcon {
                required property var modelData
                required property int index
                glyph: index + root.title.length
                tip: modelData
            }
        }
    }
    Label {
        text: root.title
        color: Theme.textDim
        font.pixelSize: 10
        Layout.alignment: Qt.AlignHCenter
    }
}
