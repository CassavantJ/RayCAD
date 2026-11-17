import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GlassPanel {
    id: root
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0
        PanelHeader { title: "Properties: Pocket 1"; Layout.fillWidth: true }
        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: scroll.availableWidth
                spacing: 0
                Section {
                    title: "Geometry"
                    Layout.fillWidth: true
                    PropertyRow { label: "Profile"; Label { text: "Sketch 2 (2 loops)"; color: Theme.text; font.pixelSize: Theme.fontSize } }
                    PropertyRow { label: "Depth"; NumberField { value: 9.525 } }
                    PropertyRow { label: "Direction"; ComboBox { model: ["One side", "Symmetric", "Two sides"]; Layout.fillWidth: true; implicitHeight: 24 } }
                    PropertyRow { label: "Taper"; NumberField { value: 0; unit: "deg" } }
                    PropertyRow { label: "Corner radius"; NumberField { value: 6.35 } }
                    PropertyRow { label: "Operation"; ComboBox { model: ["Cut", "Join", "Intersect", "New body"]; Layout.fillWidth: true; implicitHeight: 24 } }
                }
                Section {
                    title: "Placement"
                    Layout.fillWidth: true
                    PropertyRow { label: "Origin"; VectorField { x0: 25.4; y0: 12.7; z0: 19.05 } }
                    PropertyRow { label: "Normal"; VectorField { z0: 1 } }
                    PropertyRow { label: "Flip"; CheckBox { } }
                }
                Section {
                    title: "Appearance"
                    Layout.fillWidth: true
                    PropertyRow { label: "Colour"; ColorSwatch {} }
                    PropertyRow { label: "Opacity"; Slider { from: 0; to: 1; value: 1; Layout.fillWidth: true } }
                    PropertyRow { label: "Show edges"; Switch { checked: true } }
                }
                Section {
                    title: "Tolerance"
                    Layout.fillWidth: true
                    PropertyRow { label: "General"; NumberField { value: 0.127 } }
                    PropertyRow { label: "Depth"; NumberField { value: 0.051 } }
                    PropertyRow { label: "Fit"; SpinBox { from: 1; to: 12; value: 7; Layout.fillWidth: true; implicitHeight: 24 } }
                }
            }
        }
        RowLayout {
            Layout.margins: 6
            Item { Layout.fillWidth: true }
            Button { text: "Cancel" }
            Button { text: "OK"; highlighted: true }
        }
    }
}
