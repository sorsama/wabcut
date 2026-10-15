import QtQuick

// What is still mock in the shell: the media library (media::probe, thumbnails and peaks are not merged yet)
// and the playback state (PlaybackController, M4). Everything about the sequence, selection, playhead and edit
// history is real and lives in the ProjectController bridge.
QtObject {
    id: project

    // Library cards show durations at this rate until media::probe provides real stream rates.
    readonly property int fps: 24

    // ---- Playback state (driven by PlaybackController in M4) ----
    property bool playing: false
    property real shuttleSpeed: 0                 // J/K/L ladder: -8, -4, -2, -1, 0, 1, 2, 4, 8; K+J/L: -1/4, 1/4
    readonly property real measuredFps: 24.0
    readonly property int droppedFrames: 0
    readonly property string decodeBackend: Qt.platform.os === "windows" ? "d3d11va"
                                          : Qt.platform.os === "osx" ? "videotoolbox" : "vaapi"

    // Library items. `colors` index Theme.clipColors and paint the mock gradient thumbnail.
    // Names and metadata here are project content (user data), so they are not translated.
    readonly property var media: [
        { id: 1, name: "Interview A.mov", kind: "video", frames: 1728, colors: [0, 1], meta: "3840×2160 · 23.976" },
        { id: 2, name: "Interview B.mov", kind: "video", frames: 1560, colors: [0, 7], meta: "3840×2160 · 23.976" },
        { id: 3, name: "City B-roll.mp4", kind: "video", frames: 576, colors: [1, 4], meta: "1920×1080 · 24" },
        { id: 4, name: "Drone Coast.mp4", kind: "video", frames: 912, colors: [4, 7], meta: "3840×2160 · 30" },
        { id: 5, name: "Street Walk.mov", kind: "video", frames: 744, colors: [2, 3], meta: "1920×1080 · 24" },
        { id: 6, name: "Product Close-up.mp4", kind: "video", frames: 288, colors: [5, 3], meta: "1920×1080 · 60" },
        { id: 7, name: "Music Bed - Lofi.wav", kind: "audio", frames: 3120, colors: [6, 2], meta: "48 kHz · Stereo" },
        { id: 8, name: "Room Tone.wav", kind: "audio", frames: 1080, colors: [7, 1], meta: "48 kHz · Mono" },
        { id: 9, name: "Whoosh SFX.wav", kind: "audio", frames: 48, colors: [3, 5], meta: "48 kHz · Stereo" },
        { id: 10, name: "VO Take 3.wav", kind: "audio", frames: 672, colors: [0, 7], meta: "48 kHz · Mono" },
        { id: 11, name: "Lower Third", kind: "text", frames: 120, colors: [3, 5], meta: "Graphic" },
        { id: 12, name: "Title Card", kind: "text", frames: 96, colors: [5, 4], meta: "Graphic" },
        { id: 13, name: "End Credits", kind: "text", frames: 288, colors: [1, 5], meta: "Graphic" }
    ]

    // HH:MM:SS:FF (non-drop) for library durations. Sequence timecode comes from ProjectController.
    function timecode(frame) {
        const f = Math.max(0, Math.round(frame))
        const pad = n => (n < 10 ? "0" : "") + n
        const totalSeconds = Math.floor(f / fps)
        return pad(Math.floor(totalSeconds / 3600)) + ":" + pad(Math.floor(totalSeconds / 60) % 60) + ":"
             + pad(totalSeconds % 60) + ":" + pad(f % fps)
    }
}
