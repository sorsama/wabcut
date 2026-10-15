pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// ● ESSENTIAL SOUND (M5m, rail key "essential"; bridge `EssentialSound`; Premiere's Essential Sound panel, A.7):
//   * tag the selected audio clips Dialogue, Music, SFX or Ambience (a video clip stands for its linked audio), or
//     clear them; pick a preset of the type;
//   * the type's sections, every control an option of audio.essentialSound and every result an ordinary effect the
//     user can edit in Effect Controls (nothing hidden): Loudness (Auto-Match to a target), Repair, Clarity, Creative
//     (dialogue), Duration (Remix), Ducking and Width (music, ambience), Position (sound effects);
//   * a section's switch bypasses its effects; a slider applies once, when it is let go (one undo step).
// Remix… previews the music remixed to the length (joins drawn on the timeline) before Apply.
Panel {
    id: root
    objectName: "essentialSoundPanel"

    readonly property string type: EssentialSound.type
    readonly property var options: EssentialSound.options
    readonly property var sections: EssentialSound.sections
    readonly property var remix: EssentialSound.remix
    readonly property bool typed: type === "dialogue" || type === "music" || type === "sfx" || type === "ambience"
    property real remixSeconds: 0

    function focusContent() {
        if (typeRepeater.count > 0)
            (typeRepeater.itemAt(0) as Item).forceActiveFocus(Qt.TabFocusReason)
    }
    /// An option by path ("repair.noise").
    function optionAt(path) {
        let v = root.options
        for (const part of path.split("."))
            v = v === undefined || v === null ? undefined : v[part]
        return v
    }
    function sectionState(key) {
        return root.sections[key] ?? ""
    }
    /// The switch of a section: bypass what is built, or build the section with a starting setting.
    function switchSection(key, on, start) {
        if (sectionState(key) !== "")
            EssentialSound.setSectionEnabled(key, on)
        else if (on)
            start()
    }
    function percentText(v) {
        return Math.round(v * 100) + " %"
    }

    title: qsTr("Essential Sound")

    component FieldLabel: Text {
        Layout.fillWidth: true
        elide: Text.ElideRight
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    // A slider over an option: shown = option / factor; applied when the drag ends (one undo step).
    component OptionSlider: PropSlider {
        id: optionSlider
        required property string path
        property real factor: 1
        property real fallback: 0
        Layout.fillWidth: true
        value: (root.optionAt(path) ?? fallback) / factor
        onValueModified: v => EssentialSound.previewOption(optionSlider.path, v * optionSlider.factor)
        onGestureEnded: EssentialSound.commitOptions()
    }
    // A menu of choices ([{ id, name }]) behind a pill.
    component ChoiceRow: RowLayout {
        id: choice
        required property string label
        required property var choices
        required property string current
        signal chosen(string id)
        Layout.fillWidth: true
        spacing: Theme.space(2)
        FieldLabel {
            text: choice.label
        }
        PillButton {
            id: choiceButton
            compact: true
            iconName: "chevron-down"
            text: (choice.choices.find(c => c.id === choice.current) ?? { name: "" }).name
            tooltip: choice.label
            onClicked: choiceMenu.popup(choiceButton, 0, choiceButton.height + Theme.space(1))
            OptionsMenu {
                id: choiceMenu
                options: choice.choices
                current: choice.current
                onChosen: id => choice.chosen(id)
            }
        }
    }
    // A labelled switch over a boolean.
    component SwitchRow: RowLayout {
        id: switchRow
        required property string label
        required property bool on
        signal flipped(bool on)
        Layout.fillWidth: true
        spacing: Theme.space(2)
        FieldLabel {
            text: switchRow.label
        }
        Toggle {
            tooltip: switchRow.label
            checked: switchRow.on
            onToggled: {
                switchRow.flipped(checked)
                checked = Qt.binding(() => switchRow.on)
            }
            Accessible.name: switchRow.label
        }
    }

    Flickable {
        id: flick
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

            // ---- Nothing selected ----
            EmptyState {
                objectName: "essentialEmpty"
                Layout.fillWidth: true
                Layout.topMargin: Theme.space(8)
                visible: EssentialSound.clips.length === 0
                illustration: "effects"
                text: qsTr("Select audio clips to tag them as dialogue, music, sound effects or ambience.")
            }

            // ---- The selection and its type ----
            Text {
                Layout.fillWidth: true
                visible: EssentialSound.clips.length > 0
                text: qsTr("%n audio clip(s) selected", "", EssentialSound.clips.length)
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.labelTracking
            }
            Flow {
                Layout.fillWidth: true
                visible: EssentialSound.clips.length > 0
                spacing: Theme.space(1.5)
                Repeater {
                    id: typeRepeater
                    model: [
                        { id: "dialogue", name: qsTr("Dialogue"), icon: "audio-lines" },
                        { id: "music", name: qsTr("Music"), icon: "music" },
                        { id: "sfx", name: qsTr("SFX"), icon: "zap" },
                        { id: "ambience", name: qsTr("Ambience"), icon: "blend" }
                    ]
                    delegate: PillButton {
                        required property var modelData
                        objectName: "essentialType_" + modelData.id
                        compact: true
                        variant: root.type === modelData.id ? "primary" : "secondary"
                        iconName: modelData.icon
                        text: modelData.name
                        enabled: !EssentialSound.busy
                        tooltip: qsTr("Tag the selection as %1").arg(modelData.name.toLowerCase())
                        onClicked: EssentialSound.setType(modelData.id)
                    }
                }
                PillButton {
                    objectName: "essentialClear"
                    visible: root.typed || root.type === "mixed"
                    compact: true
                    variant: "ghost"
                    iconName: "x"
                    text: qsTr("Clear")
                    enabled: !EssentialSound.busy
                    tooltip: qsTr("Remove Essential Sound and the effects it added")
                    onClicked: EssentialSound.clear()
                }
            }
            Text {
                Layout.fillWidth: true
                visible: root.type === "mixed"
                wrapMode: Text.Wrap
                text: qsTr("The selected clips have different types. Tag them again to give them one type.")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Text {
                Layout.fillWidth: true
                visible: EssentialSound.clips.length > 0 && root.type === ""
                wrapMode: Text.Wrap
                text: qsTr("Pick a type: its default preset adds ordinary effects you can change here or in Effect Controls.")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            // ---- Working ----
            BeamProgress {
                objectName: "essentialProgress"
                Layout.fillWidth: true
                loader: "equalizer"
                visible: EssentialSound.busy && !(root.remix.busy ?? false)
                active: visible
                indeterminate: EssentialSound.progress < 0
                value: Math.max(0, EssentialSound.progress)
                label: EssentialSound.phase
            }
            Text {
                objectName: "essentialMessage"
                Layout.fillWidth: true
                visible: text !== "" && !EssentialSound.busy
                text: EssentialSound.message
                wrapMode: Text.Wrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }

            // ---- Preset ----
            ChoiceRow {
                objectName: "essentialPreset"
                visible: root.typed
                label: qsTr("Preset")
                choices: EssentialSound.presets
                current: EssentialSound.preset
                onChosen: id => EssentialSound.applyPreset(id)
            }

            // ---- Loudness (every type) ----
            EssentialSection {
                objectName: "essentialLoudness"
                visible: root.typed
                title: qsTr("Loudness")
                hint: qsTr("Auto-match the clip to a loudness target")
                sectionState: root.optionAt("loudness") === true ? (root.sectionState("loudness") === "off" ? "off" : "on") : ""
                onSwitched: on => root.sectionState("loudness") === "off" ? EssentialSound.setSectionEnabled("loudness", on)
                                                                           : EssentialSound.setOption("loudness", on)
                OptionSlider {
                    objectName: "essentialTargetLufs"
                    path: "targetLufs"
                    label: qsTr("Target")
                    unit: qsTr("LUFS")
                    from: -40
                    to: -10
                    decimals: 1
                    stepSize: 0.5
                    defaultValue: root.type === "dialogue" ? -23 : root.type === "ambience" ? -30 : -25
                    fallback: defaultValue
                }
            }

            // ---- Dialogue: Repair, Clarity ----
            EssentialSection {
                objectName: "essentialRepair"
                visible: root.type === "dialogue"
                title: qsTr("Repair")
                hint: qsTr("Noise, rumble, hum, sibilance and room echo")
                sectionState: root.sectionState("repair")
                onSwitched: on => root.switchSection("repair", on, () => EssentialSound.setOption("repair.noise", 0.3))
                expanded: sectionState !== "off"
                OptionSlider {
                    objectName: "essentialNoise"
                    path: "repair.noise"
                    label: qsTr("Reduce noise")
                    unit: "%"
                    factor: 0.01
                    from: 0
                    to: 100
                }
                SwitchRow {
                    objectName: "essentialEnhance"
                    label: qsTr("Enhance Speech (rendered on this computer)")
                    on: root.optionAt("repair.enhance") === true
                    onFlipped: on => EssentialSound.setOption("repair.enhance", on)
                }
                SwitchRow {
                    objectName: "essentialRumbleSwitch"
                    label: qsTr("Reduce rumble")
                    on: (root.optionAt("repair.rumble") ?? 0) > 0
                    onFlipped: on => EssentialSound.setOption("repair.rumble", on ? 80 : 0)
                }
                OptionSlider {
                    objectName: "essentialRumble"
                    visible: (root.optionAt("repair.rumble") ?? 0) > 0
                    path: "repair.rumble"
                    label: qsTr("Below")
                    unit: qsTr("Hz")
                    from: 20
                    to: 300
                    defaultValue: 80
                }
                ChoiceRow {
                    objectName: "essentialHum"
                    label: qsTr("Remove hum")
                    choices: [{ id: "0", name: qsTr("Off") }, { id: "50", name: qsTr("50 Hz") }, { id: "60", name: qsTr("60 Hz") }]
                    current: String(Math.round(root.optionAt("repair.hum") ?? 0))
                    onChosen: id => EssentialSound.setOption("repair.hum", Number(id))
                }
                OptionSlider {
                    objectName: "essentialDeess"
                    path: "repair.deess"
                    label: qsTr("De-ess")
                    unit: "%"
                    factor: 0.01
                    from: 0
                    to: 100
                }
                OptionSlider {
                    objectName: "essentialReverb"
                    path: "repair.reverb"
                    label: qsTr("Reduce reverb")
                    unit: "%"
                    factor: 0.01
                    from: 0
                    to: 100
                }
            }
            EssentialSection {
                objectName: "essentialClarity"
                visible: root.type === "dialogue"
                title: qsTr("Clarity")
                hint: qsTr("Dynamics and an EQ shape for the voice")
                sectionState: root.sectionState("clarity")
                onSwitched: on => root.switchSection("clarity", on, () => EssentialSound.setOption("clarity.dynamics", 0.5))
                expanded: sectionState !== "off"
                OptionSlider {
                    objectName: "essentialDynamics"
                    path: "clarity.dynamics"
                    label: qsTr("Dynamics")
                    unit: "%"
                    factor: 0.01
                    from: 0
                    to: 100
                }
                ChoiceRow {
                    objectName: "essentialEq"
                    label: qsTr("EQ")
                    choices: EssentialSound.eqPresets
                    current: root.optionAt("clarity.eq") ?? ""
                    onChosen: id => EssentialSound.setOption("clarity.eq", id)
                }
                OptionSlider {
                    objectName: "essentialEqAmount"
                    visible: (root.optionAt("clarity.eq") ?? "") !== ""
                    path: "clarity.eqAmount"
                    label: qsTr("Amount")
                    unit: "%"
                    factor: 0.01
                    from: 0
                    to: 100
                    defaultValue: 100
                    fallback: 1
                }
            }

            // ---- Music: Duration (Remix) ----
            EssentialSection {
                objectName: "essentialDuration"
                visible: root.type === "music"
                title: qsTr("Duration")
                hint: qsTr("Remix the music to a length: its own bars rearranged, never stretched")
                switchable: false
                expanded: true
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    FieldLabel {
                        text: qsTr("Target length")
                    }
                    NumberField {
                        objectName: "essentialRemixSeconds"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 1
                        to: 36000
                        decimals: 1
                        stepSize: 1
                        suffix: qsTr("s")
                        value: root.remixSeconds > 0 ? root.remixSeconds : Math.round(EssentialSound.clipSeconds)
                        onValueModified: v => root.remixSeconds = v
                        Accessible.name: qsTr("Target length in seconds")
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(1.5)
                    PillButton {
                        objectName: "essentialRemix"
                        compact: true
                        iconName: "repeat"
                        text: qsTr("Remix…")
                        enabled: !EssentialSound.busy
                        tooltip: qsTr("Preview the remix: its joins show on the timeline")
                        onClicked: EssentialSound.previewSelected(root.remixSeconds > 0 ? root.remixSeconds
                                                                                      : Math.round(EssentialSound.clipSeconds))
                    }
                    PillButton {
                        objectName: "essentialRemixFit"
                        compact: true
                        variant: "ghost"
                        text: qsTr("Fit to video")
                        enabled: !EssentialSound.busy && EssentialSound.clips.length > 0
                        tooltip: qsTr("End the music with the last video clip")
                        onClicked: EssentialSound.previewFit(EssentialSound.clips[0])
                    }
                    Item { Layout.fillWidth: true }
                }
                // The preview, when it came from here (the timeline's Remix tool shows it in its own popover too).
                RemixSummary {
                    objectName: "essentialRemixSummary"
                    Layout.fillWidth: true
                    visible: (root.remix.clip ?? "") !== ""
                }
            }

            // ---- Sound effects: Position ----
            EssentialSection {
                objectName: "essentialPosition"
                visible: root.type === "sfx"
                title: qsTr("Position")
                hint: qsTr("Where the effect sits, left to right (the clip's panner)")
                switchable: false
                expanded: true
                OptionSlider {
                    objectName: "essentialPan"
                    path: "pan"
                    label: qsTr("Pan")
                    unit: "%"
                    factor: 0.01
                    from: -100
                    to: 100
                    origin: 0
                }
            }

            // ---- Music and ambience: Ducking, Width ----
            EssentialSection {
                objectName: "essentialDucking"
                visible: root.type === "music" || root.type === "ambience"
                title: qsTr("Ducking")
                hint: qsTr("Dip under the dialogue while people speak")
                sectionState: root.optionAt("ducking.enabled") === true ? "on" : ""
                onSwitched: on => EssentialSound.setOption("ducking.enabled", on)
                OptionSlider {
                    objectName: "essentialDuckAmount"
                    path: "ducking.amount"
                    label: qsTr("Amount")
                    unit: qsTr("dB")
                    from: -40
                    to: 0
                    decimals: 1
                    stepSize: 0.5
                    defaultValue: -12
                    fallback: -12
                }
                OptionSlider {
                    objectName: "essentialDuckAttack"
                    path: "ducking.attack"
                    label: qsTr("Fade down")
                    unit: qsTr("s")
                    from: 0
                    to: 3
                    decimals: 2
                    stepSize: 0.05
                    defaultValue: 0.25
                    fallback: 0.25
                }
                OptionSlider {
                    objectName: "essentialDuckRelease"
                    path: "ducking.release"
                    label: qsTr("Fade up")
                    unit: qsTr("s")
                    from: 0
                    to: 3
                    decimals: 2
                    stepSize: 0.05
                    defaultValue: 0.75
                    fallback: 0.75
                }
            }
            EssentialSection {
                objectName: "essentialWidth"
                visible: root.type === "music" || root.type === "ambience"
                title: qsTr("Stereo width")
                hint: qsTr("Narrower or wider; the mono fold-down never changes")
                sectionState: root.sectionState("width")
                onSwitched: on => root.switchSection("width", on, () => EssentialSound.setOption("width", 1.4))
                expanded: sectionState !== "off"
                OptionSlider {
                    objectName: "essentialWidthAmount"
                    path: "width"
                    label: qsTr("Width")
                    unit: "%"
                    factor: 0.01
                    from: 0
                    to: 200
                    defaultValue: 100
                    fallback: 1
                }
            }

            // ---- Creative (dialogue, sound effects, ambience) ----
            EssentialSection {
                objectName: "essentialCreative"
                visible: root.type === "dialogue" || root.type === "sfx" || root.type === "ambience"
                title: qsTr("Creative")
                hint: qsTr("Place it in a room: a reverb preset")
                sectionState: root.sectionState("creative")
                onSwitched: on => root.switchSection("creative", on, () => EssentialSound.setOption("creative.reverb", "smallRoom"))
                expanded: sectionState !== "off"
                ChoiceRow {
                    objectName: "essentialReverbPreset"
                    label: qsTr("Reverb")
                    choices: EssentialSound.reverbPresets
                    current: root.optionAt("creative.reverb") ?? ""
                    onChosen: id => EssentialSound.setOption("creative.reverb", id)
                }
                OptionSlider {
                    objectName: "essentialReverbAmount"
                    visible: (root.optionAt("creative.reverb") ?? "") !== ""
                    path: "creative.amount"
                    label: qsTr("Amount")
                    unit: "%"
                    factor: 0.01
                    from: 0
                    to: 100
                    defaultValue: 30
                    // -1: the preset's own mix, shown as its usual 30 %.
                    value: (root.optionAt("creative.amount") ?? -1) < 0 ? 30 : root.optionAt("creative.amount") * 100
                }
            }
            Item { Layout.preferredHeight: Theme.space(2) }
        }
    }
}
