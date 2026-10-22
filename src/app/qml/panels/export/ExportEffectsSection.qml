pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// The Export workspace's Effects, Metadata and render options (0.2; Premiere's Export Settings > Effects, Metadata and
// the General / Video toggles): Lumetri Look/LUT, Image Overlay, Name Overlay, Timecode Overlay, Video Limiter and
// Loudness Normalization applied to the output; chapters from chapter markers, the start timecode and XMP; Use Maximum
// Render Quality, Render at Maximum Depth, Use Previews and Smart Render. Controlled like the settings drawer: it shows
// `overrides` (its "effects", "metadata" and "render" objects, in the export library's JSON) and reports a whole new
// object through overrideChanged(key, value).
Item {
    id: root

    property var summary: ({})
    property var overrides: ({})
    property bool open: false

    signal overrideChanged(string key, var value)

    readonly property bool hasVideo: summary.ok === true && summary.resolution !== undefined && !(summary.audioOnly ?? false)
    readonly property bool hasAudio: summary.ok === true && summary.audio !== undefined && !String(summary.audio).startsWith(qsTr("No audio"))

    readonly property var defaultEffects: ({
        look: { applied: false, lut: "builtin:teal-orange" },
        limiter: { applied: false, clipLevel: 100, compression: 3 },
        imageOverlay: { applied: false, image: "", position: "bottomRight", offsetX: -32, offsetY: -32, size: 100, opacity: 100 },
        nameOverlay: { applied: false, prefix: "", suffix: "", format: "sourceName", position: "bottomCenter", offsetX: 0,
                       offsetY: -48, size: 100, opacity: 100 },
        timecodeOverlay: { applied: false, source: "sequence", startTimecode: 0, display: "timecode", position: "topCenter",
                           offsetX: 0, offsetY: 32, size: 100, opacity: 100 },
        loudness: { applied: false, standard: "ebuR128", target: -23, tolerance: 1, maxTruePeak: -1, writeLog: false }
    })
    readonly property var defaultMetadata: ({ chapters: true, timecode: true, startTimecode: null, xmp: "none", title: "",
                                              description: "", xmpMarkers: true })
    readonly property var defaultRender: ({ maximumRenderQuality: false, maximumDepth: false, usePreviews: false,
                                            smartRender: true })

    readonly property var fx: merged(defaultEffects, overrides.effects)
    readonly property var meta: Object.assign({}, defaultMetadata, overrides.metadata ?? {})
    readonly property var renderOptions: Object.assign({}, defaultRender, overrides.render ?? {})

    readonly property var looks: ["builtin:teal-orange", "builtin:warm-film", "builtin:cool-night", "builtin:bleach-bypass",
                                  "builtin:mono-silver"]
    readonly property var standards: ["ebuR128", "atscA85", "custom"]
    readonly property var xmpModes: ["none", "embed", "sidecar"]

    function merged(defaults, given) {
        const out = {}
        for (const key in defaults)
            out[key] = Object.assign({}, defaults[key], (given ?? {})[key] ?? {})
        return out
    }
    function setEffect(section, key, value) {
        const next = merged(defaultEffects, overrides.effects)
        next[section][key] = value
        root.overrideChanged("effects", next)
    }
    function setMeta(key, value) {
        const next = Object.assign({}, meta)
        next[key] = value
        root.overrideChanged("metadata", next)
    }
    function setRender(key, value) {
        const next = Object.assign({}, renderOptions)
        next[key] = value
        root.overrideChanged("render", next)
    }
    function fileText(path) {
        return String(path).replace(/^file:\/\/\/?/, "")
    }

    implicitHeight: header.height + body.height
    clip: true

    component GroupLabel: Text {
        Layout.topMargin: Theme.space(2)
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }
    // Premiere's Position: the nine places of the output frame.
    component PositionGrid: Grid {
        id: grid
        property string value: "center"
        readonly property var names: ["topLeft", "topCenter", "topRight", "centerLeft", "center", "centerRight",
                                      "bottomLeft", "bottomCenter", "bottomRight"]
        signal picked(string position)
        columns: 3
        spacing: 2
        Repeater {
            model: 9
            delegate: Rectangle {
                id: cell
                required property int index
                width: Theme.space(4)
                height: Theme.space(3)
                radius: 2
                color: grid.names[cell.index] === grid.value ? Theme.accentFill : cellArea.containsMouse ? Theme.hover : Theme.raised
                border.width: Theme.hairline
                border.color: Theme.border
                Accessible.role: Accessible.RadioButton
                Accessible.name: grid.names[cell.index]
                MouseArea {
                    id: cellArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: grid.picked(grid.names[cell.index])
                }
            }
        }
    }
    // Position, Offset, Size and Opacity of an overlay; wraps in a narrow panel.
    component Placement: Flow {
        id: placement
        property var values: ({})
        signal edited(string key, var value)
        spacing: Theme.space(2)
        PositionGrid {
            value: placement.values.position ?? "center"
            onPicked: position => placement.edited("position", position)
        }
        NumberField {
            width: Theme.space(18)
            value: placement.values.offsetX ?? 0
            from: -4096
            to: 4096
            suffix: "x"
            Accessible.name: qsTr("Offset X")
            onValueModified: v => placement.edited("offsetX", v)
        }
        NumberField {
            width: Theme.space(18)
            value: placement.values.offsetY ?? 0
            from: -4096
            to: 4096
            suffix: "y"
            Accessible.name: qsTr("Offset Y")
            onValueModified: v => placement.edited("offsetY", v)
        }
        NumberField {
            width: Theme.space(18)
            value: placement.values.size ?? 100
            from: 1
            to: 1000
            suffix: "%"
            Accessible.name: qsTr("Size")
            onValueModified: v => placement.edited("size", v)
        }
        NumberField {
            width: Theme.space(18)
            value: placement.values.opacity ?? 100
            from: 0
            to: 100
            suffix: "%"
            Accessible.name: qsTr("Opacity")
            onValueModified: v => placement.edited("opacity", v)
        }
    }

    // ---- Header ----
    Rectangle {
        id: header
        width: root.width
        height: Theme.controlHeight
        radius: height / 2
        color: headerArea.containsMouse ? Theme.hover : Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border
        activeFocusOnTab: true
        objectName: "exportEffectsHeader"
        Accessible.role: Accessible.Button
        Accessible.name: root.open ? qsTr("Hide effects and metadata") : qsTr("Show effects and metadata")
        Keys.onSpacePressed: root.open = !root.open
        Keys.onReturnPressed: root.open = !root.open

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.space(4)
            anchors.rightMargin: Theme.space(3)
            spacing: Theme.space(2)
            Text {
                text: qsTr("Effects & Metadata")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                text: {
                    const on = []
                    if (root.fx.look.applied) on.push(qsTr("Look"))
                    if (root.fx.imageOverlay.applied) on.push(qsTr("Image"))
                    if (root.fx.nameOverlay.applied) on.push(qsTr("Name"))
                    if (root.fx.timecodeOverlay.applied) on.push(qsTr("Timecode"))
                    if (root.fx.limiter.applied) on.push(qsTr("Limiter"))
                    if (root.fx.loudness.applied) on.push(qsTr("Loudness"))
                    return on.join(" · ")
                }
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            Icon {
                name: "chevron-down"
                size: Theme.iconSm
                color: Theme.textSecondary
                rotation: root.open ? 180 : 0
            }
        }
        MouseArea {
            id: headerArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.open = !root.open
        }
        FocusRing {
            targetRadius: header.radius
            visible: header.activeFocus
        }
    }

    // ---- Body: one column, each effect a switch with its options under it while it is applied ----
    Item {
        id: body
        anchors.top: header.bottom
        width: root.width
        height: root.open ? rows.implicitHeight + Theme.space(3) : 0
        visible: root.open
        enabled: root.open

        ColumnLayout {
            id: rows
            y: Theme.space(3)
            width: parent.width
            spacing: Theme.space(2)

            GroupLabel { text: qsTr("Effects") }

            // Lumetri Look/LUT
            Toggle {
                objectName: "exportLookApplied"
                visible: root.hasVideo
                text: qsTr("Lumetri Look/LUT")
                checked: root.fx.look.applied
                onToggled: root.setEffect("look", "applied", checked)
            }
            Flow {
                visible: root.hasVideo && root.fx.look.applied
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space(6)
                spacing: Theme.space(1)
                Repeater {
                    model: [{ lut: "builtin:teal-orange", label: qsTr("Teal-orange") },
                            { lut: "builtin:warm-film", label: qsTr("Warm film") },
                            { lut: "builtin:cool-night", label: qsTr("Cool night") },
                            { lut: "builtin:bleach-bypass", label: qsTr("Bleach bypass") },
                            { lut: "builtin:mono-silver", label: qsTr("Mono silver") }]
                    delegate: PillButton {
                        required property var modelData
                        compact: true
                        variant: root.fx.look.lut === modelData.lut ? "primary" : "secondary"
                        text: modelData.label
                        onClicked: root.setEffect("look", "lut", modelData.lut)
                    }
                }
                PillButton {
                    compact: true
                    variant: root.looks.indexOf(root.fx.look.lut) < 0 ? "primary" : "secondary"
                    text: root.looks.indexOf(root.fx.look.lut) < 0 ? root.fileText(root.fx.look.lut).split(/[\\/]/).pop() : qsTr(".cube file…")
                    tooltip: qsTr("Choose a .cube LUT")
                    onClicked: lutDialog.open()
                }
            }

            // Image Overlay
            Toggle {
                objectName: "exportImageOverlayApplied"
                visible: root.hasVideo
                text: qsTr("Image Overlay")
                checked: root.fx.imageOverlay.applied
                onToggled: root.setEffect("imageOverlay", "applied", checked)
            }
            ColumnLayout {
                visible: root.hasVideo && root.fx.imageOverlay.applied
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space(6)
                spacing: Theme.space(1)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        text: root.fx.imageOverlay.image === "" ? qsTr("No image") : root.fileText(root.fx.imageOverlay.image)
                        elide: Text.ElideMiddle
                        color: root.fx.imageOverlay.image === "" ? Theme.textTertiary : Theme.textPrimary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textSm
                    }
                    PillButton {
                        compact: true
                        text: qsTr("Image…")
                        onClicked: imageDialog.open()
                    }
                }
                Placement {
                    Layout.fillWidth: true
                    values: root.fx.imageOverlay
                    onEdited: (key, value) => root.setEffect("imageOverlay", key, value)
                }
            }

            // Name Overlay
            Toggle {
                objectName: "exportNameOverlayApplied"
                visible: root.hasVideo
                text: qsTr("Name Overlay")
                checked: root.fx.nameOverlay.applied
                onToggled: root.setEffect("nameOverlay", "applied", checked)
            }
            ColumnLayout {
                visible: root.hasVideo && root.fx.nameOverlay.applied
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space(6)
                spacing: Theme.space(1)
                PillTabs {
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("Source name"), qsTr("Output name")]
                    currentIndex: root.fx.nameOverlay.format === "sourceName" ? 0 : 1
                    Accessible.name: qsTr("Format")
                    onActivated: index => root.setEffect("nameOverlay", "format", index === 0 ? "sourceName" : "outputName")
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    FieldInput {
                        Layout.fillWidth: true
                        text: root.fx.nameOverlay.prefix
                        placeholderText: qsTr("Prefix")
                        Accessible.name: qsTr("Prefix")
                        onEditingFinished: root.setEffect("nameOverlay", "prefix", text)
                    }
                    FieldInput {
                        Layout.fillWidth: true
                        text: root.fx.nameOverlay.suffix
                        placeholderText: qsTr("Suffix")
                        Accessible.name: qsTr("Suffix")
                        onEditingFinished: root.setEffect("nameOverlay", "suffix", text)
                    }
                }
                Placement {
                    Layout.fillWidth: true
                    values: root.fx.nameOverlay
                    onEdited: (key, value) => root.setEffect("nameOverlay", key, value)
                }
            }

            // Timecode Overlay
            Toggle {
                objectName: "exportTimecodeOverlayApplied"
                visible: root.hasVideo
                text: qsTr("Timecode Overlay")
                checked: root.fx.timecodeOverlay.applied
                onToggled: root.setEffect("timecodeOverlay", "applied", checked)
            }
            ColumnLayout {
                visible: root.hasVideo && root.fx.timecodeOverlay.applied
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space(6)
                spacing: Theme.space(1)
                PillTabs {
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("Sequence timecode"), qsTr("Generate")]
                    currentIndex: root.fx.timecodeOverlay.source === "generate" ? 1 : 0
                    Accessible.name: qsTr("Time Source")
                    onActivated: index => root.setEffect("timecodeOverlay", "source", index === 1 ? "generate" : "sequence")
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    PillTabs {
                        Layout.fillWidth: true
                        compact: true
                        model: [qsTr("Timecode"), qsTr("Frames")]
                        currentIndex: root.fx.timecodeOverlay.display === "frames" ? 1 : 0
                        Accessible.name: qsTr("Time Display")
                        onActivated: index => root.setEffect("timecodeOverlay", "display", index === 1 ? "frames" : "timecode")
                    }
                    FieldInput {
                        Layout.preferredWidth: Theme.space(32)
                        visible: root.fx.timecodeOverlay.source === "generate"
                        text: typeof root.fx.timecodeOverlay.startTimecode === "string" ? root.fx.timecodeOverlay.startTimecode : "00:00:00:00"
                        placeholderText: qsTr("Starting timecode")
                        Accessible.name: qsTr("Starting timecode")
                        onEditingFinished: root.setEffect("timecodeOverlay", "startTimecode", text)
                    }
                }
                Placement {
                    Layout.fillWidth: true
                    values: root.fx.timecodeOverlay
                    onEdited: (key, value) => root.setEffect("timecodeOverlay", key, value)
                }
            }

            // Video Limiter
            Toggle {
                objectName: "exportLimiterApplied"
                visible: root.hasVideo
                text: qsTr("Video Limiter")
                checked: root.fx.limiter.applied
                onToggled: root.setEffect("limiter", "applied", checked)
            }
            Flow {
                visible: root.hasVideo && root.fx.limiter.applied
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space(6)
                spacing: Theme.space(2)
                Text {
                    height: Theme.controlHeight
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("Clip Level")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                NumberField {
                    width: Theme.space(22)
                    value: root.fx.limiter.clipLevel
                    from: 90
                    to: 109
                    suffix: "IRE"
                    Accessible.name: qsTr("Clip Level")
                    onValueModified: v => root.setEffect("limiter", "clipLevel", v)
                }
                Text {
                    height: Theme.controlHeight
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("Compression Before Clipping")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                NumberField {
                    width: Theme.space(18)
                    value: root.fx.limiter.compression
                    from: 0
                    to: 20
                    suffix: "%"
                    Accessible.name: qsTr("Compression Before Clipping")
                    onValueModified: v => root.setEffect("limiter", "compression", v)
                }
            }

            // Loudness Normalization
            Toggle {
                objectName: "exportLoudnessApplied"
                visible: root.hasAudio
                text: qsTr("Loudness Normalization")
                checked: root.fx.loudness.applied
                onToggled: root.setEffect("loudness", "applied", checked)
            }
            ColumnLayout {
                visible: root.hasAudio && root.fx.loudness.applied
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space(6)
                spacing: Theme.space(1)
                PillTabs {
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("EBU R 128"), qsTr("ATSC A/85"), qsTr("Custom")]
                    currentIndex: Math.max(0, root.standards.indexOf(root.fx.loudness.standard))
                    Accessible.name: qsTr("Loudness Standard")
                    onActivated: index => root.setEffect("loudness", "standard", root.standards[index])
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    NumberField {
                        width: Theme.space(26)
                        enabled: root.fx.loudness.standard === "custom"
                        value: root.fx.loudness.standard === "ebuR128" ? -23 : root.fx.loudness.standard === "atscA85" ? -24 : root.fx.loudness.target
                        from: -40
                        to: -5
                        decimals: 1
                        stepSize: 0.5
                        suffix: "LUFS"
                        Accessible.name: qsTr("Target Loudness")
                        onValueModified: v => root.setEffect("loudness", "target", v)
                    }
                    NumberField {
                        width: Theme.space(20)
                        value: root.fx.loudness.tolerance
                        from: 0
                        to: 10
                        decimals: 1
                        stepSize: 0.5
                        suffix: "LU"
                        Accessible.name: qsTr("Tolerance")
                        onValueModified: v => root.setEffect("loudness", "tolerance", v)
                    }
                    NumberField {
                        width: Theme.space(26)
                        enabled: root.fx.loudness.standard === "custom"
                        value: root.fx.loudness.standard === "ebuR128" ? -1 : root.fx.loudness.standard === "atscA85" ? -2 : root.fx.loudness.maxTruePeak
                        from: -12
                        to: 0
                        decimals: 1
                        stepSize: 0.5
                        suffix: "dBTP"
                        Accessible.name: qsTr("Max True Peak Level")
                        onValueModified: v => root.setEffect("loudness", "maxTruePeak", v)
                    }
                }
                Toggle {
                    text: qsTr("Write a log file")
                    checked: root.fx.loudness.writeLog
                    onToggled: root.setEffect("loudness", "writeLog", checked)
                }
            }

            GroupLabel { text: qsTr("Metadata") }
            Toggle {
                objectName: "exportChapters"
                text: root.summary.chapterMarkers > 0 ? qsTr("Chapter markers as chapters (%1)").arg(root.summary.chapterMarkers)
                                                      : qsTr("Chapter markers as chapters")
                checked: root.meta.chapters
                onToggled: root.setMeta("chapters", checked)
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Toggle {
                    objectName: "exportTimecodeTrack"
                    text: qsTr("Start timecode")
                    checked: root.meta.timecode
                    onToggled: root.setMeta("timecode", checked)
                }
                FieldInput {
                    Layout.fillWidth: true
                    enabled: root.meta.timecode
                    text: typeof root.meta.startTimecode === "string" ? root.meta.startTimecode : ""
                    placeholderText: qsTr("Set Start Timecode")
                    Accessible.name: qsTr("Set Start Timecode")
                    onEditingFinished: root.setMeta("startTimecode", text.trim() === "" ? null : text.trim())
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Text {
                    text: qsTr("XMP")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                PillTabs {
                    objectName: "exportXmp"
                    Layout.fillWidth: true
                    compact: true
                    model: [qsTr("None"), qsTr("Embed"), qsTr("Sidecar file")]
                    currentIndex: Math.max(0, root.xmpModes.indexOf(root.meta.xmp))
                    Accessible.name: qsTr("XMP")
                    onActivated: index => root.setMeta("xmp", root.xmpModes[index])
                }
            }
            FieldInput {
                Layout.fillWidth: true
                text: root.meta.title
                placeholderText: qsTr("Title (default: the sequence name)")
                Accessible.name: qsTr("Title")
                onEditingFinished: root.setMeta("title", text)
            }
            FieldInput {
                Layout.fillWidth: true
                text: root.meta.description
                placeholderText: qsTr("Description")
                Accessible.name: qsTr("Description")
                onEditingFinished: root.setMeta("description", text)
            }

            GroupLabel { text: qsTr("Render"); visible: root.hasVideo }
            Toggle {
                objectName: "exportMaxRenderQuality"
                visible: root.hasVideo
                text: qsTr("Use Maximum Render Quality")
                checked: root.renderOptions.maximumRenderQuality
                onToggled: root.setRender("maximumRenderQuality", checked)
            }
            Toggle {
                objectName: "exportMaxDepth"
                visible: root.hasVideo
                text: qsTr("Render at Maximum Depth")
                checked: root.renderOptions.maximumDepth
                onToggled: root.setRender("maximumDepth", checked)
            }
            Toggle {
                objectName: "exportUsePreviews"
                visible: root.hasVideo && (root.summary.smartRenderAvailable ?? false)
                text: qsTr("Use Previews")
                checked: root.renderOptions.usePreviews
                onToggled: root.setRender("usePreviews", checked)
            }
            Toggle {
                objectName: "exportSmartRender"
                visible: root.hasVideo && (root.summary.smartRenderAvailable ?? false)
                text: qsTr("Smart Render (copy untouched ProRes / DNx frames)")
                checked: root.renderOptions.smartRender
                onToggled: root.setRender("smartRender", checked)
            }
        }
    }

    FileDialog {
        id: imageDialog
        title: qsTr("Image Overlay")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp)")]
        onAccepted: {
            const fxNext = root.merged(root.defaultEffects, root.overrides.effects)
            fxNext.imageOverlay.image = selectedFile.toString()
            fxNext.imageOverlay.applied = true
            root.overrideChanged("effects", fxNext)
        }
    }
    FileDialog {
        id: lutDialog
        title: qsTr("Lumetri Look/LUT")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Cube LUTs (*.cube)")]
        onAccepted: root.setEffect("look", "lut", selectedFile.toString())
    }
}
