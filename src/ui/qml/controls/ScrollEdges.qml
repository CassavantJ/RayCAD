// The edges of a scrolling area between a fixed header and footer (a panel's or a dialog's body): while there is more
// above, a thin separator at its top edge with a short fade under it; while there is more below, the same at its bottom
// edge. A row the scroll cuts then reads as scrolled under the edge, not as a broken layout. Put it over the Flickable,
// filling it (anchors.fill: the flickable); it takes no input.
import QtQuick
import RayCad.Ui

Item {
    id: root

    required property Flickable area
    // The colour the fade comes out of: the surface the area is drawn on.
    property color surface: Theme.panelBg
    // false where the header or footer draws its own rule at that edge
    property bool topLine: true
    property bool bottomLine: true
    readonly property bool moreAbove: area.contentY > 0.5
    readonly property bool moreBelow: area.contentY + area.height < area.contentHeight - 0.5
    readonly property real fade: Theme.spacingM

    Rectangle {
        objectName: "scrollEdgeTopFade"
        visible: root.moreAbove
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.fade
        gradient: Gradient {
            GradientStop { position: 0.0; color: root.surface }
            GradientStop { position: 1.0; color: Qt.rgba(root.surface.r, root.surface.g, root.surface.b, 0) }
        }
    }
    Rectangle {
        objectName: "scrollEdgeTop"
        visible: root.moreAbove && root.topLine
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Theme.borderWidth
        color: Theme.panelBorder
    }
    Rectangle {
        objectName: "scrollEdgeBottomFade"
        visible: root.moreBelow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.fade
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(root.surface.r, root.surface.g, root.surface.b, 0) }
            GradientStop { position: 1.0; color: root.surface }
        }
    }
    Rectangle {
        objectName: "scrollEdgeBottom"
        visible: root.moreBelow && root.bottomLine
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.borderWidth
        color: Theme.panelBorder
    }
}
