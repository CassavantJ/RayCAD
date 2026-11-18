// Controls gallery (raycad --gallery): every base control in its main states, for visual checks and for the
// scale tests in tests/ui/controls. Items named "gallery.*" are checked for clipping and elided text.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

FocusScope {
    id: gallery
    objectName: "gallery"

    readonly property real cardWidth: Math.floor((column.width - Theme.spacingL) / 2)

    // Opens the demo menu and tooltip (screenshots and tests look at them too).
    function openPopups() {
        demoMenu.popup(menuButton, 0, menuButton.height + Theme.spacingXs)
        demoToolTip.open()
    }

    Component.onCompleted: if (AppContext.screenshotMode) Qt.callLater(openPopups)

    component Card: PanelBase {
        id: card
        property string title
        default property alias body: cardBody.data
        width: gallery.cardWidth
        height: cardHeader.height + cardBody.implicitHeight + Theme.spacingM * 2

        PanelHeader {
            id: cardHeader
            anchors.left: parent.left
            anchors.right: parent.right
            title: card.title
        }
        Column {
            id: cardBody
            anchors.top: cardHeader.bottom
            anchors.topMargin: Theme.spacingM
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.spacingM
            anchors.rightMargin: Theme.spacingM
            spacing: Theme.spacingS
        }
    }

    component Strip: Flow {
        width: parent ? parent.width : 0
        spacing: Theme.spacingS
    }

    Rectangle {
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

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + 2 * Theme.spacingL
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        T.ScrollBar.vertical: ScrollBar {}

        Column {
            id: column
            x: Theme.spacingL
            y: Theme.spacingL
            width: flick.width - 2 * Theme.spacingL
            spacing: Theme.spacingL

            PanelBase {
                width: parent.width
                height: Theme.topBarHeight

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.spacingM
                    anchors.verticalCenter: parent.verticalCenter
                    font: Theme.titleFont
                    text: qsTr("Controls gallery")
                }
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingS
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spacingM

                    ComboBox {
                        objectName: "gallery.themeChoice"
                        anchors.verticalCenter: parent.verticalCenter
                        model: [qsTr("Follow system"), qsTr("Dark theme"), qsTr("Light theme")]
                        currentIndex: Theme.mode === "system" ? 0 : Theme.dark ? 1 : 2
                        onActivated: index => Theme.mode = ["system", "dark", "light"][index]
                    }
                    Switch {
                        objectName: "gallery.glassSwitch"
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Glass panels")
                        checked: AppContext.glass
                        onToggled: AppContext.glass = checked
                    }
                }
            }

            Flow {
                width: parent.width
                spacing: Theme.spacingL

                Card {
                    title: qsTr("Buttons")
                    Strip {
                        Button {
                            objectName: "gallery.primaryButton"
                            primary: true
                            text: qsTr("Generate toolpath")
                        }
                        Button {
                            objectName: "gallery.button"
                            text: qsTr("Cancel")
                        }
                        Button {
                            objectName: "gallery.iconButton"
                            iconName: "plus"
                            text: qsTr("Add setup")
                        }
                        Button {
                            objectName: "gallery.flatButton"
                            flat: true
                            text: qsTr("Details")
                        }
                        Button {
                            objectName: "gallery.disabledButton"
                            enabled: false
                            text: qsTr("Post")
                        }
                    }
                    Strip {
                        ToolButton {
                            objectName: "gallery.toolButton"
                            iconName: "search"
                            toolTipText: qsTr("Search")
                        }
                        ToolButton {
                            objectName: "gallery.toolButtonChecked"
                            iconName: "glass"
                            checkable: true
                            checked: true
                            toolTipText: qsTr("Glass panels")
                        }
                        ToolButton {
                            objectName: "gallery.toolButtonText"
                            iconName: "cube"
                            text: qsTr("Fit")
                            toolTipText: qsTr("Fit the view")
                        }
                        ToolButton {
                            objectName: "gallery.toolButtonCompact"
                            compact: true
                            iconName: "close"
                            toolTipText: qsTr("Close")
                        }
                        ToolButton {
                            objectName: "gallery.toolButtonDisabled"
                            iconName: "more"
                            enabled: false
                            toolTipText: qsTr("More")
                        }
                        Button {
                            id: menuButton
                            objectName: "gallery.menuButton"
                            text: qsTr("Open menu")
                            onClicked: gallery.openPopups()
                        }
                        Button {
                            id: toolTipButton
                            objectName: "gallery.toolTipButton"
                            text: qsTr("Tooltip")
                            toolTipText: qsTr("Stepover as a percentage of the tool diameter")
                        }
                    }
                }

                Card {
                    title: qsTr("Fields")
                    Strip {
                        TextField {
                            objectName: "gallery.textField"
                            placeholderText: qsTr("Operation name")
                        }
                        TextField {
                            objectName: "gallery.searchField"
                            iconName: "search"
                            placeholderText: qsTr("Search tools")
                        }
                        TextField {
                            objectName: "gallery.filledField"
                            text: qsTr("Adaptive roughing")
                        }
                    }
                    Strip {
                        NumberField {
                            objectName: "gallery.numberMm"
                            label: qsTr("Depth")
                            value: 12.7
                            unit: "mm"
                        }
                        NumberField {
                            objectName: "gallery.numberDeg"
                            label: qsTr("Angle")
                            value: 45
                            decimals: 1
                            unit: "deg"
                        }
                        NumberField {
                            objectName: "gallery.numberRpm"
                            label: qsTr("Spindle speed")
                            value: 12000
                            decimals: 0
                            stepSize: 100
                            unit: "rpm"
                        }
                        TextField {
                            objectName: "gallery.invalidField"
                            invalid: true
                            text: qsTr("12,7,0")
                        }
                        TextField {
                            objectName: "gallery.disabledField"
                            enabled: false
                            placeholderText: qsTr("Disabled")
                        }
                    }
                }

                Card {
                    title: qsTr("Choices")
                    Strip {
                        CheckBox {
                            objectName: "gallery.checkBoxChecked"
                            text: qsTr("Climb milling")
                            checked: true
                        }
                        CheckBox {
                            objectName: "gallery.checkBox"
                            text: qsTr("Coolant")
                        }
                        CheckBox {
                            objectName: "gallery.checkBoxPartial"
                            text: qsTr("All bodies")
                            tristate: true
                            checkState: Qt.PartiallyChecked
                        }
                        CheckBox {
                            objectName: "gallery.checkBoxDisabled"
                            text: qsTr("Rest machining")
                            enabled: false
                        }
                    }
                    Strip {
                        Switch {
                            objectName: "gallery.switchOn"
                            text: qsTr("Show rapids")
                            checked: true
                        }
                        Switch {
                            objectName: "gallery.switch"
                            text: qsTr("Show stock")
                        }
                        Switch {
                            objectName: "gallery.switchDisabled"
                            text: qsTr("Collision check")
                            enabled: false
                            checked: true
                        }
                    }
                    Strip {
                        ComboBox {
                            objectName: "gallery.comboBox"
                            model: [qsTr("Adaptive clearing"), qsTr("Pocket"), qsTr("Contour"), qsTr("Face")]
                        }
                        ComboBox {
                            objectName: "gallery.comboBoxDisabled"
                            enabled: false
                            model: [qsTr("Haas NGC")]
                        }
                    }
                }

                Card {
                    title: qsTr("Ranges and tabs")
                    Strip {
                        Slider {
                            objectName: "gallery.slider"
                            from: 0
                            to: 100
                            value: 40
                            Accessible.name: qsTr("Stepover")
                        }
                        Slider {
                            objectName: "gallery.sliderDisabled"
                            enabled: false
                            value: 0.7
                            Accessible.name: qsTr("Opacity")
                        }
                    }
                    Strip {
                        ProgressBar {
                            objectName: "gallery.progress"
                            value: 0.62
                        }
                    }
                    Strip {
                        TabBar {
                            objectName: "gallery.tabBar"
                            TabButton {
                                text: qsTr("Model")
                            }
                            TabButton {
                                text: qsTr("Operations")
                            }
                            TabButton {
                                text: qsTr("Tools")
                            }
                        }
                    }
                }

                Card {
                    title: qsTr("Tree rows")
                    TreeRow {
                        objectName: "gallery.treeRoot"
                        width: parent.width
                        depth: 0
                        expandable: true
                        expanded: true
                        iconName: "cube"
                        text: qsTr("Bracket")
                        onExpandToggled: expanded = !expanded
                    }
                    TreeRow {
                        objectName: "gallery.treeImport"
                        width: parent.width
                        depth: 1
                        iconName: "document"
                        text: qsTr("Import bracket.step")
                        secondaryText: qsTr("1 body")
                    }
                    TreeRow {
                        objectName: "gallery.treeSelected"
                        width: parent.width
                        depth: 1
                        expandable: true
                        highlighted: true
                        iconName: "cube"
                        text: qsTr("Body 1")
                    }
                    TreeRow {
                        objectName: "gallery.treeLeaf"
                        width: parent.width
                        depth: 2
                        text: qsTr("Fillet")
                        secondaryText: "R 3.000"
                    }
                    TreeRow {
                        objectName: "gallery.treeDisabled"
                        width: parent.width
                        depth: 2
                        enabled: false
                        text: qsTr("Chamfer (suppressed)")
                    }
                }

                Card {
                    title: qsTr("Panels and text")
                    PanelBase {
                        objectName: "gallery.innerPanel"
                        width: parent.width
                        height: innerHeader.height + Theme.controlHeight * 2
                        PanelHeader {
                            id: innerHeader
                            objectName: "gallery.panelHeader"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            title: qsTr("Setup 1")
                            iconName: "sliders"
                            ToolButton {
                                compact: true
                                iconName: "plus"
                                toolTipText: qsTr("Add")
                            }
                            ToolButton {
                                compact: true
                                iconName: "close"
                                toolTipText: qsTr("Close")
                            }
                        }
                        Row {
                            anchors.top: innerHeader.bottom
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right

                            Item {
                                width: (parent.width - splitter.width) / 2
                                height: parent.height
                                Label {
                                    anchors.centerIn: parent
                                    secondary: true
                                    text: qsTr("Left")
                                }
                            }
                            SplitterHandle {
                                id: splitter
                                objectName: "gallery.splitter"
                                height: parent.height
                                accessibleName: qsTr("Resize")
                            }
                            Item {
                                width: (parent.width - splitter.width) / 2
                                height: parent.height
                                Label {
                                    anchors.centerIn: parent
                                    secondary: true
                                    text: qsTr("Right")
                                }
                            }
                        }
                    }
                    Label {
                        objectName: "gallery.labelPrimary"
                        text: qsTr("Primary text: Graphite theme, 13 px")
                    }
                    Label {
                        objectName: "gallery.labelSecondary"
                        secondary: true
                        text: qsTr("Secondary text for hints and units")
                    }
                    Label {
                        id: monoLabel
                        objectName: "gallery.labelMono"
                        mono: true
                        text: "X 12.700  Y -4.250  Z 0.500  F 1200"
                    }
                }

                Card {
                    title: qsTr("Scrolling list")
                    ListView {
                        id: demoList
                        objectName: "gallery.list"
                        width: parent.width
                        height: Theme.rowHeight * 5
                        clip: true
                        model: 24
                        boundsBehavior: Flickable.StopAtBounds
                        T.ScrollBar.vertical: ScrollBar {
                            objectName: "gallery.scrollBar"
                            policy: T.ScrollBar.AlwaysOn
                        }
                        delegate: TreeRow {
                            required property int index
                            width: ListView.view.width
                            text: qsTr("Operation %1").arg(index + 1)
                            secondaryText: (index % 3 === 0) ? qsTr("stale") : ""
                            highlighted: ListView.isCurrentItem
                            onClicked: ListView.view.currentIndex = index
                        }
                    }
                }
            }
        }
    }

    ToolTip {
        id: demoToolTip
        objectName: "gallery.toolTip"
        parent: monoLabel
        text: qsTr("Machine coordinates use the mono font")
        delay: 0
    }

    Menu {
        id: demoMenu
        objectName: "gallery.menu"
        MenuItem {
            text: qsTr("Cut")
            shortcutText: "Ctrl+X"
        }
        MenuItem {
            text: qsTr("Copy")
            shortcutText: "Ctrl+C"
        }
        MenuItem {
            text: qsTr("Paste")
            shortcutText: "Ctrl+V"
            enabled: false
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Show edges")
            checkable: true
            checked: true
        }
        MenuItem {
            text: qsTr("New setup")
            iconName: "plus"
        }
        Menu {
            title: qsTr("Export")
            MenuItem {
                text: qsTr("STEP")
            }
            MenuItem {
                text: qsTr("STL")
            }
        }
    }
}
