import QtQuick
import QtQuick.Controls

ToolButton {
    id: root
    property int glyph: 0
    property string tip
    implicitWidth: 34
    implicitHeight: 34
    contentItem: IconGlyph { kind: root.glyph; color: root.enabled ? Theme.text : Theme.textDim }
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.text: tip
    ToolTip.delay: 600
}
