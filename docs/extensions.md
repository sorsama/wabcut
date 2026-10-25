# Extensions

Extensions add commands to Wabcut in JavaScript: a folder with a manifest and scripts that run over the same typed
command protocol as the AI assistant, `wabcut-cli` and the MCP server (docs/commands.md). They are Wabcut's answer to
Premiere's UXP panels and Resolve's scripting, and they are built for untrusted code:
- each runs in its own sandboxed process;
- it gets only the capabilities the user grants;
- every edit it makes is an undo step named after it.

Three samples come with the source (`src/script/samples/`):

| Sample | Command | Needs |
|---|---|---|
| `beat-markers` | Add Markers at Every Beat: a marker at every beat or downbeat of the beat grid, setting the tempo first when there is none | Read, EditSequence |
| `transcript-names` | Rename Clips from Transcript: clips named after the first words heard in them (video clips through their linked audio) | Read, EditSequence |
| `youtube-chapters` | YouTube Description from Chapters: every sequence's chapter markers as description text, shown read-only to copy | Read |

The code is in `src/script` (the `wabcut_script` library, the `wabcut-scripthost` helper). The app's panels are
described in `docs/internal/c12-extensions-handoff.md`.

## Quick start

```
beat-markers/
  wabcut-extension.json
  main.js
```

```json
{
  "manifestVersion": 1,
  "id": "com.example.beat-markers",
  "name": "Beat Markers",
  "version": "1.0.0",
  "author": "Example",
  "licence": "MIT",
  "minWabcutVersion": "0.1.0",
  "capabilities": ["Read", "EditSequence"],
  "main": "main.js",
  "commands": [{"id": "addBeatMarkers", "title": "Add Markers at Every Beat"}]
}
```

```js
wabcut.ui.registerCommand('addBeatMarkers', 'Add Markers at Every Beat', (args) => {
    const beats = wabcut.project.query('sequence.beats', {limit: 100000});
    const time = beats.beatFields.indexOf('time');
    wabcut.commands.batch(beats.beats.map((b) => ({command: 'marker.add', params: {time: b[time]}})),
                          {label: 'Add Markers at Every Beat'}); // one undo step: "Beat Markers: Add Markers at Every Beat"
    return {markers: beats.beats.length};
});
```

Try it without the app:

```
wabcut-cli ext validate beat-markers
wabcut-cli ext run beat-markers addBeatMarkers film.wabcut --grant-requested --save
```

In the app the extension is installed from its folder; the user reviews what it asks for and enables it, and its
commands appear in the command palette.

## The manifest

`wabcut-extension.json`, at most 64 KiB and 8 levels deep. Every field is checked; a field that fails is named in the
error. Keys the parser does not know are ignored with a warning, so a newer manifest still loads.

| Field | Required | Rules |
|---|---|---|
| `manifestVersion` | yes | `1` |
| `id` | yes | lower-case reverse-DNS, at most 100 characters: `com.example.beat-markers` |
| `name` | yes | 1 to 64 bytes. Shown in the extension manager and in undo labels. No control characters and nothing that draws nothing or rearranges the text around it: bidirectional marks, overrides and isolates, joiners, soft hyphens, line and paragraph separators, variation selectors, tags, private-use characters (nothing that could pass for another name in the permission dialog). It may not start with, or imitate, a reserved name: Wabcut, Adobe, Premiere, After Effects, DaVinci, Blackmagic, Final Cut, Media Composer, Microsoft, Anthropic (case, fullwidth forms, accents, look-alike Cyrillic and Greek letters, digits and spacing do not matter) |
| `version` | yes | semver `1.2.0` or `1.2.0-beta.1` (no build metadata) |
| `author` | yes | 1 to 128 bytes |
| `licence` (or `license`) | yes | SPDX identifier or free text, at most 64 bytes |
| `description` | no | at most 2000 bytes; may break lines |
| `minWabcutVersion` | yes | semver; an older Wabcut refuses the extension |
| `capabilities` | yes | what it asks for, each once: `Read`, `EditSequence`, `EditProject`, `MediaAnalysis` (may be empty). `Network` is refused |
| `main` | yes | the entry script |
| `scripts` | no | up to 31 more scripts, evaluated before `main` in this order (they share the global scope) |
| `commands` | no | up to 64 `{"id", "title"}`: palette commands known before the extension starts (ids `[A-Za-z][A-Za-z0-9._-]*`, titles up to 100 bytes) |

