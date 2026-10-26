# Wabcut 0.5: release notes (0.5.0, 0.5.1)

Windows 10/11 x64, with preview builds for Linux x64 and macOS on Apple silicon. Free and open source
(GPL-3.0-or-later). No account, no subscription.

Wabcut 0.5 finishes everything that was left on the roadmap's top-priority list, and makes Wabcut use the right GPU on
laptops with two of them, thanks to the report in issue #10. Caption tracks are rows on the timeline where you trim,
move and split captions, and nested and multicam clips show the waveform of their mix. The Audio Meters panel sits next
to the timeline, and dragging the playhead plays the sound under it. Scene Edit Detection is in the Clip menu, dead time
is reviewed stretch by stretch before anything is cut, OBS chapter markers come in as clip markers, and the transcript
of a source clip picks soundbites. Wabcut can also check for updates and install them, once you turn that on, and the
[pilot](pilot.md) asks for real projects edited with it. [Premiere parity](premiere-parity.md) now compares 109 areas:
Wabcut is equal in 65, better in 9, partial in 34 and missing in 1.

The project file format is still schema 18: Wabcut 0.5, 0.4, 0.3 and 0.2 open each other's projects, and Wabcut 0.5
opens 0.1 projects. 0.5 adds nothing to the project file. Chapter markers are ordinary clip markers, which older
versions show too, and the new choices (the caption rows, audio scrubbing, the Graphics card, updates, the Scene Edit
Detection and dead-time options) are settings of Wabcut on your computer. Which caption rows are targeted is not saved.

## 0.5.1 (26 October 2026)

- **Zoom the keyframe lane in Effect Controls**, as in Premiere's Effect Controls timeline: Ctrl+wheel (or Alt+wheel)
  over the lane or its ruler zooms in and out at the pointer, down to five frames across the lane, and Shift+wheel pans
  it. A zoomed lane has a zoom bar under it: drag it to move along the clip, or double-click it to see the whole clip
  again. The ruler shows the first and last frame on view, keys and Bezier handles are dragged as before (a zoomed drag
  is finer), the value graph is drawn as finely as unzoomed, and the lane pages along with the playhead during
  playback. Selecting another clip shows all of it. A plain wheel still scrolls the rows.
- **Fix:** an animated parameter's value graph draws the line of linear keys and of flat stretches again; it could
  show the keys with no line between them (Bezier curves were fine).

Projects: unchanged (schema 18). Updating from 0.5.0: Help > Check for Updates… once updates are on (Settings >
General > Updates), or install the new MSI over it.

## 0.5.0 (26 October 2026)

Projects: still schema 18, so Wabcut 0.5, 0.4, 0.3 and 0.2 open each other's projects (as with 0.4, a sequence with
Ripple Sequence Markers off has it on again after 0.3 or 0.2 saves it).

### Timeline
- **Caption tracks on the timeline:** each caption track of the sequence is a row above the video tracks (C1 on top),
  with its captions as blocks that show their first line. A hidden track's row is dimmed. Timeline display settings
  (the sliders button) > Show > Caption tracks turns the rows off.
- **Edit captions there, as clips:** a click selects a caption (and moves the playhead to its start once you let go;
  the Captions page shows it), and a double-click opens it in the Captions page ready for typing. Drag an edge to trim a
  caption or its middle to move it: it snaps to clip edges, the playhead, marks, markers and the other captions while
  Snap is on, a readout shows the change, and Esc cancels. A caption stays between its neighbours unless the track's
  style has Overlapping captions on. The Razor splits a caption where you click, Delete removes the selected one, and
  the right-click menu has Split at Playhead, Delete, Edit Text and Select in Captions. Each edit is one undo step.
- **Caption track headers:** the C1 chip targets the row for Add Edit, the eye shows or hides the track (one undo
  step), and a click on the header shows that track in the Captions page.
- **Ctrl+K and captions** follow Premiere's rules. A selected caption under the playhead is split on its own; selected
  clips under the playhead are cut on their own, with the captions left whole; with nothing selected, the captions
  under the playhead on the targeted caption rows are split together with the clips, in one undo step. No caption row
  is targeted when Wabcut starts, so Ctrl+K cuts no caption until you target a row. When the selected caption cannot be
  split there, a message says why.
- **Waveforms on nested, multicam and merged clips:** their audio clips show the waveform of the mix they play: clip
  gains and Volume keyframes, effects, track faders and pan, transitions and a nested sequence's master. The mix is
  rendered in the background and drawn as it comes in; until then, or when media is offline, the clip shows the loudest
  of the clips inside it, shaded while it builds. An edit inside the nested sequence renders it again.

