# Playback: timeline audio

The audio half of `wabcut_playback` (owner: Audio engine). The video Scheduler / PlaybackController
(owner: Graphics/playback) will live beside it in `include/wabcut/playback/` and `src/`; nothing here
needs to change for that.

| Header (`include/wabcut/playback/audio/`) | What |
|---|---|
| `AudioFeed.h` | The boundary to media: `read(MediaId, streamOrdinal, Ticks sourceTime, span<float>, channels, Rate)`, called on feeder threads, never on the callback. |
| `MediaAudioFeed.h` | The production feed: real files through `media::AudioDecoder`, an LRU of decoders, relink-aware. |
| `SyntheticAudioFeed.h` | A deterministic feed (sine, ramp, noise, constant, silence per media/stream; optional slow reads) for tests and demos. |
| `TrackRenderer.h` | Stateful, sample-accurate render of one track: source (reversed clips backwards), Volume/Panner curves, the clip's effect stack, transitions or 2 ms cut fades, then the track's effect stack and the track fader and panner (A.3). Through edits share one effect "voice"; lookahead effects are read ahead (no added latency); restarts pre-roll 0.5 s so effects settle. |
| `TrackAudio.h` | `renderTrack()`: a one-shot TrackRenderer (tests); `continuesSeamlessly()`; `mediaHeardOn()` (the media a track plays, nested ones included). |
| `OfflineMixdown.h` | The player's whole mix offline (TrackRenderer per track, mute/solo, mixer, master bus): export and the loudness jobs. Same samples as preview from the same start. |
| `TimelineAudioPlayer.h` | Plays a sequence's audio tracks through the `AudioEngine`. |
| `src/audio/NestedMix.h` (internal) | C.8: the sound of a nested or multicam clip, TrackRenderer's source for it: the nested tracks' renderers summed, the nested master stage (not for multicam), silence outside the nested sequence. |

## Pipeline

```
EditSession::snapshot()  (once per feeder pass)
   │  feeder thread: a TrackRenderer per audio track (clip + track effects), `leadTime` ahead of the playhead
   ▼
TrackStream per track  =  SpscFrameRing (PCM)  +  SpscQueue (chunk headers: timeline time, flush epoch)
   │  audio callback: read each track at the transport's timeline time + the master latency
   ▼
audio::Mixer (mute / solo / enable, meters)  →  master bus (Sequence::audioEffects, then the master fader)  →  soft clip  →  AudioEngine
                                                 (and every block into the live loudness tap: TimelineAudioPlayer::loudness())
                                                 (shuttle 1/4x..4x: mix at the timeline rate → WSOLA → device)
```

* **Effects.** Clip and track effects run on the feeder thread inside TrackRenderer, ahead of the callback, with
  their parameters (constants or curves) evaluated on a fixed 32-frame grid from the render origin, so the samples
  do not depend on chunking: preview and export (OfflineMixdown) are identical from the same start. An edit to an
  effect refills from the cutover like any edit (track stack and transition changes refill the whole queue). The
  master stack runs in the callback (the mix only exists there): the feeder publishes it as an immutable program
  (stack copy + EffectChain sharing the processors of unchanged instances) through one atomic pointer, and frees
  retired programs once a render-sequence counter shows the callback is done with them.

