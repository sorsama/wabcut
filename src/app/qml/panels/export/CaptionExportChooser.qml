pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Captions row of the export settings (I.4; P.2: Premiere's Export Options): off, burned into the picture, a
// sidecar file next to the output in one of Premiere's caption file formats, or CEA-608 embedded in the H.264 / HEVC
// stream. `chosen` gives the ExportController.queueExport "captions" override (undefined = off). 0.6 (#73): the tabs follow
// `mode` (the export's current captions mode), so a preset's or a restored export's choice shows.
RowLayout {
    id: chooser

    /// The sidecar file format ("srt", "scc", ...; CaptionFiles.h).
    property string format: "srt"
    /// 0.6 (#73): the current mode - "none", "burnIn", a sidecar format or "embed" (ExportController.describe's captionMode).
    property string mode: "none"

    signal chosen(var value)

    /// The sidecar formats (CaptionFormatButton's list, 0.6).
    readonly property var formats: formatButton.allFormats
    readonly property var current: formatButton.current

    function syncMode() {
        if (mode === "burnIn") {
            tabs.currentIndex = 1
        } else if (mode === "embed") {
            tabs.currentIndex = 3
        } else if (formats.some(f => f.id === mode)) {
            format = mode
            tabs.currentIndex = 2
        } else {
            tabs.currentIndex = 0
        }
    }
    onModeChanged: syncMode()
    Component.onCompleted: syncMode()

    function emitChoice() {
        const i = tabs.currentIndex
        chosen(i === 0 ? undefined : i === 1 ? "burnIn" : i === 2 ? format : "embed")
    }

    spacing: Theme.space(2)

    PillTabs {
        id: tabs
        objectName: "exportCaptionsTabs"
        Layout.fillWidth: true
        compact: true
        model: [qsTr("Off"), qsTr("Burn in"), qsTr("Sidecar file"), qsTr("Embed")]
        Accessible.name: qsTr("Captions")
        onActivated: chooser.emitChoice()
    }

    CaptionFormatButton {
        id: formatButton
        objectName: "exportCaptionsFormat"
        visible: tabs.currentIndex === 2
        format: chooser.format
        onPicked: id => {
            chooser.format = id
            chooser.emitChoice()
        }
    }
}
