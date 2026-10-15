pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Crash recovery: work an unclean exit left in autosaves (ProjectFile.recoveryFiles). Recover opens it with
// unsaved changes; Discard deletes it; Later keeps it for the next start.
//
// I.4, the recovery dashboard (File > Recover…, the "All versions" tab): every version of the project - as saved
// now, its autosaves and its backups (.bak1-5) - and untitled autosaves (ProjectFile.recoveryVersions), each with a
// poster frame, its sequences, clips and duration, and what differs from the saved version. "Open as copy" opens a
// version untitled, so the project file is only replaced when the user saves over it.
WabcutDialog {
    id: dialog
    objectName: "recoveryDialog"

    /// The "All versions" tab is shown.
    property bool dashboard: false

    function openUnsaved() {
        dashboard = false
        open()
    }
    function openDashboard() {
        dashboard = true
        ProjectFile.scanRecoveryVersions()
        open()
    }

    title: dashboard ? qsTr("Recover a version") : qsTr("Recover unsaved work?")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), dashboard ? Theme.space(230) : Theme.space(150))

    PillTabs {
        id: modeTabs
        objectName: "recoveryTabs"
        Layout.fillWidth: true
        compact: true
        model: [qsTr("Unsaved work"), qsTr("All versions")]
        currentIndex: dialog.dashboard ? 1 : 0
        onActivated: index => {
            dialog.dashboard = index === 1
            if (dialog.dashboard)
                ProjectFile.scanRecoveryVersions()
            currentIndex = Qt.binding(() => dialog.dashboard ? 1 : 0)
        }
    }

    Text {
        Layout.fillWidth: true
        visible: !dialog.dashboard
        text: ProjectFile.recoveryFiles.length > 0
              ? qsTr("Wabcut did not close normally. These autosaves hold changes that were not saved.")
              : qsTr("Nothing unsaved was left behind.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    // ---- All versions ----
    Text {
        Layout.fillWidth: true
        visible: dialog.dashboard
        text: ProjectFile.recoveryScanning ? qsTr("Looking through the saved, autosaved and backup versions…")
              : ProjectFile.recoveryVersions.length === 0 ? qsTr("No other versions were found.")
              : qsTr("Every version Wabcut keeps of this project. Open one as a copy to compare it or take what you need; the project file is not changed.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }
    ListView {
        id: versions
        objectName: "recoveryVersionsList"
        visible: dialog.dashboard
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(120))
        clip: true
        spacing: Theme.space(1)
        model: ProjectFile.recoveryVersions
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        delegate: Rectangle {
            id: version
            required property var modelData
            width: ListView.view.width
            height: Theme.space(26)
            radius: Theme.radiusField
            color: version.modelData.kind === "current" ? Theme.raised : Theme.sunken
            border.width: version.modelData.kind === "current" ? Theme.hairline : 0
            border.color: Theme.accentStroke

            Rectangle {
                id: poster
                anchors.left: parent.left
                anchors.leftMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.space(40)
                height: Theme.space(22.5)
                radius: Theme.radiusBadge
                color: Theme.videoBlack
                clip: true
                Image {
                    objectName: "recoveryPoster"
                    anchors.fill: parent
                    source: version.modelData.poster
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                }
                Text {
                    anchors.centerIn: parent
                    visible: version.modelData.poster === ""
                    text: version.modelData.loadable ? "…" : "!"
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
            }
            Column {
                anchors.left: poster.right
                anchors.right: versionButtons.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(0.5)

                Text {
                    width: parent.width
                    //: Recovery version title and time, e.g. "Backup 2 · 11/10/26 14:05"
                    text: qsTr("%1 · %2").arg(version.modelData.title).arg(version.modelData.when)
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: version.modelData.loadable
                          ? qsTr("%1 · %n sequence(s)", "", version.modelData.sequences).arg(version.modelData.sequence)
                            + " · " + qsTr("%n clip(s)", "", version.modelData.clips) + " · " + version.modelData.duration
                          : qsTr("Cannot be opened: %1").arg(version.modelData.error)
                    color: version.modelData.loadable ? Theme.textSecondary : Theme.warning
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: text !== ""
                    text: version.modelData.kind === "current" ? qsTr("The project as saved now") : version.modelData.comparison
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: version.modelData.file
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                    elide: Text.ElideMiddle
                }
            }
            Row {
                id: versionButtons
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space(1)

                PillButton {
                    compact: true
                    variant: version.modelData.kind === "current" ? "secondary" : "primary"
                    enabled: version.modelData.loadable
                    text: qsTr("Open as copy")
                    onClicked: {
                        if (ProjectFile.openRecoveryCopy(version.modelData.url))
                            dialog.close()
                    }
                }
            }
        }
    }

    // ---- Unsaved work ----
    ListView {
        visible: !dialog.dashboard
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(70))
        clip: true
        spacing: Theme.space(1)
        model: ProjectFile.recoveryFiles
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        delegate: Rectangle {
            id: row
            required property var modelData
            width: ListView.view.width
            height: Theme.controlHeight + Theme.space(4)
            radius: Theme.radiusField
            color: Theme.sunken

            Column {
                anchors.left: parent.left
                anchors.right: buttons.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.space(3)
                anchors.rightMargin: Theme.space(2)
                spacing: Theme.space(0.5)

                Text {
                    width: parent.width
                    text: row.modelData.name
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    //: When the autosave was written, e.g. "Autosaved 11/10/26 14:05"
                    text: qsTr("Autosaved %1").arg(row.modelData.when)
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    elide: Text.ElideRight
                }
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space(1)

                PillButton {
                    compact: true
                    text: qsTr("Discard")
                    onClicked: ProjectFile.discardRecovery(row.modelData.url)
                }
                PillButton {
                    compact: true
                    variant: "primary"
                    text: qsTr("Recover")
                    onClicked: {
                        if (ProjectFile.recover(row.modelData.url))
                            dialog.close()
                    }
                }
            }
        }
    }

    footerData: [
        PillButton {
            visible: !dialog.dashboard && ProjectFile.recoveryFiles.length > 0
            text: qsTr("Discard All")
            onClicked: {
                ProjectFile.discardAllRecovery()
                dialog.close()
            }
        },
        PillButton {
            text: dialog.dashboard ? qsTr("Close") : qsTr("Later")
            onClicked: dialog.close()
        }
    ]

    // The startup check opens it for unsaved work only; File > Recover… opens the dashboard.
    onAboutToShow: if (!dashboard && ProjectFile.recoveryFiles.length === 0) close()
}