Script paths are relative, `/`-separated, end in `.js`, and have no `.` or `..` segments. Each script is at most 1 MiB
of UTF-8 without NUL bytes, and the whole extension at most 4 MiB. A script that is a symbolic link, junction or other
reparse point is refused, so an extension can't name a file outside its folder.

## Where extensions live

| What | Where |
|---|---|
| Installed extensions | `<app data>/extensions/<id>/` (`%APPDATA%\Wabcut\Wabcut` on Windows). Installing copies the manifest and the scripts it names, nothing else |
| Per-project extensions | `<project folder>/.wabcut/extensions/<id>/`. They come with a shared project, so they are untrusted input |
| Grants and trust | `<app data>/extension-permissions.json` |
| Each extension's storage | `<app data>/extension-storage/<id>.json` |

A per-project extension never takes the place of an installed one with the same id; it is listed as invalid.
`WABCUT_DATA_DIR` moves `<app data>` for `wabcut-cli` (tests, automation).

## Permissions and trust

The install or enable dialog lists what the extension asks for:

| Capability | Lets the extension |
|---|---|
| Read | See the project's sequences, clips, markers, media names, transcripts and captions |
| EditSequence | Change clips, markers, captions, effects and audio inside sequences; each change is one undo step named after the extension |
| EditProject | Add and change tracks, bins, media, subclips and sequences |
| MediaAnalysis | Run analysis and AI commands (transcription, search, translation, dubbing). Cloud AI still asks for the user's consent and stays within the spend cap and no-cloud settings |
| Network | Never granted: extensions have no network access of their own |

The rules:
- **Grants stay within the request.** The user grants some or all of what the manifest asks for, never more. A running
  extension gets: granted AND requested AND grantable.
- **Grants hold for what was reviewed.** Each grant records the content hash (the SHA-256 of the manifest and every
  script), the author and the folder the user reviewed. A new version, another extension installed over it, or a
  copy elsewhere claiming the same id gets nothing and does not run until the user reviews it ("needs review"). The
  dialog ticks what was granted before, except when the author changed: then nothing is ticked, the dialog says so,
  and the extension starts with empty storage. Grants recorded before this rule are asked for again once.
- **Installing runs nothing.** `install()` copies the files; the extension starts only once the dialog's `enable()`
  records what the user chose.
- **Updates.** A reviewed version that asks for less than was granted gets only what it asks for.
- **AI commands.** Commands that run an AI model (`CommandSpec::runsModel`, listed below) need MediaAnalysis on top of
  their own capability. They go through the editor's own services, so the consent prompt, the spend cap and the
  project's no-cloud setting apply exactly as for the user. An extension can't give consent.
- **Revoke.** Revoking takes every grant back, stops the extension and disables it ("permissions revoked"). Its
  storage stays until it is uninstalled.
- **Per-project trust.** A per-project extension also needs the user's trust for that project folder and that exact
  content (the SHA-256 of the manifest and every script). When a teammate changes a file, the trust lapses and the
  user is asked again. A copy of the project in another folder starts untrusted.
- **Crashes.** Three crashes in a row (a crash, the memory cap, a hang that had to be killed, a failed load) disable
  the extension with a reason. Enabling it again resets the count.

## The runtime

Each extension runs in its own `wabcut-scripthost` process, the way audio plugins (`wabcut-plughost`) and AI models
(`wabcut-infer`, `wabcut-embed`) do. A script that loops, runs out of memory or crashes the engine takes down only that
process.

- **The engine** is Qt's QJSEngine (V4). No other script runtime is used.
  - It supports modern JavaScript up to optional chaining, `??`, generators, Promises and Unicode regular expressions.
  - It does not support `async` / `await`, BigInt, class fields or `import`. Chain Promises with `.then()`. Scripts are
    classic scripts sharing one global scope.
  - `globalThis` is provided.
- **No file, network or process API.** The global scope holds the ECMAScript built-ins, `wabcut` and `console`, and
  nothing else: no `require`, `fetch`, `XMLHttpRequest`, `Qt`, timers or modules. The scripts reach the helper as text
  in its first message, so the helper never opens a file.
