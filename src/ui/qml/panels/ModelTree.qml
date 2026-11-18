// The model tree (task T-020): the left panel's content. Sections for the document's features (when a document is
// attached, T-017/T-019) and for the bodies in the view, each body with a visibility toggle. Clicking a body selects
// it in the view (Ctrl toggles, Shift adds); a body holding any selected element is highlighted here.
// Rows come from Scene.tree (scene_controller.h); each is a TreeRow.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

Item {
    id: root
    objectName: "modelTree"

    ListView {
        id: list
        objectName: "modelTreeList"
        anchors.fill: parent
        anchors.topMargin: Theme.spacingXs
        anchors.bottomMargin: Theme.spacingXs
        visible: count > 0
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: Scene.tree
        ScrollBar.vertical: ScrollBar {}
        Accessible.role: Accessible.Tree
        Accessible.name: qsTr("Model")

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
            required property string rowDetail
            required property string rowSection
            readonly property bool isBody: rowKind === "body"

            objectName: isBody ? "bodyRow." + rowBody : "treeRow." + index
            width: ListView.view.width
            text: rowName
            depth: rowDepth
            expandable: rowExpandable
            expanded: rowExpanded
            iconName: isBody ? "cube" : rowKind === "feature" ? "document" : ""
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

    Label {
        objectName: "modelTreeEmpty"
        anchors.fill: parent
        anchors.margins: Theme.spacingL
        visible: list.count === 0
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        secondary: true
        text: qsTr("Features and bodies of the open document appear here.")
    }
}
