import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GlassPanel {
    implicitHeight: 40
    radius: 0
    RowLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 2
        ToolButtonIcon { glyph: 1; tip: "Back"; implicitWidth: 28; implicitHeight: 28 }
        ToolButtonIcon { glyph: 2; tip: "Forward"; implicitWidth: 28; implicitHeight: 28 }
        RibbonSeparator { implicitHeight: 24 }
        Repeater {
            model: 24
            TimelineItem {
                required property int index
                glyph: index
                current: index === 17
            }
        }
        Item { Layout.fillWidth: true }
    }
}
