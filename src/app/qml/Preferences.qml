pragma Singleton
import QtQuick
import QtCore

// User preferences, persisted with QtCore Settings:
//   appearance/dark, appearance/accent, appearance/reduceMotion, appearance/highContrast — mirrored into Theme (v1 accent names are migrated:
//     violet → ultraviolet, lime → phosphor, orange → sodium, cyan → ice, pink → infrared)
//   window/nativeFrame — use the platform's title bar and borders instead of the custom frameless chrome
//   window/startWorkspace — the workspace Wabcut opens in (its id; 0-4: the first one made from that preset; M5j)
//   panels/keyframeLane — Effect Controls shows its keyframe lane
//   editing/fitClipChoice — the Fit Clip dialog's remembered answer ("" = ask; M5h)
//   monitor/safeMargins — action/title safe guides over the monitors (M5h)
//   timeline/* — the timeline display settings, linked selection, trim offsets and track height presets (M5i)
//   appearance/uiScale — the interface size in percent (100 / 125 / 150), read by main.cpp at launch (M5j)
//   review/author — the name review notes are signed with (M5m)
//   onboarding/welcomeDone, onboarding/trainerBest — the first-run welcome was completed or skipped; the Keyboard
//     Trainer's best streak (M5j)
//   appearance/launchIntro — the echo mark's intro plays when Wabcut starts (design language v3; reduce motion skips it)
//   home/starred — the projects starred on Home, as a JSON list of file paths (design language v3)
//   home/projectFilter, home/projectSort, home/projectView, home/advancedOpen, home/newProjectChoices — the Projects
//     page's filter, sort and view, the New project page's Advanced state and last choices (UX rework)
//   timeline/playbackScroll, snapPlayhead, clipTooltips, showClipSpeed, showFxBadges — playback auto-scroll, the
//     playhead snap, the clip hover tooltip, the speed and fx badges on clips (0.4)
//   timeline/showCaptionTracks — the caption tracks as lanes above the video tracks (0.5)
//   monitor/platformSafeZone — the Program Monitor's platform safe zones on portrait sequences (0.4)
//   panels/masksExpanded, panels/transcriptFollowPlayhead — Effect Controls' Masks section, the Transcript following
//     the playhead (0.4)
//   mediaBrowser/lastFolder, highlights/shortLeadIn, shortTail, shortMinLength, shortCaptionPreset,
//     shorts/captionPreset — the Media Browser's folder, Highlights' and the Shorts panel's Short options (0.4)
//   power/keepAwake — keep the computer awake while exports and long jobs run (0.4)
// The singleton is created when Main first reads it, which applies the saved appearance before the panels bind
// to Theme, then turns on Theme.colorTransitions (start-up never animates).
QtObject {
    id: prefs

    property bool nativeFrame: false
    // The workspace Wabcut opens in (M5j, the welcome): 0 Edit, 1 Audio, 3 Color or 4 Captions.
    property int startWorkspace: 0
    // Effect Controls shows its keyframe lane (the side panel widens for it in the Edit workspace).
    property bool keyframeLane: true
    // 0.1.3 panel docking: no longer used (each workspace keeps its own layout in workspaceLayout); kept so an older
    // settings file still reads.
    property string dockLayout: ""
    // 0.1.3 workspaces (Workspaces.qml): the user's workspaces, their order and each one's panel layout and floating
    // windows, as versioned JSON ({version: 2, workspaces: [...]}); "" is the first start (one from the Edit preset).
    property string workspaceLayout: ""
    // Four-point edits whose ranges differ (M5h): "" asks every time (the Fit Clip dialog); otherwise "speed",
    // "ignoreSourceIn", "ignoreSourceOut", "ignoreSequenceIn" or "ignoreSequenceOut" ("Always use this choice").
    property string fitClipChoice: ""
    // Safe margins over the monitors (action safe and title safe).
    property bool safeMargins: false
    // Timeline display settings (M5i): Premiere's Video Head / Head and Tail / Continuous thumbnails ("off", "head",
    // "headTail", "continuous"), rectified and logarithmic waveforms, through edits, duplicate frame markers, clip
    // markers, the snap indicator and linked selection.
    property string filmstripMode: "continuous"
    property bool rectifiedWaveforms: false
    property bool logarithmicWaveforms: false
    // p02 audio: waveforms scaled by the clip's Audio Gain and Volume ("Show Audio Waveforms Using Clip Volume").
    property bool volumeWaveforms: false
    property bool showThroughEdits: true
    property bool showDuplicateFrames: false
    property bool showClipMarkers: true
    // M5l: the track fader or pan lane on audio tracks: "off", "volume" or "pan".
    property string trackKeyframes: "off"
    property bool snapIndicator: true
    property bool linkedSelection: true
    // 0.2: Premiere's Show Video / Audio Fade Handles and Show Sequence Label Color (Timeline Display Settings).
    property bool showVideoFadeHandles: true
    property bool showAudioFadeHandles: true
    property bool showSequenceLabelColor: false
    // 0.2: Sequence > Selection Follows Playhead, remembered like Premiere's menu switch (Main syncs ProjectController).
    property bool selectionFollowsPlayhead: false
    // Trim (Premiere's Preferences > Trim and Playback): the Large trim offset, and the play-around preroll/postroll.
    property int trimLargeOffset: 5
    property real trimPreroll: 2
    property real trimPostroll: 2
    // Track height presets: a JSON list of { name, video: [...], audio: [...] } (TimelineView.trackHeightPreset()).
    property string trackHeightPresets: "[]"
    // The interface size in percent (M5j): 100, 125 or 150. Applies when Wabcut next starts (main.cpp).
    property int uiScale: 100
    /// The stored size as Wabcut applies it: 125 or 150, anything else 100 (UiScale.cpp, savedUiScale).
    readonly property int effectiveUiScale: uiScale === 125 || uiScale === 150 ? uiScale : 100
    // Onboarding (M5j): the welcome ran (completed or skipped), and the Keyboard Trainer's best streak.
    property bool welcomeDone: false
    property int trainerBest: 0
    // Review notes (M5m): the name the notes and replies are signed with ("" = "Editor").
    property string reviewAuthor: ""
    // Design language v3: the echo mark's launch intro (onboarding/LaunchIntro.qml), and Home's starred projects as a
    // JSON list of project file paths (starredProjects() / setStarred()).
    property bool launchIntro: true
    property string homeStarred: "[]"
    // UX rework (stream H, Home as a start window): the Projects page's filter ("all" | "starred"), sort ("opened" |
    // "name") and view ("grid" | "list"); the New project page's Advanced settings open or closed, and its last
    // choices as JSON ({ presetId, width, height, rateNum, rateDen, advanced: { Home.createProject's Advanced keys } }).
    // (home/recentProjects, home/defaultFolder and home/recentLimit belong to HomeBridge, in C++.)
    property string homeProjectFilter: "all"
    property string homeProjectSort: "opened"
    property string homeProjectView: "grid"
    property bool homeAdvancedOpen: false
    property string homeNewProjectChoices: "{}"

    readonly property Settings _home: Settings {
        category: "home"
        property alias starred: prefs.homeStarred
        property alias projectFilter: prefs.homeProjectFilter
        property alias projectSort: prefs.homeProjectSort
        property alias projectView: prefs.homeProjectView
        property alias advancedOpen: prefs.homeAdvancedOpen
        property alias newProjectChoices: prefs.homeNewProjectChoices
    }

    /// The starred projects' file paths.
    function starredProjects(): var {
        const list = prefs.parsedJson(prefs.homeStarred, [])
        return Array.isArray(list) ? list.filter(p => typeof p === "string" && p !== "") : []
    }
    function isStarred(path: string): bool {
        return path !== "" && prefs.starredProjects().includes(path)
    }
    /// Stars or unstars the project at `path` (persisted at once).
    function setStarred(path: string, starred: bool) {
        if (path === "")
            return
        const list = prefs.starredProjects().filter(p => p !== path)
        if (starred)
            list.unshift(path)
        prefs.homeStarred = JSON.stringify(list)
    }

    readonly property Settings _onboarding: Settings {
        category: "onboarding"
        property alias welcomeDone: prefs.welcomeDone
        property alias trainerBest: prefs.trainerBest
    }

    readonly property Settings _appearance: Settings {
        id: appearance
        category: "appearance"
        property bool dark: true
        property string accent: "ultraviolet"
        property bool reduceMotion: false
        property bool highContrast: false
        property alias uiScale: prefs.uiScale
        property alias launchIntro: prefs.launchIntro

        // 0.1.3 custom theme (Settings > Appearance): Theme.customColors / customClipColors as JSON, mirrored below.
        property string customColors: "{}"
        property string customClipColors: "[]"
        property alias themeName: prefs.themeName
        property alias savedThemes: prefs.savedThemes
    }

    // ---- 0.1.3 Settings window (WP-S) ----
    // Appearance: the theme last chosen (a preset's or a saved theme's name; "" = the built-in look, edited colours
    // show as "Custom"), and the user's saved themes as a JSON list of ThemeFiles themes ({format, version, name, dark,
    // accent, colors, clipColors}).
    property string themeName: ""
    property string savedThemes: "[]"
    // Interface: the title bar's GPU and save-status pills (moved into Settings; they can come back here). (The UX
    // rework removed the left rail; the "interface/railLayout" key it left is not read any more.)
    property bool showGpuStatus: false
    property bool showSaveStatus: false
    // AI features (Settings > AI): off hides the AI entry points and turns AiController's offline mode on (no AI
    // request leaves the computer); `aiOfflineBefore` is the offline mode to restore when they are turned back on.
    property bool aiEnabled: true
    property bool aiOfflineBefore: false
    // Settings > General: the autosave interval in seconds (0 = ProjectFile's built-in minute; Main applies it).
    property int autosaveSeconds: 0

    readonly property Settings _project: Settings {
        category: "project"
        property alias autosaveSeconds: prefs.autosaveSeconds
    }

    readonly property Settings _interface: Settings {
        category: "interface"
        property alias showGpuStatus: prefs.showGpuStatus
        property alias showSaveStatus: prefs.showSaveStatus
    }
    readonly property Settings _aiFeatures: Settings {
        category: "aiFeatures"
        property alias enabled: prefs.aiEnabled
        property alias offlineBefore: prefs.aiOfflineBefore
    }

    readonly property Settings _window: Settings {
        category: "window"
        property alias nativeFrame: prefs.nativeFrame
        property alias startWorkspace: prefs.startWorkspace
    }

    readonly property Settings _panels: Settings {
        category: "panels"
        property alias keyframeLane: prefs.keyframeLane
        property alias dockLayout: prefs.dockLayout
        property alias masksExpanded: prefs.masksExpanded
        property alias transcriptFollowPlayhead: prefs.transcriptFollowPlayhead
    }

    readonly property Settings _workspaces: Settings {
        category: "workspaces"
        property alias layout: prefs.workspaceLayout
    }

    readonly property Settings _editing: Settings {
        category: "editing"
        property alias fitClipChoice: prefs.fitClipChoice
    }

    readonly property Settings _monitor: Settings {
        category: "monitor"
        property alias safeMargins: prefs.safeMargins
        property alias platformSafeZone: prefs.platformSafeZone
    }

    readonly property Settings _timeline: Settings {
        category: "timeline"
        property alias filmstripMode: prefs.filmstripMode
        property alias rectifiedWaveforms: prefs.rectifiedWaveforms
        property alias logarithmicWaveforms: prefs.logarithmicWaveforms
        property alias volumeWaveforms: prefs.volumeWaveforms
        property alias showThroughEdits: prefs.showThroughEdits
        property alias showDuplicateFrames: prefs.showDuplicateFrames
        property alias showClipMarkers: prefs.showClipMarkers
        property alias trackKeyframes: prefs.trackKeyframes
        property alias snapIndicator: prefs.snapIndicator
        property alias linkedSelection: prefs.linkedSelection
        property alias showVideoFadeHandles: prefs.showVideoFadeHandles
        property alias showAudioFadeHandles: prefs.showAudioFadeHandles
        property alias showSequenceLabelColor: prefs.showSequenceLabelColor
        property alias selectionFollowsPlayhead: prefs.selectionFollowsPlayhead
        property alias trimLargeOffset: prefs.trimLargeOffset
        property alias trimPreroll: prefs.trimPreroll
        property alias trimPostroll: prefs.trimPostroll
        property alias trackHeightPresets: prefs.trackHeightPresets
    }
    // ---- UX rework 2026-10-23, stream T (timeline and monitor QoL) ----
    // The Tracks panel's Snap toggle (S) and its time remapping view toggle, remembered between runs (timeline/snapping,
    // timeline/showSpeed). The volume lanes are not here: each workspace says whether it shows them (Workspaces).
    property bool timelineSnapping: true
    property bool timelineShowSpeed: false
    readonly property Settings _timelineView: Settings {
        category: "timeline"
        property alias snapping: prefs.timelineSnapping
        property alias showSpeed: prefs.timelineShowSpeed
    }

    // ---- 0.4, stream A (the timeline): Premiere's Timeline Playback Auto-Scrolling ("page", "smooth" or "none") and
    // Snap Playhead in Timeline When Snap Is Enabled; the clip hover tooltip; the speed and fx badges on clips.
    property string timelinePlaybackScroll: "page"
    property bool snapPlayhead: true
    property bool showClipTooltips: true
    property bool timelineShowClipSpeed: true
    property bool timelineShowFxBadges: true
    readonly property Settings _timeline04: Settings {
        category: "timeline"
        property alias playbackScroll: prefs.timelinePlaybackScroll
        property alias snapPlayhead: prefs.snapPlayhead
        property alias clipTooltips: prefs.showClipTooltips
        property alias showClipSpeed: prefs.timelineShowClipSpeed
        property alias showFxBadges: prefs.timelineShowFxBadges
    }
    // 0.5, stream A: Timeline Display > Show caption tracks (Premiere's caption tracks as lanes above the video tracks).
    property bool timelineShowCaptionTracks: true
    readonly property Settings _timeline05: Settings {
        category: "timeline"
        property alias showCaptionTracks: prefs.timelineShowCaptionTracks
    }
    // 0.4, stream B: the Program Monitor's platform safe zones on portrait sequences ("off", "shorts", "tiktok", "reels").
    property string platformSafeZone: "off"
    // 0.4, stream C: Effect Controls' Masks section open while the clip has no mask; the Transcript follows the playhead.
    property bool masksExpanded: false
    property bool transcriptFollowPlayhead: true
    // 0.4, stream D: the folder the Media Browser showed last; Highlights' Short options (lead-in, tail, "at least"
    // seconds with 0 = Off, caption style) and the Shorts panel's caption style.
    property string mediaBrowserLastFolder: ""
    property real highlightsShortLeadIn: 3
    property real highlightsShortTail: 1
    property int highlightsShortMinLength: 0
    property string highlightsShortCaptionPreset: "Shorts"
    property string shortsCaptionPreset: "Shorts"
    readonly property Settings _mediaBrowser: Settings {
        category: "mediaBrowser"
        property alias lastFolder: prefs.mediaBrowserLastFolder
    }
    readonly property Settings _highlights: Settings {
        category: "highlights"
        property alias shortLeadIn: prefs.highlightsShortLeadIn
        property alias shortTail: prefs.highlightsShortTail
        property alias shortMinLength: prefs.highlightsShortMinLength
        property alias shortCaptionPreset: prefs.highlightsShortCaptionPreset
    }
    readonly property Settings _shorts: Settings {
        category: "shorts"
        property alias captionPreset: prefs.shortsCaptionPreset
    }
    // 0.4, stream E: keep the computer awake while exports and long jobs run (Windows; Settings > General).
    property bool keepAwake: true
    readonly property Settings _power: Settings {
        category: "power"
        property alias keepAwake: prefs.keepAwake
    }

    readonly property Settings _review: Settings {
        category: "review"
        property alias author: prefs.reviewAuthor
    }

    // 0.1.3 (WP-M) gutter drags (WorkbenchSplitter), saved as resize/live: false = a guide follows the pointer and the
    // panels resize once, on release; true = the panels follow the pointer (one layout per frame). Settings > Interface.
    property bool liveResize: false
    readonly property Settings _resize: Settings {
        category: "resize"
        property alias live: prefs.liveResize
    }

    readonly property Connections _themeSync: Connections {
        target: Theme
        function onDarkChanged() { appearance.dark = Theme.dark }
        function onAccentNameChanged() { appearance.accent = Theme.accentName }
        function onReduceMotionChanged() { appearance.reduceMotion = Theme.reduceMotion }
        function onHighContrastChanged() { appearance.highContrast = Theme.highContrast }
        function onCustomColorsChanged() { appearance.customColors = JSON.stringify(Theme.customColors ?? {}) }
        function onCustomClipColorsChanged() { appearance.customClipColors = JSON.stringify(Theme.customClipColors ?? []) }
    }

    /// The saved JSON as a value, or `fallback` when it does not parse (a hand-edited settings file).
    function parsedJson(text: string, fallback: var): var {
        try {
            const value = JSON.parse(text)
            return value ?? fallback
        } catch (e) {
            return fallback
        }
    }

    Component.onCompleted: {
        Theme.dark = appearance.dark
        Theme.accentName = Theme.canonicalAccent(appearance.accent)
        appearance.accent = Theme.accentName // a v1 name is saved under its v2 name from now on
        Theme.reduceMotion = appearance.reduceMotion
        Theme.highContrast = appearance.highContrast
        // 0.1.3: the custom colours, checked (a hand-edited settings file keeps the built-in values).
        Theme.customColors = Theme.sanitizedColors(prefs.parsedJson(appearance.customColors, {}))
        Theme.customClipColors = Theme.sanitizedClipColors(prefs.parsedJson(appearance.customClipColors, []))
        Theme.uiScale = UiScaleInfo.applied // what main.cpp applied at launch (review P2-47: not the raw value)
        Theme.colorTransitions = true
    }

    // Voice-over recording (A.8, M5m; Settings > Audio, dialogs/AudioSettings.qml), saved under voiceOver/*: the
    // microphone ("" = the system default) and the VoiceOver bridge's options. VoiceOverSync (Main) hands them to the
    // bridge at start-up and saves what changes there (a latency measurement sets calibrationMs).
    property string voiceOverInput: ""
    property int voiceOverCountIn: 3
    property bool voiceOverBeep: true
    property real voiceOverPreroll: 2
    property real voiceOverPostroll: 1
    property bool voiceOverMonitor: false
    property real voiceOverMonitorGainDb: 0
    property real voiceOverCalibrationMs: 0
    readonly property Settings _voiceOver: Settings {
        category: "voiceOver"
        property alias input: prefs.voiceOverInput
        property alias countIn: prefs.voiceOverCountIn
        property alias beep: prefs.voiceOverBeep
        property alias prerollSeconds: prefs.voiceOverPreroll
        property alias postrollSeconds: prefs.voiceOverPostroll
        property alias monitor: prefs.voiceOverMonitor
        property alias monitorGainDb: prefs.voiceOverMonitorGainDb
        property alias calibrationMs: prefs.voiceOverCalibrationMs
    }

    // ---- UX rework 2026-10-23, stream E (the Effects panel) ----
    // The Effects panel's view ("list", "compact" or "icons") and its panel menu's Show Audio Plug-ins, saved as
    // effects/view and effects/showPlugins. The C++ keeps its own keys in the same group under other names
    // (effects/expandedFolders, effects/lastOpenedFolder: EffectsTree; effects/pluginGroupBy: Plugins;
    // effects/presetsFolder: EffectPresets). The filter is not saved: a filter left on would hide effects at the next start.
    property string effectsView: "list"
    property bool effectsShowPlugins: true
    readonly property Settings _effects: Settings {
        category: "effects"
        property alias view: prefs.effectsView
        property alias showPlugins: prefs.effectsShowPlugins
    }
}
