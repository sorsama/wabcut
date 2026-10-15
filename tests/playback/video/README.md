# Video playback tests and the playback benchmark

- `wabcut_playback_video_tests` (CTest label `playback`): sequence evaluation, transports, the composite pool, and the
  Scheduler / PlaybackController end to end over the generated test media. Every presented picture's burnt-in frame
  number is checked against the sequence time it claims to show.
- `wabcut_playback_bench`: the real-GPU perf harness. CTest runs a 2-second smoke (`playback.bench_smoke`) over short
  clips so it keeps working; the numbers below come from running it directly.

## Running the benchmark

```powershell
. .\tools\dev-env.ps1
python tests\playback\video\gen_perf_media.py --ffmpeg build\win-msvc\vcpkg_installed\x64-windows-release\tools\ffmpeg\ffmpeg.exe `
    --out build\win-msvc\test-output\perf-media --set full          # 6 x 1080p60 + 1 x 2160p30, 20 s, ~1.5 min once
build\win-msvc\bin\wabcut_playback_bench.exe --media build\win-msvc\test-output\perf-media --config 3x1080p60 `
    --seconds 20 --out build\win-msvc\test-output\playback\perf-3x1080p60.md
# --config 3x1080p60 | 6x1080p60 | 1x2160p30, --software (no hardware decode), --divisor 2 (half-resolution preview)
# --backend pool (default: media::DecoderPool) | sessions (private decoder threads), --zero-copy (D3D11 decoder
# surfaces sampled in place; needs --backend pool), --effects (blur + basic correction on every layer),
# --no-timestamps (no GPU timestamp queries: real wall-clock times, no "GPU time" row),
# --nest (the layers inside one nested clip), --multicam N (N streams as multicam angles, a cut every 2 s)
```

What runs: N distinct H.264 streams (different animated sources and noise seeds, ~25 Mb/s for 1080p60, ~60 Mb/s for
2160p30, GOP = 1 s, 2 B-frames) on V1..Vn. V1 is full frame; the others are picture-in-picture layers with scale,
position, rotation and opacity (nothing is occlusion-culled). The pipeline is the production one: free-run
TransportClock at 1x → Scheduler (decode through a media::DecoderPool, hardware decode with transfer to CPU, or
decoder surfaces kept on the GPU with `--zero-copy`; lookahead 6) → CompositeMailbox → Compositor on a headless D3D11 QRhi. The benchmark's main thread plays the
program monitor's render thread at a 60 Hz refresh (1 ms timer resolution plus a short spin, standing in for vsync):
each refresh takes the due composite, renders it into a 1920x1080 target and waits for the GPU.

Stages: **decode** = `VideoDecoder::frameAt` per frame on the decoder threads (hardware decode + GPU→CPU transfer);
**upload + composite** = CPU→GPU upload of every layer + layer pass + output pass until the GPU finished (render
thread); **composite only** = the same composite re-rendered with its frames resident; **render-thread refresh** = the
whole refresh; **frame age** = publish → display (how far ahead the scheduler runs); **display offset** = transport
time at display minus the frame's sequence time (A/V offset; more than one frame = late).

## Nested and multicam (I.8), same machine, 20 s runs (2026-10-12)

`--nest` plays the config's layers through one nested clip (at 90 %, so the nested picture is resampled), `--multicam N`
plays the config's first N streams as the angles of a multicam source through multicam clips that cut to the next
angle every 2 s (docs/internal/c8-nested-multicam-handoff.md, "Nested render"). Zero-copy decode, no GPU timestamps (real
wall-clock); the machine was shared with other builds (three compiler processes running), so the plain 3x1080p60 run
is the baseline taken in the same conditions.

