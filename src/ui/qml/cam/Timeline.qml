// The simulation timeline of the Simulate workspace (task T-074; CamSim, cam/sim_tool.h), along the bottom of the view.
// The track is estimated machine time: each operation a band with its name (its tool change at its start), the
// problems found as markers above it (red for errors, amber for warnings; close ones merge into one marker with a
// count), the playhead where the stock and the tool are, and a faint one where they are going while a seek runs. A press
// or a drag on the track scrubs; a marker goes to its problem (and its G-code line). The transport: start, a move back,
// play or pause, a move forward, the end, the speed, the previous and next problem. Keys (Simulate workspace): Space,
// Left and Right, Home and End, Ctrl+Left and Ctrl+Right, [ and ].
import QtQuick
import RayCad.Ui

PanelBase {
    id: root
    objectName: "simTimeline"
    visible: CamSim.hasResult
    height: column.implicitHeight + 2 * Theme.spacingS

    property var markerList: []
    readonly property real total: CamSim.totalSec
    function refreshMarkers() {
        root.markerList = CamSim.markers(track.width)
    }
    function xOf(sec) {
        return root.total > 0 ? Math.max(0, Math.min(1, sec / root.total)) * track.width : 0
    }
    Connections {
        target: CamSim
        function onResultChanged() {
            root.refreshMarkers()
        }
    }

    Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spacingS
        spacing: Theme.spacingXs

        // ---- transport ----
        Item {
            id: transport
            width: parent.width
            height: Math.max(left.implicitHeight, right.implicitHeight)

            Row {
                id: left
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingXs

                ToolButton {
                    objectName: "simToStart"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "to-start"
                    toolTipText: qsTr("Go to the start (Home)")
                    onClicked: CamSim.toStart()
                }
                ToolButton {
                    objectName: "simStepBack"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "step-back"
                    toolTipText: qsTr("One move back (Left)")
                    onClicked: CamSim.stepBack()
                }
                ToolButton {
                    objectName: "simPlay"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: CamSim.playing ? "pause" : "play"
                    checked: CamSim.playing
                    toolTipText: CamSim.playing ? qsTr("Pause (Space)") : qsTr("Play (Space)")
                    Accessible.checkable: true
                    Accessible.checked: CamSim.playing
                    onClicked: CamSim.togglePlay()
                }
                ToolButton {
                    objectName: "simStepForward"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "step-forward"
                    toolTipText: qsTr("One move forward (Right)")
                    onClicked: CamSim.stepForward()
                }
                ToolButton {
                    objectName: "simToEnd"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "to-end"
                    toolTipText: qsTr("Go to the end (End)")
                    onClicked: CamSim.toEnd()
                }
                ComboBox {
                    id: speedCombo
                    objectName: "simSpeed"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(Theme.controlHeight * 2.6, naturalWidth)
                    model: CamSim.speeds
                    currentIndex: CamSim.speedIndex
                    Accessible.name: qsTr("Playback speed, as a multiple of machine time")
                    onActivated: index => CamSim.speedIndex = index
                }
                Item {
                    width: Theme.spacingS
                    height: 1
                }
                Label {
                    objectName: "simTime"
                    anchors.verticalCenter: parent.verticalCenter
                    mono: true
                    text: CamSim.timeText
                }
            }
            Label {
                objectName: "simPosition"
                anchors.left: left.right
                anchors.right: right.left
                anchors.leftMargin: Theme.spacingS
                anchors.rightMargin: Theme.spacingS
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                elide: Text.ElideRight
                text: CamSim.positionText
            }
            Row {
                id: right
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingXs

                Label {
                    objectName: "simTimelineOutOfDate"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: CamSim.outOfDate
                    color: Theme.warning
                    text: qsTr("Out of date")
                }
                ToolButton {
                    objectName: "simPreviousEvent"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "chevron-left"
                    enabled: CamSim.events.length > 0
                    toolTipText: qsTr("Previous problem (Ctrl+Left)")
                    onClicked: CamSim.previousEvent()
                }
                ToolButton {
                    objectName: "simNextEvent"
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "chevron-right"
                    enabled: CamSim.events.length > 0
                    toolTipText: qsTr("Next problem (Ctrl+Right)")
                    onClicked: CamSim.nextEvent()
                }
            }
        }
        // ---- the track ----
        Item {
            id: track
            objectName: "simTrack"
            width: parent.width
            height: Theme.controlHeight * 1.9
            opacity: CamSim.outOfDate ? 0.55 : 1
            onWidthChanged: root.refreshMarkers()
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Simulation timeline")
            Accessible.description: CamSim.timeText

            readonly property real markerBand: Theme.controlHeightSmall * 0.6
            readonly property real barTop: markerBand + 2
            readonly property real barHeight: height - barTop

            // The operations, each a band with its name.
            Repeater {
                model: CamSim.operations
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    objectName: "simOperation." + index
                    x: modelData.start * track.width
                    y: track.barTop
                    width: Math.max(1, (modelData.end - modelData.start) * track.width)
                    height: track.barHeight
                    radius: Theme.radiusSmall
                    color: index % 2 === 0 ? Theme.fieldBg : Theme.controlBg
                    border.width: Theme.borderWidth
                    border.color: Theme.panelBorder
                    clip: true
                    Label {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: Theme.spacingXs
                        anchors.rightMargin: Theme.spacingXs
                        anchors.topMargin: 1
                        font: Theme.smallFont
                        secondary: true
                        elide: Text.ElideRight
                        visible: parent.width > Theme.controlHeight
                        text: parent.modelData.name
                    }
                }
            }
            // What is done: up to the playhead.
            Rectangle {
                objectName: "simDone"
                x: 0
                y: track.barTop + track.barHeight - height
                width: root.xOf(CamSim.positionSec)
                height: Math.max(3, track.barHeight * 0.22)
                radius: height / 2
                color: Theme.accentFill
            }
            // Where the stock is going while a seek runs.
            Rectangle {
                objectName: "simTarget"
                visible: CamSim.seeking
                x: root.xOf(CamSim.targetSec) - width / 2
                y: track.barTop
                width: 2
                height: track.barHeight
                color: Theme.accent
                opacity: 0.45
            }
            // The playhead.
            Rectangle {
                objectName: "simPlayhead"
                x: root.xOf(CamSim.positionSec) - width / 2
                y: track.markerBand / 2
                width: 2
                height: track.height - y
                color: Theme.accent
            }
            // Press or drag: scrub.
            MouseArea {
                objectName: "simScrub"
                anchors.fill: parent
                anchors.topMargin: track.barTop
                acceptedButtons: Qt.LeftButton
                onPressed: mouse => CamSim.scrubTo(mouse.x / Math.max(1, width))
                onPositionChanged: mouse => {
                    if (pressed)
                        CamSim.scrubTo(mouse.x / Math.max(1, width))
                }
            }
            // The problems, above the bands.
            Repeater {
                model: root.markerList
                delegate: Item {
                    id: marker
                    required property var modelData
                    required property int index
                    readonly property bool selected: modelData.events.indexOf(CamSim.currentEvent) >= 0
                    readonly property color colour: modelData.severity === "error" ? Theme.error
                                                    : modelData.severity === "warning" ? Theme.warning
                                                    : Theme.textSecondary
                    objectName: "simMarker." + index
                    x: modelData.x - width / 2
                    y: 0
                    width: Math.max(track.markerBand, countLabel.implicitWidth + Theme.spacingXs)
                    height: track.markerBand
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.count > 1 ? qsTr("%1 problems").arg(modelData.count) : qsTr("Problem")
                    // A diamond: original, a square turned by 45 degrees.
                    Rectangle {
                        anchors.centerIn: parent
                        width: track.markerBand * 0.7
                        height: width
                        rotation: 45
                        color: marker.colour
                        border.width: marker.selected ? 2 : 0
                        border.color: Theme.textPrimary
                        antialiasing: true
                    }
                    Label {
                        id: countLabel
                        anchors.centerIn: parent
                        visible: marker.modelData.count > 1
                        font: Theme.smallFont
                        color: Theme.onAccent
                        text: marker.modelData.count
                    }
                    MouseArea {
                        id: markerArea
                        anchors.fill: parent
                        anchors.margins: -2
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: CamSim.selectEvent(marker.modelData.first)
                    }
                    // HoverToolTip reads `hovered` and `pressed`.
                    Item {
                        id: tipAnchor
                        anchors.fill: parent
                        readonly property bool hovered: markerArea.containsMouse
                        readonly property bool pressed: markerArea.pressed
                    }
                    HoverToolTip {
                        target: tipAnchor
                        text: {
                            const evs = CamSim.events
                            const first = evs[marker.modelData.first]
                            if (!first)
                                return ""
                            return marker.modelData.count > 1
                                   ? qsTr("%1 and %2 more at %3").arg(first.title).arg(marker.modelData.count - 1)
                                                                     .arg(first.time)
                                   : qsTr("%1 at %2").arg(first.title).arg(first.time)
                        }
                    }
                }
            }
        }
    }
}
