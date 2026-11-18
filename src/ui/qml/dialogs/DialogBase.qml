// A themed modal dialog inside the window (task T-019): a scrim over the shell and a panel in the middle. Not a
// native dialog, so it follows the theme and the glass setting like every other panel.
//
//   title, text   heading and message
//   content       extra items between the message and the buttons (default property)
//   buttons       the button row, right-aligned; put the primary button last
//   rejected()    Esc, or the caller's Cancel
// While shown it takes the focus and keeps Tab inside itself; clicks and wheel events do not reach the shell.
import QtQuick
import RayCad.Ui

FocusScope {
    id: dialog

    property string title
    property string text
    property real dialogWidth: Theme.controlHeight * 16
    default property alias content: body.data
    property alias buttons: buttonRow.data
    // The item that gets the focus when the dialog appears (else the first button).
    property Item initialFocusItem: null

    signal rejected()

    anchors.fill: parent

    function focusInitial() {
        const target = dialog.initialFocusItem !== null ? dialog.initialFocusItem
                     : buttonRow.children.length > 0 ? buttonRow.children[0] : dialog
        target.forceActiveFocus(Qt.PopupFocusReason)
    }

    function holds(item) {
        for (let it = item; it; it = it.parent) {
            if (it === card)
                return true
        }
        return false
    }

    function moveFocus(forward) {
        let item = Window.activeFocusItem
        for (let i = 0; i < 500; ++i) {
            item = item ? item.nextItemInFocusChain(forward) : card.nextItemInFocusChain(forward)
            if (!item)
                return
            if (dialog.holds(item) && item.visible && item.enabled) {
                item.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                return
            }
        }
    }

    Component.onCompleted: Qt.callLater(focusInitial)
    onVisibleChanged: if (visible) Qt.callLater(focusInitial)

    // Esc wherever the focus is (the window may not have given it to the dialog yet).
    Shortcut {
        sequence: "Esc"
        enabled: dialog.visible
        onActivated: dialog.rejected()
    }
    Keys.onEscapePressed: dialog.rejected()
    Keys.onTabPressed: dialog.moveFocus(true)
    Keys.onBacktabPressed: dialog.moveFocus(false)

    // The scrim: the shell stays visible but dimmed, and takes no input while the dialog is up.
    Rectangle {
        objectName: "dialogScrim"
        anchors.fill: parent
        color: Theme.windowBg
        opacity: 0.6
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: wheel => wheel.accepted = true
    }

    PanelBase {
        id: card
        objectName: "dialogCard"
        anchors.centerIn: parent
        width: Math.min(dialog.width - 2 * Theme.spacingL, dialog.dialogWidth)
        height: column.implicitHeight + 2 * Theme.spacingL
        Accessible.role: Accessible.Dialog
        Accessible.name: dialog.title

        Column {
            id: column
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spacingL
            spacing: Theme.spacingM

            Label {
                objectName: "dialogTitle"
                width: parent.width
                font: Theme.titleFont
                wrapMode: Text.Wrap
                text: dialog.title
            }
            Label {
                objectName: "dialogText"
                visible: text !== ""
                width: parent.width
                wrapMode: Text.Wrap
                secondary: true
                text: dialog.text
            }
            Column {
                id: body
                width: parent.width
                spacing: Theme.spacingS
                visible: children.length > 0
            }
            Row {
                id: buttonRow
                anchors.right: parent.right
                spacing: Theme.spacingS
            }
        }
    }
}
