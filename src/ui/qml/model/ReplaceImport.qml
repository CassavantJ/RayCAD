// The replace dialog (task T-084; the Replacement singleton, model/replace_import.h): pick the new revision of an
// imported part's file, replace, then read what changed. Unchanged faces keep their references; changed faces are
// tinted amber, new ones green, and where a removed face was is a dashed box. Click a row to see the face. What needs
// review is handed to the reference review panel; Undo puts the old revision back. Not modal: the view works under it.
import QtQuick
import QtQuick.Dialogs
import RayCad.Ui

PanelBase {
    id: root
    objectName: "replaceImport"

    readonly property real contentWidth: Theme.controlHeight * 11
    implicitWidth: contentWidth + 2 * Theme.spacingM
    implicitHeight: header.implicitHeight + Theme.spacingS + flick.contentHeight + Theme.spacingS
                    + footer.implicitHeight + 2 * Theme.spacingM

    function fateColor(fate) {
        return fate === "new" ? Theme.success : fate === "removed" ? Theme.error : Theme.warning
    }
    function fateLabel(fate) {
        return fate === "new" ? qsTr("New") : fate === "removed" ? qsTr("Removed") : qsTr("Changed")
    }
    function faceTitle(row) {
        if (row.surface === "plane")
            return qsTr("Flat face")
        if (row.surface === "cylinder")
            return row.hole ? qsTr("Hole, %1 diameter").arg(Units.formatLength(row.diameter))
                            : qsTr("Round face, %1 diameter").arg(Units.formatLength(row.diameter))
        if (row.surface === "cone")
            return qsTr("Cone face")
        if (row.surface === "sphere")
            return qsTr("Ball face")
        if (row.surface === "torus")
            return qsTr("Rounded face")
        return qsTr("Freeform face")
    }

    // The new revision's file.
    Loader {
        id: dialogLoader
        active: false
        sourceComponent: Component {
            FileDialog {
                objectName: "replaceImportFileDialog"
                title: qsTr("New revision of %1").arg(Replacement.currentFile)
                nameFilters: Files.importFilters
                fileMode: FileDialog.OpenFile
                onAccepted: Replacement.path = selectedFile
            }
        }
    }
    Connections {
        target: Replacement
        function onFileDialogRequested(folder) {
            dialogLoader.active = true
            if (folder.toString() !== "")
                dialogLoader.item.currentFolder = folder
            dialogLoader.item.open()
        }
    }

    Item {
        anchors.fill: parent
        anchors.margins: Theme.spacingM

        Row {
            id: header
            anchors.top: parent.top
            width: root.contentWidth
            spacing: Theme.spacingS
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "refresh"
                color: Theme.textSecondary
            }
            Label {
                objectName: "replaceImportTitle"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * Theme.spacingS - Theme.iconSize - closeButton.width
                font: Theme.strongFont
                text: Replacement.featureName !== "" ? qsTr("Replace %1 with a new revision").arg(Replacement.featureName)
                                                       : qsTr("Replace with a new revision")
            }
            ToolButton {
                id: closeButton
                objectName: "replaceImportClose"
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "close"
                toolTipText: qsTr("Close")
                onClicked: Replacement.close()
            }
        }

        Flickable {
            id: flick
            objectName: "replaceImportList"
            anchors.top: header.bottom
            anchors.topMargin: Theme.spacingS
            anchors.bottom: footer.top
            anchors.bottomMargin: Theme.spacingS
            width: parent.width
            contentHeight: column.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            clip: true
            ScrollBar.vertical: ScrollBar {}

            Column {
                id: column
                width: root.contentWidth
                spacing: Theme.spacingS

                // Which import (only when there are several and none was selected).
                ComboBox {
                    objectName: "replaceImportFeature"
                    visible: Replacement.imports.length > 1 && !Replacement.hasResult
                    width: parent.width
                    model: Replacement.imports
                    textRole: "name"
                    currentIndex: {
                        for (let i = 0; i < Replacement.imports.length; ++i)
                            if (Replacement.imports[i].id === Replacement.feature)
                                return i
                        return -1
                    }
                    onActivated: (i) => Replacement.feature = Replacement.imports[i].id
                }
                Label {
                    objectName: "replaceImportCurrent"
                    visible: Replacement.feature !== "" && !Replacement.hasResult
                    width: parent.width
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    secondary: true
                    text: qsTr("Now from %1").arg(Replacement.currentFile)
                }
                Label {
                    visible: Replacement.feature === "" && !Replacement.hasResult
                    width: parent.width
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    text: Replacement.imports.length === 0 ? qsTr("The document has no imported part to replace.")
                                                            : qsTr("Choose the imported part to replace.")
                }
                Row {
                    visible: !Replacement.hasResult
                    width: parent.width
                    spacing: Theme.spacingS
                    Button {
                        id: chooseButton
                        objectName: "replaceImportChoose"
                        enabled: Replacement.feature !== "" && !Replacement.busy
                        text: qsTr("Choose file…")
                        toolTipText: qsTr("The new revision: a STEP or IGES file")
                        onClicked: Replacement.chooseFile()
                    }
                    Label {
                        objectName: "replaceImportFile"
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - chooseButton.width - Theme.spacingS
                        secondary: Replacement.fileName === ""
                        text: Replacement.fileName !== "" ? Replacement.fileName : qsTr("No file chosen")
                    }
                }
                Label {
                    visible: !Replacement.hasResult
                    width: parent.width
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    secondary: true
                    font: Theme.smallFont
                    text: qsTr("Faces that did not change keep everything that refers to them. Changed, new and removed faces are listed for you to check. One step to undo.")
                }

                // The summary.
                Label {
                    objectName: "replaceImportSummary"
                    visible: Replacement.hasResult
                    width: parent.width
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    font: Theme.strongFont
                    text: Replacement.summary
                }
                Repeater {
                    model: Replacement.hasResult ? Replacement.notes : []
                    delegate: Label {
                        required property string modelData
                        width: column.width
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        secondary: true
                        font: Theme.smallFont
                        text: modelData
                    }
                }
                Repeater {
                    model: Replacement.rows
                    delegate: Item {
                        id: row
                        required property int index
                        required property var modelData
                        readonly property bool current: Replacement.current === index
                        objectName: "replaceImportRow." + index
                        width: column.width
                        height: rowBody.implicitHeight + 2 * Theme.spacingXs
                        Accessible.role: Accessible.ListItem
                        Accessible.name: root.fateLabel(modelData.fate) + ": " + root.faceTitle(modelData)
                        Accessible.selected: current

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusSmall
                            color: row.current ? Theme.selectionFill : rowHover.hovered ? Theme.controlBgHover : "transparent"
                        }
                        HoverHandler {
                            id: rowHover
                        }
                        TapHandler {
                            onTapped: Replacement.current = row.current ? -1 : row.index
                        }
                        Row {
                            id: rowBody
                            x: Theme.spacingS
                            y: Theme.spacingXs
                            width: parent.width - 2 * Theme.spacingS
                            spacing: Theme.spacingS
                            Rectangle {
                                width: Theme.spacingS
                                height: width
                                radius: width / 2
                                anchors.verticalCenter: parent.verticalCenter
                                color: root.fateColor(row.modelData.fate)
                                Accessible.ignored: true
                            }
                            Label {
                                objectName: "replaceImportFate." + row.index
                                anchors.verticalCenter: parent.verticalCenter
                                font: Theme.smallFont
                                color: root.fateColor(row.modelData.fate)
                                text: root.fateLabel(row.modelData.fate)
                            }
                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - x
                                text: root.faceTitle(row.modelData)
                                      + (row.modelData.located ? "" : "  " + qsTr("(not in the model now)"))
                            }
                        }
                    }
                }
            }
        }

        Column {
            id: footer
            anchors.bottom: parent.bottom
            width: root.contentWidth
            spacing: Theme.spacingS

            Label {
                objectName: "replaceImportMessage"
                visible: Replacement.message !== ""
                width: parent.width
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                font: Theme.smallFont
                color: Replacement.messageIsError ? Theme.error : Theme.textSecondary
                text: Replacement.message
            }
            Row {
                spacing: Theme.spacingS
                Button {
                    objectName: "replaceImportReplace"
                    visible: !Replacement.hasResult
                    primary: true
                    enabled: Replacement.canReplace
                    text: Replacement.busy ? qsTr("Replacing…") : qsTr("Replace")
                    toolTipText: qsTr("Read the new file and keep the references of every face that did not change")
                    onClicked: Replacement.replace()
                }
                Button {
                    objectName: "replaceImportReview"
                    visible: Replacement.hasResult
                    primary: References.count > 0
                    text: References.count > 0 ? qsTr("Review references (%1)").arg(References.count)
                                               : qsTr("Review references")
                    toolTipText: qsTr("Open the reference review: what features and toolpaths need checking")
                    onClicked: Replacement.review()
                }
                Button {
                    objectName: "replaceImportUndo"
                    visible: Replacement.hasResult
                    text: qsTr("Undo replace")
                    toolTipText: qsTr("Put the previous revision back")
                    onClicked: Replacement.undo()
                }
                Button {
                    objectName: "replaceImportDone"
                    text: Replacement.hasResult ? qsTr("Done") : qsTr("Cancel")
                    onClicked: Replacement.close()
                }
            }
        }
    }

    // A replace that left references to review opens the review beside this summary, once the review has listed the
    // replaced model.
    QtObject {
        id: handover
        property bool armed: false
        function check() {
            if (!armed || !Replacement.built || References.busy)
                return
            armed = false
            if (References.count > 0)
                References.show()
        }
    }
    Connections {
        target: Replacement
        function onReplaced() { handover.armed = true; handover.check() }
        function onBuiltChanged() { handover.check() }
        function onShownChanged() { if (!Replacement.shown) handover.armed = false }
    }
    Connections {
        target: References
        function onListChanged() { handover.check() }
        function onBusyChanged() { handover.check() }
    }
}
