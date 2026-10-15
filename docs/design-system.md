# Wabcut design system

Design language v3, **"Echo"** (since 2026-10-15). The binding direction is [design-language.md](design-language.md): a
black stage with rounded islands, tab strips, pills, violet with lime, and the echo mark. This file lists the tokens,
components and layout rules that implement it. Tokens live in `src/app/qml/theme/Theme.qml`; components read them from
there and never hard-code values (`test_qml_guards` fails on a colour literal anywhere else in QML). Where this file and
design-language.md disagree, design-language.md wins. The feature records further down describe how each milestone
built its screens; their v2 visual details (slices, the beam, label strips) are superseded by v3.

## Identity (what makes a screenshot Wabcut)
1. **Islands on black:** panels are radius-22 islands on a `#000000` stage, 8 px apart.
2. **Tab strips:** a docked panel opens with a dark strip; its tab rises from it with flared feet and a hue dot.
3. **Pills:** buttons, chips, segmented groups, fields, transport and track headers; icon buttons are circles; black
   text on the accent.
4. **Violet and lime:** the accent and brand `#9B6CFF`, the brand's second colour `#D6F54C`, four supporting hues.
5. **Solid clips and the echo mark:** solid label-coloured clips with black names; the five-clip echo mark.

## Colour
### Surfaces
| Token | Dark | Light | Use |
|---|---|---|---|
| `canvas` | `#000000` | `#EDEDF0` | the stage (window background; shows between islands) |
| `sunken` | `#0B0B0C` | `#E2E2E7` | tab strips, the timeline well, segmented tracks |
| `panel` | `#141415` | `#FFFFFF` | the islands |
| `section` | `#1B1B1D` | `#F6F6F8` | cards inside a panel |
| `raised` | `#1D1D20` | `#F0F0F3` | buttons, fields, chips, menus, popovers |
| `hover` | `#26262A` | `#E6E6EB` | hover, the selected row or nav item |
| `well` | `#34343A` | `#D9D9DF` | stepper buttons, slider tracks, scroll thumbs |
| `border` / `borderStrong` | white 7% / 16% | black 8% / 16% | hairlines / separators that must read |
| `rimLight` | transparent | transparent | v2's top highlight, kept as a token for theme files |
| `textPrimary` / `textSecondary` / `textTertiary` / `textDisabled` | `#F2F2F3` / `#B4B4BA` / `#8C8C94` / `#55555C` | `#0B0B0C` / `#4A4A52` / `#6B6B74` / `#B4B4BA` | text |
| `focus` | text primary | text primary | the focus ring |
| `knob` | `#FFFFFF` | `#FFFFFF` | slider knobs, toggle thumbs |

The monitor well (`monitorSurround`) and `clipInk` are `#000000` in both themes. `section` and `well` are new in v3;
custom themes (`colorTokenKeys`, `ThemeFiles`) and the Graphite / Midnight / Paper presets set them too.

### Accent, brand and hues
| Accent (key) | `fill` = `strokeDark` | `strokeLight` | `soft` | `deep` (pressed) |
|---|---|---|---|---|
| Violet (`ultraviolet`) | `#9B6CFF` | `#6A3DE8` | `#C3A8FF` | `#8A5CFF` |
| Pink (`infrared`) | `#FF5FA8` | `#C0256E` | `#FFC2DD` | `#E84A93` |
| Lime (`phosphor`) | `#D6F54C` | `#5E7A00` | `#EEFBB0` | `#BEDC34` |
| Orange (`sodium`) | `#FF7A2F` | `#B54700` | `#FFCAA8` | `#EA661B` |
| Blue (`ice`) | `#4D8DFF` | `#1F5FD6` | `#B8D2FF` | `#3A78EA` |
`onFill` is black for all five (6–17:1, tested); the strokes keep 3:1 on panel and raised in both themes (tested).
`beamMid` / `beamEnd` equal the fill (no gradients in v3). The keys are v2's, so saved settings carry over;
`Theme.accentLabels` gives the names Settings shows, and colour names (violet, pink, lime, orange, blue, cyan) resolve.
`brand` `#9B6CFF` and `brandLime` `#D6F54C` are the echo mark's inks, independent of the accent.
Hues (`Theme.hue(name)`: violet, lime, pink, mint `#3EE6B8`, blue, orange) tag panels (`Workspaces.panelHue`), Home's
nav and project cards. Semantic: success mint (dark) / `#0B7A5C` (light), warning `#FBBF24`, danger `#F43F5E`, info blue.

### Clips and labels
- Label colours are Premiere's 16 labels (`Theme.clipColors`): Violet, Iris, Caribbean, Lavender, Cerulean, Forest,
  Rose, Mango, Purple, Blue, Teal, Magenta, Tan, Green, Brown and Yellow, retuned so black names read at ≥ 4.5:1.
- **A clip** is a solid fill of its label colour (`clipBody(c)` and `audioClipFill(c)` return it), radius 9, its name in
  `clipInk`. Audio waveforms are `clipWave` (black, 32%).
- **With thumbnails:** a `clipHeaderHeight` (16 px) band of the label colour carries the name above the filmstrip.
- **Selected:** a 2 px `textPrimary` ring outside a 2 px gap. **Disabled or offline:** hatched.
- **Transitions:** white with a black diagonal. Nested and multicam stripes and volume lines are `clipInk`.

## Geometry
- **Radii:** `radiusPanel` 22 (islands, Home cards), `radiusMenu` 20, `radiusCard` 18 (cards inside panels), `radiusField`
  14 (multi-line fields, wells), `radiusControl` 16 (a pill at control height; fully round components use height / 2 or
  `radiusPill`), `radiusTab` 12, `radiusMonitor` 12, `radiusClip` 9, `radiusBadge` 6.
- **Sizes:** grid 4 px; `gap` 8; `pad` 12; `controlHeight` 32 (compact 26), `controlHeightLarge` 40 (the title bar's
  buttons); `tabStripHeight` 36 (compact 32), `tabHeight` 30 (26); title bar 52 (compact 44); dots 8; toggles 34×20.
- **The slice:** `sliceDepth` and `sliceDepthCard` are 0; `SliceShape` (C++) still draws the v2 outline for code that
  asks for a depth, and `test_design_language` keeps its geometry test.

## Type
- `fontSans` and `fontDisplay`: **Manrope** (Regular, Medium, SemiBold, Bold, ExtraBold); `fontMono`: **JetBrains Mono**
  (Regular, Medium, SemiBold, Bold). Weight tokens: `weightBody` (Medium), `weightLabel` (Bold), `weightTitle`
  (ExtraBold).
- **Thai:** `BundledFonts` substitutes IBM Plex Sans Thai (Regular to Bold) into Manrope, JetBrains Mono, Plex Sans and
  Plex Mono, before any system font. A test checks that Thai vowels and tone marks stack (`thai-type.png`).
- IBM Plex Sans/Mono and Bai Jamjuree stay bundled for picture content (titles, caption styles, templates).
- The files ship in `src/app/resources/fonts/` (SIL OFL 1.1; pinned sources, SHA-256 and licence texts in
  `THIRD-PARTY-NOTICES.md`).
- **Scale:** 11 meta · 12 labels · 13 controls and body · 14 panel titles · 16 dialog titles · 22 heroes · 30 Home's
  greeting (`textXs` … `textHero`). Sentence case; caps only for `textMicro` labels with `Font.AllUppercase`.

## Panels, tabs and section headers
- **`Panel`:** an island. Docked (the outermost panel of a `DockSlot`), it opens with a **tab strip**: a `sunken` band
  with the island's top corners, the panel's `PanelTab` (named and coloured from the catalog: `Workspaces.panelLabel` /
  `panelHue`), then the `leading` tools, and at the right the `trailing` tools and `DockHandle` (grip, pop-out, close).
  Each tool row hides when the strip has no room. Undocked with a `title`: a `SectionHeader`. A docked panel without a
  title (a monitor in single mode) keeps the corner handle.
- **`DockTabs`:** a group's browser-style tabs, one instance per `DockSlot` (`tabPanels`, `tabbed`, `tabsWidth`), laid
  over the shown panel's strip; the panel hides its own tab and leaves them `tabsWidth`. Click or Left/Right shows a
  panel, × or a middle click closes it, dragging a tab moves that panel. `DockManager.headerZone` is the strip.
- **`SectionHeader`:** a dot (`SliceMark` is now an 8 px dot) and a bold title.
- **`PillTabs`:** a segmented pill track with a sliding accent pill (black text); `tabs: true` uses a hover-coloured pill.

## Chrome
- **Title bar (`TopBar`):** the echo mark in a round badge plus the project name and `W×H · fps` (the File menu target,
  `brandArea`), the sequence switcher, `WorkspaceTabs` (a segmented pill group, centred; `workspacePlate` slides under
  the active one), job pills while something runs, round command palette / undo / redo, Save (unsaved dot) and Export,
  round window controls.
- **Workspaces, docking, the left rail, Settings:** behaviour as in 0.1.3 (see the feature records); the rail is an
  island of round icon buttons, the active one on `hover`.
- **Appearance:** built-in themes (the default dark and light looks, Graphite, Midnight, Paper), saved themes, import /
  export (`ThemeFiles`), mode, accent, high contrast, reduce motion, interface size, the launch intro, and a colour
  picker for every colour token and the 16 clip labels.
- **Toasts:** bottom-right `section` cards with a soft shadow.

## Brand, logo and motion
- **Mark:** `resources/brand/wabcut-mark.svg` (and `-small`, `-mono`); `components/EchoMark.qml` draws the same geometry
  with rectangles (crisp, themable, animatable). `BrandLogo`: EchoMark + "Wabcut" in Manrope ExtraBold (+ version). In
  light and high-contrast themes the mark is one ink.
- **App icon set:** `resources/brand/icons/` (PNG 16–1024, `wabcut.ico` in the exe through `src/app/wabcut.rc`,
  `wabcut.icns`), rendered by `tests/app/test_brand_icons.cpp` from `wabcut-app-icon-small.svg` (16–24 px),
  `-medium.svg` (32–48) and `wabcut-app-icon.svg` (64+); run `"[.icongen]"` to regenerate. The MSI art comes from
  `cmake/packaging/wix/Make-InstallerArt.ps1`.
- **`EchoLoader`** (equalizer / chase / scan / build) and **`LoaderPill`** (a loader + label in a raised pill);
  `BeamProgress` is a solid pill bar and shows a LoaderPill when indeterminate (its `loader` picks the variant).
- **`LaunchIntro`:** the 2.1 s assemble intro, once per launch (`Preferences.launchIntro`), skipped with reduce motion.
- **Motion tokens:** `durState` 140, `durCollapse` 220, `durFast` 60, `durPress` 90, `durIntro` 2100, `durIntroFade`
  250, `easeSnapOut`, `easeIntro`. Reduce motion zeroes durations, stops the loaders and the intro, the playhead glow and
  the cut flash, and turns the workspace transition into a 120 ms fade.

## Component inventory (src/app/qml/components)
The APIs are stable; v1/v2 names are kept so other panels do not break.
| Component | v3 notes |
|---|---|
| `Panel` | Island, radius 22; docked → tab strip (`PanelTab` or the group's `DockTabs`), `leading` / `trailing` tools, `DockHandle`. |
| `PanelTab` | A strip tab: rounded top, flared feet when active, hue dot, bold name. |
| `DockTabs`, `DockSlot`, `DockHolder`, `DockHandle` | The group's tabs (one per slot), the slot, what floats, the grip / pop-out / close. |
| `SectionHeader` / `SliceMark` | Dot + bold title / an 8 px dot. |
| `PillButton` | Pill. `primary` accent + black ExtraBold, `secondary` raised, `ghost` transparent with a hairline. `beamAccent` is a no-op. |
| `IconButton` | Circle; `ghost` / `secondary` / `primary`; checked = accent with a black glyph. |
| `PillTabs` | Segmented pill track, sliding accent pill. |
| `Toggle`, `ValueSlider`, `PropSlider`, `Swatch` | Pill track with a white round thumb or knob; swatches are circles. |
| `FieldInput`, `NumberField` | Raised pills with a focus edge. |
| `ContextMenu` / `ContextMenuItem` / `Popover` / `Tooltip` | Radius 20 with a soft shadow, pill highlights; tooltips are inverted pills with an accent keycap. |
| `StatusPill`, `Toast`, `ThinScrollBar`, `FocusRing`, `EmptyState`, `Icon`, `KeyframeShape` | Raised pill / section card / `well` thumb / `Theme.focus` / illustration + action. |
| `EchoMark`, `EchoLoader`, `LoaderPill`, `BrandLogo`, `BeamProgress` | Brand mark, loaders, logo, pill progress. |
| C++ items | `SliceShape`, `SvgArt` (tinted SVG), `BundledFonts` (singleton: type families). |

**Rules for new panels:** use `Panel` and the components above with Theme tokens only (no colour literals except label
colours); black text on accent and hue fills; `BeamProgress` / `EchoLoader` for waits.

## Accessibility (unchanged, binding)
- WCAG AA: text at least 4.5:1, strokes and focus at least 3:1; the accent and text tokens are tested in both themes
  (`test-output/ui/contrast.txt`), and every clip label carries black names at 4.5:1.
- A visible focus ring (2 px `focus` + 2 px gap) on every Tab stop, keyboard reachability and screen-reader names.
- Reduce motion is honoured; colour never carries meaning by itself.

## Window chrome
- Frameless by default (`Qt.FramelessWindowHint | Qt.WindowMinMaxButtonsHint`); `Preferences.nativeFrame` switches to
  the platform's decorations live.
- The title bar's empty areas drag the window (`startSystemMove()`), and a double-click toggles maximize. `WindowFrame`
  places resize edges.
- Minimum size 1100×700. Below a height of 820 px (`compactBelowHeight`) the shell turns compact: title bar 44, controls
  26, tab strips 32, tighter track heights; the reclaimed height goes to the timeline.

## Screenshot harness (tests/app)
`ctest` renders `Main.qml` without ever showing a window: it is created with `visible: false`, and
`QQuickWindow::grabWindow()` renders it offscreen on the platform's real QRhi backend (D3D11 on Windows). Screenshots go
to `build/<preset>/test-output/ui/`:
- `dark-<accent>.png` and `light-<accent>.png` for all five accents;
- `workspace-audio`, `workspace-export`, `workspace-color`, `workspace-captions`;
- `compact-1100x700`, `popover-settings`, `popover-appearance`, `menu-sequences`;
- `dark-ultraviolet@125` and `@150`;
- `thai-type.png`.
The launch intro never plays in these runs: it starts only when the window is first shown.

## Feature records (M5a–M5h)

The sections below record what each milestone built. Their behaviour still holds; where they describe the v1 look
(floating panels, the bottom dock, pills, solid clip blocks, the playhead triangle) the v2 rules above replace it.

## M5a: the real timeline

### Ownership
- **`ProjectController`** (C++ QML singleton, `src/app/src/bridge/`) owns the `EditSession`. It builds the demo project
  through the model's edit ops (the M1 mock content: Sequence 01 at 23.976 fps, the same clips, names, label colours
  and markers, plus two more sequences; the playhead opens at 00:00:12:08) and loads `.wabcut` files with
  `loadProject(url)`. QML sees only display values: timecode strings, frame-rate and format text, opaque ids
  (`"c42"`, `"t7"`, `"s3"`) and clip properties in UI units (scale %, position in sequence pixels, opacity %,
  pan ±100). It never exposes raw ticks or numeric ids (principle 9, checked by a metaobject scan in `tests/app`).
