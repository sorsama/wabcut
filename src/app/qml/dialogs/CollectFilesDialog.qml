pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// File > Collect Files… (I.4): a portable project capsule (Collect). One folder with the project, the media its
// sequences use (whole, or trimmed to the used ranges + handles), optionally its proxies, the fonts its titles and
// captions use, and its analysis results, plus a manifest. Free space is checked first; Cancel leaves nothing behind;
// when it is done the capsule is reopened elsewhere to prove no clip is offline.
WabcutDialog {
    id: dialog
    objectName: "collectFilesDialog"

    property url folder: Collect.suggestedFolder
    property string estimateText: ""

    function options() {
        return {
            name: nameField.text,
            trim: trimTabs.currentIndex === 1,
            handles: 1.0,
            proxies: proxiesToggle.checked,
            fonts: fontsToggle.checked,
            analysis: analysisToggle.checked,
            unused: unusedToggle.checked
        }
    }
    function refreshEstimate() {
        estimateText = Collect.running ? "" : Collect.estimate(folder, options())
    }

    title: qsTr("Collect Files")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))
    onOpened: {
        nameField.text = Collect.suggestedName
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
            objectName: "collectTrimTabs"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Whole files"), qsTr("Used ranges + 1 s handles")]
            onActivated: dialog.refreshEstimate()
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
        text: dialog.estimateText + (fontsToggle.checked ? "\n" + qsTr("Fonts that come with Windows are listed in the manifest but not copied.") : "")
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
                text: qsTr("%1 files · %2 media (%3 trimmed) · %4").arg(Collect.result.files ?? 0).arg(Collect.result.media ?? 0)
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
            text: qsTr("Collect")
            onClicked: Collect.start(dialog.folder, dialog.options())
        }
    ]
}
