// The model tree (task T-020): the left panel's content. Sections for the document's features (when a document is
// attached, T-017/T-019) and for the bodies in the view, each body with a visibility toggle. Clicking a body selects
// it in the view (Ctrl toggles, Shift adds); a body holding any selected element is highlighted here. Double-clicking a
// sketch opens it for editing (T-092).
// T-098: the features come from FeatureTree (model/feature_tree.h), one model/FeatureRow.qml each: type icon, status
// and the rebuild's message, rename (F2), suppress, delete (Delete), drag to reorder, double-click to edit, an eye
// for sketches. The rollback bar sits under the last feature shown; drag it up to roll the model back. The
// parameter table (model/ParameterTable.qml) is the last section. The bodies keep their rows from Scene.tree.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

Item {
    id: root
    objectName: "modelTree"

    property bool featuresExpanded: true
    // The row the rollback bar sits under (the last one when not rolled back).
    readonly property int rollbackRow: FeatureTree.rollbackIndex >= 0 ? FeatureTree.rollbackIndex : FeatureTree.count - 1
    // While a row or the bar is dragged: the row it would go to (-1 when nothing is dragged).
    property int dropRow: -1
    property bool barDragging: false

    // The feature row under y (in featureColumn's coordinates), clamped to the rows.
    function rowAt(y) {
        let best = 0
        for (let i = 0; i < features.count; ++i) {
            const item = features.itemAt(i)
            if (item && y >= item.y)
                best = i
        }
        return best
    }

    Connections {
        target: FeatureTree
        function onRenameRequested(row) {
            const item = features.itemAt(row)
            if (item)
                item.startRename()
        }
    }

    Flickable {
        id: flick
        objectName: "modelTreeScroll"
        anchors.fill: parent
        anchors.topMargin: Theme.spacingXs
        anchors.bottomMargin: Theme.spacingXs
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentHeight: content.implicitHeight
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: content
            width: flick.width

            // ---------- features ----------
            TreeRow {
                objectName: "featuresHeader"
                visible: FeatureTree.count > 0
                width: content.width
                text: qsTr("Features")
                expandable: true
                expanded: root.featuresExpanded
                secondaryText: String(FeatureTree.count)
                font: Theme.strongFont
                onExpandToggled: root.featuresExpanded = !root.featuresExpanded
                onClicked: root.featuresExpanded = !root.featuresExpanded
            }

            FocusScope {
                id: featureScope
                objectName: "featureList"
                visible: root.featuresExpanded && FeatureTree.count > 0
                width: content.width
                height: featureColumn.implicitHeight
                activeFocusOnTab: true
                Accessible.role: Accessible.Tree
                Accessible.name: qsTr("Features")

                // T-083: Delete here deletes the current feature, not the faces selected in the view (the Delete key's
                // command in the Design workspace): the list claims the key the way Qt's shortcuts ask.
                Keys.onShortcutOverride: event => {
                    event.accepted = (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                                     && FeatureTree.currentIndex >= 0
                }
                Keys.onPressed: event => {
                    const cur = FeatureTree.currentIndex
                    if (event.key === Qt.Key_F2 && cur >= 0) {
                        features.itemAt(cur).startRename()
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && cur >= 0) {
                        FeatureTree.remove(cur)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Down) {
                        FeatureTree.currentIndex = Math.min(FeatureTree.count - 1, cur + 1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up) {
                        FeatureTree.currentIndex = Math.max(0, cur - 1)
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && cur >= 0) {
                        FeatureTree.edit(cur)
                        event.accepted = true
                    }
                }

                Column {
                    id: featureColumn
                    width: parent.width

                    Repeater {
                        id: features
                        model: FeatureTree
                        delegate: Column {
                            id: slot
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
                            required property bool isDatum
                            required property bool datumShown
                            width: featureColumn.width

                            function startRename() {
                                featureRow.startRename()
                            }

                            // Where a dragged row would land.
                            Rectangle {
                                visible: root.dropRow === slot.index && !root.barDragging
                                         && root.dragFrom > slot.index
                                width: parent.width
                                height: Theme.focusRingWidth
                                color: Theme.accent
                            }
                            FeatureRow {
                                id: featureRow
                                width: parent.width
                                index: slot.index
                                featureId: slot.featureId
                                featureName: slot.featureName
                                typeTitle: slot.typeTitle
                                iconName: slot.iconName
                                status: slot.status
                                statusText: slot.statusText
                                message: slot.message
                                suppressed: slot.suppressed
                                rolledBack: slot.rolledBack
                                isSketch: slot.isSketch
                                sketchShown: slot.sketchShown
                                isDatum: slot.isDatum
                                datumShown: slot.datumShown
                                onDragMoved: (i, y) => {
                                    root.dragFrom = i
                                    root.dropRow = root.rowAt(mapToItem(featureColumn, 0, y).y)
                                }
                                onDropped: (i, y) => {
                                    const to = root.rowAt(mapToItem(featureColumn, 0, y).y)
                                    root.dropRow = -1
                                    root.dragFrom = -1
                                    if (to !== i)
                                        FeatureTree.moveTo(i, to)
                                }
                            }
                            Rectangle {
                                visible: root.dropRow === slot.index && !root.barDragging
                                         && root.dragFrom >= 0 && root.dragFrom < slot.index
                                width: parent.width
                                height: Theme.focusRingWidth
                                color: Theme.accent
                            }

                            // The rollback bar under the last feature shown: drag it to roll back or forward.
                            Item {
                                id: bar
                                objectName: "rollbackBar"
                                visible: slot.index === (root.barDragging && root.dropRow >= 0 ? root.dropRow : root.rollbackRow)
                                width: parent.width
                                height: Theme.spacingS + Theme.focusRingWidth
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: Theme.spacingS
                                    width: parent.width - 2 * Theme.spacingS
                                    height: barArea.containsMouse || root.barDragging ? Theme.focusRingWidth * 2
                                                                                      : Theme.focusRingWidth
                                    radius: height / 2
                                    color: FeatureTree.rollbackIndex >= 0 ? Theme.accent : Theme.panelBorder
                                }
                                Accessible.role: Accessible.Slider
                                Accessible.name: qsTr("Rollback bar")
                                MouseArea {
                                    id: barArea
                                    anchors.fill: parent
                                    anchors.topMargin: -Theme.spacingXs
                                    anchors.bottomMargin: -Theme.spacingXs
                                    hoverEnabled: true
                                    cursorShape: Qt.SizeVerCursor
                                    preventStealing: true
                                    onPressed: {
                                        root.barDragging = true
                                        root.dropRow = slot.index
                                    }
                                    onPositionChanged: event => {
                                        if (pressed)
                                            root.dropRow = root.rowAt(mapToItem(featureColumn, 0, event.y).y)
                                    }
                                    onReleased: event => {
                                        const to = root.rowAt(mapToItem(featureColumn, 0, event.y).y)
                                        root.barDragging = false
                                        root.dropRow = -1
                                        FeatureTree.rollbackIndex = to >= FeatureTree.count - 1 ? -1 : to
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Why a tree action was refused (a blocked reorder, ...).
            Item {
                visible: FeatureTree.message !== "" || FeatureTree.checking
                width: content.width
                height: treeMessage.implicitHeight + Theme.spacingS
                Label {
                    id: treeMessage
                    objectName: "featureTreeMessage"
                    x: Theme.spacingS
                    width: parent.width - 2 * Theme.spacingS - dismiss.width
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    font: Theme.smallFont
                    color: FeatureTree.checking ? Theme.textSecondary : Theme.error
                    text: FeatureTree.checking ? qsTr("Checking the new order…") : FeatureTree.message
                }
                ToolButton {
                    id: dismiss
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingXs
                    compact: true
                    iconName: "close"
                    visible: !FeatureTree.checking
                    toolTipText: qsTr("Dismiss")
                    onClicked: FeatureTree.dismissMessage()
                }
            }

            // ---------- bodies (rows from Scene.tree; its feature rows are FeatureTree's above) ----------
            ListView {
                id: list
                objectName: "modelTreeList"
                width: content.width
                height: contentHeight
                interactive: false
                model: Scene.tree
                Accessible.role: Accessible.Tree
                Accessible.name: qsTr("Bodies")

                delegate: TreeRow {
                    id: row
                    required property int index
                    required property string rowKind
                    required property string rowName
                    required property int rowDepth
                    required property bool rowExpandable
                    required property bool rowExpanded
                    required property bool rowShown
                    required property bool rowSelected
                    required property int rowBody
                    required property int rowFeature
                    required property string rowDetail
                    required property string rowSection
                    readonly property bool isBody: rowKind === "body"
                    readonly property bool shownHere: rowSection === "bodies"

                    objectName: isBody ? "bodyRow." + rowBody : "treeRow." + index
                    visible: shownHere
                    height: shownHere ? implicitHeight : 0
                    width: ListView.view.width
                    text: rowName
                    depth: rowDepth
                    expandable: rowExpandable
                    expanded: rowExpanded
                    iconName: isBody ? "cube" : ""
                    secondaryText: rowDetail
                    highlighted: rowSelected
                    font: rowKind === "header" ? Theme.strongFont : Theme.uiFont
                    opacity: isBody && !rowShown ? 0.6 : 1
                    rightPadding: rightInset + Theme.spacingS + (isBody ? eye.width + Theme.spacingXs : 0)

                    onExpandToggled: Scene.tree.toggleSection(rowSection)
                    onClicked: {
                        if (isBody)
                            Selection.selectBody(rowBody)
                        else if (rowKind === "header")
                            Scene.tree.toggleSection(rowSection)
                    }

                    // Visibility toggle: an eye, struck through when the body is hidden (drawn for RayCAD).
                    Item {
                        id: eye
                        objectName: "visibilityToggle." + row.rowBody
                        visible: row.isBody
                        width: Theme.iconSize
                        height: Theme.iconSize
                        anchors.right: parent.right
                        anchors.rightMargin: row.rightInset + Theme.spacingS
                        anchors.verticalCenter: parent.verticalCenter
                        Accessible.role: Accessible.CheckBox
                        Accessible.checkable: true
                        Accessible.checked: row.rowShown
                        Accessible.name: row.rowShown ? qsTr("Hide %1").arg(row.rowName) : qsTr("Show %1").arg(row.rowName)

                        readonly property color ink: eyeHover.hovered ? Theme.textPrimary : Theme.textSecondary

                        Shape {
                            width: 16
                            height: 16
                            scale: eye.width / 16
                            transformOrigin: Item.TopLeft
                            preferredRendererType: Shape.CurveRenderer
                            ShapePath {
                                strokeColor: eye.ink
                                strokeWidth: 1.5
                                fillColor: "transparent"
                                capStyle: ShapePath.RoundCap
                                joinStyle: ShapePath.RoundJoin
                                PathSvg {
                                    path: row.rowShown
                                          ? "M 1.75 8 Q 8 1.5 14.25 8 Q 8 14.5 1.75 8 Z M 10 8 A 2 2 0 1 1 6 8 A 2 2 0 1 1 10 8 Z"
                                          : "M 1.75 8 Q 8 1.5 14.25 8 Q 8 14.5 1.75 8 Z M 3 13 L 13 3"
                                }
                            }
                        }
                        HoverHandler {
                            id: eyeHover
                        }
                        TapHandler {
                            // Takes the press, so the row under it does not also select the body.
                            gesturePolicy: TapHandler.WithinBounds
                            onTapped: Scene.setBodyVisible(row.rowBody, !row.rowShown)
                        }
                    }
                }
            }

            // ---------- parameters ----------
            ParameterTable {
                visible: Session.hasDocument
                width: content.width
            }
        }
    }

    property int dragFrom: -1

    Label {
        objectName: "modelTreeEmpty"
        anchors.fill: parent
        anchors.margins: Theme.spacingL
        visible: FeatureTree.count === 0 && list.count === 0 && !Session.hasDocument
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        secondary: true
        text: qsTr("Features and bodies of the open document appear here.")
    }
}
