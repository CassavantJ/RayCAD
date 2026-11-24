// The fixtures panel of the CAM workspace (task T-077; CamFixtures, cam/fixtures_tool.h) in the shell's viewportArea.
// It lists the document's fixtures (collision bodies, never machined), adds one from the libraries' fixture models
// (a generic vise, parallels) or from a STEP file, and places one: a click on the fixture's face that goes against the
// part, a click on the part's face, then a gap, X, Y, Z and a turn. Every change previews the fixture where it would
// go; Apply (Enter) places it as one undo step, Cancel (Esc) leaves the document as it was; Esc with nothing being
// placed closes the panel.
import QtQuick
import QtQuick.Dialogs
import RayCad.Ui

Item {
    id: root
    objectName: "fixturesPanelHost"

    // Below the selection filter, which the shell shows over the view while there are bodies.
    property real topInset: Scene.bodyCount > 0 ? Theme.controlHeight + 2 * Theme.spacingXs + Theme.panelGap : 0
    readonly property real triadSpace: Theme.controlHeight * 2 + Theme.panelGap
    readonly property real labelWidth: Theme.controlHeight * 3.6
    readonly property real fieldWidth: Theme.controlHeight * 6.5
    readonly property real rowWidth: labelWidth + fieldWidth + Theme.spacingS
    readonly property var values: CamFixtures.values
    readonly property bool placing: CamFixtures.placing !== ""
    // How much of the view the panel covers from the left edge (0 while closed): Fit frames the model beside it.
    readonly property real coverLeft: panel.visible ? panel.x + panel.width + Theme.panelGap : 0

    // ---------- rows ----------
    component FieldRow: Column {
        id: fieldRow
        property string key
        property string label
        property string placeholder: ""
        readonly property bool erroneous: CamFixtures.messageField === key && CamFixtures.message !== ""
        objectName: "fixtureField." + key
        spacing: Theme.spacingXs / 2
        Row {
            spacing: Theme.spacingS
            Label {
                width: root.labelWidth
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: fieldRow.label
            }
            TextField {
                objectName: "fixtureInput." + fieldRow.key
                width: root.fieldWidth
                text: root.values[fieldRow.key] !== undefined ? String(root.values[fieldRow.key]) : ""
                placeholderText: fieldRow.placeholder
                invalid: fieldRow.erroneous
                Accessible.name: fieldRow.label
                onTextEdited: CamFixtures.setValue(fieldRow.key, text)
                Keys.onReturnPressed: CamFixtures.commit()
                Keys.onEnterPressed: CamFixtures.commit()
                Keys.onEscapePressed: CamFixtures.escape()
            }
        }
        Label {
            objectName: "fixtureFieldError." + fieldRow.key
            visible: fieldRow.erroneous
            x: root.labelWidth + Theme.spacingS
            width: root.fieldWidth
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            font: Theme.smallFont
            color: Theme.error
            text: CamFixtures.message
        }
    }

    // A face picked in the view: what is picked and a button that makes the view's clicks pick it.
    component PickRow: Column {
        id: pickRow
        property string mode // "face", "target"
        property string label
        property string pickedText
        property string hint
        readonly property bool erroneous: CamFixtures.messageField === mode && CamFixtures.message !== ""
        objectName: "fixturePick." + mode
        spacing: Theme.spacingXs / 2
        Row {
            spacing: Theme.spacingS
            Label {
                width: root.labelWidth
                anchors.verticalCenter: parent.verticalCenter
                secondary: true
                text: pickRow.label
            }
            Label {
                objectName: "fixturePickText." + pickRow.mode
                width: root.fieldWidth - pickButton.width - Theme.spacingXs
                anchors.verticalCenter: parent.verticalCenter
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                maximumLineCount: 2
                text: pickRow.pickedText
            }
            ToolButton {
                id: pickButton
                objectName: "fixturePickButton." + pickRow.mode
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                iconName: "select"
                checked: CamFixtures.pickMode === pickRow.mode
                toolTipText: pickRow.hint
                Accessible.name: pickRow.label
                Accessible.checkable: true
                Accessible.checked: checked
                onClicked: CamFixtures.setPickMode(checked ? "" : pickRow.mode)
            }
        }
        Label {
            visible: pickRow.erroneous
            x: root.labelWidth + Theme.spacingS
            width: root.fieldWidth
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            font: Theme.smallFont
            color: Theme.error
            text: CamFixtures.message
        }
    }

    // ---------- the panel ----------
    PanelBase {
        id: panel
        objectName: "fixturesPanel"
        visible: CamFixtures.shown
        x: 0
        y: root.topInset
        width: column.implicitWidth + 2 * Theme.spacingM
        height: Math.min(root.height - root.topInset - root.triadSpace, column.implicitHeight + 2 * Theme.spacingM)
        clip: true

        Flickable {
            anchors.fill: parent
            anchors.margins: Theme.spacingM
            contentHeight: column.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height

            Column {
                id: column
                spacing: Theme.spacingS

                Item {
                    width: root.rowWidth
                    height: Math.max(titleRow.height, closeButton.height)
                    Row {
                        id: titleRow
                        spacing: Theme.spacingS
                        anchors.verticalCenter: parent.verticalCenter
                        Icon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: "lock"
                            color: Theme.accentText
                        }
                        Label {
                            objectName: "fixturesTitle"
                            anchors.verticalCenter: parent.verticalCenter
                            font: Theme.strongFont
                            text: qsTr("Fixtures")
                        }
                    }
                    ToolButton {
                        id: closeButton
                        objectName: "fixturesClose"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        compact: true
                        iconName: "close"
                        toolTipText: qsTr("Close the panel (Esc)")
                        Accessible.name: qsTr("Close the fixtures panel")
                        onClicked: CamFixtures.close()
                    }
                }
                Label {
                    width: root.rowWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    secondary: true
                    font: Theme.smallFont
                    text: qsTr("Fixtures are collision bodies: the safety checks test the tool and holder against them; they are never machined.")
                }

                // The document's fixtures.
                Label {
                    objectName: "fixturesEmpty"
                    visible: CamFixtures.fixtures.length === 0
                    width: root.rowWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    secondary: true
                    text: qsTr("No fixture yet: add a vise or parallels from the library, or import your own.")
                }
                Repeater {
                    model: CamFixtures.fixtures
                    delegate: Column {
                        id: fixtureRow
                        required property var modelData
                        required property int index
                        readonly property bool current: CamFixtures.placing === modelData.id
                        objectName: "fixtureRow." + index
                        spacing: Theme.spacingXs / 2
                        Row {
                            spacing: Theme.spacingS
                            Label {
                                objectName: "fixtureName." + fixtureRow.index
                                width: root.rowWidth - placeButton.width - Theme.spacingS
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideMiddle
                                font: fixtureRow.current ? Theme.strongFont : Theme.uiFont
                                text: fixtureRow.modelData.name
                            }
                            Button {
                                id: placeButton
                                objectName: "fixturePlace." + fixtureRow.index
                                anchors.verticalCenter: parent.verticalCenter
                                enabled: !fixtureRow.current && !CamFixtures.importing
                                text: qsTr("Place")
                                toolTipText: qsTr("Place %1 against the part").arg(fixtureRow.modelData.name)
                                onClicked: CamFixtures.place(fixtureRow.modelData.id)
                            }
                        }
                        Row {
                            spacing: Theme.spacingS
                            leftPadding: Theme.spacingM
                            Label {
                                objectName: "fixtureState." + fixtureRow.index
                                anchors.verticalCenter: parent.verticalCenter
                                secondary: true
                                font: Theme.smallFont
                                text: fixtureRow.modelData.mated ? qsTr("Mated to a face")
                                      : fixtureRow.modelData.placed ? qsTr("Moved") : qsTr("Where the file put it")
                            }
                            CheckBox {
                                objectName: "fixtureInSetup." + fixtureRow.index
                                visible: CamFixtures.setupName !== ""
                                checked: fixtureRow.modelData.inSetup
                                text: qsTr("In %1").arg(CamFixtures.setupName)
                                Accessible.name: qsTr("%1 is a fixture of %2").arg(fixtureRow.modelData.name)
                                                                                .arg(CamFixtures.setupName)
                                onToggled: CamFixtures.setInSetup(fixtureRow.modelData.id, checked)
                            }
                        }
                    }
                }

                // Adding one.
                Label {
                    text: qsTr("Add")
                    font: Theme.strongFont
                }
                Row {
                    spacing: Theme.spacingS
                    ComboBox {
                        id: libraryCombo
                        objectName: "fixturesLibrary"
                        width: root.rowWidth - addButton.width - Theme.spacingS
                        model: CamFixtures.library
                        textRole: "name"
                        enabled: CamFixtures.library.length > 0 && !CamFixtures.importing
                        Accessible.name: qsTr("Fixture models of the library")
                    }
                    Button {
                        id: addButton
                        objectName: "fixturesAdd"
                        enabled: libraryCombo.currentIndex >= 0 && CamFixtures.library.length > 0
                                 && !CamFixtures.importing
                        text: qsTr("Add")
                        toolTipText: qsTr("Add the chosen library model to the document as a fixture")
                        onClicked: CamFixtures.addFromLibrary(libraryCombo.currentIndex)
                    }
                }
                Button {
                    objectName: "fixturesImport"
                    enabled: !CamFixtures.importing
                    text: CamFixtures.importing ? qsTr("Importing…") : qsTr("Import STEP as fixture…")
                    toolTipText: qsTr("Import a STEP or IGES file (your vise, a fixture plate) as fixtures")
                    onClicked: CamFixtures.requestImport()
                }

                // The placement of one fixture.
                Column {
                    objectName: "fixturePlacement"
                    visible: root.placing
                    spacing: Theme.spacingS
                    Label {
                        objectName: "fixturePlacementTitle"
                        width: root.rowWidth
                        elide: Text.ElideMiddle
                        font: Theme.strongFont
                        text: qsTr("Place %1").arg(CamFixtures.placingName)
                    }
                    PickRow {
                        mode: "face"
                        label: qsTr("Fixture face")
                        pickedText: CamFixtures.faceText
                        hint: qsTr("Click the planar face of the fixture that goes against the part (a jaw's face, a parallel's top)")
                    }
                    PickRow {
                        mode: "target"
                        label: qsTr("Against")
                        pickedText: CamFixtures.targetText
                        hint: qsTr("Click the planar face it goes against: the part's side or bottom, or another fixture's face")
                    }
                    FieldRow {
                        key: "gap"
                        label: qsTr("Gap")
                        placeholder: qsTr("0: the faces touch")
                    }
                    FieldRow {
                        key: "angle"
                        label: qsTr("Turn")
                        placeholder: qsTr("Degrees about the face's normal")
                    }
                    FieldRow {
                        key: "x"
                        label: qsTr("Move X")
                        placeholder: "0"
                    }
                    FieldRow {
                        key: "y"
                        label: qsTr("Move Y")
                        placeholder: "0"
                    }
                    FieldRow {
                        key: "z"
                        label: qsTr("Move Z")
                        placeholder: "0"
                    }
                    Button {
                        objectName: "fixtureClearMate"
                        enabled: CamFixtures.hasFace || CamFixtures.hasTarget
                        text: qsTr("Clear mate")
                        toolTipText: qsTr("Forget both faces: the fixture is moved by the numbers alone")
                        onClicked: CamFixtures.clearMate()
                    }
                }

                // The message that names no field.
                Label {
                    objectName: "fixturesMessage"
                    visible: CamFixtures.message !== "" && CamFixtures.messageField === ""
                    width: root.rowWidth
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                    color: CamFixtures.previewStatus === "failed" ? Theme.error : Theme.textSecondary
                    text: CamFixtures.message
                }
                Row {
                    visible: root.placing
                    spacing: Theme.spacingS
                    layoutDirection: Qt.RightToLeft
                    width: root.rowWidth
                    Button {
                        objectName: "fixtureApply"
                        primary: true
                        text: qsTr("Apply")
                        enabled: CamFixtures.canCommit
                        toolTipText: qsTr("Place the fixture (Enter)")
                        onClicked: CamFixtures.commit()
                    }
                    Button {
                        objectName: "fixtureCancel"
                        text: qsTr("Cancel")
                        toolTipText: qsTr("Leave the fixture where it was (Esc)")
                        onClicked: CamFixtures.cancel()
                    }
                }
                // Below the buttons, so that Apply and Cancel never move while a preview builds.
                Label {
                    objectName: "fixturePreviewState"
                    visible: root.placing && text !== ""
                    width: root.rowWidth
                    wrapMode: Text.Wrap
                    secondary: true
                    font: Theme.smallFont
                    text: CamFixtures.previewStatus === "building" ? qsTr("Building the preview…")
                          : CamFixtures.previewStatus === "failed" ? qsTr("The preview shows the last values that worked")
                          : CamFixtures.pickMode === "face" ? qsTr("Click the fixture's face that goes against the part")
                          : CamFixtures.pickMode === "target" ? qsTr("Click the part's face it goes against")
                          : CamFixtures.canCommit ? qsTr("Ready: Apply places the fixture")
                          : ""
                }
            }
        }
    }

    // Esc cancels the placement (or closes the panel) wherever the focus is while no job claims it and no dialog over
    // the workspace (post, setup sheet, tool library, templates) has its own Esc: two enabled Esc shortcuts are
    // ambiguous to Qt and neither would fire (Shell.qml stands back for the same dialogs).
    Shortcut {
        sequence: "Esc"
        enabled: CamFixtures.shown && !Session.jobActive && !CamPost.active && !CamTools.shown
                 && !CamTemplates.shown && !CamSheet.active
        onActivated: CamFixtures.escape()
    }

    // The file dialog of "Import STEP as fixture" (not shown in tests: Files.useNativeDialogs is off there).
    Connections {
        target: CamFixtures
        function onImportDialogRequested() {
            if (!Files.useNativeDialogs)
                return
            importLoader.active = true
            importLoader.item.open()
        }
    }
    Loader {
        id: importLoader
        active: false
        sourceComponent: FileDialog {
            objectName: "fixtureImportDialog"
            title: qsTr("Import a fixture")
            nameFilters: Files.importFilters
            fileMode: FileDialog.OpenFile
            onAccepted: CamFixtures.importFile(selectedFile)
        }
    }
}
