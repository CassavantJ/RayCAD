// The constraint tools while sketching (task T-093), in SketchPanel.qml (the shell's view area):
//
//   constraintToolbar  bottom centre: the smart dimension tool (D), the geometric constraints (each enabled only when
//                      the selection fits; its tooltip says what to select), driven / driving for the selected
//                      dimension, and deleting the selected constraint. The same commands as the palette and the keys
//                      (sketch.constrain.*).
//   constraintIssues   above it, while the last solve names conflicting or redundant constraints: the list of them;
//                      a click selects one (in the view too), so it can be deleted or made driven.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "constraintsPanel"

    function shortcut(id, hovered) {
        if (!hovered)
            return ""
        const s = Shortcuts.revision >= 0 ? Shortcuts.shortcutText(id) : ""
        return s !== "" ? " (" + s + ")" : ""
    }

    PanelBase {
        id: toolbar
        objectName: "constraintToolbar"
        visible: SketchTool.active
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width, toolRow.implicitWidth + 2 * Theme.spacingXs)
        height: toolRow.implicitHeight + 2 * Theme.spacingXs
        clip: true

        Row {
            id: toolRow
            anchors.verticalCenter: parent.verticalCenter
            x: Theme.spacingXs
            spacing: Theme.spacingXs / 2

            ToolButton {
                objectName: "constraintTool.dimension"
                iconName: "glyph-dimension"
                focusPolicy: Qt.NoFocus // the keys stay with the sketch (Delete, Esc)
                checked: SketchConstraints.dimensionTool
                toolTipText: qsTr("Smart dimension: length, distance, angle, radius or diameter of what you pick")
                             + root.shortcut("sketch.constrain.dimension", hovered)
                Accessible.checkable: true
                Accessible.checked: checked
                onClicked: SketchConstraints.dimensionTool ? SketchConstraints.setDimensionTool(false)
                                                           : Commands.trigger("sketch.constrain.dimension")
            }
            Rectangle {
                width: Theme.borderWidth
                height: Theme.controlHeightSmall
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.panelBorder
            }
            Repeater {
                model: SketchConstraints.actions
                // A disabled button gets no hover: the wrapper shows why it is disabled.
                delegate: Item {
                    id: wrap
                    required property var modelData
                    readonly property bool hovered: hover.hovered
                    objectName: "constraintTool." + modelData.name
                    width: button.width
                    height: button.height
                    ToolButton {
                        id: button
                        objectName: "constraintButton." + wrap.modelData.name
                        iconName: wrap.modelData.icon
                        enabled: wrap.modelData.enabled
                        focusPolicy: Qt.NoFocus
                        toolTipText: wrap.modelData.title
                        onClicked: Commands.trigger("sketch.constrain." + wrap.modelData.name)
                    }
                    HoverHandler {
                        id: hover
                    }
                    HoverToolTip {
                        target: wrap
                        text: wrap.modelData.title + root.shortcut("sketch.constrain." + wrap.modelData.name, wrap.hovered)
                              + (wrap.modelData.enabled ? "" : ": " + wrap.modelData.reason)
                    }
                }
            }
            Rectangle {
                width: Theme.borderWidth
                height: Theme.controlHeightSmall
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.panelBorder
            }
            ToolButton {
                objectName: "constraintTool.driven"
                iconName: "driven"
                focusPolicy: Qt.NoFocus
                enabled: SketchConstraints.selectedIsDimension
                checked: SketchConstraints.selectedDriven
                toolTipText: SketchConstraints.selectedDriven
                             ? qsTr("Driven dimension (a reference): make it driving again")
                             : qsTr("Make the selected dimension driven (a reference that does not constrain)")
                Accessible.checkable: true
                Accessible.checked: checked
                onClicked: Commands.trigger("sketch.constrain.driven")
            }
            ToolButton {
                objectName: "constraintTool.delete"
                iconName: "delete"
                focusPolicy: Qt.NoFocus
                enabled: SketchConstraints.selected !== ""
                toolTipText: qsTr("Delete the selected constraint or dimension (Delete)")
                onClicked: Commands.trigger("sketch.constrain.delete")
            }
        }
    }

    PanelBase {
        id: issues
        objectName: "constraintIssues"
        visible: SketchTool.active && SketchConstraints.offenders.length > 0
        anchors.bottom: toolbar.top
        anchors.bottomMargin: Theme.spacingS
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 2 * Theme.spacingL,
                        Math.max(issueColumn.implicitWidth, Theme.controlHeight * 8) + 2 * Theme.spacingM)
        height: Math.min(parent.height / 3, issueColumn.implicitHeight + 2 * Theme.spacingS)
        clip: true

        Column {
            id: issueColumn
            x: Theme.spacingM
            y: Theme.spacingS
            width: issues.width - 2 * Theme.spacingM
            spacing: Theme.spacingXs / 2

            Label {
                font: Theme.strongFont
                color: Theme.sketchConflict
                text: SketchTool.status === "conflict" ? qsTr("Conflicting constraints") : qsTr("Redundant constraints")
            }
            Label {
                width: parent.width
                secondary: true
                font: Theme.smallFont
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                text: qsTr("Select one to delete it or make it driven.")
            }
            Repeater {
                model: SketchConstraints.offenders
                delegate: Button {
                    required property var modelData
                    required property int index
                    objectName: "constraintIssue." + index
                    width: issueColumn.width
                    checkable: false
                    focusPolicy: Qt.NoFocus
                    text: modelData.title + " · " + modelData.why
                    primary: modelData.selected
                    onClicked: SketchConstraints.selectConstraint(modelData.cid)
                }
            }
        }
    }
}
