// Spike S1, variant A: QQuickRhiItem viewport, three panels sampling the shared backdrop texture of the renderer.
import QtQuick
import QtQuick.Window
import S1 1.0

Window {
    id: win
    property bool glass: true
    property int msaa: 1
    property real flipY: 0
    visible: false
    color: "black"
    title: "S1 A: own renderer on Qt RHI"

    Viewport { id: vp; objectName: "viewport"; anchors.fill: parent; sampleCount: win.msaa; backdropEnabled: win.glass }
    Backdrop { id: bd; viewport: vp }

    Panel { x: win.width * 0.0125; y: win.height * 0.022; width: win.width * 0.16; height: win.height * 0.956
            glass: win.glass; backdrop: bd; view: vp; flipY: win.flipY; title: "Browser" }
    Panel { x: win.width * 0.8275; y: win.height * 0.022; width: win.width * 0.16; height: win.height * 0.55
            glass: win.glass; backdrop: bd; view: vp; flipY: win.flipY; title: "Properties" }
    Panel { x: win.width * 0.185; y: win.height * 0.86; width: win.width * 0.63; height: win.height * 0.118
            glass: win.glass; backdrop: bd; view: vp; flipY: win.flipY; title: "Timeline" }
}
