import QtQuick
import QtQuick.Controls

// Placeholder for the 3D viewport item (QQuickRhiItem in the product).
Rectangle {
    id: root
    gradient: Gradient {
        GradientStop { position: 0; color: "#39424e" }
        GradientStop { position: 1; color: "#14171b" }
    }
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; hoverEnabled: true }
    Label {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 10
        text: "viewport placeholder"
        color: Theme.textDim
        font.pixelSize: 12
    }
    ViewCube { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 12 }
    ViewportToolbar { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 12 }
}
