// The tool library manager (task T-046; CamTools and ToolList, cam/tools_model.h): a large panel over the shell,
// opened by the Tool Library command (cam.toolLibrary.open, Shift+T in the CAM workspace), with or without a document.
//
//   Left     Tools | Holders; the search, type, material and diameter filters and the sort; the list (virtualised),
//            the open document's tools first ("In this document", a mark where the library's original now has another
//            shape), then the library's, each with the library it comes from and a lock where it is read-only. Up and
//            Down move through it, Enter opens the selected item's fields.
//   Right    the selected item: where it is, its side view to scale (in a holder when one is chosen), its fields and
//            its presets (tools) or segments (holders); problems beside their fields in the schema's words. A
//            built-in item offers "Duplicate to my library"; the document's copy "Update from library". Save
//            (Ctrl+S) and Revert (Esc) at the bottom, with the result, or a conflict with its choices: retry, save a
//            copy to my library, discard my changes.
//   Esc      cancels the step in front (a menu, the delete question, a conflict message, unsaved edits), then closes.
// The panel itself is made the first time it is shown.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

Item {
    id: root
    objectName: "toolLibraryHost"

    Loader {
        id: loader
        objectName: "toolLibraryLoader"
        property bool used: CamTools.shown
        onUsedChanged: if (CamTools.shown) used = true
        active: used
        sourceComponent: panelComponent
    }

    Component {
        id: panelComponent

        T.Popup {
            id: popup
            objectName: "toolLibraryPanel"
            parent: T.Overlay.overlay
            anchors.centerIn: parent
            // Large, but a dialog over the shell, not a page in its place: the shell stays in sight all round it.
            readonly property real inset: Math.max(Theme.controlHeight * 3, Theme.spacingL * 4)
            width: Math.min(parent.width - 2 * inset, Theme.controlHeight * 50)
            height: Math.min(parent.height - 2 * inset, Theme.controlHeight * 32)
            padding: 0
            modal: true
            dim: false
            focus: true
            visible: CamTools.shown
            closePolicy: T.Popup.NoAutoClose
            onOpened: search.forceActiveFocus()

            readonly property bool holders: tabs.currentIndex === 1
            readonly property bool editing: CamTools.editorKind !== ""
            readonly property var choices: ({
                type: CamTools.toolTypes,
                opClass: CamTools.opClasses,
                material: popup.materialChoices,
                centreCutting: CamTools.centreCuttingChoices
            })
            readonly property var materialChoices: {
                const out = []
                for (const m of CamTools.materials)
                    out.push({ value: m.id, text: m.name })
                return out
            }

            // Esc: the step in front first.
            function stepBack() {
                if (CamTools.deleteAsked)
                    CamTools.cancelDelete()
                else if (Object.keys(CamTools.conflict).length > 0)
                    CamTools.resolveConflict("cancel")
                else if (CamTools.dirty)
                    CamTools.revert()
                else
                    CamTools.hide()
            }
            function selectRow(row) {
                if (row < 0 || row >= toolList.count)
                    return
                const r = toolList.get(row)
                if (CamTools.select(r.origin, popup.holders ? "holder" : "tool", r.itemId))
                    listView.currentIndex = row
                else
                    listView.currentIndex = toolList.rowOf(CamTools.selectedOrigin, CamTools.selectedId)
            }
            function openEditor() {
                if (popup.editing)
                    form.focusFirst()
            }

            // The preset or segment cards opened by hand (index -> true), for the item they were opened on.
            property var openRows: ({})
            property string openRowsFor: ""
            function toggleRow(i) {
                const o = Object.assign({}, openRows)
                o[i] = !(o[i] === true)
                openRows = o
            }
            function openRow(i) {
                const o = Object.assign({}, openRows)
                o[i] = true
                openRows = o
            }
            function choiceText(list, value) {
                for (const c of list) {
                    if (c.value === value)
                        return c.text
                }
                return value
            }
            // One line per card: what the row is, so a closed card still says it.
            function rowSummary(v) {
                if (!v)
                    return ""
                if (CamTools.editorKind === "holder")
                    return qsTr("Height %1 · Ø %2 to %3").arg(v.height).arg(v.lowerDiameter).arg(v.upperDiameter)
                return qsTr("%1 · %2 · %3 · %4").arg(choiceText(popup.materialChoices, v.material))
                                                .arg(choiceText(CamTools.opClasses, v.opClass))
                                                .arg(v.surfaceSpeed).arg(v.chipLoad)
            }

            background: PanelBase {
                objectName: "toolLibraryBackground"
            }

            Connections {
                target: CamTools
                function onSaved(kind, id) {
                    listView.currentIndex = toolList.rowOf("library", id)
                }
                function onEditorChanged() {
                    if (CamTools.selectedId === "")
                        listView.currentIndex = -1
                    if (popup.openRowsFor !== CamTools.selectedId) {
                        popup.openRowsFor = CamTools.selectedId
                        popup.openRows = ({})
                    }
                }
            }

            contentItem: FocusScope {
                id: scope

                // The panel's keys (inside its content, where the window's shortcut map finds them).
                Shortcut {
                    sequences: [StandardKey.Save]
                    enabled: CamTools.shown
                    onActivated: CamTools.save()
                }
                Shortcut {
                    sequence: "Esc"
                    enabled: CamTools.shown
                    onActivated: popup.stepBack()
                }
                Shortcut {
                    sequence: "Ctrl+D"
                    enabled: CamTools.shown && popup.editing
                    onActivated: {
                        if (CamTools.duplicate())
                            form.focusFirst()
                    }
                }

                PanelHeader {
                    id: header
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    title: qsTr("Tool library")
                    iconName: "endmill"

                    ToolButton {
                        objectName: "toolLibraryRefresh"
                        compact: true
                        iconName: "refresh"
                        enabled: !CamTools.refreshing
                        toolTipText: CamTools.refreshing ? qsTr("Reading the library folders…")
                                                         : qsTr("Read the library folders again")
                        onClicked: CamTools.refresh()
                    }
                    ToolButton {
                        objectName: "toolLibraryClose"
                        compact: true
                        iconName: "close"
                        toolTipText: qsTr("Close (Esc)")
                        onClicked: CamTools.hide()
                    }
                }

                // The libraries, lowest first: where each item comes from, and which ones are read-only.
                Flow {
                    id: rootsRow
                    objectName: "toolLibraryRoots"
                    anchors.top: header.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: Theme.spacingM
                    anchors.topMargin: Theme.spacingS
                    spacing: Theme.spacingM
                    Label {
                        secondary: true
                        text: qsTr("Libraries:")
                    }
                    Repeater {
                        model: CamTools.roots
                        delegate: Row {
                            required property var modelData
                            required property int index
                            objectName: "toolLibraryRoot." + index
                            spacing: Theme.spacingXs
                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: parent.modelData.writable ? "document" : "lock"
                                color: Theme.textSecondary
                            }
                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                text: (parent.modelData.kind === "user" ? qsTr("My library") : parent.modelData.name)
                                      + "  " + (parent.modelData.loading ? qsTr("(reading…)")
                                                : !parent.modelData.open ? qsTr("(cannot be opened)")
                                                : parent.modelData.kind === "builtin" ? qsTr("(built-in, read-only)")
                                                : parent.modelData.kind === "shared" ? qsTr("(shared)") : "")
                                color: parent.modelData.open || parent.modelData.loading ? Theme.textPrimary : Theme.error
                            }
                        }
                    }
                    Label {
                        visible: CamTools.issueCount > 0
                        color: Theme.warning
                        text: qsTr("%n file(s) could not be read", "", CamTools.issueCount)
                    }
                }

                // ---------------- the list ----------------
                Item {
                    id: left
                    objectName: "toolLibraryBrowser"
                    anchors.top: rootsRow.bottom
                    anchors.topMargin: Theme.spacingS
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.spacingM
                    anchors.bottomMargin: Theme.spacingM
                    width: Math.max(Theme.controlHeight * 17, parent.width * 0.42)

                    ToolList {
                        id: toolList
                        objectName: "toolLibraryList"
                        kind: popup.holders ? "holders" : "tools"
                        text: search.text
                        type: popup.holders ? "" : (typeFilter.currentValue !== undefined ? typeFilter.currentValue : "")
                        material: popup.holders ? "" : (materialFilter.currentValue !== undefined ? materialFilter.currentValue : "")
                        minDiameter: minField.text
                        maxDiameter: maxField.text
                        sort: sortChoice.currentValue !== undefined ? sortChoice.currentValue : "diameter"
                        onCountChanged: {
                            const r = toolList.rowOf(CamTools.selectedOrigin, CamTools.selectedId)
                            if (r !== listView.currentIndex)
                                listView.currentIndex = r
                        }
                    }

                    Column {
                        id: filters
                        width: parent.width
                        spacing: Theme.spacingS

                        Row {
                            spacing: Theme.spacingS
                            TabBar {
                                id: tabs
                                objectName: "toolLibraryTabs"
                                TabButton {
                                    objectName: "toolLibraryTab.tools"
                                    text: qsTr("Tools")
                                }
                                TabButton {
                                    objectName: "toolLibraryTab.holders"
                                    text: qsTr("Holders")
                                }
                            }
                            Button {
                                id: newButton
                                objectName: "toolLibraryNew"
                                iconName: "plus"
                                text: popup.holders ? qsTr("New holder") : qsTr("New tool…")
                                toolTipText: popup.holders ? qsTr("A new holder in my library")
                                                           : qsTr("A new tool in my library: choose its type")
                                enabled: CamTools.myLibrary !== ""
                                onClicked: {
                                    if (popup.holders) {
                                        CamTools.newItem("holder")
                                        form.focusFirst()
                                    } else {
                                        newMenuLoader.active = true
                                        newMenuLoader.item.popup(newButton, 0, newButton.height + Theme.spacingXs)
                                    }
                                }
                                Loader {
                                    id: newMenuLoader
                                    active: false
                                    sourceComponent: Menu {
                                        objectName: "toolLibraryNewMenu"
                                        Repeater {
                                            model: CamTools.toolTypes
                                            delegate: MenuItem {
                                                required property var modelData
                                                objectName: "toolLibraryNew." + modelData.value
                                                text: modelData.text
                                                onTriggered: {
                                                    CamTools.newItem(modelData.value)
                                                    form.focusFirst()
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        TextField {
                            id: search
                            objectName: "toolLibrarySearch"
                            width: parent.width
                            iconName: "search"
                            placeholderText: popup.holders ? qsTr("Search holders") : qsTr("Search name, maker, type, thread…")
                            Accessible.name: qsTr("Search")
                            Keys.onDownPressed: {
                                listView.forceActiveFocus()
                                popup.selectRow(Math.max(0, listView.currentIndex))
                            }
                        }
                        Row {
                            spacing: Theme.spacingS
                            visible: !popup.holders
                            ComboBox {
                                id: typeFilter
                                objectName: "toolLibraryTypeFilter"
                                width: (filters.width - Theme.spacingS) / 2
                                textRole: "text"
                                valueRole: "value"
                                model: [{ value: "", text: qsTr("Any type") }].concat(CamTools.toolTypes)
                                Accessible.name: qsTr("Tool type")
                            }
                            ComboBox {
                                id: materialFilter
                                objectName: "toolLibraryMaterialFilter"
                                width: (filters.width - Theme.spacingS) / 2
                                textRole: "text"
                                valueRole: "value"
                                model: [{ value: "", text: qsTr("Any material") }].concat(popup.materialChoices)
                                Accessible.name: qsTr("Has cutting data for")
                            }
                        }
                        Row {
                            spacing: Theme.spacingS
                            Label {
                                height: Theme.controlHeight
                                verticalAlignment: Text.AlignVCenter
                                secondary: true
                                text: qsTr("Ø")
                            }
                            TextField {
                                id: minField
                                objectName: "toolLibraryMinDiameter"
                                width: Theme.controlHeight * 3.6
                                placeholderText: qsTr("from")
                                invalid: toolList.diameterProblem !== "" && text.trim() !== ""
                                         && toolList.diameterProblem.indexOf(text.trim()) >= 0
                                Accessible.name: qsTr("Smallest diameter")
                            }
                            TextField {
                                id: maxField
                                objectName: "toolLibraryMaxDiameter"
                                width: Theme.controlHeight * 3.6
                                placeholderText: qsTr("to")
                                invalid: toolList.diameterProblem !== "" && text.trim() !== ""
                                         && toolList.diameterProblem.indexOf(text.trim()) >= 0
                                Accessible.name: qsTr("Largest diameter")
                            }
                            ComboBox {
                                id: sortChoice
                                objectName: "toolLibrarySort"
                                width: filters.width - 2 * Theme.controlHeight * 3.6 - 3 * Theme.spacingS - Theme.iconSize
                                textRole: "text"
                                valueRole: "value"
                                model: [
                                    { value: "diameter", text: qsTr("By diameter") },
                                    { value: "name", text: qsTr("By name") },
                                    { value: "type", text: qsTr("By type") }
                                ]
                                Accessible.name: qsTr("Sort")
                            }
                        }
                        Label {
                            objectName: "toolLibraryCount"
                            width: parent.width
                            secondary: true
                            font: Theme.smallFont
                            color: toolList.diameterProblem !== "" ? Theme.error : Theme.textSecondary
                            text: toolList.diameterProblem !== "" ? toolList.diameterProblem
                                  : toolList.count === toolList.total
                                    ? (popup.holders ? qsTr("%n holder(s)", "", toolList.total) : qsTr("%n tool(s)", "", toolList.total))
                                    : qsTr("%1 of %2 shown").arg(toolList.count).arg(toolList.total)
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
                            objectName: "toolLibraryView"
                            anchors.fill: parent
                            anchors.margins: Theme.borderWidth
                            clip: true
                            model: toolList
                            currentIndex: -1
                            activeFocusOnTab: true
                            boundsBehavior: Flickable.StopAtBounds
                            reuseItems: true
                            highlightMoveDuration: 0
                            ScrollBar.vertical: ScrollBar {}
                            Accessible.role: Accessible.List
                            Accessible.name: popup.holders ? qsTr("Holders") : qsTr("Tools")
                            section.property: "group"
                            section.criteria: ViewSection.FullString
                            section.delegate: Item {
                                required property string section
                                width: ListView.view.width
                                height: Theme.rowHeight
                                Label {
                                    anchors.left: parent.left
                                    anchors.leftMargin: Theme.spacingS
                                    anchors.verticalCenter: parent.verticalCenter
                                    font: Theme.strongFont
                                    text: parent.section
                                }
                            }
                            Keys.onUpPressed: event => {
                                if (currentIndex <= 0) {
                                    search.forceActiveFocus()
                                } else {
                                    popup.selectRow(currentIndex - 1)
                                }
                                event.accepted = true
                            }
                            Keys.onDownPressed: event => {
                                popup.selectRow(Math.min(count - 1, currentIndex + 1))
                                event.accepted = true
                            }
                            Keys.onPressed: event => {
                                if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                                    const page = Math.max(1, Math.floor(height / (Theme.rowHeight * 1.6)))
                                    popup.selectRow(Math.max(0, Math.min(count - 1, currentIndex
                                                                         + (event.key === Qt.Key_PageDown ? page : -page))))
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                                    popup.selectRow(event.key === Qt.Key_Home ? 0 : count - 1)
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                    popup.openEditor()
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Delete && !CamTools.readOnly) {
                                    CamTools.askDelete()
                                    event.accepted = true
                                }
                            }

                            delegate: Item {
                                id: row
                                required property int index
                                required property string itemId
                                required property string origin
                                required property string name
                                required property string typeText
                                required property string diameterText
                                required property int flutes
                                required property string rootName
                                required property bool readOnly
                                required property bool libraryChanged
                                readonly property bool selected: itemId === CamTools.selectedId
                                                                 && origin === CamTools.selectedOrigin
                                readonly property bool hovered: mouse.containsMouse
                                objectName: "toolRow." + index
                                width: ListView.view.width
                                height: Theme.rowHeight * 1.6
                                Accessible.role: Accessible.ListItem
                                Accessible.name: name + ", " + typeText + ", " + diameterText + ", " + rootName
                                                 + (readOnly ? ", " + qsTr("read-only") : "")
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
                                    onDoubleClicked: popup.openEditor()
                                }
                                Column {
                                    anchors.left: parent.left
                                    anchors.leftMargin: Theme.spacingM
                                    anchors.right: marks.left
                                    anchors.rightMargin: Theme.spacingS
                                    anchors.verticalCenter: parent.verticalCenter
                                    Label {
                                        objectName: "toolRowName"
                                        width: parent.width
                                        elide: Text.ElideRight
                                        text: row.name
                                    }
                                    Label {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        secondary: true
                                        font: Theme.smallFont
                                        text: row.flutes > 0 && !popup.holders
                                              ? qsTr("%1 · %2 · %n flute(s)", "", row.flutes).arg(row.typeText).arg(row.diameterText)
                                              : qsTr("%1 · %2").arg(row.typeText).arg(row.diameterText)
                                    }
                                }
                                Row {
                                    id: marks
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.spacingS
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingXs
                                    Label {
                                        objectName: "toolRowChanged." + row.index
                                        visible: row.libraryChanged
                                        anchors.verticalCenter: parent.verticalCenter
                                        font: Theme.smallFont
                                        color: Theme.warning
                                        text: qsTr("changed in library")
                                    }
                                    Label {
                                        objectName: "toolRowRoot." + row.index
                                        visible: row.origin !== "document"
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.min(implicitWidth, Theme.controlHeight * 4.5)
                                        elide: Text.ElideRight
                                        secondary: true
                                        font: Theme.smallFont
                                        text: row.rootName
                                    }
                                    Icon {
                                        objectName: "toolRowLock." + row.index
                                        visible: row.readOnly && row.origin !== "document"
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: "lock"
                                        color: Theme.textSecondary
                                    }
                                }
                            }
                        }
                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 2 * Theme.spacingL
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            visible: toolList.count === 0
                            secondary: true
                            text: toolList.total === 0 ? (popup.holders ? qsTr("No holders yet") : qsTr("No tools yet"))
                                                       : qsTr("Nothing matches the filters")
                        }
                    }
                }

                // ---------------- the editor ----------------
                Item {
                    id: right
                    objectName: "toolLibraryEditor"
                    anchors.top: rootsRow.bottom
                    anchors.topMargin: Theme.spacingS
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.spacingM
                    anchors.left: left.right
                    anchors.leftMargin: Theme.spacingM
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingM

                    Label {
                        anchors.centerIn: parent
                        visible: !popup.editing
                        width: parent.width - 2 * Theme.spacingL
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        secondary: true
                        text: popup.holders ? qsTr("Select a holder to see it here, or make a new one.")
                                            : qsTr("Select a tool to see it here, or make a new one.")
                    }

                    Column {
                        id: editorHead
                        visible: popup.editing
                        width: parent.width
                        spacing: Theme.spacingXs

                        Label {
                            objectName: "toolEditorTitle"
                            width: parent.width
                            elide: Text.ElideRight
                            font: Theme.titleFont
                            text: CamTools.title
                        }
                        Row {
                            spacing: Theme.spacingXs
                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: CamTools.readOnly
                                name: "lock"
                                color: Theme.textSecondary
                            }
                            Label {
                                objectName: "toolEditorWhere"
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                text: CamTools.whereText
                            }
                        }
                        Flow {
                            width: parent.width
                            spacing: Theme.spacingS
                            Button {
                                objectName: "toolDuplicate"
                                visible: CamTools.selectedOrigin !== "new"
                                primary: CamTools.readOnly && CamTools.selectedOrigin === "library"
                                iconName: "copy"
                                enabled: CamTools.myLibrary !== "" && !CamTools.saving
                                text: CamTools.readOnly ? qsTr("Duplicate to my library") : qsTr("Duplicate")
                                toolTipText: qsTr("A copy in my library to change (Ctrl+D)")
                                onClicked: {
                                    CamTools.duplicate()
                                    form.focusFirst()
                                }
                            }
                            Button {
                                objectName: "toolUpdateFromLibrary"
                                visible: CamTools.selectedOrigin === "document"
                                enabled: CamTools.canUpdateFromLibrary
                                primary: CamTools.libraryChanged
                                text: qsTr("Update from library")
                                toolTipText: CamTools.canUpdateFromLibrary
                                             ? qsTr("Replace the document's copy with the library's; operations using it are generated again")
                                             : qsTr("The document's copy is the same as the library's")
                                onClicked: CamTools.updateFromLibrary()
                            }
                            Button {
                                objectName: "toolDelete"
                                visible: !CamTools.readOnly
                                flat: true
                                iconName: "trash"
                                enabled: !CamTools.saving
                                text: CamTools.selectedOrigin === "new" ? qsTr("Discard") : qsTr("Delete")
                                onClicked: CamTools.askDelete()
                            }
                        }
                        // The document's copy whose library original has another shape now.
                        Label {
                            objectName: "toolLibraryChangedNote"
                            visible: CamTools.libraryChanged
                            width: parent.width
                            wrapMode: Text.Wrap
                            color: Theme.warning
                            text: qsTr("The library's version of this tool has another shape now. The document keeps its own copy until you update it.")
                        }
                    }

                    Item {
                        id: editorBody
                        visible: popup.editing
                        anchors.top: editorHead.bottom
                        anchors.topMargin: Theme.spacingS
                        anchors.bottom: footer.top
                        anchors.bottomMargin: Theme.spacingS
                        width: parent.width

                        // The side view, beside the fields.
                        Rectangle {
                            id: previewFrame
                            width: Math.min(parent.width * 0.34, Theme.controlHeight * 9)
                            height: parent.height
                            radius: Theme.radiusSmall
                            color: Theme.fieldBg
                            border.width: Theme.borderWidth
                            border.color: Theme.panelBorder
                            ToolProfile {
                                id: preview
                                anchors.top: parent.top
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: holderChoice.top
                                anchors.margins: Theme.spacingXs
                                profile: CamTools.profile
                            }
                            ComboBox {
                                id: holderChoice
                                objectName: "toolPreviewHolder"
                                visible: CamTools.editorKind === "tool"
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.margins: Theme.spacingS
                                height: visible ? implicitHeight : 0
                                textRole: "text"
                                valueRole: "value"
                                model: {
                                    const out = [{ value: "", text: qsTr("Without a holder") }]
                                    for (const h of CamTools.holders)
                                        out.push({ value: h.id, text: h.name })
                                    return out
                                }
                                Accessible.name: qsTr("Show in holder")
                                function sync() {
                                    const i = indexOfValue(CamTools.previewHolder)
                                    if (i !== currentIndex)
                                        currentIndex = i
                                }
                                onModelChanged: Qt.callLater(sync)
                                Component.onCompleted: Qt.callLater(sync)
                                Connections {
                                    target: CamTools
                                    function onProfileChanged() {
                                        holderChoice.sync()
                                    }
                                }
                                onActivated: CamTools.previewHolder = currentValue
                            }
                        }

                        Flickable {
                            id: fieldsScroll
                            objectName: "toolFieldsScroll"
                            anchors.left: previewFrame.right
                            anchors.leftMargin: Theme.spacingM
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            clip: true
                            contentHeight: fieldsColumn.implicitHeight
                            boundsBehavior: Flickable.StopAtBounds
                            interactive: contentHeight > height
                            ScrollBar.vertical: ScrollBar {}

                            Column {
                                id: fieldsColumn
                                width: fieldsScroll.width - Theme.spacingM
                                spacing: Theme.spacingS

                                ToolFields {
                                    id: form
                                    objectName: "toolForm"
                                    fields: CamTools.fields
                                    values: CamTools.values
                                    problems: CamTools.problems
                                    choices: popup.choices
                                    readOnly: CamTools.readOnly
                                    labelWidth: Math.min(Theme.controlHeight * 4.6, fieldsColumn.width * 0.38)
                                    fieldWidth: fieldsColumn.width - labelWidth - Theme.spacingS
                                    onEdited: (key, text) => CamTools.setValue(key, text)
                                    onCommitRequested: CamTools.save()
                                }
                                // A problem that names no field.
                                Label {
                                    objectName: "toolProblem.general"
                                    visible: CamTools.problems[""] !== undefined
                                    width: parent.width
                                    wrapMode: Text.Wrap
                                    color: Theme.error
                                    text: CamTools.problems[""] !== undefined ? CamTools.problems[""] : ""
                                }

                                // Presets (tools) or segments (holders).
                                Row {
                                    width: parent.width
                                    spacing: Theme.spacingS
                                    Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        font: Theme.strongFont
                                        text: CamTools.editorKind === "holder" ? qsTr("Segments, from the gauge line down")
                                                                              : qsTr("Cutting data per material")
                                    }
                                    ToolButton {
                                        objectName: "toolAddRow"
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: !CamTools.readOnly
                                        compact: true
                                        iconName: "plus"
                                        toolTipText: CamTools.editorKind === "holder" ? qsTr("Add a segment")
                                                                                     : qsTr("Add cutting data for a material")
                                        onClicked: {
                                            CamTools.addRow()
                                            popup.openRow(CamTools.rows.length - 1)
                                        }
                                    }
                                }
                                Label {
                                    visible: CamTools.rows.length === 0
                                    secondary: true
                                    text: CamTools.editorKind === "holder" ? qsTr("No segments") : qsTr("No cutting data yet")
                                }
                                // By count, so a card (and the field being typed in) stays while its values change.
                                Repeater {
                                    model: CamTools.rows.length
                                    delegate: Rectangle {
                                        id: card
                                        required property int index
                                        readonly property var rowValues: CamTools.rows[index] !== undefined ? CamTools.rows[index] : ({})
                                        objectName: "toolRowCard." + index
                                        width: fieldsColumn.width
                                        height: cardColumn.implicitHeight + 2 * Theme.spacingS
                                        radius: Theme.radiusSmall
                                        color: "transparent"
                                        border.width: Theme.borderWidth
                                        border.color: Theme.panelBorder
                                        readonly property string rowKey: (CamTools.editorKind === "holder" ? "segments." : "presets.") + index
                                        // Its fields are made only while it is open (a shipped tool has two dozen presets): a
                                        // row opened by hand (a new or duplicated row opens itself) or one with a problem.
                                        readonly property bool open: popup.openRows[index] === true
                                                                     || Object.keys(CamTools.problems).some(k => k === rowKey || k.startsWith(rowKey + "."))
                                        Column {
                                            id: cardColumn
                                            x: Theme.spacingS
                                            y: Theme.spacingS
                                            width: parent.width - 2 * Theme.spacingS
                                            spacing: Theme.spacingXs
                                            Row {
                                                width: parent.width
                                                spacing: Theme.spacingXs
                                                ToolButton {
                                                    id: toggleRow
                                                    objectName: "toolRowToggle." + card.index
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    compact: true
                                                    iconName: card.open ? "chevron-down" : "chevron-right"
                                                    toolTipText: card.open ? qsTr("Close") : qsTr("Open")
                                                    onClicked: popup.toggleRow(card.index)
                                                }
                                                Label {
                                                    objectName: "toolRowSummary." + card.index
                                                    width: parent.width - toggleRow.width - Theme.spacingXs
                                                           - (dupRow.visible ? dupRow.width + delRow.width + 2 * Theme.spacingXs : 0)
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    elide: Text.ElideRight
                                                    secondary: !card.open
                                                    text: popup.rowSummary(card.rowValues)
                                                }
                                                ToolButton {
                                                    id: dupRow
                                                    objectName: "toolDuplicateRow." + card.index
                                                    visible: !CamTools.readOnly
                                                    compact: true
                                                    iconName: "copy"
                                                    toolTipText: qsTr("Duplicate")
                                                    onClicked: {
                                                        CamTools.duplicateRow(card.index)
                                                        popup.openRow(card.index + 1)
                                                    }
                                                }
                                                ToolButton {
                                                    id: delRow
                                                    objectName: "toolRemoveRow." + card.index
                                                    visible: !CamTools.readOnly
                                                    compact: true
                                                    iconName: "trash"
                                                    toolTipText: qsTr("Remove")
                                                    onClicked: {
                                                        CamTools.removeRow(card.index)
                                                        popup.openRows = ({})
                                                    }
                                                }
                                            }
                                            // A problem of the row as a whole (two presets for one material and cut).
                                            Label {
                                                readonly property string key: (CamTools.editorKind === "holder" ? "segments." : "presets.") + card.index
                                                objectName: "toolProblem." + key
                                                visible: CamTools.problems[key] !== undefined
                                                width: parent.width
                                                wrapMode: Text.Wrap
                                                font: Theme.smallFont
                                                color: Theme.error
                                                text: visible ? CamTools.problems[key] : ""
                                            }
                                            Loader {
                                                active: card.open
                                                visible: active
                                                width: parent.width
                                                sourceComponent: ToolFields {
                                                    fields: CamTools.rowFields
                                                    values: card.rowValues
                                                    problems: CamTools.problems
                                                    choices: popup.choices
                                                    prefix: card.rowKey + "."
                                                    readOnly: CamTools.readOnly
                                                    labelWidth: Math.min(Theme.controlHeight * 4.6, cardColumn.width * 0.38)
                                                    fieldWidth: cardColumn.width - labelWidth - Theme.spacingS
                                                    onEdited: (key, text) => CamTools.setRowValue(card.index, key, text)
                                                    onCommitRequested: CamTools.save()
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ---- status, conflict, delete question, buttons ----
                    Column {
                        id: footer
                        visible: popup.editing || CamTools.status !== ""
                        anchors.bottom: parent.bottom
                        width: parent.width
                        spacing: Theme.spacingS

                        Rectangle {
                            id: conflictBox
                            objectName: "toolConflict"
                            readonly property var c: CamTools.conflict
                            visible: c.kind !== undefined
                            width: parent.width
                            height: visible ? conflictColumn.implicitHeight + 2 * Theme.spacingS : 0
                            radius: Theme.radiusSmall
                            color: "transparent"
                            border.width: Theme.focusRingWidth
                            border.color: Theme.error
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: c.title !== undefined ? c.title + ". " + c.message : ""
                            Column {
                                id: conflictColumn
                                x: Theme.spacingS
                                y: Theme.spacingS
                                width: parent.width - 2 * Theme.spacingS
                                spacing: Theme.spacingXs
                                Row {
                                    spacing: Theme.spacingXs
                                    Icon {
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: "warning"
                                        color: Theme.error
                                    }
                                    Label {
                                        objectName: "toolConflictTitle"
                                        anchors.verticalCenter: parent.verticalCenter
                                        font: Theme.strongFont
                                        text: conflictBox.c.title !== undefined ? conflictBox.c.title : ""
                                    }
                                }
                                Label {
                                    objectName: "toolConflictMessage"
                                    width: parent.width
                                    wrapMode: Text.Wrap
                                    text: conflictBox.c.message !== undefined ? conflictBox.c.message : ""
                                }
                                Label {
                                    objectName: "toolConflictFile"
                                    visible: conflictBox.c.file !== undefined && conflictBox.c.file !== ""
                                    width: parent.width
                                    wrapMode: Text.WrapAnywhere
                                    secondary: true
                                    font: Theme.smallFont
                                    text: conflictBox.c.file !== undefined ? qsTr("File: %1").arg(conflictBox.c.file) : ""
                                }
                                Label {
                                    objectName: "toolConflictChoices"
                                    width: parent.width
                                    wrapMode: Text.Wrap
                                    secondary: true
                                    font: Theme.smallFont
                                    text: conflictBox.c.kind === "changed"
                                          ? (conflictBox.c.action === "delete"
                                             ? qsTr("Retry deletes their version. Discard keeps it and shows it.")
                                             : qsTr("Retry writes your version over theirs. A copy keeps both. Discard drops your changes and shows theirs."))
                                          : qsTr("Retry when the file is free. A copy goes to my library instead. Discard drops your changes.")
                                }
                                Flow {
                                    width: parent.width
                                    spacing: Theme.spacingS
                                    Button {
                                        objectName: "toolConflictRetry"
                                        primary: true
                                        text: qsTr("Retry")
                                        onClicked: CamTools.resolveConflict("retry")
                                    }
                                    Button {
                                        objectName: "toolConflictCopy"
                                        visible: conflictBox.c.canCopy === true
                                        text: qsTr("Save a copy to my library")
                                        onClicked: CamTools.resolveConflict("copy")
                                    }
                                    Button {
                                        objectName: "toolConflictDiscard"
                                        text: conflictBox.c.action === "delete" ? qsTr("Keep it") : qsTr("Discard my changes")
                                        onClicked: CamTools.resolveConflict("discard")
                                    }
                                }
                            }
                        }

                        Row {
                            objectName: "toolDeleteQuestion"
                            visible: CamTools.deleteAsked
                            spacing: Theme.spacingS
                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Delete %1 from %2? Its file is removed.").arg(CamTools.title).arg(CamTools.whereText)
                            }
                            Button {
                                objectName: "toolDeleteConfirm"
                                text: qsTr("Delete")
                                onClicked: CamTools.confirmDelete()
                            }
                            Button {
                                objectName: "toolDeleteCancel"
                                flat: true
                                text: qsTr("Keep")
                                onClicked: CamTools.cancelDelete()
                            }
                        }

                        Item {
                            width: parent.width
                            height: Math.max(statusLabel.implicitHeight, buttons.height)
                            Label {
                                id: statusLabel
                                objectName: "toolStatus"
                                anchors.left: parent.left
                                anchors.right: buttons.left
                                anchors.rightMargin: Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                wrapMode: Text.Wrap
                                color: CamTools.statusIsError ? Theme.error : Theme.textSecondary
                                text: CamTools.saving ? qsTr("Saving…")
                                      : CamTools.status !== "" ? CamTools.status
                                      : CamTools.dirty && Object.keys(CamTools.problems).length > 0
                                        ? qsTr("Correct the fields marked in red to save")
                                      : CamTools.dirty ? qsTr("Not saved yet") : ""
                            }
                            Row {
                                id: buttons
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.spacingS
                                visible: popup.editing
                                Button {
                                    objectName: "toolRevert"
                                    visible: !CamTools.readOnly
                                    text: qsTr("Revert")
                                    enabled: CamTools.dirty && !CamTools.saving
                                    toolTipText: qsTr("Back to what was saved (Esc)")
                                    onClicked: CamTools.revert()
                                }
                                Button {
                                    objectName: "toolSave"
                                    visible: !CamTools.readOnly
                                    primary: !CamTools.choosing
                                    text: qsTr("Save")
                                    enabled: CamTools.canSave
                                    toolTipText: qsTr("Save to %1 (Ctrl+S)").arg(CamTools.whereText)
                                    onClicked: CamTools.save()
                                }
                                // Opened from a tool picker: the selected tool goes back to it.
                                Button {
                                    objectName: "toolUse"
                                    visible: CamTools.choosing && CamTools.editorKind === "tool"
                                    primary: true
                                    text: qsTr("Use this tool")
                                    enabled: !CamTools.dirty && (CamTools.selectedOrigin === "library"
                                                                 || CamTools.selectedOrigin === "document")
                                    toolTipText: CamTools.dirty ? qsTr("Save or revert the changes first")
                                                                : qsTr("Choose %1 for the operation and close the library").arg(CamTools.title)
                                    onClicked: CamTools.useSelected()
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
