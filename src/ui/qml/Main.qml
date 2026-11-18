// The main window. It starts hidden: AppHost sets the graphics configuration and size, then shows it.
// Shows the shell, or the controls gallery with --gallery (loaded by URL, so a normal start never compiles it).
import QtQuick
import RayCad.Ui

Window {
    id: root

    visible: false
    minimumWidth: Theme.controlHeight * 32
    minimumHeight: Theme.controlHeight * 20
    // The document name, its dirty marker and read-only state (Session, T-019).
    title: AppContext.page === "gallery" ? qsTr("RayCAD controls gallery") : Session.windowTitle
    color: Theme.windowBg

    Loader {
        id: page
        objectName: "page"
        anchors.fill: parent
        focus: true
        Component.onCompleted: {
            if (AppContext.page === "gallery")
                source = Qt.resolvedUrl("Gallery.qml")
            else
                sourceComponent = shellPage
        }
    }

    Component {
        id: shellPage
        Shell {}
    }
}
