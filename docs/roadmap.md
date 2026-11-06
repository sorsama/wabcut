# Wabcut roadmap

Wabcut is an **alpha**. This page lists what has shipped and what comes next. It is a plan, not a promise: the order
will change as people use the editor and tell us what matters to them.

**Focus:** a finished Windows editor for **gaming videos** (long and short form), **video essays** and
**documentaries**, edited with Premiere Pro's keyboard and edit rules, with AI that runs locally or with your own key.

## Shipped

### 0.7.0 (6 November 2026)
Every remaining open card on the roadmap, and the AI settings rework:
- **Timeline.** Track groups; solo for video tracks; tracks dragged into a new order; track label colours, named clip
  labels and Select Label Group; locked clips; clip notes; a pacing strip that shows where an edit drags; background
  rendering while Wabcut is idle; Render and Replace for nests; slip trim; keys for track eye, mute and solo and for
  Overlay, Insert Before / After and Append; Alt-drag then Ctrl to insert a copy; middle-button panning and held H / Z
  tools; selecting, moving and importing several markers, Add Marker from In/Out and markers that follow Track Select;
  snap options; Close Gaps, Remove Transitions and Remove Disabled Clips; clip duration and audio stream names on clips;
  a setting that stops edits adding tracks; Match Frame on nests; copying clips between projects with the files they
  use.
- **Monitors.** 360 and VR180 video with a VR viewer and VR Reorient; pixel aspect ratio in the monitors and exports;
  mirrored footage flipped; Copy Frame to the clipboard; Shift+Space plays In to Out with rolls; composition guides; a
  Source Monitor mute; multicam sync nudges; Video Output Settings for a second display (hardware video cards need
  vendor SDKs and are not supported).
- **Effects and colour.** Track Motion; Motion Blur (the footage of a retimed clip is not yet blurred); Scale Width and
  Uniform Scale; Nearest Neighbor scaling; Warp Stabilizer with GoPro and Sony gyro data, with speed changes, Analyze
  All and a keyframable Smoothness; ColorChecker matching; effects on a whole video track; Skin Smoothing; a Graph
  Editor; keyframes pinned to a clip's ends and Ripple Track Keyframes; a clip's keyframes shown from its fx badge; a
  Blend Mode picker; arithmetic in number fields; folders of LUTs and LUTs that follow a replaced file; shortcuts for
  any effect or preset; favourite effects pinned in Effect Controls.
- **AI video effects.** AI Denoise, AI Restore (upscale and deblur), AI Refocus, AI Face Retouch, AI Eye Contact and AI
  Frame Interpolation, running locally with downloaded models; Morph Cut that lines up faces; Auto B-roll that skips
  hidden tracks.
- **Graphics and text.** A Sources panel and bibliography for video essays; one font picker with script filters and
  smart search; OpenType features and optical kerning; styles for part of a text and a Styles tab; keyframable template
  fields; template folders and Install folder; Lottie templates with editable fields and dotLottie, and OGraf packages
  that ship a Lottie version (HTML OGraf needs a web engine); crop handles on titles.
- **Audio.** A 7.1 master; Dynamics Processing with a graph; spectral repair; EQ Match and Level Match; a Loudness panel
  with a radar; Auto-Mix Voices; mixer effect chains saved as presets; Premiere's mixer panel menu; bleeping with your
  own sound; poly WAV channel names; long voice-over takes that end cleanly; loudness QC that keeps the overs of very
  hot programmes.
- **Captions and transcripts.** Transcription in 100 languages and clips that switch languages; custom words; Transcribe
  again and Delete transcript; six new animated looks with emphasis; caption text rules; caption looks kept across
  projects; bilingual subtitles; captions converted to graphics; an audio description generator; TTML time fixes.
