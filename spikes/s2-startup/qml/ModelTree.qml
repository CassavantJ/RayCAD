import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GlassPanel {
    id: root
    property int currentRow: 3
    ListModel {
        id: treeModel
        ListElement { label: "fixture_plate"; depth: 0; glyph: 0; kids: true; off: false }
        ListElement { label: "Origin"; depth: 1; glyph: 1; kids: true; off: false }
        ListElement { label: "XY plane"; depth: 2; glyph: 2; kids: false; off: false }
        ListElement { label: "XZ plane"; depth: 2; glyph: 2; kids: false; off: false }
        ListElement { label: "YZ plane"; depth: 2; glyph: 2; kids: false; off: false }
        ListElement { label: "Sketches"; depth: 1; glyph: 3; kids: true; off: false }
        ListElement { label: "Sketch 1"; depth: 2; glyph: 3; kids: false; off: false }
        ListElement { label: "Sketch 2"; depth: 2; glyph: 3; kids: false; off: false }
        ListElement { label: "Sketch 3"; depth: 2; glyph: 3; kids: false; off: false }
        ListElement { label: "Bodies"; depth: 1; glyph: 5; kids: true; off: false }
        ListElement { label: "Body 1"; depth: 2; glyph: 5; kids: false; off: false }
        ListElement { label: "Features"; depth: 1; glyph: 6; kids: true; off: false }
        ListElement { label: "Extrude 1"; depth: 2; glyph: 6; kids: false; off: false }
        ListElement { label: "Pocket 1"; depth: 2; glyph: 7; kids: false; off: false }
        ListElement { label: "Pocket 2"; depth: 2; glyph: 7; kids: false; off: false }
        ListElement { label: "Hole 1 (1/4-20 UNC)"; depth: 2; glyph: 8; kids: false; off: false }
        ListElement { label: "Pattern 1"; depth: 2; glyph: 9; kids: false; off: false }
        ListElement { label: "Counterbore 1"; depth: 2; glyph: 8; kids: false; off: false }
        ListElement { label: "Fillet 1"; depth: 2; glyph: 10; kids: false; off: true }
        ListElement { label: "Chamfer 1"; depth: 2; glyph: 11; kids: false; off: false }
        ListElement { label: "Setups"; depth: 1; glyph: 12; kids: true; off: false }
        ListElement { label: "Setup 1 (top)"; depth: 2; glyph: 12; kids: false; off: false }
        ListElement { label: "Setup 2 (flip)"; depth: 2; glyph: 12; kids: false; off: false }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0
        PanelHeader { title: "Model"; Layout.fillWidth: true }
        SearchField { Layout.fillWidth: true; Layout.margins: 4; placeholderText: "Filter" }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: treeModel
            ScrollBar.vertical: ScrollBar {}
            delegate: TreeRow {
                required property int index
                required property bool kids
                required label
                required depth
                required glyph
                required property bool off
                width: ListView.view.width
                hasChildren: kids
                suppressed: off
                highlighted: index === root.currentRow
                onClicked: root.currentRow = index
            }
        }
    }
}
