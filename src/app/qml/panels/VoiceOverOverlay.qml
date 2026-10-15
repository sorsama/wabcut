pragma ComponentBehavior: Bound
import QtQuick

// Voice-over recording over the Program Monitor (A.8, M5m; Premiere's Voice-over Record). It fills the monitor's well:
//   * the count-in: a big 3, 2, 1 on a scrim with the track the pass records onto (the program is muted, a beep
//     marks each second when the option is on);
//   * pre-roll, recording, post-roll: a badge at the top left with a red dot and "Recording", the time recorded,
//     the track and the input meter, and a red frame round the well while the take records. Pre-roll and post-roll
//     (the program plays, nothing is kept) are dimmed: a hollow dot, muted text.
// Nothing otherwise. Space stops the pass and keeps the take (the bridge follows the transport); Esc cancels it.
// The state comes from the VoiceOver bridge; the properties can be set directly (a preview, the tests).
Item {
    id: root
    objectName: "voiceOverOverlay"

    property string passState: VoiceOver.state
    property int count: VoiceOver.countIn
    property real seconds: VoiceOver.recordedSeconds
    property string trackLabel: VoiceOver.targetTrack
    property var levels: VoiceOver.levels

    readonly property bool counting: passState === "countIn"
    readonly property bool rolling: passState === "preroll" || passState === "recording" || passState === "postroll"
    readonly property bool takeRecording: passState === "recording"

    /// "1:05.3": minutes, seconds and tenths.
    function elapsedText(s) {
        const tenths = Math.max(0, Math.floor(s * 10))
        const minutes = Math.floor(tenths / 600)
        const secs = Math.floor((tenths % 600) / 10)
        return minutes + ":" + String(secs).padStart(2, "0") + "." + (tenths % 10)
    }

    visible: counting || rolling

    Shortcut {
        sequence: "Esc"
        enabled: root.visible && VoiceOver.recording
        onActivated: VoiceOver.cancel()
    }

    // ---- The count-in ----
    Rectangle {
        objectName: "voiceOverCountIn"
        anchors.fill: parent
        visible: root.counting
        radius: Theme.radiusField
        color: Theme.scrim

        Column {
            anchors.centerIn: parent
            spacing: Theme.space(3)

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.trackLabel !== "" ? qsTr("Recording onto %1 in").arg(root.trackLabel) : qsTr("Recording in")
                color: Theme.scrimTextMuted
                font.family: Theme.fontSans
                font.pixelSize: Theme.textLg
                font.weight: Font.DemiBold
            }
            Rectangle {
                id: dial
                readonly property real size: Math.max(Theme.space(16), Math.min(root.width, root.height) * 0.36)
                anchors.horizontalCenter: parent.horizontalCenter
                width: size
                height: size
                radius: size / 2
                color: "transparent"
                border.width: Theme.selectionWidth * 2
                border.color: Theme.danger

                Text {
                    id: countText
                    objectName: "voiceOverCountNumber"
                    anchors.centerIn: parent
                    text: String(root.count)
                    color: Theme.scrimText
                    font.family: Theme.fontSans
                    font.pixelSize: Math.round(dial.size * 0.55)
                    font.weight: Font.Bold
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Recording starts in %1").arg(root.count)
                }
                NumberAnimation {
                    id: pop
                    target: countText
                    property: "scale"
                    from: 1.18
                    to: 1
                    duration: Theme.durState
                    easing.type: Easing.OutCubic
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Space stops · Esc cancels")
                color: Theme.scrimTextMuted
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }
    onCountChanged: {
        if (root.counting && !Theme.reduceMotion)
            pop.restart()
    }

    // ---- Pre-roll, recording, post-roll ----
    Rectangle {
        objectName: "voiceOverRecordingFrame"
        anchors.fill: parent
        visible: root.takeRecording
        radius: Theme.radiusField
        color: "transparent"
        border.width: Theme.selectionWidth
        border.color: Theme.danger
    }
    Rectangle {
        id: badge
        objectName: "voiceOverBadge"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: Theme.space(3)
        visible: root.rolling
        implicitWidth: badgeRow.implicitWidth + Theme.space(5)
        implicitHeight: Theme.controlHeight
        width: implicitWidth
        height: implicitHeight
        radius: height / 2
        color: Theme.scrim
        Accessible.role: Accessible.StaticText
        Accessible.name: badgeLabel.text + " " + elapsed.text + " " + root.trackLabel

        Row {
            id: badgeRow
            anchors.centerIn: parent
            spacing: Theme.space(2)

            Rectangle {
                id: dot
                objectName: "voiceOverDot"
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.space(3)
                height: width
                radius: width / 2
                color: root.takeRecording ? Theme.danger : "transparent"
                border.width: root.takeRecording ? 0 : Theme.selectionWidth
                border.color: Theme.danger
                SequentialAnimation on opacity {
                    running: root.takeRecording && root.visible && !Theme.reduceMotion
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation { from: 1; to: 0.35; duration: Theme.durSpring; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.35; to: 1; duration: Theme.durSpring; easing.type: Easing.InOutSine }
                }
            }
            Text {
                id: badgeLabel
                objectName: "voiceOverLabel"
                anchors.verticalCenter: parent.verticalCenter
                text: root.passState === "preroll" ? qsTr("Pre-roll")
                    : root.passState === "postroll" ? qsTr("Post-roll") : qsTr("Recording")
                color: root.takeRecording ? Theme.scrimText : Theme.scrimTextMuted
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            Text {
                id: elapsed
                objectName: "voiceOverElapsed"
                anchors.verticalCenter: parent.verticalCenter
                text: root.elapsedText(root.seconds)
                color: root.takeRecording ? Theme.scrimText : Theme.scrimTextMuted
                font.family: Theme.fontMono
                font.pixelSize: Theme.textMd
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.trackLabel !== ""
                width: trackText.implicitWidth + Theme.space(2)
                height: Theme.space(5)
                radius: Theme.radiusBadge
                color: root.takeRecording ? Theme.danger : "transparent"
                border.width: Theme.hairline
                border.color: Theme.danger
                Text {
                    id: trackText
                    anchors.centerIn: parent
                    text: root.trackLabel
                    color: root.takeRecording ? Theme.scrimText : Theme.scrimTextMuted
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                    font.weight: Font.Bold
                }
            }
            InputMeter {
                objectName: "voiceOverMeter"
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.space(24)
                levels: root.levels
            }
        }
    }
}
