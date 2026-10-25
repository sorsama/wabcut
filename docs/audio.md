# Audio: submixes, the clip mixer, Premiere's effects (0.2)

The 0.2 audio round brings Wabcut's audio closer to Premiere Pro 26: submix tracks and sends, the Audio Clip Mixer,
Premiere's audio effects catalog, Essential Sound's auto-tagging, profanity filtering, and a few fixes to the audio UI.
The engine (clock, callback, mixer, effect chain) is described in `src/audio/README.md`, and timeline audio in
`src/playback/src/audio/README.md`. This page covers what the round added. Every operation is a typed command
(`docs/commands.md`), so scripts, the MCP server and the UI do the same thing.

| | Code |
|---|---|
| Submixes and sends | model `SubmixOps.h`, `Track.h` (`output`, `sends`), `Sequence.h` (`submixTracks`); engine `playback/src/audio/SubmixGraph.*`; commands `audio/edit/src/SubmixCommands.cpp`; UI `AudioMixerRouting.cpp`, `qml/audio/{SubmixStrip,OutputMenu,SendSlots}.qml` |
| Audio Clip Mixer | `ClipMixerBridge.*`, `qml/audio/ClipMixerStrip.qml`, the Tracks / Clips tabs of `panels/AudioMixerPanel.qml` |
| Effects | model `src/model/src/EffectsAudio{Amplitude,Filter,Time}.cpp`; DSP `src/audio/src/effects/{Amplitude,Filter,Time}Effects.cpp`, `dsp/Fft.h` |
| Auto-tagging | `audio/edit/SoundClassifier.h`, `SoundTagger` (`AudioAnalysis.h`), `audio.autoTag` |
| Profanity | `ai/edit/Profanity.h`, `captions.*` parameters, `captions.hideProfanity`, `audio.bleepProfanity` |

## Submix tracks and sends

Premiere's Audio Track Mixer routing. A **submix track** is a bus: audio tracks are assigned to it (the track's
**output**) or send a copy of their sound to it (**sends**). The submix runs its own effects, fader, panner, mute and
solo on the sum, then goes to the Mix or to another submix.

