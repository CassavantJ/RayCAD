// Selection filter (task T-020): a small floating toolbar at the top of the free viewport area. Each toggle lets
// clicks, boxes and the hover pick one kind of element; the renderer prefers the most specific enabled kind near
// the cursor (vertex, then edge, then face, then body).
import QtQuick
import RayCad.Ui

PanelBase {
    id: root
    objectName: "selectionFilter"

    implicitWidth: row.implicitWidth + 2 * Theme.spacingXs
    implicitHeight: row.implicitHeight + 2 * Theme.spacingXs
    width: implicitWidth
    height: implicitHeight
    Accessible.role: Accessible.ToolBar
    Accessible.name: qsTr("Selection filter")

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spacingXs / 2

        Label {
            anchors.verticalCenter: parent.verticalCenter
            leftPadding: Theme.spacingS
            rightPadding: Theme.spacingXs
            secondary: true
            font: Theme.smallFont
            text: qsTr("Select")
        }
        ToolButton {
            objectName: "filterFaces"
            compact: true
            checkable: true
            checked: Selection.faces
            text: qsTr("Faces")
            toolTipText: qsTr("Pick faces")
            onToggled: Selection.faces = checked
        }
        ToolButton {
            objectName: "filterEdges"
            compact: true
            checkable: true
            checked: Selection.edges
            text: qsTr("Edges")
            toolTipText: qsTr("Pick edges")
            onToggled: Selection.edges = checked
        }
        ToolButton {
            objectName: "filterVertices"
            compact: true
            checkable: true
            checked: Selection.vertices
            text: qsTr("Vertices")
            toolTipText: qsTr("Pick vertices")
            onToggled: Selection.vertices = checked
        }
        ToolButton {
            objectName: "filterBodies"
            compact: true
            checkable: true
            checked: Selection.bodies
            text: qsTr("Bodies")
            toolTipText: qsTr("Pick whole bodies")
            onToggled: Selection.bodies = checked
        }
    }
}
