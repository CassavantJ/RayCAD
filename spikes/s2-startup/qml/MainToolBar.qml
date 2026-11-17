import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root
    property alias currentTab: tabs.currentIndex
    background: Rectangle { color: Theme.panel }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        TabBar {
            id: tabs
            Layout.fillWidth: true
            background: Rectangle { color: Theme.panelAlt }
            Repeater {
                model: ["Design", "Sketch", "Manufacture", "Simulate", "Tools"]
                TabButton {
                    required property string modelData
                    text: modelData
                    width: implicitWidth + 16
                }
            }
        }
        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.margins: 4
            RowLayout {
                spacing: 8
                RibbonGroup { title: "Create"; tools: ["Extrude", "Revolve", "Sweep", "Loft", "Hole"] }
                RibbonSeparator {}
                RibbonGroup { title: "Modify"; tools: ["Fillet", "Chamfer", "Shell", "Offset", "Draft", "Combine"] }
                RibbonSeparator {}
                RibbonGroup { title: "Construct"; tools: ["Plane", "Axis", "Point"] }
                RibbonSeparator {}
                RibbonGroup { title: "Inspect"; tools: ["Measure", "Section"] }
                Item { Layout.fillWidth: true }
                SearchField { Layout.preferredWidth: 200 }
            }
            RowLayout {
                spacing: 8
                RibbonGroup { title: "Draw"; tools: ["Line", "Arc", "Circle", "Rectangle", "Spline", "Slot"] }
                RibbonSeparator {}
                RibbonGroup { title: "Constrain"; tools: ["Coincident", "Parallel", "Tangent", "Equal", "Dimension"] }
                Item { Layout.fillWidth: true }
            }
            RowLayout {
                spacing: 8
                RibbonGroup { title: "Setup"; tools: ["Setup", "Stock", "WCS"] }
                RibbonSeparator {}
                RibbonGroup { title: "2D"; tools: ["Face", "Adaptive", "Pocket", "Contour", "Chamfer", "Engrave"] }
                RibbonSeparator {}
                RibbonGroup { title: "Drilling"; tools: ["Spot", "Drill", "Tap", "Ream", "Thread mill"] }
                RibbonSeparator {}
                RibbonGroup { title: "Output"; tools: ["Simulate", "Post"] }
                Item { Layout.fillWidth: true }
            }
            RowLayout {
                RibbonGroup { title: "Playback"; tools: ["Play", "Step", "Compare"] }
                Item { Layout.fillWidth: true }
            }
            RowLayout {
                RibbonGroup { title: "Libraries"; tools: ["Tools", "Machines", "Posts", "Materials"] }
                Item { Layout.fillWidth: true }
            }
        }
    }
}