- **Model.** Submixes are kept in `Sequence::submixTracks`, apart from the audio tracks (they hold no clips), and are
  labelled `S1`, `S2`, ... in commands. There are at most 32 per sequence. Each one has a channel format: mono, stereo,
  5.1 or adaptive. As in Premiere, the format is fixed once the submix is made. `Track::output` is a submix id; invalid
  means the Mix. `Track::sends` holds up to 5 sends (Premiere's five), each with a submix, Pre-Fader or Post-Fader
  (Post-Fader is the default), Mute, a level in dB that can be keyframed (default 0 dB), and a pan. A submix cannot
  output into itself or into a submix that feeds it (`audioRoutingProblem`), and validation rejects such a project.
  Deleting a submix sends its tracks back to the Mix. The project schema is 17: 16 has no routing, so the migration is
  empty.
- **Mute and solo follow the routing.** A muted track is silent in its sends too. Soloing a submix solos the tracks and
  submixes that feed it. Soloing a track solos the submixes it reaches.
- **Engine.** `SubmixGraph` is shared by the player's audio callback and `OfflineMixdown`, so preview and export route
  the same way. For each block:
  1. Each track's pre-fader audio feeds its pre-fader sends.
  2. The caller applies the track's fader.
  3. The post-fader audio feeds the post-fader sends and the track's output.
  4. The submixes run in processing order: effect chain, fader and panner, then mixer bus (meter, mute, solo) into
     their output.

  A mono submix folds its input to mono (the stereo mean, or ITU-R BS.775 for 5.1) and pans it with the constant-power
  law. Send levels and pans, and submix faders, are evaluated on the parameter grid counted from timeline 0, the same
  grid as the track faders.

  **Delay compensation:** a submix's effect latency is offset by delaying the other paths into the same bus, up to 8192
  frames per path. The total latency is added to the master read-ahead.

  The routing is published as an immutable program through one atomic pointer, so nothing on the audio thread
  allocates or locks; the real-time audit test covers a routed sequence. A sequence without submixes or sends takes the
  old mix path and renders bit-identically.
- **Mixer UI.** The Audio Track Mixer has:
  - one strip per submix after the track strips;
  - **+ Submix** (mono, stereo, 5.1, adaptive);
  - an output menu at the foot of each strip ("Mix", the submixes, "Create Mono/Stereo/5.1/Adaptive Submix");
  - up to five send slots per track strip, each with a level knob, Pre/Post, Mute and Remove.

  Each change is one undo step; a level drag is one step. The **+ Effect** menu of a submix strip adds effects to the
  submix.
- **Commands.**
  - `sequence.addSubmixTrack`;
  - `track.setOutput`;
  - `track.addSend`;
  - `track.setSend` (with `time`, the level and pan become keyframes: send automation);
  - `track.removeSend`;
  - `audio.routing` (read-only);
  - `sequence.removeTrack` deletes a submix;
  - `audio.addEffect`, `track.setVolume`, `track.setPan` and `track.setState` accept a submix (`"S1"`).

## Audio Clip Mixer

Premiere's **Audio Clip Mixer** (Window > Audio Clip Mixer, or the Clips tab of the mixer panel) has one strip per
audio track for the clip under the playhead. The strip shows the clip's Volume as a fader and its Panner as a knob,
with the track's meter, mute and solo. The strips follow the playhead.

- **Paused**, a fader move sets the clip's level. On an animated clip it sets the key at the playhead. A drag is one
  undo step.
- **Write Keyframes** (the keyframe button on a strip): while playing, a held fader or knob writes keyframes into the
  clip. You hear the change at once through the track's live mix override, and when playback stops the pass is
  committed as one undo step for every clip written.

## Premiere's audio effects

The built-in catalog now has Premiere Pro 26's audio effects, under their Premiere names and Effects panel folders. All
DSP is Wabcut's own code; no third-party code was added. Each effect works on mono, stereo and 5.1. Each one has tests:
- silence in gives silence out;
- noise stays bounded;
- latency is reported only by lookahead effects;
- output is identical whatever the block size;
- defaults are identity where Premiere's are.

Most effects are listed in the Effects panel and in the mixer's **+ Effect** menu ("Delay and Echo › Analog Delay").
The exception is Balance: Premiere 26 hides it, so it is filed under "Obsolete Audio Effects". Effects with latency
are compensated like the existing limiter and denoiser (the clip renderer and the master and submix buses read ahead).

Parameter names come from Premiere's own strings. Ranges and defaults were checked against Premiere where a source was
available; the remainder are best guesses, marked "unverified" in the descriptors' descriptions where it matters.

| Folder | Effects (id `wabcut.audio.*`) | Latency at 48 kHz | Notes |
|---|---|---|---|
| Amplitude and Compression | Amplify `amplify`, Channel Mixer `channelMixer`, Channel Volume `channelVolume`, Dynamics `dynamics`, Hard Limiter `hardLimiter`, Multiband Compressor `multibandCompressor`, Single-band Compressor `singleBandCompressor`, Tube-modeled Compressor `tubeModeledCompressor` | Hard Limiter: its lookahead; Multiband: 5 ms with Brickwall on | Multiband: Linkwitz-Riley 24 dB/oct crossovers (120 / 2000 / 10000 Hz) that sum flat within 0.02 dB. Dynamics: gate, compressor, expander, limiter (the limiter has no lookahead). DeEsser is Essential Sound's `deess`. Dynamics Processing (graph-based) is not done |
| Delay and Echo | Analog Delay `analogDelay`, Delay `delay`, Multitap Delay `multitapDelay` | 0 | Analog Delay's modes shape the feedback loop (tape, tube, analog) |
| Filter and EQ | Bandpass, Bass, FFT Filter `fftFilter`, Graphic Equalizer (10 / 20 / 30 Bands) `graphicEq10/20/30`, Highpass, Lowpass, Notch Filter `notchFilter`, Parametric Equalizer `parametricEq`, Scientific Filter `scientificFilter`, Simple Notch Filter `simpleNotch`, Simple Parametric EQ `simpleParametricEq`, Treble | FFT Filter: its FFT size; others 0 | Graphic EQ: constant-Q bands at Premiere's centres (31.25 Hz × 2^(k / bands per octave)), with gains solved so each band's centre lands on its slider. It is IIR, not Audition's FIR. Scientific Filter: Bessel, Butterworth, Chebyshev, Elliptic up to order 32 (Premiere: 66). FFT Filter: a curve of up to 12 points; Window Type is accepted but always sqrt-Hann. Premiere 26 has no "Simple Pass Filter" |
| Modulation | Chorus/Flanger `chorusFlanger`, Flanger `flanger`, Phaser `phaser` | 0 | Flanger is Audition's (Premiere shares it). After a seek the LFOs restart from the pre-roll |
| Noise Reduction/Restoration | Adaptive Noise Reduction `adaptiveNoiseReduction`, Automatic Click Remover `automaticClickRemover`, DeNoise `spectralDenoise`, DeReverb `deReverb` | FFT size; 720; 1024; 2048 frames | Spectral (STFT, sqrt-Hann, 75 % overlap) noise-floor tracking with Wiener gains. DeReverb assumes a 1 s RT60. Adaptive's "High Quality Mode" is not done |
| Reverb | Convolution Reverb `convolutionReverb`, Studio Reverb `studioReverb`, Surround Reverb `surroundReverb` | Convolution: 512; others 0 | Convolution impulses are synthesised by Wabcut (five rooms, seeded, CC0 by construction), partitioned FFT convolution matches direct convolution to 4e-8. Studio and Surround Reverb are an 8-line feedback delay network |
| Special | Distortion, Fill Left with Right, Fill Right with Left, Guitar Suite, Invert, Mastering, Swap Channels, Vocal Enhancer | Mastering: 3 ms | Distortion's two transfer graphs are one control point per polarity. Mastering at defaults is an exact delay. Loudness Radar (a meter) is not done |
| Stereo Imagery | Stereo Expander `stereoExpander` | 0 | 100 = unchanged, 0 = mono, 300 = three times the side signal |
| Time and Pitch | Pitch Shifter `pitchShifter` | 19.3 / 31.3 / 44.4 ms (Low / Medium / High precision) | Correlation-aligned splicing, the same length as the input. Ratio applies only when Semi-tones and Cents are both 0 (the model has no linked parameters) |

Effects existing before this round are unchanged: Essential Sound's EQ, compressor, limiter, Reduce Noise, filter,
DeHum, De-ess, expander, reverb and width.

## Essential Sound auto-tagging

`audio.autoTag` (Essential Sound's **Auto-tag** button) gives each audio clip a type: Dialogue, Music, SFX or Ambience.
It listens to up to 30 s from the middle of the clip, mono at 16 kHz, and classifies it with a local, deterministic
classifier (`SoundClassifier`). Nothing leaves the computer. The features are:
- syllable-rate level modulation;
- the shares of voiced frames, pitch glides and held notes;
- pauses;
- beat regularity;
- spectral flatness;
- crest factor and level spread.

They are combined with weighted soft thresholds. The tag is the clip's Essential Sound record (the panel shows the type
and offers its presets); no effect is added. Clips that already have a type keep it unless `overwrite` is set.
`minConfidence` skips uncertain clips. `dryRun` previews the proposals with their confidence; applying them is one undo
step. Tests run on synthetic speech, music, noise and effects. Premiere's auto-tagging is a trained model and will be
better on hard material: tonal ambience, or speech over music.

## Profanity

Premiere's Speech to Text **Hide profanity**: profane words in captions are shown as their first letter and asterisks
("f\*\*\*").

- **Lists.** English and Thai lists are built in. Each request can add words (`profanityWords`) or allow them
  (`allowedWords`). A trailing `*` matches every word that starts with the entry. English words match whole and ignore
  case. Thai entries match whole transcript words, or with `*` inside unspaced Thai.
- **Captions.** `captions.generate`, `captions.generateFromTranscript` and `captions.updateFromTranscript` take
  `hideProfanity`. `captions.hideProfanity` masks a caption track that already exists. The Captions page has the
  setting and the word lists, kept in the app settings.
- **Sound (Wabcut's addition).** `audio.bleepProfanity` bleeps (1 kHz) or mutes the words in the sound, with
  `padding` (default 0.02 s) on each side. It adds one audio redaction per media item, named "Profanity", so every use of
  the media is covered in preview and export. The picture is untouched, and captions over a bleep read [bleep]. It is
  one undo step; `dryRun` lists the words first.

## Audio UI fixes

- **Waveforms while peaks build.** While a long recording's peaks are decoding, the part of an audio clip they have
  not reached is shaded, with a thin edge at the build's progress. Partial results rebuild only the audio tracks that
  hold clips of that media (`PeakSource::mediaChangedSince`); before, every waveform of the timeline was rebuilt.
- **Waveforms using clip volume.** "Use clip volume" in the timeline's display settings is Premiere's "Show Audio
  Waveforms Using Clip Volume". Waveforms are scaled by each clip's Audio Gain and its Volume, including keyframes, so
  they show what is heard. With logarithmic scaling, the dB scale is applied after the volume. Rectified waveforms
  and logarithmic scaling were already there.
- **Clip volume keys (0.4).** `]` and `[` raise and lower the clip Volume Level (Effect Controls > Volume > Level) by
  1 dB, Shift+`]` and Shift+`[` by 6 dB (Premiere's Increase / Decrease Clip Volume and its default Large Volume
  Adjustment; no Settings row for the step yet). A keyframed level moves every keyframe by the step, so the shape stays;
  the level stays within -144 to +24 dB. They act on the selected clips (a video clip stands for its linked audio), else
  on the clips under the playhead on the targeted tracks (every track when none is targeted); clips on locked tracks are
  left out. Presses on the same clips within a second are one undo step ("Clip Volume"), and a toast says what changed
  ("Volume +1 dB (now -2.0 dB)"). They work while playing; the Library keeps Shift+`]` / Shift+`[` for its thumbnail
  size, and text fields keep the characters. As in Premiere these change the clip's Volume, not its Audio Gain (G).
- **Named source patching.** A multi-track recording patched to the timeline routes its streams by name (Game, Mic,
  Discord, Music, Track n; A.10's `audioPatchesByName`) when the sequence has tracks of those names. If there is none,
  a new track of that name is made. The Source Monitor's patch indicators and the track headers' tooltips show the name
  ("A1 (Game)").
- **Clip effect pre-roll.** When playback or export starts inside a clip, its effects first hear the clip from up to
  their settle time earlier (at most 2 s, `TrackRendererOptions::maxClipPreroll`). A reverb or delay then carries on as
  if playback had run through: no click and no missing tail. Preview and offline mixdown play the same samples.
- **Voice-over take recovery.** When a project opens with takes that a crash, or a quit before the take was kept, left
  in its Voice-over folder, a card offers each one. **Recover** repairs the take and puts it back on its track where it
  was recorded (one undo step). **Discard** stops offering them; the files stay in the folder.

## Known gaps

- Nested sequences mix their audio without their own submix routing: a nested sequence's submixes and sends are
  ignored inside the parent.
- Submixes have no sends of their own; only audio tracks send.
- A stereo, 5.1 or adaptive submix runs at the sequence's channel count. Only mono submixes fold.
- The engine mixes up to 16 submixes; more are silent and counted.
- The mixer's automation modes (Read, Touch, Latch, Write) cover track faders and pans, not send levels. Send automation
  is through `track.setSend` with `time`.
- The Audio Clip Mixer's meters are the track meters, not per-clip meters.
- The default send level (0 dB) and many effect ranges are unverified against Premiere.
- "Show Audio Time Units" (0.2): one switch in the Program Monitor's and the timeline's settings. The playhead stops
  on audio samples (the arrow keys step one), the ruler and the readouts count samples (or milliseconds, the
  sequence's audio display format), the timeline zooms down to single samples, and drags, trims, razor cuts, fades
  and volume keyframes of audio-only material move by samples. Video, and edits at the playhead (Insert, Overwrite,
  Add Edit), stay on the frame that holds the playhead.
