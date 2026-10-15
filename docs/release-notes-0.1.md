# Wabcut 0.1 "First Cut": release notes (0.1.2)

Windows 10/11 x64. Free and open source (GPL-3.0-or-later). No account, no subscription.

Wabcut 0.1 is the first version meant for real projects: long-form and short-form **gaming videos**, **video essays**
and **documentaries**, edited with Premiere Pro's keyboard and edit semantics, with AI that runs on your machine or
with your own API keys.

## 0.1.2 (13 October 2026, evening)

- **All 27 clip blend modes and Track Matte Key render on the GPU** (preview == export), matching the reference math.
- **Rate Stretch (R)**, **Pen (P)** on volume rubber bands, **Type (T)** in the Program Monitor, and **Audio Gain (G)**
  (set / adjust gain, normalize peaks; Shift+G opens Audio Channels): every Premiere default tool key now works.
- **Discord presets** encode a second time when the first pass ends over the size limit.
- **Auto colour, colour match and match scene work on multicam clips**; Split at Shots reads through the active angle.

## 0.1.1 (13 October 2026, same day)

- **File > Export Timeline...**: send a sequence to Premiere, Resolve or Final Cut as FCP7 XML, FCPXML, OTIO or EDL,
  with the export report (it was command-line only in 0.1.0).
- **Hand (H) and Zoom (Z) tools** in the timeline, as in Premiere.
- **Render In to Out (Enter)**: renders heavy stretches between In and Out to preview files for smooth playback
  (progress in the title bar, click to stop); edits invalidate exactly what they change.
- **Green-screen keying on the GPU**: the Ultra Key effect now renders in preview and export (facecams on a green
  screen), and the Darken blend mode joins Add, Screen, Multiply and Lighten.
- docs/premiere-parity.md: a feature-by-feature comparison with Premiere Pro 26.5 (22 equal, 6 better, 9 partial,
  3 missing of 40 areas), each backed by a test or a release-run step.

## Who it is for

- **Gaming creators:** OBS / ShadowPlay / Xbox Game Bar recordings to YouTube videos, highlight reels, montages and
  vertical Shorts/TikToks.
- **Video essayists:** script, voice-over, B-roll from your own library, animated captions, titles and music.
- **Documentary editors:** interviews and multicam, transcript-based editing, colour, archival media, lower thirds,
  and round trips with Premiere, Resolve and Final Cut.

## Highlights

### Edit like Premiere
- Premiere's default shortcuts (Windows and macOS tables, per panel scope), `.kys` import and a Keyboard Trainer.
- Insert/overwrite, lift/extract, ripple/roll/slip/slide, razor/add edit, trim mode with J/K/L, three- and
  four-point editing, source patching, track targeting and sync lock, markers with ranges and chapters.
- Nested sequences and multicam (angle grid with live switching; sync by timecode or audio).
- Keyframes with Bezier handles, speed/duration, time remapping with speed ramps, freeze frames, reverse.
- Version history, review notes with rendered frames, project merge; a recovery dashboard and portable project capsules.

### Move in and out of other editors
- One-click import of Premiere (FCP7 XML and `.prproj`), DaVinci Resolve (FCPXML/OTIO), Final Cut Pro (FCPXML), OTIO and
  EDL timelines, nested and multicam included, with a Carried / Approximated / Dropped report.
- Timeline export back to FCP7 XML, FCPXML 1.10/1.11, OTIO and EDL, with the same report, from the command line
  (File > Export Timeline..., or `wabcut-cli export-timeline <project> <out> --format fcp7-xml|fcpxml|otio|edl`).

### Gaming
- OBS multi-track recordings: game, mic and Discord audio land on their own named tracks.
- High frame rates (60/120/144/240 fps) and variable-frame-rate recordings, exact in export.
- Highlight finder (local): game-audio spikes, mic excitement, visual intensity and chat-log spikes become ranked
  markers; one click makes a highlight reel or a Short.
- Facecam picture-in-picture and vertical Shorts layouts; green-screen keying; adjustment layers and blend modes.
- Instant replay, slow motion with audible pitch-kept audio, punch-in zoom, camera shake, remove dead time.
- 12 built-in templates (subscribe, kill streak, lower third, replay stinger, countdown, callouts...) and Lottie stickers.
- Export presets: YouTube 1080p60/1440p60/4K60 (H.264, HEVC or AV1), Shorts/TikTok/Reels 1080x1920@60, Twitch, Discord
  file-size targets (10/25/50 MB), GIF and WebM for memes.

### Video essays and documentaries
- Local transcription (whisper.cpp on the GPU), text-based editing (also through multicam and nested clips, which
  stay multicam), filler and silence clean-up.
- Caption workshop: styles, speakers, corrections, animated captions (fade, pop, word by word), Thai shaping,
  translation (local OPUS-MT or your LLM key).
- Voice-over recording with punch-in and takes; Enhance Speech and stem separation (local); Essential Sound presets;
  auto-ducking; loudness to platform targets.
- Smart search over your footage (local visual + transcript search) for B-roll; rough cut from a script; retake picker.
- Colour: white balance, curves, wheels, HSL, LUTs, GPU scopes, auto colour and colour match; masks with tracking and
  AI object masks; source-anchored privacy redaction (blur/mosaic/bleep that follows the footage everywhere).

### AI, your way
- Every AI feature is optional. Local models run on your GPU/CPU; cloud models use **your own key** (Anthropic,
  OpenAI, Gemini, Deepgram, ElevenLabs, AssemblyAI...), stored in the Windows credential store, with a consent ledger, a
  spend cap and a project-level "no cloud" lock.
- An editing assistant whose every action is a typed, previewed, verified, undoable command; a local MCP server
  (read-only by default) for external agents.

### Extensible
- CLAP and VST3 audio plug-ins in a separate process; JavaScript extensions in a sandboxed host with permissions.

## System requirements

- Windows 10 (22H2) or 11, x64; a DirectX 11 GPU (hardware decode/encode used when present: NVIDIA NVENC/NVDEC, AMD AMF,
  Intel Quick Sync); 16 GB RAM recommended (8 GB minimum; local AI models need more); 2 GB disk for the app, more for
  caches and proxies.
- Developed and verified on Windows 11 with an NVIDIA RTX 3060 (32 GB RAM).

## Install

- **MSI** (per user, no administrator rights): `wabcut-0.1.0-windows-x64.msi`. Uninstalling removes program files
  only; projects, autosaves and caches stay.
- **Portable ZIP**: unzip anywhere and run `bin\wabcut.exe`.
- The packages are not code-signed yet, so Windows SmartScreen may warn on first run ("More info" > "Run anyway").

## Known issues
- Windows only in this release; Linux and macOS builds come later.
- Not code-signed; no auto-update.
- Real local-AI quality and live cloud-provider runs are not benchmarked in this release.
- No OpenFX video plug-ins or AU audio plug-ins; no cloud collaboration (versions, review notes and merge are local).
- HDR grading (ACES/OCIO) is deferred; HDR game captures are tone-mapped to SDR.
- The Pen tool edits audio volume bands; video opacity keyframes are set in Effect Controls.
- No Optical Flow frame interpolation for slow motion yet (nearest frame).

## Privacy

Nothing leaves your computer unless you turn on a cloud feature with your own key, and every cloud call is shown with its
cost first. Redacted regions and bleeped words are never uploaded in clear. Crash reports stay local until you choose
to share them, and never contain API keys.
