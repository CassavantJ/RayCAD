import QtQuick
import QtQuick.Layouts

RowLayout {
    id: root
    property real x0: 0
    property real y0: 0
    property real z0: 0
    spacing: 2
    Layout.fillWidth: true
    NumberField { value: root.x0; unit: "" }
    NumberField { value: root.y0; unit: "" }
    NumberField { value: root.z0; unit: "" }
}