| Config | Presented | Decode p50 / p99 ms | Upload+composite p50 / p99 ms | Composite only p50 / p99 ms | Refresh p50 / p99 ms | GPU mem peak |
|---|---|---|---|---|---|---|
| 3x1080p60 (baseline, flat) | 1151 / 1169 (98.5 %) | 0.57 / 1.35 | 0.26 / 0.78 | 0.13 / 0.77 | 0.30 / 22.74 | 486 MiB |
| 3x1080p60 `--nest` (a nested clip holding the 3 layers) | 1160 / 1169 (99.2 %) | 0.59 / 1.27 | 0.27 / 0.74 | 0.14 / 0.72 | 0.31 / 15.56 | 510 MiB |
| 6x1080p60 `--multicam 4` (4 angles, a cut every 2 s) | 1161 / 1169 (99.3 %) | 0.48 / 1.44 | 0.27 / 0.88 | 0.14 / 0.52 | 0.31 / 15.17 | 1553 MiB |

Rerun after merging master 92e8977 (C.11, A.8), the machine again shared with four compiler processes; the flat and
nested runs alternated so they saw the same load:

| Config | Presented | Decode p50 / p99 ms | Upload+composite p50 / p99 ms | Composite only p50 / p99 ms | Refresh p50 / p99 ms | GPU mem peak |
|---|---|---|---|---|---|---|
| 3x1080p60 (flat), 3 runs | 97.5-99.4 % | 0.55 / 1.24-1.99 | 0.26-0.27 / 0.45-1.24 | 0.12-0.13 / 0.61-0.72 | 0.31-0.39 / 1.35-25.87 | 486 MiB |
| 3x1080p60 `--nest`, 3 runs | 98.7-98.9 % | 0.54-0.56 / 1.22-1.43 | 0.27-0.28 / 0.67-0.98 | 0.14-0.15 / 0.74-0.82 | 0.31-0.39 / 18.13-20.20 | 510 MiB |
| 6x1080p60 `--multicam 4` | 1164 / 1169 (99.6 %) | 0.47 / 1.60 | 0.26 / 0.99 | 0.13 / 0.59 | 0.30 / 10.77 | 1553 MiB |

- A nested clip costs one extra canvas pass (the child compositor's) and ~24 MiB of GPU memory for its canvas: composite
  time grows by ~0.01 ms (p50) / ~0.1 ms (p99) at 1080p. The render-thread refresh p99 follows the machine's load, not
  the nest: under the same load the flat timeline's p99 ranged 1.35-25.87 ms and the nested one's 18.13-20.20 ms (the
  composite inside it stays under 1 ms). Frames dropped by those stalls are counted in "Presented" for both alike.
- A multicam clip decodes only its active angle; each angle used keeps its own decoder session (a key of its path), so
  GPU memory grows with the angles visited (zero-copy surfaces), not the decode time.
- Reports: `build/win-msvc/test-output/playback/perf-i8-{base,nested,multicam4}.md`, after the merge
  `perf-i8-post-{base,base2,base3,nested,nested2,nested3,multicam4}.md`.

## Effects (I.2): blur + basic correction on every layer, same machine, 20 s runs (2026-10-11)

`--effects` gives every layer a Gaussian Blur (radius 12 px) and a Basic Correction (exposure, contrast, saturation,
temperature): per layer one layer pass, two blur passes and a correction pass into intermediates, then a blit
("Effect passes per frame"). The first five rows are timed without GPU timestamps (`--no-timestamps`): wall-clock and
presentation are real. The last four were run with GPU timestamps for the "GPU time" column (the GPU's own execution
time per composite), but their wall-clock and presentation columns are distorted by QRhi's coarse timestamp wait (see
the I.1 correction above): use them only for GPU time.

