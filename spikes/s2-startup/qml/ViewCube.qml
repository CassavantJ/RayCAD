import QtQuick
import QtQuick.Controls

Item {
    implicitWidth: 84
    implicitHeight: 84
    Rectangle {
        anchors.centerIn: parent
        width: 54; height: 54
        rotation: 12
        color: Theme.panelAlt
        border.color: Theme.border
        radius: 4
        Label { anchors.centerIn: parent; text: "TOP"; color: Theme.text; font.pixelSize: 11; font.bold: true }
    }
    Repeater {
        model: [{ t: "X", c: "#e05252", px: 70, py: 66 }, { t: "Y", c: "#52c46a", px: 6, py: 66 }, { t: "Z", c: "#5290e0", px: 38, py: 0 }]
        Label {
            required property var modelData
            x: modelData.px; y: modelData.py
            text: modelData.t; color: modelData.c
            font.pixelSize: 11; font.bold: true
        }
    }
}
