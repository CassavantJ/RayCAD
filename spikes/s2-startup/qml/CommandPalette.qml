import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    width: 420
    height: 260
    modal: true
    focus: true
    background: GlassPanel {}
    ColumnLayout {
        anchors.fill: parent
        SearchField { Layout.fillWidth: true }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: ["Extrude", "Fillet", "Adaptive clearing", "Post process", "Measure"]
            delegate: ItemDelegate {
                required property string modelData
                width: ListView.view.width
                text: modelData
            }
        }
    }
}
