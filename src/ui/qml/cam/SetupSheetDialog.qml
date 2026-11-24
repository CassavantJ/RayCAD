// The setup sheet dialog of the CAM workspace (task T-076; CamSheet, cam/setupsheet_tool.h): the sheet the operator gets
// with the program, previewed as it prints, saved as a PDF or an HTML file. A modal card over the view: the header; on
// the left the choices (which setup or every setup, the paper, the picture, the folder and the file name), what blocks
// the sheet and what went wrong; on the right the pages; a fixed footer with the status line and the buttons.
// Esc cancels a save, or closes; Enter saves the PDF from a field.
import QtQuick
import QtQuick.Dialogs
import RayCad.Ui

FocusScope {
    id: root
    objectName: "sheetDialogHost"
    visible: CamSheet.active
    anchors.fill: parent

    // The ViewportItem the pictures are taken in.
    property var view: null
    onViewChanged: CamSheet.view = view
    Component.onCompleted: CamSheet.view = view

    readonly property real labelWidth: Theme.controlHeight * 3.6
    readonly property real fieldWidth: Theme.controlHeight * 8
    readonly property real sideWidth: labelWidth + fieldWidth + Theme.spacingS
    readonly property var values: CamSheet.values
    readonly property string state: CamSheet.state

    onVisibleChanged: if (visible) Qt.callLater(() => choiceCombo.forceActiveFocus(Qt.PopupFocusReason))
    // The combo boxes follow the model (a choice made elsewhere, the list of setups changing).
    Connections {
        target: CamSheet
        function onChoiceChanged() {
            Qt.callLater(() => choiceCombo.currentIndex = choiceCombo.indexOfValue(CamSheet.choice))
        }
        function onPaperChanged() {
            paperCombo.currentIndex = paperCombo.indexOfValue(CamSheet.paper)
        }
    }

    function holds(item) {
        for (let it = item; it; it = it.parent) {
            if (it === card)
                return true
        }
        return false
    }
    function moveFocus(forward) {
        let item = Window.activeFocusItem
        for (let i = 0; i < 500; ++i) {
            item = item ? item.nextItemInFocusChain(forward) : card.nextItemInFocusChain(forward)
            if (!item)
                return
            if (root.holds(item) && item.visible && item.enabled) {
                item.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                return
            }
        }
    }
    Keys.onTabPressed: root.moveFocus(true)
    Keys.onBacktabPressed: root.moveFocus(false)
    Keys.onEscapePressed: CamSheet.cancel()
    Shortcut {
        sequence: "Esc"
        enabled: CamSheet.active
        onActivated: CamSheet.cancel()
    }

    component Heading: Label {
        width: root.sideWidth
        font: Theme.strongFont
        topPadding: Theme.spacingS
    }
    component FieldLabel: Label {
        width: root.labelWidth
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        secondary: true
        wrapMode: Text.Wrap
        elide: Text.ElideNone
    }

    // ---------- the scrim ----------
    Rectangle {
        objectName: "sheetDialogScrim"
        anchors.fill: parent
        color: Theme.windowBg
        opacity: 0.6
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: wheel => wheel.accepted = true
    }

    PanelBase {
        id: card
        objectName: "sheetDialog"
        anchors.centerIn: parent
        width: Math.min(root.width - 2 * Theme.spacingL, root.sideWidth + Theme.controlHeight * 22)
        height: root.height - 2 * Theme.spacingL
        Accessible.role: Accessible.Dialog
        Accessible.name: CamSheet.title

        // ---- header ----
        Column {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spacingL
            anchors.bottomMargin: 0
            spacing: Theme.spacingXs
            Label {
                objectName: "sheetDialogTitle"
                width: parent.width
                font: Theme.titleFont
                wrapMode: Text.Wrap
                text: CamSheet.title
            }
            Label {
                width: parent.width
                secondary: true
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                text: qsTr("What the operator needs at the machine: work offset, stock, tools, operations and the "
                           + "estimated time, printed black on white.")
            }
        }

        // ---- the choices (left, scrolls) ----
        Flickable {
            id: side
            objectName: "sheetDialogSide"
            anchors.left: parent.left
            anchors.top: header.bottom
            anchors.bottom: footer.top
            anchors.leftMargin: Theme.spacingL
            anchors.topMargin: Theme.spacingM
            anchors.bottomMargin: Theme.spacingS
            width: root.sideWidth + Theme.spacingM
            clip: true
            contentHeight: sideColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            ScrollBar.vertical: ScrollBar {
                policy: side.interactive ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            }

            Column {
                id: sideColumn
                width: root.sideWidth
                spacing: Theme.spacingS

                // What blocks the sheet, each operation with its reason.
                Column {
                    objectName: "sheetBlocked"
                    visible: CamSheet.blockers.length > 0
                    width: root.sideWidth
                    spacing: Theme.spacingXs
                    Row {
                        spacing: Theme.spacingS
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: "warning"
                            color: Theme.error
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            font: Theme.strongFont
                            color: Theme.error
                            text: qsTr("The sheet is blocked")
                        }
                    }
                    Repeater {
                        model: CamSheet.blockers
                        delegate: Column {
                            id: blockerRow
                            required property var modelData
                            required property int index
                            objectName: "sheetBlocker." + index
                            width: root.sideWidth
                            Label {
                                width: parent.width
                                font: Theme.strongFont
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                text: qsTr("%1: %2").arg(blockerRow.modelData.name).arg(blockerRow.modelData.reasonText)
                            }
                            Label {
                                width: parent.width
                                secondary: true
                                font: Theme.smallFont
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                text: blockerRow.modelData.message
                            }
                        }
                    }
                }

                // Errors and notes.
                Repeater {
                    model: CamSheet.messages
                    delegate: Row {
                        id: messageRow
                        required property var modelData
                        required property int index
                        objectName: "sheetMessage." + index
                        spacing: Theme.spacingS
                        Icon {
                            name: messageRow.modelData.kind === "note" ? "info" : "warning"
                            color: messageRow.modelData.kind === "error" ? Theme.error : Theme.textSecondary
                        }
                        Label {
                            objectName: "sheetMessageText." + messageRow.index
                            width: root.sideWidth - Theme.iconSize - Theme.spacingS
                            wrapMode: Text.Wrap
                            elide: Text.ElideNone
                            font: Theme.smallFont
                            color: messageRow.modelData.kind === "error" ? Theme.error : Theme.textPrimary
                            text: messageRow.modelData.text
                        }
                    }
                }

                Heading {
                    text: qsTr("Sheet")
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("Setup")
                    }
                    ComboBox {
                        id: choiceCombo
                        objectName: "sheetChoice"
                        width: root.fieldWidth
                        enabled: !CamSheet.busy
                        model: CamSheet.choices
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: indexOfValue(CamSheet.choice)
                        Accessible.name: qsTr("Setup")
                        KeyNavigation.backtab: closeButton
                        onActivated: CamSheet.setChoice(currentValue)
                    }
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("Paper")
                    }
                    ComboBox {
                        id: paperCombo
                        objectName: "sheetPaper"
                        width: root.fieldWidth
                        enabled: !CamSheet.busy
                        model: [{ value: "letter", text: qsTr("Letter (8.5 × 11 in)") },
                                { value: "a4", text: qsTr("A4 (210 × 297 mm)") }]
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: indexOfValue(CamSheet.paper)
                        Accessible.name: qsTr("Paper")
                        onActivated: CamSheet.setPaper(currentValue)
                    }
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("Picture")
                    }
                    Button {
                        objectName: "sheetRetake"
                        enabled: !CamSheet.busy && root.state !== "picturing" && CamSheet.blockers.length === 0
                        text: qsTr("Take again")
                        toolTipText: qsTr("Take the picture of the part on the machine again (the view turns for a moment)")
                        onClicked: CamSheet.retakePictures()
                    }
                }
                Label {
                    x: root.labelWidth + Theme.spacingS
                    width: root.fieldWidth
                    secondary: true
                    font: Theme.smallFont
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    text: qsTr("The part as it sits on the machine, from the front right, with the stock outlined and "
                               + "the work offset's axes.")
                }

                Heading {
                    text: qsTr("Save")
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("Folder")
                    }
                    TextField {
                        id: folderField
                        objectName: "sheetInput.directory"
                        readonly property string full: root.values.directory !== undefined ? root.values.directory : ""
                        width: root.fieldWidth - browse.width - Theme.spacingS
                        enabled: !CamSheet.busy
                        text: activeFocus ? full : folderMetrics.elidedText
                        TextMetrics {
                            id: folderMetrics
                            font: folderField.font
                            text: CamSheet.directoryShown
                            elide: Qt.ElideLeft
                            elideWidth: Math.max(0, folderField.width - folderField.leftPadding - folderField.rightPadding - 2)
                        }
                        HoverToolTip {
                            target: folderField
                            text: folderField.full
                        }
                        placeholderText: qsTr("A local folder or a network share")
                        Accessible.name: qsTr("Folder for the setup sheet")
                        onTextEdited: CamSheet.setValue("directory", text)
                        Keys.onReturnPressed: CamSheet.savePdf()
                        Keys.onEnterPressed: CamSheet.savePdf()
                        Keys.onEscapePressed: CamSheet.cancel()
                    }
                    Button {
                        id: browse
                        objectName: "sheetBrowse"
                        enabled: !CamSheet.busy
                        text: qsTr("Browse…")
                        toolTipText: qsTr("Choose the folder")
                        onClicked: {
                            if (!Files.useNativeDialogs)
                                return
                            folderLoader.active = true
                            folderLoader.item.open()
                        }
                    }
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("File name")
                    }
                    TextField {
                        objectName: "sheetInput.stem"
                        width: root.fieldWidth
                        enabled: !CamSheet.busy
                        text: root.values.stem !== undefined ? root.values.stem : ""
                        Accessible.name: qsTr("File name, without .pdf or .html")
                        onTextEdited: CamSheet.setValue("stem", text)
                        Keys.onReturnPressed: CamSheet.savePdf()
                        Keys.onEnterPressed: CamSheet.savePdf()
                        Keys.onEscapePressed: CamSheet.cancel()
                    }
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("Writes")
                    }
                    Label {
                        objectName: "sheetFileNames"
                        width: root.fieldWidth
                        anchors.verticalCenter: parent.verticalCenter
                        font: Theme.strongFont
                        wrapMode: Text.WrapAnywhere
                        elide: Text.ElideNone
                        text: CamSheet.pdfName !== "" ? qsTr("%1 or %2").arg(CamSheet.pdfName).arg(CamSheet.htmlName) : "—"
                    }
                }
                Label {
                    objectName: "sheetNameNote"
                    x: root.labelWidth + Theme.spacingS
                    width: root.fieldWidth
                    visible: text !== ""
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    font: Theme.smallFont
                    secondary: true
                    text: CamSheet.nameNote
                }
            }
        }

        // ---- the pages (right, scrolls) ----
        Rectangle {
            id: previewFrame
            objectName: "sheetPreviewFrame"
            anchors.left: side.right
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.bottom: footer.top
            anchors.leftMargin: Theme.spacingM
            anchors.rightMargin: Theme.spacingL
            anchors.topMargin: Theme.spacingM
            anchors.bottomMargin: Theme.spacingS
            radius: Theme.radiusSmall
            color: Theme.controlBg
            border.width: Theme.borderWidth
            border.color: Theme.panelBorder
            clip: true

            Flickable {
                id: preview
                objectName: "sheetPreview"
                anchors.fill: parent
                anchors.margins: Theme.spacingS
                contentHeight: pagesColumn.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height
                ScrollBar.vertical: ScrollBar {
                    policy: preview.interactive ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
                }
                Column {
                    id: pagesColumn
                    width: preview.width - Theme.spacingM
                    spacing: Theme.spacingM
                    Repeater {
                        model: CamSheet.pageCount
                        delegate: Image {
                            required property int index
                            objectName: "sheetPage." + index
                            width: pagesColumn.width
                            height: width * CamSheet.pageAspect
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                            mipmap: true
                            cache: false
                            asynchronous: false
                            source: "image://setupsheet/" + CamSheet.revision + "/" + index
                            sourceSize.width: Math.round(width * Screen.devicePixelRatio)
                            Accessible.role: Accessible.Graphic
                            Accessible.name: qsTr("Page %1 of %2").arg(index + 1).arg(CamSheet.pageCount)
                        }
                    }
                }
            }
            // While there is nothing to show: what is happening, or why not.
            Label {
                objectName: "sheetPreviewNote"
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.spacingL
                visible: CamSheet.pageCount === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                secondary: true
                text: root.state === "blocked" ? qsTr("No preview: the sheet is blocked")
                    : root.state === "failed" ? qsTr("No preview")
                    : qsTr("Making the preview…")
            }
        }

        // ---- footer (fixed) ----
        Column {
            id: footer
            objectName: "sheetDialogFooter"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.spacingL
            anchors.topMargin: 0
            spacing: Theme.spacingS

            Rectangle {
                width: parent.width
                height: Theme.borderWidth
                color: Theme.panelBorder
            }
            ProgressBar {
                objectName: "sheetProgress"
                width: parent.width
                visible: root.state === "checking" || root.state === "picturing" || root.state === "making"
                         || root.state === "saving"
                indeterminate: true
            }
            Label {
                objectName: "sheetStatus"
                width: parent.width
                visible: text !== ""
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                maximumLineCount: 3
                font: Theme.smallFont
                color: root.state === "failed" || root.state === "blocked" ? Theme.error
                     : root.state === "done" ? Theme.success
                     : root.state === "asking" ? Theme.warning : Theme.textSecondary
                text: CamSheet.statusText
            }
            Label {
                id: pathLabel
                objectName: "sheetResultPath"
                width: parent.width
                visible: root.state === "done" && CamSheet.resultPath !== ""
                secondary: true
                font: Theme.smallFont
                elide: Text.ElideMiddle
                text: CamSheet.resultPath
                readonly property bool hovered: pathHover.hovered
                readonly property bool pressed: false
                HoverHandler {
                    id: pathHover
                }
                HoverToolTip {
                    target: pathLabel
                    text: CamSheet.resultPath
                }
            }
            Row {
                objectName: "sheetButtons"
                anchors.right: parent.right
                spacing: Theme.spacingS
                layoutDirection: Qt.RightToLeft

                Button {
                    objectName: "sheetSavePdf"
                    visible: root.state !== "asking"
                    primary: true
                    text: qsTr("Save PDF")
                    enabled: CamSheet.canSave
                    toolTipText: CamSheet.canSave ? qsTr("Save the sheet as %1 (Enter)").arg(CamSheet.pdfName)
                                                  : CamSheet.saveBlockedReason
                    onClicked: CamSheet.savePdf()
                }
                Button {
                    objectName: "sheetSaveHtml"
                    visible: root.state !== "asking"
                    text: qsTr("Save HTML")
                    enabled: CamSheet.canSave
                    toolTipText: CamSheet.canSave ? qsTr("Save the sheet as %1: one file that opens in any web browser")
                                                    .arg(CamSheet.htmlName)
                                                  : CamSheet.saveBlockedReason
                    onClicked: CamSheet.saveHtml()
                }
                // The file exists.
                Button {
                    objectName: "sheetReplace"
                    visible: root.state === "asking"
                    primary: true
                    text: qsTr("Replace")
                    toolTipText: qsTr("Replace %1 with the new sheet").arg(CamSheet.existingName)
                    onClicked: CamSheet.answerOverwrite("replace")
                }
                Button {
                    objectName: "sheetNewName"
                    visible: root.state === "asking"
                    text: qsTr("Keep both")
                    toolTipText: qsTr("Save the new sheet under the next free numbered name")
                    onClicked: CamSheet.answerOverwrite("newName")
                }
                Button {
                    id: closeButton
                    objectName: "sheetCancel"
                    KeyNavigation.tab: generateButton.visible ? generateButton : choiceCombo
                    text: CamSheet.busy || root.state === "asking" ? qsTr("Cancel") : qsTr("Close")
                    toolTipText: CamSheet.busy ? qsTr("Stop saving; nothing is written (Esc)") : qsTr("Close (Esc)")
                    onClicked: CamSheet.cancel()
                }
                Button {
                    id: generateButton
                    objectName: "sheetGenerateAll"
                    KeyNavigation.tab: choiceCombo
                    visible: CamSheet.blockers.some(b => b.canGenerate)
                    text: qsTr("Generate all")
                    toolTipText: qsTr("Generate every toolpath that is stale or missing")
                    onClicked: CamSheet.generateAll()
                }
            }
        }
    }

    Loader {
        id: folderLoader
        active: false
        sourceComponent: FolderDialog {
            objectName: "sheetFolderDialog"
            title: qsTr("Folder for the setup sheet")
            onAccepted: CamSheet.setFolder(selectedFolder)
        }
    }
}
