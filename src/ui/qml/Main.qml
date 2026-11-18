// The empty main window (task T-001). Colours, fonts and sizes will come from theme tokens (PLAN.md 6.5).
import QtQuick

Window {
    id: root
    visible: true
    width: Screen.width * 0.6
    height: Screen.height * 0.6
    title: qsTr("RayCAD")
}
