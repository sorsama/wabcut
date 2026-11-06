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
  5.1, 7.1 or adaptive. As in Premiere, the format is fixed once the submix is made. `Track::output` is a submix id; invalid
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
  - an output menu at the foot of each strip ("Mix", the submixes, "Create Mono/Stereo/5.1/7.1/Adaptive Submix");
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
DSP is Wabcut's own code; no third-party code was added. Each effect works on mono, stereo, 5.1 and 7.1 (0.7). Each one has tests:
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
| Amplitude and Compression | Amplify `amplify`, Channel Mixer `channelMixer`, Channel Volume `channelVolume`, Dynamics `dynamics`, Dynamics Processing `dynamicsProcessing` (0.7), Hard Limiter `hardLimiter`, Multiband Compressor `multibandCompressor`, Single-band Compressor `singleBandCompressor`, Tube-modeled Compressor `tubeModeledCompressor` | Hard Limiter: its lookahead; Multiband: 5 ms with Brickwall on; Dynamics Processing: 5.2 ms with its Limiter on | Multiband: Linkwitz-Riley 24 dB/oct crossovers (120 / 2000 / 10000 Hz) that sum flat within 0.02 dB. Dynamics: gate, compressor, expander, limiter (the limiter has no lookahead). DeEsser is Essential Sound's `deess`. Dynamics Processing (0.7): up to four bands with a graph, see below |
| Delay and Echo | Analog Delay `analogDelay`, Delay `delay`, Multitap Delay `multitapDelay` | 0 | Analog Delay's modes shape the feedback loop (tape, tube, analog) |
| Filter and EQ | Bandpass, Bass, FFT Filter `fftFilter`, Graphic Equalizer (10 / 20 / 30 Bands) `graphicEq10/20/30`, Highpass, Lowpass, Notch Filter `notchFilter`, Parametric Equalizer `parametricEq`, Scientific Filter `scientificFilter`, Simple Notch Filter `simpleNotch`, Simple Parametric EQ `simpleParametricEq`, Treble | FFT Filter: its FFT size; others 0 | Graphic EQ: constant-Q bands at Premiere's centres (31.25 Hz × 2^(k / bands per octave)), with gains solved so each band's centre lands on its slider. It is IIR, not Audition's FIR. Scientific Filter: Bessel, Butterworth, Chebyshev, Elliptic up to order 32 (Premiere: 66). FFT Filter: a curve of up to 12 points; Window Type is accepted but always sqrt-Hann. Premiere 26 has no "Simple Pass Filter" |
| Modulation | Chorus/Flanger `chorusFlanger`, Flanger `flanger`, Phaser `phaser` | 0 | Flanger is Audition's (Premiere shares it). After a seek the LFOs restart from the pre-roll |
| Noise Reduction/Restoration | Adaptive Noise Reduction `adaptiveNoiseReduction`, Automatic Click Remover `automaticClickRemover`, DeNoise `spectralDenoise`, DeReverb `deReverb`, Spectral Repair `spectralRepair` (0.7, see below) | FFT size; 720; 1024; 2048 frames; Spectral Repair: its FFT size, plus up to a second of context when it heals | Spectral (STFT, sqrt-Hann, 75 % overlap) noise-floor tracking with Wiener gains. DeReverb assumes a 1 s RT60. Adaptive's "High Quality Mode" is not done |
| Reverb | Convolution Reverb `convolutionReverb`, Studio Reverb `studioReverb`, Surround Reverb `surroundReverb` | Convolution: 512; others 0 | Convolution impulses are synthesised by Wabcut (five rooms, seeded, CC0 by construction), partitioned FFT convolution matches direct convolution to 4e-8. Studio and Surround Reverb are an 8-line feedback delay network |
| Special | Distortion, Fill Left with Right, Fill Right with Left, Guitar Suite, Invert, Mastering, Swap Channels, Vocal Enhancer | Mastering: 3 ms | Distortion's two transfer graphs are one control point per polarity. Mastering at defaults is an exact delay. Loudness Radar (a meter) is not done |
| Stereo Imagery | Stereo Expander `stereoExpander` | 0 | 100 = unchanged, 0 = mono, 300 = three times the side signal |
| Time and Pitch | Pitch Shifter `pitchShifter` | 19.3 / 31.3 / 44.4 ms (Low / Medium / High precision) | Correlation-aligned splicing, the same length as the input. Ratio applies only when Semi-tones and Cents are both 0 (the model has no linked parameters) |

Effects existing before this round are unchanged: Essential Sound's EQ, compressor, limiter, Reduce Noise, filter,
DeHum, De-ess, expander, reverb and width.

## Dynamics Processing (0.7)

