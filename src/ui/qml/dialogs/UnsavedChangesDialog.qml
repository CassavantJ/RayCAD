// The unsaved-changes prompt (task T-019): closing, quitting, opening another file or starting a new part while
// the document has unsaved changes. Save / Don't save / Cancel, answered through Files.answerPrompt.
import QtQuick
import RayCad.Ui

DialogBase {
    id: dialog
    objectName: "unsavedChangesDialog"

    title: qsTr("Save the changes to %1?").arg(Files.promptDocument)
    text: Files.promptReason === "quit" ? qsTr("If you quit without saving, the changes are lost.")
        : Files.promptReason === "close" ? qsTr("If you close it without saving, the changes are lost.")
        : qsTr("It closes when the other document opens; changes that are not saved are lost.")
    initialFocusItem: saveButton

    onRejected: Files.answerPrompt("cancel")

    buttons: [
        Button {
            objectName: "unsavedDiscard"
            text: qsTr("Don't save")
            onClicked: Files.answerPrompt("discard")
        },
        Button {
            objectName: "unsavedCancel"
            text: qsTr("Cancel")
            onClicked: Files.answerPrompt("cancel")
        },
        Button {
            id: saveButton
            objectName: "unsavedSave"
            primary: true
            text: Session.readOnly || Session.documentPath === "" ? qsTr("Save as…") : qsTr("Save")
            onClicked: Files.answerPrompt("save")
        }
    ]
}
