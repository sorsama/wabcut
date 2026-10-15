pragma ComponentBehavior: Bound
import QtQuick

// Voice-over preferences (A.8, M5m): the saved microphone and options (Preferences.voiceOver*, Settings voiceOver/*)
// go to the VoiceOver bridge when this is created (Main has one), and whatever changes on the bridge afterwards (the
// Audio settings dialog, the calibration a latency measurement sets) is saved back. The bridge does not read
// Settings itself.
QtObject {
    id: sync

    /// The saved options, in the bridge's keys.
    function savedOptions() {
        return {
            countIn: Preferences.voiceOverCountIn,
            beep: Preferences.voiceOverBeep,
            prerollSeconds: Preferences.voiceOverPreroll,
            postrollSeconds: Preferences.voiceOverPostroll,
            monitor: Preferences.voiceOverMonitor,
            monitorGainDb: Preferences.voiceOverMonitorGainDb,
            calibrationMs: Preferences.voiceOverCalibrationMs
        }
    }
    function save() {
        const o = VoiceOver.options
        Preferences.voiceOverCountIn = o.countIn
        Preferences.voiceOverBeep = o.beep
        Preferences.voiceOverPreroll = o.prerollSeconds
        Preferences.voiceOverPostroll = o.postrollSeconds
        Preferences.voiceOverMonitor = o.monitor
        Preferences.voiceOverMonitorGainDb = o.monitorGainDb
        Preferences.voiceOverCalibrationMs = o.calibrationMs
    }

    readonly property Connections _bridge: Connections {
        target: VoiceOver
        function onOptionsChanged() { sync.save() }
        function onInputChanged() { Preferences.voiceOverInput = VoiceOver.inputDevice }
    }

    Component.onCompleted: {
        VoiceOver.inputDevice = Preferences.voiceOverInput
        VoiceOver.options = sync.savedOptions()
    }
}
