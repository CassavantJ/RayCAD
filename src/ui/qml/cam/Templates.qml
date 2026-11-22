// The operation template panel (task T-057; CamTemplates, cam/templates_model.h): a large panel over the shell, opened
// on a setup from the CAM tree (its Templates button), the command Operation Templates (cam.templates.open), or on an
// operation to save it as a template (its row's Save as template button, cam.templates.saveFrom).
//
//   Browse   left: the search, the feature kind and material filters, and the list: template sets (the templates
//            applied together, for one material) with their templates under them in order, and templates of no set.
//            Up and Down move through it. Each says where it comes from; built-in ones are read-only (a lock).
//   Details  right: the setup and the holes it would apply to (every recognised hole, one group of alike holes, or the
//            holes of the faces selected in the view); what is selected (a set's steps, or a template's rules), with
//            rename and delete for a template of a writable library; and the preview: the operations it would make,
//            each with its tool and number and its hole count, or why it would make none (which tool is missing for
//            which holes), worked out before anything is applied.
//   Save     the name, set, step, kind, material, tool rule, feeds rule and heights rule of a template made from an
//            operation.
//   Footer   fixed at the bottom, never scrolled away: the status, a delete question, and Apply / Close (Save /
//            Cancel). Esc steps back (a question, a rename), then closes; Ctrl+Return applies or saves.
// No code of its own for any operation type: every type's templates show and apply alike.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

