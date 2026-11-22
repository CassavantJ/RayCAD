// The application shell (PLAN.md 6.6): floating panels over a full-window viewport.
//
//   viewportHost      fills the window, under everything: Viewport.qml (T-020), which also takes dropped files.
//   viewportArea      the part of the viewport no panel covers: overlays go here. T-020: the view cube with its
//                     buttons and the axis triad (ViewCube.qml), the selection filter (SelectionFilterBar.qml) and
//                     messageBanner. T-019: StartScreen.qml while no document is open, the read-only notice and
//                     the jobs panel (panels/JobsPanel.qml).
//   topBar            brand, the File menu (T-019), workspace tabs (AppContext.workspace), command search (T-021:
//                     opens the command palette), view toggles; the view options menu also picks the projection and
//                     the navigation preset, and opens the palette and the keyboard shortcut settings.
//   commandHost       T-021: CommandHost.qml, the palette and the shortcut settings (Loaders); ContextMenu.qml over
//                     viewportArea and the model tree opens the right-click menus.
//   leftPanel         "Model": ModelTree.qml in leftPanelContent.
//   sketch            T-092: sketch/SketchLayer.qml over the view (in viewportHost) and sketch/SketchPanel.qml (tools,
//                     plane picker, status) in viewportArea.
//   rightPanel        "Properties": SelectionInfo.qml in rightPanelContent; T-022: a second tab, the Measure panel
//                     (panels/Measure.qml), and the measurement labels over the view (panels/MeasureOverlay.qml).
//   statusBar         status text (AppContext.statusText) and jobSlot (the foreground job of Session.jobs, T-019).
//   fileDialogs       the native file dialogs, the unsaved-changes prompt and crash recovery (dialogs/, T-019).
//   workspace.<n>     one Loader per workspace, created the first time that workspace is shown.
// Only what this first screen shows is created at start; menus and tooltips are created on first use.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

