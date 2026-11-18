// Crash recovery (task T-019, PLAN.md 4.2): documents that had unsaved changes when RayCAD last stopped without
// saving or closing them. Shown at launch, and when a file that has such changes is opened. Each can be recovered
// (reopened with its changes, Files.restoreRecovery) or discarded (Session.discardRecovery); "Decide later" keeps
// them for the next launch. The list is Session.recoveries (IApp::pendingRecoveries).
import QtQuick
import RayCad.Ui

DialogBase {
    id: dialog
    objectName: "recoveryDialog"

    title: Session.recoveries.length === 1 ? qsTr("Recover unsaved changes?")
                                           : qsTr("Recover unsaved changes to %1 documents?").arg(Session.recoveries.length)
    text: qsTr("RayCAD stopped before these documents were saved or closed. Their changes can be recovered.")
    dialogWidth: Theme.controlHeight * 21
    onRejected: Session.postponeRecovery()

    Repeater {
        model: Session.recoveries
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            objectName: "recovery." + index
            width: parent ? parent.width : 0
            height: Math.max(rowText.implicitHeight, rowButtons.implicitHeight) + 2 * Theme.spacingS

            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusSmall
                color: Theme.controlBg
                border.width: Theme.borderWidth
                border.color: Theme.panelBorder
            }
            Column {
                id: rowText
                anchors.left: parent.left
                anchors.right: rowButtons.left
                anchors.leftMargin: Theme.spacingM
                anchors.rightMargin: Theme.spacingS
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingXs / 2

                Label {
                    width: parent.width
                    font: Theme.strongFont
                    text: row.modelData.title
                }
                Label {
                    width: parent.width
                    secondary: true
                    elide: Text.ElideMiddle
                    text: row.modelData.folder
                }
                Label {
                    width: parent.width
                    secondary: true
                    font: Theme.smallFont
                    text: row.modelData.time !== "" ? qsTr("%1, last change %2").arg(row.modelData.changes).arg(row.modelData.time)
                                                  : row.modelData.changes
                }
                Label {
                    objectName: "recoveryDamaged." + row.index
                    visible: row.modelData.damaged
                    width: parent.width
                    wrapMode: Text.Wrap
                    font: Theme.smallFont
                    color: Theme.error
                    text: qsTr("The last change was damaged; the changes before it can be recovered.")
                }
            }
            Row {
                id: rowButtons
                anchors.right: parent.right
                anchors.rightMargin: Theme.spacingS
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingS

                Button {
                    objectName: "recoveryDiscard." + row.index
                    text: qsTr("Discard")
                    toolTipText: qsTr("Throw these changes away")
                    onClicked: Session.discardRecovery(row.modelData.id)
                }
                Button {
                    objectName: "recoveryRestore." + row.index
                    primary: true
                    text: qsTr("Recover")
                    toolTipText: qsTr("Open the document with these changes")
                    onClicked: Files.restoreRecovery(row.modelData.id)
                }
            }
        }
    }

    buttons: [
        Button {
            objectName: "recoveryLater"
            text: qsTr("Decide later")
            toolTipText: qsTr("Keep the changes and ask again at the next start")
            onClicked: Session.postponeRecovery()
        }
    ]
}
