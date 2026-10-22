# Wabcut 0.2: release notes (0.2.0)

Windows 10/11 x64, with preview builds for Linux x64 and macOS on Apple silicon. Free and open source
(GPL-3.0-or-later). No account, no subscription.

Wabcut 0.2 closes most of the gaps that were left against Premiere Pro for the three uses Wabcut is made for:
**gaming videos**, **video essays** and **documentaries**. [Premiere parity](premiere-parity.md) compares the two area
by area: of 87 areas, Wabcut is equal in 52, better in 7, partial in 27 and missing in 1.

Projects saved by 0.2 use project file schema 18. Wabcut 0.2 opens 0.1 projects; Wabcut 0.1 cannot open projects
saved by 0.2.

## 0.2.0 (22 October 2026)

### Editing and the timeline
- **Premiere's editing and sequence commands:** Sequence Settings with Premiere's 61 presets, Scale to Frame Size and
  Set to Frame Size, Replace Edit (Alt or Shift onto a clip), Replace Footage, Make Offline, Make Subsequence, Join
  Through Edits, Frame Hold Options, Merge Clips, Interpret Footage, Go to Gap, the History panel,
  and Media and Timeline preferences (default media scaling, still and transition durations).
- **Group and Ungroup.** A group moves as one; trimming its outer edge trims (or, with Ctrl, ripples) every member on
  it in one undo step.
- **Fade handles** on the clip corners, shown on hover (Show Video / Audio Fade Handles), and the **Pen on video
  opacity**.
- **Selection Follows Playhead**, Select Next / Previous Clip on the targeted tracks, a **Zoom tool marquee**, the
  **render bar** on the ruler, **Show Audio Time Units**, and sequence tabs in the sequence's label colour.
- Your Premiere shortcuts go both ways: Keyboard Shortcuts exports a `.kys` file too.

### Motion, effects and colour
- **Time Interpolation:** Frame Sampling, Frame Blending and **Optical Flow** slow motion, and **Morph Cut** for jump
  cuts.
- **Warp Stabilizer** and **Rolling Shutter Repair**.
- **Premiere's 41 video transitions and about 70 video effects** (Distort, Stylize, Generate, Keying, Time, Video,
  Color), the Convolution Kernel effect and its presets, and **Animations** (Block, Fade, Flip, Fold, Pop, Pull, Spin,
  Spring, Travel and Surprise Me) dropped on a clip's edge like a transition.
- **Effect presets:** save any effect stack as a preset, Presets and Lumetri Presets bins, your own bins and favourites,
  import and export. The Lumetri grade exports as a `.cube` LUT.
- **Colour management and HDR:** ACES and OpenColorIO, Premiere's colour setups, log footage recognised from the camera's
  metadata, and HDR masters (Rec.2100 PQ with HDR10 metadata, or HLG). **Color Match** matches the faces' skin tones
  first.
- Bars and Tone and the Universal Counting Leader under New Item.

### Graphics and captions
- **Type on a path**, Roll and Crawl credits, Align and Distribute on several layers, responsive design pinned to the
  video frame.
- **Text Styles** in the Library: drag one onto a graphic clip to restyle all its text, and caption Track Styles with
  Push to Track Style.
- Premiere-made **.mogrt** templates import (their texts and the look of their text layers).
- **Spell check** in captions and text (Windows), **Find and Replace** in captions and the transcript, transcripts
  exported as `.txt`, `.csv` or `.docx`, and captions imported from `.srt`, `.vtt` or timed text.
- **Hide profanity** in captions (f***), with a bleep or silence in the sound.

### Monitors
- Move, scale and rotate in the Program Monitor: the bounding box, handles, anchor point and motion path.
- Rulers and guides with saved templates, a transparency grid, overlays, the dropped frame indicator, Loop, Play In to
  Out, zoom from 10 % to 1600 %, and a full-screen or second-screen output.
- **Export Frame** as PNG, JPEG, TIFF, DPX, Targa, OpenEXR, BMP or GIF.

