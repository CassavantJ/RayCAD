// Properties of the current selection (task T-020): the right panel's content. Type, and for a face its surface
// type and area, for an edge its curve type, length and radius, for a vertex its coordinates, for a body its
// volume, area and bounding box, all in millimetres. The values are worked out off the UI thread (Selection.info).
// T-022 builds the real measure tools.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "selectionInfo"

    Column {
        id: details
        objectName: "selectionInfoDetails"
        visible: Selection.count > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spacingM
        spacing: Theme.spacingXs

        Label {
            objectName: "selectionInfoTitle"
            width: parent.width
            font: Theme.strongFont
            text: Selection.infoTitle !== "" ? Selection.infoTitle : Selection.summary
        }

        Repeater {
            model: Selection.info
            delegate: Item {
                required property var modelData
                width: details.width
                height: Theme.rowHeight

                Label {
                    id: key
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.35
                    secondary: true
                    text: modelData.label
                }
                Label {
                    objectName: "selectionInfoValue"
                    anchors.left: key.right
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignRight
                    mono: true
                    text: modelData.value
                }
            }
        }

        Label {
            visible: Selection.infoBusy
            secondary: true
            text: qsTr("Measuring…")
        }
    }

    Label {
        objectName: "rightPanelEmpty"
        anchors.fill: parent
        anchors.margins: Theme.spacingL
        visible: Selection.count === 0
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        secondary: true
        text: qsTr("Select a feature, face or operation to see its properties.")
    }
}
