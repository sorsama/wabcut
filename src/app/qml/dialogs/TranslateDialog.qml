pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Captions > Translate captions… (AI-12): one new caption track per language, cue timing kept and re-cut to each
// language's reading speed (Thai broken between words). Pick the languages as chips; each says whether it translates on
// this computer (an OPUS-MT model, downloaded once) or needs the user's cloud AI. Corrections apply first and bleeped
// words stay "[bleep]": they are never sent. Translate stages the tracks with a report per language (cues re-timed,
// split or merged, still too fast); Add tracks applies them as one undo step. The cloud is never used for a no-cloud
// project, and only after the consent prompt.
WabcutDialog {
    id: dialog
    objectName: "translateDialog"

    property var selected: []
    property int trackIndex: 0
    readonly property var tracks: TranslateCaptions.captionTracks
    readonly property var track: tracks.length > 0 ? tracks[Math.min(trackIndex, tracks.length - 1)] : null
    readonly property string sourceLanguage: track && track.language ? track.language : "en"
    readonly property var chips: TranslateCaptions.languages(sourceLanguage)
    readonly property var report: TranslateCaptions.report
    readonly property var results: report.languages ?? []
    readonly property var providers: ["auto", "local", "cloud"]
    readonly property var tones: ["neutral", "casual", "formal", "playful"]
    // Thai first-class: offered first, with the languages creators publish to most.
    readonly property var featured: ["th", "en", "ja", "zh", "es", "ko", "vi", "id", "fr", "de"]

    function chipFor(code) {
        return chips.find(c => c.code === code)
    }
    function toggle(code) {
        const at = selected.indexOf(code)
        const next = selected.slice()
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(code)
        selected = next
    }
    function options(allowOnce) {
        return { captionTrack: track ? track.id : 0, languages: selected, provider: providers[providerTabs.currentIndex],
                 glossary: glossaryArea.text, tone: tones[toneTabs.currentIndex], notes: notesField.text, allowOnce: allowOnce }
    }
    /// Languages no local model translates (they need the cloud model under "Auto").
    function cloudNeeded() {
        if (providerTabs.currentIndex === 1)
            return false
        if (providerTabs.currentIndex === 2)
            return true
        return selected.some(code => { const c = chipFor(code); return !c || !c.local || !c.installed })
    }
    function translate() {
        if (cloudNeeded()) {
            const prompt = TranslateCaptions.consentPrompt(track ? track.cues : 0)
            if (prompt.needed) {
                consent.ask(prompt)
                return
            }
        }
        TranslateCaptions.preview(options(false))
    }
    function sortedChips() {
        const head = featured.map(code => chipFor(code)).filter(c => c !== undefined)
        return head.concat(chips.filter(c => featured.indexOf(c.code) < 0))
    }

    title: qsTr("Translate captions")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(230), Theme.space(230))
    onClosed: TranslateCaptions.discard()

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

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(4)

        // ---- What to translate, into what, by whom ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(2)
            enabled: !TranslateCaptions.busy

            Label { text: qsTr("Captions") }
            PillTabs {
                objectName: "translateTrack"
                Layout.fillWidth: true
                compact: true
                visible: dialog.tracks.length > 1
                model: dialog.tracks.map(t => t.name)
                currentIndex: dialog.trackIndex
                onActivated: index => dialog.trackIndex = index
            }
            Note {
                visible: dialog.tracks.length === 0
                text: qsTr("This sequence has no captions yet: generate them from the transcript first.")
                color: Theme.warning
            }
            Note {
                visible: dialog.track !== null
                text: dialog.track ? qsTr("“%1” · %n cue(s) · %2", "", dialog.track.cues).arg(dialog.track.name).arg(dialog.sourceLanguage) : ""
            }

            Label { text: qsTr("Into") }
            Flow {
                objectName: "translateLanguages"
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: dialog.sortedChips()
                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        readonly property bool on: dialog.selected.indexOf(modelData.code) >= 0
                        objectName: "translateChip_" + modelData.code
                        width: chipRow.implicitWidth + Theme.space(3)
                        height: Theme.space(7)
                        radius: height / 2
                        color: on ? Theme.accentWash : Theme.raised
                        border.width: Theme.hairline
                        border.color: on ? Theme.accentStroke : Theme.border
                        RowLayout {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: Theme.space(1)
                            Text {
                                text: chip.modelData.nativeName
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                            }
                            Text {
                                text: chip.modelData.local ? (chip.modelData.installed ? qsTr("local") : qsTr("↓ %1 MB").arg(chip.modelData.downloadMB))
                                                           : qsTr("cloud")
                                color: chip.modelData.local ? Theme.success : Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dialog.toggle(chip.modelData.code)
                        }
                        Accessible.role: Accessible.CheckBox
                        Accessible.name: modelData.name
                        Accessible.checked: on
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: dialog.selected.some(code => { const c = dialog.chipFor(code); return c && c.local && !c.installed })
                Note { text: qsTr("Translating on this computer needs a one-time model download (OPUS-MT, Apache-2.0 / CC-BY-4.0).") }
                PillButton {
                    objectName: "translateDownload"
                    compact: true
                    iconName: "download"
                    text: qsTr("Download")
                    onClicked: {
                        for (const code of dialog.selected) {
                            const c = dialog.chipFor(code)
                            if (c && c.local && !c.installed) {
                                TranslateCaptions.download(dialog.sourceLanguage, code)
                                break
                            }
                        }
                    }
                }
            }

            Label { text: qsTr("Translated by") }
            PillTabs {
                id: providerTabs
                objectName: "translateProvider"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Auto"), qsTr("This computer"),
                        TranslateCaptions.cloudAvailable ? TranslateCaptions.cloudModelName : qsTr("Your AI (not set up)")]
            }
            Note {
                text: TranslateCaptions.cloudAvailable
                      ? qsTr("Auto uses this computer where a model is installed, else your AI: only the caption text is sent, bleeped words as [bleep].")
                      : qsTr("Everything stays on this computer (no cloud AI is set up, or the project is no-cloud).")
            }
            Label { text: qsTr("Tone") }
            PillTabs {
                id: toneTabs
                objectName: "translateTone"
                Layout.fillWidth: true
                compact: true
                enabled: providerTabs.currentIndex !== 1
                model: [qsTr("Neutral"), qsTr("Casual"), qsTr("Formal"), qsTr("Playful")]
            }
            Label { text: qsTr("Glossary") }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(22)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: glossaryArea.activeFocus ? Theme.focus : Theme.border
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    TextArea {
                        id: glossaryArea
                        objectName: "translateGlossary"
                        wrapMode: TextEdit.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textSm
                        selectByMouse: true
                        placeholderText: qsTr("Wabcut = Wabcut\nfloating market = ตลาดน้ำ")
                        placeholderTextColor: Theme.textTertiary
                        background: null
                        Accessible.name: qsTr("Glossary")
                    }
                }
            }
            FieldInput {
                id: notesField
                objectName: "translateNotes"
                Layout.fillWidth: true
                placeholderText: qsTr("Notes for your AI: who speaks, the audience…")
            }
        }

        // ---- The report ----
        ColumnLayout {
            Layout.preferredWidth: Theme.space(95)
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(1.5)

            Label { text: qsTr("Result") }
            Note {
                visible: dialog.results.length === 0 && !TranslateCaptions.busy && !(dialog.report.estimate)
                text: qsTr("Choose languages and Translate: the new tracks are previewed here before they are added.")
            }
            Note {
                objectName: "translateEstimate"
                visible: dialog.report.estimate !== undefined
                text: dialog.report.estimate ? qsTr("Your AI would cost about $%1 (at most $%2).").arg(Number(dialog.report.estimate.usd).toFixed(4))
                                                   .arg(Number(dialog.report.estimate.ceilingUsd).toFixed(4)) : ""
            }
            Note {
                visible: dialog.results.length > 0
                text: qsTr("%n correction(s) applied first", "", dialog.report.corrected ?? 0) + " · "
                      + qsTr("%n bleeped word(s) kept as [bleep]", "", dialog.report.redacted ?? 0)
            }
            ListView {
                objectName: "translateReport"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(110)
                clip: true
                spacing: Theme.space(1)
                model: dialog.results
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    width: ListView.view.width
                    height: cardColumn.implicitHeight + Theme.space(3)
                    radius: Theme.radiusField
                    color: Theme.raised
                    border.width: Theme.hairline
                    border.color: (modelData.tooFast ?? 0) > 0 ? Theme.warning : Theme.border
                    ColumnLayout {
                        id: cardColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.space(1.5)
                        spacing: Theme.space(0.5)
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: card.modelData.name
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                elide: Text.ElideRight
                            }
                            StatusPill {
                                label: card.modelData.provider === "local" ? qsTr("This computer") : card.modelData.model
                                dotColor: card.modelData.provider === "local" ? Theme.success : Theme.accentFill
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%n cue(s)", "", card.modelData.cues) + " · " + qsTr("%1 re-timed, %2 merged, %3 split")
                                  .arg((card.modelData.extended ?? 0) + (card.modelData.rebalanced ?? 0)).arg(card.modelData.merged ?? 0)
                                  .arg(card.modelData.split ?? 0) + " · " + qsTr("≤ %1 chars/s").arg(card.modelData.readingSpeed)
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            wrapMode: Text.Wrap
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: (card.modelData.tooFast ?? 0) > 0 || (card.modelData.untranslated ?? 0) > 0
                            text: qsTr("%n cue(s) still read too fast", "", card.modelData.tooFast ?? 0)
                                  + ((card.modelData.untranslated ?? 0) > 0 ? " · " + qsTr("%n not translated", "", card.modelData.untranslated) : "")
                            color: Theme.warning
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            wrapMode: Text.Wrap
                        }
                        Repeater {
                            model: (card.modelData.warnings ?? []).slice(0, 3)
                            delegate: Text {
                                required property var modelData
                                Layout.fillWidth: true
                                text: modelData
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
            Note {
                objectName: "translateMessage"
                visible: TranslateCaptions.message !== ""
                text: TranslateCaptions.message
                color: Theme.warning
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: TranslateCaptions.busy
        spacing: Theme.space(2)
        StatusPill {
            label: TranslateCaptions.phase
            detail: TranslateCaptions.progress >= 0 ? Math.round(TranslateCaptions.progress * 100) + "%" : ""
            dotColor: Theme.accentFill
        }
        BeamProgress {
            Layout.fillWidth: true
            value: TranslateCaptions.progress >= 0 ? TranslateCaptions.progress : 1
            active: TranslateCaptions.busy
            label: TranslateCaptions.phase
        }
    }

    AiConsentDialog {
        id: consent
        onAllowed: always => {
            if (always)
                AiController.grantConsent(consent.prompt.provider, "chat")
            TranslateCaptions.preview(dialog.options(!always))
        }
    }

    footerData: [
        PillButton {
            text: TranslateCaptions.busy ? qsTr("Stop") : qsTr("Cancel")
            onClicked: TranslateCaptions.busy ? TranslateCaptions.cancel() : dialog.close()
        },
        PillButton {
            objectName: "translateEstimateButton"
            text: qsTr("Estimate")
            visible: TranslateCaptions.cloudAvailable
            enabled: !TranslateCaptions.busy && dialog.selected.length > 0
            onClicked: TranslateCaptions.estimate(dialog.options(false))
        },
        PillButton {
            objectName: "translatePreviewButton"
            iconName: "type"
            text: qsTr("Translate")
            enabled: !TranslateCaptions.busy && dialog.selected.length > 0 && dialog.track !== null
            onClicked: dialog.translate()
        },
        PillButton {
            objectName: "translateApplyButton"
            variant: "primary"
            iconName: "captions"
            text: qsTr("Add tracks")
            enabled: !TranslateCaptions.busy && TranslateCaptions.staged
            onClicked: {
                if (TranslateCaptions.apply() === "")
                    dialog.close()
            }
        }
    ]
}