Item {
    id: root
    objectName: "templatesHost"

    Loader {
        id: loader
        objectName: "templatesLoader"
        property bool used: CamTemplates.shown
        onUsedChanged: if (CamTemplates.shown) used = true
        active: used
        sourceComponent: panelComponent
    }

    Component {
        id: panelComponent

        T.Popup {
            id: popup
            objectName: "templatesPanel"
            parent: T.Overlay.overlay
            anchors.centerIn: parent
            readonly property real inset: Math.max(Theme.controlHeight * 3, Theme.spacingL * 4)
            width: Math.min(parent.width - 2 * inset, Theme.controlHeight * 46)
            // Saving is a short form: a smaller card.
            height: Math.min(parent.height - 2 * inset, Theme.controlHeight * (saving ? 17 : 30))
            padding: 0
            modal: true
            dim: false
            focus: true
            visible: CamTemplates.shown
            closePolicy: T.Popup.NoAutoClose
            onOpened: Qt.callLater(() => saving ? nameField.forceActiveFocus() : search.forceActiveFocus())

            readonly property bool saving: CamTemplates.mode === "save"
            readonly property var info: CamTemplates.selectedInfo
            property bool renaming: false

            function stepBack() {
                if (popup.renaming)
                    popup.renaming = false
                else if (CamTemplates.deleteAsked !== "")
                    CamTemplates.cancelDelete()
                else
                    CamTemplates.close()
            }
            function rowIndexOf(key) {
                const rows = CamTemplates.rows
                for (let i = 0; i < rows.length; ++i) {
                    if (rows[i].key === key)
                        return i
                }
                return -1
            }
            function selectRow(i) {
                const rows = CamTemplates.rows
                if (i < 0 || i >= rows.length)
                    return
                popup.renaming = false
                CamTemplates.selected = rows[i].key
                listView.currentIndex = i
                listView.positionViewAtIndex(i, ListView.Contain)
            }
            function showSelected() {
                listView.currentIndex = popup.rowIndexOf(CamTemplates.selected)
                if (listView.currentIndex >= 0)
                    listView.positionViewAtIndex(listView.currentIndex, ListView.Contain)
            }
            function indexIn(list, value) {
                for (let i = 0; i < list.length; ++i) {
                    if (list[i].value === value)
                        return i
                }
                return -1
            }

            background: PanelBase {
                objectName: "templatesBackground"
            }

            Connections {
                target: CamTemplates
                function onCatalogChanged() {
                    popup.showSelected()
                }
                function onSelectionChanged() {
                    popup.showSelected()
                }
            }

            contentItem: FocusScope {
                id: scope

                Shortcut {
                    sequence: "Esc"
                    enabled: CamTemplates.shown
                    onActivated: popup.stepBack()
                }
                Shortcut {
                    sequences: ["Ctrl+Return", "Ctrl+Enter"]
                    enabled: CamTemplates.shown
                    onActivated: popup.saving ? CamTemplates.save() : CamTemplates.apply()
                }

                PanelHeader {
                    id: header
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    title: popup.saving ? qsTr("Save %1 as a template").arg(CamTemplates.saveOperationName)
                                        : qsTr("Operation templates")
                    iconName: "template"

                    ToolButton {
                        objectName: "templatesClose"
                        compact: true
                        iconName: "close"
                        toolTipText: qsTr("Close (Esc)")
                        onClicked: CamTemplates.close()
                    }
                }

                // ---------------- browse ----------------
                Item {
                    id: left
                    objectName: "templatesBrowser"
                    visible: !popup.saving
                    anchors.top: header.bottom
                    anchors.topMargin: Theme.spacingS
                    anchors.bottom: footer.top
                    anchors.bottomMargin: Theme.spacingS
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.spacingM
                    width: Math.max(Theme.controlHeight * 15, parent.width * 0.4)

                    Column {
                        id: filters
                        width: parent.width
                        spacing: Theme.spacingS
                        TextField {
                            id: search
                            objectName: "templatesSearch"
                            width: parent.width
                            iconName: "search"
                            placeholderText: qsTr("Search name, set, kind, material, library…")
                            Accessible.name: qsTr("Search templates")
                            text: CamTemplates.search
                            onTextChanged: CamTemplates.search = text
                            Keys.onDownPressed: {
                                listView.forceActiveFocus()
                                popup.selectRow(Math.max(0, listView.currentIndex))
                            }
                        }
                        Row {
                            spacing: Theme.spacingS
                            ComboBox {
                                id: kindFilter
                                objectName: "templatesKindFilter"
                                width: (filters.width - Theme.spacingS) / 2
                                textRole: "text"
                                valueRole: "value"
                                model: CamTemplates.kinds
                                currentIndex: popup.indexIn(CamTemplates.kinds, CamTemplates.kindFilter)
                                onActivated: CamTemplates.kindFilter = currentValue
                                Accessible.name: qsTr("For")
                            }
                            ComboBox {
                                id: materialFilter
                                objectName: "templatesMaterialFilter"
                                width: (filters.width - Theme.spacingS) / 2
                                textRole: "text"
                                valueRole: "value"
                                model: CamTemplates.materials
                                currentIndex: popup.indexIn(CamTemplates.materials, CamTemplates.materialFilter)
                                onActivated: CamTemplates.materialFilter = currentValue
                                Accessible.name: qsTr("Material")
                            }
                        }
                        Label {
                            objectName: "templatesCount"
                            width: parent.width
                            secondary: true
                            font: Theme.smallFont
                            text: CamTemplates.templateCount === 1 ? qsTr("1 template")
                                  : qsTr("%1 templates").arg(CamTemplates.templateCount)
                        }
                    }

                    Rectangle {
                        anchors.top: filters.bottom
                        anchors.topMargin: Theme.spacingXs
                        anchors.bottom: parent.bottom
                        width: parent.width
                        radius: Theme.radiusSmall
                        color: "transparent"
                        border.width: Theme.borderWidth
                        border.color: listView.activeFocus ? Theme.focusRing : Theme.panelBorder

                        ListView {
                            id: listView
                            objectName: "templatesList"
                            anchors.fill: parent
                            anchors.margins: Theme.borderWidth
                            clip: true
                            model: CamTemplates.rows
                            currentIndex: -1
                            activeFocusOnTab: true
                            boundsBehavior: Flickable.StopAtBounds
                            highlightMoveDuration: 0
                            ScrollBar.vertical: ScrollBar {}
                            Accessible.role: Accessible.List
                            Accessible.name: qsTr("Templates")
                            // F2 and Delete belong to the list while it has the focus (not to a window shortcut).
                            Keys.onShortcutOverride: event => {
                                event.accepted = (event.key === Qt.Key_F2 || event.key === Qt.Key_Delete)
                                                 && popup.info.canRename === true
                            }
                            Keys.onUpPressed: event => {
                                if (currentIndex <= 0)
                                    search.forceActiveFocus()
                                else
                                    popup.selectRow(currentIndex - 1)
                                event.accepted = true
                            }
                            Keys.onDownPressed: event => {
                                popup.selectRow(Math.min(count - 1, currentIndex + 1))
                                event.accepted = true
                            }
                            Keys.onPressed: event => {
                                if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                                    popup.selectRow(event.key === Qt.Key_Home ? 0 : count - 1)
                                    event.accepted = true
                                } else if (event.key === Qt.Key_F2 && popup.info.canRename === true) {
                                    popup.renaming = true
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Delete && popup.info.canRename === true) {
                                    CamTemplates.askDelete(popup.info.id)
                                    event.accepted = true
                                }
                            }

                            delegate: Item {
                                id: row
                                required property var modelData
                                required property int index
                                readonly property bool isSet: modelData.rowKind === "set"
                                readonly property bool selected: modelData.key === CamTemplates.selected
                                readonly property bool hovered: mouse.containsMouse
                                objectName: "templateRow." + index
                                // The current row has the focus: F2 and Delete go on to the list's keys, not to a
                                // window shortcut.
                                Keys.onShortcutOverride: event => {
                                    event.accepted = (event.key === Qt.Key_F2 || event.key === Qt.Key_Delete)
                                                     && popup.info.canRename === true
                                }
                                width: ListView.view.width
                                height: Theme.rowHeight * 1.6
                                Accessible.role: Accessible.ListItem
                                Accessible.name: (isSet ? qsTr("Set %1").arg(modelData.title) : modelData.title) + ", "
                                                 + modelData.detail + (modelData.readOnly ? ", " + qsTr("read-only") : "")
                                Accessible.selected: selected

                                Rectangle {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spacingXs
                                    anchors.rightMargin: Theme.spacingXs
                                    radius: Theme.radiusSmall
                                    color: row.selected ? Theme.selectionFill : row.hovered ? Theme.controlBgHover
                                                                                            : "transparent"
                                    Rectangle {
                                        visible: row.selected
                                        width: Theme.focusRingWidth
                                        height: parent.height - 2 * Theme.spacingXs
                                        radius: width / 2
                                        anchors.verticalCenter: parent.verticalCenter
                                        x: Theme.borderWidth
                                        color: Theme.selection
                                    }
                                }
                                MouseArea {
                                    id: mouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: {
                                        listView.forceActiveFocus()
                                        popup.selectRow(row.index)
                                    }
                                }
                                Icon {
                                    id: rowIcon
                                    x: Theme.spacingM + Theme.spacingL * row.modelData.depth
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: row.isSet ? "template" : "sliders"
                                    color: Theme.textSecondary
                                }
                                Column {
                                    anchors.left: rowIcon.right
                                    anchors.leftMargin: Theme.spacingS
                                    anchors.right: lock.left
                                    anchors.rightMargin: Theme.spacingS
                                    anchors.verticalCenter: parent.verticalCenter
                                    Label {
                                        objectName: "templateRowName"
                                        width: parent.width
                                        elide: Text.ElideRight
                                        font: row.isSet ? Theme.strongFont : Theme.uiFont
                                        text: row.modelData.title
                                    }
                                    Label {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        secondary: true
                                        font: Theme.smallFont
                                        text: row.modelData.detail
                                    }
                                }
                                Icon {
                                    id: lock
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.spacingS
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "lock"
                                    color: Theme.textSecondary
                                    opacity: row.modelData.readOnly ? 1 : 0
                                }
                            }
                        }
                        Label {
                            objectName: "templatesEmpty"
                            visible: listView.count === 0
                            anchors.centerIn: parent
                            width: parent.width - 2 * Theme.spacingM
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            secondary: true
                            text: qsTr("No template matches. Clear the search, or choose another kind or material.")
                        }
                    }
                }

                // ---------------- details and preview ----------------
                Flickable {
                    id: right
                    objectName: "templatesDetailsScroll"
                    visible: !popup.saving
                    anchors.top: header.bottom
                    anchors.topMargin: Theme.spacingS
                    anchors.bottom: footer.top
                    anchors.bottomMargin: Theme.spacingS
                    anchors.left: left.right
                    anchors.leftMargin: Theme.spacingL
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingM
                    clip: true
                    contentHeight: details.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    Column {
                        id: details
                        width: right.width - Theme.spacingS
                        spacing: Theme.spacingS

                        // Where: the setup and the holes.
                        Row {
                            spacing: Theme.spacingS
                            Label {
                                width: Theme.controlHeight * 3.4
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                text: qsTr("Setup")
                            }
                            ComboBox {
                                id: setupCombo
                                objectName: "templatesSetup"
                                width: details.width - Theme.controlHeight * 3.4 - Theme.spacingS
                                textRole: "text"
                                valueRole: "value"
                                model: CamTemplates.setups
                                currentIndex: popup.indexIn(CamTemplates.setups, CamTemplates.setupId)
                                onActivated: CamTemplates.setupId = currentValue
                                Accessible.name: qsTr("Setup")
                            }
                        }
                        Row {
                            spacing: Theme.spacingS
                            Label {
                                width: Theme.controlHeight * 3.4
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                text: qsTr("Apply to")
                            }
                            ComboBox {
                                id: targetCombo
                                objectName: "templatesTarget"
                                width: details.width - Theme.controlHeight * 3.4 - Theme.spacingS
                                textRole: "text"
                                valueRole: "value"
                                model: CamTemplates.targets
                                currentIndex: popup.indexIn(CamTemplates.targets, CamTemplates.target)
                                onActivated: CamTemplates.target = currentValue
                                Accessible.name: qsTr("Apply to")
                            }
                        }

                        Rectangle {
                            width: details.width
                            height: Theme.borderWidth
                            color: Theme.panelBorder
                        }

                        // What is selected.
                        Label {
                            objectName: "templatesNothing"
                            visible: popup.info.kind === undefined
                            width: details.width
                            wrapMode: Text.Wrap
                            secondary: true
                            text: qsTr("Choose a template set (its steps are applied together, in order) or one template on the left.")
                        }
                        Item {
                            visible: popup.info.kind !== undefined
                            width: details.width
                            height: Math.max(titleLabel.implicitHeight, nameEdit.implicitHeight, manage.implicitHeight)
                            Label {
                                id: titleLabel
                                objectName: "templatesTitle"
                                visible: !popup.renaming
                                anchors.left: parent.left
                                anchors.right: manage.left
                                anchors.rightMargin: Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideRight
                                font: Theme.strongFont
                                text: popup.info.title !== undefined ? popup.info.title : ""
                            }
                            TextField {
                                id: nameEdit
                                objectName: "templatesRenameField"
                                visible: popup.renaming
                                anchors.left: parent.left
                                anchors.right: manage.left
                                anchors.rightMargin: Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                Accessible.name: qsTr("New name of the template")
                                onVisibleChanged: if (visible) {
                                    text = popup.info.title
                                    forceActiveFocus()
                                    selectAll()
                                }
                                Keys.onReturnPressed: event => {
                                    CamTemplates.rename(popup.info.id, text)
                                    popup.renaming = false
                                    event.accepted = true
                                }
                                Keys.onEnterPressed: event => {
                                    CamTemplates.rename(popup.info.id, text)
                                    popup.renaming = false
                                    event.accepted = true
                                }
                            }
                            Row {
                                id: manage
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.spacingXs
                                visible: popup.info.canRename === true
                                ToolButton {
                                    objectName: "templatesRename"
                                    compact: true
                                    iconName: "sketch"
                                    checked: popup.renaming
                                    toolTipText: popup.renaming ? qsTr("Stop renaming (Esc)") : qsTr("Rename this template (F2)")
                                    onClicked: popup.renaming = !popup.renaming
                                }
                                ToolButton {
                                    objectName: "templatesDelete"
                                    compact: true
                                    iconName: "trash"
                                    toolTipText: qsTr("Delete this template from %1 (Delete)").arg(popup.info.library !== undefined ? popup.info.library : "")
                                    onClicked: CamTemplates.askDelete(popup.info.id)
                                }
                            }
                        }
                        Label {
                            objectName: "templatesSubtitle"
                            visible: popup.info.subtitle !== undefined
                            width: details.width
                            wrapMode: Text.Wrap
                            secondary: true
                            text: popup.info.subtitle !== undefined ? popup.info.subtitle : ""
                        }
                        // A set's steps, in order.
                        Repeater {
                            model: popup.info.members !== undefined ? popup.info.members : []
                            delegate: Label {
                                required property string modelData
                                width: details.width
                                leftPadding: Theme.spacingM
                                wrapMode: Text.Wrap
                                text: modelData
                            }
                        }
                        // A template's rules.
                        Repeater {
                            model: popup.info.lines !== undefined ? popup.info.lines : []
                            delegate: Row {
                                id: ruleRow
                                required property var modelData
                                spacing: Theme.spacingS
                                Label {
                                    width: Theme.controlHeight * 3.4
                                    secondary: true
                                    wrapMode: Text.Wrap
                                    text: ruleRow.modelData.label
                                }
                                Label {
                                    width: details.width - Theme.controlHeight * 3.4 - Theme.spacingS
                                    wrapMode: Text.Wrap
                                    elide: Text.ElideNone
                                    maximumLineCount: 6
                                    text: ruleRow.modelData.value
                                }
                            }
                        }

                        Rectangle {
                            width: details.width
                            height: Theme.borderWidth
                            color: Theme.panelBorder
                        }

                        // The preview: what applying would make, before anything is applied.
                        Label {
                            width: details.width
                            font: Theme.strongFont
                            text: qsTr("What it would make")
                        }
                        Label {
                            objectName: "templatesPreviewState"
                            width: details.width
                            wrapMode: Text.Wrap
                            secondary: CamTemplates.previewState !== "refused" && CamTemplates.previewState !== "failed"
                            color: CamTemplates.previewState === "refused" || CamTemplates.previewState === "failed"
                                   ? Theme.error : Theme.textSecondary
                            text: CamTemplates.previewState === "working" ? qsTr("Working it out…")
                                  : CamTemplates.previewState === "ok" ? CamTemplates.previewSummary
                                  : CamTemplates.previewState === "" ? (popup.info.kind === undefined ? qsTr("Nothing yet")
                                                                        : CamTemplates.applyBlockedReason)
                                  : qsTr("It cannot be applied: %1").arg(CamTemplates.previewMessage.charAt(0).toLowerCase()
                                                                          + CamTemplates.previewMessage.slice(1))
                        }
                        Repeater {
                            // Only what applying would make: nothing is made while a tool is missing.
                            model: CamTemplates.previewState === "ok" ? CamTemplates.previewOps : []
                            delegate: Item {
                                id: opRow
                                required property var modelData
                                required property int index
                                objectName: "templatesPreviewOp." + index
                                width: details.width
                                height: opColumn.implicitHeight + Theme.spacingXs
                                Accessible.role: Accessible.StaticText
                                Accessible.name: qsTr("%1, tool T%2 %3, %4").arg(modelData.name)
                                                 .arg(modelData.toolNumber).arg(modelData.tool)
                                                 .arg(modelData.holes === 1 ? qsTr("1 hole") : qsTr("%1 holes").arg(modelData.holes))
                                Rectangle {
                                    width: Theme.spacingXs
                                    height: parent.height - Theme.spacingXs
                                    radius: width / 2
                                    color: Theme.accent
                                }
                                Column {
                                    id: opColumn
                                    x: Theme.spacingM
                                    width: parent.width - x
                                    Label {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        text: qsTr("%1. %2").arg(opRow.index + 1).arg(opRow.modelData.name)
                                    }
                                    Label {
                                        width: parent.width
                                        wrapMode: Text.Wrap
                                        secondary: true
                                        font: Theme.smallFont
                                        text: qsTr("T%1 %2 · %3").arg(opRow.modelData.toolNumber)
                                              .arg(opRow.modelData.tool)
                                              .arg(opRow.modelData.holes === 1 ? qsTr("1 hole")
                                                                               : qsTr("%1 holes").arg(opRow.modelData.holes))
                                    }
                                }
                            }
                        }
                    }
                }

                // ---------------- save ----------------
                Flickable {
                    id: saveForm
                    objectName: "templatesSaveScroll"
                    visible: popup.saving
                    anchors.top: header.bottom
                    anchors.topMargin: Theme.spacingM
                    anchors.bottom: footer.top
                    anchors.bottomMargin: Theme.spacingS
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.spacingL
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingL
                    clip: true
                    contentHeight: saveColumn.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    readonly property var values: CamTemplates.saveValues
                    readonly property real labelWidth: Theme.controlHeight * 5
                    readonly property real fieldWidth: Math.min(Theme.controlHeight * 16, saveColumn.width - labelWidth - Theme.spacingS)

                    // The rules to choose, each a list (no code of its own per operation type).
                    readonly property var choices: [
                        { key: "featureKind", label: qsTr("For"),
                          choices: [{ value: "auto", text: qsTr("What the operation's holes are") }].concat(CamTemplates.kinds) },
                        { key: "material", label: qsTr("Material"),
                          choices: [{ value: "setup", text: CamTemplates.materials.length > 0 ? CamTemplates.materials[0].text
                                                                                              : qsTr("The setup's material") },
                                    { value: "any", text: qsTr("Any material") }] },
                        { key: "tool", label: qsTr("Tool"),
                          choices: [{ value: "rule", text: qsTr("A rule: its type and size against the hole") },
                                    { value: "exact", text: qsTr("This exact tool") }] },
                        { key: "feeds", label: qsTr("Feeds"),
                          choices: [{ value: "calculated", text: qsTr("Calculated for the tool and material") },
                                    { value: "fixed", text: qsTr("Fixed: the operation's speed and feeds") }] },
                        { key: "heights", label: qsTr("Heights"),
                          choices: [{ value: "operation", text: qsTr("The operation's heights") },
                                    { value: "defaults", text: qsTr("The panel's defaults") }] }
                    ]

                    Column {
                        id: saveColumn
                        width: saveForm.width
                        spacing: Theme.spacingS
                        Label {
                            width: parent.width
                            wrapMode: Text.Wrap
                            secondary: true
                            text: qsTr("The template keeps the operation's type and parameters, and rules for its tool, feeds and heights. "
                                       + "It goes into your library; operations made from it later are ordinary operations.")
                        }
                        Row {
                            spacing: Theme.spacingS
                            Label {
                                width: saveForm.labelWidth
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                text: qsTr("Name")
                            }
                            TextField {
                                id: nameField
                                objectName: "templatesSaveName"
                                width: saveForm.fieldWidth
                                text: saveForm.values.name !== undefined ? saveForm.values.name : ""
                                onTextEdited: CamTemplates.setSaveValue("name", text)
                                invalid: text.trim() === ""
                                Accessible.name: qsTr("Template name")
                            }
                        }
                        Row {
                            spacing: Theme.spacingS
                            Label {
                                width: saveForm.labelWidth
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                text: qsTr("Set")
                            }
                            TextField {
                                objectName: "templatesSaveSet"
                                width: saveForm.fieldWidth
                                placeholderText: qsTr("None: the template on its own")
                                text: saveForm.values.set !== undefined ? saveForm.values.set : ""
                                onTextEdited: CamTemplates.setSaveValue("set", text)
                                Accessible.name: qsTr("Template set")
                            }
                        }
                        Row {
                            spacing: Theme.spacingS
                            Label {
                                width: saveForm.labelWidth
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                text: qsTr("Step in the set")
                            }
                            TextField {
                                objectName: "templatesSaveSequence"
                                width: Theme.controlHeight * 4
                                text: saveForm.values.sequence !== undefined ? saveForm.values.sequence : ""
                                onTextEdited: CamTemplates.setSaveValue("sequence", text)
                                invalid: !CamTemplates.canSave && text.trim() !== "" && isNaN(parseInt(text))
                                Accessible.name: qsTr("Step in the set")
                            }
                        }
                        Repeater {
                            model: saveForm.choices
                            delegate: Row {
                                id: choice
                                required property var modelData
                                objectName: "templatesSave." + modelData.key
                                spacing: Theme.spacingS
                                Label {
                                    width: saveForm.labelWidth
                                    anchors.verticalCenter: parent.verticalCenter
                                    secondary: true
                                    text: choice.modelData.label
                                }
                                ComboBox {
                                    objectName: "templatesSaveCombo." + choice.modelData.key
                                    width: saveForm.fieldWidth
                                    textRole: "text"
                                    valueRole: "value"
                                    model: choice.modelData.choices
                                    currentIndex: popup.indexIn(choice.modelData.choices, saveForm.values[choice.modelData.key])
                                    onActivated: CamTemplates.setSaveValue(choice.modelData.key, currentValue)
                                    Accessible.name: choice.modelData.label
                                }
                            }
                        }
                    }
                }

                // ---------------- the footer: always in place ----------------
                Column {
                    id: footer
                    objectName: "templatesFooter"
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.spacingM
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.spacingM
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingM
                    spacing: Theme.spacingS

                    Row {
                        objectName: "templatesDeleteQuestion"
                        visible: CamTemplates.deleteAsked !== ""
                        spacing: Theme.spacingS
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Delete %1? Its file is removed from %2; operations made from it stay.")
                                  .arg(popup.info.title !== undefined ? popup.info.title : "")
                                  .arg(popup.info.library !== undefined ? popup.info.library : "")
                        }
                        Button {
                            objectName: "templatesDeleteConfirm"
                            text: qsTr("Delete")
                            onClicked: CamTemplates.confirmDelete()
                        }
                        Button {
                            objectName: "templatesDeleteCancel"
                            flat: true
                            text: qsTr("Keep")
                            onClicked: CamTemplates.cancelDelete()
                        }
                    }
                    Item {
                        width: parent.width
                        height: Math.max(statusLabel.implicitHeight, buttons.height)
                        Label {
                            id: statusLabel
                            objectName: "templatesStatus"
                            anchors.left: parent.left
                            anchors.right: buttons.left
                            anchors.rightMargin: Theme.spacingS
                            anchors.verticalCenter: parent.verticalCenter
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            color: CamTemplates.statusIsError ? Theme.error : Theme.textSecondary
                            text: CamTemplates.busy ? qsTr("Working…") : CamTemplates.status
                        }
                        Row {
                            id: buttons
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.spacingS
                            Button {
                                objectName: "templatesApply"
                                visible: !popup.saving
                                primary: true
                                text: qsTr("Apply")
                                enabled: CamTemplates.canApply
                                toolTipText: CamTemplates.canApply
                                             ? qsTr("Make these operations in the setup, as one undo step (Ctrl+Enter)")
                                             : CamTemplates.applyBlockedReason
                                onClicked: CamTemplates.apply()
                            }
                            Button {
                                objectName: "templatesSave"
                                visible: popup.saving
                                primary: true
                                text: qsTr("Save")
                                enabled: CamTemplates.canSave
                                toolTipText: qsTr("Save the template to your library (Ctrl+Enter)")
                                onClicked: CamTemplates.save()
                            }
                            Button {
                                objectName: "templatesCloseButton"
                                text: popup.saving ? qsTr("Cancel") : qsTr("Close")
                                toolTipText: qsTr("Close the panel (Esc)")
                                onClicked: CamTemplates.close()
                            }
                        }
                    }
                }
            }
        }
    }
}
