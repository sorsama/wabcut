# Export

Render a sequence of a fixed project snapshot to a file: exactly what the program monitor shows (Principle 5,
preview == export), as fast as the machine allows (hardware encoders first), never touching the timeline the
user keeps editing. Owner: Export. Code: `src/export` (library `wabcut_export`, namespace `wabcut::exporter`),
`src/app/src/bridge/ExportController.*` + `src/app/qml/panels/ExportPanel.qml` and `panels/export/` (UI),
`wabcut-cli render` and `wabcut-cli watch` (headless), the `export.*` commands (`docs/commands.md`).

0.2 brings it to Premiere Pro 26's Export mode and Adobe Media Encoder's basics: [Quick Export](#quick-export),
[user presets](#presets-save-import-export-delete), [Premiere's formats](#premieres-formats-alpha-dnxhd-audio-only-image-sequences)
(alpha masters, DNxHD, MP3 / AAC / AIFF / Broadcast WAV, image sequences), the [Range](#range) choices,
[Effects](#effects) (overlays, Lumetri Look/LUT, Video Limiter, Loudness Normalization), [Metadata](#metadata)
(chapters, timecode track, XMP), a [soft subtitle track](#captions-soft-subtitle-track),
[smart rendering](#smart-rendering-use-previews-maximum-render-quality-and-depth), and a
[Media Encoder-style queue](#the-queue-media-encoder) with [watch folders](#watch-folders) and a [CPU limit](#cpu-limit).

## Pipeline

```
ExportRequest { snapshot, sequence, settings, output }        (snapshot acquired once: later edits don't apply)
  resolve(settings, sequence) -> range, output size, render size, frame count, audio sample count (exact rationals)
  originals check: every clip in range on a contributing track must resolve to its ORIGINAL file (never a proxy)

video  frame k at sequence time start + k / rate (exact rational output PTS)
  RenderedVideoSource
    decode thread:  playback::evaluateVideoLayers (the preview Scheduler's evaluator)
                    -> media::VideoDecoder per clip (software by default: bit-exact, reproducible)
                    -> PreparedComposite (assembled like Scheduler::publishIfReady)
    render threads: render::OffscreenRenderer (Export role: export ColorPipeline output transform, export
                    texture pool, opaque) at the render size -> RGBA8 readback        (2 QRhi devices)
    backpressure:   at most maxInFlight (4) frames requested and not yet taken
  ConvertStage thread: swscale RGBA8 -> encoder layout (BT.709 limited, left chroma; Fill crop / Fit pad)
  job thread:  libavcodec encoder (hardware first, probed with a test frame; software fallback)
audio  AudioStage thread: playback::renderTrack per track + audio::Mixer (the player's mute/solo rules) in
       1024-frame blocks from the range start -> AAC (priming as an edit list) or PCM (FFmpeg quantization);
       kept within 2 s of the video so the muxer's interleaving buffers stay small
mux    libavformat into <name>.wabcut-partial-<random><ext> next to the output; renamed over the output only
       when complete (a failure or cancel deletes it; an existing output is replaced atomically)
```

Cancelling (a `std::stop_token`) is checked every frame and also stops the video source at once, so a source
stuck in `next()` cannot hold a cancel.

## Settings and presets (`wabcut/export/ExportSettings.h`)

| Preset id | Name | What |
|---|---|---|
| `match-source` | Match Source | H.264 MP4, sequence size and rate, constant quality (CRF/CQ 16), AAC 320 k |
| `youtube-1080p` | YouTube 1080p | H.264 1920×1080, 16 Mb/s VBR (max 24), AAC 384 k |
| `youtube-4k` | YouTube 4K | H.264 3840×2160, 45 Mb/s VBR (max 68), AAC 384 k |
| `vertical-1080x1920` | TikTok / Reels | H.264 1080×1920, centre crop of the canvas (Fill), 12 Mb/s |
| `prores-422-hq` | ProRes 422 HQ | ProRes HQ 10-bit 4:2:2 (prores_ks), PCM 24-bit, MOV |
| `audio-wav` | Audio only | WAV, PCM 24-bit, 48 kHz |
| `youtube-1080p60` | YouTube 1080p60 | H.264 1920×1080 at 60 fps, 12 Mb/s VBR (max 18), AAC 384 k (I.9, gaming) |
| `youtube-1440p60` | YouTube 1440p60 | H.264 2560×1440 at 60 fps, 24 Mb/s |
| `youtube-4k60` | YouTube 4K60 (HEVC) | HEVC 3840×2160 at 60 fps, 40 Mb/s |
| `youtube-1440p60-av1` / `youtube-4k60-av1` | YouTube 1440p60 / 4K60 (AV1) | AV1 at 60 fps, 16 / 30 Mb/s (hardware AV1, else SVT-AV1) |
| `shorts-1080x1920-60` | Shorts / TikTok / Reels 60 fps | H.264 1080×1920 at 60 fps, 15 Mb/s, centre crop of a 16:9 canvas |
| `twitch-1080p60` | Twitch 1080p60 | H.264 1920×1080 at 60 fps, 6 Mb/s capped (Twitch's ceiling), AAC 160 k |
| `discord-10mb` / `discord-25mb` / `discord-50mb` | Discord 10 / 25 / 50 MB | H.264 720p / 1080p sized to stay under 10 / 25 / 50 MB (decimal), AAC 96-160 k |
| `gif-480p15` | GIF (memes) | animated GIF 480×270 at 15 fps, a 256-colour palette per frame, no sound, loops |
| `webm-vp9-720p` | WebM VP9 720p (web, memes) | VP9 1280×720 constant quality (CRF 32), Opus 128 k, WebM |
| `youtube-hdr-pq` | YouTube HDR (PQ) | HEVC Main 10, BT.2100 PQ with HDR10 metadata (hardware HEVC), MP4 (0.2: from an HDR or Wide Gamut sequence) |
| `youtube-hdr-pq-av1` | YouTube HDR (PQ, AV1) | AV1 10-bit, BT.2100 PQ with HDR10 metadata (hardware AV1, else SVT-AV1), MP4 |
| `hdr-hlg` | HDR HLG (HEVC 10-bit) | HEVC Main 10, BT.2100 HLG, MP4 |
| `prores-hdr-pq` | ProRes 422 HQ HDR master (PQ) | ProRes 422 HQ 10-bit, BT.2100 PQ, mastering display and MaxCLL / MaxFALL boxes, MOV |
| `prores-4444-alpha` | ProRes 4444 with alpha | ProRes 4444 10-bit 4:4:4 with the alpha channel, PCM 24-bit, MOV (0.2) |
| `animation-alpha` | QuickTime Animation with alpha | lossless 8-bit RGBA (qtrle), MOV |
| `cineform-alpha` | GoPro CineForm RGB 12-bit with alpha | cfhd, 12-bit RGBA, MOV |
| `dnxhr-hq` / `dnxhr-444` | DNxHR HQ / DNxHR 444 | 8-bit 4:2:2 / 10-bit 4:4:4 at any size, MOV |
| `dnxhd-1080p-hq` | DNxHD 1080p HQ | 1920×1080 8-bit at Avid's HQ bitrate for the frame rate (175 / 185 / 220 / 365 / 440 Mb/s), MOV |
| `audio-mp3` / `audio-aac` / `audio-aiff` | MP3 320 kb/s / AAC audio (M4A) / AIFF 24-bit | audio only |
| `png-sequence` … `exr-sequence` | PNG / TIFF / DPX / JPEG / Targa / OpenEXR sequence | one numbered still per frame |
| `png-still` | PNG still | the range's first frame as one PNG |

**Dynamic range** (`VideoSettings::dynamicRange`, the override `dynamicRange`: auto / sdr / pq / hlg): Auto follows
the sequence's colour setup (an HDR one writes PQ or HLG on a 10-bit HEVC, AV1 or ProRes format, else its tone-mapped
SDR version); PQ and HLG need an HDR or Wide Gamut colour setup. The HDR masters, their tags, the HDR10 mastering
display and the measured MaxCLL / MaxFALL are described in docs/color.md "HDR export".

Containers mp4 / mov / mkv / wav (0.2: mp3, m4a, aiff, image); video H.264, HEVC, ProRes (proxy/lt/standard/hq; 0.2:
4444, 4444 XQ), DNxHR (lb/sq/hq/hqx; 0.2: 444, and DNxHD lb/sq/hq/hqx), 0.2: PNG, TIFF, DPX, JPEG, Targa, OpenEXR,
QuickTime Animation, CineForm;
pixel formats yuv420p, yuv420p10, yuv422p, yuv422p10; rate control quality (CRF/CQ/ICQ/CQP) or bitrate (VBR);
output size with Fit (pad) / Fill (crop) / Stretch; frame rate (default: the sequence's); audio AAC / PCM16 /
PCM24 / AC-3 (FFmpeg's native encoder, up to 5.1, 32-48 kHz; in MP4, MOV and MKV), sample rate, channels (1-8, or 0
= as the master: 5.1 for a 5.1 master, else stereo; A.9), bitrate; range whole sequence / In-Out / work area /
custom. A 5.1 master exported to stereo is downmixed (ITU-R BS.775, docs/ai.md "Audio channels and 5.1"). The presets
stay stereo. `validate()` names every impossible combination (e.g. PCM in MP4); `resolve()` gives the exact frame
and sample counts.

**Target size** (`RateControl::TargetSize`, `VideoSettings::targetBytes`; the Discord presets): the video gets what
the audio and a 4 % margin (container, rate-control overshoot) leave of the size over the exported duration, its peaks
capped at that rate; an export that still ends over its size is encoded once more at the rate that fits
(`ExportResult::attempts`, `::bytes`). A range too long for the size (under 150 kb/s of video) is refused with the
length that fits. H.264, HEVC, AV1 and VP9.

**GIF** (`Container::Gif`, `VideoCodec::Gif`, `OutputPixelFormat::Pal8`): FFmpeg's gif encoder; the converter renders
RGB and libavfilter makes 256 colours for each frame (palettegen `stats_mode=single`, paletteuse `new=1`, sierra2_4a
dithering). No sound, at most 50 fps (GIF frame times are hundredths of a second), loops forever.

**WebM** (`Container::WebM`): VP9 (`vp9_qsv` on Intel GPUs, else libvpx-vp9 at `deadline=good`, `cpu-used=4`,
row-mt; quality maps onto CRF 0..63) or AV1 video, Opus audio (libopus, 48 kHz).

**Captions** (`CaptionExportSettings`; I.4, P.2 as Premiere's Export Options): `None`; `BurnIn` (drawn as the program
monitor draws them); a sidecar file next to the output in Premiere's formats: `Srt`, `Vtt`, `Scc`, `Mcc`, `EbuStl`,
`Ttml` (.xml), `Dfxp`, `SmpteTt` (.xml), `Itt` (interchange/CaptionFiles.h; timecode-based formats from the sequence's
start timecode at the export's first frame; `display608` picks pop-on, roll-up or paint-on for SCC and MCC); or `Embed`:
CEA-608 in the H.264 / HEVC stream as A/53 caption SEI, written by encoders that support it (libx264, libx265, NVENC,
Quick Sync H.264, VA-API, VideoToolbox; automatic selection skips the others). Redacted words are replaced in every
mode. docs/interchange.md, "Caption files".

**High frame rates and VFR** (I.9, gaming): sequences and exports run up to 240 fps; every output frame k shows the
sequence at start + k frame periods, so 120 / 144 / 240 fps captures in a sequence of their rate export frame for frame,
and variable-frame-rate captures (OBS dropping frames under load, phones) show at each sequence frame the capture frame
on screen at that time (tests/export/test_gaming_export.cpp).

## Premiere's formats: alpha, DNxHD, audio only, image sequences

(0.2; [formats Premiere exports](https://helpx.adobe.com/premiere-pro/using/formats-exported-directly-premiere-pro.html).)
Every one goes through the vcpkg FFmpeg build's own encoders (`ffmpeg -encoders`): prores_ks, dnxhd, cfhd, qtrle, png,
tiff, dpx, mjpeg, targa, exr, aac, pcm_s16be / pcm_s24be and, for MP3, Windows' Media Foundation encoder `mp3_mf` (the
build has no LAME). PNG was reported missing by an older test fixture: it is in this build (FFmpeg's `zlib` feature,
in vcpkg.json since M0), so no port feature changed.

- **Include Alpha Channel** (`alpha` override; `OutputPixelFormat::Yuva444p10`, `Rgba`, `Rgba64`, `Gbrap12`, `Rgbaf`):
  ProRes 4444 / 4444 XQ, QuickTime Animation, CineForm RGB 12-bit, PNG, TIFF, Targa, OpenEXR. The canvas is rendered
  with straight alpha (`render::OutputAlpha::Straight`, 8-bit: the 10-bit readback has 2 bits of alpha), and the bars
  of a Fit are transparent. Alpha exports are SDR.
- **DNxHD** (`DnxhrProfile::DnxhdLb`..`DnxhdHqx`): 1920×1080 only, at Avid's bitrate for the frame rate (`dnxhdBitrate`:
  LB 36/36/45/75/90, SQ 115/120/145/240/290, HQ 175/185/220/365/440 Mb/s at 23.976-24 / 25 / 29.97-30 / 50 / 59.94-60);
  HQX (10-bit) exists at 175x / 185x / 365x / 440x, not at 29.97 (this FFmpeg has no 220x). Other sizes: DNxHR.
- **Audio only**: MP3 (32-48 kHz, mono or stereo, 32..320 kb/s; a 5.1 master is downmixed), AAC in an MPEG-4 audio
  file (.m4a), AIFF (16 / 24-bit big-endian PCM), WAV. A WAV export with Metadata > timecode on is a **Broadcast
  WAV**: its `bext` chunk carries the start timecode as the time reference (samples since midnight), the originator
  ("Wabcut"), the date and the title.
- **Image sequences** (`Container::Image`, Premiere's Export As Sequence): one numbered still per frame, "<name>000.png",
  "<name>001.png", … (at least three digits, more for longer ranges), written into a folder of the export's own next
  to the output and moved into place at the end (a failed or cancelled export leaves none). PNG 8 / 16-bit with or
  without alpha, TIFF (LZW), DPX 8 / 10 / 16-bit, JPEG (the 0..51 quality scale), Targa, OpenEXR half float (ZIP).
  Export As Sequence off: a single still of the range's first frame, named as the output.

## Quick Export

Premiere's header Quick Export: the top bar's ⚡ button opens a popover with every preset (built-in and the user's;
not the HDR masters), what it writes, and the file name and location (default: next to the project, named after the
sequence); Export queues it like the Export workspace does (`ExportController::quickExport(presetId, output)`; command
`export.quick`).

## Presets: Save, Import, Export, Delete

Premiere's Save Preset / Import Preset / Preset Manager (`wabcut/export/ExportPresets.h`). A user preset is a JSON file
`"<id>.json"` in the user's "Export Presets" folder (`QStandardPaths::AppConfigLocation`; wabcut-cli: the same folder,
or `WABCUT_EXPORT_PRESETS`): `{"format": "wabcut-export-preset", "version": 1, "id": "user-…", "name", "description",
"settings": {…}}`, the settings in `settingsToJson`'s format. Ids are "user-" and the name in lowercase letters, digits
and dashes. Files are untrusted (they are shared): at most 1 MB, every value type-checked and bounded, unknown keys
ignored (a newer preset still opens), then `validate()`; a folder's broken files are left out and reported, never
fatal. The Export workspace shows them under **My presets** with **Manage…** (save the current settings under a
name, Export Preset…, Delete, Import Preset…); commands `export.preset.save / import / export / delete`.

## Range

Premiere's Range (`RangeMode`): **Entire Source** (the sequence to its last clip), **Source In/Out** (the sequence
marks), **Work Area**, **Custom** (In and Out typed as timecodes in the drawer, or `in` / `out` in the commands;
`customRange` override `{start, end}` in ticks).

## Effects

Premiere's Export Settings > Effects (`ExportEffects`, `src/export/src/FrameEffects.cpp`), applied to every output frame
after the Fit / Fill crop or pad - Premiere's "render overlays in the output frame size" - in this order: Lumetri
Look/LUT, Video Limiter, then Image, Name and Timecode Overlay (so the look never colours an overlay). Frames with
effects go through an RGBA work frame (16-bit when the render is 10-bit) and back to the encoder's format. In the
Export workspace they are the **Effects & Metadata** section under Settings: each effect a switch with its options
under it while it is applied, then Metadata and the render options (`effects` / `metadata` / `render` overrides).

- **Lumetri Look/LUT**: a built-in look (`builtin:teal-orange`, `warm-film`, `cool-night`, `bleach-bypass`, `mono-silver`,
  or the log input LUTs) or a .cube file, sampled into a 33³ lattice (tetrahedral, `color::applyLut`) and applied
  trilinearly. SDR exports only.
- **Image Overlay**: a PNG / JPEG / … image (a watermark, a logo) at **Position** (nine places), **Offset** (output
  pixels), **Size** (% of its own size) and **Opacity**.
- **Name Overlay**: **Prefix** + the source (sequence) name or the output file name + **Suffix**, white on a 60 % black
  box; Position, Offset, Size (100 % = 4.5 % of the frame height), Opacity.
- **Timecode Overlay**: **Time Source** the sequence's timecode (its start timecode + the time) or **Generate** from a
  starting timecode; **Time Display** timecode (drop-frame where the sequence uses it) or frame number; Position,
  Offset, Size, Opacity. Monospaced so the digits do not jitter.
- **Video Limiter**: **Clip Level** (90..109 IRE) and **Compression Before Clipping** (0..20 %): R'G'B' scaled together
  (hue kept, so luma and chroma stay under the level too) with a soft knee. SDR only. Wabcut's renders never exceed
  100 IRE, so 100 IRE changes nothing; lower levels limit.
- **Loudness Normalization**: **Loudness Standard** EBU R 128 (-23 LUFS, -1 dBTP), ATSC A/85 (-24 LKFS, -2 dBTP) or
  custom (target -40..-5 LUFS, **Max True Peak Level** -12..0 dBTP), **Tolerance**, a log file. The mix is measured first
  (ITU-R BS.1770-4 through libebur128: gated integrated loudness, 4x oversampled true peak), then rendered again
  through the gain (none within the tolerance) and the lookahead true-peak limiter of wabcut.audio.limiter, its delay
  compensated, so the export stays sample-exact. The log ("<output>.loudness.txt") says what was measured and done.
- Overlays on HDR exports are drawn at reference white (PQ 0.58, HLG 0.75), not at the format's peak.
- *Not built*: SDR Conform (Wabcut tone-maps HDR to SDR itself) and Time Tuner. The overlays are export-only, as in
  Premiere: the program monitor does not show them.

## Metadata

(`ExportMetadata`, `src/export/src/MetadataWriter.cpp`.)

- **Chapters** from the sequence's chapter markers in the range (MP4, MOV, MKV, M4A): each runs to the next one (or
  its own end), titled with the marker's name (else its comment).
- **Timecode**: the start timecode - **Set Start Timecode**, else the sequence's start timecode plus the range start -
  as a QuickTime timecode track (`tmcd`) in MOV and MP4, the time reference of a Broadcast WAV.
- **XMP**: **Embed in Output File** (MP4 / MOV / M4A: Adobe's top-level XMP `uuid` box BE7ACFCB-97A9-42E8-9C71-999491E3AFAC,
  appended before Content Credentials sign the file; other formats fall back to a sidecar) or **Create Sidecar File**
  ("<name>.xmp"): `dc:title`, `dc:description`, `xmp:CreatorTool` / `CreateDate`, `xmpDM:startTimeCode`, and with
  "include markers" the range's sequence markers as `xmpDM:Tracks` (start and duration in output frames, name,
  comment, Chapter / Segmentation / WebLink type and URL).
- **Title** and **Description** also go into the file's own tags (MP4 / MOV / MKV; WAV: the comment).

## Captions: soft subtitle track

(`SubtitleTrackExport`, `src/export/src/SubtitleStream.cpp`; kept apart from the caption sidecars, burn-in and embedded
CEA-608 captions, which are the Captions mode.) **Subtitle track in the file** puts the sequence's caption track (the
Captions panel's, its redacted words replaced as in sidecars) inside the video as a track players turn on and off: 3GPP
Timed Text (`mov_text`) in MP4 and MOV, SubRip in MKV, times relative to the range, tagged with the caption track's
language (ISO 639-2). A sequence without captions in the range exports without the track and says so in the result's
warnings.

## Smart rendering, Use Previews, Maximum Render Quality and Depth

(`ExportRenderOptions`, `src/export/src/SmartRender.cpp`.)

- **Smart Render** (on by default) and **Use Previews** (off): a ProRes / DNxHR / DNxHD export copies the packets of
  frames it can prove identical instead of rendering and encoding them - a single source clip shown untouched (one
  layer, full frame, no Motion / Opacity / effects / transitions / redactions / colour conversion, its own size and
  rate, an SDR display-referred sequence) whose stream has exactly the export encoder's codec parameters (codec,
  ProRes FourCC / DNx profile, size, pixel format, progressive, colour tags, square pixels, no rotation), and, with Use
  Previews, the frames inside rendered preview segments (C.15, `ExportRequest::previews`) that still show what the
  sequence shows when the export's format is the previews' (ProRes LT or DNxHR LB at the sequence's size and rate).
  The plan reads only file indexes; the encoder then runs with slice threads (no frame delay), so copied and encoded
  packets interleave in order. `ExportResult::copiedFrames` counts them. Copied frames are bit-identical to their
  source (tested). Long-GOP codecs (H.264 / HEVC / AV1) are never smart rendered.
- **Use Maximum Render Quality**: an output smaller than the sequence is rendered at the sequence size and scaled
  down with a Lanczos filter (swscale), instead of composited at the output size.
- **Render at Maximum Depth**: formats deeper than 8 bits (10-bit Y'CbCr, 16-bit / 10-bit / 12-bit RGB, half float)
  are rendered and read back at 10 bits per channel also for display-referred projects (colour-managed projects
  always are).

## The queue (Media Encoder)

Premiere's **Send to Media Encoder** (Alt+Shift+M, `file.sendToMediaEncoder`; the Export workspace's **Send to Queue**)
puts the export into the queue **held** (Media Encoder's "Ready") until **Start Queue**; Export starts it at once.
`ExportQueue` (one worker, in order): `enqueue(request, hold)`, `start`, `pause` (a queued job is held; a running one
stops between two frames, `ExportPhase::Paused`, through its `PauseGate`), `resume`, `cancel`, `remove`. The
**Export Queue** dialog (the queue header's Queue…) lists every job with its progress and per-job start / pause /
resume / cancel, Start Queue, Stop All, Clear Finished; **several destinations** from one export: `sendToQueue(sequence,
[presets…], overrides, hold)` / `export.queue` with several presets (each its own default name, all checked first).

## Watch Folders

Media Encoder's Watch Folders (`wabcut/export/WatchFolders.h`): media files dropped into a watched folder (by
extension: video, audio and image containers FFmpeg reads) are exported with the folder's preset into its Output folder
(`<folder>/Output` unless set), through the same queue. A file is taken once its size and time stayed the same over a
scan (it is not being copied any more); each becomes a project of its own (`projectForMedia`: one sequence playing the
whole file at its own size and rate, each audio stream on a track); its export is "<name><ext>" ("<name>_1…" when
taken); once encoded the source moves into `<folder>/Source` (Media Encoder's behaviour; optional). A file that fails
stays and is not retried until it changes. Only the folder's own files are watched. The app polls every 2 s and keeps
the folders in its settings (`export/watchFolders`); `wabcut-cli watch <folder> --preset <id> [--once]` runs one from
the command line.

## CPU limit

The Export Queue's **Limit CPU use** (No limit / 75 / 50 / 25 % of the logical processors; setting `export/cpuLimit`)
caps the threads of every encoder (`thread_count`, x264 / x265 / SVT-AV1 `threads`), frame converter (swscale) and the
effects stage, and keeps one render thread under 4 threads (`setExportThreadLimit`, `threadLimitForPercent`).
Hardware encoders are unaffected (they hardly use the CPU).

## Commands

`export.presets`, `export.describe`, `export.quick` (Quick Export), `export.queue` (Send to Media Encoder: several
presets and destinations, `hold`), `export.queue.list / start / pause / resume / cancel / remove`, `export.preset.save /
import / export / delete`, `export.watchFolder.add / list / remove` (docs/commands.md). They never change the project:
exports are queued after the request succeeded (a dry run reports what would be written), on the host's queue - the
app's ExportController (so the assistant's exports appear in the Export workspace) or, for wabcut-cli and MCP,
`ExportCommandServices::standalone` (`wabcut-cli cmd` waits for the exports it queued before it exits, prints their
outcome on stderr and exits 2 when one failed). The ones that
write or read a file the caller names (`export.quick`, `export.queue`, `export.preset.import / export`,
`export.watchFolder.add`) are file commands, so hosts that hold file commands back (extensions, confined MCP clients)
refuse them; they name their paths in `output` / `outputs` / `file` / `folder` (checked against MCP's allowed
folders). `export.preset.save / delete` write only the user's own presets folder and, like the queue and watch folder
edits, need the EditProject capability (not for read-only clients).

## Encoders (`wabcut/export/Encoders.h`)

`encoder = "auto"` tries one hardware encoder per GPU vendor present (NVIDIA → nvenc, AMD → amf, Intel → qsv;
VideoToolbox on macOS, VA-API on Linux), each opened with the real parameters and made to encode a test frame,
then the software encoder (libx264, libx265, prores_ks, dnxhd, libsvtav1, libvpx-vp9, gif). The result reports the encoder actually used
and every rejected candidate with its reason. Any other value is an FFmpeg encoder name used without fallback.
`setHardwareEncoderOrder()` / `WABCUT_HW_ENCODERS` override the policy; `probeVideoEncoder()` answers "which
encoder would auto pick" without exporting (the Export workspace shows it).

## C++ API

```cpp
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/RenderedVideoSource.h>

exporter::ExportRequest request;
request.snapshot = session.snapshot();          // ProjectSnapshot
request.sequence = sequenceId;
request.settings = exporter::findPreset("youtube-1080p")->settings;
request.output = "C:/Exports/cut.mp4";
request.projectFile = projectFile;              // relative media paths resolve against it
// request.videoSource: default = renderedVideoSourceFactory() (needs a QGuiApplication in the process)

exporter::ExportQueue queue([](const exporter::ExportJobStatus& s) { /* worker thread */ });
const auto id = queue.enqueue(std::move(request));
queue.cancel(id);                               // or ExportJob(request).run(stopToken) synchronously
```

Errors (`std::expected<…, wabcut::Error>`): InvalidArgument (settings, range), NotFound (unknown sequence,
offline or missing originals — the message lists them), Unsupported (no working encoder), EncoderError,
DecoderError, DeviceError (no QRhi), IoError, Cancelled.

## App integration (ExportController)

`ExportController` is a QML singleton (`import Wabcut`). It reads the `ProjectController` singleton's current
snapshot and runs jobs on its own export queue (one at a time, off the GUI thread).

| Member | Use |
|---|---|
| `presets` | `[{ id, name, description, icon }]`, the presets above (the six general ones with translated names) |
| `jobs` | `ExportJobModel`: roles `jobId` ("j1"), `title`, `outputPath`, `presetName`, `jobState` (held / queued / running / paused / done / failed / cancelled), `phaseText`, `progress` (0..1), `fpsText`, `etaText`, `encoder`, `detail` (done: "0:10 · 6× realtime"; failed: the plain-language error), `finished`; properties `count`, `activeCount`; jobs a watch folder queued join it as they report |
| `presets` (0.2: NOTIFY `presetsChanged`) | also `category` ("general", "hdr", "formats", "user") and `user`; the user's presets after the built-in ones |
| `quickExport(presetId, output)`, `sendToQueue(sequenceId, presetIds, overrides, hold)` | 0.2: Quick Export; Send to Media Encoder / several destinations |
| `start(jobId)`, `startQueue()`, `pause(jobId)`, `resume(jobId)` | 0.2: the queue |
| `savePreset(name, description, presetId, overrides, overwrite)`, `importPreset(file, overwrite)`, `exportPreset(id, file)`, `deletePreset(id)` | 0.2: Premiere's preset manager → `{ ok, id, name, error }` / bool |
| `watchFolders`, `addWatchFolder(folder, presetId, output, moveSources)`, `removeWatchFolder(id)` | 0.2: watch folders (persisted) |
| `cpuLimit` | 0.2: percent (100 = no limit), persisted |
| `busy` | a job is queued or running |
| `autoEncoders` | `{ h264, hevc, prores, dnxhr }` → the encoder "auto" picks, after `probeEncoders()` |
| `queueExport(sequenceId, presetId, overrides)` | queues an export; returns the job id, or "" with `lastError` (plain language) |
| `describe(sequenceId, presetId, overrides)` | what it would write: `{ ok, error, container, videoCodec, format, resolution, frameRate, duration, frames, encoder, quality, audio, range, output, rangeAvailable, dynamicRange (text), dynamicRangeSetting, hdr, hdrAvailable }` |
| `contentCredentials` | "off", "ai" (the default: exports that contain AI-generated media) or "always"; persisted ([Content Credentials](#content-credentials)) |
| `credentialsProducer`, `credentialsThumbnail`, `credentialsDoNotTrain`, `credentialsCertificate`, `credentialsKey`, `signingIdentity()` | what the credentials say and who signs them; persisted |
| `defaultOutputPath(sequenceId, presetId, overrides)` | next to the project file (else the Videos folder), named after the sequence, numbered when taken |
| `cancel(jobId)`, `cancelAll()`, `clearFinished()` | queue control |
| `reveal(jobId)` | shows the output in Explorer (`/select`) / Finder (`-R`) / the file manager |
| `jobQueued(jobId)`, `jobFinished(jobId, succeeded, message)` | signals |
| `project` | the ProjectController it reads (defaults to the singleton) |

`sequenceId` is ProjectController's opaque `"s…"` key; `""` means the current sequence. `overrides` keys (all
optional; 0.2: parsed by the export library, `applyOverrides`): `container` (mp4/mov/mkv/wav; 0.2: mp3, m4a, aiff, image,
gif, webm), `videoCodec` (h264/hevc/prores/dnxhr; 0.2: av1, vp9, gif, png, tiff, dpx, jpeg, targa, exr, animation,
cineform), 0.2: `alpha` (Include Alpha Channel), `pixelFormat`, `proresProfile`, `dnxhrProfile`, `exportAsSequence`,
`subtitles`, `range` "custom" with `rangeIn` / `rangeOut` (timecodes), `effects` / `metadata` / `render` (objects in
`settingsToJson`'s format; timecodes and file URLs in them are converted), `hold`; and `encoder` (auto/software/an
FFmpeg name), `quality` (0..51), `bitrateMbps`, `width` + `height` (0 = the sequence size), `scaleMode`
(fit/fill/stretch), `range` (whole/inOut/workArea), `video` / `audio` (bool), `audioCodec` (aac/pcm16/pcm24),
`sampleRate`, `audioBitrateKbps`, `dynamicRange` (auto/sdr/pq/hlg; PQ / HLG move HEVC and AV1 to 10-bit),
`contentCredentials` (off/ai/always, this export only), `output` (local path or file URL; its extension follows the
container).
Changing the codec or container also moves the dependent defaults (pixel format, PCM in WAV, AAC in MP4).

C++ only: `setProjectFile(path)` (relative media paths and the default output folder; until ExportController
reads `ProjectController::projectFile()` itself — available since I.1, a one-line follow-up — ProjectLifecycle or
the integration should call it on open/save), `queueRequest(ExportRequest, presetName)`,
`setVideoSourceFactory()` and `setRevealHandler()` (tests), `settingsFor()`, `plainError()`.

The editor's export seam (`wabcut/editor/Export.h`, I.1) wraps the same `ExportJob`: `makeExportJobExporter()`
gets the rendered video source by default now.

**Wiring for the integration (task I.1):** the top-bar Export button already switches to the Export workspace
(`onExportRequested: window.workspace = 2`); the panel does the rest. A one-click "export with the last
settings" action can call `ExportController.queueExport(ProjectController.sequenceId, "match-source", {})`
directly. Show `ExportController.busy` / `jobs.activeCount` in the top bar if desired; `jobFinished` carries the
text for a toast.

## Content Credentials

Premiere Pro 26.2's Export Settings > Content Credentials
([Export videos with Content Credentials](https://helpx.adobe.com/premiere/desktop/render-and-export/export-files/export-videos-with-content-credentials.html)):
an MP4 or MOV export can carry a signed C2PA manifest (`src/ai/c2pa`, c2pa-rs 0.91.2; docs/building.md "Content
Credentials (c2pa-rs)") that says how it was made, verifiable by any C2PA reader (e.g. Adobe's Inspect).

- **Export drawer** (MP4 / MOV, where the library loads): **Content Credentials** Off / **AI media** (the default:
  attached when the export contains AI-generated media) / Always; **Producer** (your name or studio: the schema.org
  CreativeWork author, Premiere's identity); **Thumbnail** (on by default: a 320-pixel JPEG of the export's own
  picture in the middle of its range, rendered by the exporter's renderer, redactions included, SDR). The shield
  button opens **Content Credentials settings**: the signing certificate, and "Ask generative AI models not to train
  on or use my exports" (a `cawg.training-mining` assertion with every use "notAllowed").
- **What the manifest says**: created and edited with Wabcut <version>; every source in the exported range as an
  ingredient with its SHA-256; **AI-generated media** (MediaItem::generated, the gen.* commands) declared as
  `trainedAlgorithmicMedia` ingredients and as actions placed where they play (provider, model, the prompt's SHA-256,
  never the prompt), the whole export as `compositeWithTrainedAlgorithmicMedia`; the privacy redactions applied (style,
  where; never their names); the producer, the training request and the thumbnail when set.
- **Signing**: on the export thread after the file is complete (`ExportRequest::finalize`), on a copy that must verify
  (signature, asset hash, certificate profile) before it replaces the export. No network (no timestamp authority,
  no OCSP). Signing never fails an export: a failure leaves the file complete and unsigned, writes the unsigned
  provenance sidecar as before, and says why. A signed export gets no unsigned sidecar.
- **The certificate**: by default a per-user **local identity** made on the first signed export (a local root and an
  ECDSA P-256 signing certificate with the C2PA profile; the private key in Windows Credential Manager, never in a
  file). It proves the file is unchanged since this computer signed it, but viewers show the signer as not trusted.
  **Your own certificate** (from a CA on the C2PA trust list): Content Credentials settings > Signing certificate,
  a PEM chain (signing certificate first) and its PEM private key (PKCS#8); stored as the settings
  `export/c2paCertificate` and `export/c2paKey` (paths). The other settings: `export/contentCredentials`,
  `export/c2paProducer`, `export/c2paThumbnail`, `export/c2paDoNotTrain`.
- **Checking a file**: `wabcut-cli credentials verify <file.mp4|.mov> [--trust <root.pem>] [--json]` reads and
  validates it with the same library (exit 0 valid, 1 missing or invalid, 2 cannot check).
- *Deviations from Premiere*: no Adobe account identity or linked social accounts (Premiere attaches the verified
  identity from Adobe's Content Authenticity site); MP4 and MOV only (Premiere also signs AVI, WAV, MP3 and stills);
  an extra "AI media" mode. Windows x64 only (`WABCUT_WITH_C2PA`).

## Command line

```
wabcut-cli render <project.wabcut> <out> [--preset <id>] [--range all|in-out|work-area]
                  [--sequence <name|id>] [--encoder <name>] [--test-pattern] [--no-audio]
                  [--dynamic-range auto|sdr|pq|hlg]
                  [--content-credentials[=off|ai|always]] [--c2pa-cert <chain.pem> --c2pa-key <key.pem>]
                  [--c2pa-producer <name>] [--c2pa-no-thumbnail] [--c2pa-do-not-train]
                  [--preset-file <preset.json>] [--set '<overrides json>']
wabcut-cli watch <folder> --preset <id> [--output <folder>] [--keep-sources] [--once] [--test-pattern]
wabcut-cli credentials verify <file.mp4|.mov> [--trust <root.pem>] [--json]
```

0.2: `--preset` also takes a user preset ("user-…"), `--preset-file` a preset file, `--set` overrides (e.g.
`--set '{"effects":{"timecodeOverlay":{"applied":true}},"metadata":{"xmp":"sidecar"}}'`); the output's extension may be
.mp3, .m4a or .aif, and an image preset writes numbered stills. The run prints what else it wrote (stills, chapters,
start timecode, subtitle cues, XMP, loudness, smart-rendered frames). `watch` runs a watch folder (`--once`: the files
there now, then exit).

Renders the timeline (a QGuiApplication is created for QRhi; the offscreen platform is used on Linux without a
display); `--test-pattern` encodes the synthetic pattern instead. Progress on stderr; Ctrl+C cancels and
leaves no file; exit codes 0, 2 (failure), 130 (cancelled). Content Credentials default to "ai" (signed when the
export contains AI-generated media), with the local identity unless `--c2pa-cert` / `--c2pa-key` name a certificate.

## Tests and numbers

`ctest --preset win-msvc -L "^(export|export_ui|gpu)$"` runs the export suites (GPU tests carry `[gpu]`, so
GPU-less CI can exclude them with `-LE gpu`).

- preview == export: a 1280×720 3-layer timeline (scaled H.264, ProRes at 50 % rotated 10° with 75 % opacity,
  a display-rotated clip at 60 %, a disabled clip, a gap) exported and decoded, every frame compared with the
  OffscreenRenderer preview of the same snapshot and time: PSNR min 53.8 dB (H.264 via NVENC), 52.2 dB (ProRes
  HQ), exact frame count and timing; AAC lag 0, PCM 24-bit bit-exact against the playback mix.
- benchmark (`wabcut_export_tests "[.benchmark]"`, RTX 3060): synthetic source 1080p60 NVENC 5.68× / x264
  1.31× realtime; real rendered 3-layer 1080p60 timeline NVENC ≈ 2.0× / x264 ≈ 1.2× (a shared, busy machine).

0.2: `tests/export/test_export_parity.cpp` (`ctest --test-dir build/win-msvc/tests/export -R "…"`): presets and settings
JSON (round trip, untrusted input), overrides, PresetStore; ProRes 4444 / Animation / CineForm keep alpha (decoded alpha
0 in the transparent corner, 255 elsewhere); PNG / TIFF / DPX / JPEG / Targa / OpenEXR sequences (names, counts, codecs),
a single still; MP3 / M4A / AIFF / Broadcast WAV (codec, length, `time_reference`); DNxHD 1080p HQ (profile, bitrate);
chapters (MP4, MKV), `tmcd` (MP4, MOV), embedded and sidecar XMP; mov_text / SubRip tracks (cue times, language);
overlay pixels (the logo's colour where it is, dark box and white text where the name and timecode are, nothing changed
elsewhere), a look (no chroma left with mono-silver), the limiter (no channel above 90 IRE); loudness (-16 LUFS target
measured within 1 LU, true peak under the ceiling, sample-exact length); held / paused / resumed jobs; a watch folder
end to end; smart rendering (12 source ProRes frames copied bit for bit, 12 rendered) and Use Previews (the preview
file's packets); Maximum Render Quality (the source asked at the sequence size); the CPU limit; every export command.
`tests/export/ui/test_export_ui.cpp` "Export workspace 0.2: …": the controller (Quick Export, held jobs and Start Queue,
Save / Delete Preset, custom range, watch folders, CPU limit), Send to Media Encoder from the keymap (held, the queue
shown), and the new QML sections, every effect applied, without a Qt warning.

## Known limits

- Embedded captions are CEA-608 CC1 only (no CEA-708 services, no second field), in the video stream (no QuickTime
  caption track, no MXF), as docs/interchange.md says. A smart-rendered export renders every frame when it embeds them
  (copied packets carry no caption data).
- 0.2: MP3 uses Windows' Media Foundation encoder (other platforms need FFmpeg's LAME feature); an MP3 export runs on a
  thread of its own (Media Foundation refuses a GUI thread's single-threaded COM apartment). DNxHD is 1920×1080 only.
  The export overlays are export-only (as in Premiere). Smart rendering covers intra-only ProRes and DNx exports of
  untouched single clips and preview segments, not H.264 / HEVC passthrough. No SDR Conform or Time Tuner. Watch folders
  poll (no file-system notifications) and are not recursive.
- Retimed audio (speed ≠ 1) exports as silence, like playback today; HDR export needs an HDR or
  Wide Gamut colour setup (docs/color.md); HEVC Main 10 needs a hardware encoder (vcpkg's x265 is 8-bit).
- AMF, QSV, VideoToolbox and VA-API option sets are written but only NVENC and the software encoders are
  exercised on the development machine.
- The rendered source reads RGBA8 back and converts with swscale on the CPU; converting to the encoder's YUV
  on the GPU (half the readback) is the next speed step for real timelines.
