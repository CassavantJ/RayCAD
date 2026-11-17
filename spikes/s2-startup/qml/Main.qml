import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1600
    height: 900
    visible: true
    title: "RayCAD start-up spike"
    color: Theme.window

    menuBar: AppMenuBar { onQuitRequested: Qt.quit() }
    header: MainToolBar {}
    footer: StatusBar {}

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal
            SidePanelTabs {
                SplitView.preferredWidth: 280
                SplitView.minimumWidth: 180
            }
            Viewport {
                SplitView.fillWidth: true
                StartScreen { anchors.centerIn: parent }
                Toast { anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; anchors.topMargin: 10; message: "Start-up benchmark" }
            }
            PropertiesPanel {
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 220
            }
        }
        TimelineBar { Layout.fillWidth: true }
    }
    CommandPalette { id: palette; anchors.centerIn: Overlay.overlay }
    Shortcut { sequence: "Ctrl+K"; onActivated: palette.open() }
}
