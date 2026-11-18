// Segmented tab bar (workspace tabs, panel tabs). Left/Right arrows move between tabs.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.TabBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    spacing: Theme.spacingXs / 2
    padding: Theme.spacingXs / 2
    Accessible.role: Accessible.PageTabList

    function moveBy(step) {
        const next = Math.max(0, Math.min(count - 1, currentIndex + step))
        if (next === currentIndex)
            return
        setCurrentIndex(next)
        const tab = itemAt(next)
        if (tab)
            tab.forceActiveFocus(Qt.TabFocusReason)
    }
    Keys.onLeftPressed: moveBy(-1)
    Keys.onRightPressed: moveBy(1)

    contentItem: ListView {
        model: control.contentModel
        currentIndex: control.currentIndex
        spacing: control.spacing
        orientation: ListView.Horizontal
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.AutoFlickIfNeeded
        snapMode: ListView.SnapToItem
        highlightMoveDuration: 0
        keyNavigationEnabled: false
        interactive: contentWidth > width
    }

    background: Rectangle {
        implicitHeight: Theme.controlHeight
        radius: Math.min(Theme.radiusControl, height / 2)
        color: Theme.controlBg
        border.width: Theme.borderWidth
        border.color: Theme.panelBorder
    }
}
