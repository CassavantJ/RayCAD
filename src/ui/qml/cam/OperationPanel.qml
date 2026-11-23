// The operation panel of the CAM workspace (task T-045; CamOperation, cam/operation_tool.h) in the shell's viewportArea.
// One panel for every operation type: its sections are Tool, Geometry, Heights, Parameters (SchemaForm.qml, built from
// the type's JSON Schema) and Feeds and speeds, and what each shows comes from the type's schema annotations; nothing
// here names a type. Every change previews the toolpath on a worker and checks the values: errors (red) block OK,
// warnings (amber) do not. OK (Enter) adds or changes the operation as one undo step; Cancel (Esc) leaves the document
// as it was. The status line sits under OK and Cancel, so the buttons never move while a preview builds.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "operationPanelHost"

    property var view: null
    // Below the selection filter, which the shell shows over the view while there are bodies.
    property real topInset: Scene.bodyCount > 0 ? Theme.controlHeight + 2 * Theme.spacingXs + Theme.panelGap : 0
    readonly property real triadSpace: Theme.controlHeight * 2 + Theme.panelGap
    readonly property real labelWidth: Theme.controlHeight * 3.6
    // The field column grows to show every choice of its lists in full, up to what the view can spare; only past that
    // a list's text is elided (in full in its tooltip and its open list).
    readonly property real baseFieldWidth: Theme.controlHeight * 5.5
    readonly property real maxFieldWidth: Math.max(baseFieldWidth, root.width * 0.6 - labelWidth
                                                                   - Theme.controlHeightSmall - 2 * Theme.spacingS
                                                                   - 2 * Theme.spacingM)
    readonly property real fieldWidth: Math.min(maxFieldWidth, Math.max(baseFieldWidth, comboNeed, schemaForm.comboNeed))
    property var combos: []
    readonly property real comboNeed: {
        let w = 0
        for (const c of combos) {
            if (c && c.visible)
                w = Math.max(w, c.naturalWidth)
        }
        return w
    }
    readonly property real contentWidth: labelWidth + fieldWidth + Theme.controlHeightSmall + 2 * Theme.spacingS
    // How much of the view the panel covers from the left edge (0 while closed): Fit frames the model beside it.
    readonly property real coverLeft: panel.visible ? panel.x + panel.width + Theme.panelGap : 0
    readonly property var values: CamOperation.values
    readonly property var fullValues: CamOperation.fullValues
    readonly property var messages: CamOperation.fieldMessages
    readonly property var calc: CamOperation.calculated

    // ---------- rows ----------
    component Heading: Label {
        width: root.contentWidth
        font: Theme.strongFont
        topPadding: Theme.spacingXs
    }

    component FieldRow: Column {
        id: fieldRow
        property string key
        property string label
        property string hint: ""
        property string placeholder: ""
        property string note: "" // a short line under the field (a height's reference: "from stock top")
        readonly property string error: root.messages[key] !== undefined ? String(root.messages[key]) : ""
        objectName: "operationField." + key
        spacing: Theme.spacingXs / 2
        Row {
            spacing: Theme.spacingS
            Label {
                id: rowLabel
                width: root.labelWidth
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                text: fieldRow.label
                readonly property bool hovered: labelHover.hovered
                readonly property bool pressed: false
                HoverHandler {
                    id: labelHover
                }
                HoverToolTip {
                    target: rowLabel
                    text: fieldRow.hint
                }
            }
            TextField {
                objectName: "operationInput." + fieldRow.key
                width: root.fieldWidth
                // At rest a stored length shows in the app's display format; being edited, in full.
                text: activeFocus && root.fullValues[fieldRow.key] !== undefined ? String(root.fullValues[fieldRow.key])
                      : root.values[fieldRow.key] !== undefined ? String(root.values[fieldRow.key]) : ""
                placeholderText: fieldRow.placeholder
                invalid: fieldRow.error !== ""
                Accessible.name: fieldRow.label
                Accessible.description: fieldRow.hint
                onTextEdited: CamOperation.setValue(fieldRow.key, text)
                Keys.onReturnPressed: CamOperation.commit()
                Keys.onEnterPressed: CamOperation.commit()
                Keys.onEscapePressed: CamOperation.cancel()
            }
        }
        Label {
            objectName: "operationFieldNote." + fieldRow.key
            visible: fieldRow.note !== ""
            x: root.labelWidth + Theme.spacingS
            width: root.fieldWidth
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            font: Theme.smallFont
            secondary: true
            text: fieldRow.note
        }
        Label {
            objectName: "operationFieldError." + fieldRow.key
            visible: fieldRow.error !== ""
            x: root.labelWidth + Theme.spacingS
            width: root.fieldWidth
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            font: Theme.smallFont
            color: Theme.error
            text: fieldRow.error
        }
    }

    component ChoiceRow: Row {
        id: choiceRow
        property string key
        property string label
        property var choices: [] // [{value, text}]
        property var current
        signal chosen(var value)
        objectName: "operationChoice." + key
        spacing: Theme.spacingS
        function sync() {
            const i = combo.indexOfValue(current)
            if (i !== combo.currentIndex)
                combo.currentIndex = i
        }
        onCurrentChanged: sync()
        onChoicesChanged: Qt.callLater(sync)
        Label {
            width: root.labelWidth
            anchors.verticalCenter: parent.verticalCenter
            secondary: true
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            text: choiceRow.label
        }
        ComboBox {
            id: combo
            objectName: "operationCombo." + choiceRow.key
            width: root.fieldWidth
            widthLimit: root.maxFieldWidth
            model: choiceRow.choices
            textRole: "text"
            valueRole: "value"
            Accessible.name: choiceRow.label
            Component.onCompleted: {
                choiceRow.sync()
                root.combos = root.combos.concat([combo])
            }
            Component.onDestruction: root.combos = root.combos.filter(x => x !== combo)
            onActivated: choiceRow.chosen(currentValue)
            Keys.onEscapePressed: CamOperation.cancel()
        }
    }

    // A read-out line of the Calculated block: "Spindle   1200 rpm".
    component ReadOut: Row {
        property string label
        property string value
        spacing: Theme.spacingS
        Label {
            width: root.labelWidth
            secondary: true
            font: Theme.smallFont
            text: parent.label
        }
        Label {
            width: root.fieldWidth
            font: Theme.smallFont
            mono: false
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            text: parent.value
        }
    }

    function listChoices(list, valueKey, textKey) {
        const out = []
        for (const x of list)
            out.push({ value: x[valueKey], text: x[textKey] })
        return out
    }

    // ---------- the panel ----------
    PanelBase {
        id: panel
        objectName: "operationPanel"
        visible: CamOperation.active
        x: 0
        y: root.topInset
        width: column.implicitWidth + 2 * Theme.spacingM
        // The header and the footer stay; the sections between them scroll when the window is too short for all.
        height: Math.min(root.height - root.topInset - root.triadSpace,
                         header.implicitHeight + column.implicitHeight + footer.implicitHeight
                         + 2 * Theme.spacingM + 2 * Theme.spacingS)
        clip: true

        // ---- header: what is being edited, whatever the scroll ----
        Column {
            id: header
            objectName: "operationHeader"
            x: Theme.spacingM
            y: Theme.spacingM
            width: root.contentWidth
            spacing: Theme.spacingXs / 2
            Row {
                spacing: Theme.spacingS
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "sliders"
                    color: Theme.accentText
                }
                Label {
                    objectName: "operationPanelTitle"
                    anchors.verticalCenter: parent.verticalCenter
                    font: Theme.strongFont
                    text: CamOperation.title
                }
            }
            Label {
                objectName: "operationPanelSubject"
                width: parent.width
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                secondary: true
                font: Theme.smallFont
                text: (root.values["name"] !== undefined ? root.values["name"] : "") + " · " + CamOperation.typeTitle
            }
            // How many are picked, whatever the scroll (the list itself is under Geometry and can scroll away).
            Row {
                visible: CamOperation.pickCountText !== ""
                spacing: Theme.spacingXs
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "select"
                    color: CamOperation.picks.length > 0 ? Theme.accentText : Theme.textSecondary
                }
                Label {
                    objectName: "operationPickCount"
                    anchors.verticalCenter: parent.verticalCenter
                    font: Theme.smallFont
                    secondary: CamOperation.picks.length === 0
                    text: CamOperation.pickCountText
                }
            }
        }

        Flickable {
            id: flick
            objectName: "operationScroll"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.bottom: footer.top
            anchors.leftMargin: Theme.spacingM
            anchors.rightMargin: Theme.spacingM
            anchors.topMargin: Theme.spacingS
            anchors.bottomMargin: Theme.spacingS
            contentHeight: column.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            clip: true
            // Scrolled by whole device pixels, so text stays on the pixel grid.
            pixelAligned: true
            ScrollBar.vertical: ScrollBar {}

            Column {
                id: column
                spacing: Theme.spacingS

                ChoiceRow {
                    visible: !CamOperation.editing
                    key: "type"
                    label: qsTr("Type")
                    choices: CamOperation.active ? root.listChoices(CamOperation.types, "name", "title") : []
                    current: CamOperation.type
                    onChosen: value => CamOperation.setType(value)
                }
                FieldRow {
                    key: "name"
                    label: qsTr("Name")
                }

                // ---- Tool ----
                Heading {
                    objectName: "operationSection.tool"
                    text: qsTr("Tool")
                }
                Row {
                    spacing: Theme.spacingS
                    Label {
                        width: root.labelWidth
                        anchors.verticalCenter: parent.verticalCenter
                        secondary: true
                        text: qsTr("Chosen")
                    }
                    Label {
                        objectName: "operationToolText"
                        width: root.fieldWidth
                        anchors.verticalCenter: parent.verticalCenter
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        color: CamOperation.toolId === "" ? Theme.textSecondary : Theme.textPrimary
                        text: CamOperation.toolText
                    }
                }
                // The tool library's picker: the document's tools first, then the library's of the types the
                // operation's parameters call for (x-toolTypes); a library tool is embedded when OK commits.
                ToolPicker {
                    id: toolPicker
                    objectName: "operationToolPicker"
                    width: root.contentWidth
                    visibleRows: 3.5
                    toolTypes: CamOperation.active && !CamOperation.showAllTools ? CamOperation.toolTypes : []
                    currentId: CamOperation.toolId
                    onPicked: (toolId, origin) => CamOperation.chooseTool(toolId)
                }
                CheckBox {
                    objectName: "operationShowAllTools"
                    x: root.labelWidth + Theme.spacingS - Theme.spacingXs
                    width: Math.min(implicitWidth, root.fieldWidth + Theme.spacingXs)
                    text: qsTr("Show every tool")
                    checked: CamOperation.showAllTools
                    onToggled: CamOperation.showAllTools = checked
                }
                ChoiceRow {
                    key: "holder"
                    label: qsTr("Holder")
                    choices: CamOperation.active ? root.listChoices(CamOperation.holders, "id", "name") : []
                    current: CamOperation.holderId
                    onChosen: value => CamOperation.chooseHolder(value)
                }
                FieldRow {
                    key: "stickOut"
                    label: qsTr("Stick-out")
                    hint: qsTr("How far the tool sticks out of the holder; empty: the tool's own")
                    placeholder: qsTr("the tool's")
                }
                FieldRow {
                    key: "toolNumber"
                    label: qsTr("Tool number")
                    hint: qsTr("The pocket and offset registers; empty: the tool's own")
                    placeholder: qsTr("the tool's")
                }

                // ---- Geometry ----
                Heading {
                    objectName: "operationSection.geometry"
                    visible: CamOperation.geometryKind !== "none"
                    text: qsTr("Geometry")
                }
                Row {
                    visible: CamOperation.geometryKind !== "none"
                    spacing: Theme.spacingS
                    Label {
                        objectName: "operationGeometryPrompt"
                        width: root.labelWidth + root.fieldWidth - pickButton.width - clearPicks.width
                        anchors.verticalCenter: parent.verticalCenter
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        secondary: true
                        text: CamOperation.geometryPrompt
                    }
                    ToolButton {
                        id: pickButton
                        objectName: "operationPickButton"
                        anchors.verticalCenter: parent.verticalCenter
                        compact: true
                        iconName: "select"
                        checked: CamOperation.picking
                        toolTipText: CamOperation.picking ? qsTr("Stop picking") : qsTr("Pick in the view")
                        Accessible.checkable: true
                        Accessible.checked: checked
                        onClicked: CamOperation.setPicking(!CamOperation.picking)
                    }
                    ToolButton {
                        id: clearPicks
                        objectName: "operationPickClear"
                        anchors.verticalCenter: parent.verticalCenter
                        compact: true
                        iconName: "close"
                        enabled: CamOperation.picks.length > 0
                        toolTipText: qsTr("Remove every pick")
                        onClicked: CamOperation.clearPicks()
                    }
                }
                Label {
                    visible: CamOperation.geometryKind !== "none" && CamOperation.picks.length === 0
                    x: Theme.spacingM
                    secondary: true
                    font: Theme.smallFont
                    text: qsTr("Nothing picked yet")
                }
                // Each pick, marked in the view as well; the row under the pointer marks its own more strongly. A type
                // that takes several kinds (x-geometry) lists them by kind, each kind under its heading.
                Repeater {
                    model: CamOperation.active ? CamOperation.picks : []
                    delegate: Column {
                        id: pickEntry
                        required property var modelData
                        readonly property int pickIndex: modelData.index
                        spacing: Theme.spacingXs / 2
                        Label {
                            objectName: "operationPickHeading." + pickEntry.modelData.kind
                            visible: pickEntry.modelData.heading !== ""
                            x: Theme.spacingM
                            secondary: true
                            font: Theme.smallFont
                            text: pickEntry.modelData.heading
                        }
                        Item {
                            id: pickRow
                            readonly property bool emphasised: CamOperation.hoveredPick === pickEntry.pickIndex
                            objectName: "operationPick." + pickEntry.pickIndex
                            width: pickLine.implicitWidth
                            height: pickLine.implicitHeight
                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.radiusSmall
                                color: pickRow.emphasised ? Theme.controlBgHover : "transparent"
                            }
                            HoverHandler {
                                id: pickHover
                                onHoveredChanged: {
                                    if (hovered)
                                        CamOperation.hoveredPick = pickEntry.pickIndex
                                    else if (CamOperation.hoveredPick === pickEntry.pickIndex)
                                        CamOperation.hoveredPick = -1
                                }
                            }
                            Row {
                                id: pickLine
                                spacing: Theme.spacingS
                                leftPadding: Theme.spacingM
                                Label {
                                    width: root.labelWidth + root.fieldWidth - Theme.spacingM
                                    anchors.verticalCenter: parent.verticalCenter
                                    wrapMode: Text.Wrap
                                    elide: Text.ElideNone
                                    text: pickEntry.modelData.text
                                }
                                ToolButton {
                                    objectName: "operationPickRemove." + pickEntry.pickIndex
                                    anchors.verticalCenter: parent.verticalCenter
                                    compact: true
                                    iconName: "close"
                                    toolTipText: qsTr("Remove %1").arg(pickEntry.modelData.text)
                                    onClicked: CamOperation.removePick(pickEntry.pickIndex)
                                }
                            }
                        }
                    }
                }

                // ---- Heights ----
                Heading {
                    objectName: "operationSection.heights"
                    visible: CamOperation.heights.length > 0
                    text: qsTr("Heights")
                }
                Repeater {
                    model: CamOperation.active ? CamOperation.heights : []
                    delegate: FieldRow {
                        required property var modelData
                        key: modelData.key
                        label: modelData.label
                        hint: modelData.hint
                        note: modelData.from
                    }
                }

                // ---- Parameters ----
                Heading {
                    objectName: "operationSection.parameters"
                    text: qsTr("Parameters")
                }
                SchemaForm {
                    id: schemaForm
                    form: CamOperation.form
                    labelWidth: root.labelWidth
                    fieldWidth: root.fieldWidth
                    fieldWidthLimit: root.maxFieldWidth
                    onAccepted: CamOperation.commit()
                    onCancelled: CamOperation.cancel()
                }

                // ---- Feeds and speeds ----
                Heading {
                    objectName: "operationSection.feeds"
                    text: qsTr("Feeds and speeds")
                }
                ChoiceRow {
                    key: "material"
                    label: qsTr("Material")
                    choices: CamOperation.active ? root.listChoices(CamOperation.materials, "id", "name") : []
                    current: CamOperation.materialId
                    onChosen: value => CamOperation.setMaterial(value)
                }
                Label {
                    objectName: "operationMaterialSource"
                    visible: CamOperation.materialSource === "setup" || CamOperation.materialSource === "document"
                    x: root.labelWidth + Theme.spacingS
                    width: root.fieldWidth
                    secondary: true
                    font: Theme.smallFont
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    text: CamOperation.materialSource === "setup" ? qsTr("The setup's stock material")
                                                                  : qsTr("The document's material")
                }
                // What computeFeeds gives for the tool, the material and the setup's machine.
                Column {
                    objectName: "operationCalculated"
                    spacing: Theme.spacingXs / 2
                    Label {
                        width: root.contentWidth
                        secondary: true
                        font: Theme.strongFont
                        text: qsTr("Calculated")
                    }
                    Label {
                        objectName: "operationCalculatedError"
                        visible: root.calc.available !== true
                        width: root.contentWidth
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        secondary: true
                        font: Theme.smallFont
                        text: root.calc.error !== undefined ? root.calc.error : ""
                    }
                    ReadOut {
                        visible: root.calc.available === true
                        objectName: "operationCalculatedRpm"
                        label: qsTr("Spindle")
                        value: root.calc.rpm !== undefined ? root.calc.rpm : ""
                    }
                    ReadOut {
                        visible: root.calc.available === true
                        objectName: "operationCalculatedFeed"
                        label: qsTr("Feed")
                        value: root.calc.feed !== undefined ? root.calc.feed : ""
                    }
                    ReadOut {
                        visible: root.calc.available === true
                        objectName: "operationCalculatedChipLoad"
                        label: root.calc.chipLoadLabel !== undefined ? root.calc.chipLoadLabel : qsTr("Chip load")
                        value: root.calc.chipLoad !== undefined ? root.calc.chipLoad : ""
                    }
                    ReadOut {
                        visible: root.calc.available === true
                        label: qsTr("Surface speed")
                        value: root.calc.surfaceSpeed !== undefined ? root.calc.surfaceSpeed : ""
                    }
                    ReadOut {
                        visible: root.calc.available === true
                        label: qsTr("Power")
                        value: root.calc.power !== undefined ? root.calc.power : ""
                    }
                    Label {
                        objectName: "operationCalculatedLimit"
                        visible: root.calc.available === true
                        width: root.contentWidth
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        font: Theme.smallFont
                        text: root.calc.limited !== undefined ? root.calc.limited : ""
                    }
                    Repeater {
                        model: root.calc.available === true && root.calc.notes !== undefined ? root.calc.notes : []
                        delegate: Label {
                            required property string modelData
                            width: root.contentWidth
                            wrapMode: Text.Wrap
                            elide: Text.ElideNone
                            secondary: true
                            font: Theme.smallFont
                            text: modelData
                        }
                    }
                }
                Row {
                    spacing: Theme.spacingS
                    Button {
                        objectName: "operationUseCalculated"
                        enabled: root.calc.available === true
                        text: CamOperation.feedsState === "offer" ? qsTr("Update to calculated") : qsTr("Use calculated")
                        toolTipText: qsTr("Writes the calculated speed and feeds into the fields below")
                        onClicked: CamOperation.useCalculated()
                    }
                    Label {
                        objectName: "operationFeedsNote"
                        width: root.contentWidth - parent.spacing - Theme.controlHeight * 4.5
                        anchors.verticalCenter: parent.verticalCenter
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        font: Theme.smallFont
                        color: CamOperation.feedsState === "offer" ? Theme.warning : Theme.textSecondary
                        text: CamOperation.feedsNote
                    }
                }
                FieldRow {
                    key: "rpm"
                    label: qsTr("Spindle speed")
                    hint: qsTr("Revolutions per minute")
                    placeholder: qsTr("rpm")
                }
                FieldRow {
                    key: "feedCut"
                    label: qsTr("Cut feed")
                    placeholder: Units.unitLabel === "in" ? qsTr("in/min") : qsTr("mm/min")
                }
                FieldRow {
                    visible: CamOperation.cutting.indexOf("feedPlunge") >= 0
                    key: "feedPlunge"
                    label: qsTr("Plunge feed")
                    hint: qsTr("No calculation rule: Use calculated offers half the cut feed")
                    placeholder: qsTr("the cut feed")
                }
                FieldRow {
                    visible: CamOperation.cutting.indexOf("feedRamp") >= 0
                    key: "feedRamp"
                    label: qsTr("Ramp feed")
                    hint: qsTr("No calculation rule: Use calculated offers 75 % of the cut feed")
                    placeholder: qsTr("the cut feed")
                }
                FieldRow {
                    visible: CamOperation.cutting.indexOf("feedLeadIn") >= 0
                    key: "feedLeadIn"
                    label: qsTr("Lead-in feed")
                    placeholder: qsTr("the cut feed")
                }
                FieldRow {
                    visible: CamOperation.cutting.indexOf("feedLeadOut") >= 0
                    key: "feedLeadOut"
                    label: qsTr("Lead-out feed")
                    placeholder: qsTr("the cut feed")
                }
                Label {
                    objectName: "operationFeedsRule"
                    visible: CamOperation.cutting.indexOf("feedPlunge") >= 0 || CamOperation.cutting.indexOf("feedRamp") >= 0
                    x: root.labelWidth + Theme.spacingS
                    width: root.fieldWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    secondary: true
                    font: Theme.smallFont
                    text: qsTr("Plunge and ramp feeds have no calculation rule: Use calculated sets them to 50 % and 75 % of the cut feed, which you can change.")
                }
                ChoiceRow {
                    visible: CamOperation.cutting.indexOf("coolant") >= 0
                    key: "coolant"
                    label: qsTr("Coolant")
                    choices: [
                        { value: "off", text: qsTr("Off") },
                        { value: "flood", text: qsTr("Flood") },
                        { value: "mist", text: qsTr("Mist") },
                        { value: "throughTool", text: qsTr("Through the tool") },
                        { value: "air", text: qsTr("Air") }
                    ]
                    current: root.values["coolant"]
                    onChosen: value => CamOperation.setValue("coolant", value)
                }
                CheckBox {
                    objectName: "operationCheck.clockwise"
                    visible: CamOperation.cutting.indexOf("clockwise") >= 0
                    x: root.labelWidth + Theme.spacingS - Theme.spacingXs
                    width: Math.min(implicitWidth, root.fieldWidth + Theme.spacingXs)
                    text: qsTr("Spindle clockwise (M3)")
                    checked: root.values["clockwise"] !== false
                    onToggled: CamOperation.setValue("clockwise", checked)
                }

                // ---- what the check found that does not block OK (what does is in the footer) ----
                Repeater {
                    model: CamOperation.warnings
                    delegate: Label {
                        required property string modelData
                        required property int index
                        objectName: "operationWarning." + index
                        width: root.contentWidth
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        color: Theme.warning
                        font: Theme.smallFont
                        text: modelData
                    }
                }
            }
        }

        // The scrolled area's edges at the header and the footer: a row cut there reads as scrolled under them.
        ScrollEdges {
            objectName: "operationScrollEdges"
            anchors.fill: flick
            area: flick
        }

        // ---- footer: what blocks OK, the progress, OK and Cancel and the status line, always in view ----
        Column {
            id: footer
            objectName: "operationFooter"
            x: Theme.spacingM
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.spacingM
            width: root.contentWidth
            spacing: Theme.spacingS
            Repeater {
                model: CamOperation.errors
                delegate: Label {
                    required property string modelData
                    required property int index
                    objectName: "operationError." + index
                    width: root.contentWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    maximumLineCount: 4
                    color: Theme.error
                    text: modelData
                }
            }
            Label {
                objectName: "operationMessage"
                visible: CamOperation.message !== "" && CamOperation.previewStatus === "failed"
                         && CamOperation.messageField !== "pick"
                width: root.contentWidth
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                color: Theme.error
                text: CamOperation.message
            }
            ProgressBar {
                objectName: "operationProgress"
                width: root.contentWidth
                visible: CamOperation.progress >= 0
                from: 0
                to: 1
                value: Math.max(0, CamOperation.progress)
            }
            Row {
                spacing: Theme.spacingS
                layoutDirection: Qt.RightToLeft
                width: root.contentWidth
                Button {
                    objectName: "operationOk"
                    primary: true
                    text: CamOperation.editing ? qsTr("Apply") : qsTr("OK")
                    enabled: CamOperation.canCommit
                    toolTipText: CamOperation.editing ? qsTr("Change the operation (Enter)") : qsTr("Add the operation (Enter)")
                    onClicked: CamOperation.commit()
                }
                Button {
                    objectName: "operationCancel"
                    text: qsTr("Cancel")
                    toolTipText: qsTr("Close without changing the document (Esc)")
                    onClicked: CamOperation.cancel()
                }
            }
            // Below the buttons, so that OK and Cancel never move while a preview builds.
            Label {
                objectName: "operationPreviewState"
                width: root.contentWidth
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                secondary: CamOperation.messageField !== "pick"
                color: CamOperation.messageField === "pick" ? Theme.warning : Theme.textSecondary
                font: Theme.smallFont
                text: CamOperation.statusLine
                visible: text !== ""
            }
        }
    }

    // A panel that opens starts with an empty tool search.
    Connections {
        target: CamOperation
        function onActiveChanged() {
            if (CamOperation.active)
                toolPicker.clearSearch()
            // The camera stays unless the panel now covers part of the model: then it is framed beside the panel.
            if (CamOperation.active && root.view)
                Qt.callLater(() => { if (root.view) root.view.fitIfCovered() })
        }
    }

    // Esc cancels wherever the focus is while no job claims it (Enter commits from the panel's fields).
    Shortcut {
        sequence: "Esc"
        enabled: CamOperation.active && !Session.jobActive
        onActivated: CamOperation.cancel()
    }
}