- **Library and media.** Reload Footage; Consolidate Duplicates; a rights and clearance log with a credits roll; capture
  from a capture card or webcam (Windows); search across a production; SVG, AI and EPS artwork; newer codecs listed and
  played; a new sequence per clip; nested sequences filed in a bin; Date created and saved column layouts; list
  thumbnails; Tab through cells; Copy and Create Proxies on ingest; a ProRes RAW ISO; drops anywhere on the editor;
  "Open with" handing files to the running Wabcut.
- **Export and delivery.** Smart Render for H.264 and HEVC; a check before every export; HDR10+ metadata for HEVC (Dolby
  Vision needs a licence and is not written); export history; a default export folder; a Streaming -14 LUFS target;
  Collect Files with handles, a sequence checklist, folder layouts and everything keyed to the media trimmed with it;
  cloud projects, review links and stock media on storage you own; safer spreadsheets, preset names and target-size
  exports.
- **Interchange.** File > Save for an Older Version (Wabcut 0.6 and 0.2 to 0.5); the work area and J / L cuts carried
  through FCP7 XML, FCPXML and OTIO; OMF with embedded audio refused before it renders when it would pass 2 GB; OTIO and
  EDL files with a byte order mark.
- **Shell, settings and keys.** Back up, move and reset your settings; autosaves and backups in an Auto-Save folder;
  named versions and a version-history key; Report a Bug with system information; a Gaming workspace; hidden workspace
  tabs; keyboard shortcuts you can copy and print; positional workspace keys; a read-only badge; Edit > Undo naming the
  edit; real plurals.
- **Scripting and plug-ins.** A Python API (`import wabcut`) and File > Run Python Script…; OpenFX video effect plug-ins
  on Windows; extension updates that cannot destroy a working extension.
- **Platforms.** A Windows on ARM64 preview build (not yet run on ARM64 hardware); Linux with test numbers, a
  smoke-tested tar.gz and a .wabcut file type; macOS Finder integration and a signing and notarization pipeline (needs
  an Apple Developer ID); the Windows taskbar jump list and Snap Layouts.
- **AI settings and providers.** Settings > AI rebuilt with Features, Providers, Local models, Privacy and Usage tabs; a
  model for each of seven AI features; 35 new providers (49 in all); your own OpenAI-, Anthropic- or Gemini-compatible
  providers; a library of local models with Hugging Face search and a download queue; Audio Track Mixer effect fixes,
  settings popups and reordering; Sequence Settings frame size; the Library's Text tab removed.

The full list is in the [0.7 release notes](release-notes-0.7.md).

### 0.6.0 (1 November 2026)
Every open P0 and P1 card on the roadmap, and a settings and accessibility pass:
- **Timeline.** Selected edit points drive E, Ctrl+D / Ctrl+Shift+D and Effects-panel transitions; Edit Marker and Add
  Chapter Marker, M on an existing marker opens it, and a right-click menu on markers; the non-ripple Q/W that extend or
  trim the previous and next edit to the playhead; Shift-click track toggles, a Sync Lock button and target-all keys;
  the Clip Mismatch Warning when the first clip lands in an empty sequence; out-of-sync badges with Move into Sync and
  Slip into Sync; auditions, so several takes share one cut; square timeline corners in Settings > Appearance.
- **Render and beats.** Render Selection, Delete Render Files and Delete Render Files In to Out; the beat grid, with
  Detect Beats, Snap to Beats, Cut to Beats and Show beat grid.
- **Effects and colour.** Copy and paste keyframes in Effect Controls; Motion, Opacity and Volume on several selected
  clips at once; DJI D-Log, F-Log and F-Log2, N-Log, S-Log2, Canon Log and L-Log; Glow, Colorama, Fractal Noise, Cell
  Pattern, Lightning, Scribble and Paint Bucket; Master Clip Effects shared by every instance of a source; HDR
  finishing with Minimal Tone Mapping, gamut compression, an output colour space and SDR Conform; text in Effect
  Controls, with fonts, strokes, capitals, the background box, the shadow and keyframed Source Text.
