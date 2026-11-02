pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Creator autopilot (P1 #62, docs/creator-autopilot.md): three creator jobs for the current sequence in one sheet.
//   B-roll          matching B-roll from the library for what is said, on a free track (Autopilot.previewBroll)
//   Publishing kit  title ideas, description, chapters, tags and thumbnail candidates, written as files
//   Pacing          a reference video's cut rhythm against the sequence's, and cuts to match it
// Every job runs off the GUI thread with progress and Stop, previews first (the timeline shows the staged edit as a
// ghost) and applies as one undo step. Everything works on this computer; the two cloud steps (Generate for a line
// nothing matched, Write with my AI) show the cost and ask for consent first. Main.qml opens it (Autopilot.open()).
WabcutDialog {
    id: sheet
    objectName: "autopilotSheet"

    /// 0 = B-roll, 1 = Publishing kit, 2 = Pacing.
    property alias page: tabs.currentIndex
    /// A page's own message (an error a call returned); cleared when a job starts or ends.
    property string note: ""

    title: qsTr("Creator autopilot")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(240), Theme.space(240))
    onClosed: Autopilot.discard()

    /// Shows a page's own message (an error a call returned) under the pages.
    function say(text: string) {
        sheet.note = text
    }

    Connections {
        target: Autopilot
        function onResultChanged() {
            sheet.note = ""
        }
        function onJobChanged() {
            if (Autopilot.busy)
                sheet.note = ""
        }
    }

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)

        PillTabs {
            id: tabs
            objectName: "autopilotTabs"
            Accessible.name: qsTr("Autopilot job")
            Layout.fillWidth: true
            tabs: true
            model: [qsTr("B-roll"), qsTr("Publishing kit"), qsTr("Pacing")]
            onActivated: sheet.note = ""
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(150)
            currentIndex: tabs.currentIndex

            AutopilotBrollPage {
                onSaid: text => sheet.say(text)
                onGenerateLine: line => {
                    // Apply what is staged first, then hand the line to the Generate panel (cost and consent there).
                    if (Autopilot.stagedKind === "broll") {
                        const applied = Autopilot.applyBroll()
                        if (applied !== "") {
                            sheet.say(applied)
                            return
                        }
                    }
                    const error = Autopilot.generateFor(line)
                    if (error !== "")
                        sheet.say(error)
                    else
                        sheet.close()
                }
            }
            AutopilotKitPage {
                onSaid: text => sheet.say(text)
            }
            AutopilotPacingPage {
                onSaid: text => sheet.say(text)
            }
        }

        // ---- The running job ----
        RowLayout {
            Layout.fillWidth: true
            visible: Autopilot.busy
            spacing: Theme.space(2)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                StatusPill {
                    Layout.fillWidth: true
                    label: Autopilot.phase
                    detail: Autopilot.progress >= 0 ? Math.round(Autopilot.progress * 100) + "%" : ""
                    dotColor: Theme.accentFill
                }
                BeamProgress {
                    objectName: "autopilotProgress"
                    Layout.fillWidth: true
                    value: Autopilot.progress >= 0 ? Autopilot.progress : 1
                    active: Autopilot.busy
                    label: Autopilot.phase
                }
            }
            PillButton {
                objectName: "autopilotStop"
                compact: true
                iconName: "square"
                text: qsTr("Stop")
                Accessible.name: qsTr("Stop the autopilot job")
                onClicked: Autopilot.cancel()
            }
        }
        // Fix round 1 (ARGON-17): the timeline changed while the job ran or since the preview: one click makes it again.
        RowLayout {
            objectName: "autopilotOutdated"
            Layout.fillWidth: true
            visible: Autopilot.outdated
            spacing: Theme.space(2)
            Note {
                objectName: "autopilotOutdatedText"
                text: qsTr("The timeline changed since this preview, so it may not fit any more.")
                color: Theme.warning
            }
            PillButton {
                objectName: "autopilotRerun"
                compact: true
                iconName: "rotate-cw"
                text: qsTr("Run again")
                Accessible.name: qsTr("Run the autopilot job again on the timeline as it is now")
                onClicked: {
                    const error = Autopilot.rerun()
                    if (error !== "")
                        sheet.say(error)
                }
            }
        }
        Note {
            id: messageText
            objectName: "autopilotMessage"
            visible: text !== ""
            text: sheet.note !== "" ? sheet.note : Autopilot.message
            color: Theme.warning
            maximumLineCount: 4
            elide: Text.ElideRight
        }
    }

    footerData: [
        PillButton {
            objectName: "autopilotClose"
            text: qsTr("Close")
            onClicked: sheet.close()
        }
    ]
}
