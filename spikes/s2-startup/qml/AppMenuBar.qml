import QtQuick
import QtQuick.Controls

MenuBar {
    id: root
    signal quitRequested()
    Menu {
        title: "&File"
        Action { text: "&New"; shortcut: StandardKey.New }
        Action { text: "&Open..."; shortcut: StandardKey.Open }
        Menu {
            title: "Open &Recent"
            Action { text: "fixture_plate.raycad" }
            Action { text: "manifold_block.raycad" }
            Action { text: "steel_bracket.raycad" }
        }
        MenuSeparator {}
        Action { text: "&Save"; shortcut: StandardKey.Save }
        Action { text: "Save &As..." }
        MenuSeparator {}
        Action { text: "&Import STEP/IGES..." }
        Action { text: "&Export..." }
        MenuSeparator {}
        Action { text: "E&xit"; onTriggered: root.quitRequested() }
    }
    Menu {
        title: "&Edit"
        Action { text: "&Undo"; shortcut: StandardKey.Undo }
        Action { text: "&Redo"; shortcut: StandardKey.Redo }
        MenuSeparator {}
        Action { text: "Cu&t"; shortcut: StandardKey.Cut }
        Action { text: "&Copy"; shortcut: StandardKey.Copy }
        Action { text: "&Paste"; shortcut: StandardKey.Paste }
        Action { text: "&Delete"; shortcut: StandardKey.Delete }
        MenuSeparator {}
        Action { text: "Pre&ferences..." }
    }
    Menu {
        title: "&View"
        Action { text: "Fit"; shortcut: "F" }
        Action { text: "Shaded"; checkable: true; checked: true }
        Action { text: "Shaded with edges"; checkable: true }
        Action { text: "Wireframe"; checkable: true }
        MenuSeparator {}
        Action { text: "Model tree"; checkable: true; checked: true }
        Action { text: "Properties"; checkable: true; checked: true }
        Action { text: "Timeline"; checkable: true; checked: true }
    }
    Menu {
        title: "&Manufacture"
        Action { text: "New setup" }
        Action { text: "Generate all toolpaths" }
        Action { text: "Simulate" }
        Action { text: "Post process..." }
    }
    Menu {
        title: "&Help"
        Action { text: "Documentation" }
        Action { text: "Keyboard shortcuts" }
        Action { text: "About RayCAD" }
    }
}
