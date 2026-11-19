// The dimensions of the sketch over the view (task T-093), in SketchLayer.qml above the sketch: the lines and arrows
// (SketchDimensionOverlay, C++), the value labels, the smart dimension tool's live value and the value field.
//
//   A label: drag it to move it (stored on release, one undo step); click selects the dimension (Delete removes it);
//   double-click opens the value field. Driven (reference) dimensions show in parentheses in their own colour;
//   conflicting or redundant ones in the conflict colour. "fx" marks a value given by an expression.
//   The value field: a length or angle as shown, an expression over the document's parameters, or a parameter's
//   name. Enter commits (one step); what does not read is shown under it and the field stays; Esc closes it. A new
//   dimension that would over-constrain the sketch offers to be added as driven instead.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "dimensionLayer"
    visible: SketchTool.active

    // Where the keyboard goes back to when the field closes (the sketch overlay).
    property Item focusTarget: null

    function giveFocusBack() {
        if (root.focusTarget)
            root.focusTarget.forceActiveFocus()
    }

    SketchDimensionOverlay {
        objectName: "dimensionOverlay"
        anchors.fill: parent
        constraints: SketchConstraints
        theme: Theme
    }

    Repeater {
        model: SketchConstraints.labels
        delegate: Rectangle {
            id: label
            required property string cid
            required property int slot
            required property string type
            required property string title
            required property string text
            required property string expression
            required property real px
            required property real py
            required property bool driven
            required property bool bad
            required property bool hovered
            required property bool selected
            readonly property color ink: selected ? Theme.selection
                                         : hovered ? Theme.hover
                                         : bad ? Theme.sketchConflict
                                         : driven ? Theme.sketchConstruction
                                         : Theme.accentText

            objectName: "dimensionLabel." + cid
            visible: !(SketchConstraints.editing && SketchConstraints.editId === cid)
            x: px - width / 2
            y: py - height / 2
            width: content.implicitWidth + 2 * Theme.spacingXs
            height: content.implicitHeight + Theme.spacingXs
            radius: Theme.radiusSmall
            color: Theme.panelBg
            border.width: selected ? Theme.focusRingWidth : Theme.borderWidth
            border.color: label.ink
            Accessible.role: Accessible.StaticText
            Accessible.name: label.title + " " + label.text

            Row {
                id: content
                anchors.centerIn: parent
                spacing: Theme.spacingXs / 2
                Label {
                    visible: label.expression !== ""
                    anchors.verticalCenter: parent.verticalCenter
                    font: Theme.smallFont
                    color: label.ink
                    text: "fx"
                }
                Label {
                    objectName: "dimensionText"
                    mono: true
                    color: label.ink
                    text: label.text
                }
            }

            MouseArea {
                id: area
                anchors.fill: parent
                enabled: !SketchTool.dragging // a drag of the geometry passes over them
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: pressed && moved ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                property point grab
                property point start
                property bool moved: false
                onEntered: SketchConstraints.hoverConstraint(label.cid)
                onExited: if (SketchConstraints.hovered === label.cid) SketchConstraints.hoverConstraint("")
                onPressed: (mouse) => {
                    grab = Qt.point(mouse.x - width / 2, mouse.y - height / 2)
                    start = Qt.point(mouse.x, mouse.y)
                    moved = false
                }
                onPositionChanged: (mouse) => {
                    if (!pressed)
                        return
                    if (!moved && Math.hypot(mouse.x - start.x, mouse.y - start.y) < Theme.spacingXs)
                        return
                    moved = true
                    const p = mapToItem(root, mouse.x, mouse.y)
                    SketchConstraints.dragLabel(label.cid, p.x - grab.x, p.y - grab.y)
                }
                onReleased: if (moved) SketchConstraints.dropLabel(label.cid)
                onClicked: if (!moved) SketchConstraints.selectConstraint(label.cid)
                onDoubleClicked: SketchConstraints.beginEdit(label.cid)
            }
            HoverToolTip {
                target: label // label.hovered: this dimension is hovered
                text: area.containsMouse && !area.pressed
                      ? (label.expression !== "" ? label.title + ": " + label.expression : label.title)
                      : ""
            }
        }
    }

    // The smart dimension tool: the value of the dimension the picks make, where it would go.
    Rectangle {
        objectName: "dimensionPreview"
        visible: SketchConstraints.dimensionTool && SketchConstraints.previewText !== "" && !SketchConstraints.editing
        x: SketchConstraints.previewAt.x - width / 2
        y: SketchConstraints.previewAt.y - height / 2
        width: previewText.implicitWidth + 2 * Theme.spacingXs
        height: previewText.implicitHeight + Theme.spacingXs
        radius: Theme.radiusSmall
        color: Theme.panelBg
        border.width: Theme.borderWidth
        border.color: Theme.accent
        Label {
            id: previewText
            anchors.centerIn: parent
            mono: true
            color: Theme.accentText
            text: SketchConstraints.previewText
        }
    }

    // The value field.
    Rectangle {
        id: editor
        objectName: "dimensionEditor"
        visible: SketchConstraints.editing
        property string shownError: ""
        x: Math.max(0, Math.min(root.width - width, SketchConstraints.editAt.x - width / 2))
        y: Math.max(0, Math.min(root.height - height, SketchConstraints.editAt.y - field.height / 2 - Theme.spacingS))
        width: Math.max(Theme.controlHeight * 6, column.implicitWidth) + 2 * Theme.spacingS
        height: column.implicitHeight + 2 * Theme.spacingS
        radius: Theme.radiusControl
        color: Theme.panelBg
        border.width: Theme.borderWidth
        border.color: Theme.panelBorder

        Column {
            id: column
            x: Theme.spacingS
            y: Theme.spacingS
            width: editor.width - 2 * Theme.spacingS
            spacing: Theme.spacingXs

            TextField {
                id: field
                objectName: "dimensionField"
                width: parent.width
                invalid: editor.shownError !== ""
                Accessible.name: SketchConstraints.editTitle
                placeholderText: SketchConstraints.editTitle
                onTextEdited: editor.shownError = SketchConstraints.check(text)
                onAccepted: {
                    if (SketchConstraints.commitEdit(text))
                        root.giveFocusBack()
                    else
                        editor.shownError = SketchConstraints.editError
                }
                Keys.onEscapePressed: {
                    SketchConstraints.cancelEdit()
                    root.giveFocusBack()
                }
            }
            Label {
                objectName: "dimensionError"
                visible: editor.shownError !== ""
                width: parent.width
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                font: Theme.smallFont
                color: Theme.error
                text: editor.shownError
            }
            Label {
                visible: SketchConstraints.editOverConstrains
                width: parent.width
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                font: Theme.smallFont
                color: Theme.sketchConflict
                text: qsTr("This dimension would over-constrain the sketch.")
            }
            Button {
                objectName: "dimensionAddDriven"
                visible: SketchConstraints.editOverConstrains
                width: parent.width
                text: qsTr("Add as driven dimension")
                onClicked: {
                    SketchConstraints.addAsDriven()
                    root.giveFocusBack()
                }
            }
            Label {
                visible: SketchConstraints.editNew && !SketchConstraints.editOverConstrains
                width: parent.width
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                secondary: true
                font: Theme.smallFont
                text: SketchConstraints.editChecking ? qsTr("Checking…")
                                                     : qsTr("A value, an expression or a parameter; Enter adds it")
            }
        }

        Connections {
            target: SketchConstraints
            property bool wasEditing: false
            property string lastId: ""
            function onEditChanged() {
                const open = SketchConstraints.editing
                if (open && (!wasEditing || lastId !== SketchConstraints.editId)) {
                    field.text = SketchConstraints.editText
                    editor.shownError = ""
                    field.selectAll()
                    field.forceActiveFocus()
                }
                if (!open && wasEditing && field.activeFocus)
                    root.giveFocusBack()
                wasEditing = open
                lastId = SketchConstraints.editId
            }
        }
    }
}
