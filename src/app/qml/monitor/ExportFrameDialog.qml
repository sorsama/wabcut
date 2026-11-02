pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// P.2 (E6): Premiere's Export Frame dialog (the camera in both monitors, Ctrl+Shift+E; bridge Monitors;
// docs/monitors.md): the name, the format (DPX, JPEG, PNG, TIFF, Targa, BMP, GIF, OpenEXR), the folder and "Import
// into project". The Program Monitor exports the sequence frame at the playhead, the Source Monitor its clip's frame.
// P1 (#23): a Size (Sequence size, YouTube thumbnail 1280×720, Vertical 1080×1920, Custom) with "Crop to fill", a JPEG
// quality and "Keep under 2 MB" (YouTube's thumbnail limit; on with the thumbnail size), all remembered. Its markers mode
// (openForMarkers: Markers > Export Frames at Markers…, the Highlights panel) writes one still per marker - every one,
// the chapter markers or the highlight markers' peaks - named after it, in the background with progress and Cancel.
WabcutDialog {
    id: dialog
    objectName: "exportFrameDialog"

    property bool source: false
    /// "frame" (one frame) or "markers" (a still per marker).
    property string mode: "frame"
    property var defaults: ({})
    property string format: "png"
    property url folder
    property string errorText: ""
    // P1 (#23): the size, crop, quality and size cap.
    property string size: "sequence"
    property bool fill: true
    property int quality: 100
    property bool keepUnder: true
    /// Markers mode: "all", "chapters" or "highlights".
    property string markers: "all"
    property var counts: ({})
    readonly property bool jpeg: format === "jpeg"
    readonly property var qualities: [100, 90, 75, 60]
    readonly property var markerChoices: ["all", "chapters", "highlights"]
    readonly property int markerCount: counts[markers] ?? 0

    function openFor(fromSource) {
        dialog.mode = "frame"
        dialog.source = fromSource
        dialog.load()
        dialog.errorText = dialog.defaults.available === true ? ""
                         : (fromSource ? qsTr("Load a clip with a picture in the Source Monitor") : qsTr("Open a sequence first"))
        open()
        nameField.forceActiveFocus()
    }
    /// P1 (#23): Export Frames at Markers (`highlights`: the highlight markers' peaks first).
    function openForMarkers(highlights) {
        dialog.mode = "markers"
        dialog.source = false
        dialog.load()
        dialog.counts = Monitors.markerFrameCounts()
        dialog.markers = highlights ? "highlights" : (dialog.defaults.markers ?? "all")
        if (!highlights && (dialog.counts[dialog.markers] ?? 0) === 0)
            dialog.markers = "all"
        // From the Highlights panel these are thumbnail candidates: a YouTube thumbnail (a JPEG under 2 MB).
        if (highlights)
            dialog.chooseSize("thumbnail")
        dialog.errorText = dialog.defaults.available !== true ? qsTr("Open a sequence first")
                         : (dialog.counts.all ?? 0) === 0 ? qsTr("The sequence has no markers: add some first (M)") : ""
        open()
        markerTabs.forceActiveFocus()
    }
    function load() {
        dialog.defaults = Monitors.exportFrameDefaults(dialog.source)
        nameField.text = dialog.defaults.name ?? ""
        dialog.format = dialog.defaults.format ?? "png"
        dialog.folder = dialog.defaults.folder
        importToggle.checked = dialog.defaults.importIntoProject === true
        dialog.size = dialog.defaults.size ?? "sequence"
        widthField.text = String(dialog.defaults.width ?? 1280)
        heightField.text = String(dialog.defaults.height ?? 720)
        dialog.fill = dialog.defaults.fill !== false
        dialog.quality = dialog.defaults.quality ?? 100
        dialog.keepUnder = dialog.defaults.keepUnder !== false
    }
    function formatLabel(value) {
        const formats = dialog.defaults.formats ?? []
        for (let i = 0; i < formats.length; ++i) {
            if (formats[i].value === value)
                return formats[i].label
        }
        return value
    }
    /// The size, crop, quality and cap for Monitors.exportFrame / exportFramesAtMarkers.
    function options() {
        return {
            size: dialog.size,
            width: Number(widthField.text),
            height: Number(heightField.text),
            fill: dialog.fill,
            quality: dialog.quality,
            keepUnder: dialog.keepUnder
        }
    }
    /// A size picked: the YouTube thumbnail is a JPEG under 2 MB.
    function chooseSize(value) {
        dialog.size = value
        if (value === "thumbnail") {
            dialog.format = "jpeg"
            dialog.keepUnder = true
        }
    }
    function exportNow() {
        if (dialog.mode === "markers") {
            const settings = Object.assign(dialog.options(), {
                folder: dialog.folder,
                format: dialog.format,
                markers: dialog.markers,
                importIntoProject: importToggle.checked
            })
            if (Monitors.exportFramesAtMarkers(settings)) {
                if (!Monitors.framesBusy)
                    dialog.close() // finished at once (a test's synchronous job)
            } else {
                dialog.errorText = Monitors.message
            }
            return
        }
        const written = Monitors.exportFrame(dialog.source, nameField.text, dialog.format, dialog.folder, importToggle.checked,
                                             dialog.options())
        if (written !== "")
            dialog.close()
        else
            dialog.errorText = Monitors.message
    }

    title: dialog.mode === "markers" ? qsTr("Export Frames at Markers") : qsTr("Export Frame")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    Connections {
        target: Monitors
        function onFramesFinished(ok, message) {
            if (!dialog.visible || dialog.mode !== "markers")
                return
            if (ok)
                dialog.close()
            else
                dialog.errorText = message
        }
    }

    component RowLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(3)

        RowLabel {
            visible: dialog.mode === "frame"
            text: qsTr("Name")
        }
        FieldInput {
            id: nameField
            objectName: "exportFrameName"
            visible: dialog.mode === "frame"
            Layout.fillWidth: true
            Accessible.name: qsTr("File name")
            onAccepted: dialog.exportNow()
        }
        // P1 (#23): which markers get a still (each named after its marker).
        RowLabel {
            visible: dialog.mode === "markers"
            text: qsTr("Markers")
        }
        PillTabs {
            id: markerTabs
            objectName: "exportFrameMarkers"
            visible: dialog.mode === "markers"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("All (%1)").arg(dialog.counts.all ?? 0), qsTr("Chapters (%1)").arg(dialog.counts.chapters ?? 0),
                    qsTr("Highlights (%1)").arg(dialog.counts.highlights ?? 0)]
            currentIndex: Math.max(0, dialog.markerChoices.indexOf(dialog.markers))
            Accessible.name: qsTr("Markers to export")
            onActivated: index => dialog.markers = dialog.markerChoices[index]
        }
        RowLabel { text: qsTr("Format") }
        PillButton {
            id: formatButton
            objectName: "exportFrameFormat"
            Layout.alignment: Qt.AlignLeft
            text: dialog.formatLabel(dialog.format)
            iconName: "chevron-down"
            onClicked: formatMenu.popup(formatButton, 0, formatButton.height + Theme.space(1))

            ContextMenu {
                id: formatMenu
                Instantiator {
                    model: dialog.defaults.formats ?? []
                    delegate: ContextMenuItem {
                        required property var modelData
                        objectName: "exportFrameFormat_" + modelData.value
                        text: modelData.label
                        checkable: true
                        checked: dialog.format === modelData.value
                        onTriggered: dialog.format = modelData.value
                    }
                    onObjectAdded: (index, object) => formatMenu.insertItem(index, object as ContextMenuItem)
                    onObjectRemoved: (index, object) => formatMenu.removeItem(object as ContextMenuItem)
                }
            }
        }
        // P1 (#23): the still's size.
        RowLabel { text: qsTr("Size") }
        PillTabs {
            id: sizeTabs
            objectName: "exportFrameSize"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Sequence"), qsTr("YouTube thumbnail"), qsTr("Vertical"), qsTr("Custom")]
            currentIndex: Math.max(0, ["sequence", "thumbnail", "vertical", "custom"].indexOf(dialog.size))
            Accessible.name: qsTr("Still size")
            Accessible.description: qsTr("Sequence size, YouTube thumbnail 1280 by 720, vertical 1080 by 1920, or a custom size")
            onActivated: index => dialog.chooseSize(["sequence", "thumbnail", "vertical", "custom"][index])
        }
        RowLabel {
            visible: dialog.size === "custom"
            text: qsTr("Width × height")
        }
        RowLayout {
            visible: dialog.size === "custom"
            Layout.fillWidth: true
            spacing: Theme.space(2)
            FieldInput {
                id: widthField
                objectName: "exportFrameWidth"
                Layout.preferredWidth: Theme.space(24)
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 16; top: 16384 }
                Accessible.name: qsTr("Width in pixels")
            }
            Text {
                text: qsTr("×")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            FieldInput {
                id: heightField
                objectName: "exportFrameHeight"
                Layout.preferredWidth: Theme.space(24)
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 16; top: 16384 }
                Accessible.name: qsTr("Height in pixels")
            }
            Item { Layout.fillWidth: true }
        }
        RowLabel {
            visible: dialog.size !== "sequence"
            text: qsTr("Framing")
        }
        Toggle {
            objectName: "exportFrameFill"
            visible: dialog.size !== "sequence"
            text: qsTr("Crop to fill (else black bars)")
            checked: dialog.fill
            onToggled: dialog.fill = checked
        }
        // P1 (#23): JPEG's quality, and YouTube's 2 MB limit for thumbnails.
        RowLabel {
            visible: dialog.jpeg
            text: qsTr("Quality")
        }
        PillTabs {
            objectName: "exportFrameQuality"
            visible: dialog.jpeg
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Best"), qsTr("High"), qsTr("Medium"), qsTr("Small")]
            currentIndex: Math.max(0, dialog.qualities.indexOf(dialog.quality))
            Accessible.name: qsTr("JPEG quality")
            onActivated: index => dialog.quality = dialog.qualities[index]
        }
        RowLabel {
            visible: dialog.jpeg
            text: qsTr("Size limit")
        }
        Toggle {
            objectName: "exportFrameKeepUnder"
            visible: dialog.jpeg
            text: qsTr("Keep under 2 MB (YouTube thumbnails)")
            checked: dialog.keepUnder
            onToggled: dialog.keepUnder = checked
        }
        RowLabel { text: dialog.mode === "markers" ? qsTr("Folder") : qsTr("Path") }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                objectName: "exportFrameFolder"
                Layout.fillWidth: true
                text: decodeURIComponent(dialog.folder.toString().replace(/^file:\/\/\/?/, ""))
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                variant: "ghost"
                compact: true
                text: qsTr("Browse…")
                tooltip: qsTr("Choose the folder")
                onClicked: folderDialog.open()
            }
        }
    }
    Toggle {
        id: importToggle
        objectName: "exportFrameImport"
        text: dialog.mode === "markers" ? qsTr("Import into project (a Thumbnails bin)") : qsTr("Import into project")
    }
    // P1 (#23): Export Frames at Markers runs in the background: its progress, and Cancel.
    ColumnLayout {
        objectName: "exportFrameProgress"
        Layout.fillWidth: true
        visible: dialog.mode === "markers" && Monitors.framesBusy
        spacing: Theme.space(1)
        BeamProgress {
            Layout.fillWidth: true
            implicitHeight: Theme.space(2)
            value: Math.max(0, Monitors.framesProgress)
            indeterminate: Monitors.framesProgress < 0
        }
        Text {
            Layout.fillWidth: true
            text: Monitors.framesStatus
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }
    Text {
        objectName: "exportFrameError"
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
            objectName: "exportFrameCancel"
            variant: "ghost"
            text: dialog.mode === "markers" && Monitors.framesBusy ? qsTr("Stop") : qsTr("Cancel")
            tooltip: dialog.mode === "markers" && Monitors.framesBusy ? qsTr("Stop exporting the stills") : ""
            onClicked: {
                if (dialog.mode === "markers" && Monitors.framesBusy)
                    Monitors.cancelFramesAtMarkers()
                else
                    dialog.close()
            }
        },
        PillButton {
            objectName: "exportFrameOk"
            variant: "primary"
            text: dialog.mode === "markers" ? qsTr("Export %n still(s)", "", dialog.markerCount) : qsTr("OK")
            enabled: dialog.defaults.available === true && !Monitors.framesBusy
                     && (dialog.mode === "markers" ? dialog.markerCount > 0 : nameField.text.trim() !== "")
                     && (dialog.size !== "custom" || (widthField.acceptableInput && heightField.acceptableInput))
            onClicked: dialog.exportNow()
        }
    ]

    FolderDialog {
        id: folderDialog
        currentFolder: dialog.folder
        onAccepted: dialog.folder = selectedFolder
    }
}
