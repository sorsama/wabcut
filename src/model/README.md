# wabcut_model

Pure C++23 (no Qt): time, timecode, the timeline data model, the Premiere-style edit engine, undo/redo
and `.wabcut` project files. Everything other modules need is in `include/wabcut/model/`; `src/` is
private. Implements architecture contract revision 2.

| Header | What it gives you |
|---|---|
| `Time.h` | `Ticks` (254,016,000,000 per second), `Rate` (normalized, validated), `TimeRange`; checked math: `checkedAdd/Sub/Mul/Neg`, `mulDiv(v, mul, div, Rounding)` with a 128-bit intermediate, `TimeBase` + `rescale` for stream time bases, checked `framesToTicks/ticksToFrames(…, Rounding)`, `TimeMap` (rational speed, reverse, freeze) |
| `Timecode.h` | `ticksToTimecode` / `parseTimecode`, drop-frame at 29.97/59.94 (`00:01:00;02` = frame 1800) |
| `Ids.h` | `MediaId`, `SequenceId`, `TrackId`, `ClipId`, `LinkGroupId`, `BinId`, `TransitionId`, `IdGenerator` |
| `MediaInfo.h` | probe results (filled by `media::probe`) |
| `MediaLocator.h` | absolute + project-relative path, `SourceFingerprint` (size, mtime, sampled hash), offline flag, proxy with provenance; `fingerprintFile`, `relativeMediaPath`, `resolveMediaPath` |
| `Parameters.h` | clip parameters: stable ids/keys, units, ranges, defaults (`allParameters()`), `ParamValue` (keyframe-ready), `ClipProperties`, `PropertyRole(s)` |
| `Clip.h`, `ClipList.h`, `Track.h`, `Sequence.h` | the timeline: clips sorted and non-overlapping in chunked copy-on-write storage, implicit gaps, transitions as edge objects, explicit sequence duration and work area |
| `ProjectState.h` | the immutable snapshot (media, bins, sequences), `validate()`, `transitionProblem()` |
| `ChangeSet.h` | what changed: per sequence the affected tracks, time ranges, added/removed/modified clips and property roles; `diff(before, after)` |
| `EditOps.h` | timeline edits: insert/overwrite, lift/extract, add edit, (ripple) delete, Q/W, trim, ripple, roll, slip, slide, move, link, marks — each returns an `EditResult` with its `ChangeSet` |
| `ProjectOps.h` | bins, media, sequences, tracks, track toggles, clip attributes, sequence duration and work area |
| `Snap.h` | snap targets and nearest-target search |
| `EditSession.h` | current snapshot (atomic, any thread), `apply`, undo/redo returning `ChangeSet`s, gestures and merge keys, entry + memory budgets, deferred snapshot release, ids that never rewind |
| `ProjectIO.h` | `.wabcut` JSON (schema 2, migrations from 1), atomic saves with rotated `.bak1..5`, read-only loading, recovery candidates, autosave |

## How state and edits work

* A `ProjectState` is immutable once published. Sharing goes project → sequence → track → clip chunk:
  sequences, tracks and media items are `shared_ptr<const T>` nodes, and a track's clips live in
  `ClipList` chunks of about 64. An edit copies only the chunks it touches, so trimming one clip of a
  10,000-clip track copies one chunk and shares the rest with the previous revision.
* Every edit is a pure function `(const ProjectState&, args) -> std::expected<EditResult, Error>`.
  `EditResult` holds the new state, its `ChangeSet`, the `placed`/`created`/`removed` clip ids, the
  playhead Premiere would leave and the clamped delta of a trim.
* `EditSession::apply(label, fn, mergeKey)` commits the result as an undo step; `beginGesture` /
  `endGesture` (or a repeated merge key) fold a drag into one step. Listeners get a `ChangeEvent` with
  the `ChangeSet`; `undo()`/`redo()` return one too, computed by `diff()` from structural sharing.
* Ids never rewind: the session keeps the highest id handed out outside the undo history and raises
  every edit's generator to it. Save `stateForSave()`.
* The undo history is capped by steps (500) and by an estimate of the memory its snapshots keep alive.
  Snapshots the session lets go are queued; call `releasePending()` from a worker so large revisions
  are not torn down on the GUI thread.
* A clip's name and parameters live in a shared copy-on-write `ClipContent` block (keyframes will live
  there too). New and loaded clips that still carry their media's defaults share the media's block, so
  copying clips stays cheap. Read them with `clip.name()` / `clip.properties()`.

## Premiere semantics implemented

* **Track targeting** decides where lift/extract, Mark Clip and Q/W act. **Source patching**
  (`SourcePatch`) decides where insert/overwrite put each source stream; `defaultSourcePatches` is
  Premiere's V1 / A1, A2, … default; patches to locked tracks are skipped. A.10: the audio of a
  multi-track recording (OBS, ShadowPlay, Game Bar) is routed by name instead (`audioPatchesByName`):
  each stream joins the track named for its source (Game, Mic, Discord, ...), takes a free track or
  asks for a new one (`SourcePatch::newTrack`, reported in `EditResult::createdTracks`), so one Mic
  track holds every recording's mic. See docs/architecture.md, "Multi-track recordings".
* **Sync lock** (on by default) is honoured by every ripple: insert, extract, ripple delete, ripple
  trim, Q/W and insert-mode moves shift the edited tracks and every other unlocked sync-locked track,
  cutting through clips there so later material stays in sync. Tracks with sync lock off, and locked
  tracks, never move. Ripple-deleting a gap is refused when a sync-locked track has material in it.
* **Linked clips** (`LinkGroupId`) edit together unless the op is called with `LinkMode::IgnoreLinks`.
  Trims and moves keep clip ids; splitting keeps the id on the left piece; right pieces cut from one
  group at the same time stay linked; a group left with one clip dissolves — except on a locked track,
  where the clip stays untouched in its group until the track is unlocked.
* Trims clamp to available source media, to neighbouring clips and to one frame minimum, and report
  the delta actually applied. A clip edited down to zero length is removed.
* The sequence's explicit duration grows with its content and never shrinks on its own (trailing gaps
  survive ripple removals) until `setSequenceDuration` / `fitSequenceDurationToContent`.
* **Transitions** sit on edit points (A-out/B-in with lengths before and after the cut, or one-sided
  to/from black). There are no transition edit ops yet; other edits drop transitions they invalidate
  and hand a transition to the right-hand piece when its clip is cut.
* **Three-point editing** (`resolveThreePointEdit`): sequence In+Out set the duration, a lone sequence
  Out back-times the edit, otherwise the edit starts at the sequence In or the playhead.
* After insert/overwrite the playhead goes to the end of the new clip; after lift/extract to the In
  mark (marks are cleared); after Q to the edit point.

## Tests

`tests/model/` reads like a Premiere spec: `TimelineFixture.h` builds sequences from ASCII pictures
(`"V1 tgt |Aaaa..B5bb|"`, one character per frame, digits after a clip's letter are its source In) and
renders results back for comparison. `test_perf.cpp` holds the 10,000-clip timing tests (realistic
project < 1 ms per edit, worst case < 2 ms); run the hidden benchmarks with
`wabcut_model_tests "[!benchmark]"`.