### Audio
- **The Audio Meters panel** (Window > Audio Meters): the level of the mix, a bar per channel (two for stereo, six for
  a 5.1 master) with peak, RMS and a peak hold, a 0 to −60 dB scale, the highest peak as a readout, and a clip light
  that turns red at 0 dBFS and clears when you click it. Workspaces made or reset from the Edit preset dock it right of
  the timeline. A workspace saved before keeps its layout: Window > Audio Meters (or Window > Reset Panel Layout) adds
  it. Like any panel it can be a tab, float in its own window or close; a closed timeline comes back left of it.
- **Audio scrubbing** (Shift+S, Sequence > Audio Scrubbing, Settings > Timeline & editing > "Play audio while
  scrubbing"; on by default, as in Premiere): while playback is stopped, dragging the playhead in the ruler or on the
  Program Monitor's time bar, and stepping frames, play a short piece of the mix at the new frame (60 ms, faded in and
  out). It is the mix you export: mute and solo, faders and pan, effects, submixes and the master. Dragging back plays
  the sound backwards, as tape does; a frame step plays forward, so you hear that frame's word. The Source Monitor
  scrubs its clip's own sound the same way. The picture and the transport do not move, and starting playback silences a
  scrub at once.

### Clip tools and gaming
- **Scene Edit Detection…** in the Clip menu, the timeline's clip menu (after Speed/Duration…) and the command palette,
  as in Premiere: it finds the shot changes inside the selected video clips and puts a cut at each one, a marker, or
  both, in one undo step. Choose the Detector (Cuts, fades and dissolves; Hard cuts only; Hard cuts in fast motion;
  Fades through black) and the Sensitivity. It runs on this computer, with its progress and a Cancel that changes
  nothing; then a message says what it did ("Split 2 clips at 3 cuts and added 3 markers"). Linked audio is cut too;
  nested and multicam clips are cut but get no markers. Without a video clip selected, the rows are greyed out and say
  why. The options are remembered.
- **Review dead time before it is cut:** Moments > Find dead time now opens a review in the Highlights panel (its
  scissors button opens it too; the Moments popover's Remove, which cut without showing what, became Review…). Set the
  Sensitivity, Shortest stretch, Keep at each end and Only In to Out, then Find dead time. Each stretch gets a row with
  a thumbnail, its time, its length and a remove switch, and the switched-on stretches are red bands on the timeline.
  Click a row (or press Enter) to jump there; Up and Down move, K keeps, Delete removes, Space flips, and All and None
  switch every row. Remove N stretches takes out only the switched-on ones on every unlocked track, keeping the
  handles, in one undo step. When you change an option after a find, or the sequence's clips change, the review says so
  and Remove waits for a new find, so an old list is never cut. The options are remembered.

### Import and transcript
- **Chapter markers from recordings:** the chapters of a recording (OBS Studio's Add Chapter Marker hotkey in hybrid
  MP4 or MKV, and the chapters other apps write into MP4 and MKV files) become cyan clip markers on the media you
  import, named after the chapter ("OBS marker 2" when it has no name), in the same undo step as the import. Every way
  in brings them: File > Import and Import Folder…, drops on the Library, the timeline or the Source Monitor, and the
  Media Browser's import and Ingest copy. A message says how many came in ("3 chapter markers became clip markers.").
  Preferences > Media > "Chapter markers in recordings become clip markers" (on by default) turns it off. A lone
  chapter at the start (the whole file) is left out. Chapter titles are cleaned (broken characters and control
  characters), and the highlight finder's chapter hints get the same cleaning.
- **The source clip's transcript:** the Transcript tab has a Sequence | Source switch. Source shows the words of the
  clip in the Source Monitor, with source timecodes and speakers, and highlights the word at the Source Monitor's
  position. Click a word to go there. Select words (a click, Shift-click, a drag or a Find match) to set the source In
  and Out around them, then Insert (`,`) or Overwrite (`.`) puts that soundbite into the sequence, as Premiere's Text
  panel does on a source clip; Copy copies the words. A recording with several audio streams shows one at a time
  (A1 · Game, A2 · Mic). The project's corrections apply, and words under an audio redaction read [bleep]. A clip
  without a transcript has Transcribe this clip, which transcribes that clip alone.

### Hybrid laptops and the graphics card (issue #10)
Thanks to the report in issue #10, Wabcut uses the fast GPU on laptops that have an integrated GPU and a discrete one:
- **Driver hints:** wabcut.exe asks the NVIDIA Optimus and AMD switchable-graphics drivers to start it on the discrete
  GPU.
- **The high-performance GPU first:** Wabcut ranks the GPUs in Windows' high-performance order (discrete first;
  software adapters are left out) and previews, decodes, renders and exports on the same one. On the laptop in the
  report (an AMD Radeon 780M listed first, an NVIDIA RTX 5060 Laptop GPU second), exports now use NVENC instead of AMF.