### Audio
- **Submixes and sends**, track output assignment, and the **Audio Clip Mixer** beside the Audio Track Mixer.
- **Premiere's audio effects** (46 more of them).
- Essential Sound **auto-tagging**: dialogue, music, effects and ambience recognised for you.
- Voice-over takes left by a crash are offered back when the project opens.

### Export and interchange
- **Quick Export** from the title bar, your own presets, ProRes 4444 and other formats with alpha, DNxHR, audio-only
  formats and **image sequences**.
- **Send to Queue** (Premiere's Send to Media Encoder): a queue you start, pause and resume, **watch folders** and a
  CPU limit.
- Export effects (image overlay, name and timecode overlays, a LUT, the Video Limiter, loudness normalization),
  metadata, soft subtitle tracks, a custom range and smart rendering.
- **Content Credentials** (signed C2PA) on MP4 and MOV exports from Windows: who made it and what was made with AI.
- **AAF and OMF** for audio post (Pro Tools), **nine caption file formats** in and out, and CEA-608 captions embedded in
  H.264 and HEVC.

### Project panel and media
- The **Metadata panel**: clip and file metadata, Status and camera fields, edited as XMP (in a sidecar, or in MP4 and
  MOV files themselves when you turn that on).
- **Search Bins**, the **Freeform view**, and a Project panel thumbnail zoom.
- **Productions** with project locks and Production Settings, **several projects open at once** (each with its own
  undo history), and items imported from one project into another with Reveal in Source Project.
- The **Media Browser** (Shift+8): camera cards (AVCHD, Canon XF, P2, XDCAM, XAVC) and spanned clips as single clips,
  preview and hover scrub before importing, and **Ingest** (copy, transcode or create proxies on import).
- **Photoshop files** (merged, individual layers or a sequence), **image sequences**, interlaced footage with Field
  Options, and **camera RAW** (ProRes RAW, CinemaDNG) with RAW Source Settings.

### AI
- AI analysis reads **through multicam and nested clips**: highlights, dead time, Auto Reframe and Shorts, audio sync,
  channel levels, Auto Zoom and object masks.
- **Redact faces** finds the faces for you.
- **Generated sound effects** from a prompt with your own ElevenLabs key (the cost shown first, as always).
- The AI helpers on your computer run below normal priority under a CPU cap (Settings > AI), so playback stays smooth.

### The 0.1.3 workbench, with all of the above
- The new panels (Media Browser, Metadata, Production) dock like every other panel and are in the left rail and the
  Window menu.

## Known issues
- The 0.2 features are verified by automated tests only. None of them has been through a release run with real
  projects yet: keep backups of your projects.
- AAF, OMF and broadcast caption files have been checked with open-source readers, not yet in Pro Tools, Media
  Composer or Resolve. CEA-608 is CC1 only; there is no MXF.
- Items imported from another project are copies, not live references. XMP goes into MP4 and MOV files only (other
  files keep a sidecar).
- R3D, BRAW and ARRIRAW need a decoder helper built with the camera maker's SDK; none ships with Wabcut. RAW Source
  Settings have exposure and white balance only.
- A Premiere .mogrt's layout and animation are not read (its texts and their look are). Spell check needs Windows.
- Warp Stabilizer's Subspace Warp is a per-frame perspective, not Premiere's mesh warp. No face-aware Morph Cut.
- No OpenFX video plug-ins or AU audio plug-ins; no VR / 360; no review links, shared projects or stock media.
- Linux and macOS are preview builds (see the [0.1 notes](release-notes-0.1.md#known-issues) for what they lack).
  Content Credentials are Windows only. On Linux, transcript corrections and the transcript's Find and Replace do not
  apply yet.
- Not code-signed; no auto-update. Real local-AI quality and live cloud-provider runs are not benchmarked.

## Privacy

Nothing leaves your computer unless you turn on a cloud feature with your own key, and every cloud call is shown with its
cost first. Redacted regions and bleeped words are never uploaded in clear. Crash reports stay local until you choose
to share them, and never contain API keys.
