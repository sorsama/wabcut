pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// What a timeline import (Premiere/Resolve/FCP XML, FCPXML, OTIO, EDL) carried, approximated and dropped:
// ProjectFile.importReport's map (ProjectLifecycle::importReport). A Lottie import (M5o, GraphicsImport.imported) shows
// the same way, with its own `summary` line (canvas, length, frame rate, where the clip went) and each approximated or
// dropped feature's JSON path as its location. File > Export Timeline… (0.1.1) shows interchange.export's report here too.
// P1 round (#6): a timeline import that left media offline offers Relink… (relinkRequested: the window's Relink dialog);
// the summary is one line, the offline names at most three lines and the entries list scrolls, so the buttons stay
// in the window.
WabcutDialog {
    id: dialog
    objectName: "importReportDialog"

    property var report: ({})
    /// The import left media offline and some still is: Relink… shows.
    readonly property bool offerRelink: !(dialog.report.exported ?? false) && (dialog.report.unresolved ?? []).length > 0
                                        && ProjectFile.offlineCount > 0

    /// Relink… was chosen (the dialog has closed).
    signal relinkRequested()

    function show(newReport) {
        report = newReport
        open()
    }

    //: Title of the import report, e.g. "Imported cut.xml"; an export (File > Export Timeline…) says "Exported cut.xml"
    title: report.exported ? qsTr("Exported %1").arg(report.file ?? "") : qsTr("Imported %1").arg(report.file ?? "")

    Text {
        objectName: "importReportSummary"
        Layout.fillWidth: true
        //: e.g. "Final Cut Pro 7 XML · 2 sequences · 12 media files"
        text: dialog.report.summary !== undefined ? dialog.report.summary
              : qsTr("%1 · %2 · %3").arg(dialog.report.format ?? "")
                    .arg((dialog.report.sequences ?? 0) === 1 ? qsTr("1 sequence")
                                                             : qsTr("%1 sequences").arg(dialog.report.sequences ?? 0))
                    .arg((dialog.report.media ?? 0) === 1 ? qsTr("1 media file")
                                                          : qsTr("%1 media files").arg(dialog.report.media ?? 0))
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    RowLayout {
        spacing: Theme.space(2)

        StatusPill {
            label: qsTr("Carried")
            detail: String(dialog.report.carried ?? 0)
            dotColor: Theme.success
        }
        StatusPill {
            label: qsTr("Approximated")
            detail: String(dialog.report.approximated ?? 0)
            dotColor: Theme.warning
        }
        StatusPill {
            label: qsTr("Dropped")
            detail: String(dialog.report.dropped ?? 0)
            dotColor: Theme.danger
        }
    }

    Text {
        objectName: "importReportOffline"
        Layout.fillWidth: true
        visible: (dialog.report.unresolved ?? []).length > 0
        text: qsTr("Offline (not found): %1").arg((dialog.report.unresolved ?? []).map(m => m.name).join(", "))
        color: Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
        maximumLineCount: 3
        elide: Text.ElideRight
    }

    ListView {
        id: entries
        objectName: "importReportEntries"
        Layout.fillWidth: true
        // Never taller than the window leaves room for (the title, summary and buttons stay on screen).
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(80),
                                         Math.max(Theme.space(20), (dialog.parent ? dialog.parent.height : 0) - Theme.space(90)))
        visible: count > 0
        clip: true
        spacing: Theme.space(1)
        model: dialog.report.entries ?? []
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        delegate: Rectangle {
            id: entry
            required property var modelData
            width: ListView.view.width
            height: column.implicitHeight + Theme.space(2)
            radius: Theme.radiusField
            color: Theme.sunken

            Column {
                id: column
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(0.5)

                Text {
                    width: parent.width
                    text: entry.modelData.reason
                    color: entry.modelData.outcome === "dropped" ? Theme.danger : Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    text: [entry.modelData.outcome, entry.modelData.category, entry.modelData.sequence,
                           entry.modelData.location].filter(s => s).join(" · ")
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                    elide: Text.ElideMiddle
                }
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "importReportRelink"
            visible: dialog.offerRelink
            text: qsTr("Relink…")
            iconName: "link"
            variant: "primary"
            onClicked: {
                dialog.close()
                dialog.relinkRequested()
            }
        },
        PillButton {
            objectName: "importReportClose"
            text: qsTr("Close")
            variant: dialog.offerRelink ? "ghost" : "primary"
            onClicked: dialog.close()
        }
    ]
}
