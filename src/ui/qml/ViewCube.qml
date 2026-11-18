// The view cube and its view buttons (task T-020), drawn for RayCAD: a flat-shaded cube that turns with the camera,
// in the top-right corner of the free viewport area, and a small axis triad in the bottom-left corner.
// Click a face for that standard view, a corner dot for the view along that diagonal. Fill the shell's
// viewportArea with it; only the cube, its buttons and the triad take input.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

Item {
    id: root
    objectName: "viewCube"

    // The ViewportItem to steer.
    required property var view

    readonly property vector3d camRight: view ? view.viewRight : Qt.vector3d(1, 0, 0)
    readonly property vector3d camUp: view ? view.viewUp : Qt.vector3d(0, 0, 1)
    readonly property vector3d camForward: view ? view.viewForward : Qt.vector3d(0, 1, 0)

    // Faces: the standard view each one shows (the Front view looks along +Y, so its face is the -Y side).
    readonly property var faces: [
        { key: "top", label: qsTr("Top"), n: Qt.vector3d(0, 0, 1) },
        { key: "bottom", label: qsTr("Bottom"), n: Qt.vector3d(0, 0, -1) },
        { key: "front", label: qsTr("Front"), n: Qt.vector3d(0, -1, 0) },
        { key: "back", label: qsTr("Back"), n: Qt.vector3d(0, 1, 0) },
        { key: "right", label: qsTr("Right"), n: Qt.vector3d(1, 0, 0) },
        { key: "left", label: qsTr("Left"), n: Qt.vector3d(-1, 0, 0) }
    ]
    readonly property var corners: [
        Qt.vector3d(1, 1, 1), Qt.vector3d(-1, 1, 1), Qt.vector3d(1, -1, 1), Qt.vector3d(-1, -1, 1),
        Qt.vector3d(1, 1, -1), Qt.vector3d(-1, 1, -1), Qt.vector3d(1, -1, -1), Qt.vector3d(-1, -1, -1)
    ]

    // The four corners of a face, in order around it.
    function faceCorners(n) {
        const axes = n.x !== 0 ? ["y", "z"] : n.y !== 0 ? ["z", "x"] : ["x", "y"]
        const signs = [[-1, -1], [1, -1], [1, 1], [-1, 1]]
        const out = []
        for (let i = 0; i < 4; ++i) {
            const c = Qt.vector3d(n.x, n.y, n.z)
            c[axes[0]] = signs[i][0]
            c[axes[1]] = signs[i][1]
            out.push(c)
        }
        return out
    }

    Column {
        id: cubeColumn
        objectName: "viewCubeColumn"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.spacingS
        spacing: Theme.spacingXs

        Item {
            id: cube
            objectName: "viewCubeCube"
            width: Theme.controlHeight * 3
            height: width
            anchors.horizontalCenter: parent.horizontalCenter
            readonly property real cx: width / 2
            readonly property real cy: height / 2
            readonly property real scale3d: width * 0.27

            function px(c) {
                return Qt.point(cx + scale3d * c.dotProduct(root.camRight), cy - scale3d * c.dotProduct(root.camUp))
            }

            Repeater {
                model: root.faces
                delegate: Shape {
                    id: face
                    required property var modelData
                    readonly property real facing: -modelData.n.dotProduct(root.camForward)
                    readonly property var pts: {
                        const cs = root.faceCorners(modelData.n)
                        const p = cs.map(c => cube.px(c))
                        p.push(p[0])
                        return p
                    }
                    objectName: "viewCubeFace." + modelData.key
                    anchors.fill: parent
                    visible: facing > 0.02
                    containsMode: Shape.FillContains
                    preferredRendererType: Shape.CurveRenderer
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("%1 view").arg(modelData.label)

                    ShapePath {
                        strokeColor: Theme.controlBorder
                        strokeWidth: Theme.borderWidth
                        joinStyle: ShapePath.RoundJoin
                        fillColor: faceHover.hovered ? Theme.controlBgHover : Theme.controlBg
                        PathPolyline {
                            path: face.pts
                        }
                    }
                    HoverHandler {
                        id: faceHover
                    }
                    TapHandler {
                        onTapped: if (root.view) root.view.setStandardView(face.modelData.key)
                    }
                }
            }

            // Face names at the face centres, faded as the face turns away.
            Repeater {
                model: root.faces
                delegate: Text {
                    required property var modelData
                    readonly property real facing: -modelData.n.dotProduct(root.camForward)
                    readonly property point at: cube.px(modelData.n)
                    visible: facing > 0.35
                    opacity: Math.min(1, (facing - 0.35) * 3)
                    x: at.x - width / 2
                    y: at.y - height / 2
                    text: modelData.label
                    font: Theme.smallFont
                    color: Theme.textPrimary
                    Accessible.ignored: true
                }
            }

            Repeater {
                model: root.corners
                delegate: Rectangle {
                    id: corner
                    required property var modelData
                    required property int index
                    readonly property point at: cube.px(modelData)
                    objectName: "viewCubeCorner." + index
                    visible: -modelData.normalized().dotProduct(root.camForward) > 0.3
                    width: Theme.spacingS
                    height: width
                    radius: width / 2
                    x: at.x - width / 2
                    y: at.y - height / 2
                    color: cornerHover.hovered ? Theme.accent : Theme.textSecondary
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Corner view")

                    HoverHandler {
                        id: cornerHover
                    }
                    TapHandler {
                        onTapped: if (root.view) root.view.viewAlong(-corner.modelData.x, -corner.modelData.y,
                                                                    -corner.modelData.z)
                    }
                }
            }
        }

        PanelBase {
            objectName: "viewButtons"
            anchors.horizontalCenter: parent.horizontalCenter
            width: viewButtons.implicitWidth + 2 * Theme.spacingXs
            height: viewButtons.implicitHeight + 2 * Theme.spacingXs

            Row {
                id: viewButtons
                anchors.centerIn: parent
                spacing: Theme.spacingXs / 2

                ToolButton {
                    objectName: "fitAllButton"
                    compact: true
                    text: qsTr("Fit")
                    toolTipText: qsTr("Fit everything in the view (F)")
                    onClicked: if (root.view) root.view.fitAll()
                }
                ToolButton {
                    objectName: "isoViewButton"
                    compact: true
                    text: qsTr("Iso")
                    toolTipText: qsTr("Isometric view (0)")
                    onClicked: if (root.view) root.view.setStandardView("iso")
                }
                ToolButton {
                    objectName: "projectionButton"
                    compact: true
                    text: root.view && root.view.perspective ? qsTr("Persp") : qsTr("Ortho")
                    toolTipText: root.view && root.view.perspective
                                 ? qsTr("Perspective projection (switch to orthographic, P)")
                                 : qsTr("Orthographic projection (switch to perspective, P)")
                    onClicked: if (root.view) root.view.toggleProjection()
                }
            }
        }
    }

    // Axis triad: the world X, Y and Z directions as the camera sees them.
    Item {
        id: triad
        objectName: "axisTriad"
        width: Theme.controlHeight * 2
        height: width
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        readonly property real cx: width / 2
        readonly property real cy: height / 2
        readonly property real len: width * 0.32
        Accessible.ignored: true

        function end(axis) {
            return Qt.point(cx + len * axis.dotProduct(root.camRight), cy - len * axis.dotProduct(root.camUp))
        }
        readonly property point ex: end(Qt.vector3d(1, 0, 0))
        readonly property point ey: end(Qt.vector3d(0, 1, 0))
        readonly property point ez: end(Qt.vector3d(0, 0, 1))

        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Theme.axisX
                strokeWidth: Theme.lineThinPx
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                startX: triad.cx
                startY: triad.cy
                PathLine {
                    x: triad.ex.x
                    y: triad.ex.y
                }
            }
            ShapePath {
                strokeColor: Theme.axisY
                strokeWidth: Theme.lineThinPx
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                startX: triad.cx
                startY: triad.cy
                PathLine {
                    x: triad.ey.x
                    y: triad.ey.y
                }
            }
            ShapePath {
                strokeColor: Theme.axisZ
                strokeWidth: Theme.lineThinPx
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                startX: triad.cx
                startY: triad.cy
                PathLine {
                    x: triad.ez.x
                    y: triad.ez.y
                }
            }
        }
        Text {
            text: "X"
            font: Theme.smallFont
            color: Theme.axisX
            x: triad.ex.x + (triad.ex.x - triad.cx) * 0.25 - width / 2
            y: triad.ex.y + (triad.ex.y - triad.cy) * 0.25 - height / 2
        }
        Text {
            text: "Y"
            font: Theme.smallFont
            color: Theme.axisY
            x: triad.ey.x + (triad.ey.x - triad.cx) * 0.25 - width / 2
            y: triad.ey.y + (triad.ey.y - triad.cy) * 0.25 - height / 2
        }
        Text {
            text: "Z"
            font: Theme.smallFont
            color: Theme.axisZ
            x: triad.ez.x + (triad.ez.x - triad.cx) * 0.25 - width / 2
            y: triad.ez.y + (triad.ez.y - triad.cy) * 0.25 - height / 2
        }
    }
}