- Selection and the playhead are UI state in the controller, not document state. Undo/redo history, labels and
  `canUndo`/`canRedo` come from the session; Ctrl/⌘+Z and Shift+Ctrl/⌘+Z (and Ctrl+Y) go through it, as do the new
  undo/redo buttons in the top bar. Continuous property gestures (slider drags, number scrubs) merge into one undo
  step per gesture (`clip-property:<id>:<key>` merge key, ended by `PropSlider.gestureEnded`).
- `changed(changeSet)` is emitted after every session change with `{kind, label, sequence, tracks, structural}`,
  filled from the model's ChangeSet since M5a.1 (see below).
- `mock/MockProject.qml` keeps only the media library and playback state (`playing`, `shuttleSpeed`, fps, decoder).

### TimelineView (lanes)
- One C++ `QQuickItem` draws all lanes, clips, waveforms, marker lines, selection/hover and the playhead with a few
  batched scene-graph nodes: one vertex-coloured mesh per track, plus lanes, markers, overlay, playhead and marquee
  meshes, plus a text node per labelled visible clip. **There are no per-clip QML delegates** (tested).
- Visual parity with M1: rounded clips with an anti-aliased 1 px border, label in `clipInk` (video) or `textPrimary`
  (audio), audio bodies in `audioClipFill`/`audioClipBorder` with a `waveform()` envelope, selection = accent stroke +
  glow, hover = `liftOffset` lift with shadow over `durFast`. Marker lines run through the lanes at
  `markerLineOpacity`.
- Every visual token comes from Theme through the `style` map in `TimelineLanes.qml`; the C++ has no colours or sizes of
  its own. New Theme tokens: `timelineMaxPixelsPerFrame` (24, the deepest zoom), `markerLineOpacity` (0.35),
  `minimapClipOpacity` (0.8).
- Zoom is a 0–1 `zoomLevel` mapped exponentially from fit-all to `timelineMaxPixelsPerFrame`. The zoom slider, Ctrl/⌘/
  Alt+wheel (zooms at the cursor), wheel (horizontal) and Shift+wheel (vertical) are handled in `TimelinePanel.qml`.
- The ruler, track headers and minimap are QML/C++ items that read the same view and controller: the ruler places
  ticks from `rulerFirstX`/`rulerStepPx` and asks `rulerLabel()` for timecode text; the minimap (`TimelineOverview`)
  draws all clips as merged rect runs; track headers are a Repeater over `ProjectController.tracks`.
- Interaction: click selects (Shift/Ctrl/⌘ toggles; links follow unless Alt), dragging on empty space draws a marquee,
  ruler press/drag scrubs the playhead. Hit-testing returns opaque strings: `clipAt(x, y)`, `trackAt(y)`, `timeAt(x)`.

### Performance budget (tests/app, offscreen, D3D11)
- 10,000-clip sequence: first scene graph < 50 ms; scroll and zoom frames < 4 ms in `updatePaintNode`; node count =
  fixed nodes + tracks + label nodes, bounded by the visible clips. Results are written to
  `build/<preset>/test-output/ui/timeline-perf.txt`.
- Text layout dominates a frame, so new labels are laid out at most 8 per frame (the rest follow on the next frames),
  name widths are cached and elision is quantised to 8 px steps.

### Seams left open
- **M5b (edit gestures):** `TimelineView` mouse handlers are where drag, trim (edge hit zones), razor and slip/slide
  start; they will call new `ProjectController` edit methods with merge keys for live drags. Tools other than
  selection are still state-only.
- **Media:** clip-body thumbnails; `MockProject` playback state moves to the playback controller. (Waveform
  peaks and library thumbnails landed in M5a.1.)
- *(Done in M5a.1: the model's ChangeSet replaced the pointer diff; KeymapDispatcher replaced the local key
  handling.)*

## M5a.1: model rev 2, keyboard and media

### ChangeSet-driven updates
- Every edit, undo, redo and cancelled gesture carries the model's `ChangeSet`. `ProjectController::lastChange()` gives
  C++ views the changed tracks of the current sequence with the time ranges to redraw; `changed()` keeps its QML keys
  and adds `markers` and `marks` flags.
- TimelineView rebuilds a track only when one of its changed ranges reaches into the built window. A track changed
  only elsewhere keeps its geometry and the revision it was built from, which is identical inside the window
  (`TimelineStats::skippedTracks`). Marker-only edits redraw the marker lines and nothing else.
- The clip editor reads the parameter table (`ClipProperties::value` at the playhead's clip-local time, refreshed on
  playhead moves when a parameter is animated) and writes constants clamped to each parameter's range.

### Keyboard (KeymapDispatcher)
- `Main.qml` sets `KeymapDispatcher.window` and routes `commandTriggered` in this order:
  1. the timeline view (`TimelinePanel.runCommand`): zoom `=` `-` `\`, tools `V`/`C`, snap `S`;
  2. the mock playback state: Space;
  3. `ProjectController.runCommand`: undo/redo, Select All/Deselect All, frame steps (±1, ±5), edit points
     (Up/Down on the targeted tracks, Shift+Up/Down on any track), Home/End, Shift+Home/End, sequence In/Out marks
     (I, O, X, /, Clear In/Out, Go to In/Out), New Sequence.

  Navigation keeps the playhead in view.
- `transportIntent` (J/K/L, including K+J/L slow play at ¼×) drives the mock playback state until PlaybackController is
  bridged.
- Panel scopes: TimelinePanel `timeline`, MonitorPanel `programMonitor`, LibraryPanel `project`, InspectorPanel
  `effectControls`, AudioMixerPanel `audioMixer`, EffectsPanel `effects`. The local key handlers and the undo/redo
  `Shortcut`s are gone. Dock and PillTabs claim their arrow keys with `Keys.onShortcutOverride`.
- Tooltips: IconButton, PillButton and ContextMenuItem take a keymap `command` id and show
  `KeymapDispatcher.shortcutText(command)`, refreshed by `KeymapDispatcher.revision`. MockKeymap is removed.
- The ruler shows the sequence In/Out range as an accent bar along its bottom edge: `accentWash` fill with an
  `accentStroke` hairline, height `space(1)`, open-ended when only one mark is set.

### Media
- Waveforms: `MediaPeakSource` (MediaBridge) serves real peaks from `media::WaveformPeaksService` for media whose
  `MediaLocator` resolves to a file. Peaks are computed on a worker thread and cached under
  `<CacheLocation>/media/peaks`. Demo media never resolve, so they keep the placeholder envelope.
- Library: once any project media file exists, the library lists `ProjectController.mediaItems`, with the middle frame
  from `media::ThumbnailService` (`image://wabcut-thumbnails/<path>`, decoded at `Theme.mediaThumbnailHeight` 180 px,
  masked to `radiusField`). The demo project keeps the mock library and its gradients.

### Renderer robustness
- RendererInfo reads the backend and adapter on the scene graph's thread and queues only the copied strings to the
  GUI. It is safe against teardown.
- The Vulkan probe requires a usable device: an instance plus a physical device with a graphics queue.
- A scene graph that fails to initialize falls back instead of terminating: next RHI backend → reload the window →
  finally a restart on the software renderer.

## M5b: interactive editing

### Principles
- The view never edits. A drag becomes a `GestureSpec` (TimelineEditing.h). Every mouse move runs the matching
  EditOps edit on the current snapshot (`ProjectController::previewEdit`) and the view displays the result, the
  **ghost preview**. The model is untouched until release, when `commitGesture` applies the same edit inside an
  EditSession gesture: **one drag = one undo step**, and the drop never jumps because it is exactly the preview.
  Esc cancels and nothing is committed.
- Locked tracks: the model refuses the edit, so no preview appears and nothing is committed. A move that would land
  on a locked track keeps its last valid preview.

### Tools (keys from docs/keymap.md; buttons in the Tracks header with keymap tooltips)
| Tool | Key | Press / drag |
|---|---|---|
| Selection | V | Body drag moves the selection. Ctrl/⌘ inserts instead of overwriting; Alt ignores links. Dragging across tracks moves video up and linked audio down. Edge drag trims; Ctrl/⌘ on an edge ripple-trims. |
| Track select forward / backward | A / Shift+A | Click selects every clip from the click on (or before it), on all tracks; Shift: the clicked track only. A drag then moves them. |
| Ripple edit | B | Edge drag ripple-trims. |
| Rolling edit | N | Drag an edit point to roll it. |
| Slip | Y | Body drag slips the source (left shows later frames). The readout shows the source In/Out. |
| Slide | U | Body drag slides the clip; its neighbours give and take. |
| Razor | C | Click cuts the clip under the cursor and its linked partner (Alt: just that clip); Shift+click cuts every track. A preview line follows the pointer. |

- **Edge zones:** `Theme.trimHandleWidth` (8 px since 0.1.3; was 6) on each side of a clip border, at most a third of
  the clip's width. The edge a press would edit lights up as a bracket while the pointer is over it.
- **Cursors (0.1.3, `EditCursors`):** every tool, zone and modifier has its own cursor, drawn from Boxicons glyphs
  (`resources/cursors/`) with a white outline and a soft shadow at the screen's pixel ratio: trim brackets (red, like
  Premiere's trim points) and ripple brackets (yellow) on edges, roll, rate stretch, slip, slide, razor, pen / pen add,
  track select forward / backward, move / insert, hand / grab, zoom in / out (Alt), keyframe and the volume line. The
  cursor is set as the pointer enters a clip, holds for the whole drag, and Ctrl pressed or let go flips trim ↔ ripple
  and move ↔ insert at once. `CursorArea { shape: "zoomIn" }` sets one over a QML area without taking input (the
  hand and zoom overlay, the gutters, track heights). Disabled, hidden or with shape "" it sets nothing, so what lies
  under it shows its own cursor: Qt picks the topmost item that has a cursor even when it is disabled, which is also
  why an overlay MouseArea must be hidden, not just disabled, when it is not in use.
- **Toolbar (0.1.3):** tools, modes and actions live apart, because one mixed row confused users:
  - the Tracks header holds only the tools, as Premiere's palette groups: Selection · Track select (forward /
    backward) | Ripple (roll, rate stretch, remix) · Razor · Slip (slide) · Pen | Hand (zoom) · Type. A group's button
    shows its last-used tool and a corner mark; right-click, press and hold, or ↓ opens its flyout
    (`timeline/ToolGroupButton`, `ToolFlyout`);
  - the right of the sequence tab row holds the modes as labelled toggles (Snap, Linked; `ToggleChip`), a divider,
    the view toggles (volume and speed keys, the Timeline list) and the actions (Moments, Gaming layout, display
    settings);
  - the corner left of the ruler holds the playhead timecode and the frame rate.
  - As the panel narrows, the mode labels go first, then the view toggles fold into one menu (`CheckMenu`), then the
    palette becomes one button showing the active tool. The toolbar never disappears.
- **Clicks:** Shift toggles a clip. A plain click on a clip that is already selected narrows a multi-selection to it;
  Ctrl/⌘-click deselects it. Right-click opens the clip context menu.

### Snapping
- S toggles snapping (the labelled Snap toggle on the sequence tab row).
- **Targets** (model `edit::snapTargets`): clip edges, the playhead, In/Out marks and markers. The dragged clips' own
  edges and their starting positions are excluded.
- **Reach:** `Theme.snapThreshold` (12 px since 0.1.3; was 8), converted to time at the current zoom, so snapping reaches further when
  zoomed out.
- **Which edge snaps:** of the dragged edges, the one closest to a target lands on it.
- **Indicator:** a full-height line in `accentStroke` (selection width). The razor preview line turns accent when it
  snaps.
- **Drops (0.1.3):** a Library card, the Source Monitor's picture, a search hit or a graphics template dragged over the
  lanes lands where `TimelineView.dropPoint` says: the frame under the pointer, snapped to the same targets (so it
  butts against the clip in front instead of leaving a gap). An insertion line shows where it will land and turns the
  snap colour, with the snap tick, when it snaps. A sequence dragged from the Project panel lands the same way, as a
  nested clip (its picture and its mix, linked); into itself, or into a sequence it plays, it is refused.
- **Hold for a new track (2026-10-15):** such a drag held still over the ruler (above the top video track) or below
  the last track (the empty lanes under it, or the footer) shows a band, "Hold to add track V3" / "A3", in
  `accentWash` with an `accentStroke` hairline, filling for two seconds (`TimelinePanel.newTrackHoldMs`). Then the
  track is added (`TimelineView.addDropTrack`: one "Add Track" undo step, scrolled into view) and the insertion line
  moves onto it, where the drop lands while the pointer stays in that zone. One such track per drag; holding at the
  other end moves it there. If the drag drops on another track or leaves the timeline, the track is undone again
  (`releaseDropTrack`) while it is still empty and nothing else was edited.

### Motion (Vfinity rules; all in the TimelineView scene graph, never per-clip QML)
| What | How | Token |
|---|---|---|
| Dragged clips lift | Height grows by `Theme.liftScale` (1.02) around the middle, plus a lift shadow; the width never scales, so the edges stay on the frames the clip lands on (0.1.3: a scaled long clip looked as if it overlapped its neighbour) | none |
| Drop | Springs back to rest (OutBack, a slight dip below rest) | `Theme.durBounce` |
| Shifted clips (ripple delete, ripple trim, insert, paste insert, undo, redo, Esc) | Slide from where they were drawn to where they are, ease-out | `Theme.durRipple` (180 ms) |
| Razor cuts | A short split flash that fades | `Theme.durState` |

Under reduce motion every duration is 0 and `liftScale` is 1.0, so everything jumps to its end state. Only clips near
the view animate; clips further away move at once.

### Readout and context menu
- **Readout:** while dragging, a scrim pill next to the pointer shows the applied delta (`+00:00:01:05`, prefixed with
  "Insert" for an insert move) or, for slip, the source In/Out.
- **Context menu** (right-click on a clip, ClipContextMenu.qml), for the selection:
  - Cut, Copy, Paste;
  - Ripple Delete;
  - Enable (checked state);
  - Link/Unlink (by state);
  - Speed/Duration… (disabled placeholder);
  - Rename… (a popover with the name selected, Enter to apply);
  - the eight label-colour swatches.

