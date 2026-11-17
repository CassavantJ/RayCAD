import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    spacing: 0
    TabBar {
        id: bar
        Layout.fillWidth: true
        TabButton { text: "Model" }
        TabButton { text: "CAM" }
    }
    StackLayout {
        currentIndex: bar.currentIndex
        Layout.fillWidth: true
        Layout.fillHeight: true
        ModelTree {}
        OperationList {}
    }
}
