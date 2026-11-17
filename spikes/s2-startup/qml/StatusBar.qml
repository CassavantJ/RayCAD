import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    implicitHeight: 22
    color: Theme.panelAlt
    RowLayout {
        anchors.fill: parent
        spacing: 0
        StatusItem { text: "Ready" }
        Item { Layout.fillWidth: true }
        StatusItem { text: "Selection: 1 face" }
        StatusItem { text: "Units: in" }
        StatusItem { text: "Tier: High" }
        BusyIndicator { running: false; implicitWidth: 18; implicitHeight: 18 }
        StatusItem { text: "0 jobs" }
    }
}
