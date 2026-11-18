// A floating panel over the full-window viewport (PLAN.md 6.6). Glass and flat panels have IDENTICAL layout:
// only the background below differs, and nothing in it has an implicit size, so children lay out the same way.
//
//   flat   an opaque PanelBg rounded rectangle with a PanelBorder outline.
//   glass  the shared blurred viewport backdrop (backdropSource, supplied by T-016/T-020; null for now) under a
//          PanelBgGlassTint fill, a top-edge PanelHighlightTop sheen and the same outline. With no backdrop the tint
//          alone is drawn: translucent over the viewport, its opacity chosen by the theme so text keeps 4.5:1.
//
// Children go into contentItem (inset by `padding`).
import QtQuick
import RayCad.Ui

Item {
    id: panel

    property bool glass: AppContext.glass
    property var backdropSource: AppContext.backdropSource
    property real radius: Theme.radiusPanel
    property real padding: 0
    default property alias content: contentArea.data
    readonly property alias contentItem: contentArea

    Rectangle {
        id: flatBackground
        objectName: "panelFlatBackground"
        anchors.fill: parent
        visible: !panel.glass
        radius: panel.radius
        color: Theme.panelBg
        border.width: Theme.borderWidth
        border.color: Theme.panelBorder
    }

    Item {
        id: glassBackground
        objectName: "panelGlassBackground"
        anchors.fill: parent
        visible: panel.glass

        // T-016/T-020: when panel.backdropSource is set, sample the blurred backdrop here, under the tint, at this
        // panel's scene position (one shared texture; no blur per panel).
        Rectangle {
            anchors.fill: parent
            radius: panel.radius
            color: Theme.panelBgGlassTint
            border.width: Theme.borderWidth
            border.color: Theme.panelBorder
        }

        // Top-edge highlight: the top of an inner outline, clipped to the upper half of the corner arcs.
        Item {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.borderWidth
            height: Math.max(panel.radius / 2, Theme.borderWidth * 2)
            clip: true

            Rectangle {
                width: parent.width
                height: panel.height - 2 * Theme.borderWidth
                radius: Math.max(0, panel.radius - Theme.borderWidth)
                color: "transparent"
                border.width: Theme.borderWidth
                border.color: Theme.panelHighlightTop
            }
        }
    }

    Item {
        id: contentArea
        objectName: "panelContent"
        anchors.fill: parent
        anchors.margins: panel.padding
    }
}
