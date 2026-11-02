pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// P1 round (#26): export each ranged marker, or each selected clip, as its own file (Resolve's "Individual clips"):
// highlights for Discord, client selects. Opened from the Markers panel's export menu (Export each marker range…) and
// from the Export panel's range row (Each marker / Each selected clip), through ExportController.rangesExportRequested.
// The preset (Quick Export's last one), an optional marker colour, the folder, "Wait for Start Queue", and before
// anything is queued "N files · ≈ size" (GamingExport's estimate summed over the ranges) with the first file names.
// Each file is "<sequence> - <marker or clip name>.<ext>", numbered against the disk and the queue. A range that cannot
// export refuses the batch: nothing is queued and the reason shows.
WabcutDialog {
    id: dialog
    objectName: "exportRangesDialog"

    /// "markers" (the sequence's markers with a duration) or "clips" (the selected clips, linked video and audio as one).
    property string mode: "markers"
    property string presetId: "match-source"
    /// Markers mode: only the markers of this colour (-1 = every colour).
    property int colorIndex: -1
    property url folder
    property bool hold: false
    property string errorText: ""
    /// [{ in, out, name, colorIndex, ... }] (ExportController.markerRanges / selectedClipRanges).
    property var allRanges: []
    readonly property var ranges: dialog.mode === "markers" && dialog.colorIndex >= 0
                                  ? dialog.allRanges.filter(r => r.colorIndex === dialog.colorIndex) : dialog.allRanges
    /// The colours the ranged markers use (the colour filter's swatches).
    readonly property var colors: {
        const seen = []
        for (const r of dialog.allRanges) {
            if (r.colorIndex !== undefined && seen.indexOf(r.colorIndex) < 0)
                seen.push(r.colorIndex)
        }
        return seen.sort((a, b) => a - b)
    }
    /// ExportController.describeRanges: { ok, error, count, outputs, sizeText, durationText, lowSpaceNote }.
    readonly property var plan: dialog.visible && dialog.ranges.length > 0
                                ? ExportController.describeRanges("", dialog.presetId, dialog.ranges, dialog.overrides())
                                : ({ ok: false, outputs: [] })
    readonly property var colorNames: [qsTr("Green"), qsTr("Red"), qsTr("Purple"), qsTr("Orange"), qsTr("Yellow"),
                                       qsTr("White"), qsTr("Blue"), qsTr("Cyan")]

    signal queued(int count, bool held)

    /// `preferredPreset`: the Export panel's preset when it asks ("" = Quick Export's last preset).
    function openFor(rangeMode: string, preferredPreset: string) {
        dialog.mode = rangeMode === "clips" ? "clips" : "markers"
        dialog.allRanges = dialog.mode === "clips" ? ExportController.selectedClipRanges() : ExportController.markerRanges("", -1)
        dialog.colorIndex = -1
        dialog.presetId = preferredPreset !== "" ? preferredPreset
                                                 : (ExportController.rememberedFor("").quickPreset || "match-source")
        dialog.folder = ExportController.lastFolderUrl
        dialog.hold = false
        dialog.errorText = ""
        open()
        presetButton.forceActiveFocus()
    }
    function overrides(): var {
        return dialog.folder.toString() === "" ? {} : { folder: dialog.folder.toString() }
    }
    function presetName(id: string): string {
        const found = ExportController.presets.find(p => p.id === id)
        return found ? found.name : id
    }
    function queueNow() {
        const jobs = ExportController.queueRanges("", dialog.presetId, dialog.ranges, dialog.overrides(), dialog.hold)
        if (jobs.length === 0) {
            dialog.errorText = ExportController.lastError
            return
        }
        const held = dialog.hold
        dialog.close()
        dialog.queued(jobs.length, held)
    }

    title: dialog.mode === "clips" ? qsTr("Export Each Selected Clip") : qsTr("Export Each Marker Range")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(160), Theme.space(160))

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
            objectName: "exportRangesPreset"
            Layout.alignment: Qt.AlignLeft
            text: dialog.presetName(dialog.presetId)
            iconName: "chevron-down"
            onClicked: presetMenu.popup(presetButton, 0, presetButton.height + Theme.space(1))

            ExportPresetMenu {
                id: presetMenu
                objectName: "exportRangesPresetMenu"
                presets: ExportController.presets
                current: dialog.presetId
                onChosen: id => {
                    dialog.presetId = id
                    dialog.errorText = ""
                }
            }
        }
        RowLabel {
            visible: dialog.mode === "markers" && dialog.colors.length > 1
            text: qsTr("Colour")
        }
        Row {
            objectName: "exportRangesColors"
            visible: dialog.mode === "markers" && dialog.colors.length > 1
            spacing: Theme.space(1)
            PillButton {
                objectName: "exportRangesColorAll"
                compact: true
                variant: dialog.colorIndex < 0 ? "primary" : "ghost"
                text: qsTr("All")
                tooltip: qsTr("Every marker with a duration")
                onClicked: dialog.colorIndex = -1
            }
            Repeater {
                model: dialog.colors
                delegate: Swatch {
                    required property int modelData
                    objectName: "exportRangesColor_" + modelData
                    swatchColor: Theme.markerColor(modelData)
                    tooltip: qsTr("Only %1 markers").arg(dialog.colorNames[modelData] ?? "")
                    checked: dialog.colorIndex === modelData
                    onClicked: dialog.colorIndex = dialog.colorIndex === modelData ? -1 : modelData
                }
            }
        }
        RowLabel { text: qsTr("Folder") }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                objectName: "exportRangesFolder"
                Layout.fillWidth: true
                text: dialog.plan.folder ? dialog.plan.folder
                                         : decodeURIComponent(dialog.folder.toString().replace(/^file:\/\/\/?/, ""))
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                objectName: "exportRangesBrowse"
                variant: "ghost"
                compact: true
                text: qsTr("Browse…")
                tooltip: qsTr("Choose the folder the files go to")
                onClicked: folderDialog.open()
            }
        }
    }
    Toggle {
        objectName: "exportRangesHold"
        text: qsTr("Wait for Start Queue")
        tooltip: qsTr("Queue the exports without starting them")
        checked: dialog.hold
        onToggled: dialog.hold = checked
    }
    // "12 files · ≈ 240 MB · 3:20", then the first names.
    Text {
        objectName: "exportRangesSummary"
        Layout.fillWidth: true
        text: dialog.ranges.length === 0
              ? (dialog.mode === "clips" ? qsTr("Select clips in the timeline first.")
                                         : qsTr("No marker has a duration: drag a marker's end, or set one in Edit Marker."))
              : dialog.plan.ok === true
                ? qsTr("%n file(s) · ≈ %1 · %2", "", dialog.ranges.length).arg(dialog.plan.sizeText).arg(dialog.plan.durationText)
                : qsTr("%n file(s)", "", dialog.ranges.length)
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "exportRangesNames"
        Layout.fillWidth: true
        visible: (dialog.plan.outputs ?? []).length > 0
        readonly property var names: dialog.plan.outputs ?? []
        text: names.slice(0, 4).join("\n") + (names.length > 4 ? "\n" + qsTr("and %n more", "", names.length - 4) : "")
        elide: Text.ElideMiddle
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }
    Text {
        objectName: "exportRangesWarning"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.plan.lowSpaceNote ?? ""
        color: Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "exportRangesError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText !== "" ? dialog.errorText
                                      : dialog.ranges.length > 0 && dialog.plan.ok !== true ? (dialog.plan.error ?? "") : ""
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            objectName: "exportRangesCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "exportRangesOk"
            variant: "primary"
            text: dialog.hold ? qsTr("Queue %n file(s)", "", dialog.ranges.length)
                              : qsTr("Export %n file(s)", "", dialog.ranges.length)
            enabled: dialog.ranges.length > 0 && dialog.plan.ok === true
            disabledReason: dialog.ranges.length === 0 ? qsTr("Nothing to export") : (dialog.plan.error ?? "")
            onClicked: dialog.queueNow()
        }
    ]

    FolderDialog {
        id: folderDialog
        currentFolder: dialog.folder
        onAccepted: dialog.folder = selectedFolder
    }
}
