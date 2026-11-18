// Thin overlay scroll bar: visible while scrolling or hovered, hidden when everything fits.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.ScrollBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: Theme.spacingXs / 2
    visible: policy !== T.ScrollBar.AlwaysOff && size < 1.0
    minimumSize: orientation === Qt.Horizontal ? height / width : width / height
    hoverEnabled: true

    contentItem: Rectangle {
        readonly property real thickness: control.hovered || control.pressed ? Theme.spacingS : Theme.spacingXs * 1.5
        implicitWidth: control.interactive ? thickness : Theme.borderWidth * 2
        implicitHeight: control.interactive ? thickness : Theme.borderWidth * 2
        radius: Math.min(width, height) / 2
        color: control.pressed || control.hovered ? Theme.scrollThumbHover : Theme.scrollThumb
        opacity: control.policy === T.ScrollBar.AlwaysOn || control.active || control.hovered ? 1 : 0

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.durationFast
            }
        }
    }
}