* **Timeline tags, not positions.** Every chunk carries the timeline time of its first frame. The callback
  asks for time `t` (the engine's TransportMap piece) and the stream serves exactly that: it drops stale data
  behind `t`, waits silently for data that starts slightly later, and reports a transport jump it cannot reach
  so the feeder resyncs. Seeks the player was not told about (someone calling `AudioEngine::setTimeline`)
  therefore still recover.
* **Seek (latest wins).** `seek()` asks the engine to seek with `preroll` and records the engine's seek
  generation. The feeder flushes only once `AudioEngine::appliedSeekGeneration()` shows the seek is live, so a
  callback still at the old position never sees, and discards, the new audio. It then refills from the target
  while the transport holds silent. A newer seek aborts an older fill mid-way.
* **Edits.** On a new snapshot the feeder asks the model what changed (`diff(previous, current)`, whose cost
  follows the change, not the project). Header-only changes (mute/solo/enable) go straight to the mixer. A
  track is refilled only where a changed range overlaps audio already queued for it, from that range's start
  but never earlier than `editCutover` (40 ms) ahead of the playhead. Older audio keeps playing until then and
  the new audio starts exactly there, so an edit is heard within one lead window, with no gap (crossfaded over
  2 ms when fades are on). Edits outside the queued audio cost nothing; track-list and media changes fall back to
  comparing every track.
* **Loops** are emulated by the feeder exactly as `TransportMap` wraps (same frame, same overshoot), so the
  audio and the clock wrap together.
  - **The queue is counted, not read off the cursor.** Each slot counts the frames written less what the transport
    played. A loop shorter than the lead therefore holds one lead of repeats, not a ring full of them, and a refill
    always finds room.
  - **A loop change refills from where the transport leaves the queued audio**, never later than the cutover:
    - from the old loop's wrap, when the transport now plays straight on past it (the loop was removed or ends later);
    - at once from the playhead, when the transport already wrapped at the new end.

    Leaving or moving a loop near its end therefore plays on without a gap, also when paused (tests: "Changing a loop
    just before its end ...").
* **Speed.** Forward 1/4x..4x is heard: the engine calls `renderAtSpeed`, the source mixes speed x frames of
  timeline per block and WSOLA stretches them to the device block, pitch preserved (preview only). Reverse and 8x
  stay muted (the clock still runs); when 1/1 returns after a muted speed the feeder refills from where the
  transport is.
* **Clicks.** Clip cuts are faded by `renderTrack` (through edits are not). Data↔silence and epoch switches are
  faded inside each `TrackStream`. Transport edges (start, pause, seek, preroll end, loop wrap) are de-clicked
  by the engine (`AudioDeviceConfig::declickTime`).
* **Underruns** (a slow feed) become silence and are counted (`stats().underrunFrames`). A feeder the callback
  has overtaken jumps ahead instead of feeding stale audio.
* **Real-time rules.** The callback path (`TimelineMixSource::render`, `TrackStream::read`, the mixer) only
  uses atomics, `SeqLock::tryLoad`, SPSC queues and preallocated buffers. All mutexes are `rt::CheckedMutex`,
  and `tests/playback/test_rt_audit.cpp` counts allocations and locks inside the callback (zero) across play,
  edits, seeks, loops, mute and shuttle.

## Nested sequences and multicam (C.8, A.5 part 3)

With the project set (`TrackRenderer::setProject`, `OfflineMixdownOptions::project`; the player sets every snapshot), a clip
of a sequence plays that sequence's sound as its source, then its own Volume/Panner, effects and envelope.

* **Nested clip.** The tracks the resolver reads (`nestedTracksRead`, Resolve.h) each render through their own TrackRenderer
  (recursively), summed. Then comes the nested master stage: the master effects, their latency read ahead, then the master
  fader (Premiere's nested Mix track).
* **Multicam clip.** The active angles' tracks, whatever their flags, with no master stage. An angle switch crossfades over
  `angleCrossfade` (10 ms) instead of the cut fades.
* **Time.** Nested time is `sourceTimeAt`: heard at 100 % (a remap in its unity spans), reversed from 1 s pieces of the nested
  mix that each start from a restart. Silent outside [0, duration).
* **Redactions.** They apply inside by media time. `setNesting` hands down the clips on the path, whose exceptions count.
* **Determinism.** A nested mix restarts (and pre-rolls) on any discontinuous read, so preview == export from the same start.
* **Refills.** The player refills a track from the cutover when a sequence it nests changes.

See docs/ai.md "Nested-sequence and multicam audio (A.5 part 3)".

## Real media: MediaAudioFeed

`MediaAudioFeed` (`include/wabcut/playback/audio/MediaAudioFeed.h`) is the production feed:

* It resolves `MediaId` through the session's current snapshot: `MediaItem::locator.path` (proxy-aware later)
  and `MediaInfo::audio[streamOrdinal].index`. Offline or unknown media is an error, which the player renders as
  silence and counts.
* It keeps an LRU of open `media::AudioDecoder`s keyed by (media, stream ordinal, rate, channels). A decoder
  whose item changed its locator (relink, proxy switch) or vanished is closed and reopened on next use.
* All use of `AudioDecoder::read` is in one function (`decodeInto`), so the coming
  `AudioReadResult{framesFromStream, leadingSilence, endOfStream}` change is a one-line edit there.
* It runs on the feeder thread only and refuses to run inside the audio callback.

`tests/playback/test_media_feed.cpp` (suite `playback_media`, which needs the media test clips) plays generated
clips through the player and compares every sample with the decoder's own output.
