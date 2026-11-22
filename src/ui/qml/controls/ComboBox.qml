// Drop-down choice. The popup's rows are created when it first opens.
//
// Text is never cut where there is room: `naturalWidth` is the width that shows the current choice in full, for the
// layout to give the combo (a panel grows its field column to it, up to what it can spare, `widthLimit`); only past
// that is the shown text elided, with the whole of it in a tooltip. The open list is at least as wide as its longest
// choice (within the window). `descriptionRole`: a second, smaller line under each choice in the list (a choice's
// detail, such as the G codes of a cycle), also in the tooltip.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import RayCad.Ui

T.ComboBox {
    id: control

    property string descriptionRole: ""
    // The most width the layout could give this combo; past it the text is elided (tests check that an elided combo
    // is at its limit). By default its own width: a combo whose layout cannot grow it.
    property real widthLimit: width
    // The widths of the longest choice and of the longest description, as drawn.
    readonly property real widestText: {
        control.count; // a dependency: the model's rows
        control.model;
        let w = 0;
        for (let i = 0; i < control.count; ++i)
            w = Math.max(w, textMetrics.advanceWidth(control.textAt(i)));
        return Math.ceil(w);
    }
    // Choice i's description ("" none): from a list model (a JS array or a QVariantList of maps).
    function descriptionAt(i) {
        if (control.descriptionRole === "" || !control.model || control.model.length === undefined || i < 0
                || i >= control.model.length)
            return "";
        const item = control.model[i];
        return item && item[control.descriptionRole] !== undefined ? String(item[control.descriptionRole]) : "";
    }
    readonly property real widestDescription: {
        control.count;
        control.model;
        let w = 0;
        for (let i = 0; i < control.count; ++i)
            w = Math.max(w, descriptionMetrics.advanceWidth(control.descriptionAt(i)));
        return Math.ceil(w);
    }
    // The width that shows the current choice in full (the open list shows the others).
    readonly property real naturalWidth: Math.max(implicitBackgroundWidth,
                                                  Math.ceil(textMetrics.advanceWidth(displayText)) + leftPadding
                                                  + rightPadding)
    // The description of the current choice ("" none).
    readonly property string currentDescription: {
        control.model;
        return control.descriptionAt(control.currentIndex);
    }

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
    Accessible.description: currentDescription

    FontMetrics {
        id: textMetrics
        font: control.font
    }
    FontMetrics {
        id: descriptionMetrics
        font: Theme.smallFont
    }

    delegate: T.ItemDelegate {
        id: row
        required property var model
        required property int index
        readonly property string description: control.descriptionRole !== "" && row.model[control.descriptionRole] !== undefined
                                               ? String(row.model[control.descriptionRole]) : ""

        width: ListView.view ? ListView.view.width : implicitWidth
        implicitWidth: Math.max(implicitBackgroundWidth, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(Theme.rowHeight, implicitContentHeight + topPadding + bottomPadding)
        leftPadding: Theme.spacingS + Theme.iconSize + Theme.spacingXs
        rightPadding: Theme.spacingM
        topPadding: description !== "" ? Theme.spacingXs : 0
        bottomPadding: description !== "" ? Theme.spacingXs : 0
        text: model[control.textRole]
        font: control.font
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled
        Accessible.description: description

        contentItem: Column {
            Text {
                width: parent.width
                // Alone, it fills the row and is centred in it; over a description, it takes its own height.
                height: row.description !== "" ? implicitHeight : parent.height
                text: row.text
                font: row.font
                color: Theme.textPrimary
                elide: Text.ElideRight
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignVCenter
            }
            Text {
                visible: row.description !== ""
                width: parent.width
                text: row.description
                font: Theme.smallFont
                color: Theme.textSecondary
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
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

    // The whole choice when the shown text is cut, and its description when it has one.
    HoverToolTip {
        target: control
        text: control.contentItem.truncated
              ? control.displayText + (control.currentDescription !== "" ? "\n" + control.currentDescription : "")
              : control.currentDescription
    }

    popup: T.Popup {
        objectName: "comboPopup"
        y: control.height + Theme.spacingXs
        // As wide as the longest choice (row paddings and the list's own), never narrower than the combo; kept inside
        // the window by the margins.
        width: Math.min(control.Window.width > 0 ? control.Window.width - leftMargin - rightMargin : Infinity,
                        Math.max(control.width,
                                 Math.max(control.widestText, control.widestDescription)
                                 + Theme.spacingS + Theme.iconSize + Theme.spacingXs + Theme.spacingM
                                 + leftPadding + rightPadding + Theme.spacingS))
        height: Math.min(implicitHeight, control.Window.height - topMargin - bottomMargin)
        implicitHeight: list.contentHeight + topPadding + bottomPadding
        topMargin: Theme.spacingS
        bottomMargin: Theme.spacingS
        leftMargin: Theme.spacingS
        rightMargin: Theme.spacingS
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
