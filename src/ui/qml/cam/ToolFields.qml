// The tool library manager's form (task T-046): one row per field, a label and its control, and the field's problem
// under it in the schema's words. It knows nothing about tools: the fields, their values and problems come in, edits
// go out through `edited`, so a schema-driven form (the operation panel's SchemaForm.qml, T-045) can take its place.
//
//   fields    [{key, label, kind}]: kind "text", "multiline", "choice", "int", "length", "angle", "fraction", "speed"
//   values    {key: text as shown}; problems {prefix + key: message}; choices {key: [{value, text}]} for "choice"
//   prefix    "" for the item's own fields, "presets.2." for a row's
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
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
            // A choice's text as shown.
            readonly property string choiceText: {
                const list = form.choices[key]
                if (isChoice && list !== undefined) {
                    for (const c of list) {
                        if (c.value === value)
                            return c.text
                    }
                }
                return value
            }

            function focusControl() {
                if (control.item)
                    control.item.forceActiveFocus()
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
                // The control: a field or a choice to edit; text where the item is read-only (only one is made).
                Loader {
                    id: control
                    width: form.fieldWidth
                    sourceComponent: form.readOnly ? readText
                                     : fieldRow.isChoice ? choice
                                     : fieldRow.kind === "multiline" ? area : field
                }
            }
            Component {
                id: field
                TextField {
                    objectName: "toolInput." + fieldRow.fullKey
                    width: form.fieldWidth
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
            }
            Component {
                id: area
                // A comment: several lines, wrapped; Ctrl+S saves, Enter is a new line.
                T.TextArea {
                    id: areaControl
                    objectName: "toolInput." + fieldRow.fullKey
                    width: form.fieldWidth
                    height: Math.max(Theme.controlHeight * 2.5, Math.min(implicitHeight, Theme.controlHeight * 6))
                    text: fieldRow.value
                    wrapMode: TextEdit.Wrap
                    font: Theme.uiFont
                    color: Theme.textPrimary
                    selectionColor: Theme.selectionFill
                    selectedTextColor: Theme.textPrimary
                    selectByMouse: true
                    hoverEnabled: true
                    leftPadding: Theme.spacingM
                    rightPadding: Theme.spacingM
                    topPadding: Theme.spacingXs
                    bottomPadding: Theme.spacingXs
                    clip: true
                    Accessible.name: fieldRow.modelData.label
                    onTextChanged: if (activeFocus && text !== fieldRow.value) form.edited(fieldRow.key, text)
                    Keys.onPressed: event => {
                        if (event.matches(StandardKey.Save)) {
                            form.commitRequested()
                            event.accepted = true
                        }
                    }
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: Theme.fieldBg
                        border.width: areaControl.activeFocus ? Theme.focusRingWidth : Theme.borderWidth
                        border.color: fieldRow.problem !== "" ? Theme.error
                                      : areaControl.activeFocus ? Theme.focusRing
                                      : areaControl.hovered ? Theme.controlBorder : Theme.panelBorder
                    }
                }
            }
            Component {
                id: choice
                ComboBox {
                    id: combo
                    objectName: "toolChoice." + fieldRow.fullKey
                    width: form.fieldWidth
                    textRole: "text"
                    valueRole: "value"
                    model: form.choices[fieldRow.key] !== undefined ? form.choices[fieldRow.key] : []
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
            }
            Component {
                id: readText
                // In full, on as many lines as it needs (a long comment).
                Label {
                    objectName: "toolText." + fieldRow.fullKey
                    width: form.fieldWidth
                    topPadding: fieldRow.longText ? (Theme.controlHeight - Theme.fontUiPx * 1.3) / 2 : 0
                    height: fieldRow.longText ? implicitHeight : Theme.controlHeight
                    verticalAlignment: fieldRow.longText ? Text.AlignTop : Text.AlignVCenter
                    wrapMode: fieldRow.longText ? Text.Wrap : Text.NoWrap
                    elide: fieldRow.longText ? Text.ElideNone : Text.ElideRight
                    font: fieldRow.longText ? Theme.smallFont : Theme.uiFont
                    secondary: fieldRow.value === ""
                    text: fieldRow.value === "" ? "—" : fieldRow.isChoice ? fieldRow.choiceText : fieldRow.value
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
