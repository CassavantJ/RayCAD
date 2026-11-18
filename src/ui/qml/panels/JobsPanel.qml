// Every running job (task T-019): opens, saves, imports, exports and long-running actions from IApp::activeJobs,
// the document's rebuild and the display meshing (Session.jobs). Each row has its title, the current stage, a
// progress bar and a cancel button. The status bar's job slot shows the foreground one; this panel opens from it.
import QtQuick
import RayCad.Ui

PanelBase {
    id: panel
    objectName: "jobsPanel"

    signal closeRequested()

    implicitWidth: Theme.controlHeight * 12
    implicitHeight: header.height + list.contentHeight + Theme.spacingS

    PanelHeader {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        title: Session.jobCount === 1 ? qsTr("1 job running") : qsTr("%1 jobs running").arg(Session.jobCount)
        iconName: "sliders"

        ToolButton {
            objectName: "jobsPanelClose"
            compact: true
            iconName: "close"
            toolTipText: qsTr("Hide the jobs")
            onClicked: panel.closeRequested()
        }
    }

    ListView {
        id: list
        objectName: "jobsList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.spacingXs
        clip: true
        interactive: contentHeight > height
        model: Session.jobs

        delegate: Item {
            id: row
            required property int index
            required property var jobId
            required property string jobTitle
            required property string jobStage
            required property real jobProgress
            objectName: "job." + index
            width: ListView.view.width
            height: rowColumn.implicitHeight + 2 * Theme.spacingXs

            Column {
                id: rowColumn
                anchors.left: parent.left
                anchors.right: cancel.left
                anchors.leftMargin: Theme.spacingM
                anchors.rightMargin: Theme.spacingS
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingXs

                Label {
                    objectName: "jobRowTitle"
                    width: parent.width
                    text: row.jobTitle
                }
                Label {
                    objectName: "jobRowStage"
                    width: parent.width
                    visible: text !== ""
                    secondary: true
                    font: Theme.smallFont
                    text: row.jobProgress >= 0 ? qsTr("%1 (%2 %)").arg(row.jobStage !== "" ? row.jobStage : qsTr("Working"))
                                                                  .arg(Math.round(row.jobProgress * 100))
                                               : row.jobStage
                }
                ProgressBar {
                    width: parent.width
                    indeterminate: row.jobProgress < 0
                    value: Math.max(0, row.jobProgress)
                    Accessible.name: row.jobTitle
                }
            }
            ToolButton {
                id: cancel
                objectName: "jobRowCancel." + row.index
                anchors.right: parent.right
                anchors.rightMargin: Theme.spacingXs
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "close"
                toolTipText: qsTr("Cancel %1").arg(row.jobTitle)
                onClicked: Session.cancelJob(row.jobId)
            }
        }
    }
}
