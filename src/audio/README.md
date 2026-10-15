# wabcut_audio

Real-time audio output, mixing building blocks, and the **master clock** that drives A/V sync.
Contract: `docs/architecture.md` → `audio`. No Qt.

| Header | What |
|---|---|
| `AudioSource.h` | The pull interface: `render(span, channels, rate, startTime) noexcept`, called on the audio thread. |
| `AudioEngine.h` | Device output (miniaudio), transport (seek with preroll / pause / resume / speed / loop), master clock, device enumeration, selection and recovery, Null backends. |
| `ClockSample.h` | `ClockSample`: the raw device clock (position, steady host time, rate, latency + `LatencyQuality`, device epoch). |
| `TransportMap.h` | `TransportMap` + `TransportSpeed`: the pure device-time ↔ timeline map (play, pause, seek+preroll, speed, loop, epoch rebase, `transportTime`, inverse). |
| `TimeMath.h` | Saturating wrappers (`mulDivSaturate`, `addSaturate`) over the model's checked `wabcut::mulDiv`, for real-time paths. |
| `SpscRingBuffer.h` | `SpscFrameRing`: wait-free single-producer/single-consumer ring of interleaved float frames (decoder → audio thread). |
| `SeqLock.h` | Single-writer / multi-reader publication of small structs (used for the clock and the transport request). |
| `Gain.h` | dB↔linear, constant-power pan (−3 dB centre), unity-centre stereo balance, soft clip guard. |
| `LevelMeter.h` | Per-channel peak (with release) / RMS meter and clip latch, published through atomics. |
| `Mixer.h` | `Mixer` (N buses: gain, pan, mute/solo, ramps, master gain, soft clip, meters) and `MixerSource`. |
| `Sources.h` | `SilenceSource`, `SineSource` (pure function of time), `RingBufferSource` (reads an `SpscFrameRing`). |
| `OfflineRenderer.h` | Deterministic, device-less pull of a graph for N frames (export, tests). |
| `SpscQueue.h` | `SpscQueue<T>`: wait-free SPSC FIFO of small records (e.g. chunk headers travelling with PCM). |
| `RealTime.h` | The real-time rules, `kCacheLineSize`, and the audit hooks: `rt::ScopedRealtimeSection` (the engine marks its callback and `pull()`), `rt::inRealtimeSection()`, `rt::CheckedMutex` (counts locks taken inside a section). |

## Threads

| Thread | Runs | Rules |
|---|---|---|
| Audio callback (miniaudio), or the caller of `pull()` on the Null backend | `RenderCore::process` → `AudioSource::render` | Never blocks, locks, allocates, logs or throws. Reads parameters with atomics, transport with `SeqLock::tryLoad`, sources through a raw pointer. |
| Device supervisor (one per running engine, Default/NullRealtime) | Every miniaudio context/device call; watchdog every 50 ms | Owns the device so COM init/uninit stay paired on one thread. Reopens on loss/stall, returns to the preferred device, refreshes name/latency after reroutes, frees retired sources. |
| Any control thread (GUI, playback controller) | `setSource`, `setTimeline`, `pause`, `resume`, `start`/`stop`, `selectDevice`, `clock()` | Thread-safe, not RT-safe; `clock()` is lock-free. |

**Source lifetime.** `setSource()` keeps the `shared_ptr` on the control side and gives the audio thread a raw
pointer. The audio thread brackets each block with two `seq_cst` increments of a counter; a replaced source is
released only once the counter shows the audio thread has left the block it may have used it in. Sources are
therefore never destroyed (or freed) on the audio thread. See `RenderCore.cpp`.

## Master clock

`clock()` = source time of the sample being heard now. It is built from two independent pieces:
`clockSample()` (the raw device clock, which knows nothing about timelines) and the engine's internal `TransportMap`
(pure; device time ↔ timeline). `clock()` = `TransportMap::transportTime()` + "never past the rendered audio" +
"never backwards in device time".

* The engine counts device frames. Each callback observes "frame `D − latency` is being heard now", where `D` is
  the first frame of the block and `latency` = device buffer + OS stream latency + user offset (`latency()`).
* A second-order delay-locked loop turns those observations into a smooth line (frame vs steady clock),
  filtering callback jitter and tracking audio-clock drift. Large positive errors (stream start-up burst,
  reroute) re-anchor at once; negative errors re-anchor only if they persist or exceed what was queued
  (underrun), so one late callback does not disturb the clock.
* Readers interpolate that line with `std::chrono::steady_clock`, capped at the frames actually rendered, and
  clamped by a global high-water mark (in device frames), so the clock never runs backwards while playing,
  even across device reopens or latency increases. It holds instead.
* Device frames map to timeline time through the TransportMap's short history of segments (rational speed, loop
  range). **Seek** is a discontinuity: the clock jumps to the target at once and holds until that audio is heard
  (plus an optional preroll, during which nothing is rendered). **Pause/resume/speed** changes are continuous: the
  clock follows the queued audio out and freezes on the last rendered sample, and after resume it stays frozen
  until new audio is heard.
* **Shuttle (MVP policy):** the source is rendered only at 1/1 forward; at 2×, ½×, reverse or 0 the output is muted
  and the clock advances at that speed.
