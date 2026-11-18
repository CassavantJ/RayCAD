// Keyboard focus outline drawn just outside its parent (a control's background). Does not change any geometry.
import QtQuick
import RayCad.Ui

Rectangle {
    id: ring

    property real gap: Theme.borderWidth
    readonly property real parentRadius: parent && parent.radius !== undefined ? parent.radius : 0

    anchors.fill: parent
    anchors.margins: -(gap + Theme.focusRingWidth)
    radius: parentRadius > 0 ? parentRadius + gap + Theme.focusRingWidth : 0
    color: "transparent"
    border.width: Theme.focusRingWidth
    border.color: Theme.focusRing
}
