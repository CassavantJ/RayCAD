// The File menu of the top bar (task T-019). Titles and shortcut texts come from the action registry
// (ActionInfo::title, ActionInfo::defaultShortcut); every item runs its action through AppContext.invoke, so the
// menu, the shortcuts and the command palette (T-021) do the same thing.
import QtQuick
import RayCad.Ui

Item {
    property alias menu: menu

    component ActionItem: MenuItem {
        property string actionId
        text: Session.actionTitle(actionId)
        shortcutText: Shortcuts.shortcutText(actionId)
        onTriggered: AppContext.invoke(actionId)
    }

    Menu {
        id: menu
        objectName: "fileMenu"

        ActionItem {
            objectName: "fileMenu.new"
            actionId: "file.new"
            iconName: "plus"
        }
        ActionItem {
            objectName: "fileMenu.open"
            actionId: "file.open"
            iconName: "document"
        }
        ActionItem {
            objectName: "fileMenu.import"
            actionId: "file.import"
        }
        MenuSeparator {}
        ActionItem {
            objectName: "fileMenu.save"
            actionId: "file.save"
            enabled: Session.hasDocument
        }
        ActionItem {
            objectName: "fileMenu.saveAs"
            actionId: "file.saveAs"
            enabled: Session.hasDocument
        }
        MenuSeparator {}
        ActionItem {
            objectName: "fileMenu.exportStep"
            actionId: "file.exportStep"
            enabled: Session.hasDocument
        }
        ActionItem {
            objectName: "fileMenu.exportIges"
            actionId: "file.exportIges"
            enabled: Session.hasDocument
        }
        ActionItem {
            objectName: "fileMenu.exportStl"
            actionId: "file.exportStl"
            enabled: Session.hasDocument
        }
        MenuSeparator {}
        ActionItem {
            objectName: "fileMenu.undo"
            actionId: "edit.undo"
            text: Session.undoText !== "" ? qsTr("Undo %1").arg(Session.undoText) : Session.actionTitle(actionId)
            enabled: Session.canUndo
        }
        ActionItem {
            objectName: "fileMenu.redo"
            actionId: "edit.redo"
            text: Session.redoText !== "" ? qsTr("Redo %1").arg(Session.redoText) : Session.actionTitle(actionId)
            enabled: Session.canRedo
        }
        MenuSeparator {}
        ActionItem {
            objectName: "fileMenu.close"
            actionId: "file.close"
            enabled: Session.hasDocument
        }
    }
}
