import QtQuick

// The Program Monitor's time bar (0.1.3; Premiere's monitor time ruler), laid out like the Source Monitor's mini
// timeline: the In/Out range bar over a sunken track. The track is a SequenceTimeBar (C++, one batched mesh): where
// the sequence has content (gaps show), its edit points, markers, the In/Out range and time ticks. The playhead, the
// hover timecode and the zoom bar are QML over it; a playhead move only moves the playhead line.
//
// Pointer: press seeks, drag scrubs, the wheel steps frames, Ctrl/Alt+wheel zooms at the pointer, Shift+wheel pans
// a zoomed bar; the zoom bar under it (shown while zoomed) drags the window, a double-click shows the whole sequence.
// Keys (while focused): Left/Right a frame (Shift: five), Home/End, Up/Down the previous/next edit, +/- zoom, 0 all.
// UX rework (stream T): over a marker, the hover pill shows its name, comment, time and length (SequenceTimeBar.markerAt).
// 0.5 (stream B): a press and drag is a scrub for Playback (Playback.beginScrub / endScrub): with audio scrubbing on, a
// stopped transport plays a little of the mix at each new frame, as the timeline's ruler does.
FocusScope {
    id: root
    objectName: "programTimeBar"

    readonly property alias timeBar: bar
    readonly property real zoomBarHeight: Theme.space(1.5)
    // Focus ring only for keyboard focus (a FocusScope has no visualFocus).
    property bool pointerFocus: false
    onActiveFocusChanged: {
        if (!activeFocus)
            pointerFocus = false
    }

    implicitHeight: track.y + track.height + (bar.zoomed ? Theme.space(1) + zoomBarHeight : 0)
    activeFocusOnTab: true
    enabled: bar.active

    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Sequence position")
    Accessible.description: qsTr("Playhead %1. Left and Right step a frame; Up and Down go to the previous or next edit")
                                .arg(ProjectController.playheadText)

    Keys.onPressed: event => {
        const step = (event.modifiers & Qt.ShiftModifier) ? 5 : 1
        switch (event.key) {
        case Qt.Key_Left: Playback.stepFrames(-step); break
        case Qt.Key_Right: Playback.stepFrames(step); break
        case Qt.Key_Home: ProjectController.goToStart(); break
        case Qt.Key_End: ProjectController.goToEnd(); break
        case Qt.Key_Up: ProjectController.goToPreviousEdit(); break
        case Qt.Key_Down: ProjectController.goToNextEdit(); break
        case Qt.Key_Plus:
        case Qt.Key_Equal: bar.zoomAt(2, bar.playheadX >= 0 ? bar.playheadX : bar.width / 2); break
        case Qt.Key_Minus: bar.zoomAt(0.5, bar.playheadX >= 0 ? bar.playheadX : bar.width / 2); break
        case Qt.Key_0: bar.resetZoom(); break
        default: return
        }
        event.accepted = true
    }

    // The range bar: In to Out (one mark alone runs to the other end).
    Rectangle {
        objectName: "programRangeBar"
        // Clamped to the window when the bar is zoomed in past a mark.
        readonly property real from: Math.max(0, Math.min(bar.width, bar.hasIn ? bar.inX : 0))
        readonly property real to: Math.max(0, Math.min(bar.width, bar.hasOut ? bar.outX : bar.width))
        visible: (bar.hasIn || bar.hasOut) && to > from
        x: track.x + bar.x + from
        width: Math.max(Theme.hairline * 2, to - from)
        height: Theme.sourceRangeBarHeight
        radius: height / 2
        color: Theme.accentFill
    }

    Rectangle {
        id: track
        objectName: "programTimeTrack"
        anchors.left: parent.left
        anchors.right: parent.right
        y: Theme.sourceRangeBarHeight + Theme.space(1)
        height: Theme.sourceMiniTimelineHeight
        // Design language v3: a pill track, the In/Out range an accent band over it.
        radius: height / 2
        color: Theme.hover

        SequenceTimeBar {
            id: bar
            objectName: "sequenceTimeBar"
            anchors.fill: parent
            anchors.margins: Theme.hairline
            controller: ProjectController
            contentColor: Qt.alpha(Theme.textTertiary, 0.32)
            editColor: Qt.alpha(Theme.textSecondary, 0.7)
            tickColor: Qt.alpha(Theme.textTertiary, 0.55)
            rangeColor: Qt.alpha(Theme.accentFill, 0.45)
            markColor: Theme.accentStroke
            markerColors: Theme.markerColors
        }

        FocusRing {
            targetRadius: height / 2
            visible: root.activeFocus && !root.pointerFocus
        }
    }

    // Playhead: a white pill standing a little proud of the track (grab it anywhere on the bar).
    Item {
        objectName: "programTimePlayhead"
        visible: !isNaN(bar.playheadX)
        x: track.x + bar.x + Math.round(isNaN(bar.playheadX) ? 0 : bar.playheadX)
        y: track.y - Theme.space(0.75)
        height: track.height + Theme.space(1.5)

        Rectangle {
            x: -width / 2
            width: Theme.space(1)
            height: parent.height
            radius: width / 2
            color: Theme.playhead
        }
    }

    MouseArea {
        id: pointer
        objectName: "programTimeBarArea"
        anchors.fill: track
        hoverEnabled: true
        preventStealing: true
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor

        function barX(mx) { return Math.max(0, Math.min(bar.width, mx - bar.x)) }
        /// 0.5: a press-and-drag is a scrub (Playback hears it: audio scrubbing).
        property bool scrubbing: false
        function finishScrub() {
            if (pointer.scrubbing) {
                pointer.scrubbing = false
                Playback.endScrub()
            }
        }

        onPressed: mouse => {
            root.pointerFocus = true
            root.forceActiveFocus(Qt.MouseFocusReason)
            if (!pointer.scrubbing) {
                pointer.scrubbing = true
                Playback.beginScrub()
            }
            bar.seekToX(barX(mouse.x))
        }
        onPositionChanged: mouse => {
            if (pressed) {
                Playback.scrubMoving() // the drag: a move back plays backwards (the press played forward)
                bar.seekToX(barX(mouse.x))
            }
        }
        onReleased: pointer.finishScrub()
        onCanceled: pointer.finishScrub()
        onWheel: wheel => {
            const delta = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : wheel.angleDelta.x
            if (delta === 0)
                return
            if (wheel.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier))
                bar.zoomAt(Math.pow(1.25, delta / 120), barX(wheel.x))
            else if (wheel.modifiers & Qt.ShiftModifier)
                bar.panBy(-(bar.viewEnd - bar.viewStart) * delta / 1200)
            else
                Playback.stepFrames(delta > 0 ? -1 : 1)
        }
    }

    // The timecode under the pointer; over a marker (within a few pixels), the marker: name, comment, time and length,
    // on an opaque card like the tooltips (it is taller and reaches over the readouts above).
    Rectangle {
        id: hoverPill
        objectName: "programTimeHover"
        readonly property var marker: pointer.containsMouse && !pointer.pressed ? bar.markerAt(pointer.barX(pointer.mouseX))
                                                                                : ({})
        readonly property bool atMarker: marker.index !== undefined
        visible: pointer.containsMouse && !pointer.pressed
        readonly property real at: Math.max(0, Math.min(root.width - width, pointer.mouseX - width / 2))
        x: at
        y: track.y - height - Theme.space(1)
        z: 2
        width: hoverColumn.implicitWidth + Theme.space(3)
        height: hoverColumn.implicitHeight + Theme.space(1)
        radius: atMarker ? Theme.radiusClip : height / 2
        color: atMarker ? Theme.textPrimary : Theme.scrim
        readonly property color ink: atMarker ? Theme.canvas : Theme.scrimText

        Column {
            id: hoverColumn
            anchors.centerIn: parent
            spacing: 0

            Row {
                objectName: "programTimeHoverMarker"
                visible: hoverPill.atMarker
                spacing: Theme.space(1)
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.dotSize
                    height: Theme.dotSize
                    radius: width / 2
                    color: Theme.markerColor(hoverPill.marker.colorIndex ?? 0)
                }
                Text {
                    text: (hoverPill.marker.name ?? "") !== "" ? hoverPill.marker.name : qsTr("Marker")
                    color: hoverPill.ink
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
            }
            Text {
                visible: hoverPill.atMarker && (hoverPill.marker.comment ?? "") !== ""
                width: Math.min(implicitWidth, Theme.space(70))
                text: hoverPill.marker.comment ?? ""
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                color: Qt.alpha(hoverPill.ink, 0.72)
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Text {
                id: hoverText
                text: !pointer.containsMouse ? ""
                      : hoverPill.atMarker ? ((hoverPill.marker.durationText ?? "") !== ""
                                              ? qsTr("%1 · %2 long").arg(hoverPill.marker.timecode).arg(hoverPill.marker.durationText)
                                              : hoverPill.marker.timecode)
                      : bar.timecodeAt(pointer.barX(pointer.mouseX))
                color: hoverPill.ink
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
            }
        }
    }

    // The zoom bar: the visible window over the whole sequence, shown while zoomed.
    Rectangle {
        id: zoomBar
        objectName: "programZoomBar"
        visible: bar.zoomed
        anchors.left: parent.left
        anchors.right: parent.right
        y: track.y + track.height + Theme.space(1)
        height: root.zoomBarHeight
        radius: height / 2
        color: Theme.sunken

        Rectangle {
            id: thumb
            objectName: "programZoomThumb"
            x: bar.viewStart * zoomBar.width
            width: Math.max(Theme.space(3), (bar.viewEnd - bar.viewStart) * zoomBar.width)
            height: parent.height
            radius: height / 2
            color: thumbDrag.active || thumbHover.hovered ? Theme.accentStroke : Theme.borderStrong

            HoverHandler {
                id: thumbHover
                cursorShape: Qt.SizeHorCursor
            }
            DragHandler {
                id: thumbDrag
                property real startView: 0
                target: null
                xAxis.enabled: true
                yAxis.enabled: false
                onActiveChanged: {
                    if (active)
                        startView = bar.viewStart
                }
                onActiveTranslationChanged: {
                    if (active)
                        bar.setView(startView + activeTranslation.x / zoomBar.width,
                                    startView + activeTranslation.x / zoomBar.width + (bar.viewEnd - bar.viewStart))
                }
            }
        }
        TapHandler {
            onDoubleTapped: bar.resetZoom()
        }
    }
}
