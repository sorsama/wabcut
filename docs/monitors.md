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
- **Drop a clip on the Program Monitor** (0.6, Premiere's edit overlay): drag a clip or a sequence from the Library, or
  the Source Monitor's picture (or its video or audio handle), over the Program Monitor and five zones show what a drop
  does, at the playhead and on the targeted tracks:
  - **Overwrite** (the middle): over what is there.
  - **Insert** (left): at the playhead; the clips after it move later.
  - **Overlay** (top): on the first free video track at or above the targeted one, so nothing is cut; when every one is
    taken there, a new video track on top (in the same undo step).
  - **Replace** (bottom): the clip under the playhead on the targeted video track becomes the dragged one, from its In,
    keeping its place and length (Replace Edit). Dimmed when there is no clip there.
  - **Append at End** (right): after the last clip of the sequence.

  A Library clip uses its Source Monitor In and Out, as a drag onto the timeline does; a Source Monitor drag uses its
  marks. Each drop is one undo step named after the zone, and the first clip in an empty sequence gets the Clip Mismatch
  Warning. A drop that cannot be done says why.
- **Captions in the Program Monitor** (0.6): with the captions shown in the picture, a click on a caption selects its
  cue (outlined; the Captions list scrolls to it; a cue and clips are selected one or the other, as on the timeline's
  caption lanes), a double-click opens its text in the Captions panel, and dragging the outlined caption moves the
  caption track's text block: its placement becomes Custom at the new place, kept inside the style's safe area, one
  undo step (Move Caption). A keyframed caption position is changed in Caption Style instead. Typing in the monitor
  itself is for later.
- **Markers on the time bar**: point at a marker on the Program Monitor's time bar to read its name, comment, time and
  length.

Code: `src/app/qml/components/TimecodeEntry.qml` (`ProjectController.seekToText`, `SourceMonitor.seekToText`),
the drop area in `src/app/qml/panels/SourceMonitorPanel.qml` (`SourceMonitor.load` / `loadFileItem`),
`SequenceTimeBar.markerAt` in `src/app/qml/panels/ProgramTimeBar.qml`, the Program Monitor's edit overlay
(`src/app/qml/monitor/EditOverlay.qml`, `MonitorPanel.programDrop`, `Monitors.overlayTrack` / `replaceTarget` /
`sequenceEndText`) and caption picking (`GraphicCanvas` pick objects of kind `caption`, `MonitorPanel`'s pick area).

## AI effects on the monitor (0.7)

A layer with AI Restore, AI Denoise, AI Refocus or AI Frame Interpolation shows the pictures **Process** made for its clip ([ai.md](ai.md#ai-video-effects-restore-and-denoise-07)). Where a frame has none yet it shows the original picture and the monitor's header says **AI: N layers not processed** (hover for what to do); the badge goes away as the pictures are made - the frame under the playhead updates as each one is stored.

## Move, scale and rotate in the Program Monitor

Premiere's direct manipulation of the Motion effect
([Apply the Motion effect](https://helpx.adobe.com/premiere/desktop/add-video-effects/commonly-used-effects/apply-motion-effect.html)).

- **Start**: click the direct-manipulation button on **Motion** (Transform) in Effect Controls, or **double-click the
  clip's picture** in the Program Monitor. The topmost visible clip under the pointer is selected. A bounding box with
  eight handles, the anchor-point crosshair and (when Position is keyframed) the motion path appear while the playhead
  is over the clip.
- **Gestures**:
  - drag inside the box: **Position**. Shift keeps the move horizontal or vertical.
  - drag a corner or an edge handle: **Scale**, around the anchor point **(0.7)**. A corner scales both ways while Uniform Scale
    is on; a side handle changes one axis and turns Uniform Scale off for the clip (the width, or the height for the top and
    bottom edges) - Shift keeps the shape instead. With Uniform Scale off a corner follows the pointer on both axes, and
    Shift keeps the box's shape. All of it is one undo step per drag, named Scale.
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
- **Mirrored footage** (0.7): a clip whose file flags it as mirrored (a horizontal flip in the container's display
  matrix: some phone front cameras and screen recordings) now shows flipped in the Program Monitor and the exports, the
  way the Library thumbnails and every player show it; the Motion box, the mask handles and the anchor point follow the
  flip. Imported files read the flag; a project made before 0.7 keeps what it probed until the file is relinked.
- **Scale Width and Uniform Scale** (0.7): Effect Controls > Transform has Premiere's **Uniform Scale** switch (on by default,
  no stopwatch). Switch it off and Scale becomes **Scale Height**, with a **Scale Width** row (keyframable) beside it; the
  width starts at the height, so the picture does not jump, and switching Uniform Scale on puts the width back to 100 %
  (one undo step either way). A stretched clip plays, exports, tracks, masks and round-trips through Final Cut Pro XML and
  FCP7 XML as it is drawn; Set to Frame Size, Reframe, Layouts and the Multicam grid make it uniform again or scale both
  axes, the AI zoom and the tracker ask you to switch Uniform Scale on first. Old projects load as they were.
- **Commands**: `clip.setMotion` sets Position, Scale, Scale Width, Uniform Scale, Rotation and Anchor Point together at a
  time (one undo step, keyed where animated; a `scaleWidth` turns Uniform Scale off first): what agents, scripts and
  `wabcut-cli` use.

**Crop** (Premiere 26's crop direct manipulation): the crop button on a Crop effect in Effect Controls, or **Toggle Crop
Direct Manipulation** (`monitor.toggleCropDirectManipulation`, no default key), shows the cropped picture's box; its
edge and corner handles set the effect's Left, Top, Right and Bottom (keyed like Motion, one undo step per drag). The
clip needs a Crop effect (Effects > Transform > Crop). **Text clips (0.7)**: a title is a picture on a canvas of the
sequence's size, so a selected title with a Crop effect gets the same handles (the box is the whole canvas; what you
crop is the title as it is drawn), and the evaluator puts the same Crop on its layer in playback and exports. Without a
Crop effect the monitor says to apply one.

## Loop and Play In to Out

- The **Loop** button (both monitors' transports, and the settings menus; `monitor.loop`, no default key as in
  Premiere's key sets) makes playback repeat **In to Out**: the sequence's marks in the Program Monitor, the clip's in
  the Source Monitor. A missing mark is the start or the end, so without marks the whole sequence (or clip) loops.
  Playing from outside the range starts at its In. It is remembered per monitor.
- **Play In to Out** (Ctrl+Shift+Space, now in the Program Monitor too) plays once from In and stops on the last marked
  frame, or loops with Loop on.
- **Play In to Out with Preroll and Postroll** (Shift+Space, 0.7; the palette's `transport.playInToOutAround`) plays once
  from the In minus the pre-roll to the Out plus the post-roll (Settings > Timeline & editing > "Play around the edit",
  the same times as Play Around), clamped to the sequence or clip, then puts the playhead back where it was. A missing
  mark is the start or the end. Loop is ignored; Space, K, J, L or a seek stop it where it is. With the Source Monitor
  focused it does the same on the source's marks.
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
- **Video Output Settings…** (0.7; Program Monitor gear, Monitors `requestVideoOutputSettings`) gathers the output's settings:
  - **Display.** The display is remembered by its make, model and serial number (the system's name when it has none), not by
    its place in the list: plug another display in and the output stays on the one you chose. While that display is
    disconnected the output is off (the dialog says so, and **Enable Transmit** stays checked); it comes back when the display
    does. The settings from before 0.7 name a display by its number, which is taken over once.
  - **Picture.** **Fit** shows the whole frame, letterboxed or pillarboxed on black, in the shape the sequence has (frame size
    and pixel aspect ratio); **Fill** scales it to cover the display and crops what runs over; **1:1** shows the frame's own
    pixels in the middle of the display. Toggle Full Screen (Ctrl+`) always fits.
  - **Safe margins.** The title and action safe lines over the output only (separate from the monitors' Safe Margins).
  - **Sync offset** (-500 to 500 ms). A TV or projector shows a picture some milliseconds after it is sent. A positive offset
    makes the output choose its frames that much ahead, so the picture meets the sound (the mirror's scheduler gets the
    display latency; changing it restarts the mirror for a moment). Full screen uses no offset.
  - **Match Refresh Rate** (Windows). While the output shows, the display is set to the lowest refresh rate of its current size
    that the sequence's frame rate divides evenly (24 fps: 48, 72 or 120 Hz; 25: 50 or 75; 29.97: 59.94), and is put back when
    the output stops, the display goes, the setting is turned off or Wabcut quits (the mode is set for the life of the process,
    so a crash does not leave it changed). Where the display has no such mode, or on macOS and Linux, nothing is switched.
  - **How the frame rate sits.** A line under the settings says whether the display's refresh rate is a whole multiple of the
    sequence's frame rate (every frame lasts the same), nearly one (23.976 on 24 Hz: a frame repeats now and then), a 3:2
    pulldown (24 on 60 Hz: frames alternate between two lengths) or uneven (25 on 60 Hz: judder).
  - The output follows the Program Monitor (the same transport, edits and loop); it does not show the Source Monitor.
  - **Not here:** video cards that send SDI or HDMI themselves (Blackmagic DeckLink, AJA, NDI) need their makers' SDKs, which
    Wabcut does not bundle. Send the picture to a display, or to an SDI / HDMI converter that is one; an output of its own
    resolution and frame rate, independent of the display's, is that later step.

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

## VR video view (0.7)

A VR / 360 sequence (or a 360 item in the Source Monitor) can be looked around in: the wrench menu's **VR Video View** turns
the monitor into a viewer drawn on the GPU - drag to look around, scroll to zoom the field of view, Home or a double-click to
go straight ahead. See [VR / 360 video](vr.md). The menu item is off for a flat picture; guides, rulers and the Motion box are
covered while the viewer is on.

## Pixel aspect ratio

The Program Monitor shows the sequence in the shape its **Pixel Aspect Ratio** (Sequence Settings) gives it: HDV
footage (1440x1080, 4:3 pixels), DV widescreen and anamorphic sequences are widened to the 16:9 they play as, instead of
being squeezed into the stored frame size. Changing the ratio in Sequence Settings changes the monitor at once; zoom
levels count the widened width. Square pixels (the default) look as before. Review frames and the exports carry the same
shape ([Export, settings and presets](export.md)).

Footage keeps its own shape inside it: a clip is placed by its own pixel aspect ratio over the sequence's, so an HDV clip
(4:3 pixels) fills an HDV sequence exactly and a square-pixel picture is seen at its true shape (Scale to Frame Size, Set
to Frame Size / Fill Frame and the monitor's handles and masks count the same widened frame). Titles, graphics and
captions are drawn in sequence pixels, so in an anamorphic sequence their letters are as wide as its pixels are.

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
next to the zoom, (0.4) in the wrench menu's **Playback Resolution**, and (0.6) in **View > Playback Resolution** and
**View > Use Proxies**, as in Premiere; the Source Monitor's settings menu has the same rows. Both settings are keymap
commands with no default key (`monitor.toggleProxies`, `monitor.playbackResolutionAuto` / `Full` / `Half` / `Quarter`),
so they can be bound in Keyboard Shortcuts and are in the command palette; every row acts on both monitors (the setting is
the app's, and the film icon beside the quality pill shows proxies). The quality button gives way on a narrow monitor.

**Paused Resolution** (0.6, Premiere's): the resolution a monitor draws its picture at while neither monitor plays - Full
(the default), 1/2, 1/4, or Same as playback (how Wabcut behaved before 0.6). Gameplay played at 1/2 or 1/4 is drawn
sharp the moment you stop, and goes back to the playback resolution when you play again. It is in the quality menu and
both wrenches (**Paused Resolution** under Playback Resolution), in **View > Paused Resolution** and in Settings > Media
& cache (**Paused**), and its four rows are keymap commands with no default key (`monitor.pausedResolutionFull` /
`Half` / `Quarter` / `Same`). The quality pill and badge keep showing the playback resolution, so they do not change at
every play and pause; the badge's tooltip names the paused one. The switch happens when playback starts and stops: the
decoders reopen at the new size then, which can add a moment to the start of playback on 4K media - choose Same as
playback if that matters more than a sharp still. High Quality Playback has no counterpart yet (Full is the high quality
setting).

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

## Mute the Source Monitor's sound (0.7)

A loudspeaker button on a pill at the top right of the Source Monitor's picture (it shows while the clip has sound) silences
the source's playback with one click, without touching any volume: scan loud footage, then click again to hear it. The
button turns amber with a crossed speaker while the source is muted. Only the Source Monitor's own playback is silenced
(the monitor plays the clip in a private one-clip project, whose audio tracks are muted); your project, its tracks, the
Program Monitor and exports are untouched, and nothing is added to the undo history. The mute stays for the clips you load
next and ends when Wabcut closes (it is not remembered, so a new session never starts silent). **Mute Source Sound**
(`sourceMonitor.toggleMute`, the palette and Keyboard Shortcuts; no default key) does the same from the keyboard.

Code: `SourceMonitorBridge::muted` (`applyMute`), the `sourceMuteButton` in `SourceMonitorPanel.qml`.

## Composition guides (0.7)

**View > Rule of Thirds Guides / Golden Ratio Guides / Diagonal Guides**, the Program Monitor's wrench menu (**Composition
Guides**) and the command palette switch three guides over the picture, each on its own:

- **Rule of thirds**: lines a third of the way across and down.
- **Golden ratio**: lines at 38.2 % and 61.8 % across and down.
- **Diagonals**: the frame's two diagonals.

Every line is two-tone (a dark stroke under a light one) so it reads on bright and dark pictures. They are in the monitor
only (never in exports or Export Frame), take no clicks, and hide while the Trim view or the multi-camera view shows. The
choice is remembered (`monitor/compositionGuides`). They sit beside Safe Margins and the platform safe zones above,
which already cover the vertical-video zones. Commands `programMonitor.ruleOfThirds`, `programMonitor.goldenRatio`,
`programMonitor.diagonalGuides` (no default keys). Premiere has no such overlay.

Code: `src/app/qml/panels/CompositionOverlay.qml`, `MonitorBridge::compositionGuides`.

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
- **Size (0.6)**: Sequence (the frame's own size), **YouTube thumbnail** 1280×720, **Vertical** 1080×1920 or Custom (16
  to 16384 pixels a side), scaled smoothly; **Crop to fill** fills the size by cropping the frame's centre, else the
  whole frame fits with black bars. **Quality** (JPEG): Best, High, Medium or Small. **Keep under 2 MB** (JPEG; on with
  the thumbnail size, which also picks JPEG): the quality goes down until the file is under YouTube's 2 MB thumbnail
  limit (the message says when even the lowest quality is over). All of it is remembered with the folder and format.
- **Copy to Clipboard (0.7).** The dialog's **Copy to Clipboard** button, a right-click on either camera button (**Copy
  Frame**) and **Ctrl+Alt+Shift+E** (Opt+Shift+Cmd+E on macOS; `monitor.copyFrame`, also in the palette) put the frame
  on the clipboard as a picture, at the sequence's size (the clip's own in the Source Monitor, which also answers the key
  while it has the focus), without a dialog or a file: paste it into Discord, a thumbnail editor or a bug report. The
  message says "Frame copied (1920 × 1080)". Nothing in the project changes. A Wabcut extra: Premiere has none.
- **Export Frames at Markers… (0.6)** (the Markers menu, `marker.exportFrames`; the Highlights panel's **Thumbnails…**):
  the same dialog writes one still per sequence marker - **All**, **Chapters** or **Highlights** (a highlight marker's
  still is taken at its peak) - named after the marker ("Marker 3" without a name; " (2)" for a name used twice) into
  the folder, at the size above (from the Highlights panel: a YouTube thumbnail). It runs in the background with
  progress and **Stop**; a toast says when it is done. **Import into project** puts the stills in a **Thumbnails** bin
  (made when missing), one undo step.
- Commands: `sequence.exportFrame` and `source.exportFrame` (0.6: `width`, `height`, `fit` "fit" / "fill", `quality`,
  `maxBytes`), `sequence.exportFramesAtMarkers` (`folder`, `markers` all / chapters / highlights, `colorIndex`, `at`
  peak / start, `importIntoProject`, `binName`, and the same size keys), also in `wabcut-cli` and the assistant.
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
| Export Frames at Markers… (0.6) | | | `marker.exportFrames` |

Keys without a default have none in Premiere's shipped key sets either; assign them in Keyboard Shortcuts.
