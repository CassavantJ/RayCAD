// Context menus (task T-021) for the viewport and the model tree. Place it over the area it serves (anchors.fill);
// it takes no input itself: Commands.contextMenuRequested (a window event filter, palette_commands.h) reports
// right-clicks and the instance whose area holds the point opens its menu.
//
//   viewport   the menu opens where the right button was pressed and released within the drag distance, so the
//              navigation presets that drag with the right button (pan, orbit) keep working unchanged.
//   tree       set treeRoot to the ModelTree: a right-click on a body row (TreeRow delegates of its ListView
//              "modelTreeList" with isBody and rowBody) selects that body first, unless it is selected already.
// The entries come from Commands.contextEntries(): the commands valid for the current selection (nothing selected:
// view commands, open, import; a face, edge, vertex or body: fit, select body, hide and show, copy; always undo and
// redo), see kContextMenu in palette_commands.cpp. The menu is created on first use and rebuilt each time it opens.
import QtQuick
import RayCad.Ui

Item {
    id: root

    // The ModelTree, or null in the viewport.
    property Item treeRoot: null
    readonly property alias menuLoader: menuLoader

    function findItem(item, name) {
        if (!item)
            return null
        if (item.objectName === name)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = findItem(item.children[i], name)
            if (found)
                return found
        }
        return null
    }

    // Opens the menu at (x, y) in this item's coordinates.
    function openAt(x, y) {
        const treeList = findItem(treeRoot, "modelTreeList")
        if (treeList) {
            const p = root.mapToItem(treeList, x, y)
            const row = treeList.itemAt(p.x + treeList.contentX, p.y + treeList.contentY)
            if (row && row.isBody && !Selection.isBodySelected(row.rowBody))
                Selection.selectBody(row.rowBody, 0)
        }
        menuLoader.active = true
        const menu = menuLoader.item.menu
        rebuild(menu, Commands.contextEntries())
        if (menu.count > 0)
            menu.popup(root, x, y)
    }

    function makeItem(entry) {
        return itemComponent.createObject(root, {
            commandId: entry.id,
            objectName: "contextItem." + entry.id,
            text: entry.title,
            shortcutText: entry.shortcut,
            enabled: entry.enabled,
            marked: entry.marked
        })
    }

    function rebuild(menu, entries) {
        // removeItem and removeMenu destroy what they remove (a submenu with its own items and its row).
        for (let i = menu.count - 1; i >= 0; --i) {
            const old = menu.itemAt(i)
            if (old && old.subMenu)
                menu.removeMenu(old.subMenu)
            else if (old)
                menu.removeItem(old)
        }
        for (const entry of entries) {
            if (entry.kind === "separator") {
                menu.addItem(separatorComponent.createObject(root))
            } else if (entry.kind === "menu") {
                const sub = submenuComponent.createObject(root, { title: entry.title })
                for (const child of entry.items)
                    sub.addItem(makeItem(child))
                menu.addMenu(sub)
            } else {
                menu.addItem(makeItem(entry))
            }
        }
    }

    // Commands watches the window's right button without taking events from anyone (a handler or mouse area here
    // would sit in the way of the viewport's right-button drags); each ContextMenu opens for clicks in its area.
    Connections {
        target: Commands
        function onContextMenuRequested(scenePos) {
            if (!root.visible || !root.Window.window)
                return
            const p = root.mapFromItem(null, scenePos.x, scenePos.y)
            if (p.x >= 0 && p.y >= 0 && p.x < root.width && p.y < root.height)
                root.openAt(p.x, p.y)
        }
    }

    Loader {
        id: menuLoader
        active: false
        sourceComponent: Component {
            Item {
                property alias menu: menu
                Menu {
                    id: menu
                    objectName: "contextMenu"
                }
            }
        }
    }

    Component {
        id: itemComponent
        MenuItem {
            property string commandId
            onTriggered: Commands.trigger(commandId)
        }
    }
    Component {
        id: separatorComponent
        MenuSeparator {}
    }
    Component {
        id: submenuComponent
        Menu {}
    }
}
