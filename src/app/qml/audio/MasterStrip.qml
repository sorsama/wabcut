pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The master strip of the Audio Track Mixer (M5g): the master effect stack with the limiter preset
// (audio.masterLimiter, −1 dBTP), the master fader beside its level meter, and the loudness of the mix against a
// target (YouTube −14, Podcast −16, Broadcast −23 LUFS, or custom). The loudness comes from the last analysis of
// the mix (AudioMixer.loudness), labelled as such: integrated, short-term and momentary maxima, true peak and range;
// "Analyze" measures again.
Rectangle {
    id: root
    objectName: "masterStrip"

    readonly property var master: AudioMixer.master
    readonly property var loudness: AudioMixer.loudness
    readonly property bool hasLoudness: loudness.valid === true

    function lufsText(v) {
        return v === undefined || v === null ? qsTr("−∞") : Number(v).toFixed(1)
    }

    // A loudness statistic and its value.
    component Stat: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
    component Value: Text {
        Layout.alignment: Qt.AlignRight
        color: Theme.textPrimary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textXs
    }

    implicitWidth: Theme.mixerMasterWidth
    radius: Theme.radiusField
    color: Theme.raised
    border.width: Theme.hairline
    border.color: Theme.borderStrong

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.space(2)
        spacing: Theme.space(2)

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.space(2)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Rectangle {
                    implicitWidth: masterLabel.implicitWidth + Theme.space(2)
                    implicitHeight: Theme.space(5)
                    radius: Theme.radiusBadge
                    color: Theme.accentFill
                    Text {
                        id: masterLabel
                        anchors.centerIn: parent
                        text: qsTr("Mix")
                        color: Theme.accentOnFill
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.weight: Font.Bold
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Master")
                    elide: Text.ElideRight
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
                PillButton {
                    objectName: "masterLimiterButton"
                    compact: true
                    variant: root.master.hasLimiter ? "secondary" : "primary"
                    iconName: "gauge"
                    text: root.master.hasLimiter ? qsTr("Limiter on") : qsTr("Limiter")
                    tooltip: qsTr("A true-peak limiter at %1 dBTP last on the master").arg(AudioMixer.targetTruePeak)
                    onClicked: AudioMixer.addMasterLimiter()
                }
            }

            // The master's channel format (A.9, M5n; Premiere's Sequence Settings > Audio > Master): stereo or 5.1.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Text {
                    text: qsTr("Format")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
                PillTabs {
                    id: masterFormatTabs
                    objectName: "masterFormatTabs"
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("Stereo"), qsTr("5.1")]
                    currentIndex: AudioChannels.masterFormat === "5.1" ? 1 : 0
                    Accessible.name: qsTr("Master channel format")
                    onActivated: index => {
                        AudioChannels.setMasterFormat(index === 1 ? "5.1" : "stereo")
                        currentIndex = Qt.binding(() => AudioChannels.masterFormat === "5.1" ? 1 : 0)
                    }
                }
            }

            EffectSlots {
                Layout.fillWidth: true
                target: "master"
                effects: root.master.effects ?? []
            }

            // Live loudness while playing (M5l), then the analysis of the whole mix.
            LiveLoudness {
                Layout.fillWidth: true
            }

            // Loudness
            Rectangle {
                objectName: "loudnessMeter"
                Layout.fillWidth: true
                implicitHeight: loudnessColumn.implicitHeight + Theme.space(4)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border

                ColumnLayout {
                    id: loudnessColumn
                    anchors.fill: parent
                    anchors.margins: Theme.space(2)
                    spacing: Theme.space(1)

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Loudness")
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                            font.weight: Font.DemiBold
                            font.letterSpacing: Theme.labelTracking
                        }
                        PillButton {
                            id: targetButton
                            objectName: "loudnessTarget"
                            compact: true
                            iconName: "chevron-down"
                            text: qsTr("%1 LUFS").arg(AudioMixer.targetLufs)
                            tooltip: qsTr("Loudness target")
                            onClicked: targetMenu.popup(targetButton, 0, targetButton.height + Theme.space(1))
                            OptionsMenu {
                                id: targetMenu
                                options: AudioMixer.targets.map(t => ({ id: t.id, name: t.name + " · " + t.lufs + " LUFS" }))
                                current: AudioMixer.target
                                onChosen: id => AudioMixer.target = id
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: AudioMixer.target === "custom"
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Custom target")
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        NumberField {
                            objectName: "customLufs"
                            Layout.preferredWidth: Theme.numberFieldWidth
                            from: -70
                            to: 0
                            decimals: 1
                            stepSize: 0.5
                            suffix: qsTr("LUFS")
                            value: AudioMixer.customLufs
                            onValueModified: v => AudioMixer.customLufs = v
                            Accessible.name: qsTr("Custom loudness target in LUFS")
                        }
                    }

                    // Integrated, against the target
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(1)
                        Text {
                            objectName: "integratedText"
                            text: root.hasLoudness ? root.lufsText(root.loudness.integrated) : qsTr("—")
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textTitle
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.alignment: Qt.AlignBottom
                            Layout.bottomMargin: Theme.space(1) / 2
                            text: qsTr("LUFS integrated")
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    // Scale −40 … 0 LUFS: the target tick and the integrated level.
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: Theme.space(3)
                        readonly property real lo: -40
                        function xOf(v) { return Math.max(0, Math.min(1, (v - lo) / -lo)) * width }
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width
                            height: Theme.space(1)
                            radius: height / 2
                            color: Theme.panel
                        }
                        Rectangle {
                            visible: root.hasLoudness && root.loudness.integrated !== null && root.loudness.integrated !== undefined
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.xOf(root.loudness.integrated ?? -70)
                            height: Theme.space(1)
                            radius: height / 2
                            color: Math.abs(root.loudness.delta ?? 99) < 1 ? Theme.success : Theme.warning
                        }
                        Rectangle {
                            x: parent.xOf(AudioMixer.targetLufs) - width / 2
                            width: Theme.focusRingWidth
                            height: parent.height
                            color: Theme.textPrimary
                        }
                    }
                    Text {
                        objectName: "loudnessDelta"
                        Layout.fillWidth: true
                        text: root.hasLoudness ? root.loudness.deltaText : qsTr("Not measured yet")
                        color: !root.hasLoudness ? Theme.textTertiary : Math.abs(root.loudness.delta ?? 99) < 1 ? Theme.success : Theme.warning
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        wrapMode: Text.Wrap
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        visible: root.hasLoudness
                        columns: 2
                        columnSpacing: Theme.space(2)
                        rowSpacing: 0
                        Stat { text: qsTr("Short-term (max)") }
                        Value { text: qsTr("%1 LUFS").arg(root.lufsText(root.loudness.shortTerm)) }
                        Stat { text: qsTr("Momentary (max)") }
                        Value { text: qsTr("%1 LUFS").arg(root.lufsText(root.loudness.momentary)) }
                        Stat { text: qsTr("True peak") }
                        Value {
                            text: qsTr("%1 dBTP").arg(root.lufsText(root.loudness.truePeak))
                            color: (root.loudness.truePeak ?? -99) > AudioMixer.targetTruePeak ? Theme.danger : Theme.textPrimary
                        }
                        Stat { text: qsTr("Range (LRA)") }
                        Value { text: qsTr("%1 LU").arg(root.lufsText(root.loudness.range)) }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: !root.hasLoudness ? qsTr("Analysis of the mix")
                                : root.loudness.stale ? qsTr("Analysis %1 · the project changed").arg(root.loudness.whenText)
                                                      : qsTr("Analysis %1").arg(root.loudness.whenText)
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                        PillButton {
                            objectName: "analyzeMixButton"
                            compact: true
                            enabled: AudioMixer.busy === ""
                            text: qsTr("Analyze")
                            tooltip: qsTr("Measure the loudness of the mix (EBU R128)")
                            onClicked: AudioMixer.analyze("mix")
                        }
                    }
                }
            }

            // The master fader's automation mode (M5l).
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Master automation")
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
                AutomationButton {
                    target: "master"
                    mode: root.master.automation ?? "read"
                }
            }

            Item { Layout.fillHeight: true }
        }

        // Master fader and meter, the strip's full height
        RowLayout {
            Layout.fillHeight: true
            spacing: Theme.space(1)
            Fader {
                objectName: "masterFader"
                Layout.fillHeight: true
                enabled: AudioMixer.faderAvailable
                gainDb: (AudioMixer.faderValues["master"] ?? root.master).gainDb ?? 0
                label: qsTr("Master volume")
                note: AudioMixer.faderNote
                onMoved: value => AudioMixer.setGainDb("master", value)
                onReleased: AudioMixer.endFaderGesture()
                onHeldChanged: AudioMixer.touch("master", "volume", held)
            }
            ColumnLayout {
                Layout.fillHeight: true
                spacing: Theme.space(1)
                Rectangle {
                    objectName: "masterClipLight"
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: Theme.meterBarWidth
                    implicitHeight: Theme.space(2)
                    radius: Theme.radiusBadge / 2
                    color: masterMeter.clipped ? Theme.danger : Theme.sunken
                    border.width: Theme.hairline
                    border.color: masterMeter.clipped ? Theme.danger : Theme.border
                    TapHandler {
                        onTapped: AudioMixer.clearClip("master")
                    }
                    HoverHandler {
                        enabled: masterMeter.clipped
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                MeterBar {
                    id: masterMeter
                    objectName: "masterMeter"
                    Layout.fillHeight: true
                    // A 5.1 master meters six channels (L, R, C, LFE, Ls, Rs).
                    Layout.preferredWidth: masterMeter.channels > 2 ? Theme.meterBarWidth * 3 : Theme.meterBarWidth
                    source: AudioMixer
                    channel: "master"
                    trackColor: Theme.sunken
                    lowColor: Theme.hueMint
                    midColor: Theme.hueLime
                    highColor: Theme.hueOrange
                    peakColor: Theme.textPrimary
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: masterMeter.peakText
                    color: masterMeter.clipped ? Theme.danger : Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
        }
    }
}
