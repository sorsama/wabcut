# Wabcut 0.4: release notes (0.4.0)

Windows 10/11 x64, with preview builds for Linux x64 and macOS on Apple silicon. Free and open source
(GPL-3.0-or-later). No account, no subscription.

Wabcut 0.4 is about Premiere muscle memory and doing creator work in fewer steps. On the timeline, Alt-drag copies and
Ctrl+Alt-drag rearranges, clips show their speed and effects, and Premiere keys that did nothing in 0.3 now work.
Effect Controls copies effects between clips, the transcript follows the playhead, and Highlights makes gaming Shorts
in one go. Wabcut also stops losing work quietly: quitting asks while exports run, and a failed autosave or export says
why. [Premiere parity](premiere-parity.md) now compares 105 areas: Wabcut is equal in 63, better in 9, partial in 32
and missing in 1.

The project file format is still schema 18: Wabcut 0.4, 0.3 and 0.2 open each other's projects, and Wabcut 0.4 opens
0.1 projects. 0.4 adds one optional field: a sequence with Ripple Sequence Markers turned off saves that switch. Wabcut
0.3 and 0.2 ignore it (they have no such switch) and leave it out when they save, so the sequence has Ripple Sequence
Markers on again when 0.4 opens the file.

## 0.4.0 (25 October 2026)

Projects: still schema 18, so Wabcut 0.4, 0.3 and 0.2 open each other's projects (a sequence with Ripple Sequence
Markers off has it on again after 0.3 or 0.2 saves it).

### Timeline
- **Alt-drag copies a clip**, as in Premiere: hold Alt when you drop. Press Alt before clicking to copy only the video
  or only the audio of a linked clip; let go of Alt before the drop to move just that side.
- **Ctrl+Alt-drag rearranges** shots: the gap closes and the clips at the drop move over (drop a shot at the cut after
  its neighbour to swap the two). Only the dragged clips' tracks and the tracks you drop on change, so a music bed
  below stays whole; on another track a shot lands where you let go. Shots selected apart land side by side at the
  drop in time order, with their linked sound in sync; on the same tracks the sequence keeps its length. When closing
  the gap would cut a clip you did not drag (an L or J cut), or a clip's linked sound sits apart from its picture, the
  drag says why and the drop changes nothing.
- **Shift** while dragging keeps a clip at its time when you move it to another track.
- **Clip facts:** clips show their speed after the name ([50%], [−100%]) and an **fx** badge, violet for effects and
  amber for changed or keyframed Motion, Opacity, Volume or Pan, a blend mode or time remapping. Rest the pointer on a
  clip for its start, end, duration, speed and source. Each can be switched off in the timeline's display settings.
- The playhead snaps to cuts, markers and In/Out while you drag it in the ruler (Shift snaps with Snap off).
- **Playback auto-scroll:** Page, Smooth (the playhead stays in the middle) or No scroll, in Settings > Timeline &
  editing. Page, the default, now puts the playhead on the left edge as Premiere does, instead of re-scrolling every
  tenth of a screen. Playing backwards (J) mirrors both: Page turns back a screen with the playhead on the right edge,
  Smooth keeps it in the middle.
