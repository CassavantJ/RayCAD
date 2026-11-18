// The 3D view (task T-020): fills the shell's viewportHost, under every panel (PLAN.md 6.6).
//
//   viewportItem      the ViewportItem (C++, viewport_item.h): our RHI renderer inside Qt Quick. It takes the
//                     Scene's meshes, the Selection's highlight, the Theme's colours and the tier preset, all live.
//   viewportFallback  a plain backdrop, shown only where the scene graph has no QRhi (QT_QUICK_BACKEND=software).
//   selectionBox      the rubber band of a box selection.
//   dropArea          drop RayCAD, STEP or IGES files anywhere on the window to open them (Files, T-019).
// Esc (with the view focused) cancels the running job, else the box, else clears the selection.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "viewport"

    readonly property alias view: view
    // The free part of the view (no panel over it), in this item's coordinates: fit frames the model there.
    property rect frameRect: Qt.rect(0, 0, width, height)

    Rectangle {
        objectName: "viewportFallback"
        anchors.fill: parent
        visible: !view.rhiAvailable
        gradient: Gradient {
            GradientStop {
                position: 0
                color: Theme.viewportBgTop
            }
            GradientStop {
                position: 1
                color: Theme.viewportBgBottom
            }
        }
    }

    ViewportItem {
        id: view
        objectName: "viewportItem"
        anchors.fill: parent
        visible: rhiAvailable
        focus: true
        scene: Scene
        selection: Selection
        theme: Theme
        frameRect: root.frameRect
        Accessible.role: Accessible.Graphic
        Accessible.name: qsTr("3D view")

        Keys.onEscapePressed: {
            if (Session.jobActive)
                Session.cancelForegroundJob()
            else if (boxActive)
                cancelBox()
            else
                Selection.clear()
        }
    }

    Rectangle {
        objectName: "selectionBox"
        visible: view.boxActive
        x: view.boxRect.x
        y: view.boxRect.y
        width: view.boxRect.width
        height: view.boxRect.height
        color: Theme.selectionFill
        border.width: Theme.borderWidth
        border.color: Theme.selection
    }

    DropArea {
        id: dropArea
        objectName: "dropArea"
        anchors.fill: parent
        onEntered: drag => {
            drag.accepted = drag.hasUrls && Files.canOpenAny(drag.urls)
        }
        onDropped: drop => {
            if (drop.hasUrls && Files.openUrls(drop.urls))
                drop.acceptProposedAction()
        }
    }
}
