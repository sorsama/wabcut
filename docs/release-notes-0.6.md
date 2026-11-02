# Wabcut 0.6: release notes (0.6.0)

Windows 10/11 x64, with preview builds for Linux x64 and macOS on Apple silicon. Free and open source
(GPL-3.0-or-later). No account, no subscription.

Wabcut 0.6 works through the roadmap's P0 and P1 cards. Text is now edited in Effect Controls (font, strokes,
capitals, the background box, the shadow, and Source Text keys that switch the words at a frame), and colour gets HDR
finishing: Minimal Tone Mapping, ACES gamut compression, an output colour space for each sequence and an SDR Conform
that reports what it mapped. Seven Premiere effects (Glow, Colorama, Fractal Noise, Cell Pattern, Lightning, Scribble,
Paint Bucket) and Master Clip Effects fill two of the library's gaps. Exports are steadier: the queue is kept across
restarts, several sequences or every marker can go out as separate files, a size limit fits a file to an upload cap,
offline media and a full drive are caught before the export starts, and a broadcast file leaves as MXF OP1a or OP-Atom
with CEA-608 and CEA-708 captions and dialogue, music and effects stems beside it. The Library counts how often each
item is used and opens bins in place, and relinking one file finds the others that moved with it. The timeline gains
edit-point transitions, Premiere's marker menus, the non-ripple Q and W, a Clip Mismatch Warning, out-of-sync tags,
auditions that keep several takes at one cut, a beat grid to cut music to, and render selection; the Program Monitor
takes drops from the Library, parks sharp at full resolution, and lets you click captions. Captions get Premiere's keys,
transcript speakers can be renamed and merged, a pasted script corrects the transcript's misheard names and jargon, the
voice-over has a script prompter, and loudness analysis says where the loud moments are. AAF and OMF timelines import,
and colour grades, EQ and compressors survive FCP XML. Creator Autopilot puts B-roll from your own library on the lines
that need it, writes a publishing kit and matches a reference video's cutting pace. The shell gets workspaces you can
save and lock, a History panel, Ctrl+F in the active panel, the 90 % interface size, and a screen-reader pass that named
every control.

The project file is schema 19. Wabcut 0.6 opens 0.5, 0.4, 0.3, 0.2 and 0.1 projects. A project saved by 0.6 is refused
by 0.5 and older rather than opened without its new data: auditions, Master Clip Effects, the text layers' extra
properties and the colour management and output settings. The rest of 0.6's choices are settings of Wabcut on your
computer.

## 0.6.0 (1 November 2026)

### Timeline

