# Wabcut 0.7: release notes (0.7.0)

Windows 10/11 x64, with preview builds for Linux x64, macOS on Apple silicon and, new in 0.7, Windows on ARM64. Free and
open source (GPL-3.0-or-later). No account, no subscription.

Wabcut 0.7 works through the rest of the roadmap and brings the new AI settings. The timeline gains track groups, solo
for video tracks, tracks you drag into a new order, locked clips, clip notes, a pacing strip that shows where an edit
drags, background rendering while you are idle, Render and Replace for nests and a graph editor for keyframes. Effects
get Motion Blur, Track Motion that makes a title follow a moving subject, Scale Width, a Warp Stabilizer that reads
GoPro and Sony gyro data, ColorChecker matching, effects on a whole video track and OpenFX plug-ins. Six AI video
effects (AI Denoise, AI Restore, AI Refocus, AI Face Retouch, AI Eye Contact and AI Frame Interpolation) run on your own
computer with models you choose to download, and Morph Cut can line up faces. Text gets OpenType features, optical
kerning, a font picker with script filters, styles for part of a text and a Sources panel for video essays; captions get
six animated looks, 100 transcription languages, clips that switch languages, custom words and bilingual subtitles.
Audio gets a 7.1 master, a Dynamics Processing graph, spectral repair, EQ Match, a Loudness panel with a radar,
automatic mixing of several voices and an audio description generator. Exports get H.264 and HEVC smart render, HDR10+,
an export history, a check before every export, a default export folder, richer Collect Files and, on storage you own,
cloud projects and review links. The Library gets Reload Footage, Consolidate Duplicates, a rights and clearance log
with a credits roll, SVG, AI and EPS artwork, capture from a camera or capture card and a search across a production.
There is a Python API and support for 360 and VR180 video, and File > Save for an Older Version writes a project that
Wabcut 0.6 or 0.2 to 0.5 can read. Settings > AI is rebuilt: 35 new AI providers, your own providers for every AI
feature, one model per feature and a library of local models.

The project file is still schema 19, the number 0.6 used: everything 0.7 adds is optional data, written only when you
use the feature. Wabcut 0.7 opens 0.6, 0.5, 0.4, 0.3, 0.2 and 0.1 projects. Because the number did not change, Wabcut
0.5 and older refuse a project saved by 0.6 or 0.7, while Wabcut 0.6 opens a 0.7 project and skips whatever it does not
know, which it then loses the next time it saves. A few 0.7 choices use names 0.6 does not know (the new caption
animations, source markers, a clip stretched with Uniform Scale off, a keyed Warp Stabilizer Smoothness, a ProRes RAW
clip with an ISO setting), and 0.6 can refuse a project that uses one rather than open it without it. To give a project
to someone on an older Wabcut, use File > Save for an Older Version…: pick Wabcut 0.6 or Wabcut 0.2 to 0.5, read what
that version cannot hold, and Save a Copy…; your open project is not touched. Wabcut 0.1 is not offered. The rest of
0.7's choices are settings of Wabcut on your computer.

## 0.7.0 (6 November 2026)

### Timeline

- **Track groups.** Target tracks, then a track header's menu has Group Targeted Tracks (or New Group from This Track,
  then Add to Group on the others, and Remove from Group). A group holds video tracks or audio tracks, never both and
  never a submix. Each group gets a row in the Tracks panel with a fold chevron, a name (double-click renames), a count
  and Mute, Solo and Lock for all its tracks at once, and carries a label colour. A folded group is one row with a thin
  bar per clip, and its tracks still play, render and export. The group's menu has Track Height, Target Group and, for
  audio, an Output that can be New Submix for This Group so one fader and one effect stack serve the whole group; the
  Audio Track Mixer folds the group's strips into one. Collapse All and Expand All are in the command palette, and every
  change is one undo step.
- **Solo for video tracks.** Video track headers have a solo button, green like the audio one (Shift-click solos every
  video track). While any video track is soloed only the soloed ones show in the Program Monitor, exports, nested
  sequences and the monitor overlays; the eye still hides a soloed track, and the solo is saved with the project.
  Because it also applies to exports, the lit button is the cue. The track header column is a little wider to fit it.
- **Keys for a track's eye, mute and solo.** Alt+Shift+O toggles Track Output (eye), Alt+Shift+U Track Mute,
  Ctrl+Alt+Shift+S Track Solo and Alt+Shift+S Solo Selected Clip's Track (that track alone, per kind; press again to
  unsolo), on the tracks of the selected clips or else the targeted tracks, in one undo step. On macOS Opt takes the
  place of Alt. They are in the Sequence menu, the palette and Keyboard Shortcuts.
- **Drag tracks into a new order.** Drag a track header's name chip (V1, A2…) or its free space up or down: an accent
  line shows where it will land, and on release the track moves with its clips, links, targeting, effects and settings,
  in one undo step. Video stacks upward and audio downward, each kind on its own. The header menu also has Move Track Up
  and Move Track Down (submix tracks cannot be moved, and there is no keyboard-only drag).
- **Label colours for tracks.** Right-click a track header > Label (sixteen named colours and No Label) puts a colour
  bar on the header's edge, a light wash over the lane and the same colour on the track's mixer strip. The colour is
  saved with the project and is not written to interchange files.
