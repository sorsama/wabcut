pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Audio Gain… (G, 0.1.2; Premiere's Clip > Audio Options > Audio Gain): on the selected clips (video clips stand for
// their linked audio), ClipAudio.audioGain sets the gain to a level, adjusts it by one, or normalizes the loudest sample
// peak / every clip's peak to a level (the clips are measured off the GUI thread first). One undo step.
WabcutDialog {
    id: dialog
    objectName: "audioGainDialog"

    property var clipKeys: []
    property int modeIndex: 0
    property real level: 0
    property bool running: false
    property string errorText: ""
    readonly property var modes: [
        { id: "set", label: qsTr("Set gain to"), unit: qsTr("dB") },
        { id: "adjust", label: qsTr("Adjust gain by"), unit: qsTr("dB") },
        { id: "normalizeMax", label: qsTr("Normalize max peak to"), unit: qsTr("dBFS") },
        { id: "normalizeAll", label: qsTr("Normalize all peaks to"), unit: qsTr("dBFS") }
    ]
    readonly property var mode: modes[modeIndex] ?? modes[0]

    function openFor(keys) {
        clipKeys = keys
        running = false
        errorText = keys.length === 0 ? qsTr("Select clips with sound first") : ""
        modeIndex = 0
        level = ClipAudio.clipGainDb(keys)
        open()
    }
    function chooseMode(index) {
        modeIndex = index
        // Premiere's starting values: the current gain to set, nothing to adjust, 0 dBFS peaks.
        level = mode.id === "set" ? ClipAudio.clipGainDb(clipKeys) : 0
    }
    function apply() {
        errorText = ""
        running = mode.id === "normalizeMax" || mode.id === "normalizeAll"
        const error = ClipAudio.audioGain(clipKeys, mode.id, level)
        if (error !== "") {
            running = false
            errorText = error
        } else if (!running) {
            close()
        }
    }

    title: qsTr("Audio Gain")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))

    Connections {
        target: ClipAudio
        function onAudioGainDone(error) {
            if (!dialog.running)
                return
            dialog.running = false
            if (error === "")
                dialog.close()
            else
                dialog.errorText = error
        }
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("%n clip(s) selected", "", dialog.clipKeys.length)
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    PillTabs {
        objectName: "audioGainMode"
        Layout.fillWidth: true
        compact: true
        enabled: !dialog.running
        model: [qsTr("Set"), qsTr("Adjust"), qsTr("Max peak"), qsTr("All peaks")]
        currentIndex: dialog.modeIndex
        onActivated: index => dialog.chooseMode(index)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        enabled: !dialog.running

        Text {
            Layout.fillWidth: true
            text: dialog.mode.label
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        NumberField {
            objectName: "audioGainLevel"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: dialog.mode.id === "adjust" ? -96 : dialog.mode.id === "set" ? -96 : -60
            to: dialog.mode.id === "set" || dialog.mode.id === "adjust" ? 24 : 0
            stepSize: 1
            dragStep: 0.1
            decimals: 1
            value: dialog.level
            suffix: dialog.mode.unit
            Accessible.name: dialog.mode.label
            onValueModified: value => dialog.level = value
        }
    }

    Text {
        objectName: "audioGainError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.running ? qsTr("Measuring peaks…") : dialog.errorText
        color: dialog.running ? Theme.textSecondary : Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "audioGainOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: !dialog.running && dialog.clipKeys.length > 0
            onClicked: dialog.apply()
        }
    ]
}
