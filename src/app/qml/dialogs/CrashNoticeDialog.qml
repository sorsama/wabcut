import QtQuick
import QtQuick.Layouts

// "Wabcut closed unexpectedly" (I.7, docs/release.md "Crash reports"): shown once at the next start after a crash or an
// unclean exit (CrashNotice.pending). It points at the Recovery dashboard (the autosaves and backups of the work in
// progress) and at the folder with the crash reports, and copies a short diagnostic summary - version, system, GPU and
// driver, graphics and decode backends, shown here in full - for a bug report. Nothing is sent anywhere.
WabcutDialog {
    id: dialog
    objectName: "crashNoticeDialog"

    /// Main.qml opens the Recovery dashboard.
    signal recoveryRequested()

    title: qsTr("Wabcut closed unexpectedly")

    function show() {
        if (CrashNotice.pending)
            open()
    }

    Text {
        Layout.fillWidth: true
        text: CrashNotice.detail
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("Your work is autosaved: the Recovery dashboard lists every saved, autosaved and backup version of your projects.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    // The diagnostic summary, exactly as it is copied: no paths, no project, no log.
    Rectangle {
        objectName: "crashSummary"
        Layout.fillWidth: true
        implicitHeight: summaryText.implicitHeight + Theme.space(6)
        radius: Theme.radiusCard
        color: Theme.section

        Text {
            id: summaryText
            anchors.fill: parent
            anchors.margins: Theme.space(3)

            text: CrashNotice.summary.trim()
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
            wrapMode: Text.Wrap
        }
    }
    Text {
        Layout.fillWidth: true
        text: CrashNotice.hasDump
              ? qsTr("A crash report was saved on this computer. Nothing was sent: share it only if you want to.")
              : qsTr("No crash report could be written for this run. Nothing was sent.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            objectName: "crashCopySummary"
            text: copied.running ? qsTr("Copied") : qsTr("Copy diagnostic summary")
            onClicked: {
                CrashNotice.copyDiagnosticSummary()
                copied.restart()
            }
            Timer {
                id: copied
                interval: 1500
            }
        },
        PillButton {
            objectName: "crashShowReports"
            text: qsTr("Show crash reports")
            onClicked: CrashNotice.openFolder()
        },
        PillButton {
            objectName: "crashOpenRecovery"
            text: qsTr("Open Recovery")
            onClicked: {
                CrashNotice.dismiss()
                dialog.close()
                dialog.recoveryRequested()
            }
        },
        PillButton {
            objectName: "crashDismiss"
            text: qsTr("Close")
            variant: "primary"
            onClicked: {
                CrashNotice.dismiss()
                dialog.close()
            }
        }
    ]
    onClosed: CrashNotice.dismiss()
}
