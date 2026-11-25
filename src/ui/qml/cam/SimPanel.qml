// The simulation panel of the Simulate workspace (task T-074; CamSim, cam/sim_tool.h), on the left of the view. It picks
// the setup, runs the simulation (gated like posting: toolpaths that are stale, failed or missing block it, with the
// reason and Generate all), shows the run's progress with Cancel (Esc), and then what was found: a summary with the
// stock's resolution, an out-of-date notice when the toolpaths changed since, the visibility of the stock, the tool,
// its holder, the fixtures and the toolpath, how the G-code viewer is linked (or Post… when no current program is there), and the
// problems in time order; a row goes to its problem as its marker on the timeline does.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "simPanelHost"

    // Below the selection filter, which the shell shows over the view while there are bodies.
    property real topInset: Scene.bodyCount > 0 ? Theme.controlHeight + 2 * Theme.spacingXs + Theme.panelGap : 0
    readonly property real rowWidth: Theme.controlHeight * 11
    // How much of the view the panel covers from the left edge: Fit frames the model beside it.
    readonly property real coverLeft: panel.visible ? panel.x + panel.width + Theme.panelGap : 0

    component Heading: Label {
        width: root.rowWidth
        font: Theme.strongFont
        topPadding: Theme.spacingXs
    }
    component Note: Label {
        width: root.rowWidth
        wrapMode: Text.Wrap
        elide: Text.ElideNone
        secondary: true
        font: Theme.smallFont
    }

    PanelBase {
        id: panel
        objectName: "simPanel"
        x: 0
        y: root.topInset
        width: column.implicitWidth + 2 * Theme.spacingM
        height: Math.min(root.height - root.topInset, column.implicitHeight + 2 * Theme.spacingM)
        clip: true
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Simulation")

        Flickable {
            id: flick
            anchors.fill: parent
            anchors.margins: Theme.spacingM
            contentHeight: column.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height

            Column {
                id: column
                spacing: Theme.spacingS

                Row {
                    spacing: Theme.spacingS
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: "simulate"
                        color: Theme.accentText
                    }
                    Label {
                        objectName: "simTitle"
                        anchors.verticalCenter: parent.verticalCenter
                        font: Theme.strongFont
                        text: qsTr("Simulate")
                    }
                }
                Note {
                    text: qsTr("Cuts the stock move by move and checks every move for collisions of the tool, its holder and the fixtures, rapids into material, and gouges.")
                }

                // The setup.
                Row {
                    spacing: Theme.spacingS
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Theme.controlHeight * 2.6
                        secondary: true
                        text: qsTr("Setup")
                    }
                    ComboBox {
                        id: setupCombo
                        objectName: "simSetup"
                        width: root.rowWidth - Theme.controlHeight * 2.6 - Theme.spacingS
                        model: CamSim.setups
                        textRole: "name"
                        currentIndex: CamSim.setupIndex
                        enabled: !CamSim.running && CamSim.setups.length > 0
                        Accessible.name: qsTr("Setup to simulate")
                        onActivated: index => CamSim.setSetup(CamSim.setups[index].id)
                    }
                }
                Note {
                    objectName: "simNoSetup"
                    visible: CamSim.setups.length === 0
                    text: qsTr("There is no setup yet: add one in the CAM workspace.")
                }

                // Run, cancel and progress.
                Row {
                    spacing: Theme.spacingS
                    Button {
                        objectName: "simRun"
                        primary: !CamSim.hasResult || CamSim.outOfDate
                        enabled: CamSim.canRun
                        text: CamSim.hasResult ? qsTr("Simulate again") : qsTr("Simulate")
                        toolTipText: CamSim.canRun ? qsTr("Simulate %1 at this computer's resolution").arg(CamSim.setupName)
                                                   : CamSim.runBlockedReason
                        onClicked: CamSim.run()
                    }
                    Button {
                        objectName: "simCancel"
                        visible: CamSim.running
                        text: qsTr("Cancel")
                        toolTipText: qsTr("Stop the simulation (Esc)")
                        onClicked: CamSim.cancel()
                    }
                }
                Column {
                    visible: CamSim.running
                    spacing: Theme.spacingXs
                    ProgressBar {
                        objectName: "simProgress"
                        width: root.rowWidth
                        indeterminate: CamSim.progress < 0
                        value: Math.max(0, CamSim.progress)
                        Accessible.name: qsTr("Simulation progress")
                    }
                    Note {
                        objectName: "simStage"
                        text: CamSim.stage
                    }
                }
                Label {
                    objectName: "simMessage"
                    visible: !CamSim.running && CamSim.message !== ""
                    width: root.rowWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    color: CamSim.state === "failed" ? Theme.error : Theme.textSecondary
                    text: CamSim.message
                }

                // What blocks a run.
                Column {
                    objectName: "simBlockers"
                    visible: !CamSim.running && CamSim.blockers.length > 0
                    spacing: Theme.spacingXs
                    Label {
                        width: root.rowWidth
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        text: qsTr("Simulate needs every operation's toolpath up to date:")
                    }
                    Repeater {
                        model: CamSim.blockers
                        delegate: Label {
                            required property var modelData
                            required property int index
                            objectName: "simBlocker." + index
                            width: root.rowWidth
                            wrapMode: Text.Wrap
                            elide: Text.ElideNone
                            color: Theme.warning
                            text: qsTr("%1: %2").arg(modelData.name).arg(modelData.message)
                        }
                    }
                    Button {
                        objectName: "simGenerateAll"
                        visible: CamSim.canGenerate
                        text: qsTr("Generate all")
                        toolTipText: qsTr("Generate the toolpaths that are stale or missing")
                        onClicked: CamSim.generateAll()
                    }
                }

                // The result.
                Column {
                    objectName: "simResult"
                    visible: CamSim.hasResult
                    spacing: Theme.spacingS

                    Rectangle {
                        objectName: "simOutOfDate"
                        visible: CamSim.outOfDate
                        width: root.rowWidth
                        height: staleColumn.implicitHeight + 2 * Theme.spacingS
                        radius: Theme.radiusSmall
                        color: "transparent"
                        border.width: Theme.borderWidth
                        border.color: Theme.warning
                        Column {
                            id: staleColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.spacingS
                            spacing: Theme.spacingXs
                            Label {
                                width: parent.width
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                color: Theme.warning
                                font: Theme.strongFont
                                text: qsTr("Out of date")
                            }
                            Label {
                                objectName: "simOutOfDateText"
                                width: parent.width
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                text: qsTr("%1 The stock and the problems shown are from before the change: simulate again.")
                                      .arg(CamSim.outOfDateReason)
                            }
                        }
                    }
                    Label {
                        objectName: "simSummary"
                        width: root.rowWidth
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        color: CamSim.errorCount > 0 ? Theme.error : CamSim.warningCount > 0 ? Theme.warning
                                                                                          : Theme.textPrimary
                        text: CamSim.summary
                    }
                    Note {
                        objectName: "simResolution"
                        text: qsTr("%1. Gouge depths and the stock shown depend on it; the collision checks do not.")
                              .arg(CamSim.resolutionText)
                    }
                    // When this run's jumps along the timeline will be slower than this computer's tier allows.
                    Note {
                        objectName: "simSlowSeek"
                        visible: CamSim.slowSeekText !== ""
                        text: CamSim.slowSeekText
                    }
                }

                // Visibility.
                Heading {
                    text: qsTr("Show")
                }
                Flow {
                    width: root.rowWidth
                    spacing: Theme.spacingS
                    CheckBox {
                        objectName: "simShow.stock"
                        text: qsTr("Stock")
                        checked: CamSim.showStock
                        enabled: CamSim.hasResult
                        onToggled: CamSim.showStock = checked
                    }
                    CheckBox {
                        objectName: "simShow.tool"
                        text: qsTr("Tool")
                        checked: CamSim.showTool
                        enabled: CamSim.hasResult
                        onToggled: CamSim.showTool = checked
                    }
                    CheckBox {
                        objectName: "simShow.holder"
                        text: qsTr("Holder")
                        checked: CamSim.showHolder
                        enabled: CamSim.hasResult
                        onToggled: CamSim.showHolder = checked
                    }
                    CheckBox {
                        objectName: "simShow.fixtures"
                        text: qsTr("Fixtures")
                        checked: CamSim.showFixtures
                        onToggled: CamSim.showFixtures = checked
                    }
                    CheckBox {
                        objectName: "simShow.toolpath"
                        text: qsTr("Toolpath")
                        checked: CamSim.showToolpath
                        enabled: CamSim.hasResult
                        onToggled: CamSim.showToolpath = checked
                    }
                }

                // The G-code link.
                Column {
                    visible: CamSim.hasResult
                    spacing: Theme.spacingXs
                    Heading {
                        text: qsTr("G-code")
                    }
                    Note {
                        objectName: "simGcodeText"
                        text: CamSim.gcodeText
                    }
                    Button {
                        objectName: "simGcodePost"
                        visible: CamSim.gcodeState !== "current"
                        text: qsTr("Post…")
                        toolTipText: qsTr("Open the post dialog for %1").arg(CamSim.setupName)
                        onClicked: CamSim.post()
                    }
                    Button {
                        objectName: "simGcodeShow"
                        visible: CamSim.gcodeState === "current"
                        text: qsTr("Show G-code")
                        toolTipText: qsTr("Open the G-code viewer beside the view")
                        onClicked: CamSim.showGcode()
                    }
                }

                // The problems, in time order.
                Heading {
                    visible: CamSim.hasResult && CamSim.events.length > 0
                    text: qsTr("Problems")
                }
                Repeater {
                    model: CamSim.hasResult ? CamSim.events : []
                    delegate: Rectangle {
                        id: eventRow
                        required property var modelData
                        required property int index
                        readonly property bool current: CamSim.currentEvent === index
                        objectName: "simEvent." + index
                        width: root.rowWidth
                        height: eventColumn.implicitHeight + 2 * Theme.spacingXs
                        radius: Theme.radiusSmall
                        color: current ? Theme.selectionFill : rowArea.containsMouse ? Theme.controlBgHover : "transparent"
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.title + ", " + modelData.where
                        Rectangle {
                            id: dot
                            x: Theme.spacingXs
                            y: Theme.spacingXs + (titleLabel.height - height) / 2
                            width: Theme.spacingS
                            height: width
                            rotation: 45
                            antialiasing: true
                            color: eventRow.modelData.severity === "error" ? Theme.error
                                   : eventRow.modelData.severity === "warning" ? Theme.warning : Theme.textSecondary
                        }
                        Column {
                            id: eventColumn
                            x: dot.x + dot.width + Theme.spacingS
                            y: Theme.spacingXs
                            width: parent.width - x - Theme.spacingXs
                            spacing: 1
                            Label {
                                id: titleLabel
                                width: parent.width
                                wrapMode: Text.Wrap
                                elide: Text.ElideNone
                                font: Theme.strongFont
                                text: eventRow.modelData.title
                            }
                            Label {
                                objectName: "simEventWhere." + eventRow.index
                                width: parent.width
                                secondary: true
                                font: Theme.smallFont
                                elide: Text.ElideRight
                                // "Face · move 12 · 0:07.4", or "Before the first move · 0:00.0" for the setup's.
                                text: qsTr("%1 · %2").arg(eventRow.modelData.where).arg(eventRow.modelData.time)
                            }
                            Label {
                                objectName: "simEventMessage." + eventRow.index
                                width: parent.width
                                wrapMode: Text.Wrap
                                elide: Text.ElideRight
                                maximumLineCount: eventRow.current ? 8 : 2
                                font: Theme.smallFont
                                text: eventRow.modelData.message
                            }
                        }
                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: CamSim.selectEvent(eventRow.index)
                        }
                    }
                }
            }
        }
    }

    // Esc cancels a run, or pauses playback, while nothing else claims the key: no app job (the shell's Esc cancels
    // that: the stock an earlier setup leaves is one) and no dialog with its own Esc (post, setup sheet, tool library,
    // templates). Two enabled Esc shortcuts are ambiguous to Qt and neither would fire (Shell.qml stands back likewise).
    Shortcut {
        objectName: "simEscShortcut"
        sequence: "Esc"
        enabled: root.visible && (CamSim.running || CamSim.playing) && !Session.jobActive && !CamPost.active
                 && !CamTools.shown && !CamTemplates.shown && !CamSheet.active
        onActivated: CamSim.escape()
    }
}
