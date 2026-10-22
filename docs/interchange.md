# Interchange: one-click import and timeline export

Module `wabcut_interchange` (pure C++23, no Qt) imports timelines from Premiere Pro, DaVinci Resolve,
Final Cut Pro and anything that writes OpenTimelineIO or CMX3600 EDLs, and (X.1) exports them back in the same
formats ("Export" below); since P.2 it also exports AAF and OMF for audio post and reads and writes caption files
("Audio post" and "Caption files" below). Owner: Interchange (follow-ups since C.4: Core engine).

```cpp
#include <wabcut/interchange/Import.h>

interchange::ImportOptions options;
options.mediaSearchRoots = {"D:/Footage"};
auto result = interchange::ImportRouter::importFile("Main Edit.xml", options);
if (!result) { /* result.error(): NotFound / IoError / Unsupported / ParseError */ }
// result->project: a valid ProjectState; result->report: carried / approximated / dropped;
// result->unresolvedMedia: files that were not found.
```

```
wabcut-cli import <file> [--search <dir>]... [--json] [--carried] [--edl-rate 24000/1001] [--experimental-prproj]
```

## How it works

```
file ──ImportRouter::detect──► parser (FCP7 / FCPXML / OTIO / EDL / prproj) ──► IR ──TimelineBuilder──► ProjectState
                                                    │                                   │
                                                    └──────────── ImportReport ◄────────┘     MediaRelinker
```

* **Detection** reads at most 64 KiB and goes by content first (XML root element, gzip magic, `OTIO_SCHEMA`,
  CMX3600 event lines), extension second. `.fcpxmld` and `.otiod` bundles are folders read through their
  `Info.fcpxml` / `content.otio`.
* **Parsers** turn each format into a format-neutral intermediate representation (`src/Ir.h`) in exact
  `Ticks`, and report what they cannot represent. Times are converted with a 128-bit `mulDiv` and explicit
  rounding (`src/TimeMath.h`); NTSC rates stay exact (`24000/1001`, `30000/1001`, ...).
* **TimelineBuilder** builds the project only through the model's own operations (`addBin`, `addMedia`,
  `addSequence`, `placeClips`, `setSpeed`, `frameHold`, `setTimeRemap`, `addTitle`, `setClipAttributes`,
  `linkClipGroups`, `addTransition`, `addMarker`, `markIn/markOut`, `setSequenceDuration`, `setTrackState`), so the
  result is valid by construction; `validate(Strict)` runs at the end anyway.
  * X.1 bulk build: a track's media clips go in with one `edit::placeClips` (the model's bulk placement: each
    clip checked like an overwrite edit, the track copied once) and every link group with one
    `edit::linkClipGroups`; titles and nested clips are placed one by one between the batches.
  * A retimed clip is placed at 100% and then given its speed with `SpeedChange::keepDuration`. This is
    Premiere's Speed/Duration with speed and duration unlinked: the clip keeps its position, length and media
    In, and plays `length × speed` of media. A freeze frame becomes a frame hold of its source In.
  * Variable speed becomes a **time remap** (C.6, `edit::setTimeRemap`): the clip is placed at 100% and given the
    source's curve, keys at curve time (the clip's own output time) holding media positions. Straight segments
    between the keys are exact; smooth (curved) keys are imported as straight segments and reported.
  * Transitions keep the alignment their sides describe (`center` within a frame, `startAtCut`, `endAtCut`,
    otherwise `custom`), stored on the transition.
  * Transitions are added last, on the clips that end and start at the transition's edit point. When only one
    side has a clip, the transition becomes a one-sided fade inside that clip (reported). When the media lacks
    the handles, it is shortened to the media there is (`HandleFit::UseAvailable`, reported). When no clip
    meets the cut, it is reported as a cut.
  * Titles are made with `addTitle`: a title media item in the import bin, a clip and its style effect. Imported media and sequences go into a new bin named after the file, in bins below it that mirror the source's
  (X.1: FCP7 `<bin>`s, FCPXML events and keyword collections; at most 16 levels). With
  `ImportOptions::targetProject` they are added to an existing project.
* Each media item gets a `MediaLocator`: the absolute path (the file found, or the original path when it is
  offline), the path relative to the project file (`ImportOptions::projectFile`, by default a project saved
  next to the imported file) and `offline` when it was not found. The fingerprint stays unknown at import
  (computing it reads every file); the media probe fills it in.
* Clips that overlap on one track (possible in OTIO/EDL/hand-edited XML, or FCPXML lanes from different
  parents) are never lost or overwritten: they move to an extra track and the move is reported.
* Malformed input returns `ErrorCode::ParseError`; it never crashes. Inputs are capped (256 MB file,
  512 MB decompressed .prproj), recursion is bounded (nesting depth 32) and parsing is iterative. Every parser is
  fuzzed (`tests/fuzz`, C.10).
* **Text encoding.** Imported text is always UTF-8 (C.10).
  * EDLs carry no encoding, and Avid and older tools write Windows-1252. An EDL that is not valid UTF-8 is read as
    Windows-1252: valid UTF-8 sequences stay, the other bytes are converted.
  * In XML, pugixml honours a BOM or a Latin-1 declaration. Text and attribute values that are still not UTF-8 after
    parsing are converted the same way. This covers undeclared Windows-1252 and numeric references to lone surrogates.
  * Before C.10, such bytes went into names unchanged, and the imported project could not be saved. As a last guard,
    an import whose project would not serialise is refused with `ParseError`.

## The import report

Every element read is listed once as **Carried** (exactly as in the source), **Approximated** (in the
timeline but changed; the reason says how) or **Dropped** (not in the timeline; the reason says why), with
a category — `clip`, `gap`, `transition`, `speed`, `effect`, `marker`, `track`, `sequence-setting`,
`audio-mapping`, `media`, `graphic` (Lottie, below) — and a source location: file, element path (XML path such as
`/xmeml/sequence/media/video/track[1]/transitionitem[1]`, JSON pointer such as `/tracks/children/0/children/3`,
or `event 012` for EDLs) and line (0 for JSON). `reason` is user-facing English (unlike `Error::message`).

