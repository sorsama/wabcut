pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Enhance Speech… (M5l, bridge `ClipAudio`; A.4): studio sound for dialogue, rendered on this computer. The take is
// rendered once per media file and cached (the badge says whether it is ready); Strength mixes it with the original
// live, and the bypass switch is the A/B. The first use downloads the model after its licence card.
WabcutDialog {
    id: dialog
    objectName: "enhanceSpeechDialog"

    property var clips: []                 // "c12" keys
    readonly property var info: clips.length > 0 ? ClipAudio.clipInfo(clips[0]) : ({})
    readonly property bool enhanced: info.enhanced === true
    readonly property bool ready: ClipAudio.models.speechReady === true
    readonly property bool working: ClipAudio.busy === "enhance"
    property int revision: 0               // re-reads clipInfo after an edit

    function openFor(keys) {
        clips = keys
        strength.value = info.strength ?? 100
        open()
    }
    function enhance() {
        ClipAudio.enhanceSpeech(clips, strength.value, true)
    }
    function takeText(take) {
        return take === "ready" ? qsTr("Enhanced take ready (rendered once, cached)")
             : take === "stale" ? qsTr("The take is out of date: render it again")
             : take === "missing" ? qsTr("Not rendered yet: the original plays")
             : ""
    }

    title: clips.length > 1 ? qsTr("Enhance Speech on %n clip(s)", "", clips.length) : qsTr("Enhance Speech")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(130), Theme.space(130))

    Connections {
        target: ProjectController
        function onRevisionChanged() {
            dialog.revision++
            dialog.clips = dialog.clips.slice() // re-reads info
        }
    }

    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        text: qsTr("Removes noise, hum and room from dialogue. It runs on this computer; nothing is uploaded.")
    }

    // The model's licence card, before its first download.
    Rectangle {
        objectName: "enhanceLicense"
        Layout.fillWidth: true
        visible: !dialog.ready
        implicitHeight: licenseColumn.implicitHeight + Theme.space(4)
        radius: Theme.radiusCard
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border
        ColumnLayout {
            id: licenseColumn
            anchors.fill: parent
            anchors.margins: Theme.space(2)
            spacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: qsTr("First use: downloads DeepFilterNet3 (%1)").arg(ClipAudio.models.speechSize ?? "")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.Medium
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: ClipAudio.models.speechLicense ?? ""
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        Text {
            text: qsTr("Strength")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        ValueSlider {
            id: strength
            objectName: "enhanceStrength"
            Layout.fillWidth: true
            Accessible.name: qsTr("Strength")
            from: 0
            to: 100
            stepSize: 1
            value: 100
            onMoved: {
                if (dialog.enhanced)
                    ClipAudio.setStrength(dialog.clips[0], value)
            }
            onPressedChanged: {
                if (!pressed && dialog.enhanced)
                    ClipAudio.endStrength()
            }
        }
        Text {
            Layout.preferredWidth: Theme.space(12)
            horizontalAlignment: Text.AlignRight
            text: qsTr("%1 %").arg(Math.round(strength.value))
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
    }

    // The take's state and the A/B.
    RowLayout {
        Layout.fillWidth: true
        visible: dialog.enhanced
        spacing: Theme.space(3)
        Rectangle {
            objectName: "enhanceTakeBadge"
            implicitWidth: takeLabel.implicitWidth + Theme.space(3)
            implicitHeight: takeLabel.implicitHeight + Theme.space(1)
            radius: height / 2
            color: dialog.info.take === "ready" ? Qt.alpha(Theme.success, 0.16) : Qt.alpha(Theme.warning, 0.16)
            Text {
                id: takeLabel
                anchors.centerIn: parent
                text: dialog.info.take === "ready" ? qsTr("Enhanced") : qsTr("Missing render")
                color: dialog.info.take === "ready" ? Theme.success : Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
        }
        Text {
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: dialog.takeText(dialog.info.take)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Toggle {
            objectName: "enhanceBypass"
            text: qsTr("Bypass")
            checked: dialog.info.bypassed === true
            onToggled: ClipAudio.setBypassed(dialog.clips[0], checked)
        }
    }

    BeamProgress {
        objectName: "enhanceProgress"
        Layout.fillWidth: true
        loader: "equalizer"
        visible: dialog.working
        indeterminate: ClipAudio.progress < 0
        value: Math.max(0, ClipAudio.progress)
        active: dialog.working
        label: ClipAudio.phase
    }
    Text {
        objectName: "enhanceMessage"
        Layout.fillWidth: true
        visible: text !== "" && !dialog.working
        wrapMode: Text.Wrap
        text: ClipAudio.message
        color: Theme.textTertiary
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
            objectName: "enhanceButton"
            variant: "primary"
            enabled: !dialog.working && dialog.clips.length > 0
            text: !dialog.ready ? qsTr("Download and enhance")
                  : dialog.enhanced && dialog.info.take !== "ready" ? qsTr("Render enhancement")
                  : dialog.enhanced ? qsTr("Apply strength") : qsTr("Enhance")
            onClicked: dialog.enhance()
        }
    ]
}