* **Device epochs:** every start/reopen/switch/fallback/OS reroute bumps the epoch; the audio thread rebases the
  TransportMap (the position continues, and so does a paused state or a preroll in progress, with its remaining
  length) and re-anchors the DLL. `stats().driftPpm` reports the tracked drift.
* All published time is integer: the DLL's floating-point state is internal to the estimator; `ClockSample`,
  `TransportMap` and every Ticks/frames/ns conversion use `wabcut::mulDiv` (model/Time.h) with explicit rounding.

The model is validated deterministically on the Null backend (`tests/audio/test_clock.cpp`): exact
interpolation, latency compensation, pause/resume without jumps, seek holds, jitter filtering (±1.5 ms callback
jitter → < 1 ms clock error, no visible steps), stall/underrun recovery and latency increases.

## De-click and seek hand-off

* Every transport edge cuts the waveform: start, pause, seek, the end of a preroll, a loop wrap, a source swap,
  a speed change. The engine de-clicks each one (`AudioDeviceConfig::declickTime`, 2 ms; 0 = bit-exact output).
  New audio fades in, and the last output value decays to zero beneath it, so the output never steps.
* `seek()` returns a generation and `appliedSeekGeneration()` reports when the audio thread has applied it.
  Components that prepare audio for a seek target (the timeline player's feeder) wait for it, so a callback
  still at the old position never sees the new data.

## Device status

The supervisor never reads miniaudio's live device fields: a WASAPI reroute rewrites the device's name and id
on another thread. The device's own notifications ("started" on the worker before its data loop, "rerouted" on
the rerouting thread right after it rewrote them) copy name, id and backend into a fixed-size snapshot published
through a SeqLock. `DeviceIo::readStatus` decodes only that snapshot. Writers take turns on an atomic flag, and
no mutex is involved anywhere near the data callback.

## Backends

* `Default`: the system API via miniaudio (WASAPI / Core Audio / PulseAudio / ALSA / JACK). Falls back from
  the selected device to the system default, then (if `fallbackToNull`) to `NullRealtime`, and moves back
  when the preferred device can be opened again (probed on a side context, so the fallback is never
  interrupted). `deviceError()` says why a fallback is in use.
* `NullRealtime`: miniaudio's null device, a real-time-paced silent device. For CI and headless runs.
* `Null`: no device, no thread; drive it with `pull(buffer, now)`. Same code path as the device callback.

## miniaudio

The vcpkg port is header-only. `src/miniaudio_impl.cpp` compiles the implementation once; the header is a
SYSTEM include of private sources only, and the `MA_NO_*` options are target-wide. Data callbacks are
variable-sized (`noFixedSizedCallback`), so there is no intermediary buffer and the latency maths stays exact.
On WASAPI the buffer latency is the real endpoint buffer size. The stream latency is `GetStreamLatency`, or one
audio-engine period when the driver reports 0.

## DSP and effects

`include/wabcut/audio/dsp/` holds the signal processing, all pure and RT-safe after construction:

| | What | Notes |
|---|---|---|
| `Biquad.h`, `Equalizer.h` | RBJ cookbook shelves, peaking band, high/low-pass and depth-blended notch; the 3-band EQ | double state; flat EQ is bit-exact identity |
| `Compressor.h` | feed-forward, stereo-linked peak compressor | 6 dB soft knee; attack/release as 1 - 1/e time constants on the gain in dB |
| `Limiter.h` | true-peak brick-wall limiter | BS.1770-4 4x interpolation for detection; hold + box-filtered gain so it is fully down when the delayed peak arrives; latency = lookahead + 8 frames |
| `Denoiser.h` | RNNoise 0.2 per channel, dry/wet | 48 kHz only; latency 1440 frames (one buffered frame + RNNoise's two) |
| `Repair.h` | Essential Sound's cut filter (Butterworth, 12-48 dB/oct), DeHum, de-esser, expander (A.7) | off settings are bit-exact identity; the de-esser is exact below its threshold |
| `Space.h` | Essential Sound's reverb (8-line FDN, RT60 and damping) and stereo width (A.7) | the reverb's lines are allocated by its constructor; width never changes L + R |
| `Loudness.h` | libebur128 wrapper | not RT (allocates gating blocks); for the analysis jobs |
| `TimeStretch.h` | streaming WSOLA | preview shuttle only |

`include/wabcut/audio/effects/EffectChain.h` maps the model's audio effect ids onto processors and runs a stack. Parameter
curves are evaluated on a fixed 32-frame grid counted from the render origin, never from block boundaries: each cell sees the
values at its first frame and at the next cell's, gain interpolates per sample between them, the others take the cell's start
values. That is why preview (feeder chunks, device blocks) and export (1024-frame blocks) agree to the sample. Effects with
latency are handled by the caller reading ahead (playback's TrackRenderer and master bus), so the engine's clock never needs an
extra offset.

The Essential Sound record (`wabcut.audio.essentialSound`, A.7) has no processor either: it is data (a clip's type and preset,
for the panel), and chains skip it like a disabled effect.

Enhance Speech (`wabcut.audio.enhanceSpeech`, A.4) has no processor here: it is offline. DeepFilterNet3 renders a take of
the media in `wabcut-embed`, and playback's TrackRenderer plays that take in place of the source, blended by the effect's
Strength, before the clip's Volume and redactions (`playback/audio/AudioTakes.h`; docs/ai.md "Enhance Speech and stem separation").