- **Named labels, and Select Label Group.** The clip menu's sixteen swatches carry Premiere's colour names and a ring on
  the colour the selected clips share. Edit > Label lists the names, and Select Label Group selects every clip with the
  same label as the selected ones. Settings > Appearance > Label colours also offers a softer Classic set beside Echo
  (an approximation: Adobe publishes no values, so only the first three colours are Premiere's).
- **Lock a single clip.** Clip > Lock Clip / Unlock Clip (also the clip's right-click menu and the palette) puts a
  padlock on a clip that then cannot be moved, trimmed, split, retimed, renamed, grouped, deleted, overwritten, pushed
  by a ripple or edited in Effect Controls; the edit is refused by name ("clip 'B' is locked"). A locked clip can still
  be selected, copied, played and exported. Locking a picture does not lock its linked sound. Sequence-wide helpers that
  would change a locked clip are refused as a whole.
- **Clip notes.** Right-click a clip > Clip Note… opens a small editor (Ctrl+Enter saves; up to 2,000 bytes). With Clip
  notes on (Timeline Display, or Settings > Timeline & editing; on by default) the note is printed on the clip, and the
  tooltip carries all of it. Both halves of a razor cut keep it, and copies and pastes into another project carry it.
- **A pacing strip.** Timeline Display, or Settings > Timeline & editing > Pacing strip (off by default, because it
  measures your footage) adds a thin strip under the ruler that colours stretches amber or red where the edit is likely
  to drag. Hover a stretch for the reason in numbers ("The same shot runs 60 s", "Silent for 14 s", "Slow speech: 62
  words a minute", "Little moves in the picture"); click puts In and Out around it, Shift-click selects its clips and
  right-click adds a marker for it. It is computed on this computer and uses no model. Repeated takes, an "apply the
  fix" button and a per-project sensitivity are not there.
- **Background rendering while you are idle.** After 10 idle seconds the red and yellow stretches of the render bar turn
  green by themselves. Sequence > Background Rendering (checkable, on by default) and Settings > Media & cache >
  Background rendering set it up: Start after 5 s to 5 min, wait while on battery power, wait while other programs are
  busy. The activity pill shows "Rendering in the background", the Background tasks popover can Stop it, and it pauses
  the moment you use the keyboard or mouse. Battery and other-program load are read on Windows only. Projects with HDR,
  ACES or Wide Gamut colour management re-render their render files once.
- **Render and Replace for nests.** Select a nested clip > Clip > Render and Replace… lists each nest the selection
  plays, the stretch plus handles (1 s each side by default), the format (ProRes 422 HQ, DNxHR HQ, H.264 or ProRes 4444,
  which keeps transparency) and the folder. Start queues one render per nest in the export queue; when a file is done
  the clips play it, with the same place, speed, links, keyframes and effects, as one undo step. Clip > Restore
  Unrendered brings the nest back exactly. Multi-camera clips, auditions, locked clips, Time Remapping and object masks
  are refused with a message, and the job is not kept across a restart.
- **Close gaps from a gap's menu.** Right-click a gap > Close Gaps on V1 (every gap of that track) or Close Gaps in
  Sequence, one undo step. Sequence > Go to Gap > Next in Track and Previous in Track select the gap they land on, so
  Delete or Shift+Delete ripple-deletes it (with no track targeted every track counts).
- **Clean up a sequence.** Sequence > Remove Transitions from Selection and Remove All Transitions turn edit points back
  into cuts without changing the clips; Edit > Select All Disabled Clips and Sequence > Remove Disabled Clips find and
  clear what you switched off (each clip on its own, so a disabled picture's enabled sound stays). Locked tracks are
  left alone, and each is one undo step.
- **Slip trim.** In trim mode Shift-click the other edge of a clip whose edge is selected, or use Sequence > Slip Trim
  or the Slip tab of the Trim view: +1, +5, -1, -5, Ctrl+arrows and J, K, L then slip the clip's content while the clip
  keeps its place and length (the linked sound slips too). The Trim view shows the first and last frame with their
  source timecodes. The four-up Avid display, dragging the content in the Trim view and slipping by a typed amount are
  not there.
- **Alt-drag, then Ctrl, inserts a copy.** Start a drag with Alt (copy), then press Ctrl (Cmd on macOS) before the drop:
  the copy is inserted and later clips move later instead of being overwritten. The readout says "Insert Copy" and the
  undo step is "Insert Duplicate".
- **Keys for Overlay, Insert Before, Insert After and Append.** Clip menu rows for the clip in the Source Monitor,
  placed at the playhead on the targeted tracks: Overlay (Ctrl+Alt+O), Append at End (Ctrl+Alt+P), Insert Before Clip at
  Playhead (Ctrl+Alt+,) and Insert After Clip at Playhead (Ctrl+Alt+.). On macOS the same keys use Opt+Cmd.
- **Pan with the middle mouse button; hold H or Z.** A middle-button drag on the lanes scrolls the timeline both ways
  with any tool. Hold H or Z (past 300 ms) for the Hand or Zoom tool until you let go; a tap still chooses the tool as
  before.
- **Move several markers at once.** In the Markers panel, Ctrl-click, Shift-click and Ctrl+A select markers, and Delete,
  Ctrl+X, Ctrl+C, Ctrl+V and Alt+Left / Right (Shift: ten frames) act on the selection. On the ruler a Ctrl-drag marquee
  selects flags and dragging one moves them all with a live preview. Clear Selected Marker (Ctrl+Alt+M) clears the
  selected markers, else the one at the playhead. Each action is one named undo step.
- **Import markers from a file.** The Markers panel's Import button offers Import markers…, Import markers at the
  playhead… and Replace the markers with a file…. It reads Premiere's marker CSV, tables with time, name and comment,
  text lines (YouTube chapters, "00:01:23:10 - Name", Audacity labels) and JSON, and says what it did ("Imported 2
  markers - 1 line skipped: line 4…"). Markers already there are left out, so importing twice adds nothing. Limits are
  16 MiB and 10,000 markers.
- **Add Marker from In/Out (Shift+X).** Markers > Add Marker from In/Out makes a range marker from the In mark to the
  Out mark, the Out frame included, in one undo step. It is greyed with "Set In and Out first" until both are set.
- **Markers follow Track Select.** Dragging with Track Select Forward (A) moves the sequence markers that start at or
  after the first clip moved, and Track Select Backward (Shift+A) those at or before the last, by the same distance in
  the same undo step. Markers > Ripple Sequence Markers off keeps them still. There is no marker preview while dragging.
- **Choose what the timeline snaps to.** Tracks panel display settings > Snap to (and five rows in Settings > Timeline &
  editing) switch snapping on or off for clip edges, the playhead, In/Out marks, markers and beats. S stays the master
  switch.
- **Clip duration, and audio stream names, on clips.** Tracks panel display settings > Clip duration (off by default)
  prints the length after the clip name in the sequence's time format. An audio clip from a recording with two or more
  audio streams shows its stream's title ("Gameplay · Mic"); Audio track names is on by default.
- **Stop edits from adding tracks.** Settings > Timeline & editing > Add tracks when editing clips in (on by default).
  Turned off, a drop or a Source Monitor edit leaves out a stream with no track and says so, Paste stops with "no video
  track to paste into" and an Overlay with no free video track is refused. Add Track and dragging below the last track
  still add tracks.
- **Match Frame on a nest.** With the playhead over a nested or multi-camera clip, F (Sequence > Match Frame) opens its
  source sequence in a tab on the frame the clip shows; on any other clip it loads the media in the Source Monitor as
  before.
- **Menu key for timeline menus.** In the timeline lanes the Menu key or Shift+F10 opens the menu of the selected
  transition, gap or clip where it sits, or the targeted track header's menu when nothing is selected.
- **Copy clips between projects with what they use.** Copying clips from one project into another now brings graphic
  images, template pictures, Image Matte Key pictures, grade LUTs, audition takes, the contents of nested sequences and
  Master Clip Effects, matched to files the destination already has and added when missing.
- **Clearer names for frame fitting.** Auto Fit to Frame (Premiere's Scale to Frame Size), Set Scale to Fit Frame (Set
  to Frame Size) and Set Scale to Fill Frame (Fill Frame) now read that way in the Clip menu, the clip's right-click
  menu, Default Media Scaling and the shortcut list, each with a hover line naming Premiere's term. Searching the old
  names still finds them.
- **A tidier Layout picker.** The gaming Layout picker scrolls in a small window and shows one clear message per
  problem: with no gameplay clip, one amber line and a Use the clip under the playhead button; Facecam = Clip without a
  webcam clip, a hint and a reason on the disabled button.

### Monitors

- **360 and VR180 video.** Wabcut reads a file's spherical metadata (equirectangular 360, VR180, mono, top-bottom or
  side-by-side). Clip > Modify > Interpret Footage and Sequence Settings have a VR video choice (Use VR Settings from
  File, Flat Video, Equirectangular 360, Equirectangular 180) with the stereoscopic layout, and warn when the frame is
  the wrong shape. The Program Monitor and Source Monitor get a VR viewer (wrench > VR Video View): drag to look around,
  wheel for the field of view (10 to 150), Home or a double-click to look straight ahead. A VR Reorient effect
  (Effects > Immersive Video) has keyframable Yaw, Pitch and Roll. MP4, MKV and WebM exports write the spherical
  metadata; MOV and other containers cannot. Cube maps and fisheye read as flat, stereo is viewed through the left eye
  and the full-screen output shows the flat canvas.
- **Pixel aspect ratio in the monitors and exports.** An HDV (1440x1080), DV widescreen or anamorphic sequence is shown
  in the shape it is meant to be seen as soon as you change Sequence Settings > Pixel Aspect Ratio, and exports with
  that ratio written into MP4, MOV, MKV and MXF files, so a player shows 16:9 instead of a squeezed 4:3. Scale to Frame
  Size, Fill Frame and default scaling fit to the displayed width, FCP7 XML writes the ratio, and watch folders copy it.
  Titles, graphics and captions are still drawn in sequence pixels, an output of another shape than the canvas is
  written with square pixels and smart rendering is off for such sequences. Rotation no longer shears the picture, and
  an anchor on these sequences is now in square-pixel units.
- **Mirrored footage is flipped.** A clip whose container carries a horizontal flip (some phone front cameras and screen
  recordings) is flipped in the Program Monitor and in every export, as the Library thumbnails and other players already
  did, and the Motion box, anchor and mask overlays follow. A project made before 0.7 keeps the information it read
  earlier: relink or re-import the file to pick up the flip.
- **Copy the frame to the clipboard.** Ctrl+Alt+Shift+E (Opt+Shift+Cmd+E on macOS), Copy to Clipboard in the Export
  Frame dialog or a right-click Copy Frame on a monitor's camera button puts the current frame on the clipboard as a
  picture, with no file and no dialog.
- **Play In to Out with rolls (Shift+Space).** Plays once from the In mark minus the pre-roll to the Out mark plus the
  post-roll (the Play Around times in Settings > Timeline & editing) and puts the playhead back. In the Source Monitor
  it uses the source's marks.
- **Composition guides.** View > Rule of Thirds Guides, Golden Ratio Guides and Diagonal Guides (and the Program Monitor
  wrench menu's Composition Guides) draw two-tone lines over the Program Monitor. They are never drawn in exports or
  Export Frame.
- **Mute the Source Monitor.** A loudspeaker button on the Source Monitor's picture mutes its playback for the clips you
  load next (not the project or the Program Monitor); a new session never starts muted.
- **Nudge a multicam angle's sync.** Right-click an angle in the Multi-Camera View (or Tab to it and press the Menu key)
  for Sync 1 or 10 Frames Earlier / Later, Reset to Measured Sync and Open Multi-Camera Source. An angle that is off its
  measured sync wears a badge such as "+2 f".
- **Video output to a second display.** Program Monitor gear > Video Output Settings… chooses the display by its make,
  model and serial rather than by number (unplugging turns the output off and plugging it in turns it on again), fits
  the picture (Fit, Fill or 1:1), sets safe margins and a sync offset of -500 to 500 ms, and on Windows can Match
  Refresh Rate to the sequence (a line says whether the display's rate divides evenly, such as 24 on a 48 Hz display, or
  will judder). Cards that need a vendor SDK (Blackmagic DeckLink, AJA, NDI) are not supported, a custom output size or
  frame rate independent of the display is not available, and Match Refresh Rate has only been run with a stand-in
  display.

### Effects and colour

- **Track Motion.** Make a title, picture, graphic or Blur/Mosaic clip follow a moving subject in three clicks. Open it
  from Effect Controls (the pointer button in the panel header), Clip > Track Motion… or the palette while a footage
  clip is under the playhead; pick the footage under "Subject in" and the clip under "Follows", choose "Move a clip with
  it" or "Hold it still" (a targeted stabilize) and Position or Position, scale, rotation, then mark the subject with
  Draw box on the Program Monitor or with a mask of the footage (ellipse, pen path or AI Object Mask). Tracking runs in
  the background with progress, Stop and a confidence strip, stops where the picture is lost and writes Motion keys as
  one undo step; Stop keeps the frames tracked so far. It is correct on rotated and mirrored footage and in
  non-square-pixel sequences. Corner-pin tracking of surfaces is not there, the box does not re-seed itself after a lost
  frame, and clips inside nested sequences and Time Remapping footage are refused with a reason.
- **Motion Blur.** Effects > Video Effects > Blur & Sharpen > Motion Blur has Shutter Angle (180 degrees, 0 to 720,
  keyframable; 0 changes nothing), Shutter Phase, Export Samples (16) and Preview Samples (8). The clip's Motion
  (position, scale, rotation, keyframes, Zoom Punch, Shake, tracked moves) is sampled across the shutter, so a speed
  ramp that makes the keys race also lengthens the smear, the same in preview and export. Partly done: the footage's own
  content in a speed ramp is not blurred (a retimed clip still shows one decoded frame per output frame), nested and
  multi-camera clips and opacity are not blurred, and only media clips were tested.
- **Scale Width and a Uniform Scale switch.** In Effect Controls > Transform turn Uniform Scale off: Scale becomes Scale
  Height and a keyframable Scale Width row appears, starting at the same value. On the Program Monitor a side handle
  changes one axis and turns Uniform Scale off in the same undo step, Shift keeps the shape, and Set to Frame Size
  returns a stretched clip to uniform. FCPXML and FCP7 XML carry the two values; Premiere's own files have one scale. A
  project that stretches a clip this way is refused by older Wabcut versions. Auto Zoom and the similarity tracker
  refuse a stretched clip.
- **Nearest Neighbor scaling.** Clip > Video Options > Nearest Neighbor Scaling (and the clip's right-click menu, and a
  switch under Effect Controls > Motion) keeps hard pixel edges for pixel art and retro footage when a clip is scaled,
  in preview and export. The Transform effect's own Sampling choice is still ignored.
- **Warp Stabilizer with the camera's gyro.** The Method menu has Gyro (camera metadata), offered only for footage whose
  file carries readable gyro data. A note under the effect header says what the clip has ("This clip has gyro data
  (GoPro HERO8 Black, 202 readings per second)") and after Analyze how well it synced, and four settings appear: Gyro:
  Sync (auto-sync with the picture, on by default), Offset (ms), Field of View and Rolling Shutter Read-out (ms). It
  handles speed changes, removes rolling-shutter skew and previews as it exports. It works for GoPro (HERO5 and later)
  and Sony (Alpha, FX, ZV, RX MP4 files); GoPro was checked on real HERO5 and HERO6 footage, Sony only on synthetic
  data. DJI, Insta360, RED and Blackmagic RAW are recognised and refused with a reason, and horizon lock is not done.
- **Warp Stabilizer: speed changes, Analyze All and a keyframable Smoothness.** Warp Stabilizer and Rolling Shutter
  Repair now work on clips with a speed change or Time Remapping. Sequence > Analyze All Stabilizers analyses every
  stabilizer of the sequence, nests included, as one background job and one undo step, and Effect Controls shows "Out of
  date" on a stale one. The Export panel lists stabilizers in the export range that are not analysed, with Analyze now,
  instead of silently exporting them unstabilized. Smoothness has a stopwatch: key it to steady only part of a shot (0 %
  leaves the shot as shot); a project that keys it does not open in an older Wabcut.
- **Match a clip to a ColorChecker chart.** In the Color workspace > Color panel, the Color Chart section finds the
  24-patch X-Rite ColorChecker Classic in the frame under the playhead (Find chart), shows its outline with four corner
  handles you can drag, and Match writes the fit into the clip's grade (temperature, tint, exposure, contrast,
  saturation, RGB curves and hue points) as one undo step; "Also match the other selected clips" applies it to clips
  shot in the same light. The swatches show the mean and worst delta E before and after. On two real photos the mean
  error fell from 7.0 to 2.5 and from 10.4 to 2.6. Only the ColorChecker Classic is supported.
- **Effects on a whole video track.** Right-click a video track's header > Video Track Effects… opens that track's
  effect stack (switch, move, remove), Add Effect… and the selected effect's settings with stopwatches. You cannot drag
  an effect onto a header, Effect Controls does not list the track's stack and audio tracks are separate.
- **Skin Smoothing.** Effects > Blur & Sharpen > Skin Smoothing, a beauty filter that needs no AI model: Amount, Keep
  Detail, Size, Skin Tone, Tone Range, Softness and Show > Skin Mask, all keyframable except Show. It finds skin by
  colour only, so skin-coloured wood or an orange jacket is smoothed too; use AI Face Retouch for face-aware work.
- **A graph editor for keyframes.** Window > Graph Editor (or the spline button in Effect Controls' header) shows every
  animated parameter of the clip as a curve in its own colour, with a legend, Shared scale and a Speed graph with Bezier
  handles. Select, box-select, drag keys in frames and value (Shift holds one axis), double-click a curve to add a key,
  type Time and Value, pick interpolation pills (Linear, Hold, Bezier, Ease In, Ease Out, Ease In and Out) and one of 32
  easing presets. The wheel zooms, the middle button pans, F fits. The selection is shared with Effect Controls and each
  gesture is one undo step. One clip at a time, and the view is not saved.
- **Keyframes that follow edits.** In an Effect Controls lane, right-click a key > Pin to Clip Start or Pin to Clip End:
  the key then moves with that end through trims, ripples, rolls, slips, slides and speed changes. Sequence > Ripple
  Track Keyframes (per sequence, off by default) makes the keyframes of tracks and the master follow inserts, extracts,
  ripple deletes and speed changes, like markers do.
- **See a clip's keyframes on the clip.** Click a clip's fx badge for Show Clip Keyframes: Motion, Opacity, Volume,
  Panner or any effect parameter draws its value line and keys on the clip, and Previous / Next Keyframe move the
  playhead between them. It is display only; keys are still edited in Effect Controls.
- **Blend Mode picker.** Effect Controls > Opacity has a Blend Mode choice, with Shift+= and Shift+- for the next and
  previous mode.
- **Type a calculation into a number field.** Any Effect Controls number field takes + - * / ^, parentheses and comma
  decimals (`1920/3`); `1/0` is refused.
- **Favourite effects pinned in Effect Controls.** An effect marked as a favourite in the Effects panel appears at the
  bottom of a clip's stack with an Add button; right-click a section > Pin to Effect Controls or Unpin.
- **A shortcut for any effect or preset.** Every video and audio effect and every effect preset can have its own key in
  Settings > Keyboard Shortcuts (groups Effects and Presets), or from right-click > Keyboard Shortcut… in the Effects
  panel. A bound key applies it to the selected clips in one undo step. No default keys.
- **A replaced LUT file is picked up.** A .cube LUT replaced on disk is used within about a second, and rendered
  previews that used it are marked out of date; an export waits up to 3 seconds for a missing or half-written LUT and
  then stops instead of writing frames without the look.
- **LUTs with "#" in the title load.** A .cube TITLE line with a "#" inside the quotes is read as text, and a file
  Wabcut writes reads back.
- **Image Matte Key re-reads an edited picture.** Saving over the matte image shows the new picture instead of the
  cached one.
- **Import a folder of LUTs.** In the LUT menu (Basic > Input LUT, Creative > Look) choose Import folder…: every .cube
  in it and its sub-folders is added in one undo step and listed in submenus named after the folders. Up to 500 files
  and 8 folder levels.
- **Shift while drawing masks.** Hold Shift while dragging an Ellipse or Rectangle mask for a circle or square; the Pen
  is unaffected.
- **Shift pins a Bezier handle level.** In an open value graph, dragging a handle with Shift gives a flat ease; Alt
  still breaks the pair.

### AI video effects

These run on your computer, in the same helper process as the other local AI models, on the graphics card when it can
and on the processor when it cannot. The models are not part of Wabcut: each effect's header offers a Download with a
licence card first, and Offline mode and the no-cloud lock block the download. Each effect is an ordinary clip effect
(Master Clip Effects, copy and paste, presets, undo, saved with the project). Process works in the background with
progress and Cancel; frames not processed yet play as they are and the Program Monitor shows an "AI: 1 layer not
processed" badge, and an export processes what is missing first (the Export panel counts it, with Process now) or stops
with the reason. Settings > Media & cache has an "AI video pictures" row with Clean.

- **AI Denoise.** Effects > AI > AI Denoise removes noise with a local model (DRUNet colour denoiser, 131 MB): Strength,
  Preserve Grain (15 % by default, so the picture does not look waxy), Tile Size and a keyframable Amount.
- **AI Restore: upscale and deblur.** Effects > AI > AI Restore has Scale (Auto, 1x, 2x, 4x), Upscaler (Fast or High
  quality), Deblur, Light Denoise, Preserve Grain, Tile Size and Amount. Auto does not enlarge what already fills the
  sequence. The models are Real-ESRGAN (5 MB and two of 67 MB) and NAFNet deblurring (92 MB). Partly done: there is no
  Render and Replace, no one-click switch on a Library item (a Master Clip Effect does that) and no cloud restore
  provider; it works on one frame at a time.
- **AI Frame Interpolation for slow motion.** Clip > Video Options > Time Interpolation > AI Frame Interpolation (also
  the Speed/Duration dialog) makes a picture for every output frame that falls between two source frames (RIFE 4.25
  lite, 23 MB). Until a picture exists the clip plays with Optical Flow. Frames wider than 1920 are interpolated at 1920
  and scaled up, and clips inside nested sequences are not processed.
- **AI Refocus.** Effects > AI > AI Refocus changes where the focus falls after shooting: Focus (keyframable, for a
  focus pull), Sharp Zone, Blur Size, Near Blur, Far Blur and Show (Result, Depth Map or Blur Map), from one depth map
  per frame (Depth Anything V2 Small, 99 MB). Edges of the sharp zone can flicker on busy scenes.
- **AI Face Retouch.** Effects > AI > AI Face Retouch smooths skin, heals blemishes, whitens teeth and brightens eyes on
  faces only (Smoothing, Keep Detail, Blemishes, Whiten Teeth, Bright Eyes, Show). Up to 8 faces of at least 24 pixels;
  stills and nested sequences are not processed. The face-parsing model (53 MB) is MIT-licensed but was trained on a
  research-only collection, and the licence card says so.
- **AI Eye Contact.** Effects > AI > AI Eye Contact makes a speaker who looks down at notes appear to look at the lens
  by moving the iris inside the eye opening (Amount, Down From / Down To, Side Limit, Faces, Show); lids never move, and
  blinks and turned heads are left alone. It is a face-landmark model with an iris warp, not a repainted eye, so lowered
  lids stay lowered and partial Amounts look more natural. It was checked on drawn eyes and one portrait, not on a real
  speaker reading notes.
- **Morph Cut that lines up faces.** Morph Cut has a Face Aware switch (on by default): where a face is found on both
  sides of the cut, the head's movement aligns the two pictures so a talking head stops ghosting. It needs the 0.2 MB
  face detector (the header offers it) and falls back to the old Morph Cut when a face is not found on both sides. It
  was verified on a synthetic talking head, not on real footage.
- **Auto B-roll skips hidden tracks.** Auto B-roll no longer places clips on a video track whose eye is off; with no
  other free track it makes its own "B-roll" track, and the preview names the track.

### Graphics and text

- **A Sources panel for video essays.** Window > Sources lists the claims of the sequence by time with the source behind
  each, and flags a claim with none ("Needs a source"). The + button cites a source (the claim, where it is: playhead,
  In to Out, selected clips or the words selected in the Transcript tab; address, title, author, publisher, date,
  archive link, screenshot and a note). Export copies a YouTube description block with timestamps, the bibliography
  (APA) or a numbered list, and saves Markdown, the bibliography or a PDF; Lower-third places "Source: title - author"
  over each cited claim on new Sources video tracks. Other bibliography styles, a Cite action on a transcript line and
  reusing one source at many times are not there, and the PDF is a plain list.
- **One font picker, with filters and smart search.** Effect Controls > Text > Font, Essential Graphics, Caption Style >
  Font and the Brand Kit show the font in use as a pill that opens a picker: a search field, grouped rows that show each
  font in itself, and Filters for Language or script (26 scripts, including Thai, Japanese, Chinese, Korean, Arabic,
  Hebrew, Cyrillic, Greek and Devanagari), Style, Type (Sans, Serif, Mono, Display, Handwriting, Symbol), Source
  (Installed, Brand kit, Recently used, Favourites, Project fonts) and Sort. Typing "Thai" lists fonts with Thai in the
  name first, then fonts that draw Thai; typos such as "helvetca" are tolerated. A layer's own text sets the starting
  filter, a star marks favourites and recent fonts are remembered. Variable fonts count as their named instances.
- **OpenType features and optical kerning.** Essential Graphics > Edit > Advanced has pills for Ligatures, Contextual
  Alternates, Discretionary Ligatures, Stylistic Alternates and Swashes, and a box for a feature list in CSS
  font-feature-settings style (`calt, -liga, ss01`, up to 16). Effect Controls' Text section has a Typography group with
  Optical Kerning, which spaces each Latin, Greek or Cyrillic pair from the glyph shapes. Captions take the same feature
  list (Captions > Style > Text), and text styles and Push to Text Style carry both. Optical kerning is for graphics
  only, not legacy title clips.
- **Style part of a text, and a Styles tab.** Double-click a text on the Program Monitor and select some of it: the
  project's text styles appear as buttons under the box (and Ctrl+Alt+1..9), and a click styles only the selected
  characters. Essential Graphics has a third tab, Styles: click applies a style, drag it onto a graphic clip,
  right-click for Rename, Duplicate, Move and Delete. Editing a text no longer flattens its styled runs.
- **Keyframe a template's fields.** In Essential Graphics > Edit every template field except a length has a stopwatch:
  colours fade from the key before, while text, font, picture and checkbox fields switch and hold. Keys show as chips
  under the field, and preview and export draw them, also for an imported .mogrt.
- **Template folders and Install folder.** In Essential Graphics > Callouts > Browse, Sort orders every group (name,
  length, most fields), a template of This project or My templates can be filed in a folder (Move to Folder…, up to 3
  levels), and Install folder… and Install folder into My templates… add every .mogrt and Wabcut template under a folder
  (500 files, 8 levels) in one undo step.
- **Lottie with editable fields, dotLottie and part of OGraf.** File > Import Lottie… now also reads dotLottie (.lottie)
  archives. Each text and each fixed solid colour becomes a template field named after its layer ("Title", "Bar color"),
  so Essential Graphics > Edit shows the fields and the stopwatch can key them. For OGraf (EBU spec v1) it takes a
  *.ograf.json, a package folder or a .zip, but only a package that ships a Lottie version is imported (as a template
  whose fields carry the manifest's names). Wabcut has no web engine and never runs scripts, so an HTML-only OGraf
  package is refused with a message that lists its fields; fields with no counterpart (numbers, choices, pictures) are
  listed but not used. Lottie images and themes are not supported.
- **Crop handles on titles.** A selected title with a Crop effect shows the crop handles in the Program Monitor, and
  dragging them edits the Crop effect. The handles frame the whole canvas, not the text.
- **Vector artwork stays sharp.** SVG, SVGZ, AI and EPS logos and artwork import as pictures that are drawn again at the
  size they are shown; the details are under Library and media.

### Audio

- **7.1 surround master.** Choose 7.1 in New Sequence > Advanced > Audio master or on the Audio Track Mixer's master
  strip (Stereo, 5.1 or 7.1, with a note saying what the change did). Tracks, submixes and clip channel formats can be
  7.1 (Modify > Audio Channels picks it for an 8-channel file), a mono or stereo track's pan knob becomes a puck over
  seven speakers, the master strip and Audio Meters show eight meters, every built-in audio effect runs on eight
  channels and loudness is measured in 7.1. "As the master" exports write 8 channels (AAC, Opus, PCM in WAV, MOV, MKV,
  MP4; MXF writes eight tracks); AC-3 carries 5.1 at most and MP3 is stereo. AAF and OMF write a 7.1 track as eight
  named mono tracks. A 7.1 AAF has only been read back by Wabcut, not opened in Pro Tools, Media Composer or Resolve.
  Upmixing from 5.1 leaves the rear pair silent, and an 8-channel output device is not opened.
- **Dynamics Processing with a graph.** Effects > Audio Effects > Amplitude and Compression > Dynamics Processing works
  on a clip, a track or the master. Its section shows a frequency view (20 Hz to 20 kHz) with draggable crossovers and
  thresholds, each band's gain reduction, input and output meters and the transfer curve of the selected band; each band
  has a mode (Compress, Expand, Gate or Limit) with Threshold, Ratio, Range, Attack, Release, Knee and Make-up, plus
  Solo and Bypass. Bands 1 to 4, Mix, Limiter and Auto Gate, and presets (Voice Leveler, De-esser, Noise Gate, Bass
  Tamer, Podcast Glue, Broadcast Limiter). FCP7 XML, FCPXML and OTIO carry it only as a Wabcut parameter set that other
  editors skip. No spectrum behind the graph and no sidechain.
- **Spectral repair.** Audio Tools > Spectral (also Clip > Spectral Repair…) shows the clip's spectrogram and lets you
  select with Box, Lasso, Brush or Tone and then Attenuate (0 to -96 dB), Heal, Fill with noise floor or Remove tone (a
  tone plus 1 to 16 harmonics, following a traced pitch), each button one undo step. Listen with Play selection and
  Repair on for A/B. Repairs are one non-destructive effect per clip, follow the sound through trims, moves and speed
  changes and render the same in preview and export. Heal over more than about 0.9 s fills from above and below only,
  and there is no clone, no per-channel repair and no background job.
- **EQ Match and Level Match.** Audio Tools > Match: choose a Reference (the first selected clip, the mix between In and
  Out or a project file) and a Target, press Learn (a background job that changes nothing) to see the spectra and the
  correction (±18 dB, 24 bands from 78.7 Hz to 16 kHz), shape it with Amount and Smooth, then Apply EQ or Preview on and
  off. Match level works in loudness or peak mode with Headroom and a Limit option. The Graphic Equalizer (30 Bands)
  realises its top two matched bands 1 to 3 dB short.
- **A Loudness panel and a Radar.** Window > Loudness opens a slim panel between the timeline and the Audio Meters:
  momentary and short-term bars on the EBU R128 scale around the target, numbers for short-term, momentary, integrated,
  true peak and loudness range, and a target pill (YouTube -14, Podcast -16, Broadcast -23 or the mixer's target). Its
  Radar view draws the live short-term loudness clockwise over a lap of 1 to 60 minutes, or the whole mix of the last
  Analyze mix in one turn. Loudness already measured is measured again once.
- **Auto-Mix Voices.** Audio Tools > Auto-duck > Voices lowers each microphone on Dialogue-role tracks while another
  voice speaks and brings it back before its own speaker's next word, with Amount (-12 dB), Attack, Release, Hold, an
  optional gate, a Preview with ghost keyframes and one undo step. Where two voices overlap nothing is lowered.
- **Save a mixer strip's effect chain as a preset.** Under any Audio Track Mixer strip (track, submix or master) the
  Presets row has Save effects as preset…, and lists audio presets that append their effects to the strip in one undo
  step. The Effects panel's Presets bin shows them too. The fader and panner are not part of a chain.
- **Premiere's mixer panel menu.** The "…" button in the Audio Track Mixer has Show/Hide Tracks… (Ctrl+Alt+T), Meter
  Input(s) Only (Ctrl+Shift+I) and Loop (Ctrl+L). Hidden strips are a view choice that still plays.
- **Bleep with your own sound.** Captions page > Profanity > Bleep > Replace with a sound file… plays a wav, mp3, flac,
  ogg or m4a on each profane word instead of the 1 kHz tone (cut at the word's end; the first 20 s, as mono). It applies
  in preview and export, captions read [bleep], and the file is collected with Collect Files. A missing file plays the
  tone.
- **Poly WAV channel names.** A polyphonic WAV from a field recorder shows its channel names (from its iXML track list)
  in Get Properties ("1 Boom · 2 Lav A") and in Modify > Audio Channels, and Split to Mono names the new clips after the
  microphones. WAV files only.
- **Long voice-over takes end cleanly.** A voice-over with no punch-out stops by itself at the longest a WAV can be (4
  GiB, about 3 hours of 32-bit float stereo at 48 kHz), keeps the take and tells you to record the rest as a new one;
  before, such a file got a wrong header. A real 4 GiB take was not recorded in testing.
- **Loudness QC keeps the overs of very hot programmes.** Analyze loudness on a long stream that peaks above -6 dBTP for
  hours now reports the true-peak overs from the end of the programme too; past 100,000 hot blocks it keeps the loudest
  and shows the count with a "+".
- **The mixer shows why an effect failed.** Adding an effect in the Audio Track Mixer no longer fails silently: the
  reason shows under the slots ("Enhance Speech works on dialogue clips: pick it on their track"). Enhance Speech
  becomes a slot with a count ("1/3") whose menu has Settings…, Bypass and Remove. The impossible "Essential Sound"
  entry is gone.
- **Edit an effect's settings right in the mixer.** Double-click a slot or choose Settings… for a popup with every
  parameter (slider, number, switch or menu), a per-parameter reset, an On switch, Reset to defaults and Edit plugin…
  for plug-ins with an editor. Edits are live and undoable, and an animated parameter gets a keyframe at the playhead.
  Hovering a slot shows its main values.
- **Drag a slot to reorder it.** In the Audio Track Mixer, drag an effect slot up or down; the move is one undo step.

### Captions and transcripts

- **Transcribe in 100 languages.** In the Transcript tab's set-up (sequence and Source mode) the Language spoken button
  offers Auto-detect or any of the 100 languages the recogniser knows, found by English name, own name ("Hebrew",
  "עברית") or code, each marked Strong, Fair or Limited, with a note on the model size that suits it. Translate to
  English uses the recogniser's own task (local transcription only). Right-to-left languages read from the right in the
  Transcript tab and in captions, and 11 more languages are available for caption translation. The cue-editing field of
  a Hebrew or Arabic cue does not switch its own direction.
- **A clip that switches languages.** Mixed languages (set-up page, with Auto-detect) cuts the audio at pauses and
  detects the language of each piece; the transcript starts a new paragraph where the language changes, tags it and
  shows "Languages: English 75 % - Thai 25 %". Local transcriber only, with a multilingual model; a change in the middle
  of one stretch without a pause keeps the language heard first, and it has not been tried with a real speech model.
- **Custom words.** Custom words beside Language spoken takes names, game titles and jargon (one per line or
  comma-separated), saved with the project in one undo step; the next transcription listens for them. The local
  transcriber's hint holds about 480 bytes, so only the first words that fit are used; a cloud transcriber gets the
  first 100.
- **Transcribe again, and Delete transcript.** The Transcript tab's "…" menu has both, with a panel that asks first and
  the scope (the sequence's chosen voice tracks or the selected clips). Captions made from the transcript and cuts made
  by editing its text stay. Deleting is not an undo step.
- **Six new animated caption looks.** Caption Style has 11 looks; Word Pop, Highlighter, Bounce, Typewriter, Zoom and
  Hype play in their tile on hover. The Animation menu has None, Pop, Fade, Word by word, Word pop, Bounce, Highlight
  bar, Typewriter and Zoom with Speed and a Curve (Smooth, Overshoot, Linear), and Emphasis (None, Key words or Auto:
  numbers, CAPITALS, words followed by "!") with its own colour and size. The monitor, burned-in exports and soft tracks
  draw the same. Partly done: inserting emoji into a cue is not there (the system emoji panel already works in the cue
  field).
- **Caption text rules.** Caption Style > Text rules: Letters (as written, lowercase, UPPERCASE), No punctuation and No
  accents, applied when the caption is drawn, to the SRT, VTT and other caption files and to the soft track; the cues
  keep their words as written. "3.5", "it's" and "well-known" keep their punctuation.
- **Keep caption looks across projects.** Caption Style > Save preset has Also keep it in My looks (on by default); My
  looks appear in every project's gallery, and Import looks… and Export looks… read and write a
  .wabcut-caption-presets.json file.
- **Bilingual subtitles.** In the Captions tab, a track's menu has Show with the main track: the monitor, burned-in
  exports and the Frame Inspector then draw every track marked this way together, each with its own style (a smaller
  yellow translation under an English line). Bottom tracks stack upwards from the title-safe line and top tracks
  downwards; tracks with a Custom position are never moved.
- **Turn captions into graphics.** The Captions tab's track menu has Convert to graphics: every cue becomes a graphic
  clip at its time on a new video track, with the caption style's font, outline, shadow, box and placement, in one undo
  step. The graphics can be animated like titles; animated looks, speaker names and the punctuation and accent rules do
  not carry over.
- **An audio description for blind and low-vision viewers.** Sequence > Generate Audio Description… (hidden with AI off)
  finds the gaps in the dialogue, sends one to three frames of each gap and the words around it to a chat model that
  accepts pictures (yours after consent, or one on this computer) and shows each line with its word count against what
  fits. Put on a track makes a switched-off caption track named "Audio Description", Export script… writes a timecoded
  text file for a narrator, and Voice… opens the Dub dialog prefilled with ducking. The step that reads the pictures has
  only been tried with a scripted model, never with a real vision model, and no model ships with Wabcut.
- **TTML and DFXP times with frames.** Files with a frame rate and multiplier (29.97, 23.976, 25) now place cues at the
  right time with no 0.1 % drift.
- **Soft subtitles sit beside their picture.** MKV, MP4 and MOV exports with a Subtitle track write each cue next to the
  video it belongs to, so sequential players, seeking and progressive download work.

### Library and media

- **Reload Footage.** Select items in the Library and choose Clip > Reload Footage to read each item's changed file
  again where it is: a new length, streams, size and rate show up while clips, marks, markers and effects stay. Items
  that share the file reload with it, an offline item whose file is back comes online, and the message says how many
  clips now run past a shorter file's end. It runs in the background with progress and Cancel, and is one undo step. It
  does not look for a moved file (that is Relink).
- **Consolidate Duplicates.** Edit > Consolidate Duplicates… (or Library "…" > Consolidate duplicates…) lists each set
  of duplicates with what is kept and removed and merges them in one undo step, with clips, audition takes, subclips,
  redactions, masks and corrections following the kept item. Items that read differently (Interpret Footage, timecode,
  channel map, RAW settings, Master Clip Effects) are listed, not merged. Edit > Remove Unused… is now in the Edit menu
  too.
- **A rights and clearance log, with a credits roll.** Right-click a clip > Rights and Clearance…, or File > Export >
  Rights and Clearance Log…: per source set a status (not set, cleared, pending, fair use, not required), source,
  licence, reference, territory, expiry, cost, credit line and notes. The dialog flags "Not cleared yet", "Licence
  expired on…" and "Licence expires in N days", Export log… writes CSV or JSON, Make credits roll adds a rolling graphic
  of the credit lines after the last clip, and the Media Usage Report gains rights columns.
- **Capture from a capture card, webcam or grabber.** File > Capture… opens a panel with Video device, Format, Sound,
  Quality (High quality H.264, Compact H.264 or ProRes 422), Clip name and Add to bin; Start preview shows the live
  picture and sound level, Record and Stop show dropped frames, size and room left, and Stop imports the file as one
  undo step. A capture cut short by a crash is offered on the next open with Recover and add. Windows only (Media
  Foundation). It has not been run on a physical camera, there is no deck control, and recording is deliberately not a
  command, so no script or agent can start it.
- **Search every project of a production.** Window > Production has a search field above the project list that finds
  sequences and footage in every project by names, comments, descriptions, clip metadata, tape names, file paths, bins
  and marker texts, with Import into this project, Browse the project read-only and Open the project to edit. It does
  not look into transcripts or pictures.
- **SVG, AI and EPS artwork.** File > Import, drops, Import Folder and the Media Browser take .svg, .svgz, .ai and .eps
  as pictures that are drawn again at the size they are shown, so a logo is sharp even at 10x scale. PDF-compatible AI
  files import on Windows with the built-in PDF reader. Partly done: EPS, PostScript-based AI and AI on macOS and Linux
  need Ghostscript installed (not bundled), and that path was only tried against a stand-in program. Timeline filmstrips
  of a vector clip are blank, only the first page or artboard is used and EPS and AI colour is drawn as sRGB.
- **Newer codecs, said plainly.** Help > About Wabcut, Get Properties and `wabcut-cli --version` list what Wabcut does
  with APV, 4:2:2 HEVC, 4:2:2 AV1 and MV-HEVC. APV, 4:2:2 HEVC (8, 10 and 12-bit), 4:2:2 AV1 and the base view of
  MV-HEVC play, and 8-bit 4:2:2 HEVC also exports; APV, 10-bit 4:2:2 HEVC, AV1 4:2:2 and MV-HEVC cannot be exported.
- **A new sequence per clip.** Select two or more clips in the Library, then File > New Sequence per Clip makes one
  sequence per clip in one undo step, and File > New Sequence From All Selected Clips makes one sequence with every clip
  edited in one after another.
- **File nested sequences in a bin.** Right-click a bin > File Nested Sequences Here: Nest… then puts "Nested Sequence
  01" in that bin instead of beside the sequence it came from.
- **Date created, and saved column layouts.** The Library list's column chooser has a Date created column (hidden by
  default) and Saved layouts: type a name and Save, click a name to apply it.
- **Thumbnails in the list view.** Library "…" > Thumbnails in list view (off by default) shows each item's picture in
  the Name column; rows follow the size slider and Ctrl+wheel.
- **Tab through the list's cells.** While a Library list cell is open Tab and Shift+Tab go to the next and previous
  editable cell, and Enter and Shift+Enter to the same column one row down and up.
- **Ingest: Copy and Create Proxies.** Media Browser > Ingest Settings has a fourth action that copies the files,
  imports the copies and makes proxies of them.
- **ProRes RAW gets an ISO.** Source Settings (RAW) takes the ISO the footage was shot at and the ISO to develop at,
  adding log2(develop/shot) stops of exposure. The shot ISO cannot be read from the file, so you enter it, and an older
  Wabcut cannot read such a clip.
- **Drop files or a project anywhere on the editor.** Dropping on the Program Monitor, Effect Controls, Media Browser,
  mixer or title bar imports files or opens a .wabcut as a drop on the Library does, with a "Drop to import" outline.
  Panels in their own windows do not take drops yet.
- **"Open with" Wabcut sends files to the running window.** Files opened from File Explorer reach the Wabcut that is
  already open instead of starting a second one; several selected files arrive as one import, one undo step and one
  toast. With Wabcut closed, Explorer opens Home's New project page with the recording. The installer's optional "Open
  with" checkbox is not there.
- **A folder dropped on the timeline.** It imports into the bin the Library shows (its bins nested in it), and its files
  still stay off the timeline.
- **Productions keep separate preview folders.** A production's preview folder is named "<project name> Preview Files"
  plus 6 digits from the project's path, so two projects with one file name no longer share previews. Old previews
  re-render.
- **Smaller Library fixes.** A flattened Photoshop file with a saved selection stays opaque (the extra channel counts as
  transparency only when the file says so); Remove Unused no longer deletes a master whose subclips are on the timeline;
  project names such as NUL, CON, AUX and COM1 are refused (on every system, because projects travel).

### Export and delivery

- **Smart Render copies H.264 and HEVC.** The Smart Render switch (Export settings > Render) now also appears for H.264
  and HEVC in MP4 and MOV: whole closed GOPs of untouched clips are copied without decoding, and only cuts inside a GOP,
  effects, transitions, titles, retimed clips, gaps and black are encoded. The finished job says "H.264 passthrough: 82
  % copied, 18 % re-encoded" and lists the reasons with their timecodes. In practice only Wabcut's own earlier exports
  made with the same preset copy: camera, OBS and phone files carry other encoder settings and are re-encoded, with the
  reason shown. Captions burned in, picture effects, alpha, HDR, Target Size, scaling and MKV are not smart rendered.
- **Check before export.** Export panel > Check before export… (and the Export button itself while Check before every
  export is on, the default) lists offline media (with Relink…), gaps in the picture (Close gap), flash frames, clips at
  another or variable frame rate, stabilizers not yet analysed, slow effects, a size that does not suit the sequence,
  and captions the file will not show. After Measure loudness it also flags loudness more than 2 LU from the
  destination's -14 LUFS and clipped audio. Each row has Go to, and the dialog opens only when there is something to
  report, with Export anyway. Partly done: text past title-safe and captions over the safe-zone bands are not checked
  yet, and loudness is only checked after Measure loudness.
- **HDR10+ for HEVC exports.** Export > settings drawer > HDR metadata (for HDR and Wide Gamut sequences on HEVC) offers
  None, HDR10+ and two Dolby Vision entries. HDR10+ (PQ HEVC in MP4, MOV and MKV) measures every frame in a first pass
  at half size (about a third more time) and writes per-frame and per-shot metadata; Get Properties shows what a file
  declares, and the pre-export check warns when an export would drop HDR10+ or Dolby Vision metadata. Partly done: Dolby
  Vision is not written. Dolby licenses the format, so Dolby Vision 8.1 and 8.4 are greyed out with the reason and an
  export that asks for them is refused. HDR10+ has not been checked on an HDR10+ display and has no tone-mapping curve,
  and x265 as built is 8-bit, so HEVC Main 10 needs a hardware encoder.
- **Export history.** The Export Queue dialog has a History tab with the last 100 exports that finished, newest first
  and kept between sessions: Show, Open, Export Again (same preset on the project as it is now), a remove button and
  Clear History.
- **Choose where new exports go.** Settings > General > Export > Default export folder: Last used (the old behaviour),
  Next to the project, Next to the media or A folder. The default file name in the Export panel, Quick Export and
  batches follows it.
- **A Streaming (-14 LUFS) loudness target.** Loudness Normalization has a Streaming standard (-14 LUFS, -1 dBTP), and
  the YouTube, Shorts / TikTok / Reels and Twitch tiles show a Normalize to -14 LUFS chip that is remembered.
- **Cloud projects, review links and stock media.** File > Cloud Project and Review Links… has Storage, Cloud project
  and Review links tabs. Add a synced folder (OneDrive, Dropbox, Google Drive, a network share), a WebDAV server
  (Nextcloud and the like) or an S3-compatible bucket (AWS, R2, B2, MinIO, Wasabi). A cloud project has Upload, Open,
  Sync now (a newer remote copy is never overwritten silently; you choose mine or theirs), Take the lock and Open
  revision as copy. A review link is an H.264 + AAC MP4 at 540p, 720p or 1080p that you Upload and publish with an
  expiry; a reviewer pauses on a frame, points at the picture and writes comments, and Get comments turns them into
  review notes (optionally markers) on the right frames in one undo step. Window > Stock Media searches Pexels and
  Pixabay videos and photos with your own free key and Add to project downloads into a Stock bin and records the
  licence, page and credit line in the Rights log. There is no Wabcut server: everything uses storage you own. Only the
  project file is synced, never the media, and there is no real-time co-editing or encryption. It has been run against
  fakes written from the specs, not against a real bucket, server or stock service.
- **Collect Files: handles and which sequences.** File > Collect Files… has a Handles field (0 to 10 s, for Used
  ranges + handles) and a Sequences checklist; the capsule holds the chosen sequences and everything they play, and the
  size estimate follows.
- **Collect Files keeps your folder structure.** A Folders row: All in one folder (default), Project bins or Original
  folders (the folders the files came from; a folder per drive). The manifest records the layout.
- **Collect Files trims everything keyed to the media.** With Used ranges + handles, redactions (including bleep and
  mute ranges), effect and mask keys, Master Clip Effects, markers, Source Monitor marks, the poster frame, subclip
  ranges, the Start Timecode and transcript edits move with the trim, so a blur lands on the same footage in the
  client's copy. Media with AI object masks is copied whole instead.
- **Collect Files never calls a half-copied file done.** A copy counts only when the whole file was read and written;
  otherwise it fails with "only N of M bytes could be read" and the half-copy is deleted. The size estimate is worked
  out in the background ("Working out the size…").
- **A Media Usage Report that cannot run formulas.** A text cell that starts with =, +, -, @, a tab or a carriage return
  gets a leading apostrophe in the Media Usage, marker and transcript CSVs.
- **Exports keep their shape and their names.** Export presets with similar names (two Thai names, "YouTube 4K" and
  "youtube-4k") each keep a file of their own; a typed name with dots ("Stream 2026.11") keeps its tail and gets the
  extension appended; a target-size export counts the audio channels the file really writes, and cancelling or failing
  its second pass leaves an earlier file untouched or keeps the first result with a warning; long chapter titles and
  subtitle cues are cut without splitting a character; a re-exported MXF OP-Atom set is replaced all or nothing; the
  publishing kit lists each thumbnail's own time.

### Interchange

- **Save for an Older Version.** File > Save for an Older Version… lets you pick Wabcut 0.6 or Wabcut 0.2 to 0.5, shows
  live what that version cannot hold (how many places lose it and what the other editor sees instead) and Save a Copy…
  writes the file the way that release reads it, with no key it does not have. The open project is untouched and no
  backup is rotated. The new caption animations and the source marker become "none" and a comment marker, 7.1 becomes
  5.1, and the other 0.7 and 0.6 features are removed and counted. Wabcut 0.1 is not offered (open the project in 0.2 to
  0.5 instead). A file refused by a newer Wabcut now says to use this command.
- **Work area and J/L cuts through interchange.** Alt+[ and Alt+] set the work area's In and Out (the keys existed but
  did nothing), Sequence Settings shows it with a Clear button, Sequence > Set Work Area In / Out and Clear Work Area
  are new, and a thin bar on the ruler shows it. FCP7 XML carries it in Premiere's attributes, FCPXML and OTIO carry it
  as Wabcut metadata, and an EDL reports it dropped. FCPXML now writes a picture and sound with different extents (an L
  cut or J cut) as one clip and reads it back. The FCPXML form follows Apple's documentation but was not checked in
  Final Cut Pro, and Premiere's work-area names were not checked against a real Premiere file.
- **OMF with embedded audio is refused up front.** File > Export > Timeline as OMF with Embed Audio now stops at once,
  before rendering anything, when the audio would push the file past the 2 GB OMF readers accept ("about 4.1 GB… use
  Separate Audio"). The size is an upper-bound estimate, so a file just under 2 GB can be refused.
- **OTIO and EDL files with a byte order mark import.** Files saved by older Notepad or Python's utf-8-sig are read like
  the same text without the mark.

### Shell, settings and keys

- **Back up, move and reset your settings.** Settings > General > Backup and reset has Export…, which writes every
  setting plus shortcut overrides and key sets, export presets, effect presets, graphic templates and the personal
  dictionary to one JSON file; Import…, which shows what the file holds before Replace settings and restart; and Reset…,
  which names the backup it writes first and can keep recent and starred projects and your AI and update choices. Import
  and Reset restart through the normal close path, so the unsaved-changes question appears. On Windows, holding Shift
  while Wabcut starts offers to start with default settings. API keys and other secrets are never exported, imported or
  reset.
- **Choose where autosaves and backups go.** Settings > General > Projects: Autosaves and backups (Next to the project,
  Auto-Save folder or A folder…), Backups to keep (0 to 10, default 5) and Autosave versions to keep (1 to 10, default
  3). File > Recover, Versions… and crash recovery look in every place, and Home's Rename, Duplicate and Move to Recycle
  Bin take the copies along. A new install starts with the Auto-Save folder; an install that already had settings keeps
  "Next to the project".
- **Named versions.** File > Save Version… (a name prompt; a manual checkpoint that is never an undo step) and Version
  History… (opens the Review panel on its Versions page) sit under Save a Copy…. Settings > General > Projects > Keep
  versions every chooses Off, 2, 5 (default), 10 or 30 minutes.
- **Report a bug with your system information filled in.** Help > Report a Bug… shows exactly what it would copy (Wabcut
  version and build, OS, CPU threads, GPU and driver, graphics API, video decoders, Qt, interface size, screens); Open
  on GitHub opens the new-issue form in your browser with that text filled in, Copy System Info copies it and Show Crash
  Reports opens the folder. Nothing in it names a path, your user or your computer, and Wabcut sends nothing itself.
- **Edit > Undo and Redo name the edit.** "Undo Razor", "Redo Ripple Delete". When a text field had the focus the rows
  stay plain.
- **The command palette lists only what it can run.** Commands that need a value you cannot give there, and commands
  only the app's own panels can run, are no longer shown and then failing with "The command failed". Scripts, the
  command line and MCP still see every command.
- **No more dead Premiere keys.** Shift+O in the Media Browser loads the current clip in the Source Monitor, Shift+Left
  opens its Drives and places menu, Shift+Right shows the panel and focuses its list, and Ctrl+L in Effect Controls and
  the Audio Track Mixer toggles loop playback (Ctrl+L stays Link everywhere else). Four Premiere defaults for features
  Wabcut does not have are unbound (Ctrl+Alt+T and Ctrl+Shift+I in the mixer, Ctrl+L and Space in Metadata).
- **Workspace keys follow the tab order.** Alt+Shift+1 to 9 (Opt+Shift on macOS) open the first nine workspace tabs by
  position; Next Workspace and Previous Workspace are new commands with no default key.
- **A Gaming workspace.** Window > Workspaces > New Workspace > From the Gaming Preset (or "+") adds Highlights with
  Shorts and the Library as tabs, the Program Monitor, Effect Controls with Essential Sound and the timeline below.
- **Hide the workspace tabs.** Window > Workspaces > Show Workspace Tabs, or Settings > Interface > Workspace tabs,
  gives the title bar's room back; workspaces stay in the Window menu, on Alt+Shift+1 to 9 and in the palette.
- **The Window menu is alphabetical.** Panels are sorted A to Z after the fixed rows.
- **Copy, print and filter your shortcuts.** Edit > Keyboard Shortcuts has a Changed by me filter, Copy (the commands
  shown, as tab-separated text) and Cheat sheet…, which saves every key in use as a self-contained HTML page you can
  print or save as PDF.
- **A longer first-run tour.** Help > Take the Tour has seven steps: Library, Timeline, the menu bar, the Program
  Monitor, Effects, the command palette and Export.
- **A "Read-only" badge.** When another session holds the project, the title bar shows a Read-only badge and one message
  says so, with Save As… to keep your changes.
- **Real plurals.** The app writes "1 item", "0 items" and "2 clips" instead of "item(s)" and "clip(s)" in the
  interface, command results, import reports and the command line. Scripts that match on message wording may notice.
  English only.
- **The Library has two tabs, Media and Audio.** The Text tab (the title maker) is gone. Make text from Graphics > New
  Layer > Text and the Essential Graphics panel; New Title in the palette still works, and Text Styles are in Essential
  Graphics' Text Style menu and Styles tab.
- **Frame size in Sequence Settings is readable again.** The row reads "Width [1920 px]  Height [1080 px]  16:9": labels
  outside the boxes, "px" as the unit inside, each box sized for five digits, and the frame's shape shown as a reduced
  ratio ("16:9", "9:16", "1:1"). The pairs wrap in a narrow dialog, and so does the Display format row.

### Scripting and plug-ins

- **Python scripting.** From a terminal, a build script or a render machine, `import wabcut` and
  `wabcut.connect("film.wabcut", edit=True, save="exit")` give a session in which every command Wabcut has is
  `session.<group>.<command>(…)`. Edits group in `with session.transaction("name")` as one undo step, and
  `session.render_to("out.mp4", preset=…)` renders. Install it with `pip install <Wabcut folder>/python` (pure Python
  3.11 or newer, no dependencies). Inside Wabcut, File > Run Python Script… lets you choose a .py file, review the
  permissions it asks for (Read, EditSequence, EditProject, MediaAnalysis, Files) and Run; the script works on a scratch
  copy and a clean finish is one undo step, while a failure or Stop applies nothing. Python is not bundled and a script
  is your own code, not sandboxed. A terminal script cannot attach to a running Wabcut.
- **OpenFX video effect plug-ins.** `.ofx` plug-ins in the standard OFX folder, the folders of `OFX_PLUGIN_PATH` or
  folders you add are scanned at start-up and appear in Effects > OpenFX Plug-ins. Double-click or drag one onto video
  clips; its parameters are ordinary Effect Controls rows, keyframable where the plug-in animates. A plug-in runs in its
  own helper with limited rights, so one that crashes or hangs gets a Crashed or Hung badge and is bypassed; a project
  that needs a plug-in this computer lacks opens with a Missing badge, and an export warns before rendering. Windows
  only, CPU rendering only, 8-bit; GPU-only plug-ins, overlays, generators and transitions are not supported. It was
  tried with test plug-ins and the four official SDK examples, not with commercial plug-ins.
- **Extension updates cannot destroy a working extension.** Installing or updating an extension writes the new version
  beside the old one and swaps them only when it is complete; if the disk is full or a file is locked, the old version
  stays listed, valid and running, and the error says why.
- **A hung extension stops faster.** Disabling, uninstalling, switching project or quitting with a runaway extension
  waits half a second, not a second.
- **For scripts and agents.** The command catalog grew with the features above (for example tracks and groups, markers,
  templates, AI video effects, transcripts, captions, sources, VR, spectral repair, rights, consolidate duplicates). The
  command line's MCP server also has an optional `--transactions` mode with an `edit_transaction` tool and an
  `--audit-log` file. Time values beyond about 31 days are now refused with "time out of range".

### Platforms

- **Windows on ARM64 (preview).** There is a build recipe (a Windows ARM64 PC can build and test natively; an x64 PC can
  cross-build), an ARM64 installer (MSI and portable ZIP) built by the release pipeline and its own CI workflow. Nothing
  has been built or run on ARM64 hardware yet. On ARM64 local transcription and local AI, audio plug-ins, Content
  Credentials and the managed local assistant are off; exports use the software encoders. The ARM64 build does not
  install updates itself: the update dialog sends you to the release page. Windows x64 is unchanged.
- **Linux: visible test numbers, a smoke-tested tar.gz and a .wabcut file type.** Every Linux and macOS test run now
  writes its totals, every skipped test with its reason and the known gaps into the job summary; the release's tar.gz is
  unpacked and checked in a fresh folder (desktop file, `wabcut-cli --version`, a tiny render and the app until its
  first frame under Xvfb). Linux registers a MIME type for .wabcut files, and a new Linux page covers packages, window
  systems (X11, XWayland, Wayland), graphics, hardware decoding, audio and key storage. Seven tests still fail on Linux
  and four on macOS for causes nobody has diagnosed (no Linux machine was available), and nothing here has been run on a
  real Linux desktop.
- **macOS: Finder integration and a signing pipeline.** Wabcut declares the .wabcut file type and Open With for movies,
  audio and images, and catches double-click, Open With and Dock drops. The release pipeline can sign the app with a
  Developer ID, apply the hardened runtime, notarize and staple the app and the DMG and check the result as Gatekeeper
  does, but none of that has run: it needs the owner's Apple Developer ID. Until it does, the macOS build stays the ad
  hoc preview (open it with Open Anyway). Audio Unit hosting is not done.
- **Taskbar jump list (Windows).** Right-click Wabcut's taskbar button for Starred and Recent projects (at most 10); a
  click opens the project in the running Wabcut. Settings > General > Projects > Show recent projects in the taskbar
  jump list (on by default). The Windows calls have been run only against a stand-in, so it needs one look on a real
  taskbar.
- **Snap Layouts and the window menu (Windows).** Right-click an empty part of the title bar or press Alt+Space for the
  window's system menu, and hover the maximize pill for Windows 11's Snap Layouts flyout. Tested with synthetic Windows
  messages only: the flyout has not been seen on a real Windows 11 desktop.

### AI settings and providers

- **Settings > AI is reorganised.** The AI features switch sits beside the AI title with one line saying what runs where
  ("On this computer: Assistant, Translation. Cloud: Voice (ElevenLabs). Not set up: Images, Video, Sound effects.").
  The tabs are Features, Providers, Local models, Privacy and Usage. The new Features tab (it replaces the old Models
  tab) has one card per AI feature showing what it uses now, a picker with a search box, Automatic first and models
  grouped by provider with the local ones first, and an Add a provider button that opens Providers filtered to that
  feature. Models that cannot run are greyed with the reason.
- **A model for each AI feature.** Seven features each have their own default: Assistant, Transcription, Translation,
  Voice, Images, Video and Sound effects. Automatic is the default and always prefers a local option (models that run
  now, then local models to download, then usable cloud models), so saving a key never moves the assistant to the cloud;
  you pick a cloud model yourself. Your old chat and transcription choices are migrated. The Transcript panel, caption
  translation, Dub, Generate, the assistant, agent, rough cut, takes, autopilot, shorts and describe all follow the
  routing.
- **35 new AI providers.** Nineteen cloud providers join the chat list: xAI (Grok), DeepSeek, Together AI, Fireworks AI,
  Cerebras, Cohere, Azure OpenAI / Foundry, Amazon Bedrock, Moonshot AI (Kimi), Alibaba Cloud Model Studio (Qwen), Z.ai
  (GLM), MiniMax, NVIDIA NIM, Hugging Face Inference Providers, SambaNova, DeepInfra, Nebius Token Factory, Novita AI
  and Cloudflare Workers AI. Four run on your own computer or server with no key and no consent prompt: vLLM, LiteLLM
  proxy, LocalAI and Jan. Mistral gets chat models. Twelve more serve one job each: DeepL API Pro and Free and Google
  Cloud Translation (captions), Speechmatics and Gladia (transcription), Cartesia and Fish Audio (voice), Stability,
  Black Forest Labs, Ideogram, fal.ai and Replicate (images, video and sound). The catalogue now holds 49 providers and
  102 models, each with a description, a Get a key link, a data-terms link, a one-line data policy and current prices
  where the vendor publishes them. Entries built from vendor documents without a check are marked unverified (NVIDIA,
  Nebius, Cloudflare, LiteLLM and some models). Perplexity, Luma, Hume, Rev.ai and OpenAI's own video are not there:
  their APIs ended or cannot be limited to the vendor's hosts.
- **More cloud transcription, voice, translation, image, video and sound services.** Transcription can use OpenAI
  (whisper-1 and a speaker-label model), Groq, Mistral (Voxtral), Together, DeepInfra, Speechmatics and Gladia (the last
  two delete your audio from their servers once the transcript has been read). Voices: Deepgram Aura-2, Cartesia Sonic
  and Fish Audio, and any OpenAI-compatible speech server. Images: OpenAI GPT Image, xAI Grok Imagine, Together FLUX.2,
  Stability, Black Forest Labs FLUX, Ideogram, fal.ai and Replicate. Video: xAI Grok Imagine Video and fal.ai Kling 3
  Pro. Sound effects: fal.ai ElevenLabs Sound Effects v2 and MMAudio V2, beside ElevenLabs. Only OpenAI's whisper-1
  gives true word times; others give segment times spread over the words. Downloaded pictures come only from the
  vendor's own HTTPS hosts.
- **Add your own AI provider.** Settings > AI > Providers > + Custom provider takes a name and address, a protocol
  (OpenAI-, Anthropic- or Gemini-compatible), a Bearer key, a header of its own or no key, per-feature paths and up to
  20 extra headers; model ids can be typed or listed with Fetch models, and Test connection works before anything is
  saved. An OpenAI-compatible provider can serve chat, translation, transcription, speech, images, video and sound
  effects; the other two serve chat and translation. A custom address on this computer counts as local (no consent
  prompt, works offline and under the no-cloud lock); any other address is cloud. Custom providers show "Price not
  tracked" and are not covered by the monthly spend cap. Removing one removes its key, consents, cap and models. Headers
  that carry credentials are refused: the key goes in the key field. Fetch models was only tried against error cases,
  not a real server's list.
- **A proper Providers page.** Settings > AI > Providers has a search box, filter chips (All, Connected, the seven
  features, Local, Custom) and a list grouped Connected, Your custom providers, Cloud providers and On this computer or
  your server. A provider's detail has its description and links, an API key card (masked, last four shown), a Server
  address card, Models with Use for <feature> on any model and Fetch models, Your data and Spending. Keys are checked
  when saved except where the service offers no free check (Azure, Bedrock, NVIDIA, SambaNova, Ideogram, fal.ai), which
  say so. Models whose vendor publishes no price show "Price not listed" instead of "Free" and are booked at $0, so a
  monthly cap does not cover them.
- **Local models keep working offline.** Rough cut, Shorts, Autopilot, Translate and Dub and Generate now run a model on
  this computer even with Offline mode or the no-cloud lock on; cloud models are still stopped. The assistant's
  data-flow badge judges cloud or local by the address you set.
- **Dub uses your voice provider.** The Dub dialog's cost estimate and the assistant's dub proposals use the voice model
  chosen in Settings > AI > Features, not a fixed ElevenLabs one; any OpenAI-compatible speech server accepts any
  one-line voice name, and the free voice-id field shows for every model that takes its own voice ids.
- **Safe downloads from your own server.** When a custom or self-hosted provider answers an image or video request with
  a link, Wabcut downloads it only from the provider's own address; plain http only for a local address.
- **Add a model id of any kind.** On OpenAI-compatible providers, Add model id offers transcription, voice, image, video
  and sound models, not only chat. A model you add to a cloud provider for image, video or sound has no known price, so
  Generate refuses to run it and says why.
- **A library of local models.** Settings > AI > Local models shows what this computer has (GPU and its free memory,
  RAM, CPU threads) and has Recommended, Browse, Transcription and Downloaded sections. Recommended lists tested
  assistant models (the Qwen3 tiers and Phi-4 mini), the one this computer runs best first, each with its benchmark
  score, size, licence and how it would run here ("Runs on the GPU", "Runs on the CPU (slower)" or "too big"); Download
  and use fetches the model and its llama-server build and starts the assistant. Browse searches Hugging Face GGUF
  models by task, with eight sort orders, a Fits this computer switch, filters (size, quantization, language, publisher,
  open licences only) and a model card with exact sizes and a quantization picker; other licences need an explicit "I
  read and accept" switch, and gated repos need a Hugging Face token. Transcription picks any whisper.cpp model or
  Automatic. Downloaded shows the storage folder and every model with Use and Remove, and Advanced sets context, GPU
  layers and CPU threads. The download queue has Pause, Resume, Try again and Cancel, resumes where it stopped, checks
  size and SHA-256 and refuses a drive without room. Chosen defaults survive a restart. The managed engine runs on
  Windows x64 only, and the library has been tried with recorded fixtures, not the live Hugging Face service.
- **Settings search and small windows.** Settings search finds "What each AI feature uses" and "Custom AI provider";
  text fields in AI settings have a visible fill; Privacy and Usage stack their columns in a small window. The
  Transcript panel's Set up opens Settings > AI > Features on the Transcription row.

### Fixes

- A video track that is soloed or hidden no longer lets an offline file on a track that does not render stop an export,
  and the render bar, the pre-export check and the media usage list ignore those tracks too.
- Copying an audition clip into another project, or Alt-dragging a copy, no longer produces a project that cannot be
  saved.
- A time such as 99999999:00:00:00 in a time box, or one sent through MCP or the command line, is refused with "time out
  of range" instead of wrapping to a wrong position; the bound is 31 days.
- Redaction blurs on mirrored footage land on the right side in thumbnails, filmstrips, search and Shorts pictures.
- Tracked graphics land in the right place on rotated and mirrored footage (Track Motion and the tracker).
- In trim mode, Shift-clicking a clip's edge on another track adds an edit point again instead of dropping the earlier
  ones.
- A failed autosave names your project ("Autosave failed: cannot create the autosave of Film.wabcut") instead of a
  temporary path.
- Save As and Save a Copy keep an upper-case .WABCUT extension instead of adding a second ".wabcut".
- New folders and projects inside a production refuse Windows device names, as Home does.
- The export queue is written only when something changes, not ten times a second while a job reports progress.
- Recovery posters are cached instead of being rendered again on every scan.
- Switch labels in Effect Controls ("Nearest Neighbor", "Uniform Scale") show whole in a narrow panel, and the "Aa"
  sample of a dark text style no longer vanishes on its card.
- The text of the caption look gallery uses the theme's colours, and the case option reads "Uppercase".
- Counts read correctly for one item in the pre-export check, Collect Files, the import report and the Library's
  delete-master-effects sentence ("1 clip", "1 file was not imported").

What is not done or not checked yet is under [Known limitations](#known-limitations).

## Known limitations

What needs the owner of the project:

- Windows: the packages are not code-signed yet, so Windows SmartScreen may warn on the first run (choose More info,
  then Run anyway). Updates do not depend on it: their own signature and the installer's checksum protect them.
- macOS: the signing and notarizing steps are written but have never run, because they need an Apple Developer ID
  (certificate and notary credentials). Until they do, the macOS download is the same ad hoc preview as before and asks
  you to allow the first start (System Settings > Privacy & Security > Open Anyway). A real Mac's Gatekeeper verdict and
  real Finder events have never been seen.

What needs a vendor's software, a licence or a web engine:

- Video cards: output to a Blackmagic DeckLink, AJA or NDI device needs the vendor's SDK and is not supported. Video
  Output Settings drives a second display, and its output size is the display's own.
- Dolby Vision is not written: Dolby licenses the format, so Dolby Vision 8.1 and 8.4 are greyed out and an export that
  asks for them is refused. HDR10+ is written, and has not been checked on an HDR10+ display.
- Camera gyro: GoPro and Sony only. RED and Blackmagic RAW need SDKs that cannot be linked into a GPL program, DJI has
  not published its gyro layout and Insta360's format is undocumented; those cameras are recognised and refused with a
  reason.
- OGraf: Wabcut has no web engine and never runs scripts, so an OGraf package without a Lottie version is refused with a
  message that lists its fields. Playing HTML graphics needs a web engine that is not included.
- Ghostscript is needed for EPS, PostScript-based AI files and AI files on macOS and Linux; it is not bundled.

What was only tried with stand-ins, scripted replies or generated media, and never against the real thing:

- Capture: real device listing and format reading were checked on one computer, and the read path against a generated
  1080p60 clip; no physical camera or capture card has recorded through it.
- Cloud projects, review links and stock media: tried against fakes written from the specs of S3, WebDAV, Pexels and
  Pixabay, never against a real bucket, server or service. A first run against your own bucket, server and key is the
  real test.
- AI providers: every cloud provider, transcription, voice, translation, image, video and sound service, Fetch models
  and the Hugging Face model library were tried with scripted replies and recorded fixtures only. The provider list was
  built from vendors' documents read in early November 2026, not from live calls.
- The audio description's picture-reading step has only run with a scripted model, and mixed-language transcription has
  not been tried with a real speech model.
- A real 4 GiB voice-over take was not recorded (the limit was checked by arithmetic and a lowered cap); the
  100,000-overs loudness path was tested with a lowered cap; a real Sony gyro file, a real Ghostscript run and a 7.1 AAF
  opened in Pro Tools, Media Composer or Resolve were not available.
- The Windows taskbar jump list, Snap Layouts and Match Refresh Rate were run only against stand-ins or synthetic
  messages: look at them once on a real desktop.
- Linux, macOS and Windows on ARM64: this release's features were built and run on Windows only. The preview builds for
  the other systems are checked automatically on GitHub's machines, and nobody has edited with 0.7 on a real Linux,
  macOS or ARM64 computer. Seven automated tests still fail on Linux and four on macOS for causes nobody has diagnosed.
  The AI video effects were run with the real models on one Windows machine only.
- The 0.7 changes were built and tested one area at a time on a Windows computer: none has been through a long release
  run with real projects yet, so keep backups of your projects.

In the editor:

- Project files: 0.7 saves schema 19, like 0.6. Wabcut 0.5 and older refuse a project saved by 0.6 or 0.7; Wabcut 0.6
  opens a 0.7 project and loses what it does not know the next time it saves, or refuses it when it uses a name 0.6
  lacks. Use File > Save for an Older Version… to hand a project to an older Wabcut. Track label colours, locks, clip
  notes, pins, track groups, rights notes and a ProRes RAW ISO are saved as optional data an older Wabcut ignores or
  cannot read. A project made before 0.7 keeps the information it read earlier, so mirrored footage needs a relink to be
  flipped.
- Timeline: a track group cannot be dragged as a whole, has no fader of its own without a submix and is not nested.
  Locked clips make sequence-wide helpers and AI operations fail as a whole when they would touch one. Slip trim has no
  four-up Avid display. Markers dragged with Track Select jump on release. The pacing strip looks at shot length,
  stillness, silence and slow speech only. Render and Replace cannot do time-remapped, object-masked, multi-camera or
  audition clips and is not kept across a restart.
- Effects: Motion Blur does not blur a retimed clip's own pictures, nested or multi-camera clips or opacity. Track
  Motion has no corner-pin tracking, and a lost frame ends the track. Scale Width is one number with no flip. The
  Transform effect's Sampling choice is ignored (use Nearest Neighbor Scaling). ColorChecker matching needs the Classic
  chart and has no colour-matrix stage, so it reaches about 2.5 to 2.6 delta E on the photos tried. Effects on a video
  track cannot be dragged onto a header. The Graph Editor shows one clip, does not snap values and is not saved with the
  project. Skin Smoothing finds skin by colour only.
- AI video effects: they work one frame at a time with no temporal model, so edges can shimmer on a moving face and a
  very noisy clip can flicker. Processed pictures are kept losslessly on disk with no size limit (estimated at tens of
  GB per minute of 4K, not measured on footage); Clean frees them. A downloaded model cannot be deleted from the
  interface. Stills, titles, nested sequences and transition handles are not processed. AI Restore has no Render and
  Replace and no cloud provider. AI Eye Contact was not tried on a real speaker reading notes, and lowered lids stay
  lowered. Morph Cut's face alignment was tried on a synthetic talking head only. The headers of the longest AI effect
  names overflow the property column at the default width.
- Text and captions: Optical Kerning is for graphics, not legacy title clips, and covers Latin, Greek and Cyrillic. The
  Sources panel has no Cite action on a transcript line and one bibliography style. The font picker counts a variable
  font as its named instances. The emoji inserter is not there. Animated caption looks do not carry over into Convert to
  graphics. A nested sequence still shows only its own first caption track. A transcript's custom-word hint holds about
  480 bytes locally.
- Audio: 7.1 upmixed from 5.1 leaves the rear pair silent, a 7.1 AAF is only read back by Wabcut, immersive beds and
  8-channel output devices are not supported and AC-3 carries 5.1 at most. Spectral repair has no clone or per-channel
  edit and runs on the player's thread (Heal over about 0.9 s fills from above and below). Dynamics Processing has no
  sidechain or spectrum, and other editors skip it in FCP7 XML, FCPXML and OTIO. EQ Match's top two bands are 1 to 3 dB
  short. The loudness radar has no per-track or per-clip view.
- Library: Reload Footage does not look for a moved file, and a clip that plays a stream the new file lacks makes the
  whole reload refuse. Consolidate Duplicates does not merge items that read differently. The rights log is per media
  item, not per clip, and has no Library column. Vector clips show blank filmstrips. Newer codecs are decode-only where
  no encoder is in the build (APV, 10-bit 4:2:2 HEVC, 4:2:2 AV1, MV-HEVC).
- Export: Smart Render copies H.264 and HEVC only when the source's headers match, in practice Wabcut's own earlier
  exports. The pre-export check does not look at text past title-safe or captions over the safe-zone bands. HDR10+ has
  no tone-mapping curve, and x265 as built is 8-bit, so Main 10 needs a hardware encoder. Non-square-pixel sequences
  still draw titles, graphics and captions in sequence pixels, and the Trim view, multicam grid and Export Frame stills
  use the stored aspect. Cloud projects sync the project file only, not media, and have no real-time co-editing or
  encryption; S3 uploads over 4.5 GB are refused.
- Interchange: the FCPXML form of an L or J cut follows Apple's documentation and was not opened in Final Cut Pro;
  Premiere's work-area attribute names were not checked against a real Premiere file; track label colours and locked
  clips are not written to interchange files. Save for an Older Version cannot target Wabcut 0.1.
- Shell and keys: Shift+Space is Wabcut's own key and was not checked against Premiere's published list; the Classic
  label colours are an approximation. Next and Previous Workspace and most of the new commands have no default key. The
  installer has no "Open with" checkbox.
- Scripting: Python is found at run time (3.11 or newer) and is not bundled; a script is your own code and is not
  sandboxed. OpenFX runs on Windows only, on the CPU, in 8-bit, with no HDR frames; titles, graphics, nested clips and
  track stacks do not run OpenFX effects, and no commercial plug-in has been tried.
- AI settings: calls to models with no published price are booked at $0, so the monthly cap does not cover them or
  custom providers. Azure and Cloudflare need their address set first. Nebius lists no models until you add a key. The
  local model library's managed engine runs on Windows x64 only, and its queue is not kept after a restart.
- Windows on ARM64: local transcription and local AI, audio plug-ins, Content Credentials and the managed local
  assistant are off, and the build never updates itself.
- The other [0.6 known issues](release-notes-0.6.md#known-issues) that this list does not change still apply.

## Privacy

Nothing leaves your computer unless you turn on a cloud feature with your own key, and every cloud call is shown with
its cost first. Redacted regions and bleeped words are never uploaded in clear. Crash reports stay local until you
choose to share them, and never contain API keys.

The 0.7 features work on this computer: the AI video effects (Wabcut downloads their models only when you choose
Download, with the licence shown first, and Offline mode and the no-cloud lock block the download), the Local models
library (it contacts Hugging Face only when you search or download, and sends your Hugging Face token to huggingface.co
alone), OpenFX plug-ins, spectral repair and EQ Match, the pacing strip, Render and Replace, the export history and
settings backups (a settings bundle never carries API keys). Cloud projects, review links and stock media use only
storage you add and the Pexels and Pixabay services with your own key; credentials go to the system key store, media is
never uploaded, and Offline mode and the project's no-cloud lock stop every upload, publish, sync and stock search. The
audio description sends one to three small frames of each gap and the words around it to your own chat model only after
you consent and see the cost, or uses a model on this computer. A custom AI provider at an address other than this
computer counts as cloud, with consent, the offline gate and the spend guard. Report a Bug opens GitHub in your browser
with text you can read first and sends nothing itself; recording from a capture device is not a command, so no agent or
script can start it. A Python script is your own code and has the permissions you approve. The update check is
unchanged: it contacts GitHub only when you turn on "Check for updates automatically" or choose Check for Updates…, and
it asks only for the latest release's update information, with Wabcut's version as its user agent. An update you choose
to install is downloaded from GitHub.

## Thanks

Thank you to everyone who reported the problems behind these cards and who edits real projects with the alpha: the
reports and the pilot projects decide what comes next. If something here does not work as described, or a menu row says
why it cannot run and you disagree, [open an issue](../../issues) and say what you were trying to make.
