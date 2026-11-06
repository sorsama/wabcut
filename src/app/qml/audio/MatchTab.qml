pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The Audio Tools panel's MATCH tab (0.7, card 045; Audition's Match EQ and Match Loudness, Premiere's loudness matching):
// learn from a reference and apply what was learned, each step undoable.
//   Reference  the first selected audio clip (as it renders, effects included), the mix between the sequence's In and Out
//              points, or an audio file of the project.
//   Target     the audio clips selected on the timeline.
//   Learn      listens to both in the background (Cancel stops it): the average spectrum of each over the stretches that are
//              not pauses, and the loudness, true peak, RMS and crest factor. Nothing in the project changes.
//   EQ         the spectra and the correction are drawn (MatchSpectrum): 24 bands from 79 Hz to 16 kHz, at most +-18 dB, the
//              overall level left alone. Amount and Smooth reshape the curve at once. Apply EQ gives each target a Graphic
//              Equalizer (30 Bands) marked as an EQ Match (audio.eqMatch, one undo step); Preview switches it on and off to
//              compare by ear while playing; Reset takes it off.
//   Level      the measured values of both and the gain that would match them; Match level moves the targets' gain to the
//              reference's loudness (or true peak) and, with Limit, adds a true-peak limiter at the headroom (audio.levelMatch,
//              one undo step); the report shows before and after.
Item {
    id: root
    objectName: "matchTab"

    readonly property var match: AudioMixer.match
    readonly property bool idle: AudioMixer.busy === ""
    readonly property bool ready: match.reference.set === true && match.target.count > 0
    property string notice: ""

    function numberText(v, unit) {
        return v === undefined || v === null ? qsTr("−∞") : Number(v).toFixed(1) + unit
    }
    function focusContent() {
        learnButton.forceActiveFocus(Qt.TabFocusReason)
    }
    function show(error) {
        root.notice = error
    }

    component Card: Rectangle {
        id: card
        default property alias content: body.data
        property string heading: ""
        Layout.fillWidth: true
        implicitHeight: cardColumn.implicitHeight + Theme.space(5)
        radius: Theme.radiusField
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
        ColumnLayout {
            id: cardColumn
            anchors.fill: parent
            anchors.margins: Theme.space(3)
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                visible: text !== ""
                text: card.heading
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            ColumnLayout {
                id: body
                Layout.fillWidth: true
                spacing: Theme.space(2)
            }
        }
    }
    component Caption: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
    // A pair of measurements side by side: label, reference, target.
    component StatRow: RowLayout {
        id: statRow
        property string label: ""
        property string reference: "—"
        property string target: "—"
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Text {
            Layout.fillWidth: true
            text: statRow.label
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.preferredWidth: Theme.space(18)
            horizontalAlignment: Text.AlignRight
            text: statRow.reference
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.preferredWidth: Theme.space(18)
            horizontalAlignment: Text.AlignRight
            text: statRow.target
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
    }

    Flickable {
        id: flick
        objectName: "matchScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: column
            width: flick.width - Theme.space(3)
            spacing: Theme.space(2)

            // ---- Busy, message ----
            Rectangle {
                objectName: "matchJob"
                Layout.fillWidth: true
                visible: root.match.busy === true
                implicitHeight: busyRow.implicitHeight + Theme.space(4)
                radius: Theme.radiusField
                color: Theme.accentWash
                RowLayout {
                    id: busyRow
                    anchors.fill: parent
                    anchors.margins: Theme.space(2)
                    EchoLoader {
                        Layout.alignment: Qt.AlignVCenter
                        size: Theme.space(5)
                        variant: "equalizer"
                    }
                    Text {
                        Layout.fillWidth: true
                        text: AudioMixer.busy === "matchLearn" ? qsTr("Listening to the reference and the target…")
                            : AudioMixer.busy === "matchEq" ? qsTr("Matching the EQ…") : qsTr("Matching the level…")
                        wrapMode: Text.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    PillButton {
                        objectName: "matchCancel"
                        compact: true
                        text: qsTr("Cancel")
                        onClicked: AudioMixer.cancelJob()
                    }
                }
            }
            Text {
                objectName: "matchMessage"
                Layout.fillWidth: true
                readonly property string shown: root.notice !== "" ? root.notice : (root.match.message !== "" ? root.match.message : AudioMixer.message)
                visible: shown !== ""
                text: shown
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            // ---- Reference and target ----
            Card {
                heading: qsTr("Reference")
                Text {
                    objectName: "matchReferenceLabel"
                    Layout.fillWidth: true
                    text: root.match.reference.set ? root.match.reference.label : qsTr("Nothing chosen yet")
                    elide: Text.ElideRight
                    color: root.match.reference.set ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    PillButton {
                        objectName: "matchUseClip"
                        compact: true
                        enabled: root.idle
                        text: qsTr("Use selected clip")
                        tooltip: qsTr("The first selected audio clip, as it renders")
                        onClicked: root.show(AudioMixer.matchUseSelectedClip())
                    }
                    PillButton {
                        objectName: "matchUseRange"
                        compact: true
                        enabled: root.idle
                        text: qsTr("Use In to Out range")
                        tooltip: qsTr("The mix between the sequence's In and Out points")
                        onClicked: root.show(AudioMixer.matchUseRange())
                    }
                    PillButton {
                        id: mediaButton
                        objectName: "matchUseMedia"
                        compact: true
                        enabled: root.idle
                        iconName: "chevron-down"
                        text: qsTr("Audio file…")
                        tooltip: qsTr("An audio file of the project")
                        onClicked: {
                            mediaMenu.options = AudioMixer.matchMedia().map(m => ({ id: m.id, name: m.name }))
                            if (mediaMenu.options.length === 0)
                                root.show(qsTr("The project has no media with audio"))
                            else
                                mediaMenu.popup(mediaButton, 0, mediaButton.height + Theme.space(1))
                        }
                        OptionsMenu {
                            id: mediaMenu
                            current: ""
                            onChosen: id => root.show(AudioMixer.matchUseMedia(id))
                        }
                    }
                }
            }
            Card {
                heading: qsTr("Target")
                Text {
                    objectName: "matchTargetLabel"
                    Layout.fillWidth: true
                    text: root.match.target.count > 0
                          ? (root.match.target.count === 1 ? root.match.target.label
                                                           : qsTr("%1 and %n more", "", root.match.target.count - 1).arg(root.match.target.label))
                          : qsTr("Nothing chosen yet")
                    elide: Text.ElideRight
                    color: root.match.target.count > 0 ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    objectName: "matchUseTargets"
                    compact: true
                    enabled: root.idle
                    text: qsTr("Use selected clips")
                    tooltip: qsTr("The audio clips selected on the timeline")
                    onClicked: root.show(AudioMixer.matchUseSelectedTargets())
                }
            }

            // ---- Learn, Preview, Apply, Reset ----
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                PillButton {
                    id: learnButton
                    objectName: "matchLearn"
                    variant: "primary"
                    enabled: root.idle && root.ready
                    text: qsTr("Learn")
                    tooltip: qsTr("Listen to the reference and the target and show what differs. Changes nothing")
                    onClicked: root.show(AudioMixer.matchLearn())
                }
                PillButton {
                    objectName: "matchApplyEq"
                    enabled: root.idle && root.ready && root.match.learned
                    text: root.match.applied > 0 ? qsTr("Apply EQ again") : qsTr("Apply EQ")
                    tooltip: qsTr("Give the target clips the EQ Match: one undo step")
                    onClicked: root.show(AudioMixer.matchApplyEq())
                }
                PillButton {
                    objectName: "matchPreview"
                    enabled: root.idle && root.match.applied > 0
                    text: root.match.bypassed > 0 ? qsTr("Preview: original") : qsTr("Preview: matched")
                    tooltip: qsTr("Switch the EQ Match on and off to compare by ear while playing")
                    onClicked: root.show(AudioMixer.matchTogglePreview())
                }
                PillButton {
                    objectName: "matchReset"
                    variant: "ghost"
                    enabled: root.idle && root.match.applied > 0
                    iconName: "rotate-ccw"
                    text: qsTr("Reset")
                    tooltip: qsTr("Take the EQ Match off the target clips")
                    onClicked: root.show(AudioMixer.matchReset())
                }
            }
            Caption {
                visible: root.match.applied > 0
                text: qsTr("%n clip(s) have an EQ Match", "", root.match.applied) + (root.match.bypassed > 0 ? qsTr(" (switched off)") : "")
            }

            // ---- The EQ curve ----
            Card {
                objectName: "matchEqCard"
                heading: qsTr("EQ match")
                MatchSpectrum {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.space(46)
                    bands: root.match.bands
                    offsetDb: root.match.levelOffsetDb
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(3)
                    Repeater {
                        model: [{ name: qsTr("Reference"), color: Theme.accentStroke }, { name: qsTr("Target"), color: Theme.textTertiary },
                                { name: qsTr("Target after"), color: Qt.alpha(Theme.accentStroke, 0.75) }]
                        delegate: RowLayout {
                            required property var modelData
                            spacing: Theme.space(1)
                            Rectangle {
                                implicitWidth: Theme.space(4)
                                implicitHeight: Theme.space(0.5)
                                color: parent.modelData.color
                            }
                            Text {
                                text: parent.modelData.name
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
                PropSlider {
                    objectName: "matchAmount"
                    Layout.fillWidth: true
                    label: qsTr("Amount")
                    unit: "%"
                    from: 0
                    to: 100
                    defaultValue: 100
                    value: root.match.options.amount
                    onValueModified: v => AudioMixer.setMatchOption("amount", v)
                }
                PropSlider {
                    objectName: "matchSmoothing"
                    Layout.fillWidth: true
                    label: qsTr("Smooth")
                    unit: "%"
                    from: 0
                    to: 100
                    defaultValue: 50
                    value: root.match.options.smoothing
                    onValueModified: v => AudioMixer.setMatchOption("smoothing", v)
                }
                Caption {
                    text: root.match.learned
                          ? qsTr("The reference is %1 dB %2 than the target in these bands; EQ Match leaves that to Level Match.")
                                .arg(Math.abs(root.match.levelOffsetDb).toFixed(1)).arg(root.match.levelOffsetDb >= 0 ? qsTr("louder") : qsTr("quieter"))
                          : qsTr("Choose a reference and the target clips, then Learn.")
                }
            }

            // ---- Level ----
            Card {
                objectName: "matchLevelCard"
                heading: qsTr("Level match")
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Item { Layout.fillWidth: true }
                    Text {
                        Layout.preferredWidth: Theme.space(18)
                        horizontalAlignment: Text.AlignRight
                        text: qsTr("Reference")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                    Text {
                        Layout.preferredWidth: Theme.space(18)
                        horizontalAlignment: Text.AlignRight
                        text: qsTr("Target")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
                StatRow {
                    objectName: "matchStatLoudness"
                    label: qsTr("Loudness (LUFS)")
                    reference: root.numberText(root.match.referenceStats.integrated, "")
                    target: root.numberText(root.match.targetStats.integrated, "")
                }
                StatRow {
                    objectName: "matchStatTruePeak"
                    label: qsTr("True peak (dBTP)")
                    reference: root.numberText(root.match.referenceStats.truePeak, "")
                    target: root.numberText(root.match.targetStats.truePeak, "")
                }
                StatRow {
                    objectName: "matchStatRms"
                    label: qsTr("RMS (dBFS)")
                    reference: root.numberText(root.match.referenceStats.rms, "")
                    target: root.numberText(root.match.targetStats.rms, "")
                }
                StatRow {
                    objectName: "matchStatCrest"
                    label: qsTr("Crest factor (dB)")
                    reference: root.numberText(root.match.referenceStats.crest, "")
                    target: root.numberText(root.match.targetStats.crest, "")
                }
                PillTabs {
                    objectName: "matchMode"
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("Match loudness"), qsTr("Match peak")]
                    currentIndex: root.match.options.mode === "peak" ? 1 : 0
                    Accessible.name: qsTr("What to match")
                    onActivated: index => AudioMixer.setMatchOption("mode", index === 1 ? "peak" : "loudness")
                }
                Text {
                    objectName: "matchLevelGain"
                    Layout.fillWidth: true
                    visible: root.match.learned && root.match.levelGainDb !== undefined && root.match.levelGainDb !== null
                    text: qsTr("Gain to apply: %1 dB").arg(Number(root.match.levelGainDb).toFixed(1))
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Headroom")
                        elide: Text.ElideRight
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    NumberField {
                        objectName: "matchHeadroom"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 0
                        to: 24
                        decimals: 1
                        stepSize: 0.5
                        suffix: qsTr("dB")
                        value: root.match.options.headroom
                        onValueModified: v => AudioMixer.setMatchOption("headroom", v)
                        Accessible.name: qsTr("Headroom in dB under full scale")
                    }
                }
                Toggle {
                    objectName: "matchLimit"
                    Layout.fillWidth: true
                    text: qsTr("Limit peaks to the headroom")
                    tooltip: qsTr("Add a true-peak limiter at the headroom when the matched level would pass it")
                    checked: root.match.options.limit
                    onToggled: AudioMixer.setMatchOption("limit", checked)
                }
                PillButton {
                    objectName: "matchApplyLevel"
                    Layout.fillWidth: true
                    variant: "primary"
                    enabled: root.idle && root.ready
                    text: qsTr("Match level")
                    tooltip: qsTr("Move the target clips' gain to the reference. One undo step")
                    onClicked: root.show(AudioMixer.matchApplyLevel())
                }
                // What the last Level Match did, a block per target.
                Repeater {
                    model: root.match.report
                    delegate: ColumnLayout {
                        id: reportRow
                        objectName: "matchReportRow"
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: reportRow.modelData.target
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%1 → %2 LUFS · gain %3 dB").arg(root.numberText(reportRow.modelData.before.integrated, ""))
                                    .arg(root.numberText(reportRow.modelData.after.integrated, ""))
                                    .arg(root.numberText(reportRow.modelData.gainDb, ""))
                            wrapMode: Text.Wrap
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("True peak %1 → %2 dBTP%3").arg(root.numberText(reportRow.modelData.before.truePeak, ""))
                                    .arg(root.numberText(reportRow.modelData.after.truePeak, ""))
                                    .arg(reportRow.modelData.limited ? qsTr(" (limited)") : "")
                            wrapMode: Text.Wrap
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                }
            }
        }
    }
}
