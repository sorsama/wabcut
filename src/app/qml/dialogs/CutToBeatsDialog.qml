pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Sequence > Beat Grid > Cut Selected Clips to Beats… (P1 round, D9 #8): edit.cutToBeats - the selected clips' media, one
// per group of N beats from the first beat at or after the playhead, overwriting what is there. One undo step ("Cut to
// Beats"). The options are shown once the menu has closed.
WabcutDialog {
    id: dialog
    objectName: "cutToBeatsDialog"

    /// What Cut to Beats will act on (Beats.cutInfo()), read when the dialog opens.
    property var info: ({})
    /// The beats each clip lasts: 1, 2, 4 (a bar) or 8.
    property int beatsPerCut: 4
    /// Why Cut could not start ("").
    property string problem: ""
    readonly property var groups: [1, 2, 4, 8]

    function openFor() {
        info = Beats.cutInfo()
        beatsPerCut = info.beatsPerCut ?? 4
        open()
    }
    function cut() {
        const why = Beats.cutSelection(dialog.beatsPerCut)
        if (why !== "") {
            dialog.problem = why
            return
        }
        dialog.close()
    }

    title: qsTr("Cut to Beats")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(110), Theme.space(110))

    Text {
        objectName: "cutToBeatsSummary"
        Layout.fillWidth: true
        text: qsTr("Uses the %1 selected clips in order, each from its In, from the first beat at or after the playhead. It overwrites what is on the tracks: Undo puts it back.").arg(dialog.info.clips ?? 0)
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("Beats per clip")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }
    PillTabs {
        objectName: "cutToBeatsGroups"
        Layout.fillWidth: true
        compact: true
        model: [{ label: qsTr("1 beat") }, { label: qsTr("2 beats") }, { label: qsTr("1 bar (4)") },
                { label: qsTr("2 bars (8)") }]
        currentIndex: Math.max(0, dialog.groups.indexOf(dialog.beatsPerCut))
        onActivated: index => dialog.beatsPerCut = dialog.groups[index]
        Accessible.name: qsTr("Beats per clip")
    }
    Text {
        Layout.fillWidth: true
        visible: (dialog.info.bpm ?? 0) > 0
        text: qsTr("The grid is %1 BPM, %2 beats per bar.").arg(dialog.info.bpm ?? 0).arg(dialog.info.beatsPerBar ?? 4)
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
    Text {
        objectName: "cutToBeatsProblem"
        Layout.fillWidth: true
        visible: dialog.problem !== ""
        text: dialog.problem
        color: Theme.warning
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            objectName: "cutToBeatsCancel"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "cutToBeatsApply"
            variant: "primary"
            iconName: "music"
            text: qsTr("Cut to Beats")
            enabled: (dialog.info.clips ?? 0) > 0
            onClicked: dialog.cut()
        }
    ]
}
