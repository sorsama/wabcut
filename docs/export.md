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
start timecode at the export's first frame; `display608` picks pop-on, roll-up or paint-on for SCC and MCC); or `Embed`
(Embedded captions, 0.6): CEA-608 and / or CEA-708 inside the file - in the picture stream (H.264 / HEVC caption SEI,
MPEG-2 user data), in an MXF SMPTE 436M track or in a QuickTime c608 track. See "Broadcast delivery: MXF and embedded
captions" below. Redacted words are replaced in every mode. docs/interchange.md, "Caption files".

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

## Broadcast delivery: MXF and embedded captions (0.6)

(#73.) For broadcasters and Avid workflows: **MXF OP1a** (one file) and **MXF OP-Atom** (Avid MediaFiles: a file per
essence), and closed captions inside the file as CEA-608 and CEA-708. Everything goes through the vcpkg FFmpeg 9 build's
own muxers (`mxf`, `mxf_opatom`), encoders (`dnxhd`, `mpeg2video`, `libx264`) and bitstream filter (`eia608_to_smpte436m`);
no new dependency.

**Formats** (Settings > Format > **MXF**, then **OP1a** or **OP-Atom (Avid)**; or the presets in Formats):

| Preset | Pattern | Video | Notes |
|---|---|---|---|
| MXF OP1a XDCAM HD422 | OP1a | MPEG-2 4:2:2P@HL long GOP, 50 Mb/s CBR, 8-bit 4:2:2 | 1920×1080 at 23.976-30 fps or 1280×720 at 23.976-60 fps; GOP 12 (24/25/50 Hz) or 15 (30/60 Hz), M = 3, closed |
| MXF OP1a DNxHD HQ | OP1a | DNxHD HQ at Avid's bitrate for the rate | 1920×1080 |
| MXF OP1a DNxHR HQ | OP1a | DNxHR HQ (any DNxHR profile) | any size (DNxHR needs at least 256×120) |
| MXF OP-Atom DNxHD HQ (Avid) | OP-Atom | DNxHD / DNxHR | a file for the picture, one per audio channel |
| MXF OP1a AVC-Intra 100 | OP1a | AVC-Intra 100 (libx264, SMPTE RP 2027 class 100), 10-bit 4:2:2 intra | 1920×1080 at 25 / 29.97 or 1280×720 at 50 / 59.94 |

XDCAM HD422 and AVC-Intra 100 also go into QuickTime. MXF is written at 23.976, 24, 25, 29.97, 30, 50, 59.94 or 60 fps;
other rates, rasters a codec does not define, and non-PCM audio are refused with the reason before the export starts.

