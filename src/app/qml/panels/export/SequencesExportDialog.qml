pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// P1 round (#22): export several sequences at once - Premiere's File > Export > Media with sequences selected in the
// Project panel (each one queued in Media Encoder). Opened from the Library's Export… / Send to Export Queue… rows and
// from File > Send to Media Encoder while the Library has the focus (Main.openSequencesExport). One preset for every
// sequence, or "Match each sequence's shape" (each sequence's remembered preset for its shape, else YouTube 1080p60 for
// landscape and Shorts / TikTok / Reels for vertical); a folder (each file is named after its sequence, numbered when the
// name is taken); "Wait for Start Queue". The list shows what each sequence becomes before anything is queued, and a
// sequence that cannot export (offline media, a preset that does not fit) says why - then nothing is queued.
WabcutDialog {
    id: dialog
    objectName: "sequencesExportDialog"

    /// The sequences' "s…" keys.
    property var keys: []
    /// "" = match each sequence's shape, else an export preset id.
    property string presetId: ""
    property url folder
    property bool hold: true
    property string errorText: ""
    /// ExportController.sequenceExports(keys, options): what each sequence would write.
    property var entries: []
    readonly property int readyCount: dialog.entries.filter(e => e.ok === true).length
    readonly property bool allReady: dialog.entries.length > 0 && dialog.readyCount === dialog.entries.length
    readonly property var presetChoices: [{ id: "", name: qsTr("Match each sequence's shape"), icon: "scaling" }]
                                         .concat(ExportController.presets)

    /// Queued: `count` exports (`held`: they wait for Start Queue).
    signal queued(int count, bool held)

    /// Opens on `sequenceKeys`; `queue`: Send to Export Queue (waits for Start Queue by default).
    function openFor(sequenceKeys: var, queue: bool) {
        dialog.keys = sequenceKeys.slice()
        dialog.presetId = ""
        dialog.hold = queue
        dialog.folder = ExportController.lastFolderUrl
        dialog.errorText = ""
        dialog.refresh()
        open()
        presetButton.forceActiveFocus()
    }
    function options(): var {
        const o = { folder: dialog.folder.toString(), hold: dialog.hold }
        if (dialog.presetId === "")
            o.matchShape = true
        else
            o.preset = dialog.presetId
        return o
    }
    function refresh() {
        dialog.entries = ExportController.sequenceExports(dialog.keys, dialog.options())
    }
    function presetName(id: string): string {
        const found = dialog.presetChoices.find(p => p.id === id)
        return found ? found.name : id
    }
    function queueNow() {
        const result = ExportController.queueSequences(dialog.keys, dialog.options())
        if (result.ok !== true) {
            dialog.errorText = result.error ?? ExportController.lastError
            dialog.refresh()
            return
        }
        const count = result.count
        const held = dialog.hold
        dialog.close()
        dialog.queued(count, held)
    }

    title: dialog.keys.length === 1 ? qsTr("Export Sequence") : qsTr("Export %n Sequence(s)", "", dialog.keys.length)
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))

    component RowLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(3)

        RowLabel { text: qsTr("Preset") }
        PillButton {
            id: presetButton
            objectName: "sequencesExportPreset"
            Layout.alignment: Qt.AlignLeft
            text: dialog.presetName(dialog.presetId)
            iconName: "chevron-down"
            Accessible.description: qsTr("One preset for every sequence, or each sequence's own shape")
            onClicked: presetMenu.popup(presetButton, 0, presetButton.height + Theme.space(1))

            ExportPresetMenu {
                id: presetMenu
                objectName: "sequencesExportPresetMenu"
                presets: dialog.presetChoices
                current: dialog.presetId
                onChosen: id => {
                    dialog.presetId = id
                    dialog.errorText = ""
                    dialog.refresh()
                }
            }
        }
        RowLabel { text: qsTr("Folder") }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                objectName: "sequencesExportFolder"
                Layout.fillWidth: true
                text: dialog.folder.toString() === "" ? qsTr("Next to the project")
                                                      : decodeURIComponent(dialog.folder.toString().replace(/^file:\/\/\/?/, ""))
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                objectName: "sequencesExportBrowse"
                variant: "ghost"
                compact: true
                text: qsTr("Browse…")
                tooltip: qsTr("Choose the folder the files go to")
                onClicked: folderDialog.open()
            }
        }
    }
    Toggle {
        objectName: "sequencesExportHold"
        text: qsTr("Wait for Start Queue")
        tooltip: qsTr("Queue the exports without starting them (Media Encoder's Ready)")
        checked: dialog.hold
        onToggled: dialog.hold = checked
    }

    // What each sequence becomes.
    ListView {
        id: list
        objectName: "sequencesExportList"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(64))
        clip: true
        interactive: contentHeight > height
        spacing: Theme.space(1)
        model: dialog.entries
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Sequences to export")
        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            objectName: "sequencesExportRow_" + index
            width: ListView.view.width
            implicitHeight: rowColumn.implicitHeight + Theme.space(2)
            radius: Theme.radiusBadge
            color: Theme.panel
            Accessible.role: Accessible.ListItem
            Accessible.name: row.modelData.ok === true
                             ? qsTr("%1: %2, %3").arg(row.modelData.name ?? "").arg(row.modelData.presetName ?? "")
                                   .arg(String(row.modelData.output ?? "").replace(/^.*[\\/]/, ""))
                             : qsTr("%1: %2").arg(row.modelData.name ?? row.modelData.key).arg(row.modelData.error ?? "")
            ColumnLayout {
                id: rowColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(2)
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Icon {
                        name: row.modelData.portrait === true ? "move-vertical" : "move-horizontal"
                        size: Theme.iconSm
                        color: Theme.textSecondary
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.modelData.name ?? row.modelData.key
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                    Text {
                        text: row.modelData.presetName ?? ""
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
                Text {
                    objectName: "sequencesExportRowDetail"
                    Layout.fillWidth: true
                    text: row.modelData.ok === true ? String(row.modelData.output ?? "").replace(/^.*[\\/]/, "")
                                                    : (row.modelData.error ?? "")
                    elide: Text.ElideMiddle
                    wrapMode: row.modelData.ok === true ? Text.NoWrap : Text.Wrap
                    color: row.modelData.ok === true ? Theme.textTertiary : Theme.danger
                    font.family: row.modelData.ok === true ? Theme.fontMono : Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
        }
    }
    Text {
        objectName: "sequencesExportError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            objectName: "sequencesExportCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "sequencesExportOk"
            variant: "primary"
            text: dialog.hold ? qsTr("Queue %n export(s)", "", dialog.entries.length)
                              : qsTr("Export %n sequence(s)", "", dialog.entries.length)
            enabled: dialog.allReady
            disabledReason: dialog.entries.length === 0 ? qsTr("Choose sequences to export")
                                                        : qsTr("A sequence cannot export: see the list")
            onClicked: dialog.queueNow()
        }
    ]

    FolderDialog {
        id: folderDialog
        currentFolder: dialog.folder
        onAccepted: {
            dialog.folder = selectedFolder
            dialog.refresh()
        }
    }
}
