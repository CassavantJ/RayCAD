// Popup menu (context menus, the view options menu). Items: MenuItem, MenuSeparator, nested Menu.
// Create menus lazily (inside a Loader) unless they are on the first screen.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: Theme.spacingS
    padding: Theme.spacingXs
    overlap: Theme.spacingXs
    font: Theme.uiFont

    delegate: MenuItem {}

    contentItem: ListView {
        implicitHeight: contentHeight
        model: control.contentModel
        interactive: Window.window ? contentHeight + control.topPadding + control.bottomPadding > control.height : false
        clip: true
        currentIndex: control.currentIndex
        boundsBehavior: Flickable.StopAtBounds
        T.ScrollBar.vertical: ScrollBar {}
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight * 7
        implicitHeight: Theme.rowHeight
        radius: Theme.radiusSmall + Theme.spacingXs
        color: Theme.panelBg
        border.width: Theme.borderWidth
        border.color: Theme.panelBorder
    }
}
