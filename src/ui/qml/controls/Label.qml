// Themed text. `secondary` for supporting text; `mono` for numbers and coordinates.
import QtQuick
import RayCad.Ui

Text {
    property bool secondary: false
    property bool mono: false

    color: !enabled ? Theme.textDisabled : secondary ? Theme.textSecondary : Theme.textPrimary
    font: mono ? Theme.monoFont : Theme.uiFont
    elide: Text.ElideRight
    textFormat: Text.PlainText
    verticalAlignment: Text.AlignVCenter
    Accessible.role: Accessible.StaticText
    Accessible.name: text
}
