// Thin progress bar for the job slot. `indeterminate` animates only while visible, so an idle shell draws no frames.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.ProgressBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    Accessible.role: Accessible.ProgressBar

    contentItem: Item {
        implicitWidth: Theme.controlHeight * 4
        implicitHeight: Theme.spacingXs
        clip: control.indeterminate

        Rectangle {
            id: bar
            height: parent.height
            radius: height / 2
            color: Theme.accentThinLine
            width: control.indeterminate ? parent.width / 3 : control.position * parent.width

            // Back at the start when a job's progress becomes known (the animation leaves x wherever it stopped).
            Binding on x {
                when: !control.indeterminate
                value: 0
            }
            NumberAnimation on x {
                running: control.indeterminate && control.visible
                from: -bar.width
                to: bar.parent.width
                duration: Theme.durationFast * 10
                loops: Animation.Infinite
            }
        }
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight * 4
        implicitHeight: Theme.spacingXs
        y: (control.height - height) / 2
        height: Theme.spacingXs
        radius: height / 2
        color: Theme.controlBgPressed
    }
}
