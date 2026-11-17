import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GlassPanel {
    implicitWidth: row.implicitWidth + 8
    implicitHeight: row.implicitHeight + 8
    opacity: 0.92
    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 2
        Repeater {
            model: ["Orbit", "Pan", "Zoom", "Fit", "Section", "Display", "Grid"]
            ToolButtonIcon {
                required property string modelData
                required property int index
                glyph: index + 3
                tip: modelData
            }
        }
    }
}
