# The Source and Program Monitors

The monitors work like Premiere Pro's (26.x): same names, keys and behaviour, Wabcut's own look. This page covers what
0.2 added (P.2): moving, scaling and rotating clips in the Program Monitor, Loop, full screen and a second display,
rulers and guides, the Transparency Grid, the view settings and Export Frame. Transport, marks, safe margins and the
playback resolution menu are described in [getting-started.md](getting-started.md).

Code: `src/app/qml/monitor/` (QML), the `Monitors` (`MonitorBridge`) and `ProgramMotion` (`MotionBridge`) singletons in
`src/app/src/bridge/`, `edit::setMotion` (`src/model/include/wabcut/model/MotionOps.h`), the playback loop and mirror
(`PlaybackController::setLoop` / `addMirror`), and `exporter::writeStill` (`src/export/include/wabcut/export/StillExport.h`).

## Type a time, drop a clip, read a marker

- **Type a time to go there** (as in Premiere): click the timecode under either monitor, or the Timeline's timecode
  left of its ruler (or Tab to it and type). The field opens with the time selected. Type a timecode
  (`00:01:02:03`, `1:02:03`; `:` `;` `.` `,` all work as separators), digits packed from the right (`110` is
  00:00:01:10), `2.` for two seconds, or an offset from where you are: `+15` goes 15 frames on, `-1:00` a second back.
  A hint above the field says where Enter goes; Esc (or a click elsewhere) cancels; a time Wabcut cannot read keeps
  the field open and says what it takes. The Source Monitor's timecode moves the source, in its own frames.
- **Type on the numpad** (0.4, as in Premiere): with the Timeline, the Program Monitor or the Source Monitor focused, a
  numpad digit, `+`, `-` or `.` opens that panel's timecode field with the key already typed: `+15` Enter moves 15
  frames on, `1215` Enter goes to 00:00:12:15, Esc cancels and the focus goes back. The digits above the letters keep
  their keymap commands, and a numpad key you bind in Keyboard Shortcuts runs its command instead. The Source Monitor's
  field opens only with a source loaded.
- **In to Out length** (0.4): with both sequence marks set, a raised pill right after the Program Monitor's timecode
  shows the In to Out length, the Out frame included (In at 1:00 and Out on 3:12 read `00:00:02:13`), in the sequence's
  time format. It stays when the monitor is narrow (the "/ duration" text goes first, then the readouts on the right:
  the safe margins toggle, the zoom unless zoomed in, and last the playback resolution button; the wrench menu has Safe
  Margins and Playback Resolution too) and follows I, O, X, the clears, undo and sequence switches. The Fit Clip dialog
  reads the same length.
- **Drop a clip on the Source Monitor**: drag a clip from the Library onto its picture to open it there (it joins the
  recent sources). Files dragged from File Explorer are imported first, as File > Import does, then the first one
  opens. Sequences cannot open in the Source Monitor.
- **Markers on the time bar**: point at a marker on the Program Monitor's time bar to read its name, comment, time and
  length.

Code: `src/app/qml/components/TimecodeEntry.qml` (`ProjectController.seekToText`, `SourceMonitor.seekToText`),
the drop area in `src/app/qml/panels/SourceMonitorPanel.qml` (`SourceMonitor.load` / `loadFileItem`) and
`SequenceTimeBar.markerAt` in `src/app/qml/panels/ProgramTimeBar.qml`.

## Move, scale and rotate in the Program Monitor

