// The application shell (PLAN.md 6.6): floating panels over a full-window viewport.
//
//   viewportHost      fills the window, under everything. T-020 puts Viewport.qml here in place of
//                     viewportPlaceholder (remove the placeholder and axisTriad then).
//   viewportArea      the part of the viewport no panel covers: overlays (view cube, start screen, hints) go here.
//                     T-019 replaces emptyState with the start screen.
//   topBar            brand, workspace tabs (AppContext.workspace), command search (T-021), view toggles.
//   leftPanel         "Model": T-020 puts ModelTree.qml into leftPanelContent (replace leftPanelEmpty).
//   rightPanel        "Properties": property editors go into rightPanelContent (replace rightPanelEmpty).
//   statusBar         status text (AppContext.statusText) and jobSlot (AppContext.setJob, T-019).
//   workspace.<n>     one Loader per workspace, created the first time that workspace is shown.
// Only what this first screen shows is created at start; menus and tooltips are created on first use.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

FocusScope {
    id: shell
    objectName: "shell"

    property bool leftPanelOpen: true
    property bool rightPanelOpen: true
    property real leftPanelWidth: Theme.sidePanelWidth
    property real rightPanelWidth: Theme.sidePanelWidth
    readonly property real gap: Theme.panelGap

    readonly property alias viewportHost: viewportHost
    readonly property alias viewportArea: viewportArea
    readonly property alias leftPanelContent: leftPanelContent
    readonly property alias rightPanelContent: rightPanelContent
    readonly property alias commandSearch: commandSearch
    readonly property alias jobSlot: jobSlot

    // The command search field took focus or the user pressed Enter in it (T-021 opens the palette).
    signal commandSearchRequested(string text)

    function clampPanelWidth(w) {
        return Math.round(Math.max(Theme.sidePanelMinWidth, Math.min(Theme.sidePanelMaxWidth, width / 3, w)))
    }

    function openViewMenu() {
        viewMenuLoader.active = true
        const menu = viewMenuLoader.item.menu
        menu.popup(viewOptions, viewOptions.width - menu.width, viewOptions.height + Theme.spacingXs)
    }

    // ---------- Viewport (full window, under every panel) ----------
    Item {
        id: viewportHost
        objectName: "viewportHost"
        anchors.fill: parent

        Rectangle {
            id: viewportPlaceholder
            objectName: "viewportPlaceholder"
            anchors.fill: parent
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: Theme.viewportBgTop
                }
                GradientStop {
                    position: 1
                    color: Theme.viewportBgBottom
                }
            }
        }
    }

    Item {
        id: viewportArea
        objectName: "viewportArea"
        anchors.top: topBar.bottom
        anchors.bottom: statusBar.top
        anchors.left: shell.leftPanelOpen ? leftPanel.right : parent.left
        anchors.right: shell.rightPanelOpen ? rightPanel.left : parent.right
        anchors.margins: shell.gap

        Item {
            id: workspaceHost
            objectName: "workspaceHost"
            anchors.fill: parent

            Repeater {
                model: AppContext.workspaceNames.length
                delegate: Loader {
                    required property int index
                    readonly property bool current: AppContext.workspace === index
                    property bool used: current
                    objectName: "workspace." + index
                    anchors.fill: parent
                    onCurrentChanged: if (current) used = true
                    active: used
                    visible: current
                    // Each workspace's own tools arrive with their tasks (CAM: T-044).
                    sourceComponent: Item {}
                }
            }
        }

        Column {
            id: emptyState
            objectName: "emptyState"
            anchors.centerIn: parent
            width: Math.min(parent.width - 2 * Theme.spacingL, Theme.controlHeight * 14)
            spacing: Theme.spacingS
            visible: parent.height > implicitHeight

            Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                name: "cube"
                size: Theme.controlHeight * 1.5
                color: Theme.textSecondary
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                font: Theme.titleFont
                text: qsTr("No document open")
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                secondary: true
                wrapMode: Text.Wrap
                text: qsTr("Open a STEP or RayCAD file, or start a new part.")
            }
            Item {
                width: 1
                height: Theme.spacingS
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingS

                Button {
                    objectName: "openButton"
                    primary: true
                    text: qsTr("Open…")
                    enabled: AppContext.hasSession
                    onClicked: AppContext.invoke("file.open")
                }
                Button {
                    objectName: "newPartButton"
                    text: qsTr("New part")
                    iconName: "plus"
                    enabled: AppContext.hasSession
                    onClicked: AppContext.invoke("file.new")
                }
            }
        }

        // Orientation hint until the view cube of T-020 exists.
        Item {
            id: axisTriad
            objectName: "axisTriad"
            visible: viewportPlaceholder.visible
            width: Theme.controlHeight * 2
            height: width
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            readonly property real cx: width / 2
            readonly property real cy: height * 0.58
            readonly property real len: width * 0.32
            Accessible.ignored: true

            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: Theme.axisX
                    strokeWidth: Theme.lineThinPx
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    startX: axisTriad.cx
                    startY: axisTriad.cy
                    PathLine {
                        x: axisTriad.cx - axisTriad.len * 0.87
                        y: axisTriad.cy + axisTriad.len * 0.5
                    }
                }
                ShapePath {
                    strokeColor: Theme.axisY
                    strokeWidth: Theme.lineThinPx
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    startX: axisTriad.cx
                    startY: axisTriad.cy
                    PathLine {
                        x: axisTriad.cx + axisTriad.len * 0.87
                        y: axisTriad.cy + axisTriad.len * 0.5
                    }
                }
                ShapePath {
                    strokeColor: Theme.axisZ
                    strokeWidth: Theme.lineThinPx
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    startX: axisTriad.cx
                    startY: axisTriad.cy
                    PathLine {
                        x: axisTriad.cx
                        y: axisTriad.cy - axisTriad.len
                    }
                }
            }
            Text {
                text: "X"
                font: Theme.smallFont
                color: Theme.axisX
                x: axisTriad.cx - axisTriad.len * 0.87 - width - Theme.borderWidth
                y: axisTriad.cy + axisTriad.len * 0.5 - height / 2
            }
            Text {
                text: "Y"
                font: Theme.smallFont
                color: Theme.axisY
                x: axisTriad.cx + axisTriad.len * 0.87 + Theme.borderWidth * 2
                y: axisTriad.cy + axisTriad.len * 0.5 - height / 2
            }
            Text {
                text: "Z"
                font: Theme.smallFont
                color: Theme.axisZ
                x: axisTriad.cx - width / 2
                y: axisTriad.cy - axisTriad.len - height
            }
        }
    }

    // ---------- Top bar ----------
    PanelBase {
        id: topBar
        objectName: "topBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: shell.gap
        height: Theme.topBarHeight

        Row {
            id: brand
            objectName: "brand"
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingM
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingS

            // RayCAD's mark: a geometric R on the accent colour (drawn for this project).
            Rectangle {
                width: Theme.controlHeightSmall
                height: width
                radius: Theme.radiusSmall
                color: Theme.accent
                anchors.verticalCenter: parent.verticalCenter
                Accessible.ignored: true

                // Drawn on a 16 x 16 grid, filling about three quarters of the mark.
                Shape {
                    width: 16
                    height: 16
                    anchors.centerIn: parent
                    scale: parent.width * 0.72 / 16
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeColor: Theme.onAccent
                        strokeWidth: 1.75
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        joinStyle: ShapePath.RoundJoin
                        PathSvg {
                            path: "M 5.25 12.75 V 3.25 H 8.75 A 2.6 2.6 0 0 1 8.75 8.45 H 5.25 M 8.4 8.45 L 11.25 12.75"
                        }
                    }
                }
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                font: Theme.strongFont
                text: qsTr("RayCAD")
            }
        }

        TabBar {
            id: workspaceTabs
            objectName: "workspaceTabs"
            anchors.left: brand.right
            anchors.leftMargin: Theme.spacingL
            anchors.verticalCenter: parent.verticalCenter
            currentIndex: AppContext.workspace
            onCurrentIndexChanged: AppContext.workspace = currentIndex
            Accessible.name: qsTr("Workspaces")

            Repeater {
                model: AppContext.workspaceNames
                delegate: TabButton {
                    required property string modelData
                    required property int index
                    objectName: "workspaceTab." + index
                    text: modelData
                }
            }
        }

        TextField {
            id: commandSearch
            objectName: "commandSearch"
            readonly property real leftEdge: workspaceTabs.x + workspaceTabs.width + Theme.spacingL
            readonly property real rightEdge: rightTools.x - Theme.spacingL
            width: Math.max(0, Math.min(Theme.controlHeight * 14, rightEdge - leftEdge))
            x: Math.min(Math.max(leftEdge, Math.round((topBar.width - width) / 2)), rightEdge - width)
            anchors.verticalCenter: parent.verticalCenter
            visible: width >= Theme.controlHeight * 5
            iconName: "search"
            placeholderText: qsTr("Search commands")
            Accessible.name: qsTr("Search commands")
            onActiveFocusChanged: if (activeFocus) shell.commandSearchRequested(text)
            onAccepted: shell.commandSearchRequested(text)
            Keys.onEscapePressed: {
                text = ""
                focus = false
            }
        }

        Row {
            id: rightTools
            objectName: "viewTools"
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacingS
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingXs / 2

            ToolButton {
                objectName: "toggleLeftPanel"
                iconName: "panel-left"
                toolTipText: shell.leftPanelOpen ? qsTr("Hide the model panel") : qsTr("Show the model panel")
                Accessible.checkable: true
                Accessible.checked: shell.leftPanelOpen
                onClicked: shell.leftPanelOpen = !shell.leftPanelOpen
            }
            ToolButton {
                objectName: "toggleRightPanel"
                iconName: "panel-right"
                toolTipText: shell.rightPanelOpen ? qsTr("Hide the properties panel")
                                                  : qsTr("Show the properties panel")
                Accessible.checkable: true
                Accessible.checked: shell.rightPanelOpen
                onClicked: shell.rightPanelOpen = !shell.rightPanelOpen
            }
            Rectangle {
                width: Theme.borderWidth
                height: Theme.controlHeightSmall
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.panelBorder
            }
            ToolButton {
                objectName: "themeToggle"
                iconName: Theme.dark ? "moon" : "sun"
                toolTipText: Theme.dark ? qsTr("Switch to the light theme") : qsTr("Switch to the dark theme")
                onClicked: Theme.mode = Theme.dark ? "light" : "dark"
            }
            ToolButton {
                objectName: "glassToggle"
                iconName: "glass"
                checked: AppContext.glass
                toolTipText: AppContext.glass ? qsTr("Glass panels on (switch to flat)")
                                              : qsTr("Flat panels (switch to glass)")
                Accessible.checkable: true
                Accessible.checked: checked
                onClicked: AppContext.glass = !AppContext.glass
            }
            ToolButton {
                id: viewOptions
                objectName: "viewOptions"
                iconName: "more"
                toolTipText: qsTr("View options")
                onClicked: shell.openViewMenu()
            }
        }
    }

    Loader {
        id: viewMenuLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias menu: menu
                Menu {
                    id: menu
                    objectName: "viewOptionsMenu"
                    MenuItem {
                        text: qsTr("Follow the system theme")
                        marked: Theme.mode === "system"
                        onTriggered: Theme.mode = "system"
                    }
                    MenuItem {
                        text: qsTr("Light theme")
                        marked: Theme.mode !== "system" && !Theme.dark
                        onTriggered: Theme.mode = "light"
                    }
                    MenuItem {
                        text: qsTr("Dark theme")
                        marked: Theme.mode !== "system" && Theme.dark
                        onTriggered: Theme.mode = "dark"
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("Glass panels")
                        marked: AppContext.glass
                        onTriggered: AppContext.glass = !AppContext.glass
                    }
                    MenuItem {
                        text: qsTr("Model panel")
                        marked: shell.leftPanelOpen
                        onTriggered: shell.leftPanelOpen = !shell.leftPanelOpen
                    }
                    MenuItem {
                        text: qsTr("Properties panel")
                        marked: shell.rightPanelOpen
                        onTriggered: shell.rightPanelOpen = !shell.rightPanelOpen
                    }
                }
            }
        }
    }

    // ---------- Side panels ----------
    PanelBase {
        id: leftPanel
        objectName: "leftPanel"
        visible: shell.leftPanelOpen
        anchors.top: topBar.bottom
        anchors.bottom: statusBar.top
        anchors.left: parent.left
        anchors.topMargin: shell.gap
        anchors.bottomMargin: shell.gap
        anchors.leftMargin: shell.gap
        width: shell.clampPanelWidth(shell.leftPanelWidth)

        PanelHeader {
            id: leftHeader
            objectName: "leftPanelHeader"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            title: qsTr("Model")
            iconName: "cube"

            ToolButton {
                objectName: "leftPanelClose"
                compact: true
                iconName: "close"
                toolTipText: qsTr("Hide the model panel")
                onClicked: shell.leftPanelOpen = false
            }
        }
        Item {
            id: leftPanelContent
            objectName: "leftPanelContent"
            anchors.top: leftHeader.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            Label {
                id: leftPanelEmpty
                objectName: "leftPanelEmpty"
                anchors.fill: parent
                anchors.margins: Theme.spacingL
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
                secondary: true
                text: qsTr("Features and bodies of the open document appear here.")
            }
        }
    }

    SplitterHandle {
        objectName: "leftPanelSplitter"
        visible: shell.leftPanelOpen
        anchors.left: leftPanel.right
        anchors.top: leftPanel.top
        anchors.bottom: leftPanel.bottom
        width: shell.gap
        accessibleName: qsTr("Resize the model panel")
        onMoved: delta => shell.leftPanelWidth = shell.clampPanelWidth(leftPanel.width + delta)
        onReset: shell.leftPanelWidth = Theme.sidePanelWidth
    }

    PanelBase {
        id: rightPanel
        objectName: "rightPanel"
        visible: shell.rightPanelOpen
        anchors.top: topBar.bottom
        anchors.bottom: statusBar.top
        anchors.right: parent.right
        anchors.topMargin: shell.gap
        anchors.bottomMargin: shell.gap
        anchors.rightMargin: shell.gap
        width: shell.clampPanelWidth(shell.rightPanelWidth)

        PanelHeader {
            id: rightHeader
            objectName: "rightPanelHeader"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            title: qsTr("Properties")
            iconName: "sliders"

            ToolButton {
                objectName: "rightPanelClose"
                compact: true
                iconName: "close"
                toolTipText: qsTr("Hide the properties panel")
                onClicked: shell.rightPanelOpen = false
            }
        }
        Item {
            id: rightPanelContent
            objectName: "rightPanelContent"
            anchors.top: rightHeader.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            Label {
                id: rightPanelEmpty
                objectName: "rightPanelEmpty"
                anchors.fill: parent
                anchors.margins: Theme.spacingL
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
                secondary: true
                text: qsTr("Select a feature, face or operation to see its properties.")
            }
        }
    }

    SplitterHandle {
        objectName: "rightPanelSplitter"
        visible: shell.rightPanelOpen
        anchors.right: rightPanel.left
        anchors.top: rightPanel.top
        anchors.bottom: rightPanel.bottom
        width: shell.gap
        accessibleName: qsTr("Resize the properties panel")
        onMoved: delta => shell.rightPanelWidth = shell.clampPanelWidth(rightPanel.width - delta)
        onReset: shell.rightPanelWidth = Theme.sidePanelWidth
    }

    // ---------- Status bar ----------
    PanelBase {
        id: statusBar
        objectName: "statusBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: shell.gap
        height: Theme.statusBarHeight

        Row {
            id: statusLeft
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingM
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingS

            Rectangle {
                width: Theme.spacingS
                height: width
                radius: width / 2
                anchors.verticalCenter: parent.verticalCenter
                color: AppContext.hasSession ? Theme.success : Theme.textDisabled
                Accessible.ignored: true
            }
            Label {
                id: statusText
                objectName: "statusText"
                anchors.verticalCenter: parent.verticalCenter
                text: AppContext.statusText !== "" ? AppContext.statusText
                      : AppContext.hasSession ? qsTr("Ready")
                      : qsTr("No session")
            }
        }

        Row {
            id: jobSlot
            objectName: "jobSlot"
            visible: AppContext.jobActive
            anchors.left: statusLeft.right
            anchors.leftMargin: Theme.spacingL
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingS

            Label {
                objectName: "jobTitle"
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: AppContext.jobTitle
            }
            ProgressBar {
                objectName: "jobProgress"
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.controlHeight * 5
                indeterminate: AppContext.jobProgress < 0
                value: Math.max(0, AppContext.jobProgress)
                Accessible.name: AppContext.jobTitle
            }
            ToolButton {
                objectName: "jobCancel"
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "close"
                toolTipText: qsTr("Cancel")
                onClicked: AppContext.cancelJob()
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacingM
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingL

            Label {
                objectName: "statusLook"
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: Theme.title
            }
            Label {
                objectName: "statusVersion"
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: qsTr("Version %1").arg(AppContext.version)
            }
        }
    }
}
