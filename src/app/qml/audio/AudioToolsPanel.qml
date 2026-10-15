pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// ● AUDIO TOOLS (the Audio workspace's browser slot, M5g). Every action is a typed audio command, one undo step:
//   Analyze loudness   the mix, every track and the mix, or the selected clips (audio.loudness): a report table.
//   Normalize          to the loudness target (audio.normalize), dry-run first: before → gain → after per target,
//                      then Apply.
//   Auto-duck          the music tracks under the dialogue tracks (roles, set in the mixer) by an amount with attack,
//                      release and hold (audio.autoDuck); the planned volume keys show on the timeline as dashed
//                      ghosts until Apply or Cancel.
//   Reduce noise       adds Reduce Noise (RNNoise) to the selected audio clips, or sets its amount where it is.
// Measuring runs in the background over a copy of the project; Cancel stops it.
Panel {
    id: root
    objectName: "audioToolsPanel"

    property int analyzeScope: 0   // 0 mix, 1 tracks, 2 selected clips
    property int normalizeScope: 0 // 0 mix, 1 selected clips
    readonly property bool idle: AudioMixer.busy === ""
    readonly property var pending: AudioMixer.pending

    function focusContent() {
        analyzeButton.forceActiveFocus(Qt.TabFocusReason)
    }
    function numberText(v, suffix) {
        return v === undefined || v === null ? qsTr("−∞") : Number(v).toFixed(1) + suffix
    }

    title: qsTr("Audio Tools")

    // A tool card: icon chip, title and a line on what it does, then its controls.
    component ToolCard: Rectangle {
        id: card
        property string heading: ""
        property string detail: ""
        property string iconName: ""
        default property alias content: body.data
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
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Rectangle {
                    implicitWidth: Theme.controlHeightSmall
                    implicitHeight: Theme.controlHeightSmall
                    radius: Theme.radiusField
                    color: Theme.accentWash
                    Icon {
                        anchors.centerIn: parent
                        name: card.iconName
                        size: Theme.iconSm
                        color: Theme.accentStroke
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        text: card.heading
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: card.detail
                        wrapMode: Text.Wrap
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
            }
            ColumnLayout {
                id: body
                Layout.fillWidth: true
                spacing: Theme.space(2)
            }
        }
    }
    component Head: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.DemiBold
    }
    component FieldLabel: Text {
        Layout.fillWidth: true
        elide: Text.ElideRight
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
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

            // ---- Busy, message ----
            Rectangle {
                objectName: "audioJob"
                Layout.fillWidth: true
                visible: !root.idle
                // Margins, the row, a gap, the progress bar.
                implicitHeight: busyRow.implicitHeight + Theme.space(5.5) + Theme.progressHeight
                radius: Theme.radiusField
                color: Theme.accentWash
                RowLayout {
                    id: busyRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.space(2)
                    EchoLoader {
                        objectName: "audioJobLoader"
                        Layout.alignment: Qt.AlignVCenter
                        size: Theme.space(5)
                        variant: "equalizer"
                    }
                    Text {
                        id: busyText
                        Layout.fillWidth: true
                        text: AudioMixer.busy === "analyze" ? qsTr("Measuring loudness…")
                            : AudioMixer.busy === "normalize" ? qsTr("Measuring for the normalization…")
                                                              : qsTr("Finding the dialogue…")
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    PillButton {
                        compact: true
                        text: qsTr("Cancel")
                        onClicked: AudioMixer.cancelJob()
                    }
                }
                // The job's render light (no percentage: it runs until the job ends).
                BeamProgress {
                    objectName: "audioJobProgress"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: Theme.space(2)
                    indeterminate: true
                    loader: ""
                    label: busyText.text
                }
            }
            Text {
                objectName: "audioMessage"
                Layout.fillWidth: true
                visible: AudioMixer.message !== ""
                text: AudioMixer.message
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            // ---- A dry run to apply ----
            Rectangle {
                objectName: "audioPending"
                Layout.fillWidth: true
                visible: root.pending.kind !== undefined
                implicitHeight: pendingColumn.implicitHeight + Theme.space(4)
                radius: Theme.radiusField
                color: Qt.alpha(Theme.warning, 0.10)
                border.width: Theme.hairline
                border.color: Qt.alpha(Theme.warning, 0.55)
                ColumnLayout {
                    id: pendingColumn
                    anchors.fill: parent
                    anchors.margins: Theme.space(3)
                    spacing: Theme.space(1)
                    Text {
                        Layout.fillWidth: true
                        text: root.pending.title ?? ""
                        wrapMode: Text.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.pending.info ?? ""
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Repeater {
                        model: root.pending.kind === "normalize" ? root.pending.rows : []
                        delegate: Text {
                            required property var modelData
                            Layout.fillWidth: true
                            text: qsTr("%1: %2 → %3 LUFS (gain %4 dB)").arg(modelData.target)
                                    .arg(root.numberText(modelData.before, "")).arg(root.numberText(modelData.after, ""))
                                    .arg(root.numberText(modelData.gain, ""))
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                    Repeater {
                        model: root.pending.warnings ?? []
                        delegate: Text {
                            required property string modelData
                            Layout.fillWidth: true
                            text: modelData
                            wrapMode: Text.Wrap
                            color: Theme.warning
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        PillButton {
                            compact: true
                            text: qsTr("Cancel")
                            onClicked: AudioMixer.cancelPending()
                        }
                        PillButton {
                            objectName: "applyPendingButton"
                            compact: true
                            variant: "primary"
                            text: qsTr("Apply")
                            onClicked: AudioMixer.applyPending()
                        }
                    }
                }
            }

            // ---- Loudness ----
            ToolCard {
                objectName: "loudnessTool"
                heading: qsTr("Analyze loudness")
                detail: qsTr("EBU R128: integrated loudness, range and true peak")
                iconName: "audio-lines"
                PillTabs {
                    Layout.fillWidth: true
                    compact: true
                    model: [{ label: qsTr("Mix") }, { label: qsTr("Tracks") }, { label: qsTr("Clips") }]
                    currentIndex: root.analyzeScope
                    onCurrentIndexChanged: root.analyzeScope = currentIndex
                    Accessible.name: qsTr("What to measure")
                }
                PillButton {
                    id: analyzeButton
                    objectName: "analyzeButton"
                    Layout.fillWidth: true
                    enabled: root.idle
                    variant: "primary"
                    text: qsTr("Analyze loudness")
                    onClicked: AudioMixer.analyze(["mix", "tracks", "clips"][root.analyzeScope])
                }
                // Report
                GridLayout {
                    objectName: "loudnessReport"
                    Layout.fillWidth: true
                    visible: AudioMixer.report.length > 0
                    columns: 4
                    columnSpacing: Theme.space(2)
                    rowSpacing: Theme.space(1)
                    Head { Layout.fillWidth: true; text: qsTr("Target") }
                    Head { text: qsTr("LUFS") }
                    Head { text: qsTr("LRA") }
                    Head { text: qsTr("dBTP") }
                    Repeater {
                        model: AudioMixer.report
                        delegate: Text {
                            required property var modelData
                            required property int index
                            Layout.row: index + 1
                            Layout.column: 0
                            Layout.fillWidth: true
                            text: modelData.target
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    Repeater {
                        model: AudioMixer.report
                        delegate: Text {
                            required property var modelData
                            required property int index
                            Layout.row: index + 1
                            Layout.column: 1
                            text: root.numberText(modelData.integrated, "")
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                    Repeater {
                        model: AudioMixer.report
                        delegate: Text {
                            required property var modelData
                            required property int index
                            Layout.row: index + 1
                            Layout.column: 2
                            text: root.numberText(modelData.range, "")
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                    Repeater {
                        model: AudioMixer.report
                        delegate: Text {
                            required property var modelData
                            required property int index
                            Layout.row: index + 1
                            Layout.column: 3
                            text: root.numberText(modelData.truePeak, "")
                            color: (modelData.truePeak ?? -99) > AudioMixer.targetTruePeak ? Theme.danger : Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                }
            }

            // ---- Normalize ----
            ToolCard {
                objectName: "normalizeTool"
                heading: qsTr("Normalize to target")
                detail: qsTr("Gain so the loudness meets %1 LUFS, true peak under %2 dBTP").arg(AudioMixer.targetLufs).arg(AudioMixer.targetTruePeak)
                iconName: "gauge"
                RowLayout {
                    Layout.fillWidth: true
                    FieldLabel { text: qsTr("Target") }
                    PillButton {
                        id: normalizeTarget
                        compact: true
                        iconName: "chevron-down"
                        text: {
                            for (const t of AudioMixer.targets) {
                                if (t.id === AudioMixer.target)
                                    return qsTr("%1 · %2 LUFS").arg(t.name).arg(AudioMixer.targetLufs)
                            }
                            return qsTr("%1 LUFS").arg(AudioMixer.targetLufs)
                        }
                        tooltip: qsTr("Loudness target")
                        onClicked: normalizeTargetMenu.popup(normalizeTarget, 0, normalizeTarget.height + Theme.space(1))
                        OptionsMenu {
                            id: normalizeTargetMenu
                            options: AudioMixer.targets.map(t => ({ id: t.id, name: t.name + " · " + t.lufs + " LUFS" }))
                            current: AudioMixer.target
                            onChosen: id => AudioMixer.target = id
                        }
                    }
                }
                PillTabs {
                    Layout.fillWidth: true
                    compact: true
                    model: [{ label: qsTr("Mix") }, { label: qsTr("Selected clips") }]
                    currentIndex: root.normalizeScope
                    onCurrentIndexChanged: root.normalizeScope = currentIndex
                    Accessible.name: qsTr("What to normalize")
                }
                PillButton {
                    objectName: "previewNormalizeButton"
                    Layout.fillWidth: true
                    enabled: root.idle
                    text: qsTr("Preview normalization")
                    onClicked: AudioMixer.previewNormalize(root.normalizeScope === 0 ? "mix" : "clips")
                }
            }

            // ---- Auto-duck ----
            ToolCard {
                objectName: "duckTool"
                heading: qsTr("Auto-duck music under dialogue")
                detail: qsTr("Music tracks dip while dialogue tracks speak (roles are set in the mixer)")
                iconName: "volume-2"
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.space(2)
                    rowSpacing: Theme.space(1)
                    FieldLabel { text: qsTr("Amount") }
                    NumberField {
                        objectName: "duckAmount"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: -60
                        to: 0
                        decimals: 1
                        stepSize: 0.5
                        suffix: qsTr("dB")
                        value: AudioMixer.duckAmount
                        onValueModified: v => AudioMixer.duckAmount = v
                        Accessible.name: qsTr("Ducking amount in dB")
                    }
                    FieldLabel { text: qsTr("Attack") }
                    NumberField {
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 0
                        to: 10
                        decimals: 2
                        stepSize: 0.05
                        suffix: qsTr("s")
                        value: AudioMixer.duckAttack
                        onValueModified: v => AudioMixer.duckAttack = v
                        Accessible.name: qsTr("Attack in seconds")
                    }
                    FieldLabel { text: qsTr("Release") }
                    NumberField {
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 0
                        to: 10
                        decimals: 2
                        stepSize: 0.05
                        suffix: qsTr("s")
                        value: AudioMixer.duckRelease
                        onValueModified: v => AudioMixer.duckRelease = v
                        Accessible.name: qsTr("Release in seconds")
                    }
                    FieldLabel { text: qsTr("Hold") }
                    NumberField {
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 0
                        to: 10
                        decimals: 2
                        stepSize: 0.05
                        suffix: qsTr("s")
                        value: AudioMixer.duckHold
                        onValueModified: v => AudioMixer.duckHold = v
                        Accessible.name: qsTr("Hold in seconds")
                    }
                }
                PillButton {
                    objectName: "previewDuckButton"
                    Layout.fillWidth: true
                    enabled: root.idle
                    text: qsTr("Preview ducking")
                    onClicked: AudioMixer.previewAutoDuck()
                }
            }

            // ---- Reduce noise ----
            ToolCard {
                objectName: "denoiseTool"
                heading: qsTr("Reduce noise")
                detail: qsTr("Reduce Noise (RNNoise) on the selected audio clips")
                iconName: "sparkles"
                RowLayout {
                    Layout.fillWidth: true
                    FieldLabel { text: qsTr("Amount") }
                    NumberField {
                        objectName: "denoiseAmount"
                        Layout.preferredWidth: Theme.numberFieldWidth
                        from: 0
                        to: 100
                        suffix: qsTr("%")
                        value: Math.round(AudioMixer.denoiseAmount * 100)
                        onValueModified: v => AudioMixer.denoiseAmount = v / 100
                        Accessible.name: qsTr("Noise reduction amount in percent")
                    }
                }
                PillButton {
                    objectName: "reduceNoiseButton"
                    Layout.fillWidth: true
                    text: qsTr("Reduce noise on selected clips")
                    onClicked: AudioMixer.reduceNoise()
                }
            }
        }
    }
}