| Config | Layers carry | Presented | Decode p50 / p99 ms | Upload+composite p50 / p99 ms | Refresh p50 / p99 ms | GPU time p50 / p99 ms | Effect passes | CPU (cores) | Upload MB/s | GPU mem peak |
|---|---|---|---|---|---|---|---|---|---|---|
| 3x1080p60 | blur + correction, zero-copy | 1160 / 1169 (99.2 %) | 0.64 / 1.48 | 0.39 / 1.21 | 6.27 / 16.03 | - / - | 12 | 0.18 | 0 | 604 MiB |
| 3x1080p60 | blur + correction, pool (CPU frames) | 1152 / 1169 (98.5 %) | 3.31 / 8.85 | 2.23 / 10.20 | 7.89 / 40.40 | - / - | 12 | 0.43 | 552 | 460 MiB |
| 3x1080p60 | no effects, zero-copy | 1148 / 1169 (98.2 %) | 0.65 / 3.15 | 0.33 / 1.99 | 6.34 / 25.28 | - / - | 0 | 0.19 | 0 | 486 MiB |
| 6x1080p60 | blur + correction, zero-copy | 1160 / 1169 (99.2 %) | 0.95 / 4.49 | 0.76 / 3.04 | 6.54 / 17.23 | - / - | 24 | 0.30 | 0 | 1123 MiB |
| 1x2160p30 | blur + correction, zero-copy | 581 / 585 (99.4 %) | 0.65 / 3.22 | 0.36 / 1.13 | 5.99 / 21.83 | - / - | 4 | 0.06 | 0 | 961 MiB |
| 3x1080p60 | blur + correction, zero-copy, GPU timestamps | 1019 / 1172 (86.9 %) | 0.68 / 16.24 | 15.16 / 16.48 | 40.60 / 133.76 | 4.26 / 7.86 | 12 | 0.20 | 0 | 604 MiB |
| 3x1080p60 | blur + correction, pool, GPU timestamps | 1006 / 1171 (85.9 %) | 3.08 / 7.97 | 15.31 / 16.53 | 49.57 / 170.57 | 3.14 / 4.43 | 12 | 0.28 | 481 | 460 MiB |
| 6x1080p60 | blur + correction, zero-copy, GPU timestamps | 996 / 1172 (85.0 %) | 0.92 / 16.56 | 15.35 / 16.50 | 51.39 / 253.22 | 5.27 / 6.01 | 24 | 0.32 | 0 | 1123 MiB |
| 6x1080p60 | blur + correction, pool, GPU timestamps | 977 / 1171 (83.4 %) | 5.80 / 14.68 | 15.33 / 31.20 | 48.74 / 331.19 | 5.92 / 7.49 | 24 | 0.67 | 934 | 742 MiB |

- **Real time with effects**: 3 x 1080p60 with blur + correction on every layer presents 99.2 % (zero-copy) / 98.5 %
  (pool) of frames; the render-thread refresh p99 is 16.0 ms (zero-copy) against the 16.7 ms frame; the GPU spends
  p50 3.1-4.3 ms / p99 4.4-7.9 ms per composite (12 effect passes). Twice the load (6 x 1080p60, 24 passes) still
  presents 99.2 % with GPU time p99 6.0-7.5 ms; one 2160p30 stream with effects 99.4 %.
- Effects cost GPU memory for intermediates (canvas-size RGBA16F, pooled: ~16 MiB each at 1080p).
- Reports: `build/win-msvc/test-output/playback/perf-<config>-i2-<variant>.md`.

## DecoderPool + zero-copy (I.1), same machine, 20 s runs (2026-10-11)

The scheduler now decodes through `media::DecoderPool` ("pool"); "sessions" is the previous private-thread path (still
selectable with `--backend sessions`), "zero-copy" is the pool with `OutputMode::Native` on the renderer's D3D11 device
(`docs/zero-copy.md`). Two rounds of 6 x 1080p60, each backend back to back, nothing else running:

