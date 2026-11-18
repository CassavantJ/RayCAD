// The start screen (task T-019): what the viewport area shows while no document is open.
//
//   newPartButton / openButton   a new empty part (file.new) and the Open dialog (file.open)
//   recentList                   the recent files (Session.recentFiles), most recent first: a thumbnail tile (a
//                                neutral placeholder until thumbnails are stored), the name, the folder and when it
//                                was last opened. A file that is not there any more is marked missing and can be
//                                removed from the list.
//   recentHint                   what the list is for, while it is empty
// Keyboard: Tab moves between the buttons and the list; in the list the arrow keys move, Enter opens and Delete
// removes the current file from the list.
import QtQuick
import RayCad.Ui

FocusScope {
    id: start
    objectName: "startScreen"

    readonly property real tileWidth: Theme.controlHeight * 6
    readonly property real thumbHeight: Math.round(tileWidth * 0.6)

    function openRecent(path) {
        Files.open(path)
    }

    PanelBase {
        id: card
        objectName: "startCard"
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.spacingL, Theme.controlHeight * 27)
        height: Math.min(parent.height - 2 * Theme.spacingL, column.implicitHeight + 2 * Theme.spacingL)

        Column {
            id: column
            anchors.fill: parent
            anchors.margins: Theme.spacingL
            spacing: Theme.spacingM

            // ---- heading and the two main actions ----
            Item {
                width: parent.width
                height: Math.max(heading.implicitHeight, actions.implicitHeight)

                Column {
                    id: heading
                    anchors.left: parent.left
                    anchors.right: actions.left
                    anchors.rightMargin: Theme.spacingM
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spacingXs

                    Label {
                        objectName: "startTitle"
                        width: parent.width
                        font: Theme.titleFont
                        text: qsTr("Start")
                    }
                    Label {
                        width: parent.width
                        secondary: true
                        wrapMode: Text.Wrap
                        text: Files.busy ? qsTr("Opening %1…").arg(Files.openingName)
                            : qsTr("Open a RayCAD, STEP or IGES file, or start a new part.")
                    }
                }
                Row {
                    id: actions
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spacingS

                    Button {
                        objectName: "openButton"
                        text: Files.busy ? qsTr("Opening…") : qsTr("Open…")
                        iconName: "document"
                        enabled: AppContext.hasSession && !Files.busy
                        toolTipText: Shortcuts.shortcutText("file.open")
                        onClicked: AppContext.invoke("file.open")
                    }
                    Button {
                        objectName: "newPartButton"
                        primary: true
                        text: qsTr("New part")
                        iconName: "plus"
                        enabled: AppContext.hasSession
                        toolTipText: Shortcuts.shortcutText("file.new")
                        onClicked: AppContext.invoke("file.new")
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: Theme.borderWidth
                color: Theme.panelBorder
            }

            Label {
                objectName: "recentHeading"
                width: parent.width
                font: Theme.strongFont
                text: qsTr("Recent files")
            }

            Label {
                id: recentHint
                objectName: "recentHint"
                visible: Session.recentCount === 0
                width: parent.width
                secondary: true
                wrapMode: Text.Wrap
                text: AppContext.hasSession ? qsTr("Files you open or save appear here. Drop a file anywhere on the window to open it.")
                                            : qsTr("Recent files appear here once the application has started.")
            }

            // ---- recent files ----
            GridView {
                id: recentView
                objectName: "recentList"
                visible: count > 0
                width: parent.width
                // As tall as its rows, but no taller than the space left under the heading (then it scrolls).
                height: Math.max(0, Math.min(contentHeight, start.height - 4 * Theme.spacingL - y))
                cellWidth: Math.floor(width / Math.max(1, Math.floor(width / start.tileWidth)))
                cellHeight: start.thumbHeight + Theme.controlHeight * 2.4
                clip: true
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                keyNavigationEnabled: true
                currentIndex: 0
                model: Session.recentFiles
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Recent files")

                Keys.onReturnPressed: if (currentItem) start.openRecent(currentItem.filePath)
                Keys.onEnterPressed: if (currentItem) start.openRecent(currentItem.filePath)
                Keys.onDeletePressed: if (currentItem) Session.removeRecent(currentItem.filePath)

                delegate: Item {
                    id: tile
                    required property int index
                    required property string filePath
                    required property string fileName
                    required property string folder
                    required property string openedText
                    required property bool missing
                    required property string thumbnail
                    readonly property bool current: GridView.isCurrentItem
                    objectName: "recent." + index
                    width: GridView.view.cellWidth
                    height: GridView.view.cellHeight
                    Accessible.role: Accessible.ListItem
                    Accessible.name: missing ? qsTr("%1, missing").arg(fileName) : fileName
                    Accessible.description: folder

                    Rectangle {
                        id: tileBg
                        anchors.fill: parent
                        anchors.margins: Theme.spacingXs
                        radius: Theme.radiusControl
                        color: hover.containsMouse ? Theme.controlBgHover : "transparent"

                        FocusRing {
                            visible: tile.current && recentView.activeFocus
                        }
                    }

                    // Thumbnail slot: a neutral placeholder until thumbnails are stored (M1).
                    Rectangle {
                        id: thumb
                        objectName: "recentThumbnail"
                        anchors.left: tileBg.left
                        anchors.right: tileBg.right
                        anchors.top: tileBg.top
                        anchors.margins: Theme.spacingXs
                        height: start.thumbHeight - Theme.spacingXs
                        radius: Theme.radiusSmall
                        color: Theme.controlBgPressed
                        border.width: Theme.borderWidth
                        border.color: Theme.panelBorder
                        opacity: tile.missing ? 0.5 : 1

                        Image {
                            anchors.fill: parent
                            anchors.margins: Theme.borderWidth
                            visible: tile.thumbnail !== ""
                            source: tile.thumbnail
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Icon {
                            anchors.centerIn: parent
                            visible: tile.thumbnail === ""
                            name: "cube"
                            size: Theme.iconSize * 2
                            color: Theme.textDisabled
                        }
                    }

                    Column {
                        anchors.left: thumb.left
                        anchors.right: thumb.right
                        anchors.top: thumb.bottom
                        anchors.topMargin: Theme.spacingXs
                        spacing: Theme.spacingXs / 2

                        Label {
                            objectName: "recentName"
                            width: parent.width
                            font: Theme.strongFont
                            text: tile.fileName
                        }
                        Label {
                            width: parent.width
                            secondary: true
                            font: Theme.smallFont
                            elide: Text.ElideMiddle
                            text: tile.folder
                        }
                        Label {
                            objectName: "recentWhen"
                            width: parent.width
                            secondary: !tile.missing
                            font: Theme.smallFont
                            color: tile.missing ? Theme.error : Theme.textSecondary
                            text: tile.missing ? qsTr("Missing: moved or deleted") : tile.openedText
                        }
                    }

                    MouseArea {
                        id: hover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            recentView.currentIndex = tile.index
                            start.openRecent(tile.filePath)
                        }
                    }

                    ToolButton {
                        objectName: "recentRemove." + tile.index
                        anchors.top: thumb.top
                        anchors.right: thumb.right
                        anchors.margins: Theme.spacingXs
                        compact: true
                        iconName: "close"
                        visible: tile.missing || hover.containsMouse || hovered || (tile.current && recentView.activeFocus)
                        focusPolicy: Qt.NoFocus
                        toolTipText: qsTr("Remove from the list")
                        onClicked: Session.removeRecent(tile.filePath)
                    }
                }
            }
        }
    }
}
