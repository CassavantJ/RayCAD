// The Measure panel (task T-022): the right panel's second tab. Measures the selection off the UI thread
// (MeasureTool, measure_model.h): one element shows its own measures, two the distance and angle between them with
// the X/Y/Z breakdown. Values are in the display units (Units, T-024: the document's, switchable here or in the status
// bar, live; decimals from the settings). A click on a value copies it, the header copies everything and pins the
// result so it stays while other things are selected.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "measurePanel"

    // One read-out row: label and value; a click (or Enter / Space when it has the focus) copies the value. A value
    // too long for the space beside its label (a point, a size) goes on its own line under it. Rows are light (no
    // button of their own, the copy icon is made while the pointer is over the row): a result can have 20 of them.
    component ValueRow: Rectangle {
        id: valueRow
        property string label
        property string value
        property bool primary: false
        signal copyRequested()

        readonly property bool hot: hover.hovered || activeFocus
        readonly property real valueSpace: width * 0.52 - Theme.iconSize - 2 * Theme.spacingXs
        readonly property bool stacked: metrics.width > valueSpace

        width: parent ? parent.width : 0
        height: stacked ? key.implicitHeight + valueText.implicitHeight + Theme.spacingS
                        : Math.max(Theme.rowHeight, valueText.implicitHeight + Theme.spacingXs)
        radius: Theme.radiusSmall
        color: hover.hovered ? Theme.controlBgHover : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Copy %1: %2").arg(label).arg(value)
        Keys.onReturnPressed: copyRequested()
        Keys.onEnterPressed: copyRequested()
        Keys.onSpacePressed: copyRequested()

        HoverHandler {
            id: hover
        }
        TapHandler {
            onTapped: valueRow.copyRequested()
        }
        TextMetrics {
            id: metrics
            font: Theme.monoFont
            text: valueRow.value
        }
        Label {
            id: key
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingXs
            anchors.top: valueRow.stacked ? parent.top : undefined
            anchors.topMargin: Theme.spacingXs / 2
            anchors.verticalCenter: valueRow.stacked ? undefined : parent.verticalCenter
            width: valueRow.stacked ? parent.width - Theme.iconSize - 2 * Theme.spacingXs : parent.width * 0.46
            secondary: !valueRow.primary
            elide: Text.ElideRight
            text: valueRow.label
        }
        Label {
            id: valueText
            objectName: "measureValueText"
            anchors.left: valueRow.stacked ? parent.left : key.right
            anchors.leftMargin: Theme.spacingXs
            anchors.right: parent.right
            anchors.rightMargin: Theme.iconSize + 2 * Theme.spacingXs
            anchors.top: valueRow.stacked ? key.bottom : undefined
            anchors.verticalCenter: valueRow.stacked ? undefined : parent.verticalCenter
            horizontalAlignment: Text.AlignRight
            mono: true
            elide: Text.ElideNone
            wrapMode: Text.Wrap
            color: valueRow.primary ? Theme.accentText : Theme.textPrimary
            text: valueRow.value
        }
        Loader {
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacingXs
            anchors.verticalCenter: valueText.verticalCenter
            active: valueRow.hot
            sourceComponent: Icon {
                name: "copy"
                color: Theme.textSecondary
            }
        }
        Loader {
            anchors.fill: parent
            active: valueRow.activeFocus && !hover.hovered
            sourceComponent: FocusRing {}
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 2 * Theme.spacingM
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: content
            x: Theme.spacingM
            y: Theme.spacingM
            width: flick.width - 2 * Theme.spacingM
            spacing: Theme.spacingXs

            // ---- the current measurement ----
            Item {
                width: parent.width
                height: Theme.controlHeight

                Label {
                    objectName: "measureTitle"
                    anchors.left: parent.left
                    anchors.right: tools.left
                    anchors.verticalCenter: parent.verticalCenter
                    font: Theme.strongFont
                    text: MeasureTool.hasResult ? MeasureTool.title : qsTr("Measure")
                }
                Row {
                    id: tools
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spacingXs / 2

                    ToolButton {
                        objectName: "measurePin"
                        compact: true
                        iconName: "pin"
                        enabled: MeasureTool.hasResult && !MeasureTool.busy
                        toolTipText: qsTr("Pin this measurement: it stays while you select other things")
                        onClicked: MeasureTool.pin()
                    }
                    ToolButton {
                        objectName: "measureCopyAll"
                        compact: true
                        iconName: "copy"
                        enabled: MeasureTool.hasResult || MeasureTool.pinnedCount > 0
                        toolTipText: qsTr("Copy all measurements")
                        onClicked: MeasureTool.copyAll()
                    }
                }
            }

            Label {
                objectName: "measureHint"
                width: parent.width
                visible: !MeasureTool.hasResult && !MeasureTool.busy && MeasureTool.hint !== ""
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                secondary: !MeasureTool.hintIsError
                color: MeasureTool.hintIsError ? Theme.error : Theme.textSecondary
                text: MeasureTool.hint
            }
            Label {
                objectName: "measureBusy"
                visible: MeasureTool.busy
                secondary: true
                text: qsTr("Measuring…")
            }

            Repeater {
                model: MeasureTool.rowsModel
                delegate: ValueRow {
                    required property string key
                    required property int index
                    required label
                    required value
                    required primary
                    objectName: "measureValue." + key
                    onCopyRequested: MeasureTool.copyValue(index)
                }
            }

            Repeater {
                model: MeasureTool.notes
                delegate: Label {
                    required property string modelData
                    width: content.width
                    secondary: true
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    font: Theme.smallFont
                    text: modelData
                }
            }

            // ---- units, density, what comes later ----
            Item {
                width: parent.width
                height: Theme.spacingS
            }
            Item {
                width: parent.width
                height: Theme.controlHeight

                Label {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    secondary: true
                    text: qsTr("Units")
                }
                TabBar {
                    id: units
                    objectName: "measureUnits"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    Accessible.name: qsTr("Units")
                    // T-024: the display units of the open document (Units, units.h), the same switch as the status
                    // bar's: the panel, the labels over the view and the selection read-out change together.
                    currentIndex: Units.system === "metric" ? 1 : 0

                    TabButton {
                        objectName: "measureUnits.inch"
                        text: qsTr("in")
                        onClicked: if (Units.system !== "inch") Units.toggle()
                    }
                    TabButton {
                        objectName: "measureUnits.mm"
                        text: qsTr("mm")
                        onClicked: if (Units.system !== "metric") Units.toggle()
                    }
                }
            }
            Item {
                width: parent.width
                height: Theme.controlHeight

                Label {
                    anchors.left: parent.left
                    anchors.right: density.left
                    anchors.verticalCenter: parent.verticalCenter
                    secondary: true
                    text: MeasureTool.metric ? qsTr("Density") : qsTr("Density (%1)").arg(MeasureTool.densityText)
                }
                NumberField {
                    id: density
                    objectName: "measureDensity"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.controlHeight * 4.5
                    from: 0.001
                    to: 100
                    stepSize: 0.05
                    decimals: 3
                    unit: qsTr("g/cm³")
                    label: qsTr("Density")
                    value: MeasureTool.density
                    onValueModified: v => MeasureTool.density = v
                }
            }
            Button {
                objectName: "measureCornerRadius"
                width: parent.width
                flat: true
                enabled: false
                text: qsTr("Minimum internal corner radius")
                toolTipText: Commands.revision >= 0 ? Commands.info("measure.minInternalCornerRadius").reason : ""
            }

            // ---- pinned ----
            Item {
                width: parent.width
                height: Theme.spacingS
                visible: MeasureTool.pinnedCount > 0
            }
            Item {
                width: parent.width
                height: Theme.controlHeight
                visible: MeasureTool.pinnedCount > 0

                Label {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    font: Theme.strongFont
                    text: qsTr("Pinned (%1)").arg(MeasureTool.pinnedCount)
                }
                ToolButton {
                    objectName: "measureClearPinned"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    text: qsTr("Clear")
                    toolTipText: qsTr("Remove every pinned measurement")
                    onClicked: MeasureTool.clearPinned()
                }
            }
            Repeater {
                model: MeasureTool.pinned
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    required property int index
                    objectName: "measurePinned." + index
                    width: content.width
                    height: cardColumn.implicitHeight + 2 * Theme.spacingXs
                    radius: Theme.radiusSmall
                    color: Theme.controlBg
                    border.width: Theme.borderWidth
                    border.color: Theme.panelBorder

                    Column {
                        id: cardColumn
                        x: Theme.spacingS
                        y: Theme.spacingXs
                        width: parent.width - 2 * Theme.spacingS

                        Item {
                            width: parent.width
                            height: Theme.controlHeightSmall

                            Label {
                                anchors.left: parent.left
                                anchors.right: unpin.left
                                anchors.verticalCenter: parent.verticalCenter
                                font: Theme.strongFont
                                text: card.modelData.title
                            }
                            ToolButton {
                                id: unpin
                                objectName: "measureUnpin"
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                compact: true
                                iconName: "close"
                                toolTipText: qsTr("Remove this pinned measurement")
                                onClicked: MeasureTool.unpin(card.modelData.id)
                            }
                        }
                        Repeater {
                            model: card.modelData.rows
                            delegate: ValueRow {
                                required property var modelData
                                required property int index
                                objectName: "measurePinnedValue." + modelData.key
                                width: cardColumn.width
                                label: modelData.label
                                value: modelData.value
                                primary: modelData.primary
                                onCopyRequested: MeasureTool.copyPinnedValue(card.modelData.id, index)
                            }
                        }
                    }
                }
            }
        }
    }
}
