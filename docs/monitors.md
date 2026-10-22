# The Source and Program Monitors

The monitors work like Premiere Pro's (26.x): same names, keys and behaviour, Wabcut's own look. This page covers what
0.2 added (P.2): moving, scaling and rotating clips in the Program Monitor, Loop, full screen and a second display,
rulers and guides, the Transparency Grid, the view settings and Export Frame. Transport, marks, safe margins and the
playback resolution menu are described in [getting-started.md](getting-started.md).

Code: `src/app/qml/monitor/` (QML), the `Monitors` (`MonitorBridge`) and `ProgramMotion` (`MotionBridge`) singletons in
`src/app/src/bridge/`, `edit::setMotion` (`src/model/include/wabcut/model/MotionOps.h`), the playback loop and mirror
(`PlaybackController::setLoop` / `addMirror`), and `exporter::writeStill` (`src/export/include/wabcut/export/StillExport.h`).

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
next to the zoom; Premiere's separate Paused Resolution and High Quality Playback have no counterpart yet (one
resolution serves both, and Full is the high quality setting).

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
