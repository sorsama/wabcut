pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Sequence > Beat Grid > Detect Beats from Selected Clip… and Clip > Detect Beats… (P1 round, D9 #8): analysis.detectBeats
// on a clip's music with an optional tempo range. The detection runs in Beats as one undo step ("Detect Beats"), off the
// GUI thread; the toast reports the tempo and the beats. The menu row asks for this dialog after the menu has closed.
WabcutDialog {
    id: dialog
    objectName: "detectBeatsDialog"

    /// The clip ("c12") Detect Beats works on, and its media's name, set by openFor().
    property string clipKey: ""
    property string clipName: ""
    /// Why Detect could not start, or the tempo range is wrong ("").
    property string problem: ""

    /// Opens the dialog on a clip ("c12") and its media's name.
    function openFor(key: string, name: string) {
        clipKey = key
        clipName = name
        problem = ""
        minField.text = ""
        maxField.text = ""
        open()
    }
    /// A field's tempo: 0 for an empty field (auto), NaN for something that is not a number.
    function tempoOf(text: string): real {
        const trimmed = text.trim()
        if (trimmed === "")
            return 0
        const value = Number(trimmed)
        return isFinite(value) ? value : NaN
    }

    title: qsTr("Detect Beats")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(110), Theme.space(110))

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)

        Text {
            objectName: "detectBeatsSummary"
            Layout.fillWidth: true
            text: qsTr("Finds the tempo of “%1” and adds numbered beat markers (downbeats red) and the sequence's beat grid. Snap to Beats and Cut to Beats use the grid.").arg(dialog.clipName)
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Tempo range (optional)")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.Medium
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            FieldInput {
                id: minField
                objectName: "detectBeatsMinBpm"
                Layout.fillWidth: true
                placeholderText: qsTr("Lowest BPM (auto)")
                Accessible.name: qsTr("Lowest tempo in beats per minute, or auto")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                onAccepted: dialog.startDetect()
            }
            FieldInput {
                id: maxField
                objectName: "detectBeatsMaxBpm"
                Layout.fillWidth: true
                placeholderText: qsTr("Highest BPM (auto)")
                Accessible.name: qsTr("Highest tempo in beats per minute, or auto")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                onAccepted: dialog.startDetect()
            }
        }
        Text {
            objectName: "detectBeatsProblem"
            Layout.fillWidth: true
            visible: dialog.problem !== ""
            text: dialog.problem
            color: Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Leave both on auto unless you know the music's tempo: a range narrows the search and can help with a fast or slow track.")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            wrapMode: Text.Wrap
        }
    }

    /// Checks the range and starts the detection; why it did not, in the dialog.
    function startDetect() {
        const low = dialog.tempoOf(minField.text)
        const high = dialog.tempoOf(maxField.text)
        if (isNaN(low) || isNaN(high)) {
            dialog.problem = qsTr("Enter a tempo in BPM, or leave the field empty for auto")
            return
        }
        for (const value of [low, high]) {
            if (value !== 0 && (value < 20 || value > 400)) {
                dialog.problem = qsTr("Enter a tempo between 20 and 400 BPM, or leave it on auto")
                return
            }
        }
        if (low > 0 && high > 0 && low > high) {
            dialog.problem = qsTr("The lowest tempo is above the highest")
            return
        }
        const why = Beats.detect(dialog.clipKey, low, high)
        if (why !== "") {
            dialog.problem = why
            return
        }
        dialog.close()
    }

    footerData: [
        PillButton {
            objectName: "detectBeatsCancel"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "detectBeatsStart"
            variant: "primary"
            iconName: "music"
            text: qsTr("Detect Beats")
            onClicked: dialog.startDetect()
        }
    ]
}
