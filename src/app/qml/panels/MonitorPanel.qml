pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// ● PROGRAM MONITOR: the picture (straight on the panel since 0.1.3: a black backdrop round it read as part of the
// video), a readout row (timecode · quality · zoom · safe margins · J/K/L speed · fps/dropped), the time bar (0.1.3,
// ProgramTimeBar: the sequence with its In/Out range and markers; click to seek, drag to scrub, Ctrl+wheel to zoom) and
// the transport (Premiere's Program Monitor buttons).
//
// 0.1.3, picking (GraphicCanvas.pickPress / pickDoubleClick): a click in the picture selects the object under the
// pointer - a graphic's layer, a title, a clip - as a click on it in the timeline would (the one picked last stays
// picked while it is under the pointer, so it can be dragged from behind another); a double-click edits a text in place
// (a graphic's text layer or a title, Premiere's selection tool) and each further double-click picks the object behind,
// wrapping to the top. The picked clip is outlined; pointing at an object outlines it dashed. The pick area lies under
// the other overlays (masks, review marks, graphics, the Type tool, object prompts, layouts): what they take, they keep.
//
// `programMonitorSlot` hosts the ProgramMonitor (QQuickRhiItem). The mock frame underneath stays visible until
// the monitor has presented a frame (hasFrame). I.1: the monitor shows the Playback singleton (PlaybackController
// + timeline audio on the audio clock) as soon as a real project is open (not for the demo); transport, speed,
// measured fps, dropped frames and backends are real. Sources play in the Source Monitor beside it (M5h).
//
// I.3: preview quality (MediaReadiness). The quality menu picks Auto / Full / 1/2 / 1/4 (Premiere's playback
// resolution) and "Use proxies"; the monitor composites and the decoders decode at that resolution. The header
// badge says what the picture really is ("Proxy 1/2", "1/2", "Full"; amber when an offline original is shown from
// its proxy).
//
// I.4: captions are drawn by the compositor, in the picture (no QML overlay), as exports burn them in; the quality
// menu toggles karaoke highlighting (Playback.captionKaraoke).
//
// M5h: Add Marker, Mark In/Out, Go to In/Out, frame steps, Lift and Extract (ProgramTools / ProjectController) and
// Export Frame (the camera: a PNG of the frame at the playhead from the offscreen export renderer, next to the
// project or in a chosen folder); zoom (Fit, 25–400 % of the sequence's pixels) and safe margins.
Panel {
    id: root

    required property MockProject project
    readonly property alias monitorSlot: slot

    // Narrow windows show one monitor at a time: the Source/Program switch in the header.
    property bool single: false
    signal monitorRequested(string monitor)
    /// UX rework (stream T): a typed time moved the playhead (Main brings it into view in the Timeline).
    signal playheadMoved()

    // 0 = fit the well; otherwise monitor pixels per sequence pixel (Premiere's zoom levels).
    property real zoom: 0
    // P.2: a zoomed picture's scroll (Premiere's scroll bars): the wheel scrolls it, Shift+wheel sideways.
    property real panX: 0
    property real panY: 0
    onZoomChanged: {
        panX = 0
        panY = 0
    }

    /// 0.1.2, Premiere's Type tool (T, the timeline's tool): a click in the picture adds a text layer there - a new
    /// graphic clip at the playhead, or a layer of the selected graphic (Graphics.addLayer, like Ctrl+T) - and starts
    /// editing it in place.
    property bool typeTool: false
    property int pendingTextLayer: 0

    /// The Type tool's click at (fx, fy), fractions of the frame: one undo step (0.2). Returns "" or why not.
    function addTextAt(fx, fy) {
        const error = Graphics.addLayer("text", { transform: { x: Math.round(fx * ProjectController.frameWidth),
                                                               y: Math.round(fy * ProjectController.frameHeight) } })
        if (error !== "")
            return error
        const layer = Graphics.selectedLayer
        pendingTextLayer = layer
        root.editPendingText()
        return ""
    }
    function editPendingText() {
        if (pendingTextLayer !== 0 && GraphicCanvas.selection.id === pendingTextLayer && graphicOverlay.startEditing())
            pendingTextLayer = 0
    }
    /// A double-click's pick (GraphicCanvas.pickDoubleClick): opens the inline editor when it says "edit" - over a
    /// title at once, over a graphic's text layer once the canvas has resolved it.
    function applyPick(result) {
        if (result.action !== "edit")
            return
        if (result.kind === "title") {
            titleEditor.titleClip = result.clip
            titleEditor.open(0, result.editor ?? {}, false)
        } else {
            pendingTextLayer = result.layer
            editPendingText()
        }
    }
    Connections {
        target: GraphicCanvas
        // The canvas resolves the new layer off the GUI thread: edit it once it is there.
        function onStateChanged() { root.editPendingText() }
    }

    KeymapScope.scope: "programMonitor"

    // One preview quality entry of the quality menu.
    component QualityItem: ContextMenuItem {
        property string value: ""
        checkable: true
        checked: MediaReadiness.previewQuality === value
        onTriggered: MediaReadiness.previewQuality = value
    }
    // One monitor at a time: the switch takes the header's place.
    title: single ? "" : qsTr("Program Monitor")

    trailing: [
        // What the picture is made of right now: proxies or originals, at what resolution.
        StatusPill {
            objectName: "qualityBadge"
            visible: !ProjectFile.demo
            label: MediaReadiness.qualityBadge
            dotColor: MediaReadiness.qualityState === "offline" ? Theme.warning
                      : MediaReadiness.qualityState === "full" ? Theme.success : Theme.accentStroke
            tooltip: MediaReadiness.qualityTooltip
        },
        // Effects in the picture the renderer cannot run (unknown, or saved by a newer version): shown, not hidden.
        StatusPill {
            objectName: "bypassedEffectsBadge"
            visible: Playback.bypassedEffects > 0
            label: qsTr("%n effect(s) bypassed", "", Playback.bypassedEffects)
            dotColor: Theme.warning
            tooltip: qsTr("These effects are unknown or come from a newer version of Wabcut; the picture is shown without them.")
        },
        // P.2: Morph Cut and Optical Flow motion being analysed (Premiere's banner on the Program Monitor).
        StatusPill {
            objectName: "analyzingMotionBadge"
            visible: Playback.analyzingMotion
            label: qsTr("Analyzing in background")
            dotColor: Theme.accentStroke
            tooltip: qsTr("Morph Cut and Optical Flow analyse motion before they show; render the section for real-time playback.")
        },
        // Colour management (I.9): what the monitor shows of an ACES or HDR project, e.g. "HDR · tone-mapped SDR preview".
        StatusPill {
            objectName: "colorManagementBadge"
            visible: Color.monitorLabel !== ""
            label: Color.monitorLabel
            dotColor: Theme.accentStroke
            tooltip: Color.colorManagement.hdr
                     ? qsTr("An HDR project shown on this SDR monitor: highlights are tone-mapped. 10-bit exports write the HDR picture (PQ or HLG).")
                     : qsTr("A colour-managed project: the monitor shows its output transform.")
        },
        StatusPill {
            label: ProjectController.sequenceName
            //: Sequence format badge: frame height + "p" + frame rate, e.g. 1080p24
            detail: ProjectController.formatText
            dotColor: Theme.accentStroke
        }
    ]

    // Keyboard transport arrives from KeymapDispatcher (Main.qml) and goes to Playback.
    function shuttle(direction) {
        Playback.shuttle(direction)
    }

    function togglePlay() {
        Playback.togglePlay()
    }

    // Gives the Program Monitor keyboard focus (its keymap scope).
    function focusMonitor() {
        well.pointerFocus = true
        well.forceActiveFocus(Qt.MouseFocusReason)
    }

    // A real project shows real playback; the demo keeps the mock frame.
    function attachPlayback() {
        if (!ProjectFile.demo)
            Playback.attachMonitor(programMonitor)
    }
    Component.onCompleted: attachPlayback()
    Connections {
        target: ProjectFile

        function onDocumentChanged() {
            root.attachPlayback()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        MonitorSwitch {
            Layout.alignment: Qt.AlignHCenter
            visible: root.single
            current: "program"
            onSwitched: monitor => root.monitorRequested(monitor)
        }

        // ---- Video well ----
        Rectangle {
            id: well
            objectName: "programMonitorWell"
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusMonitor
            // No backdrop of its own (0.1.3): the picture sits on the panel; a black surround read as part of the video.
            color: "transparent"
            clip: root.zoom > 0
            // Focus ring only for keyboard focus (plain Items have no visualFocus).
            property bool pointerFocus: false

            activeFocusOnTab: true
            Accessible.role: Accessible.Graphic
            Accessible.name: qsTr("Program monitor")

            onActiveFocusChanged: {
                if (!activeFocus)
                    pointerFocus = false
            }
            TapHandler {
                onTapped: {
                    well.pointerFocus = true
                    well.forceActiveFocus(Qt.MouseFocusReason)
                }
            }
            // P.2: scroll a zoomed picture (only when it overflows the well).
            WheelHandler {
                enabled: root.zoom > 0
                acceptedModifiers: Qt.NoModifier | Qt.ShiftModifier
                onWheel: event => {
                    const step = (event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x) / 2
                    const roomX = Math.max(0, (frameArea.width - well.width) / 2 + Theme.space(4))
                    const roomY = Math.max(0, (frameArea.height - well.height) / 2 + Theme.space(4))
                    if ((event.modifiers & Qt.ShiftModifier) !== 0 || event.angleDelta.x !== 0)
                        root.panX = Math.max(-roomX, Math.min(roomX, root.panX + step))
                    else
                        root.panY = Math.max(-roomY, Math.min(roomY, root.panY + step))
                }
            }

            // M5l: the multi-camera view puts the angle grid in the well's left half, the program picture in its right.
            MulticamView {
                objectName: "multicamView"
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: Theme.space(2)
                width: parent.width / 2 - Theme.space(3)
                visible: Multicam.viewOn && !Trim.viewOpen
            }

            // The largest frame of the sequence's shape that fits the well (its right half in the multi-camera view),
            // or the zoomed size.
            Item {
                id: frameArea
                objectName: "programFrameArea"
                readonly property real aspect: ProjectController.frameWidth / Math.max(1, ProjectController.frameHeight)
                readonly property real areaX: Multicam.viewOn ? well.width / 2 : 0
                readonly property real areaWidth: Multicam.viewOn ? well.width / 2 : well.width
                readonly property real fitWidth: Math.min(areaWidth - Theme.space(4), (well.height - Theme.space(4)) * aspect)
                x: areaX + (areaWidth - width) / 2 + (root.zoom > 0 ? root.panX : 0)
                y: (well.height - height) / 2 + (root.zoom > 0 ? root.panY : 0)
                visible: !Trim.viewOpen // M5i: the Trim view takes the well
                width: root.zoom > 0 ? ProjectController.frameWidth * root.zoom / Screen.devicePixelRatio : fitWidth
                height: width / aspect

                // Mock frame, shown until the ProgramMonitor presents its first frame
                Rectangle {
                    anchors.fill: parent
                    visible: !programMonitor.hasFrame
                    color: Theme.videoBlack
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Theme.mockFrame[0] }
                        GradientStop { position: 0.55; color: Theme.mockFrame[1] }
                        GradientStop { position: 1.0; color: Theme.mockFrame[2] }
                    }

                    // Title-safe guide
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: parent.width * Theme.titleSafeInset
                        color: "transparent"
                        border.width: Theme.hairline
                        border.color: Theme.guide
                        visible: !Preferences.safeMargins
                    }

                    Text {
                        anchors.centerIn: parent
                        text: ProjectController.sequenceName
                        color: Theme.scrimTextMuted
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textTitle
                        font.weight: Font.DemiBold
                    }
                }

                // P.2: Premiere's Transparency Grid under the picture (qml/monitor/).
                TransparencyGrid {
                    objectName: "programTransparencyGrid"
                    anchors.fill: parent
                    visible: Monitors.programTransparencyGrid && programMonitor.hasFrame
                }

                // ---- M4 seam ----
                Item {
                    id: slot
                    objectName: "programMonitorSlot"
                    anchors.fill: parent
                    ProgramMonitor {
                        id: programMonitor
                        objectName: "programMonitor"
                        anchors.fill: parent
                        surroundColor: Theme.videoBlack
                        transparencyGrid: Monitors.programTransparencyGrid
                        scopes: true // feeds the Scopes panel (I.5) while it is open
                        // Composite at the preview resolution (the decoders follow the same divisor).
                        previewDivisor: MediaReadiness.previewDivisor
                    }
                    // Auto quality: monitor pixels per sequence pixel. 0.1.3: once the size settles - a gutter drag or
                    // a workspace glide resizes the monitor every frame, and a new preview resolution restarts the
                    // decoders. The first size applies at once.
                    readonly property real wantedScale: frameArea.width > 0 && ProjectController.frameWidth > 0
                                                        ? frameArea.width * Screen.devicePixelRatio / ProjectController.frameWidth : 0
                    property real settledScale: 0
                    onWantedScaleChanged: {
                        if (wantedScale > 0 && settledScale > 0)
                            scaleSettle.restart()
                        else
                            settledScale = wantedScale
                    }
                    Component.onCompleted: settledScale = wantedScale
                    Timer {
                        id: scaleSettle
                        interval: Theme.durCollapse
                        onTriggered: slot.settledScale = slot.wantedScale
                    }
                    Binding {
                        target: MediaReadiness
                        property: "displayScale"
                        when: slot.settledScale > 0
                        value: slot.settledScale
                    }
                }

                // Picking (0.1.3): clicks and double-clicks on the picture that no overlay above takes. The picked
                // clip's box (solid) and what a click would pick (dashed, while pointing).
                MouseArea {
                    id: pickArea
                    objectName: "programPickArea"
                    anchors.fill: parent
                    enabled: !root.typeTool
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton
                    property var hoverQuad: []

                    function quadPoints(quad) {
                        const out = []
                        for (let i = 0; i + 1 < (quad ?? []).length; i += 2)
                            out.push(Qt.point(quad[i] * width, quad[i + 1] * height))
                        return out
                    }

                    onPressed: mouse => {
                        root.pendingTextLayer = 0
                        well.pointerFocus = true
                        well.forceActiveFocus(Qt.MouseFocusReason)
                        GraphicCanvas.pickPress(mouse.x / width, mouse.y / height)
                        hoverQuad = []
                        pickOutline.requestPaint()
                    }
                    // A double-click edits a title or a text in place; on a picture it shows Motion's box (P.2).
                    onDoubleClicked: mouse => {
                        const picked = GraphicCanvas.pickDoubleClick(mouse.x / width, mouse.y / height)
                        if (picked.action === "edit")
                            root.applyPick(picked)
                        else
                            motionOverlay.engageAt(mouse.x, mouse.y)
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            return
                        const quad = GraphicCanvas.hoverAt(mouse.x / width, mouse.y / height)
                        if (JSON.stringify(quad) !== JSON.stringify(hoverQuad)) {
                            hoverQuad = quad
                            pickOutline.requestPaint()
                        }
                    }
                    onExited: {
                        hoverQuad = []
                        pickOutline.requestPaint()
                    }

                    Canvas {
                        id: pickOutline
                        anchors.fill: parent
                        visible: !titleEditor.visible
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const outline = (points) => {
                                if (points.length < 2)
                                    return
                                ctx.beginPath()
                                ctx.moveTo(points[0].x, points[0].y)
                                for (let i = 1; i < points.length; ++i)
                                    ctx.lineTo(points[i].x, points[i].y)
                                ctx.closePath()
                                ctx.stroke()
                            }
                            const picked = GraphicCanvas.pickOutline
                            if (pickArea.hoverQuad.length > 0 && JSON.stringify(pickArea.hoverQuad) !== JSON.stringify(picked)) {
                                ctx.lineWidth = 1
                                ctx.setLineDash([4, 3])
                                ctx.strokeStyle = Theme.scrimTextMuted
                                outline(pickArea.quadPoints(pickArea.hoverQuad))
                            }
                            ctx.setLineDash([])
                            ctx.lineWidth = 1.5
                            ctx.strokeStyle = Theme.accentStroke
                            outline(pickArea.quadPoints(picked))
                        }
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        Connections {
                            target: GraphicCanvas
                            function onPickChanged() { pickOutline.requestPaint() }
                        }
                    }
                }

                // Captions (I.4) are drawn by the compositor, in the picture (Playback.captionsShown): no overlay.
                // P.2: Premiere's Overlays (qml/monitor/): source and sequence timecode over the picture.
                MonitorOverlays {
                    anchors.fill: parent
                    visible: Monitors.overlays
                }
                SafeMarginsOverlay {
                    objectName: "programSafeMargins"
                    anchors.fill: parent
                }
                // Color workspace (I.5): before/after split handle, side-by-side labels, the HSL eyedropper.
                ColorCompareOverlay {
                    anchors.fill: frameArea
                }
                // Masks (I.6): the Effect Controls target's outlines, the selected mask's handles, the drawing tools.
                MaskOverlay {
                    anchors.fill: parent
                }
                // Review notes (M5m, C.9): the note's mark; a spot or a rectangle drawn for the next note.
                ReviewMarkOverlay {
                    anchors.fill: parent
                }
                // Motion graphics (M5o, C.13): the selected graphic layer's handles, snapping guides, inline text.
                GraphicCanvasOverlay {
                    id: graphicOverlay
                    anchors.fill: parent
                    onPicked: result => root.applyPick(result)
                }
                // A title's text edited in place (0.1.3: a double-click on a title clip in the picture).
                GraphicTextEditor {
                    id: titleEditor
                    objectName: "titleTextEditor"
                    editName: "titleTextEdit"
                    accessibleName: qsTr("Text of the title")
                    property string titleClip: ""
                    canvasWidth: frameArea.width
                    canvasHeight: frameArea.height
                    onCommitted: text => {
                        GraphicCanvas.setTitleText(titleEditor.titleClip, text)
                        well.forceActiveFocus(Qt.MouseFocusReason)
                    }
                    onCancelled: well.forceActiveFocus(Qt.MouseFocusReason)
                }
                // The Type tool (0.1.2): over the picture while T is on and no text is being edited.
                MouseArea {
                    objectName: "programTypeToolArea"
                    anchors.fill: parent
                    enabled: root.typeTool && !graphicOverlay.editing && !titleEditor.visible
                    visible: enabled
                    cursorShape: Qt.IBeamCursor
                    onClicked: mouse => root.addTextAt(mouse.x / width, mouse.y / height)
                }
                // AI Object Mask (AI-14): the Object tool's clicks and box, the matte preview.
                ObjectPromptOverlay {
                    anchors.fill: parent
                }
                // Gaming layouts (M5o): the layout's schematic while the picker is open; the facecam region tool.
                LayoutMonitorOverlay {
                    anchors.fill: parent
                }
                // P.2: the guides (qml/monitor/), under Motion's box.
                GuidesOverlay {
                    anchors.fill: parent
                }
                // P.2: Motion's bounding box, handles, anchor point and motion path (qml/monitor/); on top, so its drags
                // are seen first (the pick area's double-click shows it).
                MotionOverlay {
                    id: motionOverlay
                    anchors.fill: parent
                    suspended: root.typeTool
                }
            }

            // P.2: the rulers along the top and the left (qml/monitor/), counting the frame's pixels.
            MonitorRulers {
                anchors.fill: parent
                frame: frameArea
                visible: Monitors.showRulers && !Trim.viewOpen
                z: 1
            }

            // The Trim view (M5i): outgoing | incoming frames while Trim Edit is open.
            TrimView {
                anchors.fill: parent
                anchors.margins: Theme.space(2)
                visible: Trim.viewOpen
                onVisibleChanged: {
                    if (visible)
                        Trim.refreshFrames()
                }
            }

            // P.2: Premiere's dropped frame indicator (qml/monitor/), in the picture's corner.
            DroppedFrameIndicator {
                anchors.right: frameArea.right
                anchors.top: frameArea.top
                anchors.margins: Theme.space(2)
                visible: Monitors.droppedFrameIndicator && !Trim.viewOpen
                z: 1
            }

            // Voice-over (M5m, A.8): the count-in, then the recording badge with the input meter; hidden when idle.
            VoiceOverOverlay {
                anchors.fill: parent
                z: 2
            }

            FocusRing {
                targetRadius: Theme.radiusMonitor
                visible: well.activeFocus && !well.pointerFocus
            }
        }

        // ---- Readouts: timecode left; quality, zoom, safe margins, speed and stats right ----
        // The right side drops its least important parts when space runs out.
        Item {
            id: readouts
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeightSmall

            readonly property real rightRoom: width - timecodeRow.width - Theme.space(3)

            Row {
                id: timecodeRow
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space(2)

                // The playhead timecode on an accent pill (design language v3). UX rework (stream T): click it to type a
                // time or a +/- offset (TimecodeEntry, ProjectController.seekToText: the field starts from the
                // sequence's own time, which it reads also when the pill shows audio units); Esc cancels.
                TimecodeEntry {
                    id: timecodeText
                    objectName: "programTimecode"
                    anchors.verticalCenter: parent.verticalCenter
                    height: Theme.controlHeightSmall
                    text: Monitors.audioTimeUnits ? Monitors.programTimeText : ProjectController.playheadText // P.2
                    editText: ProjectController.playheadText
                    label: qsTr("Playhead")
                    seek: typed => {
                        const moved = ProjectController.seekToText(typed)
                        if (moved)
                            root.playheadMoved()
                        return moved
                    }
                    preview: typed => ProjectController.timeEntryText(typed)
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: readouts.width >= timecodeText.implicitWidth + implicitWidth + Theme.space(40)
                    //: Total sequence duration after the playhead timecode
                    text: qsTr("/ %1").arg(ProjectController.durationText)
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
            }

            Row {
                id: rightRow
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space(2)

                // P.2: Premiere's Program Monitor settings (the wrench; qml/monitor/).
                IconButton {
                    id: settingsButton
                    objectName: "programMonitorSettingsButton"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    iconName: "settings"
                    tooltip: qsTr("Settings")
                    onClicked: settingsMenu.popup(settingsButton, 0, -settingsMenu.implicitHeight - Theme.space(1))

                    MonitorSettingsMenu {
                        id: settingsMenu
                    }
                }
                // Safe margins (action and title safe).
                IconButton {
                    objectName: "safeMarginsToggle"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    checkable: true
                    checked: Preferences.safeMargins
                    iconName: "safe-margins"
                    tooltip: qsTr("Safe margins (action and title safe)")
                    onToggled: Preferences.safeMargins = checked
                }

                // Zoom: Fit, or a share of the sequence's pixels.
                PillButton {
                    id: zoomButton
                    objectName: "programZoomButton"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    text: root.zoom > 0 ? qsTr("%1%").arg(Math.round(root.zoom * 100)) : qsTr("Fit")
                    tooltip: qsTr("Zoom")
                    onClicked: zoomMenu.popup(zoomButton, 0, -zoomMenu.implicitHeight - Theme.space(1))

                    ContextMenu {
                        id: zoomMenu
                        objectName: "programZoomMenu"

                        ContextMenuItem {
                            objectName: "zoomFit"
                            text: qsTr("Fit")
                            checkable: true
                            checked: root.zoom === 0
                            onTriggered: root.zoom = 0
                        }
                        ContextMenuSeparator {}
                        Instantiator {
                            model: Monitors.zoomLevels // P.2: Premiere's 10 % to 1600 %
                            delegate: ContextMenuItem {
                                required property var modelData
                                objectName: "zoom" + Math.round(modelData * 100)
                                text: qsTr("%1%").arg(Math.round(modelData * 100))
                                checkable: true
                                checked: root.zoom === modelData
                                onTriggered: root.zoom = modelData
                            }
                            onObjectAdded: (index, object) => zoomMenu.insertItem(index + 2, object as ContextMenuItem)
                            onObjectRemoved: (index, object) => zoomMenu.removeItem(object as ContextMenuItem)
                        }
                    }
                }

                // Preview quality (Premiere's playback resolution): Auto / Full / 1/2 / 1/4 and proxies.
                PillButton {
                    id: qualityButton
                    objectName: "previewQualityButton"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !ProjectFile.demo
                    compact: true
                    iconName: MediaReadiness.useProxies ? "film" : ""
                    text: {
                        switch (MediaReadiness.previewQuality) {
                        //: Preview quality button; Auto shows the resolution it chose, e.g. "Auto 1/2"
                        case "auto": return qsTr("Auto %1").arg(MediaReadiness.previewDivisor === 1 ? qsTr("Full") : "1/" + MediaReadiness.previewDivisor)
                        case "full": return qsTr("Full")
                        case "half": return "1/2"
                        case "quarter": return "1/4"
                        }
                        return ""
                    }
                    tooltip: qsTr("Playback resolution and proxies")
                    onClicked: qualityMenu.popup(qualityButton, 0, -qualityMenu.implicitHeight - Theme.space(1))

                    ContextMenu {
                        id: qualityMenu
                        objectName: "previewQualityMenu"

                        QualityItem { value: "auto"; text: qsTr("Auto (fit the monitor)") }
                        QualityItem { value: "full"; text: qsTr("Full") }
                        QualityItem { value: "half"; text: qsTr("1/2") }
                        QualityItem { value: "quarter"; text: qsTr("1/4") }
                        ContextMenuSeparator {}
                        ContextMenuItem {
                            objectName: "captionKaraokeItem"
                            text: qsTr("Highlight caption words")
                            checkable: true
                            checked: Playback.captionKaraoke
                            onTriggered: Playback.captionKaraoke = !Playback.captionKaraoke
                        }
                        ContextMenuSeparator {}
                        ContextMenuItem {
                            objectName: "useProxiesItem"
                            text: qsTr("Use proxies")
                            checkable: true
                            checked: MediaReadiness.useProxies
                            onTriggered: MediaReadiness.useProxies = !MediaReadiness.useProxies
                        }
                        ContextMenuSeparator {}
                        ContextMenuItem {
                            text: qsTr("Media & cache settings…")
                            iconName: "settings"
                            onTriggered: MediaReadiness.openSettings()
                        }
                    }
                }

                // J/K/L speed indicator
                Rectangle {
                    id: shuttlePill
                    anchors.verticalCenter: parent.verticalCenter
                    visible: readouts.rightRoom >= Theme.space(70)
                    width: Theme.space(18)
                    height: Theme.controlHeightSmall
                    radius: height / 2
                    color: Playback.speed !== 0 ? Theme.accentFill : Theme.raised
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Shuttle speed %1").arg(speedText.text)

                    Behavior on color { ColorAnimation { duration: Theme.durFast } }

                    Row {
                        anchors.centerIn: parent
                        spacing: Theme.space(1.5)

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            //: Key caps of the shuttle keys (Premiere J/K/L)
                            text: qsTr("J K L")
                            color: Playback.speed !== 0 ? Theme.accentOnFill : Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                        Text {
                            id: speedText
                            anchors.verticalCenter: parent.verticalCenter
                            text: Playback.speedText
                            color: Playback.speed !== 0 ? Theme.accentOnFill : Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textSm
                            font.weight: Font.DemiBold
                        }
                    }
                }

                // Measured fps while playing (the sequence rate while stopped) and frames dropped since play started;
                // the tooltip has the presentation, decode, render and audio details.
                Text {
                    id: statsReadout
                    objectName: "playbackStats"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: readouts.rightRoom >= Theme.space(110)
                    //: Monitor readout, e.g. "29.97 fps · 0 dropped"
                    text: qsTr("%1 · %n dropped", "", Playback.droppedFrames).arg(Playback.fpsText)
                    color: Playback.droppedFrames > 0 ? Theme.warning : Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm

                    HoverHandler {
                        id: statsHover
                    }
                    Tooltip {
                        visible: statsHover.hovered && Playback.statsText !== ""
                        text: Playback.statsText
                    }
                }
            }
        }

        // ---- Time bar (0.1.3): the sequence, its marks and markers; seek, scrub and zoom ----
        ProgramTimeBar {
            id: timeBar
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
        }

        // ---- Transport (Premiere's Program Monitor buttons) ----
        Item {
            id: transport
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.space(11)

            Row {
                id: buttons
                anchors.centerIn: parent
                spacing: 0
                // Narrow monitors drop the marks, Go to In/Out and the frame steps first.
                readonly property bool roomy: transport.width >= Theme.controlHeight * 13 + Theme.space(11)

                IconButton {
                    objectName: "programAddMarker"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "bookmark"
                    tooltip: qsTr("Add Marker")
                    command: "marker.add"
                    onClicked: ProgramTools.addMarker()
                }
                IconButton {
                    objectName: "programMarkIn"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "mark-in"
                    tooltip: qsTr("Mark In")
                    command: "marker.markIn"
                    onClicked: ProjectController.markInAtPlayhead()
                }
                IconButton {
                    objectName: "programMarkOut"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "mark-out"
                    tooltip: qsTr("Mark Out")
                    command: "marker.markOut"
                    onClicked: ProjectController.markOutAtPlayhead()
                }
                IconButton {
                    objectName: "programGoToIn"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "go-to-in"
                    tooltip: qsTr("Go to In")
                    command: "marker.goToIn"
                    enabled: ProjectController.markInText !== ""
                    onClicked: ProjectController.goToMarkIn()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "step-back"
                    tooltip: qsTr("Step back one frame")
                    command: "transport.stepBackward"
                    onClicked: Playback.stepFrames(-1)
                }
                IconButton {
                    objectName: "programPlay"
                    anchors.verticalCenter: parent.verticalCenter
                    variant: "primary"
                    // Design language v3: the big white play button with a dark glyph.
                    implicitWidth: Theme.space(11)
                    iconSize: Theme.iconMd
                    checkedColor: Theme.playhead
                    checkedIconColor: Theme.canvas
                    iconName: Playback.playing ? "pause" : "play"
                    tooltip: Playback.playing ? qsTr("Pause") : qsTr("Play")
                    command: "transport.playStop"
                    onClicked: Playback.togglePlay()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "step-forward"
                    tooltip: qsTr("Step forward one frame")
                    command: "transport.stepForward"
                    onClicked: Playback.stepFrames(1)
                }
                IconButton {
                    objectName: "programGoToOut"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: buttons.roomy
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "go-to-out"
                    tooltip: qsTr("Go to Out")
                    command: "marker.goToOut"
                    enabled: ProjectController.markOutText !== ""
                    onClicked: ProjectController.goToMarkOut()
                }
                // P.2: Premiere's Play In to Out and Loop (qml/monitor/).
                IconButton {
                    objectName: "programPlayInToOut"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: transport.width >= Theme.controlHeightSmall * 15 + Theme.controlHeight
                    compact: true
                    iconName: "play-in-out"
                    tooltip: qsTr("Play In to Out")
                    command: "transport.playInToOut"
                    onClicked: Playback.playInToOut()
                }
                LoopButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: transport.width >= Theme.controlHeightSmall * 15 + Theme.controlHeight
                }
                Item {
                    width: Theme.space(2)
                    height: 1
                }
                IconButton {
                    objectName: "programLift"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "lift-edit"
                    tooltip: qsTr("Lift")
                    command: "sequence.lift"
                    enabled: ProgramTools.canLift
                    onClicked: ProgramTools.lift()
                }
                IconButton {
                    objectName: "programExtract"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "extract-edit"
                    tooltip: qsTr("Extract")
                    command: "sequence.extract"
                    enabled: ProgramTools.canLift
                    onClicked: ProgramTools.extract()
                }
                // Voice-over (M5m, A.8): records onto the armed audio track; a second click stops and keeps the take.
                IconButton {
                    objectName: "programRecordVoiceOver"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    checkable: true
                    checked: VoiceOver.recording
                    iconName: "mic"
                    checkedColor: Theme.danger
                    checkedIconColor: Theme.scrimText
                    tooltip: VoiceOver.recording ? qsTr("Stop recording (keeps the take)") : qsTr("Record Voice-over")
                    command: "record.toggle"
                    onClicked: {
                        checked = Qt.binding(() => VoiceOver.recording)
                        VoiceOver.toggle()
                    }
                }
                // P.2: Premiere's Export Frame dialog (qml/monitor/ExportFrameDialog.qml; Ctrl+Shift+E).
                IconButton {
                    id: exportFrameButton
                    objectName: "programExportFrame"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    iconName: "camera"
                    tooltip: qsTr("Export Frame")
                    command: "file.exportFrame"
                    enabled: !ProjectFile.demo
                    onClicked: Monitors.requestExportFrame(false)
                }
                // M5l: Premiere's Toggle Multi-Camera View (Shift+0).
                IconButton {
                    objectName: "multicamViewToggle"
                    anchors.verticalCenter: parent.verticalCenter
                    compact: true
                    implicitWidth: Theme.controlHeight
                    checkable: true
                    checked: Multicam.viewOn
                    iconName: "layout-grid"
                    tooltip: qsTr("Multi-Camera View")
                    command: "multicam.toggleView"
                    onClicked: Multicam.viewOn = !Multicam.viewOn
                }
            }
        }
    }

}
