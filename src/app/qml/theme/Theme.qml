pragma Singleton
import QtQuick

// Wabcut design tokens: the single source of truth for colour, geometry, type and motion.
// Design language v3 "Echo" (docs/design-language.md, the binding direction since 2026-10-15): a black stage, rounded
// islands with dark tab strips, pills, violet with lime, the echo mark. Tokens and components are listed in
// docs/design-system.md. Components read tokens from here; never hard-code.
QtObject {
    id: theme

    // ---- User preferences (persisted by Preferences via QtCore Settings; defaults = the brand look) ----
    property bool dark: true
    // ultraviolet | infrared | phosphor | sodium | ice: the keys of Violet, Pink, Lime, Orange and Blue (v3 kept the v2
    // keys, so saved settings carry over). v1 names still resolve (accentAliases).
    property string accentName: "ultraviolet"
    property bool reduceMotion: false
    // High contrast (M5j, accessibility): pure black / white surfaces, AAA text (>= 7:1, tested) and strong borders.
    property bool highContrast: false
    // The interface size the app started at, in percent (100 / 125 / 150: Preferences.uiScale, applied at launch as
    // QT_SCALE_FACTOR; main.cpp sets it). Informational for QML.
    property int uiScale: 100
    // When true, colour tokens cross-fade (ColorAnimation, durState) on theme/accent changes. It stays
    // false until the saved preferences are applied, so start-up never animates away from the defaults.
    property bool colorTransitions: false
    // Layout density, set by Main from the window height (< compactBelowHeight). Compact mode trims panel chrome
    // and track heights so the timeline keeps its tracks on short screens. Not persisted.
    property bool compact: false
    // 0.1.3 custom colours (Settings > Appearance): token -> "#RRGGBB" / "#AARRGGBB" over the dark/light targets
    // (colorTokenKeys); a key not in the map keeps its built-in value. Replace the whole map to change it (a new object
    // re-evaluates the bindings, and the colours still cross-fade). High contrast ignores them: accessibility wins.
    // Preferences persists both, and custom themes (presets, saved, imported files) set them.
    property var customColors: ({})
    // The 16 clip label colours as one list (exactly 16 entries), or [] for the built-in ones.
    property var customClipColors: []
    // 0.1.3 built-in themes (Settings > Appearance): their colour maps over the dark/light targets, by theme name.
    // The default dark and light looks are the design language's own (no map); these re-tint surfaces and text,
    // keeping meta text at 4.5:1 on panels and cards.
    readonly property var themePresetColors: ({
        "Graphite": {
            canvas: "#0A0A0B", sunken: "#070708", panel: "#17171A", section: "#1E1E22", raised: "#202024",
            hover: "#2A2A2F", well: "#36363C", border: "#17FFFFFF", borderStrong: "#2EFFFFFF", rimLight: "#00FFFFFF",
            textPrimary: "#F2F2F4", textSecondary: "#B4B4BC", textTertiary: "#8D8D96", textDisabled: "#55555C" },
        "Midnight": {
            canvas: "#02040A", sunken: "#04070F", panel: "#0E1320", section: "#141A28", raised: "#161D2E",
            hover: "#1F283D", well: "#2A3550", border: "#1A8FB3FF", borderStrong: "#338FB3FF", rimLight: "#00FFFFFF",
            textPrimary: "#EDF2FB", textSecondary: "#A8B3C9", textTertiary: "#8390A8", textDisabled: "#4A5570" },
        "Paper": {
            canvas: "#E9E5DC", sunken: "#DFDACF", panel: "#FAF8F3", section: "#F3F0E9", raised: "#FFFFFF",
            hover: "#F1ECE2", well: "#DCD6CA", border: "#1A3B2F1F", borderStrong: "#2E3B2F1F", rimLight: "#00FFFFFF",
            textPrimary: "#1F1A12", textSecondary: "#4D4536", textTertiary: "#6E6553", textDisabled: "#B3AA98",
            playhead: "#1F1A12" }
    })
    // The built-in surfaces of each mode as plain values (theme preview cards draw a theme's unset tokens with them,
    // whatever the window shows now).
    readonly property var modeSwatches: ({
        dark: { canvas: "#000000", panel: "#141415", raised: "#1D1D20", textPrimary: "#F2F2F3", textTertiary: "#8C8C94", border: "#29FFFFFF" },
        light: { canvas: "#EDEDF0", panel: "#FFFFFF", raised: "#F0F0F3", textPrimary: "#0B0B0C", textTertiary: "#6B6B74", border: "#29000000" }
    })
    // The colour picker's fixed inks: the hue strip's stops, the saturation/value square's white and black ends, and
    // the marker drawn over light or dark picks.
    readonly property var pickerHueStops: ["#FF0000", "#FFFF00", "#00FF00", "#00FFFF", "#0000FF", "#FF00FF", "#FF0000"]
    readonly property color pickerWhite: "#FFFFFF"
    readonly property color pickerBlack: "#000000"
    readonly property color pickerClear: "#00000000"
    // Every colour token a custom theme may set (ThemeFiles.h keeps the same list for theme files).
    readonly property var colorTokenKeys: ["canvas", "sunken", "panel", "section", "raised", "hover", "well", "border", "borderStrong",
                                           "rimLight", "textPrimary", "textSecondary", "textTertiary", "textDisabled",
                                           "accentFill", "accentOnFill", "accentStroke", "accentSoft", "accentDeep",
                                           "beamMid", "beamEnd", "playhead", "success", "warning", "danger", "info"]
    /// The custom colour for `key` if one is set (and high contrast is off), else `fallback`.
    function custom(key, fallback) {
        if (highContrast)
            return fallback
        const value = customColors ? customColors[key] : undefined
        return typeof value === "string" && value !== "" ? value : fallback
    }
    /// `colors` reduced to known tokens with "#RRGGBB" / "#AARRGGBB" values, upper case (settings files can be
    /// edited by hand or come from another version).
    function sanitizedColors(colors) {
        const out = {}
        if (!colors || typeof colors !== "object")
            return out
        for (const key of colorTokenKeys) {
            const value = colors[key]
            if (typeof value === "string" && /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(value))
                out[key] = value.toUpperCase()
        }
        return out
    }
    /// 16 "#RRGGBB" label colours, or [] (the built-in ones).
    function sanitizedClipColors(list) {
        if (!Array.isArray(list) || list.length !== defaultClipColors.length)
            return []
        if (!list.every(c => typeof c === "string" && /^#[0-9a-fA-F]{6}$/.test(c)))
            return []
        return list.map(c => c.toUpperCase())
    }

    // ---- Accents ----
    // fill: filled buttons, the active segment, selected states; black text and glyphs sit on it (onFill, >= 4.5:1,
    // tested).  stroke: thin accent lines on surfaces (ranges, dots, slider fill); >= 3:1 on panel and raised (the
    // light stroke is a deeper shade: the fills are too light for white).  soft: hover highlight.  deep: pressed fill.
    // beam: v3 has no gradients, so its stops equal the fill (kept for theme files and old callers).
    readonly property var accents: ({
        "ultraviolet": { fill: "#9B6CFF", onFill: "#000000", strokeDark: "#9B6CFF", strokeLight: "#6A3DE8", soft: "#C3A8FF", deep: "#8A5CFF", beamMid: "#9B6CFF", beamEnd: "#9B6CFF" },
        "infrared":    { fill: "#FF5FA8", onFill: "#000000", strokeDark: "#FF5FA8", strokeLight: "#C0256E", soft: "#FFC2DD", deep: "#E84A93", beamMid: "#FF5FA8", beamEnd: "#FF5FA8" },
        "phosphor":    { fill: "#D6F54C", onFill: "#000000", strokeDark: "#D6F54C", strokeLight: "#5E7A00", soft: "#EEFBB0", deep: "#BEDC34", beamMid: "#D6F54C", beamEnd: "#D6F54C" },
        "sodium":      { fill: "#FF7A2F", onFill: "#000000", strokeDark: "#FF7A2F", strokeLight: "#B54700", soft: "#FFCAA8", deep: "#EA661B", beamMid: "#FF7A2F", beamEnd: "#FF7A2F" },
        "ice":         { fill: "#4D8DFF", onFill: "#000000", strokeDark: "#4D8DFF", strokeLight: "#1F5FD6", soft: "#B8D2FF", deep: "#3A78EA", beamMid: "#4D8DFF", beamEnd: "#4D8DFF" }
    })
    // The names Settings shows for the accent keys.
    readonly property var accentLabels: ({ "ultraviolet": qsTr("Violet"), "infrared": qsTr("Pink"), "phosphor": qsTr("Lime"),
                                           "sodium": qsTr("Orange"), "ice": qsTr("Blue") })
    readonly property var accentOrder: ["ultraviolet", "infrared", "phosphor", "sodium", "ice"]
    // Colour names (v1 settings, old projects, and v3's own names): lime → phosphor, orange → sodium, cyan and blue →
    // ice, pink → infrared, violet → ultraviolet.
    readonly property var accentAliases: ({ "violet": "ultraviolet", "lime": "phosphor", "orange": "sodium", "cyan": "ice",
                                            "blue": "ice", "pink": "infrared" })
    function canonicalAccent(name) {
        if (accents[name] !== undefined)
            return name
        return accentAliases[name] ?? "ultraviolet"
    }
    readonly property var accent: accents[canonicalAccent(accentName)]

    readonly property color accentFill: live.accentFill
    readonly property color accentOnFill: live.accentOnFill
    readonly property color accentStroke: live.accentStroke
    readonly property color accentSoft: live.accentSoft
    readonly property color accentDeep: live.accentDeep
    readonly property color accentGlow: Qt.alpha(live.accentStroke, 0.4)   // static selected/focused glow only
    // The echo mark's two inks (logo, launch intro, loaders), independent of the accent.
    readonly property color brand: "#9B6CFF"
    readonly property color brandLime: "#D6F54C"

    // ---- v2's beam, flat in v3 (every stop is the accent fill, so old gradients draw solid) ----
    readonly property color beamStart: live.accentFill
    readonly property color beamMid: live.beamMid
    readonly property color beamEnd: live.beamEnd
    readonly property var brandBeam: [brand, brand, brandLime]

    // ---- The six hues: panel dots, tags, clip kinds and project cards ----
    readonly property color hueViolet: "#9B6CFF"
    readonly property color hueLime: "#D6F54C"
    readonly property color huePink: "#FF5FA8"
    readonly property color hueMint: "#3EE6B8"
    readonly property color hueBlue: "#4D8DFF"
    readonly property color hueOrange: "#FF7A2F"
    readonly property var hues: ({ violet: hueViolet, lime: hueLime, pink: huePink, mint: hueMint, blue: hueBlue, orange: hueOrange })
    readonly property var hueOrder: ["violet", "blue", "pink", "lime", "orange", "mint"]
    /// The hue called `name` (violet, lime, pink, mint, blue, orange); the accent stroke for "accent" or an unknown name.
    function hue(name) { return hues[name] ?? live.accentStroke }

    // ---- Surfaces (dark: a black stage under near-black islands; light: white islands on a grey stage) ----
    readonly property color canvas: live.canvas        // window background (shows in the gutters only)
    readonly property color sunken: live.sunken        // panel tab strips, timeline well, segmented tracks
    readonly property color panel: live.panel          // docked panels (the islands)
    readonly property color section: live.section      // cards inside a panel (Motion, Opacity, …)
    readonly property color raised: live.raised        // buttons, fields, chips, menus, popovers
    readonly property color hover: live.hover          // hover and selected rows, the selected nav item
    readonly property color well: live.well            // stepper buttons, slider tracks, scroll thumbs
    readonly property color border: live.border
    readonly property color borderStrong: live.borderStrong
    readonly property color rimLight: live.rimLight    // v2's 1px top highlight: transparent in v3
    readonly property color shadow: live.shadow        // under popovers and dragged items only (panels have none)

    // ---- Text ----
    readonly property color textPrimary: live.textPrimary
    readonly property color textSecondary: live.textSecondary
    readonly property color textTertiary: live.textTertiary
    readonly property color textDisabled: live.textDisabled

    // ---- Semantic ----
    readonly property color success: custom("success", dark ? "#3EE6B8" : "#0B7A5C")
    readonly property color warning: custom("warning", "#FBBF24")
    readonly property color danger:  custom("danger", "#F43F5E")
    readonly property color info:    custom("info", dark ? "#4D8DFF" : "#1F5FD6")
    // Slider knobs and toggle thumbs: white in every theme (light themes add a hairline round them).
    readonly property color knob: "#FFFFFF"
    // The focus ring: white on dark, black on light (the reference's 2px outline).
    readonly property color focus: live.textPrimary

    // ---- Media surfaces (independent of light/dark: they sit on imagery or clip colours) ----
    readonly property color scrim: Qt.rgba(0, 0, 0, 0.72)             // badges over thumbnails/video
    readonly property color scrimText: "#FFFFFF"
    readonly property color videoBlack: "#000000"                     // monitor letterbox
    // Monitor well: a dark neutral in BOTH themes, so picture contrast is judged against the same surround
    // whatever the UI theme (pro-monitoring practice).
    readonly property color monitorSurround: "#000000"
    readonly property color monitorSurroundBorder: Qt.rgba(1, 1, 1, 0.06)
    readonly property color clipInk: "#000000"                        // labels and glyphs on accent, hue and clip fills
    readonly property color scrimTextMuted: Qt.rgba(1, 1, 1, 0.7)     // secondary text over video/imagery
    readonly property color guide: Qt.rgba(1, 1, 1, 0.18)             // safe-area guides over video
    readonly property var mockFrame: ["#1D1D20", "#141415", "#000000"] // placeholder program frame gradient
    // Content colours: titles and captions are part of the picture, so they never follow the UI theme. Strings, as the
    // panels store and compare them as style values.
    readonly property string contentWhite: "#FFFFFF"
    readonly property string contentInk: "#101013"
    readonly property string captionHighlight: "#FFD13F"             // C.7 caption karaoke default (CaptionStyle::highlight)

    // ---- Clip label colours: Premiere's 16 labels (imported label indices keep their meaning) ----
    // Violet, Iris, Caribbean, Lavender, Cerulean, Forest, Rose, Mango, Purple, Blue, Teal, Magenta, Tan, Green,
    // Brown, Yellow. v3 clips are solid fills of their label colour with black names on them, so every label carries
    // clipInk at >= 4.5:1 (tested).
    readonly property var defaultClipColors: ["#9B6CFF", "#8C9BFF", "#3EE6B8", "#E79BF0", "#4DB2FF", "#7BD45A",
                                              "#FF6F96", "#FFB547", "#B97CFF", "#6E8FFF", "#3CCFD0", "#FF5FD2",
                                              "#DDB585", "#4DD98A", "#C9915F", "#F2DE4E"]
    // A custom theme's labels (0.1.3) when it sets all 16.
    readonly property var clipColors: Array.isArray(customClipColors) && customClipColors.length === defaultClipColors.length
                                      ? customClipColors : defaultClipColors
    function clipColor(index) { return clipColors[((index % clipColors.length) + clipColors.length) % clipColors.length] }
    // Solid clips: the body is the label colour, waveforms and names are clipInk over it. The tints remain for
    // label-coloured areas that are not clips (library cards).
    readonly property real clipTint: 0.12
    readonly property real clipTintAudio: 0.08
    readonly property real clipTintSelected: 0.2
    readonly property int clipStrip: 0             // v2's label strip: none on solid clips
    readonly property int clipHeaderHeight: 16     // the solid band that carries the name above a clip's thumbnails
    function clipBody(c) { return c }
    function audioClipFill(c) { return c }
    function audioClipBorder(c) { return Qt.alpha(clipInk, 0.18) }
    function waveform(c) { return dark ? Qt.lighter(c, 1.25) : Qt.darker(c, 1.45) }   // on dark wells (Source monitor)
    readonly property color clipWave: Qt.alpha(clipInk, 0.32)    // waveforms drawn on a solid clip
    readonly property color clipBorder: Qt.alpha(clipInk, 0.18)
    readonly property color accentWash: Qt.alpha(live.accentStroke, 0.14) // translucent accent area (ranges, viewport)
    readonly property color playhead: live.playhead

    // ---- Geometry ----
    readonly property int radiusPanel: 22      // the islands: docked panels, Home cards
    readonly property int radiusMenu: 20       // menus, popovers
    readonly property int radiusCard: 18       // cards inside a panel, dialogs' inner cards
    readonly property int radiusField: 14      // multi-line fields and wells (single-line fields are pills)
    readonly property int radiusControl: 16    // buttons and segmented controls: a pill at control height
    readonly property int radiusPill: 999      // fully round: chips, tags, icon buttons
    readonly property int radiusClip: 9        // timeline clips, minimap
    readonly property int radiusBadge: 6       // keycaps, tiny chips
    readonly property int radiusTab: 12        // the top corners of a panel tab
    readonly property int radiusMonitor: 12    // the picture well in a monitor
    readonly property int controlHeight: 32
    readonly property int controlHeightSmall: 26
    readonly property int controlHeightLarge: 40   // the title bar's buttons
    readonly property int gap: 8               // the gutter between islands
    readonly property int pad: 12              // panel padding
    readonly property int hairline: 1
    readonly property int focusRingWidth: 2
    readonly property int focusRingGap: 2      // space between a control and its focus ring
    readonly property int selectionWidth: 2    // selected clip outline
    readonly property real clipSelectionWidth: 1.5
    readonly property int dotSize: 8           // status, tab and section dots
    readonly property int iconXs: 14
    readonly property int iconSm: 16
    readonly property int iconMd: 18
    readonly property int iconXl: 28           // Home screen drop zone
    readonly property int rowHeight: 28        // dense lists
    function space(n) { return n * 4 }         // 4px grid helper
    // v2's slice (a 12° cut on a corner). v3 draws no slices: the depths are 0, so SliceRect is a plain rounded
    // rectangle; the angle stays for SliceShape's own users.
    readonly property real sliceAngle: 12
    readonly property int sliceDepth: 0
    readonly property int sliceDepthCard: 0
    // v2's mark before a section title (a 3×12 bar); v3's SliceMark is a round dot of dotSize.
    readonly property int sliceMarkWidth: 3
    readonly property int sliceMarkHeight: 12

    // Window + layout
    readonly property int windowMinWidth: 1100
    readonly property int windowMinHeight: 700
    readonly property int windowDefaultWidth: 1600
    readonly property int windowDefaultHeight: 960
    readonly property int windowResizeMargin: 6
    readonly property int compactBelowHeight: 820   // window heights below this use the compact density
    readonly property int titleBarHeight: compact ? 44 : 52   // holds the workspace switcher and the 40 px buttons
    readonly property int railWidth: 48
    // v1's floating dock is gone (the workspace tabs live in the title bar); kept for code that still reads them.
    readonly property int dockHeight: 0
    readonly property int dockMargin: gap
    readonly property int dockItemWidth: compact ? 44 : 104
    readonly property int panelHeaderTop: compact ? 6 : 8       // panel edge → section header (undocked panels)
    readonly property int panelHeaderGap: compact ? 6 : 8       // section header or tab strip → body
    readonly property int tabStripHeight: compact ? 32 : 36     // a docked panel's tab strip
    readonly property int tabHeight: compact ? 26 : 30          // a tab in it (rises from the strip's floor)
    readonly property int panelBottomPad: compact ? 8 : 12
    readonly property int libraryMinWidth: 248
    readonly property int libraryMaxWidth: 360
    readonly property int sideMinWidth: 300     // clip editor
    readonly property int sideMaxWidth: 400
    readonly property int sideWideMinWidth: 340 // audio mixer / export side panel
    readonly property int sideWideMaxWidth: 460
    readonly property int sideKeyframesMinWidth: 420 // Effect Controls with its keyframe lane (M5d)
    readonly property int sideKeyframesMaxWidth: 600
    readonly property int sectionHeaderHeight: 26
    readonly property int splitterHit: 8        // the draggable band around a gutter
    readonly property int workspaceTabHeight: compact ? 28 : 32

    // Controls
    readonly property int toggleWidth: 34
    readonly property int toggleHeight: 20
    readonly property int toggleThumb: 14
    readonly property int toggleThumbStretch: 4 // extra thumb width while pressed
    readonly property int swatchSize: 18
    readonly property int sliderTrack: 4
    readonly property int sliderHandle: 14
    readonly property int numberFieldWidth: 84  // includes the unit suffix
    readonly property int propLabelWidth: 76
    readonly property int propCompactWidth: 300 // PropSlider drops its icon below this width
    readonly property int mediaCardMinWidth: 132
    readonly property int progressHeight: 8     // pill progress bars

    // Timeline
    readonly property int trackHeaderWidth: 204   // source patching column + target chip + toggles (M5h)
    readonly property int trackHeightVideo: compact ? 32 : 40
    readonly property int trackHeightAudio: compact ? 36 : 44
    readonly property int trackGap: 2
    readonly property int rulerHeight: 26
    readonly property int minimapHeight: 18
    readonly property real playheadWidth: 2      // the playhead line
    readonly property int playheadGlow: 6        // its glow while playing
    readonly property int playheadHead: 16      // the round knob on the ruler
    readonly property int rulerLabelSpacing: 110 // minimum px between labelled ruler ticks
    readonly property int rulerMinorSpacing: 8   // minor ticks are drawn only when at least this far apart
    readonly property int timelineMaxPixelsPerFrame: 24 // deepest timeline zoom
    readonly property real markerLineOpacity: 0.35 // marker guide lines across the lanes
    readonly property int snapTickSize: 6        // the diagonal tick flashed on a snap target
    // Keyboard Shortcuts editor (M5f): keys bound to application commands and to panel commands (Premiere's
    // purple/green split), fixed so they never clash with the chosen accent; labels on them use clipInk.
    readonly property color keyApplication: "#B97CFF"
    readonly property color keyPanel: "#4DD98A"
    readonly property int keyUnit: 38          // one key (a letter) on the shortcuts keyboard, at full size
    // Audio Track Mixer (M5g)
    readonly property int mixerStripWidth: 96     // one track's channel strip
    readonly property int mixerMasterWidth: 272   // the master strip: loudness beside its fader and meter
    readonly property int meterBarWidth: 14       // a stereo level meter (two bars)
    readonly property int sideMixerMinWidth: 460  // the mixer side slot in the Audio workspace (at least)
    // Source Monitor and three-point editing (M5h)
    readonly property int dualMonitorMinWidth: 580  // monitor area that fits Source and Program side by side (else one, toggled)
    readonly property int sourcePatchWidth: 24      // the track headers' source patching column (Premiere's left column)
    readonly property real actionSafeInset: 0.05    // safe margins, per side: action safe (90 % of the frame)
    readonly property real titleSafeInset: 0.1      // and title safe (80 %)
    readonly property var monitorZoomLevels: [0.25, 0.5, 0.75, 1, 1.5, 2, 4]   // Program Monitor zoom besides Fit
    readonly property int sourceRangeBarHeight: 4   // the Source Monitor's In/Out range bar
    readonly property int sourceMiniTimelineHeight: 16
    readonly property int dragHandleSize: 26        // drag-video-only / drag-audio-only handles under the picture
    readonly property real minimapClipOpacity: 0.8
    // Pro-grade timeline (M5i)
    // Premiere's eight marker colours (colorIndex 0-7: green, red, purple, orange, yellow, white, blue, cyan), tuned
    // to read on the lanes; "white" is a cool grey in light mode.
    readonly property var markerColors: ["#4DD98A", "#FF5F6D", "#B97CFF", "#FF7A2F", "#F2DE4E",
                                         dark ? "#E8E8EC" : "#6B6B74", "#4D8DFF", "#3CCFD0"]
    function markerColor(index) { return markerColors[((index % 8) + 8) % 8] }
    // Duplicate frame bands (one colour per reused source; Premiere cycles through ten).
    readonly property var duplicateColors: ["#FF6F96", "#3CCFD0", "#FFB547", "#4DD98A", "#E79BF0",
                                            "#4D8DFF", "#F2DE4E", "#B97CFF", "#FF7A2F", "#7BD45A"]
    readonly property int duplicateBandHeight: 3
    readonly property color trimRoll: danger       // Premiere: roll and regular trims red,
    readonly property color trimRipple: warning    // ripple trims yellow
    // 0.2: the render bar above the ruler (Premiere's): red must be rendered to play in real time, yellow may play in
    // real time, green has a preview file.
    readonly property color renderRed: danger
    readonly property color renderYellow: warning
    readonly property color renderGreen: success
    readonly property int renderBarHeight: 3
    readonly property int trackHeightMin: 24
    readonly property int trackHeightMax: 240
    readonly property int trackHeightStep: 12
    // M5l: nested and multi-camera clips carry a second stripe under their label strip (the label keeps its colour); a
    // multi-camera clip's angle cuts are marked in its colour. The track fader / pan lanes drawn on audio tracks.
    readonly property color nestedStripe: accentStroke
    readonly property color multicamStripe: dark ? "#3EE6B8" : "#0B7A5C"   // mint
    readonly property int nestedStripeHeight: 2
    readonly property color trackVolumeLane: accentStroke
    readonly property color trackPanLane: textSecondary
    readonly property int trackResizeZone: 5       // the drag zone along a track header's bottom edge
    readonly property int sequenceTabHeight: 28
    readonly property int sequenceTabMaxWidth: 200
    readonly property real filmstripPlaceholderAlpha: 0.07 // a thumbnail tile still loading
    readonly property real filmstripScrimAlpha: 0.5        // under a clip name drawn over its thumbnails
    readonly property int markerFlagWidth: 9       // the ruler's marker flag
    readonly property int trimViewOffsetWidth: 56  // the Trim view's offset readouts
    // Decode height of library thumbnails (one size for every card, so the disk cache is shared).
    readonly property int mediaThumbnailHeight: 180
    // Timeline editing (M5b): snapping reach and the edge zone that starts a trim, in px.
    // px (0.1.3: 12/8, the timeline snaps and finds edges more readily).
    readonly property int snapThreshold: 12
    readonly property int trimHandleWidth: 8
    // Dragging near the left/right (or top/bottom) edge of the lanes scrolls them (M5c).
    readonly property int autoScrollZone: 48      // px next to each edge
    readonly property int autoScrollSpeed: 1600   // px/s at the edge
    function trackHeight(kind) { return kind === "audio" ? trackHeightAudio : trackHeightVideo }

    // ---- Typography: Manrope (UI and display), JetBrains Mono (numbers, timecode), IBM Plex Sans Thai for Thai in
    // both. Bundled (SIL OFL 1.1) and registered by BundledFonts, with system fallbacks. ----
    readonly property string fontSans: BundledFonts.sans
    readonly property string fontMono: BundledFonts.mono
    readonly property string fontDisplay: BundledFonts.display
    readonly property int weightBody: Font.Medium      // body text and inactive tabs
    readonly property int weightLabel: Font.Bold       // buttons, tabs, row names
    readonly property int weightTitle: Font.ExtraBold  // titles, primary buttons, the wordmark
    readonly property int textMicro: 10  // UPPERCASE micro-tags only (PROXY, AI)
    readonly property int textXs: 11     // meta
    readonly property int textSm: 12     // labels
    readonly property int textMd: 13     // controls, body
    readonly property int textLg: 14     // panel titles, row labels
    readonly property int textXl: 14     // card titles
    readonly property int textTitle: 16  // dialog titles
    readonly property int textDisplay: 22  // dialog heroes
    readonly property int textHero: 30     // the Home greeting
    readonly property real labelTracking: 0      // sentence case everywhere: no tracked caps
    readonly property real microTracking: 0.6    // micro-tags

    // ---- Opacity ----
    readonly property real disabledOpacity: 0.4

    // ---- Motion: snap-out (fast start, soft stop) at 140 ms for state changes and 220 ms for layout; springs only
    // on drag release and flings. Nothing loops at rest; everything honours reduce motion. ----
    readonly property int durPress: dur(90)
    readonly property int durFast: dur(60)       // hover: colour and rim light only
    readonly property int durState: dur(140)
    readonly property int durCollapse: dur(220)
    readonly property int durBounce: dur(220)    // toggles/checks/drops
    readonly property int durSpring: dur(500)
    readonly property int durRipple: dur(180)    // clips shifted by an edit slide into place
    readonly property int durSpark: dur(180)     // the cut spark
    readonly property int durSnapTick: dur(90)   // the snap tick
    // The transition between workspaces: a 220 ms cross-fade (120 ms with reduce motion).
    readonly property int durSlice: reduceMotion ? 120 : 220
    readonly property int tooltipDelay: 600      // not motion: unaffected by reduceMotion
    readonly property real pressScale: reduceMotion ? 1.0 : 0.98
    readonly property real liftScale: reduceMotion ? 1.0 : 1.02   // dragged clips/cards
    readonly property real grabScale: reduceMotion ? 1.0 : 1.15   // small grabbed handles (slider knob)
    readonly property real popScale: reduceMotion ? 1.0 : 0.97    // menus/popovers/tooltips grow from this on open
    readonly property int liftOffset: 0                           // no hover lift in v2
    readonly property var easeSnapOut: [0.16, 1, 0.3, 1, 1, 1]    // OutExpo-like; use with Easing.BezierSpline
    readonly property var easeOutSoft: easeSnapOut                 // v1 name, same curve
    readonly property var easeSpring:  [0.34, 1.56, 0.64, 1, 1, 1]  // visible bounce: drops only
    // Physical spring for sliding indicators and direct manipulation (SpringAnimation). Close to critical damping:
    // it settles in ~250 ms with no visible overshoot. Disable the Behavior when reduceMotion.
    readonly property real springStiffness: 4.0
    readonly property real springDamping: 0.38
    readonly property real springMass: 1.0
    readonly property real springEpsilon: 0.25
    // The echo mark's launch intro (once per launch, skipped with reduce motion).
    readonly property int durIntro: 2100
    readonly property int durIntroFade: 250
    readonly property var easeIntro: [0.65, 0, 0.35, 1, 1, 1]
    function dur(ms) { return reduceMotion ? 0 : ms }
    /// A surface or text token: dark / light, or their high-contrast versions (M5j).
    function pick(darkValue, lightValue, contrastDark, contrastLight) {
        return highContrast ? (dark ? contrastDark : contrastLight) : (dark ? darkValue : lightValue)
    }

    // ---- Animated palette ----
    // Every themable colour follows its target (dark/light × accent) through a ColorAnimation, so switching
    // theme or accent cross-fades instead of cutting. The public tokens above are read-only views of these.
    readonly property QtObject _live: QtObject {
        id: live

        property color accentFill: theme.custom("accentFill", theme.accent.fill)
        property color accentOnFill: theme.custom("accentOnFill", theme.accent.onFill)
        property color accentStroke: theme.custom("accentStroke", theme.dark ? theme.accent.strokeDark : theme.accent.strokeLight)
        property color accentSoft: theme.custom("accentSoft", theme.accent.soft)
        property color accentDeep: theme.custom("accentDeep", theme.accent.deep)
        property color beamMid: theme.custom("beamMid", theme.accent.beamMid)
        property color beamEnd: theme.custom("beamEnd", theme.accent.beamEnd)

        // v3: a true black stage under near-black neutral islands (dark), white islands on a light grey stage (light).
        property color canvas:  theme.custom("canvas", theme.pick("#000000", "#EDEDF0", "#000000", "#FFFFFF"))
        property color sunken:  theme.custom("sunken", theme.pick("#0B0B0C", "#E2E2E7", "#000000", "#F0F0F0"))
        property color panel:   theme.custom("panel", theme.pick("#141415", "#FFFFFF", "#0A0A0A", "#FFFFFF"))
        property color section: theme.custom("section", theme.pick("#1B1B1D", "#F6F6F8", "#121212", "#F5F5F5"))
        property color raised:  theme.custom("raised", theme.pick("#1D1D20", "#F0F0F3", "#161616", "#FFFFFF"))
        property color hover:   theme.custom("hover", theme.pick("#26262A", "#E6E6EB", "#2E2E2E", "#E0E0E0"))
        property color well:    theme.custom("well", theme.pick("#34343A", "#D9D9DF", "#3A3A3A", "#C8C8C8"))
        property color border:  theme.custom("border", theme.pick(Qt.rgba(1, 1, 1, 0.07), Qt.rgba(0, 0, 0, 0.08), Qt.rgba(1, 1, 1, 0.5), Qt.rgba(0, 0, 0, 0.55)))
        property color borderStrong: theme.custom("borderStrong", theme.pick(Qt.rgba(1, 1, 1, 0.16), Qt.rgba(0, 0, 0, 0.16), Qt.rgba(1, 1, 1, 0.85), Qt.rgba(0, 0, 0, 0.85)))
        property color rimLight: theme.custom("rimLight", theme.pick(Qt.rgba(1, 1, 1, 0), Qt.rgba(1, 1, 1, 0), Qt.rgba(1, 1, 1, 0.35), Qt.rgba(0, 0, 0, 0.35)))
        property color shadow:  theme.dark ? Qt.rgba(0, 0, 0, 0.6) : Qt.rgba(0, 0, 0, 0.14)

        property color textPrimary:   theme.custom("textPrimary", theme.pick("#F2F2F3", "#0B0B0C", "#FFFFFF", "#000000"))
        property color textSecondary: theme.custom("textSecondary", theme.pick("#B4B4BA", "#4A4A52", "#ECEAF2", "#14111C"))
        property color textTertiary:  theme.custom("textTertiary", theme.pick("#8C8C94", "#6B6B74", "#D2CEDC", "#2E2A38"))
        property color textDisabled:  theme.custom("textDisabled", theme.pick("#55555C", "#B4B4BA", "#9A95A6", "#6E6880"))

        property color playhead: theme.custom("playhead", theme.pick("#FFFFFF", "#0B0B0C", "#FFFFFF", "#000000"))

        Behavior on accentFill { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on accentOnFill { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on accentStroke { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on accentSoft { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on accentDeep { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on beamMid { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on beamEnd { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on canvas { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on sunken { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on panel { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on section { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on well { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }

        Behavior on raised { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on hover { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on border { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on borderStrong { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on rimLight { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on shadow { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on textPrimary { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on textSecondary { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on textTertiary { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on textDisabled { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
        Behavior on playhead { enabled: theme.colorTransitions; ColorAnimation { duration: theme.durState; easing.type: Easing.OutCubic } }
    }
}
