// Spike S1: floating panel. Glass mode samples ONE shared blurred backdrop texture at the screen position
// of the panel; flat mode is an opaque rounded rectangle. Layout is identical in both modes.
import QtQuick

Item {
    id: root
    property bool glass: true
    property var backdrop: null     // texture provider item (shared)
    property Item view: null        // the viewport item the backdrop was made from
    property real flipY: 0
    property string title: ""

    Loader {
        anchors.fill: parent
        active: root.glass && root.backdrop !== null
        sourceComponent: ShaderEffect {
            property var backdrop: root.backdrop
            property vector4d rectInView: Qt.vector4d(root.x / root.view.width, root.y / root.view.height,
                                                      root.width / root.view.width, root.height / root.view.height)
            property vector2d sizePx: Qt.vector2d(width, height)
            property real radius: 14
            property real flipY: root.flipY
            property color tint: Qt.rgba(0.10, 0.11, 0.13, 0.55)
            fragmentShader: "qrc:/shaders/panel.frag.qsb"
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: !root.glass
        radius: 14
        color: "#1c1e22"
        border.color: "#3a3d45"
        border.width: 1
    }
    Column {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 6
        clip: true
        Text { text: root.title; color: "#f2f3f5"; font.pixelSize: 15; font.bold: true }
        Repeater {
            model: Math.max(0, Math.floor((root.height - 60) / 22))
            Text { text: "Item " + (index + 1) + "   12.500 mm"; color: "#c9ccd3"; font.pixelSize: 13 }
        }
    }
}
