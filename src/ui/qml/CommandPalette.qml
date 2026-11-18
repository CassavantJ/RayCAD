// The command palette (task T-021). Opens with Ctrl+K (the ui.commandPalette command, remappable) or from the top
// bar's command search field, under the top bar.
//
//   list mode   paletteSearch filters every command as you type (PaletteModel: the registry's ranking with the view
//               commands merged in); each row shows the title, the category and the shortcut, and the highlighted
//               row its description, or why it cannot run now. With an empty query the most used commands come
//               first. Up/Down (Page Up/Down) move, Enter runs, Esc closes; hovering highlights, a click runs.
//   form mode   commands with required arguments first show a form generated from their JSON Schema (strings,
//               numbers, booleans, enums, file paths with Browse). Enter runs, Esc goes back to the list.
// Results and errors go to the shell's message banner; long-running commands show in the status bar's job slot.
import QtQuick
import QtQuick.Templates as T
import QtQuick.Dialogs
import RayCad.Ui

Item {
    id: root

    readonly property alias popup: popup
    readonly property alias model: paletteModel
    readonly property alias list: list
    readonly property alias search: search
    readonly property alias fields: fieldRepeater
    property string mode: "list"
    property string formId: ""
    property string formTitle: ""
    property string formDescription: ""
    property var formFields: []
    property string formError: ""
    property string formErrorField: ""
    property string status: ""

    signal closed()

    function openList(text) {
        mode = "list"
        formId = ""
        status = ""
        search.text = text
        paletteModel.refresh()
        list.currentIndex = paletteModel.count > 0 ? 0 : -1
        popup.open()
        search.forceActiveFocus()
        search.cursorPosition = search.text.length
    }

    function openForm(commandId) {
        if (!popup.opened)
            popup.open()
        showForm(commandId)
    }

    function move(delta) {
        if (paletteModel.count === 0)
            return
        list.currentIndex = Math.max(0, Math.min(paletteModel.count - 1, list.currentIndex + delta))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        status = ""
    }

    function activate(index) {
        const row = paletteModel.get(index)
        if (row.commandId === undefined)
            return
        if (!row.available) {
            status = qsTr("%1 is not available: %2").arg(row.title).arg(row.reason)
            return
        }
        if (row.needsForm) {
            showForm(row.commandId)
            return
        }
        popup.close()
        Commands.run(row.commandId, "{}")
    }

    function showForm(commandId) {
        const info = Commands.info(commandId)
        formTitle = info.title || commandId
        formDescription = info.description || ""
        formError = ""
        formErrorField = ""
        formFields = Commands.formFields(commandId)
        formId = commandId
        mode = "form"
        Qt.callLater(focusFirstField)
    }

    function focusFirstField() {
        for (let i = 0; i < fieldRepeater.count; ++i) {
            const f = fieldRepeater.itemAt(i)
            if (f && f.input) {
                f.input.forceActiveFocus()
                return
            }
        }
        runButton.forceActiveFocus()
    }

    function backToList() {
        mode = "list"
        formId = ""
        paletteModel.refresh()
        search.forceActiveFocus()
    }

    function formValues() {
        const values = {}
        for (let i = 0; i < fieldRepeater.count; ++i) {
            const f = fieldRepeater.itemAt(i)
            values[f.fieldData.name] = f.value
        }
        return values
    }

    function submitForm() {
        const args = Commands.buildArgs(formId, formValues())
        if (!args.ok) {
            formError = args.error
            formErrorField = args.field
            for (let i = 0; i < fieldRepeater.count; ++i) {
                const f = fieldRepeater.itemAt(i)
                if (f.fieldData.name === args.field && f.input)
                    f.input.forceActiveFocus()
            }
            return
        }
        const commandId = formId
        popup.close()
        Commands.run(commandId, args.json)
    }

    function browse(fieldData, target) {
        dialogLoader.active = true
        const dialog = dialogLoader.item
        dialog.target = target
        dialog.title = fieldData.label
        dialog.nameFilters = fieldData.filters
        dialog.fileMode = fieldData.save ? FileDialog.SaveFile : FileDialog.OpenFile
        if (target.text !== "")
            dialog.selectedFile = Commands.fileUrl(target.text)
        dialog.open()
    }

    PaletteModel {
        id: paletteModel
        commands: Commands
        query: search.text
    }

    Connections {
        target: Shortcuts
        function onChanged() {
            if (popup.opened && root.mode === "list")
                paletteModel.refresh()
        }
    }

    T.Popup {
        id: popup
        objectName: "commandPalette"
        parent: T.Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Theme.panelGap + Theme.topBarHeight + Theme.spacingXs
        width: Math.min(parent.width - 2 * Theme.spacingL, Theme.controlHeight * 22)
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        padding: Theme.spacingS
        focus: true
        modal: false
        // Esc is handled by the content (it leaves the form first).
        closePolicy: T.Popup.CloseOnPressOutside
        onClosed: root.closed()

        background: PanelBase {
            objectName: "commandPaletteBackground"
        }

        contentItem: FocusScope {
            id: content
            implicitHeight: root.mode === "list" ? listPage.implicitHeight : formPage.implicitHeight
            Keys.onEscapePressed: {
                if (root.mode === "form")
                    root.backToList()
                else
                    popup.close()
            }

            // ---------- list ----------
            Column {
                id: listPage
                objectName: "paletteListPage"
                visible: root.mode === "list"
                width: parent.width
                spacing: Theme.spacingXs

                TextField {
                    id: search
                    objectName: "paletteSearch"
                    width: parent.width
                    focus: true
                    iconName: "search"
                    placeholderText: qsTr("Type a command")
                    Accessible.name: qsTr("Search commands")
                    onTextChanged: {
                        list.currentIndex = paletteModel.count > 0 ? 0 : -1
                        list.positionViewAtBeginning()
                        root.status = ""
                    }
                    Keys.onUpPressed: root.move(-1)
                    Keys.onDownPressed: root.move(1)
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_PageUp) {
                            root.move(-8)
                            event.accepted = true
                        } else if (event.key === Qt.Key_PageDown) {
                            root.move(8)
                            event.accepted = true
                        }
                    }
                    Keys.onReturnPressed: root.activate(list.currentIndex)
                    Keys.onEnterPressed: root.activate(list.currentIndex)
                }

                ListView {
                    id: list
                    objectName: "paletteList"
                    width: parent.width
                    height: Math.min(contentHeight, Theme.rowHeight * 12)
                    clip: true
                    model: paletteModel
                    boundsBehavior: Flickable.StopAtBounds
                    highlightMoveDuration: 0
                    keyNavigationEnabled: false
                    T.ScrollBar.vertical: ScrollBar {}
                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Commands")

                    section.property: "section"
                    section.criteria: ViewSection.FullString
                    section.delegate: Item {
                        required property string section
                        width: ListView.view.width
                        height: section === "results" ? 0 : sectionLabel.implicitHeight + Theme.spacingXs * 2
                        Label {
                            id: sectionLabel
                            visible: parent.section !== "results"
                            x: Theme.spacingS
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: Theme.spacingXs / 2
                            secondary: true
                            font: Theme.smallFont
                            text: parent.section === "recent" ? qsTr("Recently used") : qsTr("All commands")
                        }
                    }

                    delegate: Item {
                        id: row
                        required property int index
                        required property string commandId
                        required property string title
                        required property string category
                        required property string description
                        required property string shortcut
                        required property bool available
                        required property string reason
                        readonly property bool current: ListView.isCurrentItem
                        readonly property string detail: available ? description : qsTr("Not available: %1").arg(reason)

                        objectName: "paletteRow." + commandId
                        width: ListView.view.width
                        height: Math.max(Theme.rowHeight, rowContent.implicitHeight + Theme.spacingXs * 2)
                        Accessible.role: Accessible.ListItem
                        Accessible.name: shortcut !== "" ? title + ", " + shortcut : title
                        Accessible.description: detail
                        Accessible.selected: current

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusSmall
                            color: row.current ? Theme.controlBgHover : "transparent"
                        }
                        Rectangle {
                            visible: row.current
                            width: Theme.focusRingWidth
                            height: parent.height - 2 * Theme.spacingXs
                            radius: width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            x: Theme.borderWidth
                            color: Theme.accent
                        }

                        Column {
                            id: rowContent
                            anchors.left: parent.left
                            anchors.right: keyChip.visible ? keyChip.left : parent.right
                            anchors.leftMargin: Theme.spacingS
                            anchors.rightMargin: Theme.spacingS
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.spacingXs / 2

                            Item {
                                width: parent.width
                                height: Math.max(titleText.implicitHeight, categoryText.implicitHeight)
                                Text {
                                    id: titleText
                                    objectName: "title"
                                    anchors.left: parent.left
                                    anchors.right: categoryText.left
                                    anchors.rightMargin: Theme.spacingS
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: row.title
                                    font: Theme.uiFont
                                    color: row.available ? Theme.textPrimary : Theme.textDisabled
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                }
                                Text {
                                    id: categoryText
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: row.category
                                    font: Theme.smallFont
                                    color: Theme.textSecondary
                                    textFormat: Text.PlainText
                                }
                            }
                            Text {
                                objectName: "detail"
                                visible: row.current && row.detail !== ""
                                width: parent.width
                                text: row.detail
                                font: Theme.smallFont
                                color: Theme.textSecondary
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                            }
                        }

                        Rectangle {
                            id: keyChip
                            objectName: "shortcutHint"
                            visible: row.shortcut !== ""
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.spacingS
                            y: Math.round((Theme.rowHeight - height) / 2)
                            width: keyText.implicitWidth + Theme.spacingS * 2
                            height: keyText.implicitHeight + Theme.spacingXs
                            radius: Theme.radiusSmall
                            color: Theme.controlBg
                            border.width: Theme.borderWidth
                            border.color: Theme.panelBorder
                            Text {
                                id: keyText
                                anchors.centerIn: parent
                                text: row.shortcut
                                font: Theme.smallFont
                                color: row.available ? Theme.textSecondary : Theme.textDisabled
                                textFormat: Text.PlainText
                            }
                        }

                        HoverHandler {
                            id: hover
                            onPointChanged: if (hovered && !row.current) list.currentIndex = row.index
                        }
                        TapHandler {
                            onTapped: {
                                list.currentIndex = row.index
                                root.activate(row.index)
                            }
                        }
                    }
                }

                Label {
                    objectName: "paletteEmpty"
                    visible: paletteModel.count === 0
                    width: parent.width
                    topPadding: Theme.spacingS
                    bottomPadding: Theme.spacingS
                    horizontalAlignment: Text.AlignHCenter
                    secondary: true
                    text: AppContext.hasSession || search.text !== "" ? qsTr("No command matches “%1”").arg(search.text)
                                                                      : qsTr("No commands")
                }

                Label {
                    objectName: "paletteStatus"
                    width: parent.width
                    leftPadding: Theme.spacingS
                    rightPadding: Theme.spacingS
                    wrapMode: Text.Wrap
                    font: Theme.smallFont
                    secondary: root.status === ""
                    color: root.status === "" ? Theme.textSecondary : Theme.warning
                    text: root.status !== "" ? root.status
                                              : qsTr("Up and Down to move, Enter to run, Esc to close")
                }
            }

            // ---------- argument form ----------
            Column {
                id: formPage
                objectName: "paletteForm"
                visible: root.mode === "form"
                width: parent.width
                spacing: Theme.spacingS

                Row {
                    width: parent.width
                    spacing: Theme.spacingXs
                    ToolButton {
                        id: backButton
                        objectName: "formBack"
                        compact: true
                        iconName: "chevron-right"
                        rotation: 180
                        anchors.verticalCenter: parent.verticalCenter
                        toolTipText: qsTr("Back to the commands")
                        onClicked: root.backToList()
                    }
                    Label {
                        objectName: "formTitle"
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - backButton.width - parent.spacing
                        font: Theme.strongFont
                        elide: Text.ElideRight
                        text: root.formTitle
                    }
                }
                Label {
                    visible: text !== ""
                    width: parent.width
                    leftPadding: Theme.spacingXs
                    secondary: true
                    wrapMode: Text.Wrap
                    text: root.formDescription
                }

                Repeater {
                    id: fieldRepeater
                    model: root.formFields

                    delegate: Column {
                        id: field
                        required property var modelData
                        required property int index
                        readonly property var fieldData: modelData
                        readonly property bool invalid: root.formErrorField === modelData.name
                        readonly property Item input: loader.item ? loader.item.input : null
                        readonly property var value: loader.item ? loader.item.value : ""
                        // Compact: the label beside the control; the description only while the field has focus.
                        readonly property real labelWidth: Math.round(Math.min(Theme.controlHeight * 6,
                                                                               width * 0.32))

                        width: formPage.width
                        spacing: Theme.spacingXs / 2

                        Item {
                            width: parent.width
                            height: loader.height

                            Label {
                                id: fieldLabel
                                x: Theme.spacingXs
                                width: field.labelWidth - Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                color: field.invalid ? Theme.error : Theme.textPrimary
                                text: field.modelData.label
                            }
                            Loader {
                                id: loader
                                x: field.labelWidth
                                width: parent.width - field.labelWidth
                                sourceComponent: field.modelData.kind === "boolean" ? boolInput
                                                 : field.modelData.kind === "enum" ? enumInput
                                                 : textInput
                            }
                        }

                        Label {
                            objectName: "formDescription." + field.modelData.name
                            visible: text !== "" && field.input !== null && field.input.activeFocus
                            x: field.labelWidth + Theme.spacingXs
                            width: parent.width - x
                            secondary: true
                            font: Theme.smallFont
                            wrapMode: Text.Wrap
                            text: field.modelData.description
                        }

                        Component {
                            id: textInput
                            Item {
                                readonly property alias input: textField
                                readonly property alias value: textField.text
                                implicitHeight: textField.implicitHeight

                                TextField {
                                    id: textField
                                    objectName: "formField." + field.modelData.name
                                    anchors.left: parent.left
                                    anchors.right: browseButton.visible ? browseButton.left : parent.right
                                    anchors.rightMargin: browseButton.visible ? Theme.spacingXs : 0
                                    invalid: field.invalid
                                    font: field.modelData.kind === "number" || field.modelData.kind === "integer"
                                          || field.modelData.kind === "json" ? Theme.monoFont : Theme.uiFont
                                    placeholderText: field.modelData.hasDefault
                                                     ? qsTr("Default: %1").arg(field.modelData.defaultText)
                                                     : field.modelData.required ? (field.modelData.path ? qsTr("File path") : "")
                                                     : qsTr("Optional")
                                    Accessible.name: field.modelData.label
                                    onAccepted: root.submitForm()
                                    onTextEdited: if (field.invalid) root.formError = ""
                                }
                                Button {
                                    id: browseButton
                                    objectName: "browse." + field.modelData.name
                                    visible: field.modelData.path
                                    anchors.right: parent.right
                                    anchors.verticalCenter: textField.verticalCenter
                                    text: qsTr("Browse…")
                                    onClicked: root.browse(field.modelData, textField)
                                }
                            }
                        }

                        Component {
                            id: boolInput
                            CheckBox {
                                id: checkInput
                                readonly property Item input: checkInput
                                readonly property bool value: checked
                                objectName: "formField." + field.modelData.name
                                Accessible.name: field.modelData.label
                                checked: field.modelData.defaultBool
                                Keys.onReturnPressed: root.submitForm()
                                Keys.onEnterPressed: root.submitForm()
                            }
                        }

                        Component {
                            id: enumInput
                            ComboBox {
                                id: choiceInput
                                readonly property Item input: choiceInput
                                readonly property bool optional: !field.modelData.required
                                readonly property string value: optional && currentIndex === 0 ? "" : currentText
                                objectName: "formField." + field.modelData.name
                                model: optional ? [field.modelData.hasDefault
                                                   ? qsTr("Default (%1)").arg(field.modelData.defaultText)
                                                   : qsTr("Not set")].concat(field.modelData.choices)
                                                : field.modelData.choices
                                currentIndex: optional ? 0 : Math.max(0, field.modelData.choices.indexOf(
                                                                          field.modelData.defaultText))
                            }
                        }
                    }
                }

                Label {
                    objectName: "formError"
                    visible: root.formError !== ""
                    width: parent.width
                    leftPadding: Theme.spacingXs
                    wrapMode: Text.Wrap
                    color: Theme.error
                    text: root.formError
                }

                Item {
                    width: parent.width
                    height: runButton.implicitHeight

                    Row {
                        anchors.right: parent.right
                        spacing: Theme.spacingS
                        Button {
                            objectName: "formCancel"
                            text: qsTr("Cancel")
                            onClicked: root.backToList()
                        }
                        Button {
                            id: runButton
                            objectName: "formRun"
                            primary: true
                            text: qsTr("Run")
                            onClicked: root.submitForm()
                            Keys.onReturnPressed: root.submitForm()
                            Keys.onEnterPressed: root.submitForm()
                        }
                    }
                }
            }
        }
    }

    // The native file dialog of the Browse buttons, created on first use.
    Loader {
        id: dialogLoader
        active: false
        sourceComponent: Component {
            FileDialog {
                objectName: "paletteFileDialog"
                property Item target: null
                onAccepted: if (target) target.text = Commands.localPath(selectedFile)
            }
        }
    }
}
