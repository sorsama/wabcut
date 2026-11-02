pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Voice-over recording over the Program Monitor (A.8, M5m; Premiere's Voice-over Record). It fills the monitor's well:
//   * the count-in: a big 3, 2, 1 on a scrim with the track the pass records onto (the program is muted, a beep
//     marks each second when the option is on);
//   * pre-roll, recording, post-roll: a badge at the top left with a red dot and "Recording", the time recorded,
//     the track and the input meter, and a red frame round the well while the take records. Pre-roll and post-roll
//     (the program plays, nothing is kept) are dimmed: a hollow dot, muted text.
// 0.6 (P1 round, #53): the script prompter - a band along the bottom with the paragraph being read, large, and the next
// one dimmed, while a pass runs (count-in included) or while a track is armed with Settings > Audio > Show the script
// on. Right / Page Down and Left / Page Up (a presentation clicker) move through the script during a pass; its arrows
// do it any time. Mirrored for a teleprompter glass when asked.
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
    // 0.6 (#53): the prompter.
    property bool prompterShown: VoiceOver.prompterVisible
    property var paragraphs: VoiceOver.paragraphs
    property int paragraph: VoiceOver.paragraphIndex
    /// The paragraph's text size: the one chosen in Settings, smaller on a small monitor (about 11 lines of the well).
    readonly property int prompterPixelSize: Math.min(VoiceOver.prompterTextSize, Math.max(14, Math.round(root.height / 11)))

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

    visible: counting || rolling || prompterShown

    // Fix round 1 (ARGON-26): a pass that starts while a text field has the keyboard (the script's paste area, a caption's
    // text, a value being typed) would type its keys - Space, Esc, a clicker's arrows - into the field. The overlay takes
    // the keyboard for the pass and gives it back to that field afterwards.
    property bool recordingNow: VoiceOver.recording
    property Item focusBeforePass: null
    onRecordingNowChanged: root.keysForPass(root.recordingNow)
    function keysForPass(recording: bool) {
        if (recording) {
            const current = root.Window.activeFocusItem
            if (current && (current instanceof TextInput || current instanceof TextEdit)) {
                root.focusBeforePass = current
                root.forceActiveFocus()
            }
            return
        }
        const back = root.focusBeforePass
        root.focusBeforePass = null
        if (back && back.visible && root.activeFocus)
            back.forceActiveFocus()
    }

    Shortcut {
        sequence: "Esc"
        enabled: root.visible && VoiceOver.recording
        onActivated: VoiceOver.cancel()
    }
    // 0.6 (#53): the prompter's keys, only during a pass (count-in included), so a clicker drives it while you read.
    Shortcut {
        sequences: ["Right", "PgDown"]
        enabled: root.prompterShown && VoiceOver.recording
        onActivated: VoiceOver.nextParagraph()
    }
    Shortcut {
        sequences: ["Left", "PgUp"]
        enabled: root.prompterShown && VoiceOver.recording
        onActivated: VoiceOver.previousParagraph()
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

    // ---- 0.6 (#53): the script prompter ----
    Rectangle {
        id: prompter
        objectName: "voiceOverPrompter"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space(3)
        visible: root.prompterShown && root.paragraphs.length > 0
        height: Math.min(prompterColumn.implicitHeight + Theme.space(4), root.height * 0.45)
        radius: Theme.radiusField
        color: Theme.scrim
        clip: true
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("Script, paragraph %1 of %2: %3").arg(root.paragraph + 1).arg(root.paragraphs.length)
                         .arg(currentText.text)

        ColumnLayout {
            id: prompterColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space(2)
            spacing: Theme.space(1)
            // A teleprompter's glass shows it mirrored.
            transform: Scale {
                origin.x: prompterColumn.width / 2
                xScale: VoiceOver.mirror ? -1 : 1
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Text {
                    objectName: "voiceOverPrompterCount"
                    Layout.fillWidth: true
                    text: qsTr("Script · %1 of %2").arg(root.paragraph + 1).arg(root.paragraphs.length)
                    color: Theme.scrimTextMuted
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                IconButton {
                    objectName: "voiceOverPrompterPrevious"
                    compact: true
                    iconName: "chevron-up"
                    enabled: root.paragraph > 0
                    tooltip: qsTr("Previous paragraph")
                    iconColor: Theme.scrimText
                    shortcut: VoiceOver.recording ? "Left" : ""
                    onClicked: VoiceOver.previousParagraph()
                }
                IconButton {
                    objectName: "voiceOverPrompterNext"
                    compact: true
                    iconName: "chevron-down"
                    enabled: root.paragraph + 1 < root.paragraphs.length
                    tooltip: qsTr("Next paragraph")
                    iconColor: Theme.scrimText
                    shortcut: VoiceOver.recording ? "Right" : ""
                    onClicked: VoiceOver.nextParagraph()
                }
            }
            Text {
                id: currentText
                objectName: "voiceOverPrompterText"
                Layout.fillWidth: true
                text: root.paragraph >= 0 && root.paragraph < root.paragraphs.length ? root.paragraphs[root.paragraph] : ""
                wrapMode: Text.Wrap
                color: Theme.scrimText
                font.family: Theme.fontSans
                font.pixelSize: root.prompterPixelSize
                font.weight: Font.DemiBold
                lineHeight: 1.15
            }
            Text {
                objectName: "voiceOverPrompterNextText"
                Layout.fillWidth: true
                visible: text !== ""
                text: root.paragraph + 1 < root.paragraphs.length ? root.paragraphs[root.paragraph + 1] : ""
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                color: Theme.scrimTextMuted
                font.family: Theme.fontSans
                font.pixelSize: Math.round(root.prompterPixelSize * 0.6)
            }
        }
    }
}
