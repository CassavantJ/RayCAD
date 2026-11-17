import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root
    property string label
    default property alias editor: slot.data
    spacing: Theme.spacing
    Layout.fillWidth: true
    Label {
        text: root.label
        color: Theme.textDim
        font.pixelSize: Theme.fontSize
        Layout.preferredWidth: 96
        elide: Text.ElideRight
    }
    RowLayout { id: slot; Layout.fillWidth: true; spacing: 4 }
}
