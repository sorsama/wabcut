# Interchange: one-click import and timeline export

Module `wabcut_interchange` (pure C++23, no Qt) imports timelines from Premiere Pro, DaVinci Resolve,
Final Cut Pro and anything that writes OpenTimelineIO or CMX3600 EDLs, and (X.1) exports them back in the same
formats ("Export" below); since P.2 it also exports AAF and OMF for audio post and reads and writes caption files
("Audio post" and "Caption files" below), and since 0.6 it imports AAF and OMF 2.0 timelines back from Pro Tools, Media
Composer, Nuendo, Premiere Pro and DaVinci Resolve ("Audio post: AAF and OMF import"). Owner: Interchange (follow-ups since C.4: Core engine).

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
                  [--media-folder <dir>]   (AAF / OMF: where embedded audio is written; without it, it stays offline)
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
  * 0.7: a UTF-8 byte order mark (EF BB BF) in front of an `.otio` or `.edl`, as older Notepad and Python's `utf-8-sig`
    write it, is dropped once before the file is sniffed and parsed, so such a file is read like the same text without it.
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
| Sequence work area (0.7) | C (Premiere's `MZ.WorkInPoint` / `MZ.WorkOutPoint` attributes on `<sequence>`, in ticks; the whole sequence, which Premiere writes by default, is no work area) | A (`<metadata><md key="com.wabcut.workArea" value="in:out">`: Wabcut metadata, Final Cut ignores it) | A (`metadata.wabcut.sequence.workIn` / `workOut`: Wabcut reads it, other tools ignore it) | D (reported) | C (`MZ.WorkInPoint` / `MZ.WorkOutPoint`, import only) |
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
| Non-uniform scale (0.7) | C (Wabcut writes a `scalewidth` and `uniformscale` parameter beside the Basic Motion `scale`, which is the height; Premiere's and Final Cut Pro 7's files have one scale and stay uniform) | C (`scale="x y"`, keys as `x y` pairs; a negative scale, a flip, has no counterpart: uniform, approximated) | — | — | D |
| Position and anchor (X.1) | C (Basic Motion `center` / `centerOffset`, fractions of the frame) | C (`position` / `anchor`, percent of the frame height, y up) | — | — | D |
| Effects (X.1) | Gaussian Blur, Crop, Drop Shadow C (into the clip's effect stack); Wabcut's own (`effectid` `wabcut.*`) C; 0.6: Lumetri Color (Premiere's ids and values), colour correctors, RGB Curves, EQ and dynamics filters C / A (Premiere writes no audio filters into FCP7 XML; see "Colour grades and audio filters in FCP7 XML"); others D (named, with their parameter count) | 0.6: Wabcut's own (`com.wabcut.effect`) C; Gaussian Blur, Sharpen, Levels, Proc Amp, Brightness & Contrast, Color Balance (Color Wheels / Board), Vignette, Flipped, Channel EQ, Compressor, Limiter A (mapped by name; shared settings by name, the rest kept for export); others A (a disabled placeholder written back on export) | 0.6: `Effect` objects on clips and tracks: Wabcut's own (`metadata.wabcut.effect`) C; well-known names A (Wabcut's defaults); others A (placeholder, written back) | — | D |
| Sequence markers (name, comment, duration, colour) | C | — | C (stack markers) | C (`* LOC:`) | C |
| Clip markers | C (X.1: markers of the clip's source media, in media frames, as Premiere keeps them: they move with every clip of it) | A (all FCPXML markers are on clips: sequence markers) | A | — | — |
| Nested sequences / compound clips | C (C.8): a nested clip of the nested sequence, which imports as its own sequence (`<in>` in its time); a retimed nested clip A (100%) | C (`ref-clip`: a nested clip of the compound's sequence, `start` in its time); longer than the compound A (ends with it: the rest showed nothing); retimed A (100%) | C (nested `Stack`: a nested clip of a sequence made from its tracks, imported after the timelines; its constant time effect C) | — | C (X.1: `Video/AudioSequenceSource`: a nested clip of the sequence, `InPoint` in its time) |
| Multicam | C (`<multiclip>`: a multicam source, one angle per `<angle>` - its clip's first video track and its audio tracks; `synctype` as the sync method; the clip plays the `activevideoangle` / `activeaudioangle`); further video tracks of an angle D | C (`mc-clip` of a `<multicam>`: one angle per `mc-angle` (its primary storyline; connected clips D), placed where FCP synced them (sync method manual); `mc-source` angles for picture and sound) | — | — | A (X.1: the clip plays its source sequence as a nested clip; its angle switches are not read. No ground-truth sample yet) |
| Bins (X.1) | C (`<bin>`: sequences, and media whose master clip `<clip>` is in a bin) | events C (the projects and the media of the event's own clips); keyword collections A (a media item's first keyword's collection, as a bin inside the event's) | — | — | D (the import bin) |
| Titles | C (Text and Outline Text generators: text, font, size, style, alignment, colour, outline width/colour); other parameters A (listed) | C (X.1: `title` with its `text-style`: text, font, face, size, colour, alignment); other attributes A (listed) | — | — | D |
| Other generators (slug, black video, colour mattes, bars and tone, counting leader) | 0.6: Color, Slug / Black Video, Bars and Tone (HD or SD), Universal Counting Leader, Adjustment Layer C (synthetic items); Wabcut's own (`wabcut:` parameters) C; others D | 0.6: Wabcut's own (`com.wabcut.generator`) C; Final Cut's solids (Custom) A (a colour matte); a "Bars" generator A; others D | 0.6: Wabcut's own (`metadata.wabcut.generator`) C; `SolidColor` A (colour matte, or transparent video when alpha is 0), `SMPTEBars` A; others D | `BL` → gap | D |
| Auditions / sync clips | — | Auditions C (0.6: `<audition>` becomes an audition, its picks the takes, the first one active; a pick without the active pick's picture and sound D, a pick's connected clips D); sync clips A (separate clips) | C (0.6: an audition written by Wabcut, from its metadata) | — | — |
| Split edits (J/L cuts) | C (separate clips, each with its own `start` / `end` / `in` / `out`, linked) | C (0.7: one `asset-clip` with `audioStart` / `audioDuration`, the sound's own extent in the clip's time, for a picture and sound of one link group over one file, in step and not retimed; a retimed or out-of-step pair stays two clips, reported; a sound that would start before the media or the sequence is cut there, reported) | C (two clips with their own ranges and a link group) | C | C |
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
* **Pixel aspect ratio (FCP7 XML).** The sequence's ratio is written as `<pixelaspectratio>` from Apple's list - 4:3
  pixels at 1440 wide `HD-(1440x1080)`, at 960 wide `HD-(960x720)`, 3:2 at 1280 wide `HD-(1280x1080)`, D1 / DV
  `NTSC-601` and `PAL-601` (with `<anamorphic>` for the widescreen ratios) - and `square` for any ratio the list has no
  name for. FCPXML, OTIO and EDL carry no pixel aspect ratio, and no importer reads it.

### Colour grades and audio filters in FCP7 XML (0.6)

Checked against a real Premiere Pro export: Premiere writes Lumetri Color as a filter with the effect id `Lumetri`
(and an empty name), numeric parameter ids (7 Temperature, 8 Tint, 11 Exposure, 12 Contrast, 13 Highlights,
14 Shadows, 15 Whites, 16 Blacks, 20 Saturation, 26 Intensity, 28 Faded Film, 30 Vibrance, 31 Creative Saturation,
33 Tint Balance, 51-54 Vignette, unnamed section switches) and each value as Premiere's key list
(`-91445760000000000,-15.,0,0,0,0,0,0`: a time in ticks, then the value; a static value sits at -100 hours, keyframes
are further `;`-separated keys). Wabcut reads Lumetri by those ids first (two parameters are both called
"Saturation"), with the same units as its own Color Grade. Premiere also writes a base64 "Blob" of the whole Lumetri
state: Wabcut reads it only to name LUT files and to report curves, colour wheels or HSL secondary set there (FCP7 XML
has no parameters for them); the Look and Input LUT menus are only their place in Premiere's list. A bypassed Lumetri
(`pproBypass`) comes in switched off.

**Premiere does not write its audio effects into FCP7 XML.** A clip with Parametric Equalizer or Dynamics in Premiere
arrives with no audio filter at all, so Premiere's EQ and dynamics cannot travel to Wabcut (or back) through FCP7 XML;
use AAF for an audio hand-off or apply them again. The audio rows below are for FCP7's own filters, other tools that
write them, and Wabcut's own files.

Other filters are read by effect name or id and by parameter name or id (case and punctuation ignored), so the same
table works for files from Resolve, Final Cut Pro 7 and hand-written XML (`FilterMap.h`):

| Filter | Becomes | Notes |
|---|---|---|
| Lumetri Color | Color Grade: Basic Correction (temperature, tint, exposure, contrast, highlights, shadows, whites, blacks, saturation), Creative (intensity, faded film, vibrance, saturation, tint balance), Vignette (amount, midpoint, roundness, feather), the section switches | C, keyframes too. Shadow / Highlight Tint colours A (hue and amount, when a file names them). The Look / input LUT is only named in the report (FCP7 XML has no LUT path); Sharpen, and curves / wheels / HSL set in Premiere's state block, D (named) |
| Fast Color Corrector, Three-Way Color Corrector, FCP7 Color Corrector (3-way) | Color Grade: saturation, and the wheels (ASC CDL) | A: input / output levels as slope, offset and power; balance wheels as lift (shadows), gamma (midtones), gain (highlights); the values are in the report |
| RGB Curves | Color Grade curves (master, red, green, blue) | A: points sampled (at most 17 when there are more than 32) |
| ProcAmp, Levels, Color Balance, Dynamics, Parametric Equalizer, Single-band / Multiband Compressor, Simple Parametric EQ, Bass, Treble, Hard Limiter and every other effect Wabcut has under Premiere's name | the same Wabcut effect | C: by Premiere's parameter names (files Wabcut or other tools write; Premiere itself writes no audio filters) |
| FCP7 3 Band Equalizer, Premiere's legacy EQ | Parametric EQ (3-band) | C (extra mid bands D) |
| FCP7 Parametric Equalizer (one band) | Parametric Equalizer, band 1 | C |
| FCP7 Compressor/Limiter | Single-band Compressor | C (Preserve Volume A) |
| anything else | — | D: named with its parameter count |

Export goes the other way: Color Grade and Basic Correction as Lumetri Color in Premiere's own shape (effect id
`Lumetri`, the numeric parameter ids, key-list values, `pproBypass` for a switched-off grade; keyframes as timed keys),
Parametric EQ (3-band) as Premiere's Parametric Equalizer (low shelf, band 1, high shelf), and the effects Wabcut has
under Premiere's names as those, with every parameter (these audio filters are for Wabcut and FCP7-style readers:
Premiere does not take audio filters from FCP7 XML). Every Wabcut parameter and curve is also written as a `wabcut:`
parameter (`authoringApp="Wabcut"`) that other editors skip, so Wabcut reads its own file back exactly; the export
report says what other editors will not get (the grade's wheels, curves and HSL, a LUT). The import side is tested on a
trimmed real Premiere export (`tests/interchange/fixtures/premiere_lumetri_real.xml`); that Premiere applies Wabcut's
Lumetri (written without the state block), and the exact form of Premiere's keyframed values, are not yet checked in
Premiere itself.

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
| Effects | Gaussian Blur, Crop, Drop Shadow C (FCP filters); 0.6: Color Grade and Basic Correction as Lumetri Color, the EQ and dynamics effects as Premiere's (see below); every other effect C for Wabcut, skipped by other editors (a filter named by its Wabcut id); curve data C (0.6); masks A | 0.6: C (`filter-video` / `filter-audio`: Final Cut's own effect where one is equivalent, else `com.wabcut.effect/<id>`; Wabcut's keys, keyframes and data as `com.wabcut.effect` params; eased keys A; masks A; placeholders written back as they came) | C (0.6: `Effect` objects named for other tools, the instance in `metadata.wabcut.effect`; track effects too; AI object masks D) | D |
| Titles | C (Text / Outline Text generator: text, font, size, style, alignment, fill, outline) | C (`title` with `text-style`) | C | D |
| Generators (0.6: colour matte, black / transparent video, bars and tone, counting leader, adjustment layer) | C (Color, Slug, Bars and Tone generators; the others named by their Wabcut kind; settings as `wabcut:` parameters) | C (`<video ref>` of Final Cut's Custom solid or a `com.wabcut.generator` effect; a generator's sound only with its picture in the primary storyline, else D) | C (`GeneratorReference`: `SolidColor`, `SMPTEBars`, `CountingLeader`, `AdjustmentLayer`; settings in `metadata.wabcut.generator`) | D (named, with its length) |
| Track effects | D (reported) | D (reported) | C | D (reported) |
| Motion graphics (C.13) | D | D | D | D |
| Bins | not written (every item sits in the file; the import side reads bins) | not written | not written | — |
| Sequence markers (name, comment, duration, colour) | C (`pproColor`) | C for FCP's colours (blue, red/green to-dos, orange chapters); others A | C | `* LOC:` position, name and colour (comment and duration A) |
| Clip (source) markers | C (`<marker>` in each clipitem, media frames) | — | C | — |
| Marker highlights (C.15: score, confidence, reasons, peak, source) | A (appended to the comment: `[highlight 0.87 high: kill streak; mic peak]`) | A (in the note, likewise) | C | D (locators carry no comments) |
| Nested sequences | C (defined at the first clip, then by id) | C (`ref-clip` of a `<media>` compound) | C (a `Stack` per clip; the first carries the definition) | A (an event named after the sequence) |
| Multicam | C (`<multiclip>` with each clip's active angles) | C (`mc-clip` of a `<multicam>`; one audio track per angle) | C | A (an event named after the source and angle) |
| Caption tracks (0.6) | D (each track with cues reported by name, language and cue count); with **Captions > Sidecar files** C as caption files next to the timeline | same | same | same |
| Auditions (0.6) | the active take (the others reported dropped) | C (`<audition>`: the active take first, as Final Cut's active pick, then the others in take order; a take whose sound is not its picture's A) | C (the takes in Wabcut's metadata; other editors see the active take) | the active take (the others reported dropped) |

**Caption tracks with the hand-off (0.6).** None of these formats (nor AAF / OMF) carries a caption track, so the
export report lists each caption track of the exported sequence that has cues, e.g. *caption track 'English' (en, 214
cues) is not carried by timeline files* (Dropped, category Caption). File > Export Timeline… has a **Captions** row:
**Sidecar files** and a caption format (SRT by default; any format of "Caption files" below; both remembered) write
every caption track with cues next to the timeline file as `<timeline name> - <track name>[.<language>].<ext>`
(`Film - English.en.srt`, `Film - Thai.th.srt`; characters file systems refuse become `_`, a second track of the same
name gets ` (2)`; an existing file is replaced, as the timeline file is). Words the soundtrack bleeps are replaced as in
`captions.export`. Each written track's report entry becomes Carried and names its file; the summary ends with
"· 2 caption files next to it". With several sequences each timeline gets its own caption files. In commands:
`interchange.export` `"captionSidecars": "srt"` (any caption format name); the result lists the files in
`"captionFiles"`; a dry run lists them and writes nothing.

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
| Caption tracks (0.6) | D (reported per track); Captions > Sidecar files writes them next to the file (see the export matrix above) | same |

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
  (OMF readers' limit; Bento offsets are 32-bit): larger encapsulated exports are refused (use Separate Audio). 0.7: the
  size is estimated from the plan before anything is rendered (the sum of the files' samples plus a generous allowance for
  the structure), so the refusal comes at once, also from the dry run behind the export dialog's preview, not after the
  whole render and copy; `Bento::Builder::emit` still checks the real size as a backstop.
* **Deterministic**: a fixed timestamp gives the same bytes (golden files `tests/interchange/fixtures/export/audiopost.aaf`
  and `.omf`, `WABCUT_UPDATE_GOLDEN=1`). The tests read the files back with the module's own readers (`AafReader.h`,
  `bento::read`), check every clip, handle, gain, fade and marker, and the samples in the audio files.
* **Dynamics Processing and 7.1 (0.7):** Dynamics Processing (`wabcut.audio.dynamicsProcessing`) has no equivalent in FCP7 XML,
  FCPXML or OTIO: the file carries it (its parameters as "wabcut:" parameters, a Final Cut effect or an OTIO Effect named
  "Dynamics Processing", with the exact instance in the metadata), other editors skip it, and the export report names it. A 7.1 track
  is written to AAF and OMF as eight mono tracks named for their channels (L R C LFE Lrs Rrs Ls Rs) and read back as a 7.1 layout with a 7.1 master; the 7.1 master itself is not written (the report says so). Eight
  mono files or tracks from other editors are routed to the 7.1 channels by the speaker in their names, else in a 7.1 file's order.

* **Checked with other readers** during development (not test dependencies): pyaaf2 1.7.1 opens and walks every object
  of the AAF golden (811 objects), and LibAAF's `aaftool` (`--aaf-clips --aaf-essences --show-automation`) lists the
  same tracks, clips, gain automation, crossfade (power curve), fade-in, marker and embedded / separate essence. It also
  logs two messages that pyaaf2's own files produce too: no class for the Avid `TypeDefinitionGenericCharacter` and
  `ContentStorage::EssenceData` missing when nothing is embedded (optional in the AAF object specification). The OMF
  golden was parsed by deck2omf's `bentodump` (a Bento TOC dumper written against real OMF files): all 419 objects with
  their type and property names.

## Audio post: AAF and OMF import (0.6)

A mix or a cut that comes back from audio post opens as a sequence: **File > Import** (or a drop on the Library or the
Home screen) an `.aaf` or `.omf` file, as in Premiere Pro. It is one undo step ("Import <file>"); the import report
lists what was carried, approximated and dropped, each with its timecode, and offers Relink… when media is offline.

The import runs in the background, because an AAF or OMF can hold gigabytes of embedded audio. A toast shows
"Importing Mix.aaf · 40 %" with **Cancel**. Several files are imported one after another, and you can keep working
while they import. If you edit the project meanwhile, the file is imported again into the project as it is now, so your
edits are kept (the audio written the first time is reused). Cancel imports nothing and removes the audio files that
import wrote. Opening another project stops the import.

Where the embedded audio goes: a folder named "<file name> Media", in the first of these places that Wabcut can write
to:
1. the project's folder (a saved project);
2. the AAF / OMF file's folder;
3. Documents/Wabcut/Imported Media (for example an unsaved project, with the file on a read-only share or disc).

The audio never ends up offline only because the file's own folder is read-only.

```cpp
interchange::ImportOptions options;
options.projectFile = "D:/Projects/Film.wabcut";   // embedded audio goes to "D:/Projects/Mix Media/"
auto result = interchange::ImportRouter::importFile("Mix.aaf", options);
```

Detection: `.aaf` or the compound-file signature; `.omf` / `.omfi` or the Bento label at the end of the file (an OMF
without its extension is found too). Files are read on demand (`cfb::Reader::openFile`, `bento::readFile`), so an AAF or
OMF with gigabytes of embedded audio never has to fit in memory.

### What comes in

| Feature | AAF | OMF 2.0 |
|---|---|---|
| Sequences: every top-level composition (else every composition nothing plays), its name | C | C (the primary mobs) |
| Frame rate (picture or timecode track), start timecode, drop frame | C | C |
| Audio rate: the tracks' sample rate (48 kHz, 96 kHz, ...); media at another rate plays converted (reported) | C | C |
| Audio tracks: order (physical track numbers), names | C | C |
| Clips: position, length, source In, name (the master clip's) | C | C |
| Linked clips: the clips of one master clip at the same time and source position on different tracks (picture and sound, a stereo master on two mono tracks); two clips that only share a name are not linked; the halves of a split stereo clip ("Dialogue.L" / "Dialogue.R", also "_L" / "_R") are linked | C | C |
| Handles: media beyond a clip's edges | C (the files keep them; trimmed embedded files keep theirs) | C |
| Crossfades and fades in / out (Transitions; Filler on one side is a fade) | C, curve: linear = Constant Gain, power / log / Avid's power type = Constant Power | C (OMF dissolves have no curve: Constant Power; clip fade types 1 / 2 = Constant Gain / Constant Power) |
| Clip gain and volume keyframes (Audio Gain around a clip; Media Composer's constant gain and volume automation multiply) | C (curve in media time) | C |
| Clip pan | C | C |
| Track volume and pan (gain / pan around a track's whole sequence), constants or automation | C | C (constants) |
| Muted clips (Media Composer: Filler selected over the clip) | C (switched off) | — |
| Subclips, adjusted clips, nested compositions | flattened into the track (their gain carried) | flattened |
| Multichannel tracks (Audio Channel Combiner): one multichannel file | C (one clip, a 2-, 6- or n-channel track) | — |
| Multichannel tracks: one mono file per channel | A: stereo = two linked mono tracks panned left / right; 5.1 = six linked tracks routed to a 5.1 master's L R C LFE Ls Rs (by the files' names, Pro Tools' film order included); other counts = linked mono tracks | — |
| Six mono tracks named "<name> L / R / C / LFE / Ls / Rs" (Wabcut's and Premiere's breakout) | C (routed to a 5.1 master) | C |
| Video tracks: clips linked to their media (offline when not found) | C | D (OMF import brings the audio, card #68) |
| Markers (event track): position, length, comment ("name: comment" split), Avid's marker colour | C | — (OMF has none) |
| Embedded audio (WAV, AIFF-C, raw PCM) | C: written to "<file> Media" beside the project (or the file) and linked; the same file is reused on a second import | C (WAVE / AIFC media data) |
| Embedded video, other codecs | A: offline, reported | A |
| Other effects (EQ, plug-ins, video effects) | D, named with their timecode; their clip is kept | D |
| Speed changes (time warp effects) | A: the clip at 100 % | A |
| Selectors with alternatives (multigroups) | A: the selected one | A |
| Media not found | offline, its clips in place, reported (never dropped) | same |

### How it works

`AafImporter.cpp` and `OmfImporter.cpp` only translate their container into one format-neutral mob graph
(`AudioPostImport.h`); one engine resolves SourceClips through master mobs to the file they play (the channel of a
multichannel file comes from its source slot's physical track number), flattens lower-level compositions, turns
transitions into edit-point fades, gain and pan groups into clip or track parameters, and builds the importers' IR, which
the shared `TimelineBuilder` turns into the project. Definitions the importer does not know by identifier (Avid's own
operation and parameter definitions) are recognised by the names in the file's Dictionary, and objects of extension
classes read as the nearest baseline class their MetaDictionary derives them from. Both readers are bounded (object and
component counts, nesting, automation points, bytes read: streams or values that share sectors cannot make it read the
file over and over) and fuzzed (`fuzz_aaf_import`, `fuzz_omf_import`).

Tests: `tests/interchange/test_audiopost_import.cpp` (the golden AAF / OMF exports and Wabcut's own exports at 48 and
96 kHz, embedded and separate; hand-built files in the shapes Pro Tools, Media Composer and Resolve write: 96 kHz with
44.1 kHz media and a missing file, 5.1 three ways, subclips, volume automation, muted clips, fades, track pan, markers,
effects; detection; truncated, foreign and mutated files) and `tests/editor/test_audiopost_import.cpp` (one undo step,
embedded audio written and probed). During development the importer was also run over 65 AAF files exported by Pro
Tools, Media Composer, Premiere Pro and DaVinci Resolve (the test files of the LibAAF project; not in this repository):
all import, and their clips, fades, levels, pans, markers and colours match what LibAAF's `aaftool` lists. Run them
yourself with `WABCUT_AAF_CORPUS=<folder> wabcut_interchange_tests "[.corpus]"`.

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
* **Every track at once (0.6)**: the Captions page's **All tracks…** (pick a format, then a folder) writes each caption
  track with cues as `<sequence> - <track name>[.<language>].<ext>` and says "Saved 3 caption files"; Export Timeline's
  Captions row and `interchange.export "captionSidecars"` do the same next to a timeline file. The Captions page's SRT /
  VTT buttons go through `captions.export` too (0.6), so bleeped words stay replaced there as well.
* **CEA-608 details** (`Cea608.h`): byte pairs with odd parity; control codes sent twice; preamble address codes for
  rows 1-15 with indents and tab offsets; the standard, special and extended (Spanish / French / Portuguese / German /
  Danish) character sets, with the backspace the extended set needs; each caption is loaded before its start (pop-on)
  and the display is erased at its end. A caption whose text needs more frames than the gap before it starts late
  (reported, with the largest delay).
* **Embed In Output File** (export Captions row **Embed**, `CaptionExport::Embed`): the same 608 data as an SCC,
  attached to every video frame as A/53 caption data (`AV_FRAME_DATA_A53_CC`), which libx264, libx265 (its `a53cc`
  option), NVENC, Quick Sync H.264, VA-API and VideoToolbox write as caption SEI in the H.264 / HEVC stream (MP4, MOV,
  MKV). Encoders that do not (AMF, Media Foundation) are skipped by automatic selection and refused by name. The test
  decodes the file and compares every byte pair with the SCC timing; FFmpeg's `ffprobe -show_frames` lists "ATSC A53
  Part 4 Closed Captions" side data on every frame of such an export. 0.6 (#73): the same 608 data on CC1-CC4, CEA-708
  services next to it, and the caption tracks Premiere writes - an MXF SMPTE 436M track and a QuickTime c608 track
  (docs/export.md, "Broadcast delivery: MXF and embedded captions").
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

## Generators and effects in FCPXML and OTIO (0.6)

**Generators** (Premiere's File > New items, `Synthetic.h`): a colour matte, black video, transparent video, bars and
tone, the universal counting leader and adjustment layers travel as generators, never as files, so nothing is offline
after an import. Each format writes the closest generator it has, and Wabcut's kind and settings (colour, bars pattern,
tone frequency and level, leader colours and cue blips) privately, so a Wabcut round trip is exact:

| Kind | FCP7 XML | FCPXML | OTIO |
|---|---|---|---|
| Color Matte | `Color` generator, `fillcolor` | Final Cut's Custom solid, `Color` | `SolidColor`, `color` [r, g, b, 1] |
| Black Video | `Slug` | Custom solid, black | `SolidColor`, black |
| Transparent Video | named by its Wabcut kind | `com.wabcut.generator` effect | `SolidColor`, alpha 0 |
| Bars and Tone | `Bars and Tone` (picture and sound) | `com.wabcut.generator` effect; its sound rides with its picture in the primary storyline | `SMPTEBars`, pattern, tone frequency and level |
| Universal Counting Leader | named by its Wabcut kind | `com.wabcut.generator` effect | `CountingLeader` |
| Adjustment Layer | named by its Wabcut kind (its effects as filters) | `com.wabcut.generator` effect (its effects as filters) | `AdjustmentLayer` |

On import, other editors' equivalents become synthetic items too: Premiere's Black Video, FCP7's Slug and Color,
Premiere's (HD) Bars and Tone and Universal Counting Leader; Final Cut's Custom solid (a colour matte, reported); OTIO
`SolidColor` (colour as an array, an object or `#rrggbb`) and `SMPTEBars`. An EDL has no generators: each is reported
as dropped with its name and length, and its time stays black (or silent).

**Effects** (`InterchangeEffects.h`): FCPXML `filter-video` / `filter-audio` and OTIO `Effect` objects (on clips,
nested stacks and, in OTIO, tracks) are read into the effect stack:
* Wabcut's own (written by a Wabcut export) come back exactly: id, on/off, every parameter with its keyframes, curve
  data. FCPXML carries them as params keyed `com.wabcut.effect...`, OTIO in `metadata.wabcut.effect`.
* Well-known effects of other editors are mapped by name onto Wabcut's (Gaussian Blur, Sharpen, Levels, Proc Amp,
  Brightness & Contrast, Color Balance for Color Corrector 3-way / Color Wheels / Color Board, Vignette, Flipped,
  Channel EQ, Compressor, Limiter, and any Wabcut effect's own display name). Settings both sides name the same way
  (Amount, Brightness, Contrast, Hue, Saturation, Threshold, Ratio, Attack, Release, ...) are applied; the others are
  kept with the effect so an export writes them back. Reported as approximated.
* Effects Wabcut does not know are kept as a disabled placeholder (`fcpxml:<name>`, `otio:<name>`) with the source's
  name, id, on/off state and settings (OTIO: its metadata). Wabcut does not render it; an export to the same format
  writes it back as it came. Reported as approximated ("kept as a placeholder").

On export, a Wabcut effect with a Final Cut equivalent is written under Final Cut's name and id with the shared
settings; every other effect as a `com.wabcut.effect/<id>` filter other editors skip. The Final Cut effect ids
(`FFGaussianBlur`, ...) follow Final Cut's naming but have not been checked against a Final Cut install; on import
the name is enough. Masks on effects, eased keyframes (FCPXML has ease in/out only) and track effects in FCP7 XML,
FCPXML and EDL are reported.

## Auditions (0.6)

An audition (a take stack, [getting started](getting-started.md#auditions-several-takes-at-one-cut-06)) is a clip that
plays its active take, so every format gets at least that take, exactly. What happens to the other takes:

- **FCPXML** writes Final Cut's `<audition>` at the clip's place: the active take first (Final Cut's active pick), then
  the other takes in take order, each a clip element with its own source In, length, name, speed and filters, and - in
  the primary storyline, where a clip keeps its sound - its sound with it. Connected clips sit on the active pick.
  Import turns an `<audition>` into an audition whose takes are its picks, the first one active; a pick that does not
  have the active pick's picture and sound is reported and left out, and so are the connected clips of the other picks
  (Final Cut keeps them on the audition; Wabcut keeps the active pick's). Take order after a round trip: the active take
  first.
- **OTIO** writes the active take as the clip and the other takes in its Wabcut metadata (`metadata.wabcut.audition`:
  the active take's place and each take as an OTIO Clip or Stack with its own Wabcut data). Wabcut reads them back in
  take order with the same active take; other editors see the active take.
- **FCP7 XML** and **EDL** have no auditions: the active take is written and the export report lists the takes that are
  not ("audition 'Angry': its active take is written; the other 2 takes ('Calm', 'Wide') are not: Final Cut Pro 7 XML
  has no auditions").
- A take that is short of the audition's range is written over the part it has media for. An audition whose active
  take is shorter than the audition comes back with the active take's length.

## Known gaps and next steps

* **Transitions, retiming and titles (C.4)** are carried with the model's operations: `addTransition`,
  `setSpeed` with `keepDuration`, `frameHold` and `addTitle` (see "How it works").
  * Variable speed is carried as a time remap (C.6) from FCP7 XML (`graphdict`) and FCPXML (multi-point
    `timeMap`). Still approximated: wipe patterns and other transition types (mapped to the closest built-in
    transition), reverse playback (the same media range played backwards), smooth speed-ramp keys (X.1: Bezier keys
    whose tangents are worked out from the neighbouring keys, since neither format keeps handles), `.prproj` time
    remapping and OTIO non-linear time effects (100%; OTIO has no schema for them),
    and title parameters beyond text, font, size, style, alignment, colour and outline.
  * 0.6: generators are carried (card #69, "Generators and effects in FCPXML and OTIO" below). Still dropped: shapes,
    textures and other generators Wabcut has no synthetic item for.
  * X.1: keyframes of opacity, motion and volume import as curves (curve time = media time), position and anchor
    are carried, and FCP7's Gaussian Blur, Crop and Drop Shadow (and Wabcut's own effects written by an export) import
    as effect stack entries (`edit::addEffect`). 0.6 (card #66): colour grades (Premiere's Lumetri by its real ids and
    key-list values), EQ and dynamics filters of other tools are carried or approximated (Premiere itself writes no
    audio filters into FCP7 XML) (see "Colour grades and audio filters in FCP7 XML"); still dropped: other
    editors' filters Wabcut has no effect for (named in the report). 0.6: FCPXML filters and OTIO effects are read and written (card #67).
* 0.7 (card 046): Premiere's work area (`Sequence::workArea`, set by Alt+[ / Alt+], shown in Sequence Settings) travels in
  FCP7 XML (Premiere's own attributes), FCPXML and OTIO (Wabcut metadata, other editors ignore it, reported as approximated)
  and is read from `.prproj`; split-edit audio extents are two linked clips in the model (each with its own start, length and
  source In) and FCPXML now carries them with `audioStart` / `audioDuration` (the attribute meaning follows Apple's DTD:
  "define J/L cuts (i.e., split edits) on composite A/V clips", times in the clip's own time like `start`; not verified
  against Final Cut Pro itself). X.1 added the sequence start timecode, and clip markers are carried as markers of the
  source. (Per-channel audio routing has it since A.9: FCP7 and FCPXML channel
  selections import as clip channel maps.)
* Media probing during import: the CLI passes no `mediaInspector` yet, so EDL source timecodes stay absolute
  and same-name candidates are not compared by duration. Wire `media::probe` (and start timecode) in the app.
* Bins (X.1): FCP7 bins and FCPXML events and keyword collections become bins below the import bin. Not read:
  `.prproj` bins (the project view's tree is not mapped yet), FCPXML keyword collections beyond a media item's first,
  smart collections. The exporters write no bins.
* AAF / OMF import (0.6): imported files always become a new sequence (Premiere's behaviour); adding their tracks to
  the open sequence is left for later. Not read: embedded video essence, OMF 1.0 files (refused with guidance), Avid
  plug-in parameters, multigroup angles other than the selected one.
* Not supported yet: `.otioz`, OTIO `ImageSequenceReference`, OTIO nested `Track` items, multicam angle switches
  in Premiere `.prproj` files, generators in `.prproj` files.
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