- Right-click the ruler for Mark In / Out, Clear In and Out, markers, Ripple Sequence Markers and Show Audio Time Units.
- Trims show the clip's new duration while you drag (a rate stretch also the new speed, a roll both clips).
- `\` (Zoom to Sequence) goes back to your zoom on the second press.
- The clip menu has Audio Gain… and Clear, and its Paste pastes copied effects as Ctrl+V does.
- Unmuted tracks no longer show a crossed speaker, and mute and solo use the mixer's colours (amber and green).

### Keys, transport and monitors
- **Markers > Ripple Sequence Markers** (on by default): the sequence's markers (chapters, to-dos, highlights and the
  rest) move with insert, extract, ripple delete, Close Gap, ripple trims and transcript cuts, in the same undo step. A
  marker inside a removed range moves to the cut instead of being deleted. The switch is saved with each sequence.
- **Keys that did nothing now work:** Shift+J / Shift+L slow shuttle (1/4x, then 1/2x and 1x), Shift+K Play Around,
  Ctrl+Space Play from Playhead to Out, and PageUp / PageDown timeline paging. The palette no longer says "Not
  available yet" for them.
- **Alt+Up / Alt+Down** (Opt on macOS) move the selected clips one track up or down, as in Premiere. Linked video and
  audio move together (Up takes V1 to V2 and A2 to A1). At A1 with Up, or V1 with Down, the half with no track there
  stays where it is, as in Premiere, and a message says so. At the top video track (Up) or the bottom audio track
  (Down) a clip with video and audio does not move, because Wabcut adds no tracks; the message says why, as it does
  when there is no track there or it is locked.
- **] and [** raise and lower the selected clips' volume by 1 dB (Shift: 6 dB), keyframes included, as in Premiere;
  with nothing selected they act on the clips under the playhead on the targeted tracks. Presses within a second are
  one undo step, and one message shows the change (with the new level for a single clip without volume keyframes).
  With the Library focused, Shift+] and Shift+[ still size its thumbnails.
- Type on the numpad with the Timeline or a monitor focused to enter a timecode; +15 Enter moves 15 frames on, as in
  Premiere.
- The Program Monitor shows the In to Out length next to the timecode (the Out frame counts, as in Premiere). On a
  narrow monitor the Safe Margins, zoom and playback resolution buttons make room for it, in that order; the wrench
  menu has Safe Margins and Playback Resolution too.
- **Platform safe zones:** on vertical sequences the Program Monitor can shade where YouTube Shorts, TikTok or Reels put
  their buttons and captions (wrench > Platform Safe Zones). The lines have dark and light edges, so they show on
  bright and dark footage alike. Off by default; never in exports.
- Double-click (or Enter on) a smart-search result: it opens in the Source Monitor with the matched range marked, ready
  for `,` (Insert) and `.` (Overwrite). Tab reaches the results; Up and Down move between them.
- The monitors' buttons no longer spill out of narrow panels; the ones that do not fit are in a ··· menu.

### Effect Controls, colour, transcript and captions
- **Copy effects between clips:** click an effect's header in Effect Controls (Ctrl / Shift add more), press Ctrl+C,
  select other clips and press Ctrl+V. Backspace removes the selected effects (Transform, Opacity and Audio are reset
  instead), and the header's right-click menu has Cut, Copy, Paste, Clear, Effect Enabled, Reset and Move Up / Down.
  Each is one undo step.
- **More room in Effect Controls:** the Masks section stays one row until the clip has a mask or you pick a mask tool;
  your open or closed choice is remembered.
- **Apply a grade to many clips:** the Color panel menu has Apply Grade to Selected Clips and Apply Grade to All Clips
  of the media. The grade is replaced, not stacked, in one undo step, and each clip keeps its own masks (also
  `color.grade.copy` in the CLI and MCP).
- **The transcript follows the playhead** while you play or scrub, down to the word's line in a long paragraph. The
  crosshair button turns it off, and scrolling pauses it for a few seconds.
- **Right-click selected transcript words** for Copy (Ctrl+C), Copy with Timecodes, Mark In and Out, Add Marker, Add
  Chapter Marker and Lift (leaves a gap), next to Delete, Keep Only, Fix… and Find All.
- **Transcribe only your voice:** Transcribe lists the audio tracks and picks the mic and dialogue tracks. Game audio,
  music and Discord are left out unless you switch them on, and the choice is remembered by track name.
- **Shift Captions…** in the caption track menu moves all captions, or those from the playhead on, by an offset such as
  -00:00:00:12, to resync an imported SRT in one step. The palette's Shift Captions uses the track you are looking at.
- **Ken Burns moves for stills:** Presets > Image Pans adds Slow Push In, Slow Pull Out and four directional push-ins,
  eased and stretched over the whole clip.
- **Essay and documentary graphics:** five new built-in templates (Name and title, Location and date, Quote, Source,
  Chapter card) in Callouts > Browse. They take your brand kit's colours and fonts.

### Import, Home and gaming
- **Import a folder from the editor:** File > Import Folder…, the Library's ··· menu or a folder dropped on the Library
  brings it in as bins (subfolders nested) in the bin you have open, in one undo step. A message says what came in and
  what was left out (files that are not media; files File Explorer hides, such as desktop.ini, are not counted). File >
  Import now puts files in the open bin too, as in Premiere. A folder dropped on the timeline comes into the Library as
  bins without being edited in.
- **Import from Media Browser** (Ctrl+Alt+I) works, and the Media Browser imports whole folders: choose folder rows like
  clips, and each comes in as a bin. Clips and folders chosen together are one undo step.
- Import dialogs open where your last import came from. The Media Browser lists your Game Bar, ShadowPlay and OBS
  recording folders and your recent import folders, and reopens where you left it.
- **Shorts from every highlight:** Highlights' Make Shorts (N) makes a vertical Short of every accepted moment (of every
  moment not rejected when none is accepted) in one go: one undo step, the timeline stays put, and Cancel keeps the
  Shorts already made. Export all queues them at 1080x1920 (H.264 or HEVC). Accepted and rejected moments keep their
  review when a ripple edit moves their markers.
- **Short options:** Lead-in, Tail and At least (15-60 s) give a highlight Short its build-up, the reaction after it
  and a minimum length (3 s and 1 s of lead-in and tail by default); the Make a Short tooltip says about how long it
  will be.
- Shorts get the big outlined "Shorts" caption style above the platforms' buttons, in two short lines (about 10
  characters each, 14 with another style); pick another in "Caption style" (Highlights' Short options and the Shorts
  panel).
- **"Game under my voice"** in Audio Tools ducks the game (and music) under your mic, found by track name, so no roles
  are needed. Each Auto-duck mode keeps its own timing.
- **Instant replay** replays In to Out when both marks are on the clip (else the 5 seconds before the playhead on a
  long clip), with 25 / 50 % speed, Sound and Stinger options that are remembered. The fast-forward speeds have a row
  of their own.
- **Track names from the recording:** Home's Start with media names audio streams from their titles in the recording
  (OBS writes your track names there; the two untitled streams ShadowPlay and Game Bar record become Game and Mic) and
  gives the tracks roles, so your next recordings land on the same tracks. Untitled recordings with three or more
  streams get Track 1, Track 2, ... (0.3 guessed Game / Mic / Discord), with a hint about OBS's full mix on Track 1.

### Export, background work and the shell
- **Quitting asks** while an export or Render In to Out runs: keep Wabcut open, quit when the work is done, or cancel
  it and quit. Exports that wait in the queue or for Start Queue ask too. If the work ends while Wabcut asks, the
  question goes and Wabcut quits (it stays open when the work failed, and says why).
- **Background tasks:** a pill in the title bar shows exports, renders, proxies and AI jobs. Click it (or run
  Background Tasks from the command palette) for their progress and the recent messages, so a message that went by
  while you were not looking is not lost. When nothing runs, the pill counts the errors you have not seen yet
  ("1 problem").
- **Error messages are not replaced** by the next message, on Home too: the next one waits its turn. In Recent
  messages, a click on a long message shows all of it.
- When autosave cannot write your project, Wabcut says so with Save As…, and the Save button's dot turns red until a
  save works again.
- A failed export's message says why, and offers Relink… when media is offline.
- Open a finished export from its message or the export queue (Open, or a double-click on the job), with Show in
  Explorer next to it.
- Windows no longer sleeps in the middle of an export or render, the taskbar button shows the progress, and it flashes
  when the export is done while you are in another app (Settings > General > Background work).
- Panel tabs keep readable names, a tab list shows every panel of a crowded group, and header tools that do not fit
  move into a More tools menu (the chevron button before the panel's grip) instead of disappearing. A lone panel's
  tab opens its menu with the Menu key or Shift+F10 (a 0.3 known issue).

### Scripts and agents (CLI and MCP)
- `clip.move` takes `copy` (Alt-drag) and `mode: "rearrange"` (Ctrl+Alt-drag), `sequence.setSettings` takes
  `rippleMarkers`, `shorts.create` and `highlights.toShort` take `captionPreset`, and `color.grade.copy` is new.

### Fixes
- The reset button of a collapsed Effect Controls section did nothing. It resets the section now, as does the header
  menu's new Reset.
- Setting In or Out or adding a marker (and undoing it) cleared the words selected in the Transcript panel. The
  selection now stays.
- In Highlights' Short options, a Split or Zoom value you typed went back to the old one. The value now stays.
- The Metadata panel reported script errors when the item it showed went away, for example when an import was undone
  while it showed the item's tags.

What is not done or not checked yet is under [Known issues](#known-issues).

## Known issues
- The 0.4 changes are verified by automated tests only, in hidden windows. None of them has been through a release run
  with real projects yet: keep backups of your projects.
- Drag and drop and the new mouse gestures (Alt-drag copy, Ctrl+Alt-drag rearrange, Shift while dragging, folders
  dropped on the Library or the timeline, the playhead snapping in the ruler, the clip tooltip) were tested with
  simulated events, not with a real mouse.
- Screen readers have not been checked by hand (NVDA, Narrator). In Effect Controls, Tab does not stop on a single
  effect header, so one effect cannot be selected alone from the keyboard (Ctrl+A selects them all and Esc none; a
  screen reader can press a header).
- Timeline: insert-copy has no mouse gesture (Ctrl+Alt at the drop rearranges); use Copy and Paste Insert, or
  `clip.move` with `copy` and `mode: "insert"`. The fx badge has no click action yet. On the Linux preview, desktops
  that bind Alt-drag to moving windows take the Alt-drag before Wabcut sees it.
- Keys and monitors: Shift+Space (Play In to Out with pre-roll and post-roll) and Premiere's monitor Button Editor are
  not in 0.4. Slow shuttle starts at 1/4x (Premiere's first step is slower). The large clip volume step is fixed at
  6 dB (no Settings row yet). Alt+Up at the top video track and Alt+Down at the bottom audio track do not add a track
  as Premiere does. On macOS, Control+Space is the system's input-source switch when more than one input source is on,
  so Play from Playhead to Out may not reach Wabcut there. On Windows, a Chinese input method that turns on and off
  with Ctrl+Space may take the key too. Platform safe zones are approximate guides.
- Effect Controls: copied effects stay on their own clipboard for the session (not between app runs), and Time
  Remapping cannot be selected or copied as a section (it is in the speed controls).
- Transcript: Edit > Copy in the menu bar still copies timeline clips; use Ctrl+C in the transcript or the word menu to
  copy words. Streams already transcribed stay in the transcript when their track is switched off for Transcribe. In a
  short Transcript panel the Tracks checklist is below the transcriber list, so you have to scroll to it.
- Import: Home's "Import folder…" (which starts a new project) still brings the files in flat, without bins; File >
  Import Folder… in the editor makes the bins. A folder dropped on the timeline lands at the top of the Library, not
  in the open bin, and files dropped on the timeline are still one undo step each, as in 0.3. Camera cards inside an
  imported folder are left out: import them from the Media Browser, which joins their clips. Windows' file dialog
  picks files or a folder, not both, so Import… and Import Folder… are separate rows. A Media Browser that opens on a
  network folder that cannot be reached waits until Windows gives up. Some messages still count with "(s)", such as
  "Queued 2 export(s)".
- Audio Tools: the Auto-duck mode is suggested per sequence and not saved with the project.
- Background work: keeping the computer awake is Windows only for now (the setting shows on every system and says so).
  Proxies, transcription, Highlights, Shorts and Collect Files do not make quitting ask; proxies the policy wants are
  made again on the next open. Quit When Done does not wait for paused or held exports (the question says they are
  lost). The queue runs one export at a time, so exports queued behind a paused one wait until you resume it (they do
  not keep the computer awake meanwhile). The export queue and the recent messages are not kept across restarts, and
  the messages Home shows itself (its own open and import failures) are not in the recent messages. Background Tasks
  has no Window menu row (the command palette and Keyboard Shortcuts have it).
- Autosave: the failure message gives the system's reason, which can name the temporary autosave file Wabcut writes
  (such as Film.wabcut.autosave.tmp) instead of your project file.
- Panels: a tab that is only in the tab list has to be shown before you can drag it. The More tools menu lists
  buttons only; badges and switches hide until there is room.
- Linux and macOS are preview builds: nobody has tried 0.4 on a real Linux or macOS machine yet, and the Shorts caption
  lines were measured with Windows' fonts. The other [0.3 known issues](release-notes-0.3.md#known-issues) still apply
  (0.4 fixes the key for a lone panel tab's menu and adds numpad timecode entry).
- Not code-signed; no auto-update. Real local-AI quality and live cloud-provider runs are not benchmarked.

## Privacy

Nothing leaves your computer unless you turn on a cloud feature with your own key, and every cloud call is shown with its
cost first. Redacted regions and bleeped words are never uploaded in clear. Crash reports stay local until you choose
to share them, and never contain API keys.
