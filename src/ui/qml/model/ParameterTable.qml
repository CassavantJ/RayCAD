// The parameter table (task T-098; Parameters, model/params.h), a section of the model panel: every named parameter
// with its expression and value. Editing a name renames the parameter and every expression that uses it; editing an
// expression changes the value (a plain number gets the display unit for a length, degrees for an angle); the bin
// deletes a parameter, which the document refuses while something uses it (the message names the users). The last
// row adds a parameter. Every change is one undo step.
import QtQuick
import RayCad.Ui

Column {
    id: root
    objectName: "parameterTable"

    property bool expanded: false
    readonly property real nameWidth: Math.round(width * 0.3)
    readonly property real valueWidth: Math.round(width * 0.25)

    Connections {
        target: FeatureTree
        function onParametersRequested() {
            root.expanded = true
            addName.forceActiveFocus()
        }
    }

    TreeRow {
        objectName: "parametersHeader"
        width: root.width
        text: qsTr("Parameters")
        expandable: true
        expanded: root.expanded
        secondaryText: String(Parameters.count)
        font: Theme.strongFont
        onExpandToggled: root.expanded = !root.expanded
        onClicked: root.expanded = !root.expanded
    }

    Column {
        visible: root.expanded
        width: root.width
        spacing: Theme.spacingXs / 2

        Repeater {
            model: Parameters
            delegate: Item {
                id: param
                required property int index
                required property string paramName
                required property string expression
                required property string paramKind
                required property string paramValue
                required property string paramError
                objectName: "paramRow." + index
                width: root.width
                height: Math.max(nameField.implicitHeight, Theme.rowHeight)
                        + (param.paramError !== "" ? errorText.implicitHeight : 0)

                TextField {
                    id: nameField
                    objectName: "paramName." + param.index
                    x: Theme.spacingS
                    width: root.nameWidth - Theme.spacingS
                    text: param.paramName
                    font: Theme.monoFont
                    Accessible.name: qsTr("Name of parameter %1").arg(param.paramName)
                    onAccepted: Parameters.rename(param.index, text)
                    onActiveFocusChanged: if (!activeFocus && text !== param.paramName) Parameters.rename(param.index, text)
                }
                TextField {
                    id: exprField
                    objectName: "paramExpression." + param.index
                    anchors.left: nameField.right
                    anchors.leftMargin: Theme.spacingXs
                    width: root.width - root.nameWidth - root.valueWidth - deleteButton.width - Theme.spacingXs * 3
                    text: param.expression
                    font: Theme.monoFont
                    invalid: param.paramError !== ""
                    Accessible.name: qsTr("Expression of %1").arg(param.paramName)
                    onAccepted: Parameters.setExpression(param.index, text)
                    onActiveFocusChanged: if (!activeFocus && text !== param.expression) Parameters.setExpression(param.index, text)
                }
                Label {
                    objectName: "paramValue." + param.index
                    anchors.left: exprField.right
                    anchors.leftMargin: Theme.spacingXs
                    anchors.verticalCenter: nameField.verticalCenter
                    width: root.valueWidth
                    mono: true
                    secondary: true
                    horizontalAlignment: Text.AlignRight
                    text: param.paramValue
                }
                ToolButton {
                    id: deleteButton
                    objectName: "paramDelete." + param.index
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingXs
                    anchors.verticalCenter: nameField.verticalCenter
                    compact: true
                    iconName: "trash"
                    toolTipText: qsTr("Delete %1").arg(param.paramName)
                    onClicked: Parameters.remove(param.index)
                }
                Label {
                    id: errorText
                    visible: param.paramError !== ""
                    anchors.top: nameField.bottom
                    x: Theme.spacingS
                    width: root.width - 2 * Theme.spacingS
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    font: Theme.smallFont
                    color: Theme.error
                    text: param.paramError
                }
            }
        }

        // A new parameter: name, expression, kind.
        Row {
            objectName: "paramAddRow"
            x: Theme.spacingS
            spacing: Theme.spacingXs
            TextField {
                id: addName
                objectName: "paramAddName"
                width: root.nameWidth - Theme.spacingS
                font: Theme.monoFont
                placeholderText: Parameters.revision >= 0 ? Parameters.nextName() : ""
                Accessible.name: qsTr("Name of the new parameter")
            }
            TextField {
                id: addExpr
                objectName: "paramAddExpression"
                width: root.width - root.nameWidth - kindBox.width - addButton.width - Theme.spacingXs * 4
                font: Theme.monoFont
                placeholderText: "10 mm"
                Accessible.name: qsTr("Expression of the new parameter")
                onAccepted: addButton.clicked()
            }
            ComboBox {
                id: kindBox
                objectName: "paramAddKind"
                width: Theme.controlHeight * 3
                textRole: "text"
                valueRole: "value"
                model: [
                    { value: "length", text: qsTr("Length") },
                    { value: "angle", text: qsTr("Angle") },
                    { value: "count", text: qsTr("Count") },
                    { value: "unitless", text: qsTr("Number") }
                ]
            }
            ToolButton {
                id: addButton
                objectName: "paramAdd"
                compact: true
                iconName: "plus"
                enabled: addExpr.text.trim() !== ""
                toolTipText: qsTr("Add the parameter")
                onClicked: {
                    const name = addName.text.trim() !== "" ? addName.text.trim() : Parameters.nextName()
                    if (Parameters.add(name, addExpr.text, kindBox.currentValue)) {
                        addName.text = ""
                        addExpr.text = ""
                    }
                }
            }
        }

        Label {
            objectName: "paramMessage"
            visible: Parameters.message !== ""
            x: Theme.spacingS
            width: root.width - 2 * Theme.spacingS
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            font: Theme.smallFont
            color: Theme.error
            text: Parameters.message
        }
    }
}