- **The OS sandbox (Windows).** The helper runs in a job object that:
  - caps its committed memory (512 MiB by default);
  - lets it start no other process;
  - denies it the clipboard, desktop switching, global atoms, system settings and other processes' windows;
  - ends it with the editor.

  The helper is not started at all if these limits can't be set up.
- **Time.** Every call (a command handler, an event listener, loading the scripts) gets 5 s of script time. JavaScript
  reached from any event is covered, promise reactions included. Time spent waiting for the editor (a command running)
  or for the user (a prompt) does not count.
  - Past the limit, a watchdog thread interrupts the engine. This cannot be caught, and the call ends with
    `timeLimit`; the extension keeps running.
  - If the helper does not answer within the limit plus a 2 s grace, the editor cancels the call. Only progress moves
    that deadline: a request to the editor or an answered prompt. Log lines, storage writes, notifications and pongs
    never do, so a helper cannot keep a call alive by chattering.
  - Whatever it asks the editor meanwhile, a call never runs longer than 6 times the limit in all (30 s;
    `kCallHardLimitFactor`). Time the user spends on its prompts and model commands it waits for (below) do not count.
  - If the helper still does not answer after another grace, the editor kills it.
- **The editor never waits for a script.** The editor's side (`ExtensionHost`, `ExtensionManager`) belongs to the GUI
  thread, like the command engine. Calls into an extension are asynchronous, and a watcher thread hands the helper's
  messages to `pump()`. Lines to the helper go through a writer thread, so a helper that stops reading can't block
  the editor; more than 64 MiB unread ends it.
