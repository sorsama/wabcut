pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Settings > Audio (A.8, M5m; the VoiceOver bridge): the microphone voice-over records from (the system default or a
// capture device), "Test input" with its meter (the microphone is open only while it is on, and closes with the
// dialog), the voice-over options (count-in and its beep, pre-roll, post-roll, input monitoring and its gain) and the
// recording latency: the calibration, measured by a loopback pass ("Measure": clicks played through the speakers into
// the microphone) or typed. Every value is saved (Preferences voiceOver/*, through VoiceOverSync).
WabcutDialog {
    id: dialog
    objectName: "audioSettings"

    /// The capture devices: VoiceOver.inputDevices ([{ id, name, isDefault }]).
    property var devices: VoiceOver.inputDevices
    /// Enumerate the devices when the dialog opens (the tests turn it off: enumerating starts the system's audio).
    property bool refreshOnOpen: true
    /// "Test input" is on: the dialog holds the microphone open (VoiceOver.openInput()).
    property bool testing: false
    /// A loopback measurement is running.
    property bool measuring: false
    /// The last measurement's result, or "".
    property string measured: ""

    readonly property var options: VoiceOver.options
    readonly property var latency: VoiceOver.latency
    readonly property var deviceOptions: [{ id: "", name: qsTr("System default") }].concat(
        (dialog.devices ?? []).map(d => ({ id: d.id, name: d.isDefault ? qsTr("%1 (default)").arg(d.name) : d.name })))
    readonly property string deviceName: {
        const current = dialog.deviceOptions.find(o => o.id === VoiceOver.inputDevice)
        //: A saved microphone that is not connected now
        return current ? current.name : qsTr("%1 (not connected)").arg(VoiceOver.inputDevice)
    }

    /// Sets one of VoiceOver.options (the bridge clamps it; VoiceOverSync saves it).
    function setOption(key, value) {
        const o = VoiceOver.options
        o[key] = value
        VoiceOver.options = o
    }
    function setTesting(on) {
        if (on === dialog.testing)
            return
        dialog.testing = on
        if (on)
            dialog.testing = VoiceOver.openInput()
        else
            VoiceOver.closeInput()
    }
    function measure() {
        dialog.measured = ""
        dialog.measuring = VoiceOver.measureLatency()
    }
    function ms(value) {
        return value === undefined || value === null ? qsTr("–") : qsTr("%1 ms").arg(Number(value).toFixed(1))
    }

    title: qsTr("Audio")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(160), Theme.space(160))

    onAboutToShow: {
        if (dialog.refreshOnOpen)
            VoiceOver.refreshDevices()
    }
    onClosed: {
        dialog.setTesting(false)
        if (dialog.measuring)
            VoiceOver.cancel()
        dialog.measuring = false
    }

    Connections {
        target: VoiceOver
        function onLatencyMeasured(roundTripMs, clicks) {
            dialog.measuring = false
            dialog.measured = qsTr("Round trip %1 ms from %n click(s): recordings now line up with the program.", "", clicks)
                .arg(Number(roundTripMs).toFixed(1))
        }
        function onStateChanged() {
            if (dialog.measuring && !VoiceOver.recording)
                dialog.measuring = false
        }
    }

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    // ---- Input ----
    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Voice-over input")
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)

        Label { text: qsTr("Microphone") }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            PillButton {
                id: devicePill
                objectName: "inputDeviceButton"
                Layout.fillWidth: true
                compact: true
                iconName: "mic"
                text: dialog.deviceName
                tooltip: qsTr("The microphone voice-over records from")
                enabled: !VoiceOver.recording
                onClicked: deviceMenu.popup(devicePill, 0, devicePill.height + Theme.space(1))
                OptionsMenu {
                    id: deviceMenu
                    objectName: "inputDeviceMenu"
                    options: dialog.deviceOptions
                    current: VoiceOver.inputDevice
                    onChosen: id => VoiceOver.inputDevice = id
                }
            }
            IconButton {
                objectName: "refreshInputDevices"
                compact: true
                iconName: "rotate-cw"
                tooltip: qsTr("Look for microphones again")
                onClicked: VoiceOver.refreshDevices()
            }
        }

        Label { text: qsTr("Test input") }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(3)
            Toggle {
                objectName: "testInputToggle"
                checked: dialog.testing
                tooltip: qsTr("Open the microphone and show its level")
                onToggled: {
                    dialog.setTesting(checked)
                    checked = Qt.binding(() => dialog.testing)
                }
            }
            InputMeter {
                objectName: "settingsInputMeter"
                Layout.fillWidth: true
                showScale: true
            }
        }
    }
    Note {
        objectName: "inputStatus"
        text: VoiceOver.inputOpen ? qsTr("The microphone is open: %1. Speak at your recording level; keep the bar out of the red.").arg(VoiceOver.inputName)
                                  : qsTr("Wabcut opens the microphone only while you test it here or record a voice-over.")
    }

    // ---- Voice-over options ----
    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Recording")
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 4
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)

        Label { text: qsTr("Count-in") }
        NumberField {
            objectName: "countInField"
            from: 0
            to: 10
            value: dialog.options.countIn ?? 3
            suffix: qsTr("s")
            Accessible.name: qsTr("Count-in seconds")
            onValueModified: v => dialog.setOption("countIn", Math.round(v))
        }
        Label { text: qsTr("Beep") }
        Toggle {
            objectName: "beepToggle"
            checked: dialog.options.beep ?? true
            tooltip: qsTr("A beep on each second of the count-in")
            onToggled: dialog.setOption("beep", checked)
        }

        Label { text: qsTr("Pre-roll") }
        NumberField {
            objectName: "prerollField"
            from: 0
            to: 60
            decimals: 1
            stepSize: 0.5
            value: dialog.options.prerollSeconds ?? 2
            suffix: qsTr("s")
            Accessible.name: qsTr("Pre-roll seconds")
            onValueModified: v => dialog.setOption("prerollSeconds", v)
        }
        Label { text: qsTr("Post-roll") }
        NumberField {
            objectName: "postrollField"
            from: 0
            to: 60
            decimals: 1
            stepSize: 0.5
            value: dialog.options.postrollSeconds ?? 1
            suffix: qsTr("s")
            Accessible.name: qsTr("Post-roll seconds")
            onValueModified: v => dialog.setOption("postrollSeconds", v)
        }

        Label { text: qsTr("Monitor input") }
        Toggle {
            objectName: "monitorToggle"
            checked: dialog.options.monitor ?? false
            tooltip: qsTr("Hear the microphone while recording (use headphones)")
            onToggled: dialog.setOption("monitor", checked)
        }
        Label { text: qsTr("Monitor gain") }
        NumberField {
            objectName: "monitorGainField"
            enabled: dialog.options.monitor ?? false
            from: -60
            to: 12
            decimals: 1
            stepSize: 0.5
            value: dialog.options.monitorGainDb ?? 0
            suffix: qsTr("dB")
            Accessible.name: qsTr("Monitor gain")
            onValueModified: v => dialog.setOption("monitorGainDb", v)
        }
    }
    Note {
        text: qsTr("A pass records onto the first armed audio track (R in the track header or the mixer), between In and Out when both are set, else from the playhead until you stop it.")
    }

    // ---- Latency ----
    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Latency")
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 4
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)

        Label { text: qsTr("Input") }
        Label {
            objectName: "inputLatency"
            font.family: Theme.fontMono
            text: VoiceOver.inputOpen ? dialog.ms(dialog.latency.inputMs) : qsTr("–")
        }
        Label { text: qsTr("Round trip") }
        Label {
            objectName: "roundTripLatency"
            font.family: Theme.fontMono
            text: (dialog.latency.roundTripMs ?? 0) > 0 ? dialog.ms(dialog.latency.roundTripMs) : qsTr("Not measured")
        }

        Label { text: qsTr("Calibration") }
        NumberField {
            objectName: "calibrationField"
            from: -1000
            to: 1000
            decimals: 1
            stepSize: 0.5
            value: dialog.options.calibrationMs ?? 0
            suffix: qsTr("ms")
            Accessible.name: qsTr("Latency calibration")
            onValueModified: v => dialog.setOption("calibrationMs", v)
        }
        Item { Layout.columnSpan: 2; implicitHeight: 1 }
    }
    Note {
        text: qsTr("To measure, hold the microphone to the speakers (or connect the audio output to the input) and choose Measure: Wabcut plays clicks for three seconds and sets the calibration from what comes back.")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        PillButton {
            objectName: "measureLatencyButton"
            compact: true
            iconName: "timer"
            text: dialog.measuring ? qsTr("Measuring…") : qsTr("Measure")
            enabled: !VoiceOver.recording
            onClicked: dialog.measure()
        }
        Text {
            objectName: "latencyResult"
            Layout.fillWidth: true
            visible: text !== ""
            text: dialog.measured
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    Text {
        objectName: "audioSettingsError"
        Layout.fillWidth: true
        visible: text !== ""
        text: VoiceOver.lastError
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "closeAudioSettings"
            variant: "primary"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