- **Graphics.** Install Motion Graphics Template… and Export Template… for .mogrt and Wabcut templates.
- **Export and delivery.** Size estimates for every format; an export queue kept across restarts; separate Export and
  Quick Export presets for landscape and vertical; several selected sequences, every selected clip or every ranged
  marker as its own file; a YouTube thumbnail option and frames at highlight markers; YouTube chapters to the
  clipboard, chapters only and counted from the export range; a media usage report (CSV); offline media caught before
  an export, with Relink and Retry after a failure; a target file size for any export; low disk space warnings;
  dialogue, music and effects stems beside the master; MXF OP1a and OP-Atom with CEA-608/708 captions; cache limits
  enforced automatically, with render files listed as a cache.
- **Library and Home.** Relink in the import report; deleting media in use with its clips and Ctrl+Delete; Get
  Properties for Selection; Library usage counts, Video Usage and Audio Usage columns and Find in Timeline; Move to Bin,
  Cut/Paste and bin-into-bin drags; relinking that finds the rest of a moved folder, and OBS .mkv to .mp4; Link Media…
  on offline items; proxies for every selected item and for a bin; file size and data rate columns with a selection
  total; twirl-down bins; Home's Rename, Duplicate, Versions and Move to Recycle Bin, and recovery for a project that
  fails to open; Home's Import folder keeps subfolders as bins.
- **Captions, transcript and audio.** Premiere's caption keys and a Sequence > Captions submenu; speakers renamed,
  reassigned and merged from the Transcript tab; correcting the transcript from a script and updating the captions; a
  script prompter on the voice-over overlay; loudness QC with the loudest moments and true-peak overs.
- **Monitors.** Paused Resolution; dropping a Library clip on the Program Monitor to insert, overwrite, overlay, replace
  or append; clicking, editing and dragging captions in the Program Monitor; commands, View menu rows and a monitor
  button for Toggle Proxies and Playback Resolution.
- **AI and gaming.** Make a Short from In to Out; gaming actions in the Sequence and Clip menus, the command palette and
  Keyboard Shortcuts; a remembered facecam layout and region; Make Shorts/square version as a job with progress and
  Cancel; audio roles from OBS track names in the highlight finder, with corrections; Creator Autopilot with auto
  B-roll, a publishing kit and pacing match.
- **Interchange.** Caption sidecars with timeline hand-offs and Export all tracks; colour grades and audio EQ and
  compressor filters through FCP7 XML both ways; FCPXML filters and OTIO effects; AAF and OMF import; generators carried
  through interchange.
- **Shell, settings and accessibility.** History as a dockable panel with Delete and Clear History; Check for Updates…
  with an opt-in notice and What's New once after an update; Ctrl+F in the active panel and Ctrl+F6 between panel
  groups; tooltips that explain why a row is disabled; workspaces you can save, reset and lock; Clip and Sequence menu
  rows grey out; the 90 % interface size, with a window that always fits the screen; a chosen folder for the media cache
  and proxies with the free space shown; extension storage that keeps its last change; real measurements of long-file
  performance and local AI; and a screen-reader pass with an automated test and an NVDA checklist.

The full list is in the [0.6 release notes](release-notes-0.6.md).

### 0.5.3 (30 October 2026)
- The Linux and macOS downloads start: the Linux packages carry every library they need (X11 or Wayland), the preview
  draws with OpenGL, VA-API decoding finds the system's drivers, and the macOS DMG asks for macOS 15.

### 0.5.2 (26 October 2026)
- Update checks work (the releases now carry their signed update information), and the launch intro plays on the
  desktop, see-through, while Wabcut starts.

### 0.5.1 (26 October 2026)
- The Effect Controls keyframe lane zooms (Ctrl/Alt+wheel at the pointer, Shift+wheel pans, a zoom bar), and the
  value graph draws the line of linear keys again.

