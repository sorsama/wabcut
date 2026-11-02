pragma ComponentBehavior: Bound
import QtQuick

// 0.6 (#57): a caption file format picker - a pill naming the format (".srt", "TTML", ...) that opens a menu of
// Premiere's caption file formats (interchange/CaptionFiles.h). Used by the export settings' Captions row
// (CaptionExportChooser), Export Timeline's caption sidecars and the Captions page's Export all tracks.
PillButton {
    id: button

    /// The format id ("srt", "vtt", "scc", "mcc", "stl", "ttml", "smpte-tt", "dfxp", "itt").
    property string format: "srt"
    /// Only these format ids (empty: every one).
    property var only: []

    signal picked(string format)

    readonly property var allFormats: [
        { id: "srt", label: qsTr("SubRip Subtitle Format (.srt)"), short: ".srt", ext: "srt" },
        { id: "vtt", label: qsTr("WebVTT File (.vtt)"), short: ".vtt", ext: "vtt" },
        { id: "scc", label: qsTr("Scenarist Closed Caption File (.scc)"), short: ".scc", ext: "scc" },
        { id: "mcc", label: qsTr("MacCaption VANC File (.mcc)"), short: ".mcc", ext: "mcc" },
        { id: "stl", label: qsTr("EBU N19 Subtitle File (.stl)"), short: ".stl", ext: "stl" },
        { id: "ttml", label: qsTr("W3C Timed Text File (.xml)"), short: "TTML", ext: "xml" },
        { id: "smpte-tt", label: qsTr("SMPTE Timed Text File (.xml)"), short: "SMPTE-TT", ext: "xml" },
        { id: "dfxp", label: qsTr("DFXP Timed Text File (.dfxp)"), short: ".dfxp", ext: "dfxp" },
        { id: "itt", label: qsTr("iTunes Timed Text File (.itt)"), short: ".itt", ext: "itt" }
    ]
    readonly property var formats: only.length > 0 ? allFormats.filter(f => only.indexOf(f.id) >= 0) : allFormats
    readonly property var current: formats.find(f => f.id === format) ?? formats[0]

    compact: true
    variant: "secondary"
    text: current.short
    iconName: "chevron-down"
    LayoutMirroring.enabled: true    // label first, chevron last
    LayoutMirroring.childrenInherit: true
    tooltip: current.label
    Accessible.name: qsTr("Caption file format: %1").arg(current.label)
    onClicked: formatMenu.popup(button, 0, button.height + Theme.space(1))

    ContextMenu {
        id: formatMenu

        Instantiator {
            model: button.formats
            delegate: ContextMenuItem {
                required property var modelData
                objectName: "captionFormat_" + modelData.id
                text: modelData.label
                iconName: "captions"
                checkable: true
                checked: modelData.id === button.format
                onTriggered: {
                    button.format = modelData.id
                    button.picked(modelData.id)
                }
            }
            onObjectAdded: (index, object) => formatMenu.insertItem(index, object as ContextMenuItem)
            onObjectRemoved: (index, object) => formatMenu.removeItem(object as ContextMenuItem)
        }
    }
}
