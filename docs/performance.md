# Performance

"Excellent performance" is a product promise: editors notice latency before anything else. This page is how Wabcut
measures it (P.1): the benchmark suite, the machine the numbers come from, the results, the budgets, and how to profile.

## The benchmark suite

`tests/perf` (`wabcut_perf_tests`, CTest label `perf`, `gpu-perf` for the cases that need the GPU). Every benchmark
writes `build/<preset>/test-output/perf/<benchmark>.json` and checks itself against this machine's baseline.

| Benchmark | What it measures | How |
|---|---|---|
| `startup` | Cold and warm start to an interactive window; with a 1-hour, 10,000-clip project as the last project (and its 5 backups and 3 autosaves); and opening that project from the command line | `wabcut.exe` launched as a user would, `StartupProbe` (`src/app/src/StartupProbe.h`) writing milestones and quitting at the first frame. Each launch keeps its settings, app data and QML cache in a folder of its own |
| `project` | Serialize, parse, save (with backup rotation) and load of the 1-hour timeline and of C.10's 10,080-clip project; the editor document's open, save and autosave; version-history checkpoints; move, trim, ripple trim and undo through the command engine; opening a project whose 200 camera files have proxies (the proxy manager's refresh) | In process, below the UI |
| `playback` | Time to first picture and first sound after play (cold: a new pipeline; warm: decoders open), J/K/L response, frame step, scrub latency (60 drag positions at 60 Hz), on H.264 1080p, HEVC 4K, ProRes 422 1080p and the 4K clip's ProRes Proxy | `editor::PlaybackSession` as the app builds it, the Null audio engine pulled in real time, a stand-in monitor compositing every published picture on the GPU |
| `timeline` | Drag, trim and ripple on the 10,000-clip timeline: each mouse move (preview), the release (commit and its fan-out to the shell), the frame drawn after it, undo | The real QML shell offscreen, a rendered `TimelineView` |
| `memory` | Working set, private bytes and GPU memory: empty shell, the 1-hour project open, after 1,800 scripted edits (30 minutes at a brisk pace), after closing it | The real shell, its `ProjectFile` open path |
| `export` | Frames per second of 1080p and 4K exports in H.264 and HEVC, on NVENC and on x264 / x265 | `ExportJob` on the rendered timeline, AAC audio |
| `analysis` | Shot detection, search indexing and queries, the transcription pipeline | The CLI's analysis code; search and transcription through their helper protocols with stand-in helpers (no model) |

The media is generated once (`tests/perf/gen_perf_bench_media.py`, CTest fixture `perf.generate_media`): 12-second
camera-like clips with grain-like noise and a tone, H.264 High 1080p30 (GOP 30, 20 Mb/s, AAC), HEVC Main 4K30
(40 Mb/s, AAC), ProRes 422 1080p30 (PCM), and the 4K clip's half-resolution ProRes Proxy, as the app makes proxies.

### Run it

On a quiet machine (no builds, no other tests; the JSON records what else ran):

```powershell
. .\tools\dev-env.ps1
ctest --preset win-msvc -L perf -R "^Benchmark" --output-on-failure
```

One benchmark: `build\win-msvc\bin\wabcut_perf_tests.exe "Benchmark playback*"`. The other `[perf]` tests of the
other suites (frame budgets, latency ceilings) run with `-L perf` alone.

### Regressions, baselines and conditions

- Each metric is a value with a unit and a direction (lower or higher is better). Gated metrics are compared with
  `tests/perf/baselines/<machine id>.json`: a metric that costs more than 20 % more than its baseline, and more than
  its absolute slack (0.5 ms for times, 5 MB for memory), is a regression.
- A regression is measured once more before it fails (a busy moment is not a regression); each metric keeps its
  better attempt.
- Each attempt records its conditions: CPU busy %, free memory, compilers and other tests running.
- Another machine has no baseline: its run passes and records one (`test-output/perf/baseline-candidate.json`).
  `WABCUT_PERF_UPDATE_BASELINE=1` writes it into `tests/perf/baselines/`.
- Budgets (below) are reported by every run; `WABCUT_PERF_ENFORCE_BUDGETS=1` fails on an exceeded one.

## Profiling

`wabcut_perf_sampler` (`tests/perf/tools/sampler.cpp`) is a sampling profiler that needs nothing installed:

```powershell
build\win-msvc\bin\wabcut_perf_sampler.exe --hz 1000 --report report.txt --folded stacks.txt -- `
    build\win-msvc\bin\wabcut_perf_tests.exe "Benchmark project*"
build\win-msvc\bin\wabcut_perf_sampler.exe --threads all --seconds 10 --pid <pid>
```

- It suspends the sampled threads only to copy their registers and stack, then unwinds the copy with the modules'
  own unwind tables, so the program runs at nearly full speed (a 1.8 s checkpoint measured 2.0 s under it).
- Symbols come from the PDBs next to the binaries, resolved after the run, so it also works on a program that has
  exited.
- The report lists the hottest functions on the CPU (exclusive and inclusive) and where threads wait. A sample counts
  as on the CPU when the thread used about 50 µs of a core since the previous one.
- `--folded` writes collapsed stacks for any flame-graph tool.

Windows' own `wpr` records ETW traces too, but analysing them needs WPA, which is not installed on the build machines.

## The machine

Machine `4dc96eaa790eeafc` (the id hashes the CPU, its logical cores, the memory size and the GPU): Intel Core
i5-12600K (16 logical cores), 32 GB, NVIDIA GeForce RTX 3060, Windows 11 (10.0.26200), the `win-msvc` preset
(RelWithDebInfo, MSVC 19.51). The numbers are from a quiet window (no builds, no other tests: each run records it) on
2026-10-12: three passes of every benchmark on the binaries of 402fb4d, before the P.1 fixes.

- The stored baseline gates each metric on its **worst** pass: a regression is 20 % (and the slack) beyond the slowest
  quiet run. Passes on the same binaries differ by up to 25 % on single-sample and p95 metrics (a first checkpoint, a
  cold first frame, the p95 of 30 commits), so one pass as the gate would fail on noise.
- The tables show the **median** of the three passes: the representative number.

## Results

Before (median of three quiet passes, 402fb4d) and after (the seven fixes below, median of three passes in a second
quiet window; the structural items P-1 and P-2 are measured with the next build). Interactive = the
window's first frame after the first idle moment, offscreen. The full set (132 gated metrics, each with its three passes)
is `tests/perf/baselines/4dc96eaa790eeafc.json`; every run's own numbers are in `test-output/perf/*.json`.

| Area | Metric | Before | After | Budget |
|---|---|---|---|---|
| Startup | warm, to an interactive window | 852 ms | 890 ms | < 2 s |
| | cold (fresh caches and settings; the OS file cache stays warm) | 826 ms | 933 ms | < 3.5 s |
| | warm, a 10,000-clip project as the last one (not opened) | 1,943 ms | **954 ms** | < 2.5 s |
| | opening that project from the command line | 2,445 ms | **1,125 ms** | < 4 s |
| Project (1 hour, 10,000 clips) | save (with backups) | 353 ms | **225 ms** | < 1 s |
| | load | 140 ms | 130 ms | < 1 s |
| | the editor's Save / Open | 309 / 135 ms | **206** / 140 ms | < 1 / 1.5 s |
| | autosave (on the GUI thread) | 220 ms | **0.01 ms** (window #3; the write on a worker) | < 50 ms |
| | history checkpoint, first / next (C.10's project) | 1,170 / 743 ms | **1,009 / 632 ms** | — / < 1 s |
| | move / ripple trim through the command engine (p95) | 5.5 / 6.8 ms | 5.0 / 5.1 ms | < 16 ms |
| | open a project whose 200 camera files have proxies (proxy refresh) | 23,755 ms* | **407 ms** | — |
| Timeline (10,000 clips, real shell) | drag preview per mouse move (p95) | 0.44 ms | 0.55 ms† | < 8 ms |
| | drag / ripple release, model and UI updated (p95) | 9.1 / 10.2 ms | 11.4 / 14.2 ms† | < 50 ms |
| | the frame drawn after it (p95) | 1.1 / 2.0 ms | 1.2 / 2.2 ms† | < 8 ms |
| | undo (p95) | 7.3 / 9.5 ms | 11.5 / 11.1 ms† | < 50 ms |
| Memory | 1,800 scripted edits on the 1-hour project: growth | 176 MB | **94 MB** | < 500 MB |
| | kept after closing it | 198 MB | **53 MB** | < 50 MB |
| Playback | play to the first picture, cold (H.264 / HEVC 4K / ProRes / proxy) | 53 / 66 / 34 / 18 ms | 57 / **38** / 35 / 19 ms | < 500 ms |
| | play to the first picture, warm | 0.3 ms | 0.3 - 0.4 ms | < 150 ms |
| | play to the first sound | 106 ms (all) | **57 / 16 / 12 / 16 ms** | < 200 ms |
| | L (play) to moving pictures | 34 - 36 ms | 34 - 35 ms | < 150 ms |
| | frame step (p95) | 0.8 / 5.2 / 16 / 7.7 ms | 1.1 / **2.3** / 17 / 7.6 ms | < 50 ms |
| | scrub: the picture following the drag (p95) | 115 / 151 / 645 / 168 ms | 116 / 155 / 646 / 235 ms; window #3: **99 / 124 / 634 / 135 ms** | < 100 / — / — / 50 ms |
| Export (4 s 1080p, 2 s 4K) | H.264 1080p NVENC / x264 | 90 / 43 fps | 87 / 37 fps† | >= 120 / 30 fps |
| | HEVC 4K NVENC / x265 | 25 / 4.6 fps | 25 / 4.9 fps | >= 30 / — fps |
| Analysis | shot detection | 18x realtime | 19x realtime | >= 4x |
| | search query over 1,000 shots (p95) | 7.3 ms | 7.7 ms | < 50 ms |

Bold: a fix's effect. After = the median of three passes in the second quiet window (2026-10-12, 19:50-20:03) on the
binaries with the seven fixes below; the full per-metric table is in the P.1 report.

\* Before: measured back to back on a busy machine (six compilers running), not in the quiet window (the benchmark was
added after it); the same build measured 524 ms after the fix that way.

† The second window ran about 10-15 % slower than the first on code no fix touches: x264 export (pure CPU) 0.85x, the
timeline's preview steps (no commit, no snapshot) 1.2-2x, scrub and step p50 1.2-1.6x. The third window (machine back
to the first window's speed) puts the preview steps back at 0.9-1.1x but confirms the release and undo rows (see
"Hotspots fixed").

## Hotspots fixed

Found with the benchmarks and the sampler, fixed one commit each with a test (before: the quiet-window baseline;
after: the second window).

| # | Fix | What it was | Before | After |
|---|---|---|---|---|
| 1 | SnapshotReleaser (model; ProjectController, the MCP server) | EditSession queued every snapshot it let go (evicted undo steps, gesture and slider intermediates, the previous project) for a releasePending() the app never called | 198 MB kept after closing the 1-hour project; 176 MB growth over 1,800 edits | 53 MB kept; 94 MB growth |
| 2 | saveProject without re-parsing the old file (model) | each save loaded the file it replaced in full, only to decide whether it may rotate into the backups | 353 ms (1-hour save); 309 ms the editor's Save | 225 ms; 206 ms |
| 3 | History object folders once per checkpoint (model) | create_directories() per object: 30 % of a checkpoint | 1,170 / 743 ms | 1,009 / 632 ms |
| 4 | Recovery scan: lazy, on a worker (editor, app) | every start parsed the last project's autosave and 5 backups on the GUI thread | 1,943 ms start with that project; 2,445 ms opening it | 954 ms; 1,125 ms |
| 5 | The paused picture keeps its decoder (playback) | the still's sessions were reaped after 3 s, so the next step or scrub reopened the file | frame step HEVC 4K p95 5.2 ms (local files open fast: a reopen hardly shows elsewhere) | 2.3 ms |
| 6 | A seek's preroll ends when the tracks are refilled (audio) | every play from a new position held 100 ms of silence (and the picture with it) whatever the feeder did | 106 ms to the first sound (all four formats); HEVC 4K first picture 66 ms | 12 - 57 ms; 38 ms |
| 7 | Proxy refresh in linear time (editor) | O(media²) file opens comparing each proxy with every original | 23,755 ms (busy machine) | 524 ms (same conditions); 407 ms in the quiet window |
| 8 | P-1: the autosave's file work on a worker (editor, app) | the timer wrote the whole project and rotated the copies on the GUI thread, every minute | 220 ms on the GUI thread (the whole autosave) | 0.01 ms on the GUI thread (the 222 ms write on a worker) |
| 9 | P-2: a layer's jumps seek its own idle decoder (media, playback) | a scrub's backward and far jumps each opened a new decoder session (stream info, hardware probe, first decode) while under the cap | scrub: the picture following the drag, p95 115 / 151 / 645 / 168 ms (H.264 / HEVC 4K / ProRes / proxy) | 99 / 124 / 634 / 135 ms; one decoder session per layer after the scrub |

Fixes 8 and 9 are measured in the third quiet window (2026-10-13, 02:12-02:26, median of three passes), on the final
tree (with the wave-5 fixes and master 724ad5a merged); the x264 control ran at 0.98x of the first window, so the
machine matched it.

**The third window, on the final tree, confirms two slower areas** against the first window's baseline:
- timeline release and undo: medians 8-26 % above (drag / ripple / trim commit 6.98 / 8.88 / 6.63 ms), p95 up to 46 %
  (ripple commit 14.6 ms, ripple undo 14.0 ms): well inside the 50 ms budget, but real. Window #2's binaries (the seven
  fixes only, no other merges) showed it too, so a fix is the likely cause: the releaser's frees running alongside
  the GUI thread's allocations (heap calls were about 11 % of a gesture's CPU in that profile) is the first suspect;
  none of the fixes shows on the main thread by name. A/B profiling of it is the first item of the next perf work.
- startup warm and cold about 20 % slower (1,043 / 994 ms, still half the budget), with the start from a 10,000-clip
  last project 1,026 ms (fix 4 holds): the merged features (C.13, AI-14, I.9) load more QML at startup.
The stored baseline (`tests/perf/baselines/4dc96eaa790eeafc.json`) is re-recorded from this window, so the gate
compares with the final tree from now on; the first window's numbers stay in this document and in git (1d82e0d). P-3 (one diff per commit; diffSequence is about 4 % of a release on the main thread) is not done:
the plan is in the P.1 report.

Hotspots found in other owners' code, routed (the sampler's profile of the timeline benchmark, main thread):
- Every commit fans out to bridges that refresh in full on revisionChanged: MarkersBridge 16 %, TranscriptBridge 14 %,
  ClipAccessModel::rebuildIndex (rehashes every clip) 14 %, pruneSelection (a set of every clip) 11.5 % of a commit's
  ~9 ms; they could check the ChangeSet first.
- TimelineView::buildTrack elides every visible clip label through HarfBuzz on every rebuild (about half of a
  scene-graph update); a cache keyed by (text, width) would remove most of it.
- Startup is 85 % QML loading (qmlLoaded at ~0.75 s of 0.85 s): lazy panels (M5n).
- Export decodes 4K HEVC in software; NVENC 1080p export runs at 90 fps, short of its 120 fps budget, with the encoder
  far from busy.

## Budgets

`tests/perf/budgets.json` holds the product targets, each with its reason. The main ones:

| Budget | Target |
|---|---|
| Warm start to an interactive window | < 2 s |
| Start with a 10,000-clip last project | < 2.5 s (it is not opened, so it must not cost) |
| Play pressed to the first picture, warm | < 150 ms |
| Play pressed to sound | < 200 ms before the device's own latency |
| Scrub on proxies, the picture following the drag (p95) | < 50 ms |
| Frame step (p95) | < 50 ms |
| A timeline edit on 10,000 clips (command, p95) | < 16 ms (one 60 Hz frame) |
| Drag preview per mouse move (p95) | < 8 ms |
| Release to an updated timeline (p95) | < 50 ms |
| What the autosave timer holds the GUI thread for, 10,000 clips (P-1: the write runs on a worker) | < 50 ms (the whole autosave < 1 s) |
| Memory kept after closing a project | < 50 MB |
| H.264 1080p export on NVENC | ≥ 120 fps (4x realtime at 30 fps) |
| Shot detection | ≥ 4x realtime |
| Search query (p95) | < 50 ms |

## Long VODs and high frame rates (A.10)

Gaming footage is long (a 2-hour stream VOD with game, mic and voice-chat tracks) or fast (240 fps for slow motion).
`tests/perf/bench_longvod.cpp` measures what such a recording costs, through the objects the app uses: opening it,
the first picture, scrubbing anywhere in it, playing it, its waveforms and its memory.

### The recordings

| File | Written like | Video | Audio | Size |
|---|---|---|---|---|
| `longvod_h264_1440p60_2h.mkv` | OBS (Matroska) | 2 hours of 2560x1440 60 fps H.264 High 8-bit: `h264_nvenc` preset p5, CBR 24 Mb/s, GOP 120 (2 s), 2 B-frames (OBS's NVENC defaults; libx264 veryfast with the same GOP and bitrate without NVENC) | 3 AAC-LC 48 kHz stereo 160 kb/s tracks titled Game (noise, tones, loud bursts), Mic (speech-like bursts with pauses), Discord (a quieter second voice) | about 22 GB |
| `highfps_hevc_1080p240_30m.mp4` | ShadowPlay | 30 minutes of 1920x1080 240 fps HEVC Main 8-bit: `hevc_nvenc`, CBR 50 Mb/s, GOP 480 (2 s), no B-frames, `hvc1` (libx265 ultrafast without NVENC) | 2 untitled AAC tracks (game, mic: the MP4 muxer's default handler names) | about 11 GB |

The pictures are the P.1 media's camera-like pattern (test pattern, grain-like noise, a hue drift).
`tests/perf/gen_longvod_media.py` encodes the video once, as a 60 s chunk of closed 2 s GOPs, and repeats it to the full
length with FFmpeg's concat demuxer and a stream copy; each concat entry carries the chunk's exact duration, so the
timestamps run on without a gap at the joints (a keyframe every 2 s, every frame decodes). The audio tracks are encoded
at full length instead (repeated AAC chunks would repeat the encoder's priming frame at every joint), one FFmpeg per
track in parallel, while the chunk encodes. `manifest.json` next to the recordings holds each one's recipe hash, the
video encoder used, its length and size; a recording that is up to date is skipped, and each is written as
`<name>.partial.<ext>` and renamed when complete.

Generate them once (the script, or CTest test `perf.generate_longvod_media` when the build is configured with
`-DWABCUT_PERF_LONGVOD_FIXTURE=ON`; labels `perf;longvod`, up to 2 hours):

```powershell
. .\tools\dev-env.ps1
python tests\perf\gen_longvod_media.py --out build\win-msvc\test-output\perf-longvod-media
ctest --preset win-msvc -L longvod --output-on-failure    # the same, with the option on
```

The generator is not a test by default, so no `ctest` run (`-L perf`, `-LE perf`) writes 33 GB by accident, and no
test requires the recordings: the long-VOD cases skip while they are missing. The script also takes `--only <file>`,
`--chunk-seconds N` (a multiple of 2) and `--minutes N` for short versions (the cases use positions relative to the
length; the recipe hash changes, so the full length is generated again afterwards).

### Run the cases

The cases are Catch2 tests, discovered with one CTest label each (`perf`, or `gpu-perf` for those that need the
GPU), so they are selected by name, on a quiet machine:

```powershell
ctest --preset win-msvc -R "^Benchmark longvod" --output-on-failure
build\win-msvc\bin\wabcut_perf_tests.exe "[longvod]"
```

| Case | What it measures | How |
|---|---|---|
| `longvod.open` | Probe; the app's first import: New Sequence from Clip, the import, the drop on the timeline (V1 and one audio track per stream) | `editor::describeMediaFile`, `edit::addSequence`, `editor::importMediaFiles`, `editor::dropMedia`, 5 runs |
| `longvod.first_frame` | A decoder's first picture at the start and in the middle; the program monitor's first picture of a new pipeline at the start, and after a jump into the middle right after opening | `media::VideoDecoder`; bench_playback's pipeline (`editor::PlaybackSession`, the Null audio engine pulled in real time, a stand-in monitor compositing every picture on the GPU) |
| `longvod.scrub` | 60 jumps to random frames of the whole recording (a fixed seed), a 60 Hz playhead drag in the middle, 30 frame steps; the frames the codec decodes per jump | `scrub()` / `stepFrames()` through `PoolFrameSource` on the scheduler's `DecoderPool`, as bench_playback |
| `longvod.play` | 20 s of real-time playback from the middle: dropped frames (abandoned by the scheduler or superseded before display), late frames, frames delivered and decoded per second, CPU time per second | The 2-hour recording in a 1440p60 sequence, the 240 fps one in a 1080p60 sequence (the preview runs at the sequence rate) |
| `longvod.peaks` | Waveform peaks of every track, cold and warm: one stream at a time (first progress, first partial result, the result) and as the app requests them (first visible, complete, all done); how late the GUI thread's 5 ms timer fires while they build | `media::WaveformPeaksService`; `app::MediaPeakSource` asked by the test thread standing in for the GUI thread |
| `longvod.memory` | Private bytes, working set and GPU memory: before, opened with its first picture, after 20 s of play, with every track's peaks, after closing everything | The pipeline above |

Each writes `test-output/perf/longvod.<case>.json`, whose `details.definition` says exactly what each number
includes, and adds its gated metrics to `baseline-candidate.json` like the P.1 benchmarks (record the baseline from
the quiet window with `WABCUT_PERF_UPDATE_BASELINE=1`). The frame counts have an absolute slack of 3 frames, CPU time
50 ms/s and the GUI timer 5 ms on top of the 20 %. The cold peaks take the longest: each track of the 2-hour recording
is decoded twice (alone, then as the app requests them), six passes over 22 GB.

### Results

Machine `4dc96eaa790eeafc` (see "The machine"), median of three quiet passes. **Not measured yet**: the v0.1 feature
freeze (2026-10-13) came before a quiet window for the 33 GB recordings, so every `LONGVOD-...` below is still to be
measured (the benchmarks, the generator and the fixes they were written for are in the tree; the fixes are covered by
their unit tests). Delete the recordings when done (`build\win-msvc\test-output\perf-longvod-media`, about 33 GB).

| Area | Metric | 2 hours, 1440p60 H.264, 3 tracks | 30 minutes, 1080p240 HEVC, 2 tracks | Budget |
|---|---|---|---|---|
| Open | probe (p50) | LONGVOD-PROBE-2H | LONGVOD-PROBE-240 | - |
| | the file chosen to an editable sequence holding it (p50) | LONGVOD-OPEN-2H | LONGVOD-OPEN-240 | < 2 s |
| First frame | decoder: the first picture / one in the middle (p50) | LONGVOD-DECODER-2H | LONGVOD-DECODER-240 | - |
| | monitor: the first picture after opening (p50) | LONGVOD-FIRST-2H | LONGVOD-FIRST-240 | < 500 ms |
| | monitor: a jump into the middle right after opening (p50) | LONGVOD-JUMP-2H | LONGVOD-JUMP-240 | < 500 ms |
| Scrub | a jump anywhere to its picture (p50 / p95) | LONGVOD-SEEK-2H | LONGVOD-SEEK-240 | p95 < 250 ms |
| | frames decoded per jump | LONGVOD-SEEKFRAMES-2H | LONGVOD-SEEKFRAMES-240 | - |
| | a drag in the middle: the picture following it (p95) | LONGVOD-DRAG-2H | LONGVOD-DRAG-240 | - |
| | frame step (p95) | LONGVOD-STEP-2H | LONGVOD-STEP-240 | < 50 ms |
| Play | dropped / late frames in 20 s | LONGVOD-DROPPED-2H | LONGVOD-DROPPED-240 | 0 dropped |
| | frames decoded per frame delivered | LONGVOD-DECODED-2H | LONGVOD-DECODED-240 | - |
| | CPU time (ms per second) | LONGVOD-CPU-2H | LONGVOD-CPU-240 | - |
| Peaks | one stream alone, cold: the slowest stream / its first progress | LONGVOD-PEAKS-STREAM-2H | LONGVOD-PEAKS-STREAM-240 | - |
| | as the app requests them, cold: first visible / every track complete | LONGVOD-PEAKS-APP-2H | LONGVOD-PEAKS-APP-240 | first visible < 1 s |
| | warm (the disk cache): every track | LONGVOD-PEAKS-WARM-2H | LONGVOD-PEAKS-WARM-240 | - |
| | the GUI thread's 5 ms timer while they build: latest tick | LONGVOD-GUI-2H | LONGVOD-GUI-240 | < 16 ms |
| | peaks held in memory, every track | LONGVOD-PEAKS-MB-2H | LONGVOD-PEAKS-MB-240 | - |
| Memory | opening / 20 s of play / every track's peaks (private bytes) | LONGVOD-MEM-2H | LONGVOD-MEM-240 | - |
| | kept after closing it | LONGVOD-RETAINED-2H | LONGVOD-RETAINED-240 | < 50 MB |

### Budgets

In `tests/perf/budgets.json` under `longvod.<case>/...`, reported by every run (`WABCUT_PERF_ENFORCE_BUDGETS=1` fails
on an exceeded one):

| Budget | Target |
|---|---|
| The file chosen to an editable sequence holding it (`open_p50_ms`) | < 2 s |
| The first picture after opening, and after a jump into the middle (`first_frame_p50_ms`, `jump_frame_p50_ms`) | < 500 ms, as for any clip |
| A jump anywhere in the recording to its picture (`seek_p95_ms`) | < 250 ms (the 240 fps recording decodes up to 479 frames from the keyframe) |
| Frame step (`frame_step_p95_ms`) | < 50 ms, as on short clips |
| Dropped frames in 20 s of play (`dropped_frames`) | 0, also for the 240 fps source in a 60 fps sequence |
| The first waveform visible (`peaks_first_visible_ms`) | < 1 s: progressive, not after the whole stream decoded |
| The GUI thread's timer while peaks build (`gui_timer_late_max_ms`) | < 16 ms: never a missed 60 Hz frame |
| Memory kept after closing (`retained_after_close_mb`) | < 50 MB |

## Speed audio (A.10)

Slow motion, speed ramps and reverse play their audio (`audio/dsp/SpeedRender.h`, playback's `TrackRenderer.h`
"Speed"): varispeed (band-limited resampling, pitch moves) or, with Maintain Audio Pitch, a positional WSOLA stretch
(30 ms window, +-7.5 ms search, a resync every second so a seek replays at most a second).

### Cost per voice

`tests/audio/test_speed_render.cpp` "what one stretched voice costs" (`[perf]`), 48 kHz stereo, 10 s of output in
480-frame blocks, RelWithDebInfo on the machine below:

| Renderer | 0.5x | 2x |
|---|---|---|
| Varispeed | 4.2 | 15.4 |
| Maintain pitch (WSOLA 30 ms) | 7.7 | 9.6 |

(ms of CPU per second of output, 10 ms = 1 % of a core; measured 2026-10-13 during the freeze, not in a quiet window, so an
upper bound. Varispeed at 2x pays for its anti-aliasing kernel, twice as wide as at 1x. The stretch keeps a click's energy
centre within 5.8 ms of its position at 0.5x and 6.9 ms at 2x, inside its 7.5 ms search; varispeed within a frame.)

### WSOLA or Signalsmith Stretch

The spec asked to keep WSOLA (our streaming one, made positional) or bring in Signalsmith Stretch 1.1.0 (MIT,
header-only) if it measures clearly better. A scratch comparison (not in the tree: mono, constant speeds, both
stretchers fed the same material; the Signalsmith output aligned by the lag that minimises the distance) measured:

- **lsd**: log-spectral distance in dB between the output's STFT magnitude (2048-point Hann, 50 Hz-16 kHz, an
  -80 dB floor) and the source's at the mapped time. Lower is better.
- **transients**: drum hits (noise bursts with an 80 Hz body every 0.3 s), the energy from 5 ms before to 20 ms after
  each expected onset over the energy within +-40 ms. Higher is better.
- **artifacts**: a 440 + 659.3 Hz chord, the energy away from the partials relative to the partials. Lower is better.

| Material | Speed | lsd WSOLA 20 / 30 / 40 / 60 ms | lsd Signalsmith default / cheaper | lsd varispeed (pitch moves) |
|---|---|---|---|---|
| Speech (Windows TTS, 13.7 s) | 0.5x | 5.56 / **5.22** / 5.49 / 6.42 | 6.33 / 8.60 | 17.29 |
| | 0.75x | **3.75** / 4.34 / 5.07 / 6.65 | 4.66 / 5.58 | 11.95 |
| | 1.5x | **4.88** / **4.88** / 5.18 / 6.16 | 5.72 / 6.95 | 15.92 |
| | 2x | **6.28** / 6.43 / 6.63 / 6.85 | 7.42 / 8.62 | 22.84 |
| Music: Windows Ring05 (sustained chords) | 0.5x | 9.68 / 9.35 / 9.17 / 8.58 | **6.13** / 6.90 | 23.31 |
| | 2x | 11.57 / 11.22 / 11.02 / 10.82 | **6.88** / 7.08 | 14.41 |
| Music: Windows Alarm05 | 0.5x | 7.07 / 6.32 / 6.18 / 5.80 | **5.17** / 8.12 | 12.95 |
| | 2x | 8.58 / 8.21 / 7.25 / 7.95 | **6.21** / 6.85 | 13.66 |
| Music: a synthetic band (piano chords, bass, drums) | 0.5x | 5.45 / 3.99 / **3.59** / 4.73 | 7.05 / 9.18 | 12.49 |
| | 2x | 7.40 / 6.60 / **6.54** / 7.21 | 9.12 / 9.84 | 11.79 |

| Drums and chord | Speed | transients WSOLA 20 / 30 / 40 / 60 ms | Signalsmith default | artifacts WSOLA 30 ms | Signalsmith default |
|---|---|---|---|---|---|
| | 0.5x | **0.68** / 0.51 / 0.52 / 0.42 | 0.66 | -37.4 dB | -39.4 dB |
| | 0.75x | 0.68 / 0.75 / **0.82** / 0.75 | 0.72 | -34.9 dB | -39.6 dB |
| | 1.5x | 0.86 / 0.85 / **0.87** / 0.68 | 0.79 | -37.1 dB | -39.6 dB |
| | 2x | **0.88** / 0.83 / 0.84 / 0.55 | 0.84 | -39.5 dB | -39.5 dB |

CPU (mono, ms per second of output): WSOLA 30 ms 1.5-13, Signalsmith default 4.6-18.

**Decision: WSOLA, 30 ms.** Signalsmith is clearly better only on sustained polyphonic music (2-4 dB lsd on the
ringtones) and a little cleaner on steady chords; WSOLA is better on speech at every speed (the mic and voice chat of
a gaming video, what slow motion mostly carries), better on the synthetic band and on transients from 0.75x up,
cheaper, has no latency to compensate, and needs no new dependency. Its determinism (a seek replays at most a second
and yields the same samples) would also have to be rebuilt around Signalsmith's spectral state. 30 ms is the best
compromise of the windows (20 ms keeps 0.5x transients sharper, 40 ms suits sustained music a little better).
Signalsmith stays the candidate for a music-quality mode if users ask for one.