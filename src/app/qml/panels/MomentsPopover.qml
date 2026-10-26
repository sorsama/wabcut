pragma ComponentBehavior: Bound
import QtQuick

// Moments (M5o, gaming; the Tracks header's zap button): one-click speed and zoom presets at the playhead, on the
// selected clip under it or else the topmost video clip there (Moments.target). Slow motion (25 % or 50 %, how much
// footage), Speed ramp in, Freeze + replay, Fast forward (200 / 300 / 400 %; In to Out when both marks are on the
// clip), Punch in, Punch in + hold and Shake. Each is one undo step; the result is ordinary remap keys and Motion
// keyframes (the speed line, Effect Controls).
// 0.4: Instant replay replays In to Out when both marks are on the clip, else the 5 seconds before the playhead on a
// clip longer than 20 s, else the whole clip; its options under the button (25 / 50 % speed, Sound, Stinger) are
// remembered (Moments.replaySpeed / replaySound / replayLabel) and used by the clip menu's Instant Replay too. The fast
// forward speeds have their own full-width row.
Popover {
    id: root
    objectName: "momentsPopover"

    title: qsTr("Moments")
    transformOrigin: Item.TopLeft

    readonly property var target: Moments.target
    readonly property bool canRetime: root.target.speed === true
    readonly property bool canZoom: root.target.available === true
    readonly property var slowSpeeds: [25, 50]
    readonly property var slowLengths: [0.5, 1, 2]
    readonly property var fastSpeeds: [200, 300, 400]

    function run(preset, options) {
        Moments.apply(preset, options ?? {})
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Column {
        width: Theme.space(88)
        spacing: Theme.space(2)

        // What the presets act on.
        Row {
            objectName: "momentsTarget"
            width: parent.width
            spacing: Theme.space(2)

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: root.target.available ? "film" : "triangle-alert"
                size: Theme.iconSm
                color: root.target.available ? Theme.accentStroke : Theme.textTertiary
            }
            Text {
                objectName: "momentsTargetText"
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - Theme.iconSm - Theme.space(2)
                elide: Text.ElideRight
                text: root.target.available ? qsTr("%1 at %2").arg(root.target.name).arg(root.target.atText)
                                            : (root.target.reason ?? "")
                color: root.target.available ? Theme.textPrimary : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        GroupLabel { text: qsTr("Slow motion") }
        Row {
            width: parent.width
            spacing: Theme.space(2)
            PillTabs {
                id: slowSpeed
                objectName: "momentsSlowSpeed"
                width: (parent.width - parent.spacing) * 0.4
                compact: true
                model: [{ label: qsTr("25 %") }, { label: qsTr("50 %") }]
                currentIndex: 0
                Accessible.name: qsTr("Slow motion speed")
            }
            PillTabs {
                id: slowLength
                objectName: "momentsSlowLength"
                width: (parent.width - parent.spacing) * 0.6
                compact: true
                model: [{ label: qsTr("0.5 s") }, { label: qsTr("1 s") }, { label: qsTr("2 s") }]
                currentIndex: 1
                Accessible.name: qsTr("How much of the action plays slowly")
            }
        }
        PillButton {
            objectName: "momentsSlowMoButton"
            width: parent.width
            compact: true
            variant: "primary"
            iconName: "timer"
            text: qsTr("Slow-mo this moment")
            tooltip: root.canRetime ? qsTr("Eases down to %1 % around the playhead and back to normal speed")
                                      .arg(root.slowSpeeds[slowSpeed.currentIndex])
                                    : (root.target.speedReason ?? root.target.reason ?? "")
            command: "moments.slowMo"
            enabled: root.canRetime
            onClicked: root.run(slowSpeed.currentIndex === 0 ? "slowMo25" : "slowMo50",
                                { seconds: root.slowLengths[slowLength.currentIndex] })
        }

        GroupLabel { text: qsTr("Speed") }
        Row {
            width: parent.width
            spacing: Theme.space(2)
            PillButton {
                objectName: "momentsRampInButton"
                width: (parent.width - parent.spacing) / 2
                compact: true
                iconName: "gauge"
                text: qsTr("Speed ramp in")
                tooltip: qsTr("The second before the playhead plays at 300 %, slowing to normal at it")
                enabled: root.canRetime
                onClicked: root.run("rampIn")
            }
            PillButton {
                objectName: "momentsFreezeReplayButton"
                width: (parent.width - parent.spacing) / 2
                compact: true
                iconName: "repeat"
                text: qsTr("Freeze + replay")
                tooltip: qsTr("Holds the frame at the playhead, then replays the last 2 seconds at 50 %")
                enabled: root.canRetime
                onClicked: root.run("freezeReplay")
            }
        }
        // 0.4: the speeds on their own full-width row (their labels never elide), the button under them.
        PillTabs {
            id: fastSpeed
            objectName: "momentsFastSpeed"
            width: parent.width
            compact: true
            model: [{ label: qsTr("200 %") }, { label: qsTr("300 %") }, { label: qsTr("400 %") }]
            currentIndex: 1
            Accessible.name: qsTr("Fast forward speed")
        }
        PillButton {
            objectName: "momentsFastForwardButton"
            width: parent.width
            compact: true
            iconName: "skip-forward"
            text: qsTr("Fast forward")
            tooltip: qsTr("In to Out at %1 % when both marks are on the clip, else 3 seconds from the playhead")
                     .arg(root.fastSpeeds[fastSpeed.currentIndex])
            enabled: root.canRetime
            onClicked: root.run("fastForward" + root.fastSpeeds[fastSpeed.currentIndex])
        }

        GroupLabel { text: qsTr("Zoom") }
        Flow {
            width: parent.width
            spacing: Theme.space(2)
            PillButton {
                objectName: "momentsPunchInButton"
                compact: true
                iconName: "zoom-in"
                text: qsTr("Punch in")
                tooltip: qsTr("Zooms in to 130 % at the playhead, holds a second and eases back")
                command: "moments.punchIn"
                enabled: root.canZoom
                onClicked: root.run("punchIn")
            }
            PillButton {
                objectName: "momentsPunchInHoldButton"
                compact: true
                iconName: "scaling"
                text: qsTr("Punch in + hold")
                tooltip: qsTr("Zooms in to 130 % at the playhead and stays there")
                enabled: root.canZoom
                onClicked: root.run("punchInHold")
            }
            PillButton {
                objectName: "momentsShakeButton"
                compact: true
                iconName: "move-horizontal"
                text: qsTr("Shake")
                tooltip: qsTr("A short camera shake at the playhead")
                enabled: root.canZoom
                onClicked: root.run("shake")
            }
        }

        // C.16's gaming edits, when the catalog has them.
        GroupLabel {
            visible: Moments.gamingEdits.replay === true || Moments.gamingEdits.version === true
                     || Moments.gamingEdits.deadTime === true
            text: qsTr("Replay and versions")
        }
        // Instant replay and its remembered options (0.4).
        Column {
            width: parent.width
            spacing: Theme.space(1.5)
            visible: Moments.gamingEdits.replay === true
            PillButton {
                objectName: "momentsInstantReplayButton"
                width: parent.width
                compact: true
                iconName: "repeat"
                text: qsTr("Instant replay")
                tooltip: qsTr("Replays In to Out when both marks are on the clip, else the 5 seconds before the playhead (the whole clip when it is short), at %1 %")
                         .arg(Moments.replaySpeed < 0.4 ? 25 : 50)
                enabled: root.canRetime
                onClicked: root.run("instantReplay")
            }
            Row {
                objectName: "momentsReplayOptions"
                width: parent.width
                spacing: Theme.space(2)
                visible: Moments.gamingEdits.replay === true
                PillTabs {
                    objectName: "momentsReplaySpeed"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.space(30)
                    compact: true
                    model: [{ label: qsTr("25 %") }, { label: qsTr("50 %") }]
                    currentIndex: Moments.replaySpeed < 0.4 ? 0 : 1
                    Accessible.name: qsTr("Instant replay speed")
                    onActivated: index => Moments.replaySpeed = index === 0 ? 0.25 : 0.5
                }
                Toggle {
                    objectName: "momentsReplaySound"
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Sound")
                    tooltip: qsTr("The replay plays its sound too, slowed down")
                    checked: Moments.replaySound
                    onToggled: {
                        Moments.replaySound = checked
                        checked = Qt.binding(() => Moments.replaySound)
                    }
                }
                Toggle {
                    objectName: "momentsReplayLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Stinger")
                    tooltip: qsTr("A REPLAY title over the replay")
                    checked: Moments.replayLabel
                    onToggled: {
                        Moments.replayLabel = checked
                        checked = Qt.binding(() => Moments.replayLabel)
                    }
                }
            }
        }
        Flow {
            width: parent.width
            spacing: Theme.space(2)
            visible: Moments.gamingEdits.version === true
            PillButton {
                objectName: "momentsShortsVersionButton"
                visible: Moments.gamingEdits.version === true
                compact: true
                iconName: "crop"
                text: qsTr("Make Shorts version")
                tooltip: qsTr("A 1080 x 1920 copy of this sequence with the facecam split or a blurred fill")
                onClicked: root.run("shortsVersion")
            }
            PillButton {
                objectName: "momentsSquareVersionButton"
                visible: Moments.gamingEdits.version === true
                compact: true
                iconName: "square"
                text: qsTr("Make square version")
                tooltip: qsTr("A 1080 x 1080 copy of this sequence")
                onClicked: root.run("squareVersion")
            }
        }

        // Remove dead time (C.16): find it (measured off the GUI thread), see how much, remove it in one undo step.
        // 0.5: Find opens the review in the Highlights panel (each stretch listed with a remove switch, Removal bands on
        // the timeline) and Review… goes back to it; Remove is there, so nothing is cut unseen.
        Column {
            width: parent.width
            spacing: Theme.space(1.5)
            visible: Moments.gamingEdits.deadTime === true
            Row {
                spacing: Theme.space(2)
                PillButton {
                    objectName: "momentsFindDeadTimeButton"
                    compact: true
                    iconName: "scissors"
                    text: Moments.deadTime.busy === true ? qsTr("Measuring…") : qsTr("Find dead time")
                    tooltip: qsTr("Finds loading screens, walking and waiting: quiet game audio and little motion. You review the stretches before anything is cut.")
                    enabled: Moments.deadTime.busy !== true
                    onClicked: {
                        root.close() // the review comes forward in the Highlights panel
                        Moments.openDeadTimeReview()
                        Moments.findDeadTime({})
                    }
                }
                PillButton {
                    objectName: "momentsReviewDeadTimeButton"
                    visible: Moments.deadTime.found === true
                    compact: true
                    variant: "primary"
                    iconName: "list"
                    text: qsTr("Review…")
                    tooltip: qsTr("The stretches found, one by one: keep some, remove the rest in one undo step")
                    onClicked: {
                        root.close()
                        Moments.openDeadTimeReview()
                    }
                }
            }
            Text {
                objectName: "momentsDeadTimeText"
                width: parent.width
                visible: text !== ""
                text: Moments.deadTime.error ?? Moments.deadTime.text ?? ""
                wrapMode: Text.Wrap
                color: Moments.deadTime.error !== undefined ? Theme.warningText : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        // Why the last preset did not apply.
        Text {
            objectName: "momentsMessage"
            width: parent.width
            visible: Moments.message !== ""
            text: Moments.message
            wrapMode: Text.Wrap
            color: Theme.warningText
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }
}
