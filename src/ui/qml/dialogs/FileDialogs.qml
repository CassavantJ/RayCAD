// The dialogs of the file flows (task T-019), over the whole shell. Everything here is created on first use.
//
//   native   Open, Save As, Import and Export STEP/IGES/STL: the platform's file dialogs, opened when Files asks
//            (Files.*DialogRequested); with Files.useNativeDialogs off (tests) they are not shown.
//   themed   the unsaved-changes prompt (UnsavedChangesDialog.qml) and crash recovery at launch (RecoveryDialog.qml).
import QtQuick
import QtQuick.Dialogs
import RayCad.Ui

Item {
    id: root
    objectName: "fileDialogs"

    // A themed modal dialog is up: the shell's shortcuts (and Esc's job cancel) stand back.
    readonly property bool modalShown: unsavedLoader.active || recoveryLoader.active

    // ---------- native dialogs ----------
    Connections {
        target: Files
        function onOpenDialogRequested() {
            if (!Files.useNativeDialogs)
                return
            openLoader.active = true
            openLoader.item.open()
        }
        function onImportDialogRequested() {
            if (!Files.useNativeDialogs)
                return
            importLoader.active = true
            importLoader.item.open()
        }
        function onSaveAsDialogRequested(suggestedName, folder) {
            if (!Files.useNativeDialogs)
                return
            saveAsLoader.active = true
            const d = saveAsLoader.item
            if (folder.toString() !== "")
                d.currentFolder = folder
            d.selectedFile = folder.toString() !== "" ? folder + "/" + suggestedName : suggestedName
            d.open()
        }
        function onExportDialogRequested(kind, suggestedName, folder) {
            if (!Files.useNativeDialogs)
                return
            exportLoader.active = true
            const d = exportLoader.item
            d.kind = kind
            d.nameFilters = Files.exportFilters(kind)
            d.defaultSuffix = kind === "iges" ? "igs" : kind
            if (folder.toString() !== "")
                d.currentFolder = folder
            d.selectedFile = folder.toString() !== "" ? folder + "/" + suggestedName : suggestedName
            d.open()
        }
    }

    Loader {
        id: openLoader
        active: false
        sourceComponent: Component {
            FileDialog {
                objectName: "openFileDialog"
                title: qsTr("Open")
                nameFilters: Files.openFilters
                fileMode: FileDialog.OpenFiles
                onAccepted: Files.openUrls(selectedFiles)
            }
        }
    }
    Loader {
        id: importLoader
        active: false
        sourceComponent: Component {
            FileDialog {
                objectName: "importFileDialog"
                title: qsTr("Import into %1").arg(Session.documentTitle)
                nameFilters: Files.importFilters
                fileMode: FileDialog.OpenFile
                onAccepted: Files.importFile(selectedFile)
            }
        }
    }
    Loader {
        id: saveAsLoader
        active: false
        sourceComponent: Component {
            FileDialog {
                objectName: "saveAsFileDialog"
                title: qsTr("Save %1 as").arg(Session.documentTitle)
                nameFilters: Files.documentFilters
                defaultSuffix: "raycad"
                fileMode: FileDialog.SaveFile
                // The native dialog asks before replacing a file, so the save may overwrite it.
                onAccepted: Files.saveAs(selectedFile, true)
                onRejected: Files.saveAsCancelled()
            }
        }
    }
    Loader {
        id: exportLoader
        active: false
        sourceComponent: Component {
            FileDialog {
                property string kind: "step"
                objectName: "exportFileDialog"
                title: qsTr("Export %1").arg(Session.documentTitle)
                fileMode: FileDialog.SaveFile
                onAccepted: Files.exportTo(kind, selectedFile)
            }
        }
    }

    // ---------- themed dialogs ----------
    Loader {
        id: unsavedLoader
        anchors.fill: parent
        active: Files.promptVisible
        sourceComponent: Component {
            UnsavedChangesDialog {}
        }
    }
    Loader {
        id: recoveryLoader
        anchors.fill: parent
        active: Session.recoveryPromptVisible && !unsavedLoader.active
        sourceComponent: Component {
            RecoveryDialog {}
        }
    }
}
