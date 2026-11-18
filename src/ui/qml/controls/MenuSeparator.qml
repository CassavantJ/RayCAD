import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.MenuSeparator {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    topPadding: Theme.spacingXs
    bottomPadding: Theme.spacingXs
    leftPadding: Theme.spacingS
    rightPadding: Theme.spacingS

    contentItem: Rectangle {
        implicitWidth: Theme.controlHeight * 6
        implicitHeight: Theme.borderWidth
        color: Theme.panelBorder
    }
}