- **Requests the editor runs.** A request runs in the editor for at most one call's script time (5 s): a longer batch
  stops at its next command with `TimedOut`, and nothing is committed. A command that runs an AI model never runs
  inside the editor's message loop: the app runs it as a background job, with progress and Cancel
  (`ManagerDelegate::runLongCommand`, given the extension's own grants and undo labels), and the call waits for it.
  Where nothing runs such jobs, the request fails with `PermissionDenied`. `wabcut-cli ext run` runs it in line.
- **Rates.** Requests to the editor: 100 a second, bursts of 400; past that a request fails with `RateLimited`.
  Storage writes: 100 a second, bursts of 1,000; past that they are dropped (with a warning in the log). Each write
  costs what it stores: the editor keeps a running total instead of measuring the whole store again.
- **Edits.** Everything an extension changes goes through `CommandEngine`, with its schemas, references, validation
  and locked-track checks. Each request or batch is one undo step labelled `<extension name>: <title>`
  (`EnginePolicy::undoLabelPrefix`).

## API reference

The `wabcut` object and everything in it are frozen.

### `wabcut.version`, `wabcut.extension`

The running Wabcut's version, and `{id, name, version, capabilities}` (the capabilities granted).

### `wabcut.commands`

| Call | Returns |
|---|---|
| `run(name, params = {}, {dryRun, expectedRevision, idempotencyKey, version})` | The command's result: `{revision, changes, createdIds, warnings, data, dryRun, committed}` (docs/commands.md, "Results") |
| `batch([{command, params, version}], {label, dryRun, expectedRevision, idempotencyKey})` | One result for the whole batch, with `steps`: all of the commands or none, one undo step |
| `list()` | Every command as `{name, version, title, capability, runsModel, allowed}`; `allowed` says whether this extension may run it |
| `describe(name)` | The same plus `description`, `params` (JSON Schema), `example` and `aliases` |

A failed command throws a `wabcut.CommandError` with `code` (`PermissionDenied`, `InvalidParams`, `NotFound`,
`EditRejected`, ...; docs/commands.md) and `details`. A command refused because it runs an AI model has
`details.runsModel === true`. Calls are synchronous: the script waits while the editor runs the command, and that wait
does not count against its time limit (the call's overall limit still applies; see "Time"). The editor runs one
request for at most 5 s (`TimedOut` past that, nothing committed); a command that runs an AI model runs as a
background job of the app while the script waits. A request is at most 2 MiB; split larger batches.

### `wabcut.project`

| Call | Returns |
|---|---|
| `summary()` | `project.summary`'s data: the sequences (id, name, rate, length, tracks, clips) and media counts |
| `query(name, params = {})` | The data of a read command (`sequence.summary`, `sequence.beats`, `transcript.words`, ...); a command that is not a read command is refused with a TypeError |

### `wabcut.events`

`on(kind, handler)` returns a function that removes the handler; `off(kind, handler)` does the same. A handler gets
`(data, {coalesced})`.

| Kind | Data |
|---|---|
| `selectionChanged` | `{sequence, clips: [ids], markers: [indices]}` |
| `playheadMoved` | `{sequence, time (ticks), timecode}` |
| `projectSaved` | `{file}`: the file's name (`film.wabcut`), never its folder |
| `commandApplied` | `{label, kind: "edit" \| "undo" \| "redo" \| "reset", revision}`: any change to the project, the extension's own included |

Events go only to an extension granted Read: without it, `on()` has no effect (the editor ignores the subscription and
logs why). The editor passes each kind on at most every 100 ms (saves every 250 ms), the latest value winning;
`coalesced` says how many were folded into it. Only kinds the extension listens to are sent. A listener that throws is
logged; the others still run.

### `wabcut.ui`

| Call | |
|---|---|
| `registerCommand(id, title, handler)` | A palette command (`ext:<extension id>/<command id>`, assignable to a shortcut). `handler(args)` returns a JSON value or a Promise of one; the caller gets it. At most 64 |
| `notify(message, {level})` | A notification: `info`, `warning` or `error`, at most 2000 bytes |
| `toast(message)` | A short toast, at most 300 bytes |
| `prompt(form)` | Shows a form; a Promise of the answers, or `null` when cancelled. Only while a command the user ran is running (never at load or from an event listener alone), at most 5 a minute and 4 open at a time; any other prompt gets `null` without being shown |

Notifications and toasts are limited to 30 a minute. Text an extension shows is cleaned of control characters and of
everything the manifest's `name` may not hold (bidirectional marks and overrides, invisible characters, line and
paragraph separators, variation selectors, tags, private-use characters), so it can't pose as the editor's own text.

A form is:

```js
{
    title: 'Add Markers at Every Beat',      // required, at most 100 bytes
    message: 'The sequence has no beat grid yet.', // optional, at most 2000 bytes, may break lines
    submitLabel: 'Add Markers', cancelLabel: 'Cancel',
    fields: [                                 // at most 16
        {id: 'every', type: 'choice', label: 'Mark', value: 'beat',
         options: [{value: 'beat', label: 'Every beat'}, {value: 'downbeat', label: 'Every downbeat'}]},
        {id: 'bpm', type: 'number', label: 'Tempo', value: 120, min: 20, max: 300, step: 1},
        {id: 'name', type: 'text', label: 'Prefix', value: '', maxLength: 40, placeholder: 'Beat'},
        {id: 'out', type: 'text', label: 'Result', value: '0:00 Intro', multiline: true, readOnly: true},
        {id: 'all', type: 'checkbox', label: 'Every sequence', value: false},
    ],
}
```

The answers are an object of field values; the editor checks them against the form (a number within its bounds, a
choice among its options, text within `maxLength`). A read-only text field shows a result to copy and keeps its value.

### `wabcut.storage`

`get(key, fallback)`, `set(key, value)`, `remove(key)` (true if it was there) and `keys()`. The values are JSON, and
`set` throws a RangeError past these bounds:
- 256 keys of at most 128 bytes;
- 64 KiB per value;
- 1 MiB in all.

The editor keeps the store in the extension's storage file and checks the bounds again. It takes at most 100 writes a
second (bursts of 1,000) and drops the rest with a warning in the log, so the file keeps what was written before.

### `console`

`log`, `debug`, `info`, `warn` and `error` go to the extension's log: the manager panel shows it, and the CLI prints
it. The limit is 200 lines a second, each at most 4000 bytes.

### Limits

| Limit | Default |
|---|---|
| Script time per call | 5 s (`HostLimits::callTime`) |
| A whole call, waits for the editor included | 6 x the script time: 30 s (`kCallHardLimitFactor`) |
| Grace before cancel, then kill | 2 s each |
| One request run by the editor | the script time (5 s); AI model commands run as a background job instead |
| Requests / storage writes | 100 a second each, bursts of 400 / 1,000 |
| Loading | the scripts within one call's time; ready within 15 s |
| Memory per extension process | 512 MiB (Windows job object) |
| Message from the helper | 2 MiB; a handler's result 1 MiB |
| Unread lines from the helper | 10,000 |
| Registered commands / open prompts / prompts / notifications | 64 / 4 / 5 a minute, during a user's command / 30 a minute |
| Log kept by the editor | the last 500 lines |

## Commands by capability

<!-- BEGIN GENERATED (wabcut_command_docgen): extension commands -->

An extension can run a command when it was granted the command's capability. Commands marked *(AI)* run an AI model (locally or in the cloud) and need MediaAnalysis as well; the editor's consent, spend cap and no-cloud settings still apply to them. Commands marked *(files)* read or write files their parameters name: extensions never run them. Network is never granted.

| Capability | Commands |
|---|---|
| Read (81) | `audio.channels`, `audio.effects`, `audio.routing`, `brand.get`, `captions.checkSpelling`, `captions.export` *(files)*, `captions.find`, `captions.get`, `captions.presets`, `clip.details`, `clip.find`, `clip.openNested`, `clipboard.copy`, `clipboard.get`, `color.grade.get`, `color.inputSpace.get`, `color.management.get`, `effect.list`, `effects.exportPresets` *(files)*, `effects.library`, `export.describe`, `export.preset.export` *(files)*, `export.preset.import` *(files)*, `export.presets`, `export.queue` *(files)*, `export.queue.list`, `export.quick` *(files)*, `export.watchFolder.add` *(files)*, `export.watchFolder.list`, `freeform.get`, `graphic.checkSpelling`, `graphic.get`, `graphic.resolve`, `history.diff`, `history.list`, `interchange.export` *(files)*, `layout.list`, `llm.local.models`, `llm.local.status`, `lut.export` *(files)*, `lut.list`, `marker.list`, `mask.list`, `media.browse` *(files)*, `media.fileMetadata`, `media.layeredFileInfo` *(files)*, `media.list`, `media.rawSourceSettings`, `multicam.angles`, `plugins.list`, `project.summary`, `provenance.report`, `record.takes`, `redaction.list`, `render.status`, `review.list`, `scopes.read`, `search.footage` *(AI)*, `sequence.beats`, `sequence.exportMarkers`, `sequence.goToGap`, `sequence.presets`, `sequence.reverseMatchFrame`, `sequence.setPatches`, `sequence.settings`, `sequence.summary`, `shorts.find` *(AI)*, `source.load`, `spelling.check`, `takes.find` *(AI)*, `template.export` *(files)*, `template.list`, `template.resolve`, `textStyle.list`, `transcript.corrections`, `transcript.export` *(files)*, `transcript.find`, `transcript.review`, `transcript.speakers`, `transcript.text`, `transcript.words` |
| EditSequence (192) | `adjustment.add`, `analysis.detectBeats`, `analysis.detectShots`, `audio.addEffect`, `audio.addKeyframe`, `audio.autoDuck`, `audio.autoTag`, `audio.essentialSound` *(AI)*, `audio.fillChannel`, `audio.masterLimiter`, `audio.moveEffect`, `audio.moveKeyframe`, `audio.normalize`, `audio.remix`, `audio.removeEffect`, `audio.removeKeyframe`, `audio.setChannelVolume`, `audio.setChannels`, `audio.setEffect`, `audio.splitToMono`, `audio.syncClips`, `autozoom.apply`, `captions.addCue`, `captions.addTrack`, `captions.applyPreset`, `captions.findReplace`, `captions.generate`, `captions.generateFromTranscript`, `captions.hideProfanity`, `captions.import` *(files)*, `captions.mergeCues`, `captions.remove`, `captions.removeCue`, `captions.setStyle`, `captions.setTrack`, `captions.setTrackStyle`, `captions.shiftCues`, `captions.splitCue`, `captions.translate` *(AI)*, `captions.updateCue`, `captions.updateFromTranscript`, `cdl.import` *(files)*, `clip.addEffect`, `clip.addKeyframe`, `clip.enable`, `clip.fade`, `clip.frameHold`, `clip.frameHoldOptions`, `clip.group`, `clip.insert`, `clip.instantReplay`, `clip.link`, `clip.move`, `clip.moveEffect`, `clip.moveKeyframe`, `clip.overwrite`, `clip.pasteAttributes`, `clip.removeAttributes`, `clip.removeEffect`, `clip.removeKeyframe`, `clip.rename`, `clip.replace`, `clip.rippleTrim`, `clip.roll`, `clip.scaleToFrameSize`, `clip.setAnimated`, `clip.setBlendMode`, `clip.setColor`, `clip.setEffect`, `clip.setFade`, `clip.setFieldOptions`, `clip.setKeyframe`, `clip.setMotion`, `clip.setParam`, `clip.setTimeInterpolation`, `clip.setToFillFrame`, `clip.setToFrameSize`, `clip.shake`, `clip.slide`, `clip.slip`, `clip.speedDuration`, `clip.splitAtShots`, `clip.trim`, `clip.ungroup`, `clip.unlink`, `clip.zoomPunch`, `clipboard.cut`, `clipboard.paste`, `color.auto`, `color.grade.copy`, `color.grade.set`, `color.inputSpace.set`, `color.match`, `color.matchScene`, `edit.clear`, `edit.cutToBeats`, `edit.insertFromSource`, `edit.overwriteFromSource`, `edit.snapToBeats`, `effect.analyzeRollingShutter`, `effect.analyzeStabilization`, `effects.applyPreset`, `graphic.add`, `graphic.addLayer`, `graphic.align`, `graphic.applyPreset`, `graphic.removeLayer`, `graphic.setRoll`, `graphic.setTextPath`, `graphic.update`, `graphic.updateLayer`, `highlights.find`, `keyframes.move`, `keyframes.setInterpolation`, `layout.apply`, `marker.add`, `marker.clearAll`, `marker.clearInAndOut`, `marker.markIn`, `marker.markOut`, `marker.remove`, `marker.update`, `mask.add`, `mask.objectSelect` *(AI)*, `mask.propagate` *(AI)*, `mask.refine` *(AI)*, `mask.remove`, `mask.update`, `multicam.autoSwitch` *(AI)*, `multicam.flatten`, `multicam.setSync`, `multicam.switchAngle`, `record.useTake`, `review.toMarkers`, `roughcut.swapTake`, `sequence.addEdit`, `sequence.addEditToAllTracks`, `sequence.addSubmixTrack`, `sequence.applyAudioTransition`, `sequence.applyDefaultTransitions`, `sequence.applyVideoTransition`, `sequence.closeGap`, `sequence.extract`, `sequence.joinThroughEdits`, `sequence.lift`, `sequence.removeDeadTime`, `sequence.rippleTrimNextToPlayhead`, `sequence.rippleTrimPreviousToPlayhead`, `sequence.setBeatGrid`, `sequence.setMasterFormat`, `sequence.setMasterGain`, `sequence.setTargets`, `sequence.unnest`, `takes.use` *(AI)*, `template.instantiate`, `template.setField`, `textStyle.apply`, `textStyle.create`, `timeRemap.addHold`, `timeRemap.addKey`, `timeRemap.addReverse`, `timeRemap.enable`, `timeRemap.moveKey`, `timeRemap.removeKey`, `timeRemap.setInterpolation`, `timeRemap.setKey`, `timeRemap.setSpeed`, `timeline.removeSilences`, `timeline.rippleDelete`, `title.add`, `title.setFont`, `title.setText`, `track.addSend`, `track.mask`, `track.removeSend`, `track.setFormat`, `track.setOutput`, `track.setPan`, `track.setRecordArm`, `track.setRole`, `track.setSend`, `track.setState`, `track.setSurroundPan`, `track.setVolume`, `track.transform`, `transcript.cleanup`, `transcript.deleteRanges`, `transcript.keepRanges`, `transcript.moveRange`, `transition.add`, `transition.remove`, `transition.set` |
| EditProject (118) | `audio.bleepProfanity`, `audio.enhanceSpeech` *(AI)*, `audio.remixRender`, `audio.separateStems` *(AI)*, `audio.setMediaChannels`, `bin.create`, `bin.createSearch`, `bin.move`, `bin.remove`, `bin.rename`, `bin.setSearch`, `brand.set`, `captions.pushTrackStyle`, `captions.removePreset`, `captions.savePreset`, `cdl.export` *(files)*, `color.management.set`, `dub.create` *(AI)*, `effects.addToCustomBin`, `effects.deleteCustomItem`, `effects.importPresets` *(files)*, `effects.newCustomBin`, `effects.newPresetsBin`, `effects.renameCustomItem`, `effects.savePreset`, `effects.setFavorite`, `effects.setPresetProperties`, `export.preset.delete`, `export.preset.save`, `export.queue.cancel`, `export.queue.pause`, `export.queue.remove`, `export.queue.resume`, `export.queue.start`, `export.watchFolder.remove`, `freeform.alignToGrid`, `freeform.removeLayout`, `freeform.resetToGrid`, `freeform.restoreLayout`, `freeform.saveLayout`, `freeform.set`, `gen.broll` *(AI)*, `gen.extend` *(AI)*, `gen.image` *(AI)*, `gen.sfx` *(AI)*, `highlights.toReel`, `highlights.toShort`, `history.checkpoint`, `history.export` *(files)*, `history.name`, `history.restore`, `lottie.import` *(files)*, `lut.import` *(files)*, `media.duplicate`, `media.importCameraClips` *(files)*, `media.importImageSequence` *(files)*, `media.importLayeredFile` *(files)*, `media.interpretFootage`, `media.makeOffline`, `media.makeSubclip`, `media.mergeClips`, `media.newSynthetic`, `media.readXmp`, `media.replaceFootage` *(files)*, `media.setAttributes`, `media.setRawSourceSettings`, `media.setScaling`, `media.setSubclip`, `media.setTimecode`, `multicam.create`, `narration.create` *(AI)*, `project.importItems` *(files)*, `project.merge` *(files)*, `redaction.add`, `redaction.detectFaces` *(AI)*, `redaction.override`, `redaction.remove`, `redaction.update`, `reframe.createSequence` *(AI)*, `reframe.rerun` *(AI)*, `review.add`, `review.exportPackage` *(files)*, `review.importPackage` *(files)*, `review.remove`, `review.reply`, `review.resolve`, `roughcut.fromScript` *(AI)*, `sequence.addTrack`, `sequence.create`, `sequence.duplicate`, `sequence.exportFrame` *(files)*, `sequence.makeSubsequence`, `sequence.makeVersion`, `sequence.matchFrame`, `sequence.nest`, `sequence.newFromClip`, `sequence.removeTrack`, `sequence.setSettings`, `shorts.create` *(AI)*, `source.addMarker`, `source.clearMarks`, `source.exportFrame` *(files)*, `source.removeMarker`, `source.setMarks`, `source.updateMarker`, `template.import` *(files)*, `template.save`, `textStyle.remove`, `textStyle.rename`, `textStyle.update`, `transcript.addSpeaker`, `transcript.assignSpeaker`, `transcript.correctRange`, `transcript.correctWord`, `transcript.findReplace`, `transcript.mergeSpeakers`, `transcript.renameSpeaker`, `workflow.podcast` *(AI)* |
| MediaAnalysis (12) | `analysis.speakers` *(AI)*, `analysis.subjects` *(AI)*, `audio.channelLevels`, `audio.loudness`, `highlights.importChat` *(files)*, `llm.local.start` *(AI)*, `llm.local.stop`, `plugins.scan` *(files)*, `render.deletePreviewFiles`, `render.previewInOut`, `search.indexMedia` *(AI)*, `transcript.transcribe` *(AI)* |
| Network (never granted) (0) | none in the catalog |

The 29 commands that run an AI model: `analysis.speakers`, `analysis.subjects`, `audio.enhanceSpeech`, `audio.essentialSound`, `audio.separateStems`, `captions.translate`, `dub.create`, `gen.broll`, `gen.extend`, `gen.image`, `gen.sfx`, `llm.local.start`, `mask.objectSelect`, `mask.propagate`, `mask.refine`, `multicam.autoSwitch`, `narration.create`, `redaction.detectFaces`, `reframe.createSequence`, `reframe.rerun`, `roughcut.fromScript`, `search.footage`, `search.indexMedia`, `shorts.create`, `shorts.find`, `takes.find`, `takes.use`, `transcript.transcribe`, `workflow.podcast`.

<!-- END GENERATED -->

## Headless automation

```
wabcut-cli ext run <extension> <command> <project.wabcut> [--grant <capabilities> | --grant-requested] [--trust]
                   [--args '<json>'] [--answers '<json array>'] [--save] [--timeout <s>]
wabcut-cli ext validate <folder>
wabcut-cli ext list [<project.wabcut>]
```

`ext run` starts the extension on the project, runs one of its commands with `--args`, and prints a JSON report.

- **The extension** is a folder, or the id of an installed one (or of one in the project's `.wabcut/extensions`).
- **Grants.**
  - `--grant Read,EditSequence` grants within what the manifest asks for.
  - `--grant-requested` grants all of it.
  - Without either, the grants given in Wabcut apply, and only to the installed folder the user reviewed, with the
    same files. A folder elsewhere claiming the same id gets none (exit 3); an extension without any is refused.
  - Only the installed folder reads and writes the extension's storage; any other folder starts with empty storage.
- **Trust.** A per-project extension also needs the user's trust from Wabcut, or `--trust`.
- **Prompts** are answered from `--answers`, in the order they open: an object of field values, or `null` to cancel.
  The ones past the end take their forms' defaults.
- **Saving.** Nothing is saved without `--save`.
- **Exit codes:**
  - 0: the command succeeded;
  - 1: it failed;
  - 2: usage;
  - 3: the extension could not run (not found, not granted or trusted, crashed).

```json
{
  "ok": true,
  "result": {"markers": 8, "bpm": 120.0},
  "notifications": [{"level": "toast", "message": "8 beat markers added"}],
  "prompts": [],
  "log": [],
  "revision": {"before": 0, "after": 2},
  "undo": ["Beat Markers: Set Beat Grid", "Beat Markers: Add Markers at Every Beat"],
  "extension": {"id": "com.wabcut.samples.beat-markers", "name": "Beat Markers", "version": "1.0.0", "scope": "user",
                "granted": ["Read", "EditSequence"]},
  "saved": true
}
```

`ext validate` prints the checked manifest, the files, their size and the content hash, or the manifest's issues.
`ext list` prints the installed extensions (and a project's) with their grants and status.

## Security model

**Assume both the code and the helper are hostile.** Extension code is untrusted, whether it was downloaded or came
with a project. The helper process that runs it is treated as possibly compromised (for example by an engine bug).
So the editor decides everything that matters itself:
- **Messages.** Every line from the helper is checked for shape and size (`parseHostMessage`). A broken one ends the
  helper.
- **Grants.** Commands run through the editor's own engine, limited to the grants it holds: a forged request gets
  `PermissionDenied`.
- **Other checks.** Storage bounds, prompt forms and answers, notification rates and the size of what it reads are
  checked on the editor's side as well.
- **Text.** What an extension shows is sanitised, and its name may not start with or imitate a reserved one, so it
  can't pose as the editor.
- **Time and rates.** A call's deadline moves only with progress, a call has an overall limit, a request runs in the
  editor for at most the script time, model commands run outside the message loop, and requests, storage writes,
  prompts and notifications are rate-limited, so an extension can't freeze the editor.
- **Events and prompts.** Events reach only extensions granted Read, with the project file's name but not its folder.
  Prompts open only during a command the user ran.
- **Grants.** They hold for the files, author and folder the user reviewed; see "Permissions and trust".

**What an extension can never do, by design:**
- reach the network, start a process, or read or write a file (no such API exists in its engine);
- reach a file through a command: commands that read or write the files their parameters name (exports, imports,
  merges, plugin scans; `CommandSpec::touchesFiles`, marked *(files)* above) are refused whatever the grants
  (`EnginePolicy::allowFileCommands`), and the host's services treat an extension like a remote MCP client, which
  may not name script or sidecar files;
- grant itself a capability, consent to cloud AI, or raise its own limits;
- edit without an undo step bearing its name;
- delete what a command would not let any client delete;
- run as a per-project extension before the user trusts that exact content.

**Defence in depth (Windows):** the job object's memory cap, no child processes and UI restrictions.

**Not covered yet (follow-ups):**
- **File reading and the network at the OS level.** The helper runs with the user's token. An engine exploit that
  escaped the JavaScript sandbox could read the user's files or open sockets. The follow-up is an AppContainer or
  low-integrity restricted token for `wabcut-scripthost` on Windows.
- **macOS and Linux.** The memory cap and the no-child-process rule are not enforced there yet (`ChildProcessOptions`
  has no POSIX equivalent). The follow-ups are `setrlimit` (RLIMIT_AS, RLIMIT_NPROC) or a cgroup, plus seccomp on
  Linux and a sandbox profile on macOS.
- **Signing.** Extensions are not signed. The content hash pins what was reviewed for per-project trust only.
