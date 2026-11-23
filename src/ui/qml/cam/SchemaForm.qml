// The parameter form of the operation panel (task T-045): one row per field of a SchemaFormModel (cam/operation_form.h),
// which builds the fields from an operation type's JSON Schema. Nothing here knows an operation type. Numbers take
// what the feature panels take (numbers in the display unit, numbers with units, expressions, parameter names); a
// choice is a list with the schema's titles; a switch is a check box. A field's description is its hint (hover the
// label), its message shows under it, and the reset button beside it brings its default back. Fields that do not
// matter for the other values (x-relevantWhen) are hidden. What the form cannot edit is a row saying so. A list of
// points (x-format "pointsXY") is a line per point with its X and Y, add, remove and pick-in-the-view buttons.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

Column {
    id: root
    objectName: "schemaForm"

    // The SchemaFormModel to show.
    property var form: null
    property real labelWidth: Theme.controlHeight * 3.6
    property real fieldWidth: Theme.controlHeight * 5.5
    // The most the host could make fieldWidth (ComboBox.widthLimit), and the width its lists need to show every choice
    // in full: the host grows fieldWidth to comboNeed, up to its limit.
    property real fieldWidthLimit: fieldWidth
    property var combos: []
    readonly property real comboNeed: {
        let w = 0
        for (const c of combos) {
            if (c && c.visible)
                w = Math.max(w, c.naturalWidth)
        }
        return w
    }
    readonly property real resetWidth: Theme.controlHeightSmall
    // A list of points (x-format "pointsXY") taking clicks in the view: its key, "" none. The host does the picking
    // (pointPickToggled) and adds the points to the form; the form only shows which list takes them.
    property string pointPickKey: ""
    signal accepted()
    signal cancelled()
    signal pointPickToggled(string key, bool on)

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
                id: textRow
                // A list of points whose stored text does not read as points is edited as text, to fix it.
                readonly property bool used: row.kind === "number" || row.kind === "integer"
                                             || (row.kind === "string" && row.modelData.multiline !== true)
                                             || (row.kind === "points" && row.modelData.pointsRead === false)
                spacing: Theme.spacingS
                visible: used
                RowLabel {
                    // (named only when it is the row's: a multi-line text and a list of points have their own)
                    objectName: textRow.used ? "schemaLabel." + row.key : ""
                    text: row.modelData.title
                    hint: row.modelData.description
                }
                TextField {
                    objectName: textRow.used ? "schemaInput." + row.key : ""
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

            // A text on several lines (x-multiline): Enter is a line break, Ctrl+Enter accepts as Enter does elsewhere.
            Row {
                id: areaRow
                readonly property bool used: row.kind === "string" && row.modelData.multiline === true
                spacing: Theme.spacingS
                visible: used
                RowLabel {
                    objectName: areaRow.used ? "schemaLabel." + row.key : ""
                    anchors.verticalCenter: undefined
                    y: Theme.spacingXs
                    text: row.modelData.title
                    hint: row.modelData.description
                }
                T.TextArea {
                    id: area
                    objectName: areaRow.used ? "schemaInput." + row.key : ""
                    width: root.fieldWidth
                    height: Math.max(Theme.controlHeight * 2.5, Math.min(implicitHeight, Theme.controlHeight * 6))
                    text: !areaRow.used || row.modelData.value === undefined ? "" : String(row.modelData.value)
                    wrapMode: TextEdit.NoWrap
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
                    Accessible.name: row.modelData.title
                    Accessible.description: row.modelData.description
                    onTextChanged: if (activeFocus && root.form && text !== String(row.modelData.value)) root.form.setValue(row.key, text)
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && (event.modifiers & Qt.ControlModifier)) {
                            root.accepted()
                            event.accepted = true
                        }
                    }
                    Keys.onEscapePressed: root.cancelled()
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: Theme.fieldBg
                        border.width: area.activeFocus ? Theme.focusRingWidth : Theme.borderWidth
                        border.color: row.erroneous ? Theme.error
                                      : area.activeFocus ? Theme.focusRing
                                      : area.hovered ? Theme.controlBorder : Theme.panelBorder
                    }
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
                    widthLimit: root.fieldWidthLimit
                    // From choicesOf: the list is made once per schema, not again on every edit.
                    model: row.kind === "enum" && root.form ? root.form.choicesOf(row.key) : []
                    textRole: "text"
                    valueRole: "value"
                    descriptionRole: "description" // x-enumDescriptions, under each choice in the list
                    Component.onDestruction: root.combos = root.combos.filter(x => x !== combo)
                    Accessible.name: row.modelData.title
                    readonly property string current: row.kind === "enum" ? String(row.modelData.value) : ""
                    function sync() {
                        const i = indexOfValue(current)
                        if (i !== currentIndex)
                            currentIndex = i
                    }
                    onCurrentChanged: sync()
                    onModelChanged: Qt.callLater(sync)
                    Component.onCompleted: {
                        Qt.callLater(sync)
                        root.combos = root.combos.concat([combo])
                    }
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

            // A list of points on the setup's XY plane (x-format "pointsXY"): the label with "pick in the view" and
            // "add" beside it, then one line per point, X and Y in the display unit as length fields take them, and a
            // remove button. Stored in mm whatever the unit.
            Column {
                id: pointsBlock
                readonly property bool shown: row.kind === "points" && row.modelData.pointsRead !== false
                readonly property var points: shown && row.modelData.points !== undefined ? row.modelData.points : []
                readonly property real coordWidth: (root.labelWidth + root.fieldWidth - indexWidth
                                                    - Theme.controlHeightSmall - 3 * Theme.spacingS) / 2
                readonly property real indexWidth: Theme.controlHeight * 0.8
                visible: shown
                spacing: Theme.spacingXs
                Row {
                    spacing: Theme.spacingS
                    RowLabel {
                        objectName: "schemaLabel." + row.key
                        text: row.modelData.title
                        hint: row.modelData.description
                    }
                    Row {
                        width: root.fieldWidth
                        spacing: Theme.spacingXs
                        anchors.verticalCenter: parent.verticalCenter
                        ToolButton {
                            id: pointPick
                            objectName: "schemaPointPick." + row.key
                            anchors.verticalCenter: parent.verticalCenter
                            compact: true
                            iconName: "point"
                            checked: root.pointPickKey === row.key
                            toolTipText: checked ? qsTr("Stop adding points in the view")
                                                 : qsTr("Add points by clicking in the view: a hole's centre, a vertex, or a point on a face")
                            Accessible.checkable: true
                            Accessible.checked: checked
                            onClicked: root.pointPickToggled(row.key, !checked)
                        }
                        ToolButton {
                            objectName: "schemaPointAdd." + row.key
                            anchors.verticalCenter: parent.verticalCenter
                            compact: true
                            iconName: "plus"
                            toolTipText: qsTr("Add a point to type in")
                            onClicked: root.form.addPoint(row.key, 0, 0)
                        }
                        Label {
                            objectName: "schemaPointCount." + row.key
                            anchors.verticalCenter: parent.verticalCenter
                            width: root.fieldWidth - pointPick.width * 2 - 2 * parent.spacing
                            secondary: true
                            font: Theme.smallFont
                            elide: Text.ElideRight
                            text: pointPick.checked ? qsTr("Click in the view")
                                  : pointsBlock.points.length === 0 ? qsTr("None")
                                  : pointsBlock.points.length === 1 ? qsTr("1 point")
                                  : qsTr("%1 points").arg(pointsBlock.points.length)
                        }
                    }
                    ResetButton {
                        key: row.key
                        isDefault: row.modelData.isDefault
                        defaultText: row.modelData.defaultText !== "" ? row.modelData.defaultText : qsTr("none")
                    }
                }
                // By count, so a line stays (and keeps the focus) while its coordinates are typed.
                Repeater {
                    model: pointsBlock.points.length
                    delegate: Row {
                        id: pointRow
                        required property int index
                        readonly property var modelData: index < pointsBlock.points.length ? pointsBlock.points[index]
                                                                                           : ({})
                        objectName: "schemaPoint." + row.key + "." + index
                        spacing: Theme.spacingS
                        Label {
                            width: pointsBlock.indexWidth
                            anchors.verticalCenter: parent.verticalCenter
                            horizontalAlignment: Text.AlignRight
                            secondary: true
                            font: Theme.smallFont
                            text: String(pointRow.index + 1)
                        }
                        Repeater {
                            model: ["x", "y"]
                            delegate: TextField {
                                required property string modelData
                                required property int index
                                objectName: "schemaPointInput." + row.key + "." + pointRow.index + "." + modelData
                                width: pointsBlock.coordWidth
                                // At rest in the display format; being edited, in full (as a length field).
                                readonly property var shownText: activeFocus ? pointRow.modelData[modelData + "Full"]
                                                                             : pointRow.modelData[modelData]
                                text: shownText !== undefined ? String(shownText) : ""
                                placeholderText: modelData.toUpperCase()
                                invalid: pointRow.modelData[modelData + "Bad"] === true
                                Accessible.name: qsTr("%1, point %2, %3").arg(row.modelData.title)
                                                 .arg(pointRow.index + 1).arg(modelData.toUpperCase())
                                onTextEdited: root.form.setPointCoordinate(row.key, pointRow.index, index, text)
                                Keys.onReturnPressed: root.accepted()
                                Keys.onEnterPressed: root.accepted()
                                Keys.onEscapePressed: root.cancelled()
                            }
                        }
                        ToolButton {
                            objectName: "schemaPointRemove." + row.key + "." + pointRow.index
                            anchors.verticalCenter: parent.verticalCenter
                            compact: true
                            iconName: "close"
                            toolTipText: qsTr("Remove point %1").arg(pointRow.index + 1)
                            onClicked: root.form.removePoint(row.key, pointRow.index)
                        }
                    }
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
