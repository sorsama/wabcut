pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The New project page's Advanced settings (design language v3: a label and a pill control per row, no prose): frame
// size (even numbers, 16-8192, swap orientation), frame rate (Home.rates), drop-frame timecode (29.97 / 59.94 only),
// pixel aspect, fields, start timecode, audio sample rate, audio master, video and audio tracks with their format,
// colour management (Color.colorManagementModes) and the preview file format. The values come in as properties; edits
// go out as signals (the page keeps them and remembers them for next time).
GridLayout {
    id: grid
    objectName: "homeAdvancedSettings"

    property int frameWidth: 1920
    property int frameHeight: 1080
    property int rateNum: 30
    property int rateDen: 1
    property bool canDropFrame: false
    /// Home.createProject's Advanced keys (Home.advancedDefaults' shape).
    property var advanced: ({})
    readonly property var choices: Home.advancedChoices

    signal sizeEdited(int width, int height)
    signal rateChosen(int rateNum, int rateDen)
    signal valueEdited(string key, var value)

    /// An even number from 16 to 8192.
    function evenSize(v: real): int {
        return Math.max(16, Math.min(8192, Math.round(v / 2) * 2))
    }
    function aspectId(): string {
        const a = grid.advanced.pixelAspect ?? { num: 1, den: 1 }
        for (const p of grid.choices.pixelAspects) {
            if (p.num * a.den === a.num * p.den)
                return p.id
        }
        return "square"
    }

    columns: 2
    columnSpacing: Theme.space(4)
    rowSpacing: Theme.space(2)

    component Label: Text {
        Layout.alignment: Qt.AlignVCenter
        Layout.preferredWidth: Theme.space(30)
        elide: Text.ElideRight
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    // Frame size
    Label { text: qsTr("Frame size") }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1.5)
        NumberField {
            objectName: "homeFrameWidth"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 16
            to: 8192
            value: grid.frameWidth
            suffix: qsTr("px")
            Accessible.name: qsTr("Frame width in pixels")
            onValueModified: v => grid.sizeEdited(grid.evenSize(v), grid.frameHeight)
        }
        Icon {
            name: "x"
            size: Theme.iconXs
            color: Theme.textTertiary
        }
        NumberField {
            objectName: "homeFrameHeight"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 16
            to: 8192
            value: grid.frameHeight
            suffix: qsTr("px")
            Accessible.name: qsTr("Frame height in pixels")
            onValueModified: v => grid.sizeEdited(grid.frameWidth, grid.evenSize(v))
        }
        IconButton {
            objectName: "homeSwapOrientation"
            compact: true
            iconName: "arrow-left-right"
            iconSize: Theme.iconXs
            tooltip: qsTr("Swap width and height")
            onClicked: grid.sizeEdited(grid.frameHeight, grid.frameWidth)
        }
        Item { Layout.fillWidth: true }
    }

    // Frame rate and drop-frame timecode
    Label { text: qsTr("Frame rate") }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        EditingChoice {
            objectName: "homeFrameRate"
            options: Home.rates.map(r => ({ id: r.label, name: qsTr("%1 fps").arg(r.label) }))
            current: Home.rateLabel(grid.rateNum, grid.rateDen)
            Accessible.name: qsTr("Frame rate: %1").arg(text)
            onChosen: id => {
                const r = Home.rates.find(r => r.label === id)
                if (r)
                    grid.rateChosen(r.rateNum, r.rateDen)
            }
        }
        Toggle {
            objectName: "homeDropFrame"
            visible: grid.canDropFrame
            text: qsTr("Drop-frame")
            tooltip: qsTr("Drop-frame timecode keeps timecode in step with the clock at %1 fps").arg(Home.rateLabel(grid.rateNum, grid.rateDen))
            checked: grid.advanced.dropFrame === true
            onToggled: grid.valueEdited("dropFrame", checked)
        }
        Item { Layout.fillWidth: true }
    }

    // Pixel aspect and fields
    Label { text: qsTr("Pixel aspect") }
    RowLayout {
        Layout.fillWidth: true
        EditingChoice {
            objectName: "homePixelAspect"
            options: grid.choices.pixelAspects.map(p => ({ id: p.id, name: p.label }))
            current: grid.aspectId()
            Accessible.name: qsTr("Pixel aspect: %1").arg(text)
            onChosen: id => {
                const p = grid.choices.pixelAspects.find(p => p.id === id)
                if (p)
                    grid.valueEdited("pixelAspect", { num: p.num, den: p.den })
            }
        }
        Item { Layout.fillWidth: true }
    }
    Label { text: qsTr("Fields") }
    RowLayout {
        Layout.fillWidth: true
        EditingChoice {
            objectName: "homeFields"
            options: grid.choices.fields.map(f => ({ id: f.id, name: f.label }))
            current: grid.advanced.fields ?? "progressive"
            Accessible.name: qsTr("Fields: %1").arg(text)
            onChosen: id => grid.valueEdited("fields", id)
        }
        Item { Layout.fillWidth: true }
    }

    // Start timecode
    Label { text: qsTr("Start time") }
    RowLayout {
        Layout.fillWidth: true
        FieldInput {
            id: startField
            objectName: "homeStartTimecode"
            Layout.preferredWidth: Theme.space(34)
            text: grid.advanced.startTimecode ?? "00:00:00:00"
            font.family: Theme.fontMono
            maximumLength: 16
            Accessible.name: qsTr("Start timecode")
            onEditingFinished: {
                if (startField.text.trim() !== (grid.advanced.startTimecode ?? ""))
                    grid.valueEdited("startTimecode", startField.text.trim())
            }
        }
        Item { Layout.fillWidth: true }
    }

    // Audio
    Label { text: qsTr("Sample rate") }
    PillTabs {
        objectName: "homeAudioRate"
        Layout.fillWidth: true
        Layout.maximumWidth: Theme.space(66)
        compact: true
        model: grid.choices.audioRates.map(r => r.label)
        currentIndex: Math.max(0, grid.choices.audioRates.findIndex(r => r.value === grid.advanced.audioRate))
        Accessible.name: qsTr("Audio sample rate")
        onActivated: index => grid.valueEdited("audioRate", grid.choices.audioRates[index].value)
    }
    Label { text: qsTr("Audio master") }
    PillTabs {
        objectName: "homeMasterFormat"
        Layout.fillWidth: true
        Layout.maximumWidth: Theme.space(44)
        compact: true
        model: grid.choices.masterFormats.map(f => f.label)
        currentIndex: Math.max(0, grid.choices.masterFormats.findIndex(f => f.id === grid.advanced.masterFormat))
        Accessible.name: qsTr("Audio master")
        onActivated: index => grid.valueEdited("masterFormat", grid.choices.masterFormats[index].id)
    }

    // Tracks
    Label { text: qsTr("Video tracks") }
    RowLayout {
        Layout.fillWidth: true
        NumberField {
            objectName: "homeVideoTracks"
            Layout.preferredWidth: Theme.space(16)
            from: 1
            to: 99
            value: grid.advanced.videoTracks ?? 3
            Accessible.name: qsTr("Video tracks")
            onValueModified: v => grid.valueEdited("videoTracks", Math.round(v))
        }
        Item { Layout.fillWidth: true }
    }
    Label { text: qsTr("Audio tracks") }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        NumberField {
            objectName: "homeAudioTracks"
            Layout.preferredWidth: Theme.space(16)
            from: 1
            to: 99
            value: grid.advanced.audioTracks ?? 3
            Accessible.name: qsTr("Audio tracks")
            onValueModified: v => grid.valueEdited("audioTracks", Math.round(v))
        }
        PillTabs {
            objectName: "homeTrackFormat"
            Layout.fillWidth: true
            Layout.maximumWidth: Theme.space(44)
            compact: true
            model: grid.choices.trackFormats.map(f => f.label)
            currentIndex: Math.max(0, grid.choices.trackFormats.findIndex(f => f.id === grid.advanced.trackFormat))
            Accessible.name: qsTr("Audio track format")
            onActivated: index => grid.valueEdited("trackFormat", grid.choices.trackFormats[index].id)
        }
        Item { Layout.fillWidth: true }
    }

    // Colour and previews
    Label { text: qsTr("Color management") }
    RowLayout {
        Layout.fillWidth: true
        EditingChoice {
            objectName: "homeColorManagement"
            options: Color.colorManagementModes.map(m => ({ id: m.id, name: m.name }))
            current: grid.advanced.colorManagement?.mode ?? "rec709"
            Accessible.name: qsTr("Color management: %1").arg(text)
            onChosen: id => grid.valueEdited("colorManagement", { mode: id })
        }
        Item { Layout.fillWidth: true }
    }
    Label { text: qsTr("Preview files") }
    PillTabs {
        objectName: "homePreviewFormat"
        Layout.fillWidth: true
        Layout.maximumWidth: Theme.space(80)
        compact: true
        model: grid.choices.previewFormats.map(f => f.label)
        currentIndex: Math.max(0, grid.choices.previewFormats.findIndex(f => f.id === (grid.advanced.previewFormat ?? "")))
        Accessible.name: qsTr("Preview file format")
        onActivated: index => grid.valueEdited("previewFormat", grid.choices.previewFormats[index].id)
    }
}
