pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Captions row of the export settings (I.4; P.2: Premiere's Export Options): off, burned into the picture, a
// sidecar file next to the output in one of Premiere's caption file formats, or CEA-608 embedded in the H.264 / HEVC
// stream. `chosen` gives the ExportController.queueExport "captions" override (undefined = off).
RowLayout {
    id: chooser

    /// The sidecar file format ("srt", "scc", ...; CaptionFiles.h).
    property string format: "srt"

    signal chosen(var value)

    readonly property var formats: [
        { id: "srt", label: qsTr("SubRip Subtitle Format (.srt)"), short: ".srt" },
        { id: "vtt", label: qsTr("WebVTT File (.vtt)"), short: ".vtt" },
        { id: "scc", label: qsTr("Scenarist Closed Caption File (.scc)"), short: ".scc" },
        { id: "mcc", label: qsTr("MacCaption VANC File (.mcc)"), short: ".mcc" },
        { id: "stl", label: qsTr("EBU N19 Subtitle File (.stl)"), short: ".stl" },
        { id: "ttml", label: qsTr("W3C Timed Text File (.xml)"), short: "TTML" },
        { id: "smpte-tt", label: qsTr("SMPTE Timed Text File (.xml)"), short: "SMPTE-TT" },
        { id: "dfxp", label: qsTr("DFXP Timed Text File (.dfxp)"), short: ".dfxp" },
        { id: "itt", label: qsTr("iTunes Timed Text File (.itt)"), short: ".itt" }
    ]
    readonly property var current: formats.find(f => f.id === format) ?? formats[0]

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

    PillButton {
        id: formatButton
        objectName: "exportCaptionsFormat"
        visible: tabs.currentIndex === 2
        compact: true
        variant: "secondary"
        text: chooser.current.short
        iconName: "chevron-down"
        LayoutMirroring.enabled: true    // label first, chevron last
        LayoutMirroring.childrenInherit: true
        Accessible.name: qsTr("Caption file format: %1").arg(chooser.current.label)
        onClicked: formatMenu.popup(formatButton, 0, formatButton.height + Theme.space(1))

        ContextMenu {
            id: formatMenu

            Instantiator {
                model: chooser.formats
                delegate: ContextMenuItem {
                    required property var modelData
                    text: modelData.label
                    iconName: "captions"
                    checkable: true
                    checked: modelData.id === chooser.format
                    onTriggered: {
                        chooser.format = modelData.id
                        chooser.emitChoice()
                    }
                }
                onObjectAdded: (index, object) => formatMenu.insertItem(index, object as ContextMenuItem)
                onObjectRemoved: (index, object) => formatMenu.removeItem(object as ContextMenuItem)
            }
        }
    }
}
