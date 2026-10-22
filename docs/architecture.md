# Wabcut architecture & module contracts

Contract revision **2** (2026-10-11): adopts the independent architecture review (Codex). See the Decision log at the end.

This is the contract between modules. Modules are built in parallel by different owners, so **public
interfaces below are binding**. If a contract has to change, ask the coordinator first: it may affect
other owners. Internals are each owner's call. This document supersedes the original plan wherever the two differ
(C++23, not C++20; undo by snapshot, not inverse ops; implicit gaps internally, OTIO's explicit gaps only at import/export).

## Principles
1. **Integer time, checked.** Every timeline position/duration is `wabcut::Ticks` (254,016,000,000 per second,
   `src/model/include/wabcut/model/Time.h`). No floating-point time in the model or the engine. Arithmetic that can
   overflow (scaling by rates, speed maps, imported values) goes through checked helpers with explicit rounding
   (floor/ceil/nearest) and a wide (128-bit) intermediate. Never write a naive `ticks * rate / kTicksPerSecond`.
2. **GPU-first frames.** Frames move as `media::FrameHandle`: immutable, with timing/color metadata plus a **tagged payload**
   (CPU planes today; native D3D11 / CVPixelBuffer / DRM-PRIME later). Render imports frames through a `FrameImporter`.
   The MVP may ship only the CPU payload + upload path, but **no code outside `media` and `render` may assume CPU memory**.
3. **Immutable snapshots + undo by snapshot.** The project state is immutable and shared at defined granularity.
   An edit is a pure function that maps an old state to a new one plus a `ChangeSet`. Undo/redo swaps snapshots. Engine threads
   read snapshots acquired **once per job**; they never touch the audio callback.
4. **Probe, don't assume.** Hardware decode/encode backends and the RHI backend are chosen at runtime by trying them in order
   and falling back. The UI always shows the actual backends.
5. **Preview == export.** The same compositor algorithms and the same `ColorPipeline` render the monitor and export.
6. **Pure-C++ core.** `model` and `keymap` have no Qt dependency, so they are unit-testable, CI-able without Qt and reusable
   from `wabcut-cli`.
7. **Errors.** Use `std::expected<T, wabcut::Error>` across module boundaries. `Error.code` is the stable machine-readable part;
   `message` is diagnostic English, never shown to users verbatim or asserted in tests.
8. **Real-time safety.** On the audio callback: no locks, no allocation, no logging, no `atomic<shared_ptr>`, and no final
   release of large objects. On the render thread: no decode, never wait on decoder or cache locks.
9. **QML boundary.** Raw ticks (beyond JS's 2^53 after ~9.85 h) and uint64 IDs never cross into QML as numbers. Pass opaque IDs,
   formatted strings, and bounded view-relative coordinates.

## Threads
| Thread | Owns | Rules |
|---|---|---|
| GUI (Qt main) | QML, `EditSession` mutations, keymap dispatch, transport gestures | Never blocks on decode or IO |
| Scene-graph render | `ProgramMonitor` (QQuickRhiItem) → compositor | Acquires the latest **ready** `PreparedComposite` from a bounded mailbox; never decodes or waits |
| Scheduler | Evaluates the snapshot at target times, issues decode requests, publishes `PreparedComposite` | Latest-seek-wins cancellation; owns preroll |
| Decoder pool | Capped pool of decoder sessions keyed by (source, stream, cursor) | Serial per context; global software-thread budget; bounded queues |
| Audio callback (miniaudio) | Reads prepared PCM rings → device | Real-time rules above; it is the master clock source |
| Job queue | Thumbnails, waveform peaks, proxies, export, autosave serialization | Cancellable, prioritized below preview; reports progress to the GUI |

## Module contracts

### `model` — owner: Core engine (pure C++)
Public headers in `src/model/include/wabcut/model/`; overview in `src/model/README.md`. Everything below is implemented.
- **`Time.h`**: `Ticks` (254,016,000,000 per second), `TimeRange` (half-open).
  - `Rate` is a positive ratio. It has `isValid()`, `isExact()`, `normalized()`, and `make(num, den)` (validated and reduced),
    and two rates compare by value. The fast unchecked conversions (`framesToTicks(f, rate)`, `samplesToTicks`, …) are for exact
    rates.
  - Checked arithmetic returns `std::optional` on overflow: `checkedAdd/Sub/Mul/Neg` (int64 and `Ticks`), and
    `mulDiv(value, mul, div, Rounding)`.
    - `mulDiv` uses a 128-bit intermediate: `unsigned __int128` on GCC/Clang, `_umul128`/`_udiv128` on MSVC x64, and a
      portable path at compile time and on other targets.
    - `Rounding` is Floor, Ceil, Nearest (ties away from zero) or TowardZero.
  - Stream time bases: `TimeBase{num, den}` (one unit lasts num/den seconds, like `AVStream::time_base`).
    - `rescale(units, TimeBase, Rounding) -> optional<Ticks>` and `rescale(Ticks, TimeBase, Rounding) -> optional<int64>` convert
      between ticks and stream units.
    - The checked `framesToTicks/ticksToFrames(…, Rounding)` also work for non-exact rates.
  - `TimeMap{timelineOrigin, sourceOrigin, speedNum, speedDen}` provides `map`, `inverse` (none for a freeze) and `mapDuration`.
    A negative speed plays in reverse and 0 freezes. Clips expose theirs as `Clip::timeMap()`.
- `Timecode.h`: `ticksToTimecode(Ticks, Rate, bool dropFrame)` / `parseTimecode(string_view, Rate, bool dropFrame)`.
  Drop-frame applies to 29.97/59.94.
- **IDs** (`Ids.h`): strong uint64 types `MediaId`, `SequenceId`, `TrackId`, `ClipId`, `LinkGroupId`, `BinId` and `TransitionId`,
  all from one project-wide `IdGenerator`.
  - `ProjectState::ids` is only a lower bound, so edits stay pure functions.
  - `EditSession` keeps the **high-water mark outside the undo history** and raises every edit's generator to it. An id is never
    reused after undo, after redo or on a new branch.
  - Save `EditSession::stateForSave()`, which carries the mark.
- `MediaInfo.h` / `Error.h`: shared plain data (owned by the lead).
- **Project** (`ProjectState.h`): `ProjectState{ids, bins, media, sequences}`.
  - Bins form a flat parent-id tree. `media` and `sequences` are sorted vectors of `shared_ptr<const …>`. The state is immutable once
    published.
  - Every edit and undo step keeps a snapshot, so project-level collections that can grow large are `SharedList<T>`
    (`SharedList.h`, C.10): one list shared by all snapshots until one changes it (`edit()` copies first). They read like a
    `const std::vector`. These are `reviewNotes`, `transcriptEdits` and `captionPresets`. The others are still plain vectors,
    copied by each snapshot: bins, assets, redactions and speakers. Redactions with tracked keyframes are the candidate
    for the next conversion; their writers are outside the model.
  - `ids.next` is at most 2^53 (`validate`): a file cannot set the generator where it would wrap.
  - Clip start, duration and source In are at most 2^60 ticks, about 52 days (`validate`, C.11). Adding two of them
    cannot wrap int64.
  - `validate(state, Validation::Structure|Strict)` lists every broken invariant. `Strict` adds source extents and transition
    handles.
  - `MediaItem{id, bin, name, MediaLocator locator, MediaInfo info, optional<GeneratorSpec> generator}`, plus a derived default
    `ClipContent` that is not serialized.
  - A **generated item** (`generator` set, `isGenerator()`) has no file: its locator has no path, fingerprint, proxy or offline
    state (`validate` enforces it). Relinking, offline checks and export's file resolution skip it; the renderer draws it.
    `GeneratorSpec{kind, text, fontFamily, fontStyle}`; unknown kinds are kept as they are.
- **Captions** (C.7; `Sequence::captionTracks`, `CaptionStyle.h`, `CaptionText.h`, ops in `CaptionOps.h`):
  - A `CaptionTrack{id, name, language, enabled, cues, style}` holds `CaptionCue{start, duration, text, speaker}`: sorted by
    start, positive durations, UTF-8 text of at most 4 KiB, no overlaps unless `style.allowOverlap`, at most 100,000 cues and
    64 tracks a sequence (bounds against untrusted files; `captionTrackProblem`).
  - **Style** (`CaptionStyle`, saved on the track): font family / weight / size (a fraction of the frame height), fill,
    outline (width as a fraction of the font size, colour), shadow (opacity, colour, distance, angle, softness), background
    box (colour, opacity, padding, radius), placement (bottom / top of the safe area, or custom at `positionX/Y`, the only
    keyframable fields, keyed in sequence time), safe area, alignment, max width, line spacing, karaoke highlight, an
    `animation` enum reserved for kinetic captions (none / pop / fade / word by word), `showSpeakers` and `allowOverlap`.
    The default is the look captions had before (white 48/1080 text, weight 600, dark box at 0.78, bottom, 80 % wide), so
    older projects keep it. Presets: five built in (`builtinCaptionPresets`: Clean, Bold Box, Podcast, Shorts,
    Thai-friendly) and the project's own (`ProjectState::captionPresets`, names unique and never built-in).
  - **Ops**: `addCaptionTrack`, `addCaptionCue`, `updateCaptionCue`, `removeCaptionCue`, `splitCaptionCue` (at a time; the
    text at a byte offset or at the line-break opportunity nearest the time's share), `mergeCaptionCues` (re-wrapped),
    `shiftCaptionCues`, `setCaptionStyle`, `setCaptionTrackInfo`, `setCaptionTrack` / `removeCaptionTrack`, and
    `saveCaptionPreset` / `removeCaptionPreset` / `captionPresetStyle`.
  - **Line breaking** goes through a seam: `LineBreaker::breakOpportunities(text, language)`. The model's
    `simpleLineBreaker()` breaks at spaces and hyphens only; hosts with ICU or Qt (`QTextBoundaryFinder::Line`) install theirs
    as `command::CaptionServices`, so Thai, Lao, Khmer and CJK break between words. `breakCaptionLines` picks the fewest
    lines that fit, then the most balanced, preferring breaks after punctuation; `captionDisplayLength` counts combining
    marks (Thai vowels above/below, tone marks) as no cell.
  - **Captions follow edits**, in the same ChangeSet (`captionsChanged`), so one undo restores clips and captions. A caption
    track behaves like a sync-locked track that is never locked: ripples (insert, extract and transcript deletes, ripple
    delete of clips or gaps, Q/W, ripple trim, insert moves, Insert Frame Hold Segment, speed changes with ripple) move the
    cues after the edit point, take the time a removal removes out of the cues it crosses (a cue left without time goes), and
    cut a cue an insert lands in into two with the same text. Retiming an audio clip scales the cues over it with its speech
    (Speed/Duration; time-remap holds, reverse segments, segment speeds and rippled key moves add or remove time inside it,
    up to the clip's end). Edits that change no timing (overwrite, lift, delete, move, trim, roll, slip, slide, razor) and
    video-only remaps leave captions where they are. `SequenceEditor::captionsInsert / captionsRemove / captionsStretch` are
    the primitives.
- **Speakers and transcript edits** (C.7, `Transcripts.h`): `ProjectState::speakers` (`Speaker{id, name, color}`) and, per
  media audio stream, `TranscriptEdits{labels (diarization label -> speaker), speakerSpans (source ranges -> speaker),
  corrections (WordCorrection{source range, original, text})}`. The cached transcript stays raw; wabcut_ai_edit applies the
  edits when it maps transcripts onto a sequence (`TranscriptEditing.h`): a correction finds its words by source time (their
  middle in its range), so it survives re-transcription (reported applied / conflict / restored). Ops: `addSpeaker`,
  `registerSpeakerLabels`, `renameSpeaker`, `mergeSpeakers` (labels, spans and caption cues follow), `assignSpeaker`,
  `correctTranscript`. Cues name their speaker; a style with `showSpeakers` draws the name in the speaker's colour.
- **Nested sequences and multicam** (C.8, `Nesting.h`, `Resolve.h`; renderer/audio/UI obligations in
  docs/internal/c8-nested-multicam-handoff.md):
  - A clip may play a sequence instead of media (`Clip::sequence`, `media` invalid then). Its sourceIn, speed, direction and
    remap map the timeline into the nested sequence's time exactly as for media (`Clip::sourceTimeAt`, integer ticks). A video
    clip (stream Video 0) shows the nested sequence's composite (V1 up, track effects, its master video stack), then its own
    Motion/Opacity and effects; an audio clip (Audio 0) plays its stereo mix (tracks with faders, pan and effects, the master
    stack and fader), then its own Volume/Panner and effects. Past the nested sequence's end it shows and plays nothing; trims
    cannot extend it there (`clipSourceExtent`), and Strict validation does not bound it (the nested sequence may shrink).
  - A sequence never contains itself, directly or through others: `validate()` reports a cycle (`nestingCycle`) and every op
    that places a sequence refuses one (`sequenceContains`). `removeSequence` refuses a sequence in use; `removeTrack` keeps
    an angle's track and the last track of a kind another sequence plays.
  - **Nest** (`nestClips`): the selection (with linked partners) moves into a new sequence in the same bin with the parent's
    settings; clips keep their tracks (V2 stays V2), relative times, ids, effects, links among themselves (a group wholly
    inside keeps its id, part of a group gets a new one, a lone member none) and the transitions between them; one linked
    nested clip per kind replaces them on the lowest selected track of that kind. Refused when an unselected clip lies in the
    span on such a track. **Unnest** (`unnestClip`, Resolve's Decompose) puts copies of what the nested clip shows back
    inline - nested track i on the clip's track + i (audio downwards), trimmed to the window, new ids and links - only where
    exact; else it refuses with the reason (retimed or reversed, effects or non-default intrinsics on the nested clip, master
    or track processing inside, track processing or flags on the destination, a missing track, a clip in the way, a
    transition across the window's edge, a multicam source).
  - **Multicam**: a multicam source is a sequence with `Sequence::multicam` (`MulticamInfo{sync, angles}`; an angle is a
    video track and/or audio tracks of it plus an `offset`, 0..24 h). `createMulticam` builds one from media: angle i's video
    on its own video track, each audio stream on an audio track, placed at offsets from the sync method - start timecodes
    (`MediaInfo::startTimecode`), Source Monitor In or Out marks, first clip markers, audio (`AudioSyncAnalyzer`, provided
    by the host; `OffsetTableAudioSync` for tests) or given offsets. A multicam clip is a clip of that sequence with
    `Clip::angles` (`AngleSet`, at most 64 angles): a video clip shows one angle's video track alone; an audio clip mixes its
    angles' audio tracks (no master stage). `switchMulticamAngle` cuts at a time (or switches the whole clip) - picture,
    sound or both; cutting one side keeps its right piece linked with the uncut side - so angle cuts are ordinary cuts that
    trim, ripple, roll and undo like any. `flattenMulticam` (Premiere's Flatten) replaces the clip with copies of its angles'
    clips where exact; `setMulticamSync` moves an angle (its sync method becomes manual).
  - `SourceClip::sequence` (with `videoAngle` / `audioAngles`) edits a sequence into another one like media
    (`defaultSourcePatches(sequence, source)`: picture to V1, mix to A1).
  - **Leaves** (`resolveLeaves`, `resolveSequenceLeaves`): the media clips a clip shows or plays at a time, with the time in
    each nested level, the nested clips on the way (outermost first: effects apply leaf -> track -> composite/mix -> nested
    master -> nested clip -> ...), transition participation and the source-anchored redactions that apply (by media and media
    time; excepting a nested clip excepts its contents).
  - Clips of a source kind a newer build wrote (`ClipContent::unknownSource`) load, save back unchanged and play nothing.
- **Version history, review notes, diff and merge** (C.9; the UI wiring is in docs/internal/c9-history-review-handoff.md):
  - `ProjectHistory` (`History.h`) keeps a bounded history of revisions in `<file>.wabcut.history/`: a revision is the
    project as content-addressed chunks (the project root with the media list, each sequence, each track; the project
    file's JSON for those parts), stored once each as `objects/<2 hex>/<SHA-256>` (zlib with a "WCH1" + length header,
    checked against its name on read), so revisions share unchanged sequences and tracks; a checkpoint does not even
    serialise snapshot nodes it stored last time. `index.json` lists the revisions (id, UTC time, kind save / auto / agent /
    manual / restore, label, root, sizes). Crash safety: new objects first (temporary file, flush, rename), then the index
    the same way; `open()` removes temporary leftovers and `prune()` deletes objects no revision reaches. Pruning keeps
    named revisions and the newest `keepRecent` others, then drops the oldest unnamed ones (never the newest) while the
    folder is over `maxBytes`. Folders are untrusted: bounded sizes and counts, keys checked, revisions load through
    ProjectIO's migrations and checks. Hosts decide when to checkpoint (save, every few minutes, before an agent applies).
  - `ReviewNote` (`ReviewNotes.h`, ops in `ReviewOps.h`): frame-accurate notes on a sequence (start + duration, an
    optional point or rectangle in frame fractions, author, text, UTC time, open/resolved, replies), in
    `ProjectState::reviewNotes` (sorted by id, bounded, validated; removing a sequence removes its notes;
    `ChangeSet::reviewNotesChanged`). `reviewNotesToMarkers` makes comment markers (red open, green resolved).
  - `diffProjects` (`ProjectDiff.h`): what changed between two revisions, matched by id - sequences, tracks, clips
    (added, removed, moved, trimmed, retimed, source, enabled, renamed, links, effects added / removed / changed,
    keyframes and constant parameters), transitions, marks, markers, captions, media, review notes - and a summary in lines
    with timecodes. Shared (pointer-equal) sequences and tracks are skipped.
  - `mergeProjects` (`Merge.h`): three-way merge of mine and theirs against a base. Their ids made since the base are
    renumbered above mine's first (`renumberIdsFrom`, every reference following). Then three-way per object: project
    items as wholes; a sequence's settings, markers and caption tracks; each track's settings, clips and transitions.
    Conflicts (same object changed differently, removed vs changed, clips over the same time = "overlap") have stable ids
    and resolve mine (default) or theirs; the result is repaired to a valid project (lonely link groups dissolve,
    transitions whose clips no longer meet go, media / sequences / speakers a merged clip needs are kept). `edit::copySequenceFrom`
    brings one sequence of another revision in with new ids ("restore as a new sequence").
- **Clipboard, Paste Attributes** (C.11, `Clipboard.h`; UI wiring in docs/internal/c11-clipboard-handoff.md):
  - **The document.** `copyClips` makes a `ClipboardDocument`: a small project of its own. It holds one sequence with
    the copied clips exactly as they were, from time 0, on tracks counted from the lowest copied track of each kind,
    plus:
    - the transitions between copied clips;
    - the sequence markers inside the copied range;
    - optionally the caption cues there;
    - the media and nested sequences the clips play.
  - **JSON.** `clipboardToJson` / `clipboardFromJson` ("wabcut-clipboard" v1) wrap that project's .wabcut JSON. So a
    document pasted from another Wabcut is versioned, bounded and validated like a project file.
  - **Paste.** `pasteClips` places the clips exactly, keeping speed, reverse, freeze frames, time remap, effects and
    keyframes. It gives them new ids, link groups and effect instances. It follows Premiere's rules:
    - overwrite replaces what lies under each pasted clip;
    - insert ripples the destination and sync-locked tracks and the captions;
    - clips go to the lowest targeted track plus their offsets, and missing tracks are added;
    - media is found by id, fingerprint or path, else added (reported).

    One undo step. The result passes `validate(Strict)` or the paste is refused.
  - **Attributes.** `pasteAttributes` / `removeAttributes`: motion, opacity, time remapping, volume, panner and
    effects, each chosen. Keyframe times keep their offset from the clip's start or scale to its length.
  - **Commands.** The command layer keeps the clipboard in `ClipboardServices`. An engine without one keeps its own.
- **Beat grid** (`Sequence::beatGrid`, `FinishingOps.h`): tempo in thousandths of a BPM (exact), a downbeat (`phase`),
  beats per bar and optional detected beats (music that drifts). `BeatGrid::beatsIn` / `nearestBeat` / `beatAtOrAfter` give
  beats with their place in the bar; `edit::setBeatGrid` sets or clears it; snapping targets its beats
  (`SnapOptions::beats`). Detection is `wabcut_ai_analysis` (docs/ai.md, "Media analysis").
- **Titles** (`TitleOps.h`): a title is a generated item of kind `"wabcut.title"` (text in UTF-8 with `
` line breaks, font
  family and style; `info` = one unbounded still video stream at the canvas size, alpha, no audio), a clip of it on a video track,
  and the clip's `"wabcut.title.style"` effect with the numeric styling (size px, fill RGBA, stroke width + RGBA, shadow
  opacity/angle/distance/softness and colour `shadow.red/green/blue` (black by default, C.6), alignment 0/1/2, line spacing,
  tracking), keyframable like any effect parameter. Placement is the clip's intrinsic Transform.
  - Ops: `addTitle(sequence, track, at, duration, text, style, font)` makes all three in one ChangeSet (an overwrite on the
    track); `setTitleText` and `setTitleFont` change the item, so every clip of the title follows (refused while a clip of it is
    on a locked track).
- **Motion graphics** (C.13, `Graphics.h`, `GraphicOps.h`, `GraphicResolve.h`, `GraphicJson.h`; [graphics.md](graphics.md),
  rendering in `docs/internal/c13-graphics-handoff.md`):
  - **The model.** A graphic clip plays a canvas item (generator `"wabcut.graphic"`, one per frame size and rate, shared)
    and holds a `Graphic` on its content (`ClipContent::graphic`, compared deeply): layers of text (rich runs, text-range
    animators), shapes (rectangle, ellipse, polygon, star, keyed path; gradient fills, strokes, trim), images and groups,
    each with a keyframable `LayerTransform`, blend mode, drop shadow, I.6 masks, in/out points and responsive pins;
    optional responsive time; the template instance it came from. Keys are in the clip's curve time (design time for a
    responsive graphic). `graphicProblem` bounds and checks all of it (256 layers, depth 8, 16 KiB of text, 1024 path
    vertices, ranges): project files, templates and Lottie files are untrusted.
  - **Ops** (one `EditResult` each): `addGraphic`, `setGraphic`, `addGraphicLayer` / `updateGraphicLayer` /
    `removeGraphicLayer`; `applyPresetToLayer` / `applyGraphicPreset` (fade, slide, scale pop, typewriter, word by word,
    wipe, bounce; in, out, loop) write editable keys; `makeTemplate`, `saveProjectTemplate`, `instantiateTemplate`,
    `setTemplateFields` (a "clip" duration field trims the clip); `setBrandKit`.
  - **Project data.** `ProjectState::brandKit` (named colours, font roles, logos) and `templates` (sorted by id);
    `ChangeSet::brandKitChanged` / `templatesChanged`. Caption styles may name brand colours and a font role
    (`CaptionStyle::brand`, `resolvedCaptionStyle`).
  - **The resolver.** `resolveGraphic` / `resolveGraphicClip` flatten a graphic at a time into a `DrawList`: items bottom to
    top with the transform to canvas pixels, multiplied opacity, blend, shadow, masks in canvas pixels (by owner), and
    resolved paths and paints, text runs (brand fonts and colours applied, per-cluster animator values) or images. Pins
    use a `TextMeasurer` (the renderer's; `approximateTextMeasurer` otherwise). The render side only rasterises it.
  - **Lottie** import is in `wabcut_interchange` (`Lottie.h`); the user's template library folder is read and written by
    `GraphicJson.h` (`readTemplateLibrary`, `writeTemplateToLibrary`).
- **`MediaLocator`** (`MediaLocator.h`):
  - The absolute path, the project-relative path, and `SourceFingerprint{size, mtimeNs, sampleHash}`, where the hash is FNV-1a 64
    of the first and last 64 KiB.
  - The `offline` flag and an optional proxy `{path, ProxyProvenance{generator, settings, source fingerprint, generation}}`.
  - Helpers: `fingerprintFile`, `relativeMediaPath`, `resolveMediaPath` (absolute path first, then relative).
- **`Sequence`**: frame rate, canvas, audio rate, drop-frame display, video tracks V1..Vn (bottom→top) and audio tracks A1..An
  (`shared_ptr<const Track>`), In/Out marks (the Out mark is exclusive) and markers. It also has:
  - An explicit **`duration`**, at least `contentEnd()`. It grows with the content and never shrinks on its own, so trailing gaps
    survive ripples. Change it with `setSequenceDuration` / `fitSequenceDurationToContent`.
  - An optional **`workArea`** inside it (`setWorkArea`).
  - **`submixTracks`** (0.2, docs/audio.md): audio submix tracks S1..Sn, buses without clips that audio tracks are assigned
    to (`Track::output`) or send to (`Track::sends`, at most 5), with their own effects, fader, panner, mute and solo.
- **`Track`**: kind, name, enabled/locked/muted/solo, targeted flag, sync-lock flag, `clips` and `transitions`. Clips are sorted
  by start and **never overlap**; gaps are implicit.
  - Audio tracks have a **`role`** (`TrackRole`: none, dialogue, music, effects; Premiere's Essential Sound types) that the
    dialogue-first tools read: auto-ducking lowers music under dialogue. Set with `setTrackState` (a header change); video tracks
    are always none.
- **Transitions** are edge objects on a track: `Transition{id, outgoing (A), incoming (B), before, after, effect, params,
  alignment}`.
  - A transition covers [cut − before, cut + after). It can also be one-sided, at A's tail or at B's head.
  - `alignment` (C.6, stored since schema 8) is Premiere's: `Center` (whole frames of half the length before the cut, sides
    within a frame of each other), `StartAtCut` (before = 0), `EndAtCut` (after = 0) or `Custom` (the cut lies `before` into
    the transition: an explicit offset). One-sided transitions are `EndAtCut` at A's tail and `StartAtCut` at B's head.
    Operations keep it: a new length is split by it (Custom keeps the offset), changing it re-splits the length, sides set
    explicitly take the alignment they describe (`alignmentOf`; Custom when asked for), and sides shortened to the available
    handles keep it only if they still fit it. Validation checks StartAtCut/EndAtCut against the sides.
  - `handles()` gives the timeline ticks each side needs beyond its clip. Structure validation checks the geometry; Strict checks
    the handles direction-aware (`Clip::headRoom`/`tailRoom`: a reversed clip's handles lie on the other side of its source range,
    a freeze frame has any).
  - `effect` is a stable transition id from the `EffectRegistry` (`crossDissolve`, `dipToBlack`, `dipToWhite`, `wipe`,
    `constantPower`, `constantGain`; 0.2: Premiere's library, [effects.md](effects.md)); `params` are constants (a wipe's
    `angle`, `feather`; a menu parameter is one of its options' whole numbers). Unknown ids are kept and bypassed.
  - The evaluator may request two source layers around a transition; clips on a track still never overlap.
  - Ops (`FinishingOps.h`): `addTransition` (two- or one-sided; `HandleFit::Exact` fails naming the clip and how much media it
    has, `UseAvailable` shortens each side to the media and the clip, like Premiere's insufficient-media adjustment),
    `setTransition` (sides, a `duration`, an `alignment`), `removeTransition`, `transitionAtEdge` (split by alignment),
    `applyDefaultTransition` (Ctrl/Cmd+D video, Ctrl/Cmd+Shift+D audio: the edit point nearest the playhead on targeted tracks)
    and `applyDefaultTransitions` (Shift+D: both ends of each selected clip). Other edits drop transitions they invalidate, and
    when a clip is split the transition at its Out point moves to the right-hand piece.
- **`Clip`**: id, media id, stream ref (the n-th video or audio stream), `timelineRange`, `sourceIn`, `Speed` (exact rational;
  0/1 is a freeze frame), `reversed`, `maintainPitch` (audio), enabled, link group, group (0.2, Premiere's Clip > Group,
  `ClipGroups.h`; saved as the optional `group` field) and color index.
  - `sourceTimeAt(t)` is the source time shown at a timeline time (direction-aware, clamped into the source range; a freeze shows
    `sourceIn`; a remapped clip reads its remap curve); `timeMap()` gives the same mapping as a `TimeMap` (negative speed when
    reversed; not for remapped clips).
  - Trims, splits and slips go through direction-aware helpers (`moveHead`, `moveTail`, `splitOff`, `headRoom`, `tailRoom`), so a
    reversed clip keeps every frame where it was and a freeze frame can grow without limit.
  - Name, parameters (with their curves) and the effect stack live in a shared copy-on-write `ClipContent`, read through
    `name()`/`properties()`/`effects()`. Clips that keep their media's defaults share the media's block; both halves of a razor cut
    share theirs until one is edited.
- **Parameters** (`Parameters.h`): a static table, `allParameters()`, of
  `ParameterInfo{ParamId, stable key, display name, unit, default, min, max, PropertyRole}`.
  - The keys are `motion.position.x/y`, `motion.scale`, `motion.rotation`, `motion.anchor.x/y`, `opacity.opacity`, `volume.level`
    and `panner.balance`. The keys are part of the file format.
  - `ParamValue` is a constant or a keyframe `Curve`; `ClipProperties` holds one `ParamValue` per parameter.
- **Keyframes** (`Parameters.h`): `Curve{keys}` with `Keyframe{time, value, interpolation, in, out}`; interpolation is Hold, Linear,
  Bezier (per-side slope and influence, like After Effects' speed/influence), EaseIn, EaseOut or EaseInOut. A segment is shaped by
  the leaving side of its first key and the arriving side of its second.
  - **Curve time is media time** (source ticks), Premiere's behaviour: keys stick to the picture, so a slip moves them with it, a
    trim hides keys outside the clip but keeps them (they come back when the clip grows again), and both halves of a razor cut share
    the curve and evaluate seamlessly across the cut. None of these edits touches a curve, so they keep their speed.
  - `Clip::curveTimeAt(t)` maps a timeline time to curve time: the source time shown, except on a freeze frame, where effects keep
    animating at normal rate from the held frame (Premiere's Frame Hold with "Hold Filters" off). `Clip::parameterAt(id, t)` is the
    evaluator's entry point.
  - `Curve::valueAt` is exact and deterministic: integer ticks until the segment is normalised to [0, 1], values clamped outside
    the keys, exact at a key, Bezier time solved by 64 bisection steps on a curve whose time controls are clamped into the segment
    (so time is monotonic).
  - Ops: `addKeyframe`, `moveKeyframe`, `moveKeyframes` (C.6: several keys by one delta, clamped so no moved key leaves the
    clip's media range, a moved key replacing an unmoved one it lands on; `appliedDelta` says how far), `removeKeyframe` (the
    last key leaves a constant), `setKeyframe`, `setAnimated` (Premiere's stopwatch) and `setParameter`, for intrinsic parameters
    and effect parameters (`ParamRef`). Each touches one clip.
- **Effects** (`Effects.h`): a clip's `EffectStack` is an ordered list of `EffectInstance{id, effect (stable id such as
  "wabcut.blur.gaussian"), version, enabled, params (sorted key → ParamValue, curves allowed), mask (reserved)}`. Tracks carry a
  stack applied to the composited (video) or mixed (audio, before the fader) track, and sequences two master stacks: `effects`
  (video; no evaluator yet) and `audioEffects` (the master bus, before the master fader and meters).
  - The `EffectRegistry` holds descriptors: id, version, display name, category, kind (video, audio, video/audio transition),
    `intrinsic` (Motion/Transform, Opacity, Volume, Panner: their parameters are `ClipProperties`), parameter infos (key, unit,
    range, default, animatable) and the `EffectCapability` flags an evaluator needs.
  - Seeded for the finishing shelf: Basic Correction (exposure, contrast, saturation, temperature, tint), Vignette, Gaussian Blur,
    Sharpen, Crop, Drop Shadow, Title Style (`wabcut.title.style`, title clips only); audio Gain, Parametric EQ (3-band),
    Compressor, Limiter, Reduce Noise (RNNoise); the transitions above. Fades are keyframes on opacity or volume (`clip.fade`).
    Implementations belong to the renderer and the audio engine.
  - Unknown effects (an id the registry lacks, or a newer version) and unknown parameter keys are kept and saved unchanged;
    `isEvaluable()` is false for them, so evaluators bypass them.
  - Ops: `addEffect`, `removeEffect`, `moveEffect`, `setEffect`, on clips or on any `EffectTarget` (a clip, an unlocked track of
    the effect's kind, the video or the audio master). The parameter and keyframe ops take a target too; track and master
    parameters are effect parameters, keyed in timeline time. A track change is a header change of the track, a master change
    sets the sequence's `settingsChanged`.
- **Speed and holds** (`FinishingOps.h`): `setSpeed` is Speed/Duration (Ctrl/Cmd+R): each clip keeps its source In and material
  and gets length = source length / speed in whole frames; `reversed`, `maintainPitch`, and `ripple` (later material on the track
  and sync-locked tracks moves; without it the clip only grows into the gap after it). `speedForDuration` finds the speed for a
  target length without reading past the media. `frameHold` is Add Frame Hold (cut at a time, the rest freezes) or, with a
  duration, Insert Frame Hold Segment (a hold inserted with an insert-style ripple). Both refuse remapped clips.
- **Time remapping** (C.6, `TimeRemap.h`, ops in `FinishingOps.h`): Premiere's Time Remapping — speed ramps, frame-hold and
  reverse segments inside one clip.
  - A remapped clip's `ClipContent::timeRemap` is a `Curve` from **curve time** to **source position** (whole ticks, exact in a
    double up to 2^53). Its curve time runs with the timeline from `sourceIn` (`curveTimeAt(t) = sourceIn + (t − start)`, like a
    freeze frame), so its speed stays 100 % forwards, `sourceIn` is a curve time, and trims, splits and slips move along the curve
    unchanged; both halves of a razor cut share it. Parameter keyframes keep animating at timeline rate through holds and reverse.
  - Segments between keys play **forward**, in **reverse** or **hold** (equal positions, or a key with Hold interpolation: a
    step); before the first key and after the last the curve holds. A segment with two linear sides is **straight**: an exact
    rational speed, `position = a + (c − a.t)·(b − a)/(b.t − a.t)` rounded toward `a` (integer arithmetic), so a straight
    segment of speed n/d matches `sourceTimeAt` of a clip at that speed. Ease/Bezier sides make a **ramp**: the curve's value
    rounded toward its first key and kept strictly before the second; `remapProblem` refuses Bezier controls that would turn it
    back, so every segment is monotonic.
  - The source time shown is the position in forward segments and holds, and the position minus one tick in reverse ones (the
    frame that ends there, as for a reversed clip): exact at every key, monotonic between keys.
  - Ops: `setTimeRemapEnabled` (on: a remap reproducing the clip — the identity over the whole media at 100 %, keys at the clip's
    ends for other speeds and reverse, one key for a freeze; off: back to a constant speed — the single straight segment's speed
    and direction, a freeze for a video hold, else 100 % from the first frame), `setTimeRemap` (a whole curve and the curve time
    at the clip's start: importers), `addTimeRemapKey` (samples the source position; holds stay holds, otherwise a new linear
    key can reshape a smooth ramp), `moveTimeRemapKey` (between neighbours, or `ripple`:
    later keys move too), `setTimeRemapKey` (position, interpolation), `removeTimeRemapKey`, `setTimeRemapSpeed` (a segment's
    speed: its end key and the later keys move), `addTimeRemapHold` and `addTimeRemapReverse` (a 100 % boomerang). Each keeps the
    clip's length; every frame shown and every key must lie inside the media (`InvalidArgument`), and head/tail rooms stop
    before a hold past the media's end (otherwise a remapped tail can grow freely: the last key holds).
  - Remapped clips refuse `setSpeed` and `frameHold`; validation requires speed 100 % forwards and a valid curve.
    Remap edits reject time arithmetic that exceeds the signed tick range, and validation rejects unrepresentable segment
    durations and clip curve ranges before evaluating them.
  - `timeRemap.setKey` exposes a key's source tick position, interpolation and incoming/outgoing Bezier handles in one
    validated command, including linked matching curves, dry runs and one undo step. Slopes use source ticks per second of
    curve time (`kTicksPerSecond` is 1x forwards); influences are fractions of the adjacent segment. This permits ramps
    joining nonzero speeds without changing the model's `Curve` representation or bypassing source-bound validation.
- **Sharing granularity:** project → sequence → track → clip chunk.
  - `ClipList` keeps a track's clips in shared copy-on-write chunks: about 64 clips each, split above 128, merged below 16.
  - Chunks store **absolute times**. A one-clip edit on a 10k-clip track copies one chunk, but a ripple rewrites every chunk after
    the edit point (well under 1 ms at 10k clips).
  - Chunk-relative offsets would make ripples O(chunks). They are deferred, because clip reads would then return values instead of
    references.
- **`EditOps.h` / `ProjectOps.h`** are pure functions `(const ProjectState&, SequenceId, args) -> std::expected<EditResult, Error>`.
  They take the whole project because ids come from its generator and clamping needs the media.
  - `EditResult{state, changes (ChangeSet), placed, created, removed, playhead?, appliedDelta, createdTransitions, createdEffects,
    createdTracks}`.
  - Edit ops: insert, overwrite (`resolveThreePointEdit`, `defaultSourcePatches`), lift, extract, razor/add edit, delete, ripple
    delete (clips or gap), ripple trim prev/next (Q/W), trim, ripple (B), roll (N), slip (Y), slide (U), move (overwrite/insert,
    across tracks), link/unlink, marks (I/O/X//) and markers. Snap targets are in `Snap.h`.
  - Project ops cover bins, media, sequences, tracks, track state, clip attributes, duration and work area.
  - Premiere semantics include source patching vs track targeting, sync locks (ripples cut through sync-locked tracks), linked
    selection (`LinkMode`) and locked tracks never changing, with golden-case tests for each.
  - A linked partner on a locked track is left byte-identical. It may stay the only member of its link group, which `validate()`
    allows only on a locked track; the group dissolves when that track is unlocked.
  - **Multi-track recordings (OBS, ShadowPlay, Game Bar; A.10)**: every audio stream of a recording becomes its own named,
    linked audio track when the clip is edited in.
    - Names: the probe stores each stream's name in the file as `AudioStreamInfo::title` (MKV track name; MP4 handler name,
      where OBS's hybrid and native MP4 keep it). Generic names muxers write for every track ("SoundHandler", "Core Media
      Audio", ...; the one list is in `src/media/src/StreamInfo.cpp`) read as untitled. `sourceTrackName` recognises common
      sources, ignoring case and punctuation: game / desktop / system / app audio -> **Game**; mic, mic/aux, voice,
      commentary -> **Mic**; Discord, voice chat, party chat, TeamSpeak -> **Discord**; music, Spotify, BGM -> **Music**;
      (stream) alerts -> **Alerts**; OBS's `Track1`..`Track6` -> **Track N**. A number after a recognised name stays ("Mic/Aux
      2" -> "Mic 2"); other titles are used as written (at most 32 bytes). Untitled streams of a file with several of them
      are named by position (`sourceTrackNames`): the two of a recording with video are Game and Mic (ShadowPlay's "separate
      both tracks", Game Bar with the microphone), otherwise "Track 1", "Track 2", ... A file's single stream keeps
      Premiere's default patching unless its title is recognised (a camera's handler name never renames A1).
    - Routing (`audioPatchesByName`, used by `defaultSourcePatches` from A1 and by the library drop from the track under the
      pointer): a stream goes to the unlocked audio track with its name (ignoring case); an untitled stream to its place in
      Premiere's default patching when that track has no name; else to the next free track (unlocked, unnamed, empty); else
      to a new track. `SourcePatch::newTrack` carries the name: without a destination the edit adds an audio track below the
      last one (`EditResult::createdTracks`), and an unnamed destination is named. Locked tracks are never destinations: a
      locked Mic track is passed over and the next recording's mic takes a free track or a new one, named Mic. One undo
      step restores the track list.
    - Roles: a track the edit adds or names gets the role its name implies when it has none: Mic and Discord dialogue,
      Music music, Game and Alerts effects (Essential Sound's SFX); "Track N" and other titles none. Existing tracks keep
      their roles.
    - Muting or soloing a source: every recording's Mic lands on the one Mic track, so that track header's mute or solo
      (`track.setState`) acts on every mic at once. Commands: `edit.insertFromSource` / `overwriteFromSource` and `clip.insert`
      / `clip.overwrite` use this routing by default and report `createdTracks`; a `patches` entry takes `newTrack`
      (`{"source": "A3", "newTrack": "Discord"}`).
- **`ChangeSet.h`**: `ChangeSet{sequences, addedSequences, removedSequences, addedMedia, removedMedia, modifiedMedia, binsChanged}`.
  - Each `SequenceChange` holds `{sequence, tracks, addedClips, removedClips, modifiedClips, roles}` plus flags for settings,
    marks, markers, extent and the track list.
  - `tracks` is a list of `TrackChange{track, ranges, headerChanged, transitionsChanged, added, removed}`. `ranges` is the sorted,
    disjoint union of the old and new extents of every changed clip.
  - `modifiedClips` includes clips moved between tracks.
  - `roles` is a set of `PropertyRole` flags: Timing, Source (also speed, direction, pitch), Enabled, Label, Link, Track, Motion,
    Opacity, Audio, Effects. A keyframe edit reports its parameter's role.
  - `diff(before, after)` computes a ChangeSet from structural sharing: pointer-equal sequences, tracks and chunks are skipped.
- **`EditSession`** (mutations on the GUI thread):
  - `snapshot()` is available to other threads. Acquire it once per job, and never on the audio callback.
  - `apply(label, fn, mergeKey)` returns fn's outcome; an `EditResult` carries its ChangeSet. `commit()` returns
    `expected<ChangeSet>`, and `undo()/redo()` return `optional<ChangeSet>`. Also `canUndo/canRedo` and the step labels.
  - Coalescing: `beginGesture/endGesture` (they nest, and `cancelGesture` drops the gesture) or merge keys, for drags, trims and
    slider scrubs.
  - Listeners receive `ChangeEvent{kind, label, state, changes}`.
  - The undo history has an entry budget (500) and a memory budget, counted as the estimated bytes not shared with the next
    revision.
  - Superseded snapshots are queued and freed by `releasePending()` from a worker (`deferRelease`).
  - Selection, hover and playhead are **not** document state and are never in undo.
- **Premiere editing and sequence commands (P.2, E5)**: `FrameScaling.h`, `SequenceSettings.h`, `EditingOps.h`; commands in
  `src/command/src/CatalogEditing.cpp` ([commands.md](commands.md)).
  - **Scale to Frame Size** is a flag on the clip (`ClipContent::scaleToFrame`): the picture is fitted to the sequence frame
    (whole picture visible, its displayed size: pixel aspect and rotation) *before* Motion, whose Scale stays 100 %. Evaluators
    multiply Motion's scale by `scaleToFrameFactor(state, clip, width, height)` (1 without the flag; a nested clip fits its
    sequence's frame). **Set to Frame Size** / **Set to Fill Frame** write the fit (or fill) into Motion's Scale instead and
    clear the flag. `MediaItem::scaling` (Preferences > Media > Default Media Scaling, set on import) is applied by every
    source edit to the video clips it places (`scaledClipContent`).
  - **Sequence Settings** (`setSequenceSettings`): frame size, timebase, pixel aspect, fields, audio rate, preview format,
    start timecode and the time display formats (`VideoTimeDisplay`: timecode, frames, 16 mm / 35 mm feet+frames, which
    `formatSequenceTime` applies and the app shows; `AudioTimeDisplay`: audio samples, milliseconds, stored for the audio
    units display). A new timebase **conforms** the edit: on each unlocked track every edit
    point moves to the nearest frame of the new grid that both clips' media reach (else the other neighbouring frame, else the
    two sides split at the floor and ceiling frames), clips shorter than half a new frame go, and transitions, markers, marks,
    the work area, caption cues and the duration snap to the grid. Locked tracks are left alone. `SequencePresets.h` has 61
    presets (720p to UHD at 23.976 to 60 fps with drop-frame variants, 120/144/240 fps, vertical 1080x1920, square 1080x1080).
  - **Duplicate** (`duplicateSequence`, `duplicateMedia`; names "<name> Copy", "<name> Copy 2", ...) and **New Sequence From
    Clip** (`newSequenceFromClip`: the item's displayed size, its interpreted frame rate, pixel aspect and audio rate, the clip
    edited in at 0 with its marks).
  - **Replace Edit** (`replaceClips`, `replaceMatchFrameIn`): another item takes a clip's place over the same timeline range,
    keeping its effects, keyframes, speed and links (linked partners are replaced in sync); the source starts at the item's In
    point or, with Match Frame, lines the source frame up with the playhead. **Replace Footage** re-points an item (and its
    subclips) at another file, keeping every use. **Make Offline** (`MediaItem::madeOffline`) keeps an item offline until it
    is linked again.
  - **Make Subsequence** (selection and linked partners, or the targeted tracks between In and Out), **Join Through Edits**,
    **Go to Next/Previous Gap** and **Close Gap** (empty time where every involved track is empty, sync-safe), **Frame Hold
    Options** (`HoldOn`: source timecode, sequence timecode, In, Out, playhead; `ClipContent::holdFilters` makes
    `curveTimeAt` stay at the held frame).
  - **Merge Clips** (`mergeClips`) builds a merged source sequence (`MulticamInfo::merged`) from a camera clip and separately
    recorded audio, synced by In points, Out points, timecode, clip markers or the audio waveform, optionally without the
    camera's audio; it edits in like a clip with every audio angle heard.
  - **Interpret Footage** (`MediaItem::interpretation`): assumed frame rate, pixel aspect, field order, alpha (ignore, invert,
    premultiplied or straight). An assumed rate is a conform: source edits and `interpretFootage` set the clip's speed to
    `conformSpeed(item)` (assumed / native; 120 fps assumed 30 is 1/4), which every evaluator already reads; the pixel aspect
    and alpha options reach the compositor through `buildLevel` (`CompositeLayer::ignoreAlpha` / `invertAlpha`). **Modify >
    Timecode** (`MediaItem::timecode`: start timecode and reel name; `mediaStartTimecode`).
  - Preferences > Timeline's default still duration is stored per item (`MediaItem::stillDuration`, from import); the default
    video and audio transition durations are the app's settings passed to the transition commands.
- **`ProjectIO`**: `.wabcut` JSON (nlohmann; int64 ticks as JSON integers) at `schemaVersion` 18, with a migration chain (the
  history is in `ProjectIO.h`). Step 1→2 converts the media locator, parameters, sequence duration and transitions. Step 2→3 only
  adds optional fields: a parameter is `{"value": x}` or `{"keys": [{"t", "v", "i", "in", "out"}]}`, clips carry `effects`,
  `reversed` and `maintainPitch`, freeze frames have speed 0/1, transitions carry `params`, tracks and sequences carry
  `effects`. Steps 3→4→5→6 add caption tracks, media generators (titles) and the beat grid; 6→7 the audio track `role` and the
  sequence's `audioEffects` (both written only when set). Step 7→8 writes each transition's `alignment`, derived from its sides
  (`alignmentOf` with the sequence's frame), and clips may carry `timeRemap` (a curve). Step 8→9 (C.7) adds caption track
  `style` (absent: the look captions had), cue `speaker`, and the project's `speakers`, `transcriptEdits` and
  `captionPresets` (written only when there are some). Step 9→10 (C.8) adds clip `source` in place of `media` for clips of a
  sequence (`{"type": "sequence", "sequence", "angles"}`; a source type the build does not know is kept and saved back
  unchanged), sequence `multicam` and media info `startTimecode`. Step 10→11 (C.9) adds the project's `reviewNotes`
  (written only when there are some). Step 11→12 (AI-14) adds the matte mask shape (and marks A.8's and A.9's
  fields, review E5-P2-8). Step 12→13 (C.13) adds clip `graphic`, the project's `brandKit` and `templates` and caption
  style `brand` (all written only when set; GraphicJson.h's format). Step 13→14 (X.1) adds the sequence `startTimecode`,
  step 14→15 (C.15) clip `blendMode` and media generator `params`, and step 15→16 (A.10) the media info audio stream
  `title` (the stream's name in the file, for multi-track ingest), all written only when set. Step 16→17 (P.2) adds clip
  `scaleToFrame` and `holdFilters`, media `scaling`, `interpretation`, `timecode`, `stillDuration` and `madeOffline`,
  sequence `pixelAspect`, `fields`, `previewFormat`, `videoDisplay` and `audioDisplay`, multicam `merged`, clip
  `timeInterpolation` and the generator kinds `wabcut.synthetic.barsAndTone` and `wabcut.synthetic.countingLeader`, and
  colour management back from I.9: the project's `colorManagement` (`mode`, `peakNits`, `referenceWhiteNits`,
  `autoDetectLog`), clip `inputColorSpace` and media video stream `logColorSpace` (docs/color.md); all written only when
  set, one schema for the whole 0.2 round.
  - Malformed or out-of-domain values fail with `ErrorCode::ParseError`, and newer schemas fail with `VersionMismatch`.
  - Files are untrusted (C.10, fuzzed in `tests/fuzz`). JSON nested deeper than `kMaxJsonNesting` (256) is refused before
    parsing (`jsonNestingWithin`). Kept JSON such as an unknown clip source is written back recursively, and 100,000 levels
    of it would exhaust the stack. History chunks, the history index and review packages are checked the same way.
  - Loading never writes, renames or deletes anything.
  - Saves are atomic (temp file, flush, same-directory rename). They refuse a state that would not load back, and rotate
    `<file>.bak1..5`; only loadable files rotate in, so the last valid save is always kept.
  - `writeAutosave`/`Autosaver` write `<file>.autosave`.
  - `recoveryCandidates()` lists the autosave and backups for explicit crash recovery.
- **Evaluator obligations** (render, playback and audio engineers):
  - **Time.** For a clip at timeline time `t`, read the frame at `clip.sourceTimeAt(t)` (reversed, freeze and time-remap aware),
    and evaluate every parameter at `clip.curveTimeAt(t)` - use `clip.parameterAt(id, t)` and `effectParameterAt(instance, key,
    clip.curveTimeAt(t))`, never `valueAt(t - clip.start())`. Video evaluates once per output frame at the frame's start time;
    audio per sample (or per block with linear ramps no longer than one video frame) at the sample's time.
  - **Time remapping (C.6).** `sourceTimeAt` already follows the remap, for any `t` (also beyond the clip's ends, for
    transition handles: do not extrapolate with `speed`/`sourceIn`, which are 100 % and a curve time on a remapped clip).
    `curveTimeAt` runs with the timeline. A remapped clip's `speed` is 1/1, so `speed.isUnity()` no longer means "plays its media
    at 100 %": `clip.unitySpeedRanges()` gives its straight 100 % forward segments (the media continues across adjacent ones),
    read straight from `sourceTimeAt` at each range's start. A.10: the rest of the curve sounds too (ramps, other speeds,
    reverse; holds are silent), along `remapPositionAt` per sample, pitch-kept or varispeed (`maintainPitch`); see the
    playback module's `TrackRenderer.h` ("Speed") and `audio/dsp/SpeedRender.h`.
  - **Values** come out of the model already in their units (`ParamUnit`); clamp nothing yourself, except what the effect
    defines (opacity 0..1). Curves are deterministic, so preview and export agree bit for bit.
  - **Order.** Video: source frame → intrinsic Motion (Transform) and Opacity → the clip's stack in order (skip disabled
    instances and any for which `isEvaluable()` is false) → composite by track (V1 at the bottom) → track stacks → master stack
    (both placeholders today). Audio: source → speed/pitch (`maintainPitch`) → Volume and Panner → the stack → track stack →
    track fader/pan → mix → master audio stack → master fader.
  - **Titles.** A title clip's source frame is its generator drawn on a transparent canvas of the item's size: the text with the
    font family/style, styled by its `wabcut.title.style` instance evaluated at `clip.curveTimeAt(t)` (the style is how the
    source is drawn, not a pass over it). Then Motion, Opacity and the rest of the stack apply as for any clip. Its shadow has a
    colour (`shadow.red/green/blue`, 0..1, black by default) next to opacity, angle, distance and softness.
  - **Capabilities** tell you what each effect needs: `Neighborhood`/`ExtendsBounds` (pad and handle edges), `LinearLight`
    (convert from the working space first), `UsesAlpha`, `AudioStateful` (process in order, pre-roll after a seek), `AudioLookahead`
    (report latency and compensate).
  - **Transitions.** Over [cut − before, cut + after) render both layers: A continues past its Out point by reading
    `A.sourceTimeAt` beyond its end (the model guarantees the media is there), B starts early likewise; mix by the transition id
    with progress p = (t − (cut − before)) / (before + after). One-sided transitions mix with transparent (video) or silence
    (audio). `crossDissolve`: lerp; `dipToBlack`/`dipToWhite`: fade A to the colour over the first half, the colour to B over the
    second; `wipe`: a straight edge at `angle` with `feather`; `constantPower`: gains cos/sin(p·π/2); `constantGain`: linear;
    `exponentialFade` (0.2): log10(1 + 9x). The rest of Premiere's video transitions (0.2) and their options: [effects.md](effects.md).
  - **Speed.** Video: the frame nearest below `sourceTimeAt(t)` (Frame Sampling), or with the clip's Time Interpolation (P.2,
    docs/motion.md) a group of the two frames around it mixed by the source time's place between their PTS (Frame
    Blending: a dissolve; Optical Flow: a flow mix along their motion). Audio (A.10): resample by the
    speed (varispeed, band-limited), or time-stretch when `maintainPitch` is set (WSOLA); reversed audio plays its samples
    backwards; speeds under 0.1x or over 20x and holds are silent. Audio clips are never frame holds.
- **Deferred**:
  - Speed changes on nested and multicam audio (heard at 100 % only).
  - Masks.
  - Chunk-relative clip storage.
  - A memory budget for the redo branch.

### `media` — owner: Media engine (FFmpeg)
Public headers in `src/media/include/wabcut/media/`. No Qt. Every fallible call returns `std::expected<T, Error>`.
- **Source time.** Media positions are `Ticks` of *source time*: ticks since the **media origin**, the earliest
  `start_time` among the file's audio and video streams, so the streams of one file share one time base (a clip's
  `sourceIn` is in this time). Stream timestamps convert only through `StreamTime.h` (`streamToTicks`: nearest;
  `ticksToStream`: the unit containing the time, exact inverse), built on Time.h's checked `rescale`. Overflow is an
  error, never a wrapped value.
- `probe(path) -> expected<MediaInfo>` fills in:
  - the container and durations;
  - the exact nominal `Rate`, snapped to broadcast rates. VFR is read from the MP4/MOV sample index, else from
    FFmpeg's average vs real base frame rate;
  - the pixel format, bit depth and chroma subsampling;
  - the colour tags as written, chroma siting included (Unknown when untagged);
  - `alpha` and the clockwise display rotation;
  - the audio rate, channels and layout.

  Cover art is skipped.
- `sourceFingerprint(path)` (`Fingerprint.h`): size + mtime (ns) + a hash of the first/last 64 KiB, memoised per
  (path, size, mtime). It is the source identity for caches and `MediaLocator`.
- **`FrameHandle`** (`FrameHandle.h`; `VideoFrame` remains an alias for one release): immutable, cheap to copy.
  - Timing: `pts`/`duration` in source time, where [pts, pts + duration) is exactly the span the frame is on screen,
    plus the original `streamPts` and `timeBase`.
  - Pixels: size and `PixelFormat` (NV12, P010, YUV420P/P10, YUV422P/P10, YUV444P/P10, RGBA8, YUVA420P, YUVA444P10).
    12-bit and higher sources are delivered as 10-bit in the MVP.
  - Colour: a resolved `ColorInfo` that is never Unknown. Untagged streams get BT.709 for HD, BT.601 for SD, limited
    range and left-sited 4:2:0 chroma. Also `AlphaMode` (from the stream; straight when unspecified).
  - Geometry and identity: sample aspect; `Orientation{rotationDegrees clockwise, mirrored}`, applied as rotate then
    mirror (FFmpeg's convention); and `SourceIdentity{fingerprint, generation}`.
  - Payload, a tagged variant:
    - `CpuPlanes` (up to 4 planes `{data, stride, width, height}`, in texels): the default.
    - `D3D11Texture{texture, arraySlice}` (Windows, `OutputMode::Native` on the shared device, see below): a slice of
      the decoder's NV12/P010 texture array, padded to the codec's surface size (read it from the texture's desc).
    - `CVPixelBuffer` and `DrmPrime{objectFds, offsets, pitches, modifier}` are declared and reserved.
  - Every payload holds an owner and a `DeviceToken{id, generation}`: 0 is the CPU, and the generation increases when
    a device is recreated. Payloads may be released on any thread except the audio callback. Native payloads pin
    decoder surfaces, so drop them within a couple of frames.
- `VideoDecoder::open(path, stream = -1 for the best stream, DecoderOptions{...})`. The options are
  `allowHardware`, `preferredBackend`, `threadCount`, `sourceGeneration`, `outputMode` and `nativeSurfaces`.
  `outputMode` is Cpu by default; Native yields native payloads where a platform path exists (today: d3d11va on the
  shared D3D11 device) and Cpu everywhere else, so consumers always check `payloadKind()`. The decoder is
  single-thread-affine.
  - `frameAt(Ticks)` returns the frame **containing** the time:
    - The decoder looks one frame ahead before delivering, so the duration is the distance to the next presentation
      timestamp, which keeps VFR correct. The last frame keeps its container duration, or the nominal frame period.
    - Edge cases: a time before the first frame returns the first frame; a negative time is `InvalidArgument`; a time
      past the end of the last frame is `NotFound`; a timestamp out of range is `DecoderError`.
    - Sequential calls never seek. Random access seeks to the preceding keyframe and decodes forward, frame-exact.
  - Backends, in `hardwareBackendOrder()`, are ordered by measured CPU-readback throughput for the GPUs present:
    - Windows: with an NVIDIA GPU, cuda → d3d11va → qsv; otherwise d3d11va → cuda → qsv.
    - macOS: videotoolbox.
    - Linux: vaapi → cuda, or cuda first with an NVIDIA GPU.
    - Software is always last. The order can be overridden with `setHardwareBackendOrder()` or
      `WABCUT_HW_BACKENDS`; `gpuVendors()` reports what was found.
  - A backend is kept only if the first frame comes out of the GPU and reads back. Profiles a backend refuses (e.g.
    H.264 10-bit / 4:2:2 before RTX 50) and devices that fail are remembered for the process. A hardware failure
    mid-stream falls back to software. `backendName()` reports the backend in use, and `stats().hardwareFallbacks`
    reports the fallbacks.
  - Hardware frames are read back lazily, only when delivered, to NV12/P010. GPU device contexts are shared
    process-wide.
- `AudioDecoder::open(path, stream, Rate outputRate, ChannelLayout out)` (`mono`/`stereo`/`surround51`/`surround71`/
  `fromCount`; the int-channels overload remains for one release), then `read(Ticks, span<float>) ->
  expected<AudioReadResult>`. libswresample does the resampling and rematrixing.
  - The span is always completely written: `leadingSilence` frames of silence, then `framesFromStream` decoded frames,
    then silence. `endOfStream` marks the end of the file. A short or silent read is not an error.
    `AudioReadResult` converts to `size_t` (`framesFromStream`) for one release.
  - Reads are sample-exact on the output grid. Seeks pre-roll into a fresh codec context, so the samples depend only on
    the seek point (apart from AAC's PNS noise, about −100 dBFS).
  - Not real-time safe: feed the audio callback through rings.
- **Shared D3D11 device (zero-copy, `docs/zero-copy.md`).** `setSharedD3D11Device(ID3D11Device*,
  ID3D11DeviceContext*)` hands media the renderer's device; `render::shareDeviceWithDecoders(QRhi*)` is the only
  caller in the app (ProgramMonitor on a new QRhi; `OffscreenRenderer` when asked). It returns the `DeviceToken`
  native frames carry and bumps its generation on every call; `nullptr` reverts to media's own device.
  - The hook turns on `ID3D11Multithread::SetMultithreadProtected` (decoder threads and the render thread share the
    immediate context) and registers the device in `HwDeviceRegistry` as the d3d11va device. It affects decoders
    opened afterwards; open decoders keep their device.
  - A `Native` decoder on the shared device prefers d3d11va over the readback order (nothing is read back), allocates
    its surfaces with `BIND_SHADER_RESOURCE` and `nativeSurfaces` extra, and decodes with one frame thread. Any other
    backend, a software fallback or another device produces CPU planes; the renderer also rejects (and counts)
    surfaces of a device that is not its own.
  - **Retention rules.** A `D3D11Texture` payload's owner is the decoder's frame reference: while it lives, the
    surface cannot be reused. The decoder pool is finite (codec references + `nativeSurfaces`), so every holder caps
    what it keeps: a consumer sizes `nativeSurfaces` for its total (the scheduler: source cache + mailbox + 4), the
    renderer releases a frame once the GPU is past it (`FrameImporter` retire latency, the same as for uploads), and
    nothing parks native frames in long-lived caches (`FrameCache`, thumbnails and peaks use Cpu). Releasing is legal
    on any thread except the audio callback.
- `DecoderPool` (`DecoderPool.h`): video and audio sessions keyed by (source, stream, cursor), with per-kind caps.
  - Sequential requests reuse the session whose cursor is just before them.
  - A global software decode-thread budget applies; hardware sessions share the process-wide devices.
  - Priorities: Preview > Audio > Prefetch > Background.
  - `DecoderPoolConfig::decoderOptions` (output mode, native surfaces) applies to the video sessions it opens.
  - `setEpoch(client, epoch)` cancels stale seeks at once. Results come back as `Job<T>` handles with callbacks.
  - A.10: `DecoderPoolStats::videoFramesDecoded / videoFramesDelivered / videoFramesReadBack / videoFramesConverted`
    sum the sessions' `VideoDecoderStats` (`SchedulerStats::poolFramesDecoded / poolFramesDelivered` carry the first
    two). A frame `VideoDecoder` only steps over (a seek's walk from the keyframe, the three frames between two shown
    ones of a 240 fps source in a 60 fps sequence) costs the decode alone: hwaccel surfaces are read back, and
    software frames wrapped or converted (12-bit and other formats `FrameHandle` cannot carry), only when they are
    returned. QSV's wrapper decoders still copy every frame (they recycle their surfaces).
- `FrameCache`: an LRU keyed by **(SourceIdentity, stream, original stream PTS, representation/proxy generation,
  decode DeviceToken)**.
  - CPU and GPU budgets are separate. GPU accounting is an estimate until native payloads exist. There is no disk tier.
  - `FramePin` and `beginDecode` in-flight reservations are never evicted.
  - Thread-safe, but not for the render thread.
- `ThumbnailService` / `WaveformPeaksService` run as background `Job`s with versioned, checksummed disk caches keyed by
  the source fingerprint plus options (a moved file keeps its cache).
  - Thumbnails: RGBA8 with orientation and alpha applied.
  - Peaks: min/max int16 pyramids (256/2048/16384 samples per bin). A.10: `WaveformPartials` (computeWaveformPeaks,
    `WaveformPeaksService::request`) delivers snapshots while a long stream decodes, at most every `interval` (250 ms):
    exact prefixes of the result (complete bins of every level only), the coarse levels built along with the finest
    one; the result and the cache file are the same with or without them. The app's `MediaPeakSource` draws them
    (coalesced redraws every 250 ms, the rest of a clip blank, the Source Monitor shades it) and decodes up to three
    streams at once, so the audio tracks of one recording build together: each pass reads the whole file, and passes
    that run together share the OS file cache.
- Logging: `setLogSink`/`setLogLevel`. FFmpeg's `av_log` goes through the sink, and the module prints nothing.
- **Media paths of more than one file (0.2).** Three kinds of media item name more than one file, or part of one, in
  their locator path; `media::detail::openInput` is the one place that opens them, so probing, decoding, thumbnails,
  waveforms, playback, proxies and export need nothing else:
  - an image sequence (`model/ImageSequence.h`): FFmpeg's image2 pattern plus `?start=&rate=`, opened with the image2
    demuxer (CinemaDNG frames too);
  - a spanned clip (`model/SpannedClip.h`): the first part plus `?span=<name>:<microseconds>|...`, every part in the
    first one's folder, played by FFmpeg's concat demuxer from an in-memory script (the durations make it seekable);
  - a layered Photoshop file's item (`model/LayeredFile.h`): the PSD plus `?layers=...&size=document|layer`; the layers
    are composited by `media/Psd.h` (our own reader: raw, RLE and ZIP channels, 8, 16 and 32-bit float - linear light,
    written out with the sRGB curve - RGB, grayscale and CMYK converted to RGB, PSB, masks, clipping, groups, the
    separable and the non-separable blend modes) and handed to FFmpeg's PAM demuxer from memory, so saving the PSD in Photoshop
    updates the item when it is next opened.

  The file that stands for such an item (`mediaFileToCheck`: the first frame, the first part, the PSD) is the one the
  existence checks, relinking and fingerprints use; `sourceFingerprint` mixes the whole path in, so two layers of one
  PSD are two pictures in the frame cache. `relinkedMediaPath` keeps the rest of the path when such an item is relinked
  or collected. Paths read from a project are untrusted: every parser bounds every field.
- **Camera RAW (0.2, `RawDevelop.h`).** Bayer frames (FFmpeg's `prores_raw` decoder, and CinemaDNG through its TIFF
  decoder) are developed in `VideoDecoder` into 10-bit Rec.709 Y'CbCr 4:4:4: black and white levels, white balance,
  a bilinear demosaic, the codec's camera-to-XYZ matrix and gain (ProRes RAW's `AVRawColorParams`), the BT.709 OETF.
  CinemaDNG arrives from FFmpeg linearised and white balanced in camera RGB, read as linear Rec.709. Highlights above
  the white point clip. RAW Source Settings (`model/RawSourceSettings.h`: exposure, and a white balance as the colour
  temperature and tint of the light to balance for) ride at the end of the item's media path
  (`?raw=exposure:+0.50,white:3200:-10`, `&raw=...` after an image sequence's own query) like the other virtual paths'
  descriptions, so every decoder, frame cache key (`sourceFingerprint` mixes the path), thumbnail and export develops
  the item alike without new plumbing; `openInput` opens the path before them, `VideoDecoder` reads them and the
  development applies the exposure and a Bradford adaptation in XYZ from the as-shot light (`AVRawColorParams::wb_cct`
  for ProRes RAW, else 6500 K). Changing them drops the item's proxy. R3D, BRAW, ARRIRAW and Canon RAW Light need their vendors' SDKs, which a GPL-3.0 program
  cannot link: a separate helper built with the SDK (`wabcut-raw-<ext>` in WABCUT_RAW_HELPERS or `raw-helpers/` next to
  the program; `RawHelper.h` documents the pipe protocol: a JSON header, then one raw frame per requested index) is
  read as FFmpeg rawvideo through a custom AVIOContext, so everything downstream works unchanged. No helper ships.
- **Interlaced footage (0.2).** `probe` records the stream's field order (`VideoStreamInfo::fieldOrder`; Interpret
  Footage can override it). Deinterlacing is a property of the layer (`render::CompositeLayer::fieldMode`, chosen by
  `model/FieldOptions.h`): the layer shader keeps one field's rows and interpolates the others from it (Premiere's
  method), or blends rows [1 2 1] / 4 (Flicker Removal), on CPU and zero-copy GPU frames alike.

### `audio` — owner: Audio engine (miniaudio)
Public headers in `src/audio/include/wabcut/audio/`; design notes in `src/audio/README.md`.
- **Engine and devices.** `AudioEngine::start(AudioDeviceConfig{sampleRate=48000, channels=2, periodFrames=512,
  backend=Default|NullRealtime|Null, deviceId, fallbackToNull=true, latencyOffset, maxBlockFrames=4096, nullLatencyFrames})`,
  `stop()`, `isRunning()`, static `enumerateOutputDevices()` (persistable ids `"<backend>:<native id>"`), `selectDevice(id)`,
  `reopenDevice()`, `deviceName()`/`deviceId()`/`backendName()`/`deviceError()`, `latency()`, `setLatencyOffset(Ticks)` and
  `stats()` (callbacks, frames, reopens, clock resyncs, `deviceEpoch`, `driftPpm`, `usingFallbackDevice`). A supervisor thread owns
  every device call. It recovers from device loss, hung streams and default-device changes, and falls back selected → default →
  NullRealtime, returning when the preferred device opens again. Backends: `Default` is the OS API; `NullRealtime` is a silent
  real-time device (CI, headless); `Null` has no device or thread, and `pull(span, now)` runs the callback path deterministically.
- **`ClockSample clockSample()`**, readable from any thread (SeqLock): `{ int64 devicePositionFrames; int64 hostTimeNs (steady);
  Rate negotiatedRate; int64 latencyFrames; LatencyQuality quality; uint32 deviceEpoch; bool running }`. It is the raw device clock
  and knows nothing about timelines.
  - `devicePositionFrames` counts frames handed to the device. The frame being heard at `hostTimeNs` is
    `devicePositionFrames - latencyFrames`.
  - Successive samples lie on a delay-locked-loop line that filters callback jitter and tracks crystal drift (`stats().driftPpm`),
    so interpolating at `negotiatedRate` is smooth.
  - `latencyFrames` covers the endpoint buffer, the resampler delay, the OS stream latency (WASAPI `GetStreamLatency`, or one engine
    period when the driver reports 0) and the user `latencyOffset`.
  - `quality` is `Measured` (reported, or exact on Null), `Estimated`, or `UserCalibrated` (offset ≠ 0).
  - `deviceEpoch` increments on every start, reopen, device switch, fallback and OS reroute.
- **`TransportMap`** (`TransportMap.h`): pure, trivially copyable, RT-safe; no device and no threads.
  - **Model.** Segments map device frames → timeline Ticks with a rational `TransportSpeed{num, den}` (1/1 normal, 0 paused,
    negative reverse) and a loop range.
  - **Commands** (each at a device frame): `play`, `pause`, `setSpeed`, `seek(target, at, prerollFrames)`, `setLoop`,
    `rebase(epoch, oldFrame, newFrame)` and `setClock(ClockSample)`. A seek flushes: frames already in flight report the target. It
    then holds through the preroll, rendering nothing.
  - **Queries.** `timelineAtFrame`/`timelineAtDeviceTicks`; `piece()` (renderer runs that end at changes and loop wraps);
    `transportTime(hostTimeNs) -> Ticks`, which is what video follows; and the inverse `deviceTicksFor`/`hostTimeFor(Ticks)`
    (when a timeline time will be heard).
  - **Arithmetic** is integer only, through `timemath::mulDiv` (128-bit intermediate, explicit rounding). It switches to the
    Time.h `mulDiv` once that lands.
- **Engine transport**: convenience wrappers over the engine's internal TransportMap.
  - The engine **starts paused at Ticks{0}**, at speed 1/1 with no loop, so callers must `resume()`.
  - Calls: `setTimeline(t)` (= `seek(t, 0)`), `seek(t, preroll)`, `pause()`/`resume()`/`isPaused()`, `setSpeed()`/`speed()` and
    `setLoop()`/`loop()`. Commands apply at the next block, or immediately while stopped.
  - `clock()`/`clockAt(tp)` is `transportTime()` plus two guarantees: never past the rendered audio, and never backwards in device
    time (only an explicit seek or reverse speed moves the timeline back). A pending seek reports its target at once.
  - `transportMap()` returns a snapshot with the clock installed, for schedulers.
- **Latency calibration**: `latencyOffset` (config or `setLatencyOffset`) adds the output latency the OS does not report (Bluetooth,
  external DACs, routing apps). The clock compensates for it, and `quality` becomes `UserCalibrated`.
- `AudioSource` **consumes prepared PCM rings only**: `render(span<float>, channels, Rate, Ticks startTime) noexcept`.
  `setSource` is a lock-free command handoff, and the old source is reclaimed off the callback (deferred reclamation). Building
  blocks: `SpscFrameRing`, `SeqLock`, `RingBufferSource`, `SineSource`/`SilenceSource`, and `OfflineRenderer` (deterministic
  N-frame pull for export).
- Mixer utilities (`Mixer`, `MixerSource`):
  - dB↔linear;
  - constant-power pan (−3 dB) for mono buses, and a **unity-centre balance** for stereo buses;
  - mute/solo (mute wins), with click-free gain ramps;
  - a soft clip guard;
  - per-bus and master peak/RMS meters via atomics.
- **DSP** (`dsp/`): `Eq3`, `Compressor`, `Limiter` (+ `TruePeakDetector`), `Denoiser` (RNNoise), `LoudnessMeter` (libebur128) and
  `TimeStretch` (WSOLA). **Effects** (`effects/EffectChain.h`): processors for the model's audio effect ids and the chain that runs
  an `EffectStack`, evaluating curves on a fixed 32-frame grid counted from the render origin (gain ramps per sample), so the output
  does not depend on block boundaries. `bind()` (non-RT) shares processors between chains; `process()` is RT-safe.
- `Mixer::endBlock(MasterInsert*)`: the master effect stack runs on the summed buses, before the master fader, meters and soft clip.
- Shuttle policy: 1× renders through `render()`; forward 1/4×..4× through `AudioSource::renderAtSpeed` (pitch-preserving stretch,
  `isAudibleShuttle`), heard only if the source implements it; reverse, 8×, preroll and pause are silent while the transport
  advances. A speed change is a de-click seam.

### `plughost` — owner: Audio engine (A.6, [plugins.md](plugins.md))
- **`wabcut-plughost`**: the helper process that loads CLAP and VST3 plugins, one per plugin binary. It is crash-isolated: a
  plugin that crashes disables itself, never the editor. It also scans plugin files (`--scan`) and opens plugin editors.
- **`wabcut_plughost_common`**: the wire. `Protocol.h` defines the shared-memory block slots and the events; `Ipc.h` the
  processes, shared memory and base64.
- **`wabcut_plugins`**: the editor side.
  - `PluginCatalog`: the scanner, the cache, the blocklist, and the `EffectDescriptorProvider` for the model.
  - `PluginHost` / `PluginInstance`: the helper processes and the shared memory.
  - `PluginEffects`: the `audio::ExternalEffectFactory`, with its fixed-block, latency-compensated processor and an instance
    pool.
  - `PluginSystem`: the per-process setup the app and wabcut-cli start.
- Windows only for now (`WABCUT_WITH_AUDIO_PLUGINS`, vcpkg feature `audio-plugins`).

### `script` — owner: Core engine (C.12, [extensions.md](extensions.md))
Extensions: JavaScript over the typed command protocol, one sandboxed process each.
- **`wabcut_script`** (pure C++, core builds too) is the editor's side.
  - `Manifest` / `Extension`: the manifest and the folder, both untrusted and bounded, with a content hash.
  - `Permissions`: grants within what an extension asks for, revoking, per-project trust bound to the content hash,
    and auto-disabling after three crashes.
  - `Storage` and `Events`: bounded per-extension storage, and throttled editor events.
  - `Protocol`: the NDJSON wire. Every helper line is checked.
  - `ExtensionHost`: one helper.
    - It spawns the helper through `ai::ChildProcess`, in a Windows job object with a memory cap, no child processes and
      UI restrictions.
    - It runs the helper's requests through a `CommandEngine` limited to the grants (`allowModelCommands`,
      `allowFileCommands = false`, `undoLabelPrefix`).
    - It cancels calls past their deadline, then kills the helper.
    - It never blocks the GUI thread: a watcher thread and a writer thread move the lines, and `pump()` handles them.
  - `ExtensionManager`: the user's and the project's extensions, and the palette.
  - `runHeadless`: what `wabcut-cli ext run` uses.
- **`wabcut-scripthost`** (Qt Qml): QJSEngine on the main thread, with only the `wabcut` API and `console`. Every event
  the main thread handles runs under a watchdog that interrupts the engine past the call's time limit.
- **Command hooks.** Every model-running command carries `CommandSpec::runsModel`, and every command on the files its
  parameters name carries `touchesFiles` (classified by tests/script/test_runs_model.cpp). `EnginePolicy` can hold
  either back.

### `render` — owner: Graphics/playback (Qt RHI)
- `FrameImporter` (per QRhi instance and device): `FrameHandle` → RHI textures (plane views), with a texture pool. CPU planes are
  uploaded; `D3D11Texture` payloads on the QRhi's own device are wrapped in place (cached R8/RG8 or R16/RG16 texture-array views
  per surface slice, `docs/zero-copy.md`), surfaces of another device are rejected. It keeps every source frame until the GPU is
  past it and retires resources after GPU completion. Preview and export have separate pools.
- `shareDeviceWithDecoders(QRhi*)` (`ZeroCopy.h`): hands a D3D11 QRhi's device to media's d3d11va decoders (a no-op elsewhere).
- `Compositor(QRhi*)`: `render(cb, renderTarget, const PreparedComposite&)`. Zero-copy layers use a second pipeline
  (`sampler2DArray`, uv scaled to the picture inside the padded surface); the colour math is shared.
- **Effects and transitions (I.2)**: a layer with stack effects (`CompositeLayer::effects`, evaluated `LayerEffect`s) or a
  transition group (`CompositeLayer::transition`: A then B, or one side) is rendered first into canvas-size RGBA16F
  intermediates (layer with Motion/Opacity → one or more `shaders/fx.frag` passes per effect in stack order → the mix), and the
  result is blended "over" the canvas in its place; other layers draw directly as before. All in linear premultiplied light.
  - Effects: Crop (fractions of the layer's own frame through the inverse Motion transform, feather in px), Gaussian Blur
    (separable, sigma = radius / 3 in sequence px, divided by the preview divisor; wide blurs sample coarser mips), Basic
    Correction (exposure stops, contrast around a 0.18 pivot, saturation, temperature/tint as white-balance gains), Vignette
    (in the layer's frame), Sharpen (unsharp mask, radius = sigma), Drop Shadow (alpha offset by angle/distance, softened like
    the blur, layer over shadow). Transitions: cross dissolve, dip to colour, wipe (angle, feather).
  - `tests/render/EffectsReference.cpp` is the CPU twin of every pass (golden-image tests); change both together.
  - 0.2, Premiere's library ([effects.md](effects.md)): every video transition is one pass of `shaders/fx_transition.frag`
    (CPU twin `render/reference/Transitions.h`); the library's video effects are `EffectType::Library` layer effects
    (`render/EffectLibrary.h`: op, parameter order, family) run by family shaders (`fx_color.frag`, ...) that share the fx
    pipeline's vertex shader, uniform block (p0..p7) and bindings (`Compositor::fxFamilyPipeline`); CPU twins
    `render/reference/LibraryEffects.h`. `LayerEffect::matte` is a library effect's second picture (rendered like a
    Track Matte's composite, bound as tex1): Echo's echoes, the text of Timecode / Clip Name / Simple Text, Gradient
    Wipe's and Difference Matte's other track, Image Matte Key's image - the evaluator builds them (hidden groups with
    `LayerSpec::matteEffect`, or pictures from `playback::EffectContext`); Posterize Time is the evaluator holding the
    clip's time, not a pass.
  - `CompositeStats`: effects applied, bypassed (counted from the layers), transitions, offscreen passes.
- **Titles (I.2)**: `TitleRenderer` (`Title.h`) draws a `TitleStyle` (text, font, size, fill, outer stroke, alignment, line
  spacing, tracking) into a frame-size premultiplied sRGB RGBA image: QTextLayout shaping (HarfBuzz, font fallback, Thai
  clusters), glyph-run outlines stroked then filled, cached per style (one raster = one resident texture). Deterministic for
  preview and export.
- **`ColorPipeline`** (versioned, part of sequence settings and every request): source range/matrix → transfer/primaries →
  **scene-linear working space (Rec.709 primaries, linear)** with **premultiplied alpha**. All blending happens in linear RGBA16F.
  There are separate display and export output transforms and an OCIO hook point. Missing metadata uses a defined SDR fallback (BT.709 limited);
  HDR is detected and flagged. The MVP implements and tests SDR only.
- `OffscreenRenderer`: a headless QRhi rendering the same `PreparedComposite`, with backpressured async readback for export/tests.
- The backend is logged and shown (adapter + API). The policy is D3D11 first on Windows, Metal on macOS, and Vulkan → OpenGL on Linux, with an override.
  Device loss bumps the device generation and invalidates imports.

### `playback` — owner: Graphics/playback
- `Scheduler` publishes immutable **`PreparedComposite { revision, seekEpoch, targetTime, canvas, ColorPipeline, layers[{FrameHandle,
  transform, opacity, blend, effects, transition}] }`** into a **bounded mailbox**. The render thread takes the newest ready one without blocking. Stale
  epochs are discarded. If the next frame isn't ready the policy is **hold last frame** (black only after a seek with nothing decoded).
- Decoding goes through a `media::DecoderPool` (the scheduler's own, or one passed in `SchedulerOptions::pool`): one in-flight
  request per clip source, the playhead's target at Preview priority and lookahead at Prefetch, stale requests cancelled by the
  pool's per-client epoch. `DecodeBackend::Sessions` keeps the private-thread path for comparison. `SchedulerOptions::zeroCopy`
  requests native frames, caps what the sources hold and sizes the decoders' extra surfaces for it.
- Evaluation (`Evaluate.h`, shared with export) follows the model's evaluator obligations: frames at `Clip::sourceTimeAt`
  (speed, reverse, freeze; past the clip for transition handles), every parameter at `Clip::curveTimeAt` (curves in media time),
  the stack in order (disabled skipped, `isEvaluable()` failures bypassed and counted), transitions as A/B groups with progress
  over [cut − before, cut + after), title clips (C.4 generator media) as `TitleStyle` layers that decode nothing.
  `buildCompositeLayers()` turns evaluated layers and their frames into composite layers; the scheduler and export both use it,
  so preview == export.
- `PlaybackController`: `setSession`, `setSequence`, `play/pause`, `setTransportSpeed(Rational)` (J/K/L ladder ±1/2/4/8×, slow
  ±1/4× for K-held), `stepFrames(int)`, `seek(Ticks)`, `position()`, and stats (frame age, late/dropped/duplicated frames, active
  decode + render backends). It follows `TransportMap::transportTime()` plus display-latency compensation.
- P.2 (monitors, [monitors.md](monitors.md)): `setLoop(range)` is Premiere's Loop: the transport wraps inside the range
  (`TransportClock::setLoop`: the audio engine's TransportMap, or the free-run clock), the scheduler decodes no further
  than its end and drops what it queued past it at the wrap; it survives a transport swap. `addMirror(controller)` makes
  another controller (a second output: full screen, transmit) follow this one's session, sequence, transport, loop and
  epochs while it decodes and composes on its own; a mirror never commands the transport.
- Reverse/fast shuttle: seek to a valid decode entry, decode forward into a **bounded** timestamped window, present backward,
  and prefetch the previous window. Byte/frame caps fall back to lower preview resolution or reduced cadence. At 8× only the needed samples are decoded.
  A window's frames step by the source-time step between the targets (`FrameSource::targetStep`, at least the stream's frame
  period; past 0.5 s the target is asked for alone), so a 240 fps source in a 60 fps sequence delivers and caches only the
  frames that will be shown.
- Preview settings (`Scheduler::setPreview(PreviewSettings{useProxies, decodeDivisor, proxies})`, any thread, effective next
  pass): `resolveSource()` picks a media item's proxy or original (an offline original plays from its proxy), frames are
  box-downscaled after decoding to the preview resolution, and `SchedulerStats` report what the latest picture was made of
  (proxy/original layers, decoded resolution) and late frames per media. See "Proxies and preview quality" below.

- Audio side (owner: Audio engine; `src/playback/src/audio/README.md`): `TimelineAudioPlayer` (feeder thread → per-track SPSC rings →
  callback mix → master bus), `TrackRenderer` (clip and track effects, transitions, reversed clips, through-edit voices, pre-roll at
  restarts), `OfflineMixdown` (the same mix offline: export and the loudness jobs; preview == export from the same start).

### `editor` — owner: Graphics/playback (I.1)
Editor services that close the loop, as plain C++ over the engine modules (`src/editor`, `wabcut_editor`); the app's bridges
only adapt them to QML. One undo step per user action throughout.
- `MediaImport`: `importMediaFiles(session, files)` probes and fingerprints files into `MediaItem`s with a `MediaLocator` (absolute +
  project-relative path); `classifyImportFile` routes timelines (.xml/.fcpxml/.otio/.edl/.prproj, by content) to
  `importTimelineFile`, which runs `interchange::ImportRouter` into the open project and returns the `ImportReport`.
- `SourceEdit`: `dropMedia(session, {media, sequence, at, track, Insert|Overwrite})`: video on the track under the pointer (else the
  targeted video track), audio from the audio track under the pointer (else the targeted ones), linked, via `edit::insertEdit` /
  `overwriteEdit`.
- `ProjectDocument`: new/open/save/save as through `ProjectIO` (atomic, `.bakN`), dirty = the snapshot is not the one last saved
  (`isSameRevision`), autosave with rotated `.autosave`, `.autosave2`, ... and recovery, and offline media: open checks every file
  (absolute path, then project-relative, fingerprint) and marks `MediaLocator::offline`; relinking searches folders by name and
  checks the fingerprint.
- `PlaybackSession`: `AudioEngine` (master clock) + `TimelineAudioPlayer` fed by `MediaAudioFeed` + video `PlaybackController` on
  `makeTimelineAudioTransport()` (commands go to the player so its rings flush on seeks; time comes from the engine).
- `Export`: the seam behind File > Export: `setExporter()` registers an implementation (`exportSequence()` reports
  Unsupported without one); `makeExportJobExporter(videoSource)` is M6's `ExportJob` behind it.
- `ProxyManager`, `ProxyQueue`, `ResourceGovernor` (I.3): the proxy registry, the background proxy queue and the foreground
  activity it yields to. `CacheInspector`: disk use, limits and cleaning of the regenerable caches. See "Proxies and preview
  quality" below.

### Captions, recovery dashboard, project capsule (I.4)
- **Captions are drawn by the compositor** (`render::CaptionRenderer`, `Caption.h`): the cue under the playhead of the
  chosen caption track (`playback::CaptionSettings`; else the first enabled one) is the top layer of
  `evaluateVideoLayers`, so the monitor (`Scheduler::setCaptions`) and an export with "Burn in captions"
  (`ExportSettings::captions`, through the rendered source) show the same pixels. Text is shaped like titles (Thai OK),
  wrapped to 80 % of the width, stacked in the title-safe area at the bottom or top over an optional box (or with an
  outline). Karaoke highlights the words already spoken: transcript word times when they line up with the cue's words
  (`CaptionWordTimings`), else the cue's time shared by word length (`captionWordSpans`, Unicode word boundaries, so
  Thai splits too). The model has no caption style yet: the app maps the Captions panel's style
  (`CaptionStyleMapping.h`) - the seam for a model style; since C.7 the style is in the model (`CaptionTrack::style`, see
  docs/internal/c7-caption-handoff.md for the switch). Otherwise exports write `<output>.srt` / `.vtt` sidecars
  (`CaptionSidecar.h`), times relative to the exported range.
- **Recovery dashboard** (`editor::recoveryEntries`, File > Recover…): the project as saved now, its autosaves and its
  backups, and untitled autosaves, each summarised (sequences, clips, duration, last edit, what differs from the saved
  version) with a poster frame rendered by the export renderer (`renderPoster`); a version opens as an untitled copy, so
  the project file is only replaced by an explicit save.
- **Project capsule** (`editor::collectProject`, File > Collect Files…): the project and what it uses in one folder -
  media whole, or trimmed to the used ranges + handles (`exporter::trimmedCopy`: stream copies of whole GOPs; clip In
  points and keyframe/effect curves move by the measured shift, so trimmed clips stay frame-exact), proxies, fonts of
  titles and captions (Windows' font registry; fonts that ship with Windows are not copied), analysis sidecars - with
  locators relative to the project, `manifest.json` (paths, sizes, fingerprints, version), a free-space check first, a
  `.wabcut-partial-*` staging folder (cancel leaves nothing) and a verification that reopens a copy elsewhere with the
  absolute paths cleared.

### Proxies and preview quality (I.3)
Progressive media readiness: imported clips play immediately (at a reduced preview resolution if need be) while proxies are made
in the background for what the machine cannot play in real time.
- **Proxy files** (`exporter::transcodeProxy`, `src/export/Proxy.cpp`, through the export encoder layer): ProRes Proxy
  (`prores_ks` profile 0, the default), DNxHR LB, or all-intra H.264 (NVENC when it works, else libx264), at 1/2 or 1/4 size,
  audio streams copied, in `<project> Proxies/` next to the project (`<cache>/Untitled Proxies` while untitled). Proxy frame n
  covers exactly the source time of source frame n relative to the same media origin (VFR included); colour tags, pixel aspect
  (absorbing size rounding), rotation, HDR metadata and the start timecode carry over. A `.partial` file is checked (first-frame
  time, frame count) and renamed. Sources with alpha are not proxied (no alpha in these codecs).
- **Registry, not undo**: attaching a proxy is not an edit. A background completion must never become an undo step or land
  inside a gesture, so `editor::ProxyManager` keeps the registry and feeds the scheduler a `playback::ProxyTable`; a
  `ProjectDocument` save filter writes it into `MediaLocator::proxy` (provenance: generator, settings, source fingerprint,
  generation) on every save and autosave, and `proxies.json` in the proxy folder records each proxy so it is found again after
  a quit without saving. A proxy whose `provenance.source` no longer matches the locator's fingerprint (e.g. relinked to a
  re-render) is **stale**: it leaves the preview, is not saved, and is regenerated (generation + 1; the old file is deleted).
- **Policy** Off / Auto / Always. Auto proxies media larger than UHD 4K, H.264 above 8-bit or other than 4:2:0 unless a trial
  open shows the GPU decodes it, long-GOP HEVC 10-bit without hardware decoding on a weak machine (< 8 cores or > 1080p), and
  media that dropped frames in playback (`SchedulerStats::lateFramesByMedia`). A.10 (gaming captures; thresholds are constants
  in `ProxyManager.h`): a high frame rate (>= 100 fps: every frame is decoded even when only every fourth is shown) above
  1080p, or without hardware decoding; UHD 4K long-GOP media (H.264, HEVC, AV1, VP9, ...) without hardware decoding. Length
  alone is never a reason: a seek in a long recording costs what it costs in a short one with the same GOP, P.1 measured a
  proxy scrubbing no faster than a hardware-decoded original, and a 2-hour 1440p60 VOD's proxy takes about half an hour and
  is larger than the recording. Proxies keep the source's frames and timestamps, so a 240 fps proxy keeps every frame.
- **Queue**: one worker, most urgent first (under the playhead, on the sequence, selected in the library, the rest); a more
  urgent job preempts a less urgent one under 75 %; between frames the job waits while `ResourceGovernor` reports playback, a
  timeline gesture (`EditSession::inGesture()`), scrubbing or an export. A long recording (>= 30 min, `ProxyJob::longSource`)
  waits behind shorter media of the same urgency and is preempted only under 2 %: the transcode cannot resume, so a
  preemption restarts it from the first frame.
- **Preview quality** Full / 1/2 / 1/4 / Auto (the coarsest resolution that still fills the monitor's pixels) plus "Use
  proxies": the decoders downscale to it (FFmpeg has no reduced-resolution decoding for H.264/HEVC/ProRes) and the program
  monitor composites at it. Frames smaller than their stream are placed exactly where the original would be
  (`transformForFrameHeight`). The monitor badge reports what the picture is made of ("Proxy 1/2", "1/2", "Full").
- **Export always decodes originals** (`ExportJob` resolves them and fails, naming the file, when one is offline, even when its
  proxy exists).
- **Caches**: proxies, thumbnails, waveform peaks and analysis results, with per-cache limits (least recently written files go
  first, never a proxy the project uses) and "clean regenerable data" (Settings > Media & cache).

### `export` — owner: Export
- `ExportSettings` / presets as before. `ExportJob` evaluates a **fixed snapshot** at rational output PTS, with backpressured
  readback→encode queues, deterministic audio sample counts and priming/trailing policy, cancellation, and a temp file → atomic finalize.
  It defaults to original media (not proxies) and reports missing originals. A native encoder-surface path is reserved for later.
- `transcodeProxy()` (`Proxy.h`) makes proxy files through the same encoder layer (I.3; see "Proxies and preview quality").

### `keymap` — owner: Keymap (pure C++)
- `CommandId` (stable dotted strings, mapped to Premiere command names), `Scope`, `Platform`, and `KeyChord{key, mods}`, where key values equal
  Qt::Key numerically (the Qt adapter static_asserts this). Portable serialized names are separate from numeric values.
- `KeymapManager::resolve(Scope, KeyChord)` (focused scope first, then Application), `chordsFor`, `conflicts`, and user override layers.
  Tables stay **separate per OS**. On macOS, Qt reports ⌘ as ControlModifier: leave `AA_MacDontSwapCtrlAndMeta` off and never swap again.
- **`TransportGesture`** (pure state machine, separate from the resolver): consumes press/release with an autorepeat flag and a focus-lost
  reset, and emits typed transport intents (`ShuttleStep`, `SetSpeed(Rational)`, `StepFrames(n)`, `Stop`). It defines K-held J/L slow
  play, single steps, the ladder, simultaneous releases, and **ignores OS autorepeat** for the ladder. Trace tests cover all of this.
- Routing rules (the app implements them, the keymap documents them): text entry, IME composition, NumberField editing, modal dialogs and popups
  suppress editor shortcuts (via ShortcutOverride). Bindings follow logical keys; keypad Enter/Delete and AltGr are handled explicitly.

### `app` — owners: UI/UX (QML shell, theme, components); feature owners for their bridge types
- QML module URI `Wabcut`, in library target `wabcut_ui`. Tokens live only in `qml/theme/Theme.qml` (see `docs/design-system.md`).
- All user-visible strings use `qsTr()`. Command IDs are never translated. Theme, accent and reduce-motion are persisted.
- Bridges in `src/app/src/bridge/`: `ProjectController`, `TimelineView` (QQuickItem; updates **only** items in a `ChangeSet`),
  `ProgramMonitor` (QQuickRhiItem), `KeymapDispatcher` (owns `TransportGesture`, focus scopes, ShortcutOverride), and over
  `wabcut_editor` the singletons `ProjectFile` (`ProjectLifecycle`: file lifecycle, import, relink, library drops) and `Playback`
  (`PlaybackBridge`: transport, playhead follow/scrub, stats, source view). Dialogs live in `qml/dialogs/`.
- The window uses `QWindow::startSystemMove/startSystemResize`, with a native-decoration fallback setting. The monitor's framebuffer
  follows the effective DPR; project canvas dimensions are separate.

## Repository layout
```
src/<module>/include/wabcut/<module>/*.h   public headers (namespace wabcut::<module>; model types in wabcut)
src/<module>/src/*.cpp                     private implementation
tests/<module>/                            Catch2 suite via wabcut_add_test_suite(<module> ...)
src/app/qml/{theme,components,panels}/     QML
tools/                                     dev scripts (dev-env.ps1, gen_test_media.py)
docs/                                      architecture, design system, keymap, team process
```

## Decision log
- **Rev 2, 2026-10-11: independent review (Codex) adopted.**
  - P0: FrameHandle with a tagged payload; a PreparedComposite mailbox with seek epochs; checked time arithmetic and TimeMap;
    ClockSample + TransportMap; snapshot granularity + ChangeSet + gestures; a TransportGesture state machine; a ColorPipeline with linear
    premultiplied compositing.
  - Also adopted now: decoder pool, generation-aware cache keys, sequence extent + transition edges, keyboard routing rules, backend policy,
    `qsTr`, atomic/rotated saves, MediaLocator.
  - Scheduled for wave 2/3: perf harness on the real GPU (3 distinct 1080p60 streams, latency percentiles), CI for all three OSes,
    core-only configure, proxies/relink UI.
  - Kept as decided: the stack, integer ticks, implicit gaps, snapshot undo, per-OS key tables.
