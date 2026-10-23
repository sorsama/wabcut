pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● SOURCE MONITOR (M5h): Premiere's Source Monitor beside the Program Monitor (SourceMonitor singleton).
//
// The loaded source (Library double-click, Match Frame, the recent-sources menu) shows its picture, or a waveform
// for audio-only media (click to move, Ctrl/Cmd-drag to scrub), with its own timecode, duration and In/Out duration,
// an In/Out range bar over a mini timeline with the playhead, and its own transport with Insert and Overwrite.
// Drag sources onto the timeline: the picture (video and audio) and the two handles beside the timecode (video only,
// audio only). Its keymap scope is Premiere's Source Monitor: I, O, X, Shift+I/O, Ctrl+Shift+I/O/X and
// Ctrl+Shift+Space mark and play the source, not the sequence (Main.qml routes them here while it has focus).
// UX rework (stream T): click the timecode to type a source time or a +/- offset (TimecodeEntry,
// SourceMonitor.seekToText); drop a Library clip on the picture to open it here, or files from File Explorer (imported
// first, `importFiles`, then the first one opens).
Panel {
    id: root
    objectName: "sourceMonitorPanel"

    // Narrow windows show one monitor at a time: the Source/Program switch in the header.
    property bool single: false
    signal monitorRequested(string monitor)
    // Insert / Overwrite: Main runs them (a four-point edit may need the Fit Clip dialog first).
    signal editRequested(bool insert)
    signal editHovered(bool insert, bool hovered)
    /// Imports files dropped from File Explorer (Main.importUrlsThen: `importFiles(urls, after)`): `after(result)` runs once
    /// the import has run - at once, or after the unsaved-changes question when that runs first. Unset: file drops are
    /// refused.
    property var importFiles: null

    KeymapScope.scope: "sourceMonitor"
    // One monitor at a time: the switch takes the header's place.
    title: single ? "" : qsTr("Source Monitor")

    // Gives the Source Monitor keyboard focus (its keymap scope).
    function focusMonitor() {
        well.pointerFocus = true
        well.forceActiveFocus(Qt.MouseFocusReason)
    }

    // A drag source onto the timeline: `streams` is "av", "video" or "audio" (Main's source drop area).
    component SourceDrag: Item {
        id: dragItem
        property string streams: "av"
        Drag.active: dragHandler.active
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.mimeData: { "application/x-wabcut-source": dragItem.streams }
        DragHandler {
            id: dragHandler
            target: null
        }
    }

    // A handle under the picture that drags only the video or only the audio.
    component DragHandle: Rectangle {
        id: handle
        property string streams: "video"
        property string iconName: ""
        property string tip: ""
        implicitWidth: Theme.dragHandleSize
        implicitHeight: Theme.controlHeightSmall
        radius: height / 2
        color: handleHover.hovered && enabled ? Theme.hover : "transparent"
        opacity: enabled ? 1 : Theme.disabledOpacity
        Accessible.role: Accessible.Button
        Accessible.name: tip

        Icon {
            anchors.centerIn: parent
            name: handle.iconName
            size: Theme.iconSm
            color: Theme.textSecondary
        }
        Icon {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            name: "grip-vertical"
            size: Theme.iconXs - 4
            color: Theme.textTertiary
        }
        SourceDrag {
            anchors.fill: parent
            streams: handle.streams
            enabled: handle.enabled
        }
        HoverHandler {
            id: handleHover
            cursorShape: handle.enabled ? Qt.OpenHandCursor : Qt.ArrowCursor
        }
        Loader {
            active: handleHover.hovered
            sourceComponent: Tooltip {
                parent: handle
                visible: true
                text: handle.tip
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        MonitorSwitch {
            Layout.alignment: Qt.AlignHCenter
            visible: root.single
            current: "source"
            onSwitched: monitor => root.monitorRequested(monitor)
        }

        // ---- Picture (or waveform) ----
        Rectangle {
            id: well
            objectName: "sourceMonitorWell"
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusMonitor
            // No backdrop of its own (0.1.3), like the Program Monitor: the picture sits on the panel.
            color: "transparent"
            property bool pointerFocus: false

            activeFocusOnTab: true
            Accessible.role: Accessible.Graphic
            Accessible.name: SourceMonitor.loaded ? qsTr("Source monitor: %1").arg(SourceMonitor.name) : qsTr("Source monitor")

            onActiveFocusChanged: {
                if (!activeFocus)
                    pointerFocus = false
            }
            TapHandler {
                onTapped: root.focusMonitor()
            }

            Item {
                id: frameArea
                anchors.centerIn: parent
                width: Math.min(parent.width - Theme.space(4), (parent.height - Theme.space(4)) * SourceMonitor.aspect)
                height: width / SourceMonitor.aspect
                visible: SourceMonitor.kind !== "audio"

                Rectangle {
                    anchors.fill: parent
                    color: Theme.videoBlack
                    visible: SourceMonitor.loaded && !picture.hasFrame
                }
                // P.2: Premiere's Transparency Grid under the picture (qml/monitor/).
                TransparencyGrid {
                    objectName: "sourceTransparencyGrid"
                    anchors.fill: parent
                    visible: Monitors.sourceTransparencyGrid && SourceMonitor.loaded && picture.hasFrame
                }
                ProgramMonitor {
                    id: picture
                    objectName: "sourceMonitorPicture"
                    anchors.fill: parent
                    visible: SourceMonitor.loaded
                    surroundColor: Theme.videoBlack
                    transparencyGrid: Monitors.sourceTransparencyGrid
                    previewDivisor: MediaReadiness.previewDivisor
                    Component.onCompleted: SourceMonitor.attachMonitor(picture)
                }
                SafeMarginsOverlay {
                    anchors.fill: parent
                    visible: Preferences.safeMargins && SourceMonitor.hasVideo
                }
                // Dragging the picture edits video and audio in.
                SourceDrag {
                    objectName: "sourcePictureDrag"
                    anchors.fill: parent
                    streams: "av"
                    enabled: SourceMonitor.loaded && !Masks.redactTool
                }
                // Redactions (I.6): the regions covered at this position; the armed tool marks a new one.
                RedactionOverlay {
                    anchors.fill: parent
                    visible: SourceMonitor.loaded && SourceMonitor.hasVideo
                }
            }

            // Audio-only media: its waveform; a click moves the playhead, Ctrl/Cmd-drag scrubs.
            Item {
                id: waveArea
                objectName: "sourceWaveformArea"
                anchors.fill: parent
                anchors.margins: Theme.space(3)
                visible: SourceMonitor.kind === "audio"

                SourceWaveform {
                    id: waveform
                    objectName: "sourceWaveform"
                    anchors.fill: parent
                    color: Theme.accentStroke
                    baseColor: Theme.borderStrong
                }
                Rectangle {
                    x: Math.round(SourceMonitor.position * waveArea.width - width / 2)
                    width: Theme.playheadWidth
                    height: parent.height
                    color: Theme.playhead
                }
                MouseArea {
                    objectName: "sourceWaveformMouse"
                    anchors.fill: parent
                    preventStealing: true
                    property bool scrubbing: false
                    function fraction(x) { return Math.max(0, Math.min(1, x / width)) }
                    onPressed: mouse => {
                        root.focusMonitor()
                        scrubbing = (mouse.modifiers & Qt.ControlModifier) !== 0
                        if (scrubbing)
                            SourceMonitor.scrubFraction(fraction(mouse.x))
                        else
                            SourceMonitor.seekFraction(fraction(mouse.x))
                    }
                    onPositionChanged: mouse => {
                        if (scrubbing)
                            SourceMonitor.scrubFraction(fraction(mouse.x))
                    }
                    onReleased: scrubbing = false
                }
            }

            // Nothing loaded yet (the drop label takes its place while a drag is over the picture).
            Column {
                anchors.centerIn: parent
                width: parent.width - Theme.space(8)
                spacing: Theme.space(2)
                visible: !SourceMonitor.loaded && !sourceDropHighlight.visible

                Icon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "clapperboard"
                    size: Theme.iconXl
                    color: Theme.textTertiary
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: qsTr("Drag a clip here from the Library or File Explorer, or double-click it in the Library. F on the timeline matches a frame.")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }

            // The source's name and the recent sources (top left), on a pill: over the panel or the picture alike.
            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: Theme.space(1.5)
                width: Math.min(parent.width - Theme.space(3), sourceName.implicitWidth + recentButton.width + Theme.space(5))
                height: Theme.controlHeightSmall
                radius: height / 2
                color: Theme.raised
                visible: SourceMonitor.recent.length > 0 || SourceMonitor.preview

                Text {
                    id: sourceName
                    objectName: "sourceName"
                    anchors.left: parent.left
                    anchors.right: recentButton.left
                    anchors.leftMargin: Theme.space(3)
                    anchors.rightMargin: Theme.space(1)
                    anchors.verticalCenter: parent.verticalCenter
                    // 0.2: a Media Browser clip shown before it is imported (its first mark or edit imports it).
                    text: SourceMonitor.preview ? qsTr("%1 (Media Browser: not imported yet)").arg(SourceMonitor.name) : SourceMonitor.name
                    elide: Text.ElideRight
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                IconButton {
                    id: recentButton
                    objectName: "recentSourcesButton"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "chevron-down"
                    iconColor: Theme.textSecondary
                    tooltip: qsTr("Recent sources")
                    onClicked: recentMenu.popup(recentButton, 0, recentButton.height + Theme.space(1))

                    RecentSourcesMenu {
                        id: recentMenu
                    }
                }
            }

            // Drag only the video or only the audio (Premiere's handles under the picture).
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.space(1.5)
                width: handles.width + Theme.space(2)
                height: handles.height + Theme.space(1)
                radius: height / 2
                color: Theme.raised
                visible: SourceMonitor.loaded

                Row {
                    id: handles
                    anchors.centerIn: parent
                    spacing: Theme.space(1)

                    DragHandle {
                        objectName: "dragVideoOnly"
                        streams: "video"
                        iconName: "film"
                        tip: qsTr("Drag video only")
                        enabled: SourceMonitor.hasVideo
                    }
                    DragHandle {
                        objectName: "dragAudioOnly"
                        streams: "audio"
                        iconName: "audio-lines"
                        tip: qsTr("Drag audio only")
                        enabled: SourceMonitor.hasAudio
                    }
                }
            }

            FocusRing {
                targetRadius: Theme.radiusMonitor
                visible: well.activeFocus && !well.pointerFocus
            }

            // UX rework (stream T): drops onto the picture. A Library clip (application/x-wabcut-media, a media "m..." key;
            // sequences cannot open here) opens here (SourceMonitor.load: the recent menu gets it too). Files from File
            // Explorer import as File > Import does, then the first one opens (SourceMonitor.loadFileItem). While a drag
            // that can land is over the picture, an accent frame and a label say what the drop does.
            DropArea {
                id: sourceDrop
                objectName: "sourceMonitorDropArea"
                anchors.fill: parent
                keys: ["application/x-wabcut-media", "text/uri-list"]
                /// "clip", "files" or "" (nothing that opens here).
                property string kind: ""

                function kindOf(drag: var): string {
                    if (drag.formats.indexOf("application/x-wabcut-media") >= 0)
                        return drag.getDataAsString("application/x-wabcut-media").startsWith("m") ? "clip" : ""
                    return drag.hasUrls && root.importFiles ? "files" : ""
                }
                onEntered: drag => {
                    sourceDrop.kind = sourceDrop.kindOf(drag)
                    drag.accepted = sourceDrop.kind !== ""
                }
                onExited: sourceDrop.kind = ""
                onDropped: drop => {
                    const kind = sourceDrop.kindOf(drop)
                    sourceDrop.kind = ""
                    if (kind === "clip") {
                        if (SourceMonitor.load(drop.getDataAsString("application/x-wabcut-media"))) {
                            drop.accept(Qt.CopyAction)
                            root.focusMonitor()
                        }
                        return
                    }
                    if (kind !== "files")
                        return
                    drop.accept(Qt.CopyAction)
                    const urls = []
                    for (const url of drop.urls)
                        urls.push(url)
                    // A project file opens instead (Main). The first media file opens here once it is imported: now, or
                    // after the unsaved-changes question (edits on the sample project) has its answer.
                    const media = urls.find(u => !u.toString().toLowerCase().endsWith(".wabcut"))
                    root.importFiles(urls, () => {
                        if (media !== undefined && SourceMonitor.loadFileItem(media.toString()))
                            root.focusMonitor()
                    })
                }
            }
            Rectangle {
                id: sourceDropHighlight
                objectName: "sourceMonitorDropHighlight"
                anchors.fill: parent
                visible: sourceDrop.containsDrag && sourceDrop.kind !== ""
                radius: Theme.radiusMonitor
                color: Theme.accentWash
                border.width: Theme.selectionWidth
                border.color: Theme.accentStroke

                Rectangle {
                    anchors.centerIn: parent
                    width: dropLabel.implicitWidth + Theme.space(6)
                    height: Theme.controlHeight
                    radius: height / 2
                    color: Theme.accentFill

                    Text {
                        id: dropLabel
                        anchors.centerIn: parent
                        text: sourceDrop.kind === "files" ? qsTr("Drop to import and open here") : qsTr("Drop to open in the Source Monitor")
                        color: Theme.accentOnFill
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.Bold
                    }
                }
            }
        }

        // ---- Timecode and durations ----
        Item {
            id: readouts
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeightSmall

            // The source position on an accent pill (design language v3). UX rework (stream T): click it to type a time
            // or a +/- offset (TimecodeEntry, SourceMonitor.seekToText); Esc cancels.
            TimecodeEntry {
                id: timecode
                objectName: "sourceTimecode"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                height: Theme.controlHeightSmall
                enabled: SourceMonitor.loaded
                text: SourceMonitor.loaded ? SourceMonitor.positionText : "--:--:--:--"
                label: qsTr("Source position")
                fill: SourceMonitor.loaded ? Theme.accentFill : Theme.raised
                color: SourceMonitor.loaded ? Theme.accentOnFill : Theme.textTertiary
                seek: typed => SourceMonitor.seekToText(typed)
            }

            // In/Out duration (Premiere's right-hand readout) on a raised pill; the whole length too when there is room.
            Rectangle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: durationRow.width + Theme.space(4)
                height: Theme.controlHeightSmall
                radius: height / 2
                color: Theme.raised
                visible: SourceMonitor.loaded

                Row {
                    id: durationRow
                    anchors.centerIn: parent
                    spacing: Theme.space(1.5)

                    Text {
                        id: durations
                        objectName: "sourceInOutDuration"
                        anchors.verticalCenter: parent.verticalCenter
                        text: SourceMonitor.inOutText
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textSm
                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("In to Out %1 of %2").arg(SourceMonitor.inOutText).arg(SourceMonitor.durationText)
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: readouts.width >= timecode.implicitWidth + durations.implicitWidth + implicitWidth + Theme.space(14)
                        //: The source's whole duration after its In/Out duration
                        text: qsTr("/ %1").arg(SourceMonitor.durationText)
                        color: Theme.textTertiary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textSm
                    }
                }
            }
        }

        // ---- In/Out range bar over the mini timeline ----
        Item {
            id: mini
            objectName: "sourceMiniTimeline"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.sourceMiniTimelineHeight + Theme.sourceRangeBarHeight + Theme.space(1)
            enabled: SourceMonitor.loaded
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Source position")

            readonly property real inX: SourceMonitor.inPosition * width
            readonly property real outX: SourceMonitor.outPosition * width

            // The range bar: In to Out.
            Rectangle {
                objectName: "sourceRangeBar"
                x: mini.inX
                width: Math.max(Theme.hairline * 2, mini.outX - mini.inX)
                height: Theme.sourceRangeBarHeight
                radius: height / 2
                color: SourceMonitor.hasIn || SourceMonitor.hasOut ? Theme.accentFill : Theme.border
            }
            // The mini timeline: the whole source, the marked part tinted.
            Rectangle {
                id: track
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.sourceMiniTimelineHeight
                // Design language v3: a pill track, the marked part an accent band.
                radius: height / 2
                color: Theme.hover
                clip: true

                Rectangle {
                    x: mini.inX
                    width: mini.outX - mini.inX
                    height: parent.height
                    radius: height / 2
                    visible: SourceMonitor.hasIn || SourceMonitor.hasOut
                    color: Qt.alpha(Theme.accentFill, 0.45)
                }
                // Redacted ranges of the source (I.6).
                Repeater {
                    model: Masks.redactions
                    delegate: Rectangle {
                        required property var modelData
                        objectName: "sourceRedactionSpan"
                        x: modelData.start * track.width
                        width: Math.max(Theme.hairline * 2, (modelData.end - modelData.start) * track.width)
                        y: parent.height - height
                        height: Math.max(2, parent.height / 3)
                        color: modelData.enabled ? Theme.danger : Theme.textDisabled
                        opacity: 0.8
                    }
                }
                Repeater {
                    model: 9
                    delegate: Rectangle {
                        required property int index
                        x: Math.round((index + 1) * track.width / 10)
                        y: track.height - height
                        width: Theme.hairline
                        height: track.height / 3
                        color: Theme.border
                    }
                }
                // In and Out brackets
                Rectangle {
                    visible: SourceMonitor.hasIn
                    x: mini.inX
                    width: Theme.selectionWidth
                    height: parent.height
                    color: Theme.accentStroke
                }
                Rectangle {
                    visible: SourceMonitor.hasOut
                    x: mini.outX - width
                    width: Theme.selectionWidth
                    height: parent.height
                    color: Theme.accentStroke
                }
            }
            // Playhead: a white pill standing a little proud of the track.
            Rectangle {
                objectName: "sourcePlayhead"
                visible: SourceMonitor.loaded
                x: Math.round(SourceMonitor.position * mini.width - width / 2)
                y: track.y - Theme.space(0.75)
                width: Theme.space(1)
                height: track.height + Theme.space(1.5)
                radius: width / 2
                color: Theme.playhead
            }
            MouseArea {
                anchors.fill: parent
                preventStealing: true
                function fraction(x) { return Math.max(0, Math.min(1, x / width)) }
                onPressed: mouse => {
                    root.focusMonitor()
                    SourceMonitor.seekFraction(fraction(mouse.x))
                }
                onPositionChanged: mouse => SourceMonitor.scrubFraction(fraction(mouse.x))
            }
        }

        // ---- Transport ----
        Item {
            id: transport
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(11)

            Row {
                id: buttons
                anchors.centerIn: parent
                spacing: 0
                enabled: SourceMonitor.loaded
                // Narrow monitors drop Go to In/Out and the frame steps first.
                readonly property bool roomy: transport.width >= Theme.controlHeight * 11 + Theme.space(11)

                IconButton {
                    objectName: "sourceMarkIn"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "mark-in"
                    tooltip: qsTr("Mark In")
                    command: "marker.markIn"
                    onClicked: SourceMonitor.markIn()
                }
                IconButton {
                    objectName: "sourceMarkOut"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "mark-out"
                    tooltip: qsTr("Mark Out")
                    command: "marker.markOut"
                    onClicked: SourceMonitor.markOut()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "go-to-in"
                    tooltip: qsTr("Go to In")
                    command: "marker.goToIn"
                    enabled: SourceMonitor.hasIn
                    onClicked: SourceMonitor.goToIn()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "step-back"
                    tooltip: qsTr("Step back one frame")
                    command: "transport.stepBackward"
                    onClicked: SourceMonitor.stepFrames(-1)
                }
                IconButton {
                    objectName: "sourcePlay"
                    anchors.verticalCenter: parent.verticalCenter
                    variant: "primary"
                    // Design language v3: the big white play button with a dark glyph.
                    implicitWidth: Theme.space(11)
                    iconSize: Theme.iconMd
                    checkedColor: Theme.playhead
                    checkedIconColor: Theme.canvas
                    iconName: SourceMonitor.playing ? "pause" : "play"
                    tooltip: SourceMonitor.playing ? qsTr("Pause") : qsTr("Play")
                    command: "transport.playStop"
                    onClicked: SourceMonitor.togglePlay()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "step-forward"
                    tooltip: qsTr("Step forward one frame")
                    command: "transport.stepForward"
                    onClicked: SourceMonitor.stepFrames(1)
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "go-to-out"
                    tooltip: qsTr("Go to Out")
                    command: "marker.goToOut"
                    enabled: SourceMonitor.hasOut
                    onClicked: SourceMonitor.goToOut()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "play-in-out"
                    tooltip: qsTr("Play In to Out")
                    command: "transport.playInToOut"
                    onClicked: SourceMonitor.playInToOut()
                }
                // P.2: Premiere's Loop and the Source Monitor's settings (qml/monitor/).
                LoopButton {
                    anchors.verticalCenter: parent.verticalCenter
                    source: true
                }
                // P.2: Premiere's Export Frame from the Source Monitor (qml/monitor/ExportFrameDialog.qml).
                IconButton {
                    objectName: "sourceExportFrame"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "camera"
                    tooltip: qsTr("Export Frame")
                    command: "file.exportFrame"
                    enabled: SourceMonitor.hasVideo
                    onClicked: Monitors.requestExportFrame(true)
                }
                IconButton {
                    id: sourceSettingsButton
                    objectName: "sourceMonitorSettingsButton"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "settings"
                    tooltip: qsTr("Settings")
                    onClicked: sourceSettingsMenu.popup(sourceSettingsButton, 0, -sourceSettingsMenu.implicitHeight - Theme.space(1))

                    SourceSettingsMenu {
                        id: sourceSettingsMenu
                    }
                }
                Item {
                    width: Theme.space(2)
                    height: 1
                }
                IconButton {
                    objectName: "sourceInsertButton"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    // Design language v3: Insert is the accent button, Overwrite the white one.
                    variant: "primary"
                    iconName: "insert-edit"
                    tooltip: qsTr("Insert")
                    command: "clip.insert"
                    enabled: SourceMonitor.canEdit
                    onClicked: root.editRequested(true)
                    onHoveredChanged: root.editHovered(true, hovered)
                }
                IconButton {
                    objectName: "sourceOverwriteButton"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    variant: "primary"
                    checkedColor: Theme.playhead
                    checkedIconColor: Theme.canvas
                    iconName: "overwrite-edit"
                    tooltip: qsTr("Overwrite")
                    command: "clip.overwrite"
                    enabled: SourceMonitor.canEdit
                    onClicked: root.editRequested(false)
                    onHoveredChanged: root.editHovered(false, hovered)
                }
                Item {
                    width: Theme.space(2)
                    height: 1
                }
                // Redactions (I.6): the tool, and the list of this source's redactions.
                IconButton {
                    objectName: "sourceRedactButton"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    checkable: true
                    checked: Masks.redactTool
                    enabled: SourceMonitor.hasVideo
                    iconName: "eye-off"
                    tooltip: qsTr("Redact: drag over the picture to cover a region wherever this source is used")
                    onClicked: Masks.redactTool = checked
                }
                IconButton {
                    id: redactionsButton
                    objectName: "sourceRedactionsButton"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    enabled: SourceMonitor.hasVideo
                    iconName: "list"
                    tooltip: qsTr("Redactions of this source")
                    onClicked: redactionsPopover.open()

                    RedactionsPopover {
                        id: redactionsPopover
                        y: -height - Theme.space(1)
                    }
                }
            }
        }
    }
}