- **Settings > Playback & performance > Graphics card:** Automatic (the high-performance GPU, unless the Windows
  Graphics settings choose one for Wabcut) or one GPU of this computer, such as the integrated one to save battery. A
  new choice reaches exports at once and the preview when Wabcut restarts, and the row says which GPU is in use and why.
  Windows settings next to it opens the Windows Graphics settings. A chosen card that is gone falls back to Automatic.
  When you choose a GPU, decoding stays on it and does not wake the other one.
- The Performance check (Settings > Media & cache…) has a Graphics card row: the GPU in use, why, and the other GPUs.
- On the Linux preview, the export encoders follow the same ranking (NVENC before the integrated GPU's VA-API); the
  system picks the preview's GPU there.

### Updates (opt-in)
- **Off by default.** Settings > General > Updates > "Check for updates automatically" checks once a day at most and
  tells you when a new version is out, with Details; Skip This Version keeps it quiet about that one. Help > Check for
  Updates… (also in the command palette and next to the switch in Settings) checks at once.
- **Signed and checked:** the update information is signed, and Wabcut checks its signature with a key built into the
  app. The installer's size and SHA-256 checksum are checked when it downloads and again just before it runs, and
  Wabcut keeps the file locked in between. Anything that does not match is refused and deleted.
- **Installed when you quit:** on Windows, when Wabcut was installed with the MSI, choose Download and Install, then
  Install When I Quit or Quit and Install. Quitting asks about unsaved changes and running exports as usual (Quit and
  Install waits while exports run). Wabcut then installs the update into the same folder, keeps the .wabcut file
  association and opens again. If you stay open after all, the install is called off and a message says so; Later
  keeps the download for next time.
- The portable ZIP, Linux and macOS show the new version and open its release page.
- **0.5.0 is the first version that can update itself**, so the first in-app update will be to the version after it.
  From 0.4 or older, download and install 0.5.0 as before (the MSI replaces the installed version).

### Code signing
- The release workflow can now sign every program file and the MSI (with Azure Trusted Signing or a certificate),
  check the signatures, and sign the update information. **The 0.5.0 packages are still unsigned**: there is no
  certificate yet, so Windows SmartScreen may warn on the first run (choose More info, then Run anyway). Updates do not
  depend on it: the update information's own signature and the installer's checksum protect them, and once the
  packages are signed, Wabcut also requires the downloaded installer to carry that publisher's signature.

### The pilot
- **Edit a real project with Wabcut:** the pilot guide (`docs/pilot.md`) has four projects to pick from (a long gaming
  video, a Short, a video essay, a documentary), each with a checklist, and says what to write down. Report with the new
  **Pilot report** form (Issues > New issue > Pilot report); single problems can go in the new **Bug report** form.
  Reports are fixed in order of harm: crashes and lost work first, then wrong exports.

### Scripts and agents (CLI and MCP)
- `transcript.mediaWords` (new, read-only) gives a media item's words in source time, with the project's corrections
  and redactions; with `source.setMarks` and `edit.insertFromSource` an agent edits in a soundbite the same way.
  `media.importCameraClips` brings the chapter markers and counts them (`markers`). The keymap has
  `clip.sceneEditDetection`, `window.audioMeters` and `transport.toggleAudioScrubbing` (Shift+S).

### Fixes
- Splitting a caption on a track with Overlapping captions on failed when a later caption started inside the one being
  split. The halves now keep the track's captions in time order.
- Cancel in Find dead time did not stop the measuring: 0.4 measured on in the background and threw the result away. It
  stops now, and a Find right after Cancel starts once it has. A Remove that fails keeps the list.
- In the light theme, warning text in the Moments popover (why a move did not apply, a dead-time error) was amber at
  about 1.5:1, hard to read. Warning and error text now uses darker colours in light themes (at least 4.5:1); bars,
  icons and fills keep theirs.
- On laptops with two GPUs, Wabcut used the GPU Windows lists first, often the integrated one, and its encoder (issue
  #10, see above).

What is not done or not checked yet is under [Known issues](#known-issues).

## Known issues
- The 0.5 changes are verified by automated tests only, in hidden windows. None of them has been through a release run
  with real projects yet: keep backups of your projects. Caption drags and scrubbing drags were tested with simulated
  events, not with a real mouse.
- Hybrid laptops: nobody has tried the issue #10 changes on a real hybrid laptop yet. If the Performance check still
  names the integrated GPU, or exports still use its encoder, please say so in the issue. With two GPUs of the same
  maker (two NVIDIA cards, or an AMD processor's graphics next to an AMD card), the export encoder picks its own GPU of
  that maker. Vulkan on Windows (`--rhi vulkan`, not the default) ignores the Graphics card setting
  (`QT_VK_PHYSICAL_DEVICE_INDEX` picks a GPU by hand). On Linux and macOS the system picks the preview's GPU, and the
  setting only orders the export encoders.
- Updates: updating from inside Wabcut starts with 0.5.0 (0.4 and older cannot), so the first in-app update is to the
  release after 0.5.0. The Windows
  installer's upgrade from a downloaded update has not been run end to end outside the tests (they cover everything up
  to starting it). Linux and macOS cannot check the update information's signature yet, so they only name the version
  and open its release page. A release without its signed update file makes Check for Updates say "No update
  information was found for the latest release."
- Code signing: not signed yet (see above). Signing on GitHub's machines with Azure Trusted Signing has not run yet, and
  a new certificate still meets SmartScreen warnings until downloads build its reputation.
- Audio: the first scrub, like the first Play, opens the audio device on the main thread, so the window can stall
  briefly that once. The Audio Meters panel has none of Premiere's panel-menu options (dynamic peaks, the dB range,
  horizontal meters, solo buttons). With the timeline closed, the meters span the bottom of the window. A Ctrl+click
  earlier on the Source Monitor's waveform plays backwards (a plain click and the time bar play forward). A nested or
  multicam clip at a speed other than 100 % is still not heard, so its waveform is flat.
- Captions on the timeline: caption rows have a fixed height, and the timeline's list for screen readers does not list
  captions yet. Holding a drag over the caption rows does not offer a new video track (hold over the ruler). Ctrl+K on
  a one-word caption leaves it whole (a message says so; the clips are still cut). Caption-row targets are not saved.
- Scene Edit Detection: Premiere's Generate subclips in bin is not there, and nested and multicam clips get cuts but no
  markers. It has no default key and no .kys mapping. The menu bar's greyed-out row gives its reason only to screen
  readers and in the palette. Scene Edit Detection and Find dead time do not keep the computer awake.
- Dead time: after an option change, nothing is measured until you choose Find again.
- Chapter markers: Premiere's own XMP markers are not imported. A Media Browser preview shows no chapter markers until
  it is imported, and media imported before 0.5 gets none. A file whose time does not start at 0 gets its markers off
  by that much (OBS recordings start at 0).
- Source transcript: the Sequence | Source switch is by hand (it does not follow the Source Monitor's focus), and Source
  mode has no word menu. `,` and `.` pressed on the keyboard use the Source Monitor's marks as they are; the
  transcript's Insert and Overwrite buttons set them from the selected words first. Counts may read "5 word(s)".
- Linux and macOS are preview builds: nobody has tried 0.5 on a real Linux or macOS machine yet. The other
  [0.4 known issues](release-notes-0.4.md#known-issues) still apply (0.5 adds the opt-in updates).
- Real local-AI quality and live cloud-provider runs are not benchmarked.

## Privacy

Nothing leaves your computer unless you turn on a cloud feature with your own key, and every cloud call is shown with its
cost first. Redacted regions and bleeped words are never uploaded in clear. Crash reports stay local until you choose
to share them, and never contain API keys.

The update check is new, and off by default. It contacts GitHub only when you turn on "Check for updates automatically"
or choose Check for Updates…, and it asks only for the latest release's update information, with Wabcut's version as
its user agent: nothing about you or your projects is sent. An update you choose to install is downloaded from GitHub.
Nothing else new is sent: Scene Edit Detection, dead time, chapter markers and the Graphics card work on this
computer, and Transcribe this clip uses the transcriber you chose in the panel, with the same consent prompt as before.
Pilot reports are written and sent by you.
