// Drag handle between a panel and its neighbour. moved(delta) reports drag distance in logical pixels along the
// resize axis; the owner clamps the size. Focusable: arrow keys move by Theme.spacingS; double-click (or Home)
// emits reset(). The line shows on hover, drag and keyboard focus.
import QtQuick
import RayCad.Ui

Item {
    id: root

    // Qt.Horizontal: a vertical line between left and right neighbours, dragged along x.
    property int orientation: Qt.Horizontal
    property string accessibleName: qsTr("Resize panel")
    readonly property bool horizontal: orientation === Qt.Horizontal
    readonly property bool active: drag.active || hover.hovered || activeFocus

    signal moved(real delta)
    signal reset()

    implicitWidth: horizontal ? Theme.spacingS : Theme.controlHeight
    implicitHeight: horizontal ? Theme.controlHeight : Theme.spacingS
    activeFocusOnTab: true

    Accessible.role: Accessible.Grip
    Accessible.name: accessibleName
    Accessible.focusable: true

    Keys.onLeftPressed: if (horizontal) moved(-Theme.spacingS)
    Keys.onRightPressed: if (horizontal) moved(Theme.spacingS)
    Keys.onUpPressed: if (!horizontal) moved(-Theme.spacingS)
    Keys.onDownPressed: if (!horizontal) moved(Theme.spacingS)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) {
            reset()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.centerIn: parent
        readonly property real thickness: drag.active || root.activeFocus ? Theme.focusRingWidth : Theme.borderWidth
        width: root.horizontal ? thickness : Math.min(parent.width, Theme.controlHeight * 2)
        height: root.horizontal ? Math.min(parent.height, Theme.controlHeight * 2) : thickness
        radius: thickness / 2
        visible: root.active
        color: drag.active || root.activeFocus ? Theme.focusRing : Theme.controlBorder
    }

    HoverHandler {
        id: hover
        cursorShape: root.horizontal ? Qt.SplitHCursor : Qt.SplitVCursor
    }

    DragHandler {
        id: drag
        property real last: 0
        target: null
        xAxis.enabled: root.horizontal
        yAxis.enabled: !root.horizontal
        cursorShape: root.horizontal ? Qt.SplitHCursor : Qt.SplitVCursor
        onActiveChanged: last = 0
        onActiveTranslationChanged: {
            const now = root.horizontal ? activeTranslation.x : activeTranslation.y
            root.moved(now - last)
            last = now
        }
    }

    TapHandler {
        onDoubleTapped: root.reset()
    }
}
