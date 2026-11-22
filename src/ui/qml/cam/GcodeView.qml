// The G-code viewer of the CAM workspace (task T-047; CamGcode, cam/post_view.h): the last program posted, read-only,
// in the mono font with line numbers and its words coloured by the theme's G-code tokens. A ListView makes rows for the
// lines in sight only, so any length scrolls alike. A click selects a line (Shift extends) and highlights its move in
// the view; a move clicked in the view scrolls here. Keys: arrows, Page Up/Down, Home/End move the current line (Shift
// extends), Ctrl+F finds, Enter or F3 the next match, Shift+F3 the previous, Ctrl+C copies the selected lines, Ctrl+A
// selects all, Esc closes.
import QtQuick
import RayCad.Ui

Item {
    id: root
    objectName: "gcodeViewHost"

    // Below the view cube and its buttons in the top right corner.
    property real topInset: Theme.controlHeight * 3 + Theme.controlHeight * 1.5 + 2 * Theme.panelGap
    readonly property real rowHeight: Math.ceil(fm.height + 2)
    readonly property real advance: fm.advanceWidth("0")
    readonly property real gutterWidth: (CamGcode.digits + 1) * advance
    readonly property real textX: gutterWidth + Theme.spacingS

    FontMetrics {
        id: fm
        font: Theme.monoFont
    }

    function pageRows() {
        return Math.max(1, Math.floor(list.height / root.rowHeight) - 1)
    }

    PanelBase {
        id: panel
        objectName: "gcodeView"
        visible: CamGcode.open
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: root.topInset
        width: Math.min(parent.width * 0.45, Math.max(Theme.controlHeight * 15, root.gutterWidth + 56 * root.advance))
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("G-code of %1").arg(CamGcode.title)

        FocusScope {
            id: scope
            objectName: "gcodeScope"
            anchors.fill: parent
            anchors.margins: Theme.spacingS
            focus: true

            // The viewer's own keys win over the window's shortcuts while it has the focus.
            Keys.onShortcutOverride: event => {
                const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
                event.accepted = event.key === Qt.Key_Escape || event.key === Qt.Key_F3
                                 || (ctrl && (event.key === Qt.Key_C || event.key === Qt.Key_A || event.key === Qt.Key_F
                                              || event.key === Qt.Key_Home || event.key === Qt.Key_End))
            }
            Keys.onPressed: event => {
                const shift = (event.modifiers & Qt.ShiftModifier) !== 0
                const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
                if (event.key === Qt.Key_Down) CamGcode.moveCurrent(1, shift)
                else if (event.key === Qt.Key_Up) CamGcode.moveCurrent(-1, shift)
                else if (event.key === Qt.Key_PageDown) CamGcode.moveCurrent(root.pageRows(), shift)
                else if (event.key === Qt.Key_PageUp) CamGcode.moveCurrent(-root.pageRows(), shift)
                else if (event.key === Qt.Key_Home && ctrl) CamGcode.moveCurrent(-CamGcode.lineCount, shift)
                else if (event.key === Qt.Key_End && ctrl) CamGcode.moveCurrent(CamGcode.lineCount, shift)
                else if (event.key === Qt.Key_C && ctrl) CamGcode.copySelection()
                else if (event.key === Qt.Key_A && ctrl) CamGcode.selectAll()
                else if (event.key === Qt.Key_F && ctrl) { findField.forceActiveFocus(); findField.selectAll() }
                else if (event.key === Qt.Key_F3) shift ? CamGcode.findPrevious() : CamGcode.findNext()
                else if (event.key === Qt.Key_Escape) CamGcode.close()
                else return
                event.accepted = true
            }

            // ---- header: the file and close ----
            Row {
                id: header
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: Theme.spacingS
                Icon {
                    id: headerIcon
                    anchors.verticalCenter: parent.verticalCenter
                    name: "document"
                    color: Theme.accentText
                }
                Column {
                    width: header.width - headerIcon.width - closeButton.width - 2 * Theme.spacingS
                    anchors.verticalCenter: parent.verticalCenter
                    Label {
                        objectName: "gcodeTitle"
                        width: parent.width
                        font: Theme.strongFont
                        elide: Text.ElideMiddle
                        text: CamGcode.title
                    }
                    Label {
                        objectName: "gcodePath"
                        width: parent.width
                        secondary: true
                        font: Theme.smallFont
                        elide: Text.ElideMiddle
                        text: qsTr("%1 lines, %2").arg(CamGcode.lineCount).arg(CamGcode.path)
                    }
                }
                ToolButton {
                    id: closeButton
                    objectName: "gcodeClose"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "close"
                    toolTipText: qsTr("Close the G-code viewer (Esc)")
                    onClicked: CamGcode.close()
                }
            }

            // ---- find and copy ----
            Row {
                id: tools
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: header.bottom
                anchors.topMargin: Theme.spacingS
                spacing: Theme.spacingXs
                TextField {
                    id: findField
                    objectName: "gcodeFind"
                    width: tools.width - matchLabel.width - 4 * Theme.controlHeightSmall - 5 * Theme.spacingXs
                    iconName: "search"
                    placeholderText: qsTr("Find (Ctrl+F)")
                    text: CamGcode.findText
                    Accessible.name: qsTr("Find in the program")
                    onTextEdited: CamGcode.findText = text
                    Keys.onReturnPressed: event => {
                        (event.modifiers & Qt.ShiftModifier) ? CamGcode.findPrevious() : CamGcode.findNext()
                    }
                    Keys.onEnterPressed: CamGcode.findNext()
                    Keys.onEscapePressed: {
                        if (text !== "") {
                            CamGcode.findText = ""
                        } else {
                            list.forceActiveFocus()
                        }
                    }
                }
                Label {
                    id: matchLabel
                    objectName: "gcodeMatches"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.controlHeight * 2.4
                    horizontalAlignment: Text.AlignHCenter
                    secondary: true
                    font: Theme.smallFont
                    text: CamGcode.findText === "" ? "" : CamGcode.matchCount === 0 ? qsTr("No match")
                        : qsTr("%1 of %2").arg(CamGcode.currentMatch + 1).arg(CamGcode.matchCount)
                }
                ToolButton {
                    objectName: "gcodeFindPrevious"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "chevron-up"
                    enabled: CamGcode.matchCount > 0
                    toolTipText: qsTr("Previous match (Shift+F3)")
                    onClicked: CamGcode.findPrevious()
                }
                ToolButton {
                    objectName: "gcodeFindNext"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "chevron-down"
                    enabled: CamGcode.matchCount > 0
                    toolTipText: qsTr("Next match (F3)")
                    onClicked: CamGcode.findNext()
                }
                ToolButton {
                    objectName: "gcodeCopy"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "copy"
                    enabled: CamGcode.currentLine >= 0
                    toolTipText: qsTr("Copy the selected lines (Ctrl+C)")
                    onClicked: CamGcode.copySelection()
                }
                ToolButton {
                    objectName: "gcodeCopyAll"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "document"
                    enabled: CamGcode.lineCount > 0
                    toolTipText: qsTr("Copy the whole program")
                    onClicked: CamGcode.copyAll()
                }
            }

            // ---- the lines ----
            Rectangle {
                id: frame
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: tools.bottom
                anchors.bottom: info.top
                anchors.topMargin: Theme.spacingS
                anchors.bottomMargin: Theme.spacingXs
                radius: Theme.radiusSmall
                color: Theme.fieldBg
                border.width: Theme.borderWidth
                border.color: Theme.controlBorder

                ListView {
                    id: list
                    objectName: "gcodeList"
                    anchors.fill: parent
                    anchors.margins: Theme.borderWidth
                    clip: true
                    model: CamGcode
                    reuseItems: true
                    boundsBehavior: Flickable.StopAtBounds
                    // Long lines scroll sideways (nothing of a program is cut off); the line numbers stay put.
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    contentWidth: Math.max(width, root.textX + (CamGcode.columns + 1) * root.advance)
                    pixelAligned: true
                    keyNavigationEnabled: false // the scope's keys move the current line
                    activeFocusOnTab: true
                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Program lines")
                    ScrollBar.vertical: ScrollBar {
                        objectName: "gcodeScrollBar"
                    }
                    ScrollBar.horizontal: ScrollBar {
                        objectName: "gcodeScrollBarH"
                    }

                    delegate: Item {
                        id: line
                        required property int index
                        required property int lineNumber
                        required property string styled
                        required property bool hasMove
                        required property var matches
                        readonly property bool current: CamGcode.currentLine === index
                        readonly property bool selected: index >= CamGcode.selectionFirst && index <= CamGcode.selectionLast
                        readonly property bool picked: index >= CamGcode.pickedFirst && index <= CamGcode.pickedLast
                        objectName: "gcodeLine." + index
                        width: ListView.view.contentWidth
                        height: root.rowHeight

                        Rectangle {
                            anchors.fill: parent
                            visible: line.selected || line.picked
                            color: line.current || line.picked ? Theme.gcodeCurrentLine : Theme.selectionFill
                        }
                        // Matches of the find text, by column (the font is monospaced).
                        Repeater {
                            model: line.matches
                            delegate: Rectangle {
                                required property var modelData
                                x: root.textX + modelData.column * root.advance
                                width: modelData.length * root.advance
                                height: line.height
                                color: modelData.current ? Theme.accentFill : Theme.selectionFill
                                opacity: 0.6
                            }
                        }
                        Text {
                            objectName: "gcodeText"
                            x: root.textX
                            width: parent.width - x
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            font: Theme.monoFont
                            color: Theme.gcodeText
                            textFormat: Text.StyledText
                            text: line.styled
                        }
                        // The gutter, fixed at the left edge of the view while the text scrolls under it.
                        Rectangle {
                            x: line.ListView.view.contentX
                            width: root.gutterWidth + Theme.spacingXs
                            height: parent.height
                            color: line.selected || line.picked
                                   ? (line.current || line.picked ? Theme.gcodeCurrentLine : Theme.selectionFill)
                                   : Theme.fieldBg
                            Text {
                                objectName: "gcodeNumber"
                                width: root.gutterWidth - root.advance
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: Text.AlignRight
                                font: Theme.monoFont
                                color: Theme.gcodeLineNumber
                                text: line.lineNumber
                                textFormat: Text.PlainText
                            }
                            // A dot for lines that write a toolpath move (they link to the view).
                            Rectangle {
                                objectName: "gcodeMoveMark"
                                visible: line.hasMove
                                width: Math.max(4, Math.round(root.advance * 0.4))
                                height: width
                                radius: width / 2
                                x: root.gutterWidth - root.advance / 2 - width / 2
                                anchors.verticalCenter: parent.verticalCenter
                                color: Theme.accent
                                opacity: 0.8
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: mouse => {
                                CamGcode.selectLine(line.index, (mouse.modifiers & Qt.ShiftModifier) !== 0)
                                list.forceActiveFocus()
                            }
                        }
                    }
                }
            }

            // ---- the current line's move ----
            Label {
                id: info
                objectName: "gcodeLineInfo"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Math.max(implicitHeight, Theme.rowHeight)
                verticalAlignment: Text.AlignVCenter
                secondary: true
                font: Theme.smallFont
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                maximumLineCount: 2
                text: CamGcode.lineInfo
            }
        }
    }

    Connections {
        target: CamGcode
        function onScrollRequested(line) {
            list.positionViewAtIndex(line, ListView.Contain)
        }
        function onOpenChanged() {
            if (CamGcode.open)
                list.forceActiveFocus()
        }
    }
}
