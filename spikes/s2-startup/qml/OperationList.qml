import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GlassPanel {
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0
        PanelHeader { title: "Operations"; Layout.fillWidth: true }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.vertical: ScrollBar {}
            model: ListModel {
                ListElement { name: "Face"; tool: "T1 2.0 in face mill"; p: 1 }
                ListElement { name: "Adaptive rough"; tool: "T2 1/2 in 3FL"; p: 1 }
                ListElement { name: "Pocket finish"; tool: "T3 1/4 in 3FL"; p: 1 }
                ListElement { name: "Contour w/ tabs"; tool: "T2 1/2 in 3FL"; p: 0.6 }
                ListElement { name: "Spot"; tool: "T4 90 deg spot"; p: 1 }
                ListElement { name: "Drill #7"; tool: "T5 #7 drill"; p: 1 }
                ListElement { name: "Tap 1/4-20"; tool: "T6 1/4-20 tap"; p: 1 }
                ListElement { name: "Chamfer"; tool: "T7 90 deg chamfer"; p: 0.2 }
            }
            delegate: OperationRow {
                required property real p
                required name
                required tool
                width: ListView.view.width
                progress: p
            }
        }
    }
}