### Commands (ProjectController.runCommand)
| Command | Keys |
|---|---|
| Delete / Ripple Delete | Delete / Shift+Delete, Alt+Backspace |
| Add Edit (selected clips under the playhead, else targeted tracks) / Add Edit to All Tracks | Ctrl+K / Ctrl+Shift+K |
| Ripple trim the previous / next edit to the playhead | Q / W |
| Extend the selected clip's nearest edge to the playhead | E |
| Select Clip at Playhead (targeted tracks) | D |
| Link/Unlink toggle | Ctrl+L |
| Enable toggle | Shift+E |
| Nudge | Alt+←/→ (±1), Alt+Shift+←/→ (±5) |
| Slide | Alt+, / Alt+. |
| Slip | Ctrl+Alt+←/→ |
| Copy / Cut / Paste (overwrite) / Paste Insert | Ctrl+C / X / V / Ctrl+Shift+V |

Paste and Paste Insert work at the playhead on the lowest targeted track of each kind, keeping the copied layout and
links; the playhead moves to after the pasted clips. Paste Insert first opens room on the destination tracks and every
unlocked sync-locked track.

## M5c: command palette, accessibility, drag polish

### Command palette (Ctrl/⌘+Shift+P)
- **Chord:** free in Premiere's Windows and macOS default key sets (docs/keymap.md and src/keymap/keymaps). It is a
  window `Shortcut` in Main.qml until the keymap registry gets a command id for it. A search button in the top bar
  opens the palette too.
- **Overlay:** PaletteOverlay.qml, a modal popup with a scrim. It pops in with a spring: scale from `popScale`,
  `durBounce`, `easeSpring`, opacity over `durFast`. Under reduce motion every duration is 0.
