// Keyboard shortcut settings (task T-021): every command with its current key, searchable by name, category or key.
// Click a key to record a new one: press the combination (Esc cancels, Backspace or Delete removes the shortcut).
// A key that another command already uses is not taken silently: the conflict bar names that command and offers
// to move the key over (the other command is left without one). Reset puts one command back to its default, Reset
// all every command. Changes apply at once and are saved per user (Shortcuts.filePath, shortcuts.h).
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

Item {
    id: root

    readonly property alias popup: popup
    readonly property alias filter: filter
    readonly property alias list: list
    property string recordingId: ""
    // { id, sequence, message } while a conflict waits for an answer, else null.
    property var conflict: null
    property string status: ""
    readonly property var rows: {
        Shortcuts.revision
        return Shortcuts.rows(filter.text)
    }

    signal closed()

    function open() {
        status = ""
        conflict = null
        popup.open()
        filter.forceActiveFocus()
    }

    function startRecording(commandId) {
        conflict = null
        status = ""
        recordingId = commandId
        Shortcuts.recording = true
        recorder.forceActiveFocus()
    }

    function stopRecording() {
        recordingId = ""
        Shortcuts.recording = false
    }

    function record(commandId, sequence) {
        const result = Shortcuts.assign(commandId, sequence, false)
        if (!result.ok && result.conflictId)
            conflict = { id: commandId, sequence: sequence, message: result.message }
        else
            status = result.message || ""
    }

    function replaceConflict() {
        if (!conflict)
            return
        const result = Shortcuts.assign(conflict.id, conflict.sequence, true)
        status = result.message || ""
        conflict = null
        filter.forceActiveFocus()
    }

    function resetOne(commandId) {
        const result = Shortcuts.reset(commandId, false)
        if (!result.ok && result.conflictId)
            conflict = { id: commandId, sequence: result.sequence, message: result.message }
    }

    T.Popup {
        id: popup
        objectName: "shortcutSettings"
        parent: T.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.spacingL, Theme.controlHeight * 24)
        height: Math.min(parent.height - 2 * Theme.spacingL, Theme.controlHeight * 20)
        padding: 0
        modal: true
        dim: false
        focus: true
        // Esc is handled by the content: it cancels a recording or a conflict before it closes the panel.
        closePolicy: T.Popup.CloseOnPressOutside
        onClosed: {
            root.stopRecording()
            root.conflict = null
            root.closed()
        }

        background: PanelBase {
            objectName: "shortcutSettingsBackground"
        }

        contentItem: FocusScope {
            Keys.onEscapePressed: {
                if (root.conflict)
                    root.conflict = null
                else
                    popup.close()
            }

            PanelHeader {
                id: header
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                title: qsTr("Keyboard shortcuts")
                iconName: "sliders"

                ToolButton {
                    objectName: "shortcutSettingsClose"
                    compact: true
                    iconName: "close"
                    toolTipText: qsTr("Close")
                    onClicked: popup.close()
                }
            }

            TextField {
                id: filter
                objectName: "shortcutFilter"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: header.bottom
                anchors.margins: Theme.spacingS
                focus: true
                iconName: "search"
                placeholderText: qsTr("Search commands or keys")
                Accessible.name: qsTr("Search shortcuts")
            }

            ListView {
                id: list
                objectName: "shortcutList"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: filter.bottom
                anchors.bottom: footer.top
                anchors.topMargin: Theme.spacingXs
                anchors.leftMargin: Theme.spacingXs
                anchors.rightMargin: Theme.spacingXs
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.rows
                T.ScrollBar.vertical: ScrollBar {}
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Shortcuts")

                section.property: "category"
                section.delegate: Item {
                    required property string section
                    width: ListView.view.width
                    height: sectionLabel.implicitHeight + Theme.spacingS
                    Label {
                        id: sectionLabel
                        x: Theme.spacingS
                        anchors.bottom: parent.bottom
                        secondary: true
                        font: Theme.smallFont
                        text: parent.section
                    }
                }

                delegate: Item {
                    id: row
                    required property var modelData
                    readonly property bool recording: root.recordingId === modelData.id
                    readonly property bool conflicted: root.conflict !== null && root.conflict.id === modelData.id

                    objectName: "shortcutRow." + modelData.id
                    width: ListView.view.width
                    height: Theme.rowHeight + Theme.spacingXs

                    Rectangle {
                        anchors.fill: parent
                        anchors.topMargin: Theme.spacingXs / 2
                        anchors.bottomMargin: Theme.spacingXs / 2
                        radius: Theme.radiusSmall
                        color: row.recording || row.conflicted ? Theme.controlBgHover
                               : rowHover.hovered ? Theme.controlBgHover : "transparent"
                    }
                    HoverHandler {
                        id: rowHover
                    }

                    Label {
                        anchors.left: parent.left
                        anchors.right: resetButton.left
                        anchors.leftMargin: Theme.spacingS
                        anchors.rightMargin: Theme.spacingS
                        anchors.verticalCenter: parent.verticalCenter
                        // Where the key works when not everywhere: "Model" or "Sketch" (a key may serve one of each).
                        text: row.modelData.context ? qsTr("%1  (%2)").arg(row.modelData.title).arg(row.modelData.context)
                                                    : row.modelData.title
                    }

                    Button {
                        id: resetButton
                        objectName: "shortcutReset." + row.modelData.id
                        anchors.right: keyButton.left
                        anchors.rightMargin: Theme.spacingXs
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !row.modelData.isDefault
                        flat: true
                        text: qsTr("Reset")
                        toolTipText: row.modelData.defaultShortcut !== ""
                                     ? qsTr("Back to %1").arg(row.modelData.defaultShortcut)
                                     : qsTr("Back to no shortcut")
                        onClicked: root.resetOne(row.modelData.id)
                    }

                    Button {
                        id: keyButton
                        objectName: "shortcutKey." + row.modelData.id
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.spacingS
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(implicitWidth, Theme.controlHeight * 4)
                        font: row.recording || row.modelData.shortcut === "" ? Theme.uiFont : Theme.monoFont
                        checked: row.recording
                        text: row.recording ? qsTr("Press keys…")
                              : row.modelData.shortcut !== "" ? row.modelData.shortcut : qsTr("None")
                        toolTipText: qsTr("Click, then press the new shortcut for %1").arg(row.modelData.title)
                        Accessible.name: qsTr("Shortcut of %1: %2").arg(row.modelData.title).arg(text)
                        onClicked: root.startRecording(row.modelData.id)
                    }
                }
            }

            // Takes the keys while a shortcut is recorded; nothing else sees them (not even Qt's own shortcuts).
            Item {
                id: recorder
                objectName: "shortcutRecorder"
                width: 0
                height: 0
                Keys.onShortcutOverride: event => event.accepted = true
                Keys.onPressed: event => {
                    event.accepted = true
                    if (root.recordingId === "")
                        return
                    const plain = (event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier
                                                      | Qt.MetaModifier)) === 0
                    const commandId = root.recordingId
                    if (plain && event.key === Qt.Key_Escape) {
                        root.stopRecording()
                    } else if (plain && (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)) {
                        root.stopRecording()
                        root.record(commandId, "")
                    } else {
                        const sequence = Shortcuts.sequenceFromKey(event.key, event.modifiers)
                        if (sequence === "")
                            return // a modifier on its own: wait for the key
                        root.stopRecording()
                        root.record(commandId, sequence)
                    }
                    filter.forceActiveFocus()
                }
                onActiveFocusChanged: if (!activeFocus && root.recordingId !== "") root.stopRecording()
            }

            Column {
                id: footer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.spacingS
                spacing: Theme.spacingS

                Rectangle {
                    objectName: "shortcutConflict"
                    visible: root.conflict !== null
                    width: parent.width
                    height: conflictRow.implicitHeight + 2 * Theme.spacingXs
                    radius: Theme.radiusSmall
                    color: Theme.controlBg
                    border.width: Theme.borderWidth
                    border.color: Theme.warning

                    Row {
                        id: conflictRow
                        anchors.fill: parent
                        anchors.margins: Theme.spacingXs
                        anchors.leftMargin: Theme.spacingS
                        spacing: Theme.spacingS

                        Label {
                            objectName: "shortcutConflictText"
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - replaceButton.width - keepButton.width - 2 * parent.spacing
                            wrapMode: Text.Wrap
                            text: root.conflict ? qsTr("%1. Use it for this command instead?").arg(root.conflict.message)
                                                : ""
                        }
                        Button {
                            id: replaceButton
                            objectName: "conflictReplace"
                            anchors.verticalCenter: parent.verticalCenter
                            primary: true
                            text: qsTr("Replace")
                            onClicked: root.replaceConflict()
                        }
                        Button {
                            id: keepButton
                            objectName: "conflictKeep"
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Keep")
                            onClicked: {
                                root.conflict = null
                                filter.forceActiveFocus()
                            }
                        }
                    }
                }

                Item {
                    width: parent.width
                    height: resetAllButton.implicitHeight

                    Label {
                        objectName: "shortcutHint"
                        anchors.left: parent.left
                        anchors.right: resetAllButton.left
                        anchors.rightMargin: Theme.spacingS
                        anchors.verticalCenter: parent.verticalCenter
                        secondary: root.status === ""
                        color: root.status !== "" ? Theme.warning : Theme.textSecondary
                        font: Theme.smallFont
                        wrapMode: Text.Wrap
                        text: root.status !== "" ? root.status
                              : root.recordingId !== "" ? qsTr("Press the new shortcut. Esc cancels; Backspace removes it.")
                              : qsTr("Shortcuts do not run while you type in a text field.")
                    }
                    Button {
                        id: resetAllButton
                        objectName: "shortcutResetAll"
                        anchors.right: parent.right
                        text: qsTr("Reset all")
                        onClicked: {
                            Shortcuts.resetAll()
                            root.conflict = null
                        }
                    }
                }
            }
        }
    }
}
