pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// 0.6 (P1 round, #54, loudness QC): the mix's last analysis (AudioMixer.loudness) and where it happens. The loudest
// short-term (3 s) and momentary (400 ms) windows and the true peak each show their level with their time as a link that
// moves the playhead there (TimecodeLink); below them how many true-peak overs there are above the delivery ceiling
// (-1 dBTP) with a stepper through them (a link to each one's start) and "Add markers for overs": one red range marker
// over each over, named with its peak, in one undo step. Without an analysis there is nothing to show.
ColumnLayout {
    id: root
    objectName: "loudnessQc"

    readonly property var loudness: AudioMixer.loudness
    readonly property bool measured: root.loudness.valid === true
    readonly property var overs: root.measured ? (root.loudness.overs ?? []) : []
    /// How many overs there are in all (the list holds the first 200).
    readonly property int overCount: root.measured ? Math.max(root.loudness.overCount ?? 0, root.overs.length) : 0
    /// The over the stepper shows (0-based, kept inside the list).
    property int overIndex: 0
    readonly property int shownOver: Math.max(0, Math.min(root.overIndex, root.overs.length - 1))
    readonly property var over: root.overs.length > 0 ? root.overs[root.shownOver] : ({})
    readonly property bool stale: root.loudness.stale === true
    readonly property bool marked: root.loudness.overMarkers === true
    /// The last "Add markers for overs" refusal, if any.
    property string note: ""
    /// Shows a heading above (the Audio Tools card; the master strip's card has its own).
    property bool heading: false

    function lufsText(v) {
        return v === undefined || v === null ? qsTr("−∞") : Number(v).toFixed(1)
    }
    function goToOver(index: int) {
        if (root.overs.length === 0)
            return
        const n = root.overs.length
        root.overIndex = ((index % n) + n) % n
        ProjectController.seekToText(root.overs[root.overIndex].start)
    }

    spacing: Theme.space(1)
    visible: root.measured
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Loudness quality control")
    onLoudnessChanged: root.note = ""

    // One loudest-moment statistic: its level, and under it the time it happens as a link.
    component Located: ColumnLayout {
        id: stat
        property string label: ""
        property string value: ""
        property string at: ""
        property string what: ""
        property bool warn: false
        Layout.fillWidth: true
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: stat.label
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Text {
                text: stat.value
                color: stat.warn ? Theme.dangerText : Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: stat.at !== ""
            spacing: Theme.space(1)
            Item { Layout.fillWidth: true; Layout.preferredHeight: 1 }
            Text {
                text: qsTr("at")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            TimecodeLink {
                objectName: "qcLink_" + stat.objectName
                timecode: stat.at
                what: stat.what
            }
        }
    }

    Text {
        objectName: "qcHeading"
        Layout.fillWidth: true
        visible: root.heading
        text: qsTr("Mix: loudest moments and true-peak overs")
        elide: Text.ElideRight
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.DemiBold
    }
    Located {
        objectName: "shortTerm"
        label: qsTr("Short-term (max)")
        value: qsTr("%1 LUFS").arg(root.lufsText(root.loudness.shortTerm))
        at: root.loudness.shortTermAt ?? ""
        what: qsTr("Loudest short-term moment")
    }
    Located {
        objectName: "momentary"
        label: qsTr("Momentary (max)")
        value: qsTr("%1 LUFS").arg(root.lufsText(root.loudness.momentary))
        at: root.loudness.momentaryAt ?? ""
        what: qsTr("Loudest momentary moment")
    }
    Located {
        objectName: "truePeak"
        label: qsTr("True peak")
        value: qsTr("%1 dBTP").arg(root.lufsText(root.loudness.truePeak))
        warn: (root.loudness.truePeak ?? -99) > AudioMixer.targetTruePeak
        at: root.loudness.truePeakAt ?? ""
        what: qsTr("True peak")
    }
    RowLayout {
        Layout.fillWidth: true
        Text {
            Layout.fillWidth: true
            text: qsTr("Range (LRA)")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
        Text {
            text: qsTr("%1 LU").arg(root.lufsText(root.loudness.range))
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
    }

    // ---- True-peak overs ----
    Text {
        objectName: "qcOverCount"
        Layout.fillWidth: true
        Layout.topMargin: Theme.space(1)
        wrapMode: Text.Wrap
        text: root.overCount > 0 ? (root.overCount === 1
                                    ? qsTr("1 true-peak over above %1 dBTP").arg(AudioMixer.targetTruePeak)
                                    : qsTr("%1 true-peak overs above %2 dBTP").arg(root.overCount).arg(AudioMixer.targetTruePeak))
                                 : qsTr("No true-peak overs above %1 dBTP").arg(AudioMixer.targetTruePeak)
        color: root.overCount > 0 ? Theme.dangerText : Theme.success
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }
    RowLayout {
        objectName: "qcOverStepper"
        Layout.fillWidth: true
        visible: root.overs.length > 0
        spacing: Theme.space(1)
        IconButton {
            objectName: "qcOverPrevious"
            compact: true
            iconName: "chevron-left"
            enabled: root.overs.length > 1
            tooltip: qsTr("Go to the previous true-peak over")
            onClicked: root.goToOver(root.shownOver - 1)
        }
        Text {
            text: qsTr("%1 of %2").arg(root.shownOver + 1).arg(root.overs.length)
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
        TimecodeLink {
            objectName: "qcOverLink"
            timecode: root.over.start ?? ""
            what: qsTr("True-peak over")
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("%1 dBTP").arg(root.lufsText(root.over.truePeak))
            elide: Text.ElideRight
            color: Theme.dangerText
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
        }
        IconButton {
            objectName: "qcOverNext"
            compact: true
            iconName: "chevron-right"
            enabled: root.overs.length > 1
            tooltip: qsTr("Go to the next true-peak over")
            onClicked: root.goToOver(root.shownOver + 1)
        }
    }
    PillButton {
        objectName: "qcAddOverMarkers"
        Layout.fillWidth: true
        visible: root.overs.length > 0
        compact: true
        iconName: "bookmark"
        text: root.marked ? qsTr("Markers added") : qsTr("Add markers for overs")
        enabled: !root.marked && !root.stale && AudioMixer.busy === ""
        tooltip: qsTr("A red range marker over each true-peak over, in one undo step")
        disabledReason: root.marked ? qsTr("Every over of this analysis already has a marker")
                                    : root.stale ? qsTr("The project changed since the analysis: analyze the mix again") : ""
        onClicked: root.note = AudioMixer.addOverMarkers()
    }
    Text {
        objectName: "qcNote"
        Layout.fillWidth: true
        visible: text !== ""
        text: root.note
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
}
