# Audio plugins (A.6)

Wabcut hosts CLAP and VST3 audio effects on clips, tracks and the master bus. Plugins run outside the editor, in the
`wabcut-plughost` helper: a plugin that crashes or hangs disables itself with a badge, never the editor. Owner: Audio engine.
OpenFX **video** plug-ins have their own section at the end of this page ([OpenFX video plug-ins](#openfx-video-plug-ins-07)).
Code: `src/plughost` (the helper, the wire, the editor side), the plugin hooks in `src/audio` (`EffectChain.h`) and the model
(`Effects.h`), and the commands in `src/audio/edit` (`PluginLibrary.h`).

| | |
|---|---|
| Formats | CLAP (the clap headers 1.2.10, MIT), VST3 (the Steinberg VST 3 SDK 3.8.1, MIT since 3.8; see THIRD-PARTY-NOTICES.md). AU and LV2: not yet (macOS / Linux follow-up). |
| Platforms | Windows (the helper's processes, shared memory and windows are Win32). `WABCUT_WITH_AUDIO_PLUGINS` (default on in full Windows builds) and the vcpkg feature `audio-plugins`. |
| Where | Clip stacks, track stacks, the master stack: `plugin.<format>.<id>` effects, as keyframable as any effect. |
| Tested with | Eight free VST3 effects installed on the development computer (below), and the test plugins built in `tests/plughost`. |

## Out of process

```
 editor process                                               wabcut-plughost (one per plugin binary)
 ───────────────────────────────────────────────              ─────────────────────────────────────────────
 EffectChain ─ PluginProcessor ──┐                             main thread: message loop, the plugins' main-
   (feeder / export / callback)  │ shared memory per instance   thread calls, editor windows
                                 ├──── slots: input, events ──▶ audio thread per instance (MMCSS "Pro Audio"):
 PluginHost ─ HostProcess ───────┤◀─── slots: output ─────────   process the submitted blocks in order
   control: JSON lines on the    │  "work" / "done" events
   helper's stdin / stdout       └──── create, state, params, editor, quit ▶ control reader → main thread
```

**One helper per plugin binary** (a `.clap` file, a `.vst3` bundle). It hosts every instance of the plugins in that binary.
The decision, against one helper per instance:
- **The crash domain is the code that crashed.** A crash disables that plugin's instances. They would most likely crash the
  same way, and no other plugin or the editor is touched.
- **A heavy plugin loads once.** Its code, wavetables and models are shared, not loaded per instance: ten instances of one
  plugin are one process (tests: 10 instances, 1 helper).
- **Plugins that coordinate their instances keep working** (shared caches, licences, "link" features), as in any DAW that
  groups by plugin.

`HostOptions::processPerInstance` isolates every instance instead, to diagnose a misbehaving plugin.

**The wire** (`Protocol.h`, `Ipc.h`):
- **Control: JSON lines over the helper's pipes.** Create an instance (from its state), save and load state, read parameter
  values, open and close the editor, quit. The helper notifies `stateChanged`, `latencyChanged` and `editorClosed`.
  Not real-time. Protocol version 2 (helper and editor ship together).
  - The helper takes its stdin and stdout for the protocol before it loads any plug-in and points the process's
    standard output and error (and the CRT's) at its log, so what a plug-in prints never reaches the channel (review
    S5-P2-6).
  - The editor parses every line defensively: a malformed or mistyped line fails that instance or that scan, never the
    editor; a line is at most 16 MiB (past it the helper counts as crashed); no exception leaves a reader thread.
  - Helpers start with a minimal environment (no API keys or other secrets; `PATH` limited to the app's and Windows'
    folders) in their own folder, and every executable restricts its DLL search order (docs/building.md, S5-P2-5).
- **Audio: one shared-memory block per instance.** A ring of 24 block slots, each holding 256 frames of planar stereo input
  and output plus up to 256 parameter events, and two named auto-reset events.
  - The host writes a slot and publishes it (`submitted`, release), then signals "work".
  - The helper's audio thread processes slots in order, publishes `completed` (release) and signals "done".
  - The host's side is atomics, copies and a `SetEvent`: no lock, no allocation.

**Fixed blocks.** A plugin gets blocks of exactly 256 frames, aligned to the stream the effect chain renders. Preview and
export therefore give it the same blocks, and a deterministic plugin renders the same samples in both. Where the chain
restarts (a seek, a pre-roll), the first block carries a reset flag, and the helper resets the plugin's DSP state (CLAP
`reset`; VST3 `setProcessing` off and on).

## Latency, deadlines and the real-time rules

A plugin stage's output lags its input by **the pipeline plus the plugin's own latency**. It reports both, so the engine's
latency compensation (plugin delay compensation) aligns everything to the sample: the clip voices and track mixes are read
ahead in TrackRenderer, and the tracks ahead of the master bus.

| Where the stack runs | Pipeline | Waits for the helper? |
|---|---|---|
| Clip and track stacks in the player (the feeder, nested mixes) | 256 frames (one block) | Up to 50 ms a block (`playbackBlockTimeout`), well inside the feeder's lead; then Hung |
| Clip and track stacks in export, loudness and other offline renders | 256 frames | Yes, bounded by `HostOptions::blockTimeout` (3 s) |
| The master stack in the player (the audio callback) | 4096 frames (`kLivePipelineFrames`) | **Never.** A block the helper has not returned in time plays the **dry signal**, aligned, and fades back to the plug-in over the next block; it is counted (`PluginInstance::missedBlocks`) |
| The master stack in export | 4096 frames: the same latency as preview | Yes (export is not real time) |

On the audio callback a plugin stage only copies, uses atomics and signals. The RT audit
(`tests/plughost/test_plugin_rt.cpp`) plays plugins on a clip, two tracks and the master through edits of their parameters,
a seek, a loop and a plugin crash. It counts no allocation, no lock and no wait for a helper (`PluginHost::realtimeWaits()`).
A plugin that stalls 20 ms per block on the master misses its deadlines (188 blocks in a second, played dry and flagged),
and the longest callback stays at 0.17 ms.

**Loading.** In the player a plug-in instance is created on a worker (the helper started, the instance created, its state
loaded): until it is ready its stage plays the dry signal at the latency it will report, then switches over at a block
boundary, so a slow plug-in never stalls the feeder or the start of playback (A5-P2-5). Export and the other offline renders
(`audio::ScopedOfflineRender`; OfflineMixdown opens one) load it before they render.

**Channels.** A plug-in takes L and R. On a wider bus (a 5.1 master, or a 5.1 track) the stage strides by the bus's channel
count, feeds the plug-in L and R and passes the other channels through delayed by the same latency (A5-P1-2). Mono and
stereo tracks on a 5.1 master render their effects at the track's own channels before the 5.1 panner (docs/ai.md, A.9).

**What comes back is checked** (S5-P2-7, A5-P2-8): a latency over one second (in the helper, the scan, the catalog cache and
shared memory; 64-bit arithmetic) fails the instance; NaN or Inf in the output is replaced by the aligned dry signal and
anything else is clamped to ±16 (+24 dB), counted (`invalidBlocks()`); after 8 blocks with NaN the instance is Failed.

**Latency changes.** On a plug-in's latency notification the helper pauses its audio thread, deactivates and reactivates
it, and publishes a new latency generation. Clip and track stages are rebuilt on the same instance with the new
compensation; the master's adopts it at its next restart (A5-P2-4).

**Export** reports what it could not render as asked (`ExportResult::warnings`): before rendering, a plug-in that is missing
or failed to load; after, an instance that crashed, hung, was late or produced invalid output (A5-P2-6).

**A crash.** The helper's control pipe closes the moment it dies. Its instances report Crashed
(`InstanceHealth`, with the exit code) and their stages are bypassed. The bypass plays the dry signal, still delayed by the
stage's latency, so nothing moves. The UI badges the effect. The next instance of that plugin starts a new helper.

A plugin that reports an error (process failed) is bypassed as Failed. A block that takes longer than the block timeout
ahead of playback marks the instance Hung, and it is bypassed too.

## Scanning

`PluginCatalog` (`PluginCatalog.h`) finds and remembers what this computer has.

**The folders.** Windows: `%COMMONPROGRAMFILES%\CLAP`, `%LOCALAPPDATA%\Programs\Common\CLAP`, `%COMMONPROGRAMFILES%\VST3`
and `%LOCALAPPDATA%\Programs\Common\VST3`, plus the user's folders (`addUserFolder`, remembered).
- `.clap` files and `.vst3` files or bundles are found to 8 levels deep, without descending into bundles.
- A bundle whose binary has a vendor's own name (`ElgatoEq.vst3` holding `ElgatoEq_v1.3.0.110.vst3`) is found too.

**Crash isolation.** Every file is scanned by its own `wabcut-plughost --scan <file>`. The helper loads the binary, lists its
plugins and creates each effect once, to read its parameters, editor and latency at 48 kHz.
- A helper that crashes puts the file on the **blocklist** with the reason ("crashed while being scanned (exit code
  0xC0000005)").
- A helper that hangs past the timeout (30 s) is ended and blocklisted too.
- Blocked files are skipped until they change (size or time) or are unblocked.

**The cache.** `%LOCALAPPDATA%\Wabcut\plugins.json` (`WABCUT_PLUGIN_CACHE` overrides it for tests and scripts) keeps, per
file, its size and time and its plugins:
- id, name, vendor, version, category;
- parameters (id, name, module, unit, range, default, automatable, stepped, hidden);
- latency and editor.

An unchanged file is not scanned again: a second scan of the test folder takes milliseconds. The app scans new and changed
files in the background at startup (`PluginSystem::start({.scanInBackground = true})`).

## Plugin effects

**The model.** A scanned plugin is an effect descriptor: id `plugin.vst3.<class id>` or `plugin.clap.<plugin id>`, kind
Audio, category "Audio Plugins/<vendor>".
- **Its parameters** are the plugin's visible parameters, with keys `p<id>`, ranges and defaults (CLAP plain values; VST3
  normalised 0..1). Each is animatable when the plugin calls it automatable.
- **The minimal model hunk.** `Effects.h` gained an `EffectDescriptorProvider`, so `EffectRegistry::builtin().find()` answers
  plugin ids. Validation, keyframing (`clip.addKeyframe`, `audio.addKeyframe`) and the evaluators then treat a plugin like any
  effect.
- **A plugin this computer does not have** is not found: the model keeps it as an unknown effect (saved unchanged, bypassed),
  as it does any unknown effect. The badge reads "Missing plugin: TDR Nova (Tokyo Dawn Labs)" from the name it was saved with
  (`plugin.name`).

**Parameters in the engine.** The instance's own parameters, the ones it sets, are sent to the plugin. The rest stay as the
plugin's state has them.
- They go as sample-accurate events (CLAP `param_value`, VST3 parameter changes) at the parameter grid's cell starts
  (32 frames), when their value changes.
- Constants and keyframe curves work alike, like the built-in effects' parameters.
- `EffectChain` binds a plugin stage by the instance's parameter keys (any number) instead of the descriptor's eight slots.

**State.** The plugin's chunk is stored with the effect as base64 `EffectData` text.
- **The format.** Parts `plugin.state.000`, `plugin.state.001`, ... (each at most the model's 1024 characters), and a checksum
  `plugin.state.sum`.
- **The bound.** At most 45 KB of state (60 parts within the model's 64 data entries); a larger chunk is refused with the
  reason.
- **When it is applied.** A new instance is created from the state.
- **Capture.** `PluginEffects::captureState()` saves the live instance's state (to store with `setPluginState()`). A
  processor rebuilt for that new data keeps the same instance instead of reloading it (`EffectProcessor::acceptsData`).
- **VST3 chunks** hold the component's and the controller's state.

**Instances and the pool.** Every effect chain that runs a plugin effect has its own instance: preview, export, nested mixes.
- A chain that goes away leaves its instance idle in a pool, per effect (a clip voice leaving the screen, a refill after an
  edit).
- The next chain of the same effect takes it: reset, its blocks numbered on from the old ones, its state reloaded only when
  it differs.
- Plugins are therefore not created again and again during playback, and an open editor stays with the sound it edits.
- At most 32 idle instances are kept, for 60 s.

**Determinism.** Export creates its own instances from the saved state and parameters, gives them the same blocks and
pre-roll, and waits for every block.
- **A deterministic plugin renders export == preview, sample for sample.** Tested with CLAP and VST3 plugins on a clip, a
  track and the master, aligned at lag 0.
- **Plugins that are not deterministic may differ**: random modulation, analogue noise models, internal threads,
  time-of-day licensing or demo noise. This is the same for every host.
- **Edits made in a plugin's own editor** reach export only once their state has been captured into the project (below).

## Editor windows

`PluginEffects::openEditor(effect)` ("Edit plugin..." in Effect Controls) opens the plugin's own GUI in a top-level window
of the helper.
- **Which instance.** The live instance playing that effect, or a new one from its state when nothing plays it.
- **How it is attached.** CLAP embeds it (`set_parent` with the helper's HWND), or opens its floating window. VST3 uses
  `IPlugView::attached` on an HWND.
- **Why a separate window.** A separate top-level window is what both formats support on Windows without reparenting across
  processes. The GUI therefore lives and dies with the helper, never in the editor's process.
- **Edits in the editor.** A VST3 editor's parameter edits reach the processor through the SDK's lock-free
  `ParameterChangeTransfer`. Both formats report state changes (`PluginInstance::stateChanges()`).
- **Tests never open windows:** they use the headless parameter path.

## Commands and MCP

| Command | Capability | What |
|---|---|---|
| `plugins.list {format?, query?, params?}` | Read | The plugins by effect id (name, vendor, version, category, latency, editor, parameters with "params"), the blocklist, the folders |
| `plugins.scan {rescan?, addFolders?, removeFolders?, unblock?, clearBlocklist?}` | MediaAnalysis | Scans as above; blocked files come back as warnings |
| `clip.addEffect {clips, plugin, params}`, `audio.addEffect {target, plugin, params}` | EditSequence | A plugin effect on clips, a track or the master (`plugin`: its effect id, instead of a built-in `effect`) |
| `clip.addKeyframe`, `audio.addKeyframe`, `audio.setEffect`, ... | EditSequence | Its parameters (`p<id>`), like any effect's |

**The assistant** may add and remove plugin effects (`plugins.list` plus the effect commands), but never scans.
`plugins.scan` runs plugin code. It needs MediaAnalysis, which the assistant gets only for its named analysis reads, and MCP
serves it only with `--allow-analysis` (test: `[ai][agent][plugins]`).

**Hosts.** `PluginSystem` (`PluginSystem.h`) puts it all together; the app and wabcut-cli start it.
- It loads the catalog and installs the plugin effects into the engine and the model.
- It provides `audio::edit::PluginServices` for the command engine.
- The app scans in the background at startup. The CLI loads the cache, and `plugins.scan` scans.
- `wabcut-cli render` plays plugin effects.

## Measurements (Core i5-12600K, other builds running)

**Per-instance overhead with 10 instances** of the CLAP test plugin in one chain, 10 s of audio:

| Context | Per instance per 256-frame block | All ten, of real time |
|---|---|---|
| Ahead of playback (each block waited for: one round trip per block) | 4.9–5.9 µs | 0.9–1.1 % |
| Master, pipelined | 3.7–4.6 µs | 0.7–0.9 % |

**Real plugins on this computer**, already installed (nothing was installed for this). Each one scans, instantiates and
processes one second of a tone, with the latency it reports:

| Plugin | Vendor | Version | Parameters | Latency | Editor | µs per 256-frame block |
|---|---|---|---|---|---|---|
| Renegate | Auburn Sounds | 1.3.0 | 33 | 1250 | yes | 98–130 |
| Elgato Compressor | Elgato | 1.0.1.142 | 8 | 0 | yes | 20 |
| Elgato EQ | Elgato | 1.3.0.110 | 49 | 0 | yes | 27 |
| Elgato Noise Removal | Elgato | 1.1.2.119 | 3 | 0 | yes | 111 |
| Marvel GEQ | Voxengo | 1.13 | 36 | 415 | yes | 20–36 |
| RoughRider3 | Audio Damage | 3.1.0 | 13 | 0 | yes | 24–48 |
| T-De-Esser | Techivation | 0.0.4 | 19 | 0 | yes | 77–107 |
| TDR Nova | Tokyo Dawn Labs | 2.1.6 | 75 | 184 | yes | 79–91 |

A scan of each takes 86–195 ms. No CLAP plugin is installed on this computer; the CLAP path is covered by the test plugin.
Run `wabcut_plughost_tests "[.real-plugins]"` to repeat this on any computer.

## Tests (`tests/plughost`, suite `plughost`)

**The test plugins**, built with the suite:
- `WabcutTestGain.clap`: gain, latency, a deliberate crash, a stall.
- Two CLAP files that crash or hang while scanned.
- `WabcutTestGain.vst3`: built like the SDK's AGain example (AudioEffect, EditController, the SDK's factory macros), with
  gain, latency and a crash.

**Protocol:**
- CLAP and VST3 processing with their latency compensated, two plugins in a row;
- sample-accurate automation on the parameter grid (CLAP plain, VST3 normalised);
- state round trips (instance, chunk, project parts and their bound, the instance kept after a capture);
- the missing-plugin path;
- a crash that disables one instance with a badge while the other plugin (another helper) plays on, and a new instance that
  works.

**Timeline:**
- preview == export sample for sample with plugins on a clip, a track and the master, aligned by the delay compensation;
- a stalled master plugin misses deadlines, played dry and flagged, and never waits;
- the overhead with 10 instances.

**Scanner:** the blocklist (crash and hang), the cache, a restart, unblock, rescan, user folders, and real plugins (hidden).

**RT audit:** above.

**CLI:** `wabcut-cli` scans a user folder, lists, puts the plugin on a track and a clip by command, and renders the tone
through both, aligned. A missing plugin is refused, pointing to `plugins.list`.

**Elsewhere:** the commands with a fake library (`audio_edit [plugins]`), and the assistant's tools (`ai [plugins]`).

## Limits and follow-ups

- **AU (macOS) and LV2 (Linux)**, and the helper on those systems.
- **Stereo effects only.** Mono clips are upmixed by the engine; other channels of a wider bus pass dry (above); no
  sidechain inputs or instruments.
- **A master plug-in that changes its latency during an export** keeps the latency it had when the export started
  (OfflineMixdown reads the master's latency once).
- **The master restarts on a loop wrap** (its plug-ins reset there), as on a seek.
- **Speed.** Plugins see the timeline at 100 %: shuttle audition runs the engine's time-stretch after the mix, as for
  built-in effects.
- **Editor state capture** is the UI's to trigger (below). Until it does, edits made in a plugin's own window are heard in
  preview but not exported.

## For the UI engineer

- **Effects panel, the plugin browser.** List `PluginSystem::current()->catalog()->plugins()` (or `plugins.list`) under
  "Audio Plugins", grouped by vendor, with a "Rescan" action (`plugins.scan` / `catalog()->startScan()`), the blocklist with
  "Try again" (`unblock`), and "Add folder..." (`addUserFolder`). Drop a plugin on a clip or track with
  `makePluginEffect(info, id)` or the commands.
- **Effect Controls, "Edit plugin...".** `Plugins.openEditor(effectKey)` (PluginBridge): it opens the plug-in's window from
  a worker and returns at once; errors arrive in `message` (A5-P2-21). The bridge polls the state counter and stores
  changes the same way, off the GUI thread.
  - When `PluginInstance::stateChanges()` moves (poll it, or after the window closes), store
    `effects()->captureState(effect)` with `setPluginState` as an undoable edit. Export then has the editor's changes.
  - The parameters (`p<id>`) show and keyframe like any effect's; their names come from the descriptor.
- **Badges.** `effects()->status(effect)`: Missing (with the saved name), Crashed, Failed, Hung (with the reason), and
  `missedBlocks` (late on the master: worth a warning dot).
- **Waveforms and meters** need nothing new: plugin effects are part of the rendered mix.

# OpenFX video plug-ins (0.7)

Wabcut hosts OpenFX image effect plug-ins on clips, as built-in effects. OpenFX is the open standard of DaVinci Resolve, Vegas
Pro, Nuke and Natron, and many free and paid effects exist for it. The plug-ins run outside the editor, in the
`wabcut-ofxhost` helper, as the audio plugins above do: a plug-in that crashes or hangs disables itself with a badge, never the
editor. Code: `src/ofxhost` (the helper, the wire and the editor side), the hook in `src/playback`
(`playback/video/ExternalEffects.h`, `Evaluate.cpp`), and the UI in `src/app` (`OfxUi`, Effect Controls, the Effects panel).

| | |
|---|---|
| Standard | OpenFX Image Effect API 1.x. The headers are vendored unmodified (OpenFX 1.5.1, BSD-3-Clause; see THIRD-PARTY-NOTICES.md). |
| Platforms | Windows, built with the audio plugin host (`WABCUT_WITH_AUDIO_PLUGINS`). The helper's processes, shared memory and job objects are Win32 for now. |
| Contexts | Filter and General (the "Source" clip and the "Output"; further clips must be optional). |
| Rendering | On the CPU, in the pixels the plug-in asks for: Float, Short (16-bit) or Byte, as RGBA or RGB. |
| Where | The effect stack of a clip of footage or of a still image: `plugin.ofx.<plug-in identifier>` effects, keyframable where the plug-in allows. |
| Tested with | The test plug-ins built in `tests/ofxhost`, written against the raw C API, and (opt-in, `[.sdk]`) the OpenFX SDK's own Invert, Basic, ChoiceParams and Rectangle examples. |

## Using plug-ins

- **Where Wabcut looks.** `%COMMONPROGRAMFILES%\OFX\Plugins` (OpenFX's standard folder), every folder in the `OFX_PLUGIN_PATH`
  environment variable (separated by `;`), and the folders the user added. A plug-in is a `<Name>.ofx.bundle` folder holding
  `Contents\Win64\<Name>.ofx`, or a loose `.ofx` file. The app scans new and changed binaries in the background at startup;
  The Audio Plug-ins tools' Rescan (and `plugins.scan`) scan OpenFX again too, so a plug-in installed while Wabcut runs appears
  without a restart. The same Audio Plug-ins tools show the blocklist; the OpenFX blocklist is listed by `plugins.list`.
- **The Effects panel** lists the usable plug-ins in the folder "OpenFX Plug-ins", grouped by the group each plug-in files itself
  under ("Color", "Blur", ...). Double-click, Enter or a drag onto a clip (or onto Effect Controls) puts the plug-in on the
  selected video clips: one instance each, one undo step ("Add Plug-in"). Plug-ins that cannot run here are not offered (below).
- **Effect Controls** shows the plug-in as a section like any effect's: its parameters as rows, in the order of the plug-in's
  parameter page; the power button, the reset button and removing work as everywhere. Every change is one undo step named
  "<plug-in>: <parameter>"; a drag is one step. Parameters keyframe where the plug-in lets them animate.
- **Saved with the project** as an ordinary effect: its parameters (constants and keyframes) and its text settings. Nothing about
  the plug-in's binary is saved but its identifier and the name it was added with.
- **A plug-in this computer does not have** keeps its section, shown as "<name> (not available)" with a **Missing** badge. The
  effect is kept as it is (saved again unchanged), bypassed, and works again once the plug-in is installed. Removing it is one
  undo step like any effect.
- **Badges.** **Crashed** (with the helper's exit code), **Hung** (a picture took longer than the timeout), **Failed** (the plug-in
  reported an error for three pictures in a row). The layer shows the picture without the effect and nothing else is touched.
  Turning the effect off and on again tries the plug-in again (a new helper).

| OpenFX parameter | In Effect Controls (key) |
|---|---|
| Double | a slider (`name`): degrees for an angle, a percentage for a scale, pixels for a position in pixels; keyframable |
| Integer | a slider in whole steps (`name`); keyframable |
| Boolean | a switch (`name`), not keyframable |
| Choice, string choice | a menu (`name`, the option index), not keyframable |
| RGB, RGBA | one slider per channel, 0 to 100 % (`name.r`, `.g`, `.b`, `.a`); keyframable |
| 2D, 3D (double and integer) | one slider per axis (`name.x`, `.y`, `.z`); a position in normalised coordinates is a percentage of the picture and reaches the plug-in in pixels |
| String | a text row (not keyframable); saved as effect data under the parameter's name |

Hidden (secret) parameters, buttons, groups, pages, custom, bytes and parametric parameters are not offered.

## How a picture is made

```
 decoded frame ─► float RGBA ─► wabcut-ofxhost: effect 1 ─► ... effect n ─► 8-bit RGBA raster ─► compositor
 (CPU planes)     premultiplied   (shared memory both ways)                    (the layer's view)   Motion, opacity,
                                                                                                    built-in effects
```

- **Where it happens.** The evaluator collects a clip's OpenFX effects at the frame's curve time (`LayerSpec::external`: every
  parameter, constants and keyframe curves alike, and the text settings). `buildCompositeLayers` hands the layer's decoded frame
  to the processor (`OfxEffects`), which returns the picture the compositor draws instead of the frame. The player's scheduler
  and export both build their composites with that function, so **preview and export show the same picture**.
- **Order.** The plug-in effects run first, in stack order, on the clip's own picture; then the layer's Motion, opacity and the
  built-in effects, as for any clip. Time Interpolation and the stabilizers process each frame before they mix or warp it.
- **Colour.** A plug-in works on the picture as a colourist sees it: the frame's R'G'B' as the file encodes them (the YCbCr matrix and
  range applied, the transfer function left on), premultiplied when the frame has alpha. The result goes back in the same
  encoding, tagged with the frame's primaries and transfer, so the compositor's colour management reads it as it read the frame.
- **Decoder surfaces.** When the preview keeps frames on the GPU (zero-copy decoding), a CPU copy of the frame is decoded for the
  plug-in (the same file, the same frame).
- **Cache.** Results are cached by the source frame (file, frame, size, colour) and every effect's values: a paused frame, a seek
  back, a loop and the export of a preview do not run the plug-ins again (192 MiB of pictures). A plug-in that says its pictures
  change from frame to frame (noise, time-based effects) is cached per frame of the clip.
- **Threads.** Pictures are made on the scheduler's and the export's own threads, never the GUI thread. A plug-in may use the
  host's multi-thread suite; rows of the conversions run on up to eight threads.

## Out of process, and held to little

- **One helper per plug-in binary**, as for audio: a crash disables that binary's plug-ins only, a heavy plug-in loads once, and
  every instance (one per effect instance in the project, 24 kept) shares it. The helper serves one picture at a time.
- **The wire** (`src/ofxhost/common/include/wabcut/ofxhost/Protocol.h`): JSON lines over the helper's pipes (hello, create, attach,
  render, destroy, quit; each request carries an id its reply repeats) and one shared-memory block per instance holding the
  input and the output picture (float RGBA, premultiplied, top-down). The control channel is the synchronisation.
- **Limited rights (crash isolation, not a security boundary).** The helper runs with your user rights, in a Windows job object of its own, so a plug-in that crashes or hangs cannot take the editor down; install only plug-ins you trust. It runs in the job object: it cannot start other processes, use the clipboard,
  change display settings or log off, and is capped at 6 GiB of committed memory; it is ended with the editor. It starts with a
  minimal environment (no API keys), in its own folder, with a restricted DLL search order, and its standard output and error go
  to a log, never into the control channel. What it sends back is parsed defensively and bounded; a plug-in's own text is cut to
  its limits.
- **Timeouts.** A picture may take 4 s where it is played and 60 s in an offline render (export, Render In to Out); past it the
  instance counts as hung and the helper is ended. A crash is noticed the moment the helper's pipe closes.
- **Scanning.** Every binary is scanned by its own `wabcut-ofxhost --scan`; one that crashes or hangs (30 s) goes on the blocklist
  with the reason and is skipped until it changes. The cache is `%LOCALAPPDATA%\Wabcut\ofx-plugins.json`
  (`WABCUT_OFX_CACHE` overrides it for tests and scripts).

## Not supported yet

What a plug-in needs that this version does not have. The host tells plug-ins what it offers (its host properties), and a
plug-in that cannot work without it is listed as unusable here, with the reason:

- **GPU rendering**: the OpenGL, CUDA, Metal and OpenCL render suites. A plug-in that only renders on the GPU ("needed") is not
  offered; one that can also render on the CPU does.
- **On-screen controls**: overlays and interacts (handles in the Program Monitor), and the draw suite.
- **Parameters**: custom, parametric (curves), bytes and push-button parameters; parameter pages and groups only decide the order.
  A plug-in sees no keyframes of its own (Wabcut evaluates the curves and sends the values at each frame's time), and a value a
  plug-in sets itself is not saved to the project.
- **Time**: a plug-in always gets the current frame (no temporal clip access, no neighbouring frames), and the Generator, Paint,
  Retimer and Transition contexts are not run.
- **Inputs**: only the "Source" clip; optional extra clips (a mask) are not connected, and a plug-in with a required second
  input is unusable.
- **Tiles and multi-resolution**: the whole picture is rendered at the size it is given (a preview at reduced resolution gives
  plug-ins the reduced picture, so a radius in pixels looks larger there; positions as fractions of the picture do not).
- **HDR** (PQ, HLG) pictures are not given to plug-ins (the effect is bypassed); the result is 8-bit into the compositor.
  Pictures over 4096 x 4096 pixels are bypassed too.
- **Where**: clips of footage and still images only. Titles, graphics, generated media (colour mattes, bars), nested clips, track
  and master stacks bypass an OpenFX effect (the badge counts it as bypassed). A plug-in sees the stored picture, before the
  display rotation of phone footage.
- **Other**: the colour-space, dialog and progress suites (progress calls are accepted and ignored), AU and LV2 audio plugins
  (macOS and Linux follow-ups), Linux and macOS builds of the helper, a folder dialog and a blocklist view for OpenFX (use
  `OFX_PLUGIN_PATH`, or `plugins.scan` with `addFolders`, `unblock`).

## Commands and MCP

OpenFX plug-ins are part of the plugin commands, next to the audio ones:

| Command | What |
|---|---|
| `plugins.list {format?, query?, params?}` | `format: "openfx"` lists the OpenFX plug-ins: effect id `plugin.ofx.<id>`, name, version, the group as the vendor, the reason when one cannot be used here, and with "params" the effect parameters (keys as above, ranges, whether they can be keyframed); the blocklist and folders cover both kinds |
| `plugins.scan {rescan?, addFolders?, removeFolders?, unblock?, clearBlocklist?}` | scans the OpenFX folders with the audio ones: the same options, one summary |
| `clip.addEffect {clips, plugin, params}` | `plugin: "plugin.ofx.<id>"` puts a plug-in on video clips, with parameter values by effect key |
| `clip.addKeyframe`, `clip.setEffect`, ... | its parameters, and its text settings (`data`), like any effect's |

The assistant may list and add OpenFX plug-ins like audio ones, but never scans (`plugins.scan` runs plug-in code).

## Tests (`tests/ofxhost`, suite `ofxhost`; `tests/app/test_ofx_effect_controls.cpp`)

The test plug-ins, built from one source (`plugins/TestOfx.cpp`) into bundles under `<build>/test-ofx`: `WabcutTestOfx.ofx` (a
gain with every parameter kind, 8-bit RGBA and 16-bit RGB inverters, a plug-in that always fails, one that needs OpenGL, one that
needs two inputs, a General-context plug-in), a binary that crashes while it renders, one that never returns from a render, and
one that crashes while it is described.
- **Scan:** bundles found, parameters in page order, hidden ones left out, descriptors (ranges, menus, text settings), plug-ins that
  cannot run listed with the reason, the blocklist (crash while described), the cache and a restart, the folders.
- **Render:** every parameter kind reaches the plug-in, the picture keeps its orientation, Byte and Short and RGB pictures,
  identity, a larger picture than the block was made for, a failing plug-in.
- **Isolation:** a crash while rendering (exit code 0xC0000005) disables that instance while the editor and another plug-in's
  helper carry on, a hang ends after the timeout, the offline timeout is longer.
- **Effects:** a decoded frame becomes the picture the compositor draws (YUV and RGBA, straight alpha), the cache, the order of
  effects, bypassed effects, HDR refused, badges (missing, crashed, failed) and retrying.
- **Pipeline:** from a project through the evaluator to the composite layer, keyframes included.
- **App:** Effect Controls rows and undo steps, the Effects panel and applying, a missing plug-in, a crashed one.