## Support matrix

C = carried exactly, A = approximated (reported), D = dropped (reported), — = the format has no such thing.

| Feature | FCP7 XML | FCPXML | OTIO | EDL | .prproj (experimental) |
|---|---|---|---|---|---|
| Sequences: name, frame rate, NTSC/drop-frame, frame size, audio rate | C | C | rate C, size A (1920×1080) | rate A (option), size A | C |
| Start timecode (sequence starts at 01:00:00:00) | C (X.1: `Sequence::startTimecode`; times count from it) | C (`tcStart`) | C (`global_start_time`) | C (the record hour) | C (`MZ.ZeroPoint`) |
| Sequence In/Out marks | C (snapped to frames) | — | — | — | C (sub-frame marks A, snapped) |
| Video / audio tracks, order | C | lanes → tracks C; overlapping lane clips A (extra tracks) | C | V1, A1..An C | C |
| Track enable / lock / target | C | — | enable C | — | lock C, video output C, target C |
| Clips: position, length, source In, name | C (exact `pproTicksIn` when present) | C (`start` − asset `start`) | C (`source_range` − `available_range` start) | C, but source In is absolute timecode: A | C |
| Disabled clips | C | C (`enabled="0"`) | C (`enabled`) | — | C (`IsMuted`) |
| Gaps | C (implicit) | C (`<gap>`) | C (explicit `Gap` → implicit) | C (`BL`) | C |
| Trailing gap / sequence duration | C (`<duration>`) | C (`duration`) | C (longest track) | — | — |
| Linked A/V | C (`<link>`) | C (A/V of one clip) | C, inferred (same media/range/In on V and A) | C (`B`, `AA/V`, split events of one number) | C (`Link`) |
| Transitions: cross dissolve, dip to black/white, audio crossfade (constant power/gain), at the cut or one-sided at a clip's head/tail | C (`transitionitem`; Cross Fade (0dB) = constant gain, (+3dB) = constant power) | C | C (`Transition`) | C (`D`) | C |
| Other transition types (wipes, irises, additive/film dissolve, dip to colour, exponential fade) | A (straight wipe / cross dissolve / dip to black / constant power) | A | A | A (`Wnnn` → straight wipe) | A |
| Transitions without the media they need / without a clip at the cut | A (shortened to the media there is / a fade inside the clip that is there / a cut) | A | A | A | A |
| Constant speed | C (Time Remap `speed`) | C (straight `timeMap`, exact rational) | C (`LinearTimeWarp` `time_scalar`) | C (`M2`; the one-decimal rate snaps to a whole percentage: `059.9` at 29.97 = 200%) | C (`PlaybackSpeed`) |
| Reverse | A (the same media range played backwards; the first frame may differ by one frame) | A | A | A (negative `M2`) | A (`PlayBackwards`) |
| Freeze frame | C (speed 0) | C (flat `timeMap`) | C (`FreezeFrame`) | C (`M2 000.0`) | — |
| Variable speed (time remapping) | C (Time Remap `graphdict`: a time remap with the graph's keys); smooth keys A (X.1: Bezier keys whose speed ramps through them, with monotone tangents through the neighbouring keys: FCP7 keeps no handles); a variable speed without a graph A (100%) | C (a multi-point `timeMap`: a time remap, exact rational keys); `smooth`/`smooth2` points A (X.1: Bezier keys, tangents as for FCP7) | A (no OTIO schema describes a non-linear warp: a `TimeEffect` plays at 100%) | — | A (`TimeRemapping`) |
| Opacity, scale, rotation, volume, pan (static) | C (Opacity, Basic Motion, Audio Levels/Pan) | C (`adjust-blend`, `adjust-transform`, `adjust-volume`; each clip of a picture-and-sound element takes its own) | — | — | D |
| Keyframed parameters (X.1) | C (curves in the clip's media frames; curved keys A: ease in and out) | C for scale, rotation, opacity, volume (`keyframeAnimation`) | — | — | D |
| Position and anchor (X.1) | C (Basic Motion `center` / `centerOffset`, fractions of the frame) | C (`position` / `anchor`, percent of the frame height, y up) | — | — | D |
| Effects (X.1) | Gaussian Blur, Crop, Drop Shadow C (into the clip's effect stack); Wabcut's own (`effectid` `wabcut.*`) C; others D (named) | D (named) | D (named) | — | D |
| Sequence markers (name, comment, duration, colour) | C | — | C (stack markers) | C (`* LOC:`) | C |
| Clip markers | C (X.1: markers of the clip's source media, in media frames, as Premiere keeps them: they move with every clip of it) | A (all FCPXML markers are on clips: sequence markers) | A | — | — |
| Nested sequences / compound clips | C (C.8): a nested clip of the nested sequence, which imports as its own sequence (`<in>` in its time); a retimed nested clip A (100%) | C (`ref-clip`: a nested clip of the compound's sequence, `start` in its time); longer than the compound A (ends with it: the rest showed nothing); retimed A (100%) | C (nested `Stack`: a nested clip of a sequence made from its tracks, imported after the timelines; its constant time effect C) | — | C (X.1: `Video/AudioSequenceSource`: a nested clip of the sequence, `InPoint` in its time) |
| Multicam | C (`<multiclip>`: a multicam source, one angle per `<angle>` - its clip's first video track and its audio tracks; `synctype` as the sync method; the clip plays the `activevideoangle` / `activeaudioangle`); further video tracks of an angle D | C (`mc-clip` of a `<multicam>`: one angle per `mc-angle` (its primary storyline; connected clips D), placed where FCP synced them (sync method manual); `mc-source` angles for picture and sound) | — | — | A (X.1: the clip plays its source sequence as a nested clip; its angle switches are not read. No ground-truth sample yet) |
| Bins (X.1) | C (`<bin>`: sequences, and media whose master clip `<clip>` is in a bin) | events C (the projects and the media of the event's own clips); keyword collections A (a media item's first keyword's collection, as a bin inside the event's) | — | — | D (the import bin) |
| Titles | C (Text and Outline Text generators: text, font, size, style, alignment, colour, outline width/colour); other parameters A (listed) | C (X.1: `title` with its `text-style`: text, font, face, size, colour, alignment); other attributes A (listed) | — | — | D |
| Other generators (slug, black video, colour mattes, shapes) | D | D | D (`GeneratorReference`) | `BL` → gap | D |
| Auditions / sync clips | — | A (active pick / separate clips) | — | — | — |
| Split edits (J/L cuts) | C (separate clips) | A (audio trimmed to the video edit) | C | C | C |
| Keys | — | — | — | D (`K`/`KO` foreground; background kept) | — |
| Stereo/multichannel audio mapping | Premiere exploded tracks C (one track); FCP7 stereo pairs A (one clip); single channel of a stream C (a mono channel map, A.9) | C (asset streams); `audio-channel-source` C (the clip's channel map: `srcCh` / `outCh` L, R, C, 5.1 names, `active="0"` mutes), several components A (one mono channel of their sources) | A (stream 1; OTIO has no channel field) | `AA`/`A`+`A2` A (one clip), streams A | stream 1 |
| Media references | C (`pathurl`, Windows `C%3a`, UNC, macOS, UTF-8) | C (`media-rep`, 1.8 `src`, relative URLs) | C (`ExternalReference`; `MissingReference` offline, found by name) | name only (`FROM CLIP NAME`), Avid `FROM CLIP:` path | C (`ActualMediaFilePath`) |
| Declared media duration / channels | C (kept when every clip fits) | C | C (`available_range`) | — | C (`OriginalDuration`) |

Timing semantics worth knowing, per format:

* **Speeds (all formats)** — the clip's timeline position and length and its media In come from the file as
  before; the speed says how much media it plays (`length × speed`). Speeds are exact rationals: FCPXML
  `timeMap` points are rational times; percentages, OTIO time scalars and `.prproj` speeds are matched to the
  nearest fraction with a denominator up to 100,000 (exact for decimal percentages; anything else is reported
  as rounded).
* **FCP7 XML** — `start`/`end` are sequence frames; `in`/`out` are frames at the clip's own `<rate>`. A clip
  next to a transition has `start` or `end` = -1, and its `in`/`out` *include the transition's handles*
  (Apple's reference, listing 3-17): the edit point comes from the transition — Premiere's `cutPointTicks`
  when present, else the alignment (`center` = middle, rounded down like OTIO; `start`/`start-black` = its
  start; `end`/`end-black` = its end) — and an incoming clip's media In moves past the handle. With a
  speed change `in`/`out` count retimed frames, so the media In comes from `pproTicksIn` (Premiere, exact),
  else the `speedkfin` keyframe value, else `in × speed`. With variable speed the Time Remap `graphdict`
  keyframes are the curve: `when` is a frame of the clip's own `in`/`out` space (the clip's curve time starts at
  `in`, past any transition handle) and `value` the media frame. Premiere writes each stereo track as two
  "exploded" mono tracks (`currentExplodedTrackIndex`/`totalExplodedTrackCount`); they become one track.
* **FCPXML** — `sequenceTime(child) = sequenceTime(parent) + (child.offset − parent.start)`, recursively;
  top-level spine offsets count from `tcStart`. A clip without `start` begins at its asset's first frame.
  Transitions overlap the cut between back-to-back spine items. Lanes: video lanes in order (below-primary
  lanes composite under V1's content), audio: primary storyline first, then lanes below, then audio of
  connected clips above.
* **OTIO** — rates are doubles: 23.976023976… / 29.97 / 59.94 snap to the exact NTSC rationals. Times at
  different rates in one file (48 kHz audio ranges) convert exactly. Transitions take no track time
  (`in_offset` before the cut, `out_offset` after).
* **EDL** — the frame rate is not in the file: `ImportOptions::edlFrameRate` / `--edl-rate`, default
  29.97 for drop-frame EDLs and 24 otherwise (OTIO's default). Drop-frame comes from `FCM:` or `;`
  separators; labels that do not exist in drop-frame (e.g. `00:01:00;00`) drop the event and say so.
  Record time is rebased so the starting timecode hour is 0. A dissolve's cut is at the incoming event's
  record In and runs N frames into it. Source timecodes are absolute: the clip's source In stays the
  absolute timecode unless `ImportOptions::mediaInspector` reports the file's start timecode
  (`MediaFacts::startTimecode`), in which case it is converted (reported either way).

## Export (X.1)

A sequence goes back out as a timeline file other editors read, with the nested sequences and multicam sources it
plays: for the grade in Resolve, the mix elsewhere, or a Premiere Pro editor down the line.

```cpp
#include <wabcut/interchange/Export.h>

auto result = interchange::exportSequenceToFile(project, sequenceId, interchange::Format::Fcp7Xml, "Main Edit.xml");
// result->report: carried / approximated / dropped, like an import's; result->sequences: what was written.
```

```
wabcut-cli export-timeline <project.wabcut> <out> [--format fcp7-xml|fcpxml|otio|edl] [--sequence <name|id>]
                           [--fcpxml-version 1.10|1.11] [--json] [--carried]
```

The command `interchange.export` (`{"format", "path", "fcpxmlVersion"}`) does the same for scripts, the assistant
and MCP clients. It names a file, so it is a file command: MCP serves it only with `--allow-files` /
`--files-folder`, confined to those folders (docs/mcp.md). A dry run reports and writes nothing. For the UI
engineer: File > Export > Timeline as FCP XML / FCPXML / OTIO / EDL is the command with a save dialog, then the report
in the same panel as the import report.

```
ProjectState ──describeForExport──► IR (src/Ir.h) ──writer (FCP7 / FCPXML / OTIO / EDL)──► file
                       │                                      │
                       └──────────── export report ◄──────────┘
```

* **Deterministic.** The same project gives the same bytes (ids are numbered in document order), so exports can be
  diffed and checked in. Golden files of each format (`tests/interchange/fixtures/export/basic.*`) are compared byte
  for byte; a changed export is written to `test-output/export/`, and `WABCUT_UPDATE_GOLDEN=1` replaces them.
* **Read back by Wabcut.** Every format's output is imported again in the tests (`tests/interchange/test_export.cpp`):
  Wabcut -> format -> Wabcut gives the same timeline for everything the report calls Carried, compared clip by clip
  (position, length, source In, speed, remap, properties and keyframes, effects, titles, links, transitions, markers,
  nested and multicam sources). The fixtures of the import suite are exported to each format and imported again too.
* **Media** are written as absolute file URLs (`file://localhost/C%3a/...` for FCP7 XML as Premiere writes it,
  `file:///C:/...` for FCPXML and OTIO); media with only a project-relative path are resolved against
  `ExportOptions::projectFile`.

### Export support matrix

C = carried exactly (comes back the same), A = approximated (written, but changed: the report says how), D = dropped
(reported), — = the format has no such thing.

| Feature | FCP7 XML | FCPXML 1.10/1.11 | OTIO | EDL |
|---|---|---|---|---|
| Sequence settings (rate, NTSC/drop-frame, size, audio rate), duration | C | C | C | rate and size A (an EDL names neither: `--edl-rate` on import) |
| Start timecode (`Sequence::startTimecode`, schema 14) | C (`<timecode>`) | C (`tcStart`; spine offsets count from it) | C (`global_start_time`) | C on a whole hour (record timecode; between hours A: read back at the hour before) |
| In/Out marks | C | — (A, reported) | C | D |
| Tracks, order | C | V1 = primary storyline, V2.. lanes 1.., A2.. lanes -1..; A1 rides with V1 or fills V1's gaps; an A1 clip under a different V1 clip A (its own track) | C | V1, A1..A4 (V2.. and A5.. D) |
| Track name, output, lock, target, mute, role | C (`MZ.TrackName`, `MZ.TrackTargeted`); mute A (output off); role — | — (A, reported); roles as `audioRole` | C | A |
| Clips: position, length, source In, name, disabled | C (`pproTicksIn` exact; `start`/`end` -1 at transitions with handles in `in`/`out`) | C (exact rational times; a pair's sound has the picture's name and state) | C | whole frames; names come back as the media's, disabled clips as active (A) |
| Links | C (`<link>`) | V1/A1 pairs C; other tracks A (FCPXML links a clip's own sound only) | C | V1+A1 (and A2) events of one clip (`B`, `AA/V`) |
| Stereo / multichannel audio | C (Premiere's exploded mono tracks) | C (asset audio, `audio-channel-source`) | C | other streams and channel maps A |
| Transitions | C (Cross Dissolve, Dip to Color, Wipe, Cross Fade 0/+3 dB; `cutPointTicks`) | V1's C (A1's crossfade with it); other tracks D | C | dissolves C; dips, wipe patterns, constant gain A; a centred dissolve's cut moves to its start (A) |
| Constant speed, reverse, freeze | C (Time Remap) | C (`timeMap`, exact ratio) | C | `M2` (exact when the fps reads back the same) |
| Time remap | C (`graphdict`, whole frames; eased keys A: written as smooth `FCPCurve` keys, read back as Bezier keys with recomputed tangents) | C (`timeMap` points; eased keys A: `smooth2` points, read back the same way) | C (other tools see the average speed) | A (average speed) |
| Opacity, scale, rotation, position, anchor | C (Opacity, Basic Motion; position and anchor as fractions of the frame) | C (`adjust-blend`, `adjust-transform`: percent of height) | C | D |
| Volume, pan | C (Audio Levels, Audio Pan) | volume C, pan A | C | D |
| Keyframes of those | C (`<keyframe>` in media frames; eased keys A) | scale, rotation, opacity, volume C; position A | C | D |
| Effects | Gaussian Blur, Crop, Drop Shadow C (FCP filters); every other effect C for Wabcut, skipped by other editors (a filter named by its Wabcut id); masks and curve data A | D | C (masks and data A; AI object masks D) | D |
| Titles | C (Text / Outline Text generator: text, font, size, style, alignment, fill, outline) | C (`title` with `text-style`) | C | D |
| Motion graphics (C.13), other generators | D | D | D | D |
| Bins | not written (every item sits in the file; the import side reads bins) | not written | not written | — |
| Sequence markers (name, comment, duration, colour) | C (`pproColor`) | C for FCP's colours (blue, red/green to-dos, orange chapters); others A | C | `* LOC:` position, name and colour (comment and duration A) |
| Clip (source) markers | C (`<marker>` in each clipitem, media frames) | — | C | — |
| Marker highlights (C.15: score, confidence, reasons, peak, source) | A (appended to the comment: `[highlight 0.87 high: kill streak; mic peak]`) | A (in the note, likewise) | C | D (locators carry no comments) |
| Nested sequences | C (defined at the first clip, then by id) | C (`ref-clip` of a `<media>` compound) | C (a `Stack` per clip; the first carries the definition) | A (an event named after the sequence) |
| Multicam | C (`<multiclip>` with each clip's active angles) | C (`mc-clip` of a `<multicam>`; one audio track per angle) | C | A (an event named after the source and angle) |

## Audio post: AAF and OMF export (P.2)

For the mix in Pro Tools, Nuendo, Resolve / Fairlight or Media Composer, a sequence goes out as an AAF or an OMF 2.0
file with its audio rendered to new files, the way Premiere Pro's File > Export > AAF and File > Export > OMF do it.

```cpp
#include <wabcut/interchange/AudioPostExport.h>

interchange::AafExportOptions options;               // Premiere's AAF Export Settings
options.audio.files = interchange::AudioPostFiles::Separate;
exporter::ClipAudioRenderer audio(projectFile);      // the host's audio engine (wabcut/export/AudioPostHost.h)
interchange::AudioPostHost host{.audio = &audio};
auto result = interchange::exportAafToFile(project, sequenceId, "Mix.aaf", options, host);
// result->report: carried / approximated / dropped; result->audioFiles: the files written next to it.
```

```
wabcut-cli export-timeline <project.wabcut> <out.aaf|out.omf> [--format aaf|omf] [--sequence <name|id>]
    [--sample-rate 48000|96000] [--bits 16|24] [--separate-audio] [--audio-format wav|aiff] [--copy-complete]
    [--handles <frames>] [--no-breakout] [--render-effects] [--clean-copies] [--mixdown-video] [--omf-title <t>]
    [--no-pan] [--json] [--carried]
```

In the app: **File > Export Timeline…**, format **AAF** or **OMF**, with the settings below. The command is
`interchange.export` with `"format": "aaf"` or `"omf"` and an `"audioPost"` object (docs/commands.md); a dry run plans
and reports without rendering. The model module cannot decode media, so the audio comes from the host
(`AudioPostRenderer`): the app and wabcut-cli render each clip through the playback path every export uses
(`exporter::ClipAudioRenderer`, `exporter::interchangeServices`). Without one (a bare engine, an MCP server without the
export module), an AAF links the original media instead and says so; an OMF is refused.

### Settings (Premiere's names)

| Setting | AAF | OMF | Wabcut default |
|---|---|---|---|
| Mixdown Video | the picture rendered to `<name> Reference.mov` (DNxHR LB 4:2:2, PCM 24-bit; Premiere writes an MXF DNxHD/DNxHR), one clip on V1. Off: video clips link to their original files | — (audio only, video clips reported dropped) | off |
| Breakout to Mono | new mono files; a stereo track becomes "<track> Left" / "<track> Right" (panned hard left / right), 5.1 "L R C LFE Ls Rs". Off: clips link to the original media with their channels | always (OMF tracks are mono) | on |
| Render audio clip effects / Include clip copies without effects | the clips' effect stacks rendered in; with copies, "<track> FX" and "<track> No FX" tracks | — | off |
| Sample Rate / Bits per Sample | 48000 or 96000 / 16 or 24 | same | 48000 / 24 |
| Files | Embed Audio (in the file) or Separate Audio (folder "<name> Audio" next to it) | Encapsulate or Separate Audio | Embed |
| Format | Broadcast WAV (bext: description, originator, time reference = the clip's sample at its timecode, UMID = the file's MobID) or AIFF (AIFF-C when embedded) | WAV or AIFF-C | WAV |
| Render | Trim Audio Files (each clip plus handles) or Copy Complete Audio Files (whole media; clips that are retimed or nested fall back to trimmed files, reported) | same | Trim, 24 handle frames |
| OMF Title / Include Pan | — | the composition's name / the tracks' pan as a MonoAudioPan effect | sequence name / on |

### What goes into the file

| Sequence | AAF | OMF |
|---|---|---|
| Start timecode | C (Timecode slot "TC1") | C (TCCP slot) |
| Audio tracks, names, order | C (one AAF track per channel with breakout) | C (one OMF track per channel) |
| Clips: position, length, name, handles | C (sample-exact at the export rate; handles in the files) | C |
| Clip volume, its keyframes | C (MonoAudioGain OperationGroup, constant or linear VaryingValue) | C (MonoAudioGain effect, CVAL / VVAL) |
| Clip pan, channel map, speed, reverse, time remap, nested / multicam mixes, Enhance Speech takes, bleeps | rendered into the files (A when that is a change of meaning: the receiver sees the result, not the setting) | same |
| Clip effects | rendered with "Render audio clip effects", else D | D |
| Track volume and pan | C (MonoAudioGain / MonoAudioPan around the track, keyframes as points) | constant: C; keyframes A (the value at the start) |
| Crossfades | C (Transition with MonoAudioDissolve; equal power = Power interpolation, equal gain = Linear) | A (SimpleMonoAudioDissolve has no curve: linear) |
| Fades in / out at one clip | C (a Transition against Filler) | C (the source clip's FadeIn / FadeOut, linear power or linear amplitude) |
| Markers | C (DescriptiveMarker: position, length, "name: comment") | D (OMF has none) |
| Video | linked clips, or the Mixdown Video reference | D |
| Disabled clips, offline media | D (reported; the rest is written) | same |
| Track effects, mute, solo, the master bus, the 5.1 panner | D / A (reported) | same |

### How it is written

* **AAF** (`AafExporter.cpp`): a Microsoft Compound File Binary (version 4, 4096-byte sectors; `Cfb.h`) holding the
  AAF object model (`AafFile.h`): Header (Edit Protocol), the full baseline MetaDictionary (class, property and type
  definitions generated from pyaaf2's MIT-licensed tables by `tools/gen_aaf_model.py`), Dictionary, ContentStorage with
  the CompositionMob (UsageCode TopLevel), a MasterMob and a file SourceMob per audio file (WAVEDescriptor /
  AIFCDescriptor with the file's header as Summary; EssenceData with the whole file when embedded, a NetworkLocator
  when separate; PCMDescriptor for linked originals), video SourceMobs (CDCIDescriptor + NetworkLocator). Strong
  references, sets and vectors are child storages named as the AAF SDK names them; MobIDs are deterministic.
* **OMF** (`OmfExporter.cpp`): OMF Interchange 2.0 in a Bento container (`Bento.h`: value data, a TOC in 1 KiB blocks,
  reference lists for object references, the label at the end), big-endian ('MM'): HEAD (object 1) with Mobs,
  PrimaryMobs, MediaData, DefinitionObjects and IdentificationList; a CMOB; per file an MMOB and an SMOB with a WAVD /
  AIFD; WAVE / AIFC media data when encapsulated, NETL + DOSL locators when separate. A file is limited to 2 GB
  (OMF readers' limit; Bento offsets are 32-bit): larger encapsulated exports are refused (use Separate Audio).
* **Deterministic**: a fixed timestamp gives the same bytes (golden files `tests/interchange/fixtures/export/audiopost.aaf`
  and `.omf`, `WABCUT_UPDATE_GOLDEN=1`). The tests read the files back with the module's own readers (`AafReader.h`,
  `bento::read`), check every clip, handle, gain, fade and marker, and the samples in the audio files.
* **Checked with other readers** during development (not test dependencies): pyaaf2 1.7.1 opens and walks every object
  of the AAF golden (811 objects), and LibAAF's `aaftool` (`--aaf-clips --aaf-essences --show-automation`) lists the
  same tracks, clips, gain automation, crossfade (power curve), fade-in, marker and embedded / separate essence. It also
  logs two messages that pyaaf2's own files produce too: no class for the Avid `TypeDefinitionGenericCharacter` and
  `ContentStorage::EssenceData` missing when nothing is embedded (optional in the AAF object specification). The OMF
  golden was parsed by deck2omf's `bentodump` (a Bento TOC dumper written against real OMF files): all 419 objects with
  their type and property names.

## Caption files (P.2)

`CaptionFiles.h`: the caption and subtitle files Premiere Pro reads and writes, as sidecars and for broadcast.

| Format | Read | Write | What is kept |
|---|---|---|---|
| SubRip `.srt` | C | C (as I.4: text escaped so players show it literally) | text; `{\anN}` position A; italics, bold, font colours D (reported) |
| WebVTT `.vtt` | C | C (`line` / `align` for top / left / right placement) | text; `line:` and `align:` settings A (top or bottom, left / centre / right); voice, ruby, karaoke timestamps, regions D |
| Scenarist SCC `.scc` (CEA-608) | C | C | CC1; pop-on (default), roll-up 2/3/4 and paint-on; text to the 32-column, 4-row grid (rewrapped, reported); rows and columns map to top / bottom and alignment; italics, underline, colours, CC2-CC4, XDS, field 2 D |
| MacCaption `.mcc` (CEA-608 in CDPs) | C | C (MCC v1.0, 29.97 DF / 30 fps) | as SCC; CEA-708 services D |
| EBU N19 `.stl` | C | C (STL25.01 / STL30.01, Teletext, ISO 6937 with its accents) | 40 columns × 3 rows of double-height text, vertical position and justification; characters outside ISO 6937 become '?' (reported) |
| W3C TTML `.xml`, DFXP `.dfxp`, SMPTE-TT `.xml` | C | C | text and line breaks, `xml:lang`, title, regions and `tts:textAlign` / `displayAlign` → top or bottom, alignment; spans and styles A |
| iTunes Timed Text `.itt` | C | C (frame-based SMPTE timecode) | as TTML |

* **Import**: `captions.import` (or File > Import / dropping the file, like Premiere: the file becomes a new caption
  track of the current sequence, one undo step, and its report shows like a timeline import's). The format comes from
  the content, then the extension (an `.xml` is a caption file only when it is Timed Text). Timecode-based files (SCC,
  MCC, STL, iTT, SMPTE-time TTML) count from the sequence's start timecode, unless their first caption is earlier (then
  from 00:00:00:00, reported). Cues are sorted, cut where they overlap, cues with the same start joined, at most 60 s
  long (a caption a file never erases), text cleaned to valid UTF-8 within the model's limits.
* **Export**: `captions.export` (a format from the extension or `"format"`; SCC / MCC `"display"`; `"range"` sequence or
  In/Out; frame rate and drop-frame from the sequence) and the export settings' **Captions** row: **Sidecar file** in
  any of the formats above, next to the output, timecode-based ones from the sequence's start timecode at the export's
  first frame. Words the soundtrack bleeps are replaced as in burned-in captions.
* **CEA-608 details** (`Cea608.h`): byte pairs with odd parity; control codes sent twice; preamble address codes for
  rows 1-15 with indents and tab offsets; the standard, special and extended (Spanish / French / Portuguese / German /
  Danish) character sets, with the backspace the extended set needs; each caption is loaded before its start (pop-on)
  and the display is erased at its end. A caption whose text needs more frames than the gap before it starts late
  (reported, with the largest delay).
* **Embed In Output File** (export Captions row **Embed**, `CaptionExport::Embed`): the same 608 data as an SCC,
  attached to every video frame as A/53 caption data (`AV_FRAME_DATA_A53_CC`), which libx264, libx265 (its `a53cc`
  option), NVENC, Quick Sync H.264, VA-API and VideoToolbox write as caption SEI in the H.264 / HEVC stream (MP4, MOV,
  MKV). Encoders that do not (AMF, Media Foundation) are skipped by automatic selection and refused by name. Premiere
  embeds into QuickTime caption tracks and MXF; Wabcut's are in-stream (what YouTube, broadcast encoders and most players
  read). The test decodes the file and compares every byte pair with the SCC timing; FFmpeg's `ffprobe -show_frames`
  lists "ATSC A53 Part 4 Closed Captions" side data on every frame of such an export.
* **Checked with FFmpeg** during development: its `scc` and `mcc` demuxers with the `cc_dec` decoder read the golden
  files back as the same text (FFmpeg's SCC demuxer times each line from its first byte pair, so its start times are a
  few frames earlier than the caption's display; its MCC timing matches to the frame).
* Malformed and hostile files: every reader bounds sizes (64 MiB), counts (100,000 cues), nesting (32 levels of XML) and
  times; `fuzz_captions` runs every reader on each input, `fuzz_audio_post` the AAF and OMF readers.

## Lottie (motion graphics, C.13)

`importLottie` (`Lottie.h`, `src/LottieImporter.cpp`) reads Bodymovin JSON into a `Graphic` (not a timeline): shape,
solid, text and precomp layers, parenting, transforms with their easing as Bezier keys, shapes, fills, strokes,
gradients, trims, text range selectors and masks. It reports like the timeline importers, category `graphic` (and
`effect`, `media`, `marker`), with JSON pointers into the file. The support matrix and bounds are in
[graphics.md](graphics.md#lottie); `lottie.import` places the result as a clip or saves it as a template. Fuzzed by
`fuzz_lottie`.

## Media relinking (`MediaRelinker.h`)

For every media reference, in order: the **exact path** (when absolute on this platform); **project
relative** — a relative reference against the imported file's folder, then the trailing folders of the
original path under that folder and its parent (`/Volumes/X/Show/Media/a.mov` → `<dir>/Media/a.mov`, ...);
then a **file-name search** under the search roots (case-insensitive, indexed once per import, capped at
500,000 files and depth 24). Candidates whose size (when the format knows it) or duration (when an inspector
is given) differ are rejected; among the rest the one sharing the most trailing folders with the original
path wins, and an ambiguous pick is reported. Unfound media stay in the project as offline items with their
original path (`ImportResult::unresolvedMedia` lists them with every path tried). The relinker only lists
directories and reads sizes; it never opens, moves or modifies media.

The paths come from the imported file, so they are untrusted. A UNC path (`\\host\share\clip.mov`) makes Windows
contact that host when the path is checked. `ImportOptions::fileExists` (`RelinkOptions::fileExists`) replaces the
existence check. The fuzz harnesses use it so they never touch the disk or the network. A host that imports files
from strangers can use it to refuse network paths it has not seen before.

**Network paths: the policy (C.10, decided for I.8).** A network path (`\\server\share\...`, `file://server/share/...`,
`//server/share/...`) is treated exactly like a local one: it is checked with the same plain existence test, in the
same order, and a share that is missing, unreachable or refuses access leaves the media offline with its original
path, as a missing local file does - never an import error. Wabcut never mounts a share (no drive letter is mapped,
no `WNetAddConnection`), never shows or follows a credentials prompt (only plain file-system calls are made, which
fail rather than ask), and never searches a share it was not given as a search root. What the check cannot avoid on
Windows: it contacts the host named, with the signed-in user's single sign-on, as Explorer does when such a path is
opened; a host that wants that refused passes its own `fileExists`. The test is "interchange: relink: a network share
is looked up like a local folder" (`tests/interchange/test_relinker.cpp`).

## Detected but not imported

| File | Detection | Guidance given |
|---|---|---|
| DaVinci Resolve `.drp` / `.drt` / `.dra` | extension (zip archives) | Resolve: File > Export > Timeline… → FCPXML 1.10 or OpenTimelineIO |
| Final Cut Pro library `.fcpbundle` | extension | FCP: File > Export XML… (FCPXML 1.10+) |
| AAF | extension, compound-file magic | export FCP XML / FCPXML / OTIO, or an EDL from Avid |
| `.otioz` | extension | unzip and import `content.otio` |
| Premiere `.prproj` (default) | gzip magic + `<PremiereData>` | Premiere: File > Export > Final Cut Pro XML… |

## .prproj research spike — outcome

**Shipped: detection + guidance by default, and an experimental reader behind
`ImportOptions::experimentalPremiereProject` (`--experimental-prproj`).**

What we learned (structure documented at the top of `src/PremiereProject.cpp`): a `.prproj` is
gzip-compressed UTF-8 XML whose root's direct children are objects addressed by per-save `ObjectID`/`ObjectRef`
integers or persistent `ObjectUID`/`ObjectURef` GUIDs (nested scopes reuse ids, so only the root's children
are indexed). Sequences → track groups (typed by GUID) → clip tracks → track items (`Start`/`End`, `IsMuted`
= disabled) → sub-clip → clip (`InPoint`/`OutPoint`, `PlaybackSpeed`, `PlayBackwards`) → media source →
`Media` (`ActualMediaFilePath`, `Title`, `Infinite` for stills). Links, transitions (`Alignment` = ticks
before the cut) and markers (`DVAMarker` JSON with `0xAABBGGRR` colours) hang off the sequence. All times are
Premiere ticks, which are Wabcut ticks, so positions transfer losslessly. Recent versions omit default values
(`Start` 0, speed 1, `IsMuted` false). The element names were stable from Premiere 10.4 (2016) to 26.3 (2026)
in the files examined by the research, following the MIT-licensed [py-premiere](https://github.com/Pouf/py-premiere).

Verification: the reader was compared with Premiere's own ExtendScript dump (py-premiere's ground truth) for
24 sample projects saved by Premiere 26.3. In all 24, the sequences, frame rates and sizes, every track, every
clip's start, end, name and media In, and the markers matched (one project has no sequence; its media
imported). The samples share a small base project (a still, a tone, a nested sequence) and each adds one
feature: transitions, disabled clips, track mute/lock, retime, time remap, merged and multicam clips,
generators, subclips, razor/slip/roll results, extra tracks, sequence settings. Seven of them and their dumps
are test fixtures (`tests/interchange/fixtures/prproj`, MIT notice included); the tests also check locks,
disabled clips, links and marker details.

Why it stays experimental:
* the format is undocumented and changes between versions; only Premiere 26.3 output was checked against
  ground truth, older layouts (pre-CS6 projects in particular) are untested;
* effects, keyframes, clip properties, time-remap curves, audio track mute (mixer), clip channel routing,
  bins and proxies are not read;
* merged and multicam clips are internal sequences in the project; they import as sequences of their own, and since
  X.1 the clips that use them (and nested sequences) play them as nested clips; a multicam clip's angle switches are
  not read;
* a whole DOM is built: a very large project (hundreds of MB of XML) needs gigabytes of memory.

Recommendation: keep FCP7 XML export as the advertised Premiere path (it carries effects values, speed and
audio mapping and is what Adobe calls the robust interchange), and grow the .prproj reader behind the flag
with more ground-truth samples (py-premiere has ~80) before turning it on.

## Known gaps and next steps

* **Transitions, retiming and titles (C.4)** are carried with the model's operations: `addTransition`,
  `setSpeed` with `keepDuration`, `frameHold` and `addTitle` (see "How it works").
  * Variable speed is carried as a time remap (C.6) from FCP7 XML (`graphdict`) and FCPXML (multi-point
    `timeMap`). Still approximated: wipe patterns and other transition types (mapped to the closest built-in
    transition), reverse playback (the same media range played backwards), smooth speed-ramp keys (X.1: Bezier keys
    whose tangents are worked out from the neighbouring keys, since neither format keeps handles), `.prproj` time
    remapping and OTIO non-linear time effects (100%; OTIO has no schema for them),
    and title parameters beyond text, font, size, style, alignment, colour and outline.
  * Still dropped: generators other than titles.
  * X.1: keyframes of opacity, motion and volume import as curves (curve time = media time), position and anchor
    are carried, and FCP7's Gaussian Blur, Crop and Drop Shadow (and Wabcut's own effects written by an export) import
    as effect stack entries (`edit::addEffect`). Still dropped: colour, EQ and compressor filters of other editors
    (their parameters do not map onto Wabcut's), FCPXML filters, OTIO effects other than time warps.
* Premiere's work area and split-edit audio extents need model support before they can be carried (X.1 added the
  sequence start timecode, and clip markers are carried as markers of the source). (Per-channel audio routing has it since A.9: FCP7 and FCPXML channel
  selections import as clip channel maps.)
* Media probing during import: the CLI passes no `mediaInspector` yet, so EDL source timecodes stay absolute
  and same-name candidates are not compared by duration. Wire `media::probe` (and start timecode) in the app.
* Bins (X.1): FCP7 bins and FCPXML events and keyword collections become bins below the import bin. Not read:
  `.prproj` bins (the project view's tree is not mapped yet), FCPXML keyword collections beyond a media item's first,
  smart collections. The exporters write no bins.
* Not supported yet: AAF and OMF import (P.2 exports them), `.otioz`, OTIO `ImageSequenceReference`, OTIO nested `Track` items, multicam angle switches
  in Premiere `.prproj` files, generators other than titles.
* Nested and multicam sequences (C.8) are built before the sequences that use them; `ImportResult::sequences` keeps the
  parsers' order: a parent sequence comes before the sequences nested in it (FCP7 right after it, FCPXML after the project
  that uses them, OTIO after all timelines; each in the order they were met).
  A clip that would make a sequence contain itself is dropped and reported.
* `wabcut-cli` takes paths as narrow `argv`; on Windows non-ASCII paths need a wide `wmain` (CLI-wide change).
* Performance (X.1, RelWithDebInfo on the shared 32 GB build box, linked picture + sound pairs of
  `tests/model/ScaleProject.h`; `wabcut_interchange_tests "[perf]"`, the 50,000-clip case is hidden: run it with
  `"[perf]~[export]"`):

  | Clips | Format | Export before → after | Import before → after |
  |---|---|---|---|
  | 3,000 | FCP7 XML (4.1 MB) | 55 → 57 ms | 389 → 120 ms |
  | 3,000 | OTIO (12.9 MB) | 149 → 169 ms | 326 → 255 ms |
  | 50,000 | FCP7 XML (68 MB) | 797 → 689 ms | 212.3 s → 2.8 s |
  | 50,000 | OTIO (215 MB) | 3.2 → 2.8 s | 23.7 → 4.0 s |

  Before: one `overwriteEdit` per clip (each copies the track: O(n²) per track), one `linkClips` per pair, a
  `findClip` per clip for its attributes, and every report location counted the element's siblings (a track of
  25,000 clipitems: O(n²) on FCP7 XML alone). After: `placeClips` / `linkClipGroups`, one clip index, and sibling
  positions cached per large parent (`XmlLocator`). The rest is linear; the OTIO time is mostly JSON parsing of
  Wabcut's lossless metadata. Clips with a speed, a remap or effects still cost one edit each.

## Sources

* Apple, *Final Cut Pro XML Interchange Format* (FCP7 xmeml v1–v5), element reference and listings 3-6,
  3-14, 3-17 — developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML.
* Apple, *FCPXML Reference* and DTDs 1.8–1.11 (`media-rep`, `.fcpxmld` bundles, timing attributes, lanes).
* OpenTimelineIO schema docs and sources (`otio-serialized-schema.md`, `time-ranges.md`, `track.cpp`), and
  the OTIO FCP7/FCPX/CMX3600 adapters (alignment cut points, exploded stereo tracks, sample files).
* CMX 3600 EDL specification (event/M2 column layout, edit types); Avid/Resolve comment conventions
  (`* FROM CLIP NAME:`, `* LOC:`, `AUD`).
* AAF Object Specification and AAF Low-Level Container Specification (AMWA); Microsoft [MS-CFB] Compound File Binary
  File Format; pyaaf2 (MIT) for the baseline model tables and as a validator; LibAAF's `aaftool` as a validator.
* OMF Interchange Specification 2.1 (Avid, 1997: classes, data types, the audio effects dictionary); Bento
  Specification 1.0d5 (Apple, 1993: label and TOC format); deck2omf's notes and `bentodump` (GPL-2.0) on the Bento
  variant OMF files use (standard object IDs, reference lists).
* Adobe Premiere Pro help: Export AAF files, Export OMF files, Export settings reference (Captions).
* CEA-608 / EIA-608 (character sets, control codes, preamble address codes), SCC (Scenarist) and MCC (MacCaption VANC,
  SMPTE 334 CDPs) as FFmpeg's `ccaption_dec`, `sccenc`, `mccenc` read and write them; EBU Tech 3264 (STL); W3C TTML1,
  DFXP, SMPTE ST 2052-1 (SMPTE-TT); Apple iTT; ATSC A/53 Part 4 (caption SEI).
* py-premiere (MIT) and its ExtendScript ground-truth samples; Adobe Premiere Pro Scripting Guide (Time,
  TrackItem); OpenNeuro ds004505 (CC0) and other public `.prproj` files examined during the spike.
