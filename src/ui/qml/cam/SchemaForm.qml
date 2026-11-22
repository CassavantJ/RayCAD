// The parameter form of the operation panel (task T-045): one row per field of a SchemaFormModel (cam/operation_form.h),
// which builds the fields from an operation type's JSON Schema. Nothing here knows an operation type. Numbers take
// what the feature panels take (numbers in the display unit, numbers with units, expressions, parameter names); a
// choice is a list with the schema's titles; a switch is a check box. A field's description is its hint (hover the
// label), its message shows under it, and the reset button beside it brings its default back. Fields that do not
// matter for the other values (x-relevantWhen) are hidden. What the form cannot edit is a row saying so.
import QtQuick
import RayCad.Ui

Column {
    id: root
    objectName: "schemaForm"

    // The SchemaFormModel to show.
    property var form: null
    property real labelWidth: Theme.controlHeight * 3.6
    property real fieldWidth: Theme.controlHeight * 5.5
    readonly property real resetWidth: Theme.controlHeightSmall
    signal accepted()
    signal cancelled()

    spacing: Theme.spacingS

    // The label of a row, with the field's description on hover.
    component RowLabel: Label {
        id: rowLabel
        property string hint
        readonly property bool hovered: labelHover.hovered
        readonly property bool pressed: false
        width: root.labelWidth
        anchors.verticalCenter: parent.verticalCenter
        secondary: true
        wrapMode: Text.Wrap
        elide: Text.ElideNone
        maximumLineCount: 3
        HoverHandler {
            id: labelHover
        }
        HoverToolTip {
            target: rowLabel
            text: rowLabel.hint
        }
    }

    component ResetButton: ToolButton {
        property string key
        property bool isDefault: true
        property string defaultText
        objectName: "schemaReset." + key
        anchors.verticalCenter: parent.verticalCenter
        compact: true
        iconName: "reset"
        opacity: isDefault ? 0 : 1
        enabled: !isDefault
        toolTipText: defaultText !== "" ? qsTr("Back to the default (%1)").arg(defaultText) : qsTr("Back to the default")
        onClicked: root.form.resetField(key)
    }

    Repeater {
        model: root.form ? root.form.problems : []
        delegate: Label {
            required property string modelData
            required property int index
            objectName: "schemaProblem." + index
            width: root.labelWidth + root.fieldWidth + root.resetWidth + 2 * Theme.spacingS
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            color: Theme.warning
            font: Theme.smallFont
            text: modelData
        }
    }

    // One delegate per field for as long as the schema stays (a list model would make every row again on each key
    // stroke, and the field being typed in would lose the focus): each row reads its field again when the form changes.
    Repeater {
        model: root.form ? root.form.fieldCount : 0
        delegate: Column {
            id: row
            required property int index
            readonly property var modelData: root.form && root.form.revision >= 0 && index < root.form.fieldCount
                                             ? root.form.fields[index] : ({ key: "", kind: "", relevant: false,
                                                                            message: "", title: "", description: "",
                                                                            value: "", choices: [], isDefault: true,
                                                                            defaultText: "", reason: "" })
            readonly property string key: modelData.key
            readonly property string kind: modelData.kind
            readonly property bool erroneous: modelData.message !== ""
            objectName: "schemaRow." + key
            visible: modelData.relevant
            spacing: Theme.spacingXs / 2

            Row {
                spacing: Theme.spacingS
                visible: row.kind === "number" || row.kind === "integer" || row.kind === "string"
                RowLabel {
                    objectName: "schemaLabel." + row.key
                    text: row.modelData.title
                    hint: row.modelData.description
                }
                TextField {
                    objectName: "schemaInput." + row.key
                    width: root.fieldWidth
                    // At rest a stored length shows in the app's display format; being edited, in full.
                    text: !row.visible || row.modelData.value === undefined ? ""
                          : activeFocus && row.modelData.fullValue ? row.modelData.fullValue
                          : String(row.modelData.value)
                    placeholderText: row.modelData.defaultText
                    invalid: row.erroneous
                    Accessible.name: row.modelData.title
                    Accessible.description: row.modelData.description
                    onTextEdited: root.form.setValue(row.key, text)
                    Keys.onReturnPressed: root.accepted()
                    Keys.onEnterPressed: root.accepted()
                    Keys.onEscapePressed: root.cancelled()
                }
                ResetButton {
                    key: row.key
                    isDefault: row.modelData.isDefault
                    defaultText: row.modelData.defaultText
                }
            }

            Row {
                spacing: Theme.spacingS
                visible: row.kind === "enum"
                RowLabel {
                    text: row.modelData.title
                    hint: row.modelData.description
                }
                ComboBox {
                    id: combo
                    objectName: "schemaCombo." + row.key
                    width: root.fieldWidth
                    // From choicesOf: the list is made once per schema, not again on every edit.
                    model: row.kind === "enum" && root.form ? root.form.choicesOf(row.key) : []
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: row.modelData.title
                    readonly property string current: row.kind === "enum" ? String(row.modelData.value) : ""
                    function sync() {
                        const i = indexOfValue(current)
                        if (i !== currentIndex)
                            currentIndex = i
                    }
                    onCurrentChanged: sync()
                    onModelChanged: Qt.callLater(sync)
                    Component.onCompleted: Qt.callLater(sync)
                    onActivated: root.form.setValue(row.key, currentValue)
                    // Esc closes the list when it is open (the popup takes it), else the panel, as from a field.
                    Keys.onEscapePressed: root.cancelled()
                }
                ResetButton {
                    key: row.key
                    isDefault: row.modelData.isDefault
                    defaultText: row.modelData.defaultText
                }
            }

            Row {
                spacing: Theme.spacingS
                visible: row.kind === "boolean"
                Item {
                    width: root.labelWidth
                    height: 1
                }
                CheckBox {
                    id: check
                    objectName: "schemaCheck." + row.key
                    width: root.fieldWidth
                    text: row.modelData.title
                    checked: row.kind === "boolean" && row.modelData.value === true
                    Accessible.description: row.modelData.description
                    onToggled: root.form.setValue(row.key, checked)
                    readonly property bool hoveredHint: hovered
                    HoverToolTip {
                        target: check
                        text: row.modelData.description
                    }
                }
                ResetButton {
                    key: row.key
                    isDefault: row.modelData.isDefault
                    defaultText: row.modelData.defaultText
                }
            }

            // What the form cannot edit: said, not left out.
            Row {
                spacing: Theme.spacingS
                visible: row.kind === "unsupported"
                RowLabel {
                    text: row.modelData.title
                    hint: row.modelData.description
                }
                Label {
                    objectName: "schemaUnsupported." + row.key
                    width: root.fieldWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    color: Theme.warning
                    font: Theme.smallFont
                    text: qsTr("Cannot edit this parameter here: %1").arg(row.modelData.reason)
                }
            }

            Label {
                objectName: "schemaError." + row.key
                visible: row.erroneous
                x: root.labelWidth + Theme.spacingS
                width: root.fieldWidth
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                font: Theme.smallFont
                color: Theme.error
                text: row.modelData.message
            }
        }
    }
}
