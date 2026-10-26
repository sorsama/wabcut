import QtQuick

// Timeline lanes: hosts the C++ TimelineView (src/app/src/bridge/TimelineView), which draws every track,
// clip, waveform, marker line, the selection and the playhead line with batched scene-graph nodes — there are
// no per-clip QML delegates. This file only feeds it the design tokens from Theme and the ProjectController.
Item {
    id: root

    objectName: "timelineLanes"

    readonly property alias view: view
    // Editing tool and snapping (TimelinePanel owns the state).
    property string tool: "select"
    property bool snapping: true
    property bool showVolume: false
    property bool showSpeed: false

    signal pressed()

    // Trim settings from Preferences (M5i).
    Binding { target: Trim; property: "largeOffset"; value: Preferences.trimLargeOffset }
    Binding { target: Trim; property: "preroll"; value: Preferences.trimPreroll }
    Binding { target: Trim; property: "postroll"; value: Preferences.trimPostroll }
    Binding { target: Trim; property: "linkedSelection"; value: Preferences.linkedSelection }

    TimelineView {
        id: view
        objectName: "timelineView"
        anchors.fill: parent
        controller: ProjectController
        tool: root.tool
        snapping: root.snapping
        showVolume: root.showVolume
        showSpeed: root.showSpeed
        playing: Playback.playing
        focus: true                 // Esc reaches a drag in progress
        onPressed: root.pressed()
        // M5i: display settings (Preferences), linked selection and the trim edit points (Trim).
        filmstripMode: Preferences.filmstripMode
        rectifiedWaveforms: Preferences.rectifiedWaveforms
        logarithmicWaveforms: Preferences.logarithmicWaveforms
        volumeWaveforms: Preferences.volumeWaveforms
        showThroughEdits: Preferences.showThroughEdits
        showDuplicateFrames: Preferences.showDuplicateFrames
        showClipMarkers: Preferences.showClipMarkers
        trackKeyframes: Preferences.trackKeyframes
        showSnapIndicator: Preferences.snapIndicator
        linkedSelection: Preferences.linkedSelection
        // 0.2: the render bar's preview cache, the fade handle switches and Show Audio Time Units (one setting with
        // the Program Monitor's, kept by Monitors; ProjectController moves the playhead by samples while it is on).
        readiness: MediaReadiness
        showVideoFadeHandles: Preferences.showVideoFadeHandles
        showAudioFadeHandles: Preferences.showAudioFadeHandles
        audioTimeUnits: ProjectController.audioTimeUnits
        // 0.4: the speed after a retimed clip's name and the fx badge (Settings > Timeline & editing > Tracks, the
        // display settings), and Premiere's "Snap Playhead in Timeline When Snap Is Enabled" for the ruler.
        showClipSpeed: Preferences.timelineShowClipSpeed
        showFxBadges: Preferences.timelineShowFxBadges
        snapPlayhead: Preferences.snapPlayhead
        // 0.5: the caption tracks as lanes (Timeline Display > Show caption tracks); a selected cue lives in Captions,
        // so the Captions page follows it.
        showCaptionTracks: Preferences.timelineShowCaptionTracks
        captions: Captions
        trimPoints: Trim.points
        trimType: Trim.type
        onEditPointClicked: (clipId, edge, type, modifiers) => Trim.selectEditPoint(clipId, edge, type, modifiers)

        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Timeline lanes")

        // Design language v3: solid clips in their label colour with black names and waveforms, a white ring (with a
        // lane-coloured gap) round the selection, a flat white playhead, light transitions with a dark bow tie.
        style: ({
            laneColor: Theme.sunken,
            clipBorder: Theme.clipBorder,
            videoLabelColor: Theme.clipInk,
            audioLabelColor: Theme.clipInk,
            selectionColor: Theme.textPrimary,
            glowColor: Theme.accentGlow,
            glowSize: 0,          // v3: selection is the ring, no glow
            shadowSize: 0,        // v2: hover changes colour only
            shadowColor: Theme.shadow,
            playheadColor: Theme.playhead,
            marqueeFillColor: Theme.accentWash,
            marqueeBorderColor: Theme.accentStroke,
            clipColors: Theme.clipColors,
            audioFillColors: Theme.clipColors.map(c => Theme.audioClipFill(c)),
            audioBorderColors: Theme.clipColors.map(c => Theme.audioClipBorder(c)),
            waveformColors: Theme.clipColors.map(() => Theme.clipWave),
            labelFont: Qt.font({ family: Theme.fontSans, pixelSize: Theme.textSm, weight: Font.ExtraBold }),
            microSize: Theme.textMicro,
            microTracking: Theme.microTracking,
            laneRadius: Theme.radiusClip,
            clipRadius: Theme.radiusClip,
            selectionWidth: Theme.selectionWidth,
            selectionGap: Theme.selectionWidth,
            liftOffset: Theme.liftOffset,
            liftDuration: Theme.durFast,
            playheadWidth: Theme.playheadWidth,
            labelPadding: Theme.space(2),
            labelTop: Theme.space(0.5),
            waveTop: Theme.space(4.5),
            waveBottom: Theme.space(1),
            markerWidth: Theme.hairline,
            markerOpacity: Theme.markerLineOpacity,
            videoTrackHeight: Theme.trackHeightVideo,
            audioTrackHeight: Theme.trackHeightAudio,
            trackGap: Theme.trackGap,
            rulerLabelSpacing: Theme.rulerLabelSpacing,
            rulerMinorSpacing: Theme.rulerMinorSpacing,
            maxPixelsPerFrame: Theme.timelineMaxPixelsPerFrame,
            minLabelWidth: Theme.space(6),
            snapColor: Theme.accentStroke,
            ghostColor: Theme.textTertiary,
            razorColor: Theme.textPrimary,
            snapThreshold: Theme.snapThreshold,
            trimHandleWidth: Theme.trimHandleWidth,
            liftScale: Theme.liftScale,
            settleDuration: Theme.durBounce,
            rippleDuration: Theme.durRipple,
            flashDuration: Theme.durSpark,
            autoScrollZone: Theme.autoScrollZone,
            autoScrollSpeed: Theme.autoScrollSpeed,
            markerInkColor: Theme.clipInk,
            highlightColor: Theme.accentStroke,
            removalColor: Theme.danger,
            volumeColor: Theme.clipInk,
            volumeKeyColor: Theme.warning,
            volumeKeyBorder: Theme.clipInk,
            // Design language v3: solid bodies (no label strip; a name band over thumbnails), a flat white "beam"
            // (playhead and cut spark), dark hatching on the solid fill, no slice notch or slant.
            clipBodyColors: Theme.clipColors.map(c => Theme.clipBody(c)),
            stripHeight: Theme.clipStrip,
            clipHeaderHeight: Theme.clipHeaderHeight,
            hatchColor: Qt.alpha(Theme.clipInk, 0.3),
            beamStart: Theme.playhead,
            beamMid: Theme.playhead,
            beamEnd: Theme.playhead,
            playheadGlow: Theme.reduceMotion ? 0 : Theme.playheadGlow,
            selectedTint: Theme.clipTintSelected - Theme.clipTint,
            notchSize: 0,
            sliceAngle: 0,
            transitionColor: Theme.scrimText,
            transitionInk: Theme.clipInk,
            previewAddedColor: Theme.accentStroke,
            // M5i: marker colours, duplicate frame bands, through edits, trim brackets, filmstrips, track heights.
            markerColors: Theme.markerColors,
            duplicateColors: Theme.duplicateColors,
            duplicateBandHeight: Theme.duplicateBandHeight,
            throughEditColor: Qt.alpha(Theme.textPrimary, 0.75),
            trimRollColor: Theme.trimRoll,
            trimRippleColor: Theme.trimRipple,
            placeholderColor: Qt.alpha(Theme.textPrimary, Theme.filmstripPlaceholderAlpha),
            scrimColor: Qt.alpha(Theme.clipInk, Theme.filmstripScrimAlpha),
            minTrackHeight: Theme.trackHeightMin,
            maxTrackHeight: Theme.trackHeightMax,
            trackHeightStep: Theme.trackHeightStep,
            // M5l: nested and multi-camera clips, the track fader / pan lanes.
            nestedStripe: Theme.clipInk,
            multicamStripe: Theme.clipInk,
            nestedStripeHeight: Theme.nestedStripeHeight,
            trackVolumeLane: Theme.trackVolumeLane,
            trackPanLane: Theme.trackPanLane,
            // M5o: a responsive graphic's protected intro and outro.
            responsiveBand: Qt.alpha(Theme.clipInk, 0.14),
            responsiveEdge: Theme.clipInk,
            // 0.4: the fx badge after a clip's name: violet with stack effects, amber with changed or keyframed
            // Motion, Opacity, Volume, Pan or time remapping (its ink and hairline are markerInkColor).
            fxChangedColor: Theme.warning,
            fxEffectsColor: Theme.accentFill,
            // 0.5: caption lanes - compact rows, cues as solid neutral blocks with their first line in clipInk.
            captionTrackHeight: Theme.trackHeightCaption,
            captionColor: Theme.captionCue,
            captionInkColor: Theme.clipInk
        })
    }
}
