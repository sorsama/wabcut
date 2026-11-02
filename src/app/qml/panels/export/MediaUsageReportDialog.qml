pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// P1 round (#25): File > Export > Media Usage Report… - every source the sequence uses, for archive clearances, music
// licensing and credits (Avid's source lists; Premiere's EDL reports). The command sequence.mediaUsage makes the text
// (one row per use: media, path, tape, scene / shot / take, AI-generated, track, record and source In/Out, duration,
// speed; then the totals per source); ExportController.writeMediaUsage writes it. Range: the whole sequence or In to
// Out; "Report through nested sequences" (flatten: on by default, multicam clips by their active angle); CSV (opens in
// a spreadsheet) or JSON. The line under the choices says how many uses and sources the report has before it is saved.
WabcutDialog {
    id: dialog
    objectName: "mediaUsageReportDialog"

    property string range: "sequence"
    property bool flatten: true
    property string format: "csv"
    readonly property var options: ({ range: dialog.range, flatten: dialog.flatten, format: dialog.format })
    /// ExportController.mediaUsage: { ok, uses, media, error } for the line under the choices.
    readonly property var preview: dialog.visible ? ExportController.mediaUsage("", dialog.options) : ({ ok: false })
    property string errorText: ""

    /// Written: the report's file, with its number of uses and sources.
    signal written(string path, int uses, int media)

    function openDialog() {
        dialog.range = "sequence"
        dialog.errorText = ""
        open()
        rangeTabs.forceActiveFocus()
    }
    function save(file: url) {
        const result = ExportController.writeMediaUsage(file.toString(), "", dialog.options)
        if (result.ok !== true) {
            dialog.errorText = result.error ?? ""
            return
        }
        dialog.close()
        dialog.written(result.path, result.uses, result.media)
    }

    title: qsTr("Media Usage Report")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(140), Theme.space(140))

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

        RowLabel { text: qsTr("Range") }
        PillTabs {
            id: rangeTabs
            objectName: "mediaUsageRange"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Whole sequence"), qsTr("In to Out")]
            currentIndex: dialog.range === "inOut" ? 1 : 0
            Accessible.name: qsTr("Report range")
            onActivated: index => dialog.range = index === 1 ? "inOut" : "sequence"
        }
        RowLabel { text: qsTr("Format") }
        PillTabs {
            objectName: "mediaUsageFormat"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("CSV (spreadsheet)"), qsTr("JSON")]
            currentIndex: dialog.format === "json" ? 1 : 0
            Accessible.name: qsTr("Report format")
            onActivated: index => dialog.format = index === 1 ? "json" : "csv"
        }
    }
    Toggle {
        objectName: "mediaUsageFlatten"
        text: qsTr("Report through nested sequences and multicam clips")
        tooltip: qsTr("List the sources inside nested sequences (in this sequence's timecode), and a multicam clip's active angle")
        checked: dialog.flatten
        onToggled: dialog.flatten = checked
    }
    Text {
        objectName: "mediaUsageSummary"
        Layout.fillWidth: true
        text: dialog.preview.ok === true
              ? qsTr("%n use(s)", "", dialog.preview.uses ?? 0) + " · " + qsTr("%n source(s)", "", dialog.preview.media ?? 0)
              : (dialog.preview.error ?? "")
        color: dialog.preview.ok === true ? Theme.textPrimary : Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "mediaUsageError"
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
            objectName: "mediaUsageCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "mediaUsageSave"
            variant: "primary"
            text: qsTr("Save…")
            enabled: dialog.preview.ok === true
            disabledReason: dialog.preview.error ?? ""
            onClicked: saveDialog.open()
        }
    ]

    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        currentFolder: ExportController.lastFolderUrl
        defaultSuffix: dialog.format
        nameFilters: dialog.format === "json" ? [qsTr("JSON files (*.json)")] : [qsTr("CSV files (*.csv)")]
        onAccepted: dialog.save(selectedFile)
    }
}
