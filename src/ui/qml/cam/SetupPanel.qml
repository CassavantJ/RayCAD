// The setup panel of the CAM workspace (task T-044; CamSetup, cam/setup_tool.h) in the shell's viewportArea, and its
// origin markers and triad labels over the 3D view. Creating a setup on an imported part takes four interactions:
// New setup, a click on the top face, a click on an origin marker on the previewed stock, OK (the default stock, a box
// with no offsets, is accepted as it is). Lengths take numbers in the display unit, numbers with units, expressions
// and parameter names; every change previews the stock and the WCS triad. OK (Enter) adds or edits the setup as one
// undo step; Cancel (Esc) leaves the document as it was. The status line sits under OK and Cancel, so the buttons
// never move while a preview builds.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "setupPanelHost"

    // The ViewportItem (Viewport.qml's `view`): the markers and labels go over it, in its coordinates.
    property var view: null
    // Below the selection filter, which the shell shows over the view while there are bodies.
    property real topInset: Scene.bodyCount > 0 ? Theme.controlHeight + 2 * Theme.spacingXs + Theme.panelGap : 0

    readonly property var values: CamSetup.values
    readonly property string stock: values["stock"] !== undefined ? values["stock"] : "box"
    // The view cube's axis triad: a square of 2 control heights in the bottom left corner of the view (ViewCube.qml).
    readonly property real triadSpace: Theme.controlHeight * 2 + Theme.panelGap
    readonly property real labelWidth: Theme.controlHeight * 3.6
    readonly property real fieldWidth: Theme.controlHeight * 5.5

    Binding {
        target: CamSetup
        property: "view"
        value: root.view
    }

    // ---------- rows ----------
    component FieldRow: Column {
        id: fieldRow
        property string key
        property string label
        property string placeholder: ""
        readonly property bool erroneous: CamSetup.messageField === key && CamSetup.message !== ""
        objectName: "setupField." + key
        spacing: Theme.spacingXs / 2
        Row {
            spacing: Theme.spacingS
            Label {
                width: root.labelWidth
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: fieldRow.label
            }
            TextField {
                objectName: "setupInput." + fieldRow.key
                width: root.fieldWidth
                text: root.values[fieldRow.key] !== undefined ? String(root.values[fieldRow.key]) : ""
                placeholderText: fieldRow.placeholder
                invalid: fieldRow.erroneous
                Accessible.name: fieldRow.label
                onTextEdited: CamSetup.setValue(fieldRow.key, text)
                Keys.onReturnPressed: CamSetup.commit()
                Keys.onEnterPressed: CamSetup.commit()
                Keys.onEscapePressed: CamSetup.cancel()
            }
        }
        Label {
            objectName: "setupFieldError." + fieldRow.key
            visible: fieldRow.erroneous
            x: root.labelWidth + Theme.spacingS
            width: root.fieldWidth
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            font: Theme.smallFont
            color: Theme.error
            text: CamSetup.message
        }
    }

    // One side of an OffsetRow: the side's sign and its field.
    component OffsetHalf: Row {
        id: half
        property string key
        property string sign // "−" or "+"
        property string accessibleName
        property real signWidth
        property real fieldWidth
        spacing: Theme.spacingXs
        Label {
            objectName: "setupSign." + half.key
            width: half.signWidth
            height: field.height
            horizontalAlignment: Text.AlignHCenter
            secondary: true
            elide: Text.ElideNone
            text: half.sign
            Accessible.ignored: true
        }
        TextField {
            id: field
            objectName: "setupInput." + half.key
            width: half.fieldWidth
            text: root.values[half.key] !== undefined ? String(root.values[half.key]) : ""
            invalid: CamSetup.messageField === half.key && CamSetup.message !== ""
            Accessible.name: half.accessibleName
            onTextEdited: CamSetup.setValue(half.key, text)
            Keys.onReturnPressed: CamSetup.commit()
            Keys.onEnterPressed: CamSetup.commit()
            Keys.onEscapePressed: CamSetup.cancel()
        }
    }

    // The box stock's two offsets along one axis on one row: "Offset X   − [field]  + [field]".
    component OffsetRow: Column {
        id: offsetRow
        property string axis // "X", "Y", "Z"
        readonly property string negKey: axis.toLowerCase() + "Neg"
        readonly property string posKey: axis.toLowerCase() + "Pos"
        readonly property bool erroneous: CamSetup.message !== ""
                                          && (CamSetup.messageField === negKey || CamSetup.messageField === posKey)
        readonly property real signWidth: Theme.spacingM
        readonly property real halfWidth: (root.fieldWidth - 2 * signWidth - 3 * Theme.spacingXs) / 2
        objectName: "setupOffsets." + axis
        spacing: Theme.spacingXs / 2

        Row {
            spacing: Theme.spacingS
            Label {
                objectName: "setupOffsetLabel." + offsetRow.axis
                width: root.labelWidth
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: qsTr("Offset %1").arg(offsetRow.axis)
            }
            Row {
                spacing: Theme.spacingXs
                OffsetHalf {
                    key: offsetRow.negKey
                    sign: "−"
                    signWidth: offsetRow.signWidth
                    fieldWidth: offsetRow.halfWidth
                    accessibleName: qsTr("Offset on the minus %1 side").arg(offsetRow.axis)
                }
                OffsetHalf {
                    key: offsetRow.posKey
                    sign: "+"
                    signWidth: offsetRow.signWidth
                    fieldWidth: offsetRow.halfWidth
                    accessibleName: qsTr("Offset on the plus %1 side").arg(offsetRow.axis)
                }
            }
        }
        Label {
            objectName: "setupFieldError." + (CamSetup.messageField === offsetRow.posKey ? offsetRow.posKey
                                                                                        : offsetRow.negKey)
            visible: offsetRow.erroneous
            x: root.labelWidth + Theme.spacingS
            width: root.fieldWidth
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            font: Theme.smallFont
            color: Theme.error
            text: CamSetup.message
        }
    }

    component ChoiceRow: Row {
        id: choiceRow
        property string key
        property string label
        property var choices: [] // [{value, text}]
        readonly property var current: root.values[key]
        objectName: "setupChoice." + key
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
            text: choiceRow.label
        }
        ComboBox {
            id: combo
            objectName: "setupCombo." + choiceRow.key
            width: root.fieldWidth
            model: choiceRow.choices
            textRole: "text"
            valueRole: "value"
            Accessible.name: choiceRow.label
            Component.onCompleted: choiceRow.sync()
            onActivated: CamSetup.setValue(choiceRow.key, currentValue)
        }
    }

    // A reference picked in the view: what is picked, a button that makes the view's clicks pick it, and Clear.
    component PickRow: Row {
        id: pickRow
        property string mode   // "face", "edge", "vertex"
        property string label
        property string pickedText
        property bool picked: false
        property string hint
        objectName: "setupPick." + mode
        spacing: Theme.spacingS
        Label {
            width: root.labelWidth
            anchors.verticalCenter: parent.verticalCenter
            secondary: true
            text: pickRow.label
        }
        Label {
            objectName: "setupPickText." + pickRow.mode
            width: root.fieldWidth - pickButton.width - clearButton.width - 2 * Theme.spacingXs
            anchors.verticalCenter: parent.verticalCenter
            // In full, on two lines if it must (a long body name).
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            maximumLineCount: 2
            text: pickRow.pickedText
        }
        ToolButton {
            id: pickButton
            objectName: "setupPickButton." + pickRow.mode
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            iconName: "select"
            checked: CamSetup.pickMode === pickRow.mode
            toolTipText: pickRow.hint
            Accessible.checkable: true
            Accessible.checked: checked
            onClicked: CamSetup.setPickMode(checked ? "" : pickRow.mode)
        }
        ToolButton {
            id: clearButton
            objectName: "setupPickClear." + pickRow.mode
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            iconName: "close"
            enabled: pickRow.picked
            toolTipText: qsTr("Clear")
            onClicked: CamSetup.clearPick(pickRow.mode)
        }
    }

    component SwitchRow: CheckBox {
        property string key
        objectName: "setupCheck." + key
        x: root.labelWidth + Theme.spacingS - Theme.spacingXs
        checked: root.values[key] === true
        onToggled: CamSetup.setValue(key, checked)
    }

    // The origins in cam.h's corner numbering, seen from +Z with X to the right and Y up.
    readonly property var originChoices: [
        { value: "corner0", text: qsTr("Top front left") },
        { value: "corner1", text: qsTr("Top front right") },
        { value: "corner2", text: qsTr("Top back right") },
        { value: "corner3", text: qsTr("Top back left") },
        { value: "corner4", text: qsTr("Bottom front left") },
        { value: "corner5", text: qsTr("Bottom front right") },
        { value: "corner6", text: qsTr("Bottom back right") },
        { value: "corner7", text: qsTr("Bottom back left") },
        { value: "topCenter", text: qsTr("Top centre") },
        { value: "modelPoint", text: qsTr("Vertex of the model") }
    ]
    function originName(key) {
        for (const c of originChoices) {
            if (c.value === key)
                return c.text.toLowerCase()
        }
        return key
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
        objectName: "setupPanel"
        visible: CamSetup.active
        x: 0
        y: root.topInset
        width: column.implicitWidth + 2 * Theme.spacingM
        // Clear of the view's axis triad in the bottom left corner (ViewCube.qml), which would otherwise show its
        // labels under the panel's edge; the panel scrolls when the window is too short for it.
        height: Math.min(root.height - root.topInset - root.triadSpace, column.implicitHeight + 2 * Theme.spacingM)
        clip: true

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
                        name: "cube"
                        color: Theme.accentText
                    }
                    Label {
                        objectName: "setupPanelTitle"
                        anchors.verticalCenter: parent.verticalCenter
                        font: Theme.strongFont
                        text: CamSetup.title
                    }
                }

                FieldRow {
                    key: "name"
                    label: qsTr("Name")
                }

                // Part and fixture bodies: a heading, then each body by name with its role in this setup.
                Column {
                    objectName: "setupBodies"
                    spacing: Theme.spacingXs
                    Label {
                        objectName: "setupBodiesHeading"
                        text: qsTr("Bodies")
                        font: Theme.strongFont
                    }
                    Repeater {
                        model: CamSetup.active ? CamSetup.bodies : []
                        delegate: Row {
                            id: bodyRow
                            required property var modelData
                            required property int index
                            spacing: Theme.spacingS
                            leftPadding: Theme.spacingM
                            // The body's name, indented under the heading; a long one is elided, in full on hover.
                            Label {
                                id: bodyName
                                objectName: "setupBodyName." + bodyRow.index
                                width: root.labelWidth - Theme.spacingM
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                elide: Text.ElideMiddle
                                text: bodyRow.modelData.name
                                readonly property bool hovered: nameHover.hovered
                                readonly property bool pressed: false
                                HoverHandler {
                                    id: nameHover
                                }
                                HoverToolTip {
                                    target: bodyName
                                    text: bodyName.truncated ? bodyRow.modelData.name : ""
                                }
                            }
                            ComboBox {
                                id: roleCombo
                                objectName: "setupBodyRole." + bodyRow.index
                                width: root.fieldWidth
                                textRole: "text"
                                valueRole: "value"
                                model: [
                                    { value: "part", text: qsTr("Part") },
                                    { value: "fixture", text: qsTr("Fixture") },
                                    { value: "none", text: qsTr("Ignore") }
                                ]
                                // Set once the model is there (a binding would read indexOfValue before it is).
                                readonly property string role: bodyRow.modelData.role
                                function sync() {
                                    const i = indexOfValue(role)
                                    if (i !== currentIndex)
                                        currentIndex = i
                                }
                                onRoleChanged: sync()
                                Component.onCompleted: Qt.callLater(sync)
                                Accessible.name: qsTr("Role of %1").arg(bodyRow.modelData.name)
                                onActivated: CamSetup.setBodyRole(bodyRow.modelData.id, currentValue)
                            }
                        }
                    }
                }

                // The work coordinate system.
                PickRow {
                    mode: "face"
                    label: qsTr("Z face")
                    pickedText: CamSetup.zFaceText
                    picked: CamSetup.hasZFace
                    hint: qsTr("Click a planar face in the view: Z points out of it")
                }
                SwitchRow {
                    key: "flipZ"
                    text: qsTr("Flip Z")
                }
                PickRow {
                    mode: "edge"
                    label: qsTr("X edge")
                    pickedText: CamSetup.xEdgeText
                    picked: CamSetup.hasXEdge
                    hint: qsTr("Click a straight edge in the view: X runs along it")
                }
                SwitchRow {
                    key: "flipX"
                    text: qsTr("Flip X")
                }

                // Stock.
                ChoiceRow {
                    key: "stock"
                    label: qsTr("Stock")
                    choices: [
                        { value: "box", text: qsTr("Box") },
                        { value: "cylinder", text: qsTr("Cylinder") },
                        { value: "solid", text: qsTr("Solid body") }
                    ]
                }
                OffsetRow {
                    visible: root.stock === "box"
                    axis: "X"
                }
                OffsetRow {
                    visible: root.stock === "box"
                    axis: "Y"
                }
                OffsetRow {
                    visible: root.stock === "box"
                    axis: "Z"
                }
                ChoiceRow {
                    visible: root.stock === "cylinder"
                    key: "axis"
                    label: qsTr("Axis")
                    choices: [
                        { value: "x", text: qsTr("Along X") },
                        { value: "y", text: qsTr("Along Y") },
                        { value: "z", text: qsTr("Along Z") }
                    ]
                }
                FieldRow {
                    visible: root.stock === "cylinder"
                    key: "diameter"
                    label: qsTr("Diameter")
                }
                FieldRow {
                    visible: root.stock === "cylinder"
                    key: "length"
                    label: qsTr("Length")
                }
                FieldRow {
                    visible: root.stock === "cylinder"
                    key: "endOffset"
                    label: qsTr("End offset")
                }
                ChoiceRow {
                    visible: root.stock === "solid"
                    key: "solidBody"
                    label: qsTr("Stock body")
                    choices: CamSetup.active ? root.listChoices(CamSetup.bodies, "id", "name") : []
                }

                // Origin: a marker on the previewed stock, or this list.
                ChoiceRow {
                    key: "origin"
                    label: qsTr("Origin")
                    choices: root.originChoices
                }
                PickRow {
                    visible: root.values["origin"] === "modelPoint"
                    mode: "vertex"
                    label: qsTr("Origin vertex")
                    pickedText: CamSetup.vertexText
                    picked: CamSetup.hasVertex
                    hint: qsTr("Click a vertex in the view")
                }
                ChoiceRow {
                    key: "workOffset"
                    label: qsTr("Work offset")
                    choices: [
                        { value: 1, text: "G54" }, { value: 2, text: "G55" }, { value: 3, text: "G56" },
                        { value: 4, text: "G57" }, { value: 5, text: "G58" }, { value: 6, text: "G59" }
                    ]
                }
                FieldRow {
                    key: "programNumber"
                    label: qsTr("Program")
                }
                ChoiceRow {
                    key: "machine"
                    label: qsTr("Machine")
                    choices: CamSetup.active ? root.listChoices(CamSetup.machines, "id", "name") : []
                }
                ChoiceRow {
                    key: "post"
                    label: qsTr("Post")
                    choices: {
                        const out = []
                        for (const p of CamSetup.posts)
                            out.push({ value: p, text: p })
                        return out
                    }
                }

                // The message about the values that names no field.
                Label {
                    objectName: "setupMessage"
                    visible: CamSetup.message !== "" && CamSetup.messageField === ""
                    width: root.labelWidth + root.fieldWidth + Theme.spacingS
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    color: CamSetup.previewStatus === "failed" ? Theme.error : Theme.textSecondary
                    text: CamSetup.message
                }
                Row {
                    spacing: Theme.spacingS
                    layoutDirection: Qt.RightToLeft
                    width: root.labelWidth + root.fieldWidth + Theme.spacingS
                    Button {
                        objectName: "setupOk"
                        primary: true
                        text: CamSetup.editing ? qsTr("Apply") : qsTr("OK")
                        enabled: CamSetup.canCommit
                        toolTipText: CamSetup.editing ? qsTr("Change the setup (Enter)") : qsTr("Add the setup (Enter)")
                        onClicked: CamSetup.commit()
                    }
                    Button {
                        objectName: "setupCancel"
                        text: qsTr("Cancel")
                        toolTipText: qsTr("Close without changing the document (Esc)")
                        onClicked: CamSetup.cancel()
                    }
                }
                // Below the buttons, so that OK and Cancel never move while a preview builds.
                Label {
                    objectName: "setupPreviewState"
                    secondary: true
                    font: Theme.smallFont
                    // The preview's state, else the next step still missing, else that OK would do it.
                    text: CamSetup.previewStatus === "building" ? qsTr("Building the preview…")
                          : CamSetup.previewStatus === "failed" ? qsTr("The preview shows the last values that worked")
                          : root.values["origin"] === "modelPoint" && !CamSetup.hasVertex
                            ? qsTr("Click a vertex for the origin")
                          : CamSetup.pickMode === "face" && !CamSetup.hasZFace ? qsTr("Click the top face of the part")
                          : CamSetup.pickMode === "edge" && !CamSetup.hasXEdge ? qsTr("Click a straight edge for X")
                          : CamSetup.canCommit
                            ? (CamSetup.editing ? qsTr("Ready: Apply changes the setup")
                                                : qsTr("Ready: OK adds the setup"))
                          : ""
                    visible: text !== ""
                }
            }
        }
    }

    // Esc cancels wherever the focus is while no job claims it (Enter commits from the panel's fields).
    Shortcut {
        sequence: "Esc"
        enabled: CamSetup.active && !Session.jobActive
        onActivated: CamSetup.cancel()
    }

    // ---------- over the view: origin markers and the triad's labels ----------
    Item {
        id: overView
        objectName: "setupOverlay"
        parent: root.view ? root.view : root
        anchors.fill: parent
        visible: CamSetup.active

        Repeater {
            model: CamSetup.active ? CamSetup.markers : []
            delegate: Rectangle {
                id: marker
                required property var modelData
                readonly property bool current: modelData.current
                readonly property bool hot: markerArea.containsMouse
                readonly property bool hovered: hot
                readonly property bool pressed: markerArea.pressed
                objectName: "setupMarker." + modelData.key
                width: Theme.spacingM + (hot || current ? Theme.spacingXs : 0)
                height: width
                radius: width / 2
                x: modelData.x - width / 2
                y: modelData.y - height / 2
                color: current ? Theme.accent : hot ? Theme.accentFill : Theme.panelBg
                border.width: Theme.focusRingWidth
                border.color: Theme.accent
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Origin: %1").arg(root.originName(modelData.key))

                MouseArea {
                    id: markerArea
                    anchors.fill: parent
                    anchors.margins: -Theme.spacingXs
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: CamSetup.pickOrigin(marker.modelData.key)
                }
                HoverToolTip {
                    target: marker
                    text: qsTr("Origin: %1").arg(root.originName(marker.modelData.key))
                }
            }
        }
        Repeater {
            model: CamSetup.active ? CamSetup.triadLabels : []
            delegate: Text {
                required property var modelData
                objectName: "setupAxisLabel." + modelData.axis
                x: modelData.x - width / 2
                y: modelData.y - height / 2
                text: modelData.axis
                font: Theme.strongFont
                color: modelData.axis === "X" ? Theme.axisX : modelData.axis === "Y" ? Theme.axisY : Theme.axisZ
                style: Text.Outline
                styleColor: Theme.panelBg
                textFormat: Text.PlainText
            }
        }
    }
}
