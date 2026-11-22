// The tool library manager's form (task T-046): one row per field, a label and its control, and the field's problem
// under it in the schema's words. It knows nothing about tools: the fields, their values and problems come in, edits
// go out through `edited`, so a schema-driven form (the operation panel's SchemaForm.qml, T-045) can take its place.
//
//   fields    [{key, label, kind}]: kind "text", "multiline", "choice", "int", "length", "angle", "fraction", "speed"
//   values    {key: text as shown}; problems {prefix + key: message}; choices {key: [{value, text}]} for "choice"
//   prefix    "" for the item's own fields, "presets.2." for a row's
pragma ComponentBehavior: Bound

import QtQuick
import RayCad.Ui

Column {
    id: form

    property var fields: []
    property var values: ({})
    property var problems: ({})
    property var choices: ({})
    property string prefix: ""
    property bool readOnly: false
    property real labelWidth: Theme.controlHeight * 4.2
    property real fieldWidth: Theme.controlHeight * 7
    property string objectPrefix: "toolField."

    signal edited(string key, string text)
    signal commitRequested()
    signal cancelRequested()

    spacing: Theme.spacingXs

    // The first field's control, for "open the editor" from the list.
    function focusFirst() {
        const first = rows.itemAt(0)
        if (first)
            first.focusControl()
    }

    Repeater {
        id: rows
        model: form.fields
        delegate: Column {
            id: fieldRow
            required property var modelData
            required property int index
            readonly property string key: modelData.key
            readonly property string fullKey: form.prefix + key
            readonly property string kind: modelData.kind
            readonly property string value: form.values[key] !== undefined ? String(form.values[key]) : ""
            readonly property string problem: form.problems[fullKey] !== undefined ? form.problems[fullKey] : ""
            readonly property bool isChoice: kind === "choice"
            readonly property bool longText: kind === "multiline" && form.readOnly

            function focusControl() {
                if (isChoice)
                    combo.forceActiveFocus()
                else
                    field.forceActiveFocus()
            }

            objectName: form.objectPrefix + fullKey
            spacing: Theme.spacingXs / 2

            Row {
                spacing: Theme.spacingS
                Label {
                    width: form.labelWidth
                    height: Theme.controlHeight
                    verticalAlignment: Text.AlignVCenter
                    secondary: true
                    elide: Text.ElideRight
                    text: fieldRow.modelData.label
                }
                TextField {
                    id: field
                    objectName: "toolInput." + fieldRow.fullKey
                    visible: !fieldRow.isChoice && !fieldRow.longText
                    width: form.fieldWidth
                    readOnly: form.readOnly
                    text: fieldRow.value
                    invalid: fieldRow.problem !== ""
                    Accessible.name: fieldRow.modelData.label
                    Accessible.description: fieldRow.problem
                    onTextEdited: form.edited(fieldRow.key, text)
                    Keys.onReturnPressed: form.commitRequested()
                    Keys.onEnterPressed: form.commitRequested()
                    Keys.onPressed: event => {
                        if (event.matches(StandardKey.Save)) {
                            form.commitRequested()
                            event.accepted = true
                        }
                    }
                }
                ComboBox {
                    id: combo
                    objectName: "toolChoice." + fieldRow.fullKey
                    visible: fieldRow.isChoice
                    width: form.fieldWidth
                    enabled: !form.readOnly
                    textRole: "text"
                    valueRole: "value"
                    model: fieldRow.isChoice && form.choices[fieldRow.key] !== undefined ? form.choices[fieldRow.key] : []
                    Accessible.name: fieldRow.modelData.label
                    function sync() {
                        const i = indexOfValue(fieldRow.value)
                        if (i !== currentIndex)
                            currentIndex = i
                    }
                    onModelChanged: Qt.callLater(sync)
                    Component.onCompleted: Qt.callLater(sync)
                    Connections {
                        target: fieldRow
                        function onValueChanged() {
                            combo.sync()
                        }
                    }
                    onActivated: form.edited(fieldRow.key, currentValue)
                }
                // A long comment of a read-only item, in full.
                Label {
                    objectName: "toolText." + fieldRow.fullKey
                    visible: fieldRow.longText
                    width: form.fieldWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    font: Theme.smallFont
                    text: fieldRow.value
                }
            }
            // The problem, beside the field it is about.
            Label {
                objectName: "toolProblem." + fieldRow.fullKey
                visible: fieldRow.problem !== ""
                x: form.labelWidth + Theme.spacingS
                width: form.fieldWidth
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                font: Theme.smallFont
                color: Theme.error
                text: fieldRow.problem
            }
        }
    }
}
