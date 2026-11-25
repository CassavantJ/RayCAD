// The reference review panel (task T-083; the References singleton, model/directedit_review.h): every face, edge and point
// that a model edit split, merged, moved or removed under a feature or a CAM operation, grouped by owner, each in shop
// words with what it means for its owner. Click a row to see it in the view: what it points at now in the
// selection's colour, where it was when picked dashed in amber. Confirm takes a likely match; Re-pick asks for the
// element to use now (Use, or Enter; Esc cancels), one step to undo. When toolpaths are out of date the panel offers to
// regenerate them all. Not modal: the view works under it. An empty list says so.
import QtQuick
import RayCad.Ui

PanelBase {
    id: root
    objectName: "referenceReview"

    // What it would like; the shell may give it less (a small window): the content then follows the width it has.
    readonly property real preferredContentWidth: Theme.controlHeight * 11
    readonly property real contentWidth: Math.max(0, width - 2 * Theme.spacingM)
    signal closeRequested()

    implicitWidth: preferredContentWidth + 2 * Theme.spacingM
    // The title, the list and the footer; the shell caps the height, and then only the list scrolls: the title and
    // the footer (the note, the out-of-date toolpaths with their one action, the last message) stay in view.
    implicitHeight: header.implicitHeight + Theme.spacingS + column.implicitHeight
                    + (footer.implicitHeight > 0 ? Theme.spacingS + footer.implicitHeight : 0) + 2 * Theme.spacingM
    // The least the shell should give it: the title, a row's height of list and the footer.
    readonly property real minimumHeight: header.implicitHeight + 2 * Theme.spacingS + Theme.controlHeight * 1.5
                                          + footer.implicitHeight + 2 * Theme.spacingM

    function stateColor(state) {
        return state === "broken" ? Theme.error : Theme.warning
    }

    Item {
        id: frame
        anchors.fill: parent
        anchors.margins: Theme.spacingM

        Row {
            id: header
            anchors.top: parent.top
            width: root.contentWidth
            spacing: Theme.spacingS
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "warning"
                color: References.count > 0 ? Theme.warning : Theme.textSecondary
            }
            Label {
                objectName: "referenceReviewTitle"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * Theme.spacingS - Theme.iconSize - closeButton.width
                font: Theme.strongFont
                text: References.count > 0 ? qsTr("Reference review: %1 to check").arg(References.count)
                                                : qsTr("Reference review")
            }
            ToolButton {
                id: closeButton
                objectName: "referenceReviewClose"
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "close"
                toolTipText: qsTr("Close (Ctrl+Shift+R opens it again)")
                onClicked: References.hide()
            }
        }

        Flickable {
            id: flick
            objectName: "referenceReviewList"
            anchors.top: header.bottom
            anchors.topMargin: Theme.spacingS
            anchors.bottom: footer.top
            anchors.bottomMargin: footer.implicitHeight > 0 ? Theme.spacingS : 0
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

                Label {
                    objectName: "referenceReviewEmpty"
                    visible: References.count === 0
                    width: parent.width
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    secondary: true
                    text: References.busy ? qsTr("Checking the references…")
                          : qsTr("Nothing to review: every feature and operation finds the faces, edges and points it was given.")
                }

                Repeater {
                    id: rows
                    model: References.rows
                    delegate: Item {
                        id: row
                        required property int index
                        required property var modelData
                        readonly property bool group: modelData.kind === "group"
                        readonly property bool current: References.current === index
                        readonly property bool picking: References.picking && current
                        objectName: (group ? "referenceGroup." : "referenceRow.") + index
                        width: column.width
                        height: group ? groupTitle.implicitHeight + Theme.spacingXs : body.implicitHeight + 2 * Theme.spacingXs
                        Accessible.role: group ? Accessible.StaticText : Accessible.ListItem
                        Accessible.name: group ? modelData.title : modelData.place + ": " + modelData.words
                        Accessible.selected: current

                        Label {
                            id: groupTitle
                            visible: row.group
                            y: Theme.spacingXs
                            width: parent.width
                            font: Theme.strongFont
                            text: row.group ? row.modelData.title : ""
                        }

                        Rectangle {
                            visible: !row.group
                            anchors.fill: parent
                            radius: Theme.radiusSmall
                            color: row.current ? Theme.selectionFill : rowHover.hovered ? Theme.controlBgHover : "transparent"
                        }
                        HoverHandler {
                            id: rowHover
                            enabled: !row.group
                        }
                        TapHandler {
                            enabled: !row.group
                            onTapped: References.current = row.current ? -1 : row.index
                        }

                        Column {
                            id: body
                            visible: !row.group
                            x: Theme.spacingS
                            y: Theme.spacingXs
                            width: parent.width - 2 * Theme.spacingS
                            spacing: Theme.spacingXs / 2

                            Row {
                                width: parent.width
                                spacing: Theme.spacingS
                                Rectangle {
                                    width: Theme.spacingS
                                    height: width
                                    radius: width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: row.group ? "transparent" : root.stateColor(row.modelData.state)
                                    Accessible.ignored: true
                                }
                                Label {
                                    objectName: "referenceState." + row.index
                                    anchors.verticalCenter: parent.verticalCenter
                                    font: Theme.smallFont
                                    color: row.group ? Theme.textSecondary : root.stateColor(row.modelData.state)
                                    text: row.group ? "" : row.modelData.stateLabel
                                }
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - x
                                    secondary: true
                                    font: Theme.smallFont
                                    text: row.group ? "" : row.modelData.place
                                }
                            }
                            Label {
                                objectName: "referenceWords." + row.index
                                width: parent.width
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                text: row.group ? "" : row.modelData.words
                            }
                            Label {
                                width: parent.width
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                secondary: true
                                font: Theme.smallFont
                                text: row.group ? "" : row.modelData.consequence
                            }
                            Label {
                                visible: !row.group && row.current && row.modelData.reason !== ""
                                width: parent.width
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                secondary: true
                                font: Theme.smallFont
                                text: row.group ? "" : qsTr("Why: %1").arg(row.modelData.reason)
                            }
                            // The current row's actions.
                            Row {
                                visible: !row.group && row.current && !row.picking
                                spacing: Theme.spacingS
                                Button {
                                    objectName: "referenceConfirm." + row.index
                                    visible: !row.group && row.modelData.canConfirm === true
                                    primary: true
                                    text: qsTr("Confirm")
                                    toolTipText: qsTr("Use the likely match that is highlighted")
                                    onClicked: References.confirm(row.index)
                                }
                                Button {
                                    objectName: "referenceRepick." + row.index
                                    text: qsTr("Re-pick")
                                    toolTipText: qsTr("Click the element to use now in the view")
                                    onClicked: References.repick(row.index)
                                }
                            }
                            // Re-pick: what to click, then Use.
                            Column {
                                visible: row.picking
                                width: parent.width
                                spacing: Theme.spacingXs
                                Label {
                                    width: parent.width
                                    wrapMode: Text.Wrap
                                    elide: Text.ElideNone
                                    font: Theme.smallFont
                                    text: References.pickHint
                                }
                                Row {
                                    spacing: Theme.spacingS
                                    Button {
                                        objectName: "referenceUse." + row.index
                                        primary: true
                                        enabled: References.pickCount > 0
                                        text: References.pickCount > 1 ? qsTr("Use these %1").arg(References.pickCount)
                                                                            : qsTr("Use")
                                        toolTipText: qsTr("Put the picked element in its place (Enter): one step to undo")
                                        onClicked: References.usePicks()
                                    }
                                    Button {
                                        objectName: "referencePickCancel." + row.index
                                        text: qsTr("Cancel")
                                        onClicked: References.cancelPick()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Column {
            id: footer
            objectName: "referenceReviewFooter"
            anchors.bottom: parent.bottom
            width: root.contentWidth
            spacing: Theme.spacingS

            Label {
                objectName: "referenceViewNote"
                visible: text !== ""
                width: parent.width
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                secondary: true
                font: Theme.smallFont
                text: References.viewNote
            }

            // Toolpaths a change made out of date: one action for them all.
            Rectangle {
                visible: References.staleCount > 0 || References.generatingCount > 0
                width: parent.width
                height: Theme.borderWidth
                color: Theme.panelBorder
            }
            Row {
                objectName: "referenceStale"
                visible: References.staleCount > 0 || References.generatingCount > 0
                width: parent.width
                spacing: Theme.spacingS
                Label {
                    objectName: "referenceStaleText"
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - regenerate.width - Theme.spacingS
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    color: Theme.warning
                    text: References.staleCount > 0
                          ? (References.staleCount === 1 ? qsTr("1 toolpath is out of date.")
                                                              : qsTr("%1 toolpaths are out of date.").arg(References.staleCount))
                          : qsTr("Generating toolpaths…")
                }
                Button {
                    id: regenerate
                    objectName: "referenceRegenerate"
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: References.staleCount > 0
                    iconName: "refresh"
                    text: qsTr("Regenerate all")
                    toolTipText: qsTr("Generate every out-of-date toolpath (Ctrl+Shift+G)")
                    onClicked: References.regenerateAll()
                }
            }

            Label {
                objectName: "referenceMessage"
                visible: References.message !== ""
                width: parent.width
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                font: Theme.smallFont
                color: References.messageIsError ? Theme.error : Theme.textSecondary
                text: References.message
            }
        }
    }

    // Re-pick: Esc leaves it, Enter uses what is picked (while no other panel claims the keys).
    Shortcut {
        sequence: "Esc"
        enabled: References.picking && !Session.jobActive && !DirectEdit.active && !FeatureTool.active
                 && !CamOperation.active && !CamSetup.active
        onActivated: References.cancelPick()
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: References.picking && References.pickCount > 0 && !DirectEdit.active
                 && !(root.Window.activeFocusItem instanceof TextInput)
        onActivated: References.usePicks()
    }
}
