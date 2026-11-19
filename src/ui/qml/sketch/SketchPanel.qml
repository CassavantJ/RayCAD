// The sketch mode's panels over the view (task T-092), in the shell's viewportArea:
//
//   modelTools       with a document open and no sketch: the "New sketch" button (T-098 adds the feature tools here).
//   sketchPlanePicker  choosing where a new sketch goes: XY / XZ / YZ, the document's datum planes, the selected
//                    planar face; an offset makes a datum plane on it first.
//   sketchToolbar    while sketching: the drawing tools (the same commands as their keys and the palette), the
//                    construction toggle and Finish.
//   sketchStatus     (top centre) the sketch's name, how constrained it is (in the status colours) and what the tool wants next.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "sketchPanel"

    function shortcut(id) {
        const s = Shortcuts.revision >= 0 ? Shortcuts.shortcutText(id) : ""
        return s !== "" ? " (" + s + ")" : ""
    }

    // ---------- no sketch open ----------
    PanelBase {
        id: modelTools
        objectName: "modelTools"
        visible: Session.hasDocument && !SketchTool.active && !SketchTool.placing
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: modelColumn.implicitWidth + 2 * Theme.spacingXs
        height: modelColumn.implicitHeight + 2 * Theme.spacingXs

        Column {
            id: modelColumn
            anchors.centerIn: parent
            spacing: Theme.spacingXs / 2
            ToolButton {
                objectName: "newSketchButton"
                iconName: "sketch"
                enabled: !Session.readOnly
                toolTipText: qsTr("New sketch") + root.shortcut("sketch.mode.new")
                onClicked: Commands.trigger("sketch.mode.new")
            }
        }
    }

    // ---------- the plane picker ----------
    PanelBase {
        id: picker
        objectName: "sketchPlanePicker"
        visible: SketchTool.placing
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(pickerColumn.implicitWidth, Theme.controlHeight * 7) + 2 * Theme.spacingM
        height: pickerColumn.implicitHeight + 2 * Theme.spacingM

        Column {
            id: pickerColumn
            anchors.fill: parent
            anchors.margins: Theme.spacingM
            spacing: Theme.spacingXs

            Label {
                font: Theme.strongFont
                text: qsTr("New sketch")
            }
            Label {
                width: parent.width
                secondary: true
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                text: qsTr("Pick a plane, or select a planar face in the view.")
            }
            Repeater {
                model: SketchTool.placing ? SketchTool.planes : []
                delegate: Button {
                    required property var modelData
                    objectName: "sketchPlane." + modelData.key
                    width: pickerColumn.width
                    text: modelData.title
                    onClicked: SketchTool.placeOn(modelData.key, offsetField.text)
                }
            }
            Row {
                spacing: Theme.spacingS
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    secondary: true
                    text: qsTr("Offset")
                }
                TextField {
                    id: offsetField
                    objectName: "sketchPlaneOffset"
                    width: pickerColumn.width - x
                    placeholderText: "0 " + Units.unitLabel
                    Accessible.name: qsTr("Offset of the sketch plane (makes a datum plane)")
                }
            }
            Button {
                objectName: "sketchPlaneCancel"
                width: pickerColumn.width
                text: qsTr("Cancel")
                onClicked: SketchTool.cancelPlacing()
            }
        }
    }

    // ---------- the toolbar ----------
    PanelBase {
        id: toolbar
        objectName: "sketchToolbar"
        visible: SketchTool.active
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: toolColumn.implicitWidth + 2 * Theme.spacingXs
        height: Math.min(parent.height, toolColumn.implicitHeight + 2 * Theme.spacingXs)
        clip: true

        Column {
            id: toolColumn
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Theme.spacingXs
            spacing: Theme.spacingXs / 2

            Repeater {
                model: [
                    { name: "select", icon: "select", title: qsTr("Select and drag") },
                    { name: "line", icon: "line", title: qsTr("Line") },
                    { name: "rectangle", icon: "rectangle", title: qsTr("Rectangle") },
                    { name: "circle", icon: "circle", title: qsTr("Circle") },
                    { name: "arc3", icon: "arc3", title: qsTr("Arc through three points") },
                    { name: "arcCenter", icon: "arcCenter", title: qsTr("Arc from centre, start and end") },
                    { name: "arcTangent", icon: "arcTangent", title: qsTr("Tangent arc") },
                    { name: "slot", icon: "slot", title: qsTr("Slot") },
                    { name: "spline", icon: "spline", title: qsTr("Spline") },
                    { name: "trim", icon: "trim", title: qsTr("Trim") },
                    { name: "extend", icon: "extend", title: qsTr("Extend") },
                    { name: "offset", icon: "offset", title: qsTr("Offset") },
                    { name: "fillet", icon: "fillet", title: qsTr("Fillet corner") }
                ]
                delegate: ToolButton {
                    required property var modelData
                    objectName: "sketchTool." + modelData.name
                    iconName: modelData.icon
                    checked: SketchTool.tool === modelData.name
                    toolTipText: modelData.title + root.shortcut("sketch.tool." + modelData.name)
                    Accessible.checkable: true
                    Accessible.checked: checked
                    onClicked: Commands.trigger("sketch.tool." + modelData.name)
                }
            }
            Rectangle {
                width: Theme.controlHeightSmall
                height: Theme.borderWidth
                anchors.horizontalCenter: parent.horizontalCenter
                color: Theme.panelBorder
            }
            ToolButton {
                objectName: "sketchTool.construction"
                iconName: "construction"
                checked: SketchTool.construction
                toolTipText: qsTr("Construction geometry: the selection, or what is drawn next")
                             + root.shortcut("sketch.tool.construction")
                Accessible.checkable: true
                Accessible.checked: checked
                onClicked: Commands.trigger("sketch.tool.construction")
            }
            ToolButton {
                objectName: "sketchFinish"
                iconName: "finish"
                toolTipText: qsTr("Finish the sketch") + root.shortcut("sketch.mode.finish")
                onClicked: Commands.trigger("sketch.mode.finish")
            }
        }
    }

    // ---------- status ----------
    PanelBase {
        id: status
        objectName: "sketchStatus"
        visible: SketchTool.active
        // Top centre: where the selection filter is outside the sketch (it is hidden while sketching).
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 2 * Theme.spacingL, statusColumn.implicitWidth + 2 * Theme.spacingM)
        height: statusColumn.implicitHeight + 2 * Theme.spacingS

        Column {
            id: statusColumn
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingM
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingXs / 2

            Row {
                spacing: Theme.spacingS
                Rectangle {
                    width: Theme.spacingS
                    height: width
                    radius: width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    color: SketchTool.status === "full" ? Theme.sketchConstrained
                           : SketchTool.status === "over" || SketchTool.status === "conflict"
                             || SketchTool.status === "failed" ? Theme.sketchConflict
                           : Theme.sketchGeometry
                }
                Label {
                    font: Theme.strongFont
                    text: SketchTool.sketchName
                }
                Label {
                    objectName: "sketchStatusText"
                    secondary: true
                    text: SketchTool.statusText
                }
            }
            Label {
                objectName: "sketchHint"
                secondary: true
                font: Theme.smallFont
                text: SketchTool.hint
            }
        }
    }
}
