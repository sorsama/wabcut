pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The pre-export check (0.7): one list of what is likely to go wrong with the export about to start, each row with its
// place on the timeline (Go to) and a fix where Wabcut has one. Offline media, gaps and flash frames, mixed frame
// rates, stabilizers that were never analyzed, slow effects, a size that does not suit the sequence, captions, and -
// once measured here - loudness and clipped audio (ExportController.preExportCheck, exporter::preExportCheck).
//
// Opened by the Export panel's Check button, or by the Export button itself while "Check before every export" is on and
// the check finds a problem or a warning (`exportMode`: the footer then offers Export anyway, unless a problem stops it).
// Fixes that only touch the timeline or the project run here (Relink, Close gap); the ones that change this export's
// settings are reported with fixRequested(fix, row) for the Export panel, which owns them.
WabcutDialog {
    id: dialog
    objectName: "preExportCheckDialog"

    title: qsTr("Check before export")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    /// What is checked: the Export panel's sequence, preset, overrides and destination (`platform`: { name, lufs }).
    property string sequenceId: ""
    property string presetId: "match-source"
    property var overrides: ({})
    property var platform: ({})
    /// Opened by the Export button: Export anyway starts the export.
    property bool exportMode: false
    /// ExportController.preExportCheck's rows.
    property var rows: []
    property string fixError: ""

    readonly property int problemCount: rows.filter(r => r.severity === "problem").length
    readonly property int warningCount: rows.filter(r => r.severity === "warning").length
    readonly property int noteCount: rows.filter(r => r.severity === "note").length
    readonly property var measured: ExportController.measuredLoudness

    /// Export anyway was clicked.
    signal exportRequested()
    /// A fix that changes this export's settings ("turnOnLoudness", "matchSequenceSize", "turnOnCaptions").
    signal fixRequested(string fix, var row)

    function openFor(sequence: string, preset: string, exportOverrides: var, destination: var, forExport: bool) {
        dialog.sequenceId = sequence
        dialog.presetId = preset
        dialog.overrides = exportOverrides
        dialog.platform = destination
        dialog.exportMode = forExport
        dialog.fixError = ""
        dialog.refresh()
        dialog.open()
    }
    function refresh() {
        dialog.rows = ExportController.preExportCheck(dialog.sequenceId, dialog.presetId, dialog.overrides, dialog.platform)
    }
    function severityColor(severity: string): color {
        return severity === "problem" ? Theme.danger : severity === "warning" ? Theme.warning : Theme.textTertiary
    }
    function applyFix(row: var) {
        dialog.fixError = ""
        if (row.fix === "relink") {
            dialog.close()
            ExportController.relinkRequested()
            return
        }
        if (row.fix === "closeGap") {
            ProjectController.selectClip(row.clip)
            dialog.fixError = Editing.closeGap()
            dialog.refresh()
            return
        }
        dialog.fixRequested(row.fix, row)
        dialog.refresh()
    }

    // The mix is measured on request: it renders the whole range's sound.
    Connections {
        target: ExportController
        function onMeasuringLoudnessChanged() {
            if (!ExportController.measuringLoudness && dialog.visible)
                dialog.refresh()
        }
    }

    // ---- Summary ----
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Icon {
            Layout.alignment: Qt.AlignVCenter
            name: dialog.rows.length === 0 ? "shield-check" : "triangle-alert"
            size: Theme.iconMd
            color: dialog.rows.length === 0 ? Theme.success : dialog.problemCount > 0 ? Theme.danger : Theme.warning
        }
        Text {
            objectName: "preExportCheckSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            font.weight: Font.Medium
            text: {
                if (dialog.rows.length === 0)
                    return qsTr("Nothing found: this export looks ready.")
                const parts = []
                if (dialog.problemCount > 0)
                    parts.push(qsTr("%n problem(s)", "", dialog.problemCount))
                if (dialog.warningCount > 0)
                    parts.push(qsTr("%n warning(s)", "", dialog.warningCount))
                if (dialog.noteCount > 0)
                    parts.push(qsTr("%n note(s)", "", dialog.noteCount))
                return parts.join(" · ")
            }
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    // ---- The rows ----
    ListView {
        id: list
        objectName: "preExportCheckList"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(80))
        visible: count > 0
        clip: true
        spacing: Theme.space(1.5)
        boundsBehavior: Flickable.StopAtBounds
        model: dialog.rows
        ScrollBar.vertical: ThinScrollBar {}
        Accessible.role: Accessible.List
        Accessible.name: qsTr("What the check found")

        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            objectName: "preExportCheckRow_" + row.index
            width: list.width - (list.ScrollBar.vertical && list.ScrollBar.vertical.visible ? Theme.space(2) : 0)
            implicitHeight: rowLayout.implicitHeight + Theme.space(3)
            height: implicitHeight
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border
            Accessible.role: Accessible.ListItem
            Accessible.name: qsTr("%1: %2").arg(row.modelData.severity).arg(row.modelData.text)

            RowLayout {
                id: rowLayout
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(2)

                Rectangle {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: Theme.space(1.5)
                    Layout.preferredWidth: Theme.dotSize
                    Layout.preferredHeight: Theme.dotSize
                    radius: Theme.dotSize / 2
                    color: dialog.severityColor(row.modelData.severity)
                    Accessible.ignored: true
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        objectName: "preExportCheckText_" + row.index
                        Layout.fillWidth: true
                        text: row.modelData.text
                        wrapMode: Text.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: row.modelData.detail !== ""
                        text: row.modelData.detail
                        wrapMode: Text.Wrap
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
                PillButton {
                    objectName: "preExportCheckGoTo_" + row.index
                    Layout.alignment: Qt.AlignVCenter
                    compact: true
                    variant: "ghost"
                    visible: row.modelData.hasPlace
                    text: row.modelData.timecode
                    iconName: "go-to-in"
                    tooltip: qsTr("Go to this place in the timeline")
                    onClicked: {
                        ExportController.goToCheckRow(row.modelData)
                        dialog.close()
                    }
                }
                PillButton {
                    objectName: "preExportCheckFix_" + row.index
                    Layout.alignment: Qt.AlignVCenter
                    compact: true
                    variant: "secondary"
                    visible: row.modelData.fix !== "none"
                    text: row.modelData.fixText
                    onClicked: dialog.applyFix(row.modelData)
                }
            }
        }
    }

    Text {
        Layout.fillWidth: true
        visible: dialog.fixError !== ""
        text: dialog.fixError
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    // ---- Loudness: measured on request (it renders the range's sound once) ----
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        PillButton {
            objectName: "preExportMeasureLoudness"
            compact: true
            variant: "secondary"
            iconName: "gauge"
            text: ExportController.measuringLoudness ? qsTr("Measuring…") : qsTr("Measure loudness")
            enabled: !ExportController.measuringLoudness
            tooltip: qsTr("Plays the exported range's sound once, without a sound card, to find its loudness and peak")
            onClicked: ExportController.measureLoudness(dialog.sequenceId, dialog.presetId, dialog.overrides)
        }
        PillButton {
            objectName: "preExportCancelMeasure"
            compact: true
            variant: "ghost"
            visible: ExportController.measuringLoudness
            text: qsTr("Cancel")
            onClicked: ExportController.cancelLoudnessMeasure()
        }
        Text {
            objectName: "preExportLoudnessResult"
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: dialog.measured.ok === false ? Theme.danger : Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
            text: ExportController.measuringLoudness ? qsTr("Working out the loudness…")
                  : dialog.measured.ok === true
                    ? qsTr("%1 LUFS · peak %2 dBTP").arg(isFinite(dialog.measured.integratedLufs) ? dialog.measured.integratedLufs.toFixed(1) : "-∞")
                                                   .arg(isFinite(dialog.measured.truePeakDbtp) ? dialog.measured.truePeakDbtp.toFixed(1) : "-∞")
                  : dialog.measured.cancelled === true ? qsTr("Not measured")
                  : (dialog.measured.error ?? "")
        }
    }

    Toggle {
        objectName: "preExportCheckBefore"
        text: qsTr("Check before every export")
        checked: ExportController.checkBeforeExport
        onToggled: ExportController.checkBeforeExport = checked
    }

    footerData: [
        PillButton {
            objectName: "preExportCheckAgain"
            variant: "ghost"
            text: qsTr("Check again")
            onClicked: dialog.refresh()
        },
        PillButton {
            objectName: "preExportCheckClose"
            variant: "ghost"
            text: qsTr("Close")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "preExportCheckExport"
            variant: "primary"
            visible: dialog.exportMode
            enabled: dialog.problemCount === 0
            text: qsTr("Export anyway")
            iconName: "download"
            tooltip: dialog.problemCount > 0 ? qsTr("Fix the problems first: the export would fail") : ""
            onClicked: {
                dialog.close()
                dialog.exportRequested()
            }
        }
    ]
}
