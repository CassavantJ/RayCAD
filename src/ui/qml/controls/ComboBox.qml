// Drop-down choice. The popup's rows are created when it first opens.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    topPadding: Theme.spacingXs
    bottomPadding: Theme.spacingXs
    leftPadding: Theme.spacingM
    rightPadding: Theme.spacingS + Theme.iconSize + Theme.spacingXs
    font: Theme.uiFont
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: displayText

    delegate: T.ItemDelegate {
        id: row
        required property var model
        required property int index

        width: ListView.view ? ListView.view.width : implicitWidth
        implicitWidth: Math.max(implicitBackgroundWidth, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(Theme.rowHeight, implicitContentHeight + topPadding + bottomPadding)
        leftPadding: Theme.spacingS + Theme.iconSize + Theme.spacingXs
        rightPadding: Theme.spacingM
        text: model[control.textRole]
        font: control.font
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled

        contentItem: Text {
            text: row.text
            font: row.font
            color: Theme.textPrimary
            elide: Text.ElideRight
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignVCenter
        }
        Icon {
            visible: control.currentIndex === row.index
            name: "check"
            color: Theme.accentText
            x: Theme.spacingS
            anchors.verticalCenter: parent.verticalCenter
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: row.highlighted ? Theme.controlBgHover : "transparent"
        }
    }

    indicator: Icon {
        name: "chevron-down"
        color: control.enabled ? Theme.textSecondary : Theme.textDisabled
        x: control.width - width - Theme.spacingS
        y: control.topPadding + Math.round((control.availableHeight - height) / 2)
    }

    contentItem: Text {
        objectName: "label"
        text: control.displayText
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        elide: Text.ElideRight
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight * 5
        implicitHeight: Theme.controlHeight
        radius: Math.min(Theme.radiusControl, height / 2)
        color: control.down ? Theme.controlBgPressed
               : control.hovered && control.enabled ? Theme.controlBgHover
               : Theme.controlBg
        border.width: Theme.borderWidth
        border.color: Theme.panelBorder

        FocusRing {
            visible: control.visualFocus
        }
    }

    popup: T.Popup {
        y: control.height + Theme.spacingXs
        width: control.width
        height: Math.min(implicitHeight, control.Window.height - topMargin - bottomMargin)
        implicitHeight: list.contentHeight + topPadding + bottomPadding
        topMargin: Theme.spacingS
        bottomMargin: Theme.spacingS
        padding: Theme.spacingXs

        contentItem: ListView {
            id: list
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            T.ScrollBar.vertical: ScrollBar {}
        }

        background: Rectangle {
            color: Theme.panelBg
            radius: Theme.radiusSmall + Theme.spacingXs
            border.width: Theme.borderWidth
            border.color: Theme.panelBorder
        }
    }
}
