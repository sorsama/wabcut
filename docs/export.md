# Export

Render a sequence of a fixed project snapshot to a file: exactly what the program monitor shows (Principle 5,
preview == export), as fast as the machine allows (hardware encoders first), never touching the timeline the
user keeps editing. Owner: Export. Code: `src/export` (library `wabcut_export`, namespace `wabcut::exporter`),
`src/app/src/bridge/ExportController.*` + `src/app/qml/panels/ExportPanel.qml` and `panels/export/` (UI),
`wabcut-cli render` (headless).

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

Containers mp4 / mov / mkv / wav; video H.264, HEVC, ProRes (proxy/lt/standard/hq), DNxHR (lb/sq/hq/hqx);
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

**High frame rates and VFR** (I.9, gaming): sequences and exports run up to 240 fps; every output frame k shows the
sequence at start + k frame periods, so 120 / 144 / 240 fps captures in a sequence of their rate export frame for frame,
and variable-frame-rate captures (OBS dropping frames under load, phones) show at each sequence frame the capture frame
on screen at that time (tests/export/test_gaming_export.cpp).

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
| `presets` | `[{ id, name, description, icon }]`, the six presets above (translated names) |
| `jobs` | `ExportJobModel`: roles `jobId` ("j1"), `title`, `outputPath`, `presetName`, `jobState` (queued / running / done / failed / cancelled), `phaseText`, `progress` (0..1), `fpsText`, `etaText`, `encoder`, `detail` (done: "0:10 · 6× realtime"; failed: the plain-language error), `finished`; properties `count`, `activeCount` |
| `busy` | a job is queued or running |
| `autoEncoders` | `{ h264, hevc, prores, dnxhr }` → the encoder "auto" picks, after `probeEncoders()` |
| `queueExport(sequenceId, presetId, overrides)` | queues an export; returns the job id, or "" with `lastError` (plain language) |
| `describe(sequenceId, presetId, overrides)` | what it would write: `{ ok, error, container, videoCodec, format, resolution, frameRate, duration, frames, encoder, quality, audio, range, output, rangeAvailable }` |
| `defaultOutputPath(sequenceId, presetId, overrides)` | next to the project file (else the Videos folder), named after the sequence, numbered when taken |
| `cancel(jobId)`, `cancelAll()`, `clearFinished()` | queue control |
| `reveal(jobId)` | shows the output in Explorer (`/select`) / Finder (`-R`) / the file manager |
| `jobQueued(jobId)`, `jobFinished(jobId, succeeded, message)` | signals |
| `project` | the ProjectController it reads (defaults to the singleton) |

`sequenceId` is ProjectController's opaque `"s…"` key; `""` means the current sequence. `overrides` keys (all
optional): `container` (mp4/mov/mkv/wav), `videoCodec` (h264/hevc/prores/dnxhr), `encoder` (auto/software/an
FFmpeg name), `quality` (0..51), `bitrateMbps`, `width` + `height` (0 = the sequence size), `scaleMode`
(fit/fill/stretch), `range` (whole/inOut/workArea), `video` / `audio` (bool), `audioCodec` (aac/pcm16/pcm24),
`sampleRate`, `audioBitrateKbps`, `output` (local path or file URL; its extension follows the container).
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

## Command line

```
wabcut-cli render <project.wabcut> <out> [--preset <id>] [--range all|in-out|work-area]
                  [--sequence <name|id>] [--encoder <name>] [--test-pattern] [--no-audio]
```

Renders the timeline (a QGuiApplication is created for QRhi; the offscreen platform is used on Linux without a
display); `--test-pattern` encodes the synthetic pattern instead. Progress on stderr; Ctrl+C cancels and
leaves no file; exit codes 0, 2 (failure), 130 (cancelled).

## Tests and numbers

`ctest --preset win-msvc -L "^(export|export_ui|gpu)$"` runs the export suites (GPU tests carry `[gpu]`, so
GPU-less CI can exclude them with `-LE gpu`).

- preview == export: a 1280×720 3-layer timeline (scaled H.264, ProRes at 50 % rotated 10° with 75 % opacity,
  a display-rotated clip at 60 %, a disabled clip, a gap) exported and decoded, every frame compared with the
  OffscreenRenderer preview of the same snapshot and time: PSNR min 53.8 dB (H.264 via NVENC), 52.2 dB (ProRes
  HQ), exact frame count and timing; AAC lag 0, PCM 24-bit bit-exact against the playback mix.
- benchmark (`wabcut_export_tests "[.benchmark]"`, RTX 3060): synthetic source 1080p60 NVENC 5.68× / x264
  1.31× realtime; real rendered 3-layer 1080p60 timeline NVENC ≈ 2.0× / x264 ≈ 1.2× (a shared, busy machine).

## Known limits

- Retimed audio (speed ≠ 1) exports as silence, like playback today; no alpha or HDR export (HDR game captures are
  tone-mapped into SDR, docs/color.md "HDR game captures"); target sizes are rate-controlled, not two-pass.
- AMF, QSV, VideoToolbox and VA-API option sets are written but only NVENC and the software encoders are
  exercised on the development machine.
- The rendered source reads RGBA8 back and converts with swscale on the CPU; converting to the encoder's YUV
  on the GPU (half the readback) is the next speed step for real timelines.