FocusScope {
    id: shell
    objectName: "shell"

    // T-024: the panels open as they were left (AppSettings); changes are saved.
    property bool leftPanelOpen: AppSettings.panelOpen("model")
    property bool rightPanelOpen: AppSettings.panelOpen("properties")
    property bool jobsPanelOpen: false
    // T-022: the right panel's tab: 0 properties, 1 measure (T-024: restored and saved like the panels).
    property int rightTab: AppSettings.value("panels.properties.tab") === "measure" ? 1 : 0
    property real leftPanelWidth: AppSettings.panelWidth("model", Theme.sidePanelWidth)
    property real rightPanelWidth: AppSettings.panelWidth("properties", Theme.sidePanelWidth)
    onLeftPanelOpenChanged: AppSettings.setPanelOpen("model", leftPanelOpen)
    onRightPanelOpenChanged: AppSettings.setPanelOpen("properties", rightPanelOpen)
    onRightTabChanged: {
        const tab = rightTab === 1 ? "measure" : "properties"
        if (AppSettings.value("panels.properties.tab") !== tab)
            AppSettings.set("panels.properties.tab", tab)
    }
    readonly property real gap: Theme.panelGap

    readonly property alias viewportHost: viewportHost
    readonly property alias viewportArea: viewportArea
    readonly property alias leftPanelContent: leftPanelContent
    readonly property alias rightPanelContent: rightPanelContent
    readonly property alias commandSearch: commandSearch
    readonly property alias jobSlot: jobSlot

    // The command search field took focus or the user pressed Enter in it (T-021 opens the palette).
    signal commandSearchRequested(string text)
    onCommandSearchRequested: text => commandHost.openPalette(text)

    function clampPanelWidth(w) {
        return Math.round(Math.max(Theme.sidePanelMinWidth, Math.min(Theme.sidePanelMaxWidth, width / 3, w)))
    }

    function openViewMenu() {
        viewMenuLoader.active = true
        const menu = viewMenuLoader.item.menu
        menu.popup(viewOptions, viewOptions.width - menu.width, viewOptions.height + Theme.spacingXs)
    }

    function openFileMenu() {
        fileMenuLoader.active = true
        fileMenuLoader.item.menu.popup(fileMenuButton, 0, fileMenuButton.height + Theme.spacingXs)
    }

    // T-024: panels/Settings.qml, created on first use (view options menu, ui.settings, Ctrl+,).
    function openSettings(section) {
        viewport.view.forceActiveFocus()
        settingsLoader.active = true
        settingsLoader.item.open(section || "")
    }
    Connections {
        target: AppSettings
        function onOpenRequested() {
            shell.openSettings()
        }
    }
    Loader {
        id: settingsLoader
        objectName: "settingsLoader"
        active: false
        sourceComponent: Component {
            Settings {
                onClosed: viewport.view.forceActiveFocus()
                onShortcutsRequested: Commands.openShortcutSettings()
            }
        }
    }

    // ---------- Viewport (full window, under every panel) ----------
    Item {
        id: viewportHost
        objectName: "viewportHost"
        anchors.fill: parent

        Viewport {
            id: viewport
            anchors.fill: parent
            // viewportHost fills the shell, so viewportArea's geometry is already in this item's coordinates.
            frameRect: Qt.rect(viewportArea.x, viewportArea.y, viewportArea.width, viewportArea.height)
        }
        // T-022: measurement labels over the model (same geometry as the view item).
        MeasureOverlay {
            anchors.fill: parent
            view: viewport.view
            visible: Scene.bodyCount > 0
        }
        // T-092: the sketch being edited, its pointer and keys (sketch/SketchLayer.qml).
        SketchLayer {
            anchors.fill: parent
            view: viewport.view
        }
        // T-098: the feature tools' drag handles over the view (model/FeatureLayer.qml).
        FeatureLayer {
            anchors.fill: parent
            view: viewport.view
        }
    }

    // T-022: the M key and the measure commands show the Measure tab; it draws its witness while shown.
    Connections {
        target: MeasureTool
        function onShowRequested() {
            shell.rightPanelOpen = true
            shell.rightTab = 1
        }
    }
    Binding {
        target: MeasureTool
        property: "active"
        value: shell.rightPanelOpen && shell.rightTab === 1
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
            // A workspace's panels go over the view's own overlays (the view cube's axis triad in the bottom left
            // corner), as the Design workspace's feature panel does.
            z: 1

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
                    // Each workspace's own tools arrive with their tasks. CAM (T-044): the setup panel; the operation
                    // panel (T-045), the tool library (T-046) and posting (T-047) go into camWorkspace beside it.
                    sourceComponent: index === 1 ? camWorkspace : emptyWorkspace
                }
            }
            Component {
                id: emptyWorkspace
                Item {}
            }
            Component {
                id: camWorkspace
                Item {
                    objectName: "camWorkspace"
                    // T-044: cam/SetupPanel.qml (and its markers and labels over the view).
                    SetupPanel {
                        anchors.fill: parent
                        view: viewport.view
                    }
                    // T-045: cam/OperationPanel.qml, in the same place (one of the two is open at a time). The tool
                    // library (T-046) and posting (T-047) go here beside them.
                    OperationPanel {
                        anchors.fill: parent
                        view: viewport.view
                    }
                    // T-045: the selected operation's toolpath in the view (CamToolpaths draws into the viewport).
                    readonly property int toolpathsShown: CamToolpaths.shownCount
                    // T-047: the G-code viewer on the right, and the post dialog over everything.
                    GcodeView {
                        anchors.fill: parent
                    }
                    PostDialog {}
                }
            }
        }
        // T-044: the model tools and panels are the Design workspace's; entering CAM closes an open feature tool
        // (nothing changes in the document) and finishes the sketch being edited (its steps are saved already).
        readonly property bool camShown: AppContext.workspace === 1
        onCamShownChanged: {
            if (!camShown)
                return
            if (FeatureTool.active)
                FeatureTool.cancel()
            if (SketchTool.active)
                Commands.trigger("sketch.mode.finish")
        }

        // T-021: right-click (press and release without dragging) opens the context menu.
        ContextMenu {
            objectName: "viewportContextMenu"
            anchors.fill: parent
        }

        // T-019: recent files, New part and Open while no document is open.
        StartScreen {
            id: startScreen
            anchors.fill: parent
            visible: !Session.hasDocument
        }

        // The view cube, the view buttons and the axis triad (T-020).
        ViewCube {
            anchors.fill: parent
            view: viewport.view
            visible: Session.hasDocument || !AppContext.hasSession
        }

        SelectionFilterBar {
            id: selectionFilterBar
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            visible: Scene.bodyCount > 0 && !SketchTool.active
        }

        // T-092: New sketch, the plane picker, the sketch tools and the sketch's status (sketch/SketchPanel.qml).
        SketchPanel {
            anchors.fill: parent
            visible: !viewportArea.camShown // T-044: Design only
        }
        // T-098: the open feature tool's property panel and the Extrude offer after a sketch (model/FeaturePanel.qml).
        FeaturePanel {
            anchors.fill: parent
            visible: !viewportArea.camShown
        }

        // T-019: someone else holds the document's write lock.
        ReadOnlyNotice {
            anchors.top: selectionFilterBar.visible ? selectionFilterBar.bottom : parent.top
            anchors.topMargin: selectionFilterBar.visible ? shell.gap : 0
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width - 2 * Theme.spacingL, implicitWidth)
            visible: Session.readOnly
        }

        // T-023: section view and draft analysis (panels/Section.qml), opened from the top bar and the commands.
        Section {
            anchors.left: parent.left
            anchors.top: selectionFilterBar.visible ? selectionFilterBar.bottom : parent.top
            anchors.topMargin: selectionFilterBar.visible ? shell.gap : 0
            width: Math.min(parent.width - 2 * Theme.spacingL, implicitWidth)
            // Never past the bottom of the view area (it scrolls instead) at small window sizes.
            height: Math.max(0, Math.min(implicitHeight, parent.height - anchors.topMargin
                                         - (selectionFilterBar.visible ? selectionFilterBar.height : 0)))
            visible: SectionView.panelOpen && Session.hasDocument
            onCloseRequested: SectionView.panelOpen = false
        }

        // T-019: every running job; opened from the status bar's job slot.
        JobsPanel {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            width: Math.min(parent.width - 2 * Theme.spacingL, implicitWidth)
            height: Math.min(parent.height / 2, implicitHeight)
            visible: shell.jobsPanelOpen && Session.jobCount > 0
            onCloseRequested: shell.jobsPanelOpen = false
        }

        // A dismissible message (a file that could not be opened, ...): AppContext.showMessage.
        PanelBase {
            id: messageBanner
            objectName: "messageBanner"
            z: 2 // over a workspace's panels (workspaceHost)
            visible: AppContext.message !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            width: Math.min(parent.width - 2 * Theme.spacingL, messageRow.implicitWidth + 2 * Theme.spacingS)
            height: messageRow.implicitHeight + 2 * Theme.spacingXs

            Rectangle {
                width: Theme.focusRingWidth
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: Theme.spacingXs
                radius: width / 2
                color: AppContext.messageIsError ? Theme.error : Theme.accent
            }
            Row {
                id: messageRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.spacingS
                anchors.rightMargin: Theme.spacingXs
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingS

                Label {
                    objectName: "messageText"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, messageBanner.parent.width - 2 * Theme.spacingL
                                    - messageClose.width - 3 * Theme.spacingS)
                    wrapMode: Text.Wrap
                    text: AppContext.message
                }
                ToolButton {
                    id: messageClose
                    objectName: "messageClose"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "close"
                    toolTipText: qsTr("Dismiss")
                    onClicked: AppContext.dismissMessage()
                }
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

        // T-019: the File menu (dialogs/FileMenu.qml), created on first use.
        ToolButton {
            id: fileMenuButton
            objectName: "fileMenuButton"
            anchors.left: brand.right
            anchors.leftMargin: Theme.spacingM
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("File")
            enabled: AppContext.hasSession
            toolTipText: qsTr("New, open, save, import and export")
            onClicked: shell.openFileMenu()
        }

        TabBar {
            id: workspaceTabs
            objectName: "workspaceTabs"
            anchors.left: fileMenuButton.right
            anchors.leftMargin: Theme.spacingM
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
            // T-021: Tab passes through the field; typing in it, a click or Enter opens the palette.
            onActiveFocusChanged: if (activeFocus) Qt.callLater(() => {
                // focusReason is up to date only after the focus change has been delivered.
                if (activeFocus && focusReason !== Qt.TabFocusReason && focusReason !== Qt.BacktabFocusReason)
                    shell.commandSearchRequested(text)
            })
            onTextEdited: if (text !== "") shell.commandSearchRequested(text)
            // After the press: the field takes the focus on press itself, which would take it back from the palette.
            onPressed: Qt.callLater(() => shell.commandSearchRequested(text))
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
            ToolButton { // T-023
                objectName: "toggleSectionPanel"
                iconName: "section"
                checked: SectionView.panelOpen
                enabled: Session.hasDocument
                toolTipText: SectionView.panelOpen ? qsTr("Hide section and draft analysis")
                                                   : qsTr("Section view and draft analysis")
                Accessible.checkable: true
                Accessible.checked: checked
                onClicked: SectionView.panelOpen = !SectionView.panelOpen
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
                    MenuSeparator {}
                    // T-021
                    MenuItem {
                        objectName: "openCommandPalette"
                        text: qsTr("Command palette")
                        shortcutText: Shortcuts.revision >= 0 ? Shortcuts.shortcutText("ui.commandPalette") : ""
                        onTriggered: Commands.openPalette()
                    }
                    MenuItem {
                        objectName: "openShortcutSettings"
                        text: qsTr("Keyboard shortcuts…")
                        shortcutText: Shortcuts.revision >= 0 ? Shortcuts.shortcutText("ui.keyboardShortcuts") : ""
                        onTriggered: Commands.openShortcutSettings()
                    }
                    // T-024
                    MenuItem {
                        objectName: "openSettings"
                        text: qsTr("Settings…")
                        shortcutText: Shortcuts.revision >= 0 ? Shortcuts.shortcutText("ui.settings") : ""
                        onTriggered: shell.openSettings()
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("Perspective projection")
                        marked: viewport.view.perspective
                        onTriggered: viewport.view.toggleProjection()
                    }
                    MenuSeparator {}
                    // Navigation presets (T-020): mouse mappings, see viewport_navigation.cpp.
                    MenuItem {
                        objectName: "navigationPreset.0"
                        text: qsTr("Mouse: %1").arg(viewport.view.navigationPresetNames[0])
                        marked: viewport.view.navigationPreset === 0
                        onTriggered: viewport.view.navigationPreset = 0
                    }
                    MenuItem {
                        objectName: "navigationPreset.1"
                        text: qsTr("Mouse: %1").arg(viewport.view.navigationPresetNames[1])
                        marked: viewport.view.navigationPreset === 1
                        onTriggered: viewport.view.navigationPreset = 1
                    }
                    MenuItem {
                        objectName: "navigationPreset.2"
                        text: qsTr("Mouse: %1").arg(viewport.view.navigationPresetNames[2])
                        marked: viewport.view.navigationPreset === 2
                        onTriggered: viewport.view.navigationPreset = 2
                    }
                }
            }
        }
    }

    Loader {
        id: fileMenuLoader
        active: false
        sourceComponent: Component {
            FileMenu {}
        }
    }

    // Esc cancels the foreground job wherever the focus is (only while one runs, so text fields keep their Esc,
    // and not under a dialog, whose Esc is Cancel).
    Shortcut {
        sequence: "Esc"
        enabled: Session.jobActive && !fileDialogs.modalShown
        onActivated: Session.cancelForegroundJob()
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
            // T-044: the CAM workspace shows its setups and operations here.
            title: viewportArea.camShown ? qsTr("CAM") : qsTr("Model")
            iconName: viewportArea.camShown ? "sliders" : "cube"

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

            ModelTree {
                id: modelTree
                anchors.fill: parent
                visible: !viewportArea.camShown
            }
            // T-021: right-click in the tree (on a body row: selects it first).
            ContextMenu {
                objectName: "treeContextMenu"
                anchors.fill: parent
                treeRoot: modelTree
                visible: !viewportArea.camShown
            }
            // T-044: cam/CamTree.qml, made the first time the CAM workspace is shown.
            Loader {
                objectName: "camTreeLoader"
                anchors.fill: parent
                property bool used: viewportArea.camShown
                onUsedChanged: if (viewportArea.camShown) used = true
                active: used
                visible: viewportArea.camShown
                sourceComponent: Component {
                    CamTree {}
                }
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
        onMoved: delta => {
            shell.leftPanelWidth = shell.clampPanelWidth(leftPanel.width + delta)
            AppSettings.setPanelWidth("model", shell.leftPanelWidth)
        }
        onReset: {
            shell.leftPanelWidth = Qt.binding(() => Theme.sidePanelWidth)
            AppSettings.setPanelWidth("model", -1)
        }
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
            title: shell.rightTab === 1 ? qsTr("Measure") : qsTr("Properties")
            iconName: shell.rightTab === 1 ? "ruler" : "sliders"

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

            // T-022: Properties | Measure.
            TabBar {
                id: rightTabs
                objectName: "rightPanelTabs"
                anchors.top: parent.top
                anchors.topMargin: Theme.spacingS
                anchors.horizontalCenter: parent.horizontalCenter
                currentIndex: shell.rightTab
                onCurrentIndexChanged: shell.rightTab = currentIndex
                Accessible.name: qsTr("Properties or measure")

                TabButton {
                    objectName: "rightTab.properties"
                    text: qsTr("Properties")
                }
                TabButton {
                    objectName: "rightTab.measure"
                    text: qsTr("Measure")
                }
            }
            SelectionInfo {
                anchors.top: rightTabs.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: shell.rightTab === 0
            }
            Measure {
                anchors.top: rightTabs.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: shell.rightTab === 1
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
        onMoved: delta => {
            shell.rightPanelWidth = shell.clampPanelWidth(rightPanel.width - delta)
            AppSettings.setPanelWidth("properties", shell.rightPanelWidth)
        }
        onReset: {
            shell.rightPanelWidth = Qt.binding(() => Theme.sidePanelWidth)
            AppSettings.setPanelWidth("properties", -1)
        }
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
            visible: Session.jobActive
            anchors.left: statusLeft.right
            anchors.leftMargin: Theme.spacingL
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingS

            Label {
                objectName: "jobTitle"
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: Session.jobTitle
            }
            ProgressBar {
                objectName: "jobProgress"
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.controlHeight * 5
                indeterminate: Session.jobProgress < 0
                value: Math.max(0, Session.jobProgress)
                Accessible.name: Session.jobTitle
            }
            ToolButton {
                objectName: "jobCancel"
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "close"
                toolTipText: qsTr("Cancel (Esc)")
                onClicked: Session.cancelForegroundJob()
            }
            ToolButton {
                objectName: "jobsToggle"
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: shell.jobsPanelOpen ? "chevron-down" : "chevron-up"
                text: Session.jobCount > 1 ? String(Session.jobCount) : ""
                toolTipText: shell.jobsPanelOpen ? qsTr("Hide the jobs") : qsTr("Show every running job")
                onClicked: shell.jobsPanelOpen = !shell.jobsPanelOpen
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacingM
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingL

            // T-024: the quick unit toggle of the open document (display only; Units, units.h).
            ToolButton {
                objectName: "unitsToggle"
                anchors.verticalCenter: parent.verticalCenter
                visible: Units.hasDocument
                compact: true
                text: Units.unitLabel
                checked: Units.overridden
                toolTipText: Units.system === "inch" ? qsTr("Show lengths in millimetres")
                                                     : qsTr("Show lengths in inches")
                Accessible.name: qsTr("Display units: %1").arg(Units.unitLabel)
                onClicked: Units.toggle()
            }
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

    // T-021: the command palette and the shortcut settings panel, created on first use.
    CommandHost {
        id: commandHost
        view: viewport.view
        searchField: commandSearch
    }

    // T-019: file dialogs, the unsaved-changes prompt and crash recovery, over everything.
    FileDialogs {
        id: fileDialogs
        anchors.fill: parent
        z: 10
    }
}
