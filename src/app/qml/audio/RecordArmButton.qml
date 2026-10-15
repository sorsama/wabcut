pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Record arm (A.8, M5m): arms or disarms an audio track for voice-over recording (VoiceOver.setRecordArm:
// track.setRecordArm, one undo step; a locked track can be armed). `letter` draws the mixer strip's "R" beside M and S;
// otherwise it is the track header's microphone. Armed: filled red. While a pass records onto the track a ring pulses
// round it (steady with Reduce motion) and a click stops the pass and keeps the take (VoiceOver.stop()).
// A pass records onto the first armed audio track; the Program Monitor's record button and Record Voice-over
// (record.toggle) start it.
// The armed state is the model's Track::recordArmed, read from the mixer strips (AudioMixer.strips[].recordArmed)
// unless the host binds `armed`.
AbstractButton {
    id: control

    /// The track's key ("t12").
    required property string track
    /// The track's label ("A2"): the pass names the track it records onto by it (VoiceOver.targetTrack).
    property string trackLabel: ""
    property bool armed: AudioMixer.strips.some(s => s.key === control.track && s.recordArmed === true)
    /// The mixer strip's letter instead of the microphone.
    property bool letter: false
    /// Why the last click was refused (a video track, no sequence), in the tooltip until the next click.
    property string error: ""

    readonly property bool recordingHere: VoiceOver.recording && control.trackLabel !== ""
                                          && VoiceOver.targetTrack === control.trackLabel
    readonly property bool lit: armed || recordingHere
    readonly property string tip: {
        if (control.error !== "")
            return control.error
        if (control.recordingHere)
            return qsTr("Recording onto %1: click to stop and keep the take").arg(control.trackLabel)
        return control.armed ? qsTr("%1 is armed for voice-over recording: click to disarm").arg(control.trackLabel)
                             : qsTr("Arm %1 for voice-over recording").arg(control.trackLabel)
    }

    /// The click: stop a pass recording here, else arm or disarm.
    function activate() {
        if (control.recordingHere) {
            VoiceOver.stop()
            return
        }
        control.error = VoiceOver.setRecordArm(control.track, !control.armed)
    }

    implicitWidth: letter ? Theme.space(6) : Theme.controlHeightSmall
    implicitHeight: letter ? Theme.controlHeightSmall - Theme.space(2) : Theme.controlHeightSmall
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    onClicked: control.activate()

    Accessible.role: Accessible.CheckBox
    Accessible.checkable: true
    Accessible.checked: control.armed
    Accessible.name: qsTr("Record-arm %1").arg(control.trackLabel)
    Accessible.description: control.tip

    background: Rectangle {
        radius: control.letter ? Theme.radiusBadge : Theme.radiusControl
        color: control.lit ? Theme.danger
             : control.down ? Theme.raised
             : control.hovered ? Theme.hover
             : control.letter ? Theme.sunken : "transparent"
        border.width: control.letter ? Theme.hairline : 0
        border.color: control.lit ? Theme.danger : Theme.border
        opacity: control.enabled ? 1 : Theme.disabledOpacity
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        // Recording onto this track: a ring that pulses while the pass runs.
        Rectangle {
            id: ring
            objectName: "recordingRing"
            anchors.fill: parent
            anchors.margins: -Theme.space(0.75)
            radius: parent.radius + Theme.space(0.75)
            color: "transparent"
            border.width: Theme.selectionWidth
            border.color: Theme.danger
            visible: control.recordingHere
            SequentialAnimation on opacity {
                running: ring.visible && !Theme.reduceMotion
                loops: Animation.Infinite
                alwaysRunToEnd: true
                NumberAnimation { from: 1; to: 0.25; duration: Theme.durSpring; easing.type: Easing.InOutSine }
                NumberAnimation { from: 0.25; to: 1; duration: Theme.durSpring; easing.type: Easing.InOutSine }
            }
        }
        FocusRing {
            targetRadius: control.letter ? Theme.radiusBadge : Theme.radiusControl
            visible: control.visualFocus
        }
    }

    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            visible: !control.letter
            name: "mic"
            size: Theme.iconSm
            color: control.lit ? Theme.scrimText : control.hovered ? Theme.textPrimary : Theme.textSecondary
        }
        Text {
            anchors.centerIn: parent
            visible: control.letter
            //: Record-arm (one letter on the mixer strip, beside M and S)
            text: qsTr("R")
            color: control.lit ? Theme.clipInk : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.Bold
        }
    }

    HoverHandler {
        enabled: control.enabled
        cursorShape: Qt.PointingHandCursor
    }

    Loader {
        active: control.hovered
        sourceComponent: Tooltip {
            parent: control
            visible: true
            text: control.tip
            shortcut: control.recordingHere && KeymapDispatcher.revision >= 0 ? KeymapDispatcher.shortcutText("record.toggle") : ""
        }
    }
}
