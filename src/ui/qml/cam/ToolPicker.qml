// A self-contained tool picker (task T-046) for whoever needs a tool chosen, such as the operation panel (T-045): a
// search field over a virtualised list of the document's tools and the library's, each with its type, diameter and the
// library it comes from. Click or Enter picks; Up and Down move from the search field.
//
// The button beside the search opens the tool library manager (cam.toolLibrary.open); the list follows what is saved
// there, as it follows the library folders.
//
// API
//   toolType: string       only tools of this type (a file format name such as "drill"); "" any
//   toolTypes: list        only tools of one of these types (an operation type's x-toolTypes); [] any
//   includeDocument: bool  list the open document's tools first (default true)
//   currentId: string      the tool shown as chosen
//   libraryButton: bool    show the button that opens the tool library (default true)
//   signal picked(string toolId, string origin)   origin "document" or "library"
// To use the pick, embed the tool on first use in the same step as the operation edit: pass
// CamTools.libraryArgsFor(toolId, holderId) as the "library" member of cam.operation.add / cam.operation.edit (empty when
// the document has the tool already), or let CamTools.assignTool(operationId, toolId, holderId) run the edit.
pragma ComponentBehavior: Bound

import QtQuick
import RayCad.Ui

FocusScope {
    id: picker
    objectName: "toolPicker"

    property string toolType: ""
    property list<string> toolTypes: []
    property bool includeDocument: true
    property string currentId: ""
    property bool libraryButton: true
    property real visibleRows: 6

    signal picked(string toolId, string origin)

    implicitWidth: Theme.controlHeight * 12
    implicitHeight: search.height + Theme.spacingXs + frame.height

    ToolList {
        id: list
        objectName: "toolPickerList"
        kind: "tools"
        type: picker.toolType
        types: picker.toolTypes
        text: search.text
        includeDocument: picker.includeDocument
    }

    // Empties the search (a panel that opens anew).
    function clearSearch() {
        search.text = ""
        view.currentIndex = -1
        view.positionViewAtBeginning()
    }

    function pick(row) {
        if (row < 0 || row >= list.count)
            return
        const r = list.get(row)
        picker.picked(r.itemId, r.origin)
    }

    TextField {
        id: search
        objectName: "toolPickerSearch"
        width: parent.width - (openLibrary.visible ? openLibrary.width + Theme.spacingXs : 0)
        iconName: "search"
        focus: true
        placeholderText: qsTr("Search tools")
        Accessible.name: qsTr("Search tools")
        Keys.onDownPressed: {
            view.forceActiveFocus()
            view.currentIndex = Math.min(list.count - 1, Math.max(0, view.currentIndex))
        }
        Keys.onReturnPressed: picker.pick(view.currentIndex >= 0 ? view.currentIndex : 0)
        Keys.onEnterPressed: picker.pick(view.currentIndex >= 0 ? view.currentIndex : 0)
    }
    ToolButton {
        id: openLibrary
        objectName: "toolPickerOpenLibrary"
        visible: picker.libraryButton
        anchors.right: parent.right
        anchors.verticalCenter: search.verticalCenter
        iconName: "endmill"
        toolTipText: qsTr("Open the tool library") + (Shortcuts.revision >= 0 && Shortcuts.shortcutText("cam.toolLibrary.open") !== ""
                                                      ? " (" + Shortcuts.shortcutText("cam.toolLibrary.open") + ")" : "")
        Accessible.name: qsTr("Open the tool library")
        onClicked: Commands.trigger("cam.toolLibrary.open")
    }

    Rectangle {
        id: frame
        anchors.top: search.bottom
        anchors.topMargin: Theme.spacingXs
        width: parent.width
        height: Theme.rowHeight * 2 * picker.visibleRows + 2
        radius: Theme.radiusSmall
        color: "transparent"
        border.width: Theme.borderWidth
        border.color: Theme.panelBorder

        ListView {
            id: view
            objectName: "toolPickerView"
            anchors.fill: parent
            anchors.margins: 1
            clip: true
            model: list
            currentIndex: -1
            activeFocusOnTab: true
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            ScrollBar.vertical: ScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Tools")
            Keys.onReturnPressed: picker.pick(currentIndex)
            Keys.onEnterPressed: picker.pick(currentIndex)
            Keys.onUpPressed: event => {
                if (currentIndex <= 0) {
                    search.forceActiveFocus()
                    event.accepted = true
                } else {
                    event.accepted = false
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
                required property string rootName
                required property bool readOnly
                readonly property bool current: ListView.isCurrentItem
                readonly property bool chosen: itemId === picker.currentId
                objectName: "toolPickerRow." + index
                width: ListView.view.width
                height: Theme.rowHeight * 2
                Accessible.role: Accessible.ListItem
                Accessible.name: name + ", " + typeText + ", " + diameterText
                Accessible.selected: chosen

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: Theme.radiusSmall
                    color: row.chosen ? Theme.selectionFill : row.current || mouse.containsMouse ? Theme.controlBgHover
                                                                                             : "transparent"
                }
                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.spacingS
                    anchors.rightMargin: Theme.spacingS
                    anchors.verticalCenter: parent.verticalCenter
                    Label {
                        width: parent.width
                        elide: Text.ElideRight
                        text: row.name
                    }
                    Label {
                        width: parent.width
                        elide: Text.ElideRight
                        secondary: true
                        font: Theme.smallFont
                        text: row.origin === "document"
                              ? qsTr("%1 · %2 · in this document").arg(row.typeText).arg(row.diameterText)
                              : qsTr("%1 · %2 · %3").arg(row.typeText).arg(row.diameterText).arg(row.rootName)
                    }
                }
                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        view.currentIndex = row.index
                        picker.pick(row.index)
                    }
                }
            }
        }
        Label {
            anchors.centerIn: parent
            visible: list.count === 0
            secondary: true
            text: qsTr("No tool matches")
        }
    }
}
