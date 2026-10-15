pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Clip > Auto switch… (AI-13): cuts a multicam clip to whoever is speaking, on this computer. Preview finds who speaks
// when (the transcripts, each angle's microphone, each camera's faces) and plans the cuts with the rules on the right;
// the angles show what each camera sees and whom the analysis thinks it shows (change it when it guessed wrong); the
// cut list explains every cut, and the timeline shows them as ghosts. Switch angles adds ordinary angle cuts as one
// undo step: switch any of them by hand afterwards.
WabcutDialog {
    id: dialog
    objectName: "autoSwitchDialog"

    /// The multicam clip ("c<id>"); set by openFor() (empty: the one under the playhead).
    property string clipKey: ""
    readonly property var result: AutoSwitch.result
    readonly property var speakers: result.speakers ?? []
    readonly property var angles: result.angles ?? []
    readonly property var cuts: result.cuts ?? []
    readonly property var presetIds: ["calm", "interview", "energetic"]
    /// The user's angle assignments: {angle: choice}, choice "auto" | "wide" | a speaker key.
    property var assignments: ({})

    function openFor(key) {
        clipKey = key !== undefined && key !== "" ? key : AutoSwitch.defaultClip()
        assignments = ({})
        open()
    }
    function loadPreset(id) {
        const v = AutoSwitch.presetValues(id)
        holdSlider.value = v.minHold
        leadSlider.value = v.lead
        overlapSlider.value = v.overlapMin
        silenceSlider.value = v.silenceWide
        varietySlider.value = v.varietyAfter
        varietyLengthSlider.value = v.varietyLength
        overlapToggle.checked = v.wideOnOverlap
        laughterToggle.checked = v.wideOnLaughter
        reactionsToggle.checked = v.reactions
        openToggle.checked = v.openWide
        midWordToggle.checked = v.avoidMidWord
    }
    function options() {
        const list = []
        for (const key in assignments) {
            const choice = assignments[key]
            if (choice === "wide")
                list.push({ angle: Number(key), role: "wide" })
            else if (choice !== "auto")
                list.push({ angle: Number(key), role: "closeUp", speaker: choice })
        }
        return { preset: presetIds[presetTabs.currentIndex], minHold: holdSlider.value, lead: leadSlider.value,
                 overlapMin: overlapSlider.value, silenceWide: silenceSlider.value, varietyAfter: varietySlider.value,
                 varietyLength: varietyLengthSlider.value, wideOnOverlap: overlapToggle.checked,
                 wideOnLaughter: laughterToggle.checked, reactions: reactionsToggle.checked, openWide: openToggle.checked,
                 avoidMidWord: midWordToggle.checked, target: soundToggle.checked ? "both" : "video", faces: facesToggle.checked,
                 angles: list }
    }
    function refresh() {
        AutoSwitch.preview(clipKey, options())
    }
    function assign(angle, choice) {
        const next = Object.assign({}, assignments)
        next[angle] = choice
        assignments = next
        refresh()
    }
    function angleColor(angle) {
        const colors = Theme.clipColors
        return colors.length > 0 ? colors[(angle * 5 + 2) % colors.length] : Theme.accentFill
    }
    function clock(seconds) {
        const s = Math.max(0, seconds)
        const m = Math.floor(s / 60)
        const r = s - m * 60
        return m + ":" + (r < 10 ? "0" : "") + r.toFixed(1)
    }
    function roleText(a) {
        if (a.role === "wide")
            return qsTr("Wide shot")
        if (a.role === "closeUp")
            return (a.speakers ?? []).length > 0 ? qsTr("Close-up of %1").arg(a.speakers.join(", ")) : qsTr("Close-up")
        return qsTr("Not sure")
    }

    title: qsTr("Auto switch “%1”").arg(result.name ?? "")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(260), Theme.space(260))
    onOpened: {
        loadPreset(presetIds[presetTabs.currentIndex])
        refresh()
    }
    onClosed: AutoSwitch.discard()

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Value: Text {
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    component Rule: ColumnLayout {
        id: rule
        property alias slider: ruleSlider
        property string label: ""
        property string unit: " s"
        property alias from: ruleSlider.from
        property alias to: ruleSlider.to
        property alias stepSize: ruleSlider.stepSize
        property alias value: ruleSlider.value
        property string zeroText: ""
        Layout.fillWidth: true
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Label { text: rule.label; Layout.fillWidth: true }
            Value { text: rule.zeroText !== "" && ruleSlider.value <= 0 ? rule.zeroText : ruleSlider.value.toFixed(ruleSlider.stepSize < 0.1 ? 2 : 1) + rule.unit }
        }
        ValueSlider {
            id: ruleSlider
            Layout.fillWidth: true
            Accessible.name: rule.label
            onPressedChanged: if (!pressed) dialog.refresh()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(4)

        // ---- Who is on which camera, and the cuts ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(2)

            SectionHeader {
                Layout.fillWidth: true
                text: qsTr("Cameras and speakers")
            }
            Flow {
                id: angleFlow
                objectName: "autoSwitchAngles"
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Repeater {
                    model: dialog.angles
                    delegate: Rectangle {
                        id: angleCard
                        required property var modelData
                        // A close-up's face, cropped; the wide shot whole.
                        readonly property var face: modelData.role !== "wide" ? (modelData.face ?? null) : null
                        readonly property var choices: ["auto"].concat(dialog.speakers.map(s => s.key)).concat(["wide"])
                        objectName: "autoSwitchAngle_" + modelData.angle
                        width: Theme.space(58)
                        implicitHeight: angleColumn.implicitHeight + Theme.space(2)
                        radius: Theme.radiusCard
                        color: Theme.raised
                        border.width: modelData.confirmed ? 1.5 : Theme.hairline
                        border.color: modelData.confirmed ? Theme.accentStroke : Theme.border
                        ColumnLayout {
                            id: angleColumn
                            anchors.fill: parent
                            anchors.margins: Theme.space(1)
                            spacing: Theme.space(1)
                            // What the camera sees: the face it found, cropped from the frame (else the whole frame).
                            Rectangle {
                                id: thumb
                                Layout.fillWidth: true
                                Layout.preferredHeight: width * 9 / 16
                                radius: Theme.radiusField
                                color: Theme.sunken
                                clip: true
                                readonly property real zoom: angleCard.face ? Math.max(1, Math.min(2.5, 0.55 / Math.max(angleCard.face.w, angleCard.face.h * 9 / 16, 0.05))) : 1
                                Image {
                                    objectName: "autoSwitchThumb"
                                    source: angleCard.modelData.frame ?? ""
                                    sourceSize: Qt.size(480, 270)
                                    asynchronous: true
                                    fillMode: Image.Stretch
                                    width: thumb.width * thumb.zoom
                                    height: thumb.height * thumb.zoom
                                    x: angleCard.face ? Math.min(0, Math.max(thumb.width - width, thumb.width / 2 - (angleCard.face.x + angleCard.face.w / 2) * width)) : 0
                                    y: angleCard.face ? Math.min(0, Math.max(thumb.height - height, thumb.height / 2 - (angleCard.face.y + angleCard.face.h / 2) * height)) : 0
                                }
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    width: Theme.space(1)
                                    height: parent.height
                                    color: dialog.angleColor(angleCard.modelData.angle - 1)
                                }
                                StatusPill {
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: Theme.space(1)
                                    label: angleCard.modelData.name
                                    dotColor: dialog.angleColor(angleCard.modelData.angle - 1)
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: dialog.roleText(angleCard.modelData)
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: (angleCard.modelData.confirmed ? qsTr("Set by you") : qsTr("Guess, %1% sure").arg(Math.round((angleCard.modelData.confidence ?? 0) * 100)))
                                      + ((angleCard.modelData.reason ?? "") !== "" && !angleCard.modelData.confirmed ? " · " + angleCard.modelData.reason : "")
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            PillTabs {
                                objectName: "autoSwitchAssign_" + angleCard.modelData.angle
                                Layout.fillWidth: true
                                Accessible.name: qsTr("Who is on %1").arg(angleCard.modelData.name)
                                compact: true
                                enabled: !AutoSwitch.busy
                                model: [qsTr("Auto")].concat(dialog.speakers.map(s => s.name)).concat([qsTr("Wide")])
                                currentIndex: Math.max(0, angleCard.choices.indexOf(dialog.assignments[angleCard.modelData.angle] ?? "auto"))
                                onActivated: index => dialog.assign(angleCard.modelData.angle, angleCard.choices[index])
                            }
                        }
                    }
                }
            }
            Note {
                visible: dialog.angles.length > 0
                text: qsTr("%n speaker(s) found: ", "", dialog.speakers.length) + dialog.speakers.map(s => s.name + " " + Math.round(s.seconds) + " s").join(" · ")
            }

            // The cuts over the clip: one band per shot in its camera's colour.
            RowLayout {
                Layout.fillWidth: true
                SectionHeader {
                    Layout.fillWidth: true
                    text: qsTr("Cuts")
                }
                Text {
                    objectName: "autoSwitchSummary"
                    visible: dialog.cuts.length > 0
                    text: qsTr("%n cut(s)", "", Math.max(0, dialog.cuts.length - 1)) + " · "
                          + qsTr("average shot %1 s").arg((dialog.result.stats?.averageShot ?? 0).toFixed(1))
                          + ((dialog.result.stats?.snapped ?? 0) > 0 ? " · " + qsTr("%n moved off a word", "", dialog.result.stats.snapped) : "")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
            Rectangle {
                objectName: "autoSwitchStrip"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(7)
                radius: Theme.radiusField
                color: Theme.sunken
                visible: dialog.cuts.length > 0
                clip: true
                Repeater {
                    model: dialog.cuts
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        readonly property real duration: Math.max(0.001, dialog.result.duration ?? 1)
                        readonly property real endOffset: index + 1 < dialog.cuts.length ? dialog.cuts[index + 1].offset : duration
                        x: Math.max(0, modelData.offset) / duration * parent.width
                        width: Math.max(1, (endOffset - Math.max(0, modelData.offset)) / duration * parent.width - 1)
                        height: parent.height
                        color: Qt.alpha(dialog.angleColor(modelData.angle - 1), 0.75)
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.space(0.5)
                            anchors.verticalCenter: parent.verticalCenter
                            visible: parent.width > width + Theme.space(1)
                            text: parent.modelData.angleName
                            color: Theme.contentWhite
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                }
            }
            ListView {
                id: cutList
                objectName: "autoSwitchCuts"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(52)
                clip: true
                spacing: Theme.space(0.5)
                model: dialog.cuts
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}
                delegate: Rectangle {
                    id: cutRow
                    required property var modelData
                    width: cutList.width
                    implicitHeight: cutLine.implicitHeight + Theme.space(1)
                    radius: Theme.radiusField
                    color: Theme.panel
                    RowLayout {
                        id: cutLine
                        anchors.fill: parent
                        anchors.leftMargin: Theme.space(1)
                        anchors.rightMargin: Theme.space(1)
                        spacing: Theme.space(1.5)
                        Value {
                            Layout.preferredWidth: Theme.space(14)
                            text: dialog.clock(cutRow.modelData.offset)
                        }
                        Rectangle {
                            Layout.preferredWidth: Theme.space(1.5)
                            Layout.preferredHeight: Theme.space(1.5)
                            radius: width / 2
                            color: dialog.angleColor(cutRow.modelData.angle - 1)
                        }
                        Text {
                            Layout.fillWidth: true
                            text: cutRow.modelData.explanation
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            elide: Text.ElideRight
                        }
                    }
                }
                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - Theme.space(4)
                    visible: cutList.count === 0 && !AutoSwitch.busy
                    illustration: "transcript"
                    text: AutoSwitch.message !== "" ? AutoSwitch.message
                                                   : qsTr("Preview to see who speaks when and where the cuts go. Transcribe the microphones first for the best cuts.")
                }
            }
            Note {
                objectName: "autoSwitchMessage"
                visible: AutoSwitch.message !== "" && dialog.cuts.length > 0
                text: AutoSwitch.message
                color: Theme.warning
                maximumLineCount: 4
                elide: Text.ElideRight
            }
        }

        // ---- The rules ----
        ColumnLayout {
            Layout.preferredWidth: Theme.space(72)
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(1.5)
            enabled: !AutoSwitch.busy

            Label { text: qsTr("Style") }
            PillTabs {
                id: presetTabs
                objectName: "autoSwitchPreset"
                Layout.fillWidth: true
                Accessible.name: qsTr("Style")
                compact: true
                currentIndex: 0
                model: [qsTr("Podcast calm"), qsTr("Interview"), qsTr("Energetic")]
                onActivated: index => {
                    dialog.loadPreset(dialog.presetIds[index])
                    dialog.refresh()
                }
            }
            Rule { id: holdSlider; objectName: "autoSwitchMinHold"; label: qsTr("Hold a shot at least"); from: 0.5; to: 6; stepSize: 0.1 }
            Rule { id: leadSlider; objectName: "autoSwitchLead"; label: qsTr("Cut before they speak"); from: 0; to: 1; stepSize: 0.05 }
            Rule { id: overlapSlider; objectName: "autoSwitchOverlap"; label: qsTr("Crosstalk before wide"); from: 0; to: 3; stepSize: 0.1 }
            Rule { id: silenceSlider; objectName: "autoSwitchSilence"; label: qsTr("Pause before wide"); from: 0; to: 10; stepSize: 0.5; zeroText: qsTr("never") }
            Rule { id: varietySlider; objectName: "autoSwitchVariety"; label: qsTr("Variety wide shot after"); from: 0; to: 60; stepSize: 1; zeroText: qsTr("never") }
            Rule { id: varietyLengthSlider; objectName: "autoSwitchVarietyLength"; label: qsTr("Variety shot length"); from: 1; to: 10; stepSize: 0.5 }
            Toggle { id: overlapToggle; objectName: "autoSwitchWideOnOverlap"; Layout.fillWidth: true; text: qsTr("Wide when people talk over each other"); checked: true; onToggled: dialog.refresh() }
            Toggle { id: laughterToggle; objectName: "autoSwitchWideOnLaughter"; Layout.fillWidth: true; text: qsTr("Wide for laughter"); checked: true; onToggled: dialog.refresh() }
            Toggle { id: reactionsToggle; objectName: "autoSwitchReactions"; Layout.fillWidth: true; text: qsTr("Reaction shots of the listener"); onToggled: dialog.refresh() }
            Toggle { id: openToggle; objectName: "autoSwitchOpenWide"; Layout.fillWidth: true; text: qsTr("Open on the wide shot"); checked: true; onToggled: dialog.refresh() }
            Toggle { id: midWordToggle; objectName: "autoSwitchMidWord"; Layout.fillWidth: true; text: qsTr("Never cut in the middle of a word"); checked: true; onToggled: dialog.refresh() }
            Toggle {
                id: facesToggle
                objectName: "autoSwitchFaces"
                Layout.fillWidth: true
                text: qsTr("Look at the cameras' faces")
                tooltip: qsTr("Finds who is on which camera and whose lips move. Off: quicker on long recordings; set the cameras yourself")
                checked: true
                onToggled: dialog.refresh()
            }
            Toggle {
                id: soundToggle
                objectName: "autoSwitchSound"
                Layout.fillWidth: true
                text: qsTr("Switch the sound too")
                tooltip: qsTr("Off: the picture switches and the sound stays the mix of the microphones")
                onToggled: dialog.refresh()
            }
            Note { text: qsTr("Runs on this computer, free. The cuts are ordinary angle cuts: change any of them afterwards in the timeline or the multicam monitor.") }
        }
    }

    // ---- The job: a pill and the UV beam ----
    RowLayout {
        Layout.fillWidth: true
        visible: AutoSwitch.busy
        spacing: Theme.space(2)
        StatusPill {
            objectName: "autoSwitchJobPill"
            label: AutoSwitch.phase
            detail: AutoSwitch.progress >= 0 ? Math.round(AutoSwitch.progress * 100) + "%" : ""
            dotColor: Theme.accentFill
        }
        BeamProgress {
            Layout.fillWidth: true
            value: AutoSwitch.progress >= 0 ? AutoSwitch.progress : 1
            active: AutoSwitch.busy
            label: AutoSwitch.phase
        }
    }

    footerData: [
        PillButton {
            text: AutoSwitch.busy ? qsTr("Stop") : qsTr("Cancel")
            onClicked: AutoSwitch.busy ? AutoSwitch.cancel() : dialog.close()
        },
        PillButton {
            objectName: "autoSwitchPreviewButton"
            iconName: "rotate-cw"
            text: qsTr("Preview")
            enabled: !AutoSwitch.busy
            onClicked: dialog.refresh()
        },
        PillButton {
            objectName: "autoSwitchApplyButton"
            variant: "primary"
            iconName: "scissors"
            text: qsTr("Switch angles")
            enabled: !AutoSwitch.busy && AutoSwitch.staged
            onClicked: {
                if (AutoSwitch.apply() === "")
                    dialog.close()
            }
        }
    ]
}
