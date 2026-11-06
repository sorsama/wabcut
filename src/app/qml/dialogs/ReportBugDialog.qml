pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Report a Bug… (0.7, card 016; Help > Report a Bug…): the system information a bug report needs, shown in full and
// read-only exactly as it is copied (CrashNotice.systemInfo: the version, OS, graphics card and driver, the graphics
// API, the video decoders, Qt, the interface size and the screens; redacted, no paths, no user name, no project).
//   Copy System Info   puts the text on the clipboard.
//   Open on GitHub     opens the new-issue form in the browser with the version and the system information filled in
//                      (CrashNotice.bugReportUrl, the bug report form of .github/ISSUE_TEMPLATE); the app sends nothing.
//   Show Crash Reports the folder with the reports and dumps of earlier crashes (CrashNotice.openFolder).
WabcutDialog {
    id: dialog
    objectName: "reportBugDialog"

    /// The graphics API and adapter this window renders with ("D3D11 on NVIDIA GeForce RTX 3060"); Main.qml binds it to
    /// its RendererInfo.
    property string renderer: ""
    /// The text shown and copied (read when the dialog opens).
    property string info: ""
    /// The address "Open on GitHub" opens.
    readonly property string issueUrl: CrashNotice.bugReportUrl(dialog.info)

    function openDialog() {
        dialog.info = CrashNotice.systemInfo(dialog.renderer)
        open()
        githubButton.forceActiveFocus()
    }

    title: qsTr("Report a Bug")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    Text {
        Layout.fillWidth: true
        text: qsTr("Describe what went wrong on GitHub. Wabcut sends nothing itself: Open on GitHub opens the bug report form in your browser with the details below filled in. Do not attach private media or API keys.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    // The system information, exactly as it is copied and put into the form.
    Rectangle {
        objectName: "reportBugInfoBox"
        Layout.fillWidth: true
        implicitHeight: infoText.implicitHeight + Theme.space(6)
        radius: Theme.radiusCard
        color: Theme.section

        TextEdit {
            id: infoText
            objectName: "reportBugInfo"
            anchors.fill: parent
            anchors.margins: Theme.space(3)
            readOnly: true
            selectByMouse: true
            text: dialog.info
            color: Theme.textPrimary
            selectionColor: Theme.accentFill
            selectedTextColor: Theme.accentOnFill
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
            wrapMode: TextEdit.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("System information")
            Accessible.description: text
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(2)

        PillButton {
            id: githubButton
            objectName: "reportBugOpenGitHub"
            variant: "primary"
            iconName: "external-link"
            text: qsTr("Open on GitHub")
            tooltip: qsTr("Opens the bug report form in your browser; Wabcut sends nothing")
            onClicked: Qt.openUrlExternally(dialog.issueUrl)
        }
        PillButton {
            objectName: "reportBugCopy"
            iconName: "copy"
            text: copied.running ? qsTr("Copied") : qsTr("Copy System Info")
            tooltip: qsTr("Puts the system information on the clipboard")
            onClicked: {
                CrashNotice.copyText(dialog.info)
                copied.restart()
            }
            Timer {
                id: copied
                interval: 1500
            }
        }
        PillButton {
            objectName: "reportBugCrashReports"
            iconName: "folder-open"
            text: qsTr("Show Crash Reports")
            tooltip: qsTr("Opens the folder with the reports of earlier crashes; nothing is sent")
            onClicked: CrashNotice.openFolder()
        }
    }

    footerData: [
        PillButton {
            objectName: "reportBugClose"
            variant: "ghost"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
