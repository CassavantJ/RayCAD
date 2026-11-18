// Where the command palette and the shortcut settings panel live (task T-021). Both are created on first use: each
// sits in a Loader that becomes active the first time it is needed, so start-up creates only this item.
//
//   paletteLoader    CommandPalette.qml: Commands.paletteRequested (Ctrl+K, ui.commandPalette), the top bar's
//                    command search field (Shell.commandSearchRequested), Commands.formRequested (a shortcut or a
//                    menu ran an action that needs arguments)
//   settingsLoader   panels/ShortcutSettings.qml: Commands.shortcutSettingsRequested (ui.keyboardShortcuts)
// When a popup closes the focus goes back to the 3D view.
import QtQuick
import RayCad.Ui

Item {
    id: host
    objectName: "commandHost"

    // The item that gets the focus back when a popup closes (the viewport item).
    property Item view: null
    // The top bar's command search field: cleared and unfocused when the palette takes over from it.
    property Item searchField: null

    readonly property alias paletteLoader: paletteLoader
    readonly property alias settingsLoader: settingsLoader

    function openPalette(text) {
        if (searchField) {
            searchField.text = ""
            searchField.focus = false
        }
        restoreFocus()
        paletteLoader.active = true
        paletteLoader.item.openList(text || "")
    }

    function openForm(commandId) {
        restoreFocus()
        paletteLoader.active = true
        paletteLoader.item.openForm(commandId)
    }

    function openShortcutSettings() {
        if (paletteLoader.item)
            paletteLoader.item.popup.close()
        restoreFocus()
        settingsLoader.active = true
        settingsLoader.item.open()
    }

    function restoreFocus() {
        if (view)
            view.forceActiveFocus()
    }

    Connections {
        target: Commands
        function onPaletteRequested(text) {
            host.openPalette(text)
        }
        function onFormRequested(commandId) {
            host.openForm(commandId)
        }
        function onShortcutSettingsRequested() {
            host.openShortcutSettings()
        }
    }

    Loader {
        id: paletteLoader
        objectName: "paletteLoader"
        active: false
        sourceComponent: Component {
            CommandPalette {
                onClosed: host.restoreFocus()
            }
        }
    }

    Loader {
        id: settingsLoader
        objectName: "shortcutSettingsLoader"
        active: false
        sourceComponent: Component {
            ShortcutSettings {
                onClosed: host.restoreFocus()
            }
        }
    }
}
