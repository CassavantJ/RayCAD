// Section view and draft analysis (task T-023): a floating panel over the viewport (Shell.qml opens it from the
// top bar and the view.section.* / view.draft.* commands). Everything it shows and sets is SectionView
// (section_model.h): the cutting plane (XY, YZ, ZX or the selected planar face), its offset in document units
// with a slider and a numeric field over the extent of the model, flip, the cap (only on tiers whose preset has
// section caps; otherwise disabled with the reason), and draft analysis with its pull direction and threshold.
// Dragging the offset only moves the plane in the renderer; nothing is meshed again.
pragma ComponentBehavior: Bound

import QtQuick
import RayCad.Ui

PanelBase {
    id: panel
    objectName: "sectionPanel"

    signal closeRequested()

    readonly property var planes: ["xy", "yz", "zx", "face"]
    readonly property var pulls: ["+z", "-z", "+x", "-x", "+y", "-y", "face"]

    implicitWidth: Theme.controlHeight * 11
    implicitHeight: header.height + body.implicitHeight + 2 * Theme.spacingM

    PanelHeader {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        title: qsTr("Section and draft")
        iconName: "section"

        ToolButton {
            objectName: "sectionPanelClose"
            compact: true
            iconName: "close"
            toolTipText: qsTr("Hide the section panel")
            onClicked: panel.closeRequested()
        }
    }

    // Scrolls when the window is too short for the whole panel (Shell.qml limits its height to the view area).
    Flickable {
        id: scroller
        objectName: "sectionPanelScroller"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        clip: true
        contentWidth: width
        contentHeight: body.implicitHeight + 2 * Theme.spacingM
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: body
            x: Theme.spacingM
            y: Theme.spacingM
            width: scroller.width - 2 * Theme.spacingM
            spacing: Theme.spacingS

            // ---------- Section ----------
            Switch {
                objectName: "sectionActive"
                width: parent.width
                text: qsTr("Section view")
                checked: SectionView.active
                enabled: SectionView.hasModel || SectionView.active
                onToggled: SectionView.active = checked
            }

            TabBar {
                id: planeTabs
                objectName: "sectionPlane"
                width: parent.width
                enabled: SectionView.active
                currentIndex: Math.max(0, panel.planes.indexOf(SectionView.plane))
                Accessible.name: qsTr("Section plane")

                TabButton {
                    objectName: "sectionPlane.xy"
                    text: qsTr("XY")
                    onClicked: SectionView.plane = "xy"
                }
                TabButton {
                    objectName: "sectionPlane.yz"
                    text: qsTr("YZ")
                    onClicked: SectionView.plane = "yz"
                }
                TabButton {
                    objectName: "sectionPlane.zx"
                    text: qsTr("ZX")
                    onClicked: SectionView.plane = "zx"
                }
                TabButton {
                    objectName: "sectionPlane.face"
                    text: qsTr("Face")
                    enabled: SectionView.faceAvailable || SectionView.plane === "face"
                    onClicked: {
                        if (!SectionView.planeFromSelectedFace())
                            planeTabs.currentIndex = Math.max(0, panel.planes.indexOf(SectionView.plane))
                    }
                    HoverToolTip {
                        text: SectionView.faceAvailable ? qsTr("Cut parallel to the selected planar face")
                                                        : SectionView.faceReason
                    }
                }
            }

            Item {
                width: parent.width
                height: offsetField.implicitHeight

                Slider {
                    id: offsetSlider
                    objectName: "sectionOffsetSlider"
                    anchors.left: parent.left
                    anchors.right: offsetField.left
                    anchors.rightMargin: Theme.spacingS
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: SectionView.active && SectionView.hasModel
                    // Each binding reads SectionView.unit, so a unit switch (Units) converts the range and value live.
                    from: SectionView.unit !== "" ? SectionView.toUnits(SectionView.offsetMin) : 0
                    to: SectionView.unit !== "" ? SectionView.toUnits(SectionView.offsetMax) : 0
                    value: SectionView.unit !== "" ? SectionView.toUnits(SectionView.offset) : 0
                    live: true
                    Accessible.name: qsTr("Section offset")
                    Accessible.description: SectionView.offsetText
                    // Every move only moves the plane in the renderer (uniforms), so dragging stays smooth.
                    onMoved: SectionView.offset = SectionView.fromUnits(value)
                }
                NumberField {
                    id: offsetField
                    objectName: "sectionOffsetField"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.controlHeight * 4
                    enabled: SectionView.active && SectionView.hasModel
                    label: qsTr("Section offset")
                    unit: SectionView.unit
                    decimals: SectionView.unitDecimals
                    from: SectionView.unit !== "" ? SectionView.toUnits(SectionView.offsetMin) : 0
                    to: SectionView.unit !== "" ? SectionView.toUnits(SectionView.offsetMax) : 0
                    stepSize: Math.max((to - from) / 100, Math.pow(10, -decimals))
                    // "1/2 in", "12.7 mm" or a plain number in the display units (the Units singleton).
                    parser: text => SectionView.toUnits(SectionView.parseLength(text))
                    onValueModified: v => SectionView.offset = SectionView.fromUnits(v)
                }
                // A Binding element, not a property binding: the field assigns its own value when edited, which
                // would break a plain binding and stop it following the slider.
                Binding {
                    target: offsetField
                    property: "value"
                    value: SectionView.unit !== "" ? SectionView.toUnits(SectionView.offset) : 0
                }
                // New decimals with the same value: the text is formatted again.
                Connections {
                    target: SectionView
                    function onUnitChanged() {
                        if (!offsetField.activeFocus)
                            offsetField.text = offsetField.format(offsetField.value)
                    }
                }
            }

            Row {
                spacing: Theme.spacingM

                CheckBox {
                    objectName: "sectionFlip"
                    text: qsTr("Flip")
                    enabled: SectionView.active
                    checked: SectionView.flipped
                    onToggled: SectionView.flipped = checked
                }
                CheckBox {
                    id: capBox
                    objectName: "sectionCap"
                    text: qsTr("Cap the cut")
                    enabled: SectionView.active && SectionView.capAvailable
                    checked: SectionView.cap && SectionView.capAvailable
                    onToggled: SectionView.cap = checked
                }
            }
            Label {
                objectName: "sectionCapReason"
                width: parent.width
                visible: !SectionView.capAvailable
                secondary: true
                font: Theme.smallFont
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                text: SectionView.capReason
            }

            Rectangle {
                width: parent.width
                height: Theme.borderWidth
                color: Theme.panelBorder
            }

            // ---------- Draft analysis ----------
            Switch {
                objectName: "draftActive"
                width: parent.width
                text: qsTr("Draft analysis")
                checked: SectionView.draft
                enabled: SectionView.hasModel || SectionView.draft
                onToggled: SectionView.draft = checked
            }

            Item {
                width: parent.width
                height: pullBox.implicitHeight

                Label {
                    id: pullLabel
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    secondary: true
                    text: qsTr("Pull")
                }
                ComboBox {
                    id: pullBox
                    objectName: "draftPull"
                    anchors.left: pullLabel.right
                    anchors.leftMargin: Theme.spacingS
                    anchors.right: parent.right
                    enabled: SectionView.draft
                    model: [qsTr("+Z (up)"), qsTr("-Z (down)"), qsTr("+X"), qsTr("-X"), qsTr("+Y"), qsTr("-Y"),
                            qsTr("Selected face")]
                    currentIndex: Math.max(0, panel.pulls.indexOf(SectionView.pull))
                    onActivated: index => {
                        const pull = panel.pulls[index]
                        if (pull === "face") {
                            if (!SectionView.pullFromSelectedFace()) {
                                AppContext.showMessage(SectionView.faceReason, true)
                                currentIndex = Math.max(0, panel.pulls.indexOf(SectionView.pull))
                            }
                        } else {
                            SectionView.pull = pull
                        }
                    }
                }
            }

            Item {
                width: parent.width
                height: thresholdField.implicitHeight

                Label {
                    anchors.left: parent.left
                    anchors.right: thresholdField.left
                    anchors.verticalCenter: parent.verticalCenter
                    secondary: true
                    text: qsTr("Near vertical within")
                }
                NumberField {
                    id: thresholdField
                    objectName: "draftThreshold"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.controlHeight * 3.5
                    enabled: SectionView.draft
                    label: qsTr("Draft threshold")
                    unit: qsTr("°")
                    decimals: 1
                    from: 0
                    to: 89
                    stepSize: 0.5
                    onValueModified: v => SectionView.draftThreshold = v
                }
                Binding {
                    target: thresholdField
                    property: "value"
                    value: SectionView.draftThreshold
                }
            }

            // Legend: the three classes in the colours the viewport uses for them.
            Column {
                objectName: "draftLegend"
                width: parent.width
                spacing: Theme.spacingXs
                visible: SectionView.draft

                Repeater {
                    model: [
                        { colour: Theme.draftPositive, text: qsTr("Positive draft (releases)") },
                        { colour: Theme.draftVertical,
                          text: qsTr("Near vertical (within ±%1°)").arg(Number(SectionView.draftThreshold).toFixed(1)) },
                        { colour: Theme.draftNegative, text: qsTr("Negative draft (undercut)") }
                    ]
                    delegate: Row {
                        required property var modelData
                        spacing: Theme.spacingS

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: Theme.spacingM
                            height: width
                            radius: Theme.radiusSmall / 2
                            color: parent.modelData.colour
                            border.width: Theme.borderWidth
                            border.color: Theme.panelBorder
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            font: Theme.smallFont
                            text: parent.modelData.text
                        }
                    }
                }
            }
        }
    }
}
