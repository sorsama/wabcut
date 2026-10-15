pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Live loudness of what leaves the player (M5l; A.3's LoudnessTap: EBU R128 / BS.1770-4): momentary (400 ms),
// short-term (3 s), integrated since play started or since Reset, and true peak, against the mixer's target. The
// analysis card under it measures the whole mix offline; this one follows playback.
Rectangle {
    id: root
    objectName: "liveLoudness"

    readonly property var live: AudioMixer.liveLoudness
    readonly property bool valid: live.valid === true

    implicitHeight: column.implicitHeight + Theme.space(4)
    radius: Theme.radiusField
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: Theme.border

    component Reading: ColumnLayout {
        id: reading
        property string label: ""
        property string value: ""
        property bool warn: false
        Layout.fillWidth: true
        spacing: 0
        Text {
            text: reading.value
            color: reading.warn ? Theme.danger : Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textMd
            font.weight: Font.DemiBold
        }
        Text {
            text: reading.label
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.space(2)
        spacing: Theme.space(1)

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Live loudness")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.labelTracking
            }
            PillButton {
                objectName: "liveLoudnessReset"
                compact: true
                variant: "ghost"
                iconName: "rotate-ccw"
                text: qsTr("Reset")
                tooltip: qsTr("Start a new integrated measurement")
                onClicked: AudioMixer.resetLoudness()
            }
        }
        // Two by two: the master strip's column is narrow.
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.space(2)
            rowSpacing: Theme.space(1)
            Reading {
                objectName: "liveMomentary"
                //: Live loudness: momentary (the last 400 ms)
                label: qsTr("M · LUFS")
                value: root.valid ? root.live.momentaryText : "—"
            }
            Reading {
                objectName: "liveShortTerm"
                //: Live loudness: short-term (the last 3 s)
                label: qsTr("S · LUFS")
                value: root.valid ? root.live.shortTermText : "—"
            }
            Reading {
                objectName: "liveIntegrated"
                //: Live loudness: integrated (since play or reset)
                label: qsTr("I · LUFS")
                value: root.valid ? root.live.integratedText : "—"
            }
            Reading {
                objectName: "liveTruePeak"
                //: Live loudness: true peak
                label: qsTr("TP · dBTP")
                value: root.valid ? root.live.truePeakText : "—"
                warn: root.live.over === true
            }
        }
        Text {
            Layout.fillWidth: true
            visible: root.valid && (root.live.deltaText ?? "") !== ""
            text: root.live.deltaText ?? ""
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }
}
