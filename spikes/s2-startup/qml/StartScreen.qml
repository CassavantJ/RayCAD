import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GlassPanel {
    implicitWidth: 540
    implicitHeight: 250
    opacity: 0.96
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10
        Label { text: "RayCAD"; color: Theme.text; font.pixelSize: 22; font.bold: true }
        Label { text: "Recent documents"; color: Theme.textDim; font.pixelSize: Theme.fontSize }
        RowLayout {
            spacing: 10
            RecentFileCard { fileName: "fixture_plate.raycad"; detail: "Today 09:12" }
            RecentFileCard { fileName: "manifold_block.raycad"; detail: "Yesterday" }
            RecentFileCard { fileName: "steel_bracket.raycad"; detail: "12 Nov" }
        }
        RowLayout {
            Button { text: "New document"; highlighted: true }
            Button { text: "Open..." }
            Button { text: "Import STEP..." }
            Item { Layout.fillWidth: true }
            CheckBox { text: "Show at start"; checked: true }
        }
    }
}
