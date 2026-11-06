pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Sequence Settings (P.2, E5): Premiere's Sequence > Sequence Settings on an existing sequence - a preset or the
// timebase, frame size, pixel aspect ratio, fields, audio sample rate, display formats, start time and preview file
// format. OK applies them as one undo step (sequence.setSettings); a new timebase keeps the edit in real time with
// every edit point on the new frame grid.
WabcutDialog {
    id: dialog
    objectName: "sequenceSettingsDialog"

    property string sequenceKey: ""
    property var settings: ({})
    property string errorText: ""
    // The values being edited.
    property string presetId: ""
    property real fps: 30
    property int frameWidth: 1920
    property int frameHeight: 1080
    property real pixelAspect: 1
    property string fields: "progressive"
    property int audioRate: 48000
    property bool dropFrame: false
    property string videoDisplay: "timecode"
    property string audioDisplay: "audioSamples"
    property string previewFormat: ""
    // 0.7 (card 046): the work area (timecodes, "" = none) and the wish to remove it.
    property string workAreaIn: ""
    property string workAreaOut: ""
    property bool clearWorkArea: false
    // 0.7 (VR / 360 video): none, equirect360 or equirect180; mono, topBottom or sideBySide.
    property string vrProjection: "none"
    property string vrStereo: "mono"
    // A measuring sample for the frame size fields' width; never shown.
    readonly property string fiveDigitSample: "00000"
    readonly property bool ntsc: Math.abs(dialog.fps - 29.97) < 0.01 || Math.abs(dialog.fps - 59.94) < 0.01
    /// r07: a frame size box fits five digits (16384) and its "px" with NumberField's paddings, in the current fonts.
    readonly property real frameFieldWidth: Math.max(Theme.numberFieldWidth,
                                                     Math.ceil(fiveDigits.advanceWidth + unitPx.advanceWidth) + Theme.space(2.5) * 2 + Theme.space(2))
    /// The frame's shape, reduced ("16:9", "9:16", "1:1").
    readonly property string aspectText: {
        let a = Math.round(dialog.frameWidth)
        let b = Math.round(dialog.frameHeight)
        if (a <= 0 || b <= 0)
            return ""
        const w = a
        const h = b
        while (b > 0) {
            const t = a % b
            a = b
            b = t
        }
        return qsTr("%1:%2").arg(w / a).arg(h / a)
    }

    TextMetrics {
        id: fiveDigits
        font.family: Theme.fontMono
        font.pixelSize: Theme.textMd
        text: dialog.fiveDigitSample
    }
    TextMetrics {
        id: unitPx
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
        text: qsTr("px")
    }

    readonly property var presets: Editing.sequencePresets()
    readonly property var presetOptions: {
        const out = [{ id: "", name: qsTr("Custom") }]
        for (const p of dialog.presets)
            out.push({ id: p.id, name: p.name })
        return out
    }
    readonly property var rateOptions: [
        { id: "23.976", name: qsTr("23.976 frames/second") }, { id: "24", name: qsTr("24 frames/second") },
        { id: "25", name: qsTr("25 frames/second") }, { id: "29.97", name: qsTr("29.97 frames/second") },
        { id: "30", name: qsTr("30 frames/second") }, { id: "50", name: qsTr("50 frames/second") },
        { id: "59.94", name: qsTr("59.94 frames/second") }, { id: "60", name: qsTr("60 frames/second") },
        { id: "120", name: qsTr("120 frames/second") }, { id: "144", name: qsTr("144 frames/second") },
        { id: "240", name: qsTr("240 frames/second") }
    ]
    readonly property var aspectOptions: [
        { id: "1", name: qsTr("Square Pixels (1.0)") }, { id: "0.9091", name: qsTr("D1/DV NTSC (0.9091)") },
        { id: "1.2121", name: qsTr("D1/DV NTSC Widescreen 16:9 (1.2121)") }, { id: "1.094", name: qsTr("D1/DV PAL (1.0940)") },
        { id: "1.4587", name: qsTr("D1/DV PAL Widescreen 16:9 (1.4587)") }, { id: "2", name: qsTr("Anamorphic 2:1 (2.0)") },
        { id: "1.3333", name: qsTr("HD Anamorphic 1080 (1.333)") }, { id: "1.5", name: qsTr("DVCPRO HD (1.5)") }
    ]
    readonly property var fieldOptions: [
        { id: "progressive", name: qsTr("No Fields (Progressive Scan)") },
        { id: "upperFirst", name: qsTr("Upper Field First") }, { id: "lowerFirst", name: qsTr("Lower Field First") }
    ]
    readonly property var audioRateOptions: [
        { id: "32000", name: qsTr("32000 Hz") }, { id: "44100", name: qsTr("44100 Hz") }, { id: "48000", name: qsTr("48000 Hz") },
        { id: "88200", name: qsTr("88200 Hz") }, { id: "96000", name: qsTr("96000 Hz") }
    ]
    readonly property var videoDisplayOptions: [
        { id: "timecode", name: dialog.ntsc && dialog.dropFrame ? qsTr("Drop-Frame Timecode") : qsTr("Non-Drop-Frame Timecode") },
        { id: "frames", name: qsTr("Frames") }, { id: "feet16mm", name: qsTr("Feet + Frames 16mm") },
        { id: "feet35mm", name: qsTr("Feet + Frames 35mm") }
    ]
    readonly property var audioDisplayOptions: [
        { id: "audioSamples", name: qsTr("Audio Samples") }, { id: "milliseconds", name: qsTr("Milliseconds") }
    ]
    readonly property var vrOptions: [
        { id: "none", name: qsTr("Flat video (not VR)") }, { id: "equirect360", name: qsTr("Equirectangular 360") },
        { id: "equirect180", name: qsTr("Equirectangular 180 (VR180)") }
    ]
    readonly property var vrStereoOptions: [
        { id: "mono", name: qsTr("Monoscopic") }, { id: "topBottom", name: qsTr("Stereoscopic: Top - Bottom") },
        { id: "sideBySide", name: qsTr("Stereoscopic: Side by Side") }
    ]
    // Whether the frame is the shape one eye's picture of the projection has (2:1 for 360, 1:1 for 180).
    readonly property bool vrShapeFits: {
        if (dialog.vrProjection === "none")
            return true
        const eyeW = dialog.vrStereo === "sideBySide" ? dialog.frameWidth / 2 : dialog.frameWidth
        const eyeH = dialog.vrStereo === "topBottom" ? dialog.frameHeight / 2 : dialog.frameHeight
        const wanted = dialog.vrProjection === "equirect180" ? 1 : 2
        return Math.abs(eyeW / Math.max(1, eyeH) / wanted - 1) <= 0.05
    }
    readonly property var previewOptions: [
        { id: "", name: qsTr("Default") }, { id: "prores", name: qsTr("Apple ProRes") },
        { id: "dnxhr", name: qsTr("Avid DNxHR") }, { id: "h264", name: qsTr("I-frame only H.264") }
    ]

    function rateId(value) {
        for (const r of dialog.rateOptions) {
            if (Math.abs(Number(r.id) - value) < 0.005)
                return r.id
        }
        return String(Math.round(value * 1000) / 1000)
    }
    function aspectId(value) {
        for (const a of dialog.aspectOptions) {
            if (Math.abs(Number(a.id) - value) < 0.0005)
                return a.id
        }
        return String(Math.round(value * 10000) / 10000)
    }
    function openFor(key) {
        dialog.sequenceKey = key
        dialog.settings = Editing.sequenceSettings(key)
        dialog.errorText = dialog.settings.name === undefined ? qsTr("Open a sequence first") : ""
        dialog.presetId = ""
        dialog.fps = dialog.settings.fps ?? 30
        dialog.frameWidth = dialog.settings.width ?? 1920
        dialog.frameHeight = dialog.settings.height ?? 1080
        dialog.pixelAspect = dialog.settings.pixelAspect ?? 1
        dialog.fields = dialog.settings.fields ?? "progressive"
        dialog.audioRate = dialog.settings.audioRate ?? 48000
        dialog.dropFrame = dialog.settings.dropFrame ?? false
        dialog.videoDisplay = dialog.settings.videoDisplay ?? "timecode"
        dialog.audioDisplay = dialog.settings.audioDisplay ?? "audioSamples"
        dialog.previewFormat = dialog.settings.previewFormat ?? ""
        dialog.workAreaIn = dialog.settings.workAreaIn ?? ""
        dialog.workAreaOut = dialog.settings.workAreaOut ?? ""
        dialog.clearWorkArea = false
        dialog.vrProjection = dialog.settings.vrProjection ?? "none"
        dialog.vrStereo = dialog.settings.vrStereo ?? "mono"
        nameField.text = dialog.settings.name ?? ""
        startField.text = dialog.settings.startTimecode ?? "00:00:00:00"
        open()
    }
    function choosePreset(id) {
        dialog.presetId = id
        for (const p of dialog.presets) {
            if (p.id !== id)
                continue
            dialog.fps = p.fps
            dialog.frameWidth = p.width
            dialog.frameHeight = p.height
            dialog.dropFrame = p.dropFrame
        }
    }
    function apply() {
        const values = {
            frameRate: Number(dialog.rateId(dialog.fps)), width: dialog.frameWidth, height: dialog.frameHeight,
            pixelAspect: dialog.pixelAspect, fields: dialog.fields, audioRate: dialog.audioRate,
            dropFrame: dialog.ntsc && dialog.dropFrame, videoDisplay: dialog.videoDisplay,
            audioDisplay: dialog.audioDisplay, previewFormat: dialog.previewFormat,
            vr: { projection: dialog.vrProjection, stereo: dialog.vrProjection === "none" ? "mono" : dialog.vrStereo }
        }
        if (nameField.text.trim() !== "")
            values.name = nameField.text.trim()
        if (startField.text.trim() !== "")
            values.startTimecode = startField.text.trim()
        if (dialog.clearWorkArea && dialog.workAreaIn !== "")
            values.clearWorkArea = true
        const error = Editing.applySequenceSettings(dialog.sequenceKey, values)
        if (error === "")
            dialog.close()
        else
            dialog.errorText = error
    }

    title: qsTr("Sequence Settings")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(190), Theme.space(190))

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)

        Label { text: qsTr("Sequence name") }
        FieldInput {
            id: nameField
            objectName: "sequenceSettingsName"
            Layout.fillWidth: true
            Accessible.name: qsTr("Sequence name")
        }

        Label { text: qsTr("Preset") }
        EditingChoice {
            objectName: "sequenceSettingsPreset"
            Layout.fillWidth: true
            options: dialog.presetOptions
            current: dialog.presetId
            onChosen: id => dialog.choosePreset(id)
        }

        Label { text: qsTr("Timebase") }
        EditingChoice {
            objectName: "sequenceSettingsTimebase"
            Layout.fillWidth: true
            options: dialog.rateOptions
            current: dialog.rateId(dialog.fps)
            onChosen: id => {
                dialog.fps = Number(id)
                dialog.presetId = ""
            }
        }

        // r07: the labels sit outside the boxes (a long unit inside cut the numbers off), each box fits five digits and
        // its "px", and the pairs wrap under each other in a narrow dialog.
        Label { text: qsTr("Frame size") }
        Flow {
            objectName: "sequenceSettingsFrameSize"
            Layout.fillWidth: true
            spacing: Theme.space(3)

            Row {
                spacing: Theme.space(1.5)
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Width")
                }
                NumberField {
                    objectName: "sequenceSettingsWidth"
                    width: dialog.frameFieldWidth
                    from: 16
                    to: 16384
                    value: dialog.frameWidth
                    suffix: qsTr("px")
                    Accessible.name: qsTr("Frame width")
                    onValueModified: value => {
                        dialog.frameWidth = value
                        dialog.presetId = ""
                    }
                }
            }
            Row {
                spacing: Theme.space(1.5)
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Height")
                }
                NumberField {
                    objectName: "sequenceSettingsHeight"
                    width: dialog.frameFieldWidth
                    from: 16
                    to: 16384
                    value: dialog.frameHeight
                    suffix: qsTr("px")
                    Accessible.name: qsTr("Frame height")
                    onValueModified: value => {
                        dialog.frameHeight = value
                        dialog.presetId = ""
                    }
                }
            }
            // The frame's shape ("16:9"), as Premiere shows it next to the size.
            Label {
                objectName: "sequenceSettingsAspect"
                height: Theme.controlHeightSmall
                verticalAlignment: Text.AlignVCenter
                text: dialog.aspectText
                color: Theme.textTertiary
            }
        }

        Label { text: qsTr("Pixel aspect ratio") }
        EditingChoice {
            Layout.fillWidth: true
            options: dialog.aspectOptions
            current: dialog.aspectId(dialog.pixelAspect)
            onChosen: id => dialog.pixelAspect = Number(id)
        }

        Label { text: qsTr("Fields") }
        EditingChoice {
            Layout.fillWidth: true
            options: dialog.fieldOptions
            current: dialog.fields
            onChosen: id => dialog.fields = id
        }

        Label { text: qsTr("Display format") }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            EditingChoice {
                objectName: "sequenceSettingsVideoDisplay"
                options: dialog.videoDisplayOptions
                current: dialog.videoDisplay
                onChosen: id => dialog.videoDisplay = id
            }
            Toggle {
                objectName: "sequenceSettingsDropFrame"
                text: qsTr("Drop-frame")
                enabled: dialog.ntsc
                checked: dialog.ntsc && dialog.dropFrame
                onToggled: dialog.dropFrame = checked
            }
        }

        Label { text: qsTr("Start time") }
        FieldInput {
            id: startField
            objectName: "sequenceSettingsStartTime"
            Layout.preferredWidth: Theme.space(40)
            font.family: Theme.fontMono
            Accessible.name: qsTr("Start time")
        }

        // 0.7 (card 046): the sequence's work area (Alt+[ and Alt+] set it at the playhead; interchange files carry it).
        Label { text: qsTr("Work area") }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                objectName: "sequenceSettingsWorkArea"
                Layout.fillWidth: true
                text: dialog.workAreaIn === "" || dialog.clearWorkArea ? qsTr("Not set: the whole sequence")
                                                                         //: Sequence Settings: the work area's In and Out timecodes
                                                                         : qsTr("%1 to %2").arg(dialog.workAreaIn).arg(dialog.workAreaOut)
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textMd
                elide: Text.ElideRight
                Accessible.name: qsTr("Work area")
            }
            PillButton {
                objectName: "sequenceSettingsClearWorkArea"
                compact: true
                visible: dialog.workAreaIn !== "" && !dialog.clearWorkArea
                text: qsTr("Clear")
                tooltip: qsTr("Remove the work area when OK is pressed")
                onClicked: dialog.clearWorkArea = true
            }
        }

        Label { text: qsTr("Audio sample rate") }
        EditingChoice {
            Layout.fillWidth: true
            options: dialog.audioRateOptions
            current: String(dialog.audioRate)
            onChosen: id => dialog.audioRate = Number(id)
        }

        Label { text: qsTr("Audio display format") }
        EditingChoice {
            Layout.fillWidth: true
            options: dialog.audioDisplayOptions
            current: dialog.audioDisplay
            onChosen: id => dialog.audioDisplay = id
        }

        Label { text: qsTr("Preview file format") }
        EditingChoice {
            Layout.fillWidth: true
            options: dialog.previewOptions
            current: dialog.previewFormat
            onChosen: id => dialog.previewFormat = id
        }

        Label { text: qsTr("VR video") }
        EditingChoice {
            objectName: "sequenceSettingsVr"
            Layout.fillWidth: true
            options: dialog.vrOptions
            current: dialog.vrProjection
            onChosen: id => dialog.vrProjection = id
        }

        Label {
            visible: dialog.vrProjection !== "none"
            text: qsTr("Stereoscopic layout")
        }
        EditingChoice {
            objectName: "sequenceSettingsVrStereo"
            Layout.fillWidth: true
            visible: dialog.vrProjection !== "none"
            options: dialog.vrStereoOptions
            current: dialog.vrStereo
            onChosen: id => dialog.vrStereo = id
        }
    }
    Text {
        objectName: "sequenceSettingsVrHint"
        Layout.fillWidth: true
        visible: dialog.vrProjection !== "none"
        text: dialog.vrShapeFits
              ? qsTr("The Program Monitor can look around in this sequence (wrench > VR Video View) and exports are marked as VR video (MP4, MKV, WebM).")
              : qsTr("The frame should be 2:1 for one eye of a 360 picture (such as 3840 × 1920) and 1:1 for 180; set the frame size to match.")
        color: dialog.vrShapeFits ? Theme.textTertiary : Theme.warningText
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("A new timebase keeps every clip where it is in time and puts each edit point on the nearest frame of the new rate.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "sequenceSettingsError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "sequenceSettingsOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.sequenceKey !== ""
            onClicked: dialog.apply()
        }
    ]
}
