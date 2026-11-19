// The feature tools (task T-098) in the model tools column (sketch/SketchPanel.qml, under New sketch): Extrude,
// Revolve, Fillet, Chamfer, Hole and Datum plane. Each button runs its command (model.tool.*), as its key and the
// command palette do; the button of the open tool is checked.
import QtQuick
import RayCad.Ui

Column {
    id: root
    objectName: "modelToolButtons"
    spacing: Theme.spacingXs / 2

    // The key of a command, for a tooltip; read only while hovered (the shortcut map loads on first use).
    function shortcut(id, hovered) {
        if (!hovered)
            return ""
        const s = Shortcuts.revision >= 0 ? Shortcuts.shortcutText(id) : ""
        return s !== "" ? " (" + s + ")" : ""
    }

    Rectangle {
        width: Theme.controlHeightSmall
        height: Theme.borderWidth
        anchors.horizontalCenter: parent.horizontalCenter
        color: Theme.panelBorder
    }

    Repeater {
        model: [
            { tool: "extrude", type: "extrude", icon: "extrude", title: qsTr("Extrude") },
            { tool: "revolve", type: "revolve", icon: "revolve", title: qsTr("Revolve") },
            { tool: "fillet", type: "fillet", icon: "fillet3d", title: qsTr("Fillet") },
            { tool: "chamfer", type: "chamfer", icon: "chamfer", title: qsTr("Chamfer") },
            { tool: "hole", type: "hole", icon: "hole", title: qsTr("Hole") },
            { tool: "datumPlane", type: "datum_plane", icon: "datum", title: qsTr("Datum plane") }
        ]
        delegate: ToolButton {
            required property var modelData
            objectName: "modelTool." + modelData.tool
            iconName: modelData.icon
            enabled: !Session.readOnly
            checked: FeatureTool.active && FeatureTool.type === modelData.type
            toolTipText: modelData.title + root.shortcut("model.tool." + modelData.tool, hovered)
            Accessible.checkable: true
            Accessible.checked: checked
            onClicked: {
                if (checked)
                    FeatureTool.cancel()
                else
                    Commands.trigger("model.tool." + modelData.tool)
            }
        }
    }
}