| Config | Round | Backend (decode) | Presented | Late | Decode p50 / p95 / p99 ms | Upload+composite p50 / p99 ms | Composite only p50 / p99 ms | Refresh p50 / p99 ms | CPU (cores) | Upload MB/s | GPU mem peak |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 6 x 1080p60 | 1 | sessions (cuda x6) | 1147 / 1170 (98.1 %) | 1 | 4.54 / 7.40 / 9.60 | 4.44 / 16.11 | 0.40 / 15.70 | 9.40 / 72.39 | 0.64 | 1098 | 649 MiB |
| 6 x 1080p60 | 1 | pool (cuda x6) | 1102 / 1181 (93.3 %) | 1 | 4.49 / 8.02 / 12.08 | 4.67 / 16.28 | 0.46 / 15.92 | 13.42 / 196.44 | 0.60 | 1045 | 553 MiB |
| 6 x 1080p60 | 1 | **zero-copy** (d3d11va x6) | **1166 / 1170 (99.7 %)** | 0 | **0.74 / 1.30 / 2.89** | **0.60 / 1.60** | 0.63 / 1.24 | 6.16 / 15.91 | **0.21** | **0** | 934 MiB |
| 6 x 1080p60 | 2 | sessions (cuda x6) | 1064 / 1170 (90.9 %) | 1 | 4.93 / 7.99 / 11.64 | 6.81 / 23.87 | 0.55 / 16.27 | 16.90 / 147.79 | 0.75 | 1018 | 649 MiB |
| 6 x 1080p60 | 2 | pool (cuda x6) | 1105 / 1170 (94.4 %) | 1 | 5.11 / 8.81 / 15.94 | 5.48 / 30.70 | 0.44 / 16.10 | 11.68 / 256.55 | 0.72 | 1057 | 553 MiB |
| 6 x 1080p60 | 2 | **zero-copy** (d3d11va x6) | **1149 / 1169 (98.2 %)** | 0 | **0.87 / 2.00 / 8.68** | **0.68 / 6.90** | 0.58 / 4.42 | 6.40 / 22.42 | **0.27** | **0** | 934 MiB |
| 3 x 1080p60 | - | sessions (cuda x3) | 1058 / 1174 (90.1 %) | 1 | 2.97 / 5.84 / 9.80 | 14.95 / 16.35 | 14.88 / 16.28 * | 31.99 / 101.38 | 0.29 | 504 | 390 MiB |
| 3 x 1080p60 | - | pool (cuda x3) | 1064 / 1172 (90.8 %) | 0 | 3.13 / 7.40 / 11.05 | 14.68 / 16.53 | 14.70 / 16.28 * | 28.21 / 132.67 | 0.34 | 508 | 342 MiB |
| 3 x 1080p60 | - | **zero-copy** (d3d11va x3) | 1146 / 1171 (97.9 %) | 0 | **0.63 / 0.96 / 1.38** | 0.69 / 16.08 | 0.52 / 16.06 | 7.06 / 121.25 | **0.14** | **0** | 486 MiB |
| 1 x 2160p30 | - | sessions (cuda x1) | 583 / 585 (99.6 %) | 0 | 4.02 / 19.42 / 28.77 | 15.16 / 16.80 * | 15.14 / 16.49 * | 12.59 / 44.70 | 0.17 | 372 | 465 MiB |
| 1 x 2160p30 | - | pool (cuda x1) | 584 / 585 (99.8 %) | 0 | 4.06 / 18.02 / 23.85 | 14.54 / 16.63 * | 14.66 / 16.31 * | 10.07 / 44.41 | 0.20 | 372 | 417 MiB |
| 1 x 2160p30 | - | **zero-copy** (d3d11va x1) | 585 / 585 (99.9 %) | 0 | **0.64 / 16.16 / 16.73** | 15.34 / 16.54 * | 15.40 / 16.52 * | 15.76 / 44.55 | **0.07** | **0** | 699 MiB |

- **Pool vs sessions: no regression.** Decode times, CPU and upload match within run-to-run noise (the presented
  fraction swings 91–98 % between rounds on *both* CPU-path backends: render-thread stalls, see below).
