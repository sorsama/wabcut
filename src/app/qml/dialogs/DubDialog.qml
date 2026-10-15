pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Captions > Dub… and Narration (AI-12): AI voice-over with the user's own voice provider (ElevenLabs, OpenAI or
// Gemini; their key, their bill). Dub speaks a caption track (usually a translated one), each line fitted to its cue:
// sped up by at most 15 % with the pitch kept, otherwise moved and flagged. Narration speaks a script, line by line
// over a rough cut's beats or one after another. The estimate comes first; Generate needs a spending cap and the
// provider's consent; the timing report lists every line that was sped up, moved or flagged. Add to timeline puts the
// speech on a new Dialogue track with the original dialogue ducked under it, as one undo step; the audio is marked
// AI-generated. Cloning the speaker's own voice is opt-in, needs "I have the speaker's permission", and is never offered
// for a redacted speaker (the run refuses it).
WabcutDialog {
    id: dialog
    objectName: "dubDialog"

    property int modeIndex: 0
    property int trackIndex: 0
    property int modelIndex: 0
    property string voice: ""
    property int speakerIndex: 0
    readonly property bool narration: modeIndex === 1
    readonly property var tracks: Dub.captionTracks
    readonly property var track: tracks.length > 0 ? tracks[Math.min(trackIndex, tracks.length - 1)] : null
    readonly property var models: AiController.speechModels
    readonly property var model: models.length > 0 ? models[Math.min(modelIndex, models.length - 1)] : null
    readonly property var voices: model ? Dub.voices(model.id) : []
    readonly property var speakers: Dub.speakers
    readonly property var plan: Dub.plan
    readonly property var report: Dub.report
    readonly property var fit: report.fit ?? null
    readonly property bool canClone: model !== null && model.provider === "elevenlabs"

    function options(allowOnce) {
        const o = { kind: narration ? "narration" : "dub", model: model ? model.id : "", voice: voice, style: styleField.text,
                    maxStretch: stretchSlider.value / 100, duck: duckSlider.value, maxCost: capField.value, allowOnce: allowOnce }
        if (narration)
            o.text = scriptArea.text
        else if (track)
            o.captionTrack = track.id
        if (cloneToggle.checked && canClone) {
            o.cloneVoice = true
            o.rightsConfirmed = rightsToggle.checked
            if (speakers.length > 0)
                o.speaker = speakers[Math.min(speakerIndex, speakers.length - 1)].id
        }
        return o
    }
    function estimate() {
        Dub.estimate(options(false))
    }
    function generate() {
        const prompt = Dub.consentPrompt(model ? model.id : "", plan.characters ?? 0, cloneToggle.checked && canClone)
        if (prompt.needed) {
            consent.ask(prompt)
            return
        }
        Dub.run(options(false))
    }
    /// Opens on "dub" (a caption track) or "narration" (a script); Dub.open(mode) asks Main.qml for it.
    function openFor(mode) {
        modeIndex = mode === "narration" ? 1 : 0
        open()
    }
    /// Prefills from the assistant's request (Dub.proposal).
    function prefill(p) {
        modeIndex = p.kind === "narration" ? 1 : 0
        if (p.text)
            scriptArea.text = p.text
        const t = tracks.findIndex(x => x.id === p.captionTrack)
        if (t >= 0)
            trackIndex = t
        const m = models.findIndex(x => x.id === p.model)
        if (m >= 0)
            modelIndex = m
        voice = p.voice ?? ""
        if (p.style)
            styleField.text = p.style
        if (p.cloneVoice)
            cloneToggle.checked = true
        Dub.clearProposal()
    }

    title: narration ? qsTr("Narration") : qsTr("Dub")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(230), Theme.space(230))
    onOpened: {
        if (Object.keys(Dub.proposal).length > 0)
            prefill(Dub.proposal)
        estimate()
    }
    onClosed: Dub.discard()

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

    PillTabs {
        objectName: "dubMode"
        Layout.fillWidth: true
        model: [qsTr("Dub captions"), qsTr("Narration from a script")]
        currentIndex: dialog.modeIndex
        onActivated: index => { dialog.modeIndex = index; dialog.estimate() }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(4)

        // ---- What is said, and by which voice ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(2)
            enabled: !Dub.busy

            Label { text: dialog.narration ? qsTr("Script") : qsTr("Captions to speak") }
            PillTabs {
                objectName: "dubTrack"
                Layout.fillWidth: true
                compact: true
                visible: !dialog.narration && dialog.tracks.length > 0
                model: dialog.tracks.map(t => t.name)
                currentIndex: dialog.trackIndex
                onActivated: index => { dialog.trackIndex = index; dialog.estimate() }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(34)
                visible: dialog.narration
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: scriptArea.activeFocus ? Theme.focus : Theme.border
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    TextArea {
                        id: scriptArea
                        objectName: "dubScript"
                        wrapMode: TextEdit.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        selectByMouse: true
                        placeholderText: qsTr("One line per beat. On a rough cut, line 1 goes over beat 1.")
                        placeholderTextColor: Theme.textTertiary
                        background: null
                        onEditingFinished: dialog.estimate()
                        Accessible.name: qsTr("Script")
                    }
                }
            }

            Label { text: qsTr("Voice provider") }
            Flow {
                objectName: "dubModel"
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: dialog.models
                    delegate: Rectangle {
                        id: modelChip
                        required property var modelData
                        required property int index
                        readonly property bool on: index === dialog.modelIndex
                        width: modelText.implicitWidth + Theme.space(4)
                        height: Theme.space(7)
                        radius: height / 2
                        color: on ? Theme.accentWash : Theme.raised
                        border.width: Theme.hairline
                        border.color: on ? Theme.accentStroke : Theme.border
                        opacity: modelData.usable === false ? 0.6 : 1
                        Text {
                            id: modelText
                            anchors.centerIn: parent
                            text: modelChip.modelData.name
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { dialog.modelIndex = modelChip.index; dialog.voice = ""; dialog.estimate() }
                        }
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.name
                        Accessible.checked: on
                    }
                }
            }
            Note {
                text: dialog.model ? dialog.model.providerName + " · " + dialog.model.price
                                     + (dialog.model.usable === false ? " · " + (dialog.model.reason ?? qsTr("not set up")) : "") : ""
            }
            Label { text: qsTr("Voice") }
            Flow {
                objectName: "dubVoices"
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: dialog.voices
                    delegate: Rectangle {
                        id: voiceChip
                        required property var modelData
                        required property int index
                        readonly property bool on: dialog.voice === modelData.id || (dialog.voice === "" && index === 0)
                        width: voiceText.implicitWidth + Theme.space(4)
                        height: Theme.space(7)
                        radius: height / 2
                        color: on ? Theme.accentWash : Theme.raised
                        border.width: Theme.hairline
                        border.color: on ? Theme.accentStroke : Theme.border
                        Text {
                            id: voiceText
                            anchors.centerIn: parent
                            text: voiceChip.modelData.name + (voiceChip.modelData.description ? " · " + voiceChip.modelData.description : "")
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dialog.voice = voiceChip.modelData.id
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                FieldInput {
                    id: voiceIdField
                    objectName: "dubVoiceId"
                    Layout.fillWidth: true
                    visible: dialog.model !== null && dialog.model.provider === "elevenlabs"
                    placeholderText: qsTr("Or a voice id from your ElevenLabs library")
                    onEditingFinished: if (text.trim() !== "") dialog.voice = text.trim()
                }
                PillButton {
                    objectName: "dubPreviewVoice"
                    compact: true
                    iconName: "volume-2"
                    text: qsTr("Hear it")
                    tooltip: qsTr("Speaks one short line with this voice (a few cents at most)")
                    onClicked: Dub.previewVoice(dialog.options(false))
                }
            }
            FieldInput {
                id: styleField
                objectName: "dubStyle"
                Layout.fillWidth: true
                placeholderText: qsTr("Delivery: warm, unhurried…")
            }

            Toggle {
                id: cloneToggle
                objectName: "dubClone"
                Layout.fillWidth: true
                visible: !dialog.narration && dialog.canClone
                text: qsTr("Clone the speaker's own voice")
                onToggled: dialog.estimate()
            }
            PillTabs {
                objectName: "dubCloneSpeaker"
                Layout.fillWidth: true
                compact: true
                visible: cloneToggle.checked && cloneToggle.visible && dialog.speakers.length > 0
                model: dialog.speakers.map(s => s.name)
                currentIndex: dialog.speakerIndex
                onActivated: index => dialog.speakerIndex = index
            }
            Toggle {
                id: rightsToggle
                objectName: "dubRights"
                Layout.fillWidth: true
                visible: cloneToggle.checked && cloneToggle.visible
                text: qsTr("I have the speaker's permission to clone their voice")
            }
            Note {
                visible: cloneToggle.checked && cloneToggle.visible
                text: qsTr("A short recording of their speech is uploaded to ElevenLabs for this run and the cloned voice is deleted afterwards. Speakers with redacted speech are never cloned.")
            }
        }

        // ---- Timing, cost and the report ----
        ColumnLayout {
            Layout.preferredWidth: Theme.space(95)
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(1.5)

            Label { text: qsTr("Fit to the timing") }
            RowLayout {
                Layout.fillWidth: true
                Text { text: qsTr("Speed up at most"); color: Theme.textTertiary; font.family: Theme.fontSans; font.pixelSize: Theme.textSm }
                Item { Layout.fillWidth: true }
                Text { text: Math.round(stretchSlider.value) + " %"; color: Theme.textTertiary; font.family: Theme.fontMono; font.pixelSize: Theme.textSm }
            }
            ValueSlider {
                id: stretchSlider
                objectName: "dubStretch"
                Layout.fillWidth: true
                from: 0
                to: 25
                stepSize: 1
                value: 15
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: dialog.narration ? qsTr("Duck other audio") : qsTr("Duck the original dialogue"); color: Theme.textTertiary; font.family: Theme.fontSans; font.pixelSize: Theme.textSm }
                Item { Layout.fillWidth: true }
                Text { text: Math.round(duckSlider.value) + " dB"; color: Theme.textTertiary; font.family: Theme.fontMono; font.pixelSize: Theme.textSm }
            }
            ValueSlider {
                id: duckSlider
                objectName: "dubDuck"
                Layout.fillWidth: true
                from: -40
                to: 0
                stepSize: 1
                value: dialog.narration ? -12 : -18
            }

            Label { text: qsTr("Cost") }
            Note {
                objectName: "dubEstimate"
                text: dialog.plan.lines !== undefined
                      ? qsTr("%n line(s)", "", dialog.plan.lines) + " · " + qsTr("%n character(s)", "", dialog.plan.characters)
                        + " · " + qsTr("about $%1 (at most $%2)").arg(Number(dialog.plan.estimate.usd).toFixed(3)).arg(Number(dialog.plan.estimate.ceilingUsd).toFixed(3))
                      : qsTr("Estimating…")
                color: Theme.textSecondary
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: qsTr("Stop if it would cost more than"); color: Theme.textTertiary; font.family: Theme.fontSans; font.pixelSize: Theme.textSm }
                Item { Layout.fillWidth: true }
                NumberField {
                    id: capField
                    objectName: "dubCap"
                    Layout.preferredWidth: Theme.space(22)
                    from: 0.01
                    to: 100
                    stepSize: 0.05
                    decimals: 2
                    suffix: " $"
                    value: Math.max(0.05, Math.ceil(((dialog.plan.estimate ? dialog.plan.estimate.ceilingUsd : 0) * 1.25) * 100) / 100)
                    onValueModified: v => value = v
                }
            }

            Label { text: qsTr("Timing") }
            Note {
                objectName: "dubFit"
                visible: dialog.fit !== null
                text: dialog.fit ? qsTr("%1 as spoken · %2 sped up (at most ×%3) · %4 moved · %5 flagged")
                                   .arg(dialog.fit.fitted).arg(dialog.fit.stretched).arg(Number(dialog.fit.maxSpeed).toFixed(2))
                                   .arg(dialog.fit.shifted).arg(dialog.fit.flagged) : ""
                color: dialog.fit && dialog.fit.flagged > 0 ? Theme.warning : Theme.textSecondary
            }
            ListView {
                objectName: "dubReport"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(60)
                clip: true
                spacing: Theme.space(0.5)
                model: dialog.report.placed ?? []
                delegate: RowLayout {
                    id: row
                    required property var modelData
                    width: ListView.view.width
                    spacing: Theme.space(1)
                    Rectangle {
                        Layout.preferredWidth: Theme.space(1.5)
                        Layout.preferredHeight: Theme.space(1.5)
                        radius: width / 2
                        color: row.modelData.flagged ? Theme.warning : row.modelData.stretched || row.modelData.shifted ? Theme.accentFill : Theme.success
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Line %1 · %2 s").arg(row.modelData.line).arg(Number(row.modelData.seconds).toFixed(2))
                              + (row.modelData.stretched ? " · ×" + Number(row.modelData.speed).toFixed(2) : "")
                              + (row.modelData.note ? " · " + row.modelData.note : "")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        elide: Text.ElideRight
                    }
                }
            }
            Note {
                visible: dialog.fit !== null
                text: qsTr("The speech goes on a new “%1” track (Dialogue). It is marked AI-generated on the timeline and in exports.").arg(dialog.plan.track ?? "")
            }
            Note {
                objectName: "dubMessage"
                visible: Dub.message !== ""
                text: Dub.message
                color: Theme.warning
            }
            Note {
                visible: !Dub.cloudAvailable
                text: qsTr("Voice-over needs your cloud voice provider: this project is no-cloud or Wabcut is offline.")
                color: Theme.warning
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: Dub.busy
        spacing: Theme.space(2)
        StatusPill {
            label: Dub.phase
            detail: Dub.progress >= 0 ? Math.round(Dub.progress * 100) + "%" : ""
            dotColor: Theme.accentFill
        }
        BeamProgress {
            Layout.fillWidth: true
            value: Dub.progress >= 0 ? Dub.progress : 1
            active: Dub.busy
            label: Dub.phase
        }
    }

    AiConsentDialog {
        id: consent
        onAllowed: always => {
            if (always) {
                AiController.grantConsent(consent.prompt.provider, consent.prompt.capability)
                if (consent.prompt.capability === "cloneVoice") // the prompt named the text to speak too
                    AiController.grantConsent(consent.prompt.provider, "speak")
            }
            Dub.run(dialog.options(!always))
        }
    }

    footerData: [
        PillButton {
            text: Dub.busy ? qsTr("Stop") : qsTr("Cancel")
            onClicked: Dub.busy ? Dub.cancel() : dialog.close()
        },
        PillButton {
            objectName: "dubGenerateButton"
            iconName: "audio-lines"
            text: qsTr("Generate")
            enabled: !Dub.busy && Dub.cloudAvailable && dialog.plan.lines !== undefined
                     && (!cloneToggle.checked || !cloneToggle.visible || rightsToggle.checked)
            onClicked: dialog.generate()
        },
        PillButton {
            objectName: "dubApplyButton"
            variant: "primary"
            iconName: "plus"
            text: qsTr("Add to timeline")
            enabled: !Dub.busy && Dub.staged
            onClicked: {
                if (Dub.apply() === "")
                    dialog.close()
            }
        }
    ]
}
