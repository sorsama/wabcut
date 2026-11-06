pragma ComponentBehavior: Bound
import QtQuick
import QtCore
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// File > Collect Files… (I.4): a portable project capsule (Collect). One folder with the project, the media its
// sequences use (whole, or trimmed to the used ranges + handles), optionally its proxies, the fonts its titles and
// captions use, and its analysis results, plus a manifest. Free space is checked first; Cancel leaves nothing behind;
// when it is done the capsule is reopened elsewhere to prove no clip is offline. Used ranges come with handles of 0 to 10 s
// (remembered), and a checklist picks the sequences to archive (the final cut alone leaves out every abandoned version's
// footage; the sequences a chosen one nests or plays come along).
WabcutDialog {
    id: dialog
    objectName: "collectFilesDialog"

    property url folder: Collect.suggestedFolder
    /// Card 003: the handle length (seconds) kept for the next time.
    readonly property real handlesSeconds: remembered.handlesSeconds
    /// Which sequences are ticked, by index in ProjectController.sequenceNames (an index not listed here is ticked).
    property var unticked: ({})
    readonly property int sequenceCount: ProjectController.sequenceNames.length
    readonly property int chosenCount: {
        let n = 0
        for (let i = 0; i < dialog.sequenceCount; ++i) {
            if (dialog.unticked[i] !== true)
                ++n
        }
        return n
    }

    Settings {
        id: remembered
        category: "collect"
        property real handlesSeconds: 1.0
        /// Card 076: "flat", "bins" or "folders" (how the media are laid out below Media/), kept for the next time.
        property string layout: "flat"
    }
    readonly property var layoutNames: ["flat", "bins", "folders"]

    function setAllSequences(on: bool) {
        const next = ({})
        if (!on) {
            for (let i = 0; i < dialog.sequenceCount; ++i)
                next[i] = true
        }
        dialog.unticked = next
        refreshEstimate()
    }
    function options() {
        const result = {
            name: nameField.text,
            trim: trimTabs.currentIndex === 1,
            handles: dialog.handlesSeconds,
            layout: dialog.layoutNames[layoutTabs.currentIndex] ?? "flat",
            proxies: proxiesToggle.checked,
            fonts: fontsToggle.checked,
            analysis: analysisToggle.checked,
            unused: unusedToggle.checked
        }
        // All ticked: no list (every sequence, also ones made while the dialog is open).
        if (dialog.chosenCount < dialog.sequenceCount) {
            const picked = []
            for (let i = 0; i < dialog.sequenceCount; ++i) {
                if (dialog.unticked[i] !== true)
                    picked.push(i)
            }
            result.sequences = picked
        }
        return result
    }
    // The size is worked out on a worker thread (card #173): toggling an option never freezes the dialog.
    function refreshEstimate() {
        if (!Collect.running)
            Collect.requestEstimate(folder, options())
    }

    title: qsTr("Collect Files")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))
    onOpened: {
        nameField.text = Collect.suggestedName
        dialog.unticked = ({})
        layoutTabs.currentIndex = Math.max(0, dialog.layoutNames.indexOf(remembered.layout))
        refreshEstimate()
    }

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    // 0.7 (card 003): the body scrolls when the window is too short for all of it (the smallest window is 1100 x 700);
    // the buttons stay in view below it.
    Flickable {
        id: scroller
        objectName: "collectScroller"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(bodyColumn.implicitHeight,
                                         Math.max(Theme.space(40), (dialog.parent ? dialog.parent.height : Theme.space(170)) - Theme.space(64)))
        contentWidth: width
        contentHeight: bodyColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: bodyColumn
            width: scroller.width - Theme.space(3) // room for the scroll bar
            spacing: Theme.space(3)

            Note {
                text: qsTr("Copies the project and everything it uses into one folder that opens anywhere: media, fonts, and (if you like) proxies and analysis results, with a manifest of every file.")
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.space(4)
                rowSpacing: Theme.space(2)
                enabled: !Collect.running

                Label { text: qsTr("Name") }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Theme.controlHeightSmall
                    radius: Theme.radiusField
                    color: Theme.sunken
                    border.width: Theme.hairline
                    border.color: nameField.activeFocus ? Theme.accentStroke : Theme.border
                    TextInput {
                        id: nameField
                        objectName: "collectNameField"
                        anchors.fill: parent
                        anchors.leftMargin: Theme.space(2)
                        anchors.rightMargin: Theme.space(2)
                        verticalAlignment: TextInput.AlignVCenter
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        clip: true
                        selectByMouse: true
                        onEditingFinished: dialog.refreshEstimate()
                    }
                }

                Label { text: qsTr("Folder") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        objectName: "collectFolderText"
                        text: dialog.folder.toString().replace(/^file:\/\/\/?/, "")
                        elide: Text.ElideMiddle
                        color: Theme.textPrimary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textSm
                    }
                    PillButton {
                        compact: true
                        text: qsTr("Choose…")
                        onClicked: folderDialog.open()
                    }
                }

                Label { text: qsTr("Media") }
                PillTabs {
                    id: trimTabs
                    Accessible.name: qsTr("Media") // P1 #61
                    objectName: "collectTrimTabs"
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("Whole files"), qsTr("Used ranges + handles")]
                    onActivated: dialog.refreshEstimate()
                }

                Label { text: qsTr("Handles") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    NumberField {
                        id: handlesField
                        objectName: "collectHandlesField"
                        Accessible.name: qsTr("Handles in seconds")
                        value: dialog.handlesSeconds
                        from: 0
                        to: 10
                        stepSize: 0.5
                        decimals: 1
                        suffix: qsTr("s")
                        enabled: trimTabs.currentIndex === 1
                        onValueModified: v => {
                            remembered.handlesSeconds = v
                            dialog.refreshEstimate()
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Extra footage kept before and after each used range, for regrading and re-cutting.")
                        wrapMode: Text.Wrap
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }

                // Card 076: where the media files go inside the capsule's Media folder.
                Label {
                    text: qsTr("Folders")
                    Layout.alignment: Qt.AlignTop
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    PillTabs {
                        id: layoutTabs
                        objectName: "collectLayoutTabs"
                        Accessible.name: qsTr("Folder layout of the media")
                        Layout.fillWidth: true
                        compact: true
                        model: [qsTr("All in one folder"), qsTr("Project bins"), qsTr("Original folders")]
                        onActivated: remembered.layout = dialog.layoutNames[layoutTabs.currentIndex] ?? "flat"
                    }
                    Note {
                        objectName: "collectLayoutNote"
                        text: layoutTabs.currentIndex === 1
                              ? qsTr("Media go into folders named like the project's bins, so the capsule reads like the Project panel.")
                              : layoutTabs.currentIndex === 2
                                ? qsTr("Media keep the folder structure they have now, below the folder they share.")
                                : qsTr("Every file goes into one Media folder; names are made unique.")
                    }
                }

                Label {
                    text: qsTr("Sequences")
                    Layout.alignment: Qt.AlignTop
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        PillButton {
                            objectName: "collectSequencesAll"
                            compact: true
                            text: qsTr("All")
                            onClicked: dialog.setAllSequences(true)
                        }
                        PillButton {
                            objectName: "collectSequencesNone"
                            compact: true
                            text: qsTr("None")
                            onClicked: dialog.setAllSequences(false)
                        }
                        Text {
                            objectName: "collectSequencesCount"
                            Layout.fillWidth: true
                            text: qsTr("%1 of %2 chosen").arg(dialog.chosenCount).arg(dialog.sequenceCount)
                            elide: Text.ElideRight
                            color: dialog.chosenCount > 0 ? Theme.textSecondary : Theme.warning
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                    // One switch per sequence in a scrolling column (a few dozen at most: no virtualised list needed).
                    Flickable {
                        id: sequenceList
                        objectName: "collectSequenceList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(sequenceColumn.implicitHeight, Theme.space(34))
                        contentWidth: width
                        contentHeight: sequenceColumn.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ThinScrollBar {}
                        Accessible.role: Accessible.List
                        Accessible.name: qsTr("Sequences to collect")
                        Column {
                            id: sequenceColumn
                            width: sequenceList.width
                            spacing: Theme.space(1)
                            Repeater {
                                model: ProjectController.sequenceNames
                                delegate: Toggle {
                                    id: sequenceToggle
                                    required property string modelData
                                    required property int index
                                    objectName: "collectSequence_" + sequenceToggle.index
                                    width: sequenceColumn.width
                                    text: sequenceToggle.modelData
                                    checked: dialog.unticked[sequenceToggle.index] !== true
                                    onToggled: {
                                        const next = Object.assign({}, dialog.unticked)
                                        if (sequenceToggle.checked)
                                            delete next[sequenceToggle.index]
                                        else
                                            next[sequenceToggle.index] = true
                                        dialog.unticked = next
                                        dialog.refreshEstimate()
                                    }
                                }
                            }
                        }
                    }
                    Note {
                        text: qsTr("Sequences a chosen one nests or plays are collected with it. Media only the other sequences use stays out.")
                    }
                }

                Item { implicitWidth: 1; implicitHeight: 1 }
                Column {
                    spacing: Theme.space(1)
                    Toggle {
                        id: proxiesToggle
                        objectName: "collectProxiesToggle"
                        text: qsTr("Include proxies")
                        enabled: trimTabs.currentIndex === 0
                        onToggled: dialog.refreshEstimate()
                    }
                    Toggle {
                        id: fontsToggle
                        objectName: "collectFontsToggle"
                        text: qsTr("Include fonts of titles and captions")
                        checked: true
                        onToggled: dialog.refreshEstimate()
                    }
                    Toggle {
                        id: analysisToggle
                        objectName: "collectAnalysisToggle"
                        text: qsTr("Include transcripts and analysis results")
                        onToggled: dialog.refreshEstimate()
                    }
                    Toggle {
                        id: unusedToggle
                        objectName: "collectUnusedToggle"
                        text: qsTr("Include media no sequence uses")
                        onToggled: dialog.refreshEstimate()
                    }
                }
            }
            Note {
                objectName: "collectEstimateText"
                visible: !Collect.running
                text: (Collect.estimating ? qsTr("Working out the size…") : Collect.estimateText) + (fontsToggle.checked ? "\n" + qsTr("Fonts that come with Windows are listed in the manifest but not copied.") : "")
            }

            // ---- Progress ----
            ColumnLayout {
                Layout.fillWidth: true
                visible: Collect.running
                spacing: Theme.space(1)
                Text {
                    Layout.fillWidth: true
                    text: Collect.current !== "" ? qsTr("%1 · %2").arg(Collect.phase).arg(Collect.current) : Collect.phase
                    elide: Text.ElideMiddle
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                BeamProgress {
                    objectName: "collectProgress"
                    Layout.fillWidth: true
                    value: Collect.progress
                    active: Collect.running
                    label: Collect.phase
                    loader: "build"
                }
            }

            // ---- Result ----
            Rectangle {
                objectName: "collectResult"
                Layout.fillWidth: true
                visible: !Collect.running && Collect.result.ok !== undefined
                implicitHeight: resultColumn.implicitHeight + Theme.space(4)
                radius: Theme.radiusField
                color: Theme.sunken
                ColumnLayout {
                    id: resultColumn
                    anchors.fill: parent
                    anchors.margins: Theme.space(2)
                    spacing: Theme.space(1)
                    Text {
                        Layout.fillWidth: true
                        text: Collect.result.ok
                              ? (Collect.result.verified ? qsTr("Collected and verified: reopened elsewhere, no clip offline, every file matches the manifest.")
                                                         : qsTr("Collected, but the check found %n offline clip(s)", "", Collect.result.offline ?? 0))
                              : (Collect.result.error ?? "")
                        wrapMode: Text.Wrap
                        color: Collect.result.ok && Collect.result.verified ? Theme.success : Theme.warning
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: Collect.result.ok === true
                        text: qsTr("%n file(s) · %1 media (%2 trimmed) · %3", "", Collect.result.files ?? 0).arg(Collect.result.media ?? 0)
                              .arg(Collect.result.trimmed ?? 0).arg(Collect.result.size ?? "")
                              + "\n" + (Collect.result.folderPath ?? "")
                              + ((Collect.result.notes ?? []).length > 0 ? "\n" + (Collect.result.notes ?? []).join("\n") : "")
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
            }

        }
    }

    FolderDialog {
        id: folderDialog
        currentFolder: dialog.folder
        onAccepted: {
            dialog.folder = selectedFolder
            dialog.refreshEstimate()
        }
    }

    footerData: [
        PillButton {
            visible: Collect.result.ok === true
            text: qsTr("Show folder")
            onClicked: Collect.reveal()
        },
        PillButton {
            text: Collect.running ? qsTr("Cancel") : qsTr("Close")
            onClicked: Collect.running ? Collect.cancel() : dialog.close()
        },
        PillButton {
            objectName: "collectStartButton"
            variant: "primary"
            visible: !Collect.running
            enabled: dialog.chosenCount > 0
            text: qsTr("Collect")
            onClicked: Collect.start(dialog.folder, dialog.options())
        }
    ]
}
