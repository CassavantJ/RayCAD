// The panel of the open direct-edit tool (task T-083; DirectEdit, model/directedit_tool.h) in the shell's
// viewportArea, where the feature panel goes (one of the two is open at a time). Delete faces, Push/Pull and Resize
// hole: the faces picked in the view, the blend-run offer, the distance or diameter (numbers in the display unit,
// numbers with a unit, expressions, parameters), OK and Cancel, and below them the message about the edit: the
// kernel's warning in amber before OK, its refusal in red (OK then stays off and nothing changes), how long a slow
// preview has run. Enter commits (also from the view), Esc cancels wherever the focus is while no job claims it.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "directEditPanelHost"

    // As the feature panel: right of the model tools column, below the selection filter.
    property real leftInset: Theme.controlHeight + 2 * Theme.spacingXs + Theme.panelGap
    property real topInset: Scene.bodyCount > 0 && !SketchTool.active
                            ? Theme.controlHeight + 2 * Theme.spacingXs + Theme.panelGap : 0

    readonly property string tool: DirectEdit.tool
    readonly property var values: DirectEdit.values
    readonly property real labelWidth: Theme.controlHeight * 3.4
    readonly property real fieldWidth: Theme.controlHeight * 6
    readonly property real rowWidth: labelWidth + fieldWidth + Theme.spacingS

    // The tool opens with the focus in its field (Push/Pull, Resize hole) or on the panel (Delete faces), so the
    // keyboard goes on from there: type, Enter.
    Connections {
        target: DirectEdit
        function onActiveChanged() {
            if (!DirectEdit.active)
                return
            Qt.callLater(() => {
                const field = root.tool === "pushPull" ? distanceField.input
                            : root.tool === "resize" ? diameterField.input : null
                if (field) {
                    field.forceActiveFocus()
                    field.selectAll()
                } else {
                    focusScope.forceActiveFocus()
                }
            })
        }
    }

    component FieldRow: Row {
        id: fieldRow
        property string key
        property string label
        property string placeholder: ""
        readonly property alias input: field
        objectName: "directEditField." + key
        spacing: Theme.spacingS
        Label {
            width: root.labelWidth
            anchors.verticalCenter: parent.verticalCenter
            secondary: true
            text: fieldRow.label
        }
        TextField {
            id: field
            objectName: "directEditInput." + fieldRow.key
            width: root.fieldWidth
            text: root.values[fieldRow.key] !== undefined ? String(root.values[fieldRow.key]) : ""
            placeholderText: fieldRow.placeholder
            invalid: DirectEdit.previewStatus === "failed"
            Accessible.name: fieldRow.label
            Accessible.description: DirectEdit.message
            onTextEdited: DirectEdit.setValue(fieldRow.key, text)
            Keys.onReturnPressed: DirectEdit.commit()
            Keys.onEnterPressed: DirectEdit.commit()
            Keys.onEscapePressed: DirectEdit.cancel()
        }
    }

    PanelBase {
        id: panel
        objectName: "directEditPanel"
        visible: DirectEdit.active
        x: root.leftInset
        y: root.topInset
        width: column.implicitWidth + 2 * Theme.spacingM
        height: Math.min(root.height - root.topInset, column.implicitHeight + 2 * Theme.spacingM)
        clip: true

        FocusScope {
            id: focusScope
            objectName: "directEditFocus"
            anchors.fill: parent
            Keys.onReturnPressed: DirectEdit.commit()
            Keys.onEnterPressed: DirectEdit.commit()

            Flickable {
                anchors.fill: parent
                anchors.margins: Theme.spacingM
                contentHeight: column.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height

                Column {
                    id: column
                    spacing: Theme.spacingS

                    Row {
                        spacing: Theme.spacingS
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: root.tool === "delete" ? "deleteFace" : root.tool === "pushPull" ? "pushPull"
                                  : "resizeHole"
                            color: Theme.accentText
                        }
                        Label {
                            objectName: "directEditTitle"
                            anchors.verticalCenter: parent.verticalCenter
                            font: Theme.strongFont
                            text: DirectEdit.title
                        }
                    }

                    // The faces picked.
                    Column {
                        spacing: Theme.spacingXs / 2
                        Row {
                            spacing: Theme.spacingS
                            Label {
                                width: root.labelWidth
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                text: root.tool === "resize" ? qsTr("Hole") : qsTr("Faces")
                            }
                            Label {
                                objectName: "directEditPickText"
                                width: root.fieldWidth - clearPicks.width - Theme.spacingXs
                                anchors.verticalCenter: parent.verticalCenter
                                text: DirectEdit.pickText
                            }
                            ToolButton {
                                id: clearPicks
                                objectName: "directEditPickClear"
                                compact: true
                                iconName: "close"
                                enabled: DirectEdit.pickCount > 0
                                toolTipText: qsTr("Clear the picked faces")
                                onClicked: Selection.clear()
                            }
                        }
                        Label {
                            x: root.labelWidth + Theme.spacingS
                            width: root.fieldWidth
                            wrapMode: Text.Wrap
                            elide: Text.ElideNone
                            secondary: true
                            font: Theme.smallFont
                            text: DirectEdit.pickHint
                        }
                    }

                    // Delete faces on a fillet that joins others: the whole run, in one click.
                    Row {
                        visible: root.tool === "delete" && DirectEdit.blendRunCount > 0
                        spacing: Theme.spacingS
                        Item {
                            width: root.labelWidth
                            height: 1
                        }
                        Button {
                            objectName: "directEditBlendRun"
                            iconName: "fillet3d"
                            text: qsTr("Select the whole blend run (%1 faces)").arg(DirectEdit.blendRunCount)
                            toolTipText: qsTr("This fillet joins other fillets: removing it alone usually fails")
                            onClicked: DirectEdit.selectBlendRun()
                        }
                    }

                    FieldRow {
                        id: distanceField
                        visible: root.tool === "pushPull"
                        key: "distance"
                        label: qsTr("Distance")
                        placeholder: qsTr("e.g. 2 mm, -0.5 in")
                    }
                    Label {
                        visible: root.tool === "pushPull"
                        x: root.labelWidth + Theme.spacingS
                        width: root.fieldWidth
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        secondary: true
                        font: Theme.smallFont
                        text: qsTr("Positive adds material, negative removes it. Drag the arrow to change it; Shift for small steps.")
                    }
                    FieldRow {
                        id: diameterField
                        visible: root.tool === "resize"
                        key: "diameter"
                        label: qsTr("Diameter")
                        placeholder: qsTr("New diameter")
                    }
                    Label {
                        objectName: "directEditHint"
                        visible: root.tool === "resize" && DirectEdit.hint !== ""
                        x: root.labelWidth + Theme.spacingS
                        width: root.fieldWidth
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        secondary: true
                        font: Theme.smallFont
                        text: DirectEdit.hint
                    }

                    Row {
                        spacing: Theme.spacingS
                        layoutDirection: Qt.RightToLeft
                        width: root.rowWidth
                        Button {
                            objectName: "directEditOk"
                            primary: true
                            text: DirectEdit.editing ? qsTr("Apply") : qsTr("OK")
                            enabled: DirectEdit.canCommit
                            toolTipText: qsTr("Make the edit (Enter): one step to undo")
                            onClicked: DirectEdit.commit()
                        }
                        Button {
                            objectName: "directEditCancel"
                            text: qsTr("Cancel")
                            toolTipText: qsTr("Close without changing the part (Esc)")
                            onClicked: DirectEdit.cancel()
                        }
                    }
                    // Below the buttons, so OK and Cancel never move while a preview builds: the kernel's message
                    // (amber: a warning to read before OK; red: the edit was refused and nothing changes), and how
                    // long a slow preview has been running.
                    Label {
                        objectName: "directEditMessage"
                        visible: DirectEdit.message !== ""
                        width: root.rowWidth
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        color: DirectEdit.previewStatus === "warning" ? Theme.warning
                               : DirectEdit.previewStatus === "failed" ? Theme.error
                               : Theme.textSecondary
                        text: DirectEdit.message
                    }
                    Row {
                        objectName: "directEditBusy"
                        visible: DirectEdit.previewStatus === "building"
                        spacing: Theme.spacingS
                        ProgressBar {
                            anchors.verticalCenter: parent.verticalCenter
                            width: Theme.controlHeight * 3
                            indeterminate: true
                            Accessible.name: qsTr("Working out the edit")
                        }
                        Label {
                            objectName: "directEditBusyText"
                            anchors.verticalCenter: parent.verticalCenter
                            secondary: true
                            font: Theme.smallFont
                            text: DirectEdit.busySeconds >= 1
                                  ? qsTr("Working out the edit: %1 s. Esc cancels.").arg(DirectEdit.busySeconds)
                                  : qsTr("Working out the edit…")
                        }
                    }
                }
            }
        }
    }

    // Esc cancels wherever the focus is while no job claims it; Enter makes the edit from the view too (a text field
    // has its own Enter).
    Shortcut {
        objectName: "directEditEsc"
        sequence: "Esc"
        enabled: DirectEdit.active && !Session.jobActive
        onActivated: DirectEdit.cancel()
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: DirectEdit.active && DirectEdit.canCommit
                 && !(root.Window.activeFocusItem instanceof TextInput)
        onActivated: DirectEdit.commit()
    }
}