**Every MXF file has**: a timecode track (the export's start timecode - Set Start Timecode, else the sequence's start
timecode plus the range start; drop-frame `;` at 29.97 / 59.94 when the sequence uses it), the material package name
(the Metadata title, else the sequence's name), a **reel** (tape) source package when Settings > **Reel** has a name
(Avid's Tape column; `reelName`, at most 64 bytes), the signal standard (SMPTE 274M at 1080 lines, 296M at 720), and the
UMIDs FFmpeg generates (unique per file). The finished job's details show the operational pattern, the timecode, the
audio layout, the material package UMID and the embedded captions.

**Audio**: 16 or 24-bit PCM at 48 kHz (Settings > **Audio**). Settings > **Audio channels** offers 2, 4 or 8 tracks (or
as the master: 2, or 6 for a 5.1 master). OP1a writes **mono tracks** (one per channel: AS-11 / DPP delivery, Avid) or
**one multichannel track** (Settings > **Audio tracks**); OP-Atom always writes one mono file per channel,
"<name>_A1.mxf", "<name>_A2.mxf", … next to "<name>.mxf" (the video). The mix is rendered at its own channel count
(stereo, or 5.1 for a 5.1 master into 6 or more tracks) and spread over the tracks: track n carries mix channel n, the
tracks after the mix are silent (a stereo programme in 8 tracks: 1-2 the mix, 3-8 silence), and the details say so.
The OP-Atom audio files are moved into place only after the picture file is, so a failed or cancelled export never
leaves audio files without their picture.

**OP-Atom and Media Composer: separate clips.** Media Composer makes one master clip of OP-Atom files that share one
material package: the same material package UMID in every file, each file's material package listing every track of
the clip (picture and each audio channel) while it holds the essence of one. FFmpeg 9's `mxf_opatom` muxer cannot write
that: each file gets a material package of its own, listing only its own track, with a random UMID (there is no option
to set it; its bit-exact mode gives every file of every export the same fixed UMID, which would collide in Avid's media
database, so Wabcut does not use it). Wabcut does not patch the files afterwards either (files that claim one clip but
disagree on its tracks cannot be checked without Media Composer). So the picture and each audio channel come into Media
Composer as separate clips with the same name and start timecode: sync them there by timecode (AutoSync or a group by
source timecode). For one clip with every track, export **MXF OP1a** instead, which Media Composer links (Source Browser)
as one clip. The job's details show the picture file's UMID ("picture UMID … (each file has its own)").

**Where OP-Atom files go**: next to each other in the folder you export to, as Premiere and DaVinci Resolve write them.
Media Composer indexes media it finds in a drive's `Avid MediaFiles/MXF/<number>` folder: export into such a folder
(for example `Avid MediaFiles/MXF/1`) to have the files show up in its Media Tool without importing them.

**Embedded captions** (Settings > Captions > **Embed**; `ClosedCaptions.h`): the caption track the program monitor
shows, as

- **Standard**: **608 + 708** (default: digital 708 with the 608 fallback older decoders read), **608** or **708**.
- **608 channel**: CC1 / CC2 (field 1) or CC3 / CC4 (field 2; their miscellaneous control codes are 15h / 1Dh).
- **708 service**: 1-63 (1 is the primary language; 7-63 use the extended service block header).
- **Carry in**: **Auto** - MXF: the SMPTE 436M track; QuickTime: the H.264 / HEVC stream plus a c608 track for CC1 /
  CC2 (other codecs: the c608 track alone); MP4 / MKV: the video stream - or **Video stream**, **Caption track**,
  **Both**.
- **Display**: pop-on (default), roll-up (3 rows; the `captionDisplay` key also takes 2 and 4) or paint-on, for 608 and
  708 alike.

Each frame carries the same ATSC A/53 `cc_data`: 600 / fps triplets (25 at 23.976, 24 at 25, 20 at 29.97, 12 at 50, 10
at 59.94; the 9600 bit/s of a DTVCC channel): the 608 byte pairs of the SCC writer's timing and a null pair on each field
not in use, then one DTVCC packet of the 708 service, then 708 padding. 708 pop-on captions are loaded ahead of time into
a hidden window - windows 0 and 1 take turns, shown on the caption's first frame and hidden at its end (a gap) or when
the next one shows; roll-up is one window of 2-4 rows that scrolls a line up per line; paint-on writes into a visible
window. Windows sit at the bottom or top of the safe area, justified as the track is aligned, at most 32 columns and 4
rows (pop-on); the pen takes the track's text colour, the box colour and opacity as its background, and an outline as a
uniform edge, on 708's 64-colour palette. Text is ASCII, Latin-1, the music note and 708's G2 symbols (curly quotes,
ellipsis, bullet, ™, Š, Œ, …); other characters become `?`. A caption that cannot be loaded before it starts is shown
late; a missing caption track, or one without captions in the range, gives a warning and a file without captions -
never a silent success. Each problem (late, rewrapped, replaced characters) is in the job's warnings.

Where they go: in the picture through encoders that write caption data (libx264, libx265, NVENC, Quick Sync H.264,
VA-API, VideoToolbox, mpeg2video; AVC-Intra does not); an MXF OP1a file's last stream is an SMPTE 436M ANC track (one CDP
per edit unit: ANC DID 61h / SDID 01h on line 9, the CDP frame-rate code of the export's rate); a QuickTime file gets a
`c608` closed caption track (`clcp` handler; field 1 pairs, one sample per frame, two frames at 50 / 59.94).

**Refused, with the reason** (validate / resolve): 708 where nothing can carry it (QuickTime with ProRes, DNxHR, …: only
c608 is written - choose 608); captions in the picture of a codec that has no caption data (ProRes, DNxHR, AV1, VP9,
AVC-Intra); a caption track in MP4 / MKV; CC3 / CC4 in a QuickTime caption track; any embedded captions in OP-Atom (one
essence per file: write an SCC or MCC sidecar, or export OP1a).

Override keys (`ExportController.queueExport`, the export commands, `wabcut-cli render --set`): `container` "mxf" /
"mxf-opatom", `videoCodec` "xdcam-hd422" / "avc-intra-100", `audioChannels` 4 / 8, `reelName`, `mxfAudioLayout` "mono" /
"multichannel", `captions` "embed", `captionStandard` "608+708" / "608" / "708", `cc608Channel` 1-4, `cc708Service`
1-63, `captionCarriage` "auto" / "video" / "track" / "both", `captionDisplay` "pop-on" / "roll-up-2" / "roll-up-3" /
"roll-up-4" / "paint-on". Settings JSON: `captions.standard` / `channel608` / `service708` / `carriage` / `display608`
and `mxf.audioLayout` / `reelName`, written only when set (older presets stay byte for byte). Smart rendering is off for
MXF (its constant-size DNxHD index wants every frame from one encoder).

## Quick Export

Premiere's header Quick Export: the top bar's ⚡ button opens a popover with every preset (built-in and the user's;
not the HDR masters), what it writes, and the file name and location (default: next to the project, named after the
sequence); Export queues it like the Export workspace does (`ExportController::quickExport(presetId, output,
overrides)`; command `export.quick`). The preset list is a keyboard list (Up / Down pick, Enter exports). A sequence
with In / Out marks or a work area adds a **Range** row (Entire sequence / In to Out / Work Area; the Export button
names the range). Quick Export says when it starts (a toast with Cancel) and when it is done.

## Export remembers the last choices

Per user, not per project (QSettings `export/*`, `ExportController` `lastPreset`, `lastGamingPreset`,
`lastQuickPreset`, `lastFolder`, `lastRange`):

- **Preset.** The Export workspace starts on the preset (or gaming preset) picked last; Quick Export on its own last
  pick. A user preset deleted since falls back to Match Source.
- **Folder.** An export to a folder you chose (Browse… or a typed path) makes it the default for the next one: the
  default file name, both Browse… dialogs and Quick Export start there. When that folder is gone (an unplugged drive),
  exports go next to the project again.
- **Range.** The range picked last (Entire Source, In to Out, Work Area) when the sequence has it; with nothing picked
  yet, a sequence with In / Out marks exports In to Out (`suggestedRange`). The Export button says so ("Export In to
  Out", "Export Work Area"). The range stays when you pick another preset: it belongs to the sequence.
- **Reset to defaults** (the Save to card) forgets all of it: Match Source, next to the project, the whole sequence
  unless the sequence is marked.
- **Vertical sequences (0.6).** A 9:16 sequence (taller than wide) remembers presets of its own - the Export workspace's,
  the gaming card's and Quick Export's (`export/lastPresetPortrait`, `lastGamingPresetPortrait`,
  `lastQuickPresetPortrait`; `ExportController.rememberedFor(sequenceId)`) - so YouTube 1080p60 picked for a landscape
  video is never preselected for a Short: a vertical sequence with nothing remembered starts on Match Source (its own
  size). Switching between sequences of the two shapes switches the choice; a pick made for a sequence in this session
  stays with it.

## Before an export starts (0.6)

- **The estimate covers every format.** The tiles' and the panel's "≈ size · time" prices AV1 and VP9 like H.264 /
  HEVC (a size target, a bitrate, or constant quality with the codec's factor: AV1 0.55, VP9 and HEVC 0.65 of H.264's
  bits), GIF by its palette (about 0.8 bits a pixel), stills and image sequences per frame from the pixel format and the
  format's compression (PNG about half, TIFF LZW and OpenEXR 0.6, DPX and Targa as they are, JPEG a quarter byte a
  pixel; a single still is one frame), CineForm like ProRes HQ, QuickTime Animation at about half its RGBA bytes, and
  Opus and MP3 sound at their bitrate. Estimates of the picture-dependent formats say so.
- **Shapes.** A 16:9 gaming preset on a 9:16 sequence says the sequence is pillarboxed (black bars at the sides) and
  points at Shorts / TikTok / Reels; any vertical export longer than 3:00 says YouTube will treat it as a regular video,
  not a Short. Neither blocks the export.
- **Size limit.** The settings drawer's **Size limit** row (H.264, HEVC, AV1 and VP9): Off, 8, 25, 100 or 500 MB, or your
  own (1-4000 MB, typed and Enter; the last one typed is remembered) - WhatsApp, email, X, Discord Nitro. The whole file
  stays under that many decimal megabytes (the export's target size: the video gets what the sound and the container
  leave, and a file that still ends over it is encoded once more). The Quality row waits while a limit is set; the
  summary says "Fits 25 MB (≈ 6.4 Mb/s)", a range too long for the size is refused with how much fits, and under 0.03
  bits a pixel the panel warns that the picture will block up (a smaller frame, a lower rate or a shorter range looks
  better). Override key `targetMB`.
- **Offline media.** The export never uses proxies, so an offline or missing original file would stop it. The Export
  panel and Quick Export find them before you start (the media the range plays, nested and multicam clips included):
  "1 offline file(s) in this export: A001.mov. Relink to export." with **Relink…** (the Relink dialog), and Export and
  Send to Queue wait until they are relinked.
- **Disk space.** The estimate is checked against the free space of the drive the file goes to (the chosen file, else
  the default one): when the file needs more than is free (with a 20 % margin) the panel and Quick Export say "Only
  2.1 GB is free on Data; about 3.4 GB is needed for the export." The export stays allowed (the estimate can be high).
  While exports run their drives are checked every 30 seconds and a drive under 2 GB is said once in a toast. Render In
  to Out (ProRes LT previews) and Create proxy check their folders the same way and do not start when the drive is
  short (the toast says why).
- **Retry.** A failed or cancelled export from the Export workspace, Quick Export or Send to Queue has **Retry** on its
  queue row: the same sequence, preset and settings on the project as it is now (relinked media is found), into the
  same file, or "name (2).mp4" when that name is taken by then. A job that stopped on offline media also has
  **Relink…** on its row.

When an export ends anywhere in the app, a toast says so: "Exported <file>" with **Open** (the file in its default
app) and **Show in Explorer** beside it (Finder on macOS; both still find the file after the queue's **Clear
finished**, and say so when the file was moved or deleted), or "Export failed: <why>" - the same reason as its queue
row, e.g. "Export failed: Some original media is offline or missing: sine_48k.wav. Relink it, then export again." -
with **Show the queue** (the Export workspace, or the Export panel where your workspace has one; your layout is not
changed). When the export failed because original media is offline, the toast offers **Relink…** first (Home closes
and the Relink dialog opens) with Show the queue beside it. A failure toast is an error: it is never covered by a later
message (those wait their turn) and it is kept in the title bar's **Background tasks** popover under Recent messages.
Watch folders and Media Browser transcodes report in their own places; a cancelled job says nothing; a finished export
from the Export workspace while its queue is in view needs no toast. Render In to Out reports the same way ("Previews
rendered", or why it failed with **Try Again**). In the queue (the Export panel, the Export Queue dialog and the
Background tasks popover) a finished job has **Open** before **Show**, and a double-click on it opens it too; a file
that is gone says "The file was moved or deleted." under its row.

**While it exports.** Wabcut keeps Windows from going to sleep on its own while exports (running or queued to run, not
paused or held), Render In to Out, proxies, transcription, Highlights, Shorts or Collect Files run (Settings > General >
Background work > **Keep the computer awake while exporting**, on by default; the screen can still turn off, and Sleep
chosen by hand, closing the lid or a low battery still sleep). `powercfg /requests` names Wabcut while it holds the
computer awake. The taskbar button shows the current export's progress (yellow while it is paused; Render In to Out's
when no export runs), turns red when an export fails while you are in another app, and flashes when an export or Render
In to Out ends while Wabcut is not the active window; that message's time starts only once you are back. Keeping the
computer awake is Windows only for now; the taskbar flash (a Dock bounce on macOS, urgency on Linux) works everywhere.

**Quitting while exporting.** Closing Wabcut (the title bar's ×, File > Exit, Alt+F4, the taskbar) while exports run,
wait in the queue, are paused or wait for Start Queue, or while Render In to Out runs, asks first ("Quit while
exporting?"): what is going on ("“Highlights.mp4” is exporting (38 %). 1 more export waits to start."), and that
exports that have not finished are lost (previews rendered so far are kept). **Keep Wabcut Open** (Enter, Esc) changes
nothing; **Quit When Done** (while something runs that ends on its own) keeps Wabcut open and closes it once the running
work is done - paused and held exports are cancelled then, unsaved projects still ask, and a failure meanwhile calls it
off ("… Wabcut stayed open."); **Cancel Exports and Quit** cancels every export (and stops Render In to Out), then the
close goes on to the unsaved-projects questions. Closing a project, opening another or going Home does not ask: exports
render a snapshot and keep running. The queue is not kept across restarts.

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
- 0.6: **SDR Conform** (an HDR or Wide Gamut sequence delivered as SDR: target peak, paper white, BT.2390 / ACES /
  Reinhard tone mapping, ACES / saturation / clip gamut mapping, legal range) and the **output colour space** (Rec.709,
  Rec.2020 SDR, P3-D65, Rec.2100 PQ / HLG) are the sequence's (Color Management) and each export preset's
  (`video.outputSpace`, `video.sdrConform`; `--set '{"outputSpace": "p3-d65"}'`; the CLI's `--output-space` and
  `--sdr-conform*`). The drawer's details show the colour chain, the job's details what SDR Conform mapped; see
  [color.md](color.md) "HDR finishing (0.6)".
- *Not built*: Time Tuner. The overlays are export-only, as in Premiere: the program monitor does not show them.

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

## Audio stems: dialogue, music and effects (0.6)

Export settings > **Audio stems** > **By track role** (Premiere's multichannel stems, Resolve's Deliver stems; a DME or
M&E delivery) also writes one audio-only file per **track role** next to the export: the audio tracks' roles from the
Audio Track Mixer / Essential Sound (Dialogue, Music, Effects; tracks without a role are "Other"). A line under the row
names the files the export will write, e.g. "Also writes 3 stem(s), 24-bit WAV, next to the export: Dialogue
(Episode 12_DX.wav), Music (Episode 12_MX.wav), Other (Episode 12_OTHER.wav)."

* **Which stems.** A role is written when at least one of its audio tracks is played by the export: enabled, not
  muted, soloed when any track is soloed, and with a clip on it. So the stems split exactly what the master plays.
  Nested sequences go with the role of the track they sit on. A multicam source sequence has no stems.
* **What a stem is.** An ordinary export of a copy of the project in which that role's tracks are soloed and every other
  track (and submix) is not, through the same mix path as the master: WAV, 24-bit PCM, the master's sample rate (48 kHz
  when the export has no audio), the master's channel format (a 5.1 master gives 5.1 stems), the same range and the same
  Broadcast WAV start timecode. So the stems line up with the master sample for sample, and without master effects
  they add up to it. Stems are never loudness-normalised (Effects > Loudness Normalization applies to the master only)
  and carry no captions, XMP or Content Credentials.
* **Master effects.** The master bus's effects (a limiter, an EQ) stay on by default, as Premiere renders stems through
  the master. **Stems without master effects** leaves them out (the master fader stays).
* **Names and the queue.** `<export name>_DX.wav`, `_MX.wav`, `_FX.wav`, `_OTHER.wav` next to the export. They are
  queued right after the export (held with it when sent to the queue). Each is a job of its own in the queue
  ("Dialogue stem"), so a failed stem can be retried on its own. Send to Queue with several destinations writes one set,
  after the first destination.
* **Overrides and commands.** `ExportController.queueExport(..., { stems: "role", stemsMasterEffects: false })`;
  `describe` returns `stems` (`[{ key, name, suffix, tracks, path, file }]`), `stemsOn` and `stemsText`. A stem's own
  request carries `stemOnly: "dialogue" | "music" | "effects" | "other"` (with `output` its file). `export.quick` and
  `export.queue` take `"stems": "role"` and `"stemsMasterEffects"`; the stems appear in `"exports"` with a `"stem"` key
  (with `"ranges"`, each range gets its own stems). The C++ side is `wabcut/export/Stems.h`.
* **Not built.** Stems by submix (each submix track as a stem) would need routing changes and is left for later.

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

### The queue is kept across restarts (0.6)

As Adobe Media Encoder keeps its queue between sessions, Wabcut keeps every export that has not finished. At each change
of the queue (an export queued, started, paused, finished, cancelled or removed; not its progress) the app writes the
queue folder in its settings folder (`Export Queue`): `queue.json` with one entry per unfinished export (its file,
preset, settings, sequence and state) and a copy of the project as it was when the export was queued (it may hold edits
that were never saved; exports queued from one version share one copy). On the next start:

- Exports that were waiting (held or queued) come back **held**: Ready, waiting for **Start Queue**. Nothing starts by
  itself after a restart, so an export can't overwrite a file or fill a drive by surprise.
- An export that was running or paused (Wabcut was closed, or crashed, during it) comes back **interrupted**: a
  finished row with **Retry**, which exports it again from the start into the same file (also when that file is the one
  it was replacing; a file written there by something else since it started gets a numbered name, "Film (2).mp4"). It
  keeps exporting the project it was queued with, not the project open now. If its file was completed before Wabcut
  closed, it comes back done - never when the file there is still the one it was replacing, or when its temporary file
  (`<name>.wabcut-partial-…`) is still next to it.
- A toast says what came back ("3 export(s) from last time are waiting"), with **Start Queue** and **Clear** (Clear
  cancels them).

A finished, cancelled or removed export leaves the folder, and its project copy goes with the last export that uses
it. Ingest transcodes and watch-folder exports are not kept (a watch folder queues again from its folder). Only one
Wabcut keeps the queue: a second one running at the same time keeps its queue in memory as before. When you quit with
exports in the queue, the quit question says they are kept and offers **Keep Queue and Quit**; **Cancel Exports and
Quit** cancels them for good. API: `ExportController.restoreQueue()` → `{ count, held, interrupted, done, skipped }`
(once per run), `clearRestored()`, `keepsQueue`; queue rows have a `restored` role and the state `"interrupted"`.

### Export several sequences at once (0.6)

Select sequences in the Library and choose **Export…** (one sequence: it opens in the Export workspace) or **Send to
Export Queue…** in the item menu, or press **Send to Media Encoder** (Alt+Shift+M) while the Library has the focus. The
**Export Sequences** dialog shows what each sequence becomes before anything is queued:

- **Preset**: one preset for every sequence, or **Match each sequence's shape** (each sequence's last preset for its
  shape; else YouTube 1080p60 for landscape and Shorts / TikTok / Reels 1080×1920 for vertical sequences).
- **Folder**: each file is named after its sequence ("Long edit.mp4"), numbered when the name is taken on disk or in the
  queue; the folder becomes the next default.
- **Wait for Start Queue** (on for Send to Export Queue…).

A sequence that cannot export (offline media, a preset that does not fit) says why in the list, and then nothing is
queued. API: `sequenceExports(keys, options)`, `queueSequences(keys, { preset | gaming | matchShape, folder, hold })`.

### Each marker or clip as its own file (0.6)

For highlight clips, client selects or Discord uploads: **Export each marker range…** in the Markers panel's export menu,
or **Each marker…** / **Each selected clip…** under Range in the Export panel's settings. Every marker with a duration
(optionally of one colour), or every selected clip (a linked video and audio clip counts once), becomes one export,
named "<sequence> - <marker or clip name>.<ext>" and numbered when taken. The dialog says "12 file(s) · ≈ 240 MB · 3:20"
before anything is queued, warns when the drive is short, and an empty range refuses the whole batch. API:
`queueRanges(sequence, preset, [{ in, out, name }], overrides, hold)`, `markerRanges(sequence, colorIndex)`,
`selectedClipRanges()`, `describeRanges(...)`; command `export.queue` with `ranges` (one preset) and `folder`.

### Media usage report (0.6)

**File > Export > Media Usage Report…** lists every source the sequence uses, for archive clearances, music licensing
and credits: one row per use with the media's name, file, tape (reel) name, scene / shot / take, whether it is
AI-generated, the track, the record In / Out (the sequence's timecode from its start time), the source In / Out (the
media's own timecode, at the clip's speed), the duration, seconds and speed; then a totals block per source (uses,
seconds on screen, seconds of the source shown, overlapping uses counted once). Options: the whole sequence or In to
Out; **Report through nested sequences and multicam clips** (on: nested clips are listed in the top sequence's
timecode, a multicam clip by its active angle; off: a nested clip is one row); CSV (UTF-8 with a byte order mark, so
spreadsheets read accents and non-Latin names) or JSON. A linked video and audio clip is one use; titles, mattes and
graphics are not listed; a time-remapped clip is reported as its whole range with the note "remapped". Command:
`sequence.mediaUsage { sequence, range, format, flatten }` (read only; it returns the text and the report).

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
| `jobs` | `ExportJobModel`: roles `jobId` ("j1"), `title`, `outputPath`, `presetName`, `jobState` (held / queued / running / paused / done / failed / cancelled), `phaseText`, `progress` (0..1), `fpsText`, `etaText`, `encoder`, `detail` (done: "0:10 · 6× realtime"; failed: the plain-language error), `finished`, 0.4 `errorKind` (failed: "offline", "space", "access", "folder", "encoder" or ""); properties `count`, `activeCount`, 0.4 `runningCount` (running or queued to run), `pausedCount`, `heldCount`, the current job `currentJobId` / `currentTitle` / `currentState` (running, else paused, else queued; "" for none) / `currentProgress` (-1 for none) / `currentEta` (NOTIFY `summaryChanged`); jobs a watch folder queued join it as they report |
| `presets` (0.2: NOTIFY `presetsChanged`) | also `category` ("general", "hdr", "formats", "user") and `user`; the user's presets after the built-in ones |
| `quickExport(presetId, output, overrides)`, `sendToQueue(sequenceId, presetIds, overrides, hold)` | 0.2: Quick Export (UX rework: optional `overrides`, e.g. the range); Send to Media Encoder / several destinations |
| `lastPreset`, `lastGamingPreset`, `lastQuickPreset`, `lastFolder` (+ `lastFolderUrl`), `lastRange`; `suggestedRange(sequenceId)`, `folderUrlOf(path)`, `forgetExportChoices()` | UX rework: [the last choices](#export-remembers-the-last-choices), NOTIFY `exportMemoryChanged` |
| `rememberedFor(sequenceId)`, `lastTargetMB` | 0.6: the last choices for the sequence's shape `{ portrait, preset, gamingPreset, quickPreset }` ([vertical sequences](#export-remembers-the-last-choices)); the last custom size limit |
| `retry(jobId)`, the `jobs` role `retryable` | 0.6: queues a failed or cancelled job again (its sequence, preset and overrides) into the same file - also when a file was there before the job was queued (the one it was to replace); a numbered file ("name (2).ext") when another job writes that name or a file was written there since → the new job id, or "" with `lastError` |
| `relinkRequested()`, `diskSpaceLow(message)` | 0.6 signals: the panel, Quick Export or a job row asks Main for the Relink dialog (QML emits it); a running export's drive is under 2 GB (once per drive) |
| `job(jobId)`, `openOutput(jobId)` | UX rework: one job for its toast `{ jobId, title, outputPath, outputUrl, folderUrl, presetName, state, detail, finished, origin, exists, errorKind }` (`origin`: export, quick, queue, transcode, watch, command; also a `jobs` role; 0.4 `errorKind` as the role); opens a done job's file in its app (0.4: `jobId` may also be the absolute path of an export's file, so Open works after Clear Finished; false when the file is gone) |
| `start(jobId)`, `startQueue()`, `pause(jobId)`, `resume(jobId)` | 0.2: the queue |
| `savePreset(name, description, presetId, overrides, overwrite)`, `importPreset(file, overwrite)`, `exportPreset(id, file)`, `deletePreset(id)` | 0.2: Premiere's preset manager → `{ ok, id, name, error }` / bool |
| `watchFolders`, `addWatchFolder(folder, presetId, output, moveSources)`, `removeWatchFolder(id)` | 0.2: watch folders (persisted) |
| `cpuLimit` | 0.2: percent (100 = no limit), persisted |
| `busy` | a job is queued or running |
| `autoEncoders` | `{ h264, hevc, prores, dnxhr }` → the encoder "auto" picks, after `probeEncoders()` |
| `queueExport(sequenceId, presetId, overrides)` | queues an export; returns the job id, or "" with `lastError` (plain language) |
| `describe(sequenceId, presetId, overrides)` | what it would write: `{ ok, error, container, videoCodec, format, resolution, frameRate, duration, frames, encoder, quality, audio, range, output, rangeAvailable, dynamicRange (text), dynamicRangeSetting, hdr, hdrAvailable }`; 0.6: `offline` (names) and `offlineCount`, `targetMB`, `targetMbps`, `lowBitrate`, `lowBitrateNote`; `stems`, `stemsOn`, `stemsText` ([audio stems](#audio-stems-dialogue-music-and-effects-06)) |
| `contentCredentials` | "off", "ai" (the default: exports that contain AI-generated media) or "always"; persisted ([Content Credentials](#content-credentials)) |
| `credentialsProducer`, `credentialsThumbnail`, `credentialsDoNotTrain`, `credentialsCertificate`, `credentialsKey`, `signingIdentity()` | what the credentials say and who signs them; persisted |
| `defaultOutputPath(sequenceId, presetId, overrides)` | in `lastFolder` when it exists, else next to the project file (else the Videos folder), named after the sequence, numbered when taken |
| `cancel(jobId)`, `cancelAll()`, `clearFinished()` | queue control |
| `reveal(jobId)` | shows the output in Explorer (`/select`) / Finder (`-R`) / the file manager |
| `jobQueued(jobId)`, `jobFinished(jobId, succeeded, message)` | signals (0.4: `message` is the failed job's reason, the row's `detail`) |
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
`contentCredentials` (off/ai/always, this export only), 0.6: `targetMB` (a size limit, 1-4000 decimal MB; 0 = off),
`output` (local path or file URL; its extension follows the container).
GamingExport's `estimate(sequenceId, presetId, overrides)` (0.6) also returns `freeBytes` (-1 unknown), `freeText`,
`folder`, `lowSpace` and `lowSpaceNote` for the output's drive.
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

0.6 (#73): `tests/export/test_mxf_export.cpp` checks the files with ffprobe and libavformat and reads the captions back:
XDCAM HD422 720p50 OP1a with 8 mono PCM tracks (track 1 the mix, 3 and 8 silent), timecode, reel, material package UMID,
OP1a UL and an SMPTE 436M track (one CDP per frame, DID / SDID, frame-rate code); DNxHD at 23.976, 25, 29.97, 50 and
59.94 fps (rate, drop-frame timecode, CDP rate code, cc_count, 608 and 708 back at each rate); OP-Atom (OP-Atom UL, a
DNxHR file and four mono PCM files, timecode and reel on each); AVC-Intra 100 720p50 (High 4:2:2 Intra, 10-bit) with
one 4-channel track; H.264 in QuickTime at 29.97 and 59.94 with 708 service 2 in the caption SEI and 608 in a c608
track; a 708 service with two pop-on windows, back-to-back captions and a gap; roll-up on an extended service with 608
on CC3; paint-on at 59.94; a late caption reported; the refusals; missing and empty caption tracks; JSON and overrides.
608 is decoded by FFmpeg's own `cc_dec` (a reference independent of the writer), 708 by `readCea708`.
`tests/export/ui/test_mxf_export_ui.cpp`: the summary and the finished job's details, and the drawer's MXF and
Embedded captions rows (screenshots `export-mxf-{dark,light}.png`, `export-mxf-captions-{dark,light}.png`).

## Known limits

- Embedded captions (0.6): one caption track a file (one 608 channel and one 708 service; no bilingual CC1 + CC3 /
  service 1 + 2 yet); QuickTime `c708` tracks are not written (FFmpeg's MOV muxer has no c708 writer: 708 goes into the
  H.264 / HEVC stream instead); OP-Atom files carry no captions; 708 windows are at most 32 columns (4:3 safe title) and
  use the predefined window and pen styles with the track's colours (no per-cue positions, fonts or italics). A
  smart-rendered export renders every frame when it embeds them (copied packets carry no caption data).
- MXF (0.6): progressive pictures only (no interlaced 1080i XDCAM / AVC-Intra); OP-Atom files are separate clips in
  Media Composer (FFmpeg gives each file its own material package; Avid's own MediaFiles list every track in one) - an
  Avid import has not been tried here (no Media Composer); no D-10 (IMX), AS-11 / DPP metadata or AVC-Intra 50.
- 0.2: MP3 uses Windows' Media Foundation encoder (other platforms need FFmpeg's LAME feature); an MP3 export runs on a
  thread of its own (Media Foundation refuses a GUI thread's single-threaded COM apartment). DNxHD is 1920×1080 only.
  The export overlays are export-only (as in Premiere). Smart rendering covers intra-only ProRes and DNx exports of
  untouched single clips and preview segments, not H.264 / HEVC passthrough. No Time Tuner. Watch folders
  poll (no file-system notifications) and are not recursive.
- Retimed audio (speed ≠ 1) exports as silence, like playback today; HDR export needs an HDR or
  Wide Gamut colour setup (docs/color.md); HEVC Main 10 needs a hardware encoder (vcpkg's x265 is 8-bit).
- AMF, QSV, VideoToolbox and VA-API option sets are written but only NVENC and the software encoders are
  exercised on the development machine.
- The rendered source reads RGBA8 back and converts with swscale on the CPU; converting to the encoder's YUV
  on the GPU (half the readback) is the next speed step for real timelines.
