// One feature of the model tree (task T-098; FeatureTree, model/feature_tree.h): the type's icon, the name, the build
// status, and below a failed or waiting feature a details row with the rebuild's message (also the tooltip of the
// warning mark). Click makes it current; double-click edits it (its tool's panel, or the sketch mode for a sketch);
// F2 renames, Delete deletes. Dragging it up or down reorders the history (the tree checks the new order first).
// A sketch has an eye that shows or hides it in the view; a suppressed feature is struck through; features after
// the rollback bar are greyed.
import QtQuick
import RayCad.Ui

Item {
    id: row

    required property int index
    required property string featureId
    required property string featureName
    required property string typeTitle
    required property string iconName
    required property string status
    required property string statusText
    required property string message
    required property bool suppressed
    required property bool rolledBack
    required property bool isSketch
    required property bool sketchShown

    readonly property bool current: FeatureTree.currentIndex === index
    readonly property bool failed: status === "failed" || status === "notBuilt"
    readonly property bool warned: status === "warning"
    readonly property bool hovered: mouse.containsMouse
    readonly property bool pressed: mouse.pressed
    property bool renaming: false
    property real dragOffset: 0

    // Reordering: the row asks its tree where it is dropped (y in this row's coordinates).
    signal dragMoved(int index, real y)
    signal dropped(int index, real y)

    objectName: "featureRow." + index
    implicitHeight: main.height + (details.visible ? details.implicitHeight + Theme.spacingXs : 0)
    opacity: rolledBack ? 0.45 : 1

    function startRename() {
        renaming = true
        nameField.text = featureName
        nameField.forceActiveFocus()
        nameField.selectAll()
    }

    Accessible.role: Accessible.TreeItem
    Accessible.name: featureName + ", " + typeTitle + (statusText !== "" ? ", " + statusText : "")
    Accessible.selected: current

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacingXs
        anchors.rightMargin: Theme.spacingXs
        radius: Theme.radiusSmall
        color: row.current ? Theme.selectionFill : row.hovered ? Theme.controlBgHover : "transparent"
        Rectangle {
            visible: row.current
            width: Theme.focusRingWidth
            height: parent.height - 2 * Theme.spacingXs
            radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            x: Theme.borderWidth
            color: Theme.selection
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        property real pressY: 0
        property bool dragging: false
        onPressed: event => {
            pressY = event.y
            dragging = false
            FeatureTree.currentIndex = row.index
        }
        onPositionChanged: event => {
            if (!pressed)
                return
            if (!dragging && Math.abs(event.y - pressY) > Theme.spacingS)
                dragging = true
            if (dragging) {
                row.dragOffset = event.y - pressY
                row.dragMoved(row.index, event.y)
            }
        }
        onReleased: event => {
            if (dragging)
                row.dropped(row.index, event.y)
            dragging = false
            row.dragOffset = 0
        }
        onDoubleClicked: FeatureTree.edit(row.index)
    }

    Item {
        id: main
        width: parent.width
        height: Theme.rowHeight

        Icon {
            id: icon
            x: Theme.spacingXs + Theme.spacingS + Theme.spacingL
            anchors.verticalCenter: parent.verticalCenter
            name: row.iconName
            color: row.failed ? Theme.error : Theme.textSecondary
        }
        Text {
            id: nameText
            objectName: "featureName"
            visible: !row.renaming
            anchors.left: icon.right
            anchors.leftMargin: Theme.spacingXs
            anchors.right: marks.left
            anchors.rightMargin: Theme.spacingXs
            anchors.verticalCenter: parent.verticalCenter
            text: row.featureName
            font.family: Theme.uiFont.family
            font.pixelSize: Theme.uiFont.pixelSize
            font.strikeout: row.suppressed
            color: row.suppressed ? Theme.textSecondary : Theme.textPrimary
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
        TextField {
            id: nameField
            objectName: "featureRename"
            visible: row.renaming
            anchors.left: icon.right
            anchors.leftMargin: Theme.spacingXs
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacingS
            anchors.verticalCenter: parent.verticalCenter
            Accessible.name: qsTr("New name of %1").arg(row.featureName)
            onAccepted: {
                FeatureTree.rename(row.index, text)
                row.renaming = false
            }
            Keys.onEscapePressed: row.renaming = false
            onActiveFocusChanged: if (!activeFocus) row.renaming = false
        }

        Row {
            id: marks
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacingS
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingXs
            visible: !row.renaming

            Item {
                id: warning
                objectName: "featureWarning." + row.index
                visible: row.failed || row.warned
                width: Theme.iconSize
                height: Theme.iconSize
                anchors.verticalCenter: parent.verticalCenter
                readonly property bool hovered: warnHover.hovered
                readonly property bool pressed: false
                Icon {
                    anchors.fill: parent
                    name: "warning"
                    color: row.failed ? Theme.error : Theme.warning
                }
                HoverHandler {
                    id: warnHover
                }
                HoverToolTip {
                    target: warning
                    text: row.message !== "" ? row.message : row.statusText
                }
            }
            Text {
                objectName: "featureStatus"
                anchors.verticalCenter: parent.verticalCenter
                visible: text !== ""
                text: row.status === "ok" || row.status === "building" ? "" : row.statusText
                font: Theme.smallFont
                color: row.failed ? Theme.error : Theme.textSecondary
                textFormat: Text.PlainText
            }
            ToolButton {
                objectName: "featureSuppress." + row.index
                visible: row.hovered || row.suppressed
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "suppress"
                checked: row.suppressed
                toolTipText: row.suppressed ? qsTr("Unsuppress") : qsTr("Suppress (switch off)")
                onClicked: FeatureTree.setSuppressed(row.index, !row.suppressed)
            }
            // A sketch's eye: shown in the view or not (drawn for RayCAD, as the bodies' eye).
            Item {
                id: eye
                objectName: "sketchVisibility." + row.index
                visible: row.isSketch
                width: Theme.iconSize
                height: Theme.iconSize
                anchors.verticalCenter: parent.verticalCenter
                Accessible.role: Accessible.CheckBox
                Accessible.checkable: true
                Accessible.checked: row.sketchShown
                Accessible.name: row.sketchShown ? qsTr("Hide %1").arg(row.featureName) : qsTr("Show %1").arg(row.featureName)
                Icon {
                    anchors.fill: parent
                    name: row.sketchShown ? "eye" : "eye-off"
                    color: eyeHover.hovered ? Theme.textPrimary : Theme.textSecondary
                }
                HoverHandler {
                    id: eyeHover
                }
                TapHandler {
                    gesturePolicy: TapHandler.WithinBounds
                    onTapped: FeatureTree.setSketchShown(row.index, !row.sketchShown)
                }
            }
        }
    }

    // Why it failed, or what it waits on.
    Label {
        id: details
        objectName: "featureDetails." + row.index
        visible: row.failed && row.message !== ""
        anchors.top: main.bottom
        x: icon.x
        width: row.width - x - Theme.spacingS
        wrapMode: Text.Wrap
        elide: Text.ElideNone
        maximumLineCount: 4
        font: Theme.smallFont
        color: Theme.error
        text: row.message
    }
}