- **Zero-copy** removes the GPU→CPU transfer and the upload: decode p50 ~4.5–5 ms → ~0.7 ms per frame, upload +
  composite ~4.5–7 ms → ~0.6 ms, CPU ~0.6–0.75 → ~0.2 cores, PCIe upload ~1 GB/s → 0, and the best presented fraction
  in every round. It costs GPU memory: the decoders' surface pools grow by the frames the pipeline may hold
  (+280 MiB at 6 streams). The importer reported 0 uploads and 0 rejected surfaces in every zero-copy run.
- (*) **Correction (I.2): those ~15 ms composites are a measurement artifact, not GPU cost.** The harness asked QRhi for
  GPU timestamps, and on D3D11 reading them back waits for the frame by polling with a sleep, which rounds any frame
  still running at the first poll up to the system timer period (15.6 ms: Windows 11 gives a process without a
  visible window no finer timer, despite `timeBeginPeriod(1)`). Frames done before the first poll (fast GPU clocks,
  tiny composites) showed the true 0.4 ms. Since I.2 `OffscreenOptions::timestamps` is off by default and the
  benchmark has `--no-timestamps`; see the I.2 section for clean numbers.
- Reports: `build/win-msvc/test-output/playback/perf-<config>-i1{b,c,d}-<backend>.md`.

## Re-measure after the M2.1 + M3.1 merges (M4.1), same machine, 20 s runs

Two merge points: after model rev 2 (parameter table, ChangeSet-driven scheduler; decoder backend still d3d11va) and
after media rev 2 (FrameHandle; media's backend policy now puts **cuda** first on NVIDIA because its GPU->CPU readback
is faster). No regression; hardware decode got faster with cuda. The machine was shared with other builds.

| Config | Merge point | Decode | Presented | Late | Decode p50 / p95 / p99 ms | Upload+composite p50 / p99 ms | Composite only p50 / p99 ms | Refresh p50 / p99 ms | Display offset p99 ms | CPU (cores) | GPU mem peak |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 3 x 1080p60 | M2.1 | d3d11va x3 | 1167 / 1169 (99.8 %) | 0 | 5.26 / 12.32 / 16.52 | 2.37 / 5.65 | 0.43 / 2.09 | 2.56 / 6.88 | 0.82 | - | - |
| 3 x 1080p60 | M3.1 (final) | cuda x3 | 1167 / 1169 (99.8 %) | 0 | 3.09 / 5.64 / 10.47 | 2.47 / 6.53 | 0.45 / 1.89 | 2.64 / 7.66 | 0.80 | 0.41 | 390 MiB |
| 3 x 1080p60 (noisy run) | M3.1 | cuda x3 | 1164 / 1169 (99.6 %) | 0 | 3.91 / 6.60 / 11.62 | 2.88 / 7.70 | 0.58 / 2.87 | 3.17 / 17.14 | 9.75 | 0.48 | 390 MiB |
| 6 x 1080p60 | M2.1 | d3d11va x6 | 1167 / 1169 (99.8 %) | 1 | 6.50 / 13.63 / 19.97 | 3.16 / 5.22 | 0.57 / 0.82 | 3.33 / 5.63 | 0.87 | - | - |
| 6 x 1080p60 | M3.1 (final) | cuda x6 | 1165 / 1169 (99.6 %) | 1 | 5.80 / 8.16 / 11.52 | 4.89 / 7.78 | 0.49 / 1.07 | 5.10 / 8.84 | 1.03 | 0.82 | 649 MiB |
| 1 x 2160p30 | M2.1 | d3d11va x1 | 585 / 585 (100 %) | 0 | 14.31 / 27.24 / 45.89 | 6.97 / 20.62 | 3.79 / 8.82 | 2.07 / 15.23 | 0.79 | - | - |
| 1 x 2160p30 | M3.1 (final) | cuda x1 | 584 / 585 (99.9 %) | 0 | 4.52 / 20.55 / 29.73 | 3.67 / 19.44 | 1.15 / 14.92 | 2.61 / 25.96 (33.3 budget) | 6.50 | 0.26 | 465 MiB |

