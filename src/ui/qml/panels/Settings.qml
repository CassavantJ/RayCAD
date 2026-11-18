// The settings panel (task T-024): opened from the view options menu, the command palette (ui.settings) and Ctrl+,.
// Sections on the left, the chosen section's settings on the right:
//   General      units of new documents, display decimals for inch and mm, the display units of the open document
//                (the quick toggle), how many recent files the start screen lists
//   Appearance   theme (follow the system, dark, light) and glass panels
//   Graphics     the detected hardware, the tier in effect and why, the tier override, and every per-option
//                override of the tier preset, each with "reset to the tier default"
//   Navigation   the mouse mapping of the 3D view
//   Keyboard     opens the keyboard shortcut settings
// Every change applies at once and is saved per user (AppSettings, settings.h). Colours, fonts and sizes come from
// Theme only.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

Item {
    id: root

    readonly property alias popup: popup
    property string section: "general"
    readonly property var sections: [
        { id: "general", title: qsTr("General"), icon: "document" },
        { id: "appearance", title: qsTr("Appearance"), icon: "sun" },
        { id: "graphics", title: qsTr("Graphics"), icon: "glass" },
        { id: "navigation", title: qsTr("Navigation"), icon: "cube" },
        { id: "keyboard", title: qsTr("Keyboard"), icon: "sliders" }
    ]
    readonly property var values: AppSettings.values
    readonly property var defaults: AppSettings.tierDefaults
    readonly property var effective: AppSettings.effectivePreset

    signal closed()
    signal shortcutsRequested()

    function open(sectionId) {
        if (sectionId)
            section = sectionId
        popup.open()
        sectionList.forceActiveFocus()
        if (section === "graphics")
            AppSettings.requestHardware()
    }

    function showSection(sectionId) {
        section = sectionId
        sectionList.currentIndex = sections.findIndex(s => s.id === sectionId)
        content.contentY = 0
        if (sectionId === "graphics")
            AppSettings.requestHardware()
    }

    function onOff(v) {
        return v ? qsTr("on") : qsTr("off")
    }
    function tierTitle(t) {
        return t === "high" ? qsTr("High") : t === "medium" ? qsTr("Medium") : qsTr("Low")
    }
    function msaaText(n) {
        return n > 0 ? qsTr("%1x").arg(n) : qsTr("Off")
    }

    // One setting: its title and a line about it on the left, the control on the right, and for a setting that
    // has a tier default the default and a reset button underneath.
    component SettingRow: Item {
        id: row
        property string key: ""
        property string title: AppSettings.title(key)
        property string description: AppSettings.description(key)
        // The tier default as text ("" when the setting has none), and whether the user set the setting.
        property string defaultText: ""
        readonly property bool overridden: AppSettings.revision >= 0 && AppSettings.isSet(key)
        default property alias control: controlSlot.data

        objectName: "settingRow." + key
        width: parent ? parent.width : 0
        height: Math.max(textColumn.implicitHeight, controlSlot.childrenRect.height) + Theme.spacingS

        Column {
            id: textColumn
            anchors.left: parent.left
            anchors.right: controlSlot.left
            anchors.rightMargin: Theme.spacingM
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingXs / 2

            Label {
                width: parent.width
                wrapMode: Text.Wrap
                text: row.title
            }
            Label {
                width: parent.width
                visible: text !== ""
                secondary: true
                font: Theme.smallFont
                wrapMode: Text.Wrap
                text: row.description
            }
            Row {
                visible: row.defaultText !== ""
                spacing: Theme.spacingS
                height: visible ? Theme.controlHeightSmall : 0

                Label {
                    objectName: "settingDefault." + row.key
                    anchors.verticalCenter: parent.verticalCenter
                    secondary: true
                    font: Theme.smallFont
                    text: row.overridden ? qsTr("Tier default: %1").arg(row.defaultText)
                                         : qsTr("Follows the tier (%1)").arg(row.defaultText)
                }
                Button {
                    objectName: "settingReset." + row.key
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.overridden
                    flat: true
                    text: qsTr("Reset to tier default")
                    font: Theme.smallFont
                    onClicked: AppSettings.reset(row.key)
                }
            }
        }
        Item {
            id: controlSlot
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    component SectionTitle: Label {
        width: parent ? parent.width : 0
        topPadding: Theme.spacingS
        font: Theme.strongFont
    }

    component InfoRow: Item {
        property string label
        property string value
        property string name
        width: parent ? parent.width : 0
        height: Theme.rowHeight
        Label {
            id: infoKey
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width * 0.38
            secondary: true
            text: parent.label
        }
        Label {
            objectName: "hardware." + parent.name
            anchors.left: infoKey.right
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: parent.value
        }
    }

    T.Popup {
        id: popup
        objectName: "settingsPanel"
        parent: T.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.spacingL, Theme.controlHeight * 34)
        height: Math.min(parent.height - 2 * Theme.spacingL, Theme.controlHeight * 26)
        padding: 0
        modal: true
        dim: false
        focus: true
        closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside
        onClosed: root.closed()

        background: PanelBase {
            objectName: "settingsBackground"
        }

        contentItem: FocusScope {
            PanelHeader {
                id: header
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                title: qsTr("Settings")
                iconName: "sliders"

                ToolButton {
                    objectName: "settingsClose"
                    compact: true
                    iconName: "close"
                    toolTipText: qsTr("Close")
                    onClicked: popup.close()
                }
            }

            // ---- sections ----
            ListView {
                id: sectionList
                objectName: "settingsSections"
                anchors.left: parent.left
                anchors.top: header.bottom
                anchors.bottom: footer.top
                anchors.margins: Theme.spacingS
                width: Theme.controlHeight * 7
                focus: true
                clip: true
                interactive: false
                spacing: Theme.spacingXs / 2
                model: root.sections
                currentIndex: root.sections.findIndex(s => s.id === root.section)
                keyNavigationEnabled: true
                onCurrentIndexChanged: if (currentIndex >= 0 && root.sections[currentIndex].id !== root.section)
                                           root.showSection(root.sections[currentIndex].id)
                Accessible.role: Accessible.PageTabList
                Accessible.name: qsTr("Settings sections")

                delegate: Rectangle {
                    id: sectionRow
                    required property var modelData
                    required property int index
                    readonly property bool current: root.section === modelData.id
                    objectName: "settingsSection." + modelData.id
                    width: ListView.view.width
                    height: Theme.rowHeight + Theme.spacingXs
                    radius: Theme.radiusSmall
                    color: current ? Theme.controlRaised : sectionHover.hovered ? Theme.controlBgHover : "transparent"
                    border.width: current && !Theme.dark ? Theme.borderWidth : 0
                    border.color: Theme.panelBorder
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.title
                    Accessible.selected: current

                    IconText {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingS
                        alignment: Qt.AlignLeft
                        iconName: sectionRow.modelData.icon
                        iconColor: sectionRow.current ? Theme.accentText : Theme.textSecondary
                        text: sectionRow.modelData.title
                        font: sectionRow.current ? Theme.strongFont : Theme.uiFont
                        color: sectionRow.current ? Theme.textPrimary : Theme.textSecondary
                    }
                    HoverHandler {
                        id: sectionHover
                    }
                    TapHandler {
                        onTapped: root.showSection(sectionRow.modelData.id)
                    }
                    FocusRing {
                        visible: sectionList.activeFocus && sectionRow.current
                    }
                }
            }

            Rectangle {
                id: divider
                anchors.left: sectionList.right
                anchors.top: header.bottom
                anchors.bottom: footer.top
                anchors.leftMargin: Theme.spacingS
                anchors.topMargin: Theme.spacingS
                anchors.bottomMargin: Theme.spacingS
                width: Theme.borderWidth
                color: Theme.panelBorder
            }

            // ---- the section's settings ----
            Flickable {
                id: content
                objectName: "settingsContent"
                anchors.left: divider.right
                anchors.right: parent.right
                anchors.top: header.bottom
                anchors.bottom: footer.top
                anchors.leftMargin: Theme.spacingM
                anchors.rightMargin: Theme.spacingXs
                clip: true
                contentWidth: width
                contentHeight: pages.implicitHeight + 2 * Theme.spacingS
                boundsBehavior: Flickable.StopAtBounds
                T.ScrollBar.vertical: ScrollBar {}

                Item {
                    id: pages
                    y: Theme.spacingS
                    width: content.width - Theme.spacingM
                    implicitHeight: general.visible ? general.implicitHeight
                                  : appearance.visible ? appearance.implicitHeight
                                  : graphics.visible ? graphics.implicitHeight
                                  : navigation.visible ? navigation.implicitHeight
                                  : keyboard.implicitHeight

                    // ---------- General ----------
                    Column {
                        id: general
                        objectName: "settingsPage.general"
                        visible: root.section === "general"
                        width: parent.width
                        spacing: Theme.spacingXs

                        SectionTitle {
                            text: qsTr("Units")
                        }
                        SettingRow {
                            key: "units.newDocument"
                            ComboBox {
                                objectName: "setting.units.newDocument"
                                width: Theme.controlHeight * 7
                                model: [qsTr("Inch"), qsTr("Millimetre")]
                                currentIndex: root.values["units.newDocument"] === "metric" ? 1 : 0
                                onActivated: index => AppSettings.set("units.newDocument", ["inch", "metric"][index])
                            }
                        }
                        SettingRow {
                            key: "units.decimalsInch"
                            NumberField {
                                objectName: "setting.units.decimalsInch"
                                width: Theme.controlHeight * 4
                                from: 0
                                to: 8
                                decimals: 0
                                value: root.values["units.decimalsInch"]
                                label: qsTr("Decimals (inch)")
                                onValueModified: v => {
                                    AppSettings.set("units.decimalsInch", Math.round(v))
                                    value = Qt.binding(() => root.values["units.decimalsInch"])
                                }
                            }
                        }
                        SettingRow {
                            key: "units.decimalsMetric"
                            NumberField {
                                objectName: "setting.units.decimalsMetric"
                                width: Theme.controlHeight * 4
                                from: 0
                                to: 8
                                decimals: 0
                                value: root.values["units.decimalsMetric"]
                                label: qsTr("Decimals (mm)")
                                onValueModified: v => {
                                    AppSettings.set("units.decimalsMetric", Math.round(v))
                                    value = Qt.binding(() => root.values["units.decimalsMetric"])
                                }
                            }
                        }
                        SettingRow {
                            key: "displayUnits"
                            title: Units.hasDocument ? qsTr("Units shown for %1").arg(Session.documentTitle)
                                                     : qsTr("Units shown")
                            description: Units.hasDocument
                                         ? (Units.overridden
                                            ? qsTr("Switched for this session; the document is in %1.")
                                                  .arg(Units.documentSystem === "inch" ? qsTr("inches")
                                                                                       : qsTr("millimetres"))
                                            : qsTr("The document's own units. Switching changes only what is shown."))
                                         : qsTr("No document is open: this is the unit of new documents.")
                            TabBar {
                                objectName: "setting.displayUnits"
                                currentIndex: Units.system === "metric" ? 1 : 0
                                TabButton {
                                    objectName: "displayUnits.inch"
                                    text: qsTr("in")
                                    onClicked: if (Units.system !== "inch") Units.toggle()
                                }
                                TabButton {
                                    objectName: "displayUnits.metric"
                                    text: qsTr("mm")
                                    onClicked: if (Units.system !== "metric") Units.toggle()
                                }
                            }
                        }
                        Label {
                            objectName: "unitsExample"
                            width: parent.width
                            secondary: true
                            font: Theme.smallFont
                            text: Units.revision >= 0
                                  ? qsTr("Example: one inch shows as %1, a 50 mm square as %2.")
                                        .arg(Units.formatLength(25.4)).arg(Units.formatArea(2500))
                                  : ""
                        }
                        SectionTitle {
                            text: qsTr("Start screen")
                        }
                        SettingRow {
                            key: "files.recentShown"
                            NumberField {
                                objectName: "setting.files.recentShown"
                                width: Theme.controlHeight * 4
                                from: 1
                                to: 20
                                decimals: 0
                                value: root.values["files.recentShown"]
                                label: qsTr("Recent files shown")
                                onValueModified: v => {
                                    AppSettings.set("files.recentShown", Math.round(v))
                                    value = Qt.binding(() => root.values["files.recentShown"])
                                }
                            }
                        }
                    }

                    // ---------- Appearance ----------
                    Column {
                        id: appearance
                        objectName: "settingsPage.appearance"
                        visible: root.section === "appearance"
                        width: parent.width
                        spacing: Theme.spacingXs

                        SectionTitle {
                            text: qsTr("Appearance")
                        }
                        SettingRow {
                            key: "appearance.theme"
                            ComboBox {
                                objectName: "setting.appearance.theme"
                                width: Theme.controlHeight * 7
                                model: [qsTr("Follow the system"), qsTr("Dark"), qsTr("Light")]
                                currentIndex: Theme.mode === "system" ? 0 : Theme.dark ? 1 : 2
                                onActivated: index => AppSettings.set("appearance.theme",
                                                                      ["system", "dark", "light"][index])
                            }
                        }
                        SettingRow {
                            key: "appearance.glass"
                            defaultText: root.onOff(root.defaults["appearance.glass"])
                            Switch {
                                objectName: "setting.appearance.glass"
                                checked: AppContext.glass
                                text: checked ? qsTr("Glass") : qsTr("Flat")
                                onToggled: AppSettings.set("appearance.glass", checked)
                            }
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.Wrap
                            secondary: true
                            font: Theme.smallFont
                            text: qsTr("Panels keep the same layout in both styles; only their background changes.")
                        }
                    }

                    // ---------- Graphics ----------
                    Column {
                        id: graphics
                        objectName: "settingsPage.graphics"
                        visible: root.section === "graphics"
                        width: parent.width
                        spacing: Theme.spacingXs

                        SectionTitle {
                            text: qsTr("Detected hardware")
                        }
                        Label {
                            objectName: "hardwareDetecting"
                            visible: AppSettings.hardwareState !== "ready"
                            secondary: true
                            text: AppSettings.hardwareState === "detecting" ? qsTr("Detecting…")
                                                                            : qsTr("Not detected yet.")
                        }
                        Column {
                            objectName: "hardwareInfo"
                            visible: AppSettings.hardwareState === "ready"
                            width: parent.width
                            InfoRow {
                                name: "gpuName"
                                label: qsTr("Graphics adapter")
                                value: AppSettings.hardware.gpuName || ""
                            }
                            InfoRow {
                                name: "gpuType"
                                label: qsTr("Type")
                                value: AppSettings.hardware.gpuType || ""
                            }
                            InfoRow {
                                name: "vram"
                                label: qsTr("Video memory")
                                value: AppSettings.hardware.vram || ""
                            }
                            InfoRow {
                                name: "ram"
                                label: qsTr("Memory")
                                value: AppSettings.hardware.ram || ""
                            }
                            InfoRow {
                                name: "cores"
                                label: qsTr("CPU cores")
                                value: AppSettings.hardware.cores || ""
                            }
                            InfoRow {
                                name: "graphicsApi"
                                label: qsTr("Graphics API")
                                value: AppSettings.hardware.graphicsApi || ""
                            }
                            InfoRow {
                                name: "driver"
                                label: qsTr("Driver")
                                value: AppSettings.hardware.driver || ""
                            }
                            InfoRow {
                                name: "computeShaders"
                                label: qsTr("Compute shaders")
                                value: AppSettings.hardware.computeShaders || ""
                            }
                        }

                        SectionTitle {
                            text: qsTr("Tier")
                        }
                        Rectangle {
                            objectName: "tierSummary"
                            width: parent.width
                            height: tierText.implicitHeight + 2 * Theme.spacingS
                            radius: Theme.radiusSmall
                            color: Theme.controlBg
                            border.width: Theme.borderWidth
                            border.color: Theme.panelBorder

                            Label {
                                id: tierText
                                objectName: "tierReason"
                                anchors.fill: parent
                                anchors.margins: Theme.spacingS
                                wrapMode: Text.Wrap
                                text: {
                                    const tier = root.tierTitle(AppSettings.effectiveTier)
                                    if (AppSettings.hardwareState !== "ready")
                                        return AppSettings.tierForced
                                               ? qsTr("In use: %1, which you chose.").arg(tier)
                                               : qsTr("In use: %1 until the hardware check ends.").arg(tier)
                                    if (AppSettings.tierForced)
                                        return qsTr("In use: %1, which you chose. Automatic detection would choose %2")
                                            .arg(tier).arg(AppSettings.tierReason)
                                    return qsTr("In use: %1").arg(AppSettings.tierReason)
                                }
                            }
                        }
                        SettingRow {
                            key: "graphics.tier"
                            ComboBox {
                                objectName: "setting.graphics.tier"
                                width: Theme.controlHeight * 8
                                model: [AppSettings.detectedTier !== ""
                                            ? qsTr("Automatic (%1)").arg(root.tierTitle(AppSettings.detectedTier))
                                            : qsTr("Automatic"),
                                        qsTr("Low"), qsTr("Medium"), qsTr("High")]
                                currentIndex: ["auto", "low", "medium", "high"].indexOf(root.values["graphics.tier"])
                                onActivated: index => AppSettings.set("graphics.tier",
                                                                      ["auto", "low", "medium", "high"][index])
                            }
                        }

                        SectionTitle {
                            text: qsTr("Options")
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.Wrap
                            secondary: true
                            font: Theme.smallFont
                            text: qsTr("Each option follows the tier unless you change it. Changes apply at once.")
                        }
                        SettingRow {
                            key: "graphics.msaaSamples"
                            defaultText: root.msaaText(root.defaults["graphics.msaaSamples"])
                            ComboBox {
                                objectName: "setting.graphics.msaaSamples"
                                width: Theme.controlHeight * 5
                                model: [qsTr("Off"), qsTr("2x"), qsTr("4x"), qsTr("8x")]
                                currentIndex: [0, 2, 4, 8].indexOf(root.effective["graphics.msaaSamples"])
                                onActivated: index => AppSettings.set("graphics.msaaSamples", [0, 2, 4, 8][index])
                            }
                        }
                        Repeater {
                            model: ["graphics.smoothEdges", "graphics.transparency", "graphics.sectionCaps",
                                    "graphics.ambientOcclusion", "graphics.shadows"]
                            delegate: SettingRow {
                                id: boolRow
                                required property string modelData
                                key: modelData
                                defaultText: root.onOff(root.defaults[modelData])
                                Switch {
                                    objectName: "setting." + boolRow.modelData
                                    checked: root.effective[boolRow.modelData] === true
                                    onToggled: AppSettings.set(boolRow.modelData, checked)
                                }
                            }
                        }
                        Repeater {
                            model: [
                                { key: "graphics.meshChordTolMm", unit: "mm", decimals: 3, from: 0.001, to: 1, step: 0.01 },
                                { key: "graphics.meshAngleTolDeg", unit: "deg", decimals: 1, from: 1, to: 45, step: 1 },
                                { key: "graphics.simCellMm", unit: "mm", decimals: 3, from: 0.01, to: 1, step: 0.01 },
                                { key: "graphics.maxBackplotSegments", unit: "", decimals: 0, from: 100000,
                                  to: 100000000, step: 100000 }
                            ]
                            delegate: SettingRow {
                                id: numberRow
                                required property var modelData
                                key: modelData.key
                                defaultText: Number(root.defaults[modelData.key]).toFixed(modelData.decimals)
                                             + (modelData.unit !== "" ? " " + modelData.unit : "")
                                NumberField {
                                    objectName: "setting." + numberRow.modelData.key
                                    width: Theme.controlHeight * 5
                                    from: numberRow.modelData.from
                                    to: numberRow.modelData.to
                                    stepSize: numberRow.modelData.step
                                    decimals: numberRow.modelData.decimals
                                    unit: numberRow.modelData.unit
                                    label: AppSettings.title(numberRow.modelData.key)
                                    value: root.effective[numberRow.modelData.key]
                                    onValueModified: v => {
                                        AppSettings.set(numberRow.modelData.key, v)
                                        // Keep following the setting (a reset puts the tier default back here).
                                        value = Qt.binding(() => root.effective[numberRow.modelData.key])
                                    }
                                }
                            }
                        }
                        Item {
                            width: parent.width
                            height: resetGraphics.implicitHeight + Theme.spacingS
                            Button {
                                id: resetGraphics
                                objectName: "settingsResetGraphics"
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                text: qsTr("Reset all graphics settings")
                                enabled: AppSettings.setKeys.some(k => k.startsWith("graphics.")
                                                                  && !k.startsWith("graphics.last"))
                                         || AppSettings.setKeys.indexOf("appearance.glass") >= 0
                                onClicked: {
                                    for (const k of AppSettings.setKeys.slice())
                                        if (k.startsWith("graphics.") && !k.startsWith("graphics.last"))
                                            AppSettings.reset(k)
                                    AppSettings.reset("appearance.glass")
                                }
                            }
                        }
                    }

                    // ---------- Navigation ----------
                    Column {
                        id: navigation
                        objectName: "settingsPage.navigation"
                        visible: root.section === "navigation"
                        width: parent.width
                        spacing: Theme.spacingXs

                        SectionTitle {
                            text: qsTr("Mouse navigation")
                        }
                        Repeater {
                            model: AppSettings.navigationPresetIds
                            delegate: Rectangle {
                                id: presetRow
                                required property string modelData
                                required property int index
                                readonly property bool current: AppSettings.navigationPresetIndex === index
                                objectName: "navigationChoice." + modelData
                                width: navigation.width
                                height: presetText.implicitHeight + 2 * Theme.spacingS
                                radius: Theme.radiusSmall
                                color: current ? Theme.accentFill : presetHover.hovered ? Theme.controlBgHover
                                                                                          : "transparent"
                                border.width: Theme.borderWidth
                                border.color: current ? Theme.accentThinLine : Theme.panelBorder
                                Accessible.role: Accessible.RadioButton
                                Accessible.name: AppSettings.navigationPresetNames[index]
                                Accessible.checked: current

                                Column {
                                    id: presetText
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.margins: Theme.spacingS
                                    spacing: Theme.spacingXs / 2
                                    Label {
                                        width: parent.width
                                        font: presetRow.current ? Theme.strongFont : Theme.uiFont
                                        text: AppSettings.navigationPresetNames[presetRow.index]
                                    }
                                    Label {
                                        width: parent.width
                                        wrapMode: Text.Wrap
                                        secondary: true
                                        font: Theme.smallFont
                                        text: AppSettings.navigationPresetDescriptions[presetRow.index] || ""
                                    }
                                }
                                HoverHandler {
                                    id: presetHover
                                }
                                TapHandler {
                                    onTapped: AppSettings.set("navigation.preset", presetRow.modelData)
                                }
                            }
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.Wrap
                            secondary: true
                            font: Theme.smallFont
                            text: qsTr("In every style the wheel zooms about the point under the cursor and the left "
                                       + "button selects.")
                        }
                    }

                    // ---------- Keyboard ----------
                    Column {
                        id: keyboard
                        objectName: "settingsPage.keyboard"
                        visible: root.section === "keyboard"
                        width: parent.width
                        spacing: Theme.spacingS

                        SectionTitle {
                            text: qsTr("Keyboard shortcuts")
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.Wrap
                            text: qsTr("Every command can have a shortcut. Search them, change them and put them back "
                                       + "to their defaults in the shortcut settings.")
                        }
                        Button {
                            objectName: "settingsOpenShortcuts"
                            text: qsTr("Open keyboard shortcuts…")
                            onClicked: {
                                popup.close()
                                root.shortcutsRequested()
                            }
                        }
                    }
                }
            }

            // ---- footer ----
            Item {
                id: footer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.spacingS
                height: Math.max(Theme.controlHeightSmall, footerText.implicitHeight)

                Label {
                    id: footerText
                    objectName: "settingsFooter"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.Wrap
                    font: Theme.smallFont
                    color: AppSettings.lastError !== "" ? Theme.warning : Theme.textSecondary
                    text: AppSettings.lastError !== "" ? AppSettings.lastError
                          : AppSettings.filePath !== "" ? qsTr("Changes apply at once and are saved for you only.")
                          : qsTr("Changes apply at once; they are not saved in this session.")
                }
            }
        }
    }
}