- **Rows** come from CommandPalette (C++) providers:
  - **Keymap commands:** every CommandId, with its menu group and current shortcut (`shortcutText`). They run
    through `KeymapDispatcher.trigger`, so they take the same route as their keys.
  - **Typed edit commands:** from the command catalog through CommandBridge (pencil icon; the group says "Timeline
    edit" or "Project edit"). Each action is listed once: a catalog command the keymap already runs is left out, and
    a keymap command the app has not wired yet (Speed/Duration…, Apply Default Transitions) is listed as the typed
    command instead.
  - **"Ask AI…":** a separate, always-visible row under a divider.
- **Ranking:** fuzzy subsequence matching with bonuses for word starts, runs, prefixes and initials ("rd" → Ripple
  Delete). Typing a shortcut ("ctrl+k") finds its command. Recent commands come first for an empty query and get a
  boost otherwise.
- **Disabled rows:** stay listed, dimmed (`disabledOpacity`), with the reason in `warning` ("Select a clip first",
  "Nothing to undo", "Nothing has been copied", "Not available yet"). Running one shows the reason and runs nothing.
- **Parameter prompts:** an edit command with parameters opens a short form:
  - timecode and text fields, number fields, `Toggle`, choice pills, track pills and the label-colour swatches;
  - clips and the sequence come from the selection, never typed; parameters that name an object by id (an effect
    instance, a transition) are never asked for, so commands that need one are disabled ("Needs values the palette
    cannot ask for yet") until the palette can pick them;
  - a clip parameter (`param`) is a choice of the stable parameter keys; any catalog time or length is a timecode
    field;
  - every change runs a **dry run** ("Preview: Changes 1 clip"); Apply commits one undo step named after the command.
- **AI:** "Ask AI…" opens a placeholder panel ("AI assistant is not configured") holding the typed request. The AI
  platform's AiController plugs in as another PaletteProvider; providers may return their own rows for a query and
  refresh them later.

### Accessibility of the scene-graph timeline
- **Clip list:** `ProjectController.clipList` (ClipAccessModel) is the timeline's semantic twin.
  - **Rows:** one per clip, tracks in display order. Each row has a spoken name, for example "City B-roll, V2,
    00:00:04:00 to 00:00:17:23, selected, locked".
  - **Sync:** ChangeSets (only changed tracks are rebuilt), undo and the selection keep it current.
  - **Presentation:** a ListView (`timelineClipAccess`) over the lanes whose rows draw nothing and take no pointer
    input. It is the Tab stop after the lanes and rings the lanes while focused.
- **Keyboard only** (in the clip list):
  - ←/→ move to the previous/next clip on the track;
  - ↑/↓ move to the nearest clip on the track above/below;
  - [ and ] choose the head or tail edge;
  - Ctrl+←/→ (⌥←/→ on macOS) trims that edge a frame, with Shift five frames (Premiere's Trim Backward/Forward).
    Each trim is one undo step;
  - Esc clears the chosen edge.

  The list claims these keys (ShortcutOverride), so they do not step the playhead there.
- **Not colour alone:**
  - A selected clip gets the 2 px accent outline plus a corner bracket, dark with a light core, so it shows on any
    clip colour.
  - The keyboard-trim edge shows a `[` or `]` bracket.
  - A targeted track's name chip gets a ring and a "▸" before its name. The chip is the target toggle, with the
    accessible name "Target V1" and a checked state.

### Drag polish
- **Auto-scroll:** dragging (gestures and the marquee) within `Theme.autoScrollZone` (48 px) of an edge scrolls the
  lanes. The speed is proportional to how deep the pointer is in the zone, up to `Theme.autoScrollSpeed` (1600 px/s)
  at the edge and beyond it, smoothed over about 80 ms. The drag keeps moving with the content. Vertical auto-scroll
  works the same way when the tracks are taller than the view.
- **New track:** dropping an audio clip below the last track creates a new audio track (`edit::addTrack` plus the
  move, one undo step). The preview shows the new lane, and the headers show a ghost "+ New track A4" header.
- **Linked partners across tracks (2026-10-15):** the dragged clip's kind crosses the tracks it is dragged over; its
  linked partners of the other kind cross as many tracks outward only where they land on a free stretch of an unlocked
  track, else they stay on their tracks (linked audio dragged from A2 to A1 leaves its picture on V3 when V2 holds a
  clip there; dragged two tracks, the picture goes to V1 if V1 is free). A clip the dragged kind replaces frees its own
  link partners, so dragging onto another A/V clip replaces both its halves (`movedTracks` in TimelineEditing.cpp).


## M5d: finishing (effects, keyframes, transitions, speed, titles)

### Effect Controls (the side panel in the Edit workspace; was the Clip Editor)
- **Rows:** `ProjectController.effectControls` (EffectControlsModel) is one flat list model with stable row keys
  (`h:motion`, `p:scale`, `h:e12`, `e12:radius`, `t:duration`). An edit updates rows in place; effect sections
  that move are moved rows, so the ListView animates them (`move`: a spring; `displaced`: `durState`, ease-out).
- **Sections:**
  - Transform, Opacity and (for a linked or audio clip) Audio are pinned at the top.
  - Then the clip's effect stack, video then audio. Each section header has a collapse chevron, a drag grip
    (reorder; the header lifts with `liftScale`), an fx power switch, reset and delete.
  - A disabled effect is struck through and dimmed (`disabledOpacity`). An effect this version does not know says
    "(not available)": it is kept and bypassed.
- **Parameter row:** stopwatch (`timer` icon: animate) · label · slider (hidden when the lane is shown) · number
  field with unit · reset · keyframe navigation (previous, add/remove the key at the playhead (`diamond`), next).
  A value change on an animated parameter keys the playhead. Values are UI units: % for fractions and factors,
  −100..100 for balance, and unbounded model ranges get a practical slider range (ParameterUi.h).
- **Keyframe lane:** on the right of the rows, toggled from the panel header (`Preferences.keyframeLane`, default
  on). While it is on, the side panel widens to `sideKeyframes` (`Theme.sideKeyframesMinWidth` 420 to
  `sideKeyframesMaxWidth` 600 px, 36 % of the work area), animated over `durCollapse`.
  - The lane covers the clip. Its ruler shows the clip's first and last timecode and scrubs the playhead, whose line
    crosses every row.
  - **Shape, not colour alone:** each key's shape names its interpolation. Diamond = linear, square = hold,
    hourglass = Bezier, circle = ease in and out, half round = ease in or ease out (round on the eased side).
    Selected keys fill with the accent and grow (`grabScale`).
  - **Mouse:** click selects, Shift adds, Ctrl/⌘ toggles. A drag moves the selected keys, frame-quantised and kept
    inside the clip, one undo step per drag; Alt duplicates. Dropping onto another key is refused. A drag on empty
    lane is a rubber band; right-click opens the interpolation menu (Linear, Bezier, Hold, Ease In, Ease Out, Ease
    In and Out, Delete). Delete/Backspace removes the selected keys while the panel has focus.
- **Transitions:** a selected transition shows its duration (timecode field), alignment (Center / Start at Cut /
  End at Cut pills; Custom when uneven), its kind and its parameters. A length the media cannot cover is fitted to
  the available handles, and the panel says so (`notice`, `warning`).
- **Empty:** "Select a clip or a transition…". Effects dropped on the panel are added to the clip at the stack
  position under the pointer (accent wash and border while hovering).

### Effects library (rail: Effects)
- A search field (fuzzy, as in the palette) over the EffectRegistry: Video Effects, Video Transitions, Audio
  Effects, Audio Transitions. Each row shows a category icon (`sun` colour, `droplet` blur, `crop`, `square`
  perspective, `gauge` dynamics, `volume-2` amplitude, `blend`/`fold-horizontal` transitions) and its category,
  dimmed. Intrinsic effects (Transform, Opacity, Volume, Panner) are not listed.
- Rows drag with mime `application/x-wabcut-effect` (the effect id). Enter or a double-click adds an effect to the
  selected clip.

### Transitions on the timeline
- **Drawing:** a transition is a dark frame across the edit (`markerInkColor` at 62 %) with a light bow tie meeting
  at the cut, a shape no clip has. A one-sided transition shows a single wedge. A selected one gets the selection
  glow and outline. It is drawn in the overlay pass, so a transition-only change rebuilds no track.
- **Dropping one:** the edit point nearest the pointer (48 px reach, else the nearer end of the clip under it)
  lights up with what the drop would do. The transition is dry-run first:
  - accent: it fits as asked ("Cross Dissolve · 00:00:01:00");
  - amber: "using the available handles" (shortened to the media there is);
  - red: the model's reason, or the wrong kind of track.

  A dropped transition is selected.
- **Editing one:** a click selects it (clips and transitions are selected one or the other). Its edges drag its
  length before or after the cut, frame by frame, limited by the clips and their handles, one undo step per drag.
  Delete removes it. Ctrl/⌘+D and Ctrl/⌘+Shift+D apply the default video or audio transition at the playhead;
  Shift+D applies them to the selection.

### Speed/Duration (Ctrl/⌘+R, the clip menu, the palette)
- A WabcutDialog. Speed (%, with 50/100/200 quick picks) and duration (timecode) are linked: typing one recomputes
  the other for the selected clip. It also has Reverse Speed, Maintain Audio Pitch (audio only) and Ripple Edit,
  Shifting Trailing Clips.
- Every change is dry-run through the typed command `clip.speedDuration`: "Preview: changes 1 clip", or the reason
  in amber. OK applies it as one undo step.

### Titles (Library ▸ Text: Essential Graphics)
- **Style:** a live preview card plus New Title, brand presets (one per Theme accent: accent fill on its deep
  tone), text, font (Sans/Serif/Mono/Display), size, bold, alignment, fill swatches (white, ink, clip colours),
  stroke and shadow.
- **What edits go to:** with a title clip selected the panel says "Editing “…”". Text and font change the title
  item (every clip of it); the rest becomes the clip's keyframable Title Style effect at the playhead. Otherwise it
  is the draft for the next New Title.
- **New Title:** `title.add` at the playhead on the first free video track at or above the targeted one (a new
  graphic does not overwrite the picture under it). The new clip is selected. It is also a palette row.

## M5e: the Transcript panel (edit the video by editing its text)

### Where it lives
- **Placement:** the browser slot, opened from the rail's "Transcript" entry (key `captions`, icon `captions`). It sits
  next to the Library and Effects, where Premiere puts its Text panel, so the timeline and the monitor stay in view
  while editing.
- **Tabs:** Transcript, Clean up and Captions (PillTabs). Switching tabs drops a pending preview.

### Transcript
- **Text:** paragraphs in timeline order. A new paragraph starts at a speaker change, a pause of 2 s or more, or after
  80 words.
  - Each paragraph has a header: a drag grip, the speaker ("Speaker 2", tinted with a clip colour) and the start
    timecode (mono, tertiary).
  - Words sit in a Flow with the spacing their language writes. Thai and other unspaced languages put word units side
    by side with no spaces, so lines break only between the model's word boundaries.
- **Word states:**
  - current (under the playhead): accent fill with on-fill text;
  - selected: accent wash with an accent hairline;
  - search match: warning wash;
  - removing (in a preview): danger text, struck through, on a faint danger wash. This state wins over the others
    (no fill or hairline), so a cut reads as one red band.
- **Pointer:** a click moves the playhead to the word and selects it; Shift extends, Ctrl/⌘ toggles. The playhead
  is on frames: a seek goes to the first frame that starts inside the word (or cue), so it is the current one. A drag selects
  across paragraphs. Selected words light their time range on the timeline: an accent wash with edges on their
  tracks and the linked video (`ProjectController::setTimelineHighlights`, drawn in the overlay pass).
- **Edits are previewed first:**
  - Delete (Delete/Backspace, claimed before the keymap's Delete while the text has focus) and Keep only dry-run the
    plan: the words strike through, the timeline shows each cut as a red ghost with a strike line
    (`Theme.danger`), and the action bar turns danger-washed with "Removes 00:00:04:02 in 2 cuts".
  - Remove (or Return) applies it as one undo step through `transcript.deleteRanges` / `transcript.keepRanges`.
    Cancel or Esc drops it.
- **Move:** dragging a paragraph's grip moves it before another paragraph (`transcript.moveRange`; the paragraph
  lifts with `liftScale`).
- **Find:** a pill search field marks every match (case-insensitive, across words and inside Thai runs). Return or
  ↑/↓ jump to the next match (selected, playhead there); "Select all" selects every match.
- **No transcript yet:**
  - **Provider list:** radio rows for "On this computer" (whisper.cpp, when the build has the local helper), then the
    cloud transcription models from Preferences › AI, each with its price or the reason it cannot run.
  - **Actions:** "Transcribe sequence" and "Selected clips". A cloud provider shows the AI consent prompt
    (AiConsentDialog) before the first upload.
  - **Progress:** a thin accent bar with Cancel.
  - **Set up:** with no usable provider, a sunken card explains how to get one and opens Preferences › AI.
  - **Where results go:** into the analysis cache, so other projects using the same media find them.

### Clean up
- **Controls:** a Fillers switch and Find, then "Silences over" (seconds, 0 = off) and "Padding around cuts" (ms) as
  label/field rows, so the narrow panel never scrolls sideways.
- **Items:** each found item is a card with its kind icon (`type` amber for fillers, `volume-x` info-blue for
  silences), the label, the context ("…so we went [um] to the…"), its length and a remove switch (on for confident
  finds). Cards set to remove get a faint danger wash.
- **Bulk and apply:** "Remove all" / "Keep all", then Preview (the words strike through, the timeline ghosts) and
  Remove: one undo step (`transcript.deleteRanges`, fillers as words, silences as pauses keeping room tone).

### Captions
- **Generate / Regenerate:** `captions.generate` with characters per line and 1 or 2 lines. Thai lines break between
  word units.
- **Cues:** each cue is a card with start and end timecode fields (sized to their timecode, giving way in a narrow
  panel), a go-to button, a delete button and its text
  (TextArea). Edits are committed when a field is left, one undo step each. The cue under the playhead is
  accent-washed.
- **Style:** "Show on monitor" on the Style header; presets Clean, Boxed (the default: white on a dark box), Brand
  (accent soft on accent deep) and Pop; a Box switch and Top/Bottom in the title-safe area.
  - The compositor draws the captions in the picture with this style (I.4): the program monitor and burned-in
    exports alike. The M5e QML overlay is gone.
  - The model stores no caption style yet; the style lives in CaptionsBridge (CaptionStyleMapping.h maps it).
- **Export:** SRT or VTT through a save dialog, written by `toSrt`/`toVtt`, which keep the cue's line breaks.

## M5f: the Home screen and the Keyboard Shortcuts editor

### Home screen
- **When:** the app opens on it (`main()` turns on `Home.showAtLaunch`; the shell tests start on the editor), and
  File > Home shows it again.
  - It covers the editor, which is disabled and, once covered, not drawn.
  - "Back to <project>" / "Explore the demo project" and Esc return to the editor.
  - No editor shortcut fires behind it (`KeymapScope.suppressShortcuts`).
- **Look:** the canvas with a soft Wabcut Violet radial glow from the top right, and floating cards (`HomeCard`):
  - the panel surface, `radiusPanel`, a soft `RectangularShadow`, an accent-wash icon chip and a title;
  - on open, the cards rise 12 px and fade in one after another (60 ms apart, `durCollapse`/`durSpring` with
    `easeOutSoft`). Under reduce-motion they simply appear;
  - interactive cards lift `liftOffset` with a shadow on hover;
  - the drop zone grows to `liftScale` with `easeSpring` when files are dragged over it (drops are where a bounce
    is allowed).
- **Layout:** two columns. The left (wider) column has the headline (`Theme.textDisplay`, Home only), One-click
  import and Recent projects. The right column has New project and Learn.
  - One column below 920 px. Content is at most 1440 px wide and scrolls vertically.
  - At 1100×700 every card fits across (tested).
- **One-click import (the headline):**
  - **Drop zone:** a large dashed `sunken` zone with a round accent icon. Files dropped anywhere on Home land here,
    and it turns accent-washed with an accent dashed border while dragging.
  - **Editor buttons:** "Import from Premiere Pro / DaVinci Resolve / Final Cut Pro" cards. Each has a neutral
    monogram chip (not the editor's logo) and one line on what to export there:
    - "In Premiere: File › Export › Final Cut Pro XML";
    - "In Resolve: File › Export › Timeline › FCPXML or OTIO";
    - "In Final Cut Pro: File › Export XML…".

    Each also lists the formats it accepts.
  - **Checks before importing:** every file is checked first (`ProjectFile.detectImport`).
    - Timelines and media import into a new project, and its first imported sequence opens (`importFiles` with
      `newProject` and `openImported`; no empty "Sequence 01" is left).
    - The import report dialog follows.
    - A `.wabcut` file opens.
  - **Experimental .prproj:** it never imports silently. A warning-tinted card names the file, says
    "experimental reader", explains the trade-off and suggests Final Cut Pro XML. "Import with the experimental
    reader" opts in.
  - **Unreadable files:** Resolve `.drp` / AAF / `.fcpbundle` get the importer's guidance ("what to export
    instead") in an info card. Import errors show in a danger card.
- **New project:**
  - Name, location (Browse… with a folder dialog; the path is shown elided in the middle), then the sequence
    preset as a 3-column grid of radio cards.
  - Each card has the frame's shape (landscape, portrait or square outline), the name and the rate in mono. The
    selected card has an accent border and accent wash.
  - Presets: 1080p at 23.976 / 25 / 29.97 / 30 / 60, 4K UHD, Vertical 9:16 (1080×1920), Square 1:1 and Custom.
  - 4K, vertical, square and custom offer frame-rate pills; custom adds width × height fields.
  - A mono summary ("1920 × 1080 · 23.976 fps") sits beside the primary Create button. Validation errors (name,
    folder, an existing file) show in danger text.
  - Create saves `<folder>/<name>.wabcut` and opens it (`Home.createProject` → `ProjectFile.newProjectWith` +
    `saveAs`).
- **Recent projects:**
  - Cards are 16:9 thumbnails: the first video's frame through `image://wabcut-thumbnails`, else an accent-deep
    gradient with the format in mono. Under each are the name, the folder (elided in the middle) and when it was
    last opened ("Today, 10:21").
  - A file that is gone is dimmed with a scrim "Missing" badge; a click offers "Remove from list". Hover shows ×
    (remove).
  - Open… is in the card header.
  - The list is kept in QSettings `home/recentProjects`, at most 12, newest first. It is updated whenever the open
    project's file changes (open, save as, new).
- **Learn:** links to the local documents that exist (`docs/interchange.md`, `docs/keymap.md`, `docs/roadmap.md`),
  each with a title, a line and an external-link icon.

### Keyboard Shortcuts editor
- **Dialog:** a large modal over the scrim, at most 1320×960 px; it fits 1100×700. The header has the title, the
  key set menu ("Premiere Pro (default)", "Wabcut", user sets; "· modified"; Save as… / Delete), Import…,
  Export…, Reset (confirmed in a popover) and close.
- **Modifier toggles:** "Keys with [Ctrl] [Shift] [Alt]" (⌘ ⌥ ⇧ ⌃ on macOS), as pills, primary when held. Beside
  them, a legend of Application / Panel / Both swatches.
- **Keyboard:** keycaps on a key-unit grid (`Theme.keyUnit`, 38 px at most; it scales down to fit).
  - Each key shows its label and, under it, the first command it runs (`textXs`).
  - Fills: application `Theme.keyApplication` (violet), panel `Theme.keyPanel` (teal), both split down the middle,
    labels in `clipInk`. Free keys are `raised`, and modifier keys are `sunken` (accent when held).
  - Selected key: an accent ring. A command dragged over a key also rings it.
  - Hover shows every command name in a tooltip.
  - Keys are pointer targets only; the command list is the keyboard path.
- **Conflict bar:** warning-tinted, between the keyboard and the list. For example "X is already used by “Mark
  Clip” (Application). Reassign it to “Razor Tool”?", with any panel/application shadowing lines, then Cancel and
  primary Reassign (Assign when it only shadows).
- **Message bar:** what happened, or an import report. It is success-, info- or danger-tinted and dismissible.
- **Commands:** a pill search field, a panel filter menu, and a list in sections by group (uppercase tertiary
  labels).
  - A row has a grip, the name, a teal panel chip for panel commands, the shortcuts as mono keycaps (× on hover),
    + and, when overridden, a reset arrow.
  - The row being typed into shows an accent-washed capture keycap ("Type a shortcut… (Esc cancels)", then the
    modifiers held, "Ctrl+…").
  - Commands that Wabcut does not run yet are tertiary, with a tooltip.
- **Key detail panel:** a `sunken` side panel with the selected chord as a large mono keycap. It lists the
  commands bound to it per panel (an application/panel colour bar, name, panel, ×), and "Give to “<command>”" for
  the command chosen in the list.
- **Footer:** "Changes apply at once and are saved", the platform, and Done.

## M5g: the Audio workspace and Bezier keyframe handles

### Layout
- **Where things go:** the Audio workspace (dock) opens **Audio Tools** in the browser slot. The rail also has it, as
  "Audio Tools". The **Audio Track Mixer** goes in the side slot.
  - The side slot widens to about half the work area (`Theme.sideMixerMinWidth` at least, animated with
    `durCollapse`).
  - The timeline's share drops to 0.38 (0.44 compact) so the strips get height.
  - The timeline scrolls to the audio tracks and shows their volume keyframes.
- **Leaving:** the browser panel it replaced comes back, and the band hides again.
- **Meters:** they poll only while the mixer is on screen.

### Audio Track Mixer
- **Track strips:** one per audio track, `Theme.mixerStripWidth`, a raised card. From the top:
  - the track chip in its clip colour, with the track name;
  - a **role** chip: Dialogue (info), Music (violet), Effects (orange) or "No role", which opens a menu
    (`track.setRole`);
  - the **effect slots**: a dot when active, dimmed and struck through when bypassed. Each slot's menu bypasses it,
    moves it up or down, or removes it. "+ Effect" lists Gain, EQ, Compressor, Limiter and Reduce Noise. Each change
    is an `audio.*` command on the track;
  - the **pan** knob, an arc from the centre;
  - **M / S / R**: mute (warning), solo (success) and a record-arm placeholder (disabled);
  - the peak-hold readout and the **clip light**, which is danger-red and latched until clicked;
  - the **fader** beside the stereo **meter**;
  - the gain in mono.
- **Master strip** (`Theme.mixerMasterWidth`): two columns.
  - **Left:** the "Mix" chip, the **Limiter** preset (`audio.masterLimiter`, −1 dBTP; "Limiter on" once the master
    ends with one), the master effect slots, and the **loudness** card.
  - **Loudness card:** the target menu (YouTube −14, Podcast −16, Broadcast (EBU R128) −23, Custom with a LUFS
    field). The integrated LUFS in `textTitle` mono, and a −40…0 LUFS bar with the target tick (success within
    1 LU, warning otherwise). Then "2.2 LU below the target", short-term and momentary maxima, true peak (danger
    above the ceiling) and LRA.
  - **Analysis label:** "Analysis 11:26 AM" (plus "the project changed" when stale) beside **Analyze**. These are
    figures from the last analysis of the mix, not a live meter: an RT-safe live loudness tap is logged for the audio
    engine.
  - **Right:** the master fader, meter and clip light, at the strip's full height.
- **Meter** (`MeterBar`, a C++ scene-graph item: a few rectangles per channel, about 30 Hz).
  - Two bars on `sunken`, scale −60…0 dBFS. RMS fills in success (below −18), warning (to −6) and danger.
  - The peak is a 1 px `textPrimary` line; the peak hold is a 2 px bar in its zone colour, held 1.5 s.
  - QML only sees the clip light and the hold readout.
- **Fader and pan:** these wait for track gain and pan in the model. They are disabled (`disabledOpacity`), with the
  note as their tooltip.
  - The **fader** scale is +12 dB to −∞ with a 0 dB tick. Drag; Shift drags fine; double-click sets 0 dB; ↑/↓ step
    0.5 dB (0.1 with Shift).
  - The **pan knob:** drag; double-click centres; ←/→ step.

### Audio Tools
- **Cards:** each is a `sunken` card with an accent-wash icon chip, a title and one line on what it does:
  - **Analyze loudness:** Mix / Tracks / Clips tabs, then a table (target, LUFS, LRA, dBTP; true peak danger above
    the ceiling);
  - **Normalize to target:** the target menu, Mix / Selected clips, "Preview normalization";
  - **Auto-duck music under dialogue:** amount (dB), attack, release and hold (s), then "Preview ducking";
  - **Reduce noise:** amount (%), "Reduce noise on selected clips" (`clip.addEffect` / `clip.setEffect` with
    RNNoise, one undo step).
- **Dry runs:** measuring commands run on a worker thread over a copy of the project.
  - While one runs, an accent-wash bar says what is being measured, with Cancel.
  - A dry run waits in a warning-tinted card: the title ("Duck the music by 12.0 dB"), what it changes
    ("1 speech region(s), 6 volume keyframes on 1 music clip(s)", or per-target "before → after LUFS (gain)"),
    warnings, then Cancel and **Apply**.
  - Apply commits the previewed project as one undo step, but only if nothing changed since the preview. Any edit
    drops the preview.
- **Auto-duck ghosts:** the planned volume of the music clips is drawn over them on the timeline as a dashed accent
  rubber band with dots (the external preview's `volumes`).

### Timeline volume rubber band
- **Toggle:** the spline button in the Tracks header, on in the Audio workspace.
- **The line:** each audio clip's Volume is a light line on a dark underlay, readable on every clip colour in both
  themes. It is sampled every 3 px, so Bezier and ease shapes show. Keys are warning-amber: diamond for linear,
  circle for Bezier, square for hold.
- **Position:** the band sits under the clip label. Its scale is fader-like: +6 dB at the top, 0 dB a quarter down,
  −12 dB halfway, −60 dB and quieter at the bottom.
- **Editing** (Selection tool):
  - click the line to add a key at the level it has there;
  - drag a key to move it in level and time (frame-quantised, between its neighbours; Shift: level only);
  - drag the line to move a constant volume, or the two keys around the press;
  - Alt-click a key to switch between Bezier (smooth tangents) and linear.

  Each drag is one undo step. The cursor shows a hand over keys and ↕ over the line.

### Bezier handles (Effect Controls)
- **Value graph:** an animated parameter's spline button opens its value graph in the keyframe lane. The row grows
  by `space(24)`.
  - The graph is a `sunken` well with the curve as a 2 px accent line, sampled across the clip.
  - The value range fits the curve, keys and handles with a margin, and is frozen while a handle is dragged.
  - Keys sit at their values.
- **Handles:** Bezier keys show in and out handles: a hairline to a ring knob. They are accent-coloured when linked,
  warning-coloured when broken.
- **Dragging a knob** shapes that side's tangent: how far along the segment is the influence, how high is the
  slope (`edit::setKeyframe`, one undo step).
  - The two handles of a key stay linked: the other side takes the same slope and keeps its length.
  - Alt breaks them.
- **Becoming Bezier:** a key turned Bezier from the interpolation menu gets smooth tangents, the slope between its
  neighbours (Premiere's Auto Bezier), instead of flat ones.

## M5h: the Source Monitor and three-/four-point editing

### Layout
- **Edit workspace:** the Source Monitor (left) and the Program Monitor (right) sit side by side in the monitor area.
  Below `Theme.dualMonitorMinWidth` (580 px of monitor area) one monitor shows at a time, with a Source / Program
  switch (`MonitorSwitch`) in place of its header.
- **Other workspaces:** the Program Monitor only.
- **Which monitor shows:** loading a source (Library double-click, Match Frame, the recent-sources menu) shows the
  Source Monitor; Reverse Match Frame shows the Program Monitor.

### Source Monitor (`SourceMonitorPanel`, bridge `SourceMonitor`)
- **Picture:** its own `ProgramMonitor` (`sourceMonitorPicture`) plays the source through a private PlaybackSession.
  It has its own transport and sound, and pauses the program (and the other way round).
- **Audio-only media:** a waveform (`SourceWaveform`) replaces the picture. A click moves the playhead; Ctrl/Cmd-drag
  scrubs.
- **Over the picture:** the source's name and the recent-sources menu (top), and the drag-video-only and
  drag-audio-only handles (bottom). Dragging the picture itself drags video and audio.
- **Readouts:** the source timecode, the In/Out duration, and the whole duration when there is room.
- **Range bar:** the In/Out range bar over a mini timeline with the playhead; press to jump, drag to scrub.
- **Transport:** Mark In / Out, Go to In / Out, step back / forward (dropped first in narrow monitors), Play,
  Play In to Out, then Insert and Overwrite. Hovering Insert / Overwrite shows the edit as a ghost on the timeline
  (a dry run in TimelineView's external preview).
- **Marks:** stored on the media item (`MediaItem::markIn/markOut`, saved with the project) through
  `source.setMarks` / `source.clearMarks`, one undo step each.
- **Keys:** the panel's keymap scope is `sourceMonitor`; the routing is in [keymap.md](keymap.md), "The Source Monitor
  scope".

### Program Monitor additions
- **Buttons:** Add Marker, Mark In / Out, Go to In / Out, frame steps, Lift, Extract, and Export Frame. Export Frame (the
  camera) writes a PNG of the frame at the playhead, from the offscreen export renderer, next to the project or in a
  chosen folder.
- **Readout row:** timecode, safe margins (action and title safe from Theme insets), zoom (Fit, 25–400 %), playback
  resolution and proxies, J/K/L speed and stats.

### Track headers: Premiere's two columns
- **Source patching (left column):** shown while a source is loaded. Each indicator (V1, A1, A2, …) is filled
  when it takes part in Insert / Overwrite and outlined and struck through when switched off.
  - Click an empty slot to patch the source there.
  - Drag an indicator to another track to repatch; an indicator already there swaps places.
  - Alt-click to switch an indicator off or on; Space does the same from the keyboard.
  - Patching is view state per sequence, checked by `sequence.setPatches`.
- **Targeting (the name chip):** a ring and "▸" when targeted; changed through `sequence.setTargets`.

### Fit Clip (`FitClipDialog`)
- **When:** a four-point Insert / Overwrite whose source and sequence ranges differ by a frame or more.
- **Choices:**
  - change clip speed (fit to fill), through `edit::setSpeed` in the same undo step;
  - ignore source In;
  - ignore source Out;
  - ignore sequence In;
  - ignore sequence Out.
- **"Always use this choice":** stores the choice in `Preferences.fitClipChoice`.

## M5i: the pro-grade timeline

### Sequence tabs (`SequenceTabBar`, bridge `SequenceTabs`)
- **Where:** a strip of tabs above the ruler, one per open sequence (Premiere). The current tab is raised, with a
  slice-cut underline in the accent.
- **Gestures:**
  - Click a tab to show its sequence.
  - Close a tab with its × or a middle click; the last tab stays.
  - Drag a tab sideways to reorder; an accent line marks where it lands.
  - Ctrl+Tab / Ctrl+Shift+Tab cycle tabs from the Timeline.
- **What each tab keeps:** its playhead (`SequenceTabs`), its zoom and scroll (TimelineView, per sequence), and its
  targets and source patching (per sequence already).
- **Opening:** double-click a sequence in the Library's strip of sequences (Media tab, above the media). The sequence
  menu and New Sequence open a tab too.
- **Nested sequences:** when the model has them, a nested sequence clip opens its tab through `SequenceTabs.open()`.

### Track heights
- **Storage:** TimelineView keeps the height per track (view state). Track headers bind their height to
  `trackHeightOf()` through `layoutRevision`.
- **Resizing:**
  - Drag a header's bottom edge (`Theme.trackResizeZone`); a double-click resets the track.
  - Ctrl/Alt with + and - step video / audio tracks by `Theme.trackHeightStep`.
  - Shift+= expands all tracks; Shift+- minimises them to `Theme.trackHeightMin`.
- **Presets:** the display settings save and recall them, as Premiere does (`Preferences.trackHeightPresets`).
- **Waveforms:** they scale with the audio track's height.

### Video filmstrips (Premiere's Video Head / Head and Tail / Continuous)
- **Pipeline:** `MediaFilmstripSource` extracts frames with media::ThumbnailService (disk-cached), using the media's
  proxy when it has one, and packs them into one atlas per media item on its worker thread. The GUI thread never
  decodes.
- **Rendering:** TimelineView uploads each atlas as a texture. Each track has one textured node per media atlas,
  between its clip mesh and a second mesh drawn over the thumbnails (hatching, through edits, clip markers, duplicate
  frame bands and the scrim behind the clip name).
- **Density:** Continuous adapts to the clip's width: the head frame when it is under two tiles, head and tail under
  three, a full strip beyond that. Head and Head and Tail are fixed.
- **Loading:** tiles still loading are flat placeholders (`Theme.filmstripPlaceholderAlpha`).
- **Memory:** atlases live in memory up to a byte budget (128 MiB), least recently used first out.
- **Video only:** audio tracks keep their waveforms from the peak cache.

### Trim mode (bridge `Trim`, `TrimView` in the Program Monitor)
- **Edit points:** a clip's head or tail. Premiere's brackets mark them: red for roll and regular trims, yellow for
  ripple (`Theme.trimRoll` / `Theme.trimRipple`); a roll brackets both sides of the cut.
- **Selecting:**
  - A click on an edge selects it: a regular trim with the Selection tool (Ctrl/⌘: ripple), or the Ripple and Roll
    tools' own type. Shift adds more.
  - Trim Edit (Shift+T) selects the edit points nearest the playhead on the targeted tracks as a roll and opens the
    Trim view.
  - Toggle Trim Type (Ctrl+Shift+T; Control+T on macOS, Premiere's shipped keys) cycles roll → ripple → regular.
  - Clicking a clip, or Esc, leaves trim mode.
- **Trimming:**
  - Ctrl/⌥+arrows trim one frame; with Shift, the Large trim offset (`Preferences.trimLargeOffset`, 5).
  - J/K/L trim dynamically: playback starts at the edit and K stops and trims by how far it played. K+J/L taps trim
    one frame.
  - Every trim is one undo step.
- **The Trim view:** while open it takes the whole monitor area.
  - Outgoing | incoming frames, each rendered with only its track visible, off the GUI thread.
  - Each side's clip name, source timecode and offset badge (frames trimmed since trim mode started).
  - Buttons: −5, −1, Play Around (a preroll/postroll loop), +1, +5, exit, and the trim type.

### Markers 2.0
- **Colours:** Premiere's eight (`Theme.markerColors`: green, red, purple, orange, yellow, white (a cool grey in light
  mode), blue, cyan).
- **Types:** comment, chapter, segmentation and web link (URL, http or https only, plus a frame target).
- **The ruler:** each sequence marker is a slice-cut flag; a range marker adds a bar to its end, and the lanes show a
  wash across it.
  - Drag a flag to move the marker; Alt-drag sets its duration.
  - Double-click opens `MarkerEditor`: name, comment, In, duration, colour, type, URL.
- **Clip markers:** they live on the media item and draw on every clip that uses that part of the source, as a flag at
  the clip's top. M with a selected clip under the playhead (Timeline) or in the Source Monitor adds one.
- **The Markers panel:**
  - Sequence or Clip scope, a search over names and comments, and the colour filter swatches.
  - Rows show the colour bar, name and type badge, then times and comment. Click jumps to the marker, double-click
    edits it, Delete removes it.
  - Export: CSV with Premiere's columns, or YouTube chapters with YouTube's rules reported as warnings.
- **Keys:** M, Shift+M / Ctrl+Shift+M (next / previous), Ctrl+Alt+M (the marker at the playhead), Ctrl+Alt+Shift+M
  (all).

### Timeline display settings (the Tracks header's sliders button, `TimelineDisplayPopover`)
- **Thumbnails:** off / Head / Head and tail / Continuous.
- **Waveforms:** Rectified (drawn from the bottom) and Logarithmic (a 48 dB range).
- **Indicators:** through edits (a bow tie on cuts that continue the same source), duplicate frame markers (a band
  along the bottom of video that reuses another clip's frames, one colour per source), clip markers, the snap
  indicator.
- **Selection:** linked selection; Alt inverts it for one click. The chain button in the Tracks header toggles it too.
- **Track heights:** expand all, minimise all, reset, and the presets.
- **Persistence:** all of it is saved in `Preferences` (timeline/*).

## M5j: accessibility, onboarding and the v2 polish pass

### Time remapping
- The timeline's timer button shows the speed band on video clips. Click the band to add a speed key, Ctrl-drag
  vertically to scale segment speed, drag a key's left or right half outward to create a smooth transition, and
  Alt-drag horizontally for a hold. The drag readout previews the requested amount; release applies it and Esc
  cancels it. Commands validate media bounds and linked clips; every gesture is one undo step.
- Effect Controls has a collapsible Time remapping section with source-time and speed graphs, key interpolation,
  and presets for ramp in, ramp out, ramp in-out, hold and reverse at the playhead. Up/Down chooses a graph key,
  Left/Right moves it, Insert adds at the playhead and Delete removes it.
- Frame sampling is implemented. Frame blending is shown disabled. Audio plays only in straight forward 100%
  segments; ramps, holds, reverse and other speeds are silent under C.6's policy.
- The timeline keeps its batched scene graph: speed bands add mesh geometry only while enabled.

### Clean-up audio checks
- Find checks candidate pauses through the audio energy service on background workers, grouped by media stream.
  The progress beam and Cancel control remain available while decoding runs.
- Cancel, edits, sequence/project switches, selection changes and changed review options invalidate pending results.
  Completed results are published only when their snapshot and selection still match the editor.

### The Timeline list (`TimelineListView`, the Tracks header's list button)
- **What:** the timeline as a list, for screen readers and keyboard users: one heading per track, then its clips in
  time order, each with its name, start, duration, effects and whether it is selected (`ClipAccessModel`).
- **Roles:** the list is `Accessible.List`, track headings `Accessible.Heading`, clips `Accessible.ListItem`. A clip's
  spoken name: "Interview A, V1, 00:00:04:00 to 00:00:17:23, 00:00:14:00 long, effects: Gaussian Blur, selected".
- **Keys:** Up/Down move, Return/Space select (Ctrl+Space adds), Home/End, Delete removes, Esc goes back to the lanes.
  Choosing a clip moves the playhead to it and reveals it on the lanes.

### Keyboard only, end to end
- Tab and Shift+Tab always move focus (the dispatcher never takes them); a button reached with the keyboard takes
  Space; media cards and sequence chips take Return.
- Shift+1 / Shift+3 / Shift+7 (Premiere's panel keys) focus the Library, the Timeline and Effects. The path import →
  Source Monitor → Insert → edit → export works without a mouse (a test drives it).
- **Focus is never lost:** popovers give focus back to what had it when they close; after a workspace or panel
  switch, an item that is now hidden hands focus to the timeline (else the workspace tabs). Esc closes the top
  popover, dialog or tour, one at a time.

### High contrast and the interface size (Settings > Appearance)
- **High contrast:** `Theme.highContrast` swaps the surfaces for black (white in light mode), text for full white
  (black) and borders for strong hairlines. Every text token passes WCAG AAA (7:1) on its surfaces, accents 4.5:1,
  strong borders 3:1 (a test checks both themes). `Theme.pick(dark, light, hcDark, hcLight)` chooses per token.
- **Interface size:** 100 / 125 / 150 % (`Preferences.uiScale`), applied as `QT_SCALE_FACTOR` when Wabcut starts
  (main.cpp, unless the environment sets one); the page says so until the restart.

### Onboarding (`qml/onboarding/`, fun, skippable, never in the way)
- **Welcome ("Coming from Premiere?")**: on first run, after the launch Home screen closes (never over it). Five
  pages: hello (a run of Premiere keys lighting up, what carries over); your keys (Premiere's defaults, or import a
  `.kys`, with a taste of the active keys); where you start (the workspace, remembered as `Preferences.startWorkspace`)
  and your look (accent, dark or light); AI on your terms (M5n: each AI feature and where it runs, on this computer or
  with your own provider key; a cloud call asks for consent first and stays within your monthly cap; the no-cloud
  lock in Settings > AI); ready (the tour or the Keyboard Trainer). Every choice applies at once
  behind the scrim. Skip or Esc on any page ends it; either way `Preferences.welcomeDone` keeps it from coming back.
- **Tour (`CoachMarks`)**: three slice-style coach marks, Library → Timeline → Export. The rest of the window dims;
  the highlighted part stays live, so opening Export on the last step completes the tour. The callout is a slice
  card beside the highlight (else below or above it), with a slice tag "2 / 3" on the frame. Right/Return go on, Left
  goes back, Esc ends; focus goes back where it was.
- **Keyboard Trainer (`KeyboardTrainer`, bridge `Trainer`)**: Wabcut names an action and you press its shortcut,
  resolved from the active keymap (an imported or edited key set trains its own keys). Rounds of ten from Editing,
  Clipboard (Paste Attributes, Paste Insert), Markers (In/Out and markers), Trim, Playback, Tools, Timeline, Multicam
  (M5n) or all; a command without a key in the active set is left out. Score: 100 a prompt, up to 50 for speed, 10 per streak step, half with
  the hint. A right answer lights the keycaps and plays the UV-beam spark (a 12° beam slash behind the prompt; none
  under reduce motion); a wrong key shakes the prompt and says what that key does. The stage takes every key
  (ShortcutOverride) except Tab and Esc, so nothing in the editor runs. The best streak is kept.
- **What's new (`WhatsNewDialog`)**: static highlights, refreshed per build from the milestones merged since the last
  refresh (M5n: M5k-M5m, C.7-C.12, A.3-A.9, AI-9-AI-13); nothing is fetched.
- **Help:** the rail's Help button (Welcome and setup, Take the tour, Keyboard Trainer, What's new, Keyboard
  shortcuts) and the palette's Help rows run any of it again.

### The v2 polish pass
- **Sentence case everywhere:** all caps only for acronyms and file formats (LUFS, HEVC, SRT) and for micro-tags at
  `Theme.textMicro` (10 px). The Color panel's group labels, the compare overlay and the Scopes now follow it. A test
  scans the QML for all-caps labels and for `Font.AllUppercase` above the micro size.
- **Colours come from Theme:** no hex colour in QML outside Theme.qml (a test scans for them). Titles and captions use
  `Theme.contentWhite` / `contentInk`, content colours that never follow the UI theme.
- **Empty states:** every panel's empty and no-match states use `EmptyState` (illustration, one line, one action):
  the Library (empty project, empty tab, no match with Clear search), Effects (no match), Effect Controls (nothing
  selected, a new "effects" illustration), Markers (none yet, a new "markers" illustration; no match), Captions,
  Clean up, the Keyboard Shortcuts list and the palette (text only, no illustration).
- **Render light on every job:** `BeamProgress` on export pills, Collect Files, the smart search index pill, the proxy
  queue pill, audio jobs and the assistant. Jobs without a percentage use `indeterminate` (the whole track lit, the
  highlight moving).
- **Tooltips show shortcuts:** buttons and tabs with a keymap command show its key (`command:`), now including the
  workspace tabs (Premiere's workspace keys, Export Media) and the rail's Library and Effects.
- **Keymap rows (docs/keymap.md):** M5i's trim and all-track rows are verified against the shipped key sets: Toggle
  Trim Type is Ctrl+Shift+T (Control+T on macOS), the trims and Expand / Minimize All Tracks are `context.global`.

## M5k: the Library (Premiere's Project panel) and Explain this frame

### The Library (`LibraryPanel`, bridge `Library` = LibraryBridge, rows `Library.items` = LibraryModel)
- **Bins:** nested bins in the model (`ProjectOps.h`: `addBin`, `renameBin`, `moveToBin`, `removeBins`; one undo step
  each). New Bin (Ctrl/⌘+B) makes "Bin 01" in the bin shown and starts an inline rename; New Bin from Selection
  (the item menu) makes a bin named after the first item and moves the selection into it. Double-click a bin to open
  it in place; its menu opens it as a **bin tab** (text tabs with a slice-cut underline above the breadcrumb; × or a
  middle click closes one). The **breadcrumb** (Project › Footage › Day 1) walks back up; while Find lists the whole
  project it says how many it found instead.
- **Moving:** drag cards or list rows onto a bin card, a bin row, a crumb or a bin tab
  (`application/x-wabcut-library`: the selection's keys). Media still drag onto the timeline
  (`application/x-wabcut-media`). Deleting a bin deletes what is inside; the model refuses while a sequence outside
  it uses its media, and the toast says which.
- **Views:** grid (icon view) or list (Premiere's list view), `project.iconView` / `project.listView` /
  `project.toggleView`, the two header buttons. Rows are grouped bins, then sequences, then media, and sorted inside
  the groups by the list's sort column.
- **Grid cards (`LibraryCard`):** a 16:9 well with the thumbnail or the **poster frame** (decoded on a pool thread,
  `PosterImageProvider`), else a dark body tinted with the item's label; a 3px **label strip**; the duration badge;
  tags (Offline, AI, SUBCLIP micro-tag, the proxy chip with its render light); the name and a mono meta line (a
  subclip shows its In–Out). Selected: an accent outline over the accent wash. **Hover scrub** (Shift+H) shows the
  timeline's filmstrip tile under the pointer (`FilmstripScrub`: a sub-rectangle of the atlas texture, no decoding)
  and the timecode in the badge; "Set poster frame here" in the menu uses that position.
- **List view (`LibraryListView`):** a TableView over the model, so only the cells in view exist. Columns: Name,
  Label, Type, Duration, Frame rate, Resolution, Codec, Audio, Media In, Media Out, Proxy, Analysis (Transcribed ·
  Indexed · Shots), Generated, Date modified, File path, Comment, Description. Click a header to sort (again:
  descending), drag a header to move its column, drag its right edge to resize; the columns button chooses them (the
  name always shows). All of it is saved (`library/*` settings). 28 px rows, alternate rows on `sunken`, mono for
  times and numbers.
- **Editing:** a click on the name of the selected item, F2 or Enter renames inline; double-click a Comment or
  Description cell to edit it; the Label submenu (Premiere's 16 names) labels the selection. Every edit is one undo
  step (`setMediaAttributes`); new clips of a media item take its name and label.
- **Subclips:** Make Subclip (Ctrl/⌘+U) asks for a name and "Restrict trims to subclip boundaries" (on, Premiere's
  default) and makes the subclip from the Source Monitor's marks (or, from the item menu, the item's own marks). A
  subclip is a media item with its master's file and a range; its marks start as the range, so three-point edits use
  it; restricted, no trim, slip, slide, speed, remap or transition can reach outside it (the model's `sourceBounds`).
  Edit Subclip changes In, Out and the restriction.
- **Find (Ctrl/⌘+F):** the search bar's text matches names, comments, descriptions and paths; the funnel's filters
  (type, label, transcribed, offline, with proxy, AI-generated, unused, duplicates, file date) are chips, the active
  ones on the accent fill. Any of them lists matches from every bin, each with its bin path. With Smart on (AI-8) the
  smart results replace the grid and the same filters narrow them.
- **Housekeeping (the tools menu):** hover scrub; Show unused media / Remove unused media (a confirmation popover; one
  undo step; the files stay on disk); Show duplicate media (the same file content imported twice; subclips do not
  count); Make subclip. Reveal in File Explorer (Finder on macOS) is in the item menu.
- **Proxies (I.3) and analysis:** proxy state from MediaReadiness; transcript, search index and shot detection from a
  background scan of the analysis cache and the smart-search index (never on the GUI thread).
- **Privacy (review P2-9):** thumbnails, posters, hover-scrub tiles and timeline filmstrips are decoded from the raw
  file, so a frame a source redaction covers is not shown (a placeholder instead).

### Explain this frame (`FrameInspectorPanel`, bridge `FrameInspector`)
- **Where:** the side panel in the Edit-like workspaces; Effect Controls' crosshair button opens it, × goes back. It
  evaluates only while it is on screen, after the playhead or the project changes.
- **Picture, top layer first:** track and clip, the source (original, proxy, proxy for an offline original,
  generated title) and its file, the source timecode and frame number in the media's rate, the timing (speed,
  reverse, freeze frame, or the time-remap segment "segment 2 of 3, 200 % forward"), a transition in progress with
  its side and progress, opacity and motion, the effect stack (applied, off in the project, bypassed because this
  version cannot run it, bypassed for the preview) with each effect's masks, the grade's active stages (input LUT →
  basic → creative → curves → wheels → HSL → vignette), redactions, and "covered by an opaque layer above".
- **Sound, A1 first:** the source timecode and sample, why a clip is silent (track off or muted, another soloed, the
  clip disabled, offline, or not 100 % forward under C.6), the gain chain (clip volume and pan, the A.3 track fader
  and pan, the master fader),
  clip and track effects, the audio transition, bleeps and mutes. **Master:** the master stack. **Captions:** the cue,
  its In–Out and where its timing comes from (cue In/Out, karaoke word times from the transcript or estimated).
- **Preview-only overrides:** "Solo" on a layer or clip and "Bypass" on an effect change what the monitor and the
  speakers play through a private copy of the project (the program's PlaybackController and TimelineAudioPlayer follow
  it; the same scheduler and track renderer). The project's session is never written: no undo step, no revision.
  A banner counts the overrides and clears them; they follow edits and drop on a sequence or project switch.
- **Built on the evaluators:** `explainFrame()` reads `playback::evaluateVideoLayers` (with and without occlusion
  culling), `resolveSource`, the LayerSpec effects, grades and redactions, `captionTrackFor`; a test compares it
  with the evaluator on a timeline with a remap, a dissolve, a grade with an effect mask, a redaction, a caption and
  a bleep.

## M5l: multi-camera, nested sequences, track faders and the audio tools

### Multi-camera (bridge `Multicam` = MulticamEditBridge)
- **Create Multi-Camera Source Sequence…** (the Library's item menu on a selection of media; `MulticamCreateDialog`):
  the angles in order, each with a name field (empty: the file's name); **Synchronize by** as `PillTabs` (Timecode,
  In points, Out points, Clip markers, Audio), proposed from the items (all with timecode → Timecode, all with sound →
  Audio, else In points); the sequence name ("<first angle> Multicam" as the placeholder). Create runs
  `multicam.create` on a worker (a `BeamProgress` with the phase). By Audio it first measures the angles with
  `audio.syncClips` (A.5) so the report has them. **The report** replaces the form: "“X” is in the Library, synced by
  audio", then a four-column grid (angle · name, where it starts as a timecode in mono, the confidence: "exact" for the
  marks, "93 %" by sound, a drift or problem note in warning) and the warnings. The Library shows the new source.
- **The multi-camera view** (Program Monitor; the monitor bar's grid button, Shift+0): the well splits in two, the
  **angle grid** on the left (`panels/multicam/MulticamView.qml`), the program picture on the right.
  - The grid is the multicam clip under the playhead (the topmost video track that has one): 2×2 up to 4 angles, 3×3
    up to 9, 4×4 up to 16. It is **one picture** drawn by the export renderer (`MulticamGrid`: a private copy of the
    project where each angle's track is scaled into its cell, one composite per frame, the monitor's colour
    pipeline) at 640×360, on a worker that keeps the source open while playing (live) and draws one frame when
    paused.
  - Each cell: a scrim label bottom left (the key number in mono, the angle's name, a speaker when it is heard). The
    angle in the picture has the **active border** (`selectionWidth + 1`): warning yellow when paused, danger red
    while playing (switches are being recorded), as in Premiere. Hover draws a hairline.
  - The header: the source's name (hidden in a narrow monitor) and a compact pill with a menu, **Picture and sound /
    Picture / Sound** (what a switch changes).
  - The footer (mono, tertiary): "640×360 · 1.0 ms · p95 1.3", the grid's request-to-readback cost per frame with the
    renderer open ("640×360 · opened in 160 ms" until a frame reused it); "Recording: 3 switches" in danger while
    playing. The worker keeps its source open while paused too, so stepping forward costs a frame, not a reopen.
- **Switching** (Premiere's keys, docs/keymap.md "Multi-camera"): a click or **1-9** is Select Camera, Ctrl/⌘+click or
  **Ctrl+1-9** (Control on macOS) is Cut to Camera. Paused, Select switches the piece under the playhead and Cut cuts
  at the playhead (`multicam.switchAngle`, one undo step each). While playing both record a switch at the playhead;
  playback's stop sends them as `multicam.switchAngle` "switches" lists in one batch: **one undo step** ("Multi-Camera
  Switches"). The camera keys act only while the view is on.
- **In the timeline:** a multicam clip is labelled "MC2 · Close" (the active angle), has a second 2 px stripe in
  `multicamStripe` (mint) under its label strip, the stacked icon at its right end, and a small mint triangle at each
  **angle cut** (the edit point between two pieces of the same source). **Flatten** is in the clip menu
  (`multicam.flatten`); Open Multi-Camera Source opens the source in a tab.

### Nested sequences (bridge `Nesting` = NestingBridge)
- **Nest…** (the clip menu, Premiere's right-click > Nest…): `NestDialog` asks for the name ("Nested Sequence 01",
  02, …; Enter nests); `sequence.nest` takes the selection and its linked partners; one undo step.
- **Nested clips** get a 2 px `nestedStripe` (violet) under the label strip and the stacked icon (two offset frames
  drawn in the stripe colour) at the right end; the label is the clip's name.
- **Opening:** a double-click on a nested or multicam clip (or the menu's Open Nested Sequence) opens its sequence in a
  **sequence tab** (M5i) with the playhead on the frame the clip showed.
- **The breadcrumb** (`nestedBreadcrumb`, at the right end of the sequence tab bar while the current tab was opened
  from a nested clip): the layers icon in `nestedStripe`, then "Main › Interview Cut" in `textSm`; the parent crumbs
  are text buttons (secondary, accent on hover), the current one `textPrimary` DemiBold; a click goes back to that
  sequence's tab.
- **Reveal Nested Sequence in Library** brings the Library forward and selects the sequence; **Unnest** puts the clips
  back (refused with the model's reason, shown as a toast).

### Audio Track Mixer faders and automation (A.3)
- The **fader** and **pan knob** are live now: they are the track's Volume and Panner (`track.setVolume` /
  `track.setPan`) and the master fader (`sequence.setMasterGain`). A drag is one undo step (merged), Shift drags fine,
  double-click resets to 0 dB / centre. On an automated track, paused, a move sets the key at the playhead.
- **Automation mode** (`AutomationButton`, a compact pill at the foot of each strip, under the gain readout; the
  master's beside its fader): Read, Touch, Latch, Write (Premiere's modes). A dot on the pill: warning in a writing mode, danger while this pass records the strip. Read follows the
  automation; Touch writes while the fader or knob is held and returns to the automation on release; Latch writes from
  the first touch until playback stops; Write writes the whole pass. The pass is written when playback stops: thinned
  keys, anchored one 40 ms gap before and after each written stretch, **one undo step** ("Mixer Automation").
- **Live loudness** (`LiveLoudness`, the master strip, above the analysis card): momentary, short-term and integrated
  LUFS and true peak from the player's loudness tap (BS.1770-4), mono `textMd` readings with small captions ("M ·
  LUFS"), the true peak in danger above the target's ceiling, "0.6 LU below the target" under them, **Reset** (a new
  integration). It polls with the meters, only while the mixer is on screen.
- **Track keyframe lanes:** the Timeline display popover's "Audio track keyframes" (Off / Track volume / Track pan,
  saved in Preferences) draws each audio track's fader (`trackVolumeLane`) or pan (`trackPanLane`) as a line across
  the row with its keys as squares, over the clips.

### Enhance Speech, stems and Synchronize (A.4, A.5; bridge `ClipAudio` = ClipAudioBridge)
- **The clip menu** (audio clips, or video clips with sound): Enhance Speech…, Render Enhancement (only while the
  effect waits for a take), Separate Stems…, Synchronize (two or more clips selected).
- **Enhance Speech…** (`EnhanceSpeechDialog`): one line on what it does (on this computer, nothing uploaded); the
  model's licence card before its first download; **Strength** (0–100 %, live, one undo step per drag); the take
  badge ("Enhanced" in success, "Missing render" in warning) with "Enhanced take ready (rendered once, cached)"; the
  **Bypass** toggle (the effect off: the A/B); a `BeamProgress` while it renders; Enhance / Download and enhance.
- **Separate Stems…** (`StemsDialog`): what it makes (drums, bass, other, vocals as new media in a "<source> stems"
  bin, linked to their source), the licence card, the progress. In the Library a stem shows a "Stem" chip and
  "From <source>"; the list's Generated column says "Separated from <source>".
- **Synchronize** runs `audio.syncClips {apply, link}`: the external audio moves into sync with the camera clip and
  links to it (one undo step); `SyncReportDialog` lists each clip, its offset (mono, signed timecode), confidence and
  drift (a speed proposal when it matters), the reference marked.
- **Badges in the timeline:** the clip label's micro-tag (the AI tag's slot): **ENHANCED** (success) when the take
  plays, **NOT RENDERED** (warning) when the effect waits for a take (none, stale or missing on disk).

## M5m: the caption workshop, review and version history, audio plugins, Essential Sound and Remix, voice-over, Paste Attributes

### Caption workshop (C.7; bridge `Captions` = CaptionsBridge)
- **Where:** the Captions workspace shows the **Caption Style** panel in the side slot (Effect Controls stays in Edit) and
  the Transcript panel's Captions tab in the browser. The style lives in the caption track (the model's `CaptionStyle`),
  so it saves, undoes, exports and burns in with the project.
- **Caption Style panel** (`panels/captions/CaptionStylePanel.qml`), sections with `labelTracking` group labels:
  - **Track:** name, language and the burn-in switch; "On monitor" in the header draws the captions over the program.
  - **Presets:** small chips on `videoBlack` with an "Aa" sample in the preset's fill (its box behind it when it has one),
    the name under; the current one has the accent ring, a project preset an × on hover. "Name this look…" + Save preset.
  - **Text:** font and weight as pills with menus, Size (px on a 1080-line frame), Line spacing, Max width, the three
    alignment buttons, Fill (`CaptionColorRow`: a swatch of the value, its hex in mono, a palette of `contentWhite`,
    `contentInk`, black, `captionHighlight` (the karaoke yellow, a new content token) and the clip colours).
  - **Outline** (width as a share of the size, colour), **Shadow** (opacity first; distance, angle, softness and colour
    appear once it shows), **Background box** (switch; colour, opacity, padding, corner radius), **Position**
    (Bottom / Top / Custom tabs, safe area, centre X / Y for Custom), **Karaoke and speakers** (spoken-word colour,
    speaker names, overlapping captions, animation).
  - A tertiary note says plainly what the monitor cannot draw yet (shadow, alignment, custom position, speaker names,
    animation): saved and exported, drawn once the renderer supports them.
  - Every control is `Captions.setStyle(key, value)`; a slider drag merges into one undo step ("Caption Style"). The
    program monitor redraws as it moves (`captionStyleFrom` maps the model style to the compositor's).
- **Captions tab** (`CaptionsPage` + `panels/captions/CaptionCueRow.qml`):
  - the track pill (its menu lists the tracks, "New caption track"), delete track;
  - Generate / Regenerate (`captions.generateFromTranscript`, with speakers and corrections), characters and lines;
    **Update from transcript** (`captions.updateFromTranscript`, timing kept), primary and with an accent-wash banner
    after a correction on the Transcript tab;
  - Translate… and Dub… (AI-12) under them; "At playhead" adds a cue (two seconds or up to the next cue).
  - **A cue row:** the speaker pill (a dot in the speaker's colour; its menu assigns, clears, adds a speaker or renames
    and recolours one inline), the reading speed (mono "17.4 cps", warning-tinted above the language's comfortable
    speed), go to, split at the playhead (enabled under the playhead), join with the next, delete; start and end in
    mono fields with a frame nudge on each side; the text edited in place. The cue under the playhead has the accent
    wash and the list follows it.
- **Fix… on the Transcript tab** (selection bar): the selected words, editable; Fix runs `transcript.correctWord` /
  `correctRange`, and the Captions tab then suggests Update from transcript.

### Review and version history (C.9; bridges `Review` and `History`)
- **The Review panel** (rail key "review", the message-square icon; `panels/review/`): Notes | Versions as PillTabs.
- **Notes:** All / Open (n) / Resolved (n) tabs, a search field and "All sequences"; the notes in time order as cards
  (`ReviewNoteCard`: a timecode chip that seeks, the author and when, a mark badge, the text, the replies and a reply
  field, resolve and delete; a resolved note is dimmed with a green tag; the note at the playhead or the selected one
  is outlined). The composer at the bottom: the note's text ("Note at 00:00:00:12"), a crosshair toggle to mark the
  frame, Add note (at the playhead, or over In-Out when both are set). Notes to markers, Export… and Import… of a
  review package (a folder with an HTML page with thumbnails for the client, `notes.json` back in).
- **The mark** (`ReviewMarkOverlay`, over the Program Monitor's frame): while marking, a click is a spot (a ring), a
  drag a rectangle, in frame fractions; the selected note's or the playhead's note's mark is drawn in the accent
  (green when resolved).
- **Versions** (`ReviewVersionsPage`): "Name this version…" + Save version; the versions newest first (an icon per kind:
  saved, auto, before an AI edit, before a restore, named; a double-click renames; the size it cost on disk), each with
  Compare (with now, or with another picked version) and a menu: Restore (one undo step; the current project is kept as
  a version first), Restore a sequence as a new one, Export as project…. **Compare** (`VersionCompareView`): "“Client
  cut v1” → Now", the summary lines, per sequence the clip rows (added, removed, changed with what changed), and the
  timeline highlights: the old ranges struck through, the new ones as ghosts (the external preview), dropped by any
  edit.
- **Checkpoints** come from saves, every five minutes of editing, and before the assistant's staged plan can be applied.
  An unsaved project has no history (the page says so).

### Audio plugins (A.6; bridge `Plugins`)
- **Effects panel:** an "Audio Plugins · n" section under the effects (`panels/plugins/PluginBrowser`): By vendor / By
  category tabs, Rescan and settings buttons, a warning line when files were blocked ("2 plugin(s) blocked: they
  crashed or hung while scanned. Review…"), the groups, and rows (`PluginRow`: name, vendor, a CLAP / VST3 tag) that
  drag onto clips, the timeline, Effect Controls and mixer strips like any effect (a plugin drop goes through
  `Plugins.apply`, which stores the plugin's name for the missing badge). The panel's search filters them.
- **Effect Controls:** a plugin effect's header has its badge (`PluginBadge`: MISSING, CRASHED, FAILED, HUNG in danger
  or warning micro-tags; a dot for late blocks on the master) and "Edit plugin…" (external-link icon; disabled when the
  plugin has no editor window). The editor opens in the helper's own window; its state changes are saved into the
  project as one undo step ("Plugin Settings").
- **Mixer strips:** a plugin slot shows the plugin's name and badge; "+ Effect" lists the plugins; a plugin drops on a
  strip.
- **Settings > Audio plugins…** (`AudioPluginsSettings`): what plugins are and how a crash is contained, the folders
  (standard and the user's, Add folder…), Scan all again / Rescan, and the blocklist with the reason and "Try again".

### Essential Sound and Remix (A.7; bridge `EssentialSound`)
- **The Essential Sound panel** (rail key "essential", the sliders icon): "n audio clip(s) selected", the type buttons
  (Dialogue, Music, SFX, Ambience as PillButtons, the current one primary; Clear), the preset menu, then the type's
  sections (`essential/EssentialSection`: a raised card with a title, one line of what it does and a switch that
  bypasses its effects; the controls inside): Loudness (target, Auto-match), Repair (reduce noise, Enhance Speech,
  rumble, hum 50/60, de-ess, room echo), Clarity (EQ preset and amount, dynamics), Creative (reverb preset and amount);
  Music: Duration (Remix…), Ducking, Width; SFX: Position (pan); Ambience: Ducking, Width. Every control is an option of
  `audio.essentialSound`; a slider applies once when let go (one undo step); every result is an ordinary effect in
  Effect Controls.
- **Remix** (`essential/RemixPopover` + `RemixSummary`, also in the Duration section): the target and the result (with
  the error), the joins with how seamless each is (green from 80 %, amber from 35 %, red below), the warnings, Apply /
  Cancel and a menu (Fit to video, Render as take). On the timeline the preview draws a marker at each join, the new
  length as a ghost and the dropped tail struck through.

### Timeline: the Remix tool
- The Tracks toolbar's last tool (music icon, Premiere's Remix tool, no default key). Dragging an audio clip's end
  draws the proposed length as a marquee-styled ghost from the clip's start to the pointer (at least a second, on
  frames); Esc cancels. Release asks Essential Sound for a remix preview (a dry run) and opens the Remix popover at
  the pointer; the model changes only on Apply.

### Audio Track Mixer additions
- **"+ Effect"** lists Enhance Speech and the A.7 effects (Filter, DeHum, De-ess, Expander, Reverb, Width) beside Gain,
  EQ, Compressor, Limiter and Reduce Noise. Enhance Speech is a clip effect (a rendered take of each file): picked on a
  track it opens the Enhance Speech dialog for that track's clips; on the master it says so.
- **Automation passes are heard while they record:** the strips' live fader and pan go to the player's live mix
  override (`TimelineAudioPlayer::setLiveTrackMix`, A.8: ramped, the model untouched) while Touch, Latch or Write
  writes them, and back to the automation when they stop; the pass is written as keys at stop.

### Voice-over recording (A.8; bridge `VoiceOver`)
- **Record arm** (`audio/RecordArmButton`): the track header's microphone on audio rows (between Solo and Lock) and the
  mixer strip's "R" beside M and S (it replaces the disabled placeholder). Armed = filled `danger`; a click is
  `track.setRecordArm` (one undo step; a locked track can be armed, a video track refuses in the tooltip). The armed
  state is the model's `Track::recordArmed` (`AudioMixer.strips[].recordArmed`). While a pass records onto the track a
  `danger` ring pulses round the button (steady with Reduce motion) and a click stops and keeps the take.
- **Starting a pass:** the Program Monitor's microphone button (after Extract; `danger` while recording) and Record
  Voice-over (`record.toggle`; `record.start` / `record.stop` also run). A pass records onto the first armed audio
  track.
- **The overlay** (`panels/VoiceOverOverlay`, over the Program Monitor's well): the count-in is a big 3, 2, 1 in a
  `danger` ring on the `scrim` with "Recording onto A2 in"; then a badge at the top left (dot, "Recording", m:ss.t,
  the track chip, the input meter), dimmed during pre-roll and post-roll, and a `danger` frame round the well while
  the take records. Hidden when idle. Esc cancels the pass; Space stops it and keeps the take.
- **Input meter** (`audio/InputMeter`): RMS bar (`success`, `warning` from -18 dB, `danger` from -6 dB), a peak tick
  and a clip light that stays lit until clicked; vertical or horizontal, optional dB scale.
- **Takes** (`panels/TakesSubmenu`, in the clip's context menu): "Take n · 1.6 s · date · device", newest first, the
  take heard checked; choosing one is `record.useTake` (one undo step). Disabled on clips that are not recorded takes.
- **Settings > Audio** (`dialogs/AudioSettings`; Settings > Audio > Voice-over "Open…"): the microphone (System default or a
  device, refresh), Test input with its meter (the microphone is open only while it is on), count-in 0-10 s and its
  beep, pre-roll, post-roll, input monitoring and its gain, and the latency (input, round trip, the calibration typed
  or measured by a loopback pass: "Measure"). Saved under `voiceOver/*` in Preferences; `VoiceOverSync` (Main) hands
  them to the bridge at start-up and saves later changes.
- **Top bar:** a `danger`-dot status pill ("Microphone" / "Recording") while the microphone is open.
- **Clip menu:** a recorded take's clip gets the **Takes** submenu after the audio tools; a submenu row in any
  `ContextMenu` ends in a `chevron-right` (`ContextMenuItem`).

### Clipboard, Paste Attributes and Remove Attributes (C.11; bridge `Clipboard`)
- **Cut, Copy, Paste, Paste Insert** go through the model's clipboard (`clipboard.*`, one undo step each), so a paste
  keeps speed, reverse, freeze, time remapping, effects and keyframes, transitions and markers among the copied
  clips, and finds or adds the media (what it could not keep is a toast). Paste selects the pasted clips and moves the
  playhead after them (the view follows). The copy is also on the system clipboard (`application/x-wabcut-clipboard`),
  so it pastes in another Wabcut window or project; the assistant shares the same clipboard.
- **Clip menu:** Cut, Copy, Paste, Paste Insert, Paste Attributes…, Remove Attributes… (with their shortcuts:
  Ctrl+Alt+V for Paste Attributes, Ctrl+Shift+V for Paste Insert; Remove Attributes has no default key, as in
  Premiere).
- **The attributes dialog** (`dialogs/AttributesDialog`, Edit > Paste Attributes… / Remove Attributes…): two columns of
  `Toggle`s, Video Attributes (Motion, Opacity, Time Remapping, each video effect by name) and Audio Attributes
  (Volume, Channel Volume / Panner, each audio effect), then Scale Attribute Times (paste) or All effects (remove).
  Under them a live preview on `sunken`: each selected clip, what it takes or loses (`success`) and what it skips with
  the reason (`textTertiary`). OK is one undo step; a refusal shows in `danger` under the preview.

## M5n: extensions, audio channels and 5.1, startup, accessibility

### Extensions (C.12; bridge `Extensions`)
- **The manager** (`dialogs/ExtensionsManager`; Settings > Extensions…, the palette's Manage Extensions…): one
  `panels/extensions/ExtensionRow` card per extension on `sunken` with a hairline (a `danger` border when it is in error
  or broken):
  - the puzzle icon, name and version (mono);
  - micro-tags: `Project` (`info` outline) for one that came with the project, `Crashed ×n` (`danger`), and the status
    (On `success`, Review needed / Not trusted `warning`, Off `textTertiary`, Error / Broken `danger`);
  - author · licence and the description;
  - the granted capabilities as `accentWash` chips, "Asks for more" (`warning` outline) when an update asks for more;
  - its commands with their shortcuts, and why it is off or broken;
  - the actions its status allows (Review…, Trust…, Enable… / Retry…, Disable, Revoke, Stop trusting, Uninstall with an
    inline confirm) and the log (mono `textXs` on `canvas`, errors in `danger`).
  The manager also has Rescan, Install from folder… (the permission dialog first) and the folder installed extensions
  live in.
- **The permission dialog** (`dialogs/ExtensionPermissionDialog`): "Allow **name** to:", one `sunken` row per
  capability (bold title, one sentence, a `Toggle` on by default; the border turns `accentStroke` while on), notes in an
  `accentWash` box, then author · licence · version and the folder (mono). Cancel / Allow (Install and allow when
  installing). A project extension not trusted yet adds a `warning`-outlined box saying where its code comes from;
  the buttons become Don't trust / Trust and enable.
- **Forms** (`dialogs/ExtensionPromptDialog`; `panels/extensions/ExtensionPromptHost` keeps one per open form):
  non-modal, "<extension>: <title>", the message above the fields; `FieldInput`, a `sunken` text area, a read-only
  mono area with Copy, `NumberField`, `Toggle`, `PillButton` + `OptionsMenu` for choices. Values outside the form's
  bounds keep it open with a `danger` line.
- **Notifications** are toasts with the extension's name as the source; a crash is a `danger` toast, and three in a
  row turn the extension off (the toast says so; Enable is on its row).
- **AI-model commands** an extension runs (C.14 `runLongCommand`) are command jobs: a copy of the project on a worker,
  with the extension's own grants; the top bar shows a status pill (the extension, its step and percent; `accentStroke`
  dot) with a Cancel button while one runs, and the result is one undo step named after the extension. A project
  edited meanwhile is not overwritten: the extension is told to run it again.
- **Palette and shortcuts:** an "Extensions" group in the palette (each command, plus Manage Extensions…). In Keyboard
  Shortcuts the commands are under Extensions with no default key; a bound command whose extension is off or gone
  stays as "(unavailable)" and keeps its key.

### Audio channels and 5.1 (A.9; bridge `AudioChannels`)
- **Modify > Audio Channels…** (`dialogs/AudioChannelsDialog`; the clip menu's Audio Channels, Channel Volume's header,
  the Library's item menu): the clip format as PillTabs (Mono, Stereo, 5.1, Adaptive with a channel count), preset
  pills (Fill Left with Right, Fill Right with Left, Swap Channels, Sum to mono, Default; Split to Mono for one clip),
  then the grid on `sunken`: a row per clip channel (L R, L R C LFE Ls Rs, Mono, 1..n), a column per source channel, a
  check cell (`accentFill`, check icon) where a source feeds the channel (several are averaged) and a Mute column
  (`danger`). "Measure levels" puts each source channel's RMS under its label and marks the silent ones `warning`. OK
  is one undo step; every clip keeps its channels' volumes.
- **Clip menu > Audio Channels** (clips with sound): Audio Channels…, Fill Left with Right, Fill Right with Left, Swap
  Channels, Split to Mono.
- **Channel Volume** in Effect Controls (`audio/ChannelVolumeSection`, between the effects and Masks): a row per clip
  channel with the stopwatch, a dB PropSlider (-60..+24, filled from 0) and the key at the playhead; a drag is one undo
  step; a muted channel is dimmed.
- **Track format:** an audio track that is not stereo shows a mono-font badge on its name chip's corner (1.0 mono, 5.1,
  1:1 adaptive; `raised`, `borderStrong`); right-click the header, the badge, or Menu / Shift+F10 on the chip for the
  format menu (`TrackHeaderMenu`: checkable Mono, Stereo, 5.1, Adaptive).
- **5.1 master** (the master strip's Format tabs, Stereo | 5.1): mono and stereo strips show the 5.1 panner instead of
  the pan knob (`audio/SurroundPanner`: a `sunken` square with the listener's circle, the L C R Ls Rs labels and the
  `accentFill` puck; drag, arrow keys, double-click for the front centre; the LFE send below, off under -60 dB). 5.1
  and adaptive tracks keep a disabled knob. The master meter shows six bars.
- **Timeline:** a clip that does not play its stream as it is carries a micro-tag after its name (FILL L, FILL R, SWAP,
  MONO, CH n, SUM, 5.1, MUTED, CH VOL, MAP), drawn in the scene graph, after ENHANCED when both apply.
- **Export settings:** Audio channels (As the master, Mono, Stereo, 5.1) and Audio codec (AAC, AC-3; not for WAV); the
  summary line names the channel layout.

### Startup and editing performance (M5n)
- **Lazy panels** (`components/LazyPanel`, a Loader): the browser tabs other than the Library (Effects, Transcript,
  Audio Tools, Markers, Generate, Shorts, Takes, Essential Sound, Review), Scopes, and the side slot's panels of the
  other workspaces (Caption Style, Explain this frame, Audio Track Mixer, Export, Color) are created when first shown, or
  in the background after the window's first frame (incubated between frames; a 2 s fallback when no frame comes),
  and then kept: switching back is instant and keeps their state. The Library, the monitors, Effect Controls and the
  timeline are created up front. An engine without an incubation controller (a plain QQmlEngine: the tests, the e2e
  suites) gets every panel up front, as before. `window.panelsReady` says when all are there.
- **Timings:** `UiProfile` (WABCUT_UI_PROFILE prints them): "Main.qml completed", "first frame", "panels ready" and a
  span per lazily created panel; the start itself is P.1's StartupProbe (WABCUT_PERF_STARTUP).
- **Per edit:** listeners refresh only for what a commit changed (`ProjectController::lastChange()`: the ChangeSet, its
  sequence and the snapshot it started from): Markers on marker / timecode / scoped-clip changes (the playhead re-lists
  only at marker edges), the transcript map from the ChangeSet, Effect Controls / Color / Masks when their clip (or the
  clip under the playhead) changed, the selection pruned of removed clips only, the clip list's index per changed track,
  the timeline's lane facts per changed clip, and elided clip labels cached by (name, 8 px width) until the label style
  changes.
- **Measured** (quiet window, 2026-10-13, one binary; startup by P.1's StartupProbe, eager = `WABCUT_EAGER_PANELS=1`
  = every panel up front as before):

  | | before | after |
  |---|---|---|
  | warm start to an interactive window | 1,024 ms | 719 ms |
  | cold start | 1,072 ms | 715 ms |
  | warm: Main.qml loaded / first frame | 865 / 1,011 ms | 684 / 781 ms |
  | an edit on 10,000 clips, the whole window (p50 move / trim / undo) | 12.0 / 11.3 / 10.8 ms | 4.7 / 5.0 / 5.2 ms |
  | the timeline's scene-graph update after an edit / a zoom / a jump scroll (p50) | 1.17 / 3.28 / 3.91 ms | 0.22 / 0.57 / 1.15 ms |

  The per-edit "before" was taken before the gating on a busy machine (`tests/app/test_perf_ui.cpp`, `[perf]`).

### Accessibility (M5n)
- Every `Panel` is a named pane (its title). Focusable items that are not buttons (caption presets, mask rows, the
  Library list) are Tab stops with a `FocusRing`, act on Space / Enter and a screen reader's press action, and claim
  their keys with `Keys.onShortcutOverride` so the keymap does not turn Space into Play there.
- Mouse-only gestures have a keyboard or menu equivalent: rename a version from its More menu, "Compare version N with
  this one" buttons, the Library's Sort by submenu, the mixer's clip light and the track format badge as buttons.
- `tests/app/A11yAudit.h` (`auditAccessibility`, `tabChain`, `tabReaches`) checks names, roles and Tab reachability per
  panel; the M5n suites run it over every panel added since M5j.

## M5o: gaming videos - New gaming video, Highlights, layouts, callouts, Moments, gaming export

Wabcut's first target is a finished Windows editor for gaming videos (OBS / ShadowPlay / Game Bar recordings to
YouTube videos, highlight reels, montages and Shorts / TikTok / Reels). M5o is that UX on the wave's engine work;
everything below uses the v2 tokens and components ("Ultraviolet Cut"). Not in this milestone: a brand kit editor
(the `Brand` singleton is registered so the caption pickers' brand swatches work, with no UI entry), colour
management UI, and deep layer editing of motion graphics (kept behind "Advanced").

### New gaming video (Home; bridge `GamingStart`)
- **The card** (`home/GamingVideoCard`, first in Home's right column, the `gamepad-2` icon): drop a recording
  (anywhere on Home: a single video goes to this card) or choose one. The probe runs off the GUI thread; the card shows
  size, frame rate (the nominal one, kept: 60 stays 60, 59.94 is named), duration and codec, with pill badges tinted
  by tone (4K and High fps `accentStroke`, Long · proxies `warning`, VFR `textSecondary`, HDR · tone-mapped `info`)
  and a one-line note for each that changes what happens.
- **One row per audio stream:** a `Toggle` (include it), the track name (Game / Mic / Discord suggested in order,
  then "Audio n"; editable), the channel layout and the track it lands on (A1, A2... renumbered as rows are left out).
- **Create** (primary) names the project after the recording, in the chosen folder, after the unsaved-changes prompt:
  a project at the recording's size and rate, the recording placed at 0 with each included stream on its named track,
  proxies started for long / 4K / high-fps files, then saved. A failure is a `danger` line on the card; Home stays.
- Recent projects stay below, as before.

### Highlights (rail "Highlights", `zap`; bridge `Highlights`; G.2)
- **Header:** Find highlights (primary) with an options popover (signals: game audio, your voice, on screen, chat,
  stream markers; how many; how strict; the chat log: Import chat… runs `highlights.importChat` and shows "Twitch chat
  · 18,234 messages · Gameplay.mkv" plus its warnings). Find runs as a job: a progress bar with the step ("measuring
  'VOD.mkv' 42%") and Cancel; its markers are one undo step.
- **Moment cards** (`panels/highlights/HighlightCard`, one column for the narrow slot): the frame at the peak (decoded
  off the GUI thread, cached, redactions applied) with the rank, title, range and length, the score and a confidence
  chip, a 32-bar waveform of the range with the peak, the signals as bars and up to three reasons (more on expand).
  Accept / Reject / Jump / Preview. Accepted cards carry a `success` strip; rejected ones are dimmed and listed last.
- **Footer:** Make highlight reel (accepted moments, else every one not rejected, in time order; order, length,
  transition, music, slow motion) and Make a Short (the chosen moment, vertical); both open the new sequence.
- **Keyboard:** Tab to the list; Up / Down / Home / End between cards, A accept, R reject, Enter jump, Space preview
  (claimed with `Keys.onShortcutOverride`).
- Until G.2 is in the catalog the panel says highlights arrive with the next engine update.

### Layouts (bridge `Layouts`; C.15 `layout.apply`)
- **Where:** the clip menu's Layout… and the Timeline tracks header's layout button open the picker beside the Program
  Monitor (`panels/layout/LayoutPicker`).
- **Three cards with live schematics:** Facecam corner (picture-in-picture), Vertical split (gameplay and facecam
  stacked, 9:16), Vertical blur fill.
- **The facecam:** a clip above the gameplay (auto-picked) or a region drawn over the webcam inside the capture,
  right on the monitor.
- **The chosen layout's options:** corner, size, margin, circle, corner radius, border width and colour (`Swatch`es:
  white, ink and the clip colours), shadow, split, facecam on top, blur, zoom.
- **Checking and applying:** every change is checked by a dry run off the GUI thread (its refusal is shown before
  Apply). Apply is one undo step of plain, editable clip settings.
- **The monitor** shows the layout's schematic while the picker is open (`LayoutMonitorOverlay`; over the picture
  only the facecam box is labelled). Esc or Cancel closes it.

### Callouts (rail "Callouts"; bridges `Graphics`, `GraphicCanvas`, `GraphicsImport`; C.13 / C.16)
- **Browse:** Gaming (C.16's built-in callouts from `template.list`, category "gaming": subscribe, kill streak, lower
  third, arrow / circle callouts and more; read-only), Stickers (Lottie animations kept as project templates, with
  "Import Lottie sticker…"), This project and My templates, as cards with live thumbnails painted off the GUI thread
  ("4.0 s · 3 fields"). Drag one onto the timeline, or press Enter / double-click to add it at the playhead; its first
  text field's layer is selected, ready to type. Until the built-ins are in the catalog the Gaming group says
  "Gaming templates arrive with the next update" (no stand-ins).
- **Edit** (a graphic clip selected): the template's fields, the selected layer's text, font, size and colour; layers,
  their properties, animation presets, pins and Responsive Design behind an "Advanced" disclosure.
- **On the Program Monitor** (`GraphicCanvasOverlay`): the selected layer's box and handles (move, scale, rotate,
  anchor; paragraph text box edges), snapping guides (`accentStroke`; Ctrl does not snap), double-click or Enter to
  type in place (`GraphicTextEditor`, Thai input included); arrows nudge (Shift: 10 px), Tab picks the next layer.
- **Lottie** (File > Import Lottie…, or drop a .json): imported on a worker as a graphic clip at the playhead (one undo
  step), then the import report (`ImportReportDialog`: carried, approximated, dropped, each with its JSON path).

### Moments (Timeline tracks header `zap` button, the clip menu's Moments submenu; bridge `Moments`)
- **The popover:** Slow motion 25 / 50 % for 0.5 / 1 / 2 s, "Slow-mo this moment" (primary), Speed ramp in,
  Freeze + replay, Fast forward 200 / 300 / 400 %, Punch in, Punch in + hold and Shake.
- **What a preset acts on:** the selected clip under the playhead, else the topmost video clip there. An audio clip
  hands over to its linked video; locked tracks are refused.
- **The result:** each preset is one undo step of ordinary time-remap keys or Motion keyframes (the speed line and
  Effect Controls edit them afterwards). The speed presets ripple later clips, and linked audio follows the picture.
- **Keyboard:** Slow-mo at Playhead and Punch-in at Playhead are keymap commands with no default key
  ([keymap.md](keymap.md) footnote 20); an assigned key shows in the menu and the popover.
- **C.16's gaming edits** (a "Replay and versions" group in the popover; Instant Replay and Make Shorts Version in the
  submenu), shown only when the catalog has the commands:
  - Instant replay (`clip.instantReplay`): the clip again right after itself at half speed, with a ramp, the REPLAY
    stinger and white flashes. One undo step; later material moves.
  - Make Shorts / square version (`sequence.makeVersion`): a 1080x1920 or 1080x1080 copy of the sequence, which then
    opens.
  - Remove dead time (`sequence.removeDeadTime`): Find dead time measures the gameplay's motion and the game audio
    off the GUI thread (the app's `appGamingServices`: editor::activityMeasure). The popover then says how much it
    found ("Dead time: 3 stretches, 0:42 in all"), and Remove takes exactly those stretches out (handles kept, every
    unlocked track) in one undo step.
  - The assistant's commands get the same host services (media activity, Auto Reframe for versions without a facecam).

### The Export panel and its gaming presets (bridge `GamingExport`; design language v3)
- **Layout** (`panels/ExportPanel`): filter chips on top (All, each destination in its hue, General; the current chip
  inverted), then a grid of preset tiles: "For gaming" first, then "General" (the built-in presets), each under a small
  caps label with its count. Below the tiles: the chosen preset's card (name, format, file size and export time, what
  to know), the settings drawer (a section card; labels beside the rows, above them in a narrow slot) and "Save to".
  Pinned at the foot: the chosen preset with "≈ size · time to export" and the Export pill, then the queue. A slot
  760 px wide or more puts the tiles on the left and the rest in a column on the right.
- **Gaming groups:** YouTube (1080p60, 1440p60, 4K60, pink), Shorts / TikTok / Reels (1080x1920, mint), Twitch
  (1080p60, violet), Discord size targets (10 / 25 / 50 MB, blue), GIF and WebM (lime). GIF and WebM are disabled
  with "Coming soon" until the exporter has those formats. The built-in presets are orange.
- **Each tile** (`panels/export/GamingPresetCard`, also behind `PresetCard`) has the destination's glyph in a circle of
  its hue, the name, what it makes (two lines at most) and the estimate for the range being exported: the size as a
  mono pill and the export time beside a timer. The selected tile is the accent fill with black text. The size is
  bitrate x length; the time comes from P.1's encoder benchmark. Tiles lay out with anchors, so nothing outgrows them.
- **Frame rates** are capped at 60, never raised. A Discord target fits 95 % of its size; a range too long for it says
  how long fits.
- **Keyboard:** the chips are one Tab stop (Left / Right pick a filter), and so is the gaming grid (the chosen tile,
  else the first shown). The arrows and Home / End move between the tiles shown, Space / Enter choose; the keys are
  claimed from the editor's shortcuts.

### Onboarding and accessibility (M5o)
- What's new leads with the six gaming highlights (Made for gaming videos, Find the highlights, Facecam layouts,
  Callouts and stickers, Slow-mo and punch-ins, Export for every platform).
- Every new panel is a named pane with named controls and passes `A11yAudit` (`auditAccessibility`, `tabReaches`) in
  its window test: the gaming card, Highlights (and its options popover), the layout picker, the Moments popover, the
  callouts browser and the gaming presets.
