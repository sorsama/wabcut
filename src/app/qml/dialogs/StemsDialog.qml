pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Separate Stems… (M5l, bridge `ClipAudio`; A.4): splits a clip's sound into drums, bass, other and vocals with
// HTDemucs on this computer. The stems become media items in a "<source> stems" bin next to the source, each marked
// as separated from it. The first use downloads the model after its licence card.
WabcutDialog {
    id: dialog
    objectName: "stemsDialog"

    property string clipKey: ""   // "c12"
    readonly property bool ready: ClipAudio.models.stemsReady === true
    readonly property bool working: ClipAudio.busy === "stems"
    property bool started: false

    function openFor(key) {
        clipKey = key
        started = false
        open()
    }

    title: qsTr("Separate Stems")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(120), Theme.space(120))

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        text: qsTr("Splits the clip's sound into drums, bass, other and vocals, as new media in a bin next to it. It runs on this computer and needs about 2.5 GB of free memory.")
    }
    Rectangle {
        Layout.fillWidth: true
        visible: !dialog.ready
        implicitHeight: license.implicitHeight + Theme.space(4)
        radius: Theme.radiusCard
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
        Text {
            id: license
            anchors.fill: parent
            anchors.margins: Theme.space(2)
            wrapMode: Text.Wrap
            text: qsTr("First use: downloads HTDemucs (%1). %2").arg(ClipAudio.models.stemsSize ?? "").arg(ClipAudio.models.stemsLicense ?? "")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }
    BeamProgress {
        objectName: "stemsProgress"
        Layout.fillWidth: true
        loader: "equalizer"
        visible: dialog.working
        indeterminate: ClipAudio.progress < 0
        value: Math.max(0, ClipAudio.progress)
        active: dialog.working
        label: ClipAudio.phase
    }
    Text {
        objectName: "stemsMessage"
        Layout.fillWidth: true
        visible: dialog.started && !dialog.working && text !== ""
        wrapMode: Text.Wrap
        text: ClipAudio.message
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: dialog.working ? qsTr("Cancel") : qsTr("Close")
            onClicked: {
                if (dialog.working)
                    ClipAudio.cancel()
                else
                    dialog.close()
            }
        },
        PillButton {
            objectName: "stemsButton"
            variant: "primary"
            visible: !dialog.started || dialog.working
            enabled: !dialog.working && dialog.clipKey !== ""
            text: dialog.ready ? qsTr("Separate") : qsTr("Download and separate")
            onClicked: {
                if (ClipAudio.separateStems(dialog.clipKey) === "")
                    dialog.started = true
            }
        }
    ]
}
