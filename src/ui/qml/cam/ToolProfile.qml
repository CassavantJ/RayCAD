// The side view of a tool, and of its holder when one is chosen (task T-046): CamTools.profile drawn to scale (the
// same scale across and along the axis, fitted to the item), with the axis as a dashed line and the cutting part in
// the accent colour. The geometry comes from the item's geometry fields only (cam/tools_logic.h toolProfile), so the
// drawing changes when, and only when, they do. Original drawing; colours from Theme only.
import QtQuick
import QtQuick.Shapes
import RayCad.Ui

Item {
    id: root
    objectName: "toolProfile"

    // {outline, cutter, holder: [point], halfWidth, height, stickOut, diameter, length} in mm, y up from the tip.
    property var profile: ({})
    readonly property bool hasProfile: profile !== undefined && profile !== null && profile.height !== undefined
                                       && profile.height > 0
    readonly property real margin: Theme.spacingM
    readonly property real labelSpace: Theme.rowHeight
    // px per mm, the same both ways.
    readonly property real scale: hasProfile
                                  ? Math.min((width - 2 * margin) / (2 * Math.max(profile.halfWidth, 0.001)),
                                             (height - 2 * margin - labelSpace) / Math.max(profile.height, 0.001))
                                  : 1
    readonly property real baseY: height - margin - labelSpace

    function mapped(list) {
        const out = []
        if (!list)
            return out
        for (let i = 0; i < list.length; ++i)
            out.push(Qt.point(width / 2 + list[i].x * scale, baseY - list[i].y * scale))
        return out
    }

    Accessible.role: Accessible.Graphic
    Accessible.name: hasProfile ? qsTr("Side view, %1 across, %2 long").arg(Units.formatLength(profile.diameter))
                                                                     .arg(Units.formatLength(profile.length))
                                : qsTr("No side view")

    Shape {
        id: shape
        objectName: "toolProfileShape"
        anchors.fill: parent
        visible: root.hasProfile
        preferredRendererType: Shape.CurveRenderer

        // The holder behind the tool.
        ShapePath {
            strokeColor: Theme.textSecondary
            strokeWidth: 1
            fillColor: Theme.controlBgPressed
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: root.mapped(root.profile.holder) }
        }
        // The tool.
        ShapePath {
            strokeColor: Theme.textPrimary
            strokeWidth: 1.25
            fillColor: Theme.controlBg
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: root.mapped(root.profile.outline) }
        }
        // Its cutting part.
        ShapePath {
            strokeColor: Theme.accentThinLine
            strokeWidth: 1.25
            fillColor: Theme.accentFill
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: root.mapped(root.profile.cutter) }
        }
        // The axis.
        ShapePath {
            strokeColor: Theme.textDisabled
            strokeWidth: 1
            fillColor: "transparent"
            strokeStyle: ShapePath.DashLine
            dashPattern: [4, 3]
            startX: root.width / 2
            startY: root.baseY + Theme.spacingXs
            PathLine {
                x: root.width / 2
                y: root.baseY - (root.hasProfile ? root.profile.height * root.scale : 0) - Theme.spacingXs
            }
        }
    }

    // The two sizes that matter most, under the drawing.
    Label {
        objectName: "toolProfileSizes"
        visible: root.hasProfile
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacingXs
        width: parent.width - 2 * Theme.spacingS
        horizontalAlignment: Text.AlignHCenter
        secondary: true
        font: Theme.smallFont
        text: root.hasProfile ? (Units.revision >= 0
                                 ? qsTr("Ø %1 · L %2").arg(Units.formatLength(root.profile.diameter))
                                                      .arg(Units.formatLength(root.profile.length))
                                 : "")
                              : ""
    }

    Label {
        visible: !root.hasProfile
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.spacingM
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        secondary: true
        text: qsTr("The side view appears when the sizes are valid")
    }
}