Premiere's direct manipulation of the Motion effect
([Apply the Motion effect](https://helpx.adobe.com/premiere/desktop/add-video-effects/commonly-used-effects/apply-motion-effect.html)).

- **Start**: click the direct-manipulation button on **Motion** (Transform) in Effect Controls, or **double-click the
  clip's picture** in the Program Monitor. The topmost visible clip under the pointer is selected. A bounding box with
  eight handles, the anchor-point crosshair and (when Position is keyframed) the motion path appear while the playhead
  is over the clip.
- **Gestures**:
  - drag inside the box: **Position**. Shift keeps the move horizontal or vertical.
  - drag a corner or an edge handle: **Scale**, around the anchor point. Wabcut's Motion has one Scale, so every handle
    scales uniformly; Premiere's Scale Width with Uniform Scale off has no counterpart yet.
  - drag just outside a corner: **Rotation** around the anchor point. Shift rounds to 45-degree steps.
  - drag the anchor crosshair: **Anchor Point**, with Position following so the picture stays where it is. Alt moves
    the anchor point alone (the picture moves), as in Premiere.
  - drag a dot of the motion path: that keyframe's Position.
- **Keys**: Ctrl+arrows nudge by a pixel, Ctrl+Shift+arrows by five (Premiere's Nudge Selected Object; they nudge the
  selected graphic layer when no clip's Motion is selected). Esc cancels a drag, else lets go of Motion. Selecting another
  clip, or a press outside the box, lets go too.
- **Keyframes**: a gesture on a parameter whose stopwatch is on sets (or adds) its keyframe at the playhead, a constant
  parameter changes everywhere, as in Effect Controls. Each gesture is one undo step.
- **Snapping**: with **Snap in Program Monitor** on (Ctrl+Shift+;) the box's edges and centre snap to the frame's edges
  and centre and to the visible guides, showing a line where they snap. Ctrl turns snapping off during a drag.
- **Commands**: `clip.setMotion` sets Position, Scale, Rotation and Anchor Point together at a time (one undo step,
  keyed where animated): what agents, scripts and `wabcut-cli` use.

**Crop** (Premiere 26's crop direct manipulation): the crop button on a Crop effect in Effect Controls, or **Toggle Crop
Direct Manipulation** (`monitor.toggleCropDirectManipulation`, no default key), shows the cropped picture's box; its
edge and corner handles set the effect's Left, Top, Right and Bottom (keyed like Motion, one undo step per drag). The
clip needs a Crop effect (Effects > Transform > Crop).

## Loop and Play In to Out

- The **Loop** button (both monitors' transports, and the settings menus; `monitor.loop`, no default key as in
  Premiere's key sets) makes playback repeat **In to Out**: the sequence's marks in the Program Monitor, the clip's in
  the Source Monitor. A missing mark is the start or the end, so without marks the whole sequence (or clip) loops.
  Playing from outside the range starts at its In. It is remembered per monitor.
- **Play In to Out** (Ctrl+Shift+Space, now in the Program Monitor too) plays once from In and stops on the last marked
  frame, or loops with Loop on.
- **Play Around** (Shift+K, 0.4) plays from the playhead minus the pre-roll to the playhead plus the post-roll once,
  then puts the playhead back where it was (Settings > Timeline & editing > "Play around the edit", 2 s each by
  default). With an edit point selected it is Trim's play-around, as in Premiere's trim mode; with the Source Monitor
  focused it plays around the source position. Space, K, J, L or a seek stop it where it is. Loop is ignored.
- **Play from Playhead to Out** (Ctrl+Space, 0.4) plays from the playhead to the Out mark (no Out: the end) and stops on
  the last marked frame; at or past the Out it does nothing. With the Source Monitor focused it plays to the source's
  Out. Loop is ignored. On macOS, Control+Space is the system's input-source switch when more than one input source is
  on: change one of the two keys then.
- **Slow shuttle** (Shift+J / Shift+L, 0.4): a press plays at 1/4x that way and keeps playing after you let go; the next
  press the same way steps to 1/2x, then 1x. K, Space, J or L end or change it as usual. The speed pill shows the speed
  (`▶ ¼×`). Premiere's first slow step is about 1/10x; Wabcut starts at its K+L slow speed.
- Sound and picture wrap together: the audio engine's transport wraps (`TimelineAudioPlayer::setLoop`), the video
  scheduler decodes no further than the loop's end and drops what it queued past it.

## Full screen and a second display

- **Toggle Full Screen** (Ctrl+`, `monitor.toggleFullScreen`) shows the program full screen on the display the window
  is on. There Space plays, J/K/L shuttle, Left/Right step, and Esc, Ctrl+` or a double-click return.
- **Enable Transmit** (`monitor.enableTransmit`; Premiere's Mercury Transmit to a display) keeps the program full
  screen on the display chosen under **Transmit To** in the Program Monitor's settings menu, without taking the
  keyboard focus. **Disable Video Output When in the Background** (on by default, as in Premiere) hides it while
  Wabcut is not the active application. Both are remembered.
- The output is a separate top-level window whose monitor is fed by a playback **mirror**: the same transport, edits,
  loop, captions and colour views, decoded on its own in CPU frames (the window's GPU device is not the one the
  decoders share), so the main Program Monitor keeps playing too. It costs a second decode of the visible layers while
  it is open. Video cards (AJA, Blackmagic) are not supported.

## Rulers and guides

Premiere's Program Monitor View commands
([Add or remove guides](https://helpx.adobe.com/premiere/desktop/get-started/source-and-program-monitor-adjustments/add-or-remove-guides.html),
[Save, export and import custom guides](https://helpx.adobe.com/premiere/desktop/get-started/source-and-program-monitor-adjustments/save-export-and-import-custom-guides.html)),
in the Program Monitor's settings menu (the gear):

- **Show Rulers** (Ctrl+R): rulers along the top and the left count the frame's pixels from its top-left corner at the
  current zoom; right-click a ruler for **Pixels** or **Percentage**.
- **Show Guides** (Ctrl+;), **Lock Guides** (Ctrl+Alt+Shift+R), **Add Guide…** (orientation, position in pixels or
  percent, colour), **Clear Guides**. Drag from a ruler to add a guide, drag a guide to move it, out of the frame to
  remove it, double-click it to edit it. Pixel guides snap to whole pixels.
- **Guide Templates**: choose one to replace the guides with it; **Save Guides as Template…** names the current set;
  **Manage Guides…** applies, deletes, exports (`.guides`, Wabcut's JSON) and imports templates.
- Like Premiere's, guides and templates belong to you, not to a project: they persist across projects and sessions.
  Pixel guides measure the current sequence's frame, percent guides follow any frame size.
- **Snap in Program Monitor** (Ctrl+Shift+;): see Motion above.

## Transparency Grid

Each monitor's settings menu has **Transparency Grid**: a white and grey checkerboard where the picture is transparent
(no clip, transparent video, a graphic's empty areas). The monitor then keeps the frame's alpha instead of compositing it
over black. Off by default, remembered per monitor.

## View settings

In the Program Monitor:

- **Zoom**: Fit, or Premiere's levels from 10 % to 1600 % of the sequence's pixels. A zoomed picture that overflows the
  monitor scrolls with the mouse wheel (Shift+wheel sideways), like Premiere's scroll bars.
- **Show Dropped Frame Indicator**: a light in the picture's corner, green while playback keeps up, amber once frames
  were dropped since play started (the tooltip counts them). The readout under the picture keeps the fps and the count.
- **Overlays**: the source timecode of the topmost clip at the playhead (with its name) and the sequence timecode over
  the bottom of the picture (Premiere's default overlays). Not in exports.
- **Show Audio Time Units**: the timecode readout counts audio samples instead of frames (hh:mm:ss:samples at the
  sequence's audio rate, or hh:mm:ss.milliseconds with the sequence's Milliseconds audio display). 0.2: the same switch
  as the timeline's (its display settings): the playhead then moves by samples and the timeline edits audio-only
  material by samples (docs/audio.md).

All are off by default and remembered. Playback resolution (Auto, Full, 1/2, 1/4) and proxies are in the quality menu
next to the zoom and (0.4) in the wrench menu's **Playback Resolution**, as in Premiere; the quality button gives way
on a narrow monitor. Premiere's separate Paused Resolution and High Quality Playback have no counterpart yet (one
resolution serves both, and Full is the high quality setting).

## Platform safe zones (Shorts, TikTok, Reels)

On a vertical sequence (taller than wide), the Program Monitor's wrench > **Platform Safe Zones** shades where YouTube
Shorts, TikTok or Instagram Reels put their own buttons and text over the video: the top bar, the right-hand action
column and the bottom caption and description band, hatched on a light scrim with a label ("Shorts UI"). Every line is
two-tone, dark under light, so the bands show on bright footage as well as on dark. Keep titles,
faces and captions out of the shaded parts. Choose **Off** (the default), **YouTube Shorts**, **TikTok** or **Instagram
Reels**; the choice is remembered. On landscape and square sequences nothing shows, and the menu says "Shown on vertical
sequences". The bands are approximate public guides (fractions of the frame):

| App | Top bar | Bottom band | Right column |
|---|---|---|---|
| YouTube Shorts | top 8 % | bottom 21 %, left 86 % of the width | right 14 %, from 45 % to 79 % down |
| TikTok | top 7 % | bottom 20 %, left 86 % | right 14 %, from 40 % to 80 % down |
| Instagram Reels | top 10 % | bottom 20 %, left 86 % | right 14 %, from 50 % to 80 % down |

The Shorts builder's caption line (68 % down) sits above every bottom band. The overlay is in the monitor only (never in
exports or Export Frame), takes no clicks, and hides while the Trim view or the multi-camera view shows. Premiere has no
such overlay.

Code: `src/app/qml/panels/PlatformSafeZoneOverlay.qml` (the bands table), `src/app/qml/monitor/MonitorSettingsMenu.qml`,
`Preferences.platformSafeZone` (`monitor/platformSafeZone`).

## Transport buttons

Both monitors' button rows always fit their panel (0.4). When a monitor is too narrow, the least-used buttons hide first
and a **···** button (More controls) at the row's end lists exactly the hidden ones, with the same icon, name, shortcut
and action; Loop, Redact and Multi-Camera View show their check there. The order on screen never changes.

- Program Monitor, always shown: Add Marker, Play, Lift, Extract, Record Voice-over. Then, as room allows: Mark In and
  Mark Out, Go to In and Go to Out, the frame steps, Play In to Out, Loop, Export Frame, Multi-Camera View.
- Source Monitor, always shown: Mark In, Mark Out, Play, Insert, Overwrite. Then: Go to In and Go to Out, Play In to
  Out, the frame steps, Loop, Export Frame, Redact, Redactions (its list then opens from the ··· button), Settings (a
  submenu in the ··· menu).

Premiere's Button Editor (dragging buttons on and off the bar) is not in Wabcut yet.

Code: `src/app/qml/monitor/TransportOverflowButton.qml`, the `transport` rows of `MonitorPanel.qml` and
`SourceMonitorPanel.qml`.

## Export Frame

The camera button in both monitors, or Ctrl+Shift+E (Shift+E on macOS), opens **Export Frame**
([Export a still frame](https://helpx.adobe.com/premiere/desktop/render-and-export/export-files/export-a-still-frame.html)):

- **Name**: the sequence's (or clip's) name and the timecode. **Format**: DPX (10-bit), JPEG, PNG, TIFF (LZW), Targa,
  BMP, GIF (a fixed palette of 252 colours, dithered) or OpenEXR (half float, linear light, uncompressed). **Path**: the folder (the
  project's at first, then the last one used). **Import into project** adds the still to the project (one undo step).
- The Program Monitor exports the sequence frame at the playhead, rendered as an export renders it (original media, the
  export colour transform) at the sequence's size; the Source Monitor exports its clip's frame at its own size.
- Commands: `sequence.exportFrame` and `source.exportFrame`, also in `wabcut-cli` and the assistant.
- PNG is written by Qt and OpenEXR by Wabcut's own writer: the FFmpeg build in use has no zlib, which FFmpeg's PNG and
  EXR coders need. For the same reason a PNG or EXR still cannot be imported back into the project there (the other
  formats can).

## Keys

| Command | Windows | macOS | Id |
|---|---|---|---|
| Loop | | | `monitor.loop` |
| Play In to Out | Ctrl+Shift+Space | Ctrl+Shift+Space | `transport.playInToOut` |
| Play Around (0.4) | Shift+K | Shift+K | `transport.playAround` |
| Play from Playhead to Out (0.4) | Ctrl+Space | Ctrl+Space (Control) | `transport.playFromPlayheadToOut` |
| Shuttle Slow Left / Right (0.4) | Shift+J / Shift+L | Shift+J / Shift+L | `transport.shuttleSlowBackward` / `transport.shuttleSlowForward` |
| Type a timecode (0.4) | numpad digits, + , - , . | numpad digits, + , - , . | (no command: the focused panel's timecode) |
| Toggle Full Screen | Ctrl+` | Ctrl+` | `monitor.toggleFullScreen` |
| Enable Transmit | | | `monitor.enableTransmit` |
| Show Rulers | Ctrl+R | Cmd+R | `programMonitor.showRulers` |
| Show Guides | Ctrl+; | Cmd+; | `programMonitor.showGuides` |
| Lock Guides | Ctrl+Alt+Shift+R | Opt+Shift+Cmd+R | `programMonitor.lockGuides` |
| Snap in Program Monitor | Ctrl+Shift+; | Shift+Cmd+; | `programMonitor.snap` |
| Add Guide… / Clear Guides / Save Guides as Template… / Manage Guides… | | | `programMonitor.addGuide` / `clearGuides` / `saveGuides` / `manageGuides` |
| Transparency Grid | | | `programMonitor.transparencyGrid`, `sourceMonitor.transparencyGrid` |
| Show Dropped Frame Indicator / Overlays / Show Audio Time Units | | | `programMonitor.droppedFrameIndicator` / `overlays` / `audioTimeUnits` |
| Nudge Selected Object | Ctrl+arrows (Shift: by five) | Cmd+arrows | `programMonitor.nudge*` |
| Toggle Crop Direct Manipulation | | | `monitor.toggleCropDirectManipulation` |
| Export Frame | Ctrl+Shift+E | Shift+E | `file.exportFrame` |

Keys without a default have none in Premiere's shipped key sets either; assign them in Keyboard Shortcuts.
