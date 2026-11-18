// A row of a list or tree (model tree, operation list, recent files). Depth indents it; `expandable` shows the
// disclosure chevron (click it, or Right/Left, to expand and collapse). `highlighted` is the selected state.
// As a ListView / TreeView delegate, the view owns keyboard focus and sets highlighted from its selection.
import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.ItemDelegate {
    id: control

    property int depth: 0
    property bool expandable: false
    property bool expanded: false
    property string iconName: ""
    property string secondaryText: ""
    readonly property real indent: depth * Theme.spacingL

    signal expandToggled()

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    leftInset: Theme.spacingXs
    rightInset: Theme.spacingXs
    topPadding: Theme.spacingXs / 2
    bottomPadding: Theme.spacingXs / 2
    leftPadding: leftInset + Theme.spacingXs + indent + Theme.iconSize + Theme.spacingXs
    rightPadding: rightInset + Theme.spacingS
    spacing: Theme.spacingS
    font: Theme.uiFont
    hoverEnabled: true

    Accessible.role: expandable || depth > 0 ? Accessible.TreeItem : Accessible.ListItem
    Accessible.name: secondaryText !== "" ? text + ", " + secondaryText : text
    Accessible.selected: highlighted
    Accessible.expandable: expandable
    Accessible.expanded: expanded

    Keys.onRightPressed: event => {
        if (expandable && !expanded)
            expandToggled()
        else
            event.accepted = false
    }
    Keys.onLeftPressed: event => {
        if (expandable && expanded)
            expandToggled()
        else
            event.accepted = false
    }

    Icon {
        id: chevron
        objectName: "chevron"
        visible: control.expandable
        name: control.expanded ? "chevron-down" : "chevron-right"
        color: Theme.textSecondary
        x: control.leftInset + Theme.spacingXs + control.indent
        anchors.verticalCenter: parent.verticalCenter

        TapHandler {
            onTapped: control.expandToggled()
        }
    }

    contentItem: Item {
        implicitWidth: main.implicitWidth + (secondary.visible ? control.spacing + secondary.implicitWidth : 0)
        implicitHeight: Math.max(main.implicitHeight, secondary.implicitHeight, Theme.rowHeight - control.topPadding - control.bottomPadding)

        IconText {
            id: main
            anchors.left: parent.left
            anchors.right: secondary.visible ? secondary.left : parent.right
            anchors.rightMargin: secondary.visible ? control.spacing : 0
            anchors.verticalCenter: parent.verticalCenter
            alignment: Qt.AlignLeft
            iconName: control.iconName
            iconColor: control.enabled ? Theme.textSecondary : Theme.textDisabled
            text: control.text
            font: control.font
            spacing: control.spacing
            color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        }
        Text {
            id: secondary
            objectName: "secondaryText"
            visible: control.secondaryText !== ""
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: control.secondaryText
            font: Theme.monoFont
            color: control.enabled ? Theme.textSecondary : Theme.textDisabled
            textFormat: Text.PlainText
        }
    }

    background: Rectangle {
        implicitHeight: Theme.rowHeight
        radius: Theme.radiusSmall
        color: control.highlighted ? Theme.selectionFill
               : control.down ? Theme.controlBgPressed
               : control.hovered && control.enabled ? Theme.controlBgHover
               : "transparent"
        border.width: control.visualFocus ? Theme.focusRingWidth : 0
        border.color: Theme.focusRing

        Rectangle {
            visible: control.highlighted
            width: Theme.focusRingWidth
            height: parent.height - 2 * Theme.spacingXs
            radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            x: Theme.borderWidth
            color: Theme.selection
        }
    }
}
