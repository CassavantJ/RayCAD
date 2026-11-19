// The property panel of the open feature tool (task T-098; FeatureTool, model/features.h) in the shell's
// viewportArea, to the right of the model tools column. One panel for every tool: the rows of the open tool's type
// are shown. Numeric fields take numbers in the display unit, numbers with units, expressions and parameter names;
// each change rebuilds the preview, and the document's message about the values shows under the field it names (or
// at the bottom). OK (Enter) adds or edits the feature as one undo step; Cancel (Esc) leaves the document as it was.
// After a sketch is finished the panel offers Extrude for it.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "featurePanelHost"

    // The left edge of the panel (the model tools column is left of it) and its top (below the selection filter,
    // which the shell shows over the view while there are bodies and no sketch is open).
    property real leftInset: Theme.controlHeight + 2 * Theme.spacingXs + Theme.panelGap
    property real topInset: Scene.bodyCount > 0 && !SketchTool.active
                            ? Theme.controlHeight + 2 * Theme.spacingXs + Theme.panelGap : 0

    readonly property string type: FeatureTool.type
    readonly property var values: FeatureTool.values
    readonly property bool sweep: type === "extrude" || type === "revolve"
    readonly property bool blend: type === "fillet" || type === "chamfer"
    readonly property real labelWidth: Theme.controlHeight * 3.4
    readonly property real fieldWidth: Theme.controlHeight * 5.5

    // ---------- rows ----------
    component FieldRow: Column {
        id: fieldRow
        property string key
        property string label
        property string placeholder: ""
        readonly property bool erroneous: FeatureTool.messageField === key && FeatureTool.message !== ""
        objectName: "featureField." + key
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
                id: field
                objectName: "featureInput." + fieldRow.key
                width: root.fieldWidth
                text: root.values[fieldRow.key] !== undefined ? String(root.values[fieldRow.key]) : ""
                placeholderText: fieldRow.placeholder
                invalid: fieldRow.erroneous
                Accessible.name: fieldRow.label
                onTextEdited: FeatureTool.setValue(fieldRow.key, text)
                Keys.onReturnPressed: FeatureTool.commit()
                Keys.onEnterPressed: FeatureTool.commit()
                Keys.onEscapePressed: FeatureTool.cancel()
            }
        }
        Label {
            objectName: "featureFieldError." + fieldRow.key
            visible: fieldRow.erroneous
            x: root.labelWidth + Theme.spacingS
            width: root.fieldWidth
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            font: Theme.smallFont
            color: Theme.error
            text: FeatureTool.message
        }
    }

    component ChoiceRow: Row {
        id: choiceRow
        property string key
        property string label
        property var choices: [] // [{value, text}]
        readonly property var current: root.values[key]
        objectName: "featureChoice." + key
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
            objectName: "featureCombo." + choiceRow.key
            width: root.fieldWidth
            model: choiceRow.choices
            textRole: "text"
            valueRole: "value"
            Accessible.name: choiceRow.label
            Component.onCompleted: choiceRow.sync()
            onActivated: FeatureTool.setValue(choiceRow.key, currentValue)
        }
    }

    component SwitchRow: CheckBox {
        property string key
        property string toolTipText: ""
        HoverToolTip {
            text: parent.toolTipText
        }
        objectName: "featureCheck." + key
        x: root.labelWidth + Theme.spacingS - Theme.spacingXs
        checked: root.values[key] === true
        onToggled: FeatureTool.setValue(key, checked)
    }

    component PickRow: Column {
        id: pickRow
        objectName: "featurePicks"
        spacing: Theme.spacingXs / 2
        Row {
            spacing: Theme.spacingS
            Label {
                width: root.labelWidth
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: root.sweep ? qsTr("Regions") : root.type === "hole" ? qsTr("Points")
                      : root.type === "datum_plane" ? qsTr("Face") : qsTr("Edges")
            }
            Label {
                objectName: "featurePickText"
                width: root.fieldWidth - clearPicks.width - pickToggle.width - 2 * Theme.spacingXs
                anchors.verticalCenter: parent.verticalCenter
                text: FeatureTool.pickText
            }
            ToolButton {
                id: pickToggle
                objectName: "featurePickToggle"
                visible: root.blend
                width: visible ? implicitWidth : 0
                compact: true
                iconName: "select"
                checked: FeatureTool.picking
                toolTipText: FeatureTool.picking ? qsTr("Done picking: show the preview") : qsTr("Pick edges in the view")
                onClicked: FeatureTool.picking = !FeatureTool.picking
            }
            ToolButton {
                id: clearPicks
                objectName: "featurePickClear"
                visible: root.type !== "datum_plane"
                width: visible ? implicitWidth : 0
                compact: true
                iconName: "close"
                enabled: FeatureTool.pickCount > 0
                toolTipText: qsTr("Clear what is picked")
                onClicked: FeatureTool.clearPicks()
            }
        }
        Label {
            x: root.labelWidth + Theme.spacingS
            width: root.fieldWidth
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            secondary: true
            font: Theme.smallFont
            text: FeatureTool.pickHint
        }
    }

    function sketchChoices() {
        const out = []
        for (const s of FeatureTool.sketches)
            out.push({ value: s.id, text: s.name })
        return out
    }
    function bodyChoices() {
        const out = []
        for (const b of FeatureTool.bodies)
            out.push({ value: b.id, text: b.name })
        return out
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
        objectName: "featurePanel"
        visible: FeatureTool.active
        x: root.leftInset
        y: root.topInset
        width: column.implicitWidth + 2 * Theme.spacingM
        height: Math.min(root.height - root.topInset, column.implicitHeight + 2 * Theme.spacingM)
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
                        name: root.type === "datum_plane" ? "datum" : root.type === "fillet" ? "fillet3d" : root.type
                        color: Theme.accentText
                    }
                    Label {
                        objectName: "featurePanelTitle"
                        anchors.verticalCenter: parent.verticalCenter
                        font: Theme.strongFont
                        text: FeatureTool.title
                    }
                }

                // Sketch and picks.
                ChoiceRow {
                    visible: root.sweep || root.type === "hole"
                    key: "sketch"
                    label: qsTr("Sketch")
                    choices: FeatureTool.active ? root.sketchChoices() : []
                }
                PickRow {
                    visible: FeatureTool.active
                             && (root.type !== "datum_plane" || root.values["base"] === "face")
                }

                // Extrude and revolve.
                ChoiceRow {
                    visible: root.sweep
                    key: "operation"
                    label: qsTr("Operation")
                    choices: [
                        { value: "new", text: qsTr("New body") },
                        { value: "join", text: qsTr("Join") },
                        { value: "cut", text: qsTr("Cut") },
                        { value: "intersect", text: qsTr("Intersect") }
                    ]
                }
                ChoiceRow {
                    visible: (root.sweep && root.values["operation"] !== "new") || root.type === "hole"
                    key: "target"
                    label: qsTr("Body")
                    choices: FeatureTool.active ? root.bodyChoices() : []
                }
                FieldRow {
                    visible: root.type === "extrude"
                    key: "distance"
                    label: qsTr("Distance")
                }
                ChoiceRow {
                    visible: root.type === "revolve"
                    key: "axis"
                    label: qsTr("Axis")
                    choices: root.type === "revolve" ? root.listChoices(FeatureTool.axes, "key", "title") : []
                }
                FieldRow {
                    visible: root.type === "revolve"
                    key: "angle"
                    label: qsTr("Angle")
                }
                SwitchRow {
                    visible: root.sweep || root.type === "hole" || root.type === "datum_plane"
                    key: "flip"
                    text: root.type === "datum_plane" ? qsTr("Flip the normal") : qsTr("Reverse direction")
                }
                SwitchRow {
                    visible: root.type === "extrude"
                    key: "symmetric"
                    text: qsTr("Symmetric")
                    toolTipText: qsTr("Half the distance to each side of the sketch")
                }
                FieldRow {
                    visible: root.type === "extrude"
                    key: "draft"
                    label: qsTr("Draft")
                    placeholder: "0 deg"
                }

                // Fillet and chamfer.
                FieldRow {
                    visible: root.type === "fillet"
                    key: "radius"
                    label: qsTr("Radius")
                }
                FieldRow {
                    visible: root.type === "chamfer"
                    key: "distance"
                    label: qsTr("Distance")
                }

                // Hole.
                ChoiceRow {
                    visible: root.type === "hole"
                    key: "kind"
                    label: qsTr("Kind")
                    choices: [
                        { value: "simple", text: qsTr("Simple") },
                        { value: "counterbore", text: qsTr("Counterbore") },
                        { value: "countersink", text: qsTr("Countersink") }
                    ]
                }
                SwitchRow {
                    visible: root.type === "hole"
                    key: "tapped"
                    text: qsTr("Tapped (thread)")
                }
                ChoiceRow {
                    visible: root.type === "hole" && root.values["tapped"] === true
                    key: "series"
                    label: qsTr("Thread series")
                    choices: root.type === "hole" ? root.listChoices(FeatureTool.threadSeries, "id", "name") : []
                }
                ChoiceRow {
                    visible: root.type === "hole" && root.values["tapped"] === true
                    key: "size"
                    label: qsTr("Size")
                    choices: {
                        if (root.type !== "hole")
                            return []
                        const out = []
                        for (const z of FeatureTool.threadSizes)
                            out.push({ value: z.designation, text: z.designation + "  (" + qsTr("drill %1").arg(z.tapDrill) + ")" })
                        return out
                    }
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["tapped"] === true
                    key: "threadClass"
                    label: qsTr("Class")
                    placeholder: qsTr("e.g. 6H")
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["tapped"] === true
                    key: "threadDepth"
                    label: qsTr("Thread depth")
                    placeholder: qsTr("the whole hole")
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["tapped"] !== true
                    key: "diameter"
                    label: qsTr("Diameter")
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["kind"] === "counterbore"
                    key: "cboreDiameter"
                    label: qsTr("C'bore ⌀")
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["kind"] === "counterbore"
                    key: "cboreDepth"
                    label: qsTr("C'bore depth")
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["kind"] === "countersink"
                    key: "csinkDiameter"
                    label: qsTr("C'sink ⌀")
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["kind"] === "countersink"
                    key: "csinkAngle"
                    label: qsTr("C'sink angle")
                }
                ChoiceRow {
                    visible: root.type === "hole"
                    key: "end"
                    label: qsTr("Depth")
                    choices: [
                        { value: "through", text: qsTr("Through") },
                        { value: "blind", text: qsTr("Blind") }
                    ]
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["end"] === "blind"
                    key: "depth"
                    label: qsTr("Hole depth")
                }
                ChoiceRow {
                    visible: root.type === "hole" && root.values["end"] === "blind"
                    key: "bottom"
                    label: qsTr("Bottom")
                    choices: [
                        { value: "angle", text: qsTr("Drill point") },
                        { value: "flat", text: qsTr("Flat") }
                    ]
                }
                FieldRow {
                    visible: root.type === "hole" && root.values["end"] === "blind" && root.values["bottom"] === "angle"
                    key: "tipAngle"
                    label: qsTr("Tip angle")
                }

                // Datum plane.
                ChoiceRow {
                    visible: root.type === "datum_plane"
                    key: "base"
                    label: qsTr("Base")
                    choices: root.type === "datum_plane" ? root.listChoices(FeatureTool.planes, "key", "title") : []
                }
                FieldRow {
                    visible: root.type === "datum_plane"
                    key: "offset"
                    label: qsTr("Offset")
                }

                // The message about the values that names no field, and the preview's state.
                Label {
                    objectName: "featureMessage"
                    visible: FeatureTool.message !== "" && FeatureTool.messageField === ""
                    width: root.labelWidth + root.fieldWidth + Theme.spacingS
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    color: FeatureTool.previewStatus === "warning" ? Theme.warning
                           : FeatureTool.previewStatus === "failed" ? Theme.error : Theme.textSecondary
                    text: FeatureTool.message
                }
                Label {
                    objectName: "featurePreviewState"
                    secondary: true
                    font: Theme.smallFont
                    text: FeatureTool.previewStatus === "building" ? qsTr("Building the preview…")
                          : FeatureTool.previewStatus === "failed" ? qsTr("The preview shows the last values that built")
                          : FeatureTool.picking ? qsTr("The preview is hidden while you pick")
                          : ""
                    visible: text !== ""
                }

                Row {
                    spacing: Theme.spacingS
                    layoutDirection: Qt.RightToLeft
                    width: root.labelWidth + root.fieldWidth + Theme.spacingS
                    Button {
                        objectName: "featureOk"
                        primary: true
                        text: FeatureTool.editing ? qsTr("Apply") : qsTr("OK")
                        enabled: FeatureTool.canCommit
                        toolTipText: qsTr("Add the feature (Enter)")
                        onClicked: FeatureTool.commit()
                    }
                    Button {
                        objectName: "featureCancel"
                        text: qsTr("Cancel")
                        toolTipText: qsTr("Close without changing the document (Esc)")
                        onClicked: FeatureTool.cancel()
                    }
                }
            }
        }
    }

    // Esc cancels wherever the focus is while no job claims it (Enter commits from the panel's fields).
    Shortcut {
        sequence: "Esc"
        enabled: FeatureTool.active && !Session.jobActive
        onActivated: FeatureTool.cancel()
    }

    // ---------- the offer after a sketch ----------
    PanelBase {
        id: offer
        objectName: "extrudeOffer"
        visible: !FeatureTool.active && FeatureTool.offerSketch.id !== undefined && !SketchTool.active
        x: root.leftInset
        y: root.topInset
        width: offerRow.implicitWidth + 2 * Theme.spacingS
        height: offerRow.implicitHeight + 2 * Theme.spacingXs

        Row {
            id: offerRow
            anchors.centerIn: parent
            spacing: Theme.spacingS
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Extrude %1?").arg(FeatureTool.offerSketch.name || "")
            }
            Button {
                objectName: "extrudeOfferAccept"
                anchors.verticalCenter: parent.verticalCenter
                primary: true
                iconName: "extrude"
                text: qsTr("Extrude")
                onClicked: FeatureTool.acceptOffer()
            }
            ToolButton {
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "close"
                toolTipText: qsTr("Dismiss")
                onClicked: FeatureTool.dismissOffer()
            }
        }
    }
}