### 0.5.0 (26 October 2026)
Every open top-priority item, and the right GPU on hybrid laptops (issue #10):
- Caption tracks as rows on the timeline, where captions are trimmed, moved, split and deleted like clips; nested,
  multicam and merged clips show the waveform of their mix.
- The Audio Meters panel beside the timeline, and audio scrubbing (Shift+S) in the timeline, the Program Monitor and
  the Source Monitor.
- Scene Edit Detection… in the Clip menu; dead time reviewed stretch by stretch, with thumbnails, before it is cut.
- OBS chapter markers become clip markers on import; the Transcript panel's Source mode picks a soundbite from a
  source clip for Insert or Overwrite.
- Hybrid laptops preview, decode and export on the discrete GPU, and Settings has a Graphics card choice.
- Opt-in updates, signed and checked, installed when you quit; the release workflow can sign the packages (no
  certificate yet); a [pilot](pilot.md) for editing real projects.

The full list is in the [0.5 release notes](release-notes-0.5.md).

### 0.4.0 (25 October 2026)
Premiere's muscle memory and quicker creator work:
- Alt-drag copies clips and Ctrl+Alt-drag rearranges shots; clips show their speed, an fx badge and a tooltip with
  their times; a ruler menu, trim readouts with the new duration, and Page / Smooth / No playback scrolling.
- Premiere keys that did nothing: Shift+J / Shift+L, Shift+K, Ctrl+Space, Alt+Up / Alt+Down, ] and [ for clip volume,
  PageUp / PageDown and numpad timecodes. Ripple Sequence Markers; the Program Monitor shows the In to Out length and
  platform safe zones.
- Copy and paste effects in Effect Controls, Apply Grade to many clips, a transcript that follows the playhead with a
  word menu, Transcribe that picks the voice tracks, Shift Captions…, Ken Burns presets and five essay and documentary
  templates.
- Import Folder… makes bins; Highlights makes a Short of every accepted moment and Export all queues them; Shorts get
  a lead-in, a tail and their own caption style; "Game under my voice" ducking.
- Quitting asks while exports run, a Background tasks pill with recent messages, failed autosaves and exports say why,
  and Windows stays awake while it exports.

The full list is in the [0.4 release notes](release-notes-0.4.md).

### 0.3.1 (23 October 2026)
- Save Preset… in Effect Controls' right-click menu works, so a clip's animation can be saved as a preset and exported.

### 0.3.0 (23 October 2026)
Wabcut works more like a desktop app:
- Home is a start window with Home, Projects and Learn pages, one Import for files, folders and timelines, and a New
  project page with Advanced settings that can start from your media.
- Premiere's menu bar (File to Help) in the title bar, in place of the left rail. The Window menu opens panels in the
  group you clicked last; Maximize Frame and a tab menu for every panel.
- The Effects panel in Premiere's folders, with List, Compact and Icons views, presets imported and exported per bin,
  and effects applied to every selected clip.
- Type a timecode to jump, ripple-delete a gap, add and delete tracks from a track's menu, marker names on the ruler,
  and default transitions you choose.
- Export remembers your last choices, Settings has a search, and Wabcut can reopen the last project when it starts.
- One Wabcut at a time, and the window opens where you left it.

The full list is in the [0.3 release notes](release-notes-0.3.md).

### 0.2.0 (22 October 2026)
Most of what Premiere Pro has for gaming videos, video essays and documentaries ([how it compares](premiere-parity.md)):
- Premiere's editing and sequence commands, Group / Ungroup, fade handles, the Pen on video opacity and Selection
  Follows Playhead.
- Premiere's 41 video transitions and about 70 video effects, Optical Flow slow motion, Morph Cut, Warp Stabilizer and
  effect presets.
- Colour management with ACES / OpenColorIO and HDR masters; Content Credentials on exports made on Windows.
- Move, scale and rotate in the Program Monitor, rulers and guides, loop playback and a full-screen output.
- Quick Export, your own presets, alpha and image sequences, and a Media Encoder-style queue with watch folders.
- Submixes, sends, the Audio Clip Mixer and Premiere's audio effects; AAF and OMF for audio post; nine caption file
  formats in and out.
- The Project panel: metadata with XMP, search bins, the freeform view, productions, several open projects, the Media
  Browser with camera cards and Ingest, Photoshop layers and camera RAW.
- AI that reads through multicam and nested clips, automatic face redaction, and sound effects generated with your
  own key.

The full list is in the [0.2 release notes](release-notes-0.2.md).

### 0.1.3 (16 October 2026)
- A new look ("Echo") and a new logo: rounded panels, pill controls, browser-style panel tabs, violet and lime.
- Workspaces you make yourself, and panels you dock as tabs, side by side, along an edge or in their own windows.
- One Settings window: custom colours and theme files, the AI switch and your own model names, a left rail you can
  reorder and trim.
- The Program Monitor works like Premiere's: click to select, double-click to edit text, double-click again for what
  is behind.
- Drag files from File Explorer straight onto the timeline; drops overwrite (Ctrl inserts) and never cut other tracks.
- A redesigned Export page, a time bar under the Program Monitor and a Tracks toolbar with Premiere's tool groups.
- Preview builds for Linux (AppImage) and macOS on Apple silicon.

### 0.1.2 (13 October 2026)
- All 27 clip blend modes and the Track Matte Key render on the GPU, so preview matches export.
- Every Premiere default tool key works: Rate Stretch (R), Pen (P) on audio volume bands, Type (T) and Audio Gain (G).
- Discord presets encode a second time when the first pass goes over the size limit.
- Auto colour, colour match and match scene work on multicam clips.

### 0.1.1 (13 October 2026)
- File > Export Timeline: send a sequence to Premiere, Resolve or Final Cut (FCP7 XML, FCPXML, OTIO, EDL).
- Hand (H) and Zoom (Z) tools; Render In to Out (Enter) for smooth playback of heavy sections.
- Green-screen keying (Ultra Key) on the GPU.

### 0.1.0 "First Cut" (13 October 2026)
The first release meant for real projects:
- **Editing:** Premiere's shortcuts and edit rules, nested sequences, multicam, keyframes, speed ramps, version history.
- **Interchange:** one-click import from Premiere, Resolve and Final Cut, with a report of what carried over.
- **Gaming:** named game/mic/Discord tracks, high and variable frame rates, a highlight finder, instant replay,
  facecam and Shorts layouts, 12 templates, creator export presets.
- **Essays and documentaries:** local transcription, text-based editing, animated captions with Thai, voice-over,
  speech enhancement, smart search, colour with scopes, privacy redaction.
- **AI your way:** local models or your own API keys, with a cost shown before every cloud call and a "no cloud" lock.

The full list is in the [release notes](release-notes-0.1.md).

## Next: toward a beta

Make Wabcut safe to trust with real projects:
- Fix what people find in real projects, starting with the [pilot](pilot.md). 0.1 to 0.7 were tested with a demo
  project and scripted test media, not yet with real users' projects.
- Check AAF, OMF and broadcast caption files in Pro Tools, Media Composer and Resolve.
- A code-signing certificate, so Windows SmartScreen stops warning (0.6 has the signing steps and opt-in updates).
- Check the 0.4 drag gestures with a real mouse: Alt-drag copy, Ctrl+Alt-drag rearrange, Shift-drag and folder drops
  are covered by simulated input only.
- Measure local AI quality and live cloud providers on real footage.
- Manual screen-reader checks (NVDA) on top of the automated keyboard and accessibility tests.

## Wanted, not scheduled

- macOS and Linux beyond the preview builds: local transcription, audio plug-ins and the managed local assistant
  there, testing on real machines, and Intel Macs. Windows comes first.
- Warp Stabilizer's mesh warp.
- Live references between open projects (items imported from another project are copies today).
- R3D, BRAW and ARRIRAW decoding (each needs a decoder helper built with the camera maker's SDK).

## Not planned for now

- Live co-editing.
- A plug-in marketplace.
- A Fusion- or After Effects-class compositor.
- Publishing straight to social platforms.

Something missing that you need? Open an issue and say what you are trying to make.
