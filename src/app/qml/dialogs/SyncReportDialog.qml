pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Synchronize (audio) report (M5l, bridge `ClipAudio`; A.5): Premiere's Synchronize by audio for dual-system sound.
// The reference (the first clip with a picture) stays; the other clips moved by their offset and the external audio is
// linked to it. Each row: the offset, how sure the match is, and clock drift when the devices disagree (a speed is
// proposed, not applied). Undo takes the whole sync back.
WabcutDialog {
    id: dialog
    objectName: "syncReportDialog"

    readonly property var report: ClipAudio.syncReport
    readonly property var rows: report.rows ?? []

    title: qsTr("Synchronized by audio")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    component Caption: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Text {
        Layout.fillWidth: true
        visible: dialog.report.ok === false
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        text: dialog.report.error ?? ""
    }
    GridLayout {
        objectName: "syncReportRows"
        Layout.fillWidth: true
        visible: dialog.rows.length > 0
        columns: 4
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(1)
        Caption { text: qsTr("Clip") }
        Caption { text: qsTr("Moved by") }
        Caption { text: qsTr("Confidence") }
        Caption { text: qsTr("Note") }
        Repeater {
            model: dialog.rows.length * 4
            delegate: Text {
                required property int index
                readonly property var row: dialog.rows[Math.floor(index / 4)] ?? ({})
                readonly property int column: index % 4
                Layout.fillWidth: column === 3
                elide: Text.ElideRight
                color: column === 3 && !row.synced ? Theme.warning : column === 0 ? Theme.textPrimary : Theme.textSecondary
                font.family: column === 1 || column === 2 ? Theme.fontMono : Theme.fontSans
                font.pixelSize: Theme.textSm
                text: column === 0 ? (row.name ?? "")
                      : column === 1 ? (row.reference ? qsTr("reference") : (row.offset ?? ""))
                      : column === 2 ? (row.confidence ?? "")
                      : !row.synced ? (row.problem ?? "") : (row.drift ?? "") !== "" ? qsTr("drift %1").arg(row.drift) : ""
            }
        }
    }
    Repeater {
        model: dialog.report.warnings ?? []
        delegate: Text {
            required property var modelData
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            text: modelData
        }
    }

    footerData: [
        PillButton {
            variant: "primary"
            text: qsTr("Done")
            onClicked: dialog.close()
        }
    ]
}
