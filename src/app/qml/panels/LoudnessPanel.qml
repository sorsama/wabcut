pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// ● LOUDNESS (0.7, cards 104 and 141; Window > Loudness, docked beside the Timeline): the loudness of what plays, always in
// view. Two views:
//   Meter   the momentary (400 ms) and short-term (3 s) loudness as two vertical bars on the EBU R128 scale around the
//           target (+9 to -18 LU), with the numbers: M and S in LUFS, the integrated loudness since play started or Reset
//           (against the target), the true peak and the loudness range. Live: it follows the player (A.3's LoudnessTap,
//           libebur128: BS.1770-4 K-weighting, gated integration, 4x oversampled true peak), no Analyze needed.
//   Radar   the Loudness Radar (LoudnessRadar): one revolution of the short-term loudness - live over a lap of 1, 5, 10, 30
//           or 60 minutes, or the whole programme (Analyze mix) in one turn - with the range band.
// The target (YouTube -14, Podcast -16, Broadcast -23 LUFS or custom) is the Audio Track Mixer's. The meters poll only while
// this panel is on screen (it counts as a viewer, AudioMixer.setMeterViewer), so a closed panel costs nothing.
Panel {
    id: root
    objectName: "loudnessPanel"

    /// 0: Meter, 1: Radar.
    property int view: 0
    readonly property var live: AudioMixer.liveLoudness
    readonly property bool valid: live.valid === true
    readonly property bool metering: root.visible && root.enabled
    readonly property var lapChoices: [{ id: "60", name: qsTr("1 minute") }, { id: "300", name: qsTr("5 minutes") },
                                       { id: "600", name: qsTr("10 minutes") }, { id: "1800", name: qsTr("30 minutes") },
                                       { id: "3600", name: qsTr("1 hour") }]

    function focusContent() {
        viewTabs.forceActiveFocus(Qt.OtherFocusReason)
    }

    title: qsTr("Loudness")
    Accessible.name: qsTr("Loudness")

    onMeteringChanged: {
        AudioMixer.setMeterViewer(root, root.metering)
        AudioMixer.radarShown = root.metering && root.view === 1
    }
    onViewChanged: AudioMixer.radarShown = root.metering && root.view === 1
    Component.onCompleted: {
        AudioMixer.setMeterViewer(root, root.metering)
        AudioMixer.radarShown = root.metering && root.view === 1
    }
    Component.onDestruction: AudioMixer.radarShown = false

    // One reading: a value over its label.
    component Reading: ColumnLayout {
        id: reading
        property string label: ""
        property string value: ""
        property bool warn: false
        property bool large: false
        Layout.fillWidth: true
        spacing: 0
        Text {
            Layout.fillWidth: true
            text: reading.value
            elide: Text.ElideRight
            color: reading.warn ? Theme.danger : Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: reading.large ? Theme.textTitle : Theme.textMd
            font.weight: Font.DemiBold
        }
        Text {
            Layout.fillWidth: true
            text: reading.label
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }

    // A vertical level bar on the scale target + 9 LU (top) to target - 18 LU (bottom), with the target as a tick.
    component LevelBar: Item {
        id: bar
        property var value: null          // LUFS or null
        property string caption: ""
        readonly property real target: AudioMixer.targetLufs
        readonly property real scaleTop: bar.target + 9
        readonly property real scaleBottom: bar.target - 18
        readonly property bool known: bar.value !== null && bar.value !== undefined
        function yOf(lufs: real): real {
            const f = Math.max(0, Math.min(1, (lufs - bar.scaleBottom) / (bar.scaleTop - bar.scaleBottom)))
            return (1 - f) * track.height
        }
        function zoneColor(lufs: real): color {
            return lufs > bar.target + 3 ? Theme.danger : lufs > bar.target + 1 ? Theme.warning
                 : lufs >= bar.target - 1 ? Theme.success : Theme.info
        }
        implicitWidth: Theme.space(9)
        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.space(1)
            Rectangle {
                id: track
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.radiusBadge
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border
                clip: true
                Rectangle {
                    visible: bar.known
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: bar.known ? track.height - bar.yOf(Number(bar.value)) : 0
                    color: bar.known ? bar.zoneColor(Number(bar.value)) : "transparent"
                }
                Rectangle { // the target
                    y: bar.yOf(bar.target) - height / 2
                    width: parent.width
                    height: Theme.focusRingWidth
                    color: Theme.textPrimary
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: bar.caption
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
        }
    }

    Flickable {
        id: flick
        objectName: "loudnessScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(column.implicitHeight, flick.height)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: column
            width: flick.width - (flick.interactive ? Theme.space(3) : 0)
            height: Math.max(implicitHeight, flick.height)
            spacing: Theme.space(2)

            // The view, the target and Reset: wrapping, so a narrow panel never clips them.
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                PillTabs {
                    id: viewTabs
                    objectName: "loudnessViewTabs"
                    compact: true
                    model: [qsTr("Meter"), qsTr("Radar")]
                    currentIndex: root.view
                    Accessible.name: qsTr("Loudness view")
                    onActivated: index => root.view = index
                }
                PillButton {
                    id: targetButton
                    objectName: "loudnessPanelTarget"
                    compact: true
                    variant: "ghost"
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
                PillButton {
                    objectName: "loudnessPanelReset"
                    compact: true
                    variant: "ghost"
                    iconName: "rotate-ccw"
                    text: qsTr("Reset")
                    tooltip: qsTr("Start a new integrated measurement and a new radar lap")
                    onClicked: AudioMixer.resetLoudness()
                }
            }

            // ---- Meter ----
            RowLayout {
                id: meterView
                objectName: "loudnessMeterView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: Theme.space(40)
                visible: root.view === 0
                spacing: Theme.space(3)

                RowLayout {
                    Layout.fillHeight: true
                    spacing: Theme.space(1)
                    // The scale: +9, 0 (target) and -18 LU.
                    Item {
                        Layout.fillHeight: true
                        implicitWidth: Theme.space(7)
                        Repeater {
                            model: [{ lu: 9, label: "+9" }, { lu: 0, label: "0" }, { lu: -9, label: "−9" }, { lu: -18, label: "−18" }]
                            delegate: Text {
                                required property var modelData
                                // Level bars have a caption under them: the scale lines up with the first bar's track.
                                y: levelM.yOf(AudioMixer.targetLufs + modelData.lu) - height / 2
                                width: parent.width
                                horizontalAlignment: Text.AlignRight
                                text: modelData.label
                                color: modelData.lu === 0 ? Theme.textPrimary : Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }
                    LevelBar {
                        id: levelM
                        objectName: "loudnessBarMomentary"
                        Layout.fillHeight: true
                        value: root.valid ? root.live.momentary : null
                        caption: qsTr("M")
                    }
                    LevelBar {
                        objectName: "loudnessBarShortTerm"
                        Layout.fillHeight: true
                        value: root.valid ? root.live.shortTerm : null
                        caption: qsTr("S")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.space(2)
                    Reading {
                        objectName: "loudnessShortTerm"
                        large: true
                        //: Loudness panel: short-term loudness (the last 3 s)
                        label: qsTr("Short-term · LUFS")
                        value: root.valid ? root.live.shortTermText : "—"
                    }
                    Reading {
                        objectName: "loudnessMomentary"
                        //: Loudness panel: momentary loudness (the last 400 ms)
                        label: qsTr("Momentary · LUFS")
                        value: root.valid ? root.live.momentaryText : "—"
                    }
                    Reading {
                        objectName: "loudnessIntegrated"
                        //: Loudness panel: integrated loudness (since play started or Reset)
                        label: qsTr("Integrated · LUFS")
                        value: root.valid ? root.live.integratedText : "—"
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.valid && (root.live.deltaText ?? "") !== ""
                        text: root.live.deltaText ?? ""
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                    Reading {
                        objectName: "loudnessTruePeak"
                        label: qsTr("True peak · dBTP")
                        value: root.valid ? root.live.truePeakText : "—"
                        warn: root.live.over === true
                    }
                    Reading {
                        objectName: "loudnessRange"
                        //: Loudness panel: loudness range (LRA)
                        label: qsTr("Range · LU")
                        value: root.valid ? (root.live.rangeText ?? "—") : "—"
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !root.valid
                        text: qsTr("Play the sequence to see its loudness.")
                        wrapMode: Text.Wrap
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
            }

            // ---- Radar ----
            ColumnLayout {
                id: radarView
                objectName: "loudnessRadarView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.view === 1
                spacing: Theme.space(2)

                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    PillTabs {
                        objectName: "loudnessRadarMode"
                        compact: true
                        model: [qsTr("Live"), qsTr("Programme")]
                        currentIndex: AudioMixer.radarMode === "programme" ? 1 : 0
                        Accessible.name: qsTr("Radar source")
                        onActivated: index => AudioMixer.radarMode = index === 1 ? "programme" : "live"
                    }
                    PillButton {
                        id: lapButton
                        objectName: "loudnessRadarLap"
                        visible: AudioMixer.radarMode === "live"
                        compact: true
                        variant: "ghost"
                        iconName: "chevron-down"
                        text: root.lapChoices.find(l => l.id === String(AudioMixer.radarLap))?.name ?? qsTr("5 minutes")
                        tooltip: qsTr("How long one turn of the radar takes")
                        onClicked: lapMenu.popup(lapButton, 0, lapButton.height + Theme.space(1))
                        OptionsMenu {
                            id: lapMenu
                            options: root.lapChoices
                            current: String(AudioMixer.radarLap)
                            onChosen: id => AudioMixer.radarLap = Number(id)
                        }
                    }
                    PillButton {
                        objectName: "loudnessRadarAnalyze"
                        visible: AudioMixer.radarMode === "programme"
                        compact: true
                        variant: "primary"
                        enabled: AudioMixer.busy === ""
                        text: AudioMixer.busy === "analyze" ? qsTr("Measuring…") : qsTr("Analyze mix")
                        tooltip: qsTr("Measure the whole mix (EBU R128) and draw it in one turn")
                        onClicked: AudioMixer.analyze("mix")
                    }
                }

                LoudnessRadar {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: Theme.space(36)
                    Layout.preferredHeight: Math.min(width, Theme.space(90))
                    radar: AudioMixer.radar
                }

                Text {
                    objectName: "loudnessRadarSummary"
                    Layout.fillWidth: true
                    text: (AudioMixer.radar.stale === true ? qsTr("The project changed since this analysis. ") : "")
                          + (AudioMixer.radar.valid === true ? (AudioMixer.radar.text ?? "") : "")
                    visible: text !== ""
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
            }
        }
    }
}