- **Edit points take transitions and E.** Select an edit point (click a clip's edge, or Shift+T) and Ctrl+D /
  Ctrl+Shift+D put the default video or audio transition on that point, not on the edit nearest the playhead; a
  transition applied from the Effects panel goes there too. E moves the selected edit points to the playhead with the
  current trim type (roll, ripple or regular), in one undo step.
- **Markers like Premiere.** Press M over an existing marker to open it instead of adding a second one; Markers > Edit
  Marker… and Add Chapter Marker… are new; and a right-click on a marker on the ruler, the Program Monitor's time bar or
  in the Markers panel changes its colour or type, jumps to it or deletes it.
- **Cut to the beat.** Sequence > Beat Grid has Detect Beats from Selected Clip…, Snap Selected Clips to Beats, Cut
  Selected Clips to Beats… and Clear Beat Grid, and Clip > Detect Beats… runs the detection from a clip. Detect Beats
  reads the tempo of a clip with sound (a lowest and highest BPM keeps the search in range), adds numbered beat markers
  and the sequence's beat grid; Snap moves the selected clips' edges to the nearest beat, and Cut to Beats cuts them
  every 1, 2, 4 or 8 beats from the playhead. Timeline Display > Show > Beat grid draws the beats as thin ticks on the
  ruler, downbeats taller. Each job is one undo step, and all four are in the command palette and Keyboard Shortcuts with
  no default key.
- **Shift+Q / Shift+W and Ctrl+Alt+Q / Ctrl+Alt+W.** Premiere's non-ripple companions of Q and W: Shift+Q and Shift+W
  extend the edit before or after the playhead to it, Ctrl+Alt+Q and Ctrl+Alt+W trim the clip under the playhead to it
  and leave a gap. Both are in the Sequence menu too.
- **Faster track headers.** Shift-click a track's eye, mute, solo, sync lock or lock (or its target) to set it on every
  track of that kind. A Sync Lock button on the headers shows which tracks a ripple shifts, and Ctrl+0 / Ctrl+9
  (Ctrl+Alt+0 / Ctrl+Alt+9) toggle all video or audio targets.
- **Clip Mismatch Warning.** Drop a 1440p 60 fps recording into an empty 1080p 30 fps sequence and Wabcut offers to
  change the sequence to match it, in the same undo step as the drop, as Premiere does. "Always do this" remembers the
  answer, and Settings > Timeline & editing changes it.
- **Out-of-sync tags.** When a clip's linked picture and sound slip apart (an Alt-drag, a trim on one side), a red "+5"
  or "-12" tag on the clip shows the offset in frames, and the clip's right-click menu has Move into Sync and Slip into
  Sync.
- **Auditions: several takes at one cut.** Select clips and choose Clip > Auditions > New Audition (the same submenu, as
  Audition, is on a clip's right-click menu): they become one clip whose takes share the cut, with the first take active.
  Window > Take Selector, or the Program Monitor's take strip, lists every take with its frame, name and length;
  Play auditions a take in place and leaves no undo step, and Use This Take switches without moving the playhead. Add
  selected clips or the Source Monitor's clip to an audition, duplicate, reorder or remove takes, and Flatten Audition
  keeps the active one. Auditions move, trim, cut and copy like clips, every take keeps its own effects, the timeline
  says "Take 2 of 4", and playback and export show the active take.
- **Render Selection, and render files you can clear.** Sequence > Render Selection renders the red and yellow segments
  under the selected clips without setting In and Out; clips that touch merge into one range and the time between them
  is left alone. Sequence > Delete Render Files and Delete Render Files In to Out ask first with the size ("Delete 1.2 GB
  of render files?"), delete in the background, and turn the render bar back to red or yellow. All three are in Keyboard
  Shortcuts and the palette with no default key, and grey out with the reason when they cannot run.
- **The cache limits look after themselves.** Settings > Media & cache > Disk caches has "Keep the caches within their
  limits automatically", on by default: it runs shortly after Wabcut starts and after each batch of proxies, and waits
  while you play, render, delete render files or export. Render files (this project) is a new row there with the size,
  file count and free space of your project's preview files and its own Clean; render files are never trimmed by a size
  limit.
- **Square timeline corners.** Settings > Appearance > Timeline corners > Square draws clips, transitions, lanes,
  caption cues and the track headers with square corners, the way Premiere does. Rounded stays the default.

### Effects and colour

- **Master Clip Effects.** Right-click an effect > Apply to Master Clip and every clip of that recording plays it,
  including clips you add later. Change it once in Effect Controls' Master Clip section and every instance follows; one
  switch turns it off for a single clip, and the Library marks the item with an fx tag.
- **New effects.** Glow (Stylize) and Colorama, Fractal Noise, Cell Pattern, Lightning, Scribble and Paint Bucket
  (Generate) are in the Effects panel, keyframable, and the same in preview and export. Animated backgrounds, mattes,
  lightning bolts and drawn-on scribbles no longer need another app; twelve new presets get you started.
- **HDR finishing.** A new Wide Gamut (Minimal Tone Mapping) setup, ACES gamut compression, and an output colour space
  for each sequence and export (Rec.709, Rec.2020, P3-D65, Rec.2100 PQ or HLG). SDR Conform turns an HDR sequence into
  an SDR file with your choice of tone mapping, shows a before and after split on the monitor, and reports what it
  mapped.
- **Copy and paste keyframes.** Select keyframes in Effect Controls and press Ctrl+C (or Ctrl+X), then move the
  playhead or select another clip and press Ctrl+V: the keys land on the same parameters with their spacing and easing.
  The keyframe's right-click menu has Cut, Copy and Paste too.
- **Several clips at once.** Select several clips and Effect Controls shows their shared Scale, Position, Rotation,
  Opacity, Volume and Pan. Change a value once to set it on all of them, in one undo step; values that differ show a
  dash.
- **More camera log formats.** Media Color Space now has DJI D-Log, Fujifilm F-Log and F-Log2, Nikon N-Log, Sony
  S-Log2, the original Canon Log, Leica L-Log and RED Log Film, and Wabcut picks them up when the camera's metadata
  names them. DJI D-Log M is not there yet (DJI has not published its formula), nor is Fujifilm's F-Log2 C, which uses
  a different gamut.

### Text and graphics

- **Text in Effect Controls.** Select a graphic and Effect Controls has a Text section for each text layer: the text,
  font, size, alignment and vertical alignment, tracking, kerning, leading, capitals, faux bold and italic, fill, any
  number of strokes, the background box, the shadow (now with Size), and the layer's position and scale. Sizes, spacing
  and transforms keyframe like any other row, Source Text keyframes switch the text at a frame, everything renders the
  same in preview and export, and the Essential Graphics panel shows the same settings.
- **Install and export templates.** Graphics > Install Motion Graphics Template… (and "…into My Templates…") adds a
  Premiere .mogrt file or a Wabcut template to your libraries, and Callouts > Browse has an Install template… button.
  Dropping a template on the Library or the Timeline, or picking one in File > Import, installs it instead of importing
  it as media. Any template's right-click menu, built-ins included, has Export template… to share it as a Wabcut
  template file.

### Export and delivery

- **The export queue survives a restart.** Exports waiting in the queue come back the next time you open Wabcut, ready
  for Start Queue. One stopped by a quit or a crash comes back with Retry, using the project exactly as you queued it,
  and quitting with exports queued offers Keep Queue and Quit.
- **Export several sequences at once.** Select sequences in the Library and choose Export… or Send to Export Queue…:
  one preset for all, or Match each sequence's shape (YouTube for landscape, Shorts, TikTok or Reels for vertical).
  Each file is named after its sequence.
- **Each marker or clip as its own file.** Export each marker range… in the Markers panel, and Each marker… or Each
  selected clip… in the export settings, write one file per highlight or clip, named after it, with the file count and
  total size shown first.
- **Dialogue, music and effects stems.** Export settings > Audio stems > By track role writes a 24-bit WAV per track
  role next to your export — Dialogue (_DX), Music (_MX), Effects (_FX) and Other (_OTHER) — over the same range and at
  the same sample rate, so they line up with the master sample for sample; Stems without master effects leaves the
  master bus effects out, and stems are never loudness-normalised. Each stem is its own queue row, held with the master
  and retried on its own.
- **Broadcast delivery: MXF.** Settings > Format > MXF, then MXF: OP1a or OP-Atom (Avid). OP1a writes XDCAM HD422,
  DNxHD / DNxHR or AVC-Intra 100 with PCM audio as mono tracks or one multichannel track, a timecode track, the reel name
  and the material package name; OP-Atom (Avid) writes DNxHD / DNxHR as one file for the picture and one per audio
  channel. Five MXF presets are in Formats, and the finished job's details name the pattern, the audio layout and the
  UMID.
- **CEA-608 and CEA-708 captions in the file.** Settings > Captions > Embed has the Embedded captions rows: Standard
  (608 + 708, 608, or 708), 608 channel CC1–CC4, 708 service 1–63, Carry in, and Display (pop-on, roll-up or paint-on).
  The captions ride in an MXF SMPTE 436M track, in the H.264, HEVC or XDCAM picture, and in a QuickTime c608 track; a
  line under the rows says what this export embeds or why the format cannot carry it, and a sequence with no captions in
  the range is a warning rather than a silent success.
- **Media usage report.** File > Export > Media Usage Report… lists every source the sequence uses, with record and
  source timecodes, tape, scene and take, speed, and totals per source, as a CSV for clearances, licensing and credits.
- **Export size estimates for every format.** GIF, WebM, AV1, MP3, image sequences, stills, CineForm and QuickTime
  Animation now show a real file size in the Export workspace instead of "1 KB".
- **Exports follow the sequence's shape.** Vertical (9:16) sequences remember their own Export and Quick Export
  presets, so a Short no longer starts on your YouTube 16:9 preset; a 16:9 preset on a vertical sequence says it will
  be pillarboxed, and a vertical export over 3:00 reminds you it will not count as a YouTube Short.
- **A size limit for any export.** The Export settings' Size limit fits the whole file into 8, 25, 100 or 500 MB, or a
  size you type, on H.264, HEVC, AV1 or VP9, shows the bitrate that fits and warns when it is too low for the picture.
  Only your own value is remembered; the limit starts off.
- **Offline media before you export, Retry after.** The Export panel and Quick Export list offline files with Relink…
  and wait until they are back; a failed or cancelled export has Retry (and Relink… when media was offline) right on
  its queue row.
- **Low disk space warnings.** The Export panel warns when the drive is too full for the file, running exports warn
  when a drive drops under 2 GB, and Render In to Out and Create proxy say so instead of filling the disk.
- **YouTube chapters to the clipboard.** The Markers panel's export menu copies YouTube chapters straight to the
  clipboard, can use chapter markers only, and can count from your In point (From In to Out) for exports with pre-roll.
- **YouTube thumbnails from Export Frame.** Export Frame has sizes (YouTube thumbnail 1280x720, Vertical, Custom), JPEG
  quality and Keep under 2 MB; Markers > Export Frames at Markers… and the Highlights panel's Thumbnails… save a still
  at every marker or highlight peak, optionally into a Thumbnails bin.

### Library and Home

- **Reorganise bins.** Bins drag into other bins, onto the breadcrumb or onto a bin tab, and the target lights up as
  you drag. Right-click > Move to Bin lists every bin, and Ctrl+X / Ctrl+V in the Library move items between bins. Each
  move is one undo step.
- **Link Media.** Right-click an offline clip in the Library or on the timeline and choose Link Media…, or double-click
  an offline Library item, to relink just that file.
- **Smarter relinking.** Locating one missing file now relinks the others that moved with it ("Relinked 1 file and 37
  others"). An OBS recording remuxed from .mkv to .mp4 is recognised as the same recording and relinked after one
  confirmation.
- **Relink from the import report.** Importing a timeline whose media is missing offers Relink… right in the import
  report, and the report now fits small windows.
- **Proxies for many clips.** Proxy > Create proxies works on every selected clip, and on a whole bin, with a count of
  what was queued.
- **Sizes and totals.** The Library has File Size (and optional Data Rate) columns, and a line under it shows how many
  items there are, how many are selected, and their total size and length.
- **Get Properties.** File > Get Properties for Selection (Ctrl+Shift+H), or Properties… on a Library item, shows a
  clip's codec, frame rate (variable or not), bit depth, audio streams, file size and data rate, with Copy all.
- **Library usage.** Each Library card shows how many times your sequences use it, and an "Unused" mark when they do
  not. Click the count, or use Find in Timeline in the item menu, to jump to any place it is used; the list view has
  Video Usage and Audio Usage columns.
- **Deleting media in use.** Deleting media a sequence uses now asks first and can remove its clips too, in one undo
  step. Ctrl+Delete (Delete with Options) always asks.
- **Twirl-down bins.** Bins in the Library's list view open in place (chevron, Right and Left, Alt for every level), so
  you can browse, sort and select across bins. Wabcut remembers which bins are open for each project.
- **Home project menu.** Rename…, Duplicate…, Versions… and Move to Recycle Bin… are in Home's project menu. Moved
  projects come back with Undo, and a project that fails to open offers to recover an autosave or backup.
- **Home's Import folder keeps subfolders as bins.** A folder imported from Home's Import folder now comes in with one
  bin per subfolder, as File > Import Folder already did in the editor.

### Captions, transcript and audio

- **Premiere's caption keys.** Ctrl+Alt+A makes a caption track, Ctrl+Alt+C adds a caption at the playhead with its
  text ready to type over, and Ctrl+Alt+Up / Ctrl+Alt+Down jump to the previous or next caption. They are also under a
  new Sequence > Captions submenu and in the command palette.
- **Captions in the Program Monitor.** A click selects the caption (the Captions list and the timeline follow), a
  double-click opens its text for editing, and dragging moves the caption anywhere in the safe area in one undo step.
- **Speakers in the Transcript tab.** The tab shows your speakers' names and colours. Click a speaker's name to rename
  or recolour it, say who speaks a paragraph (or the selected words), or merge two speakers into one. Each change is
  one undo step.
- **Correct the transcript from your script.** Paste the script you read, from the Transcript tab or Sequence >
  Captions, and Wabcut fixes the transcript's misheard names and jargon to match it; capitals, numbers and abbreviations
  follow the script too. You see every change first, apply them in one undo step, then update your captions in one
  click. It works with retakes, with lines broken up by an aside, and in Thai.
- **Script prompter for voice-over.** Read your narration from the Program Monitor while you record a voice-over. Open
  or paste your script in Settings > Audio, step through it with the arrow keys or a presenter clicker, mirror it for a
  teleprompter, and optionally drop a marker at each paragraph. Wabcut remembers your place for each project.
- **Loudness QC.** Loudness analysis now shows the loudest momentary and short-term moments and the true peak, each with
  its time, on the master strip and the Audio Tools card. Click a time to jump there, step through every true-peak over
  above -1 dBTP, or mark them all on the timeline with red range markers in one undo step; the Audio Tools report has
  an Overs column.

### Monitors

- **Paused Resolution.** Play 4K gameplay at 1/2 or 1/4 and the parked frame is sharp again as soon as you stop. It is
  Full by default; choose 1/2, 1/4 or Same as playback in the quality menu, the monitors' wrench, View > Paused
  Resolution or Settings > Media & cache.
- **Drop a clip on the Program Monitor.** Drag a clip from the Library, or the Source Monitor's picture, onto the
  Program Monitor to see Premiere's edit overlay: Insert, Overwrite, Overlay (a free track above, or a new one),
  Replace (the clip under the playhead) or Append at End. Each drop is one undo step at the playhead on your targeted
  tracks.
- **Toggle Proxies and Playback Resolution everywhere.** Both are commands you can bind and find in the command
  palette, and the View menu has Premiere's Playback Resolution submenu (Auto, Full, 1/2, 1/4) and Use Proxies. The
  Source Monitor's settings menu has the same rows, and all of them show the state the Program Monitor's quality button
  shows.

### AI and gaming tools

- **Make Shorts version no longer freezes Wabcut.** It runs in the background with its progress and a Cancel button, in
  the Moments popover and in a toast. The version is one undo step, and if you edit while it runs, nothing is changed
  behind your back.
- **The highlight finder reads your OBS track names.** Mic/Aux is your voice, Game and Desktop Audio are game sound,
  and Discord and Music are left out. If a guess is wrong, correct each stream in Find options > Audio streams; Wabcut
  remembers it.
- **Wabcut remembers your facecam setup.** The facecam region you draw is kept per recording, the layout picker opens
  with your last options (Reset brings back the defaults), the Short options are remembered, and Make Shorts version
  uses your region and split.
- **Every gaming moment is a command.** Speed Ramp In, Freeze and Replay, Fast Forward 300 %, Punch In and Hold, Shake
  and Instant Replay are in Clip > Moments and in the command palette, and Make Shorts Version, Make Square Version,
  Find Dead Time…, Find Highlights… and Auto Reframe Sequence… are in Sequence > Gaming. They can be bound in Keyboard
  Shortcuts, and rows grey out and say why while there is nothing to act on.
- **Make a Short from In to Out.** Sequence > Make a Short from In to Out (also in the Moments popover, the clip menu's
  Moments submenu and the palette) turns the In and Out you marked into a 1080x1920 Short with your Short options —
  layout, facecam, hook title, captions and length — in one undo step. No highlight find, and no extra subsequence.
- **Creator Autopilot.** Sequence > Creator Autopilot… opens one sheet with three tabs. B-roll puts clips from your own
  library on a free track above the A-roll to match what each line of the transcript says — by picture when the smart
  search's model is installed, and always by names, bins, descriptions, markers and transcript phrases — with a switch on
  each suggestion and Generate… for the lines nothing matched. Publishing kit writes title ideas, a description with
  chapters, tags and 1280x720 thumbnails into a publishing kit folder, and can have your own AI write the copy after
  showing the cost. Pacing compares your cuts with a reference video's rhythm and suggests cuts you apply as markers or
  as edits. Everything previews on the timeline first, applies as one undo step, runs without the cloud, and uploads
  nothing.

### Interchange

- **Import AAF and OMF.** Mixes and cuts coming back from Pro Tools, Media Composer, Nuendo, Premiere or Resolve open
  as a sequence: tracks, clips with their handles, fades and crossfades, clip gain and volume keyframes, track volume
  and pan, markers and 5.1 layouts. Embedded audio is written next to your project, and missing media comes in offline,
  ready to relink.
- **Colour grades, EQ and dynamics survive FCP XML.** Lumetri Color, the colour correctors, RGB Curves, ProcAmp,
  Levels, Parametric Equalizer, Dynamics and the compressors come into Wabcut from Premiere and Resolve XML, and go
  back out as Premiere's own effects. A Lumetri grade from Premiere lands where Premiere put it — Basic Correction,
  Creative and Vignette, by Premiere's own parameter ids, with its keyframes and bypass state — and Wabcut writes Lumetri
  back in the form Premiere writes; a LUT, curves or wheels that FCP XML cannot carry are named in the report. Anything
  approximated is listed with its values, and filters Wabcut lacks are named. Premiere puts no audio filters in FCP XML,
  so hand over EQ and Dynamics with AAF.
- **Generators survive interchange.** Colour mattes, black and transparent video, bars and tone, the counting leader
  and adjustment layers come through FCP7 XML, FCPXML and OTIO with their settings, both ways. EDL exports list them in
  the report.
- **Effects in FCPXML and OTIO.** Effects from Final Cut Pro and Resolve appear on the clip after import: common ones as
  Wabcut effects, the rest kept and written back on export. Wabcut's own effects, keyframes included, survive an FCPXML
  or OTIO round trip.
- **Captions travel with timeline hand-offs.** File > Export Timeline… has a Captions row — Off or Sidecar files, with a
  format picker from SRT to the other eight formats: every caption track with cues is written next to the timeline file,
  named after the timeline and the track, with bleeped words replaced, and the report lists the files instead of dropping
  the tracks silently. The Captions page's All tracks… saves every language in one go, and its SRT and VTT buttons keep
  bleeped words replaced too.

### Shell, settings and accessibility

- **Workspaces you can keep.** Window > Workspaces adds Save Changes to This Workspace, Save as New Workspace…, Reset to
  <Preset> Preset… and Lock Workspace. Reset to Saved Layout (Alt+Shift+0) now returns to the layout you saved, and a
  locked workspace keeps every panel in place and shows a lock on its tab.
- **History is a panel.** Window > History opens and closes it, and it docks, tabs or floats like any other panel.
  Backspace deletes the current step and every step after it, and its menu has Clear History; both ask first, because
  they cannot be undone.
- **Ctrl+F searches where you are.** Ctrl+F searches the panel you are working in (Effects, Markers, Metadata,
  Transcript, Callouts, Review) and falls back to the Library elsewhere. Ctrl+F6 and Ctrl+Shift+F6 move the keyboard to
  the next or previous panel group.
- **Tooltips explain why.** Tooltips explain greyed-out buttons and menu rows ("Undo: Nothing to undo"), appear when you
  Tab to a button, show its shortcut, and wrap when long. The panel grip has a tooltip and a screen-reader name.
- **Menu rows grey out.** Clip and Sequence menu rows, and their command palette rows, grey out when there is nothing
  for them to act on — Video Options, Merge Clips, Make Offline, Lift / Extract, Insert / Overwrite, Speed/Duration and
  more. Rest the pointer on a greyed row to see why; shortcuts still run and tell you what is missing.
- **The update notice and What's New.** When a newer Wabcut is out (found by an update check you turned on), the Help
  menu shows a dot and names the version, and What's New opens once after you update.
- **Interface size.** Settings > Appearance > Interface size has a 90 % step for small laptop screens, and sizes too
  large for your screen are greyed with "Too large for this screen". Restart now applies a new size right away and
  reopens your project, and the Wabcut window never opens larger than the screen.
- **Where the media cache and proxies go.** Settings > Media & cache can put proxies in a folder you choose, such as a
  fast drive, instead of next to each project; it can also move the media cache and the AI models folder (from the next
  start, with Restart now). Each location shows how much space is free on its drive.
- **A screen-reader pass.** Wabcut was checked against what screen readers read: every button, tab strip, slider and
  field has a name, dialogs are announced with their title, and scroll bars and the Timeline pane are named properly. A
  test keeps it that way, and the accessibility guide has a step-by-step NVDA checklist.

### Fixes
- Local AI now runs on your graphics card on PCs with both integrated and discrete graphics (most gaming laptops and
  many desktops): it uses the same card as Settings > Graphics card and checks the card's real free memory, so the
  medium and large models load and the small one answers about eight times faster.
- The AI assistant no longer repeats an action Wabcut refused until it runs out of steps: it gets a hint to correct it,
  and stops with a clear message if it tries the same thing again.
- Jumping around a very long MKV recording (OBS's default) no longer reads the file from its start: a jump into the
  middle of a two-hour recording shows its picture in about 50 ms.
- Closing a project releases most of its memory again (about 50 MB kept instead of 135 MB after a one-hour project).

- A marker start that falls between two frames (from a script, an extension or the MCP API, say) is now placed on the
  frame that contains it, instead of being stored where nothing could show it; a range marker keeps the exact length
  you asked for.
- Choice pills in Effect Controls ("Color Channels / Alpha Channel", "On Top / Behind / None") wrapped and spilled over
  the next rows in a narrow panel. A row now shows pills when they fit on one line and a menu button when they do not,
  in every effect.
- An extension's last saved setting could be lost if the extension stopped right after saving it.
- A Lumetri Color from Premiere's Final Cut Pro XML landed on the wrong section when the file had two parameters of the
  same name (both Saturation sliders); it now follows Premiere's own parameter ids, keeps its bypass state and its
  keyframes, and is written back the way Premiere writes it.
- A filter parameter in a Final Cut XML longer than 64 KB was cut off, about the size of a Lumetri state block; the
  limit is now 4 MB, so a large grade comes in whole.
- Automatic cache limits now clean only thumbnails and waveforms. Proxies and analysis results (transcripts, object
  masks) are never deleted automatically, because a project you closed may still need them, and opening Settings >
  Media & cache or changing a limit deletes nothing now.
- Master Clip Effects now play everywhere a clip does: Posterize Time, Warp Stabilizer, Rolling Shutter Repair, Track
  Matte Key and Enhance Speech, AAF and OMF exports, FCPXML and OTIO takes, and the Source Monitor, which follows each
  edit. Render previews turn red or yellow again when a master clip effect changes.
- Copy and paste in Effect Controls works on Master Clip Effects, and Source Text keyframes paste their own words onto
  another graphic.
- A take played in the Take Selector or the Program Monitor's take strip is only a preview: it adds no undo step, keeps
  your Redo history, and saving or autosaving while it plays keeps the take you had.
- AAF and OMF import runs in the background with progress and Cancel instead of freezing the window; it is one undo
  step, and its embedded audio goes to a folder Wabcut can write when the file's own drive cannot be written.
- MXF OP-Atom audio files appear only after the picture file is written. Media Composer sees an OP-Atom export as
  separate clips; use OP1a when you need one linked clip.
- CEA-708 captions no longer disappear after an empty caption, load close to when they show, and roll-up lines after a
  gap start on the bottom row again.
- Retry on a failed or interrupted export writes to the file you chose again rather than "Film (2).mp4", and an export
  cut short while overwriting no longer comes back reported as finished.
- Deleting a bin or a file that an audition's other take uses asks first and removes only that take, and a selection of
  bins, files and nested sequences deletes in any order.
- Delete with Options with "Remove their clips" off now deletes the selected items no sequence uses, instead of being a
  button that can only refuse.
- Versions… on Home lists only that project's versions, and recovering unsaved work asks about unsaved changes first and
  closes Home.
- Creator Autopilot says when the timeline changed while a B-roll or pacing job ran, and Run again remakes the preview in
  one click.
- Move to Bin ticks and greys out the bin the items are already in, and Relink opens in the folder where you last found
  media.
- Duplicate names from Ingest transcodes and watch-folder exports now read "Name (2)" like every other export.
- Messages now say "1 clip" and "2 clips" instead of "clip(s)" in the places this release touched.
- A voice-over pass takes the keyboard from a text field, so Space and a presenter clicker drive the prompter.
- The command palette's selected greyed-out row is readable in the light theme, Settings > Media & cache's notes are
  darker, and a dialog's backdrop no longer turns a light-theme window almost black.
- The script correction sheet says why a script could not be read instead of showing an older refusal, and its button
  says "Open script…".
- The play buttons in the Take Selector and the Program Monitor's take strip announce which take they play.

What is not done or not checked yet is under [Known issues](#known-issues).

## Known issues

- The 0.6 changes are verified by automated tests only, in hidden windows. None of them has been through a release run
  with real projects yet: keep backups of your projects. Nothing that needs a real mouse was tried with one: dragging a
  bin onto a bin, the Program Monitor's drop zones, and the 0.4 copy and rearrange gestures were driven by simulated
  input.
- Project files: 0.6 saves schema 19. Wabcut 0.6 opens older projects, but 0.5 and older refuse a project saved by 0.6
  rather than open it without the auditions, Master Clip Effects, text properties and colour output settings they do not
  know, so a project saved by 0.6 cannot be opened by an older Wabcut.
- Text in Effect Controls: template thumbnails in the Essential Graphics Browse tab draw the new kerning and capitals
  but not strokes, faux styles or the shadow size (the timeline, the Program Monitor and exports draw everything). The
  Program Monitor's in-place editor shows the base style while you type, the committed text follows its keys. A rich
  text with several runs is still edited run by run in Essential Graphics; Effect Controls edits the layer as a whole.
  With many text layers the Text sections are rebuilt on every playhead move; nothing slow was seen, but it was not
  measured.
- Copy and paste keyframes: keys paste onto the same parameter on the clip you select, but not onto a selected row of
  the same kind, because Effect Controls has no row selection; a parameter the target clip lacks is named rather than
  invented.
- Templates: a .mogrt's own shapes, layout and animation are still not read, and the install report says what was
  approximated. Wabcut cannot write a .mogrt back out: Export template… always writes a Wabcut template file. A template
  dropped on the timeline is installed into the project's library rather than placed at the drop point, and from Home
  with no project open it can only go to My templates.
- New effects: Colorama's Modify, Pixel Selection and Masking groups, Lightning's Pull Force, Direction and Composite on
  Original, and Cell Pattern's HQ variants are not there. Paint Bucket's flood fill can stop short in a maze-like
  region far from the fill point. Wabcut's noise, patterns and palettes are its own (deterministic for a seed and
  time), not After Effects' exact pixels.
- HDR finishing: the Program Monitor always shows the Rec.709 picture whatever output space you choose for the export,
  P3-D65 is tagged with an unspecified transfer, and SDR Conform's legal range is a soft clip above 90 % rather than a
  hard clip. Minimal Tone Mapping rolls off each scene-referred layer as it is read, which is not Premiere's wording.
- Master Clip Effects: a Wabcut round trip through FCP7 XML or FCPXML brings a master stack back as per-clip copies
  where the format carries effects. An effect cannot be dropped on a Library item or in the Source Monitor onto its
  master clip, the Source Monitor's fx badge and the timeline fx chip do not count master clip effects, and master clip
  effects do not apply to nested and multicam clips.
- Export queue: a restored karaoke burn-in export loses its per-word highlight timings (regular captions are kept), and
  a restored export uses the current Content Credentials identity rather than the one it was queued with. The Export
  Ranges dialog's preset menu lists export presets, not the gaming cards. Only one Wabcut keeps the queue: a second copy
  open at the same time keeps its own queue in memory until it is the only one.
- Broadcast delivery: a QuickTime caption track carries 608 only (708 goes in the H.264 or HEVC picture, and other
  QuickTime codecs get 608 alone), and one export writes one caption track, so a bilingual 608 channel plus two 708
  services in one file is not there yet. Interlaced 1080i XDCAM and AVC-Intra, AVC-Intra 50 and 200, XAVC, JPEG 2000,
  D-10 (IMX) and AS-11 / DPP metadata are not there. 708 captions use 32-column windows and have no per-cue positions
  or fonts. An MXF OP-Atom has not been opened in Media Composer (its files come in as separate clips rather than one
  linked master clip; export OP1a when you need one linked clip) and no MXF has been through a broadcaster's QC tool;
  FFmpeg is what the files were read back with.
- Stems: dialogue, music, effects and other are the only stems — there are no submix stems, a multicam source sequence
  gets none (export the sequence it is cut into), and an MXF export still writes silence in the tracks a stem would fill.
  Stems are WAV 24-bit with no loudness normalisation and no captions, XMP or Content Credentials.
- Export estimates and disk space: AV1, VP9 and GIF encode speeds are estimates, not measured. The panel's free-space
  figure refreshes when you change something, not on its own timer. YouTube chapters has no "add a 0:00 chapter" action
  and does not offer source-clip markers.
- Library: File Size does not add up the parts of spanned clips or image sequences, and relinking in a very large folder
  (up to 250,000 files) can pause the window briefly. Find in Timeline has no key binding of its own.
- Render files: the disk-cache size limits never trim them and the "Clean regenerable data" button leaves them alone;
  only Render files (this project)'s own Clean or the two Delete Render Files rows remove them, and deleting is not an
  undo step because the project does not change. The limits cover only thumbnails and waveforms by design. An explicit
  Clean or Clean regenerable data still removes every proxy in that folder the open project does not use, including
  other projects' files, and analysis results such as object-mask mattes, so keep other projects out of a proxy folder
  you clean.
- A crashed export can leave a ".wabcut-partial" file next to its output; nothing removes it yet. A few older messages
  still read "clip(s)" instead of "1 clip" or "2 clips".
- Beat grid: Detect Beats works on a clip on the timeline, not on a Library item, and there is no Tap Tempo (the command
  takes taps, the window does not). The beats are ticks on the ruler only, not lines across the tracks. Cut to Beats
  overwrites the tracks it lands on and has no dry-run preview (Undo puts them back), and the grid is not written into
  exports — the beat markers are.
- Auditions: the other takes are not listed in Effect Controls (switch to a take to edit its effects), and a Library
  item cannot be dragged onto an audition — use Add Source Monitor Clip to Audition. FCP7 XML, EDL and AAF/OMF write the
  active take only (FCP7 XML and EDL say so in the report). Through FCPXML and OpenTimelineIO the take order comes back
  active first, a short active take comes back at its active length, and Master Clip Effects are written on the active
  take only; adjacent auditions whose active take is short can leave an empty track after import.
- Markers: a range marker's end can still fall between frames when its length is not a whole number of frames;
  timecode readouts floor it and exports round it.
- Gaming: Find Highlights from its key opens the panel and starts the find, but that path has only been checked as far
  as starting it. Slow-mo 50 % and Fast Forward 200 % and 400 % in Clip > Moments are not bindable (they have no
  command of their own). The palette lists both the bindable row and the older typed edit for Instant Replay, Shorts,
  dead time and Auto Reframe; the bindable one always runs. While the Moments popover is open, the Make Shorts progress
  shows both in the popover and in the toast.
- Creator Autopilot: pacing match only suggests cuts — no punch-in zoom on a jump cut and no cut to another angle — and
  B-roll comes from your own library (no stock libraries), one Generate per unmatched line, each priced and approved on
  its own. A suggestion can be switched on or off but not swapped for its second-best match, and thumbnails have no text
  overlay and no face-aware crop. The publishing kit's cloud copy is covered by the monthly cap and the estimate shown
  first, but has no per-call maximum of its own.
- Captions and transcript: the Sequence > Captions submenu is in the menu bar, not in the sequence pill's own menu.
  Double-clicking a caption in the Program Monitor opens its text in the Captions page; typing over it in the monitor
  itself is not there. There is no loudness radar display, only the lists and links, and counts can read "2 true-peak
  over(s)". The script correction's sound keys are English-flavoured: other Latin-script languages fall back on letter
  similarity, a correction whose words span two clips is left out and counted, and "three point five" is not equated
  with "3.5". In a timeline hand-off, a caption track that cannot be written in the chosen format keeps its Dropped
  entry with the reason, and the timeline file is still written.
- Monitors: Paused Resolution is one setting for both monitors, and 1/8 and 1/16 are not there; a ProRes clip at 1/2
  can take about 160 ms to start when the paused resolution differs from the playback one.
- Shell: a floating window of a locked workspace can still be moved by its window frame, and closing it docks it back.
  The Media Browser has no search field, so Ctrl+F there goes to the Library. Ctrl+F6 moves between panel groups, but
  there are no next and previous panel shortcuts.
- Accessibility: the screen-reader pass walked the accessibility tree and added a test that keeps the names in place;
  nobody has yet gone through it with NVDA. Speech wording, the timeline lanes and live announcements (toasts, export
  progress) are on the manual checklist, and a dialog's title may be read twice. The Welcome demo's J K L keycaps are
  Tab stops read as single letters, and the window buttons are read between Home's header and its content.
- Interchange: effect masks are not written to FCPXML, and a generator's sound without its picture in the primary
  storyline is dropped. Foreign keyframed parameters kept on a placeholder keep only their first value, and OTIO effect
  metadata over 1 KB is not kept. Some of the effect ids used for Final Cut, and for Premiere's FCP7 XML, are
  assumptions; a Lumetri with no state block has not been imported into Premiere itself, and the multi-key value form
  has only been tried on files written the way Premiere writes them. Premiere's Gaussian Blur imports its effect but not
  its radius, and Lumetri curves, colour wheels, HSL secondary and the shadow and highlight tint colours are reported,
  not imported or written back; the section switch ids are inferred from their position. Premiere's own EQ and Dynamics
  do not travel through FCP7 XML at all — use AAF for audio.
- AAF and OMF import always makes a new sequence (there is no "merge into the current sequence"), and embedded video
  essence (DNxHD in AAF), OMF 1.0, Avid plug-in parameters and multigroup alternates are not supported. It runs in the
  background now, but a full mix session has not been imported from Pro Tools yet.
- The builds are not code-signed yet, so Windows SmartScreen may warn on the first run (choose More info, then Run
  anyway) and macOS asks you to allow the first start. Updates do not depend on it: their own signature and the
  installer's checksum protect them.
- Linux and macOS are preview builds: nobody has edited with 0.6 on a real Linux or macOS machine yet, and the macOS
  download needs macOS 15 or newer on Apple silicon (Intel Macs are not supported). The other
  [0.5 known issues](release-notes-0.5.md#known-issues) still apply.
- Performance: long files open fast. A 22 GB, two-hour 1440p60 recording becomes a sequence in 75 ms and a 30-minute
  1080p240 HEVC one in 132 ms, and a jump into the middle of the two-hour file shows its picture in about 50 ms. Project
  save and load, frame stepping, autosave and playback are within their budgets, while scrubbing camera originals is 2 ms
  over its 100 ms budget and scrubbing proxies is 82 ms against 50 ms. NVENC export is below its speed budget (44.8 fps
  on H.264 1080p against 120) on a machine whose GPU was shared with other applications. Memory kept after a one-hour
  project closes is about 50 MB (the budget) instead of 135 MB; what remains is heap pages Windows cannot hand back and
  the GPU driver's own decoder memory, reused by the next project. Measured on a 16-thread desktop with 32 GB of RAM and
  a GeForce RTX 3060, with Wabcut limited to 80 % of the CPU.
- Local AI: the small local model gets 17 of the 56 benchmark requests right; on a GeForce RTX 3060 the medium and large
  models get 29 and 30. Cloud providers were not benchmarked.

## Privacy

Nothing leaves your computer unless you turn on a cloud feature with your own key, and every cloud call is shown with
its cost first. Redacted regions and bleeped words are never uploaded in clear. Crash reports stay local until you
choose to share them, and never contain API keys.

The 0.6 features work on this computer: text and graphics, the new effects, HDR finishing and SDR Conform, Master Clip
Effects, auditions, the beat grid, render previews, the export queue, broadcast MXF delivery and embedded captions,
dialogue, music and effects stems, the library's usage counts, reading and writing interchange files, the caption and
monitor changes, the transcript's speaker edits, Correct the transcript from your script, the script prompter, loudness
analysis and Creator Autopilot. Creator Autopilot's B-roll, publishing kit and pacing work without the cloud and upload
nothing; its Write with my AI step sends the transcript text and chapter titles to your own chat model, shows the cost
first and asks before it runs. The update check is unchanged: it contacts GitHub only when you turn on "Check for
updates automatically" or choose Check for Updates…, and it asks only for the latest release's update information, with
Wabcut's version as its user agent. An update you choose to install is downloaded from GitHub.

## Thanks

Thank you to everyone who reported the problems behind these cards and who edits real projects with the alpha: the
reports and the pilot projects decide what comes next. If something here does not work as described, or a menu row says
why it cannot run and you disagree, [open an issue](../../issues) and say what you were trying to make.
