pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Help > Check for Updates... (P0 round; Updates, docs/release.md "Updates"): checks the latest release's signed version
// manifest and says what it found. A newer Wabcut shows its notes; with the installer (Windows, installed with the MSI)
// it downloads and verifies it (signature, size, SHA-256, Authenticode), then installs after Wabcut closes - "Install
// When I Quit", or "Quit and Install", which closes the window the normal way (unsaved changes and running exports ask
// first; the button waits while background work runs). Elsewhere it opens the release page. Opened by Main
// (window.showUpdates) from the Help menu, the palette, Settings > General and the toast of an automatic check. Later
// (or Close) leaves it in every stage without scheduling anything. QtQuick.Controls is imported for the notes' scroll bar
// only (no menus in this file).
WabcutDialog {
    id: dialog
    objectName: "updateDialog"

    /// Main's window.backgroundWorkRunning: exports or other long work are running, so "Quit and Install" waits.
    property bool backgroundWorkRunning: false

    readonly property string stage: Updates.state === "ready" && Updates.installScheduled ? "scheduled" : Updates.state

    title: qsTr("Updates")

    component Body: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    Body {
        objectName: "updateStatus"
        color: dialog.stage === "failed" ? Theme.dangerText : Theme.textPrimary // fix round 1: error text reads on light panels
        text: {
            switch (dialog.stage) {
            case "checking":
                return qsTr("Checking for updates…")
            case "upToDate":
                //: %1 is this version, e.g. 0.4.0
                return qsTr("Wabcut %1 is the newest version.").arg(Updates.currentVersion)
            case "available":
                //: %1 the new version, %2 this one
                return qsTr("Wabcut %1 is available. You have %2.").arg(Updates.latestVersion).arg(Updates.currentVersion)
            case "downloading":
                return qsTr("Downloading Wabcut %1…").arg(Updates.latestVersion)
            case "ready":
                return qsTr("Wabcut %1 is downloaded. Its signature and checksum are verified.").arg(Updates.latestVersion)
            case "scheduled":
                return qsTr("Wabcut %1 installs when you quit Wabcut, then Wabcut opens again.").arg(Updates.latestVersion)
            case "failed":
                return Updates.error
            }
            return qsTr("Wabcut %1").arg(Updates.currentVersion)
        }
    }
    Note {
        objectName: "updateReleased"
        visible: Updates.released !== "" && (dialog.stage === "available" || dialog.stage === "ready")
        //: %1 is a date
        text: qsTr("Released %1").arg(Updates.released)
    }

    // What is new (the manifest's Markdown; only from a verified manifest).
    Rectangle {
        objectName: "updateNotesBox"
        Layout.fillWidth: true
        visible: Updates.notes !== "" && (dialog.stage === "available" || dialog.stage === "ready" || dialog.stage === "scheduled")
        implicitHeight: Math.min(notes.implicitHeight + Theme.space(4), Theme.space(64))
        radius: Theme.radiusCard
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border

        Flickable {
            id: notesFlick
            anchors.fill: parent
            anchors.margins: Theme.space(2)
            clip: true
            contentHeight: notes.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            Accessible.role: Accessible.Pane
            Accessible.name: qsTr("What's new")
            ScrollBar.vertical: ThinScrollBar {
                objectName: "updateNotesScrollBar"
            }

            Text {
                id: notes
                objectName: "updateNotes"
                width: notesFlick.width
                text: Updates.notes
                textFormat: Text.MarkdownText
                color: Theme.textPrimary
                linkColor: Theme.accentStroke
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                wrapMode: Text.Wrap
                onLinkActivated: link => Qt.openUrlExternally(link)
            }
        }
    }

    BeamProgress {
        objectName: "updateProgress"
        Layout.fillWidth: true
        visible: dialog.stage === "downloading" || dialog.stage === "checking"
        indeterminate: dialog.stage === "checking"
        value: Updates.progress
        active: true
        label: dialog.stage === "checking" ? qsTr("Checking for updates") : qsTr("Downloading the update")
        loader: dialog.stage === "checking" ? "scan" : ""
    }
    Note {
        objectName: "updateProgressText"
        visible: dialog.stage === "downloading" && Updates.progressText !== ""
        text: Updates.progressText
    }
    Note {
        objectName: "updateScheduledOlder"
        visible: dialog.stage === "available" && Updates.installScheduled
        //: %1 the downloaded version waiting for the quit, %2 the newer one just found
        text: qsTr("Wabcut %1 is downloaded and installs when you quit. Downloading %2 replaces it.")
              .arg(Updates.scheduledVersion).arg(Updates.latestVersion)
    }
    Note {
        objectName: "updateInstallHint"
        visible: dialog.stage === "available" && !Updates.canInstall && Updates.installHint !== ""
        text: Updates.installHint
    }
    Note {
        visible: dialog.stage === "ready" || dialog.stage === "scheduled"
        text: qsTr("Installing closes Wabcut, updates it and opens it again. Your projects, settings and caches stay as they are.")
    }
    Note {
        objectName: "updateBackgroundWork"
        visible: (dialog.stage === "ready" || dialog.stage === "scheduled") && dialog.backgroundWorkRunning
        color: Theme.warningText
        text: qsTr("Exports or other background work are running. Install when you quit, or wait until they finish.")
    }
    Note {
        objectName: "updateLastChecked"
        visible: (dialog.stage === "upToDate" || dialog.stage === "failed") && Updates.lastChecked !== ""
        //: %1 is a date and time
        text: qsTr("Last checked %1").arg(Updates.lastChecked)
    }

    footerData: [
        PillButton {
            objectName: "updateSkip"
            visible: dialog.stage === "available"
            variant: "ghost"
            text: qsTr("Skip This Version")
            tooltip: qsTr("Automatic checks stay quiet about this version")
            onClicked: {
                Updates.skipThisVersion()
                dialog.close()
            }
        },
        PillButton {
            objectName: "updateCancel"
            visible: dialog.stage === "checking" || dialog.stage === "downloading"
            text: qsTr("Cancel")
            onClicked: Updates.cancel()
        },
        PillButton {
            objectName: "updateDontInstall"
            visible: dialog.stage === "scheduled"
            text: qsTr("Don't Install")
            onClicked: Updates.cancelInstall()
        },
        PillButton {
            objectName: "updateRetry"
            visible: dialog.stage === "failed"
            text: qsTr("Try Again")
            onClicked: Updates.check()
        },
        PillButton {
            objectName: "updateClose"
            text: dialog.stage === "available" || dialog.stage === "ready" ? qsTr("Later") : qsTr("Close")
            tooltip: dialog.stage === "ready" ? qsTr("Keeps the download; nothing installs until you choose to") : ""
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "updateInstallOnQuit"
            visible: dialog.stage === "ready"
            text: qsTr("Install When I Quit")
            onClicked: {
                Updates.installWhenClosed()
                dialog.close()
            }
        },
        PillButton {
            objectName: "updateDownload"
            visible: dialog.stage === "available" && Updates.canInstall
            variant: "primary"
            iconName: "download"
            text: qsTr("Download and Install")
            onClicked: Updates.download()
        },
        PillButton {
            objectName: "updateOpenPage"
            visible: dialog.stage === "available" && !Updates.canInstall
            variant: "primary"
            text: qsTr("Open the Release Page")
            onClicked: {
                Updates.openReleasePage()
                dialog.close()
            }
        },
        PillButton {
            objectName: "updateQuitAndInstall"
            visible: dialog.stage === "ready" || dialog.stage === "scheduled"
            enabled: !dialog.backgroundWorkRunning
            variant: "primary"
            text: qsTr("Quit and Install")
            tooltip: dialog.backgroundWorkRunning ? qsTr("Waits until the exports and other background work finish")
                                                  : qsTr("Closes Wabcut, installs the update and opens Wabcut again")
            onClicked: {
                dialog.close()
                Updates.quitAndInstall()
            }
        }
    ]
}