## M4 results (before the merges): NVIDIA GeForce RTX 3060 (driver 617.14), Windows 11, D3D11, 20 s runs (2026-10-11)

The machine was shared with other engineers' builds while measuring (CPU load varied between runs).

| Config | Decode backend | Presented | Late | Abandoned | Decode p50 / p95 / p99 ms | Upload+composite p50 / p95 / p99 ms | Composite only p50 / p99 ms | Refresh p50 / p99 ms | Display offset p50 / p99 ms | CPU (cores) | GPU mem (process) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 3 x 1080p60 | d3d11va x3 | 1165 / 1169 (99.6 %) | 0 | 0 | 6.46 / 14.42 / 20.44 | 4.10 / 6.49 / 7.87 | 1.21 / 3.11 | 4.36 / 12.72 | 0.00 / 4.81 | 0.88 | 279 MiB |
| 3 x 1080p60 (run 1) | d3d11va x3 | 1070 / 1169 (91.5 %) | 10 | 96 | 4.28 / 12.89 / 45.33 (max 322) | 1.96 / 2.80 / 3.45 | 0.34 / 0.89 | 2.05 / 3.99 | 0.01 / 3.57 | 0.65 | 279 MiB |
| 6 x 1080p60 | d3d11va x6 | 1167 / 1169 (99.8 %) | 1 | 0 | 6.75 / 13.84 / 20.06 | 3.39 / 6.34 / 9.10 | 0.61 / 0.97 | 3.60 / 10.84 | 0.00 / 0.96 | 1.18 | 512 MiB |
| 1 x 2160p30 (stretch) | d3d11va x1 | 585 / 585 (100 %) | 0 | 0 | 20.58 / 43.01 / 55.90 | 7.60 / 16.18 / 20.69 | 4.51 / 12.27 | 2.57 / 24.83 | 0.01 / 1.78 | 0.51 | 420 MiB |
| 3 x 1080p60, software decode | software x3 | 1143 / 1169 (97.8 %) | 0 | 0 | 0.13 / 0.24 / 0.49 (frame threads) | 2.57 / 8.87 / 13.95 | 0.34 / 5.85 | 2.87 / 38.48 | 0.00 / 13.71 | 2.05 | 48 MiB |

Budget: 16.67 ms per frame at 60 fps (33.33 ms at 30 fps). Notes:

- **Every hardware-decode configuration meets its render-thread budget at p99** (12.7 ms and 10.8 ms against
  16.67 ms at 60 fps; 24.8 ms against 33.33 ms for 4K30), and presentation follows the clock (display offset p50 ≈ 0,
  p99 < 5 ms). Software decode misses the budget at p99 (38.5 ms refreshes, 26 frames not presented) because its
  decoder threads compete with the render thread for the CPU.
- Run 1 of 3 x 1080p60 hit a single 322 ms decoder stall (other builds on the machine); the scheduler abandoned the
  targets it could no longer show in time instead of showing them late, then recovered. Run 2 is the representative one.
- Per-frame hardware decode latency is mostly the GPU→CPU transfer (~3.2 ms per 1080p frame, see `docs/zero-copy.md`);
  three to six decoders still run comfortably in parallel because each has a full frame period (16.67 ms) per frame.
- 4K30: 20.6 ms decode + transfer per frame fits the 33 ms budget only thanks to the lookahead (frame age ~210 ms).
  "Duplicated refreshes" (585) are expected: a 30 fps sequence shows each frame for two 60 Hz refreshes.
- Software decode uses FFmpeg frame threads, so per-call latency is tiny while 2 cores stay busy.
- GPU memory is the process's DXGI local-segment usage (decoder surfaces of every session + texture pool + canvas).