Premiere's **Audio Effects > Amplitude and Compression > Dynamics Processing** (`wabcut.audio.dynamicsProcessing`): graph-based
multiband dynamics. Drag it onto a clip, a track (the mixer strip's **+ Effect** menu) or the master. Its section in Effect
Controls opens with a graph; the same controls are rows below it, every one keyframable on the timeline grid except the
choices (mode, band count) and the two optional stages.

**The graph** (Effect Controls, under the effect's header):
- The **frequency view** (20 Hz to 20 kHz, logarithmic) shows the bands side by side. Drag a **divider** to move a crossover,
  drag a band's **threshold** handle up and down (-80 to 0 dB). The bar hanging from the top of a band is its **gain
  reduction**; the meters on the left and right are the effect's input and output peaks. Click a band to select it.
- The **transfer curve** of the selected band (input level against output level, over the dashed 1:1 line) shows the mode,
  threshold, knee, range and make-up. Drag its **threshold** handle sideways and its **ratio** handle up and down (for a gate the second
  handle is the range; a limiter has none).
- Each band has **Solo** and **Bypass** switches, a **mode** (Compress, Expand, Gate, Limit) and number fields: Threshold, Ratio,
  Range, Attack, Release, Knee, Make-up. Below: **Input**, **Output**, **Mix**, the **Limiter** and the **Auto Gate** switches.
- **Presets** (Neutral, Voice Leveler, De-esser, Noise Gate, Bass Tamer, Podcast Glue, Broadcast Limiter) set the crossovers, the
  bands, the limiter and the Auto Gate in one undo step; Input, Output and Mix stay. Any setting can be saved with **Save
  Preset** like every effect, and is stored in the project.
- Arrow keys move a focused handle (Shift: coarse). The numbers are the effect's parameters, so the command palette, `audio.addEffect`
  and the MCP surface set them as well.

**The sound:**
- Up to four bands (**Bands** 1 to 4) split by **Linkwitz-Riley 24 dB/octave** crossovers; the lower bands are all-passed at the
  upper crossovers, so the bands sum to an all-pass (flat magnitude, within 0.05 dB) when nothing acts.
- A band's **mode** decides its curve. **Compress**: above the threshold the level rises 1/ratio as fast. **Limit**: ratio infinity.
  **Expand**: below the threshold the level falls ratio times as fast. **Gate**: an expander that takes the whole **Range**
  as soon as the level is under. A soft **Knee** (dB, centred on the threshold) joins the lines with a quadratic; **Range** is the
  most a band is ever taken down (0 to 80 dB); **Make-up** adds afterwards. The static curve is `dynamicsCurveDb()`: the graph draws
  the DSP's own function.
- **Attack** and **Release** (time constants of the gain change, 1 - 1/e of a step): Compress and Limit follow the smooth
  decoupled peak detector (Giannoulis, Massberg and Reiss, JAES 2012), Expand and Gate open in the attack time and close in
  the release time (a 10 ms peak envelope in front). The curve is evaluated per sample.
- **One gain per band for every channel** (from the band's peak over the channels), the same for mono, stereo, 5.1 and 7.1; the LFE
  of 5.1 and 7.1 is not listened to and follows the gain.
- Master section: **Input** and **Output** gain, a dry/wet **Mix** (0 % is the input untouched), an **Auto Gate** in front of
  the bands (threshold, release; 2 ms attack and 50 ms hold) and an optional **Limiter** after them: a 5 ms lookahead true-peak
  limiter (ceiling, release) that is the effect's only latency (about 5.2 ms; the dry signal of the mix is delayed to match).
- **Solo** and **Bypass** are read at every cell and smoothed over 5 ms, so switching them while the sound plays makes no click.
- The graph's meters show the audio as it is rendered, which for the effect of a clip or a track is a fraction of a second
  ahead of the speakers (the player renders those stacks ahead of playback; the master's are live). While the sound is stopped they
  fall to the floor.
- Costs about 2 % of one core for stereo at 48 kHz with every stage on. Preview and export give the same samples whatever the
  block size.

**Interchange:** FCP7 XML, FCPXML and OTIO have no equivalent. Each file still carries the effect, so Wabcut reads it back with
every setting and keyframe; other editors skip it, and the export report names it ("Dynamics Processing: other editors skip it").

Tests: `tests/audio/test_dynamics_processing.cpp` (flat bands, every mode's static curve against the reference function,
attack and release times, the meters, solo and bypass, mono / stereo / 5.1 / 7.1, automation, the limiter's latency and dry
delay, the Auto Gate, the CPU budget), `tests/app/test_d7_audio_ui.cpp` (the graph row, the edits, presets and meters, and the
panel), `tests/interchange/test_interchange_effects.cpp`. Left for later: a spectrum analyser behind the frequency view,
sidechain input from another track, per-channel (unlinked) detection, auto make-up.

## 7.1 surround master (0.7)

A sequence's master can be **stereo, 5.1 or 7.1** (New Sequence > Advanced > Audio master, the master strip's **Format** tabs, `sequence.setMasterFormat`),
and a track or a submix can be 7.1 too (track header menu, Add Tracks, `track.setFormat`, **+ Submix**).
- **Channel order** is a 7.1 WAVE file's and FFmpeg's `7.1`: L, R, C, LFE, then the **rear surrounds Lrs, Rrs** (channels 5 and 6),
  then the **side surrounds Ls, Rs** (channels 7 and 8). A 5.1 mix's L, R, C and LFE are the first four. No channel is renumbered
  between a file, the mix and the device.
- **Downmix** (ITU-R BS.775): 7.1 to 5.1 is Ls' = Ls + 0.7071 Lrs and Rs' = Rs + 0.7071 Rrs (the side pair as it is, the rear pair
  at -3 dB); 7.1 to stereo is that and then L' = L + 0.7071 C + 0.7071 Ls', R' likewise, the LFE left out; to mono the mean of the
  stereo. Preview on a stereo device and export to stereo use them.
- **Upmix from 5.1:** switching a 5.1 sequence to 7.1 puts 5.1's Ls / Rs on the **side pair** and leaves the **rear pair silent**; the
  master strip (and the command's `note`) says so. `upmix51To71(true)` is the variant that also copies them into the rear pair at
  -3 dB each. Mono and stereo go to C and L / R. Going back folds the rear pair into the surrounds at -3 dB.
- **Panner:** on a 7.1 master a mono or stereo track's pan knob becomes a puck over **seven** speakers at L -30, C 0, R +30, Ls
  -90, Rs +90, Lrs -150 and Rrs +150 degrees (VBAP, constant power, the LFE send as in 5.1).
- **Meters:** the master strip and the Audio Meters panel show eight channels named L R C LFE Lrs Rrs Ls Rs.
- **Effects:** every built-in audio effect runs on eight channels (the engine's limit is eight); the reverbs keep the LFE dry as in 5.1.
- **Loudness:** measured in 7.1 (libebur128 weighs the four surrounds +1.5 dB and ignores the LFE).
- **Export:** "As the master" writes 8 channels for AAC, Opus and PCM (WAV, MOV, MKV, MP4; ffprobe reports `7.1`); AC-3 carries 5.1 at
  most, so a 7.1 master is downmixed to 5.1 for it; MP3 is stereo. Audio Channels also offers **7.1** explicitly. MXF writes the 7.1
  mix over 8 tracks. AAF and OMF break a 7.1 track out to eight mono tracks named for their channels (L R C LFE Lrs Rrs Ls Rs) and read them back as a 7.1 layout with a 7.1 master; the master itself is not written
  (each track keeps its own channels). Eight mono files from other editors are routed too when their names carry the speaker (L, R, C,
  LFE, Ls, Rs, Lrs / Lsr, Rrs / Rsr), else in the order of a 7.1 file, and the import report says which.
- **Older Wabcut:** File > Save for an Older Version writes a 7.1 master as 5.1 and 7.1 tracks and channel maps as adaptive ones, and
  lists it ("A 7.1 audio master, tracks and channel maps").

Tests: `tests/model/test_audio_channels_model.cpp`, `tests/playback/test_audio_channels.cpp` (the conversion coefficients, channel
identity, the panner's positions and constant power, mono and stereo tracks on a 7.1 master, the 5.1 to 7.1 upmix, exports to stereo
and 5.1, preview equals offline), `tests/audio/test_effects_catalog.cpp` (every effect on eight channels), `tests/export`, `tests/audio_edit`
(`sequence.setMasterFormat`'s note), `tests/app/test_d7_audio_ui.cpp`.

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
- **Your own sound instead of the tone (0.7).** `audio.replaceProfanity` plays a sound file of yours (a short effect, a
  chosen bleep) on each of the words: **Bleep > Replace with a sound file…** on the Captions page. The sound starts with
  each word and is cut at the word's end (a shorter sound is followed by silence); only its first 20 seconds are used, and
  it is played as mono. The file joins the project's files, so it is found again, relinked and collected with the media; if
  it goes missing the tone plays. Like a bleep it is one undo step (the file and the redaction together), applies in every
  use of the media, in preview and export, and captions over the words read [bleep]. It reads a file you name, so it is a
  file command: an extension cannot run it. Mute and a sound do not mix (a sound replaces the bleep).

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
- **Audio Meters (0.5).** Premiere's Window > Audio Meters: the level of the mix (the master's meter: peak, RMS and
  a 1.5 s peak hold per channel, two bars for stereo, six for a 5.1 master, eight for a 7.1 one) over the panel's height, beside a 0 to
  -60 dB scale, with the highest peak hold as a readout and a clip light that a click (or Space) clears. The Edit
  preset docks it right of the timeline, slim, for workspaces made or reset from the preset from 0.5 on (saved
  workspaces keep their panels: Window > Audio Meters or Reset to Saved Layout adds it). Like any panel it closes,
  joins a group as a tab and floats; narrower than its tab strip it shows the dock's handle over its corner on hover.
  The meters poll (~30 Hz) while the Audio Meters panel or the Audio Track Mixer shows: each counts as one viewer
  (`AudioMixer.setMeterViewer`). `window.audioMeters` has no default key, as in Premiere.
- **Audio scrubbing (0.5).** Premiere's Play Audio While Scrubbing, on by default: while the transport is stopped,
  dragging the playhead in the timeline's ruler or on the Program Monitor's time bar, and stepping frames (the arrow
  keys, the step buttons), play a short window of the sequence mix at the new frame: 60 ms, or one frame of a sequence
  under about 17 fps, at most 200 ms, faded in and out over 5 ms. It is the export mix (mute, solo, faders, pan, clip
  and track effects, submixes, the master bus), rendered off the GUI thread by `playback::AudioScrubber` on a feed of
  its own, at most one window every 30 ms (a newer request replaces one still waiting; a new window fades out the one
  sounding), and played through the AudioEngine's overlay, so the transport clock and the picture do not move. A drag
  that goes back plays the window before the playhead backwards, from the playhead back in time (Premiere's backward
  scrub; the fades are applied after the reversal); a step plays the frame stepped to forward. The Source Monitor scrubs
  its clip the same way (its time bar and waveform, its step buttons and keys: the clip's own audio streams, through its
  own playback session). Shift+S
  (Toggle Audio Scrubbing), Sequence > Audio Scrubbing and Settings > Timeline & editing > "Play audio while
  scrubbing" turn it on and off (remembered). The first scrub opens the audio device if playback has not yet.

## Script prompter for voice-over (0.6)

Settings > Audio > Script prompter shows a narration script on the Program Monitor while a voice-over records, so the
script is not read from another window. **Open script...** reads a .txt, .md or .fountain file (UTF-8, up to 256 KB);
**Paste...** takes typed or pasted text. Markdown headings, list markers, quotes and emphasis are stripped; a blank line
or a heading starts a paragraph (a script without blank lines is one paragraph per line).

The overlay shows a band along the bottom of the monitor with the paragraph being read, large, and the next one dimmed,
from the count-in to the end of the take, and also while a track is armed for recording when **Show when armed** is on
(the default). Right or Page Down moves on, Left or Page Up goes back during a take (a presentation clicker sends these
keys); Space still stops the take. The band's arrows work at any time. **Text size** (16-96 px; smaller when the monitor is too small for it) and **Mirror** (for a
teleprompter's glass) are options, and **Marker per paragraph** drops a marker at the playhead, named after the
paragraph's first words, when a take starts and at each paragraph you move on to while recording (one undo step each).

The script is not part of the project: it, the paragraph reached and the options are kept in the user's settings, by
project file, so the place survives takes and reopening the project. A script opened from a file is read again from the
file when the project reopens, if the file is still there.

## Loudness QC: where the loudest moments and true-peak overs are (0.6)

`audio.loudness` also reports where things happen: `maxMomentaryAt` and `maxShortTermAt` (where the loudest 400 ms and
3 s windows start) and `truePeakAt` (the 100 ms block holding the true peak), each a timecode and ticks on the timeline,
and the true-peak overs above `oversAbove` (default -1 dBTP; -6 to 0): neighbouring 100 ms blocks above the ceiling
(gaps of one block bridged) merged into ranges with their highest peak, at most 200 listed, `overCount` counting them
all. Results cached before 0.6 are measured again.

A measurement keeps the 100 ms blocks above -6 dBTP, up to 100,000 of them. A programme with more (a stream whose music bed
peaks above -6 dBTP for hours) keeps its loudest: the quietest quarter of the list is dropped and the floor rises, so the
last hours are listed as well as the first. Overs above a ceiling at or over that floor are exact; when `oversAbove` is
below it, the overs are counted above the floor instead, `oversAbove` in the report says which ceiling was used and
`oversTruncated` is true (the Audio Tools report then shows the count with a "+", and the master strip's text names the
raised ceiling). Results cached before 0.7 are measured again.

In the Audio workspace, the master strip's loudness card and the Audio Tools card show the loudest short-term and
momentary moments and the true peak with their times as links that move the playhead there, then "N true-peak overs
above -1 dBTP" with a stepper through them. **Add markers for overs** puts a red range marker over each over, named with
its peak, in one undo step "Add True-Peak Over Markers". The Audio Tools report has an Overs column (a link to the first
over of each row). The ceiling is the delivery's -1 dBTP.

## Audio Track Mixer menu: Show/Hide Tracks, Meter Inputs Only, Loop (0.7)

The **⋯** button at the right of the Audio Track Mixer's header opens Premiere's panel menu, each row with its key:

- **Show/Hide Tracks…** (`audioMixer.showHideTracks`, Ctrl+Alt+T; Opt+Cmd+T on macOS) opens a checklist of the strips - tracks,
  then submixes - with the track's colour, its name and a switch. Switching one off takes its strip out of the mixer's row;
  **Show All** brings every strip back. A hidden track still plays, keeps its mute, solo, fader and effects and still
  counts in the mix: hiding only gives the other strips room. The choice is remembered for the session, per sequence, and
  is not an edit (no undo step, the project is not changed). The button's tooltip says how many tracks are hidden, and a
  mixer whose strips are all hidden says so.
- **Meter Inputs Only** (`audioMixer.meterInputsOnly`, Ctrl+Shift+I): while the microphone is open (a track is armed for
  voice-over recording and the input meter is running), the meter of a record-armed track shows the microphone's level and
  the meters of the other tracks stay empty, so the level you set while recording is the only one moving. The master keeps
  metering the mix. With the microphone closed the switch changes nothing.
- **Loop** (`audioMixer.loop`, Ctrl+L; Cmd+L on macOS) is the Program Monitor's Loop (`monitor.loop`): playback repeats the
  sequence, or its In to Out range. The row shows the switch.

All three are in the command palette and in Keyboard Shortcuts under Audio Track Mixer. Tests:
`tests/app/test_d5_audio_ui.cpp` (Show/Hide Tracks, Meter Inputs Only, Loop, the menu and the checklist with screenshots).

## Match: EQ Match and Level Match (0.7)

**Audio Tools > Match** (the second tab of the panel) learns from a reference, as Audition's Match EQ and Match Loudness and
Premiere's loudness matching do:

1. **Reference**: the first selected audio clip (as it renders, its effects included), the mix between the sequence's In and
   Out points, or an audio file of the project (**Audio file…** lists the project's media with audio).
2. **Target**: **Use selected clips** takes the audio clips selected on the timeline (a video clip stands for its linked audio).
3. **Learn** listens to both in the background (**Cancel** stops it) and changes nothing. It shows, for both: the loudness (LUFS,
   BS.1770-4 / EBU R128), the true peak, the RMS and the crest factor, and draws the average spectra over the 30 third-octave
   bands of the Graphic Equalizer (31 Hz to 25 kHz): the reference, the target and the target as it will sound after the match,
   with the correction below as bars (±18 dB around a zero line).

**EQ Match** (`audio.eqMatch`): the average spectrum of each signal is taken over the stretches that are not pauses (frames
within 25 dB of the loud ones; a long silence or a noise floor between phrases does not count), the channels' powers averaged
(the LFE of a 5.1 signal left out). The correction is the difference of the two in the 24 bands from 79 Hz to 16 kHz with the
overall level difference removed (EQ Match changes the tone, Level Match the level), smoothed, limited to ±18 dB a band and scaled
by **Amount** (0-100 %, default 100); **Smooth** (default 50 %) says how closely it follows each band. The bands beside the
matched ones fade out (at half per band below, 0.9 per band above). Both sliders reshape the curve at once, without listening again.
**Apply EQ** gives each target clip a **Graphic Equalizer (30 Bands)** with those sliders - a cascade of minimum-phase peaking
filters, so it costs no latency and plays the same in preview and export - marked as an EQ Match (effect data), in one undo step
"EQ Match". It sits after the clip's normalisation gain and before its dynamics, is an ordinary effect in Effect Controls,
and carries the reference's and the target's spectra, so the picture comes back after a save and a load. Applying again replaces
the earlier EQ Match and learns without it (the target is heard without its own earlier correction). **Preview** switches the EQ
Match of the target clips off and on (one undo step each) to compare by ear while playing; **Reset** takes it off
(`audio.eqMatchReset`). A reference that is silent or has less than a second of audible sound, or a target like that, is refused
with a reason; a different channel layout on either side is fine (each is measured the way export hears it).

**Level Match** (`audio.levelMatch`): **Match level** moves the target clips' normalisation gain until their integrated
loudness is within 0.1 LU of the reference's (**Match loudness**), or their true peak within 0.1 dB of its (**Match peak**),
measuring again after each step, so a compressor after the gain is accounted for. **Headroom** (default 1 dB) is the true peak
the result may not pass: with **Limit peaks to the headroom** a true-peak limiter at that ceiling is added after the gain
(gain plus limiting), otherwise a warning says the peak passes it. The panel lists the measured loudness and true peak before
and after for each clip. One undo step "Level Match". The command also takes a track or the mix as the target.

Commands: `audio.eqMatch` (`reference`: `"mix"`, `{"clip"}`, `{"track"}` or `{"media", "stream"}`; `referenceFrom` / `referenceTo`;
`clips`; `from` / `to`; `amount`; `smoothing`; `maxGain`; `apply` false only learns and reports the curve), `audio.eqMatchReset`
and `audio.levelMatch` (`reference`; `clips`, `track` or the mix; `mode`; `headroom`; `limit`; `tolerance`; `maxPasses`).
Engine: `dsp::SpectrumAverager` and `dsp::LevelAccumulator` (`dsp/Match.h`), `fitEqMatch` (`edit/MatchCurve.h`), and the host's
`LoudnessAnalyzer::measureMatch` (one render of the real path: clip, track and master effects count). Tests:
`tests/audio/test_match.cpp` (a known tilt is recovered within 0.5 dB, the Graphic EQ makes the target sound like it, pauses do
not count, ±18 dB, mono and 5.1), `tests/audio_edit/test_match_commands.cpp`, `tests/app/test_d5_audio_ui.cpp`.
Limit: the Graphic Equalizer's bell filters narrow towards the sample rate's limit, so the two top matched bands (12.7 and 16 kHz at
48 kHz) are realised one to three dB short of the curve; the rest of the range follows it within a dB.
Left for later: a match of the stretch of a long reference chosen on a waveform, saving a learned curve as a preset of its own
(an EQ Match is an effect, so it can already be saved with the clip's effect presets), and a per-band amount.

## Loudness panel and Loudness Radar (0.7)

**Window > Loudness** opens the Loudness panel right of the timeline (a slim column between the timeline and the Audio Meters;
it closes, joins a group as a tab and floats like any panel). It is the loudness of what plays, always in view - not an
effect window and not an analysis you run. **Meter** shows the momentary (400 ms) and the short-term (3 s) loudness as two bars
on the EBU R128 scale around the delivery target (+9 to -18 LU, a tick at the target; green within 1 LU of it, amber up to
3 LU above and red past that, blue below), with the numbers: short-term, momentary and integrated loudness in LUFS (the
integrated one since play started or **Reset**, with how far it is from the target), the true peak in dBTP (red over the
target's ceiling) and the loudness range in LU, refreshed once a second. The values come from the player's loudness tap
(libebur128: K-weighting, gated integration, 4x oversampled true peak, the channel map of EBU R128 - what export measures),
off the audio thread, and only while the panel is on screen (it counts as a meter viewer). The **target** (YouTube -14, Podcast
-16, Broadcast -23 LUFS or custom) is the one of the Audio Track Mixer's master strip.

**Radar** is the Loudness Radar: one revolution of the short-term loudness, time running clockwise from the top. Each slot is a
wedge whose length is its loudness, drawn from a hole in the middle; the bright ring is the target, rings every 6 LU, the
shaded band the loudness range (the 10th to the 95th percentile of the gated short-term values, EBU Tech 3342) and wedges are
green within 1 LU of the target, amber up to 3 LU above, red past that, blue below. **Live** draws what plays since the play
started or Reset over a lap of 1, 5, 10, 30 or 60 minutes (up to 360 slots; the white spoke is where the sound is now and the
wedges ahead of it are the lap before, fading); **Programme** draws the whole mix of the last **Analyze mix** in one turn
(`audio.loudness` with `series`: the short-term loudness once a second from the start). Slots average the energy of their
seconds. The radar reads out its range and the low and high ends of the band; for a programme analysed before the project
changed it says so. Results cached before 0.7 are measured again.

Command: `audio.loudness` also lists `shortTermSeries` with `"series": true`. Tests: `tests/audio/test_match.cpp` (the series, the
live range, the tap's history, the folding and the band), `tests/app/test_d5_audio_ui.cpp` (the panel beside the timeline,
the radar's live lap and programme, screenshots).
Left for later: a radar of a track or a clip on its own, a gating setting other than EBU's, and a loudness history graph.

## Mixer effects: settings, failures, Enhance Speech (r07)

In the Audio Track Mixer, each strip's effect stack (tracks, submixes and the master) works like this:

- **Settings.** **Settings…** in a slot's menu, or a double-click on the slot, opens the effect's settings beside the
  strip. Every parameter gets its own control: a slider with a number box and its unit, a switch, or a menu. Changes
  are live while playing. Each change is one undo step, and a slider drag counts as one step. On an animated parameter
  the change sets the key at the playhead, as the fader does. The popup also has an **On** switch (bypass), **Reset to
  defaults** and, for a plugin with an editor, **Edit plugin…**. Hovering a slot shows its main values ("Threshold
  −20.0 dB · Ratio 4.00 · …").
- **Reorder.** Drag a slot up or down to move it in the stack (one undo step). **Move up** and **Move down** are still in
  its menu.
- **Presets (0.7).** The **Presets** row under a strip's slots saves its effect chain: **Save effects as preset…** asks for a
  name, a type (Scale, Anchor to In Point, Anchor to Out Point: how keyframes land on a chain of another length, with
  *Anchor to In Point* the default because a track effect's keys are timeline times), a description and a Presets bin.
  The preset keeps every effect in order with its settings and keyframes (a bypassed effect stays bypassed); the fader
  and panner are not part of it. The row also lists your audio presets: choosing one adds its effects after the strip's
  own, as one undo step (**Apply Preset to Tracks**). Tracks, submixes and the master take them alike. Presets live in
  your library, so they show in the Effects panel's Presets bin too (applied there, to the selected audio clips).
  Commands: `effects.saveTrackPreset` (`target`: `A1`, `S1` or `master`) and `effects.applyTrackPreset`.
- **Failures show on the strip.** If an effect cannot be added, the strip says why under the slots. Examples: Enhance
  Speech picked on the master or on an empty track, or a plugin that fails to load. The Essential Sound record belongs
  to a clip, so the menu no longer lists it.
- **Enhance Speech** is rendered per clip, so it goes on the track's clips: picking it opens the Enhance Speech window
  for them. If its speech model is not downloaded yet, the strip says so and has a button to open the window again.
  Once the clips have it, the strip shows an **Enhance Speech** slot with the number of clips ("1/3"). From that slot,
  **Settings…** (or a double-click) opens the window, and **Bypass** and **Remove from the clips** act on every clip
  of the track in one undo step.

Bridge: `AudioMixer.effectSettings`, `setEffectParam` / `endEffectGesture`, `resetEffect`, `setClipEffectEnabled`,
`removeClipEffect` (`AudioMixerEffects.cpp`); UI `qml/audio/{EffectSlots,MixerEffectSettings}.qml`. Tests:
`tests/app/test_mixer_effects.cpp`.

## Auto-mix of several voices (0.7)

A podcast, an interview or a co-op stream with one microphone per person piles up room noise, breathing and bleed
whenever somebody else talks. **Audio Tools > Auto-duck > Voices** (the third tab of the card, "Several voices") lowers the
microphone of every voice that is not speaking while another voice speaks, and brings it back before its own speaker's
next word. The voices are the audio tracks with the **Dialogue** role (give two or more tracks that role in the mixer;
the card lists the tracks it will mix and says what is missing). It is `audio.autoMix`:

- Speech is found on each voice track on its own, as `audio.autoDuck` finds it (transcript words where the media has a
  transcript, else the audio level), so a voice is known by what its own microphone carries. Bleed of one voice into the
  other's microphone counts as speech there only when the level says so: close microphones work best.
- A microphone goes down by **Amount** (default -12 dB) over **Attack** (0.1 s) once the other voices have spoken, and
  back up over **Release** (0.5 s). A voice stays on for **Hold** (0.4 s) after its last word: a breath or a pause
  between sentences does not lower it, and a gap shorter than attack plus release between two lowered stretches is
  bridged, so a microphone does not pump.
- A voice is never lowered under its own words. Where both speak (talking over each other) nothing is lowered. The ramp
  that brings a microphone back ends 0.08 s (`lead`) before its speaker's first word, also when that speaker starts talking
  over somebody, and a ramp down that would run into the voice's own held speech starts after it instead. Speech shorter
  than 0.2 s (`minSpeech`: a cough, an "mm") is not a voice, and a stretch too short to go down and come back is left alone.
- **Also quiet a microphone while nobody speaks** (`"gate": true`) adds a noise gate for each voice: the silent
  microphones dip in the pauses too.
- The result is plain volume keyframes on the clips of each voice track (editable afterwards, replacing the clip's volume
  keys in the range like Auto-Duck). **Preview ducking** shows them as ghosts on the timeline first, with how many lowered
  stretches and keyframes each voice gets; **Apply** is one undo step, **Auto-Mix Voices**. A locked track is skipped with a
  warning.

Command: `audio.autoMix` (`tracks`, `amount`, `attack`, `release`, `hold`, `lead`, `minSpeech`, `gate`, `source`, `from`,
`to`; `dryRun` previews; the report lists each voice's speech runs, lowered regions and clips). Planner:
`audio::edit::autoMixRegions` (`AutoMix.h`). Tests: `tests/audio_edit/test_auto_mix.cpp`, `tests/app/test_d2_text_audio_ui.cpp`.
Left for later: bleed-aware detection from the levels of all microphones together (a voice is speaking where its microphone
is the loudest), a per-voice amount, and mixing voices across nested sequences.

## Spectral Repair (0.7)

A cough over a sentence, a chair squeak, an alarm in the street, a camera click, a mains hum, one note held through a
dialogue: broadband tools (DeNoise, a notch, a gate) dull the whole clip to get rid of them. **Audio Tools > Spectral**
(the third tab of the panel; also **Clip menu > Spectral Repair…**) shows the sound of the selected audio clip as a
spectrogram and repairs what you select in it, and nothing else. Nothing is rendered into a file: the repairs are an
edit list stored with the clip (one `Spectral Repair` effect, `wabcut.audio.spectralRepair`, made on the first repair),
played the same in the preview and in every export, and undone like any edit. A video clip stands for its linked audio.

**Seeing.** The picture is the short-time Fourier transform of the clip (1024, 2048 or 4096 points, sqrt-Hann windows, 75 %
overlap), time to the right, frequency up (**Log** or **Linear**), loudness as colour on a dB scale you set (**Range**: the
quietest and loudest level the colours span). The colour map is its own for the dark and the light theme (quiet is the
panel's own tone, loud is dark violet to white-hot), so it reads in both. The wheel zooms around the pointer, Shift+wheel and
the bar under the picture scroll, **Fit** shows the whole clip; the line under the picture reads time, frequency and level
under the pointer; a click on the time ruler (or in the picture with the Box tool, without dragging) moves the playhead; the
playhead is drawn in the picture. **Channel** looks at one channel of a stereo or 5.1 clip instead of the mix. **Original /
Result** switches between the clip's sound and the sound as its repairs leave it, so an edit can be judged by eye.
The picture is drawn on a worker thread from tiles of 8-bit levels (a minute of sound is about twenty-two 260 kB tiles) that are
computed when they are first needed, kept within a 96 MB budget and computed again when they fall out of it, so a long
recording scrolls and zooms without waiting and without growing: zoomed out, 2^n neighbouring columns are pooled into
one (the loudest, so a click survives), and the picture shows the part already analysed while the rest comes in.

**Selecting.** **Box** drags a rectangle of time and frequency, **Lasso** draws around the sound, **Brush** paints over it
(the brush's half-width in milliseconds and half-height in semitones are set beside the tools), **Tone** clicks a steady tone
(the click snaps to the strongest peak near it) or drags along a moving one. The line under the tools gives the selection's
start and end, its length and its frequency range.

**Repairing.** Each button is one undo step with its own name:

- **Attenuate** lowers the selection by the dB beside it (down to -96), with a soft edge in frequency of two bins. The sound
  outside the selected seconds is the clip's, sample for sample.
- **Heal** replaces the selection with what surrounds it: for every bin it takes the level of the nearest untouched sound
  before and after it, and above and below it, weighted by the inverse square of the distance, with a fixed random phase, so no
  tone and no "musical noise" is made. A cough over a steady bed disappears into the bed; a squeak in room noise becomes
  room noise. The **%** beside it says how much of the repair replaces the sound.
- **Fill with noise floor** replaces the selection with the quiet background around it (the lower quartile of the untouched
  levels within half a second), for a selection whose surroundings are speech or music that Heal would copy into it.
- **Remove tone** takes out the steady tone in the selection (a box around a hum finds its frequency; a click with the Tone
  tool names it) and **Harmonics** of it, over the selection's time, or over the whole clip for a click. It works in the
  time domain: the tone is demodulated at its frequency, low-passed (each notch is 12 Hz wide) and subtracted, one harmonic
  after the other, which cuts only the narrow band of the tone (less than 1 dB of change 20 Hz away) and follows a tone
  whose pitch moves when it is traced (**Tone** tool, dragged along the harmonic). Removal is to -60 dB and fades in and out over 20 ms.
- Every repair acts on exactly the seconds selected: an STFT frame is about 40 ms long, but the correction is gated to the
  selection's start and end (with a crossfade of a quarter of its length, at most 5 ms), so a click can be healed without
  touching its neighbours.
- Overlapping repairs apply in the order they were made, each on what the one before left. At most 400 repairs on a clip
  (a lasso or a brush stroke keeps up to 48 points); the list under the tools shows them (the bin button takes one off, **Clear
  all** the lot, a click on a row moves the playhead there).

**Listening.** **Play selection** plays the selected seconds once and returns the playhead. **Repaired / Selection / Removed**
sets what the player plays (it is a parameter of the effect: the repaired clip, only the sound inside the repairs' regions,
or only what the repairs take away) and a note reminds you to put it back to Repaired before an export. **Repair on** switches
the clip's repairs off to compare with the original (an A/B on the effect's switch).

**How it is stored and rendered.** Edit times are seconds of the clip's *source* and frequencies are hertz of the source
sound, so a repair stays on the sound when the clip is trimmed, moved or its speed changed (a clip resampled to another speed
without Preserve Pitch sounds an octave up at double speed, and the repair follows the sound up). Edits are text lines in the
effect's data (`e00`...`e62`, up to 1024 bytes each, one line per edit: kind, shape, level, amount, start and end,
frequency range, brush size, harmonics, width and its points), so an older Wabcut that does not know the effect keeps it
untouched and plays the clip without it. The effect looks at the sound around each edit: its latency is the FFT size plus
the context the heal looks ahead (at most a second), compensated like any effect's, and the player pre-rolls it after a seek.
The STFT frames lie on the project's own sample grid, so preview and export, starting anywhere, cut the sound into the same
frames and agree to the last bit (`tests/audio/test_spectral_repair.cpp` "Preview and export render the repair identically");
the tone removal is a filter with a memory that dies away within a second, so it agrees to better than a millionth after the pre-roll.
The `fftSize` parameter of the effect (also in Effect Controls) is the analysis size; a larger size resolves narrower
tones, a smaller one localises clicks. Keyframes do not apply: the effect has none.

**Commands:** `audio.spectralAttenuate`, `audio.spectralHeal`, `audio.spectralNoiseFloor`, `audio.spectralRemoveTone` (the
region as `start`, `end`, `low`, `high`, or `points` for a lasso, a brush stroke or a tone's trace), `audio.spectralRepairRemove`
(`index` or `all`), `audio.spectralRepairOptions` (`fftSize`, `listen`, `enabled`) and the read-only `audio.spectralRepairEdits`.
Tests: `tests/audio/test_spectral_repair.cpp` (the text form and its bounds; Attenuate by the asked dB with the rest within
0.1 dB; Heal of a squeak, a cough and a click, Fill with noise floor, Remove tone of a steady, a harmonic and a moving tone; the
listen modes; 96 kHz and 5.1; the clip's time map and speed; the tiled spectrogram and its memory budget),
`tests/audio_edit/test_spectral_commands.cpp` (commands, undo, save and load), `tests/app/test_d8_spectral_ui.cpp` (the tab,
dark and light screenshots).
Left for later: a **clone** repair from another time-frequency region, an automatic **harmonic trace** (the Tone tool is traced
by hand), a background job for repairs longer than a second (Heal there fills from above and below, and a warning says so; the
effect renders on the player's feeder thread, never on the audio callback), phase-coherent heal for strongly tonal surroundings
(Heal fills with noise-like phase, so a held note is filled with the note's level but not its waveform), and per-channel repairs
(a repair applies to every channel of the clip).

## Known gaps

- Nested sequences mix their audio without their own submix routing: a nested sequence's submixes and sends are
  ignored inside the parent.
- Submixes have no sends of their own; only audio tracks send.
- A stereo, 5.1, 7.1 or adaptive submix runs at the sequence's channel count. Only mono submixes fold.
- The engine mixes up to 16 submixes; more are silent and counted.
- The mixer's automation modes (Read, Touch, Latch, Write) cover track faders and pans, not send levels. Send automation
  is through `track.setSend` with `time`.
- The Audio Clip Mixer's meters are the track meters, not per-clip meters.
- The Audio Meters panel (0.5) has none of Premiere's panel menu (dynamic peaks, the dB range, a horizontal layout).
- The Audio Clip Mixer has no Show/Hide Tracks (0.7: the Audio Track Mixer has).
- The default send level (0 dB) and many effect ranges are unverified against Premiere.
- "Show Audio Time Units" (0.2): one switch in the Program Monitor's and the timeline's settings. The playhead stops
  on audio samples (the arrow keys step one), the ruler and the readouts count samples (or milliseconds, the
  sequence's audio display format), the timeline zooms down to single samples, and drags, trims, razor cuts, fades
  and volume keyframes of audio-only material move by samples. Video, and edits at the playhead (Insert, Overwrite,
  Add Edit), stay on the frame that holds the playhead.
