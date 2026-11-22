// The CAM tree (task T-044; CamTreeModel, cam/setup_tree.h): the left panel's content in the CAM workspace. One row per
// setup, in program order, and under each its operations, in order: name, type and a status badge (up to date, stale,
// generating with its progress, failed or broken reference with the reason under the row and in the warning mark's
// tooltip, suppressed, not generated: toolpaths are not saved, so a document opened anew starts so). Click makes a row current; double-click (or Enter) edits it: a setup opens the setup panel,
// an operation goes to the operation panel (T-045). F2 renames, Delete deletes, the arrows on a hovered row move it,
// the switch suppresses an operation. While there is no setup the tree says how to start.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "camTree"
    // T-045: the operation panel hears the tree's edit requests from its start (CamOperation connects to them).
    readonly property bool operationPanelOpen: CamOperation.active

    Connections {
        target: CamTreeModel
        function onRenameRequested(row) {
            const item = rows.itemAt(row)
            if (item)
                item.startRename()
        }
    }

    // A row's tool button: in its place whether shown or not (`shown` only fades it in and lets it take clicks).
    component RowTool: ToolButton {
        property bool shown: true
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        compact: true
        opacity: shown ? 1 : 0
        enabled: shown
        Accessible.ignored: !shown
    }

    // The badge's colour: a dot beside the status text.
    function badgeColor(status) {
        return status === "upToDate" ? Theme.success
             : status === "stale" || status === "notGenerated" ? Theme.warning
             : status === "generating" ? Theme.accent
             : status === "failed" || status === "broken" ? Theme.error
             : Theme.textDisabled
    }

    Flickable {
        id: flick
        objectName: "camTreeScroll"
        anchors.fill: parent
        anchors.topMargin: Theme.spacingXs
        anchors.bottomMargin: Theme.spacingXs
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentHeight: content.implicitHeight
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: content
            width: flick.width

            TreeRow {
                objectName: "setupsHeader"
                visible: CamTreeModel.setupCount > 0
                width: content.width
                text: qsTr("Setups")
                secondaryText: String(CamTreeModel.setupCount)
                font: Theme.strongFont
                rightPadding: rightInset + Theme.spacingS + newSetup.width + Theme.spacingXs
                ToolButton {
                    id: newSetup
                    objectName: "camTreeNewSetup"
                    anchors.right: parent.right
                    anchors.rightMargin: parent.rightInset + Theme.spacingXs
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "plus"
                    toolTipText: qsTr("New setup") + (Shortcuts.revision >= 0 && Shortcuts.shortcutText("cam.tool.newSetup") !== ""
                                                       ? " (" + Shortcuts.shortcutText("cam.tool.newSetup") + ")" : "")
                    onClicked: Commands.trigger("cam.tool.newSetup")
                }
            }

            FocusScope {
                id: scope
                objectName: "camTreeList"
                visible: CamTreeModel.count > 0
                width: content.width
                height: rowColumn.implicitHeight
                activeFocusOnTab: true
                Accessible.role: Accessible.Tree
                Accessible.name: qsTr("Setups and operations")

                Keys.onPressed: event => {
                    const cur = CamTreeModel.currentIndex
                    if (event.key === Qt.Key_F2 && cur >= 0) {
                        rows.itemAt(cur).startRename()
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && cur >= 0) {
                        CamTreeModel.remove(cur)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Down) {
                        CamTreeModel.currentIndex = Math.min(CamTreeModel.count - 1, cur + 1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up) {
                        CamTreeModel.currentIndex = Math.max(0, cur - 1)
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && cur >= 0) {
                        CamTreeModel.edit(cur)
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Right || event.key === Qt.Key_Left) && cur >= 0) {
                        const r = CamTreeModel.get(cur)
                        if (r.rowKind === "setup" && r.expanded === (event.key === Qt.Key_Left))
                            CamTreeModel.toggleExpanded(cur)
                        event.accepted = true
                    }
                }

                Column {
                    id: rowColumn
                    width: parent.width

                    Repeater {
                        id: rows
                        model: CamTreeModel
                        delegate: Item {
                            id: row
                            required property int index
                            required property string rowKind
                            required property string rowId
                            required property string rowName
                            required property string rowDetail
                            required property string status
                            required property string statusText
                            required property string message
                            required property real progress
                            required property bool suppressed
                            required property int rowDepth
                            required property bool expanded
                            required property bool canMoveUp
                            required property bool canMoveDown

                            readonly property bool isSetup: rowKind === "setup"
                            readonly property bool current: CamTreeModel.currentIndex === index
                            readonly property bool bad: status === "failed" || status === "broken"
                            // Anywhere on the row, its tool buttons included (a passive handler, unlike the
                            // MouseArea's own hover, which a button under the pointer takes).
                            readonly property bool hovered: rowHover.hovered
                            property bool renaming: false

                            function startRename() {
                                renaming = true
                                nameField.text = rowName
                                nameField.forceActiveFocus()
                                nameField.selectAll()
                            }
                            function endRename() {
                                renaming = false
                                nameField.focus = false
                            }

                            objectName: (isSetup ? "setupRow." : "operationRow.") + index
                            width: rowColumn.width
                            implicitHeight: main.height + (details.visible ? details.implicitHeight + Theme.spacingXs : 0)
                            height: implicitHeight
                            Accessible.role: Accessible.TreeItem
                            Accessible.name: rowName + (statusText !== "" ? ", " + statusText : "")
                            Accessible.selected: current

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacingXs
                                anchors.rightMargin: Theme.spacingXs
                                radius: Theme.radiusSmall
                                color: row.current ? Theme.selectionFill : row.hovered ? Theme.controlBgHover : "transparent"
                                Rectangle {
                                    visible: row.current
                                    width: Theme.focusRingWidth
                                    height: parent.height - 2 * Theme.spacingXs
                                    radius: width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: Theme.borderWidth
                                    color: Theme.selection
                                }
                            }

                            HoverHandler {
                                id: rowHover
                            }
                            MouseArea {
                                id: mouse
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton
                                onPressed: {
                                    CamTreeModel.currentIndex = row.index
                                    scope.forceActiveFocus()
                                }
                                onDoubleClicked: CamTreeModel.edit(row.index)
                            }

                            Item {
                                id: main
                                width: parent.width
                                height: Theme.rowHeight

                                Icon {
                                    id: chevron
                                    visible: row.isSetup
                                    x: Theme.spacingXs + Theme.spacingXs
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: row.expanded ? "chevron-down" : "chevron-right"
                                    color: Theme.textSecondary
                                    TapHandler {
                                        onTapped: CamTreeModel.toggleExpanded(row.index)
                                    }
                                }
                                Icon {
                                    id: icon
                                    x: Theme.spacingXs + Theme.spacingS + Theme.spacingL * (row.rowDepth + 1)
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: row.isSetup ? "cube" : "sliders"
                                    color: row.bad ? Theme.error : Theme.textSecondary
                                }
                                Text {
                                    objectName: "camRowName"
                                    visible: !row.renaming
                                    anchors.left: icon.right
                                    anchors.leftMargin: Theme.spacingXs
                                    anchors.right: marks.left
                                    anchors.rightMargin: Theme.spacingXs
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: row.rowName
                                    font.family: Theme.uiFont.family
                                    font.pixelSize: Theme.uiFont.pixelSize
                                    font.strikeout: row.suppressed
                                    color: row.suppressed ? Theme.textSecondary : Theme.textPrimary
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                }
                                TextField {
                                    id: nameField
                                    objectName: "camRowRename." + row.index
                                    visible: row.renaming
                                    anchors.left: icon.right
                                    anchors.leftMargin: Theme.spacingXs
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.spacingS
                                    anchors.verticalCenter: parent.verticalCenter
                                    Accessible.name: qsTr("New name of %1").arg(row.rowName)
                                    function accept() {
                                        CamTreeModel.rename(row.index, text)
                                        row.endRename()
                                    }
                                    Keys.onReturnPressed: event => {
                                        accept()
                                        event.accepted = true
                                    }
                                    Keys.onEnterPressed: event => {
                                        accept()
                                        event.accepted = true
                                    }
                                    Keys.onEscapePressed: event => {
                                        row.endRename()
                                        event.accepted = true
                                    }
                                    onActiveFocusChanged: if (!activeFocus && row.renaming) row.endRename()
                                }

                                // The right end of the row: the status badge, and in its place while the pointer is
                                // on the row the row's tools (a new operation and Post for setups, T-045 and T-047;
                                // move up and down; suppress for operations), each at a fixed place from the right
                                // edge, kept while hidden (only its opacity changes). The slot is as wide as the wider
                                // of the two whether or not the pointer is there, so the name never moves or shortens
                                // when the tools appear, and no tool ever comes up under a name the pointer is on.
                                Item {
                                    id: slot
                                    objectName: "camRowSlot." + row.index
                                    anchors.right: parent.right
                                    anchors.rightMargin: Theme.spacingS
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.max(badge.implicitWidth, rowTools.implicitWidth)
                                    height: Math.max(badge.implicitHeight, rowTools.implicitHeight)
                                    visible: !row.renaming

                                    // The badge: a dot in the status colour and the status.
                                    Row {
                                        id: badge
                                        objectName: "camRowBadge." + row.index
                                        visible: row.status !== "" && !row.hovered
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: Theme.spacingXs
                                        Rectangle {
                                            width: Theme.spacingS
                                            height: width
                                            radius: width / 2
                                            anchors.verticalCenter: parent.verticalCenter
                                            color: root.badgeColor(row.status)
                                            Accessible.ignored: true
                                        }
                                        Text {
                                            objectName: "camRowStatus"
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: row.statusText
                                            font: Theme.smallFont
                                            color: row.bad ? Theme.error : Theme.textSecondary
                                            textFormat: Text.PlainText
                                        }
                                    }
                                    Row {
                                        id: rowTools
                                        objectName: "camRowTools." + row.index
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: Theme.spacingXs

                                        RowTool {
                                            objectName: "camRowNewOperation." + row.index
                                            visible: row.isSetup
                                            shown: row.hovered
                                            iconName: "plus"
                                            toolTipText: qsTr("New operation in this setup (Shift+O)")
                                            onClicked: CamOperation.startFor(row.rowId)
                                        }
                                        RowTool {
                                            objectName: "camRowPost." + row.index
                                            visible: row.isSetup
                                            shown: row.hovered
                                            iconName: "post"
                                            toolTipText: qsTr("Post this setup (Ctrl+P)")
                                            onClicked: CamPost.open(row.rowId)
                                        }
                                        RowTool {
                                            objectName: "camRowUp." + row.index
                                            shown: row.hovered && row.canMoveUp
                                            iconName: "chevron-up"
                                            toolTipText: qsTr("Move up")
                                            onClicked: CamTreeModel.moveUp(row.index)
                                        }
                                        RowTool {
                                            objectName: "camRowDown." + row.index
                                            shown: row.hovered && row.canMoveDown
                                            iconName: "chevron-down"
                                            toolTipText: qsTr("Move down")
                                            onClicked: CamTreeModel.moveDown(row.index)
                                        }
                                        RowTool {
                                            objectName: "camRowSuppress." + row.index
                                            visible: !row.isSetup
                                            shown: row.hovered
                                            iconName: "suppress"
                                            checked: row.suppressed
                                            toolTipText: row.suppressed ? qsTr("Unsuppress") : qsTr("Suppress (switch off)")
                                            onClicked: CamTreeModel.setSuppressed(row.index, !row.suppressed)
                                        }
                                    }
                                }

                                // The warning mark, left of the badge's place: its tooltip says why the row failed.
                                Row {
                                    id: marks
                                    anchors.right: slot.left
                                    anchors.rightMargin: Theme.spacingXs
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingXs
                                    visible: !row.renaming

                                    Item {
                                        id: warning
                                        objectName: "camRowWarning." + row.index
                                        visible: row.bad
                                        width: Theme.iconSize
                                        height: Theme.iconSize
                                        anchors.verticalCenter: parent.verticalCenter
                                        readonly property bool hovered: warnHover.hovered
                                        readonly property bool pressed: false
                                        Icon {
                                            anchors.fill: parent
                                            name: "warning"
                                            color: Theme.error
                                        }
                                        HoverHandler {
                                            id: warnHover
                                        }
                                        HoverToolTip {
                                            target: warning
                                            text: row.message !== "" ? row.message : row.statusText
                                        }
                                    }
                                }
                            }

                            // What the setup is on, or why the row failed.
                            Label {
                                id: details
                                objectName: "camRowDetails." + row.index
                                visible: row.bad ? row.message !== "" : row.isSetup && row.rowDetail !== ""
                                anchors.top: main.bottom
                                x: icon.x
                                width: row.width - x - Theme.spacingS
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                maximumLineCount: 4
                                font: Theme.smallFont
                                color: row.bad ? Theme.error : Theme.textSecondary
                                text: row.bad ? row.message : row.rowDetail
                            }
                        }
                    }
                }
            }

            // Why a tree action was refused.
            Item {
                visible: CamTreeModel.message !== ""
                width: content.width
                height: treeMessage.implicitHeight + Theme.spacingS
                Label {
                    id: treeMessage
                    objectName: "camTreeMessage"
                    x: Theme.spacingS
                    width: parent.width - 2 * Theme.spacingS - dismiss.width
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    font: Theme.smallFont
                    color: Theme.error
                    text: CamTreeModel.message
                }
                ToolButton {
                    id: dismiss
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingXs
                    compact: true
                    iconName: "close"
                    toolTipText: qsTr("Dismiss")
                    onClicked: CamTreeModel.dismissMessage()
                }
            }
        }
    }

    // No setup yet: how to start.
    Column {
        objectName: "camTreeEmpty"
        visible: CamTreeModel.setupCount === 0
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.spacingL
        spacing: Theme.spacingM
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            secondary: true
            text: Session.hasDocument
                  ? qsTr("No setups yet. Add one, then click the top face of the part and an origin on the stock.")
                  : qsTr("Open or import a part, then add a setup here.")
        }
        Button {
            objectName: "camTreeEmptyNewSetup"
            anchors.horizontalCenter: parent.horizontalCenter
            visible: Session.hasDocument
            primary: true
            iconName: "plus"
            text: qsTr("New setup")
            onClicked: Commands.trigger("cam.tool.newSetup")
        }
    }
}
