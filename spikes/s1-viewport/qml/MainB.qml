// Spike S1, variant B: OCCT V3d_View in a QQuickFramebufferObject (Qt Quick on OpenGL), same three panels.
// Glass: ShaderEffectSource (quarter resolution) -> one MultiEffect blur -> ShaderEffectSource, shared by all panels.
import QtQuick
import QtQuick.Window
import QtQuick.Effects
import S1 1.0

Window {
    id: win
    property bool glass: true
    property int msaa: 1
    property real flipY: 0
    visible: false
    color: "black"
    title: "S1 B: OCCT V3d_View in QQuickFramebufferObject"

    OcctView { id: vp; objectName: "viewport"; anchors.fill: parent }

    Loader {
        id: chain
        active: win.glass
        sourceComponent: Item {
            property alias output: blurred
            width: vp.width / 4; height: vp.height / 4
            visible: false
            ShaderEffectSource {
                id: quarter
                anchors.fill: parent
                sourceItem: vp
                textureSize: Qt.size(Math.round(vp.width * Screen.devicePixelRatio / 4), Math.round(vp.height * Screen.devicePixelRatio / 4))
                live: true; smooth: true; visible: false
            }
            MultiEffect {
                id: blur
                anchors.fill: parent
                source: quarter
                blurEnabled: true; blur: 1.0; blurMax: 16
                visible: false
            }
            ShaderEffectSource {
                id: blurred
                anchors.fill: parent
                sourceItem: blur
                textureSize: quarter.textureSize
                live: true; smooth: true; visible: false
            }
        }
    }
    property var bd: chain.item ? chain.item.output : null

    Panel { x: win.width * 0.0125; y: win.height * 0.022; width: win.width * 0.16; height: win.height * 0.956
            glass: win.glass; backdrop: win.bd; view: vp; flipY: win.flipY; title: "Browser" }
    Panel { x: win.width * 0.8275; y: win.height * 0.022; width: win.width * 0.16; height: win.height * 0.55
            glass: win.glass; backdrop: win.bd; view: vp; flipY: win.flipY; title: "Properties" }
    Panel { x: win.width * 0.185; y: win.height * 0.86; width: win.width * 0.63; height: win.height * 0.118
            glass: win.glass; backdrop: win.bd; view: vp; flipY: win.flipY; title: "Timeline" }
}
