// The post dialog of the CAM workspace (task T-047; CamPost, cam/post_tool.h): posts a setup and writes the program.
// A modal card over the view: the header, a scrolling body (the notice about unverified posts and machines, what
// blocks posting with Generate where that is the fix, the post and its properties, the program, the output folder,
// the file name and the overwrite rule, then what the post said), and a fixed footer with the status line, the progress
// and the buttons, which never scroll away. Esc cancels a run, or closes; Enter posts from a field.
import QtQuick
import QtQuick.Dialogs
import RayCad.Ui

FocusScope {
    id: root
    objectName: "postDialogHost"
    visible: CamPost.active
    anchors.fill: parent

    readonly property real labelWidth: Theme.controlHeight * 4.2
    readonly property real fieldWidth: Theme.controlHeight * 9
    readonly property real contentWidth: labelWidth + fieldWidth + Theme.controlHeightSmall + 2 * Theme.spacingS
    readonly property var values: CamPost.values
    readonly property string state: CamPost.state

    onVisibleChanged: if (visible) Qt.callLater(() => postCombo.forceActiveFocus(Qt.PopupFocusReason))

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

    // Tab and Shift+Tab follow the controls' own focus chain, which would run on past the card's ends into the shell:
    // the last button leads back to the first control, and the first control back to the last button
    // (KeyNavigation, below).
    readonly property Item lastButton: generateAllButton.visible ? generateAllButton : cancelButton
    Keys.onEscapePressed: CamPost.cancel()
    Shortcut {
        sequence: "Esc"
        enabled: CamPost.active
        onActivated: CamPost.cancel()
    }

    // ---------- rows ----------
    component Heading: Label {
        width: root.contentWidth
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
    component Field: Row {
        id: fieldRow
        property string key
        property string label
        property string placeholder
        property string hint
        objectName: "postField." + key
        spacing: Theme.spacingS
        FieldLabel {
            text: fieldRow.label
        }
        TextField {
            objectName: "postInput." + fieldRow.key
            width: root.fieldWidth
            enabled: !CamPost.busy
            text: root.values[fieldRow.key] !== undefined ? String(root.values[fieldRow.key]) : ""
            placeholderText: fieldRow.placeholder
            Accessible.name: fieldRow.label
            Accessible.description: fieldRow.hint
            onTextEdited: CamPost.setValue(fieldRow.key, text)
            Keys.onReturnPressed: CamPost.post()
            Keys.onEnterPressed: CamPost.post()
            Keys.onEscapePressed: CamPost.cancel()
        }
    }
    component Choice: Row {
        id: choiceRow
        property string key
        property string label
        property var choices: [] // [{value, text}]
        readonly property string current: root.values[key] !== undefined ? String(root.values[key]) : ""
        objectName: "postChoice." + key
        spacing: Theme.spacingS
        function sync() {
            const i = combo.indexOfValue(current)
            if (i !== combo.currentIndex)
                combo.currentIndex = i
        }
        onCurrentChanged: sync()
        FieldLabel {
            text: choiceRow.label
        }
        ComboBox {
            id: combo
            objectName: "postCombo." + choiceRow.key
            width: root.fieldWidth
            enabled: !CamPost.busy
            model: choiceRow.choices
            textRole: "text"
            valueRole: "value"
            Accessible.name: choiceRow.label
            Component.onCompleted: choiceRow.sync()
            onActivated: CamPost.setValue(choiceRow.key, currentValue)
        }
    }

    // ---------- the scrim ----------
    Rectangle {
        objectName: "postDialogScrim"
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
        objectName: "postDialog"
        anchors.centerIn: parent
        width: Math.min(root.width - 2 * Theme.spacingL, root.contentWidth + 2 * Theme.spacingL + Theme.spacingM)
        height: Math.min(root.height - 2 * Theme.spacingL,
                         header.height + body.implicitHeight + footer.height + 2 * Theme.spacingM)
        Accessible.role: Accessible.Dialog
        Accessible.name: CamPost.title

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
                objectName: "postDialogTitle"
                width: parent.width
                font: Theme.titleFont
                wrapMode: Text.Wrap
                text: CamPost.title
            }
            Label {
                objectName: "postDialogMachine"
                width: parent.width
                secondary: true
                wrapMode: Text.Wrap
                text: CamPost.machineName !== "" ? qsTr("Machine: %1").arg(CamPost.machineName)
                                                 : qsTr("The setup has no machine")
            }
            // T-059: the setup's cycle-time estimate, h:mm:ss, marked as an estimate, with what it assumed.
            Label {
                objectName: "postDialogCycleTime"
                visible: CamPost.cycleTime !== ""
                width: parent.width
                secondary: true
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                text: CamPost.cycleTime
            }
            // The post and the unit it writes, where they cannot be missed (the Units row below changes the unit).
            Label {
                objectName: "postDialogOutput"
                width: parent.width
                font: Theme.strongFont
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                text: qsTr("%1, writing %2").arg(CamPost.postId !== "" ? CamPost.postId : qsTr("no post"))
                                            .arg(root.values.units === "inch" ? qsTr("inches (G20)")
                                                                              : qsTr("millimetres (G21)"))
            }
        }

        // ---- body (scrolls) ----
        Flickable {
            id: flick
            objectName: "postDialogScroll"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.bottom: footer.top
            anchors.leftMargin: Theme.spacingL
            anchors.rightMargin: Theme.spacingM
            anchors.topMargin: Theme.spacingM
            anchors.bottomMargin: Theme.spacingS
            clip: true
            contentHeight: body.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            // Always shown while the body is taller than the card, so the sections below are not missed.
            ScrollBar.vertical: ScrollBar {
                objectName: "postDialogScrollBar"
                policy: flick.interactive ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            }
            // What the post said is at the top of the body: bring it into view when it arrives.
            Connections {
                target: CamPost
                function onStateChanged() {
                    if (CamPost.state === "failed" || CamPost.state === "done" || CamPost.state === "asking")
                        flick.contentY = 0
                }
            }

            Column {
                id: body
                width: root.contentWidth
                spacing: Theme.spacingS

                // The notice (PLAN.md risk 4): shown while anything is unverified; it cannot be dismissed for good.
                Rectangle {
                    objectName: "postNotice"
                    visible: CamPost.notice !== ""
                    width: root.contentWidth
                    height: noticeRow.implicitHeight + 2 * Theme.spacingS
                    radius: Theme.radiusSmall
                    color: "transparent"
                    border.width: Theme.borderWidth
                    border.color: Theme.warning
                    Row {
                        id: noticeRow
                        x: Theme.spacingS
                        y: Theme.spacingS
                        spacing: Theme.spacingS
                        Icon {
                            name: "warning"
                            color: Theme.warning
                        }
                        Label {
                            objectName: "postNoticeText"
                            width: root.contentWidth - 3 * Theme.spacingS - Theme.iconSize
                            wrapMode: Text.Wrap
                            elide: Text.ElideNone
                            text: CamPost.notice
                        }
                    }
                }

                // What blocks posting, each operation with its reason; Generate where that is the fix.
                Column {
                    objectName: "postBlocked"
                    visible: CamPost.blockers.length > 0
                    width: root.contentWidth
                    spacing: Theme.spacingXs
                    Row {
                        spacing: Theme.spacingS
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: "warning"
                            color: Theme.error
                        }
                        Label {
                            objectName: "postBlockedTitle"
                            anchors.verticalCenter: parent.verticalCenter
                            font: Theme.strongFont
                            color: Theme.error
                            text: qsTr("Posting is blocked")
                        }
                    }
                    Repeater {
                        model: CamPost.blockers
                        delegate: Rectangle {
                            id: blockerRow
                            required property var modelData
                            required property int index
                            objectName: "postBlocker." + index
                            width: root.contentWidth
                            height: Math.max(blockerText.implicitHeight, generateButton.visible ? generateButton.height : 0)
                                    + 2 * Theme.spacingS
                            radius: Theme.radiusSmall
                            color: Theme.controlBg
                            border.width: Theme.borderWidth
                            border.color: Theme.panelBorder
                            Column {
                                id: blockerText
                                anchors.left: parent.left
                                anchors.right: generateButton.visible ? generateButton.left : parent.right
                                anchors.margins: Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.spacingXs / 2
                                Label {
                                    objectName: "postBlockerName." + blockerRow.index
                                    width: parent.width
                                    font: Theme.strongFont
                                    wrapMode: Text.Wrap
                                    elide: Text.ElideNone
                                    text: qsTr("%1: %2").arg(blockerRow.modelData.name).arg(blockerRow.modelData.reasonText)
                                }
                                Label {
                                    objectName: "postBlockerMessage." + blockerRow.index
                                    width: parent.width
                                    secondary: true
                                    font: Theme.smallFont
                                    wrapMode: Text.Wrap
                                    elide: Text.ElideNone
                                    text: blockerRow.modelData.message
                                }
                            }
                            Button {
                                id: generateButton
                                objectName: "postGenerate." + blockerRow.index
                                visible: blockerRow.modelData.canGenerate
                                anchors.right: parent.right
                                anchors.rightMargin: Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Generate")
                                toolTipText: qsTr("Generate the toolpath of %1").arg(blockerRow.modelData.name)
                                onClicked: CamPost.generate(blockerRow.modelData.opId)
                            }
                        }
                    }
                }

                // ---- what the post said (errors first in the program's interest: at the top) ----
                Repeater {
                    model: CamPost.messages
                    delegate: Row {
                        id: messageRow
                        required property var modelData
                        required property int index
                        objectName: "postMessage." + index
                        spacing: Theme.spacingS
                        Icon {
                            name: messageRow.modelData.kind === "note" ? "info" : "warning"
                            color: messageRow.modelData.kind === "error" ? Theme.error
                                 : messageRow.modelData.kind === "warning" ? Theme.warning : Theme.textSecondary
                        }
                        Label {
                            objectName: "postMessageText." + messageRow.index
                            width: root.contentWidth - Theme.iconSize - Theme.spacingS
                            wrapMode: Text.Wrap
                            elide: Text.ElideNone
                            font: Theme.smallFont
                            color: messageRow.modelData.kind === "error" ? Theme.error : Theme.textPrimary
                            text: (messageRow.modelData.opName !== "" ? qsTr("%1: ").arg(messageRow.modelData.opName) : "")
                                  + messageRow.modelData.text
                                  + (messageRow.modelData.scriptLine >= 0
                                     ? qsTr(" (%1, line %2)").arg(messageRow.modelData.script)
                                                              .arg(messageRow.modelData.scriptLine) : "")
                        }
                    }
                }

                // ---- the post ----
                Heading {
                    objectName: "postSection.post"
                    text: qsTr("Post")
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("Post processor")
                    }
                    ComboBox {
                        id: postCombo
                        objectName: "postCombo"
                        width: root.fieldWidth
                        enabled: !CamPost.busy
                        model: CamPost.posts
                        currentIndex: CamPost.posts.indexOf(CamPost.postId)
                        Accessible.name: qsTr("Post processor")
                        KeyNavigation.backtab: root.lastButton
                        onActivated: CamPost.setPost(currentText)
                    }
                }
                Label {
                    objectName: "postTitle"
                    x: root.labelWidth + Theme.spacingS
                    width: root.fieldWidth
                    visible: text !== ""
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    secondary: CamPost.postState !== "failed"
                    color: CamPost.postState === "failed" ? Theme.error : Theme.textSecondary
                    font: Theme.smallFont
                    text: CamPost.postState === "ok" ? CamPost.postTitle
                        : CamPost.postState === "loading" ? qsTr("Reading the post…") : CamPost.postMessage
                }

                // ---- the program ----
                Heading {
                    objectName: "postSection.program"
                    text: qsTr("Program")
                }
                Field {
                    key: "programNumber"
                    label: qsTr("Program number")
                    hint: qsTr("The setup's program number (O number)")
                }
                Field {
                    key: "programComment"
                    label: qsTr("Program comment")
                    placeholder: qsTr("None: the document's name is used")
                    hint: qsTr("Written in the program's header")
                }
                Choice {
                    key: "units"
                    label: qsTr("Units")
                    choices: [{ value: "metric", text: qsTr("Millimetres (G21)") },
                              { value: "inch", text: qsTr("Inches (G20)") }]
                }

                // ---- output ----
                Heading {
                    objectName: "postSection.output"
                    text: qsTr("Output")
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("Folder")
                    }
                    // At rest the folder shows as its end with "…" in front, under the home folder as "~" (in full in
                    // the tooltip); being edited, as it is. What is written to is always the folder as it is.
                    TextField {
                        id: folderField
                        objectName: "postInput.directory"
                        readonly property string full: root.values.directory !== undefined ? root.values.directory : ""
                        width: root.fieldWidth - browse.width - Theme.spacingS
                        enabled: !CamPost.busy
                        text: activeFocus ? full : folderMetrics.elidedText
                        TextMetrics {
                            id: folderMetrics
                            font: folderField.font
                            text: CamPost.directoryShown
                            elide: Qt.ElideLeft
                            elideWidth: Math.max(0, folderField.width - folderField.leftPadding - folderField.rightPadding - 2)
                        }
                        HoverToolTip {
                            target: folderField
                            text: folderField.full
                        }
                        placeholderText: qsTr("A local folder or a network share")
                        Accessible.name: qsTr("Output folder")
                        onTextEdited: CamPost.setValue("directory", text)
                        Keys.onReturnPressed: CamPost.post()
                        Keys.onEnterPressed: CamPost.post()
                        Keys.onEscapePressed: CamPost.cancel()
                    }
                    Button {
                        id: browse
                        objectName: "postBrowse"
                        enabled: !CamPost.busy
                        text: qsTr("Browse…")
                        toolTipText: qsTr("Choose the output folder")
                        onClicked: {
                            if (!Files.useNativeDialogs)
                                return
                            folderLoader.active = true
                            folderLoader.item.open()
                        }
                    }
                }
                Field {
                    key: "pattern"
                    label: qsTr("File name")
                    hint: qsTr("Fields: {program}, {program:5} (zero-padded), {setup}, {doc}, {part}, {comment}, {post}, {machine}, {ext}. "
                               + "Whitespace in a field's value becomes one _; the pattern's own text keeps its spaces.")
                }
                Label {
                    objectName: "postPatternHelp"
                    x: root.labelWidth + Theme.spacingS
                    width: root.fieldWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    secondary: true
                    font: Theme.smallFont
                    text: qsTr("Fields: {program}, {program:5}, {setup}, {doc}, {part}, {comment}, {post}, {machine}, {ext}. "
                               + "Spaces in a field's value become _ (many controls refuse them); your own text stays as typed.")
                }
                Row {
                    spacing: Theme.spacingS
                    FieldLabel {
                        text: qsTr("Writes")
                    }
                    Label {
                        objectName: "postFileName"
                        width: root.fieldWidth
                        anchors.verticalCenter: parent.verticalCenter
                        font: Theme.strongFont
                        wrapMode: Text.WrapAnywhere
                        elide: Text.ElideNone
                        text: CamPost.fileName !== "" ? CamPost.fileName : "—"
                    }
                }
                Label {
                    objectName: "postFileNameNote"
                    x: root.labelWidth + Theme.spacingS
                    width: root.fieldWidth
                    visible: text !== ""
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    font: Theme.smallFont
                    color: CamPost.fileName === "" ? Theme.error : Theme.textSecondary
                    text: CamPost.fileNameNote
                }
                Choice {
                    key: "overwrite"
                    label: qsTr("If the file exists")
                    choices: [{ value: "ask", text: qsTr("Ask") },
                              { value: "replace", text: qsTr("Replace it") },
                              { value: "newName", text: qsTr("Write a new numbered name") },
                              { value: "refuse", text: qsTr("Do not write") }]
                }
                Row {
                    visible: CamPost.maxNameLength > 0
                    spacing: Theme.spacingS
                    Item {
                        width: root.labelWidth
                        height: 1
                    }
                    CheckBox {
                        objectName: "postLimitName"
                        width: root.fieldWidth
                        enabled: !CamPost.busy
                        text: qsTr("Cut the name to the machine's %1 characters").arg(CamPost.maxNameLength)
                        checked: root.values.limitNameLength === true
                        onToggled: CamPost.setValue("limitNameLength", checked)
                    }
                }

                // ---- the post's own properties (declared by its script; the machine's values are the defaults) ----
                // The heading and its rows as one block (they scroll together), and no heading where the post declares
                // no property that applies.
                Column {
                    objectName: "postPropertiesBlock"
                    readonly property bool any: {
                        if (!CamPost.form || CamPost.form.revision < 0)
                            return false
                        for (const f of CamPost.form.fields) {
                            if (f.relevant)
                                return true
                        }
                        return false
                    }
                    visible: any
                    spacing: Theme.spacingS
                    Heading {
                        objectName: "postSection.properties"
                        text: qsTr("Post properties")
                    }
                    SchemaForm {
                        objectName: "postProperties"
                        form: CamPost.form
                        labelWidth: root.labelWidth
                        fieldWidth: root.fieldWidth
                        enabled: !CamPost.busy
                        onAccepted: CamPost.post()
                        onCancelled: CamPost.cancel()
                    }
                }
            }
        }

        // The scrolled body's edges at the header and the footer: a row cut there reads as scrolled under them.
        ScrollEdges {
            objectName: "postDialogScrollEdges"
            anchors.fill: flick
            area: flick
            bottomLine: false // the footer's own rule is just below
        }

        // ---- footer (fixed) ----
        Column {
            id: footer
            objectName: "postDialogFooter"
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
                objectName: "postProgress"
                width: parent.width
                visible: CamPost.busy
                indeterminate: CamPost.progress < 0
                from: 0
                to: 1
                value: Math.max(0, CamPost.progress)
            }
            Label {
                objectName: "postStatus"
                width: parent.width
                visible: text !== ""
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                maximumLineCount: 3
                font: Theme.smallFont
                color: root.state === "failed" ? Theme.error
                     : root.state === "done" ? Theme.success
                     : root.state === "asking" ? Theme.warning : Theme.textSecondary
                text: CamPost.statusText !== "" ? CamPost.statusText
                    : !CamPost.canPost ? CamPost.postBlockedReason : qsTr("Ready to post")
            }
            // Under it, the whole path of the file just written, cut in the middle when long (in full in the tooltip).
            Label {
                id: pathLabel
                objectName: "postResultPath"
                width: parent.width
                visible: root.state === "done" && CamPost.resultPath !== ""
                secondary: true
                font: Theme.smallFont
                elide: Text.ElideMiddle
                text: CamPost.resultPath
                readonly property bool hovered: pathHover.hovered
                readonly property bool pressed: false
                HoverHandler {
                    id: pathHover
                }
                HoverToolTip {
                    target: pathLabel
                    text: CamPost.resultPath
                }
            }
            Row {
                id: buttonRow
                objectName: "postButtons"
                anchors.right: parent.right
                spacing: Theme.spacingS
                layoutDirection: Qt.RightToLeft

                // Ready: Post (primary) and Close.
                Button {
                    objectName: "postPost"
                    visible: !CamPost.busy && root.state !== "asking" && root.state !== "done"
                    primary: true
                    text: qsTr("Post")
                    enabled: CamPost.canPost
                    toolTipText: CamPost.canPost ? qsTr("Post the setup and write the program (Enter)")
                                                 : CamPost.postBlockedReason
                    onClicked: CamPost.post()
                }
                // The file exists (rule "ask").
                Button {
                    objectName: "postReplace"
                    visible: root.state === "asking"
                    primary: true
                    text: qsTr("Replace")
                    toolTipText: qsTr("Replace %1 with the new program").arg(CamPost.existingName)
                    onClicked: CamPost.answerOverwrite("replace")
                }
                Button {
                    objectName: "postNewName"
                    visible: root.state === "asking"
                    text: qsTr("Keep both")
                    toolTipText: qsTr("Write the new program under the next free numbered name")
                    onClicked: CamPost.answerOverwrite("newName")
                }
                // Written: the program in the viewer.
                Button {
                    objectName: "postShowGcode"
                    visible: root.state === "done"
                    primary: true
                    text: qsTr("Show G-code")
                    toolTipText: qsTr("Open the program in the G-code viewer")
                    onClicked: CamPost.showGcode()
                }
                // T-076: the setup sheet of the same setup, next to posting (this dialog closes, the sheet opens).
                Button {
                    objectName: "postSetupSheet"
                    visible: !CamPost.busy && root.state !== "asking"
                    text: qsTr("Setup sheet…")
                    toolTipText: qsTr("Make the setup sheet for the operator: a PDF or HTML page with the work offset, "
                                      + "stock, tools and operations")
                    onClicked: {
                        const id = CamPost.setupId
                        CamPost.close()
                        CamSheet.open(id)
                    }
                }
                Button {
                    id: cancelButton
                    objectName: "postCancel"
                    KeyNavigation.tab: generateAllButton.visible ? generateAllButton : postCombo
                    text: CamPost.busy || root.state === "asking" ? qsTr("Cancel") : qsTr("Close")
                    toolTipText: CamPost.busy ? qsTr("Stop posting; nothing is written (Esc)") : qsTr("Close (Esc)")
                    onClicked: CamPost.cancel()
                }
                Button {
                    id: generateAllButton
                    objectName: "postGenerateAll"
                    KeyNavigation.tab: postCombo
                    visible: !CamPost.busy && root.state !== "asking" && root.state !== "done"
                             && CamPost.blockers.some(b => b.canGenerate)
                    text: qsTr("Generate all")
                    toolTipText: qsTr("Generate every toolpath that is stale or missing")
                    onClicked: CamPost.generateAll()
                }
            }
        }
    }

    Loader {
        id: folderLoader
        active: false
        sourceComponent: FolderDialog {
            objectName: "postFolderDialog"
            title: qsTr("Folder for the program")
            onAccepted: CamPost.setFolder(selectedFolder)
        }
    }
}
