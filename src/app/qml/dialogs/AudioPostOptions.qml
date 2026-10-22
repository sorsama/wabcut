pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// P.2: the AAF and OMF settings of File > Export Timeline (Premiere Pro's Export AAF and Export OMF settings): the new
// audio files (sample rate, bits, embedded or next to the file, WAV or AIFF, trimmed with handles or complete) and, for
// AAF, Mixdown Video, Breakout to Mono and the clip effects; for OMF, its title and pan. values() is the "audioPost"
// object of the interchange.export command (docs/commands.md).
GridLayout {
    id: root
    objectName: "audioPostOptions"

    /// OMF's settings instead of AAF's.
    property bool omf: false
    /// The OMF title's placeholder: the sequence's name.
    property string sequenceName: ""

    columns: 2
    columnSpacing: Theme.space(3)
    rowSpacing: Theme.space(2)

    function values() {
        const v = {
            sampleRate: rateTabs.currentIndex === 1 ? 96000 : 48000,
            bitsPerSample: bitsTabs.currentIndex === 0 ? 16 : 24,
            files: filesTabs.currentIndex === 1 ? "separate" : "embed",
            audioFormat: formatTabs.currentIndex === 1 ? "aiff" : "wav",
            render: renderTabs.currentIndex === 1 ? "complete" : "trim",
            handleFrames: Math.round(handles.value)
        }
        if (root.omf) {
            v.omfTitle = omfTitle.text
            v.includePan = includePan.checked
        } else {
            v.mixdownVideo = mixdownVideo.checked
            v.breakoutToMono = breakout.checked
            v.renderAudioClipEffects = clipEffects.checked
            v.includeClipCopiesWithoutEffects = clipEffects.checked && cleanCopies.checked
        }
        return v
    }

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    Label { text: qsTr("OMF title"); visible: root.omf }
    FieldInput {
        id: omfTitle
        objectName: "audioPostOmfTitle"
        visible: root.omf
        Layout.fillWidth: true
        placeholderText: root.sequenceName
    }

    Label { text: qsTr("Sample rate") }
    PillTabs {
        id: rateTabs
        objectName: "audioPostSampleRate"
        Layout.fillWidth: true
        compact: true
        model: ["48000 Hz", "96000 Hz"]
    }

    Label { text: qsTr("Bits per sample") }
    PillTabs {
        id: bitsTabs
        objectName: "audioPostBits"
        Layout.fillWidth: true
        compact: true
        model: ["16", "24"]
        currentIndex: 1
    }

    Label { text: qsTr("Files") }
    PillTabs {
        id: filesTabs
        objectName: "audioPostFiles"
        Layout.fillWidth: true
        compact: true
        model: [root.omf ? qsTr("Encapsulate") : qsTr("Embed audio"), qsTr("Separate audio")]
    }

    Label { text: qsTr("Format") }
    PillTabs {
        id: formatTabs
        objectName: "audioPostFormat"
        Layout.fillWidth: true
        compact: true
        model: [root.omf ? "WAV" : qsTr("Broadcast WAV"), "AIFF"] // file format names stay as they are
    }

    Label { text: qsTr("Render") }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        PillTabs {
            id: renderTabs
            objectName: "audioPostRender"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Trim audio files"), qsTr("Copy complete files")]
        }
        NumberField {
            id: handles
            objectName: "audioPostHandles"
            enabled: renderTabs.currentIndex === 0
            Layout.preferredWidth: Theme.space(24)
            value: 24
            from: 0
            to: 1000
            suffix: qsTr("fr")
            Accessible.name: qsTr("Handle frames")
            onValueModified: v => handles.value = v
        }
    }

    Label { text: qsTr("Pan"); visible: root.omf }
    Toggle {
        id: includePan
        objectName: "audioPostIncludePan"
        visible: root.omf
        text: qsTr("Include pan")
        checked: true
    }

    Label { text: qsTr("Audio"); visible: !root.omf }
    ColumnLayout {
        visible: !root.omf
        spacing: Theme.space(1)
        Toggle {
            id: breakout
            objectName: "audioPostBreakout"
            text: qsTr("Breakout to mono")
            tooltip: qsTr("Render new mono files; stereo tracks become Left and Right tracks. Off: link the original media.")
            checked: true
        }
        Toggle {
            id: clipEffects
            objectName: "audioPostClipEffects"
            text: qsTr("Render audio clip effects")
        }
        Toggle {
            id: cleanCopies
            objectName: "audioPostCleanCopies"
            enabled: clipEffects.checked
            text: qsTr("Include clip copies without effects")
        }
    }

    Label { text: qsTr("Video"); visible: !root.omf }
    Toggle {
        id: mixdownVideo
        objectName: "audioPostMixdownVideo"
        visible: !root.omf
        text: qsTr("Mixdown video")
        tooltip: qsTr("Render the picture to one reference movie next to the AAF")
    }
}
