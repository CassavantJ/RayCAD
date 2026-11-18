// Shown over the viewport while the document is read-only (task T-019, PLAN.md 4.3): someone else holds the
// file's write lock. Says who and since when, and offers Save As (the only way to keep changes).
import QtQuick
import RayCad.Ui

PanelBase {
    id: notice
    objectName: "readOnlyNotice"

    implicitWidth: row.implicitWidth + 2 * Theme.spacingS
    implicitHeight: Math.max(row.implicitHeight, Theme.controlHeight) + 2 * Theme.spacingXs

    Rectangle {
        width: Theme.focusRingWidth
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.spacingXs
        radius: width / 2
        color: Theme.warning
    }
    Row {
        id: row
        anchors.left: parent.left
        anchors.leftMargin: Theme.spacingM
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spacingS

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: "info"
            color: Theme.textSecondary
        }
        Label {
            id: holder
            objectName: "readOnlyHolder"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, notice.parent ? notice.parent.width - 2 * Theme.spacingL
                                                           - saveAs.width - Theme.controlHeight * 2 : implicitWidth)
            wrapMode: Text.Wrap
            text: {
                const who = Session.lockHolder !== "" ? Session.lockHolder : qsTr("another session")
                let s = Session.lockSince !== "" ? qsTr("Read-only: %1 has had this file open since %2.").arg(who).arg(Session.lockSince)
                                                 : qsTr("Read-only: %1 has this file open.").arg(who)
                if (Session.lockStale)
                    s += " " + qsTr("The lock looks stale.")
                return s
            }
        }
        Button {
            id: saveAs
            objectName: "readOnlySaveAs"
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Save as…")
            toolTipText: qsTr("Keep your changes in a copy of the file")
            onClicked: Files.requestSaveAs()
        }
    }
}
