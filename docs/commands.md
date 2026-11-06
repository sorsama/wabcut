# Edit commands

<!-- Generated from the command registry by wabcut_command_docgen; do not edit by hand.
     Regenerate: build\win-msvc\bin\wabcut_command_docgen.exe docs/commands.md docs/mcp.md
     (the command test suite fails while this file is out of date). -->

The typed command protocol (`src/command`, library `wabcut_command`) is the one way agents, the CLI, scripts and MCP clients edit a project. Commands are thin, validated adapters over the model's `edit::` operations: the model decides what an edit does, the protocol names it, checks it and reports what changed.

## Requests

```json
{"command": "timeline.rippleDelete", "version": 1, "params": {"clips": [42]},
 "expectedRevision": 7, "idempotencyKey": "agent-run-3/step-12", "dryRun": false}
```

* `command`: a name below, one of its aliases, or its LLM tool name (`.` written as `_`).
* `version`: optional; the newest version when left out.
* `expectedRevision`: optional; the request fails with `StaleRevision` unless the session is at that revision. Every result reports the current `revision`.
* `idempotencyKey`: optional; a successful request is remembered per session, and a replay with the same key returns the first result (`"replayed": true`) instead of editing again. Reusing a key for a different request fails with `IdempotencyConflict`.
* `dryRun`: validate and compute the result and its ChangeSet without committing. In process the result carries the would-be state as a preview snapshot. Nothing outside the project changes either: what a command does to the host (the clipboard, a version saved to disk) happens only once a request commits, never for a dry run or a batch that fails.

A **batch** applies several commands in order as one undo step, all or nothing:

```json
{"commands": [{"command": "sequence.addEdit", "params": {"time": "00:00:05:00"}},
              {"command": "clip.setParam", "params": {"clips": [{"track": "V1", "time": "00:00:06:00"}],
                                                 "param": "opacity.opacity", "value": 0.5}}],
 "label": "Dim the second half", "expectedRevision": 7, "dryRun": true}
```

`expectedRevision`, `idempotencyKey` and `dryRun` belong to the batch, not to its commands. Each command sees the state the previous ones produced; read commands inside a batch report in `steps`.

## References

* **Times** are integer ticks (254016000000 per second) or timecode strings in the sequence's rate (`"00:01:02:03"`, `;` before the frames for drop-frame). Deltas may be negative (`"-00:00:00:05"`).
* **Tracks** are labels (`"V1"` is the bottom video track, `"A1"` the first audio track) or track ids.
* **Clips** are clip ids or `{"track": "V1", "time": "00:00:03:00"}` for the clip under that time.
* **Media** is a media id or the media item's exact name.
* **Sequence**: every sequence command takes an optional `sequence` id; it may be left out when the project has exactly one sequence.

## Results

```json
{"ok": true, "revision": 8, "changes": {"sequences": [{"sequence": 2, "tracks": [...], "addedClips": [], "removedClips": [42], ...}]},
 "dryRun": false, "committed": true}
```

`changes` (the model's ChangeSet: per sequence the touched tracks and time ranges, added, removed and modified clips, property roles and other `changed` parts), `createdIds`, `warnings`, `data` (a read command's answer) and `steps` (per command of a batch) are left out when empty.

Failures change nothing and carry `{"ok": false, "error": {"code", "message", "details"}}`. Codes: `UnknownCommand`, `UnsupportedVersion`, `InvalidRequest`, `InvalidParams` (details.issues lists JSON-pointer paths), `NotFound`, `PermissionDenied`, `StaleRevision`, `IdempotencyConflict`, `EditRejected` (the model refused the edit), `InvariantViolation`, `LockedTrack`, `Cancelled`, `TimedOut` (the caller stopped a long request; see `ExecutionControl`), `Internal`. In a batch, details.index names the failing command.

Every request is checked in release builds too: params against the schema, references against the project, the result against the model invariants (`validate()`), and locked tracks must come out unchanged.

**Capabilities** scope what a client may run: `Read`, `EditSequence`, `EditProject`, `MediaAnalysis`, `Network` (the last two have no commands yet).

## Command-line and LLM tools

```
wabcut-cli cmd <project.wabcut> --json '<request or batch>' [--dry-run] [--save]
wabcut-cli tools --format anthropic|openai|jsonschema
wabcut-cli mcp --project <file.wabcut> [--allow-edit] [--allow-analysis] [--save-on-exit | --autosave]
```

`mcp` serves the catalog to MCP clients such as Claude Desktop, Claude Code and Codex; see [docs/mcp.md](mcp.md).

`tools` prints the catalog as Anthropic Messages API `tools`, OpenAI Chat Completions `tools` or plain JSON Schema definitions. Each CLI run is a fresh session (revision 0).

**Transcript commands** (`transcript.*`, `captions.generate`; library `wabcut_ai_edit`, see [docs/ai.md](ai.md#text-based-editing)) edit the video by editing what it says. They need the transcripts of the project's media, which the host provides (`EnginePolicy::services`): `wabcut-cli` reads them from the analysis cache that transcription fills, and without them the commands fail with `InvalidRequest`. Their word indices refer to `transcript.text` / `transcript.words` at the request's revision, so pass `expectedRevision`. `wabcut-cli transcript-edit` and `wabcut-cli captions` wrap them for scripts.

**Media-analysis commands** (`analysis.*`, `clip.splitAtShots`; library `wabcut_ai_analysis`, see [docs/ai.md](ai.md#media-analysis-shots-and-beats)) detect shots and beats in the project's media (cached per source file); the host provides the analysis (`AnalysisServices`), else they fail with `InvalidRequest`. `analysis.detectBeats` sets the sequence beat grid that `edit.snapToBeats`, `edit.cutToBeats` and timeline snapping use; `sequence.setBeatGrid` sets it by hand or from taps.

## Catalog

| Command | Capability | Title | Aliases |
|---|---|---|---|
| [`adjustment.add`](#adjustment.add-v1) | EditSequence | New Adjustment Layer |  |
| [`analysis.detectBeats`](#analysis.detectBeats-v1) | EditSequence | Detect Beats |  |
| [`analysis.detectShots`](#analysis.detectShots-v1) | EditSequence | Detect Shots |  |
| [`analysis.speakers`](#analysis.speakers-v1) | MediaAnalysis | Who Speaks When |  |
| [`analysis.subjects`](#analysis.subjects-v1) | MediaAnalysis | Find Subjects |  |
| [`audio.addEffect`](#audio.addEffect-v1) | EditSequence | Add Track or Master Effect |  |
| [`audio.addKeyframe`](#audio.addKeyframe-v1) | EditSequence | Add Track or Master Keyframe |  |
| [`audio.autoDuck`](#audio.autoDuck-v1) | EditSequence | Auto-Duck Music |  |
| [`audio.autoMix`](#audio.autoMix-v1) | EditSequence | Auto-Mix Voices |  |
| [`audio.autoTag`](#audio.autoTag-v1) | EditSequence | Auto-tag Audio |  |
| [`audio.bleepProfanity`](#audio.bleepProfanity-v1) | EditProject | Bleep Profanity |  |
| [`audio.channelLevels`](#audio.channelLevels-v1) | MediaAnalysis | Measure Channel Levels |  |
| [`audio.channels`](#audio.channels-v1) | Read | Audio Channels |  |
| [`audio.effects`](#audio.effects-v1) | Read | Audio Effect Stack |  |
| [`audio.enhanceSpeech`](#audio.enhanceSpeech-v1) | EditProject | Enhance Speech |  |
| [`audio.eqMatch`](#audio.eqMatch-v1) | EditSequence | EQ Match |  |
| [`audio.eqMatchReset`](#audio.eqMatchReset-v1) | EditSequence | Reset EQ Match |  |
| [`audio.essentialSound`](#audio.essentialSound-v1) | EditSequence | Essential Sound |  |
| [`audio.fillChannel`](#audio.fillChannel-v1) | EditSequence | Fill Channel |  |
| [`audio.levelMatch`](#audio.levelMatch-v1) | EditSequence | Level Match |  |
| [`audio.loudness`](#audio.loudness-v1) | MediaAnalysis | Measure Loudness |  |
| [`audio.masterLimiter`](#audio.masterLimiter-v1) | EditSequence | Master True-Peak Limiter |  |
| [`audio.moveEffect`](#audio.moveEffect-v1) | EditSequence | Reorder Track or Master Effect |  |
| [`audio.moveKeyframe`](#audio.moveKeyframe-v1) | EditSequence | Move Track or Master Keyframe |  |
| [`audio.normalize`](#audio.normalize-v1) | EditSequence | Normalize Loudness |  |
| [`audio.remix`](#audio.remix-v1) | EditSequence | Remix Music |  |
| [`audio.remixRender`](#audio.remixRender-v1) | EditProject | Render Music Remix |  |
| [`audio.removeEffect`](#audio.removeEffect-v1) | EditSequence | Remove Track or Master Effect |  |
| [`audio.removeKeyframe`](#audio.removeKeyframe-v1) | EditSequence | Remove Track or Master Keyframe |  |
| [`audio.replaceProfanity`](#audio.replaceProfanity-v1) | EditProject | Replace Profanity with a Sound |  |
| [`audio.routing`](#audio.routing-v1) | Read | Audio Routing |  |
| [`audio.separateStems`](#audio.separateStems-v1) | EditProject | Separate Stems |  |
| [`audio.setChannelVolume`](#audio.setChannelVolume-v1) | EditSequence | Set Channel Volume |  |
| [`audio.setChannels`](#audio.setChannels-v1) | EditSequence | Modify Audio Channels |  |
| [`audio.setEffect`](#audio.setEffect-v1) | EditSequence | Set Track or Master Effect |  |
| [`audio.setMediaChannels`](#audio.setMediaChannels-v1) | EditProject | Modify Media Audio Channels |  |
| [`audio.spectralAttenuate`](#audio.spectralAttenuate-v1) | EditSequence | Attenuate Spectral Selection |  |
| [`audio.spectralHeal`](#audio.spectralHeal-v1) | EditSequence | Heal Spectral Selection |  |
| [`audio.spectralNoiseFloor`](#audio.spectralNoiseFloor-v1) | EditSequence | Fill Spectral Selection with Noise Floor |  |
| [`audio.spectralRemoveTone`](#audio.spectralRemoveTone-v1) | EditSequence | Remove Tone |  |
| [`audio.spectralRepairEdits`](#audio.spectralRepairEdits-v1) | Read | Spectral Repair Edits |  |
| [`audio.spectralRepairOptions`](#audio.spectralRepairOptions-v1) | EditSequence | Spectral Repair Options |  |
| [`audio.spectralRepairRemove`](#audio.spectralRepairRemove-v1) | EditSequence | Remove Spectral Edit |  |
| [`audio.splitToMono`](#audio.splitToMono-v1) | EditSequence | Split to Mono |  |
| [`audio.syncClips`](#audio.syncClips-v1) | EditSequence | Synchronize by Audio |  |
| [`audioDescription.createTrack`](#audioDescription.createTrack-v1) | EditSequence | Audio Description Track |  |
| [`audioDescription.findGaps`](#audioDescription.findGaps-v1) | Read | Find Gaps for Audio Description |  |
| [`audition.addSource`](#audition.addSource-v1) | EditSequence | Add Source to Audition |  |
| [`audition.addTakes`](#audition.addTakes-v1) | EditSequence | Add to Audition |  |
| [`audition.create`](#audition.create-v1) | EditSequence | New Audition |  |
| [`audition.describe`](#audition.describe-v1) | Read | Describe Audition |  |
| [`audition.duplicateTake`](#audition.duplicateTake-v1) | EditSequence | Duplicate Take |  |
| [`audition.flatten`](#audition.flatten-v1) | EditSequence | Flatten Audition |  |
| [`audition.moveTake`](#audition.moveTake-v1) | EditSequence | Move Take |  |
| [`audition.removeTake`](#audition.removeTake-v1) | EditSequence | Remove Take |  |
| [`audition.renameTake`](#audition.renameTake-v1) | EditSequence | Rename Take |  |
| [`audition.setActive`](#audition.setActive-v1) | EditSequence | Use Take |  |
| [`autopilot.broll`](#autopilot.broll-v1) | EditSequence | Auto B-roll |  |
| [`autopilot.pacing`](#autopilot.pacing-v1) | EditSequence | Pacing Match |  |
| [`autopilot.publishKit`](#autopilot.publishKit-v1) | Read | Publishing Kit |  |
| [`autozoom.apply`](#autozoom.apply-v1) | EditSequence | Auto Zoom |  |
| [`bin.create`](#bin.create-v1) | EditProject | New Bin |  |
| [`bin.createSearch`](#bin.createSearch-v1) | EditProject | Create Search Bin |  |
| [`bin.move`](#bin.move-v1) | EditProject | Move to Bin |  |
| [`bin.remove`](#bin.remove-v1) | EditProject | Delete Bin |  |
| [`bin.rename`](#bin.rename-v1) | EditProject | Rename Bin |  |
| [`bin.setNestTarget`](#bin.setNestTarget-v1) | EditProject | File Nests in Bin |  |
| [`bin.setSearch`](#bin.setSearch-v1) | EditProject | Edit Search Bin |  |
| [`brand.get`](#brand.get-v1) | Read | Brand Kit |  |
| [`brand.set`](#brand.set-v1) | EditProject | Set Brand Kit |  |
| [`captions.addCue`](#captions.addCue-v1) | EditSequence | Add Caption |  |
| [`captions.addTrack`](#captions.addTrack-v1) | EditSequence | New Caption Track |  |
| [`captions.applyPreset`](#captions.applyPreset-v1) | EditSequence | Apply Caption Preset |  |
| [`captions.checkSpelling`](#captions.checkSpelling-v1) | Read | Check Spelling in Captions |  |
| [`captions.export`](#captions.export-v1) | Read | Export Captions |  |
| [`captions.exportPresets`](#captions.exportPresets-v1) | Read | Export Caption Presets |  |
| [`captions.find`](#captions.find-v1) | Read | Find in Captions |  |
| [`captions.findReplace`](#captions.findReplace-v1) | EditSequence | Find and Replace in Captions |  |
| [`captions.generate`](#captions.generate-v1) | EditSequence | Generate Captions |  |
| [`captions.generateFromTranscript`](#captions.generateFromTranscript-v1) | EditSequence | Captions from Transcript |  |
| [`captions.get`](#captions.get-v1) | Read | Caption Cues |  |
| [`captions.hideProfanity`](#captions.hideProfanity-v1) | EditSequence | Hide Profanity in Captions |  |
| [`captions.import`](#captions.import-v1) | EditSequence | Import Captions |  |
| [`captions.importPresets`](#captions.importPresets-v1) | EditProject | Import Caption Presets |  |
| [`captions.mergeCues`](#captions.mergeCues-v1) | EditSequence | Merge Captions |  |
| [`captions.presets`](#captions.presets-v1) | Read | Caption Presets |  |
| [`captions.pushTrackStyle`](#captions.pushTrackStyle-v1) | EditProject | Push to Track Style |  |
| [`captions.remove`](#captions.remove-v1) | EditSequence | Delete Caption Track |  |
| [`captions.removeCue`](#captions.removeCue-v1) | EditSequence | Delete Caption |  |
| [`captions.removePreset`](#captions.removePreset-v1) | EditProject | Delete Caption Preset |  |
| [`captions.savePreset`](#captions.savePreset-v1) | EditProject | Save Caption Preset |  |
| [`captions.setStyle`](#captions.setStyle-v1) | EditSequence | Caption Style |  |
| [`captions.setTrack`](#captions.setTrack-v1) | EditSequence | Caption Track Settings |  |
| [`captions.setTrackStyle`](#captions.setTrackStyle-v1) | EditSequence | Track Style |  |
| [`captions.shiftCues`](#captions.shiftCues-v1) | EditSequence | Shift Captions |  |
| [`captions.splitCue`](#captions.splitCue-v1) | EditSequence | Split Caption |  |
| [`captions.toGraphics`](#captions.toGraphics-v1) | EditSequence | Convert Captions to Graphics |  |
| [`captions.translate`](#captions.translate-v1) | EditSequence | Translate Captions |  |
| [`captions.updateCue`](#captions.updateCue-v1) | EditSequence | Edit Caption |  |
| [`captions.updateFromTranscript`](#captions.updateFromTranscript-v1) | EditSequence | Update Captions from Transcript |  |
| [`cdl.export`](#cdl.export-v1) | EditProject | Export CDL |  |
| [`cdl.import`](#cdl.import-v1) | EditSequence | Import CDL |  |
| [`clip.addEffect`](#clip.addEffect-v1) | EditSequence | Apply Effect |  |
| [`clip.addKeyframe`](#clip.addKeyframe-v1) | EditSequence | Add Keyframe | `keyframes.add` |
| [`clip.details`](#clip.details-v1) | Read | Clip Details |  |
| [`clip.enable`](#clip.enable-v1) | EditSequence | Enable |  |
| [`clip.fade`](#clip.fade-v1) | EditSequence | Fade |  |
| [`clip.find`](#clip.find-v1) | Read | Find Clips |  |
| [`clip.frameHold`](#clip.frameHold-v1) | EditSequence | Frame Hold |  |
| [`clip.frameHoldOptions`](#clip.frameHoldOptions-v1) | EditSequence | Frame Hold Options |  |
| [`clip.group`](#clip.group-v1) | EditSequence | Group | `clips.group` |
| [`clip.insert`](#clip.insert-v1) | EditSequence | Insert | `sequence.insert` |
| [`clip.instantReplay`](#clip.instantReplay-v1) | EditSequence | Instant Replay |  |
| [`clip.link`](#clip.link-v1) | EditSequence | Link |  |
| [`clip.lock`](#clip.lock-v1) | EditSequence | Lock Clip |  |
| [`clip.move`](#clip.move-v1) | EditSequence | Move |  |
| [`clip.moveEffect`](#clip.moveEffect-v1) | EditSequence | Reorder Effect |  |
| [`clip.moveKeyframe`](#clip.moveKeyframe-v1) | EditSequence | Move Keyframe |  |
| [`clip.nearestNeighborScaling`](#clip.nearestNeighborScaling-v1) | EditSequence | Nearest Neighbor Scaling |  |
| [`clip.openNested`](#clip.openNested-v1) | Read | Open Nested Sequence |  |
| [`clip.overwrite`](#clip.overwrite-v1) | EditSequence | Overwrite | `sequence.overwrite` |
| [`clip.pasteAttributes`](#clip.pasteAttributes-v1) | EditSequence | Paste Attributes |  |
| [`clip.pinKeyframe`](#clip.pinKeyframe-v1) | EditSequence | Pin Keyframe | `keyframes.pin` |
| [`clip.removeAttributes`](#clip.removeAttributes-v1) | EditSequence | Remove Attributes |  |
| [`clip.removeEffect`](#clip.removeEffect-v1) | EditSequence | Remove Effect |  |
| [`clip.removeKeyframe`](#clip.removeKeyframe-v1) | EditSequence | Remove Keyframe | `keyframes.remove` |
| [`clip.rename`](#clip.rename-v1) | EditSequence | Rename Clip |  |
| [`clip.replace`](#clip.replace-v1) | EditSequence | Replace With Clip |  |
| [`clip.restoreUnrendered`](#clip.restoreUnrendered-v1) | EditSequence | Restore Unrendered |  |
| [`clip.rippleTrim`](#clip.rippleTrim-v1) | EditSequence | Ripple Edit |  |
| [`clip.roll`](#clip.roll-v1) | EditSequence | Rolling Edit |  |
| [`clip.scaleToFrameSize`](#clip.scaleToFrameSize-v1) | EditSequence | Auto Fit to Frame |  |
| [`clip.setAnimated`](#clip.setAnimated-v1) | EditSequence | Toggle Animation |  |
| [`clip.setBlendMode`](#clip.setBlendMode-v1) | EditSequence | Set Blend Mode |  |
| [`clip.setColor`](#clip.setColor-v1) | EditSequence | Set Label Color | `clip.label` |
| [`clip.setEffect`](#clip.setEffect-v1) | EditSequence | Set Effect |  |
| [`clip.setFade`](#clip.setFade-v1) | EditSequence | Set Fade |  |
| [`clip.setFieldOptions`](#clip.setFieldOptions-v1) | EditSequence | Field Options |  |
| [`clip.setKeyframe`](#clip.setKeyframe-v1) | EditSequence | Set Keyframe |  |
| [`clip.setMotion`](#clip.setMotion-v1) | EditSequence | Motion | `motion.set` |
| [`clip.setNote`](#clip.setNote-v1) | EditSequence | Set Clip Note | `clip.note` |
| [`clip.setParam`](#clip.setParam-v1) | EditSequence | Set Clip Parameter |  |
| [`clip.setTimeInterpolation`](#clip.setTimeInterpolation-v1) | EditSequence | Time Interpolation |  |
| [`clip.setToFillFrame`](#clip.setToFillFrame-v1) | EditSequence | Set Scale to Fill Frame |  |
| [`clip.setToFrameSize`](#clip.setToFrameSize-v1) | EditSequence | Set Scale to Fit Frame |  |
| [`clip.shake`](#clip.shake-v1) | EditSequence | Camera Shake |  |
| [`clip.slide`](#clip.slide-v1) | EditSequence | Slide |  |
| [`clip.slip`](#clip.slip-v1) | EditSequence | Slip |  |
| [`clip.speedDuration`](#clip.speedDuration-v1) | EditSequence | Speed/Duration | `clip.speed` |
| [`clip.splitAtShots`](#clip.splitAtShots-v1) | EditSequence | Split at Shots |  |
| [`clip.trim`](#clip.trim-v1) | EditSequence | Trim |  |
| [`clip.ungroup`](#clip.ungroup-v1) | EditSequence | Ungroup | `clips.ungroup` |
| [`clip.unlink`](#clip.unlink-v1) | EditSequence | Unlink |  |
| [`clip.zoomPunch`](#clip.zoomPunch-v1) | EditSequence | Zoom Punch |  |
| [`clipboard.copy`](#clipboard.copy-v1) | Read | Copy |  |
| [`clipboard.cut`](#clipboard.cut-v1) | EditSequence | Cut |  |
| [`clipboard.get`](#clipboard.get-v1) | Read | Clipboard |  |
| [`clipboard.paste`](#clipboard.paste-v1) | EditSequence | Paste |  |
| [`color.auto`](#color.auto-v1) | EditSequence | Auto Color |  |
| [`color.chartMatch`](#color.chartMatch-v1) | EditSequence | Match to Color Chart |  |
| [`color.grade.copy`](#color.grade.copy-v1) | EditSequence | Apply Grade |  |
| [`color.grade.get`](#color.grade.get-v1) | Read | Get Color Grade |  |
| [`color.grade.set`](#color.grade.set-v1) | EditSequence | Set Color Grade |  |
| [`color.inputSpace.get`](#color.inputSpace.get-v1) | Read | Get Input Colour Space |  |
| [`color.inputSpace.set`](#color.inputSpace.set-v1) | EditSequence | Set Input Colour Space |  |
| [`color.management.get`](#color.management.get-v1) | Read | Get Colour Management |  |
| [`color.management.set`](#color.management.set-v1) | EditProject | Set Colour Management |  |
| [`color.match`](#color.match-v1) | EditSequence | Color Match |  |
| [`color.matchScene`](#color.matchScene-v1) | EditSequence | Match Scene |  |
| [`dub.create`](#dub.create-v1) | EditProject | Dub |  |
| [`edit.clear`](#edit.clear-v1) | EditSequence | Delete | `clip.delete` |
| [`edit.cutToBeats`](#edit.cutToBeats-v1) | EditSequence | Cut to Beats |  |
| [`edit.insertFromSource`](#edit.insertFromSource-v1) | EditSequence | Insert from Source |  |
| [`edit.overwriteFromSource`](#edit.overwriteFromSource-v1) | EditSequence | Overwrite from Source |  |
| [`edit.snapToBeats`](#edit.snapToBeats-v1) | EditSequence | Snap to Beats |  |
| [`effect.aiStatus`](#effect.aiStatus-v1) | Read | AI Effects Status |  |
| [`effect.analyzeAll`](#effect.analyzeAll-v1) | EditSequence | Analyze All Stabilizers |  |
| [`effect.analyzeRollingShutter`](#effect.analyzeRollingShutter-v1) | EditSequence | Analyze Rolling Shutter |  |
| [`effect.analyzeStabilization`](#effect.analyzeStabilization-v1) | EditSequence | Analyze Stabilization |  |
| [`effect.list`](#effect.list-v1) | Read | List Effects |  |
| [`effect.processAi`](#effect.processAi-v1) | MediaAnalysis | Process AI Effects |  |
| [`effects.addToCustomBin`](#effects.addToCustomBin-v1) | EditProject | Add to Custom Bin |  |
| [`effects.applyPreset`](#effects.applyPreset-v1) | EditSequence | Apply Preset |  |
| [`effects.applyTrackPreset`](#effects.applyTrackPreset-v1) | EditSequence | Apply Preset to Tracks |  |
| [`effects.deleteCustomItem`](#effects.deleteCustomItem-v1) | EditProject | Delete Custom Item |  |
| [`effects.exportPresets`](#effects.exportPresets-v1) | Read | Export Effect Presets |  |
| [`effects.importPresets`](#effects.importPresets-v1) | EditProject | Import Presets |  |
| [`effects.library`](#effects.library-v1) | Read | Effects Library |  |
| [`effects.newCustomBin`](#effects.newCustomBin-v1) | EditProject | New Custom Bin |  |
| [`effects.newPresetsBin`](#effects.newPresetsBin-v1) | EditProject | New Presets Bin |  |
| [`effects.renameCustomItem`](#effects.renameCustomItem-v1) | EditProject | Rename Custom Item |  |
| [`effects.savePreset`](#effects.savePreset-v1) | EditProject | Save Effect Preset |  |
| [`effects.saveTrackPreset`](#effects.saveTrackPreset-v1) | EditProject | Save Track Effects as Preset |  |
| [`effects.setFavorite`](#effects.setFavorite-v1) | EditProject | Add to Favorites |  |
| [`effects.setPresetProperties`](#effects.setPresetProperties-v1) | EditProject | Preset Properties |  |
| [`export.check`](#export.check-v1) | Read | Check Before Export |  |
| [`export.describe`](#export.describe-v1) | Read | Describe Export |  |
| [`export.preset.delete`](#export.preset.delete-v1) | EditProject | Delete Preset |  |
| [`export.preset.export`](#export.preset.export-v1) | Read | Export Preset |  |
| [`export.preset.import`](#export.preset.import-v1) | Read | Import Preset |  |
| [`export.preset.save`](#export.preset.save-v1) | EditProject | Save Preset |  |
| [`export.presets`](#export.presets-v1) | Read | Export Presets |  |
| [`export.queue`](#export.queue-v1) | Read | Send to Media Encoder |  |
| [`export.queue.cancel`](#export.queue.cancel-v1) | EditProject | Stop |  |
| [`export.queue.list`](#export.queue.list-v1) | Read | Export Queue |  |
| [`export.queue.pause`](#export.queue.pause-v1) | EditProject | Pause |  |
| [`export.queue.remove`](#export.queue.remove-v1) | EditProject | Remove |  |
| [`export.queue.resume`](#export.queue.resume-v1) | EditProject | Resume |  |
| [`export.queue.start`](#export.queue.start-v1) | EditProject | Start Queue |  |
| [`export.quick`](#export.quick-v1) | Read | Quick Export |  |
| [`export.watchFolder.add`](#export.watchFolder.add-v1) | Read | Add Watch Folder |  |
| [`export.watchFolder.list`](#export.watchFolder.list-v1) | Read | Watch Folders |  |
| [`export.watchFolder.remove`](#export.watchFolder.remove-v1) | EditProject | Remove Watch Folder |  |
| [`freeform.alignToGrid`](#freeform.alignToGrid-v1) | EditProject | Align to Grid |  |
| [`freeform.get`](#freeform.get-v1) | Read | Freeform View |  |
| [`freeform.removeLayout`](#freeform.removeLayout-v1) | EditProject | Delete Layout |  |
| [`freeform.resetToGrid`](#freeform.resetToGrid-v1) | EditProject | Reset to Grid |  |
| [`freeform.restoreLayout`](#freeform.restoreLayout-v1) | EditProject | Restore Layout |  |
| [`freeform.saveLayout`](#freeform.saveLayout-v1) | EditProject | Save as New Layout |  |
| [`freeform.set`](#freeform.set-v1) | EditProject | Arrange in Freeform View |  |
| [`gen.broll`](#gen.broll-v1) | EditProject | Generate broll |  |
| [`gen.extend`](#gen.extend-v1) | EditProject | Generate extend |  |
| [`gen.image`](#gen.image-v1) | EditProject | Generate image |  |
| [`gen.sfx`](#gen.sfx-v1) | EditProject | Generate sfx |  |
| [`graphic.add`](#graphic.add-v1) | EditSequence | New Graphic |  |
| [`graphic.addLayer`](#graphic.addLayer-v1) | EditSequence | Add Graphic Layer |  |
| [`graphic.align`](#graphic.align-v1) | EditSequence | Align Layers |  |
| [`graphic.applyPreset`](#graphic.applyPreset-v1) | EditSequence | Apply Animation Preset |  |
| [`graphic.checkSpelling`](#graphic.checkSpelling-v1) | Read | Check Spelling in Graphic |  |
| [`graphic.get`](#graphic.get-v1) | Read | Graphic |  |
| [`graphic.removeLayer`](#graphic.removeLayer-v1) | EditSequence | Delete Graphic Layer |  |
| [`graphic.resolve`](#graphic.resolve-v1) | Read | Graphic Draw List |  |
| [`graphic.setRoll`](#graphic.setRoll-v1) | EditSequence | Roll/Crawl Options |  |
| [`graphic.setTextPath`](#graphic.setTextPath-v1) | EditSequence | Type on a Path |  |
| [`graphic.update`](#graphic.update-v1) | EditSequence | Edit Graphic |  |
| [`graphic.updateLayer`](#graphic.updateLayer-v1) | EditSequence | Edit Graphic Layer |  |
| [`highlights.find`](#highlights.find-v1) | EditSequence | Find Highlights |  |
| [`highlights.importChat`](#highlights.importChat-v1) | MediaAnalysis | Import Chat Log |  |
| [`highlights.toReel`](#highlights.toReel-v1) | EditProject | Make Highlight Reel |  |
| [`highlights.toShort`](#highlights.toShort-v1) | EditProject | Make Highlight Short |  |
| [`history.checkpoint`](#history.checkpoint-v1) | EditProject | Save Version |  |
| [`history.diff`](#history.diff-v1) | Read | Compare Versions |  |
| [`history.export`](#history.export-v1) | EditProject | Save Version As |  |
| [`history.list`](#history.list-v1) | Read | Version History |  |
| [`history.name`](#history.name-v1) | EditProject | Name Version |  |
| [`history.restore`](#history.restore-v1) | EditProject | Restore Version |  |
| [`interchange.export`](#interchange.export-v1) | Read | Export Timeline |  |
| [`keyframes.ease`](#keyframes.ease-v1) | EditSequence | Ease Keyframes |  |
| [`keyframes.move`](#keyframes.move-v1) | EditSequence | Move Keyframes |  |
| [`keyframes.setInterpolation`](#keyframes.setInterpolation-v1) | EditSequence | Set Keyframe Interpolation |  |
| [`layout.apply`](#layout.apply-v1) | EditSequence | Apply Layout |  |
| [`layout.list`](#layout.list-v1) | Read | List Layouts |  |
| [`llm.local.models`](#llm.local.models-v1) | Read | Local assistant models |  |
| [`llm.local.start`](#llm.local.start-v1) | MediaAnalysis | Local assistant start |  |
| [`llm.local.status`](#llm.local.status-v1) | Read | Local assistant status |  |
| [`llm.local.stop`](#llm.local.stop-v1) | MediaAnalysis | Local assistant stop |  |
| [`lottie.import`](#lottie.import-v1) | EditProject | Import Lottie |  |
| [`lut.export`](#lut.export-v1) | Read | Export .cube |  |
| [`lut.import`](#lut.import-v1) | EditProject | Import LUT |  |
| [`lut.importFolder`](#lut.importFolder-v1) | EditProject | Import LUT Folder |  |
| [`lut.list`](#lut.list-v1) | Read | List LUTs |  |
| [`marker.add`](#marker.add-v1) | EditSequence | Add Marker | `markers.add` |
| [`marker.clearAll`](#marker.clearAll-v1) | EditSequence | Clear All Markers | `markers.clearAll` |
| [`marker.clearInAndOut`](#marker.clearInAndOut-v1) | EditSequence | Clear In and Out |  |
| [`marker.duplicate`](#marker.duplicate-v1) | EditSequence | Duplicate Markers | `markers.duplicate` |
| [`marker.import`](#marker.import-v1) | EditSequence | Import Markers | `markers.import` |
| [`marker.list`](#marker.list-v1) | Read | List Markers |  |
| [`marker.markIn`](#marker.markIn-v1) | EditSequence | Mark In |  |
| [`marker.markOut`](#marker.markOut-v1) | EditSequence | Mark Out |  |
| [`marker.moveMany`](#marker.moveMany-v1) | EditSequence | Move Markers | `markers.moveMany` |
| [`marker.remove`](#marker.remove-v1) | EditSequence | Remove Marker | `markers.remove` |
| [`marker.removeMany`](#marker.removeMany-v1) | EditSequence | Remove Markers | `markers.removeMany` |
| [`marker.update`](#marker.update-v1) | EditSequence | Edit Marker | `markers.update` |
| [`mask.add`](#mask.add-v1) | EditSequence | Add Mask |  |
| [`mask.list`](#mask.list-v1) | Read | List Masks |  |
| [`mask.objectSelect`](#mask.objectSelect-v1) | EditSequence | Select Object |  |
| [`mask.propagate`](#mask.propagate-v1) | EditSequence | Track Object Mask |  |
| [`mask.refine`](#mask.refine-v1) | EditSequence | Refine Object Mask |  |
| [`mask.remove`](#mask.remove-v1) | EditSequence | Remove Mask |  |
| [`mask.update`](#mask.update-v1) | EditSequence | Update Mask |  |
| [`media.browse`](#media.browse-v1) | Read | Media Browser |  |
| [`media.duplicate`](#media.duplicate-v1) | EditProject | Duplicate |  |
| [`media.fileMetadata`](#media.fileMetadata-v1) | Read | File Metadata |  |
| [`media.importCameraClips`](#media.importCameraClips-v1) | EditProject | Import from Media Browser |  |
| [`media.importImageSequence`](#media.importImageSequence-v1) | EditProject | Import Image Sequence |  |
| [`media.importLayeredFile`](#media.importLayeredFile-v1) | EditProject | Import Layered File |  |
| [`media.interpretFootage`](#media.interpretFootage-v1) | EditProject | Interpret Footage |  |
| [`media.layeredFileInfo`](#media.layeredFileInfo-v1) | Read | Layered File Info |  |
| [`media.list`](#media.list-v1) | Read | List Media |  |
| [`media.makeOffline`](#media.makeOffline-v1) | EditProject | Make Offline |  |
| [`media.makeSubclip`](#media.makeSubclip-v1) | EditProject | Make Subclip |  |
| [`media.mergeClips`](#media.mergeClips-v1) | EditProject | Merge Clips |  |
| [`media.newSynthetic`](#media.newSynthetic-v1) | EditProject | New Synthetic Item |  |
| [`media.rawSourceSettings`](#media.rawSourceSettings-v1) | Read | RAW Source Settings |  |
| [`media.readXmp`](#media.readXmp-v1) | EditProject | Read XMP Metadata |  |
| [`media.reload`](#media.reload-v1) | EditProject | Reload Footage |  |
| [`media.replaceFootage`](#media.replaceFootage-v1) | EditProject | Replace Footage |  |
| [`media.setAttributes`](#media.setAttributes-v1) | EditProject | Media Attributes |  |
| [`media.setRawSourceSettings`](#media.setRawSourceSettings-v1) | EditProject | Source Settings |  |
| [`media.setRights`](#media.setRights-v1) | EditProject | Set Rights and Clearance |  |
| [`media.setScaling`](#media.setScaling-v1) | EditProject | Default Media Scaling |  |
| [`media.setSubclip`](#media.setSubclip-v1) | EditProject | Edit Subclip |  |
| [`media.setTimecode`](#media.setTimecode-v1) | EditProject | Modify Timecode |  |
| [`multicam.angles`](#multicam.angles-v1) | Read | Multicam Angles |  |
| [`multicam.autoSwitch`](#multicam.autoSwitch-v1) | EditSequence | Auto Switch Multicam |  |
| [`multicam.create`](#multicam.create-v1) | EditProject | Create Multi-Camera Source Sequence |  |
| [`multicam.flatten`](#multicam.flatten-v1) | EditSequence | Flatten |  |
| [`multicam.setSync`](#multicam.setSync-v1) | EditSequence | Set Angle Sync |  |
| [`multicam.switchAngle`](#multicam.switchAngle-v1) | EditSequence | Switch Multicam Angle |  |
| [`narration.create`](#narration.create-v1) | EditProject | Narration |  |
| [`ograf.import`](#ograf.import-v1) | EditProject | Import OGraf |  |
| [`ograf.inspect`](#ograf.inspect-v1) | Read | Inspect OGraf |  |
| [`plugins.list`](#plugins.list-v1) | Read | List Audio Plugins |  |
| [`plugins.scan`](#plugins.scan-v1) | MediaAnalysis | Scan Audio Plugins |  |
| [`project.consolidateDuplicates`](#project.consolidateDuplicates-v1) | EditProject | Consolidate Duplicates |  |
| [`project.importItems`](#project.importItems-v1) | EditProject | Import from Project |  |
| [`project.merge`](#project.merge-v1) | EditProject | Merge Projects |  |
| [`project.rightsLog`](#project.rightsLog-v1) | Read | Rights and Clearance Log |  |
| [`project.summary`](#project.summary-v1) | Read | Project Summary |  |
| [`provenance.report`](#provenance.report-v1) | Read | Generated media provenance |  |
| [`record.takes`](#record.takes-v1) | Read | Voice-over Takes |  |
| [`record.useTake`](#record.useTake-v1) | EditSequence | Use Voice-over Take |  |
| [`redaction.add`](#redaction.add-v1) | EditProject | Add Redaction |  |
| [`redaction.detectFaces`](#redaction.detectFaces-v1) | EditProject | Redact Faces |  |
| [`redaction.list`](#redaction.list-v1) | Read | List Redactions |  |
| [`redaction.override`](#redaction.override-v1) | EditProject | Redaction Override |  |
| [`redaction.remove`](#redaction.remove-v1) | EditProject | Remove Redaction |  |
| [`redaction.update`](#redaction.update-v1) | EditProject | Update Redaction |  |
| [`reframe.createSequence`](#reframe.createSequence-v1) | EditProject | Auto Reframe |  |
| [`reframe.rerun`](#reframe.rerun-v1) | EditProject | Reframe Again |  |
| [`render.deletePreviewFiles`](#render.deletePreviewFiles-v1) | MediaAnalysis | Delete Render Files |  |
| [`render.previewInOut`](#render.previewInOut-v1) | MediaAnalysis | Render In to Out |  |
| [`render.status`](#render.status-v1) | Read | Render Bar |  |
| [`review.add`](#review.add-v1) | EditProject | Add Review Note |  |
| [`review.exportPackage`](#review.exportPackage-v1) | EditProject | Export Review Package |  |
| [`review.importComments`](#review.importComments-v1) | EditProject | Import Review Comments |  |
| [`review.importPackage`](#review.importPackage-v1) | EditProject | Import Review Package |  |
| [`review.list`](#review.list-v1) | Read | Review Notes |  |
| [`review.remove`](#review.remove-v1) | EditProject | Delete Review Note |  |
| [`review.reply`](#review.reply-v1) | EditProject | Reply to Review Note |  |
| [`review.resolve`](#review.resolve-v1) | EditProject | Resolve Review Note |  |
| [`review.toMarkers`](#review.toMarkers-v1) | EditSequence | Review Notes to Markers |  |
| [`roughcut.fromScript`](#roughcut.fromScript-v1) | EditProject | Rough Cut from Script |  |
| [`roughcut.swapTake`](#roughcut.swapTake-v1) | EditSequence | Swap Take |  |
| [`scopes.read`](#scopes.read-v1) | Read | Read Scopes |  |
| [`search.footage`](#search.footage-v1) | Read | Search Footage |  |
| [`search.indexMedia`](#search.indexMedia-v1) | MediaAnalysis | Index Media for Search |  |
| [`sequence.addEdit`](#sequence.addEdit-v1) | EditSequence | Add Edit | `timeline.razor` |
| [`sequence.addEditToAllTracks`](#sequence.addEditToAllTracks-v1) | EditSequence | Add Edit to All Tracks | `timeline.razorAll` |
| [`sequence.addSubmixTrack`](#sequence.addSubmixTrack-v1) | EditSequence | Add Audio Submix Track |  |
| [`sequence.addTrack`](#sequence.addTrack-v1) | EditProject | Add Track |  |
| [`sequence.applyAudioTransition`](#sequence.applyAudioTransition-v1) | EditSequence | Apply Audio Transition |  |
| [`sequence.applyDefaultTransitions`](#sequence.applyDefaultTransitions-v1) | EditSequence | Apply Default Transitions to Selection |  |
| [`sequence.applyVideoTransition`](#sequence.applyVideoTransition-v1) | EditSequence | Apply Video Transition |  |
| [`sequence.beats`](#sequence.beats-v1) | Read | List Beats |  |
| [`sequence.closeGap`](#sequence.closeGap-v1) | EditSequence | Close Gap |  |
| [`sequence.create`](#sequence.create-v1) | EditProject | New Sequence |  |
| [`sequence.creditsRoll`](#sequence.creditsRoll-v1) | EditSequence | Credits Roll from Rights Log |  |
| [`sequence.duplicate`](#sequence.duplicate-v1) | EditProject | Duplicate Sequence |  |
| [`sequence.exportFrame`](#sequence.exportFrame-v1) | EditProject | Export Frame | `frame.export` |
| [`sequence.exportFramesAtMarkers`](#sequence.exportFramesAtMarkers-v1) | EditProject | Export Frames at Markers |  |
| [`sequence.exportMarkers`](#sequence.exportMarkers-v1) | Read | Export Markers |  |
| [`sequence.extendNextToPlayhead`](#sequence.extendNextToPlayhead-v1) | EditSequence | Extend Next Edit to Playhead |  |
| [`sequence.extendPreviousToPlayhead`](#sequence.extendPreviousToPlayhead-v1) | EditSequence | Extend Previous Edit to Playhead |  |
| [`sequence.extract`](#sequence.extract-v1) | EditSequence | Extract |  |
| [`sequence.goToGap`](#sequence.goToGap-v1) | Read | Go to Gap |  |
| [`sequence.joinThroughEdits`](#sequence.joinThroughEdits-v1) | EditSequence | Join Through Edits |  |
| [`sequence.lift`](#sequence.lift-v1) | EditSequence | Lift |  |
| [`sequence.makeSubsequence`](#sequence.makeSubsequence-v1) | EditProject | Make Subsequence |  |
| [`sequence.makeVersion`](#sequence.makeVersion-v1) | EditProject | Make Vertical or Square Version |  |
| [`sequence.matchFrame`](#sequence.matchFrame-v1) | EditProject | Match Frame |  |
| [`sequence.mediaUsage`](#sequence.mediaUsage-v1) | Read | Media Usage Report |  |
| [`sequence.moveTrack`](#sequence.moveTrack-v1) | EditProject | Move Track |  |
| [`sequence.nest`](#sequence.nest-v1) | EditProject | Nest |  |
| [`sequence.newFromClip`](#sequence.newFromClip-v1) | EditProject | New Sequence From Clip |  |
| [`sequence.newFromClips`](#sequence.newFromClips-v1) | EditProject | New Sequences From Clips |  |
| [`sequence.presets`](#sequence.presets-v1) | Read | Sequence Presets |  |
| [`sequence.removeDeadTime`](#sequence.removeDeadTime-v1) | EditSequence | Remove Dead Time |  |
| [`sequence.removeDisabledClips`](#sequence.removeDisabledClips-v1) | EditSequence | Remove Disabled Clips |  |
| [`sequence.removeTrack`](#sequence.removeTrack-v1) | EditProject | Delete Track |  |
| [`sequence.removeTransitions`](#sequence.removeTransitions-v1) | EditSequence | Remove Transitions |  |
| [`sequence.reverseMatchFrame`](#sequence.reverseMatchFrame-v1) | Read | Reverse Match Frame |  |
| [`sequence.rippleTrimNextToPlayhead`](#sequence.rippleTrimNextToPlayhead-v1) | EditSequence | Ripple Trim Next Edit to Playhead |  |
| [`sequence.rippleTrimPreviousToPlayhead`](#sequence.rippleTrimPreviousToPlayhead-v1) | EditSequence | Ripple Trim Previous Edit to Playhead |  |
| [`sequence.setBeatGrid`](#sequence.setBeatGrid-v1) | EditSequence | Set Beat Grid |  |
| [`sequence.setMasterFormat`](#sequence.setMasterFormat-v1) | EditSequence | Set Master Channel Format |  |
| [`sequence.setMasterGain`](#sequence.setMasterGain-v1) | EditSequence | Set Master Gain |  |
| [`sequence.setPatches`](#sequence.setPatches-v1) | Read | Set Source Patching |  |
| [`sequence.setSettings`](#sequence.setSettings-v1) | EditProject | Sequence Settings |  |
| [`sequence.setTargets`](#sequence.setTargets-v1) | EditSequence | Set Track Targets |  |
| [`sequence.settings`](#sequence.settings-v1) | Read | Sequence Settings Info |  |
| [`sequence.summary`](#sequence.summary-v1) | Read | Sequence Summary |  |
| [`sequence.trimNextToPlayhead`](#sequence.trimNextToPlayhead-v1) | EditSequence | Trim Next Edit to Playhead |  |
| [`sequence.trimPreviousToPlayhead`](#sequence.trimPreviousToPlayhead-v1) | EditSequence | Trim Previous Edit to Playhead |  |
| [`sequence.unnest`](#sequence.unnest-v1) | EditSequence | Unnest |  |
| [`shorts.create`](#shorts.create-v1) | EditProject | Create Shorts |  |
| [`shorts.find`](#shorts.find-v1) | Read | Find Shorts |  |
| [`source.addMarker`](#source.addMarker-v1) | EditProject | Add Clip Marker |  |
| [`source.clearMarks`](#source.clearMarks-v1) | EditProject | Clear Source Marks |  |
| [`source.exportFrame`](#source.exportFrame-v1) | EditProject | Export Frame (Source Monitor) |  |
| [`source.importMarkers`](#source.importMarkers-v1) | EditProject | Import Clip Markers |  |
| [`source.load`](#source.load-v1) | Read | Load Source |  |
| [`source.removeMarker`](#source.removeMarker-v1) | EditProject | Remove Clip Marker |  |
| [`source.setMarks`](#source.setMarks-v1) | EditProject | Set Source Marks |  |
| [`source.updateMarker`](#source.updateMarker-v1) | EditProject | Edit Clip Marker |  |
| [`sources.add`](#sources.add-v1) | EditSequence | Add Source |  |
| [`sources.export`](#sources.export-v1) | Read | Export Sources |  |
| [`sources.list`](#sources.list-v1) | Read | List Sources |  |
| [`sources.lowerThirds`](#sources.lowerThirds-v1) | EditSequence | Show Citations as Lower Thirds |  |
| [`sources.remove`](#sources.remove-v1) | EditSequence | Remove Source |  |
| [`sources.update`](#sources.update-v1) | EditSequence | Edit Source |  |
| [`spelling.check`](#spelling.check-v1) | Read | Check Spelling |  |
| [`takes.find`](#takes.find-v1) | Read | Find Takes |  |
| [`takes.use`](#takes.use-v1) | EditSequence | Use Take |  |
| [`template.clearFieldKeys`](#template.clearFieldKeys-v1) | EditSequence | Stop Animating a Template Field |  |
| [`template.export`](#template.export-v1) | Read | Export Template |  |
| [`template.import`](#template.import-v1) | EditProject | Import Motion Graphics Template |  |
| [`template.importFolder`](#template.importFolder-v1) | EditProject | Import a Folder of Templates |  |
| [`template.instantiate`](#template.instantiate-v1) | EditSequence | Add Template |  |
| [`template.list`](#template.list-v1) | Read | Templates |  |
| [`template.removeFieldKey`](#template.removeFieldKey-v1) | EditSequence | Remove a Template Field Key |  |
| [`template.resolve`](#template.resolve-v1) | Read | Template Draw List |  |
| [`template.save`](#template.save-v1) | EditProject | Save as Template |  |
| [`template.setField`](#template.setField-v1) | EditSequence | Edit Template Fields |  |
| [`template.setFieldKey`](#template.setFieldKey-v1) | EditSequence | Key a Template Field |  |
| [`template.setFolder`](#template.setFolder-v1) | EditProject | File a Template in a Folder |  |
| [`textStyle.apply`](#textStyle.apply-v1) | EditSequence | Apply Text Style |  |
| [`textStyle.applyRange`](#textStyle.applyRange-v1) | EditSequence | Apply Text Style to Part of a Text |  |
| [`textStyle.create`](#textStyle.create-v1) | EditSequence | Create Text Style |  |
| [`textStyle.list`](#textStyle.list-v1) | Read | Text Styles |  |
| [`textStyle.move`](#textStyle.move-v1) | EditProject | Reorder Text Styles |  |
| [`textStyle.remove`](#textStyle.remove-v1) | EditProject | Delete Text Style |  |
| [`textStyle.rename`](#textStyle.rename-v1) | EditProject | Rename Text Style |  |
| [`textStyle.update`](#textStyle.update-v1) | EditProject | Push to Text Style |  |
| [`timeRemap.addHold`](#timeRemap.addHold-v1) | EditSequence | Add Hold Segment |  |
| [`timeRemap.addKey`](#timeRemap.addKey-v1) | EditSequence | Add Time Remap Key |  |
| [`timeRemap.addReverse`](#timeRemap.addReverse-v1) | EditSequence | Add Reverse Segment |  |
| [`timeRemap.enable`](#timeRemap.enable-v1) | EditSequence | Time Remapping |  |
| [`timeRemap.moveKey`](#timeRemap.moveKey-v1) | EditSequence | Move Time Remap Key |  |
| [`timeRemap.removeKey`](#timeRemap.removeKey-v1) | EditSequence | Remove Time Remap Key |  |
| [`timeRemap.setInterpolation`](#timeRemap.setInterpolation-v1) | EditSequence | Set Time Remap Interpolation |  |
| [`timeRemap.setKey`](#timeRemap.setKey-v1) | EditSequence | Set Time Remap Key |  |
| [`timeRemap.setSpeed`](#timeRemap.setSpeed-v1) | EditSequence | Set Segment Speed |  |
| [`timeline.removeSilences`](#timeline.removeSilences-v1) | EditSequence | Remove Silences |  |
| [`timeline.rippleDelete`](#timeline.rippleDelete-v1) | EditSequence | Ripple Delete | `edit.rippleDelete` |
| [`title.add`](#title.add-v1) | EditSequence | New Title |  |
| [`title.setFont`](#title.setFont-v1) | EditSequence | Set Title Font |  |
| [`title.setText`](#title.setText-v1) | EditSequence | Edit Title Text |  |
| [`track.addEffect`](#track.addEffect-v1) | EditSequence | Add Video Track Effect |  |
| [`track.addKeyframe`](#track.addKeyframe-v1) | EditSequence | Add Video Track Effect Keyframe |  |
| [`track.addSend`](#track.addSend-v1) | EditSequence | Add Track Send |  |
| [`track.effects`](#track.effects-v1) | Read | Video Track Effect Stack |  |
| [`track.mask`](#track.mask-v1) | EditSequence | Track Mask |  |
| [`track.moveEffect`](#track.moveEffect-v1) | EditSequence | Reorder Video Track Effect |  |
| [`track.removeEffect`](#track.removeEffect-v1) | EditSequence | Remove Video Track Effect |  |
| [`track.removeKeyframe`](#track.removeKeyframe-v1) | EditSequence | Remove Video Track Effect Keyframe |  |
| [`track.removeSend`](#track.removeSend-v1) | EditSequence | Remove Track Send |  |
| [`track.setEffect`](#track.setEffect-v1) | EditSequence | Set Video Track Effect |  |
| [`track.setFormat`](#track.setFormat-v1) | EditSequence | Set Track Channel Format |  |
| [`track.setOutput`](#track.setOutput-v1) | EditSequence | Set Track Output |  |
| [`track.setPan`](#track.setPan-v1) | EditSequence | Set Track Pan |  |
| [`track.setRecordArm`](#track.setRecordArm-v1) | EditSequence | Set Track Record Arm |  |
| [`track.setRole`](#track.setRole-v1) | EditSequence | Set Track Role |  |
| [`track.setSend`](#track.setSend-v1) | EditSequence | Set Track Send |  |
| [`track.setState`](#track.setState-v1) | EditSequence | Set Track State |  |
| [`track.setSurroundPan`](#track.setSurroundPan-v1) | EditSequence | Set Track Surround Panner |  |
| [`track.setVolume`](#track.setVolume-v1) | EditSequence | Set Track Volume |  |
| [`track.transform`](#track.transform-v1) | EditSequence | Track Motion |  |
| [`trackGroup.addTracks`](#trackGroup.addTracks-v1) | EditSequence | Add Tracks to Group |  |
| [`trackGroup.create`](#trackGroup.create-v1) | EditSequence | Group Tracks |  |
| [`trackGroup.removeTracks`](#trackGroup.removeTracks-v1) | EditSequence | Remove Tracks from Group |  |
| [`trackGroup.route`](#trackGroup.route-v1) | EditSequence | Route Track Group |  |
| [`trackGroup.set`](#trackGroup.set-v1) | EditSequence | Track Group Settings |  |
| [`trackGroup.setTracksState`](#trackGroup.setTracksState-v1) | EditSequence | Set Track Group State |  |
| [`trackGroup.ungroup`](#trackGroup.ungroup-v1) | EditSequence | Ungroup Tracks |  |
| [`transcript.addSpeaker`](#transcript.addSpeaker-v1) | EditProject | New Speaker |  |
| [`transcript.assignSpeaker`](#transcript.assignSpeaker-v1) | EditProject | Assign Speaker |  |
| [`transcript.cleanup`](#transcript.cleanup-v1) | EditSequence | Remove Fillers and Silences |  |
| [`transcript.correctFromScript`](#transcript.correctFromScript-v1) | EditProject | Correct from Script |  |
| [`transcript.correctRange`](#transcript.correctRange-v1) | EditProject | Correct Words |  |
| [`transcript.correctWord`](#transcript.correctWord-v1) | EditProject | Correct Word |  |
| [`transcript.corrections`](#transcript.corrections-v1) | Read | Transcript Corrections |  |
| [`transcript.customWords`](#transcript.customWords-v1) | Read | List Custom Words |  |
| [`transcript.delete`](#transcript.delete-v1) | MediaAnalysis | Delete Transcript |  |
| [`transcript.deleteRanges`](#transcript.deleteRanges-v1) | EditSequence | Delete Words |  |
| [`transcript.export`](#transcript.export-v1) | Read | Export Transcript |  |
| [`transcript.find`](#transcript.find-v1) | Read | Find in Transcript |  |
| [`transcript.findReplace`](#transcript.findReplace-v1) | EditProject | Find and Replace in Transcript |  |
| [`transcript.keepRanges`](#transcript.keepRanges-v1) | EditSequence | Keep Only Words |  |
| [`transcript.mediaWords`](#transcript.mediaWords-v1) | Read | Media Transcript Words |  |
| [`transcript.mergeSpeakers`](#transcript.mergeSpeakers-v1) | EditProject | Merge Speakers |  |
| [`transcript.moveRange`](#transcript.moveRange-v1) | EditSequence | Move Words |  |
| [`transcript.renameSpeaker`](#transcript.renameSpeaker-v1) | EditProject | Rename Speaker |  |
| [`transcript.review`](#transcript.review-v1) | Read | Review Fillers and Silences |  |
| [`transcript.setCustomWords`](#transcript.setCustomWords-v1) | EditProject | Custom Words |  |
| [`transcript.speakers`](#transcript.speakers-v1) | Read | Speakers |  |
| [`transcript.text`](#transcript.text-v1) | Read | Transcript Text |  |
| [`transcript.transcribe`](#transcript.transcribe-v1) | MediaAnalysis | Transcribe Media |  |
| [`transcript.words`](#transcript.words-v1) | Read | Transcript Words |  |
| [`transition.add`](#transition.add-v1) | EditSequence | Add Transition |  |
| [`transition.remove`](#transition.remove-v1) | EditSequence | Remove Transition |  |
| [`transition.set`](#transition.set-v1) | EditSequence | Set Transition |  |
| [`vr.info`](#vr.info-v1) | Read | VR Video Info |  |
| [`vr.reorient`](#vr.reorient-v1) | EditSequence | VR Reorient |  |
| [`workflow.podcast`](#workflow.podcast-v1) | EditProject | Podcast Workflow |  |

<a id="adjustment.add-v1"></a>

### `adjustment.add` v1: New Adjustment Layer

Adds an adjustment layer clip (File > New > Adjustment Layer, dropped on the timeline): its effects (colour grade, blur, masks, keyframes) apply to the composite of every video track below it for its duration; its Opacity and blend mode mix the adjusted picture over the unadjusted one. Inside a nested sequence it adjusts that sequence's lower tracks only. "media" reuses an existing adjustment layer item; without it a new one is made for the sequence. Add effects with clip.addEffect. createdIds: the clip (and the new item).

Capability `EditSequence`. Tool name `adjustment_add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string | yes | Where it starts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `duration` | integer \| string |  | How long it lasts. Default 5 seconds. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `media` | integer \| string (1..) |  | An existing adjustment layer item to reuse. A media id or exact name. |
| `placement` | `targeted` \| `freeTrackAtOrAbove` \| `newTrack` |  | "targeted" (default): on "track", overwriting what is there. "freeTrackAtOrAbove": the first unlocked video track from "track" upwards that is free over its range. "newTrack": a new video track on top. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) |  | A video track. Default: the first targeted unlocked video track. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"adjustment.add","params":{"at":"00:00:10:00","duration":"00:00:05:00","placement":"newTrack"}}
```

<a id="analysis.detectBeats-v1"></a>

### `analysis.detectBeats` v1: Detect Beats

Finds the tempo and beats of a clip's music: beat markers labelled 1-2-3-4 (downbeats red) and the sequence's beat grid (snapping, edit.snapToBeats and edit.cutToBeats use it). For a media item the beats are listed in media time. Results are cached per source file.

Capability `EditSequence`. Tool name `analysis_detectBeats`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `beatsPerBar` | integer (1..32) |  | Default 4. |
| `clip` | integer \| object (1..) |  | The music clip. A clip id, or {"track", "time"} for the clip there. |
| `grid` | boolean |  | Set the sequence's beat grid from the beats. Default true. |
| `markers` | `all` \| `downbeats` \| `none` |  | Which beats get markers. Default all. |
| `maxBpm` | number (30.0..600.0) |  | Fastest tempo to consider. Default 200. |
| `media` | integer \| string (1..) |  | Or a media item. A media id or exact name. |
| `minBpm` | number (20.0..400.0) |  | Slowest tempo to consider. Default 60. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"analysis.detectBeats","params":{"clip":{"time":"00:00:00:00","track":"A1"},"markers":"downbeats"}}
```

<a id="analysis.detectShots-v1"></a>

### `analysis.detectShots` v1: Detect Shots

Finds the cuts (hard cuts, fades through black, dissolves; flashes are not cuts) in a clip's video and, for a clip, marks them with sequence markers ("markers": false to only list them). For a media item the cuts are listed in media time. Results are cached per source file.

Capability `EditSequence`. Tool name `analysis_detectShots`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The clip. A clip id, or {"track", "time"} for the clip there. |
| `detector` | `combined` \| `content` \| `adaptive` \| `threshold` |  | Detector: combined (hard cuts, fades, dissolves; flashes ignored; default), content, adaptive or threshold (fades through black). |
| `markers` | boolean |  | Add a marker at each cut in the clip. Default true. |
| `media` | integer \| string (1..) |  | Or a media item. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `threshold` | number (1.0..255.0) |  | Content threshold (default 27; lower finds more cuts). |

```json
{"command":"analysis.detectShots","params":{"clip":{"time":"00:00:10:00","track":"V1"}}}
```

<a id="analysis.speakers-v1"></a>

### `analysis.speakers` v1: Who Speaks When

Active-speaker detection for a multicam source, on this computer: who speaks when (with crosstalk, silence and laughter) and which angle shows whom (close-up or wide), from the transcripts with their speakers, each angle's microphone level and each camera's faces and lip motion. Nothing changes. "angles" sets angles yourself; they win over the guesses.

Capability `MediaAnalysis`. Tool name `analysis_speakers`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `angles` | object[] |  | Your angle assignments; they win over the guesses. |
| `angles[].angle` | integer (1..64) | yes | The angle (1 = the first). |
| `angles[].role` | `closeUp` \| `wide` \| `unknown` |  | What it shows. |
| `angles[].speaker` | string |  | Whom a close-up shows: a speaker's key or name (analysis.speakers lists them). |
| `clip` | integer (1..) |  | A multicam clip of the sequence (default: its first). |
| `faces` | boolean |  | Look at the cameras' faces (default true; false is quicker on long recordings: the angles' roles then come from your assignments and the microphones). |
| `multicam` | integer (1..) |  | Or the multicam source sequence itself. |
| `segments` | integer (0..100000) |  | At most this many segments listed (default 400). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"analysis.speakers","params":{}}
```

<a id="analysis.subjects-v1"></a>

### `analysis.subjects` v1: Find Subjects

Finds the faces, people and salient regions of a media item over time, shot by shot (5-10 samples a second, denser while the picture moves; faces and people tracked within each shot). Reframing uses it; the result is cached per file, so running it again is cheap. Runs locally: nothing leaves the computer.

Capability `MediaAnalysis`. Tool name `analysis_subjects`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) | yes | Media id or exact name. |
| `stream` | integer (0..15) |  | Video stream index (default 0). |

```json
{"command":"analysis.subjects","params":{"media":1}}
```

<a id="audio.addEffect-v1"></a>

### `audio.addEffect` v1: Add Track or Master Effect

Adds an audio effect to an audio track's stack (applied to the track's mix, before its fader) or to the master bus (applied to the sequence's mix, before the master fader). Parameters not given take their defaults. An audio plugin ("plugin": its effect id from plugins.list) goes in the same way; its parameters are "p<id>" keys. The new instance id is returned in createdIds.

Capability `EditSequence`. Tool name `audio_addEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | `wabcut.audio.gain` \| `wabcut.audio.eq3` \| `wabcut.audio.compressor` \| `wabcut.audio.limiter` \| `wabcut.audio.denoise` \| `wabcut.audio.filter` \| `wabcut.audio.dehum` \| `wabcut.audio.deess` \| `wabcut.audio.expander` \| `wabcut.audio.reverb` \| `wabcut.audio.width` \| `wabcut.audio.amplify` \| `wabcut.audio.channelMixer` \| `wabcut.audio.dynamics` \| `wabcut.audio.hardLimiter` \| `wabcut.audio.multibandCompressor` \| `wabcut.audio.dynamicsProcessing` \| `wabcut.audio.singleBandCompressor` \| `wabcut.audio.tubeModeledCompressor` \| `wabcut.audio.distortion` \| `wabcut.audio.guitarSuite` \| `wabcut.audio.mastering` \| `wabcut.audio.vocalEnhancer` \| `wabcut.audio.pitchShifter` \| `wabcut.audio.graphicEq10` \| `wabcut.audio.graphicEq20` \| `wabcut.audio.graphicEq30` \| `wabcut.audio.parametricEq` \| `wabcut.audio.simpleParametricEq` \| `wabcut.audio.highpass` \| `wabcut.audio.lowpass` \| `wabcut.audio.bandpass` \| `wabcut.audio.simpleNotch` \| `wabcut.audio.bass` \| `wabcut.audio.treble` \| `wabcut.audio.notchFilter` \| `wabcut.audio.scientificFilter` \| `wabcut.audio.fftFilter` \| `wabcut.audio.stereoExpander` \| `wabcut.audio.balance` \| `wabcut.audio.channelVolume` \| `wabcut.audio.fillLeftWithRight` \| `wabcut.audio.fillRightWithLeft` \| `wabcut.audio.invert` \| `wabcut.audio.swapChannels` \| `wabcut.audio.delay` \| `wabcut.audio.analogDelay` \| `wabcut.audio.multitapDelay` \| `wabcut.audio.chorusFlanger` \| `wabcut.audio.flanger` \| `wabcut.audio.phaser` \| `wabcut.audio.convolutionReverb` \| `wabcut.audio.studioReverb` \| `wabcut.audio.surroundReverb` \| `wabcut.audio.deReverb` \| `wabcut.audio.spectralDenoise` \| `wabcut.audio.adaptiveNoiseReduction` \| `wabcut.audio.automaticClickRemover` \| `wabcut.audio.spectralRepair` |  | The audio effect id (effect.list with kind "audio"). |
| `index` | integer (0..) |  | Position in the stack (0 = first). Default: last. |
| `params` | object |  | Constant parameter values by key, e.g. {"ceiling": -1}. |
| `plugin` | string |  | Or an audio plugin's effect id (plugins.list), e.g. "plugin.vst3.<id>". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the master audio bus) or an audio track: a label ("A1") or a track id. Clip stacks are edited with clip.addEffect and the other clip.* commands. |

```json
{"command":"audio.addEffect","params":{"effect":"wabcut.audio.compressor","params":{"threshold":-24},"target":"A1"}}
```

<a id="audio.addKeyframe-v1"></a>

### `audio.addKeyframe` v1: Add Track or Master Keyframe

Adds a keyframe to a parameter of a track or master effect, or (no "effect") to the fader: "volume" or "pan" - at a timeline time (track and master curves run in timeline time). A constant parameter becomes a curve.

Capability `EditSequence`. Tool name `audio_addKeyframe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) |  | The effect; omit it for the fader. The effect instance id (audio.effects lists them). |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` |  | Default linear. |
| `param` | string | yes | The parameter key (effect.list), or the fader's "volume" / "pan". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the master audio bus) or an audio track: a label ("A1") or a track id. Clip stacks are edited with clip.addEffect and the other clip.* commands. |
| `time` | integer \| string | yes | Where the key goes. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `value` | number | yes | The value, in the parameter's unit. |

```json
{"command":"audio.addKeyframe","params":{"param":"volume","target":"A2","time":"00:00:10:00","value":-18}}
```

<a id="audio.autoDuck-v1"></a>

### `audio.autoDuck` v1: Auto-Duck Music

Lowers music under speech: finds speech on the dialogue tracks (transcript words, or the audio level where there is no transcript) and writes volume keyframes on the music tracks' clips - down by "amount" over "attack" before each phrase, back up over "release" after "hold", bridging short pauses. Replaces the music clips' volume keyframes in the range (like Premiere's Auto Ducking); the keys stay editable. "preset": "gameUnderVoice" ducks a gaming recording's game audio and music under the streamer's mic: the tracks named Mic (else the dialogue-role ones) are the speech, the tracks named Game or Desktop Audio (else the effects-role ones) and the music tracks duck, -10 dB with a 0.12 s attack, 0.35 s hold and 0.8 s release; given parameters still win. The report names the tracks used. One undo step; dryRun previews it.

Capability `EditSequence`. Tool name `audio_autoDuck`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `amount` | number (-60.0..0.0) |  | How far the music goes down, dB (default -12). |
| `attack` | number (0.0..10.0) |  | Seconds to go down, ending where speech starts (default 0.25). |
| `dialogue` | (integer \| string)[], min 1 |  | Speech tracks (default: the dialogue-role tracks). |
| `from` | integer \| string |  | Only from this time (default: the start). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `hold` | number (0.0..10.0) |  | Seconds to stay down after speech (default 0.25). |
| `includeMusic` | boolean |  | gameUnderVoice: the music tracks duck with the game (default true). |
| `minSpeech` | number (0.0..5.0) |  | Ignore speech shorter than this (default 0.12). |
| `music` | (integer \| string)[], min 1 |  | Tracks to duck (default: the music-role tracks). |
| `preset` | `default` \| `gameUnderVoice` |  | The timing and tracks to start from (default: default). |
| `release` | number (0.0..10.0) |  | Seconds to come back up (default 0.75). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `source` | `auto` \| `transcript` \| `energy` |  | Where speech is found (default auto: transcripts, else the audio level). |
| `to` | integer \| string |  | Only up to this time (default: the end). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"audio.autoDuck","params":{"amount":-15,"release":1.0}}
```

<a id="audio.autoMix-v1"></a>

### `audio.autoMix` v1: Auto-Mix Voices

Levels a multi-microphone recording (podcast, interview, co-op stream): finds speech on each voice track and lowers the volume of the microphones whose speaker is silent while someone else talks, with keyframes on that track's clips. A voice is never lowered under its own words (it stays on for "hold" after its last word, and its microphone is back up "lead" before its next first word, also when it starts talking over someone); where both speak nothing is lowered; short pauses are bridged. With "gate" a microphone is also lowered while nobody speaks. The voice tracks are "tracks" (default: the dialogue-role tracks, at least two). Replaces those clips' volume keyframes in the range (like audio.autoDuck); the keys stay editable. The report lists each voice's speech runs, lowered regions and clips. One undo step; dryRun previews it.

Capability `EditSequence`. Tool name `audio_autoMix`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `amount` | number (-60.0..0.0) |  | How far a silent microphone goes down, dB (default -12). |
| `attack` | number (0.0..10.0) |  | Seconds to go down (default 0.1). |
| `from` | integer \| string |  | Only from this time (default: the start). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `gate` | boolean |  | Also lower a microphone while nobody is speaking (default false). |
| `hold` | number (0.0..10.0) |  | Seconds a voice stays on after its last word (default 0.4). |
| `lead` | number (0.0..5.0) |  | A microphone is back up this long before its speaker's first word (default 0.08). |
| `minSpeech` | number (0.0..5.0) |  | Ignore speech shorter than this: a cough, "mm" (default 0.2). |
| `release` | number (0.0..10.0) |  | Seconds to come back up (default 0.5). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `source` | `auto` \| `transcript` \| `energy` |  | Where speech is found (default auto: transcripts, else the audio level). |
| `to` | integer \| string |  | Only up to this time (default: the end). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `tracks` | (integer \| string)[], min 2 |  | The voice tracks, one per microphone (default: the dialogue-role tracks; at least two). |

```json
{"command":"audio.autoMix","params":{"amount":-15,"tracks":["A1","A2"]}}
```

<a id="audio.autoTag-v1"></a>

### `audio.autoTag` v1: Auto-tag Audio

Essential Sound's automatic tagging: listens to each audio clip (up to 30 s from its middle) and tags it Dialogue, Music, SFX or Ambience, with a local, deterministic classifier (syllable-rate modulation, pitch glides and held notes, beat regularity, pauses, spectral flatness, crest; nothing leaves the computer). The tag is the clip's Essential Sound record: the panel shows the type and offers its presets; no effect is added. Clips that already have a type keep it unless "overwrite". Without "clips" or "tracks": every audio clip of the sequence. dryRun previews the proposals with their confidence; applying them is one undo step.

Capability `EditSequence`. Tool name `audio_autoTag`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[] |  | The audio clips (video clips stand for their linked audio). |
| `minConfidence` | number (0.0..1.0) |  | Tag only above this confidence, 0..1 (default 0). |
| `overwrite` | boolean |  | Retag clips that already have a type (default false). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (integer \| string)[] |  | Every clip of these audio tracks. |

```json
{"command":"audio.autoTag","params":{"tracks":["A2"]}}
```

<a id="audio.bleepProfanity-v1"></a>

### `audio.bleepProfanity` v1: Bleep Profanity

Bleeps (a 1 kHz tone) or mutes the profane words the sequence's transcript finds, in the sound: one audio redaction per media item over those words (with "padding" each side), so they stay bleeped in every use of the media, in preview and export, and captions over them read [bleep]. The picture is untouched. Built-in English and Thai lists, "profanityWords" added, "allowedWords" taken out. dryRun lists the words first. One undo step. A sound file of your own instead of the tone: audio.replaceProfanity.

Capability `EditProject`. Tool name `audio_bleepProfanity`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `allowedWords` | string[] |  | Words of the built-in lists to allow. |
| `mode` | `bleep` \| `mute` |  | Bleep (default) or mute. |
| `padding` | number (0.0..1.0) |  | Seconds added before and after each word. Default 0.02. |
| `profanityWords` | string[] |  | Words to treat as profane besides the built-in English and Thai lists. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.bleepProfanity","params":{"mode":"bleep"}}
```

<a id="audio.channelLevels-v1"></a>

### `audio.channelLevels` v1: Measure Channel Levels

Peak and RMS of each source channel of audio clips (the clip's source range, at most 120 s), and the channels 30 dB or more under the loudest ("silentChannels"): which microphone is on which channel, and whether a recording has sound on one side only. Decodes media; changes nothing.

Capability `MediaAnalysis`. Tool name `audio_channelLevels`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[], min 1 | yes | The audio clips (video clips stand for their linked audio). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.channelLevels","params":{"clips":[12]}}
```

<a id="audio.channels-v1"></a>

### `audio.channels` v1: Audio Channels

What each audio clip plays (its source channel count and channel map: the clip's, its media item's or none), each audio track's channel format and 5.1 panner, and the master's format. A read: it changes nothing.

Capability `Read`. Tool name `audio_channels`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[], min 1 |  | The audio clips (video clips stand for their linked audio). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.channels","params":{}}
```

<a id="audio.effects-v1"></a>

### `audio.effects` v1: Audio Effect Stack

The effect stack of an audio track or of the master audio bus: instance ids, effect ids, enabled, parameter values (constants or keyframes in timeline ticks), and the fader after it: "volume" (dB) and, for tracks, "pan".

Capability `Read`. Tool name `audio_effects`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the master audio bus) or an audio track: a label ("A1") or a track id. Clip stacks are edited with clip.addEffect and the other clip.* commands. |

```json
{"command":"audio.effects","params":{"target":"master"}}
```

<a id="audio.enhanceSpeech-v1"></a>

### `audio.enhanceSpeech` v1: Enhance Speech

Studio-quality dialogue (Premiere's Enhance Speech, Resolve's Voice Isolation): renders each clip's media with DeepFilterNet3 on this computer (no audio leaves it; the model downloads on first use), records the enhanced take on the media item and puts the Enhance Speech effect first in each clip's stack: the clip plays the take blended with the original by "strength" (toggle the effect to compare), before its level and redactions. A take already rendered for the same file and model is reused. Video clips stand for their linked audio. With "render": false only the effect is applied (it plays the original until the take is rendered). Records the take on the media item, so it is a project edit. One undo step.

Capability `EditProject`. Tool name `audio_enhanceSpeech`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[], min 1 | yes | The clips to enhance. |
| `render` | boolean |  | Render missing takes (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `strength` | number (0.0..100.0) |  | Mix of the enhanced take, percent (default 100). |

```json
{"command":"audio.enhanceSpeech","params":{"clips":[12,15],"strength":80}}
```

<a id="audio.eqMatch-v1"></a>

### `audio.eqMatch` v1: EQ Match

Matches the tone of clips to a reference (Audition's Match EQ, Premiere's tone matching): listens to the reference (a clip, a track, a range of the mix or a media file) and to the target clips, averages the spectrum of each over the stretches that are not pauses, and gives every target a Graphic Equalizer (30 Bands) whose sliders are the smoothed difference: 24 bands from 79 Hz to 16 kHz, at most +-18 dB, a minimum-phase correction that costs no latency and plays the same in preview and export. The overall level is left alone (audio.levelMatch matches that). An earlier EQ Match on the clip is replaced. The instance is an ordinary effect, editable in Effect Controls, and keeps both spectra for the panel. "apply": false only learns and reports the curve. One undo step. Refused: a silent reference or target, or one with less than a second of audible sound.

Capability `EditSequence`. Tool name `audio_eqMatch`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `amount` | number (0.0..100.0) |  | How much of the correction to apply, percent. Default 100. |
| `apply` | boolean |  | Add the effect (default true); false: only learn and report. |
| `clips` | (integer \| object)[] | yes | The audio clips (video clips stand for their linked audio). |
| `from` | integer \| string |  | Learn only from this time of the target (default: the start). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `maxGain` | number (0.0..18.0) |  | The most any band may be moved, dB. Default 18. |
| `reference` | object \| string | yes | What to match: "mix" (the sequence's mix), {"clip": id}, {"track": "A1"} (a track on its own), or {"media": id [, "stream": n]} (an audio file of the project, its audio stream n from 0). Clips and the mix are heard as they render, effects included. Limit the stretch with referenceFrom / referenceTo (a media file: from its start). |
| `referenceFrom` | integer \| string |  | Use the reference from this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `referenceTo` | integer \| string |  | Use the reference up to this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `smoothing` | number (0.0..100.0) |  | How smooth the curve is, percent: 0 follows each band, 100 is gentle. Default 50. |
| `to` | integer \| string |  | Learn only up to this time of the target (default: the end). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"audio.eqMatch","params":{"clips":[15],"reference":{"clip":12}}}
```

<a id="audio.eqMatchReset-v1"></a>

### `audio.eqMatchReset` v1: Reset EQ Match

Takes the EQ Match off the clips (the effect audio.eqMatch added; other equalizers stay). One undo step.

Capability `EditSequence`. Tool name `audio_eqMatchReset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[] | yes | The audio clips (video clips stand for their linked audio). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.eqMatchReset","params":{"clips":[15]}}
```

<a id="audio.essentialSound-v1"></a>

### `audio.essentialSound` v1: Essential Sound

Premiere's Essential Sound panel: tags clips as dialogue, music, sfx or ambience and applies a preset of that type as ordinary, editable effects at the top of each clip's stack (an Essential Sound record with the type and options, then one marked effect per setting: Gain for loudness; Dialogue Repair: Reduce Rumble (high-pass), DeHum, Reduce Noise or Enhance Speech, Noise Gate, Reduce Reverb (expander), DeEss; Clarity: EQ preset (podcastVoice, phone, radio, streamerVoice), Dynamics (compressor) and a true-peak limiter last; Creative: reverb (smallRoom, largeRoom, hall, plate, outside); Stereo Width), then matches each clip's loudness to the type's target. Music and ambience can duck under the dialogue (volume keyframes) and music can be remixed to a duration ("duration"); sound effects take a position ("pan"). Applying again replaces what Essential Sound put there; "type": "none" removes it. Presets - dialogue: default, streamerVoice (a gaming mic at -16 LUFS: rumble cut, Reduce Noise, gate, de-esser, EQ, compressor with makeup, limiter at -1 dBTP), cleanUpNoisy, podcastVoice, phone, radio, reduceRoomEcho, removeHum, smallRoom, largeRoom, outside; music: default, background (ducked), wide; sfx: default, background; ambience: default, underDialogue (ducked), wide. "options" change a preset's settings. "tracks" applies it to every clip of those tracks (the mic track of a recording in one request). Without "type" a clip keeps its recorded type, else its track's role. One undo step; dryRun previews it.

Capability `EditSequence`. Tool name `audio_essentialSound`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[], min 1 |  | The clips (video clips stand for their linked audio). |
| `options` | object |  |  |
| `options.clarity` | object |  |  |
| `options.clarity.dynamics` | number (0.0..1.0) |  | Dynamics (a compressor) amount, 0 = off .. 1. |
| `options.clarity.eq` | `none` \| `podcastVoice` \| `phone` \| `radio` \| `streamerVoice` |  | An EQ preset. |
| `options.clarity.eqAmount` | number (0.0..1.0) |  | 0 = off .. 1. |
| `options.clarity.limiter` | boolean |  | A true-peak limiter, the last effect of the stack (default false; streamerVoice: true). |
| `options.clarity.limiterCeiling` | number (-24.0..0.0) |  | The limiter's ceiling, dBTP (default -1). |
| `options.clarity.makeup` | boolean |  | The compressor gives back the gain it takes from a voice at the loudness target (default false; streamerVoice: true). |
| `options.creative` | object |  |  |
| `options.creative.amount` | number (0.0..1.0) |  | The reverb's mix, 0..1 (default: the preset's). |
| `options.creative.reverb` | `none` \| `smallRoom` \| `largeRoom` \| `hall` \| `plate` \| `outside` |  | A reverb preset. |
| `options.ducking` | object |  |  |
| `options.ducking.amount` | number (-60.0..0.0) |  | How far it goes down, dB (default -12). |
| `options.ducking.attack` | number (0.0..10.0) |  | Seconds to go down (default 0.25). |
| `options.ducking.enabled` | boolean |  | Duck under dialogue (default true when "ducking" is given). |
| `options.ducking.hold` | number (0.0..10.0) |  | Seconds to stay down after speech (default 0.25). |
| `options.ducking.release` | number (0.0..10.0) |  | Seconds to come back (default 0.75). |
| `options.duration` | number (0.1..36000.0) |  | Music: Remix the clip to this many seconds. |
| `options.loudness` | boolean |  | Match the clip's loudness (Premiere's Auto-Match; default true). |
| `options.pan` | number (-1.0..1.0) |  | Sound effects: position, -1 (left) .. 1 (right). |
| `options.repair` | object |  |  |
| `options.repair.deess` | number (0.0..1.0) |  | DeEss amount, 0 = off .. 1. |
| `options.repair.enhance` | boolean |  | Enhance Speech (DeepFilterNet3, a rendered take on this computer). |
| `options.repair.gate` | number (0.0..1.0) |  | Noise Gate (an expander with a high ratio, after Reduce Noise) amount, 0 = off .. 1; it closes under the voice's loudness target. |
| `options.repair.hum` | number (0.0..70.0) |  | DeHum: 50 or 60 Hz mains hum and harmonics (0 = off). |
| `options.repair.noise` | number (0.0..1.0) |  | Reduce Noise (RNNoise) amount, 0 = off .. 1. |
| `options.repair.reverb` | number (0.0..1.0) |  | Reduce Reverb (an expander) amount, 0 = off .. 1. |
| `options.repair.rumble` | number (0.0..500.0) |  | Reduce Rumble: high-pass at this frequency, Hz (0 = off; 80 is typical). |
| `options.targetLufs` | number (-60.0..0.0) |  | The loudness target, LUFS (defaults: dialogue -23, music and sfx -25, ambience -30). |
| `options.width` | number (0.0..2.0) |  | Stereo width, 0 (mono) .. 1 (unchanged) .. 2. |
| `preset` | `default` \| `streamerVoice` \| `cleanUpNoisy` \| `podcastVoice` \| `phone` \| `radio` \| `reduceRoomEcho` \| `removeHum` \| `smallRoom` \| `largeRoom` \| `outside` \| `background` \| `wide` \| `underDialogue` |  | A preset of the type (default: default). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `setTrackRole` | boolean |  | Give a track without a role the type's role (default true). |
| `tracks` | (integer \| string)[], min 1 |  | Or every clip on these audio tracks. |
| `type` | `dialogue` \| `music` \| `sfx` \| `ambience` \| `none` |  | The clips' type; none removes Essential Sound. |

```json
{"command":"audio.essentialSound","params":{"clips":[12,15],"options":{"repair":{"hum":60}},"preset":"podcastVoice","type":"dialogue"}}
```

<a id="audio.fillChannel-v1"></a>

### `audio.fillChannel` v1: Fill Channel

Premiere's Fill Left with Right and Fill Right with Left: both sides of the clips play one channel - for audio heard on one side only ("fix the left-only audio": from left). "from" is the side that has the sound. One undo step.

Capability `EditSequence`. Tool name `audio_fillChannel`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[], min 1 | yes | The audio clips (video clips stand for their linked audio). |
| `from` | `left` \| `right` | yes | The channel with the sound. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | `left` \| `right` |  | The side to fill (default: the other one). |

```json
{"command":"audio.fillChannel","params":{"clips":[12],"from":"left"}}
```

<a id="audio.levelMatch-v1"></a>

### `audio.levelMatch` v1: Level Match

Matches the loudness of the target to a reference (Premiere's Essential Sound loudness match, Audition's Match Loudness): measures the reference (a clip, a track, a range of the mix or a media file) and the target (clips, a track, or the mix) the way export hears them (EBU R128 integrated loudness, true peak, RMS, crest factor) and moves the target's normalisation gain until it meets the reference, in "mode": "loudness" (integrated loudness, default) or "peak" (true peak). "headroom" is the true peak the result may not pass (dB under full scale, default 1): with "limit" a true-peak limiter at that ceiling is added (gain plus limiting), otherwise a warning says so. Reports the measurements before and after. One undo step. Refused: a silent reference.

Capability `EditSequence`. Tool name `audio_levelMatch`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[] |  | The audio clips (video clips stand for their linked audio). |
| `from` | integer \| string |  | Measure only from this time of the target. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `headroom` | number (0.0..24.0) |  | The true peak the result may not pass, dB under full scale. Default 1. |
| `limit` | boolean |  | Add a true-peak limiter at the headroom when the result passes it. |
| `maxPasses` | integer (1..8) |  | Measure-and-adjust passes at most. Default 4. |
| `mode` | `loudness` \| `peak` |  | What to match. Default loudness. |
| `reference` | object \| string | yes | What to match: "mix" (the sequence's mix), {"clip": id}, {"track": "A1"} (a track on its own), or {"media": id [, "stream": n]} (an audio file of the project, its audio stream n from 0). Clips and the mix are heard as they render, effects included. Limit the stretch with referenceFrom / referenceTo (a media file: from its start). |
| `referenceFrom` | integer \| string |  | Use the reference from this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `referenceTo` | integer \| string |  | Use the reference up to this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Measure only up to this time of the target. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `tolerance` | number (0.01..3.0) |  | Stop within this many dB of the reference. Default 0.1. |
| `track` | integer \| string |  | Or match this audio track. A label ("A1") or a track id. |

```json
{"command":"audio.levelMatch","params":{"clips":[15],"limit":true,"reference":{"clip":12}}}
```

<a id="audio.loudness-v1"></a>

### `audio.loudness` v1: Measure Loudness

EBU R128 / ITU-R BS.1770 loudness as it renders: integrated LUFS, loudness range (LU), true peak (dBTP), sample peak, loudest momentary and short-term LUFS, and where they are (maxMomentaryAt, maxShortTermAt: where the loudest 400 ms / 3 s window starts; truePeakAt: the 100 ms block of the true peak; each a timecode and ticks on the timeline) with the true-peak overs above "oversAbove" (default -1 dBTP; at most 200 listed, overCount counts them all; a programme with more than 100,000 blocks above -6 dBTP keeps only its loudest, so "oversAbove" rises to the ceiling counted and "oversTruncated" is true). Targets: the mix (with mute/solo and the master effects: what export writes), a track (its clips and track effects) or a clip on its own.

Capability `MediaAnalysis`. Tool name `audio_loudness`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | integer \| string |  | Only from this time (default: the start). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `oversAbove` | number (-6.0..0.0) |  | List true-peak overs above this ceiling in dBTP (default -1; -6..0). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `series` | boolean |  | 0.7: also list the short-term (3 s) loudness once a second from the start (shortTermSeries, LUFS, null where unknown): what the Loudness Radar draws. |
| `targets` | (string \| object)[], min 1 |  | What to measure (default: the mix). |
| `to` | integer \| string |  | Only up to this time (default: the end). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"audio.loudness","params":{"targets":["mix",{"track":"A1"}]}}
```

<a id="audio.masterLimiter-v1"></a>

### `audio.masterLimiter` v1: Master True-Peak Limiter

Puts a true-peak brick-wall limiter at the end of the master audio bus (or updates the one there): no true peak of the mix goes above the ceiling (default -1 dBTP, the streaming and EBU R128 maximum).

Capability `EditSequence`. Tool name `audio_masterLimiter`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `ceiling` | number (-24.0..0.0) |  | Ceiling in dBTP (default -1). |
| `lookahead` | number (0.0..20.0) |  | Lookahead in ms (default 5). |
| `release` | number (1.0..1000.0) |  | Release in ms (default 50). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.masterLimiter","params":{"ceiling":-1}}
```

<a id="audio.moveEffect-v1"></a>

### `audio.moveEffect` v1: Reorder Track or Master Effect

Moves an effect to another position in a track or master stack.

Capability `EditSequence`. Tool name `audio_moveEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) | yes | The effect. The effect instance id (audio.effects lists them). |
| `index` | integer (0..) | yes | New position (0 = first). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the master audio bus) or an audio track: a label ("A1") or a track id. Clip stacks are edited with clip.addEffect and the other clip.* commands. |

```json
{"command":"audio.moveEffect","params":{"effect":77,"index":0,"target":"master"}}
```

<a id="audio.moveKeyframe-v1"></a>

### `audio.moveKeyframe` v1: Move Track or Master Keyframe

Moves keyframe `index` of a track or master effect parameter or of the fader ("volume" / "pan", no "effect") to another timeline time; fails if another key is there.

Capability `EditSequence`. Tool name `audio_moveKeyframe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) |  | The effect; omit it for the fader. The effect instance id (audio.effects lists them). |
| `index` | integer (0..) | yes | Keyframe index (0 = first). |
| `param` | string | yes | The parameter key, or "volume" / "pan". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the master audio bus) or an audio track: a label ("A1") or a track id. Clip stacks are edited with clip.addEffect and the other clip.* commands. |
| `time` | integer \| string | yes | The key's new time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"audio.moveKeyframe","params":{"index":1,"param":"volume","target":"A2","time":"00:00:12:00"}}
```

<a id="audio.normalize-v1"></a>

### `audio.normalize` v1: Normalize Loudness

Sets a gain so the loudness hits a delivery target: YouTube / Spotify -14 LUFS, podcasts and Apple Podcasts -16, broadcast EBU R128 -23, ATSC -24, or a custom "lufs". The gain is the first effect of each clip's, the track's or the master's stack (adjusted when it is already there), measured again and refined until within tolerance, so compressors and limiters after it are accounted for. With "limit" the master gets the true-peak limiter at the preset's ceiling first. One undo step.

Capability `EditSequence`. Tool name `audio_normalize`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object)[], min 1 |  | Normalize each of these clips. |
| `from` | integer \| string |  | Only from this time (default: the start). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `limit` | boolean |  | Add or update the master true-peak limiter (mix only). |
| `lufs` | number (-70.0..0.0) |  | A custom integrated loudness target instead of a preset. |
| `maxPasses` | integer (1..10) |  | Most measure-and-adjust passes (default 4). |
| `preset` | `youtube` \| `spotify` \| `podcast` \| `applePodcasts` \| `broadcast` \| `atsc` |  | The delivery target (default youtube). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Only up to this time (default: the end). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `tolerance` | number (0.01..3.0) |  | Allowed error in LU (default 0.1). |
| `track` | integer \| string |  | Normalize this track. A label ("A1") or a track id. |
| `truePeak` | number (-24.0..0.0) |  | True-peak ceiling in dBTP (default: the preset's, -1). |

```json
{"command":"audio.normalize","params":{"limit":true,"preset":"podcast"}}
```

<a id="audio.remix-v1"></a>

### `audio.remix` v1: Remix Music

Premiere's Remix: retimes a music clip to a target length by rearranging its own bars - repeating or skipping sections where the music around the jump sounds alike (beats and bars, then chroma and MFCC self-similarity) - never by stretching it. The intro and the outro are kept. The clip becomes its pieces on its track (the first keeps its id; they share its level and effects) with a short constant-gain crossfade on each join just before a beat; audio.remixRender makes one rendered take instead. The report lists every join with its confidence (0..1). Target: "duration" (seconds), "end" (a timeline time) or "fit": "video" (the end of the last video clip). One undo step; dryRun previews it.

Capability `EditSequence`. Tool name `audio_remix`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | The music clip. |
| `crossfade` | number (0.0..500.0) |  | Join crossfade in ms (default: an eighth of a beat, 10..50). |
| `duration` | number (0.1..36000.0) |  | The target length in seconds. |
| `end` | integer \| string |  | Or where the music should end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `fit` | `video` |  | Or end with the last video clip. |
| `keepIntro` | boolean |  | Play the intro as it is, once (default true). |
| `keepOutro` | boolean |  | Play the outro as it is, once (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.remix","params":{"clip":31,"fit":"video"}}
```

<a id="audio.remixRender-v1"></a>

### `audio.remixRender` v1: Render Music Remix

audio.remix rendered into one file: the remix of the music clip (the same plan: its own bars rearranged, intro and outro kept, crossfaded joins) becomes a new media item derived from the song (a float WAV in the user's cache), and the clip plays it from its start. The report lists the joins like audio.remix's. Adds media, so it is a project edit. One undo step; dryRun previews it.

Capability `EditProject`. Tool name `audio_remixRender`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | The music clip. |
| `crossfade` | number (0.0..500.0) |  | Join crossfade in ms (default: an eighth of a beat, 10..50). |
| `duration` | number (0.1..36000.0) |  | The target length in seconds. |
| `end` | integer \| string |  | Or where the music should end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `fit` | `video` |  | Or end with the last video clip. |
| `keepIntro` | boolean |  | Play the intro as it is, once (default true). |
| `keepOutro` | boolean |  | Play the outro as it is, once (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.remixRender","params":{"clip":31,"duration":60}}
```

<a id="audio.removeEffect-v1"></a>

### `audio.removeEffect` v1: Remove Track or Master Effect

Removes an effect from a track or master stack.

Capability `EditSequence`. Tool name `audio_removeEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) | yes | The effect. The effect instance id (audio.effects lists them). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the master audio bus) or an audio track: a label ("A1") or a track id. Clip stacks are edited with clip.addEffect and the other clip.* commands. |

```json
{"command":"audio.removeEffect","params":{"effect":77,"target":"A1"}}
```

<a id="audio.removeKeyframe-v1"></a>

### `audio.removeKeyframe` v1: Remove Track or Master Keyframe

Removes keyframe `index` of a track or master effect parameter or of the fader ("volume" / "pan", no "effect"); the last one leaves a constant.

Capability `EditSequence`. Tool name `audio_removeKeyframe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) |  | The effect; omit it for the fader. The effect instance id (audio.effects lists them). |
| `index` | integer (0..) | yes | Keyframe index (0 = first; audio.effects lists them). |
| `param` | string | yes | The parameter key, or "volume" / "pan". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the master audio bus) or an audio track: a label ("A1") or a track id. Clip stacks are edited with clip.addEffect and the other clip.* commands. |

```json
{"command":"audio.removeKeyframe","params":{"effect":77,"index":0,"param":"ceiling","target":"master"}}
```

<a id="audio.replaceProfanity-v1"></a>

### `audio.replaceProfanity` v1: Replace Profanity with a Sound

Plays a sound file of yours (a short effect, a chosen bleep) in place of the profane words the sequence's transcript finds: like audio.bleepProfanity, one audio redaction per media item over those words (with "padding" each side), so the sound plays in every use of the media, in preview and export, and captions over them read [bleep]. The sound starts with each word and is cut at the word's end (a shorter sound is followed by silence); only its first 20 seconds are used. The file joins the project's files (found again, relinked and collected with the media); if it goes missing the tone plays. The picture is untouched. Built-in English and Thai lists, "profanityWords" added, "allowedWords" taken out. dryRun lists the words first. One undo step.

Capability `EditProject`. Tool name `audio_replaceProfanity`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `allowedWords` | string[] |  | Words of the built-in lists to allow. |
| `padding` | number (0.0..1.0) |  | Seconds added before and after each word. Default 0.02. |
| `profanityWords` | string[] |  | Words to treat as profane besides the built-in English and Thai lists. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sound` | string | yes | Path of a sound file (wav, mp3, flac, ogg, m4a...; at most 64 MiB). |

```json
{"command":"audio.replaceProfanity","params":{"sound":"C:/Sounds/boing.wav"}}
```

<a id="audio.routing-v1"></a>

### `audio.routing` v1: Audio Routing

Every audio track's output ("mix" or a submix) and sends, and every submix track with its format, output, sends, effect count, mute and solo. A read: it changes nothing.

Capability `Read`. Tool name `audio_routing`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.routing","params":{}}
```

<a id="audio.separateStems-v1"></a>

### `audio.separateStems` v1: Separate Stems

Splits a music or mixed clip's audio into vocals, drums, bass and other with HTDemucs on this computer (no audio leaves it; the model downloads on first use, and the job refuses on a computer with too little free memory). Each stem becomes a new media item, linked to the source by its provenance ("separated from X with htdemucs"), in a bin named after the source. One undo step.

Capability `EditProject`. Tool name `audio_separateStems`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) |  | The clip to separate (video clips stand for their linked audio). |
| `media` | integer (1..) |  | Or a media item. |
| `model` | `htdemucs` |  | The separation model (default htdemucs). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `stream` | integer (0..) |  | Its audio stream (with "media"; default 0). |

```json
{"command":"audio.separateStems","params":{"clip":21}}
```

<a id="audio.setChannelVolume-v1"></a>

### `audio.setChannelVolume` v1: Set Channel Volume

Premiere's Channel Volume: the volume of one channel of audio clips (1-based clip channel), dB, a constant, or with "time" a keyframe there (the channel's volume becomes animated). One undo step.

Capability `EditSequence`. Tool name `audio_setChannelVolume`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `channel` | integer (1..32) | yes | The clip channel, 1-based (1 = left, 2 = right). |
| `clips` | (integer \| object)[], min 1 | yes | The audio clips (video clips stand for their linked audio). |
| `db` | number (-144.0..24.0) | yes | The level in dB (-144 = silence .. +24). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Put a keyframe here instead of setting a constant. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"audio.setChannelVolume","params":{"channel":2,"clips":[12],"db":-6}}
```

<a id="audio.setChannels-v1"></a>

### `audio.setChannels` v1: Modify Audio Channels

Premiere's Modify > Audio Channels on clips: which source channels feed each of the clip's channels, in a clip format (mono, stereo, 5.1, adaptive), with mutes and per-channel volume. A "preset" (fillLeft, fillRight, swap, sumToMono, mono with "channel", stereo, identity, default) or a "map". Use it when the two microphones of an interview are on the left and right channels of one file, or a recording has sound on one side only. One undo step.

Capability `EditSequence`. Tool name `audio_setChannels`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `channel` | integer (1..64) |  | For the mono preset: the source channel, 1-based. |
| `clips` | (integer \| object)[], min 1 | yes | The audio clips (video clips stand for their linked audio). |
| `map` | object |  |  |
| `map.channels` | object[], min 1 | yes | One entry per clip channel (L, R, C, LFE, Ls, Rs for 5.1; L, R, C, LFE, Lrs, Rrs, Ls, Rs for 7.1). |
| `map.channels[].gainDb` | number (-144.0..24.0) |  | This channel's volume, dB. |
| `map.channels[].muted` | boolean |  | Mute this clip channel. |
| `map.channels[].sources` | (integer (1..64))[] |  | Source channels averaged into this clip channel (empty: silent). |
| `map.format` | `mono` \| `stereo` \| `5.1` \| `7.1` \| `adaptive` | yes | The clip's channel format. |
| `preset` | `default` \| `stereo` \| `identity` \| `mono` \| `sumToMono` \| `fillLeft` \| `fillRight` \| `swap` |  | default: no map (the stream as it is); stereo: source 1 and 2 as L and R; identity: every source channel as it is; mono: one source channel ("channel"); sumToMono: every channel averaged; fillLeft: both sides from the right channel (Fill Left with Right); fillRight: both from the left; swap: left and right exchanged. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.setChannels","params":{"clips":[12],"preset":"fillLeft"}}
```

<a id="audio.setEffect-v1"></a>

### `audio.setEffect` v1: Set Track or Master Effect

Enables or disables an effect of a track or master stack, or sets constant parameters.

Capability `EditSequence`. Tool name `audio_setEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) | yes | The effect. The effect instance id (audio.effects lists them). |
| `enabled` | boolean |  | On or bypassed. |
| `params` | object |  | Parameter values by key. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the master audio bus) or an audio track: a label ("A1") or a track id. Clip stacks are edited with clip.addEffect and the other clip.* commands. |

```json
{"command":"audio.setEffect","params":{"effect":77,"params":{"ceiling":-2},"target":"master"}}
```

<a id="audio.setMediaChannels-v1"></a>

### `audio.setMediaChannels` v1: Modify Media Audio Channels

Modify > Audio Channels on media items: the channel map every audio clip of the item plays unless it has its own (audio.setChannels). The same presets and maps. It changes media items, so it is a project edit. One undo step.

Capability `EditProject`. Tool name `audio_setMediaChannels`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `channel` | integer (1..64) |  | For the mono preset: the source channel, 1-based. |
| `map` | object |  |  |
| `map.channels` | object[], min 1 | yes | One entry per clip channel (L, R, C, LFE, Ls, Rs for 5.1; L, R, C, LFE, Lrs, Rrs, Ls, Rs for 7.1). |
| `map.channels[].gainDb` | number (-144.0..24.0) |  | This channel's volume, dB. |
| `map.channels[].muted` | boolean |  | Mute this clip channel. |
| `map.channels[].sources` | (integer (1..64))[] |  | Source channels averaged into this clip channel (empty: silent). |
| `map.format` | `mono` \| `stereo` \| `5.1` \| `7.1` \| `adaptive` | yes | The clip's channel format. |
| `media` | (integer \| string)[], min 1 | yes | The media items. |
| `preset` | `default` \| `stereo` \| `identity` \| `mono` \| `sumToMono` \| `fillLeft` \| `fillRight` \| `swap` |  | default: no map (the stream as it is); stereo: source 1 and 2 as L and R; identity: every source channel as it is; mono: one source channel ("channel"); sumToMono: every channel averaged; fillLeft: both sides from the right channel (Fill Left with Right); fillRight: both from the left; swap: left and right exchanged. |

```json
{"command":"audio.setMediaChannels","params":{"media":[4],"preset":"stereo"}}
```

<a id="audio.spectralAttenuate-v1"></a>

### `audio.spectralAttenuate` v1: Attenuate Spectral Selection

Lowers a region of a clip's sound on the time-frequency plane (Audition's spectral frequency display with the marquee, lasso or brush) by "gainDb" decibels, with a soft edge, and leaves the rest of the sound alone: a cough, a chair squeak, a camera click or a single note over dialogue. The region is a rectangle ("start", "end" in seconds of the clip's source, "low", "high" in Hz), a lasso ("points") or a brush stroke ("points" with "radiusSeconds" and "radiusOctaves"). Not destructive: the edit is added to the clip's Spectral Repair effect (made on the first edit), which plays the same in preview and export and stays on the sound when the clip is trimmed, moved or retimed. One undo step.

Capability `EditSequence`. Tool name `audio_spectralAttenuate`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `amount` | number (1.0..100.0) |  | Heal and noise floor: how much of the repair replaces the sound, percent (default 100). |
| `clip` | integer (1..) | yes | The audio clip (a video clip stands for its linked audio). |
| `end` | number |  | Rectangle, or the tone's time range: where it ends, in seconds of the clip's source. |
| `gainDb` | number (-96.0..0.0) |  | Attenuate: the change in level, dB (default -24). |
| `harmonics` | integer (1..16) |  | Remove Tone: the fundamental and this many harmonics are removed (1..16, default 1). |
| `high` | number (1.0..192000.0) |  | Rectangle: its highest frequency, Hz. |
| `hz` | number (1.0..192000.0) |  | Remove Tone: the frequency of a steady tone (instead of "points"). |
| `logFrequency` | boolean |  | A lasso's edges are straight on a logarithmic frequency axis (default) or a linear one. |
| `low` | number (1.0..192000.0) |  | Rectangle: its lowest frequency, Hz. |
| `points` | object[] |  | The lasso's corners, the brush stroke's centre line, or the tone's trace (its frequency over time; one point is a steady tone). |
| `points[].hz` | number (1.0..192000.0) | yes | Frequency. |
| `points[].time` | number | yes | Seconds of the clip's source. |
| `radiusOctaves` | number (0.01..10.0) |  | Brush: half its size in frequency, octaves (default 0.25). |
| `radiusSeconds` | number (0.001..60.0) |  | Brush: half its size in time, seconds (default 0.05). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shape` | `rectangle` \| `lasso` \| `brush` |  | The region's shape. Default rectangle. |
| `start` | number |  | Rectangle, or the tone's time range: where it starts, in seconds of the clip's source. |
| `widthHz` | number (1.0..500.0) |  | Remove Tone: the width of each notch, Hz (default 12). |

```json
{"command":"audio.spectralAttenuate","params":{"clip":12,"end":4.7,"gainDb":-30,"high":3200,"low":700,"start":4.2}}
```

<a id="audio.spectralHeal-v1"></a>

### `audio.spectralHeal` v1: Heal Spectral Selection

Replaces a region of a clip's sound with what surrounds it (Audition's healing brush): the level of the nearest untouched sound before, after, above and below the region, with a natural phase so no tone or musical noise is made. Regions are as in audio.spectralAttenuate; "amount" is how much of the repair replaces the sound. Heal looks about a second around the region; a longer one is filled from the sound above and below it (a warning says so). Not destructive; one undo step.

Capability `EditSequence`. Tool name `audio_spectralHeal`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `amount` | number (1.0..100.0) |  | Heal and noise floor: how much of the repair replaces the sound, percent (default 100). |
| `clip` | integer (1..) | yes | The audio clip (a video clip stands for its linked audio). |
| `end` | number |  | Rectangle, or the tone's time range: where it ends, in seconds of the clip's source. |
| `gainDb` | number (-96.0..0.0) |  | Attenuate: the change in level, dB (default -24). |
| `harmonics` | integer (1..16) |  | Remove Tone: the fundamental and this many harmonics are removed (1..16, default 1). |
| `high` | number (1.0..192000.0) |  | Rectangle: its highest frequency, Hz. |
| `hz` | number (1.0..192000.0) |  | Remove Tone: the frequency of a steady tone (instead of "points"). |
| `logFrequency` | boolean |  | A lasso's edges are straight on a logarithmic frequency axis (default) or a linear one. |
| `low` | number (1.0..192000.0) |  | Rectangle: its lowest frequency, Hz. |
| `points` | object[] |  | The lasso's corners, the brush stroke's centre line, or the tone's trace (its frequency over time; one point is a steady tone). |
| `points[].hz` | number (1.0..192000.0) | yes | Frequency. |
| `points[].time` | number | yes | Seconds of the clip's source. |
| `radiusOctaves` | number (0.01..10.0) |  | Brush: half its size in frequency, octaves (default 0.25). |
| `radiusSeconds` | number (0.001..60.0) |  | Brush: half its size in time, seconds (default 0.05). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shape` | `rectangle` \| `lasso` \| `brush` |  | The region's shape. Default rectangle. |
| `start` | number |  | Rectangle, or the tone's time range: where it starts, in seconds of the clip's source. |
| `widthHz` | number (1.0..500.0) |  | Remove Tone: the width of each notch, Hz (default 12). |

```json
{"command":"audio.spectralHeal","params":{"clip":12,"end":4.7,"high":4000,"low":300,"start":4.2}}
```

<a id="audio.spectralNoiseFloor-v1"></a>

### `audio.spectralNoiseFloor` v1: Fill Spectral Selection with Noise Floor

Replaces a region of a clip's sound with the quiet background around it: the lower quartile of the untouched sound near the region, with a natural phase. For a region whose surroundings carry speech or music that Heal would copy into it. Regions are as in audio.spectralAttenuate. Not destructive; one undo step.

Capability `EditSequence`. Tool name `audio_spectralNoiseFloor`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `amount` | number (1.0..100.0) |  | Heal and noise floor: how much of the repair replaces the sound, percent (default 100). |
| `clip` | integer (1..) | yes | The audio clip (a video clip stands for its linked audio). |
| `end` | number |  | Rectangle, or the tone's time range: where it ends, in seconds of the clip's source. |
| `gainDb` | number (-96.0..0.0) |  | Attenuate: the change in level, dB (default -24). |
| `harmonics` | integer (1..16) |  | Remove Tone: the fundamental and this many harmonics are removed (1..16, default 1). |
| `high` | number (1.0..192000.0) |  | Rectangle: its highest frequency, Hz. |
| `hz` | number (1.0..192000.0) |  | Remove Tone: the frequency of a steady tone (instead of "points"). |
| `logFrequency` | boolean |  | A lasso's edges are straight on a logarithmic frequency axis (default) or a linear one. |
| `low` | number (1.0..192000.0) |  | Rectangle: its lowest frequency, Hz. |
| `points` | object[] |  | The lasso's corners, the brush stroke's centre line, or the tone's trace (its frequency over time; one point is a steady tone). |
| `points[].hz` | number (1.0..192000.0) | yes | Frequency. |
| `points[].time` | number | yes | Seconds of the clip's source. |
| `radiusOctaves` | number (0.01..10.0) |  | Brush: half its size in frequency, octaves (default 0.25). |
| `radiusSeconds` | number (0.001..60.0) |  | Brush: half its size in time, seconds (default 0.05). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shape` | `rectangle` \| `lasso` \| `brush` |  | The region's shape. Default rectangle. |
| `start` | number |  | Rectangle, or the tone's time range: where it starts, in seconds of the clip's source. |
| `widthHz` | number (1.0..500.0) |  | Remove Tone: the width of each notch, Hz (default 12). |

```json
{"command":"audio.spectralNoiseFloor","params":{"clip":12,"end":4.7,"high":4000,"low":300,"start":4.2}}
```

<a id="audio.spectralRemoveTone-v1"></a>

### `audio.spectralRemoveTone` v1: Remove Tone

Takes a steady tone and its harmonics out of a clip's sound, or a tone whose pitch moves: mains hum, a whistle, an alarm, a single held note. "hz" is the frequency of a steady tone (or "points" trace a moving one over the clip's source time), "harmonics" how many multiples to remove, "widthHz" how wide each notch is, "start" and "end" the time range it is on for, "gainDb" how far the tone goes down. Cuts only the narrow band of the tone; the sound around it is untouched. Not destructive; one undo step.

Capability `EditSequence`. Tool name `audio_spectralRemoveTone`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `amount` | number (1.0..100.0) |  | Heal and noise floor: how much of the repair replaces the sound, percent (default 100). |
| `clip` | integer (1..) | yes | The audio clip (a video clip stands for its linked audio). |
| `end` | number |  | Rectangle, or the tone's time range: where it ends, in seconds of the clip's source. |
| `gainDb` | number (-96.0..0.0) |  | How far the tone goes down, dB (default -60). |
| `harmonics` | integer (1..16) |  | Remove Tone: the fundamental and this many harmonics are removed (1..16, default 1). |
| `high` | number (1.0..192000.0) |  | Rectangle: its highest frequency, Hz. |
| `hz` | number (1.0..192000.0) |  | Remove Tone: the frequency of a steady tone (instead of "points"). |
| `logFrequency` | boolean |  | A lasso's edges are straight on a logarithmic frequency axis (default) or a linear one. |
| `low` | number (1.0..192000.0) |  | Rectangle: its lowest frequency, Hz. |
| `points` | object[] |  | The lasso's corners, the brush stroke's centre line, or the tone's trace (its frequency over time; one point is a steady tone). |
| `points[].hz` | number (1.0..192000.0) | yes | Frequency. |
| `points[].time` | number | yes | Seconds of the clip's source. |
| `radiusOctaves` | number (0.01..10.0) |  | Brush: half its size in frequency, octaves (default 0.25). |
| `radiusSeconds` | number (0.001..60.0) |  | Brush: half its size in time, seconds (default 0.05). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shape` | `rectangle` \| `lasso` \| `brush` |  | The region's shape. Default rectangle. |
| `start` | number |  | Rectangle, or the tone's time range: where it starts, in seconds of the clip's source. |
| `widthHz` | number (1.0..500.0) |  | Remove Tone: the width of each notch, Hz (default 12). |

```json
{"command":"audio.spectralRemoveTone","params":{"clip":12,"end":30,"harmonics":4,"hz":50,"start":0}}
```

<a id="audio.spectralRepairEdits-v1"></a>

### `audio.spectralRepairEdits` v1: Spectral Repair Edits

Lists the edits of a clip's Spectral Repair in order (kind, shape, time range in seconds of the source, frequency range, level change, points) and its options (FFT size, listen mode, enabled). Changes nothing.

Capability `Read`. Tool name `audio_spectralRepairEdits`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | The audio clip. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.spectralRepairEdits","params":{"clip":12}}
```

<a id="audio.spectralRepairOptions-v1"></a>

### `audio.spectralRepairOptions` v1: Spectral Repair Options

Sets how a clip's Spectral Repair is analysed and heard: "fftSize" (1024, 2048 or 4096: more frequency or more time resolution), "listen" ("off" plays the repaired sound, "selection" only the sound inside the edits, "removed" only what the repair takes away: for judging an edit by ear), and "enabled" (false plays the clip without the repair, for an A/B comparison). One undo step.

Capability `EditSequence`. Tool name `audio_spectralRepairOptions`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | The audio clip. |
| `enabled` | boolean |  | false bypasses the repair. |
| `fftSize` | integer (1024..4096) |  | 1024, 2048 or 4096. |
| `listen` | `off` \| `selection` \| `removed` |  | What is heard. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.spectralRepairOptions","params":{"clip":12,"listen":"removed"}}
```

<a id="audio.spectralRepairRemove-v1"></a>

### `audio.spectralRepairRemove` v1: Remove Spectral Edit

Takes one edit ("index", counted from 0 in audio.spectralRepairEdits) or all of them ("all": true) off a clip's Spectral Repair; the effect goes with the last edit. One undo step.

Capability `EditSequence`. Tool name `audio_spectralRepairRemove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | Remove every edit. |
| `clip` | integer (1..) | yes | The audio clip. |
| `index` | integer (0..) |  | The edit to remove, counted from 0. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audio.spectralRepairRemove","params":{"clip":12,"index":0}}
```

<a id="audio.splitToMono-v1"></a>

### `audio.splitToMono` v1: Split to Mono

Splits a clip's channels into linked mono clips, one per source channel, on separate audio tracks (the clip keeps channel 1; the others go on the free audio tracks below it, new ones when needed, or on "tracks"): "split the two mics into separate tracks". Each keeps the clip's level and effects. One undo step.

Capability `EditSequence`. Tool name `audio_splitToMono`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | The audio clip (or a video clip with linked audio). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (integer \| string)[] |  | The tracks for the other channels. |

```json
{"command":"audio.splitToMono","params":{"clip":12}}
```

<a id="audio.syncClips-v1"></a>

### `audio.syncClips` v1: Synchronize by Audio

Lines recordings of one event up by their audio: a camera's scratch audio and a separate recorder (dual-system sound), or several cameras. Returns each recording's offset to the reference (the one named, else the first clip with a picture), its confidence, the overlap and the clock drift between the devices (ppm, with the speed that would cancel it). "media" entries may be arrays: a camera's recording split into several files, played back to back in the order they were recorded. With clips and "apply" (Premiere's Synchronize), the other clips move to where they line up with the reference, and the external audio (clips without a picture) links to it; one undo step, dryRun previews it.

Capability `EditSequence`. Tool name `audio_syncClips`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `apply` | boolean |  | Move the clips into sync (with "clips"). |
| `clips` | (integer \| object)[], min 2 |  | Clips to sync. |
| `link` | boolean |  | With "apply": link the external audio to the reference (default true). |
| `media` | (integer \| string \| array)[], min 2 |  | Media items to sync (an array: one recording in several files). |
| `minConfidence` | number (0.0..1.0) |  | Least confidence that syncs (0..1, default 0.5). |
| `minOverlap` | number (0.5..3600.0) |  | Least common audio in seconds (default 5). |
| `reference` | integer (1..) |  | The clip or media id the others are synced to. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `stream` | integer (0..) |  | The audio stream of each media item (with "media"; default 0). |

```json
{"command":"audio.syncClips","params":{"apply":true,"clips":[12,18]}}
```

<a id="audioDescription.createTrack-v1"></a>

### `audioDescription.createTrack` v1: Audio Description Track

Puts audio description lines on a caption track "Audio Description" (switched off, so they are not burned into the picture): one cue per line at its gap, editable in the Captions tab, exported as a narrator's script with captions.export, or voiced with dub.create (name "Audio Description", duck about -12 dB so the dialogue and music go down under each line). One undo step.

Capability `EditSequence`. Tool name `audioDescription_createTrack`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `enabled` | boolean |  | Show the lines as subtitles (default false: they are a script to read and voice, not burned in). |
| `language` | string |  | The lines' language (ISO 639-1). |
| `lines` | object[], min 1 | yes | The lines, each in its gap; they may not overlap. A line of "NONE" or no words is left out. |
| `lines[].duration` | integer \| string |  | How long the slot is (instead of "end"). |
| `lines[].end` | integer \| string |  | Where it ends (or give "duration"). |
| `lines[].start` | integer \| string | yes | Where the line starts: a timecode or ticks. |
| `lines[].text` | string | yes | What is said, one phrase. |
| `name` | string |  | The track's name (default "Audio Description"). |
| `replace` | boolean |  | Replace the earlier track of that name (default true); false adds another. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audioDescription.createTrack","params":{"lines":[{"end":"00:00:06:00","start":"00:00:03:00","text":"A woman opens a red door."}]}}
```

<a id="audioDescription.findGaps-v1"></a>

### `audioDescription.findGaps` v1: Find Gaps for Audio Description

The gaps in a sequence's dialogue (from its transcript) long enough for an audio description line: where each is on the timeline, how many words fit at a speaking pace, and the words said just before and after. The first step of Sequence > Generate Audio Description; changes nothing. Needs a transcript (or "silent": true).

Capability `Read`. Tool name `audioDescription_findGaps`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `maxWords` | integer (1..200) |  | Most words in a line whatever the silence (default 40). |
| `minGap` | number (0.5..60.0) |  | The shortest silence worth a line, seconds (default 2), after 0.3 s are kept clear of the speech on each side. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `silent` | boolean |  | The sequence has no speech: the whole sequence is silence (default false). |
| `wordsPerSecond` | number (0.5..6.0) |  | How fast the lines are read, words per second (default 2.5, about 150 a minute). |

```json
{"command":"audioDescription.findGaps","params":{"minGap":2.5}}
```

<a id="audition.addSource-v1"></a>

### `audition.addSource` v1: Add Source to Audition

A media item (its picture and sound streams, in order, for the audition's picture and sound) or a sequence becomes a take after the last one, its "in" at the audition's start (default: the source's start).

Capability `EditSequence`. Tool name `audition_addSource`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The audition. A clip id, or {"track", "time"} for the clip there. |
| `in` | integer \| string \| null |  | Source time at the audition's start. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `media` | integer \| string (1..) |  | The media item to add (or give "source"). A media id or exact name. |
| `name` | string |  | The take's name (default: the source's). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `source` | integer (1..) |  | A sequence to add as a nested take instead of media. |

```json
{"command":"audition.addSource","params":{"clip":42,"in":"00:00:12:00","media":7,"name":"Take 3 (wide)"}}
```

<a id="audition.addTakes-v1"></a>

### `audition.addTakes` v1: Add to Audition

Final Cut's Add to Audition: the clips (with their linked partners) become takes after the audition's last take, lined up with its start at their own In points, and leave the timeline. The active take stays. A clip that is not an audition yet becomes one.

Capability `EditSequence`. Tool name `audition_addTakes`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The audition. A clip id, or {"track", "time"} for the clip there. |
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips to add as takes. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audition.addTakes","params":{"clip":42,"clips":[70]}}
```

<a id="audition.create-v1"></a>

### `audition.create` v1: New Audition

Final Cut's Create Audition: the clips (with their linked partners) become the takes of one audition at the place of the earliest of them (take 1, active); the others follow in timeline order, lined up with its start at their own In points, and leave the timeline (no ripple). Every take must have as many picture and sound clips as the first. One clip alone becomes an audition with a copy of itself, the copy active (Duplicate as Audition). The audition moves, trims, ripples, razors and copies like a clip; playback and export show only its active take.

Capability `EditSequence`. Tool name `audition_create`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips that become the takes. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audition.create","params":{"clips":[42,57,63]}}
```

<a id="audition.describe-v1"></a>

### `audition.describe` v1: Describe Audition

The takes of an audition in take order: name, source (media or sequence), source In at the audition's start, effect count, how far each is short of the audition's range at its head and tail, and which one is active. A clip that is not an audition answers with its one take.

Capability `Read`. Tool name `audition_describe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The audition. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audition.describe","params":{"clip":42}}
```

<a id="audition.duplicateTake-v1"></a>

### `audition.duplicateTake` v1: Duplicate Take

Final Cut's Duplicate as Audition: a copy of a take (default: the active one), named "<name> copy", goes right after it and becomes the active take - to try other effects on it.

Capability `EditSequence`. Tool name `audition_duplicateTake`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The audition (or a clip: it becomes one). A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `take` | integer (1..32) |  | The take to copy (default: the active one). Takes are numbered from 1 in take order. |

```json
{"command":"audition.duplicateTake","params":{"clip":42}}
```

<a id="audition.flatten-v1"></a>

### `audition.flatten` v1: Flatten Audition

Final Cut's Finalize Audition: the auditions among the clips (with their linked picture or sound) keep only their active take and become plain clips.

Capability `EditSequence`. Tool name `audition_flatten`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The auditions. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"audition.flatten","params":{"clips":[42]}}
```

<a id="audition.moveTake-v1"></a>

### `audition.moveTake` v1: Move Take

Moves a take to another place in take order; the active take stays active.

Capability `EditSequence`. Tool name `audition_moveTake`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The audition. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `take` | integer (1..32) | yes | The take to move. Takes are numbered from 1 in take order. |
| `to` | integer (1..32) | yes | Its new place. Takes are numbered from 1 in take order. |

```json
{"command":"audition.moveTake","params":{"clip":42,"take":3,"to":1}}
```

<a id="audition.removeTake-v1"></a>

### `audition.removeTake` v1: Remove Take

Removes a take. The active take passes to the next one (the previous one for the last); an audition left with one take becomes a plain clip of it.

Capability `EditSequence`. Tool name `audition_removeTake`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The audition. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `take` | integer (1..32) | yes | The take to remove. Takes are numbered from 1 in take order. |

```json
{"command":"audition.removeTake","params":{"clip":42,"take":3}}
```

<a id="audition.renameTake-v1"></a>

### `audition.renameTake` v1: Rename Take

Renames a take (of the picture and the sound together; the clip's name for the active take).

Capability `EditSequence`. Tool name `audition_renameTake`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The audition. A clip id, or {"track", "time"} for the clip there. |
| `name` | string | yes | The take's new name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `take` | integer (1..32) | yes | The take. Takes are numbered from 1 in take order. |

```json
{"command":"audition.renameTake","params":{"clip":42,"name":"Angry read","take":2}}
```

<a id="audition.setActive-v1"></a>

### `audition.setActive` v1: Use Take

Makes a take of the audition (and of its linked picture or sound) the active take: the clip keeps its place and links and plays that take over the audition's range, as far as the take has media there (a shorter take leaves the rest empty, and a longer take used later fills it again). The playhead does not move.

Capability `EditSequence`. Tool name `audition_setActive`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The audition. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `take` | integer (1..32) | yes | The take to use. Takes are numbered from 1 in take order. |

```json
{"command":"audition.setActive","params":{"clip":42,"take":2}}
```

<a id="autopilot.broll-v1"></a>

### `autopilot.broll` v1: Auto B-roll

Reads what is said in a sequence (its transcript, line by line) and puts matching B-roll from the library on a free video track: video items the sequence does not use yet, matched by picture (the smart search, when its model is installed) and by words (the item's name, bin, Description, Comment, clip markers and transcript). Each clip lasts as long as its line (2 to 6 s), at most 4 per minute, 4 s apart, never over the first 3 s; the same shot is never used twice. The track is the lowest free video track above the A-roll, else a new "B-roll" track. Returns the suggestions and the lines nothing matched (with a prompt for generating B-roll). With "placements" it places exactly those instead. Needs transcripts. One undo step; dryRun previews it.

Capability `EditSequence`. Tool name `autopilot_broll`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `gapSeconds` | number (0.0..120.0) |  | Seconds between two B-roll clips at least (default 4). |
| `introSeconds` | number (0.0..600.0) |  | No B-roll over the first seconds (default 3). |
| `maxSeconds` | number (0.5..60.0) |  | The longest B-roll clip, in seconds (default 6). |
| `minScore` | number (0.0..1.0) |  | How well a match must fit, 0..1 (default 0.35). |
| `minSeconds` | number (0.5..30.0) |  | The shortest B-roll clip, in seconds (default 2). |
| `perMinute` | number (0.5..30.0) |  | At most this many B-roll clips per minute (default 4). |
| `placements` | object[], min 1 |  | Place exactly these (the preview's chosen suggestions) instead of searching. |
| `placements[].at` | number (0.0..) |  | Where it starts on the timeline, in seconds. |
| `placements[].atTicks` | integer (0..) |  | Or exactly, in ticks. |
| `placements[].duration` | number (0.0..) |  | How long, in seconds. |
| `placements[].durationTicks` | integer (1..) |  | Or exactly, in ticks. |
| `placements[].line` | integer (1..) |  | The line of the transcript it is for (1-based, as the suggestions number them). |
| `placements[].media` | integer \| string (1..) | yes | The B-roll's media item. A media id or exact name. |
| `placements[].sourceIn` | number (0.0..) |  | The first moment of the media to show, in seconds. |
| `placements[].sourceInTicks` | integer (0..) |  | Or exactly, in ticks. |
| `sequence` | integer (1..) |  | The sequence; only needed when the project has several. |
| `visual` | boolean |  | Match by picture too when the visual search model is installed (default true). |

```json
{"command":"autopilot.broll","params":{"perMinute":3}}
```

<a id="autopilot.pacing-v1"></a>

### `autopilot.pacing` v1: Pacing Match

Compares the cut rhythm of a reference video (its shot cuts, found by shot detection and cached per file) with the sequence's (where the picture changes) - cuts per minute, the median and average shot length, the shot length over the running time - and suggests cuts so the sequence's long shots get about as long as the reference's at the same point: each moved to the best nearby place (a sentence end, a pause, a beat of the beat grid; never inside a word). Shots already shorter are left alone: it never removes footage. "apply": "markers" adds a marker at each suggestion, "cuts" adds an edit on all tracks there (one undo step); "only" applies the chosen suggestions (1-based). "referenceCuts" (seconds) with "referenceLength" gives a rhythm instead of a video.

Capability `EditSequence`. Tool name `autopilot_pacing`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `apply` | `none` \| `markers` \| `cuts` |  | What to do with the suggestions: list them (default), add markers, or cut there. |
| `only` | (integer (1..))[] |  | Apply only these suggestions (1-based). |
| `reference` | integer \| string (1..) |  | The reference video (a media item of the project). A media id or exact name. |
| `referenceCuts` | (number (0..))[] |  | Or the reference's cuts, in seconds. |
| `referenceLength` | number (0.1..) |  | With referenceCuts: the reference's length in seconds. |
| `sequence` | integer (1..) |  | The sequence; only needed when the project has several. |
| `strength` | number (0.0..1.0) |  | 1 = the reference's shot lengths (default); 0.5 = halfway between the sequence's and the reference's. |
| `tolerance` | number (1.0..5.0) |  | A shot longer than this times the target gets cuts (default 1.35). |

```json
{"command":"autopilot.pacing","params":{"apply":"markers","referenceCuts":[2.5,5.0,7.0,9.5],"referenceLength":12.0}}
```

<a id="autopilot.publishKit-v1"></a>

### `autopilot.publishKit` v1: Publishing Kit

Prepares a sequence's upload: title ideas (its most telling lines, the hook counting extra), a description (a short lead, the chapters, hashtags), chapters (the sequence's chapter markers, else its transcript's topics; the first at 0:00, with warnings when YouTube would not show them), tags (the most specific words and repeated phrases, at most 15 and 500 characters) and thumbnail candidates (highlight markers, chapter starts, shot middles and telling lines, scored for sharpness, contrast, colour and exposure, 5 s apart). With "folder" it writes them into a "<sequence> - publishing kit" folder there: title.txt, description.txt, chapters.txt, tags.txt, thumbnail-N.jpg (1280 x 720, under 2 MB), publishing-kit.md and publishing-kit.json. Nothing is uploaded and the project does not change. "title", "description" and "tags" replace what it would write. With "cloud" the user's chat model writes the title ideas, description and tags from the transcript text (needs a chat model; never the video).

Capability `Read`. Tool name `autopilot_publishKit`. Runs an AI model (extensions need MediaAnalysis for it too). Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `chapters` | `auto` \| `markers` \| `topics` \| `none` |  | Chapters from the chapter markers, from the transcript's topics, either ("auto", the default: markers when there are any) or none. |
| `cloud` | boolean |  | Let the user's chat model write the title ideas, description and tags from the transcript text (default false). |
| `description` | string |  | The description to write (default: the one made). |
| `folder` | string |  | Write the kit into a new folder inside this one. |
| `sequence` | integer (1..) |  | The sequence; only needed when the project has several. |
| `tags` | string[] |  | The tags to write (default: the ones found). |
| `thumbnails` | integer (0..12) |  | Thumbnail candidates (default 4). |
| `title` | string |  | The title to write (default: the first idea). |

```json
{"command":"autopilot.publishKit","params":{"chapters":"auto","thumbnails":4}}
```

<a id="autozoom.apply-v1"></a>

### `autozoom.apply` v1: Auto Zoom

Zooms a screen recording in where things happen (clicks, typing, where the cursor rests) and back out when nothing does, like Screen Studio: smooth, editable Position and Scale keyframes on the clip (the AI-9 camera optimiser), never showing outside the frame. Activity comes from a cursor sidecar when there is one, else from the frames. A multicam or nested clip is zoomed as one picture from the recording it shows (the active angle), keys on the clip itself. Running it again replaces its keys. One undo step; dryRun with "preview" returns the zoom path.

Capability `EditSequence`. Tool name `autozoom_apply`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clicks` | boolean |  | Zoom on clicks (default true). |
| `clip` | integer \| object (1..) | yes | The screen-recording clip: a clip id, or {"track", "time"} for the clip there. |
| `cursor` | string |  | A cursor sidecar's content: Wabcut JSON ({"format": "wabcut.cursor", ...}) or CSV t,x,y[,event]. |
| `detect` | `auto` \| `sidecar` \| `frames` |  | Where activity comes from: a cursor sidecar if there is one, else the frames (default); only a sidecar; only the frames. |
| `dwell` | boolean |  | Zoom where the cursor comes to rest (default true). |
| `easing` | `easeInOut` \| `linear` \| `smooth` |  | Key interpolation: ease in and out (default), linear, or the smooth optimised path. |
| `fps` | number (2.0..30.0) |  | Frames analysed per second when detecting (default 10). |
| `hold` | number (0.2..20.0) |  | Seconds to stay zoomed after the last click or keystroke there (default 1.5). |
| `idle` | number (0.2..30.0) |  | Activity closer than this (seconds) in one area shares a zoom; longer quiet zooms out (default 2.5). |
| `maxZoom` | number (1.1..4.0) |  | Largest zoom, 1.1..4 (default 2). |
| `preview` | boolean |  | Also return the window over time and the activity, for drawing (default false). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sidecar` | string |  | A cursor sidecar file (only where the host allows reading files). |
| `smoothness` | number (0.0..1.0) |  | Camera smoothness, 0 (follows closely) .. 1 (moves only when it must); default 0.5. |
| `transition` | number (0.1..5.0) |  | Shortest zoom in / out / pan, in seconds (default 0.6). |
| `typing` | boolean |  | Zoom on typing (default true). |

```json
{"command":"autozoom.apply","params":{"clip":42,"easing":"easeInOut","maxZoom":2.0}}
```

<a id="bin.create-v1"></a>

### `bin.create` v1: New Bin

Adds a bin (in "parent", default the project's root). Returns its id.

Capability `EditProject`. Tool name `bin_create`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `name` | string | yes | The bin's name. |
| `parent` | integer (1..) |  | The bin it goes in. A bin id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"bin.create","params":{"name":"Interviews"}}
```

<a id="bin.createSearch-v1"></a>

### `bin.createSearch` v1: Create Search Bin

Premiere's Create Search Bin: a bin named "name" in "parent" (default the root) that lists, live, the bins, sequences and media items of the whole project its "search" finds. It holds nothing itself. Returns its id.

Capability `EditProject`. Tool name `bin_createSearch`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `name` | string | yes | The search bin's name. |
| `parent` | integer (1..) |  | The bin it goes in. A bin id. |
| `search` | object | yes | The query. |
| `search.duplicates` | boolean |  | Only media that shares its file with another item. |
| `search.field` | `` \| `name` \| `comment` \| `description` \| `scene` \| `shot` \| `take` \| `logNote` \| `client` \| `tapeName` \| `path` |  | Where to look: "" (all: name, comment, description, clip metadata, tape name, path) or one property. |
| `search.from` | string |  | File date from (YYYY-MM-DD, inclusive). |
| `search.generated` | boolean |  | Only AI-generated media. |
| `search.good` | boolean |  | Only media marked Good. |
| `search.label` | integer (-1..15) |  | Only this label colour (0..15; -1 = any). |
| `search.offline` | boolean |  | Only offline media. |
| `search.proxy` | boolean |  | Only media with a proxy. |
| `search.text` | string |  | What to find (case-insensitive); empty finds everything. |
| `search.to` | string |  | File date to (YYYY-MM-DD, inclusive). |
| `search.transcribed` | boolean |  | Only transcribed media. |
| `search.type` | `` \| `video` \| `audio` \| `still` \| `title` \| `sequence` \| `subclip` |  | Only this kind of item ("" = any). |
| `search.unused` | boolean |  | Only media no sequence uses. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"bin.createSearch","params":{"name":"Good takes","search":{"good":true}}}
```

<a id="bin.move-v1"></a>

### `bin.move` v1: Move to Bin

Moves bins, media items and sequences into "target" (absent: the project's root). A bin cannot go into itself or one of its sub-bins.

Capability `EditProject`. Tool name `bin_move`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bins` | (integer (1..))[] |  | Bins to move. |
| `media` | (integer (1..))[] |  | Media items to move. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sequences` | (integer (1..))[] |  | Sequences to move. |
| `target` | integer (1..) |  | The bin they go in. A bin id. |

```json
{"command":"bin.move","params":{"media":[3,4],"target":7}}
```

<a id="bin.remove-v1"></a>

### `bin.remove` v1: Delete Bin

Deletes bins with everything in them (sub-bins, media items, sequences). Refused while media inside is used by a sequence that is not deleted with it.

Capability `EditProject`. Tool name `bin_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bins` | (integer (1..))[], min 1 | yes | The bins. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"bin.remove","params":{"bins":[7]}}
```

<a id="bin.rename-v1"></a>

### `bin.rename` v1: Rename Bin

Renames a bin (the name may not be empty).

Capability `EditProject`. Tool name `bin_rename`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) | yes | The bin. A bin id. |
| `name` | string | yes | The new name (not empty). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"bin.rename","params":{"bin":7,"name":"Selects"}}
```

<a id="bin.setNestTarget-v1"></a>

### `bin.setNestTarget` v1: File Nests in Bin

Chooses the bin Nest… files new nested sequences in ("Nested Sequence 01" goes there instead of beside the sequence it was made in), or with "enabled": false stops doing so. One bin at most: choosing one takes it from the other. A search bin holds nothing and is refused. One undo step.

Capability `EditProject`. Tool name `bin_setNestTarget`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) | yes | The bin. A bin id. |
| `enabled` | boolean |  | Turn it on (default) or off for this bin. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"bin.setNestTarget","params":{"bin":7}}
```

<a id="bin.setSearch-v1"></a>

### `bin.setSearch` v1: Edit Search Bin

Replaces the query of a search bin.

Capability `EditProject`. Tool name `bin_setSearch`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) | yes | The search bin. A bin id. |
| `search` | object | yes | The query. |
| `search.duplicates` | boolean |  | Only media that shares its file with another item. |
| `search.field` | `` \| `name` \| `comment` \| `description` \| `scene` \| `shot` \| `take` \| `logNote` \| `client` \| `tapeName` \| `path` |  | Where to look: "" (all: name, comment, description, clip metadata, tape name, path) or one property. |
| `search.from` | string |  | File date from (YYYY-MM-DD, inclusive). |
| `search.generated` | boolean |  | Only AI-generated media. |
| `search.good` | boolean |  | Only media marked Good. |
| `search.label` | integer (-1..15) |  | Only this label colour (0..15; -1 = any). |
| `search.offline` | boolean |  | Only offline media. |
| `search.proxy` | boolean |  | Only media with a proxy. |
| `search.text` | string |  | What to find (case-insensitive); empty finds everything. |
| `search.to` | string |  | File date to (YYYY-MM-DD, inclusive). |
| `search.transcribed` | boolean |  | Only transcribed media. |
| `search.type` | `` \| `video` \| `audio` \| `still` \| `title` \| `sequence` \| `subclip` |  | Only this kind of item ("" = any). |
| `search.unused` | boolean |  | Only media no sequence uses. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"bin.setSearch","params":{"bin":12,"search":{"field":"scene","text":"interview"}}}
```

<a id="brand.get-v1"></a>

### `brand.get` v1: Brand Kit

The project's brand kit (null when it has none).

Capability `Read`. Tool name `brand_get`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"brand.get","params":{}}
```

<a id="brand.set-v1"></a>

### `brand.set` v1: Set Brand Kit

Sets the project's brand kit - named colours, font roles and logos (media of the project) that graphics, templates and caption styles refer to by name - or removes it (null). Everything that refers to it re-themes.

Capability `EditProject`. Tool name `brand_set`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `kit` | object \| null | yes | {"name", "colors": [{"name", "rgba": [r, g, b, a]}], "fonts": [{"role", "family", "style", "weight"}], "logos": [{"name", "media": id or name}]}, or null. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"brand.set","params":{"kit":{"colors":[{"name":"Primary","rgba":[0.9,0.1,0.4,1.0]}],"fonts":[{"family":"Kanit","role":"heading","weight":700}],"name":"Studio"}}}
```

<a id="captions.addCue-v1"></a>

### `captions.addCue` v1: Add Caption

Adds a cue from "start" to "end" (or for "duration"). Refused where it would overlap another cue (unless the track's style allows overlaps). Returns its index.

Capability `EditSequence`. Tool name `captions_addCue`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `duration` | integer \| string |  | Or how long it lasts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `end` | integer \| string |  | Where it ends. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speaker` | integer (1..) |  | Speaker id (transcript.speakers lists them). |
| `start` | integer \| string | yes | Where it starts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `text` | string | yes | The caption. UTF-8; a newline starts a new line. |

```json
{"command":"captions.addCue","params":{"end":"00:00:03:12","start":"00:00:01:00","text":"Hello there."}}
```

<a id="captions.addTrack-v1"></a>

### `captions.addTrack` v1: New Caption Track

Adds an empty caption track, styled by a preset (built in: Clean, Bold Box, Podcast, Shorts, Thai-friendly, Word Pop, Highlighter, Bounce, Typewriter, Zoom, Hype; or one of the project's) and/or style fields. The default look is white text in a dark box at the bottom.

Capability `EditSequence`. Tool name `captions_addTrack`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `language` | string |  | ISO 639-1 language, e.g. "en", "th". |
| `name` | string |  | Track name. Default "Captions". |
| `preset` | string |  | A caption preset (captions.presets lists them). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `style` | object |  | Style fields to change; the others keep their values. |
| `style.alignment` | `left` \| `center` \| `right` |  | Line alignment. |
| `style.allowOverlap` | boolean |  | Cues of the track may overlap (stacked). |
| `style.animation` | `none` \| `pop` \| `fade` \| `wordByWord` \| `wordPop` \| `bounce` \| `highlightBar` \| `typewriter` \| `zoom` |  | Kinetic captions: pop / fade (the whole caption), wordByWord (words appear as spoken), wordPop (each word pops in), bounce (the spoken word hops), highlightBar (a bar slides to the spoken word), typewriter (letters appear one by one), zoom (the spoken word grows). The word looks follow the transcript's word times (else the cue's time shared by word length). |
| `style.animationEase` | `smooth` \| `overshoot` \| `linear` |  | The curve of a word's transition (default overshoot: a little past, then settles). |
| `style.animationSpeed` | number (0.05..1.0) |  | Seconds one word's transition takes (default 0.15); fade takes a third longer. |
| `style.box` | boolean |  | A background box behind the text. |
| `style.boxColor` | string \| array, min 3 |  | Box colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.boxOpacity` | number (0.0..1.0) |  | Box opacity. |
| `style.boxPadding` | number (0.0..2.0) |  | Box padding, fraction of the font size. |
| `style.boxRadius` | number (0.0..2.0) |  | Box corner radius, fraction of the font size. |
| `style.brand` | object |  | C.13: brand kit colours and font role that override the colours and font above while the project's kit has them (brand.set re-themes the captions); fields not given keep their values. |
| `style.brand.box` | string |  | A brand kit colour for the box. |
| `style.brand.fill` | string |  | A brand kit colour for the text ("" = none). |
| `style.brand.font` | string |  | A brand kit font role ("body") for the family and weight. |
| `style.brand.highlight` | string |  | A brand kit colour for the karaoke highlight. |
| `style.brand.outline` | string |  | A brand kit colour for the outline. |
| `style.brand.shadow` | string |  | A brand kit colour for the shadow. |
| `style.emphasis` | `none` \| `keywords` \| `auto` |  | Emphasised words are drawn in emphasisColor and emphasisScale larger: keywords (the words of emphasisWords) or auto (those plus numbers, words in capitals and words followed by "!"). |
| `style.emphasisColor` | string \| array, min 3 |  | Colour of emphasised words. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.emphasisScale` | number (1.0..1.5) |  | How much larger an emphasised word is drawn (1 = not larger). |
| `style.emphasisWords` | string[], min 0 |  | The keywords (at most 64 words of up to 64 bytes). |
| `style.features` | string |  | OpenType features for the shaper, as CSS font-feature-settings: calt, -liga, ss01, salt=2 (at most 16; "" = the font's defaults). Persian and Arabic joining forms and contextual alternates are on already. |
| `style.fill` | string \| array, min 3 |  | Text colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.fontFamily` | string |  | Font family; empty: the platform's sans-serif. |
| `style.fontSize` | number (0.005..0.5) |  | Font size as a fraction of the frame height (0.0444 = 48 px on 1080p). |
| `style.fontWeight` | integer (100..900) |  | 100..900 (400 regular, 700 bold). |
| `style.highlight` | string \| array, min 3 |  | Karaoke: colour of the words already spoken. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.lineSpacing` | number (0.5..3.0) |  | Line spacing (1 = the font's). |
| `style.maxWidth` | number (0.1..1.0) |  | Widest text block, fraction of the frame width. |
| `style.outline` | string \| array, min 3 |  | Outline colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.outlineWidth` | number (0.0..0.5) |  | Outline width, fraction of the font size (0 = none). |
| `style.placement` | `bottom` \| `top` \| `custom` |  | Bottom or top of the safe area, or at positionX/positionY. |
| `style.positionX` | number (0.0..1.0) |  | Custom placement: centre of the text, fraction of the frame width. |
| `style.positionY` | number (0.0..1.0) |  | Custom placement: centre of the text, fraction of the frame height. |
| `style.safeArea` | number (0.0..0.4) |  | Safe-area inset from each frame edge, fraction of the frame. |
| `style.shadowAngle` | number (-360.0..360.0) |  | Shadow direction in degrees, clockwise from the x axis. |
| `style.shadowColor` | string \| array, min 3 |  | Shadow colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.shadowDistance` | number (0.0..2.0) |  | Shadow distance, fraction of the font size. |
| `style.shadowOpacity` | number (0.0..1.0) |  | Shadow opacity (0 = none). |
| `style.shadowSoftness` | number (0.0..2.0) |  | Shadow blur, fraction of the font size. |
| `style.showSpeakers` | boolean |  | Start each cue with its speaker's name in the speaker's colour. |
| `style.stripAccents` | boolean |  | Draw and export the text without accents: marks on Latin, Greek and Cyrillic letters go, and a few letters fold to plain ones (Polish l-stroke, o-slash, eth, thorn); Thai vowel and tone marks stay. |
| `style.stripPunctuation` | boolean |  | Draw and export the text without commas, full stops, quotes and the like (apostrophes and hyphens inside words, and the separators of numbers, stay). |
| `style.textCase` | `asWritten` \| `lower` \| `upper` |  | The case the caption text is drawn and exported in (cue text is not changed). |

```json
{"command":"captions.addTrack","params":{"language":"en","name":"English","preset":"Bold Box"}}
```

<a id="captions.applyPreset-v1"></a>

### `captions.applyPreset` v1: Apply Caption Preset

Gives a caption track the style of a preset: built in (Clean, Bold Box, Podcast, Shorts, Thai-friendly, Word Pop, Highlighter, Bounce, Typewriter, Zoom, Hype) or one of the project's.

Capability `EditSequence`. Tool name `captions_applyPreset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `preset` | string | yes | Preset name (case does not matter). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.applyPreset","params":{"preset":"Thai-friendly"}}
```

<a id="captions.checkSpelling-v1"></a>

### `captions.checkSpelling` v1: Check Spelling in Captions

Premiere's spell check for captions: the misspelled words of a caption track's cues ("captionTrack", default the first) in its language (or "language"), from the host's spell checker and the user's dictionary: the cue, the word's byte range in the cue's text, the word, what the checker advises ("suggest", "replace" with the first suggestion, or "delete" a repeated word) and up to 8 suggestions. "available" is false when the host cannot check that language (nothing is listed). At most 2000 words. Corrections are captions.updateCue or captions.findReplace edits.

Capability `Read`. Tool name `captions_checkSpelling`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `language` | string |  | BCP 47 or ISO 639-1. Default: the track's, else the user's. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `suggestions` | boolean |  | List suggestions. Default true. |

```json
{"command":"captions.checkSpelling","params":{"captionTrack":57}}
```

<a id="captions.export-v1"></a>

### `captions.export` v1: Export Captions

Writes a caption track of the sequence to a caption file (Premiere's Create Sidecar File): "format" srt, vtt, scc, mcc (CEA-608: rewrapped to 32 columns and 4 rows; "display" pop-on, roll-up or paint-on), stl (EBU N19), ttml, dfxp, smpte-tt or itt; default from the extension of "path". Timecode-based formats start at the sequence's start timecode. Words the soundtrack bleeps are replaced as in exports. Returns what the format could not hold exactly. The project does not change; a dry run writes nothing.

Capability `Read`. Tool name `captions_export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id. Default: the first shown one. |
| `display` | `pop-on` \| `roll-up-2` \| `roll-up-3` \| `roll-up-4` \| `paint-on` |  | SCC and MCC: how the 608 captions appear. Default pop-on. |
| `dropFrame` | boolean |  | Drop-frame timecode for 29.97 / 59.94. Default: the sequence's. |
| `format` | `srt` \| `vtt` \| `scc` \| `mcc` \| `stl` \| `ttml` \| `dfxp` \| `smpte-tt` \| `itt` |  | The format. Default: from the extension. |
| `frameRate` | string |  | Frame rate of timecode-based files: "23.976", "24", "25", "29.97", "30", "50", "59.94", "60" or "num/den". Default: the sequence's (SCC and MCC: 29.97). |
| `path` | string | yes | The file to write (UTF-8 path); an existing one is replaced. |
| `range` | `sequence` \| `inOut` |  | The whole sequence (default) or its In/Out marks, times counted from the range's start. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.export","params":{"format":"scc","path":"C:/Delivery/Episode 3.scc"}}
```

<a id="captions.exportPresets-v1"></a>

### `captions.exportPresets` v1: Export Caption Presets

Writes the project's caption presets (or the ones named) to a file (.wabcut-caption-presets.json) that captions.importPresets reads into any other project: how one look is reused across projects. "merge" keeps the presets already in the file (those of the same name are replaced) - how the app's own library of looks grows. Refuses to replace a file unless "overwrite" or "merge". Built-in presets are in every project and are not written. The project does not change; a dry run writes nothing.

Capability `Read`. Tool name `captions_exportPresets`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `merge` | boolean |  | Keep the presets already in the file (default false). |
| `names` | string[], min 1 |  | Only these presets (default: all the project's). |
| `overwrite` | boolean |  | Replace an existing file (default false). |
| `path` | string | yes | The file to write (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.exportPresets","params":{"names":["Channel look"],"path":"C:/Looks/channel.wabcut-caption-presets.json"}}
```

<a id="captions.find-v1"></a>

### `captions.find` v1: Find in Captions

Premiere's Find (the arrows of Find and Replace): every match of "find" in the cues of a caption track ("captionTrack") or of every caption track of the sequence (default), in track order, then cue order, then position: the caption track, cue index, byte range in the cue's text, the cue's start, and the cue text. Plain text; "matchCase" and "wholeWord" as captions.findReplace. At most 1000 matches.

Capability `Read`. Tool name `captions_find`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `find` | string | yes | The text to find (plain text, not a pattern). |
| `matchCase` | boolean |  | Match Case. Default false. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `wholeWord` | boolean |  | Whole Word. Default false. |

```json
{"command":"captions.find","params":{"find":"colour","wholeWord":true}}
```

<a id="captions.findReplace-v1"></a>

### `captions.findReplace` v1: Find and Replace in Captions

Premiere's Find and Replace for captions: replaces every match of "find" in the cues of a caption track ("captionTrack") or of every caption track of the sequence (default), in one undo step - or, with "at" (a match captions.find listed: its caption track, cue and start byte), only that match (Premiere's Replace); the result then names the next match after it ("next", or null). Plain text; "matchCase" and "wholeWord" as Premiere's options (without Match Case, Unicode simple case folding; whole word treats letters, digits and marks of every script as word characters). A dry run lists the matches without changing anything. A replacement that would leave a cue empty is refused.

Capability `EditSequence`. Tool name `captions_findReplace`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | object |  | Replace only this match (captions.find lists them). |
| `at.captionTrack` | integer (1..) | yes | The match's caption track id. |
| `at.cue` | integer (0..100000) | yes | The match's cue. Cue index in start order (captions.get lists them). |
| `at.start` | integer (0..4096) | yes | The match's first byte in the cue's text. |
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `find` | string | yes | The text to find (plain text, not a pattern). |
| `matchCase` | boolean |  | Match Case. Default false. |
| `replace` | string | yes | What each match becomes (may be empty). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `wholeWord` | boolean |  | Whole Word. Default false. |

```json
{"command":"captions.findReplace","params":{"find":"colour","replace":"color","wholeWord":true}}
```

<a id="captions.generate-v1"></a>

### `captions.generate` v1: Generate Captions

Makes a caption track from the transcript: cues of at most maxLines lines of maxCharsPerLine characters and maxDuration seconds, broken at pauses, speaker changes and sentence ends; Thai lines break between words, never at spaces that are not there. Replaces a caption track when "replace" names one.

Capability `EditSequence`. Tool name `captions_generate`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `allowedWords` | string[] |  | Words of the built-in lists to allow. |
| `hideProfanity` | boolean |  | Hide profanity: profane words read as their first letter and asterisks ("f***"). Default false. |
| `maxCharsPerLine` | integer (8..200) |  | Default 42. |
| `maxDuration` | number (1.0..60.0) |  | Longest cue in seconds. Default 7. |
| `maxLines` | integer (1..4) |  | Default 2. |
| `minDuration` | number (0.0..10.0) |  | Shortest cue in seconds. Default 0.7. |
| `name` | string |  | Track name. Default "Captions". |
| `profanityWords` | string[] |  | Words to treat as profane besides the built-in English and Thai lists. |
| `replace` | integer (1..) |  | Caption track id to replace. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.generate","params":{"maxCharsPerLine":37}}
```

<a id="captions.generateFromTranscript-v1"></a>

### `captions.generateFromTranscript` v1: Captions from Transcript

Makes a caption track from the sequence's transcript (with the project's corrections and speakers): cues of at most maxLines lines of maxCharsPerLine characters and maxDuration, broken at pauses, speaker changes and (optionally) sentence ends, at least "gap" apart. Thai and other unspaced scripts break between words, and where the host provides line-break rules also inside them. Each cue gets the speaker of most of its words (diarization labels without a speaker get one). "replace" regenerates a caption track and keeps its style.

Capability `EditSequence`. Tool name `captions_generateFromTranscript`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `allowedWords` | string[] |  | Words of the built-in lists to allow. |
| `breakOnSpeakerChange` | boolean |  | A new speaker starts a new cue. Default true. |
| `gap` | number (0.0..2.0) |  | Least time between two cues in seconds. Default 0. |
| `hideProfanity` | boolean |  | Hide profanity: profane words read as their first letter and asterisks ("f***"). Default false. |
| `language` | string |  | Track language (default: the transcript's). |
| `maxCharsPerLine` | integer (8..200) |  | Default 42. |
| `maxDuration` | number (1.0..60.0) |  | Longest cue in seconds. Default 7. |
| `maxLines` | integer (1..4) |  | Default 2. |
| `maxPause` | number (0.05..30.0) |  | A longer pause between words starts a new cue. Default 1. |
| `minDuration` | number (0.0..10.0) |  | Shortest cue in seconds. Default 0.7. |
| `name` | string |  | Track name. Default "Captions". |
| `preset` | string |  | A caption preset for its style (captions.presets). |
| `profanityWords` | string[] |  | Words to treat as profane besides the built-in English and Thai lists. |
| `replace` | integer (1..) |  | Caption track id to replace. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speakers` | boolean |  | Name speakers on the cues. Default true. |
| `splitOnPunctuation` | boolean |  | A sentence end closes a cue once it is half full. Default true. |

```json
{"command":"captions.generateFromTranscript","params":{"gap":0.08,"maxCharsPerLine":32,"preset":"Podcast"}}
```

<a id="captions.get-v1"></a>

### `captions.get` v1: Caption Cues

The cues of a caption track (default: the sequence's first) as arrays in cueFields order: start, end, text (its lines joined by newlines). A cue over redacted speech (a bleep or mute) reads "[bleep]" as a whole; redactedCues lists their indices. sequence.summary lists the caption tracks.

Capability `Read`. Tool name `captions_get`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id. Default: the first. |
| `from` | integer \| string |  | Only cues that end after this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `limit` | integer (1..100000) |  | Most cues to return (default 500). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Only cues that start before this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"captions.get","params":{"from":"00:00:00:00","to":"00:01:00:00"}}
```

<a id="captions.hideProfanity-v1"></a>

### `captions.hideProfanity` v1: Hide Profanity in Captions

Premiere's Hide profanity on a caption track that exists: every profane word of its cues reads as its first letter and asterisks ("f***"), with the built-in English and Thai lists, "profanityWords" added and "allowedWords" taken out. One undo step.

Capability `EditSequence`. Tool name `captions_hideProfanity`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `allowedWords` | string[] |  | Words of the built-in lists to allow. |
| `captionTrack` | integer (1..) |  | Caption track id. Default: the first. |
| `profanityWords` | string[] |  | Words to treat as profane besides the built-in English and Thai lists. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.hideProfanity","params":{"captionTrack":57}}
```

<a id="captions.import-v1"></a>

### `captions.import` v1: Import Captions

Reads a caption file into a new caption track of the sequence (as Premiere imports a caption file): SubRip (.srt), WebVTT (.vtt), Scenarist SCC and MacCaption MCC (CEA-608), EBU N19 (.stl), W3C TTML / DFXP / SMPTE-TT (.xml, .dfxp) or iTunes Timed Text (.itt); the format comes from the content and the extension unless "format" says. Timecode-based files (SCC, MCC, STL, iTT) count from the sequence's start timecode. Cue text is plain: styles, positions and 608 display modes are approximated (the track takes the placement the cues agree on), as the returned report says. Also Premiere's timed transcript text (.txt: "start - end" timecodes, the speaker, the text; a speaker named there becomes the project's speaker of that name), and the same as a Word document (.docx, transcript.export's). "offset" is added to every cue's time; overlapping cues are trimmed to the next cue's start. Untrusted input: at most 16 MiB and 100,000 cues, 4096 bytes a cue.

Capability `EditSequence`. Tool name `captions_import`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `srt` \| `vtt` \| `scc` \| `mcc` \| `stl` \| `ttml` \| `dfxp` \| `smpte-tt` \| `itt` |  | The file's format. Default: detected. |
| `frameRate` | string |  | Frame rate of timecode-based files: "23.976", "24", "25", "29.97", "30", "50", "59.94", "60" or "num/den". Default: the sequence's (SCC and MCC: 29.97). |
| `language` | string |  | ISO 639-1 language. Default: the file's, else none. |
| `name` | string |  | The new track's name. Default: the file's name. |
| `offset` | integer \| string |  | Added to every cue's time. Default 0. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `path` | string | yes | The caption file (UTF-8 path). |
| `preset` | string |  | A caption preset for its style (captions.presets). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.import","params":{"path":"C:/Delivery/Episode 3.scc"}}
```

<a id="captions.importPresets-v1"></a>

### `captions.importPresets` v1: Import Caption Presets

Reads a caption presets file (captions.exportPresets, the app's Export Presets) into the project: every preset in it, or the ones named, becomes one of the project's own. A name the project already has is skipped unless "replace" (then every caption track that follows it takes the new look); built-in names are never replaced. One undo step.

Capability `EditProject`. Tool name `captions_importPresets`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `names` | string[], min 1 |  | Only these presets (default: all in the file). |
| `path` | string | yes | The caption presets file (UTF-8 path). |
| `replace` | boolean |  | Replace the project's presets of the same names (default false: skipped). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.importPresets","params":{"path":"C:/Looks/channel.wabcut-caption-presets.json"}}
```

<a id="captions.mergeCues-v1"></a>

### `captions.mergeCues` v1: Merge Captions

Joins "count" consecutive cues (default 2) into one, its text re-wrapped to maxLines lines of maxCharsPerLine characters.

Capability `EditSequence`. Tool name `captions_mergeCues`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `count` | integer (2..1000) |  | How many cues (default 2). |
| `cue` | integer (0..100000) | yes | The first cue. Cue index in start order (captions.get lists them). |
| `maxCharsPerLine` | integer (8..200) |  | Default 42. |
| `maxLines` | integer (1..6) |  | Default 2. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.mergeCues","params":{"count":2,"cue":3}}
```

<a id="captions.presets-v1"></a>

### `captions.presets` v1: Caption Presets

The caption presets: built in, then the project's, each with its style.

Capability `Read`. Tool name `captions_presets`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.presets","params":{}}
```

<a id="captions.pushTrackStyle-v1"></a>

### `captions.pushTrackStyle` v1: Push to Track Style

Premiere's Push to Track Style: the caption track's current look becomes its track style (a project preset), and every caption track that follows that style, in every sequence, takes it. Refused for a built-in style (save it under a new name with captions.savePreset).

Capability `EditProject`. Tool name `captions_pushTrackStyle`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.pushTrackStyle","params":{}}
```

<a id="captions.remove-v1"></a>

### `captions.remove` v1: Delete Caption Track

Deletes a caption track of the sequence.

Capability `EditSequence`. Tool name `captions_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) | yes | Caption track id (sequence.summary lists them). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.remove","params":{"captionTrack":57}}
```

<a id="captions.removeCue-v1"></a>

### `captions.removeCue` v1: Delete Caption

Deletes a cue of a caption track.

Capability `EditSequence`. Tool name `captions_removeCue`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `cue` | integer (0..100000) | yes | The cue. Cue index in start order (captions.get lists them). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.removeCue","params":{"cue":3}}
```

<a id="captions.removePreset-v1"></a>

### `captions.removePreset` v1: Delete Caption Preset

Deletes one of the project's caption presets.

Capability `EditProject`. Tool name `captions_removePreset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `name` | string | yes | Preset name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"captions.removePreset","params":{"name":"Channel look"}}
```

<a id="captions.savePreset-v1"></a>

### `captions.savePreset` v1: Save Caption Preset

Saves a named caption style in the project: a caption track's style, with "style" fields applied on top (or the default look). Replaces the project preset of that name (every caption track that follows it as its Track Style takes the new look); built-in names are refused.

Capability `EditProject`. Tool name `captions_savePreset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track whose style to save. |
| `name` | string | yes | Preset name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `style` | object |  | Style fields to change; the others keep their values. |
| `style.alignment` | `left` \| `center` \| `right` |  | Line alignment. |
| `style.allowOverlap` | boolean |  | Cues of the track may overlap (stacked). |
| `style.animation` | `none` \| `pop` \| `fade` \| `wordByWord` \| `wordPop` \| `bounce` \| `highlightBar` \| `typewriter` \| `zoom` |  | Kinetic captions: pop / fade (the whole caption), wordByWord (words appear as spoken), wordPop (each word pops in), bounce (the spoken word hops), highlightBar (a bar slides to the spoken word), typewriter (letters appear one by one), zoom (the spoken word grows). The word looks follow the transcript's word times (else the cue's time shared by word length). |
| `style.animationEase` | `smooth` \| `overshoot` \| `linear` |  | The curve of a word's transition (default overshoot: a little past, then settles). |
| `style.animationSpeed` | number (0.05..1.0) |  | Seconds one word's transition takes (default 0.15); fade takes a third longer. |
| `style.box` | boolean |  | A background box behind the text. |
| `style.boxColor` | string \| array, min 3 |  | Box colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.boxOpacity` | number (0.0..1.0) |  | Box opacity. |
| `style.boxPadding` | number (0.0..2.0) |  | Box padding, fraction of the font size. |
| `style.boxRadius` | number (0.0..2.0) |  | Box corner radius, fraction of the font size. |
| `style.brand` | object |  | C.13: brand kit colours and font role that override the colours and font above while the project's kit has them (brand.set re-themes the captions); fields not given keep their values. |
| `style.brand.box` | string |  | A brand kit colour for the box. |
| `style.brand.fill` | string |  | A brand kit colour for the text ("" = none). |
| `style.brand.font` | string |  | A brand kit font role ("body") for the family and weight. |
| `style.brand.highlight` | string |  | A brand kit colour for the karaoke highlight. |
| `style.brand.outline` | string |  | A brand kit colour for the outline. |
| `style.brand.shadow` | string |  | A brand kit colour for the shadow. |
| `style.emphasis` | `none` \| `keywords` \| `auto` |  | Emphasised words are drawn in emphasisColor and emphasisScale larger: keywords (the words of emphasisWords) or auto (those plus numbers, words in capitals and words followed by "!"). |
| `style.emphasisColor` | string \| array, min 3 |  | Colour of emphasised words. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.emphasisScale` | number (1.0..1.5) |  | How much larger an emphasised word is drawn (1 = not larger). |
| `style.emphasisWords` | string[], min 0 |  | The keywords (at most 64 words of up to 64 bytes). |
| `style.features` | string |  | OpenType features for the shaper, as CSS font-feature-settings: calt, -liga, ss01, salt=2 (at most 16; "" = the font's defaults). Persian and Arabic joining forms and contextual alternates are on already. |
| `style.fill` | string \| array, min 3 |  | Text colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.fontFamily` | string |  | Font family; empty: the platform's sans-serif. |
| `style.fontSize` | number (0.005..0.5) |  | Font size as a fraction of the frame height (0.0444 = 48 px on 1080p). |
| `style.fontWeight` | integer (100..900) |  | 100..900 (400 regular, 700 bold). |
| `style.highlight` | string \| array, min 3 |  | Karaoke: colour of the words already spoken. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.lineSpacing` | number (0.5..3.0) |  | Line spacing (1 = the font's). |
| `style.maxWidth` | number (0.1..1.0) |  | Widest text block, fraction of the frame width. |
| `style.outline` | string \| array, min 3 |  | Outline colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.outlineWidth` | number (0.0..0.5) |  | Outline width, fraction of the font size (0 = none). |
| `style.placement` | `bottom` \| `top` \| `custom` |  | Bottom or top of the safe area, or at positionX/positionY. |
| `style.positionX` | number (0.0..1.0) |  | Custom placement: centre of the text, fraction of the frame width. |
| `style.positionY` | number (0.0..1.0) |  | Custom placement: centre of the text, fraction of the frame height. |
| `style.safeArea` | number (0.0..0.4) |  | Safe-area inset from each frame edge, fraction of the frame. |
| `style.shadowAngle` | number (-360.0..360.0) |  | Shadow direction in degrees, clockwise from the x axis. |
| `style.shadowColor` | string \| array, min 3 |  | Shadow colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.shadowDistance` | number (0.0..2.0) |  | Shadow distance, fraction of the font size. |
| `style.shadowOpacity` | number (0.0..1.0) |  | Shadow opacity (0 = none). |
| `style.shadowSoftness` | number (0.0..2.0) |  | Shadow blur, fraction of the font size. |
| `style.showSpeakers` | boolean |  | Start each cue with its speaker's name in the speaker's colour. |
| `style.stripAccents` | boolean |  | Draw and export the text without accents: marks on Latin, Greek and Cyrillic letters go, and a few letters fold to plain ones (Polish l-stroke, o-slash, eth, thorn); Thai vowel and tone marks stay. |
| `style.stripPunctuation` | boolean |  | Draw and export the text without commas, full stops, quotes and the like (apostrophes and hyphens inside words, and the separators of numbers, stay). |
| `style.textCase` | `asWritten` \| `lower` \| `upper` |  | The case the caption text is drawn and exported in (cue text is not changed). |

```json
{"command":"captions.savePreset","params":{"captionTrack":57,"name":"Channel look"}}
```

<a id="captions.setStyle-v1"></a>

### `captions.setStyle` v1: Caption Style

Changes how a caption track looks: font, size (fraction of the frame height), fill, outline, shadow, background box, placement (bottom, top or a custom position), alignment, width, karaoke highlight, speaker names. Fields not given keep their values. Preview and burn-in export both use it.

Capability `EditSequence`. Tool name `captions_setStyle`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `style` | object | yes | Style fields to change; the others keep their values. |
| `style.alignment` | `left` \| `center` \| `right` |  | Line alignment. |
| `style.allowOverlap` | boolean |  | Cues of the track may overlap (stacked). |
| `style.animation` | `none` \| `pop` \| `fade` \| `wordByWord` \| `wordPop` \| `bounce` \| `highlightBar` \| `typewriter` \| `zoom` |  | Kinetic captions: pop / fade (the whole caption), wordByWord (words appear as spoken), wordPop (each word pops in), bounce (the spoken word hops), highlightBar (a bar slides to the spoken word), typewriter (letters appear one by one), zoom (the spoken word grows). The word looks follow the transcript's word times (else the cue's time shared by word length). |
| `style.animationEase` | `smooth` \| `overshoot` \| `linear` |  | The curve of a word's transition (default overshoot: a little past, then settles). |
| `style.animationSpeed` | number (0.05..1.0) |  | Seconds one word's transition takes (default 0.15); fade takes a third longer. |
| `style.box` | boolean |  | A background box behind the text. |
| `style.boxColor` | string \| array, min 3 |  | Box colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.boxOpacity` | number (0.0..1.0) |  | Box opacity. |
| `style.boxPadding` | number (0.0..2.0) |  | Box padding, fraction of the font size. |
| `style.boxRadius` | number (0.0..2.0) |  | Box corner radius, fraction of the font size. |
| `style.brand` | object |  | C.13: brand kit colours and font role that override the colours and font above while the project's kit has them (brand.set re-themes the captions); fields not given keep their values. |
| `style.brand.box` | string |  | A brand kit colour for the box. |
| `style.brand.fill` | string |  | A brand kit colour for the text ("" = none). |
| `style.brand.font` | string |  | A brand kit font role ("body") for the family and weight. |
| `style.brand.highlight` | string |  | A brand kit colour for the karaoke highlight. |
| `style.brand.outline` | string |  | A brand kit colour for the outline. |
| `style.brand.shadow` | string |  | A brand kit colour for the shadow. |
| `style.emphasis` | `none` \| `keywords` \| `auto` |  | Emphasised words are drawn in emphasisColor and emphasisScale larger: keywords (the words of emphasisWords) or auto (those plus numbers, words in capitals and words followed by "!"). |
| `style.emphasisColor` | string \| array, min 3 |  | Colour of emphasised words. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.emphasisScale` | number (1.0..1.5) |  | How much larger an emphasised word is drawn (1 = not larger). |
| `style.emphasisWords` | string[], min 0 |  | The keywords (at most 64 words of up to 64 bytes). |
| `style.features` | string |  | OpenType features for the shaper, as CSS font-feature-settings: calt, -liga, ss01, salt=2 (at most 16; "" = the font's defaults). Persian and Arabic joining forms and contextual alternates are on already. |
| `style.fill` | string \| array, min 3 |  | Text colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.fontFamily` | string |  | Font family; empty: the platform's sans-serif. |
| `style.fontSize` | number (0.005..0.5) |  | Font size as a fraction of the frame height (0.0444 = 48 px on 1080p). |
| `style.fontWeight` | integer (100..900) |  | 100..900 (400 regular, 700 bold). |
| `style.highlight` | string \| array, min 3 |  | Karaoke: colour of the words already spoken. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.lineSpacing` | number (0.5..3.0) |  | Line spacing (1 = the font's). |
| `style.maxWidth` | number (0.1..1.0) |  | Widest text block, fraction of the frame width. |
| `style.outline` | string \| array, min 3 |  | Outline colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.outlineWidth` | number (0.0..0.5) |  | Outline width, fraction of the font size (0 = none). |
| `style.placement` | `bottom` \| `top` \| `custom` |  | Bottom or top of the safe area, or at positionX/positionY. |
| `style.positionX` | number (0.0..1.0) |  | Custom placement: centre of the text, fraction of the frame width. |
| `style.positionY` | number (0.0..1.0) |  | Custom placement: centre of the text, fraction of the frame height. |
| `style.safeArea` | number (0.0..0.4) |  | Safe-area inset from each frame edge, fraction of the frame. |
| `style.shadowAngle` | number (-360.0..360.0) |  | Shadow direction in degrees, clockwise from the x axis. |
| `style.shadowColor` | string \| array, min 3 |  | Shadow colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `style.shadowDistance` | number (0.0..2.0) |  | Shadow distance, fraction of the font size. |
| `style.shadowOpacity` | number (0.0..1.0) |  | Shadow opacity (0 = none). |
| `style.shadowSoftness` | number (0.0..2.0) |  | Shadow blur, fraction of the font size. |
| `style.showSpeakers` | boolean |  | Start each cue with its speaker's name in the speaker's colour. |
| `style.stripAccents` | boolean |  | Draw and export the text without accents: marks on Latin, Greek and Cyrillic letters go, and a few letters fold to plain ones (Polish l-stroke, o-slash, eth, thorn); Thai vowel and tone marks stay. |
| `style.stripPunctuation` | boolean |  | Draw and export the text without commas, full stops, quotes and the like (apostrophes and hyphens inside words, and the separators of numbers, stay). |
| `style.textCase` | `asWritten` \| `lower` \| `upper` |  | The case the caption text is drawn and exported in (cue text is not changed). |

```json
{"command":"captions.setStyle","params":{"style":{"box":false,"fontSize":0.05,"outlineWidth":0.08}}}
```

<a id="captions.setTrack-v1"></a>

### `captions.setTrack` v1: Caption Track Settings

Renames a caption track, sets its language, shows/hides it, or draws it together with the main caption track (bilingual captions).

Capability `EditSequence`. Tool name `captions_setTrack`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `enabled` | boolean |  | Shown (burned in and exported). |
| `language` | string |  | ISO 639-1 language. |
| `name` | string |  | New name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `showTogether` | boolean |  | Drawn on screen together with the main caption track (the one chosen for the monitor or the export, else the first shown): the monitor and exports show both, stacked in track order (the first topmost), each in its own style. Needs "enabled". |

```json
{"command":"captions.setTrack","params":{"captionTrack":57,"showTogether":true}}
```

<a id="captions.setTrackStyle-v1"></a>

### `captions.setTrackStyle` v1: Track Style

Makes a caption track follow a caption preset (Premiere's Track Style menu): it takes the preset's style, and saving that preset again (captions.savePreset, captions.pushTrackStyle) restyles every track that follows it. "style": "" makes the track follow nothing (it keeps its look).

Capability `EditSequence`. Tool name `captions_setTrackStyle`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `style` | string | yes | Preset name (built in or the project's), or "" for none. |

```json
{"command":"captions.setTrackStyle","params":{"style":"Channel look"}}
```

<a id="captions.shiftCues-v1"></a>

### `captions.shiftCues` v1: Shift Captions

Moves the cues that start in [from, to) (default: all of them) by a signed delta, e.g. to resync captions. Refused when a cue would start before 0 or overlap a cue that stays.

Capability `EditSequence`. Tool name `captions_shiftCues`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `delta` | integer \| string | yes | How far; positive = later. Signed timecode ("-00:00:00:05") or ticks. |
| `from` | integer \| string |  | First cue start that moves. Default: 0. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Cues starting at or after this stay. Default: the end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"captions.shiftCues","params":{"delta":"-00:00:00:12","from":"00:01:00:00"}}
```

<a id="captions.splitCue-v1"></a>

### `captions.splitCue` v1: Split Caption

Cuts a cue in two at a time inside it. The text splits at "textOffset" (a byte offset) or else at the line-break opportunity nearest the same share of the text (Thai and other unspaced scripts break between words where the host provides line-break rules).

Capability `EditSequence`. Tool name `captions_splitCue`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string | yes | Where the second cue starts (inside the cue). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `cue` | integer (0..100000) | yes | The cue. Cue index in start order (captions.get lists them). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `textOffset` | integer (1..4096) |  | Byte offset of the second cue's text. |

```json
{"command":"captions.splitCue","params":{"at":"00:00:12:10","cue":3}}
```

<a id="captions.toGraphics-v1"></a>

### `captions.toGraphics` v1: Convert Captions to Graphics

Turns the cues of a caption track into graphic clips, one per cue at the cue's time, so the text can be animated, restyled and moved like a title (Essential Graphics, graphic.applyPreset, Effect Controls). Each is a text layer in the track's caption style - font, size, colours, outline, shadow, background box, alignment, placement, line spacing - as paragraph text in a box as wide as the caption block and as tall as its lines: the box grows with the text, and a caption at the bottom (top) is pinned to the frame's edge, so editing it to a longer text grows it upwards (downwards) from the safe margin. Lower and upper case are applied; the kinetic animation looks, speaker names, punctuation and accent rules are not (they belong to caption rendering). The graphics go on new video tracks on top (more than one when cues overlap), or on "track" when it is free under every cue. "captions" says what happens to the cues: "hide" switches the caption track off so the text is not drawn twice (default), "keep" leaves it showing, "remove" deletes the track. One undo step.

Capability `EditSequence`. Tool name `captions_toGraphics`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `captions` | `hide` \| `keep` \| `remove` |  | What happens to the caption track. Default hide. |
| `from` | integer \| string |  | Only the cues that start at or after this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Only the cues that start before this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `track` | string \| integer (1..) |  | A free video track for the graphics (default: new tracks on top). "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"captions.toGraphics","params":{"captionTrack":57,"captions":"hide"}}
```

<a id="captions.translate-v1"></a>

### `captions.translate` v1: Translate Captions

Translates a caption track into new caption tracks, one per language, keeping the cue timing: cues that would read too fast in the target language borrow time from the gaps, merge with a neighbour or move words across, and text that does not fit two lines is split into more cues (Thai broken between words). The corrections layer applies first, and bleeped words stay "[bleep]" (never sent as content). A local model (OPUS-MT, on this computer) or the user's cloud model (consent and spend cap; caption text only) with a glossary and a tone. One undo step; "estimate" prices a cloud run first.

Capability `EditSequence`. Tool name `captions_translate`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | The caption track to translate; only needed when the sequence has several. |
| `estimate` | boolean |  | Only price the cloud translation; nothing is sent or changed (default false). |
| `glossary` | object[] |  | Terms and how to translate them (a term mapped to itself stays as it is: names, brands). |
| `glossary[].source` | string | yes |  |
| `glossary[].target` | string | yes |  |
| `languages` | string[], min 1 | yes | Target languages, ISO 639-1 ("th", "ja", "es", "zh-Hant" ...): one new caption track each. |
| `notes` | string |  | Extra instructions for a cloud model (audience, who speaks). |
| `provider` | `auto` \| `local` \| `cloud` |  | Who translates: a local model on this computer, the user's cloud model, or "auto" (local when a model for the pair is installed, else the cloud model unless the project is no-cloud; default). |
| `readingSpeed` | number (2.0..40.0) |  | Characters per second the new captions may ask of a reader (default: the language's, e.g. English 20, Thai 17, Japanese 4). |
| `replace` | boolean |  | Replace an earlier translation of this track into the same language (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sourceLanguage` | string |  | The captions' language when the track does not say (ISO 639-1). |
| `tone` | `neutral` \| `casual` \| `formal` \| `playful` |  | The register of a cloud translation (default neutral). |

```json
{"command":"captions.translate","params":{"languages":["th","ja"],"provider":"auto"}}
```

<a id="captions.updateCue-v1"></a>

### `captions.updateCue` v1: Edit Caption

Changes a cue's text, start, end or speaker (null clears the speaker).

Capability `EditSequence`. Tool name `captions_updateCue`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | Caption track id (sequence.summary lists them). Default: the sequence's first. |
| `cue` | integer (0..100000) | yes | The cue. Cue index in start order (captions.get lists them). |
| `end` | integer \| string |  | New end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speaker` | integer \| null (1..) |  | Speaker id, or null for none. |
| `start` | integer \| string |  | New start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `text` | string |  | New text. UTF-8; a newline starts a new line. |

```json
{"command":"captions.updateCue","params":{"cue":3,"text":"Hello, world."}}
```

<a id="captions.updateFromTranscript-v1"></a>

### `captions.updateFromTranscript` v1: Update Captions from Transcript

Rewrites the text of a caption track's cues from the transcript words under them (after transcript corrections), keeping their timing and style; cues with no words under them stay as they are.

Capability `EditSequence`. Tool name `captions_updateFromTranscript`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `allowedWords` | string[] |  | Words of the built-in lists to allow. |
| `breakOnSpeakerChange` | boolean |  | A new speaker starts a new cue. Default true. |
| `captionTrack` | integer (1..) |  | Caption track id. Default: the first. |
| `gap` | number (0.0..2.0) |  | Least time between two cues in seconds. Default 0. |
| `hideProfanity` | boolean |  | Hide profanity: profane words read as their first letter and asterisks ("f***"). Default false. |
| `maxCharsPerLine` | integer (8..200) |  | Default 42. |
| `maxDuration` | number (1.0..60.0) |  | Longest cue in seconds. Default 7. |
| `maxLines` | integer (1..4) |  | Default 2. |
| `maxPause` | number (0.05..30.0) |  | A longer pause between words starts a new cue. Default 1. |
| `minDuration` | number (0.0..10.0) |  | Shortest cue in seconds. Default 0.7. |
| `profanityWords` | string[] |  | Words to treat as profane besides the built-in English and Thai lists. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `splitOnPunctuation` | boolean |  | A sentence end closes a cue once it is half full. Default true. |

```json
{"command":"captions.updateFromTranscript","params":{"captionTrack":57}}
```

<a id="cdl.export-v1"></a>

### `cdl.export` v1: Export CDL

Writes clips' colour wheels as an ASC CDL: slope = gain - lift, offset = lift + offset, power = gamma, saturation. .cc holds one clip; .ccc and .cdl several (ids are the clip names). Refuses to replace a file unless "overwrite".

Capability `EditProject`. Tool name `cdl_export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The clip. A clip id, or {"track", "time"} for the clip there. |
| `clips` | (integer \| object (1..))[], min 1 |  | Clips to change (or "clip" for one). |
| `effect` | integer (1..) |  | The grade's effect instance id when the clip has several (clip.details lists them). Default: its first grade. |
| `overwrite` | boolean |  | Replace an existing file. Default false. |
| `path` | string | yes | The .cc, .ccc or .cdl file to write. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"cdl.export","params":{"clip":42,"path":"C:/grades/shot42.cc"}}
```

<a id="cdl.import-v1"></a>

### `cdl.import` v1: Import CDL

Sets clips' colour wheels from an ASC CDL file (.cc, .ccc, .cdl): the correction whose id is the clip's name, else the one at the clip's position, or "index". Values beyond the wheels' ranges are clamped (with a warning).

Capability `EditSequence`. Tool name `cdl_import`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The clip. A clip id, or {"track", "time"} for the clip there. |
| `clips` | (integer \| object (1..))[], min 1 |  | Clips to change (or "clip" for one). |
| `index` | integer (0..) |  | Which correction in the file (0 = first). |
| `path` | string | yes | The CDL file. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"cdl.import","params":{"clip":42,"path":"C:/grades/shot42.cc"}}
```

<a id="clip.addEffect-v1"></a>

### `clip.addEffect` v1: Apply Effect

Adds an effect to the stack of each clip (video effects on video clips, audio effects on audio clips). Parameters not given take their defaults. An audio plugin ("plugin": its effect id from plugins.list) goes on audio clips the same way. The new instance ids are returned in createdIds.

Capability `EditSequence`. Tool name `clip_addEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to add the effect to. |
| `effect` | `wabcut.color.basic` \| `wabcut.vignette` \| `wabcut.color.grade` \| `wabcut.blur.gaussian` \| `wabcut.sharpen` \| `wabcut.crop` \| `wabcut.dropShadow` \| `wabcut.title.style` \| `wabcut.mask` \| `wabcut.mosaic` \| `wabcut.key.trackMatte` \| `wabcut.border` \| `wabcut.key.ultra` \| `wabcut.adjust.levels` \| `wabcut.adjust.procAmp` \| `wabcut.adjust.extract` \| `wabcut.adjust.convolutionKernel` \| `wabcut.channel.invert` \| `wabcut.color.brightnessContrast` \| `wabcut.color.tint` \| `wabcut.color.changeColor` \| `wabcut.color.changeToColor` \| `wabcut.color.leaveColor` \| `wabcut.color.balance` \| `wabcut.color.videoLimiter` \| `wabcut.image.blackWhite` \| `wabcut.image.colorPass` \| `wabcut.image.colorReplace` \| `wabcut.image.gammaCorrection` \| `wabcut.key.alphaAdjust` \| `wabcut.key.luma` \| `wabcut.noise.noise` \| `wabcut.stylize.posterize` \| `wabcut.stylize.solarize` \| `wabcut.stylize.strobeLight` \| `wabcut.stylize.threshold` \| `wabcut.stylize.replicate` \| `wabcut.blur.directional` \| `wabcut.blur.camera` \| `wabcut.vr.reorient` \| `wabcut.distort.cornerPin` \| `wabcut.distort.lensDistortion` \| `wabcut.distort.magnify` \| `wabcut.distort.mirror` \| `wabcut.distort.offset` \| `wabcut.distort.spherize` \| `wabcut.distort.twirl` \| `wabcut.distort.waveWarp` \| `wabcut.distort.turbulentDisplace` \| `wabcut.distort.transform` \| `wabcut.transform.horizontalFlip` \| `wabcut.transform.verticalFlip` \| `wabcut.transform.edgeFeather` \| `wabcut.perspective.basic3d` \| `wabcut.perspective.bevelEdges` \| `wabcut.perspective.bevelAlpha` \| `wabcut.perspective.radialShadow` \| `wabcut.blur.unsharpMask` \| `wabcut.retouch.skinSmoothing` \| `wabcut.stylize.alphaGlow` \| `wabcut.stylize.glow` \| `wabcut.stylize.colorEmboss` \| `wabcut.stylize.emboss` \| `wabcut.stylize.findEdges` \| `wabcut.stylize.roughenEdges` \| `wabcut.noise.dustScratches` \| `wabcut.noise.median` \| `wabcut.key.colorKey` \| `wabcut.generate.fourColorGradient` \| `wabcut.generate.checkerboard` \| `wabcut.generate.circle` \| `wabcut.generate.grid` \| `wabcut.generate.lensFlare` \| `wabcut.generate.ramp` \| `wabcut.generate.colorama` \| `wabcut.generate.fractalNoise` \| `wabcut.generate.cellPattern` \| `wabcut.generate.lightning` \| `wabcut.generate.scribble` \| `wabcut.generate.paintBucket` \| `wabcut.transition.blockDissolve` \| `wabcut.transition.gradientWipe` \| `wabcut.transition.linearWipe` \| `wabcut.transition.radialWipe` \| `wabcut.transition.venetianBlinds` \| `wabcut.key.differenceMatte` \| `wabcut.key.imageMatte` \| `wabcut.time.echo` \| `wabcut.time.posterizeTime` \| `wabcut.video.clipName` \| `wabcut.video.simpleText` \| `wabcut.video.timecode` \| `wabcut.audio.gain` \| `wabcut.audio.eq3` \| `wabcut.audio.compressor` \| `wabcut.audio.limiter` \| `wabcut.audio.denoise` \| `wabcut.audio.enhanceSpeech` \| `wabcut.audio.filter` \| `wabcut.audio.dehum` \| `wabcut.audio.deess` \| `wabcut.audio.expander` \| `wabcut.audio.reverb` \| `wabcut.audio.width` \| `wabcut.audio.essentialSound` \| `wabcut.audio.amplify` \| `wabcut.audio.channelMixer` \| `wabcut.audio.dynamics` \| `wabcut.audio.hardLimiter` \| `wabcut.audio.multibandCompressor` \| `wabcut.audio.dynamicsProcessing` \| `wabcut.audio.singleBandCompressor` \| `wabcut.audio.tubeModeledCompressor` \| `wabcut.audio.distortion` \| `wabcut.audio.guitarSuite` \| `wabcut.audio.mastering` \| `wabcut.audio.vocalEnhancer` \| `wabcut.audio.pitchShifter` \| `wabcut.audio.graphicEq10` \| `wabcut.audio.graphicEq20` \| `wabcut.audio.graphicEq30` \| `wabcut.audio.parametricEq` \| `wabcut.audio.simpleParametricEq` \| `wabcut.audio.highpass` \| `wabcut.audio.lowpass` \| `wabcut.audio.bandpass` \| `wabcut.audio.simpleNotch` \| `wabcut.audio.bass` \| `wabcut.audio.treble` \| `wabcut.audio.notchFilter` \| `wabcut.audio.scientificFilter` \| `wabcut.audio.fftFilter` \| `wabcut.audio.stereoExpander` \| `wabcut.audio.balance` \| `wabcut.audio.channelVolume` \| `wabcut.audio.fillLeftWithRight` \| `wabcut.audio.fillRightWithLeft` \| `wabcut.audio.invert` \| `wabcut.audio.swapChannels` \| `wabcut.audio.delay` \| `wabcut.audio.analogDelay` \| `wabcut.audio.multitapDelay` \| `wabcut.audio.chorusFlanger` \| `wabcut.audio.flanger` \| `wabcut.audio.phaser` \| `wabcut.audio.convolutionReverb` \| `wabcut.audio.studioReverb` \| `wabcut.audio.surroundReverb` \| `wabcut.audio.deReverb` \| `wabcut.audio.spectralDenoise` \| `wabcut.audio.adaptiveNoiseReduction` \| `wabcut.audio.automaticClickRemover` \| `wabcut.audio.spectralRepair` \| `wabcut.blur.motion` \| `wabcut.stabilize.warp` \| `wabcut.stabilize.rollingShutter` \| `wabcut.ai.restore` \| `wabcut.ai.denoise` \| `wabcut.ai.refocus` \| `wabcut.ai.faceRetouch` \| `wabcut.ai.eyeContact` |  | The effect id (effect.list). |
| `index` | integer (0..) |  | Position in the stack (0 = first). Default: last. |
| `params` | object |  | Parameter values by key, e.g. {"radius": 12}. |
| `plugin` | string |  | Or an audio plugin's effect id (plugins.list), e.g. "plugin.vst3.<id>". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.addEffect","params":{"clips":[42],"effect":"wabcut.blur.gaussian","params":{"radius":12}}}
```

<a id="clip.addKeyframe-v1"></a>

### `clip.addKeyframe` v1: Add Keyframe

Adds a keyframe at a timeline time inside the clip (or replaces the one there). A parameter that was constant becomes animated with just this key. Keyframes stick to the media: slipping the clip moves them with the picture, trims hide but keep them.

Capability `EditSequence`. Tool name `clip_addKeyframe`. Aliases: `keyframes.add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` |  | Shape after this key. Default linear. |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where the key goes (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `value` | number | yes | The value, in the parameter's unit. |

```json
{"command":"clip.addKeyframe","params":{"clip":42,"param":"opacity.opacity","time":"00:00:04:00","value":0.0}}
```

<a id="clip.details-v1"></a>

### `clip.details` v1: Clip Details

Everything about one clip: track, timeline and source range (ticks and timecode), media, stream, speed, enabled, label colour, linked clips and all parameter values (and effects' text data, e.g. Simple Text's text).

Capability `Read`. Tool name `clip_details`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.details","params":{"clip":42}}
```

<a id="clip.enable-v1"></a>

### `clip.enable` v1: Enable

Enables or disables clips (Premiere Shift+E). Disabled clips stay in place but are not shown or heard.

Capability `EditSequence`. Tool name `clip_enable`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to change. |
| `enabled` | boolean | yes | true to enable, false to disable. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.enable","params":{"clips":[42],"enabled":false}}
```

<a id="clip.fade-v1"></a>

### `clip.fade` v1: Fade

Fades clips in and/or out with keyframes: opacity on video clips (to 0), level on audio clips (to -144 dB). The fade ends (or starts) at the value the parameter has there.

Capability `EditSequence`. Tool name `clip_fade`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to fade. |
| `duration` | integer \| string | yes | Fade length (at most the clip length). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `edge` | `in` \| `out` \| `both` |  | Which end. Default out. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.fade","params":{"clips":[42,43],"duration":"00:00:01:00","edge":"both"}}
```

<a id="clip.find-v1"></a>

### `clip.find` v1: Find Clips

Finds clips by name (case-insensitive substring), by a time they cover, by track and by media. Returns arrays in clipFields order.

Capability `Read`. Tool name `clip_find`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `limit` | integer (1..10000) |  | Most clips to return (default 50). |
| `media` | integer \| string (1..) |  | Only clips of this media. A media id or exact name. |
| `name` | string |  | Part of the clip name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Only clips under this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `tracks` | (string \| integer (1..))[] |  | Only these tracks. |

```json
{"command":"clip.find","params":{"name":"interview","tracks":["V1"]}}
```

<a id="clip.frameHold-v1"></a>

### `clip.frameHold` v1: Frame Hold

Holds the frame at a time. Without duration (Add Frame Hold) the clip is cut there and the rest shows that frame; with a duration (Insert Frame Hold Segment) a still of that length is inserted, rippling the track and sync-locked tracks. Effects keep animating through the hold.

Capability `EditSequence`. Tool name `clip_frameHold`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | A video clip. A clip id, or {"track", "time"} for the clip there. |
| `duration` | integer \| string |  | Insert a hold segment of this length instead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The frame to hold (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"clip.frameHold","params":{"clip":42,"duration":"00:00:02:00","time":"00:00:03:00"}}
```

<a id="clip.frameHoldOptions-v1"></a>

### `clip.frameHoldOptions` v1: Frame Hold Options

Premiere's Clip > Video Options > Frame Hold Options: the whole video clip holds one frame - at a source timecode, a sequence timecode, its In point, its Out point or the playhead ("time": a media time for sourceTimecode, a sequence time for sequenceTimecode and playhead) - keeping its place and length. "holdFilters" holds its keyframed effects at that frame too (off: they keep animating). "enabled": false plays the clip normally again from the frame it held. Add Frame Hold / Insert Frame Hold Segment are clip.frameHold.

Capability `EditSequence`. Tool name `clip_frameHoldOptions`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The video clip. A clip id, or {"track", "time"} for the clip there. |
| `enabled` | boolean |  | Hold (default) or play normally again. |
| `holdFilters` | boolean |  | Hold the clip's effects at the held frame too. |
| `holdOn` | `sourceTimecode` \| `sequenceTimecode` \| `inPoint` \| `outPoint` \| `playhead` |  | Which frame to hold (default inPoint). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | The time for sourceTimecode (a media timecode) or sequenceTimecode / playhead (a sequence timecode); ticks or a timecode. |

```json
{"command":"clip.frameHoldOptions","params":{"clip":42,"holdFilters":true,"holdOn":"playhead","time":"00:00:04:12"}}
```

<a id="clip.group-v1"></a>

### `clip.group` v1: Group

Groups clips (Premiere's Clip > Group, Ctrl+G): clicking any of them selects the whole group, so it moves, copies and deletes together; Alt-click selects one clip of it. Clips leave the groups they were in. Unlike a link, clips on any tracks can be grouped.

Capability `EditSequence`. Tool name `clip_group`. Aliases: `clips.group`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 2 | yes | Clips to group (at least two). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.group","params":{"clips":[42,43,57]}}
```

<a id="clip.insert-v1"></a>

### `clip.insert` v1: Insert

Insert edit (Premiere ','): puts a media source range onto the patched tracks at a time and pushes later material on those tracks and every sync-locked track later, cutting through clips there. Video and audio clips are linked.

Capability `EditSequence`. Tool name `clip_insert`. Aliases: `sequence.insert`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `angle` | integer (1..64) |  | A multicam source's angle for the picture (1 = the first; default the first with a picture). |
| `at` | integer \| string |  | Where the edit starts (the playhead). Ignored when the sequence has In/Out marks: Premiere's three-point editing uses them. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `audio` | (string \| integer (1..))[] |  | Tracks for the media's audio streams in order (default A1, A2, ...); [] for none. |
| `audioAngles` | (integer (1..64))[], min 1 |  | A multicam source's angles for the sound (default the first with sound). |
| `media` | integer \| string (1..) |  | The media to edit in. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sourceIn` | integer \| string |  | Source In: the first source frame to use. Default: the media start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sourceOut` | integer \| string |  | Source Out (exclusive). Default: the media end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sourceSequence` | integer (1..) |  | Or a sequence to edit in as a nested clip (C.8; a multicam clip when it is a multicam source). Its In/Out marks are the default source range. |
| `video` | string \| integer \| null |  | Track for the media's video (default V1); null for none. |

```json
{"command":"clip.insert","params":{"at":"00:00:10:00","media":3,"sourceIn":"00:00:02:00","sourceOut":"00:00:05:00"}}
```

<a id="clip.instantReplay-v1"></a>

### `clip.instantReplay` v1: Instant Replay

Replays a moment right after itself in slow motion, Premiere-style instant replay in one step: the moment's clips are copied and inserted right after it (everything later moves, sync-locked tracks stay in sync) with a speed ramp (100 % easing into "speed" and back), the REPLAY stinger template over the replay and a white flash at each cut. The moment is a video clip ("clip": its span, and its linked clips), or a range ("in"/"out", default the sequence In/Out marks: every clip of media overlapping it). Graphics and titles are not replayed. The replay is silent unless "includeAudio" (slowed, the pitch drops). One undo step.

Capability `EditSequence`. Tool name `clip_instantReplay`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The moment: this video clip's span. A clip id, or {"track", "time"} for the clip there. |
| `flash` | boolean |  | A dip-to-white flash at the cut into the replay and out of it. |
| `flashSeconds` | number (0.04..2.0) |  | Length of each flash. Default 0.2. Seconds. |
| `in` | integer \| string |  | The moment's start (instead of a clip). Default the sequence In mark. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `includeAudio` | boolean |  | Replay the audio too, slowed. Default: the replay is silent. |
| `label` | boolean |  | Lay the REPLAY stinger over the replay. |
| `labelTemplate` | string |  | The stinger template's id (template.list). Default "wabcut.gaming.replay". |
| `labelText` | string |  | The stinger's text, when its template has a "label" field. |
| `out` | integer \| string |  | The moment's end, exclusive. Default the sequence Out mark (its frame included). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `ramp` | boolean |  | Ease from 100 % into the slow speed and back out (time remapping). |
| `rippleMarkers` | boolean |  | Sequence markers after the moment move with the material. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speed` | number (0.05..1.0) |  | Slow-motion speed (0.5 = half speed). Default 0.5. |

```json
{"command":"clip.instantReplay","params":{"clip":42,"speed":0.5}}
```

<a id="clip.link-v1"></a>

### `clip.link` v1: Link

Links clips so they select and edit together.

Capability `EditSequence`. Tool name `clip_link`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 2 | yes | Clips to link (at least two). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.link","params":{"clips":[42,43]}}
```

<a id="clip.lock-v1"></a>

### `clip.lock` v1: Lock Clip

Locks or unlocks clips (0.7), without locking their whole track. A locked clip cannot be moved, trimmed, split, retimed, deleted, overwritten, pushed by a ripple or changed in any other way (an edit that would is refused, naming the clip) until it is unlocked; it can still be selected, copied and played. Exactly the given clips change: pass their linked partners too. Works on clips of locked tracks. Copies of a locked clip are not locked.

Capability `EditSequence`. Tool name `clip_lock`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to lock or unlock. |
| `locked` | boolean | yes | true to lock, false to unlock. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.lock","params":{"clips":[42],"locked":true}}
```

<a id="clip.move-v1"></a>

### `clip.move` v1: Move

Moves clips in time and across tracks. "overwrite" (plain drag) replaces what is at the destination; "insert" (Ctrl-drag) makes room there; "rearrange" (Ctrl+Alt-drag) closes the clips' old place and makes room at the drop, on the dragged clips' and the destination tracks only (a clip dropped at the cut after its neighbour swaps with it). With copy (Alt-drag) the clips are copied there instead and the originals stay (overwrite or insert; not rearrange). Clips cannot start before 0.

Capability `EditSequence`. Tool name `clip_move`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audioTracks` | integer |  | Audio track shift; +1 = one track down (A1 -> A2). Default 0. |
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to move. |
| `copy` | boolean |  | Alt-drag: copy the clips instead of moving them; createdIds lists the copies. Default false. |
| `delta` | integer \| string |  | Time shift; positive = later. Default 0. Signed timecode ("-00:00:00:05") or ticks. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `mode` | `overwrite` \| `insert` \| `rearrange` |  | Default overwrite. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `videoTracks` | integer |  | Video track shift; +1 = one track up (V1 -> V2). Default 0. |

```json
{"command":"clip.move","params":{"clips":[42],"delta":"00:00:02:00","videoTracks":1}}
```

<a id="clip.moveEffect-v1"></a>

### `clip.moveEffect` v1: Reorder Effect

Moves an effect to another position in the clip's stack.

Capability `EditSequence`. Tool name `clip_moveEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) | yes | The effect to move. The effect instance id (clip.details lists them). |
| `index` | integer (0..) | yes | New position (0 = first). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.moveEffect","params":{"clip":42,"effect":77,"index":0}}
```

<a id="clip.moveKeyframe-v1"></a>

### `clip.moveKeyframe` v1: Move Keyframe

Moves a keyframe to another timeline time inside the clip.

Capability `EditSequence`. Tool name `clip_moveKeyframe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `index` | integer (0..) | yes | Keyframe index (0 = first; clip.details lists the keys). |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The new time (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"clip.moveKeyframe","params":{"clip":42,"index":1,"param":"opacity.opacity","time":"00:00:06:00"}}
```

<a id="clip.nearestNeighborScaling-v1"></a>

### `clip.nearestNeighborScaling` v1: Nearest Neighbor Scaling

Clip > Video Options > Nearest Neighbor Scaling: the clip's picture is resized without smoothing, so pixel art and low-resolution game captures keep hard pixel edges when Motion > Scale (or Scale to Frame Size) enlarges them. "enabled": false goes back to smooth scaling. Audio clips in the list (or linked) are skipped. One undo step.

Capability `EditSequence`. Tool name `clip_nearestNeighborScaling`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips (their video clips change). |
| `enabled` | boolean |  | On (default) or off. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.nearestNeighborScaling","params":{"clips":[42]}}
```

<a id="clip.openNested-v1"></a>

### `clip.openNested` v1: Open Nested Sequence

The sequence a nested or multicam clip plays (open it to edit inside), the range of it the clip shows and, for a multicam clip, its active angles.

Capability `Read`. Tool name `clip_openNested`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The nested or multicam clip. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.openNested","params":{"clip":57}}
```

<a id="clip.overwrite-v1"></a>

### `clip.overwrite` v1: Overwrite

Overwrite edit (Premiere '.'): puts a media source range onto the patched tracks at a time, replacing whatever is there. Nothing moves.

Capability `EditSequence`. Tool name `clip_overwrite`. Aliases: `sequence.overwrite`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `angle` | integer (1..64) |  | A multicam source's angle for the picture (1 = the first; default the first with a picture). |
| `at` | integer \| string |  | Where the edit starts (the playhead). Ignored when the sequence has In/Out marks: Premiere's three-point editing uses them. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `audio` | (string \| integer (1..))[] |  | Tracks for the media's audio streams in order (default A1, A2, ...); [] for none. |
| `audioAngles` | (integer (1..64))[], min 1 |  | A multicam source's angles for the sound (default the first with sound). |
| `media` | integer \| string (1..) |  | The media to edit in. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sourceIn` | integer \| string |  | Source In: the first source frame to use. Default: the media start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sourceOut` | integer \| string |  | Source Out (exclusive). Default: the media end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sourceSequence` | integer (1..) |  | Or a sequence to edit in as a nested clip (C.8; a multicam clip when it is a multicam source). Its In/Out marks are the default source range. |
| `video` | string \| integer \| null |  | Track for the media's video (default V1); null for none. |

```json
{"command":"clip.overwrite","params":{"at":"00:00:10:00","audio":[],"media":3,"sourceOut":"00:00:03:00"}}
```

<a id="clip.pasteAttributes-v1"></a>

### `clip.pasteAttributes` v1: Paste Attributes

Premiere's Paste Attributes (Ctrl+Alt+V): the chosen attributes of the clipboard's clip (the first one of each target's kind: video attributes from the copied video clip, audio ones from the audio clip) or of "source" onto "clips". Parameter values and keyframes replace the target's; effects are added after its own; time remapping replaces its speed. Keyframes keep their time from the clip's start, or stretch to the target's length with "scaleTimes". One undo step; "changes" lists what each clip took and skipped (dryRun previews).

Capability `EditSequence`. Tool name `clip_pasteAttributes`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `attributes` | object | yes | Which attributes to paste. |
| `attributes.allEffects` | boolean |  | Every effect of the source clip. |
| `attributes.effects` | (integer (0..))[] |  | Effects to paste, by their place in the source clip's stack. |
| `attributes.motion` | boolean |  | Motion (Transform): position, scale, rotation, anchor; video. |
| `attributes.opacity` | boolean |  | Opacity; video. |
| `attributes.panner` | boolean |  | Panner (Premiere's Channel Volume and Panner); audio. |
| `attributes.timeRemap` | boolean |  | Time Remapping. |
| `attributes.volume` | boolean |  | Volume; audio. |
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips to paste onto. |
| `scaleTimes` | boolean |  | Scale Attribute Times: keyframes stretched to the target's length. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `source` | integer \| object (1..) |  | Take the attributes from this clip of the sequence instead of the clipboard. A clip id, or {"track", "time"} for the clip there. |

```json
{"command":"clip.pasteAttributes","params":{"attributes":{"effects":[0],"motion":true},"clips":[44,45]}}
```

<a id="clip.pinKeyframe-v1"></a>

### `clip.pinKeyframe` v1: Pin Keyframe

Pins a keyframe to the clip's start or end (0.7), or unpins it. A pinned key moves to that end of the clip (keeping its value; a key already there is replaced) and follows it from then on: when an edit moves that end of the clip - a trim, a slip, a roll, a speed change - the key moves with it, so a fade-in keyed at the head of a clip still starts at its head after the head is trimmed. Unpinning leaves the key where it is. A razor cut gives both halves the curve; the key stays with the half whose end it is on.

Capability `EditSequence`. Tool name `clip_pinKeyframe`. Aliases: `keyframes.pin`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `index` | integer (0..) | yes | Keyframe index (0 = first; clip.details lists the keys). |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `pin` | `start` \| `end` \| `none` | yes | start: the clip's first frame; end: its last frame; none: unpin. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.pinKeyframe","params":{"clip":42,"index":0,"param":"opacity.opacity","pin":"start"}}
```

<a id="clip.removeAttributes-v1"></a>

### `clip.removeAttributes` v1: Remove Attributes

Premiere's Remove Attributes: the chosen parameters go back to their defaults (keyframes too), time remapping is turned off (the clip plays at 100 % from the frame it started on), effects are removed by id (or all). One undo step; "changes" lists what each clip lost.

Capability `EditSequence`. Tool name `clip_removeAttributes`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `attributes` | object | yes | Which attributes to remove. |
| `attributes.allEffects` | boolean |  | Every effect in the stack. |
| `attributes.effects` | string[] |  | Effects to remove, by id (every instance of each). |
| `attributes.motion` | boolean |  | Motion (Transform): position, scale, rotation, anchor; video. |
| `attributes.opacity` | boolean |  | Opacity; video. |
| `attributes.panner` | boolean |  | Panner (Premiere's Channel Volume and Panner); audio. |
| `attributes.timeRemap` | boolean |  | Time Remapping. |
| `attributes.volume` | boolean |  | Volume; audio. |
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.removeAttributes","params":{"attributes":{"allEffects":true,"opacity":true},"clips":[44]}}
```

<a id="clip.removeEffect-v1"></a>

### `clip.removeEffect` v1: Remove Effect

Removes an effect instance from a clip's stack.

Capability `EditSequence`. Tool name `clip_removeEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) | yes | The effect to remove. The effect instance id (clip.details lists them). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.removeEffect","params":{"clip":42,"effect":77}}
```

<a id="clip.removeKeyframe-v1"></a>

### `clip.removeKeyframe` v1: Remove Keyframe

Removes a keyframe; removing the last one leaves the parameter constant at its value.

Capability `EditSequence`. Tool name `clip_removeKeyframe`. Aliases: `keyframes.remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `index` | integer (0..) | yes | Keyframe index (0 = first; clip.details lists the keys). |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.removeKeyframe","params":{"clip":42,"index":0,"param":"opacity.opacity"}}
```

<a id="clip.rename-v1"></a>

### `clip.rename` v1: Rename Clip

Renames clips (the name shown on the timeline; the media keeps its name).

Capability `EditSequence`. Tool name `clip_rename`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to rename. |
| `name` | string | yes | The new name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.rename","params":{"clips":[42],"name":"Interview, take 2"}}
```

<a id="clip.replace-v1"></a>

### `clip.replace` v1: Replace With Clip

Premiere's Replace Edit (Sequence > Replace With Clip; Shift-drag from the Source Monitor, Alt-drag from the Project panel onto a clip): the clips (with linked partners) play "media" instead - same place, length, speed and direction, effects and attributes kept; the clip takes the item's name. From Source Monitor / From Bin: the first clip starts with the item's In mark (or "sourceIn"); "matchFrame" (From Source Monitor, Match Frame) lines the Source Monitor's frame up with the sequence playhead instead. Linked partners keep their sync. Refused when the item lacks the media the clip needs.

Capability `EditSequence`. Tool name `clip_replace`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips to replace; the first one sets where the source starts. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `matchFrame` | object |  | From Source Monitor, Match Frame: the source frame shown at the sequence playhead. |
| `matchFrame.at` | integer \| string | yes | The sequence time it lines up with (the playhead). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `matchFrame.sourceTime` | integer \| string \| null | yes | The frame the Source Monitor shows. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `media` | integer \| string (1..) | yes | The item to put in their place. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sourceIn` | integer \| string \| null |  | The source time the first clip starts with. Default: the item's In mark, else its start. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |

```json
{"command":"clip.replace","params":{"clips":[42],"media":7}}
```

<a id="clip.restoreUnrendered-v1"></a>

### `clip.restoreUnrendered` v1: Restore Unrendered

Plays the nested sequence again on clips that Render and Replace (0.7) swapped for a rendered file: the clip keeps its place, In and Out, speed, links, keyframes and effects, and the rendered file stays in the project. Pass the clips (their linked partners come along); clips that were not rendered and replaced are left alone. Refused when none of them was, when the nested sequence is gone or no longer has the picture or sound the clip plays, or when it would contain the sequence it is in. Rendering itself is a menu action (Clip > Render and Replace...): it runs in the export queue and replaces the clips when its file is done.

Capability `EditSequence`. Tool name `clip_restoreUnrendered`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Rendered clips to restore. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.restoreUnrendered","params":{"clips":[42]}}
```

<a id="clip.rippleTrim-v1"></a>

### `clip.rippleTrim` v1: Ripple Edit

Ripple edit (B): moves a clip's In or Out point and ripples later material (sync-locked tracks too).

Capability `EditSequence`. Tool name `clip_rippleTrim`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip whose edge moves. A clip id, or {"track", "time"} for the clip there. |
| `delta` | integer \| string | yes | How far the edge moves; positive = later. Signed timecode ("-00:00:00:05") or ticks. |
| `edge` | `head` \| `tail` | yes | Which edge: "head" (the clip's In point) or "tail" (its Out point). |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.rippleTrim","params":{"clip":42,"delta":"00:00:00:12","edge":"head"}}
```

<a id="clip.roll-v1"></a>

### `clip.roll` v1: Rolling Edit

Rolling edit (N): moves the edit point at a clip's edge, trimming the neighbour the other way.

Capability `EditSequence`. Tool name `clip_roll`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip whose edge moves. A clip id, or {"track", "time"} for the clip there. |
| `delta` | integer \| string | yes | How far the edge moves; positive = later. Signed timecode ("-00:00:00:05") or ticks. |
| `edge` | `head` \| `tail` | yes | Which edge: "head" (the clip's In point) or "tail" (its Out point). |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.roll","params":{"clip":42,"delta":"00:00:00:06","edge":"tail"}}
```

<a id="clip.scaleToFrameSize-v1"></a>

### `clip.scaleToFrameSize` v1: Auto Fit to Frame

Premiere's Clip > Video Options > Scale to Frame Size (also the clip's right-click menu; Wabcut names it Auto Fit to Frame, since the fit is automatic and Motion > Scale is not touched): fits each video clip's picture into the sequence frame (whole picture visible, letterboxed when the aspect differs) before its Motion, so Motion > Scale stays 100 % and a later Scale applies to the fitted picture. "enabled": false turns it off (the picture shows at its pixel size again). Audio clips in the list (or linked) are skipped.

Capability `EditSequence`. Tool name `clip_scaleToFrameSize`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips (their video clips are scaled). |
| `enabled` | boolean |  | On (default) or off. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.scaleToFrameSize","params":{"clips":[42]}}
```

<a id="clip.setAnimated-v1"></a>

### `clip.setAnimated` v1: Toggle Animation

Premiere's stopwatch. On: the constant value becomes one keyframe at "time" (default: the clip's start). Off: all keyframes are deleted and the parameter keeps the value it has at "time".

Capability `EditSequence`. Tool name `clip_setAnimated`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `animated` | boolean | yes | true to animate, false to make constant. |
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | The reference time (inside the clip). Default: the clip start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"clip.setAnimated","params":{"animated":true,"clip":42,"param":"motion.scale"}}
```

<a id="clip.setBlendMode-v1"></a>

### `clip.setBlendMode` v1: Set Blend Mode

Sets the blend mode of video clips (the Opacity section's Blend Mode, Premiere's list): how each clip combines with what is below it. "normal" puts it over; the darken, lighten, contrast, inversion and component groups follow Premiere's menu. Opacity (keyframable) still applies on top. Audio clips are refused.

Capability `EditSequence`. Tool name `clip_setBlendMode`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The video clips. |
| `mode` | `normal` \| `dissolve` \| `darken` \| `multiply` \| `colorBurn` \| `linearBurn` \| `darkerColor` \| `lighten` \| `screen` \| `colorDodge` \| `add` \| `lighterColor` \| `overlay` \| `softLight` \| `hardLight` \| `vividLight` \| `linearLight` \| `pinLight` \| `hardMix` \| `difference` \| `exclusion` \| `subtract` \| `divide` \| `hue` \| `saturation` \| `color` \| `luminosity` | yes | The blend mode (Premiere's menu order). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setBlendMode","params":{"clips":[42],"mode":"screen"}}
```

<a id="clip.setColor-v1"></a>

### `clip.setColor` v1: Set Label Color

Sets the label colour of clips (0-15).

Capability `EditSequence`. Tool name `clip_setColor`. Aliases: `clip.label`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to change. |
| `colorIndex` | integer (0..15) | yes | Label colour index. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setColor","params":{"clips":[42],"colorIndex":3}}
```

<a id="clip.setEffect-v1"></a>

### `clip.setEffect` v1: Set Effect

Turns an effect on or off or sets its parameters (constants; animate them with clip.addKeyframe and "effect") or its text data ("data": Simple Text's {"text": "..."}, Image Matte Key's {"image": "media:<id>"} naming a still image in the project; an empty string removes it).

Capability `EditSequence`. Tool name `clip_setEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `data` | object |  | Text data by key (strings, at most 1024 bytes each). |
| `effect` | integer (1..) | yes | The effect. The effect instance id (clip.details lists them). |
| `enabled` | boolean |  | On or off. |
| `params` | object |  | Parameter values by key. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setEffect","params":{"clip":42,"effect":77,"params":{"saturation":0}}}
```

<a id="clip.setFade-v1"></a>

### `clip.setFade` v1: Set Fade

Sets a clip's fade in (edge "head") or fade out ("tail"), what dragging the fade handle at a clip's top corner does: a one-sided transition inside the clip, Cross Dissolve on video (from or to black) and Constant Power on audio (from or to silence). The duration is clamped to the clip; 0 removes the fade. An edge that already has a transition with the neighbouring clip is refused. (clip.fade keyframes opacity or volume instead.)

Capability `EditSequence`. Tool name `clip_setFade`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `duration` | integer \| string | yes | The fade's length (0 removes it). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `edge` | `head` \| `tail` | yes | "head": the fade in at the clip's In point; "tail": the fade out at its Out point. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setFade","params":{"clip":42,"duration":"00:00:01:00","edge":"tail"}}
```

<a id="clip.setFieldOptions-v1"></a>

### `clip.setFieldOptions` v1: Field Options

Premiere's Clip > Video Options > Field Options for interlaced footage. "reverseFieldDominance" swaps the clip's field order; "processing" is "none" (default), "interlaceConsecutiveFrames" (kept for interchange: Wabcut renders progressive frames), "alwaysDeinterlace" (show one field, the dominant one, even in an interlaced sequence or on footage without fields) or "flickerRemoval" (blend the fields slightly against flicker on thin horizontal lines). Interlaced footage in a progressive sequence is deinterlaced anyway, both fields shown when the sequence runs at the field rate. Audio clips in the list (or linked) are skipped. One undo step.

Capability `EditSequence`. Tool name `clip_setFieldOptions`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips (their video clips change). |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `processing` | `none` \| `interlaceConsecutiveFrames` \| `alwaysDeinterlace` \| `flickerRemoval` |  | Processing Options. |
| `reverseFieldDominance` | boolean |  | Swap the clip's field order. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setFieldOptions","params":{"clips":[42],"processing":"alwaysDeinterlace"}}
```

<a id="clip.setKeyframe-v1"></a>

### `clip.setKeyframe` v1: Set Keyframe

Changes a keyframe's value, interpolation (hold, linear, bezier, easeIn, easeOut, easeInOut) or Bezier handles (slope in units per second, influence 0..1 of the segment).

Capability `EditSequence`. Tool name `clip_setKeyframe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `inInfluence` | number (0.0..1.0) |  | Bezier: arriving influence. |
| `inSlope` | number |  | Bezier: arriving slope. |
| `index` | integer (0..) | yes | Keyframe index (0 = first; clip.details lists the keys). |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` |  | New interpolation. |
| `outInfluence` | number (0.0..1.0) |  | Bezier: leaving influence. |
| `outSlope` | number |  | Bezier: leaving slope. |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `value` | number |  | New value. |

```json
{"command":"clip.setKeyframe","params":{"clip":42,"index":0,"interpolation":"easeInOut","param":"volume.level"}}
```

<a id="clip.setMotion-v1"></a>

### `clip.setMotion` v1: Motion

Sets a video clip's Motion - Position, Scale, Rotation, Anchor Point - in one step, as dragging the clip in the Program Monitor does (move, scale by a handle, rotate outside a corner, move the anchor point). Each value given is in the model's unit: position and anchor in pixels from the centre (sequence pixels for position, source pixels for the anchor), scale as a factor (1 = 100%), rotation in degrees clockwise. A parameter whose stopwatch is on gets a keyframe at "time" (the key there changes, else a new one continues the curve's shape); a constant one takes the value. Parameters left out keep their values. Scale Width and the Uniform Scale switch are Premiere's: with uniformScale off, "scale" is the Scale Height and "scaleWidth" the width; giving a scaleWidth turns Uniform Scale off first, and switching it carries the width along (off: the width starts at the Scale; on: the width goes back to 100%), so the picture does not jump.

Capability `EditSequence`. Tool name `clip_setMotion`. Aliases: `motion.set`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `anchorX` | number |  | Anchor Point X: offset from the clip frame's centre, in source pixels. |
| `anchorY` | number |  | Anchor Point Y: offset from the clip frame's centre, in source pixels. |
| `clip` | integer \| object (1..) | yes | The video clip. A clip id, or {"track", "time"} for the clip there. |
| `positionX` | number |  | Position X: the anchor's offset from the frame's centre, in sequence pixels (right is positive). |
| `positionY` | number |  | Position Y: the anchor's offset from the frame's centre, in sequence pixels (down is positive). |
| `rotation` | number |  | Rotation in degrees, clockwise. |
| `scale` | number (0.0..100.0) |  | Scale as a factor (1 = 100%); the Scale Height while Uniform Scale is off. |
| `scaleWidth` | number (0.0..100.0) |  | Scale Width as a factor (1 = 100%); turns Uniform Scale off. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Where animated parameters are keyed (inside the clip). Required when one of the parameters given is animated; default the clip's start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `uniformScale` | boolean |  | Premiere's Uniform Scale switch (on by default). Not keyframable. |

```json
{"command":"clip.setMotion","params":{"clip":42,"positionX":320.0,"positionY":-180.0,"scale":0.5,"time":"00:00:02:00"}}
```

<a id="clip.setNote-v1"></a>

### `clip.setNote` v1: Set Clip Note

Writes a free-text note on clips (0.7): up to 2,000 bytes of text kept with the clip in the project, read on the timeline when Timeline Display's "Clip notes" is on and in the clip's tooltip. An empty note clears it. A razor cut gives both pieces the note and a copy carries it; other clips of the same media do not have it.

Capability `EditSequence`. Tool name `clip_setNote`. Aliases: `clip.note`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to write the note on. |
| `note` | string | yes | The note (empty clears it). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setNote","params":{"clips":[42],"note":"Cut the cough here"}}
```

<a id="clip.setParam-v1"></a>

### `clip.setParam` v1: Set Clip Parameter

Sets one effect parameter (motion, opacity, volume, pan) on clips.

Capability `EditSequence`. Tool name `clip_setParam`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to change. |
| `param` | `motion.position.x` \| `motion.position.y` \| `motion.scale` \| `motion.rotation` \| `motion.anchor.x` \| `motion.anchor.y` \| `opacity.opacity` \| `volume.level` \| `panner.balance` \| `motion.scaleWidth` \| `motion.uniformScale` | yes | The parameter: motion.position.x [-10000000.0..10000000.0, default 0.0], motion.position.y [-10000000.0..10000000.0, default 0.0], motion.scale [0.0..100.0, default 1.0], motion.rotation [-1000000.0..1000000.0, default 0.0], motion.anchor.x [-10000000.0..10000000.0, default 0.0], motion.anchor.y [-10000000.0..10000000.0, default 0.0], opacity.opacity [0.0..1.0, default 1.0], volume.level [-144.0..24.0, default 0.0], panner.balance [-1.0..1.0, default 0.0], motion.scaleWidth [0.0..100.0, default 1.0], motion.uniformScale [0.0..1.0, default 1.0]. Position/anchor in pixels from the centre, scale as a factor (1 = 100%), rotation in degrees, opacity 0..1, volume in dB, pan -1 (left)..1 (right). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `value` | number | yes | The new value, in the parameter's unit. |

```json
{"command":"clip.setParam","params":{"clips":[42],"param":"volume.level","value":-6.0}}
```

<a id="clip.setTimeInterpolation-v1"></a>

### `clip.setTimeInterpolation` v1: Time Interpolation

Clip > Video Options > Time Interpolation: how retimed video clips make the frames between their media's frames (a speed other than 100 %, reverse, time remapping, Rate Stretch, or footage at another frame rate than the sequence's). "frameSampling" (the default) repeats or drops whole frames; "frameBlending" mixes the two frames around each moment; "opticalFlow" warps both along their motion and blends them (smoothest; the render bar shows those sections red until rendered); "aiInterpolation" (0.7) has a neural model make the frames (effect.processAi makes the pictures ahead of time; where they are missing the clip plays with Optical Flow and an export makes them). Linked audio partners are left alone.

Capability `EditSequence`. Tool name `clip_setTimeInterpolation`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The video clips. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `method` | `frameSampling` \| `frameBlending` \| `opticalFlow` \| `aiInterpolation` | yes | Frame Sampling, Frame Blending, Optical Flow or AI Frame Interpolation. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setTimeInterpolation","params":{"clips":[42],"method":"opticalFlow"}}
```

<a id="clip.setToFillFrame-v1"></a>

### `clip.setToFillFrame` v1: Set Scale to Fill Frame

Premiere's Fill Frame (the clip's right-click menu; Wabcut names it Set Scale to Fill Frame): sets each video clip's Motion > Scale so its picture covers the whole sequence frame, cropping what overhangs (a 16:9 clip in a 9:16 Short fills the height). Keys on Scale are replaced by the constant; Auto Fit to Frame (clip.scaleToFrameSize) goes off. data.scale: the first clip's new Scale in percent.

Capability `EditSequence`. Tool name `clip_setToFillFrame`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips (their video clips are scaled). |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setToFillFrame","params":{"clips":[42]}}
```

<a id="clip.setToFrameSize-v1"></a>

### `clip.setToFrameSize` v1: Set Scale to Fit Frame

Premiere's Set to Frame Size (Fit to Frame Size in Premiere 25 and later; Clip > Video Options and the right-click menu; Wabcut names it Set Scale to Fit Frame): sets each video clip's Motion > Scale so its whole picture fits the sequence frame, so the Effect Controls show how much it was scaled. Keys on Scale are replaced by the constant; Auto Fit to Frame (clip.scaleToFrameSize) goes off. data.scale: the first clip's new Scale in percent.

Capability `EditSequence`. Tool name `clip_setToFrameSize`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips (their video clips are scaled). |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.setToFrameSize","params":{"clips":[42]}}
```

<a id="clip.shake-v1"></a>

### `clip.shake` v1: Camera Shake

A camera shake on a video clip from "at": Motion Position (and Rotation) keys of a seeded random pattern that decays over "seconds", plus a slight zoom so the clip's edges do not show ("cover"). The same request makes the same shake ("seed" picks another). Keys already inside the shake are replaced. One undo step.

Capability `EditSequence`. Tool name `clip_shake`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string | yes | When the shake starts (the impact). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `clip` | integer \| object (1..) | yes | The video clip. A clip id, or {"track", "time"} for the clip there. |
| `cover` | boolean |  | Zoom in slightly so the edges stay hidden. |
| `frequency` | number (1.0..60.0) |  | Direction changes per second. Default 14. |
| `intensity` | number (0.5..1000.0) |  | Largest offset in sequence pixels. Default 24. |
| `rotation` | number (0.0..45.0) |  | Largest rotation in degrees (0 = none). Default 0. |
| `seconds` | number (0.02..30.0) |  | How long it shakes. Default 0.5. Seconds. |
| `seed` | integer (0..) |  | The pattern's seed (0 = from the clip and the time). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.shake","params":{"at":"00:00:12:10","clip":42,"intensity":30}}
```

<a id="clip.slide-v1"></a>

### `clip.slide` v1: Slide

Slide (U): moves a clip between its neighbours, trimming them to compensate.

Capability `EditSequence`. Tool name `clip_slide`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip to slide. A clip id, or {"track", "time"} for the clip there. |
| `delta` | integer \| string | yes | How far it moves; positive = later. Signed timecode ("-00:00:00:05") or ticks. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.slide","params":{"clip":42,"delta":"-00:00:00:08"}}
```

<a id="clip.slip-v1"></a>

### `clip.slip` v1: Slip

Slip (Y): shifts which source frames clips show, keeping their place and length.

Capability `EditSequence`. Tool name `clip_slip`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to slip. |
| `delta` | integer \| string | yes | Source shift; positive = later source frames. Signed timecode ("-00:00:00:05") or ticks. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.slip","params":{"clips":[42],"delta":"00:00:01:00"}}
```

<a id="clip.speedDuration-v1"></a>

### `clip.speedDuration` v1: Speed/Duration

Ctrl+R: plays each clip's material at a new speed (percent; 200 = twice as fast) or fits it into a new duration; can reverse it and keep audio pitch. With ripple, later material on the track and sync-locked tracks moves; without, the clip only grows into the gap after it.

Capability `EditSequence`. Tool name `clip_speedDuration`. Aliases: `clip.speed`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to retime. |
| `duration` | integer \| string |  | New length instead of a speed. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `keepDuration` | boolean |  | Speed and duration unlinked: keep each clip's length and play more or less of its media (the source Out moves). Default false. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `maintainPitch` | boolean |  | Audio: keep the pitch (time-stretch) instead of varispeed. Default: on for a clip leaving 100 %, else unchanged. |
| `reverse` | boolean |  | Play backwards. |
| `ripple` | boolean |  | Shift trailing clips. Default false. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speed` | number (0.001..100000.0) |  | Speed in percent (100 = normal). |
| `timeInterpolation` | `frameSampling` \| `frameBlending` \| `opticalFlow` \| `aiInterpolation` |  | The dialog's Time Interpolation (P.2, as clip.setTimeInterpolation): Frame Sampling, Frame Blending or Optical Flow for the video clips. Default: unchanged. |

```json
{"command":"clip.speedDuration","params":{"clips":[42],"ripple":true,"speed":50}}
```

<a id="clip.splitAtShots-v1"></a>

### `clip.splitAtShots` v1: Split at Shots

Razors each clip at the cuts in its video (scene-edit detection), with its linked partners. One undo step; dryRun previews the split.

Capability `EditSequence`. Tool name `clip_splitAtShots`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to split. |
| `detector` | `combined` \| `content` \| `adaptive` \| `threshold` |  | Detector: combined (hard cuts, fades, dissolves; flashes ignored; default), content, adaptive or threshold (fades through black). |
| `linked` | boolean |  | Split linked partners too. Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `threshold` | number (1.0..255.0) |  | Content threshold (default 27; lower finds more cuts). |

```json
{"command":"clip.splitAtShots","params":{"clips":[42]}}
```

<a id="clip.trim-v1"></a>

### `clip.trim` v1: Trim

Trim an edge (selection tool): makes a clip longer or shorter - extend it into a gap or shorten it - by moving its In or Out point by a delta, without moving anything else. Clamped to neighbours, source media and one frame; the delta actually applied is returned.

Capability `EditSequence`. Tool name `clip_trim`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip whose edge moves. A clip id, or {"track", "time"} for the clip there. |
| `delta` | integer \| string | yes | How far the edge moves; positive = later. Signed timecode ("-00:00:00:05") or ticks. |
| `edge` | `head` \| `tail` | yes | Which edge: "head" (the clip's In point) or "tail" (its Out point). |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.trim","params":{"clip":42,"delta":"-00:00:00:10","edge":"tail"}}
```

<a id="clip.ungroup-v1"></a>

### `clip.ungroup` v1: Ungroup

Ungroups clips (Premiere's Clip > Ungroup, Ctrl+Shift+G): every group one of the clips belongs to is dissolved, all its members included. Clips that are not grouped are left alone.

Capability `EditSequence`. Tool name `clip_ungroup`. Aliases: `clips.ungroup`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips whose groups to dissolve. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.ungroup","params":{"clips":[42]}}
```

<a id="clip.unlink-v1"></a>

### `clip.unlink` v1: Unlink

Takes clips out of their link groups.

Capability `EditSequence`. Tool name `clip_unlink`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to unlink. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.unlink","params":{"clips":[42]}}
```

<a id="clip.zoomPunch-v1"></a>

### `clip.zoomPunch` v1: Zoom Punch

A punch-in zoom on a moment of a video clip: Motion Scale keys ease in to "scale" times the clip's scale, hitting at "at", hold, and ease back out ("attack" and "release" are shares of "seconds"). With "focus" the zoom closes in on that point of the frame (Position keys too). Keys already inside the punch are replaced. One undo step; the keys stay editable.

Capability `EditSequence`. Tool name `clip_zoomPunch`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string | yes | When the punch hits (full zoom). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `attack` | number (0.05..0.9) |  | Share of the punch easing in. Default 0.2. |
| `clip` | integer \| object (1..) | yes | The video clip. A clip id, or {"track", "time"} for the clip there. |
| `easing` | `linear` \| `easeIn` \| `easeOut` \| `easeInOut` |  | The keys' easing. Default easeInOut. |
| `focus` | object |  | The point the zoom closes in on. Default: the clip's anchor. |
| `focus.x` | number (0.0..1.0) | yes | Across, a fraction of the frame width. |
| `focus.y` | number (0.0..1.0) | yes | Down, a fraction of the frame height. |
| `release` | number (0.0..0.9) |  | Share easing back out (0 = cut back). Default 0.4. |
| `scale` | number (1.01..10.0) |  | Zoom at the peak, a factor of the clip's scale. Default 1.3. |
| `seconds` | number (0.02..30.0) |  | The whole punch. Default 0.5. Seconds. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clip.zoomPunch","params":{"at":"00:00:12:10","clip":42,"scale":1.4}}
```

<a id="clipboard.copy-v1"></a>

### `clipboard.copy` v1: Copy

Copies clips (with their link partners unless "linked": false) to the clipboard exactly: speed, reverse, freeze frames, time remapping, effects and keyframes, names, labels, links, the transitions between them and the markers over them (captions on request). The project does not change.

Capability `Read`. Tool name `clipboard_copy`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captions` | boolean |  | Copy the caption cues inside the copied range (default false). |
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips to copy. |
| `export` | boolean |  | Also return the clipboard as JSON text ("document"), to paste it into another project or Wabcut (default false). |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `markers` | boolean |  | Copy the sequence markers inside the copied range (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clipboard.copy","params":{"clips":[42,43]}}
```

<a id="clipboard.cut-v1"></a>

### `clipboard.cut` v1: Cut

Copies clips to the clipboard (like clipboard.copy) and deletes them, leaving gaps (Premiere's Cut). One undo step.

Capability `EditSequence`. Tool name `clipboard_cut`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captions` | boolean |  | Copy the caption cues inside the copied range (default false). |
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips to copy. |
| `export` | boolean |  | Also return the clipboard as JSON text ("document"), to paste it into another project or Wabcut (default false). |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `markers` | boolean |  | Copy the sequence markers inside the copied range (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clipboard.cut","params":{"clips":[42]}}
```

<a id="clipboard.get-v1"></a>

### `clipboard.get` v1: Clipboard

What the clipboard holds: clips, length, tracks, markers, captions, media; with "export" also the clipboard as JSON text.

Capability `Read`. Tool name `clipboard_get`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `export` | boolean |  | Also return the clipboard as JSON text (default false). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"clipboard.get","params":{}}
```

<a id="clipboard.paste-v1"></a>

### `clipboard.paste` v1: Paste

Pastes the clipboard (or a "document" from clipboard.copy's export) at "at": overwrite (Ctrl+V) replaces what lies under the pasted clips, insert (Ctrl+Shift+V) first opens the clipboard's length on the destination and sync-locked tracks. Clips go to the lowest targeted track of their kind (or "videoTrack" / "audioTrack") plus their copied offsets; missing tracks are added. Media is the project's own when it has the file (by id, fingerprint or path), else it is added; "notes" say which. One undo step.

Capability `EditSequence`. Tool name `clipboard_paste`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `addTracks` | boolean |  | Add the video / audio tracks the pasted clips need and the sequence lacks (default true); false pastes only onto the tracks there and fails when they are not enough. |
| `at` | integer \| string | yes | Where the first copied clip lands (the playhead). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `audioTrack` | string \| integer (1..) |  | Destination of the lowest copied audio track. "V1" (bottom video track), "A2", ... or a track id. |
| `captions` | boolean |  | Paste the copied caption cues (default true). |
| `document` | string |  | A clipboard as JSON text (clipboard.copy with "export"); default: this session's clipboard. |
| `markers` | boolean |  | Paste the copied markers (default true). |
| `mode` | `overwrite` \| `insert` |  | Overwrite (default) or insert. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `videoTrack` | string \| integer (1..) |  | Destination of the lowest copied video track. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"clipboard.paste","params":{"at":"00:00:10:00","mode":"insert"}}
```

<a id="color.auto-v1"></a>

### `color.auto` v1: Auto Color

Auto colour: looks at the clip (at "time", default its middle) without its grade and sets the grade's white balance (temperature, tint) and levels (exposure, whites, blacks) as ordinary, editable Basic parameters. One undo step.

Capability `EditSequence`. Tool name `color_auto`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The clip. A clip id, or {"track", "time"} for the clip there. |
| `clips` | (integer \| object (1..))[], min 1 |  | Clips to change (or "clip" for one). |
| `effect` | integer (1..) |  | The grade's effect instance id when the clip has several (clip.details lists them). Default: its first grade. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | The frame to analyse (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"color.auto","params":{"clips":[42,43]}}
```

<a id="color.chartMatch-v1"></a>

### `color.chartMatch` v1: Match to Color Chart

Matches a clip to an X-Rite ColorChecker Classic (24 patches) shown in it: finds the chart in the picture of the clip (at "time", default its middle; without its grade) or takes the four "corners", reads each patch as a trimmed mean of its middle, and fits the clip's grade so the patches come out at the chart's published colours (CIE L*a*b* D50 of the edition after November 2014 or "before-2014", read as sRGB on the monitor): Basic temperature, tint, exposure, contrast and saturation, RGB curves through the grey row and hue vs hue / saturation / luma points at the hues of the coloured patches ("neutralOnly" leaves out saturation and the hue points). The grade's other stages stay; its RGB and hue curves are replaced; keyframes of those Basic parameters are replaced by the new values. "also" gives the same correction to other clips shot in the same light. Reports every patch's delta E 2000 before and after. Refused when no chart is found, fewer than 12 patches can be read, or the fit does not lower the mean error (a chart placed wrongly). One undo step; dryRun reports the fit without changing anything.

Capability `EditSequence`. Tool name `color_chartMatch`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `also` | (integer \| object (1..))[], min 1 |  | Other clips shot in the same light that get the same correction. |
| `clip` | integer \| object (1..) | yes | The clip that shows the chart. A clip id, or {"track", "time"} for the clip there. |
| `corners` | (number[], min 2)[], min 4 |  | The chart's four outer corners (the outer edges of the 24-patch area) as fractions of the picture, y down, in the order top-left, top-right, bottom-right, bottom-left of the chart read upright: dark skin's top-left corner, bluish green's top-right, black's bottom-right, white's bottom-left. Omit to find the chart automatically. |
| `edition` | `after-2014` \| `before-2014` |  | Which print of the chart. Default after-2014. |
| `effect` | integer (1..) |  | The grade's effect instance id when the clip has several (clip.details lists them). Default: its first grade. |
| `neutralOnly` | boolean |  | Only white balance, exposure, contrast and the grey row's curves. Default false. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | The frame with the chart (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"color.chartMatch","params":{"also":[43,44],"clip":{"time":"00:00:03:00","track":"V1"}}}
```

<a id="color.grade.copy-v1"></a>

### `color.grade.copy` v1: Apply Grade

Copies a clip's whole colour grade (every parameter with its keyframes, curves, LUTs, stage switches and the on / off state) over other clips' grades: "clips", or with "scope": "media" every other video clip that shows the same source (the same media item; for nested clips the same sequence, and for multicam the same angle). Each target's first grade is replaced in place (its id, stack position and its own masks stay); a target without a grade gets one at the end of its stack. "replace": false appends a second grade instead, as Paste Attributes does. Audio clips, clips on locked tracks and the source itself are skipped (listed with the reason). One undo step; refused when the source has no grade or no target changes.

Capability `EditSequence`. Tool name `color_grade_copy`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 |  | The clips to apply the grade to (scope "clips"). |
| `from` | integer \| object (1..) | yes | The clip whose grade is copied. A clip id, or {"track", "time"} for the clip there. |
| `replace` | boolean |  | Replace each target's grade (default true); false appends another grade. |
| `scope` | `clips` \| `media` |  | Which clips: "clips" (default) or every video clip of the source's media ("media"). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"color.grade.copy","params":{"from":42,"scope":"media"}}
```

<a id="color.grade.get-v1"></a>

### `color.grade.get` v1: Get Color Grade

A clip's grade at a time (default: its middle): parameters (only those changed unless "all"), animated ones, curves, LUTs, missing LUTs and whether it does anything. "grade": null when the clip has none.

Capability `Read`. Tool name `color_grade_get`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | List every parameter, not only those changed. Default false. |
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | The grade's effect instance id when the clip has several (clip.details lists them). Default: its first grade. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | When (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"color.grade.get","params":{"clip":42}}
```

<a id="color.grade.set-v1"></a>

### `color.grade.set` v1: Set Color Grade

Sets a clip's colour grade (wabcut.color.grade; added at the end of the stack when the clip has none): "params" by registry key (only those given change; effect.list lists them with ranges), "stages" on or off (basic, creative, curves, wheels, hsl, vignette), "curves" as [x, y] points (master/red/green/blue x and y in 0..1; hueSat/hueHue/hueLuma x = hue in turns and lumaSat/satSat y with 0.5 = no change; [] resets), and the "inputLut" / "look" ("builtin:<name>" or "asset:<id>" from lut.list; "" removes). A constant replaces an animated parameter. One undo step for every clip.

Capability `EditSequence`. Tool name `color_grade_set`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The clip. A clip id, or {"track", "time"} for the clip there. |
| `clips` | (integer \| object (1..))[], min 1 |  | Clips to change (or "clip" for one). |
| `curves` | object |  | Curve name -> [[x, y], ...]. |
| `effect` | integer (1..) |  | The grade's effect instance id when the clip has several (clip.details lists them). Default: its first grade. |
| `enabled` | boolean |  | Turn the whole grade on or off. |
| `inputLut` | string |  | Input LUT reference (Basic), or "" for none. |
| `look` | string |  | Look LUT reference (Creative), or "" for none. |
| `params` | object |  | Parameter key -> number, e.g. {"basic.exposure": 0.5, "basic.saturation": 120}. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `stages` | object |  | Stage -> true/false, e.g. {"hsl": false}. |

```json
{"command":"color.grade.set","params":{"clip":{"time":"00:00:05:00","track":"V1"},"params":{"basic.contrast":15,"basic.exposure":0.4}}}
```

<a id="color.inputSpace.get-v1"></a>

### `color.inputSpace.get` v1: Get Input Colour Space

Video clips' input colour spaces: the override (null = auto), what auto reads (the stream's tags or the detected camera log), the log the media's metadata names (null = none), and the one in effect, with the project's colour management. Read only.

Capability `Read`. Tool name `color_inputSpace_get`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The clip. A clip id, or {"track", "time"} for the clip there. |
| `clips` | (integer \| object (1..))[], min 1 |  | Clips to change (or "clip" for one). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"color.inputSpace.get","params":{"clip":42}}
```

<a id="color.inputSpace.set-v1"></a>

### `color.inputSpace.set` v1: Set Input Colour Space

Sets how video clips' pictures are read into the working space (Premiere Pro's Override Media Color Space): "auto" follows the stream's colour tags (BT.709, BT.2020, PQ, HLG, sRGB, linear) and the camera log the media's metadata names, or a space by id - camera log formats are chosen here when the metadata does not say (s-log3, s-log3-cine, v-log, logc3, logc4, canon-log3, canon-log2, red-log3g10, apple-log, acescct; color.management.get lists them all). Direct Rec.709 tone-maps log and HDR clips into SDR. One undo step for every clip.

Capability `EditSequence`. Tool name `color_inputSpace_set`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The clip. A clip id, or {"track", "time"} for the clip there. |
| `clips` | (integer \| object (1..))[], min 1 |  | Clips to change (or "clip" for one). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `space` | `auto` \| `rec709` \| `srgb` \| `rec2020` \| `rec2100-pq` \| `rec2100-hlg` \| `s-log3-cine` \| `s-log3` \| `s-log2-cine` \| `s-log2` \| `v-log` \| `logc3` \| `logc4` \| `canon-log3` \| `canon-log2` \| `canon-log` \| `red-log3g10` \| `red-log-film` \| `apple-log` \| `d-log` \| `f-log` \| `f-log2` \| `l-log` \| `n-log` \| `linear-rec709` \| `acescg` \| `acescct` \| `aces2065-1` | yes | The input colour space, or "auto". |

```json
{"command":"color.inputSpace.set","params":{"clips":[42,43],"space":"s-log3-cine"}}
```

<a id="color.management.get-v1"></a>

### `color.management.get` v1: Get Colour Management

The project's colour management, Premiere Pro's sequence Color Setup: "mode" (rec709 = Direct Rec.709 (SDR), the default; hdr-hlg = Direct HLG (HDR); hdr-pq = Direct PQ (HDR); aces = Wide Gamut (Tone Mapped), ACES 1.3 with the grade in ACEScct; wide-gamut = Wide Gamut (No Tone Mapping); off = Disable Color Management), the HDR peak and graphics (reference) white in nits, whether log media is auto-detected, and the modes and input colour spaces there are (ids, names and descriptions, for color.management.set and color.inputSpace.set).

Capability `Read`. Tool name `color_management_get`.

No parameters.

```json
{"command":"color.management.get","params":{}}
```

<a id="color.management.set-v1"></a>

### `color.management.set` v1: Set Colour Management

Sets the project's colour management (Premiere Pro's sequence Color Setup): "mode" rec709 (Direct Rec.709 (SDR), the default: SDR media as it is, HDR media and log clips tone-mapped into SDR on input), hdr-hlg or hdr-pq (Direct HLG / PQ (HDR): a linear Rec.2020 working space, SDR clips and titles at the graphics white; the monitor shows a tone-mapped SDR preview, HDR exports carry HLG or PQ), aces (Wide Gamut (Tone Mapped): ACES 1.3, clips read through their input colour space into ACEScg, the grade works in ACEScct, the ACES output transforms), wide-gamut (Wide Gamut (No Tone Mapping): ACEScg, clipped at the output's white) or off (Disable Color Management: no conversion at all). "peakNits" (100..10000, default 1000) is the HDR mastering peak, "referenceWhiteNits" (80..400, default 203) the graphics white where SDR white lands, "autoDetectLog" (default true) reads clips as the camera log their metadata names. 0.6: wide-gamut-minimal (Wide Gamut (Minimal Tone Mapping): ACEScg; SDR media and graphics unchanged, camera log and HDR media gamut compressed and their highlights rolled off), "gamutCompression" (ACES 1.3 reference gamut compression of camera log and HDR media in managed setups; values below 0 are then kept) at "gamutCompressionStrength" (0..1), "outputSpace" (what exports write: auto, rec709, rec2020, p3-d65, rec2100-pq, rec2100-hlg) and "sdrConform" (how an HDR or Wide Gamut sequence becomes an SDR export: enabled, targetPeakNits, paperWhiteNits, toneMap bt2390 / aces / reinhard, gamut aces-rgc / saturation / clip, legalRange). Only the given fields change. One undo step.

Capability `EditProject`. Tool name `color_management_set`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `autoDetectLog` | boolean |  | Read clips as the camera log their metadata names. |
| `gamutCompression` | boolean |  | ACES 1.3 gamut compression of camera log and HDR media. |
| `gamutCompressionStrength` | number (0.0..1.0) |  | Gamut compression strength, 0..1. |
| `mode` | `rec709` \| `hdr-hlg` \| `hdr-pq` \| `aces` \| `wide-gamut-minimal` \| `wide-gamut` \| `off` |  | The colour setup. |
| `outputSpace` | `auto` \| `rec709` \| `rec2020` \| `p3-d65` \| `rec2100-pq` \| `rec2100-hlg` |  | What exports write. |
| `peakNits` | number (100.0..10000.0) |  | HDR mastering peak in nits, 100..10000. |
| `referenceWhiteNits` | number (80.0..400.0) |  | HDR graphics (reference) white in nits, 80..400. |
| `sdrConform` | object |  | SDR Conform (only the given fields change). |
| `sdrConform.enabled` | boolean |  | Conform SDR exports of HDR and Wide Gamut setups. |
| `sdrConform.gamut` | `aces-rgc` \| `saturation` \| `clip` |  | The gamut mapping. |
| `sdrConform.legalRange` | boolean |  | Soft-clip the encoded components above 90 %. |
| `sdrConform.paperWhiteNits` | number (48.0..400.0) |  | Where the reference white lands, in nits. |
| `sdrConform.targetPeakNits` | number (48.0..400.0) |  | The SDR display's white in nits (100 or 203). |
| `sdrConform.toneMap` | `bt2390` \| `aces` \| `reinhard` |  | The tone-mapping operator. |

```json
{"command":"color.management.set","params":{"mode":"hdr-pq","peakNits":1000}}
```

<a id="color.match-v1"></a>

### `color.match` v1: Color Match

Makes clips look like a reference frame: "reference" is {"clip", "time"} (that clip as it looks, alone) or a sequence time (the whole picture). Fits the grade's exposure, contrast, temperature, tint and saturation so the clip's colour statistics (Oklab mean and spread) move to the reference's, by "strength"; "skinPriority" weighs skin tones more, "faceDetection" (Premiere's Face Detection) the faces' skin. Editable parameters, one undo step.

Capability `EditSequence`. Tool name `color_match`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | The clip to change. A clip id, or {"track", "time"} for the clip there. |
| `clips` | (integer \| object (1..))[], min 1 |  | Clips to change (or "clip" for one). |
| `effect` | integer (1..) |  | The grade's effect instance id when the clip has several (clip.details lists them). Default: its first grade. |
| `faceDetection` | boolean |  | Face Detection: find the faces on both pictures and match their skin tones first (needs the face detector). Default false. |
| `reference` | object \| integer \| string | yes | {"clip": ..., "time": ...} or a sequence time. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `skinPriority` | boolean |  | Weigh skin tones 4x. Default false. |
| `strength` | number (0.0..1.0) |  | How far to go, 0..1. Default 1. |

```json
{"command":"color.match","params":{"clips":[44,45],"reference":{"clip":43}}}
```

<a id="color.matchScene-v1"></a>

### `color.matchScene` v1: Match Scene

Shot-to-shot consistency: matches every clip of the reference clip's scene to it - "scene" (default) is the run of clips on its track cut from the same take without gaps (what clip.splitAtShots makes), "track" every clip on its track, "media" every clip of the same media item, or an explicit "clips" list. One undo step; dryRun previews it.

Capability `EditSequence`. Tool name `color_matchScene`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 |  | Or exactly these clips. |
| `faceDetection` | boolean |  | Face Detection: find the faces on both pictures and match their skin tones first (needs the face detector). Default false. |
| `reference` | integer \| object (1..) | yes | The clip the others should look like. A clip id, or {"track", "time"} for the clip there. |
| `scope` | `scene` \| `track` \| `media` |  | Which clips. Default scene. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `skinPriority` | boolean |  | Weigh skin tones 4x. Default false. |
| `strength` | number (0.0..1.0) |  | How far to go, 0..1. Default 1. |

```json
{"command":"color.matchScene","params":{"reference":{"time":"00:00:12:00","track":"V1"}}}
```

<a id="dub.create-v1"></a>

### `dub.create` v1: Dub

AI voice-over in another language with the user's own voice provider (ElevenLabs, OpenAI or Gemini; BYOK): each caption cue is spoken and fitted to its slot (sped up at most 15 %, pitch kept; else moved and flagged), on a new "Dub" audio track with the Dialogue role, the original dialogue ducked under it, as one undo step. Bleeped words are never spoken. The audio is marked AI-generated. Paid: "estimate" prices it; a run needs "maxCost" and the app's approval (standalone CLI/MCP and the assistant only estimate or open the Dub dialog).

Capability `EditProject`. Tool name `dub_create`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captionTrack` | integer (1..) |  | The caption track to speak (usually a translated one); only needed when the sequence has several. |
| `cloneVoice` | boolean |  | Clone the speaker's own voice (ElevenLabs instant cloning) instead of a stock voice. Opt-in; needs rightsConfirmed and speaker; never for a speaker with redacted speech. The clone is deleted after the run. |
| `duck` | number (-60.0..0.0) |  | How far the original dialogue goes down under the new lines, dB (default -18; 0 = not at all). |
| `estimate` | boolean |  | Only plan and price the run; nothing is sent or changed (default false). |
| `language` | string |  | The language spoken (ISO 639-1); dub default: the caption track's. |
| `maxCost` | number (0.0..1000.0) |  | The run's hard cap in USD: refused before anything is sent when it could cost more, stopped before a line that could pass it. Required for a run. |
| `maxStretch` | number (0.0..0.5) |  | How much faster a line may be played to fit (0.15 = 15 %, the default; pitch kept). |
| `model` | string |  | Voice model (registry id): eleven_v3 / eleven_v4 (Thai and 70+ languages), eleven_flash_v2_5, gpt-4o-mini-tts, gemini-3.8-flash-tts. Default: the host's, else eleven_v3. |
| `name` | string |  | The new audio track's name. |
| `rightsConfirmed` | boolean |  | The user confirmed: "I have the speaker's permission" to clone their voice. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speaker` | integer (1..) |  | The project speaker whose voice is cloned. |
| `style` | string |  | Delivery instructions ("warm, unhurried"), where the model takes them. |
| `voice` | string |  | The model's voice (an ElevenLabs voice id, an OpenAI voice such as "marin", a Gemini voice such as "Kore"); default: the model's first. |

```json
{"command":"dub.create","params":{"estimate":true,"language":"th"}}
```

<a id="edit.clear-v1"></a>

### `edit.clear` v1: Delete

Delete (Premiere Delete key): removes clips and leaves gaps.

Capability `EditSequence`. Tool name `edit_clear`. Aliases: `clip.delete`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to delete. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"edit.clear","params":{"clips":[42,{"time":"00:00:03:00","track":"V2"}]}}
```

<a id="edit.cutToBeats-v1"></a>

### `edit.cutToBeats` v1: Cut to Beats

Music-video assembly: places the given clips' media (from each clip's source In) and media items (from their start) one after another, each lasting beatsPerCut beats of the beat grid, starting at the first beat at or after "at" (the playhead). Overwrites what is there; video goes to videoTrack, audio only when audioTrack is given (the music usually stays alone).

Capability `EditSequence`. Tool name `edit_cutToBeats`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string | yes | Start at the first beat at or after this time (the playhead). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `audioTrack` | string \| integer (1..) |  | Audio destination; none by default. "V1" (bottom video track), "A2", ... or a track id. |
| `beatsPerCut` | integer (1..64) |  | Beats per clip. Default 4 (a bar). |
| `clips` | (integer \| object (1..))[], min 0 |  | Clips whose media to use, in order. |
| `media` | (integer \| string (1..))[] |  | Media items to use, after the clips. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `videoTrack` | string \| integer (1..) |  | Video destination. Default V1. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"edit.cutToBeats","params":{"at":"00:00:00:00","beatsPerCut":2,"media":["Shot 1","Shot 2","Shot 3"]}}
```

<a id="edit.insertFromSource-v1"></a>

### `edit.insertFromSource` v1: Insert from Source

Insert edit from the source (Premiere ','), with three-point editing: the source marks (stored on the media unless given), the sequence In/Out marks and the playhead decide where it goes and what it uses. Later material on the patched and sync-locked tracks moves later. A four-point edit whose ranges differ takes a "fit" choice (Premiere's Fit Clip dialog).

Capability `EditSequence`. Tool name `edit_insertFromSource`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string |  | Where the edit starts (the playhead). Ignored when the sequence has In/Out marks: Premiere's three-point editing uses them. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `fit` | `speed` \| `ignoreSourceIn` \| `ignoreSourceOut` \| `ignoreSequenceIn` \| `ignoreSequenceOut` |  | Four-point edit whose source and sequence ranges differ (Premiere's Fit Clip dialog): "speed" changes the clip speed to fill the range (fit to fill); the others ignore one mark. Default: ignoreSourceOut, with a warning. |
| `media` | integer \| string (1..) | yes | The source media. A media id or exact name. |
| `patches` | object[] |  | Source patching: which source track goes to which sequence track. |
| `patches[].enabled` | boolean |  | false switches the indicator off (Alt-click). Default true. |
| `patches[].newTrack` | string |  | Audio source tracks: the name of the track it goes to (multi-track recordings). Without "track" the edit adds an audio track with this name below the last one; an unnamed "track" is given the name. A recognised name sets the new track's role (Mic, Discord: dialogue; Music: music; Game, Alerts: effects). |
| `patches[].source` | string | yes | Source track: "V1" (the video), "A1", "A2", ... (audio streams). |
| `patches[].track` | string \| integer \| null |  | Destination track ("V2", "A1", ...), or null for none. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sequenceMarks` | boolean |  | false places the edit at "at" even when the sequence has In/Out marks (a drag onto the timeline). Default true. |
| `sourceIn` | integer \| string \| null |  | Source In: the first source frame. Default: the media's stored In mark, else its start; null for none. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `sourceOut` | integer \| string \| null |  | Source Out (exclusive). Default: the media's stored Out mark, else its end; null for none. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `targets` | (string \| integer (1..))[] |  | Tracks to target after the edit (Premiere's targeting column). Default: unchanged. |

```json
{"command":"edit.insertFromSource","params":{"at":"00:00:10:00","media":3,"patches":[{"source":"V1","track":"V2"},{"source":"A1","track":"A1"}]}}
```

<a id="edit.overwriteFromSource-v1"></a>

### `edit.overwriteFromSource` v1: Overwrite from Source

Overwrite edit from the source (Premiere '.'), with three-point editing like edit.insertFromSource: replaces what is on the patched tracks; nothing moves. "fit": "speed" retimes the clip to fill the sequence In/Out exactly.

Capability `EditSequence`. Tool name `edit_overwriteFromSource`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string |  | Where the edit starts (the playhead). Ignored when the sequence has In/Out marks: Premiere's three-point editing uses them. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `fit` | `speed` \| `ignoreSourceIn` \| `ignoreSourceOut` \| `ignoreSequenceIn` \| `ignoreSequenceOut` |  | Four-point edit whose source and sequence ranges differ (Premiere's Fit Clip dialog): "speed" changes the clip speed to fill the range (fit to fill); the others ignore one mark. Default: ignoreSourceOut, with a warning. |
| `media` | integer \| string (1..) | yes | The source media. A media id or exact name. |
| `patches` | object[] |  | Source patching: which source track goes to which sequence track. |
| `patches[].enabled` | boolean |  | false switches the indicator off (Alt-click). Default true. |
| `patches[].newTrack` | string |  | Audio source tracks: the name of the track it goes to (multi-track recordings). Without "track" the edit adds an audio track with this name below the last one; an unnamed "track" is given the name. A recognised name sets the new track's role (Mic, Discord: dialogue; Music: music; Game, Alerts: effects). |
| `patches[].source` | string | yes | Source track: "V1" (the video), "A1", "A2", ... (audio streams). |
| `patches[].track` | string \| integer \| null |  | Destination track ("V2", "A1", ...), or null for none. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sequenceMarks` | boolean |  | false places the edit at "at" even when the sequence has In/Out marks (a drag onto the timeline). Default true. |
| `sourceIn` | integer \| string \| null |  | Source In: the first source frame. Default: the media's stored In mark, else its start; null for none. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `sourceOut` | integer \| string \| null |  | Source Out (exclusive). Default: the media's stored Out mark, else its end; null for none. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `targets` | (string \| integer (1..))[] |  | Tracks to target after the edit (Premiere's targeting column). Default: unchanged. |

```json
{"command":"edit.overwriteFromSource","params":{"fit":"speed","media":3,"sourceIn":"00:00:01:00","sourceOut":"00:00:05:00"}}
```

<a id="edit.snapToBeats-v1"></a>

### `edit.snapToBeats` v1: Snap to Beats

Trims each clip's head and/or tail to the nearest beat of the beat grid when it is within the tolerance (linked partners follow). Edges that cannot reach their beat (a neighbouring clip, too little media) are left and reported.

Capability `EditSequence`. Tool name `edit_snapToBeats`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | Clips to snap. |
| `edges` | `both` \| `head` \| `tail` |  | Which edges. Default both. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tolerance` | integer \| string |  | Largest move. Default: a quarter beat. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"edit.snapToBeats","params":{"clips":[42,43],"tolerance":"00:00:00:06"}}
```

<a id="effect.aiStatus-v1"></a>

### `effect.aiStatus` v1: AI Effects Status

Where the AI video effects stand: AI Restore and AI Denoise ("kind": "pictures"), AI Refocus's depth maps ("depth"), AI Face Retouch's face maps ("faces"), AI Eye Contact's eye measurements ("gaze") and AI Frame Interpolation's in-between pictures ("interpolation"). For each video clip of the sequence with such an effect (or the one named), the chain they form ("Restore 2x, Denoise", "Depth maps", "Frame interpolation"), the models it needs and whether each is on this computer (size, licence), how many of the clip's pictures are processed, the output size and about how long the rest takes. States: ready, partial, notProcessed, needsModel, noHelper. A read: it changes nothing.

Capability `Read`. Tool name `effect_aiStatus`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) |  | Only this clip (default: every clip of the sequence with AI effects). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effect.aiStatus","params":{}}
```

<a id="effect.analyzeAll-v1"></a>

### `effect.analyzeAll` v1: Analyze All Stabilizers

Analyses every Warp Stabilizer and Rolling Shutter Repair of the sequence (and of the sequences nested in it, and of its clips' media's Master Clip Effects) that is not analysed yet or whose analysis is out of date - the clip was trimmed longer, moved onto other footage, or a setting such as Smoothness changed - in one job with one progress bar and one undo step. `reanalyze` redoes every one. The raw motion is cached per media, so redoing an effect whose footage was analysed before is quick. Effects that cannot be analysed (a nested clip's own, a freeze frame, a switched-off effect) are listed under `skipped`. Morph Cut needs no analysis step: it analyses itself as soon as it is applied. Runs as a job with progress and cancel.

Capability `EditSequence`. Tool name `effect_analyzeAll`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `kind` | `all` \| `warp` \| `shutter` |  | Which effects: Warp Stabilizer and Rolling Shutter Repair ("all", the default), or one of them. |
| `nested` | boolean |  | Also the sequences nested in this one (their clips keep their own effects). |
| `reanalyze` | boolean |  | Analyse every one again, not only those that are not analysed or out of date. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effect.analyzeAll","params":{"reanalyze":false}}
```

<a id="effect.analyzeRollingShutter-v1"></a>

### `effect.analyzeRollingShutter` v1: Analyze Rolling Shutter

Rolling Shutter Repair's analysis: measures the picture's motion per frame of a video clip's footage and keys it (motion.x, motion.y: source pixels per frame) into the clip's Rolling Shutter Repair (added at the top of its stack when it has none), which then skews each line back by its share of the scan (Rolling Shutter Rate, Scan Direction). Shares the cached analysis with Warp Stabilizer's Position method. One in the clip's media's Master Clip Effects (named by `effect`) is analysed over the frames every clip of that media shows.

Capability `EditSequence`. Tool name `effect_analyzeRollingShutter`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | The video clip (id). |
| `effect` | integer (1..) |  | Its Rolling Shutter Repair instance (id), in the clip's stack or its media's Master Clip Effects. Default: the clip's first (or a new one). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effect.analyzeRollingShutter","params":{"clip":42}}
```

<a id="effect.analyzeStabilization-v1"></a>

### `effect.analyzeStabilization` v1: Analyze Stabilization

Warp Stabilizer's Analyze: tracks the camera motion of a video clip's footage, smooths the camera path with the effect's settings (Result, Smoothness, Method, Preserve Scale, Framing, Maximum Scale, Action-Safe Margin, Additional Scale, Detailed Analysis, Rolling Shutter Ripple, Crop Less Smooth More) and keys the compensating transform per frame into the clip's Warp Stabilizer (added at the top of its stack when it has none), with the crop or auto-scale that hides the moving borders. The keys stay editable. Changing a setting and running it again re-smooths from the cached analysis. Method 4 (Gyro, camera metadata) takes the rotation the camera recorded in the file (GoPro HERO5 and later, Sony cameras with a gyro) instead of tracking the picture: it is synced to the picture on a few stretches of the clip (gyroSync, gyroOffset, gyroFov, gyroReadout), smoothed with the same settings and keyed as a corner pin; the result reports the camera, the sync and the field of view under `gyro`, and a file without gyro data is refused with the reason. A Warp Stabilizer in the clip's media's Master Clip Effects (named by `effect`) is analysed over the frames every clip of that media shows, so every instance plays it stabilized. Runs as a job with progress and cancel; a dry run analyses and previews the keys.

Capability `EditSequence`. Tool name `effect_analyzeStabilization`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | The video clip (id). |
| `effect` | integer (1..) |  | Its Warp Stabilizer instance (id), in the clip's stack or its media's Master Clip Effects. Default: the clip's first (or a new one). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effect.analyzeStabilization","params":{"clip":42}}
```

<a id="effect.list-v1"></a>

### `effect.list` v1: List Effects

The effects and transitions Wabcut knows: id, name, category, kind (video, audio, videoTransition, audioTransition) and parameters as arrays in paramFields order. builtIn effects are part of every clip (set their parameters with clip.setParam or keyframes); the others go into a clip's stack.

Capability `Read`. Tool name `effect_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `kind` | `video` \| `audio` \| `videoTransition` \| `audioTransition` |  | Only effects of this kind. |

```json
{"command":"effect.list","params":{"kind":"video"}}
```

<a id="effect.processAi-v1"></a>

### `effect.processAi` v1: Process AI Effects

Makes the pictures the AI video effects show: for one clip (or every clip of the sequence whose pictures are incomplete) the part of its media it uses goes through the chain's models - denoise, deblur, upscale, in that order, on this computer - and is stored in the processed cache, where playback and exports find it. Also AI Refocus's depth map, AI Face Retouch's face map and AI Eye Contact's eye measurements of every frame, and, for a retimed clip whose Time Interpolation is AI Frame Interpolation, the in-between picture of every output frame. Pictures already made are skipped, so a stopped run continues. Needs the models downloaded and the AI helper. A job with progress and cancel; it changes nothing in the project (no undo step). A dry run lists what it would process.

Capability `MediaAnalysis`. Tool name `effect_processAi`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) |  | Only this clip (default: every clip of the sequence whose AI pictures are incomplete). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effect.processAi","params":{"clip":42}}
```

<a id="effects.addToCustomBin-v1"></a>

### `effects.addToCustomBin` v1: Add to Custom Bin

Puts effects, transitions or presets in a custom bin (dragging them there in Premiere). Items already in it stay once.

Capability `EditProject`. Tool name `effects_addToCustomBin`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `customBin` | string | yes | The custom bin. |
| `items` | string[], min 1 | yes | The items. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.addToCustomBin","params":{"customBin":"Gaming","items":["crossDissolve"]}}
```

<a id="effects.applyPreset-v1"></a>

### `effects.applyPreset` v1: Apply Preset

Applies a preset to clips (double-click or drag from the Effects panel's Presets bin): its stack effects are added after each clip's own, its fixed effects replace the clip's Motion / Opacity / Volume / Panner values (keyframes included), and keyframes are timed by its type. Clips of the other kind (a video preset on audio) are skipped and reported. Built-in presets (effects.library) and the user's.

Capability `EditSequence`. Tool name `effects_applyPreset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips. |
| `preset` | string | yes | The preset's name (case-insensitive). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.applyPreset","params":{"clips":[42],"preset":"Twirl In"}}
```

<a id="effects.applyTrackPreset-v1"></a>

### `effects.applyTrackPreset` v1: Apply Preset to Tracks

Applies an audio preset to the effect chain of audio tracks, submix tracks or the audio master: its effects are added after the chain's own (keyframes timed by the preset's type against the track's length). A preset saved from a clip also works; its Volume and Panner are skipped and reported. One undo step.

Capability `EditSequence`. Tool name `effects_applyTrackPreset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `preset` | string | yes | The preset's name (case-insensitive): the user's or a built-in audio preset. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `targets` | (integer \| string)[], min 1 | yes | The chains to apply it to. |

```json
{"command":"effects.applyTrackPreset","params":{"preset":"Voice chain","targets":["A2"]}}
```

<a id="effects.deleteCustomItem-v1"></a>

### `effects.deleteCustomItem` v1: Delete Custom Item

Deletes a user item of the Effects panel (Premiere's Delete Custom Item): a preset (it leaves custom bins and favourites too), a presets bin with the presets in it, a custom bin, or one item of a custom bin ("customBin" with "item"). Built-in presets and bins cannot be deleted.

Capability `EditProject`. Tool name `effects_deleteCustomItem`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `customBin` | string |  | A custom bin. |
| `item` | string |  | With "customBin": the item to take out of it (its reference, effect id or preset name). |
| `preset` | string |  | A user preset. |
| `presetsBin` | string |  | A presets bin (its path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.deleteCustomItem","params":{"customBin":"Gaming","item":"crossDissolve"}}
```

<a id="effects.exportPresets-v1"></a>

### `effects.exportPresets` v1: Export Effect Presets

Writes presets to a file (Premiere's Export Preset) another Wabcut can import: named presets (built-in or the user's) and/or the user's presets in a presets bin ("" = all of them).

Capability `Read`. Tool name `effects_exportPresets`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `overwrite` | boolean |  | Replace an existing file (default false). |
| `path` | string | yes | The file to write (".wabcut-presets.json"). |
| `presets` | string[] |  | Presets by name. |
| `presetsBin` | string |  | Every user preset in this bin and the bins inside it. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.exportPresets","params":{"path":"C:/Presets/mine.wabcut-presets.json","presetsBin":""}}
```

<a id="effects.importPresets-v1"></a>

### `effects.importPresets` v1: Import Presets

Adds the presets of a presets file (effects.exportPresets) to the user's library (Premiere's Import Presets). The file is untrusted: read bounded and checked. A name in use gets a number ("Glow 2").

Capability `EditProject`. Tool name `effects_importPresets`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | string |  | Put them in this presets bin (default: the bins they were exported from). |
| `path` | string | yes | The presets file. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.importPresets","params":{"path":"C:/Presets/mine.wabcut-presets.json"}}
```

<a id="effects.library-v1"></a>

### `effects.library` v1: Effects Library

The Effects panel's presets and the user's items: every preset (the built-in Presets and Lumetri Presets bins, and the user's presets with their presets bins; name, type, description, bin, kind, effects), the user's presets bins, custom bins (their items: effects, transitions and presets) and favourites. Built-in presets are made for the sequence's frame size (PiP positions). The library is the user's, kept by the host ("file"; "error" when it could not be read or written).

Capability `Read`. Tool name `effects_library`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.library","params":{}}
```

<a id="effects.newCustomBin-v1"></a>

### `effects.newCustomBin` v1: New Custom Bin

Makes a custom bin in the Effects panel (Premiere's New Custom Bin): a bin of shortcuts to effects, transitions and presets. Default name: "Custom Bin 01", "Custom Bin 02", ...

Capability `EditProject`. Tool name `effects_newCustomBin`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `items` | string[] |  | Items to put in it. |
| `name` | string |  | Its name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.newCustomBin","params":{"items":["wabcut.blur.gaussian","preset:Twirl In"],"name":"Gaming"}}
```

<a id="effects.newPresetsBin-v1"></a>

### `effects.newPresetsBin` v1: New Presets Bin

Makes a bin in the Presets bin for the user's presets ("A/B": B inside A; the parents are made too).

Capability `EditProject`. Tool name `effects_newPresetsBin`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | string | yes | The bin's path, e.g. "Lower thirds". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.newPresetsBin","params":{"bin":"Lower thirds"}}
```

<a id="effects.renameCustomItem-v1"></a>

### `effects.renameCustomItem` v1: Rename Custom Item

Renames a custom bin, a presets bin or a user preset (custom bins and favourites follow a renamed preset).

Capability `EditProject`. Tool name `effects_renameCustomItem`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `customBin` | string |  | A custom bin to rename. |
| `name` | string | yes | The new name. |
| `preset` | string |  | A user preset to rename. |
| `presetsBin` | string |  | A presets bin to rename (its path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.renameCustomItem","params":{"customBin":"Custom Bin 01","name":"Gaming"}}
```

<a id="effects.savePreset-v1"></a>

### `effects.savePreset` v1: Save Effect Preset

Saves effects of a clip as a preset in the user's library (Premiere's Effect Controls > Save Preset): stack effects by index and the clip's fixed effects (motion, opacity with its blend mode, volume, panner), with their keyframes and the preset type that times them on other clips. LUT files are referred to by path, tracked matte masks are left out ("notes"). Not an undo step: the library is the user's.

Capability `EditProject`. Tool name `effects_savePreset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | string |  | The presets bin it goes in, e.g. "Lower thirds/Blue" (default: the Presets bin). |
| `clip` | integer \| object (1..) | yes | The clip whose effects to save. A clip id, or {"track", "time"} for the clip there. |
| `description` | string |  | A description (Premiere's Description field). |
| `effects` | (integer (0..))[] |  | Stack effects to save. |
| `fixed` | (`motion` \| `opacity` \| `volume` \| `panner`)[] |  | Fixed effects to save (Motion and Opacity of a video clip, Volume and Panner of an audio clip). |
| `master` | (integer (0..))[] |  | 0.6: master clip effects to save (they come first in the preset). |
| `name` | string | yes | The preset's name: unique among the user's presets, not a built-in's. |
| `replace` | boolean |  | Replace a preset of yours with that name (default false: refused). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `type` | `scale` \| `anchorToInPoint` \| `anchorToOutPoint` |  | Premiere's preset type: how the keyframes land on a clip of another length. "scale" (default): stretched with the clip; "anchorToInPoint": the same distance from its In point; "anchorToOutPoint": the same distance from its Out point. |

```json
{"command":"effects.savePreset","params":{"clip":42,"effects":[0],"fixed":["motion"],"name":"Slow push","type":"anchorToInPoint"}}
```

<a id="effects.saveTrackPreset-v1"></a>

### `effects.saveTrackPreset` v1: Save Track Effects as Preset

Saves the effect chain of an audio track, a submix track or the audio master (an Audio Track Mixer strip) as a preset in the user's library: its effects in order with their settings and keyframes (a bypassed effect stays bypassed). The fader and panner are not part of a chain. Apply it to other strips or tracks with effects.applyTrackPreset; it also shows in the Effects panel's Presets bin as an audio preset. The preset type times keyframes on a chain of another length (track effects use timeline times); default "anchorToInPoint" (they keep their time). Not an undo step: the library is the user's.

Capability `EditProject`. Tool name `effects_saveTrackPreset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | string |  | The presets bin it goes in, e.g. "Voice chains" (default: the Presets bin). |
| `description` | string |  | A description. |
| `effects` | (integer (0..))[] |  | Only these effects (default: the whole chain). |
| `name` | string | yes | The preset's name: unique among the user's presets, not a built-in's. |
| `replace` | boolean |  | Replace a preset of yours with that name (default false: refused). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| string | yes | "master" (the audio master), an audio track ("A1" or its id) or a submix track ("S1"). |
| `type` | `scale` \| `anchorToInPoint` \| `anchorToOutPoint` |  | Premiere's preset type: how the keyframes land on a clip of another length. "scale" (default): stretched with the clip; "anchorToInPoint": the same distance from its In point; "anchorToOutPoint": the same distance from its Out point. |

```json
{"command":"effects.saveTrackPreset","params":{"name":"Voice chain","target":"A1"}}
```

<a id="effects.setFavorite-v1"></a>

### `effects.setFavorite` v1: Add to Favorites

Marks an effect, transition or preset as a favourite (the Effects panel's Favorites bin), or with "favorite": false takes it off the list.

Capability `EditProject`. Tool name `effects_setFavorite`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `favorite` | boolean |  | true (default) adds it, false removes it. |
| `item` | string | yes | "effect:<id>", "preset:<name>", or an effect id or preset name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"effects.setFavorite","params":{"item":"wabcut.blur.gaussian"}}
```

<a id="effects.setPresetProperties-v1"></a>

### `effects.setPresetProperties` v1: Preset Properties

Changes a user preset (Premiere's Preset Properties): its name (custom bins and favourites follow), type, description, or the presets bin it is in.

Capability `EditProject`. Tool name `effects_setPresetProperties`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | string |  | The presets bin to move it to ("" = the Presets bin). |
| `description` | string |  | A new description. |
| `name` | string |  | A new name. |
| `preset` | string | yes | The preset. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `type` | `scale` \| `anchorToInPoint` \| `anchorToOutPoint` |  | Premiere's preset type: how the keyframes land on a clip of another length. "scale" (default): stretched with the clip; "anchorToInPoint": the same distance from its In point; "anchorToOutPoint": the same distance from its Out point. |

```json
{"command":"effects.setPresetProperties","params":{"preset":"Slow push","type":"scale"}}
```

<a id="export.check-v1"></a>

### `export.check` v1: Check Before Export

0.7, the pre-export check: one list of what is likely to go wrong when the sequence is exported with a preset (and overrides), worked out without exporting - offline or missing media (a problem: the export stops), gaps in the picture, flash frames, clips at another frame rate, stabilizers never analyzed, slow effects, a size or shape that does not suit the sequence, captions the file will not show. Rows come most serious first (problem, warning, note), then in timeline order: kind, severity, a plain message, the fix the app offers ("relink", "closeGap", "matchSequenceSize", "turnOnCaptions" or "none"), and where it is ("at" in ticks, "timecode", "clip", "track"). Loudness rows need a measurement of the mix, which the app makes on request; this command does not render.

Capability `Read`. Tool name `export_check`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `in` | integer \| string |  | Custom range start (with "out"). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `out` | integer \| string |  | Custom range end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `overrides` | object |  | Settings on top of the preset (ExportController's keys, docs/export.md): container, videoCodec, encoder, quality, bitrateMbps, maxBitrateMbps, frameRate, width, height, scaleMode, range (whole \| inOut \| workArea \| custom), video, audio, audioCodec, audioChannels, sampleRate, audioBitrateKbps, dynamicRange, dynamicMetadata (none \| hdr10plus: HDR10+ on a PQ HEVC master), captions, pixelFormat, alpha (Include Alpha Channel), proresProfile, dnxhrProfile, exportAsSequence, subtitles, targetMB (a size limit in MB, 1-4000, for H.264, HEVC, AV1 or VP9: the whole file at most that size; 0 = off), and the objects effects, metadata and render (export.describe shows their fields). |
| `preset` | string |  | A preset id (export.presets: a built-in one such as "match-source", or a user preset "user-..."). |
| `range` | `whole` \| `inOut` \| `workArea` \| `custom` |  | Premiere's Range: Entire Source (default), Source In/Out, Work Area, Custom. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"export.check","params":{"preset":"youtube-1080p"}}
```

<a id="export.describe-v1"></a>

### `export.describe` v1: Describe Export

What an export of the sequence with a preset (and overrides) would write, without exporting: the output file, the resolved settings (container, codecs, size, frame rate, effects, metadata, ...), the range and its frame count.

Capability `Read`. Tool name `export_describe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `in` | integer \| string |  | Custom range start (with "out"). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `out` | integer \| string |  | Custom range end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `output` | string |  | The output file (default: next to the project, named after the sequence). |
| `overrides` | object |  | Settings on top of the preset (ExportController's keys, docs/export.md): container, videoCodec, encoder, quality, bitrateMbps, maxBitrateMbps, frameRate, width, height, scaleMode, range (whole \| inOut \| workArea \| custom), video, audio, audioCodec, audioChannels, sampleRate, audioBitrateKbps, dynamicRange, dynamicMetadata (none \| hdr10plus: HDR10+ on a PQ HEVC master), captions, pixelFormat, alpha (Include Alpha Channel), proresProfile, dnxhrProfile, exportAsSequence, subtitles, targetMB (a size limit in MB, 1-4000, for H.264, HEVC, AV1 or VP9: the whole file at most that size; 0 = off), and the objects effects, metadata and render (export.describe shows their fields). |
| `preset` | string |  | A preset id (export.presets: a built-in one such as "match-source", or a user preset "user-..."). |
| `range` | `whole` \| `inOut` \| `workArea` \| `custom` |  | Premiere's Range: Entire Source (default), Source In/Out, Work Area, Custom. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"export.describe","params":{"overrides":{"quality":18},"preset":"youtube-1080p"}}
```

<a id="export.preset.delete-v1"></a>

### `export.preset.delete` v1: Delete Preset

Deletes a user preset (built-in presets cannot be deleted).

Capability `EditProject`. Tool name `export_preset_delete`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `id` | string | yes | A preset id (export.presets: a built-in one such as "match-source", or a user preset "user-..."). |

```json
{"command":"export.preset.delete","params":{"id":"user-my-youtube"}}
```

<a id="export.preset.export-v1"></a>

### `export.preset.export` v1: Export Preset

Writes a preset (built-in or the user's) to a preset file others can import.

Capability `Read`. Tool name `export_preset_export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `file` | string | yes | The file to write. |
| `id` | string | yes | A preset id (export.presets: a built-in one such as "match-source", or a user preset "user-..."). |

```json
{"command":"export.preset.export","params":{"file":"C:/Presets/My YouTube.json","id":"user-my-youtube"}}
```

<a id="export.preset.import-v1"></a>

### `export.preset.import` v1: Import Preset

Premiere's Import Preset: reads a Wabcut export preset file (.json) and stores it with the user's presets. The file is checked like any untrusted input (types, ranges, sizes).

Capability `Read`. Tool name `export_preset_import`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `file` | string | yes | The preset file. |
| `overwrite` | boolean |  | Replace a preset of the same name (default false). |

```json
{"command":"export.preset.import","params":{"file":"C:/Presets/Client delivery.json"}}
```

<a id="export.preset.save-v1"></a>

### `export.preset.save` v1: Save Preset

Premiere's Save Preset: stores a preset's settings with overrides on top as a user preset (a JSON file in the user's export presets folder). "overwrite" replaces one of the same name.

Capability `EditProject`. Tool name `export_preset_save`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `description` | string |  | A one-line description. |
| `name` | string | yes | The preset's name (1..256 bytes); its id is "user-" and the name in lowercase. |
| `overrides` | object |  | Settings on top of the preset (ExportController's keys, docs/export.md): container, videoCodec, encoder, quality, bitrateMbps, maxBitrateMbps, frameRate, width, height, scaleMode, range (whole \| inOut \| workArea \| custom), video, audio, audioCodec, audioChannels, sampleRate, audioBitrateKbps, dynamicRange, dynamicMetadata (none \| hdr10plus: HDR10+ on a PQ HEVC master), captions, pixelFormat, alpha (Include Alpha Channel), proresProfile, dnxhrProfile, exportAsSequence, subtitles, targetMB (a size limit in MB, 1-4000, for H.264, HEVC, AV1 or VP9: the whole file at most that size; 0 = off), and the objects effects, metadata and render (export.describe shows their fields). |
| `overwrite` | boolean |  | Replace a preset of the same name (default false). |
| `preset` | string |  | A preset id (export.presets: a built-in one such as "match-source", or a user preset "user-..."). |

```json
{"command":"export.preset.save","params":{"name":"My YouTube","overrides":{"bitrateMbps":20},"preset":"youtube-1080p"}}
```

<a id="export.presets-v1"></a>

### `export.presets` v1: Export Presets

The export presets: the built-in ones (YouTube, Shorts / TikTok / Reels, Twitch, Discord sizes, ProRes / DNxHR / DNxHD masters, ProRes 4444 / QuickTime Animation / CineForm with alpha, MP3 / AAC / AIFF / WAV audio, PNG / TIFF / DPX / JPEG / Targa / OpenEXR image sequences, HDR) and the user's saved ones: id, name, description, builtIn, and the full settings.

Capability `Read`. Tool name `export_presets`.

No parameters.

```json
{"command":"export.presets","params":{}}
```

<a id="export.queue-v1"></a>

### `export.queue` v1: Send to Media Encoder

Queues exports of the sequence, one per preset in "presets" (to the matching "outputs"), with the same overrides: Premiere's Export with several destinations and Send to Media Encoder. They run one after another in the background. With "hold" they wait in the queue until export.queue.start (Media Encoder's queue before Start Queue). P1: "ranges" ([{in, out, name}], with one preset) queues one export per range instead - each marker range or clip as its own file - named "<sequence> - <name>" in "folder" (default: the preset's usual folder) and numbered when taken; an empty range queues none. 0.6: "stems": "role" queues the dialogue / music / effects stems after the first export (after each range's with "ranges"); they are listed with a "stem" key.

Capability `Read`. Tool name `export_queue`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `folder` | string |  | P1, with "ranges": the folder the files go to. |
| `hold` | boolean |  | Queue without starting (default false). |
| `in` | integer \| string |  | Custom range start (with "out"). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `out` | integer \| string |  | Custom range end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `outputs` | string[] |  | The output file of each preset, in order (default: next to the project). |
| `overrides` | object |  | Settings on top of the preset (ExportController's keys, docs/export.md): container, videoCodec, encoder, quality, bitrateMbps, maxBitrateMbps, frameRate, width, height, scaleMode, range (whole \| inOut \| workArea \| custom), video, audio, audioCodec, audioChannels, sampleRate, audioBitrateKbps, dynamicRange, dynamicMetadata (none \| hdr10plus: HDR10+ on a PQ HEVC master), captions, pixelFormat, alpha (Include Alpha Channel), proresProfile, dnxhrProfile, exportAsSequence, subtitles, targetMB (a size limit in MB, 1-4000, for H.264, HEVC, AV1 or VP9: the whole file at most that size; 0 = off), and the objects effects, metadata and render (export.describe shows their fields). |
| `presets` | string[], min 1 | yes | One export per preset (Premiere's several destinations). |
| `range` | `whole` \| `inOut` \| `workArea` \| `custom` |  | Premiere's Range: Entire Source (default), Source In/Out, Work Area, Custom. |
| `ranges` | object[], min 1 |  | P1: one file per range (one preset; not with "outputs"). |
| `ranges[].in` | integer \| string | yes | The range's start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `ranges[].name` | string |  | Names its file. |
| `ranges[].out` | integer \| string | yes | The range's end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `stems` | `off` \| `role` |  | 0.6: "role" also writes the dialogue, music and effects stems (one audio-only 24-bit WAV per track role with an audible track, "_DX", "_MX", "_FX", "_OTHER" for tracks without a role) next to the export, queued after it, sample-aligned with it; never loudness-normalised. Default off. |
| `stemsMasterEffects` | boolean |  | 0.6, with "stems": the stems go through the master bus's effects (default true). |

```json
{"command":"export.queue","params":{"presets":["youtube-1080p60","shorts-1080x1920-60"]}}
```

<a id="export.queue.cancel-v1"></a>

### `export.queue.cancel` v1: Stop

Cancels jobs (a running export leaves no file). "jobs": the job ids; "all": every job of the queue.

Capability `EditProject`. Tool name `export_queue_cancel`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | Every job (default false). |
| `jobs` | string[], min 1 |  | The jobs. |

```json
{"command":"export.queue.cancel","params":{"jobs":["j1"]}}
```

<a id="export.queue.list-v1"></a>

### `export.queue.list` v1: Export Queue

The export queue: every job with its output, preset, state (held, queued, running, paused, done, failed, cancelled), progress 0..1 and detail (the result or the error).

Capability `Read`. Tool name `export_queue_list`.

No parameters.

```json
{"command":"export.queue.list","params":{}}
```

<a id="export.queue.pause-v1"></a>

### `export.queue.pause` v1: Pause

Pauses jobs: a running one between two frames, a queued one is held. "jobs": the job ids; "all": every job of the queue.

Capability `EditProject`. Tool name `export_queue_pause`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | Every job (default false). |
| `jobs` | string[], min 1 |  | The jobs. |

```json
{"command":"export.queue.pause","params":{"jobs":["j1"]}}
```

<a id="export.queue.remove-v1"></a>

### `export.queue.remove` v1: Remove

Removes finished jobs from the list. "jobs": the job ids; "all": every job of the queue.

Capability `EditProject`. Tool name `export_queue_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | Every job (default false). |
| `jobs` | string[], min 1 |  | The jobs. |

```json
{"command":"export.queue.remove","params":{"jobs":["j1"]}}
```

<a id="export.queue.resume-v1"></a>

### `export.queue.resume` v1: Resume

Resumes paused jobs and starts held ones. "jobs": the job ids; "all": every job of the queue.

Capability `EditProject`. Tool name `export_queue_resume`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | Every job (default false). |
| `jobs` | string[], min 1 |  | The jobs. |

```json
{"command":"export.queue.resume","params":{"jobs":["j1"]}}
```

<a id="export.queue.start-v1"></a>

### `export.queue.start` v1: Start Queue

Starts held jobs (Media Encoder's Start Queue). "jobs": the job ids; "all": every job of the queue.

Capability `EditProject`. Tool name `export_queue_start`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | Every job (default false). |
| `jobs` | string[], min 1 |  | The jobs. |

```json
{"command":"export.queue.start","params":{"jobs":["j1"]}}
```

<a id="export.quick-v1"></a>

### `export.quick` v1: Quick Export

Premiere's Quick Export: exports the sequence with one preset (default "match-source") to "output" (default: next to the project, named after the sequence) in the background, through the export queue. Returns the job and what it writes; export.queue.list follows it.

Capability `Read`. Tool name `export_quick`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `in` | integer \| string |  | Custom range start (with "out"). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `out` | integer \| string |  | Custom range end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `output` | string |  | The output file; its extension follows the format. |
| `overrides` | object |  | Settings on top of the preset (ExportController's keys, docs/export.md): container, videoCodec, encoder, quality, bitrateMbps, maxBitrateMbps, frameRate, width, height, scaleMode, range (whole \| inOut \| workArea \| custom), video, audio, audioCodec, audioChannels, sampleRate, audioBitrateKbps, dynamicRange, dynamicMetadata (none \| hdr10plus: HDR10+ on a PQ HEVC master), captions, pixelFormat, alpha (Include Alpha Channel), proresProfile, dnxhrProfile, exportAsSequence, subtitles, targetMB (a size limit in MB, 1-4000, for H.264, HEVC, AV1 or VP9: the whole file at most that size; 0 = off), and the objects effects, metadata and render (export.describe shows their fields). |
| `preset` | string |  | A preset id (export.presets: a built-in one such as "match-source", or a user preset "user-..."). |
| `range` | `whole` \| `inOut` \| `workArea` \| `custom` |  | Premiere's Range: Entire Source (default), Source In/Out, Work Area, Custom. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `stems` | `off` \| `role` |  | 0.6: "role" also writes the dialogue, music and effects stems (one audio-only 24-bit WAV per track role with an audible track, "_DX", "_MX", "_FX", "_OTHER" for tracks without a role) next to the export, queued after it, sample-aligned with it; never loudness-normalised. Default off. |
| `stemsMasterEffects` | boolean |  | 0.6, with "stems": the stems go through the master bus's effects (default true). |

```json
{"command":"export.quick","params":{"output":"C:/Exports/Episode 12.mp4","preset":"youtube-1080p60"}}
```

<a id="export.watchFolder.add-v1"></a>

### `export.watchFolder.add` v1: Add Watch Folder

Watches a folder: every media file dropped into it (once it stops growing) is exported with the preset into "output" (default "<folder>/Output"), through the export queue; with "moveSources" (default true) the source then moves into "<folder>/Source".

Capability `Read`. Tool name `export_watchFolder_add`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `folder` | string | yes | The folder to watch (created when missing). |
| `moveSources` | boolean |  | Move encoded sources into Source (default true). |
| `output` | string |  | Where the exports go. |
| `preset` | string | yes | A preset id (export.presets: a built-in one such as "match-source", or a user preset "user-..."). |

```json
{"command":"export.watchFolder.add","params":{"folder":"D:/Ingest/Drop here","preset":"youtube-1080p"}}
```

<a id="export.watchFolder.list-v1"></a>

### `export.watchFolder.list` v1: Watch Folders

Media Encoder's Watch Folders: each folder, its preset and output folder, whether sources move into "Source" once encoded, and how many files are queued, done and failed.

Capability `Read`. Tool name `export_watchFolder_list`.

No parameters.

```json
{"command":"export.watchFolder.list","params":{}}
```

<a id="export.watchFolder.remove-v1"></a>

### `export.watchFolder.remove` v1: Remove Watch Folder

Stops watching a folder (its queued exports go on).

Capability `EditProject`. Tool name `export_watchFolder_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `id` | string | yes | The watch folder's id (export.watchFolder.list). |

```json
{"command":"export.watchFolder.remove","params":{"id":"w1"}}
```

<a id="freeform.alignToGrid-v1"></a>

### `freeform.alignToGrid` v1: Align to Grid

Premiere's Align to Grid: the named items (all when none) snapped to the nearest grid cell of the view's clip size; items on one cell stack.

Capability `EditProject`. Tool name `freeform_alignToGrid`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin (absent: the root). A bin id. |
| `items` | object[] |  | The items to align. |
| `items[].id` | integer (1..) | yes | Its id. |
| `items[].kind` | `media` \| `sequence` \| `bin` | yes | What it is. |
| `items[].size` | number (0.25..4.0) |  | Its own size (0.25 .. 4, default 1). |
| `items[].x` | number (-1000000.0..1000000.0) |  | Left edge (view units). |
| `items[].y` | number (-1000000.0..1000000.0) |  | Top edge (view units). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"freeform.alignToGrid","params":{}}
```

<a id="freeform.get-v1"></a>

### `freeform.get` v1: Freeform View

A bin's Freeform view: its clip size, where each item is (back to front) and its saved layouts.

Capability `Read`. Tool name `freeform_get`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin (absent: the root). A bin id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"freeform.get","params":{}}
```

<a id="freeform.removeLayout-v1"></a>

### `freeform.removeLayout` v1: Delete Layout

Deletes the bin's saved Freeform layout "name".

Capability `EditProject`. Tool name `freeform_removeLayout`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin (absent: the root). A bin id. |
| `name` | string | yes | The layout's name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"freeform.removeLayout","params":{"name":"Storyboard"}}
```

<a id="freeform.resetToGrid-v1"></a>

### `freeform.resetToGrid` v1: Reset to Grid

Premiere's Reset to Grid: every item of the bin placed row by row in the chosen order ("by": name, label or type; bins, sequences and media keep their groups), "columns" per row, at size 1.

Capability `EditProject`. Tool name `freeform_resetToGrid`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin (absent: the root). A bin id. |
| `by` | `name` \| `label` \| `type` |  | The order (default name). |
| `columns` | integer (1..100) |  | Items per row (default 5). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"freeform.resetToGrid","params":{"by":"label","columns":4}}
```

<a id="freeform.restoreLayout-v1"></a>

### `freeform.restoreLayout` v1: Restore Layout

Arranges the bin's Freeform view as saved layout "name".

Capability `EditProject`. Tool name `freeform_restoreLayout`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin (absent: the root). A bin id. |
| `name` | string | yes | The layout's name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"freeform.restoreLayout","params":{"name":"Storyboard"}}
```

<a id="freeform.saveLayout-v1"></a>

### `freeform.saveLayout` v1: Save as New Layout

Saves the bin's Freeform arrangement under "name" (replacing a layout of that name).

Capability `EditProject`. Tool name `freeform_saveLayout`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin (absent: the root). A bin id. |
| `name` | string | yes | The layout's name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"freeform.saveLayout","params":{"name":"Storyboard"}}
```

<a id="freeform.set-v1"></a>

### `freeform.set` v1: Arrange in Freeform View

Places items of a bin in its Freeform view (Premiere's Freeform view: thumbnails anywhere, overlapping ones stacked; later items over earlier ones). "items" replaces the arrangement unless "merge" (default true) keeps the other items where they are, moving the named ones to the front. "scale" is the clip size.

Capability `EditProject`. Tool name `freeform_set`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin (absent: the root). A bin id. |
| `items` | object[] |  | Where the items go. |
| `items[].id` | integer (1..) | yes | Its id. |
| `items[].kind` | `media` \| `sequence` \| `bin` | yes | What it is. |
| `items[].size` | number (0.25..4.0) |  | Its own size (0.25 .. 4, default 1). |
| `items[].x` | number (-1000000.0..1000000.0) |  | Left edge (view units). |
| `items[].y` | number (-1000000.0..1000000.0) |  | Top edge (view units). |
| `merge` | boolean |  | Keep the items not named (default true). |
| `scale` | number (0.25..4.0) |  | The clip size (0.25 .. 4). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"freeform.set","params":{"items":[{"id":3,"kind":"media","x":40,"y":0}]}}
```

<a id="gen.broll-v1"></a>

### `gen.broll` v1: Generate broll

App: opens a human-reviewed Generate proposal with consent and cost preview; acceptance imports one staged edit. Standalone CLI/MCP refuses paid generation without an approval host.

Capability `EditProject`. Tool name `gen_broll`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `aspect` | `16:9` \| `9:16` | yes | 16:9 or 9:16 |
| `at` | integer (0..) |  | Placement in timeline ticks. |
| `duration` | number (0.1..10.0) | yes | Duration in seconds. |
| `prompt` | string | yes | What to generate. |
| `sequence` | integer (1..) |  | Sequence id. |

```json
{"command":"gen.broll","params":{"aspect":"16:9","duration":4,"prompt":"Aerial city"}}
```

<a id="gen.extend-v1"></a>

### `gen.extend` v1: Generate extend

App: opens a human-reviewed Generate proposal with consent and cost preview; acceptance imports one staged edit. Standalone CLI/MCP refuses paid generation without an approval host.

Capability `EditProject`. Tool name `gen_extend`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | Original clip id. |
| `prompt` | string |  | What to generate. |
| `seconds` | number (0.1..10.0) | yes | Extension seconds. |
| `sequence` | integer (1..) |  | Sequence id. |
| `side` | `head` \| `tail` | yes | head or tail |

```json
{"command":"gen.extend","params":{"clip":1,"seconds":4,"side":"tail"}}
```

<a id="gen.image-v1"></a>

### `gen.image` v1: Generate image

App: opens a human-reviewed Generate proposal with consent and cost preview; acceptance imports one staged edit. Standalone CLI/MCP refuses paid generation without an approval host.

Capability `EditProject`. Tool name `gen_image`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `prompt` | string | yes | What to generate. |
| `sequence` | integer (1..) |  | Sequence id. |
| `size` | `720p` | yes | Image output tier, currently 720p. |

```json
{"command":"gen.image","params":{"prompt":"Soft paper background","size":"720p"}}
```

<a id="gen.sfx-v1"></a>

### `gen.sfx` v1: Generate sfx

App: opens a human-reviewed Generate proposal with consent and cost preview; acceptance imports one staged edit. Standalone CLI/MCP refuses paid generation without an approval host.

Capability `EditProject`. Tool name `gen_sfx`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer (0..) |  | Placement in timeline ticks, on a new audio track. |
| `duration` | number (0.5..30.0) |  | Seconds (0.5 to 30); omit and the model picks the length. |
| `loop` | boolean |  | A seamless loop (ambience, engine hum). Default false. |
| `prompt` | string | yes | What to generate. |
| `sequence` | integer (1..) |  | Sequence id. |

```json
{"command":"gen.sfx","params":{"duration":4,"prompt":"Footsteps on gravel, slow"}}
```

<a id="graphic.add-v1"></a>

### `graphic.add` v1: New Graphic

Adds a motion graphic as a clip on a video track (overwriting what is there): "graphic" is the graphic's JSON (docs/graphics.md: name, width/height of its design canvas - default the sequence's frame - layers of text, shapes, images and groups, responsive time). Layer ids 0 get fresh ones. Add layers later with graphic.addLayer, animate them with graphic.applyPreset or keyframes in the layer JSON. createdIds: the clip, and the canvas media item when the project had none of this size.

Capability `EditSequence`. Tool name `graphic_add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string | yes | Where the graphic starts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `duration` | integer \| string |  | How long it lasts. Default 5 seconds. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `graphic` | object |  | The graphic (docs/graphics.md). Default: an empty one. |
| `name` | string |  | The graphic's name (also the clip's). |
| `placement` | `targeted` \| `freeTrackAtOrAbove` \| `newTrack` |  | "targeted": on "track" (or the targeted video track), overwriting what is there; the default when "track" is given. "freeTrackAtOrAbove": the first unlocked video track from "track" (or the targeted one) upwards with nothing in the graphic's time range, a new top track when none is free; the default without "track", so nothing under the playhead is overwritten. "newTrack": a new video track on top. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) |  | A video track. Default: the first targeted unlocked video track. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"graphic.add","params":{"at":"00:00:05:00","graphic":{"layers":[{"id":1,"kind":"text","text":{"runs":[{"size":64,"text":"Somchai"}]},"transform":{"x":160,"y":880}}]},"name":"Lower third","track":"V2"}}
```

<a id="graphic.addLayer-v1"></a>

### `graphic.addLayer` v1: Add Graphic Layer

Adds a layer (JSON, docs/graphics.md) to a graphic clip: on top of "parent" (a group layer; default the top level), or at "index" (0 = bottom). An id of 0 or one in use gets a fresh one; returns it as "layer".

Capability `EditSequence`. Tool name `graphic_addLayer`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `index` | integer (0..) |  | Where in the parent's list, 0 = bottom (default: on top). |
| `layer` | object | yes | The layer: {"kind": "text" \| "shape" \| "image" \| "group", ...}. |
| `parent` | integer (0..4294967295) |  | A group layer to add it to (default 0: the top level). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"graphic.addLayer","params":{"clip":42,"layer":{"kind":"shape","shape":{"cornerRadius":16,"height":120,"type":"rectangle","width":640},"transform":{"x":480,"y":900}}}}
```

<a id="graphic.align-v1"></a>

### `graphic.align` v1: Align Layers

Premiere's Essential Graphics Align and Distribute: lines layers up with the video frame ("to": "frame", the default) or with each other ("selection") - left, horizontal centre, right, top, vertical centre, bottom - or spaces three or more evenly (distributeHorizontal / distributeVertical: centres evenly spaced between the outermost two). Layers move by their Position (an animated one with all its keys), measured as drawn at "time" (default the clip's start).

Capability `EditSequence`. Tool name `graphic_align`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `align` | `left` \| `horizontalCenter` \| `right` \| `top` \| `verticalCenter` \| `bottom` \| `distributeHorizontal` \| `distributeVertical` | yes | What to do. |
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `layers` | (integer (1..))[], min 1 | yes | The layers. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | When to measure the layers (inside the clip). Default its start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `to` | `frame` \| `selection` |  | Align to the video frame (default) or the selection. |

```json
{"command":"graphic.align","params":{"align":"horizontalCenter","clip":42,"layers":[2,3]}}
```

<a id="graphic.applyPreset-v1"></a>

### `graphic.applyPreset` v1: Apply Animation Preset

Animates a layer with editable keyframes: fade, slide, scalePop, bounce (the transform), typewriter and wordByWord (a text-range animator: characters or words appear in turn), wipe (a mask). "in" animates the graphic's start, "out" its end, "loop" repeats over the whole clip. Keys inside the preset's time replace the ones there; others stay.

Capability `EditSequence`. Tool name `graphic_applyPreset`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `direction` | `left` \| `right` \| `up` \| `down` |  | Slide and wipe: where from (in), to (out) or along (loop). Default left. |
| `distance` | number (0.0..1000000.0) |  | Slide and bounce: how far, pixels (default a quarter of the canvas). |
| `duration` | integer \| string |  | How long it takes (one cycle of a loop). Default half a second. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `easing` | `linear` \| `easeIn` \| `easeOut` \| `easeInOut` \| `overshoot` |  | Default easeOut. |
| `layer` | integer (1..) | yes | The layer. A layer id. |
| `overshoot` | number (0.0..100.0) |  | Scale pop, bounce and overshoot easing: how far past, % (default 10). |
| `phase` | `in` \| `out` \| `loop` |  | In (default), out or loop. |
| `preset` | `fade` \| `slide` \| `scalePop` \| `typewriter` \| `wordByWord` \| `wipe` \| `bounce` | yes | The preset. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"graphic.applyPreset","params":{"clip":42,"duration":"00:00:01:00","layer":2,"preset":"typewriter"}}
```

<a id="graphic.checkSpelling-v1"></a>

### `graphic.checkSpelling` v1: Check Spelling in Graphic

Spell check for a graphic clip's text: the misspelled words of every text layer (groups included, bottom to top), each in its layer's language (or "language"), from the host's spell checker and the user's dictionary - the layer, the word's byte range in the layer's text, the word, the checker's advice and up to 8 suggestions, as captions.checkSpelling lists them. Corrections are graphic.setText edits.

Capability `Read`. Tool name `graphic_checkSpelling`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `language` | string |  | BCP 47 or ISO 639-1. Default: each layer's, else the user's. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `suggestions` | boolean |  | List suggestions. Default true. |

```json
{"command":"graphic.checkSpelling","params":{"clip":42}}
```

<a id="graphic.get-v1"></a>

### `graphic.get` v1: Graphic

A graphic clip's graphic as JSON (docs/graphics.md), with its layers listed flat (bottom to top, groups before their children) and, for a template instance, its template and field values.

Capability `Read`. Tool name `graphic_get`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"graphic.get","params":{"clip":42}}
```

<a id="graphic.removeLayer-v1"></a>

### `graphic.removeLayer` v1: Delete Graphic Layer

Removes a layer from a graphic clip (a group with its children); pins to it and template fields bound to it go too.

Capability `EditSequence`. Tool name `graphic_removeLayer`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `layer` | integer (1..) | yes | The layer. A layer id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"graphic.removeLayer","params":{"clip":42,"layer":3}}
```

<a id="graphic.resolve-v1"></a>

### `graphic.resolve` v1: Graphic Draw List

The draw list of a graphic clip at a timeline time (default its start): every visible layer bottom to top with its transform to canvas pixels [a, b, c, d, e, f], opacity, blend mode, shadow, masks, and its path and paints, text runs (and per-character animator values) or image - with the brand kit applied. What the renderer draws (docs/graphics.md).

Capability `Read`. Tool name `graphic_resolve`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | A timeline time (default the clip's start). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"graphic.resolve","params":{"clip":42,"time":"00:00:06:00"}}
```

<a id="graphic.setRoll-v1"></a>

### `graphic.setRoll` v1: Roll/Crawl Options

Makes a graphic clip roll up (credits: Premiere's Essential Graphics Roll) or crawl left or right (a ticker: the Legacy Titler's Crawl Left / Crawl Right) over the clip, so the clip's length sets the speed; "roll": null stops it. Start Off Screen / End Off Screen (default on), Preroll and Postroll (held at the start / end), Ease-In and Ease-Out (speeding up / slowing down) as Premiere has them. The graphic is designed in place: layers below the frame are the rest of the roll.

Capability `EditSequence`. Tool name `graphic_setRoll`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `roll` | object \| null | yes | The options, or null for none. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"graphic.setRoll","params":{"clip":42,"roll":{"easeOut":"00:00:00:12","kind":"roll","postroll":"00:00:01:00"}}}
```

<a id="graphic.setTextPath-v1"></a>

### `graphic.setTextPath` v1: Type on a Path

Puts a text layer on a path (the Legacy Titler's Path Type tool, After Effects' Path Options): "shape" line, arc (the top of an ellipse: text over a rainbow), circle (clockwise from its left: text round the outside) or wave, "width" and "height" in pixels, centred on the layer's origin - or custom "vertices" ("closed" for a closed path); "none" puts it back on straight lines. Options: "reverse" (Reverse Path), "perpendicular" (characters turn with the path, default true), "forceAlignment" (spread from the first margin to the last), "firstMargin" / "lastMargin" (pixels; keyframe the first margin with graphic.updateLayer to send the text along the path). Without "shape" only the options change. The text's alignment places it between the margins. One undo step.

Capability `EditSequence`. Tool name `graphic_setTextPath`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `closed` | boolean |  | custom: a closed path. Default false. |
| `firstMargin` | number (-1000000.0..1000000.0) |  | First Margin, pixels. |
| `forceAlignment` | boolean |  | Force Alignment. |
| `height` | number (1.0..1000000.0) |  | The arc's or wave's height, pixels. Default 300 (wave 120). |
| `lastMargin` | number (-1000000.0..1000000.0) |  | Last Margin, pixels. |
| `layer` | integer (1..) | yes | The text layer. A layer id. |
| `perpendicular` | boolean |  | Perpendicular To Path. |
| `reverse` | boolean |  | Reverse Path. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shape` | `none` \| `line` \| `arc` \| `circle` \| `wave` \| `custom` |  | The path. Default: keep the layer's (arc when it has none). |
| `vertices` | ((number (-1000000.0..1000000.0))[], min 2)[], min 2 |  | custom: the path's vertices in the layer's pixels. |
| `width` | number (1.0..1000000.0) |  | The shape's width, pixels. Default 800 (a circle: its diameter). |

```json
{"command":"graphic.setTextPath","params":{"clip":42,"height":320,"layer":2,"shape":"arc","width":900}}
```

<a id="graphic.update-v1"></a>

### `graphic.update` v1: Edit Graphic

Changes a graphic clip's graphic with a JSON merge patch (RFC 7386) over its JSON (graphic.get): objects merge, null resets a key to its default, arrays ("layers") are replaced whole. For one layer use graphic.updateLayer.

Capability `EditSequence`. Tool name `graphic_update`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `set` | object | yes | The merge patch, e.g. {"name": "Title", "responsive": {...}}. |

```json
{"command":"graphic.update","params":{"clip":42,"set":{"responsive":{"duration":1270080000000,"intro":254016000000,"outro":254016000000}}}}
```

<a id="graphic.updateLayer-v1"></a>

### `graphic.updateLayer` v1: Edit Graphic Layer

Changes a layer of a graphic clip with a JSON merge patch (RFC 7386) over the layer's JSON: {"text": {"runs": [{"text": "New"}]}} replaces the runs, {"transform": {"x": 200}} moves it, a curve ({"keys": [{"t": ticks, "v": value}, ...]} or just the array of keys) animates it. Its id stays.

Capability `EditSequence`. Tool name `graphic_updateLayer`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `layer` | integer (1..) | yes | The layer. A layer id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `set` | object | yes | The merge patch. |

```json
{"command":"graphic.updateLayer","params":{"clip":42,"layer":2,"set":{"transform":{"opacity":80}}}}
```

<a id="highlights.find-v1"></a>

### `highlights.find` v1: Find Highlights

Finds the best moments of a gaming recording on this computer: game-audio spikes, the player's voice (louder or higher than usual, shouting, laughter), the picture (fast action, flashes, bursts of picture changes), the recording's imported Twitch / YouTube chat log (busy chat, emotes; highlights.importChat) and OBS chapter markers or a marker file next to the recording. Each moment gets a score, a confidence and the reasons, and a marker ("Highlight 1: ..."; earlier highlight markers are replaced). For a media item the moments are in media time and become clip markers. Measurements are cached per source file.

Capability `EditSequence`. Tool name `highlights_find`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audioRoles` | (`auto` \| `game` \| `mic` \| `mixed` \| `ignore`)[] |  | What each audio stream of the recordings carries, by stream (OBS track 1, 2, ...): "auto" keeps the stream's own role. Default: with several streams, a stream titled like OBS's Mic/Aux is the mic, Game/Desktop Audio and Alerts are game, Discord and Music are left out; streams without such a title: the most voice-like is the mic (unless a title named it), the others game. A single stream is mixed. |
| `chatDelay` | number (0.0..120.0) |  | How late chat reacts, seconds (default 6). |
| `chatOffset` | number (-86400.0..86400.0) |  | Chat time + this = recording time, seconds (default: as imported). |
| `count` | integer (1..200) |  | How many moments at most. Default 10. |
| `keywords` | string[] |  | Words and emotes that count as reactions, besides the usual ones. |
| `markers` | boolean |  | Mark the moments. Default true. |
| `media` | integer \| string (1..) |  | Analyse this media item instead of the sequence. A media id or exact name. |
| `minGap` | number (0.0..3600.0) |  | Seconds between two moments' peaks. Default 20. |
| `minScore` | number (0.0..1.0) |  | Score a moment needs (0..1). Default 0.25. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `signals` | (`game` \| `mic` \| `visual` \| `chat` \| `markers`)[] |  | The signals to use. Default all ("visual" decodes the picture: the slow part). |

```json
{"command":"highlights.find","params":{"count":8,"signals":["game","mic","visual"]}}
```

<a id="highlights.importChat-v1"></a>

### `highlights.importChat` v1: Import Chat Log

Reads a stream's chat log (TwitchDownloader JSON, yt-dlp live_chat.json or chat-downloader JSON) and keeps it with a recording's analyses, so highlights.find counts busy chat and emotes. Nothing is uploaded.

Capability `MediaAnalysis`. Tool name `highlights_importChat`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `file` | string | yes | The chat log file. |
| `media` | integer \| string (1..) |  | The recording it belongs to (default: the longest one the sequence plays). A media id or exact name. |
| `offset` | number (-86400.0..86400.0) |  | Chat time + this = recording time, seconds. Default 0. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"highlights.importChat","params":{"file":"C:/Users/me/Videos/stream-chat.json"}}
```

<a id="highlights.toReel-v1"></a>

### `highlights.toReel` v1: Make Highlight Reel

Makes a new sequence from highlight moments (the sequence's highlight markers, or "moments"): each moment with handles, best first up to "count" / "maxDuration", in time or score order, with transitions between them. With "music" the cuts land on its beats (every "beatsPerCut" beats) and the music plays under the reel; "slowMotion" slows the moment's peak. One undo step.

Capability `EditProject`. Tool name `highlights_toReel`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `beatsPerCut` | integer (1..64) |  | Cuts every this many beats (or a multiple). Default 4. |
| `count` | integer (1..50) |  | How many moments at most (best first). Default all, at most 50. |
| `handleAfter` | number (0.0..30.0) |  | Seconds added after each moment. Default 0.5. |
| `handleBefore` | number (0.0..30.0) |  | Seconds added before each moment. Default 0.5. |
| `maxDuration` | number (1.0..3600.0) |  | Longest reel, seconds (best moments first). |
| `moments` | object[] |  | Moments in sequence seconds (default: the highlight markers). |
| `moments[].end` | number (0.0..86400.0) | yes | Seconds. |
| `moments[].peak` | number (0.0..86400.0) |  | Seconds. |
| `moments[].score` | number (0.0..1.0) |  | 0..1 |
| `moments[].start` | number (0.0..86400.0) | yes | Seconds. |
| `moments[].title` | string |  | Label. |
| `music` | integer \| string (1..) |  | Music to cut to: the cuts land on its beats and it plays under the reel. A media id or exact name. |
| `musicStart` | number (0.0..86400.0) |  | Where the music starts, seconds into it. Default 0. |
| `name` | string |  | Name of the new sequence. Default "<sequence> - Highlights". |
| `order` | `time` \| `score` |  | Order of the moments in the reel. Default time. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `slowMotion` | boolean |  | Slow down each moment's peak. Default false. |
| `slowMotionSeconds` | number (0.1..10.0) |  | Seconds of footage slowed. Default 1.5. |
| `slowMotionSpeed` | number (0.1..1.0) |  | Speed of the slow part (0.1..1). Default 0.5. |
| `transition` | `crossDissolve` \| `dipToBlack` \| `dipToWhite` \| `none` |  | Transition between moments (audio crossfades). Default crossDissolve. |
| `transitionDuration` | number (0.0..5.0) |  | Seconds. Default 0.5. |

```json
{"command":"highlights.toReel","params":{"count":8,"order":"time","transition":"crossDissolve"}}
```

<a id="highlights.toShort-v1"></a>

### `highlights.toShort` v1: Make Highlight Short

Makes one highlight moment (a highlight marker by rank, or start/end) a vertical 1080x1920 Short: the moment cut out with every track, laid out with layout.apply ("verticalSplit" with the facecam under the gameplay when there is one: a facecam clip above the gameplay or "facecamRegion"; else "verticalBlurFill": the whole game view over a blurred copy), captioned (the "Shorts" caption style unless "captionPreset" says otherwise), with the moment's title as the hook and loudness for Shorts. One undo step.

Capability `EditProject`. Tool name `highlights_toShort`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `blur` | number (0.0..500.0) |  | verticalBlurFill: the background's blur radius. Default 40. |
| `captionPreset` | string |  | The caption style: a built-in preset (Shorts, Clean, Bold Box, Podcast, Thai-friendly) or one of the project's. Default "Shorts" for vertical output, none (the default look) otherwise; "" = the default look. |
| `captions` | boolean |  | Captions from the transcript. Default true. |
| `end` | number (0.0..86400.0) |  | Seconds. |
| `facecam` | integer \| object |  | The facecam clip in the sequence (default: a clip of other media above the gameplay). |
| `facecamOnTop` | boolean |  | verticalSplit: the facecam on top. |
| `facecamRegion` | object |  |  |
| `facecamRegion.height` | number (0.01..1.0) | yes | A fraction of the frame height. |
| `facecamRegion.width` | number (0.01..1.0) | yes | A fraction of the frame width. |
| `facecamRegion.x` | number (0.0..1.0) | yes | Left, a fraction of the frame width. |
| `facecamRegion.y` | number (0.0..1.0) | yes | Top, a fraction of the frame height. |
| `handleAfter` | number (0.0..30.0) |  | Seconds added after. Default 0. |
| `handleBefore` | number (0.0..30.0) |  | Seconds added before. Default 0. |
| `hookTitle` | boolean |  | Show the title at the start. Default true. |
| `layout` | `auto` \| `verticalSplit` \| `verticalBlurFill` |  | The layout (layout.apply). Default auto: verticalSplit with a facecam, else verticalBlurFill. |
| `moment` | integer (1..200) |  | Rank of the highlight marker. Default 1 (the best). |
| `normalize` | boolean |  | Loudness for Shorts. Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `split` | number (0.2..0.8) |  | verticalSplit: the gameplay's share of the height. Default 0.6. |
| `start` | number (0.0..86400.0) |  | Seconds. |
| `title` | string |  | Hook title (default: the moment's title). |
| `zoom` | number (1.0..4.0) |  | verticalBlurFill: the gameplay's scale over fit-to-width. Default 1. |

```json
{"command":"highlights.toShort","params":{"moment":1}}
```

<a id="history.checkpoint-v1"></a>

### `history.checkpoint` v1: Save Version

Records the project as it is now in the version history (unchanged since the last version: nothing is added). Give "label" to name it ("Client cut v2"): named versions are never pruned. Hosts call it on save, every few minutes of editing and before an AI agent applies a plan (kind).

Capability `EditProject`. Tool name `history_checkpoint`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `kind` | `save` \| `auto` \| `agent` \| `manual` \| `restore` |  | What made it (default manual). |
| `label` | string |  | A name for the version. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"history.checkpoint","params":{"label":"Client cut v2"}}
```

<a id="history.diff-v1"></a>

### `history.diff` v1: Compare Versions

What changed from one version to another ("current" is the project as it is): per sequence the clips added, removed, moved, trimmed or retimed, effects and keyframes changed, settings, tracks, markers and captions; media and review notes. "summary" says it in lines with timecodes.

Capability `Read`. Tool name `history_diff`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | integer \| string | yes | A version id, or "current". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | A version id, or "current" (default). |

```json
{"command":"history.diff","params":{"from":12,"to":"current"}}
```

<a id="history.export-v1"></a>

### `history.export` v1: Save Version As

Writes a saved version as a project file of its own ("restore as a new project"). Refuses to replace a file unless "overwrite".

Capability `EditProject`. Tool name `history_export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `overwrite` | boolean |  | Replace an existing file (default false). |
| `path` | string | yes | The new project file (UTF-8 path). |
| `revision` | integer (1..) | yes | The version's id (history.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"history.export","params":{"path":"C:/Projects/Film (client cut v2).wabcut","revision":12}}
```

<a id="history.list-v1"></a>

### `history.list` v1: Version History

The project's saved versions, oldest first, as arrays: id, createdAt (UTC), kind (save, auto, agent, manual, restore), label (named versions are kept), sequences, clips, the bytes the version added.

Capability `Read`. Tool name `history_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"history.list","params":{}}
```

<a id="history.name-v1"></a>

### `history.name` v1: Name Version

Names a saved version (an empty label unnames it). Named versions are kept when the history prunes.

Capability `EditProject`. Tool name `history_name`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `label` | string | yes | The name. |
| `revision` | integer (1..) | yes | The version's id (history.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"history.name","params":{"label":"Client cut v2","revision":12}}
```

<a id="history.restore-v1"></a>

### `history.restore` v1: Restore Version

Goes back to a saved version: the whole project (one undo step; the project as it was is saved as a version first), or, with "sequence", that sequence of the version as a new sequence next to the current ones (the media it plays is the project's own where the project still has it). The no-cloud lock stays on.

Capability `EditProject`. Tool name `history_restore`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | integer (1..) |  | Restore only this sequence of the version, as a new sequence. |
| `name` | string |  | The new sequence's name (default: "<name> (version <id>)"). |
| `revision` | integer (1..) | yes | The version's id (history.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"history.restore","params":{"revision":12}}
```

<a id="interchange.export-v1"></a>

### `interchange.export` v1: Export Timeline

Writes the sequence (with the sequences it nests and its multicam sources) as a timeline file other editors read, at "path": Final Cut Pro 7 XML ("fcp7-xml": Premiere Pro, DaVinci Resolve), FCPXML ("fcpxml": Final Cut Pro, Resolve; "fcpxmlVersion" 1.10 or 1.11), OpenTimelineIO ("otio") or a CMX3600 EDL ("edl"); or for audio post as an AAF ("aaf": Pro Tools, Nuendo, Resolve / Fairlight, Media Composer) or an OMF 2.0 file ("omf") with its audio rendered to new files ("audioPost": Premiere's AAF and OMF settings). Returns the export report: how many elements were carried exactly, and every one approximated (rounded to frames, a transition the format has no name for, ...) or dropped (graphics and effects the format cannot describe). None of these formats carries caption tracks: each caption track with cues is reported as dropped, unless "captionSidecars" names a caption file format - then each is written next to the file as "<name> - <track name>[.<language>].<ext>" (words the soundtrack bleeps replaced, as captions.export does), reported as carried, and listed in "captionFiles". The project does not change; a dry run writes nothing.

Capability `Read`. Tool name `interchange_export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audioPost` | object |  | AAF and OMF only. |
| `audioPost.audioFormat` | `wav` \| `aiff` |  | Broadcast WAV or AIFF. Default wav. |
| `audioPost.bitsPerSample` | integer (16..24) |  | 16 or 24. Default 24. |
| `audioPost.breakoutToMono` | boolean |  | AAF: render new mono audio files, stereo tracks split into Left and Right tracks (Breakout to Mono). Off: the clips link to their original files. |
| `audioPost.files` | `embed` \| `separate` |  | The audio inside the file (Embed Audio / Encapsulate) or in a folder next to it (Separate Audio). Default embed. |
| `audioPost.handleFrames` | integer (0..1000) |  | Trim: handle frames before and after each clip. Default 24. |
| `audioPost.includeClipCopiesWithoutEffects` | boolean |  | With renderAudioClipEffects: clean copies on "No FX" tracks. |
| `audioPost.includePan` | boolean |  | OMF: write the tracks' pan. |
| `audioPost.mixdownVideo` | boolean |  | AAF: render the video to one reference file next to the AAF (Mixdown Video). |
| `audioPost.omfTitle` | string |  | OMF: the composition's name. Default the sequence's. |
| `audioPost.render` | `trim` \| `complete` |  | Trim Audio Files (the parts used, plus handles) or Copy Complete Audio Files. Default trim. |
| `audioPost.renderAudioClipEffects` | boolean |  | Render the clips' audio effects into the new files. |
| `audioPost.sampleRate` | integer (48000..96000) |  | 48000 or 96000 (Hz). Default 48000. |
| `captionSidecars` | `srt` \| `vtt` \| `scc` \| `mcc` \| `stl` \| `ttml` \| `dfxp` \| `smpte-tt` \| `itt` |  | 0.6: also write every caption track with cues next to the file in this caption format (default: none). |
| `fcpxmlVersion` | `1.10` \| `1.11` |  | FCPXML only (default 1.10). |
| `format` | `fcp7-xml` \| `fcpxml` \| `otio` \| `edl` \| `aaf` \| `omf` | yes | The file format. |
| `path` | string | yes | The file to write (UTF-8 path); an existing one is replaced. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"interchange.export","params":{"audioPost":{"files":"separate"},"format":"aaf","path":"C:/Projects/Film (for the mix).aaf"}}
```

<a id="keyframes.ease-v1"></a>

### `keyframes.ease` v1: Ease Keyframes

Shapes the segment after each keyframe (0.7, the Graph Editor's easing library) like an easing preset: the Penner families (sine, quad, cubic, quart, quint, expo, circ, back), the web's ease keywords, fastOutSlowIn, snap and linear, each one unit-square cubic Bezier. Both keys of a segment become Bezier with the preset's handles; a key that was not Bezier keeps the shape of its other side, so only the eased segment changes, and a Hold key stays a step. The last key has no segment after it. Returns how many segments were shaped.

Capability `EditSequence`. Tool name `keyframes_ease`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `keys` | (integer (0..))[] |  | The keys whose following segment is eased. Default: all of them. |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `preset` | `linear` \| `ease` \| `easeIn` \| `easeOut` \| `easeInOut` \| `easeInSine` \| `easeOutSine` \| `easeInOutSine` \| `easeInQuad` \| `easeOutQuad` \| `easeInOutQuad` \| `easeInCubic` \| `easeOutCubic` \| `easeInOutCubic` \| `easeInQuart` \| `easeOutQuart` \| `easeInOutQuart` \| `easeInQuint` \| `easeOutQuint` \| `easeInOutQuint` \| `easeInExpo` \| `easeOutExpo` \| `easeInOutExpo` \| `easeInCirc` \| `easeOutCirc` \| `easeInOutCirc` \| `easeInBack` \| `easeOutBack` \| `easeInOutBack` \| `fastOutSlowIn` \| `slowOutFastIn` \| `snap` | yes | The easing preset. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"keyframes.ease","params":{"clip":42,"param":"motion.scale","preset":"easeInOutCubic"}}
```

<a id="keyframes.move-v1"></a>

### `keyframes.move` v1: Move Keyframes

Moves several keyframes of one parameter together by a timeline distance, keeping their spacing. The distance is clamped so the keys stay within the clip (keys already outside it do not move further out); a moved key landing on another key replaces it. Returns the moved keys' new indices.

Capability `EditSequence`. Tool name `keyframes_move`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `delta` | integer \| string | yes | How far they move; positive = later. Signed timecode ("-00:00:00:05") or ticks. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `keys` | (integer (0..))[] |  | The keys to move. Default: all of them. |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"keyframes.move","params":{"clip":42,"delta":"00:00:00:12","keys":[1,2],"param":"motion.scale"}}
```

<a id="keyframes.setInterpolation-v1"></a>

### `keyframes.setInterpolation` v1: Set Keyframe Interpolation

Sets the interpolation (hold, linear, bezier, easeIn, easeOut, easeInOut) of several keyframes of one parameter at once.

Capability `EditSequence`. Tool name `keyframes_setInterpolation`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | For an effect parameter: The effect instance id (clip.details lists them). |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` | yes | The new interpolation. |
| `keys` | (integer (0..))[] |  | The keys. Default: all of them. |
| `param` | string | yes | The parameter: a clip parameter (motion.position.x, motion.position.y, motion.scale, motion.rotation, motion.anchor.x, motion.anchor.y, opacity.opacity, volume.level, panner.balance, motion.scaleWidth, motion.uniformScale), or with "effect" a parameter key of that effect (see effect.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"keyframes.setInterpolation","params":{"clip":42,"interpolation":"easeInOut","param":"motion.scale"}}
```

<a id="layout.apply-v1"></a>

### `layout.apply` v1: Apply Layout

Lays out a gameplay clip and a facecam in one step, with plain editable clip settings: "pip" (gameplay fills the frame, the facecam in a corner with rounded corners or a circle, a border and a drop shadow), "verticalSplit" (9:16: gameplay on top, facecam below, or the other way round) or "verticalBlurFill" (9:16: the gameplay fitted to the width over a blurred copy of itself filling the frame, an optional facecam on top). The facecam is a clip on a track above the gameplay's, or "facecamRegion": the part of the capture where the webcam already shows (a copy of the gameplay cropped to it). Copies of the gameplay go on free tracks above it (new tracks when none is free) and are linked with it. Applying again replaces the layout's own effects. One undo step. Make a Shorts sequence first with sequence.create preset "shorts".

Capability `EditSequence`. Tool name `layout_apply`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `blur` | number (0.0..500.0) |  | verticalBlurFill: the background's blur radius in pixels. Default 40. |
| `borderColor` | (number (0.0..1.0))[], min 3 |  | Border colour [red, green, blue]. Default white. |
| `borderWidth` | number (0.0..100.0) |  | Border width in sequence pixels (0 = none). Default 4. |
| `circle` | boolean |  | A round facecam. |
| `corner` | `topLeft` \| `topRight` \| `bottomLeft` \| `bottomRight` |  | Where the facecam box goes. Default bottomRight. |
| `cornerRadius` | number (0.0..0.5) |  | Rounded corners: a fraction of the box's shorter side. Default 0.12. |
| `facecam` | integer \| object (1..) |  | The facecam clip (video, on a track above the gameplay's). A clip id, or {"track", "time"} for the clip there. |
| `facecamOnTop` | boolean |  | verticalSplit: the facecam on top. |
| `facecamRegion` | object |  | Where the capture already shows the webcam, as a part of the gameplay frame (fractions). The facecam is then a copy of the gameplay cropped to it; no facecam clip is needed. |
| `facecamRegion.height` | number (0.01..1.0) | yes | Height, a fraction of the frame's height. |
| `facecamRegion.width` | number (0.01..1.0) | yes | Width, a fraction of the frame's width. |
| `facecamRegion.x` | number (0.0..1.0) | yes | Left edge, a fraction of the gameplay frame's width. |
| `facecamRegion.y` | number (0.0..1.0) | yes | Top edge, a fraction of its height. |
| `gameplay` | integer \| object (1..) | yes | The gameplay clip (video). A clip id, or {"track", "time"} for the clip there. |
| `margin` | number (0.0..0.4) |  | Gap to the frame edges, a fraction of the frame height. Default 0.03. |
| `preset` | `pip` \| `verticalSplit` \| `verticalBlurFill` | yes | The layout. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shadow` | boolean |  | A drop shadow under the facecam. |
| `size` | number (0.05..1.0) |  | The facecam's width, a fraction of the frame width. Default 0.28. |
| `split` | number (0.2..0.8) |  | verticalSplit: the gameplay's share of the height. Default 0.6. |
| `zoom` | number (1.0..4.0) |  | verticalBlurFill: the gameplay's scale over fit-to-width (above 1 crops its sides). Default 1. |

```json
{"command":"layout.apply","params":{"corner":"bottomLeft","facecam":43,"gameplay":42,"preset":"pip","size":0.3}}
```

<a id="layout.list-v1"></a>

### `layout.list` v1: List Layouts

The layout presets layout.apply takes and their options with defaults: facecam picture-in-picture and the vertical 9:16 shorts layouts.

Capability `Read`. Tool name `layout_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"layout.list","params":{}}
```

<a id="llm.local.models-v1"></a>

### `llm.local.models` v1: Local assistant models

Inspect the managed local assistant and its installable models; never downloads implicitly.

Capability `Read`. Tool name `llm_local_models`.

No parameters.

```json
{"command":"llm.local.models","params":{}}
```

<a id="llm.local.start-v1"></a>

### `llm.local.start` v1: Local assistant start

Start or stop the managed local assistant (an installed, allow-listed model); never downloads implicitly.

Capability `MediaAnalysis`. Tool name `llm_local_start`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `model` | string |  | Installed allow-listed model id. |

```json
{"command":"llm.local.start","params":{}}
```

<a id="llm.local.status-v1"></a>

### `llm.local.status` v1: Local assistant status

Inspect the managed local assistant and its installable models; never downloads implicitly.

Capability `Read`. Tool name `llm_local_status`.

No parameters.

```json
{"command":"llm.local.status","params":{}}
```

<a id="llm.local.stop-v1"></a>

### `llm.local.stop` v1: Local assistant stop

Start or stop the managed local assistant (an installed, allow-listed model); never downloads implicitly.

Capability `MediaAnalysis`. Tool name `llm_local_stop`.

No parameters.

```json
{"command":"llm.local.stop","params":{}}
```

<a id="lottie.import-v1"></a>

### `lottie.import` v1: Import Lottie

Imports a Lottie animation (Bodymovin JSON from After Effects, LottieFiles; or a dotLottie .lottie archive) as a motion graphic: from a file ("path") or the JSON itself ("data"). With "at" it becomes a clip there; with "saveAs" a template in the project's library (both are allowed). Its texts and the colors that stay the same are template fields (named after their layers): a clip placed with "at" is an instance of the template, so Essential Graphics edits and keys them (template.setField, template.setFieldKey). Shapes, fills, strokes, gradients, trims, transforms with their easing, text, masks and precomps come in; the report lists what was approximated or dropped (images, effects, mattes, expressions, dotLottie themes and state machines...). A dotLottie archive holding several animations imports its initial one, or the one named by "animation".

Capability `EditProject`. Tool name `lottie_import`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `animation` | string |  | A dotLottie archive's animation id (default: its initial one). |
| `at` | integer \| string |  | Place it as a clip starting here. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `data` | string |  | The Lottie JSON itself. |
| `duration` | integer \| string |  | The clip's length (default the animation's). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `path` | string |  | The .json or .lottie file (UTF-8 path). |
| `placement` | `targeted` \| `freeTrackAtOrAbove` \| `newTrack` |  | "targeted": on "track" (or the targeted video track), overwriting what is there; the default when "track" is given. "freeTrackAtOrAbove": the first unlocked video track from "track" (or the targeted one) upwards with nothing in the graphic's time range, a new top track when none is free; the default without "track", so nothing under the playhead is overwritten. "newTrack": a new video track on top. |
| `saveAs` | object |  |  |
| `saveAs.description` | string |  | Description. |
| `saveAs.id` | string | yes | Template id. |
| `saveAs.name` | string | yes | Template name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) |  | A video track. Default: the first targeted unlocked video track. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"lottie.import","params":{"at":"00:00:03:00","path":"D:/Motion/confetti.json","track":"V3"}}
```

<a id="lut.export-v1"></a>

### `lut.export` v1: Export .cube

Writes a clip's colour grade as a 3D LUT .cube file (Premiere's Lumetri panel > Export .cube): "size" points per side (default 33, as Premiere), display-referred Rec.709 in and out, evaluated at "time" (default the clip's middle). Every pixel-local part of the grade goes in - input LUT, Basic, Creative, Curves, Color Wheels, the HSL secondary through its qualifier; the vignette, the secondary's denoise / blur / sharpen and masks cannot and are listed in "leftOut". Outputs are clamped to 0..1. Refuses to replace a file unless "overwrite"; a dry run writes nothing.

Capability `Read`. Tool name `lut_export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | The grade's effect instance id when the clip has several (clip.details lists them). Default: its first grade. |
| `overwrite` | boolean |  | Replace an existing file. Default false. |
| `path` | string | yes | The .cube file to write. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `size` | integer (2..65) |  | Lattice points per side, 2..65. Default 33. |
| `time` | integer \| string |  | When (inside the clip). Default its middle. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `title` | string |  | The TITLE line. Default the clip's name. |

```json
{"command":"lut.export","params":{"clip":42,"path":"C:/grades/shot42.cube"}}
```

<a id="lut.import-v1"></a>

### `lut.import` v1: Import LUT

Adds a .cube LUT file (1D or 3D, up to 65 points per side) to the project as an asset: it relinks and is collected like media. Returns its reference for color.grade.set's inputLut / look. Importing the same file again returns the existing asset. "folder" ('/'-separated names, e.g. "Film Emulation/Kodak") lists it under that group in the Color panel's LUT menu; given for a LUT already in the project it moves the LUT there.

Capability `EditProject`. Tool name `lut_import`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `group` | string |  | The LUT menu group, '/'-separated names. Default none (listed with the built-ins). |
| `name` | string |  | Display name. Default the file name. |
| `path` | string | yes | The .cube file. |

```json
{"command":"lut.import","params":{"path":"C:/LUTs/FilmLook.cube"}}
```

<a id="lut.importFolder-v1"></a>

### `lut.importFolder` v1: Import LUT Folder

Adds every .cube LUT in a folder (and, unless "recursive" is false, its sub-folders up to 8 levels; hidden folders and more than 500 files are left out) to the project as assets and groups them in the Color panel's LUT menu by folder: the group is "folder" (default the folder's own name) with the sub-folder names below it. Only the head of each file is checked, so a large folder imports at once; a file that cannot be read shows when it is used. A LUT already in the project is not added again and is listed under its new group. One undo step. Returns the counts and the assets' ids.

Capability `EditProject`. Tool name `lut_importFolder`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `group` | string |  | The top group of the LUT menu, '/'-separated names. Default the folder's name. |
| `path` | string | yes | The folder. |
| `recursive` | boolean |  | Also take the sub-folders' LUTs. Default true. |

```json
{"command":"lut.importFolder","params":{"path":"C:/LUTs/Film Emulation"}}
```

<a id="lut.list-v1"></a>

### `lut.list` v1: List LUTs

The built-in looks and camera input LUTs (with where their math comes from) and the project's LUT assets (path, offline, how many effects use each).

Capability `Read`. Tool name `lut_list`.

No parameters.

```json
{"command":"lut.list","params":{}}
```

<a id="marker.add-v1"></a>

### `marker.add` v1: Add Marker

Adds a sequence marker. Premiere's marker colours are 0-7: green, red, purple, orange, yellow, white, blue, cyan.

Capability `EditSequence`. Tool name `marker_add`. Aliases: `markers.add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `colorIndex` | integer (0..15) |  | Marker colour index. |
| `comment` | string |  | Marker comment. |
| `duration` | integer \| string |  | Length for a range marker. Default 0 (a point marker). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `highlight` | object \| null |  | C.15: a highlight finder's verdict on the marked moment (the Highlights panel reads it). null removes it. |
| `name` | string |  | Marker name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | string |  | Web link frame target. |
| `time` | integer \| string | yes | Where the marker starts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `type` | `comment` \| `chapter` \| `segmentation` \| `webLink` \| `source` |  | Premiere's marker type (0.7: "source" marks a citation, see sources.add). Default comment. |
| `url` | string |  | Web link URL (http or https). |

```json
{"command":"marker.add","params":{"name":"Chapter 2","time":"00:01:00:00","type":"chapter"}}
```

<a id="marker.clearAll-v1"></a>

### `marker.clearAll` v1: Clear All Markers

Removes every marker of the sequence (Premiere's Clear All Markers).

Capability `EditSequence`. Tool name `marker_clearAll`. Aliases: `markers.clearAll`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"marker.clearAll","params":{}}
```

<a id="marker.clearInAndOut-v1"></a>

### `marker.clearInAndOut` v1: Clear In and Out

Clears the sequence In and Out marks.

Capability `EditSequence`. Tool name `marker_clearInAndOut`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"marker.clearInAndOut","params":{}}
```

<a id="marker.duplicate-v1"></a>

### `marker.duplicate` v1: Duplicate Markers

Copies several sequence markers in one step (0.7): each marker at the given indices gets a copy `offset` later (or earlier when negative) with the same name, comment, colour, type, link and length. The copies' new indices are in the result. One undo step.

Capability `EditSequence`. Tool name `marker_duplicate`. Aliases: `markers.duplicate`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `indexes` | (integer (0..))[], min 1 | yes | Marker indices to copy. |
| `offset` | integer \| string | yes | How far from the originals (not 0). It lands on the frame grid. Signed timecode ("-00:00:00:05") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"marker.duplicate","params":{"indexes":[0,1],"offset":"00:00:10:00"}}
```

<a id="marker.import-v1"></a>

### `marker.import` v1: Import Markers

Reads a marker list into the sequence's markers: a CSV or tab-separated table with a header row (Premiere's marker export, a spreadsheet: Marker Name / Name, Description / Comment, In / Start / Time, Out, Duration, Marker Type, Color), a text file with one marker a line (YouTube chapters "0:00 Intro", "00:01:23:10 Name", Audacity labels) or JSON (an array, or an object with a "markers" array; marker.list's output reads back). Times: HH:MM:SS:FF at the sequence's frame rate, H:MM:SS.fff or M:SS clocks, seconds ("12.5", "12.5s"), "1500ms"; they land on the frame grid. "offset" is added to every time (a file that counts from 0 to start at the playhead). The markers join the ones already there, leaving out exact copies (reading a file twice adds nothing); "replace" clears the existing markers first. A line that cannot be read is skipped and listed in the result's warnings. Untrusted input: at most 16 MiB and the 10,000 markers a sequence holds.

Capability `EditSequence`. Tool name `marker_import`. Aliases: `markers.import`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `auto` \| `csv` \| `txt` \| `json` |  | The file's shape. Default auto: detected from the content. |
| `offset` | integer \| string |  | Added to every marker's time. Default 0. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `path` | string | yes | The marker file (UTF-8 path). |
| `replace` | boolean |  | Remove the sequence's markers first. Default false. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"marker.import","params":{"path":"C:/Delivery/chapters.csv"}}
```

<a id="marker.list-v1"></a>

### `marker.list` v1: List Markers

The sequence's markers in time order with all their fields: index (what marker.update and marker.remove take), start, duration, name, comment, colour, type, web link, and the highlight a highlight finder wrote (score, confidence, reasons, peak, source; null when none).

Capability `Read`. Tool name `marker_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"marker.list","params":{}}
```

<a id="marker.markIn-v1"></a>

### `marker.markIn` v1: Mark In

Sets the sequence In mark at the start of the frame at a time.

Capability `EditSequence`. Tool name `marker_markIn`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The time to mark. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"marker.markIn","params":{"time":"00:00:04:00"}}
```

<a id="marker.markOut-v1"></a>

### `marker.markOut` v1: Mark Out

Sets the sequence Out mark at the end of the frame at a time (the frame is included).

Capability `EditSequence`. Tool name `marker_markOut`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The last frame to include. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"marker.markOut","params":{"time":"00:00:09:29"}}
```

<a id="marker.moveMany-v1"></a>

### `marker.moveMany` v1: Move Markers

Moves several sequence markers by the same amount in one step (0.7): `by` is added to the start of each marker at the given indices (negative = earlier; their lengths stay; a highlight's peak moves with its marker). The move is cut short so the earliest of them does not start before 0; the result says how far they went. The list stays sorted by start, so the result lists the markers' new indices. One undo step.

Capability `EditSequence`. Tool name `marker_moveMany`. Aliases: `markers.moveMany`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `by` | integer \| string | yes | How far; positive = later. It lands on the frame grid. Signed timecode ("-00:00:00:05") or ticks. |
| `indexes` | (integer (0..))[], min 1 | yes | Marker indices to move. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"marker.moveMany","params":{"by":"00:00:01:00","indexes":[1,2]}}
```

<a id="marker.remove-v1"></a>

### `marker.remove` v1: Remove Marker

Removes the sequence marker at an index (as listed by sequence.summary).

Capability `EditSequence`. Tool name `marker_remove`. Aliases: `markers.remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `index` | integer (0..) | yes | Marker index. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"marker.remove","params":{"index":0}}
```

<a id="marker.removeMany-v1"></a>

### `marker.removeMany` v1: Remove Markers

Removes several sequence markers in one step (0.7): the markers at the given indices (as marker.list numbers them) go; the others stay. One undo step.

Capability `EditSequence`. Tool name `marker_removeMany`. Aliases: `markers.removeMany`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `indexes` | (integer (0..))[], min 1 | yes | Marker indices to remove. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"marker.removeMany","params":{"indexes":[0,2,3]}}
```

<a id="marker.update-v1"></a>

### `marker.update` v1: Edit Marker

Changes the sequence marker at an index (as listed by sequence.summary): any of its time, duration, name, comment, colour, type, URL and frame target. Keys left out keep their values; the markers stay sorted by start.

Capability `EditSequence`. Tool name `marker_update`. Aliases: `markers.update`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `colorIndex` | integer (0..15) |  | Marker colour index (Premiere's colours are 0-7). |
| `comment` | string |  | Marker comment. |
| `duration` | integer \| string |  | New length (0 = a point marker). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `highlight` | object \| null |  | C.15: a highlight finder's verdict on the marked moment (the Highlights panel reads it). null removes it. |
| `index` | integer (0..) | yes | Marker index. |
| `name` | string |  | Marker name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | string |  | Web link frame target. |
| `time` | integer \| string |  | New start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `type` | `comment` \| `chapter` \| `segmentation` \| `webLink` \| `source` |  | Premiere's marker type (0.7: "source" marks a citation, see sources.add). Default comment. |
| `url` | string |  | Web link URL (http or https). |

```json
{"command":"marker.update","params":{"colorIndex":1,"index":0,"name":"Interview"}}
```

<a id="mask.add-v1"></a>

### `mask.add` v1: Add Mask

Adds a mask to a clip: to an effect ("effect"; the effect then only changes the masked area - a local correction, including the colour grade) or, without "effect", to the clip's opacity mask (it cuts the clip out). Shapes: ellipse and rectangle (center, size, rotation, roundness) or a Bezier "path". Edge: feather and expansion; opacity; invert; add/subtract/intersect with the masks before it. Every parameter can be keyframed. Returns the mask's id.

Capability `EditSequence`. Tool name `mask_add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The video clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | The effect instance id (clip.details lists them). mask.add: omit for the clip's opacity mask (cut-outs); the others: default the effect holding the mask. |
| `enabled` | boolean |  | On or off. |
| `inverted` | boolean |  | Cover the outside instead. |
| `mode` | `add` \| `subtract` \| `intersect` |  | How it combines with the masks before it. Default add. |
| `name` | string |  | Display name, e.g. "Face". |
| `params` | object |  | Parameter -> number or {"keys": [{"time", "value", "interpolation"}]} (key times are sequence times on the clip): center.x/.y, size.x/.y (0..1 of the frame), rotation, roundness, feather and expansion (frame heights), opacity, transform.x/.y/.scaleX/.scaleY/.rotation/.skew/.anchorX/.anchorY. |
| `path` | array |  | Path shape: its vertices, [x, y] or {x, y, inX, inY, outX, outY} (tangents as offsets), 0..1 of the frame. |
| `pathKeys` | array |  | Animated path: [{"time", "vertices"}], every key with the same vertex count. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shape` | `ellipse` \| `rectangle` \| `path` |  | Default ellipse. |

```json
{"command":"mask.add","params":{"clip":42,"params":{"center.x":0.4,"feather":0.05,"size.x":0.3,"size.y":0.45},"shape":"ellipse"}}
```

<a id="mask.list-v1"></a>

### `mask.list` v1: List Masks

A clip's masks by effect, with their parameters (keys at media times) and paths.

Capability `Read`. Tool name `mask_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"mask.list","params":{"clip":42}}
```

<a id="mask.objectSelect-v1"></a>

### `mask.objectSelect` v1: Select Object

AI Object Mask: segments the person or object you click on (positive and negative clicks), draw a box around, or name ("subject": a face, a person, a car...) on a frame of a clip with SAM 2.1 (local, on this computer), and tracks it through the shot. The result is a mask that follows it, on an effect of the clip ("blur the background behind the speaker": a Gaussian Blur with an inverted object mask; "brighten the red car": a grade limited to it) or on the clip's opacity (remove the background). Stops at shot cuts unless acrossShots. On a nested or multicam clip it segments what the clip shows (the nest's picture, the active angle) and masks the clip itself; nothing is flattened. One undo step; dryRun previews it.

Capability `EditSequence`. Tool name `mask_objectSelect`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `acrossShots` | boolean |  | Keep tracking across shot cuts (default false: stop at the cuts around the frame). |
| `box` | object |  | A box around the object, normalised frame coordinates. |
| `box.height` | number (0.0..1.0) | yes | 0..1 of the frame height. |
| `box.width` | number (0.0..1.0) | yes | 0..1 of the frame width. |
| `box.x` | number (0.0..1.0) | yes | Left, 0..1. |
| `box.y` | number (0.0..1.0) | yes | Top, 0..1. |
| `clip` | integer \| object (1..) | yes | A clip id, or {"track", "time"} for the clip there. |
| `colour` | `red` \| `orange` \| `yellow` \| `green` \| `cyan` \| `blue` \| `purple` \| `pink` \| `white` \| `black` \| `grey` |  | With "subject": the one of this colour ("the red car"). |
| `effect` | integer (1..) |  | The effect the mask limits (clip.details lists them): blur the background (with "inverted"), grade only the subject. Omit for the clip's opacity mask: a cut-out that removes the background. |
| `expansion` | number (-0.5..0.5) |  | Grow (+) or choke (-) the matte, in frame heights (default 0). |
| `feather` | number (0.0..0.5) |  | Soft edge, in frame heights (default 0.004). |
| `in` | integer \| string |  | Track no earlier than this (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `inverted` | boolean |  | Everything but the object (default false). |
| `mode` | `add` \| `subtract` \| `intersect` |  | How it combines with the masks before it (default add). |
| `name` | string |  | The mask's name. Default the subject's, or "Object". |
| `out` | integer \| string |  | Track no later than this (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `pick` | `largest` \| `center` \| `left` \| `right` |  | With "subject": which one when there are several (default the largest). |
| `points` | object[] |  | Clicks on the object (positive) and off it (negative), normalised frame coordinates. |
| `points[].positive` | boolean |  | true: part of the object (default); false: not part of it. |
| `points[].x` | number (0.0..1.0) | yes | 0..1 of the frame width. |
| `points[].y` | number (0.0..1.0) | yes | 0..1 of the frame height. |
| `propagate` | `none` \| `forward` \| `backward` \| `both` |  | Track the object through the clip from the frame: default both ways (a job: progress, cancel); none = this frame only (refine first, then mask.propagate). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `subject` | string |  | Instead of clicks: what to find on the frame, "face" or a COCO class ("person", "car", "dog", "bicycle", ...). |
| `time` | integer \| string |  | The frame the clicks or the box are on, inside the clip. Default the clip's first frame. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"mask.objectSelect","params":{"clip":42,"points":[{"positive":true,"x":0.52,"y":0.4}],"time":"00:00:05:00"}}
```

<a id="mask.propagate-v1"></a>

### `mask.propagate` v1: Track Object Mask

Tracks an object mask's object through the clip (or In / Out) from its last clicked frame: after a selection made with "propagate": "none", or across a shot cut with acrossShots. A job with progress; one undo step.

Capability `EditSequence`. Tool name `mask_propagate`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `acrossShots` | boolean |  | Keep tracking across shot cuts (default false: stop at the cuts around the frame). |
| `clip` | integer \| object (1..) | yes | A clip id, or {"track", "time"} for the clip there. |
| `direction` | `none` \| `forward` \| `backward` \| `both` |  | Default both. |
| `effect` | integer (1..) |  | The effect holding the mask; default the one that has it. |
| `in` | integer \| string |  | Track no earlier than this (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `mask` | integer (1..) | yes | The object mask. |
| `out` | integer \| string |  | Track no later than this. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"mask.propagate","params":{"clip":42,"direction":"forward","mask":1}}
```

<a id="mask.refine-v1"></a>

### `mask.refine` v1: Refine Object Mask

More clicks (positive: part of the object; negative: not) or a new box on a frame of an object mask: segments it again there and re-tracks from that frame. One undo step.

Capability `EditSequence`. Tool name `mask_refine`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `acrossShots` | boolean |  | Keep tracking across shot cuts (default false: stop at the cuts around the frame). |
| `box` | object |  | A box around the object, normalised frame coordinates. |
| `box.height` | number (0.0..1.0) | yes | 0..1 of the frame height. |
| `box.width` | number (0.0..1.0) | yes | 0..1 of the frame width. |
| `box.x` | number (0.0..1.0) | yes | Left, 0..1. |
| `box.y` | number (0.0..1.0) | yes | Top, 0..1. |
| `clip` | integer \| object (1..) | yes | A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | The effect holding the mask; default the one that has it. |
| `in` | integer \| string |  | Re-track no earlier than this. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `mask` | integer (1..) | yes | The object mask (mask.objectSelect's "mask"). |
| `out` | integer \| string |  | Re-track no later than this. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `points` | object[] |  | Clicks on the object (positive) and off it (negative), normalised frame coordinates. |
| `points[].positive` | boolean |  | true: part of the object (default); false: not part of it. |
| `points[].x` | number (0.0..1.0) | yes | 0..1 of the frame width. |
| `points[].y` | number (0.0..1.0) | yes | 0..1 of the frame height. |
| `propagate` | `none` \| `forward` \| `backward` \| `both` |  | Re-track from the frame over the frames the mask covers: default both ways on a frame that was clicked before, else forward. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | The frame the clicks or the box are on, inside the clip. Default the clip's first frame. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"mask.refine","params":{"clip":42,"mask":1,"points":[{"positive":false,"x":0.61,"y":0.55}],"time":"00:00:07:12"}}
```

<a id="mask.remove-v1"></a>

### `mask.remove` v1: Remove Mask

Removes a mask from its effect.

Capability `EditSequence`. Tool name `mask_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | The effect instance id (clip.details lists them). mask.add: omit for the clip's opacity mask (cut-outs); the others: default the effect holding the mask. |
| `mask` | integer (1..) | yes | The mask id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"mask.remove","params":{"clip":42,"mask":1}}
```

<a id="mask.update-v1"></a>

### `mask.update` v1: Update Mask

Changes a mask: only the fields given (name, shape, mode, inverted, enabled, params, path / pathKeys). A number replaces an animated parameter; {"keys": [...]} replaces its keys.

Capability `EditSequence`. Tool name `mask_update`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `effect` | integer (1..) |  | The effect instance id (clip.details lists them). mask.add: omit for the clip's opacity mask (cut-outs); the others: default the effect holding the mask. |
| `enabled` | boolean |  | On or off. |
| `inverted` | boolean |  | Cover the outside instead. |
| `mask` | integer (1..) | yes | The mask id. |
| `mode` | `add` \| `subtract` \| `intersect` |  | How it combines with the masks before it. Default add. |
| `name` | string |  | Display name, e.g. "Face". |
| `params` | object |  | Parameter -> number or {"keys": [{"time", "value", "interpolation"}]} (key times are sequence times on the clip): center.x/.y, size.x/.y (0..1 of the frame), rotation, roundness, feather and expansion (frame heights), opacity, transform.x/.y/.scaleX/.scaleY/.rotation/.skew/.anchorX/.anchorY. |
| `path` | array |  | Path shape: its vertices, [x, y] or {x, y, inX, inY, outX, outY} (tangents as offsets), 0..1 of the frame. |
| `pathKeys` | array |  | Animated path: [{"time", "vertices"}], every key with the same vertex count. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shape` | `ellipse` \| `rectangle` \| `path` |  | Default ellipse. |

```json
{"command":"mask.update","params":{"clip":42,"inverted":true,"mask":1,"params":{"expansion":0.02}}}
```

<a id="media.browse-v1"></a>

### `media.browse` v1: Media Browser

Premiere's Media Browser listing of a folder: a camera card (AVCHD, Canon XF, P2, XDCAM EX / HD, XAVC) or a folder inside one lists the card's clips instead of its folders, a recording split into several files (a spanned clip, GoPro chapters) as one clip; any other folder its subfolders and media files. Each entry: folder or clip, name, path (a clip's first file), its files, the card, P2's audio channel files, bytes.

Capability `Read`. Tool name `media_browse`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `folder` | string | yes | The folder (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.browse","params":{"folder":"E:/"}}
```

<a id="media.duplicate-v1"></a>

### `media.duplicate` v1: Duplicate

Premiere's Edit > Duplicate on a project item: another item for the same file (or title, synthetic item) with its settings, Source Monitor marks, clip markers and metadata, in the same bin, named "<name> Copy" unless "name" is given. Clips in sequences keep playing the original. createdIds: the new item.

Capability `EditProject`. Tool name `media_duplicate`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) | yes | The item to duplicate. A media id or exact name. |
| `name` | string |  | The copy's name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.duplicate","params":{"media":3}}
```

<a id="media.fileMetadata-v1"></a>

### `media.fileMetadata` v1: File Metadata

The XMP of a media item's file (the Metadata panel's File section): a sidecar "<name>.xmp" first, else the packet inside the file, each property with its schema.

Capability `Read`. Tool name `media_fileMetadata`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) | yes | The media item. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.fileMetadata","params":{"media":3}}
```

<a id="media.importCameraClips-v1"></a>

### `media.importCameraClips` v1: Import from Media Browser

Imports clips as the Media Browser lists them (media.browse): each "files" entry is a clip's first file; a spanned clip imports as ONE item playing its parts in turn, a P2 clip as a merged clip of its picture and its audio channel files (Merge Clips). Into "bin" (default the root). "copyTo": a folder - Premiere's Ingest (Copy): each clip's files are copied into <copyTo>/<clip name>/ first and the copies imported, so the card can be taken out. Clip markers the host reads from the files (the app and the CLI: a recording's chapter markers, as File > Import adds them) come in with the items; "markers" counts them. One undo step.

Capability `EditProject`. Tool name `media_importCameraClips`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin they go in. A bin id. |
| `copyTo` | string |  | Ingest: copy the clips' files into this folder first. |
| `files` | string[], min 1 | yes | The clips. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.importCameraClips","params":{"files":["E:/DCIM/100GOPRO/GX010042.MP4"]}}
```

<a id="media.importImageSequence-v1"></a>

### `media.importImageSequence` v1: Import Image Sequence

Premiere's Import with "Image Sequence" ticked: the numbered stills starting at "path" (PNG, JPEG, TIFF, DPX, Targa, OpenEXR or BMP; that file and the ones numbered after it without a gap, e.g. shot_0001.png, shot_0002.png, ...) become one video media item playing one still per frame at "frameRate" (default 29.97 fps, Premiere's Indeterminate Media Timebase), named after its first file, in "bin" (default the root). Returns its id, the frame count and the rate.

Capability `EditProject`. Tool name `media_importImageSequence`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin it goes in. A bin id. |
| `frameRate` | number (0.01..1000.0) |  | Frames per second (23.976, 24, 25, 29.97, 30, 50, 59.94, 60, ...). |
| `path` | string | yes | The first still of the sequence (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.importImageSequence","params":{"frameRate":24,"path":"D:/Renders/shot_0001.png"}}
```

<a id="media.importLayeredFile-v1"></a>

### `media.importLayeredFile` v1: Import Layered File

Premiere's Import Layered File for a Photoshop file (.psd / .psb). "as": "mergeAll" (default: one picture of every visible layer), "merged" (one picture of the "layers" chosen), "layers" (Individual Layers: one item per chosen layer, named "<layer>/<file>", in a new bin named after the file) or "sequence" (the same items, plus a sequence of the document's size with one layer per video track, bottom layer on V1, each "duration" seconds long, default 5). "footage": "document" (default: each layer on the document's canvas, where it sits) or "layer" (Layer Size: cropped to the layer's pixels; in a sequence each clip is placed where the layer sits). "layers": layer indices from media.layeredFileInfo (default every visible layer). The items read the file each time they are opened, so saving it in Photoshop updates them. One undo step.

Capability `EditProject`. Tool name `media_importLayeredFile`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `as` | `mergeAll` \| `merged` \| `layers` \| `sequence` |  | How the layers import. |
| `bin` | integer (1..) |  | The bin it goes in. A bin id. |
| `duration` | number (0.04..86400.0) |  | Sequence: each layer's length in seconds. |
| `footage` | `document` \| `layer` |  | Footage Dimensions. |
| `layers` | (integer (0..))[] |  | The layers (bottom = 0). |
| `path` | string | yes | The Photoshop file (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.importLayeredFile","params":{"as":"sequence","footage":"layer","path":"D:/Design/Lower third.psd"}}
```

<a id="media.interpretFootage-v1"></a>

### `media.interpretFootage` v1: Interpret Footage

Premiere's Clip > Modify > Interpret Footage on items: "frameRate" assumes that rate for the file's frames (a 120, 144 or 240 fps game capture as 30 or 60 fps plays every frame in slow motion: its clips, new and placed, run at that speed through the speed engine and its frame interpolation; placed clips keep their source In and grow into the gap after them), "pixelAspect" conforms the pixel aspect ratio, "fieldOrder" the fields, and "ignoreAlpha" / "invertAlpha" / "premultiplication" the alpha channel; "vr" says the picture is VR / 360 video (its projection and eye layout) or flat, whatever the file's spherical metadata says. A field that is absent keeps its interpretation; null goes back to what the file says.

Capability `EditProject`. Tool name `media_interpretFootage`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `fieldOrder` | `progressive` \| `upperFirst` \| `lowerFirst` \| `null` |  | Conform the field order; null: the file's. |
| `frameRate` | number \| object \| null |  | Assume this frame rate (fps, or {"num", "den"}); null: the file's. |
| `ignoreAlpha` | boolean |  | Ignore the alpha channel (opaque). |
| `invertAlpha` | boolean |  | Invert the alpha channel. |
| `media` | (integer \| string (1..))[], min 1 | yes | The items. |
| `pixelAspect` | number \| object \| null |  | Conform to this pixel aspect ratio; null: the file's. |
| `premultiplication` | `straight` \| `premultiplied` \| `null` |  | Conform the alpha premultiplication; null: the file's. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `vr` | object \| null |  | Treat the picture as this VR format (projection none is a flat picture); null: what the file's spherical metadata says. |

```json
{"command":"media.interpretFootage","params":{"frameRate":30,"media":[3]}}
```

<a id="media.layeredFileInfo-v1"></a>

### `media.layeredFileInfo` v1: Layered File Info

What Premiere's Import Layered File dialog lists for a Photoshop file (.psd / .psb): the document's size, bit depth (8, 16 or 32) and colour mode (rgb, cmyk - converted to RGB on import - or grayscale) and its layers bottom to top - index, name, visibility (its own eye and its groups'), whether it has pixels or is a group, its bounds in document pixels, blend mode and the groups holding it.

Capability `Read`. Tool name `media_layeredFileInfo`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `path` | string | yes | The Photoshop file (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.layeredFileInfo","params":{"path":"D:/Design/Lower third.psd"}}
```

<a id="media.list-v1"></a>

### `media.list` v1: List Media

Lists the project's media as arrays in mediaFields order: id, name, duration in ms, whether it has video, its number of audio streams, whether it is a still image and whether it is offline.

Capability `Read`. Tool name `media_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `name` | string |  | Only media whose name contains this (case-insensitive). |

```json
{"command":"media.list","params":{"name":"interview"}}
```

<a id="media.makeOffline-v1"></a>

### `media.makeOffline` v1: Make Offline

Premiere's Make Offline (Project panel; the media files remain on disk): the items go offline - their clips show Media Offline - until they are linked again (File > Relink). Titles and other generated items have no file and are refused.

Capability `EditProject`. Tool name `media_makeOffline`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | (integer \| string (1..))[], min 1 | yes | The items. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.makeOffline","params":{"media":[3]}}
```

<a id="media.makeSubclip-v1"></a>

### `media.makeSubclip` v1: Make Subclip

A new media item playing "in".."out" (exclusive) of a media item's file, named "name", in "bin" (default the root). Restricted (default) keeps trims inside that range. Returns its id.

Capability `EditProject`. Tool name `media_makeSubclip`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin it goes in. A bin id. |
| `in` | integer \| string \| null | yes | Start (media time). Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `media` | integer \| string (1..) | yes | The master item. A media id or exact name. |
| `name` | string | yes | The subclip's name. |
| `out` | integer \| string \| null | yes | End, exclusive (media time). Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `restricted` | boolean |  | Keep trims inside the range (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.makeSubclip","params":{"in":"00:00:02:00","media":3,"name":"Laugh","out":"00:00:05:00"}}
```

<a id="media.mergeClips-v1"></a>

### `media.mergeClips` v1: Merge Clips

Premiere's Clip > Merge Clips: a merged clip of a video clip and separately recorded audio, synced by their In points, Out points, timecode, first clip markers or audio waveforms ("audio" needs the host's audio analysis). It is a synced source (a merged multicam source: the camera as angle 1, each recorder as another); edited into a sequence it shows the camera and plays every recording (and the camera's sound unless "removeCameraAudio"), and trims, ripples and undoes like any clip; multicam.flatten turns a clip of it into the items' own linked clips. createdIds: the merged clip.

Capability `EditProject`. Tool name `media_mergeClips`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audio` | (integer \| string (1..))[], min 1 | yes | The separately recorded audio. |
| `bin` | integer (1..) |  | Bin for it (bin id). Default: the video's. |
| `name` | string |  | The merged clip's name. Default: "<video> - Merged". |
| `removeCameraAudio` | boolean |  | Leave the camera's own sound out. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sync` | `inPoints` \| `outPoints` \| `timecode` \| `markers` \| `audio` |  | Synchronize point (default inPoints). |
| `video` | integer \| string (1..) |  | The camera clip (an item with video). A media id or exact name. |

```json
{"command":"media.mergeClips","params":{"audio":[4],"removeCameraAudio":true,"sync":"timecode","video":3}}
```

<a id="media.newSynthetic-v1"></a>

### `media.newSynthetic` v1: New Synthetic Item

Makes a synthetic project item (File > New): "barsAndTone" (SMPTE colour bars, params pattern 0 HD RP 219 or 1 SD EG 1, with a sine tone: tone.frequency, tone.level), "blackVideo", "colorMatte" (params color.r/g/b, 0..1: a background for text, a flash), "countingLeader" (Premiere's Universal Counting Leader: 8 seconds counting down from 8 to 2 with a clock wipe, its colours and cue blips as params) or "transparentVideo" (a layer of its own for effects that draw). It is made for the sequence's frame size and rate, needs no file and can be any length (the leader is 8 seconds). Put it on the timeline with clip.overwrite or clip.insert. createdIds: the item.

Capability `EditProject`. Tool name `media_newSynthetic`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin to put it in (bin id). Default: the root. |
| `kind` | `barsAndTone` \| `blackVideo` \| `colorMatte` \| `countingLeader` \| `transparentVideo` | yes | What to make. |
| `name` | string |  | The item's name. Default: the kind's name ("Color Matte"). |
| `params` | object |  | Settings by key; missing ones take the defaults (a white matte, HD bars with a 1 kHz tone at -20 dBFS, the leader's Premiere colours). |
| `params.background.b` | number (0.0..1.0) |  | countingLeader: Background Color Blue. |
| `params.background.g` | number (0.0..1.0) |  | countingLeader: Background Color Green. |
| `params.background.r` | number (0.0..1.0) |  | countingLeader: Background Color Red. |
| `params.color.b` | number (0.0..1.0) |  | colorMatte: Blue. |
| `params.color.g` | number (0.0..1.0) |  | colorMatte: Green. |
| `params.color.r` | number (0.0..1.0) |  | colorMatte: Red. |
| `params.cueBlipAllSeconds` | number (0.0..1.0) |  | countingLeader: Cue Blip at All Second Starts. |
| `params.cueBlipOn2` | number (0.0..1.0) |  | countingLeader: Cue Blip on 2. |
| `params.cueBlipOnOut` | number (0.0..1.0) |  | countingLeader: Cue Blip on Out. |
| `params.line.b` | number (0.0..1.0) |  | countingLeader: Line Color Blue. |
| `params.line.g` | number (0.0..1.0) |  | countingLeader: Line Color Green. |
| `params.line.r` | number (0.0..1.0) |  | countingLeader: Line Color Red. |
| `params.numeral.b` | number (0.0..1.0) |  | countingLeader: Numeral Color Blue. |
| `params.numeral.g` | number (0.0..1.0) |  | countingLeader: Numeral Color Green. |
| `params.numeral.r` | number (0.0..1.0) |  | countingLeader: Numeral Color Red. |
| `params.pattern` | number (0.0..1.0) |  | barsAndTone: Pattern (0 HD SMPTE RP 219, 1 SD SMPTE EG 1). |
| `params.target.b` | number (0.0..1.0) |  | countingLeader: Target Color Blue. |
| `params.target.g` | number (0.0..1.0) |  | countingLeader: Target Color Green. |
| `params.target.r` | number (0.0..1.0) |  | countingLeader: Target Color Red. |
| `params.tone.frequency` | number (20.0..20000.0) |  | barsAndTone: Tone Frequency. |
| `params.tone.level` | number (-60.0..0.0) |  | barsAndTone: Tone Level (dBFS). |
| `params.wipe.b` | number (0.0..1.0) |  | countingLeader: Wipe Color Blue. |
| `params.wipe.g` | number (0.0..1.0) |  | countingLeader: Wipe Color Green. |
| `params.wipe.r` | number (0.0..1.0) |  | countingLeader: Wipe Color Red. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.newSynthetic","params":{"kind":"colorMatte","params":{"color.b":0.6,"color.g":0.2,"color.r":0.1}}}
```

<a id="media.rawSourceSettings-v1"></a>

### `media.rawSourceSettings` v1: RAW Source Settings

The Source Settings a camera RAW item (ProRes RAW, CinemaDNG) is developed with: "raw" whether the item is camera RAW at all, "exposure" in stops, "whiteBalance" "asShot" (the camera's) or "custom" with the "temperature" (K) and "tint" of the light it is balanced for, and "isoShot" / "isoDevelop" / "isoStops" when the item is developed for another ISO than it was shot at.

Capability `Read`. Tool name `media_rawSourceSettings`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) | yes | A media item. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.rawSourceSettings","params":{"media":3}}
```

<a id="media.readXmp-v1"></a>

### `media.readXmp` v1: Read XMP Metadata

Fills the clip metadata of media items from the XMP of their files (Premiere's links: Scene, Shot, Take, Log Note, Good, Client, Tape Name, Comment, Description and the camera fields). "overwrite" (default true) replaces what is set; false only fills empty fields. Items without XMP stay. One undo step.

Capability `EditProject`. Tool name `media_readXmp`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | (integer \| string (1..))[], min 1 | yes | The media items. |
| `overwrite` | boolean |  | Replace fields that are set (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.readXmp","params":{"media":[3,4]}}
```

<a id="media.reload-v1"></a>

### `media.reload` v1: Reload Footage

Reload Footage (Project panel): reads the items' files again where they are, for a clip or image sequence that was re-exported or changed by another program. Each item takes the file's new length, streams, frame size and rate in place; its clips, marks, markers, effects and settings stay, and a proxy made of the old file is dropped. Items that share the file with a selected one are reloaded with it. Titles and other generated items are skipped. An item whose file reads the same as before changes nothing (no undo step); an offline item whose file is back comes online. Refused when a clip plays a stream the new file lacks; clips that now run past the end of a shorter file are listed in "warnings" and play what the file has. Needs a host that reads media files (the app, wabcut-cli). One undo step.

Capability `EditProject`. Tool name `media_reload`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | (integer \| string (1..))[], min 1 | yes | The items. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.reload","params":{"media":[3]}}
```

<a id="media.replaceFootage-v1"></a>

### `media.replaceFootage` v1: Replace Footage

Premiere's Replace Footage (Project panel): the item plays another file from now on - every clip of it keeps its place, source times, effects and markers, and its marks, markers and settings stay; the old file's proxy is dropped. "rename" (default true) gives the item the new file's name. Refused when a clip would play past the new file's end or the file lacks a stream a clip plays. Needs a host that reads media files (the app, wabcut-cli).

Capability `EditProject`. Tool name `media_replaceFootage`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) | yes | The item. A media id or exact name. |
| `path` | string | yes | The new file (an absolute path). |
| `rename` | boolean |  | Rename the item to the new file's name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.replaceFootage","params":{"media":3,"path":"D:/footage/A001_v2.mov"}}
```

<a id="media.setAttributes-v1"></a>

### `media.setAttributes` v1: Media Attributes

Library metadata of media items: name (new clips take it; existing clips keep theirs), label colour, comment, description, poster frame (a source time, or null to clear it) and (0.2) Premiere's clip metadata: scene, shot, take, log note, client, good, tape name, status and the camera fields (camera angle, label and model, shot date and location). Fields not given stay. One undo step for any number of items.

Capability `EditProject`. Tool name `media_setAttributes`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `cameraAngle` | string |  | Camera Angle. |
| `cameraLabel` | string |  | Camera Label. |
| `cameraModel` | string |  | Camera Model. |
| `client` | string |  | Client. |
| `colorIndex` | integer (0..15) |  | Label colour, 0..15. |
| `comment` | string |  | Comment. |
| `description` | string |  | Description. |
| `good` | boolean |  | Good (a circled take). |
| `logNote` | string |  | Log Note. |
| `media` | (integer \| string (1..))[], min 1 | yes | The media items. |
| `name` | string |  | The name (not empty). |
| `posterFrame` | integer \| string \| null |  | Poster frame (media time); null clears it. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `scene` | string |  | Scene. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `shot` | string |  | Shot. |
| `shotDate` | string |  | Shot Date (ISO-8601 text). |
| `shotLocation` | string |  | Shot Location. |
| `status` | string |  | Status. |
| `take` | string |  | Take. |
| `tapeName` | string |  | Tape Name (the reel name; empty clears it). |

```json
{"command":"media.setAttributes","params":{"colorIndex":4,"comment":"Best take","media":[3]}}
```

<a id="media.setRawSourceSettings-v1"></a>

### `media.setRawSourceSettings` v1: Source Settings

Premiere's Clip > Modify > Source Settings for camera RAW items (ProRes RAW, CinemaDNG): develops the items with "exposure" (stops, -5 to +5) and a white balance - "asShot" (the camera's) or "custom": the "temperature" (2000 to 15000 K; lower turns the picture bluer) and "tint" (-100 to +100, toward magenta) of the light to balance for, from the as-shot light (ProRes RAW's temperature; 6500 K for CinemaDNG). ISO: "isoShot" (the ISO the camera recorded at, which Wabcut cannot read from the file) and "isoDevelop" (the ISO to develop for) add log2(develop / shot) stops, a stop per doubling; "isoShot" alone, or equal ISOs, put it back to the camera's. Absent fields keep the first item's. Playback, thumbnails and export all develop the items so; their proxies are dropped (made from the old development). Items that are not camera RAW are refused. One undo step.

Capability `EditProject`. Tool name `media_setRawSourceSettings`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `exposure` | number (-5.0..5.0) |  | Exposure, stops. |
| `isoDevelop` | integer (50..204800) |  | The ISO to develop for; with "isoShot" it adds log2(develop / shot) stops of exposure. |
| `isoShot` | integer (50..204800) |  | The ISO the camera recorded at. |
| `media` | (integer \| string (1..))[], min 1 | yes | The items. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `temperature` | integer (2000..15000) |  | Colour temperature of the light to balance for, K (custom). |
| `tint` | integer (-100..100) |  | Tint, + toward magenta (custom). |
| `whiteBalance` | `asShot` \| `custom` |  | White balance. |

```json
{"command":"media.setRawSourceSettings","params":{"exposure":0.5,"media":[3],"temperature":3200,"whiteBalance":"custom"}}
```

<a id="media.setRights-v1"></a>

### `media.setRights` v1: Set Rights and Clearance

Sets the rights and clearance record of media items (the Rights and Clearance Log): where the footage came from ("source"), its "licence" terms and "reference" number, the "territory" it may be shown in, the day it "expiry" (YYYY-MM-DD), what it cost ("cost" and a three-letter "currency"), the "creditLine" as it must appear in the credits, free "notes" and its "status": cleared, pending, fairUse or notRequired ("" = not said). Only the fields given change; an empty text clears one. A subclip without a record of its own uses its master's. Titles and graphics are refused. One undo step for all the items.

Capability `EditProject`. Tool name `media_setRights`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `cost` | string \| number \| null |  | What it cost, as an amount (1250, "1,250.00"); empty or null clears it. |
| `creditLine` | string |  | As it must appear in the credits. |
| `currency` | string |  | Three capital letters (USD, EUR, GBP). |
| `expiry` | string |  | The last day it may be shown, YYYY-MM-DD; empty = none. |
| `licence` | string |  | The licence terms. |
| `media` | (integer \| string (1..))[], min 1 | yes | The media items. |
| `notes` | string |  | Free notes. |
| `reference` | string |  | The licence, asset or invoice number. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `source` | string |  | Who supplied or owns it. |
| `status` | `` \| `cleared` \| `pending` \| `fairUse` \| `notRequired` |  | Where the clearance stands. |
| `territory` | string |  | Where it may be shown. |

```json
{"command":"media.setRights","params":{"cost":"250.00","creditLine":"Archive footage courtesy of Newsreel Archive","currency":"GBP","expiry":"2028-06-30","media":[4,5],"source":"Newsreel Archive","status":"cleared"}}
```

<a id="media.setScaling-v1"></a>

### `media.setScaling` v1: Default Media Scaling

Sets the default media scaling of media items (Premiere's Preferences > Media > Default Media Scaling, which Premiere applies to items as they are imported): how later insert / overwrite edits and drops size their video clips in a sequence of another frame size - "none" (pixel size), "scaleToFrameSize", "setToFrameSize" or "setToFillFrame". Clips already in sequences keep their size.

Capability `EditProject`. Tool name `media_setScaling`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | (integer \| string (1..))[], min 1 | yes | The media items. |
| `scaling` | `none` \| `scaleToFrameSize` \| `setToFrameSize` \| `setToFillFrame` | yes | The default media scaling. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.setScaling","params":{"media":[3],"scaling":"setToFrameSize"}}
```

<a id="media.setSubclip-v1"></a>

### `media.setSubclip` v1: Edit Subclip

Changes a subclip's range and restriction (Premiere's Edit Subclip). Restricting is refused while a clip of it uses media outside the new range.

Capability `EditProject`. Tool name `media_setSubclip`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `in` | integer \| string \| null | yes | Start (media time). Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `media` | integer \| string (1..) | yes | The subclip. A media id or exact name. |
| `out` | integer \| string \| null | yes | End, exclusive (media time). Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `restricted` | boolean |  | Keep trims inside the range (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"media.setSubclip","params":{"in":"00:00:02:00","media":12,"out":"00:00:06:00"}}
```

<a id="media.setTimecode-v1"></a>

### `media.setTimecode` v1: Modify Timecode

Premiere's Clip > Modify > Timecode on items: the timecode of the first frame ("timecode" in the item's frame rate, or ticks; null: the file's own) and the reel (tape) name. Multicam and Merge Clips sync by it, and the Source Monitor shows it.

Capability `EditProject`. Tool name `media_setTimecode`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | (integer \| string (1..))[], min 1 | yes | The items. |
| `reelName` | string |  | The reel name (at most 128 bytes). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `timecode` | integer \| string \| null |  | The start timecode ("01:00:00:00"), ticks, or null for the file's. |

```json
{"command":"media.setTimecode","params":{"media":[3],"reelName":"A001","timecode":"01:00:00:00"}}
```

<a id="multicam.angles-v1"></a>

### `multicam.angles` v1: Multicam Angles

A multicam source's angles: their names, tracks and sync offsets, and how they were synced.

Capability `Read`. Tool name `multicam_angles`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `multicam` | integer (1..) |  | The multicam source. Default: the "sequence". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"multicam.angles","params":{"multicam":12}}
```

<a id="multicam.autoSwitch-v1"></a>

### `multicam.autoSwitch` v1: Auto Switch Multicam

Cuts a multicam clip to whoever is speaking, on this computer: the active speaker's close-up after a minimum hold, cut a little before they start, the wide shot for crosstalk, laughter, long pauses and for variety on long monologues, optional reaction shots, never in the middle of a word. Presets: calm (podcast), interview, energetic; every rule tunable. The result is ordinary angle cuts (editable afterwards), one undo step, each with its reason ("Cut to Cam B: Speaker 2 starts talking"); dryRun previews them.

Capability `EditSequence`. Tool name `multicam_autoSwitch`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `angles` | object[] |  | Your angle assignments; they win over the guesses. |
| `angles[].angle` | integer (1..64) | yes | The angle (1 = the first). |
| `angles[].role` | `closeUp` \| `wide` \| `unknown` |  | What it shows. |
| `angles[].speaker` | string |  | Whom a close-up shows: a speaker's key or name (analysis.speakers lists them). |
| `avoidMidWord` | boolean |  | Move cuts to the nearest pause between words (default true). |
| `clip` | integer (1..) |  | The multicam clip (its picture or sound; default: the sequence's first). |
| `faces` | boolean |  | Look at the cameras' faces (default true; false is quicker on long recordings). |
| `lead` | number (0.0..2.0) |  | Seconds a cut comes before the speaker starts (a J-cut). |
| `maxSnap` | number (0.0..2.0) |  | Seconds a cut may move to reach a pause. |
| `minHold` | number (0.2..60.0) |  | Seconds a shot is held at least. |
| `openLength` | number (0.0..30.0) |  | Seconds of that opening wide shot. |
| `openWide` | boolean |  | Open on the wide shot. |
| `overlapMin` | number (0.0..10.0) |  | Seconds of crosstalk before going wide. |
| `preset` | `calm` \| `interview` \| `energetic` |  | Podcast calm, Interview (default) or Energetic. |
| `reactionHold` | number (0.3..10.0) |  | Seconds a reaction shot lasts at least. |
| `reactionStrength` | number (0.0..1.0) |  | How strong a reaction must be (0..1). |
| `reactions` | boolean |  | Cut to a listener's close-up when they react strongly (laughter, a quick word). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `silenceWide` | number (0.0..60.0) |  | Seconds of silence before going wide (0 = never). |
| `target` | `video` \| `both` |  | Switch the picture only (default: the sound stays the mic mix) or both. |
| `varietyAfter` | number (0.0..600.0) |  | Seconds on one close-up before a short wide shot (0 = never). |
| `varietyLength` | number (0.5..30.0) |  | Seconds that variety wide shot lasts. |
| `wideOnLaughter` | boolean |  | The wide shot for laughter. |
| `wideOnOverlap` | boolean |  | The wide shot when people talk over each other. |

```json
{"command":"multicam.autoSwitch","params":{"preset":"interview"}}
```

<a id="multicam.create-v1"></a>

### `multicam.create` v1: Create Multi-Camera Source Sequence

Premiere's Create Multi-Camera Source Sequence: a new sequence with one angle per media item (its video on a video track, each audio stream on an audio track) synced by "sync": their start timecodes, Source Monitor In or Out marks, first clip markers, audio waveforms (needs the host's audio analysis) or "offsets" given by hand. Cut it into a sequence with sequence.insert / overwrite "sourceSequence".

Capability `EditProject`. Tool name `multicam_create`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `angleNames` | string[] |  | Angle names. Default: the media names. |
| `bin` | integer (1..) |  | Bin for the new sequence. Default: the first angle's bin. |
| `media` | (integer \| string (1..))[], min 1 | yes | The angles, in order. |
| `name` | string |  | Name of the new sequence. Default: "<first angle> Multicam". |
| `offsets` | (integer \| string)[] |  | manual: one per angle. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sync` | `timecode` \| `inPoints` \| `outPoints` \| `markers` \| `audio` \| `manual` |  | How the angles line up (default timecode). |

```json
{"command":"multicam.create","params":{"media":[3,4,5],"name":"Podcast","sync":"audio"}}
```

<a id="multicam.flatten-v1"></a>

### `multicam.flatten` v1: Flatten

Premiere's Flatten: a multicam clip (with its linked multicam partners) is replaced by the clips of the angles it plays, trimmed to what it shows (audio angles' tracks on its audio track and the ones below). Refused when that would not look and sound the same (effects on it, retimed, a clip in the way).

Capability `EditSequence`. Tool name `multicam_flatten`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The multicam clip. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"multicam.flatten","params":{"clip":57}}
```

<a id="multicam.setSync-v1"></a>

### `multicam.setSync` v1: Set Angle Sync

Moves one angle of a multicam source in time (its sync offset: where its material starts); every multicam clip then shows that angle shifted. The source's sync method becomes manual.

Capability `EditSequence`. Tool name `multicam_setSync`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `angle` | integer (1..64) | yes | The angle. An angle number: 1 is the first (Premiere's 1-9 keys). |
| `multicam` | integer (1..) |  | The multicam source. Default: the "sequence". |
| `offset` | integer \| string | yes | Its new offset (0 to 24 hours). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"multicam.setSync","params":{"angle":2,"multicam":12,"offset":"00:00:01:05"}}
```

<a id="multicam.switchAngle-v1"></a>

### `multicam.switchAngle` v1: Switch Multicam Angle

Premiere's angle switch (the 1-9 keys while playing): the multicam clip is cut at "at" and plays "angle" from there to its next cut; without "at" the whole clip switches. "switches" records a list of [time, angle] switches (a live multicam edit) as one undo step. "target": the picture, the sound or both (default).

Capability `EditSequence`. Tool name `multicam_switchAngle`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `angle` | integer (1..64) |  | The angle to switch to. An angle number: 1 is the first (Premiere's 1-9 keys). |
| `at` | integer \| string |  | Where the switch happens (default: the whole clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `clip` | integer \| object (1..) | yes | A multicam clip of the track to switch. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `switches` | object[], min 1 |  | Several switches in time order instead of one. |
| `switches[].angle` | integer (1..64) | yes | The angle. An angle number: 1 is the first (Premiere's 1-9 keys). |
| `switches[].at` | integer \| string | yes | Switch time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `target` | `video` \| `audio` \| `both` |  | What switches (default both). |

```json
{"command":"multicam.switchAngle","params":{"clip":57,"switches":[{"angle":2,"at":"00:00:04:00"},{"angle":1,"at":"00:00:09:12"}]}}
```

<a id="narration.create-v1"></a>

### `narration.create` v1: Narration

Narration from a script with the user's own voice provider (BYOK): each line spoken, over its rough-cut beat when the sequence is a rough cut (AI-11) or one after another, on a new "Narration" audio track with the Dialogue role and the other audio ducked under it, as one undo step. The audio is marked AI-generated. Paid: "estimate" prices it; a run needs "maxCost" and the app's approval.

Capability `EditProject`. Tool name `narration_create`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string |  | Where the first line starts when not on beats (default the sequence's start). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `cloneVoice` | boolean |  | Clone the speaker's own voice (ElevenLabs instant cloning) instead of a stock voice. Opt-in; needs rightsConfirmed and speaker; never for a speaker with redacted speech. The clone is deleted after the run. |
| `duck` | number (-60.0..0.0) |  | How far the other audio goes down under the narration, dB (default -12; 0 = not at all). |
| `estimate` | boolean |  | Only plan and price the run; nothing is sent or changed (default false). |
| `gap` | number (0.0..10.0) |  | Seconds between lines when not on beats (default 0.5). |
| `language` | string |  | The language spoken (ISO 639-1); dub default: the caption track's. |
| `maxCost` | number (0.0..1000.0) |  | The run's hard cap in USD: refused before anything is sent when it could cost more, stopped before a line that could pass it. Required for a run. |
| `maxStretch` | number (0.0..0.5) |  | How much faster a line may be played to fit (0.15 = 15 %, the default; pitch kept). |
| `model` | string |  | Voice model (registry id): eleven_v3 / eleven_v4 (Thai and 70+ languages), eleven_flash_v2_5, gpt-4o-mini-tts, gemini-3.8-flash-tts. Default: the host's, else eleven_v3. |
| `name` | string |  | The new audio track's name. |
| `onBeats` | boolean |  | On a rough cut (roughcut.fromScript), line i over beat i when the counts match (default true). |
| `rightsConfirmed` | boolean |  | The user confirmed: "I have the speaker's permission" to clone their voice. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speaker` | integer (1..) |  | The project speaker whose voice is cloned. |
| `style` | string |  | Delivery instructions ("warm, unhurried"), where the model takes them. |
| `text` | string | yes | The script: one line per beat (Markdown headings, bullets and [B-roll] cues are dropped). |
| `voice` | string |  | The model's voice (an ElevenLabs voice id, an OpenAI voice such as "marin", a Gemini voice such as "Kore"); default: the model's first. |

```json
{"command":"narration.create","params":{"estimate":true,"text":"Welcome to Chiang Mai."}}
```

<a id="ograf.import-v1"></a>

### `ograf.import` v1: Import OGraf

Imports an OGraf graphic (the EBU's open HTML graphics format) as a motion graphic template. OGraf graphics are drawn by web pages with scripts; Wabcut has no web engine and runs no scripts, so it imports a package that also ships a Lottie version of the graphic (one .json or .lottie animation in the package, or the one the manifest names in "v_wabcut_lottie"). The manifest's fields (title, name) name the template fields whose Lottie layers are called alike. "path" is the manifest (*.ograf.json: its folder is the package), a package folder or a .zip package. A package without a Lottie version fails with a message that says so (ograf.inspect lists its fields). With "at" it becomes a clip there (an instance of the template: Essential Graphics edits and keys its fields); with "saveAs" a template in the project's library.

Capability `EditProject`. Tool name `ograf_import`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string |  | Place it as a clip starting here. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `duration` | integer \| string |  | The clip's length (default the animation's). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `manifest` | string |  | Which graphic of a package that holds several: its manifest's path inside the package. |
| `path` | string | yes | The manifest (*.ograf.json), a package folder or a .zip package (UTF-8 path). |
| `placement` | `targeted` \| `freeTrackAtOrAbove` \| `newTrack` |  | "targeted": on "track" (or the targeted video track), overwriting what is there; the default when "track" is given. "freeTrackAtOrAbove": the first unlocked video track from "track" (or the targeted one) upwards with nothing in the graphic's time range, a new top track when none is free; the default without "track", so nothing under the playhead is overwritten. "newTrack": a new video track on top. |
| `saveAs` | object |  |  |
| `saveAs.description` | string |  | Description. |
| `saveAs.id` | string |  | Template id (default: from the graphic's id). |
| `saveAs.name` | string |  | Template name (default: the graphic's). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) |  | A video track. Default: the first targeted unlocked video track. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"ograf.import","params":{"at":"00:00:03:00","path":"D:/Graphics/lower-third.ograf.json"}}
```

<a id="ograf.inspect-v1"></a>

### `ograf.inspect` v1: Inspect OGraf

Reads an OGraf package's manifest without importing it: its name, id, author, the fields (title, type, default) and the custom actions it declares, the script that draws it (never run) and whether Wabcut can draw it - only a package with a Lottie version can be (ograf.import); "why" says what is missing otherwise.

Capability `Read`. Tool name `ograf_inspect`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `manifest` | string |  | Which graphic of a package that holds several: its manifest's path. |
| `path` | string | yes | The manifest (*.ograf.json), a package folder or a .zip package (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"ograf.inspect","params":{"path":"D:/Graphics/lower-third.ograf.json"}}
```

<a id="plugins.list-v1"></a>

### `plugins.list` v1: List Audio Plugins

The audio plugins (CLAP, VST3) this computer has, by effect id ("plugin.vst3.<class id>", "plugin.clap.<plugin id>"): name, vendor, version, category, latency (frames), whether it has an editor, and with "params" its parameters (keys "p<id>", ranges, automatable). Put one on clips or tracks with clip.addEffect / audio.addEffect ("plugin"); its parameters are keyframed like any effect's. Also the blocklist (plugins that crashed or hung while scanned) and the folders scanned. The OpenFX video plug-ins are listed too (format "openfx", effect ids "plugin.ofx.<id>", parameter keys by the plug-in's parameter names): they go on video clips with clip.addEffect ("plugin").

Capability `Read`. Tool name `plugins_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `clap` \| `vst3` \| `openfx` |  | Only plugins of this format. |
| `params` | boolean |  | Include every plugin's parameters. |
| `query` | string |  | Only plugins whose name or vendor contains this (any case). |

```json
{"command":"plugins.list","params":{"params":true,"query":"nova"}}
```

<a id="plugins.scan-v1"></a>

### `plugins.scan` v1: Scan Audio Plugins

Scans the audio plugin folders (Windows: Common Files\CLAP and \VST3, and the user's folders): new and changed files, each in its own helper process, so a plugin that crashes or hangs while scanned goes on the blocklist and nothing else is disturbed (the OpenFX video plug-ins are scanned the same way, in the same folders). Unchanged files keep their cached result ("rescan" scans them again). Adds or removes user folders and takes files off the blocklist first. Runs plugin code: not available to the assistant.

Capability `MediaAnalysis`. Tool name `plugins_scan`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `addFolders` | string[] |  | User plugin folders to add (remembered). |
| `clearBlocklist` | boolean |  | Empty the blocklist first. |
| `removeFolders` | string[] |  | User plugin folders to remove. |
| `rescan` | boolean |  | Scan unchanged files again too. |
| `unblock` | string[] |  | Files to take off the blocklist. |

```json
{"command":"plugins.scan","params":{"addFolders":["D:\\Plugins"]}}
```

<a id="project.consolidateDuplicates-v1"></a>

### `project.consolidateDuplicates` v1: Consolidate Duplicates

Premiere's Edit > Consolidate Duplicates: media items that are the same file (equal size and sample hash; subclips and titles are not) are merged into one. Everything that played a removed item - clips and audition takes in every sequence, graphic images, AI matte masks, redactions, subclips' masters, brand logos - plays the kept one, which is the item that is online, then has the most clips, then the lowest id. Only items with equal Interpret Footage, timecode, audio channels, frame scaling, camera RAW settings and Master Clip Effects merge; the others stay and are listed under "skipped" with the reason. The keeper's empty comment, description, label, metadata and rights are filled from the removed items, markers are added (once per start and name), transcript corrections move over when the keeper has none. "media" limits it to the groups of those items. One undo step; "groups" in the result is the report (a dry run shows it without changing anything).

Capability `EditProject`. Tool name `project_consolidateDuplicates`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | (integer \| string (1..))[] |  | Only the groups of duplicates these items are in (default: all). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"project.consolidateDuplicates","params":{}}
```

<a id="project.importItems-v1"></a>

### `project.importItems` v1: Import from Project

Premiere's import of a project (or of selected sequences): copies of another project's bins (with everything in them), sequences (with what they play: media, nested and multicam sequences, LUTs, speakers, transcript corrections, redactions, review notes, templates and styles this project lacks) and media items, in "into" (default the root) or a new bin "binName". Media this project already has (same file) is reused, so no item is duplicated; files are found where the other project is. "everything" brings the whole project. The copies remember the other project and their id there (importSource: Reveal in Source Project, relinks offered). One undo step.

Capability `EditProject`. Tool name `project_importItems`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `binName` | string |  | A new bin here that holds them. |
| `bins` | (integer (1..))[] |  | Bins there. |
| `everything` | boolean |  | The whole project (default false). |
| `into` | integer (1..) |  | The bin here they go in. A bin id. |
| `media` | (integer (1..))[] |  | Media items there. |
| `path` | string | yes | The other project file (UTF-8 path). |
| `reuseMedia` | boolean |  | Reuse media this project has (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `sequences` | (integer (1..))[] |  | Sequences there. |

```json
{"command":"project.importItems","params":{"path":"D:/Productions/Doc/Interviews.wabcut","sequences":[12]}}
```

<a id="project.merge-v1"></a>

### `project.merge` v1: Merge Projects

Three-way merge of another copy of this project ("theirs", a project file) into this one, both edited from "base" (the file they started from): edits of different sequences, tracks or clips merge; the same thing changed differently on both sides is a conflict with a stable id. With unresolved conflicts the result is only staged (the project does not change; the conflicts and what would change are returned) unless "apply"; "choices" resolves them ([{conflict: id, take: "mine" | "theirs"}]; unresolved ones keep mine). A merge that cannot give a valid project with those choices is staged too, with "invalid" saying why. One undo step.

Capability `EditProject`. Tool name `project_merge`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `apply` | boolean |  | Apply even with unresolved conflicts (they keep mine). Default false. |
| `base` | string | yes | The common ancestor's project file. |
| `choices` | object[] |  | How conflicts resolve (the others keep mine). |
| `choices[].conflict` | string | yes | A conflict's id. |
| `choices[].take` | `mine` \| `theirs` | yes | Which side wins. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `theirs` | string | yes | Their project file (UTF-8 path). |

```json
{"command":"project.merge","params":{"base":"C:/Projects/Film (sent).wabcut","choices":[{"conflict":"clip:812","take":"theirs"}],"theirs":"C:/Projects/Film (Alex).wabcut"}}
```

<a id="project.rightsLog-v1"></a>

### `project.rightsLog` v1: Rights and Clearance Log

The clearance report documentary makers hand to legal: one row per source used in the sequence ("scope" sequence, the default, in the order each first appears) or per file in the project ("project"), with its clearance status, source, licence, reference, territory, expiry, cost, credit line and notes, how often and where it is used, and flags - uncleared (pending, or never said), expired on "asOf" or expiring within "soonDays" - and a summary: items per status, sources without a record, sources that need attention and the total cost per currency (a master and its subclips count once). "flaggedOnly" keeps the rows that need attention; "includeUnrecorded" (default true) keeps sources with no rights record. "text" is the CSV (or the JSON) to write; "report" the same as data. Nothing is written to disk.

Capability `Read`. Tool name `project_rightsLog`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `asOf` | string |  | The day to judge expiry dates on, YYYY-MM-DD (usually the delivery or air date). Default today. |
| `flaggedOnly` | boolean |  | Only the rows that need attention. |
| `format` | `csv` \| `json` |  | The text format. Default csv. |
| `includeUnrecorded` | boolean |  | Keep sources with no rights record. Default true. |
| `range` | `sequence` \| `inOut` |  | For scope sequence: the whole sequence (default) or its In/Out marks. |
| `scope` | `sequence` \| `project` |  | Sources used in the sequence (default) or every file. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `soonDays` | integer (0..3650) |  | Flag a licence that expires within this many days of asOf (0 = never). Default 30. |

```json
{"command":"project.rightsLog","params":{"asOf":"2026-12-01"}}
```

<a id="project.summary-v1"></a>

### `project.summary` v1: Project Summary

Lists the project's sequences (id, rate, length, track and clip counts) and media counts. Start here.

Capability `Read`. Tool name `project_summary`.

No parameters.

```json
{"command":"project.summary","params":{}}
```

<a id="provenance.report-v1"></a>

### `provenance.report` v1: Generated media provenance

Unsigned report of generated segments; no prompts or credentials.

Capability `Read`. Tool name `provenance_report`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id. |

```json
{"command":"provenance.report","params":{}}
```

<a id="record.takes-v1"></a>

### `record.takes` v1: Voice-over Takes

The takes of a recorded voice-over slot (Premiere's repeated voice-over passes over one range): each pass over the same track and range made a media item in the Voice-over bin, and the clip plays the newest unless record.useTake chose another. Lists them newest first with their number, media, start, length, input device, time and which one the clip plays.

Capability `Read`. Tool name `record_takes`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | A clip that plays a recorded take. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"record.takes","params":{"clip":42}}
```

<a id="record.useTake-v1"></a>

### `record.useTake` v1: Use Voice-over Take

Plays another take of a recorded voice-over slot (record.takes): that take's media goes over its recorded range on the clip's track as an overwrite, with the clip's level, pan and effects. The takes stay in the Voice-over bin. One undo step; dryRun previews it.

Capability `EditSequence`. Tool name `record_useTake`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer (1..) | yes | A clip that plays a recorded take. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `take` | integer (1..) | yes | The take number. |

```json
{"command":"record.useTake","params":{"clip":42,"take":2}}
```

<a id="redaction.add-v1"></a>

### `redaction.add` v1: Add Redaction

Marks a region of a media item in SOURCE time for privacy: every use of that range in every sequence (trimmed, sped up, reversed, reused) is blurred, pixelated or covered in preview, export and burn-in, before the clip's own effects; "audio" mutes or bleeps it (only "words" when given). Mark by "media" + "source", or through a "clip" between "in" and "out". The region can be keyframed or tracked (track.mask on a clip's mask, then copied) - its key times are media times. Returns the redaction's id.

Capability `EditProject`. Tool name `redaction_add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audio` | `none` \| `mute` \| `bleep` |  | What happens to its sound. Default none. |
| `clip` | integer \| object (1..) |  | Or mark through a clip: its media, between "in" and "out". A clip id, or {"track", "time"} for the clip there. |
| `color` | array |  | Solid: [r, g, b], 0..1. |
| `enabled` | boolean |  | On or off. |
| `in` | integer \| string |  | Sequence time inside the clip where the marked range starts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `media` | integer \| string |  | The media item (id or exact name). |
| `name` | string |  | e.g. "Licence plate". |
| `out` | integer \| string |  | Sequence time inside the clip where it ends. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `region` | object |  | The area: {"x", "y", "width", "height", "shape": "rectangle"\|"ellipse", "feather"} (0..1 of the frame), or mask fields {"shape", "params", "path", "pathKeys"} (key times are media times). Default a centred ellipse. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `source` | object |  | {"start", "end"}: the media time it covers (ticks, or a timecode). |
| `strength` | number (0.0..1.0) |  | Blur radius or mosaic cell in frame heights. Default 0.05. |
| `style` | `blur` \| `mosaic` \| `solid` |  | Default blur. |
| `words` | array |  | Only these media time ranges are muted or bleeped (transcript words): [{"start", "end"}]. |

```json
{"command":"redaction.add","params":{"clip":42,"in":"00:00:03:00","out":"00:00:06:00","region":{"height":0.08,"width":0.15,"x":0.6,"y":0.55},"style":"mosaic"}}
```

<a id="redaction.detectFaces-v1"></a>

### `redaction.detectFaces` v1: Redact Faces

Finds every face of a media item (or a clip's part of it) with the face detector on this computer, follows each one and covers it with its own source-anchored redaction (blur, mosaic or solid) that moves with it: every use of the media in every sequence is covered. Fail closed: a face seen once is redacted, with a margin. One undo step; dryRun previews it.

Capability `EditProject`. Tool name `redaction_detectFaces`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | A video clip (its media between the clip's In and Out): a clip id, or {"track", "time"}. |
| `in` | integer \| string |  | Media time to start (default the start). |
| `margin` | number (0.0..1.0) |  | Grow each face's box by this share of its size on every side (default 0.25). |
| `media` | integer (1..) |  | The media item (with "in" / "out" in its time), or give a "clip". |
| `minFaceHeight` | number (0.0..0.5) |  | Leave out faces smaller than this share of the frame height (default 0.02). |
| `name` | string |  | The redactions' name ("Face": "Face 1", "Face 2"...). |
| `out` | integer \| string |  | Media time to end (default the end). |
| `samplesPerSecond` | number (1.0..30.0) |  | Pictures looked at per second (default 8). |
| `sequence` | integer (1..) |  | Sequence id of the clip; only needed when the project has several. |
| `strength` | number (0.0..0.5) |  | Blur radius / mosaic cell, in frame heights (default 0.05). |
| `style` | `blur` \| `mosaic` \| `solid` |  | How the faces are covered (default blur). |

```json
{"command":"redaction.detectFaces","params":{"clip":42,"style":"mosaic"}}
```

<a id="redaction.list-v1"></a>

### `redaction.list` v1: List Redactions

The project's redactions (of one media item with "media").

Capability `Read`. Tool name `redaction_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string |  | Only this media item. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"redaction.list","params":{}}
```

<a id="redaction.override-v1"></a>

### `redaction.override` v1: Redaction Override

"Don't redact here": the redaction skips this clip (and, with "linked", its linked partners), or applies again with "redact": true. A clip later cut from an excepted one is redacted again until excepted too.

Capability `EditProject`. Tool name `redaction_override`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `linked` | boolean |  | Its linked partners too. Default true. |
| `redact` | boolean |  | true: redact this clip again. Default false (except it). |
| `redaction` | integer (1..) | yes | The redaction id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"redaction.override","params":{"clip":42,"redaction":3}}
```

<a id="redaction.remove-v1"></a>

### `redaction.remove` v1: Remove Redaction

Removes a redaction everywhere.

Capability `EditProject`. Tool name `redaction_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `redaction` | integer (1..) | yes | The redaction id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"redaction.remove","params":{"redaction":3}}
```

<a id="redaction.update-v1"></a>

### `redaction.update` v1: Update Redaction

Changes a redaction: only the fields given.

Capability `EditProject`. Tool name `redaction_update`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audio` | `none` \| `mute` \| `bleep` |  | What happens to its sound. Default none. |
| `clip` | integer \| object (1..) |  | Or mark through a clip: its media, between "in" and "out". A clip id, or {"track", "time"} for the clip there. |
| `color` | array |  | Solid: [r, g, b], 0..1. |
| `enabled` | boolean |  | On or off. |
| `in` | integer \| string |  | Sequence time inside the clip where the marked range starts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `media` | integer \| string |  | The media item (id or exact name). |
| `name` | string |  | e.g. "Licence plate". |
| `out` | integer \| string |  | Sequence time inside the clip where it ends. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `redaction` | integer (1..) | yes | The redaction id. |
| `region` | object |  | The area: {"x", "y", "width", "height", "shape": "rectangle"\|"ellipse", "feather"} (0..1 of the frame), or mask fields {"shape", "params", "path", "pathKeys"} (key times are media times). Default a centred ellipse. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `source` | object |  | {"start", "end"}: the media time it covers (ticks, or a timecode). |
| `strength` | number (0.0..1.0) |  | Blur radius or mosaic cell in frame heights. Default 0.05. |
| `style` | `blur` \| `mosaic` \| `solid` |  | Default blur. |
| `words` | array |  | Only these media time ranges are muted or bleeped (transcript words): [{"start", "end"}]. |

```json
{"command":"redaction.update","params":{"color":[0,0,0],"redaction":3,"style":"solid"}}
```

<a id="reframe.createSequence-v1"></a>

### `reframe.createSequence` v1: Auto Reframe

Makes a copy of a sequence at another aspect ("9:16" for vertical shorts, "1:1", "4:5", "16:9") whose video clips keep their subjects in frame: per shot, a smooth virtual camera (still, then steady pans) follows the main subject, all faces, the speaker or an anchor. The result is ordinary, editable Position/Scale keyframes on each clip (nothing is rendered); the source sequence is not changed. Run analysis.subjects first for long media (otherwise this runs it). The new sequence remembers its source, so reframe.rerun can make it again.

Capability `EditProject`. Tool name `reframe_createSequence`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `anchor` | (number (0.0..1.0))[], min 2 |  | Anchor mode: the point to keep centred, [x, y] in 0..1 of the picture. |
| `aspect` | string |  | Aspect of the new sequence: "9:16" (vertical), "1:1", "4:5", "16:9" or any "W:H". Default "9:16". |
| `fit` | `crop` \| `letterbox` \| `blur` |  | Where the subjects cannot fit: "crop" anyway (default), zoom out with "letterbox" bars, or zoom out over a "blur"red fill. |
| `height` | integer (16..8192) |  | Output height in pixels (with width). |
| `margin` | number (0.0..0.4) |  | Room kept around a subject, as a fraction of the frame. Default 0.04. |
| `mode` | `follow` \| `faces` \| `speaker` \| `anchor` |  | What the frame follows: "follow" the main subject of each shot (default), keep all "faces" in, the "speaker" who is talking, or a fixed "anchor" point. |
| `name` | string |  | Name of the new sequence. Default: the source's name and the aspect. |
| `preview` | boolean |  | Report the frame path of each clip for drawing (default true). |
| `sequence` | integer (1..) |  | The sequence to reframe; only needed when the project has several. |
| `smoothness` | number (0.0..1.0) |  | 0 = follow closely .. 1 = move only when needed. Default 0.5. |
| `width` | integer (16..8192) |  | Output width in pixels (with height). Default: the source's short side kept. |

```json
{"command":"reframe.createSequence","params":{"aspect":"9:16","mode":"follow"}}
```

<a id="reframe.rerun-v1"></a>

### `reframe.rerun` v1: Reframe Again

Makes a reframed sequence again from its source sequence (after the source was edited), with the parameters it was made with, or new ones given here. It replaces the reframed sequence's contents (its manual keyframe changes too); its id, name and bin stay. Fails when the source sequence was deleted.

Capability `EditProject`. Tool name `reframe_rerun`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `anchor` | (number (0.0..1.0))[], min 2 |  | Anchor mode: the point to keep centred, [x, y] in 0..1 of the picture. |
| `aspect` | string |  | Aspect of the new sequence: "9:16" (vertical), "1:1", "4:5", "16:9" or any "W:H". Default "9:16". |
| `fit` | `crop` \| `letterbox` \| `blur` |  | Where the subjects cannot fit: "crop" anyway (default), zoom out with "letterbox" bars, or zoom out over a "blur"red fill. |
| `height` | integer (16..8192) |  | Output height in pixels (with width). |
| `margin` | number (0.0..0.4) |  | Room kept around a subject, as a fraction of the frame. Default 0.04. |
| `mode` | `follow` \| `faces` \| `speaker` \| `anchor` |  | What the frame follows: "follow" the main subject of each shot (default), keep all "faces" in, the "speaker" who is talking, or a fixed "anchor" point. |
| `preview` | boolean |  | Report the frame path of each clip for drawing (default true). |
| `sequence` | integer (1..) |  | The reframed sequence to make again. |
| `smoothness` | number (0.0..1.0) |  | 0 = follow closely .. 1 = move only when needed. Default 0.5. |
| `width` | integer (16..8192) |  | Output width in pixels (with height). Default: the source's short side kept. |

```json
{"command":"reframe.rerun","params":{"sequence":2}}
```

<a id="render.deletePreviewFiles-v1"></a>

### `render.deletePreviewFiles` v1: Delete Render Files

Deletes preview files: of the sequence between "start" and "end" (In to Out with Sequence > Delete Render Files In to Out), of the whole sequence, or with "all" of every sequence in the project. The edit itself is not affected; the segments show red or yellow again. Needs a host with a preview cache; a dry run lists what would be deleted.

Capability `MediaAnalysis`. Tool name `render_deletePreviewFiles`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | Every sequence's preview files. |
| `end` | integer \| string |  | Where to end. Default: the sequence's Out mark, else its end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `start` | integer \| string |  | Where to start. Default: the sequence's In mark, else its start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"render.deletePreviewFiles","params":{"all":true}}
```

<a id="render.previewInOut-v1"></a>

### `render.previewInOut` v1: Render In to Out

Renders preview files for the red and yellow segments between In and Out (or "start" / "end"; else the whole sequence), so they play in real time; segments with a valid preview are skipped. The cache keys each file by the segment's content and drops it when an edit changes that content. Needs a host with a preview renderer (the app); a dry run lists the segments it would render.

Capability `MediaAnalysis`. Tool name `render_previewInOut`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `end` | integer \| string |  | Where to end. Default: the sequence's Out mark, else its end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `start` | integer \| string |  | Where to start. Default: the sequence's In mark, else its start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"render.previewInOut","params":{"end":"00:00:30:00","start":"00:00:00:00"}}
```

<a id="render.status-v1"></a>

### `render.status` v1: Render Bar

The render bar of a sequence (Premiere's red/yellow/green bar): segments between edit points, each "red" (must be rendered to play in real time: nested sequences, adjustment layers or effects heavier than real time), "yellow" (effects, transitions, blend modes or track mattes that may play in real time), "rendered" (a valid preview file exists) or "none" (plays as it is). Rendered segments show only in hosts with a preview cache.

Capability `Read`. Tool name `render_status`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `end` | integer \| string |  | Where to end. Default: the sequence's Out mark, else its end. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `start` | integer \| string |  | Where to start. Default: the sequence's In mark, else its start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"render.status","params":{"end":"00:01:00:00","start":"00:00:00:00"}}
```

<a id="review.add-v1"></a>

### `review.add` v1: Add Review Note

A frame-accurate comment on the sequence: at "time" (a frame) or from "start" to "end", optionally pointing at a spot or rectangle of the frame ("mark"), by "author". Returns its id.

Capability `EditProject`. Tool name `review_add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Who wrote it. |
| `end` | integer \| string |  | The end of the range (exclusive). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `mark` | object |  | Where on the frame: a rectangle in frame fractions from the top left (width = height = 0: a point). |
| `mark.height` | number (0.0..1.0) |  | Height, 0..1 (default 0). |
| `mark.width` | number (0.0..1.0) |  | Width, 0..1 (default 0). |
| `mark.x` | number (0.0..1.0) | yes | Left, 0..1. |
| `mark.y` | number (0.0..1.0) | yes | Top, 0..1. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `start` | integer \| string |  | Or the start of a range. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `text` | string | yes | The comment (UTF-8). |
| `time` | integer \| string |  | The frame it is about. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"review.add","params":{"author":"Client","mark":{"height":0.1,"width":0.2,"x":0.7,"y":0.05},"text":"The logo is too small","time":"00:00:12:05"}}
```

<a id="review.exportPackage-v1"></a>

### `review.exportPackage` v1: Export Review Package

Writes a review package folder for a client or reviewer: notes.json (the notes with timecodes) and index.html (one self-contained page with a thumbnail per note, its mark drawn on it), plus the thumbnails in frames/. Notes: the sequence's ("sequence"; all the project's without it), or "status" only. Thumbnails come from the host's renderer, else placeholder cards.

Capability `EditProject`. Tool name `review_exportPackage`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `path` | string | yes | The package folder (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `status` | `open` \| `resolved` |  | Only notes with this status. |
| `title` | string |  | The page's title. |

```json
{"command":"review.exportPackage","params":{"path":"C:/Review/Client cut v2.wabreview","title":"Client cut v2"}}
```

<a id="review.importComments-v1"></a>

### `review.importComments` v1: Import Review Comments

Reads the comments file a reviewer exported from a review link's page ("path", or its text in "text") and adds each comment as a review note of the sequence at "start" (where the review copy began in the sequence, default 0) plus the comment's frame (comments past "duration" are left out and listed). "link" is the page's public link id: a file that carries another one is refused. The same reviewer, time and text is not added twice. One undo step; with "markers" the new notes also become sequence markers.

Capability `EditProject`. Tool name `review_importComments`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `duration` | integer \| string |  | How long the review copy is; comments after it are left out (default: no limit). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `link` | string | yes | The review link's public id (what the file's "link" must be). |
| `markers` | boolean |  | Also make sequence markers of the new notes (default false). |
| `path` | string |  | The comments file (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `start` | integer \| string |  | Where the review copy begins in the sequence (default 0). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `text` | string |  | Or the file's text (at most 4 MiB). |

```json
{"command":"review.importComments","params":{"link":"9f2c0a41d7e855b3a6c17d20","path":"C:/Review/comments-maria-1a2b.json"}}
```

<a id="review.importPackage-v1"></a>

### `review.importPackage` v1: Import Review Package

Reads a review package's notes back (one undo step): replies and status update the matching notes (by id, else by sequence, time and text), new notes are added at their timecodes, and notes that cannot be placed or that changed on both sides are listed as conflicts and left out.

Capability `EditProject`. Tool name `review_importPackage`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `path` | string | yes | The package folder (UTF-8 path). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"review.importPackage","params":{"path":"C:/Review/Client cut v2.wabreview"}}
```

<a id="review.list-v1"></a>

### `review.list` v1: Review Notes

The review notes of the sequence ("sequence"; all the project's without it), in time order, with their timecodes, mark, author, status and replies ([author, text, createdAt]).

Capability `Read`. Tool name `review_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `status` | `open` \| `resolved` |  | Only notes with this status. |

```json
{"command":"review.list","params":{"status":"open"}}
```

<a id="review.remove-v1"></a>

### `review.remove` v1: Delete Review Note

Deletes a note and its replies.

Capability `EditProject`. Tool name `review_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `note` | integer (1..) | yes | The note's id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"review.remove","params":{"note":31}}
```

<a id="review.reply-v1"></a>

### `review.reply` v1: Reply to Review Note

Adds a reply to a note's thread.

Capability `EditProject`. Tool name `review_reply`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Who wrote it. |
| `note` | integer (1..) | yes | The note's id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `text` | string | yes | The reply (UTF-8). |

```json
{"command":"review.reply","params":{"author":"Editor","note":31,"text":"Bigger in v3"}}
```

<a id="review.resolve-v1"></a>

### `review.resolve` v1: Resolve Review Note

Marks a note resolved (or open again with "resolved": false).

Capability `EditProject`. Tool name `review_resolve`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `note` | integer (1..) | yes | The note's id. |
| `resolved` | boolean |  | true (default): resolved; false: open. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"review.resolve","params":{"note":31}}
```

<a id="review.toMarkers-v1"></a>

### `review.toMarkers` v1: Review Notes to Markers

Turns the sequence's review notes ("notes", or all of them) into sequence markers: named after the note's first line, the comment holding the text, author and replies, red while open, green once resolved. With "remove" the notes go.

Capability `EditSequence`. Tool name `review_toMarkers`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `notes` | (integer (1..))[] |  | The notes (default: all). |
| `remove` | boolean |  | Delete the notes too (default false). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"review.toMarkers","params":{"remove":true}}
```

<a id="roughcut.fromScript-v1"></a>

### `roughcut.fromScript` v1: Rough Cut from Script

Makes a first cut from a script, an outline or bullet points: each line is found in the transcripts of the recordings (fuzzy, Thai too), the best take of it chosen (fewest fillers and false starts, complete sentence, clean audio; alternates kept), and the takes assembled in script order as a new sequence with linked video and audio trimmed to the words with small handles. Optional: clean-up of fillers and pauses, B-roll suggestions on their own track. Returns the alignment report (per line: the chosen take, its confidence, the alternates). Needs transcripts. One undo step; dryRun previews it.

Capability `EditProject`. Tool name `roughcut_fromScript`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `broll` | boolean |  | Suggest B-roll from the footage for lines with visual cues or that the footage clearly shows, on their own track (default false). |
| `cleanup` | boolean |  | Also remove fillers and pauses (transcript.cleanup) from the cut (default false). |
| `cleanupSilences` | number (0.2..10.0) |  | With cleanup: the shortest pause removed, in seconds (default 0.8). |
| `file` | string |  | A .txt / .md file holding the script (only where the host allows reading files). |
| `fromSequence` | integer (1..) |  | Take the lines from the media this sequence uses, and its frame size and rate. |
| `interpret` | boolean |  | Let the user's cloud chat model place loose outline bullets first (transcript text only; needs consent; never for a no-cloud project; default false). |
| `markers` | boolean |  | A marker at each section heading of the script (default true). |
| `media` | (integer \| string (1..))[], min 1 |  | Media to take the lines from. Default: every media item with a transcript. |
| `minMatch` | number (0.2..0.95) |  | How closely a take must say the line, 0.2..0.95 (default 0.45). |
| `name` | string |  | The new sequence's name (default "Rough cut: <title>"). |
| `prefer` | `best` \| `cleanest` \| `first` \| `last` |  | Which take of a line to use: the best overall (default), the cleanest (fewest fillers and false starts), the first or the last recorded. |
| `split` | `auto` \| `lines` \| `sentences` |  | Beats: "auto" (bullets and short lines whole, prose per sentence; default), every line, or every sentence. |
| `text` | string |  | The script, outline or bullet points (plain text or Markdown). |

```json
{"command":"roughcut.fromScript","params":{"prefer":"cleanest","text":"# Launch video\nWe built Wabcut for creators.\nIt cuts in seconds, not hours.\n[B-roll: timeline close-up]"}}
```

<a id="roughcut.swapTake-v1"></a>

### `roughcut.swapTake` v1: Swap Take

In a rough cut made by roughcut.fromScript, plays another recorded take of one line in place of the current one; what follows moves to fit (a ripple). The report of roughcut.fromScript numbers the lines and their takes.

Capability `EditSequence`. Tool name `roughcut_swapTake`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `beat` | integer (1..) | yes | The line, 1-based as the report numbers it. |
| `sequence` | integer (1..) |  | The rough cut (a sequence made by roughcut.fromScript); only needed when the project has several. |
| `take` | integer (1..) | yes | The take of that line to use, 1-based as the report lists them. |

```json
{"command":"roughcut.swapTake","params":{"beat":3,"take":2}}
```

<a id="scopes.read-v1"></a>

### `scopes.read` v1: Read Scopes

Scope statistics of the picture at a time (or of one clip alone): luma percentiles in IRE, clipped highlights and crushed blacks, mean colour, saturation and colour cast, with a verdict ("underexposed", "overexposed", "low contrast", "clipped highlights", "crushed blacks" or "ok"). Read only.

Capability `Read`. Tool name `scopes_read`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | Only this clip, at "time" or its middle. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Sequence time. Default 0. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"scopes.read","params":{"time":"00:00:10:00"}}
```

<a id="search.footage-v1"></a>

### `search.footage` v1: Search Footage

Smart search: finds the moments of the project's media that show or say what the query describes ("a dog on the beach", "the part where she mentions the budget", Thai too), by what the frames show (an image-text model) and what is said (the transcripts). Returns hits best first: the media, its source range (sourceIn / sourceOut in ticks, start / end in seconds) ready for clip.insert, the matched transcript words and a score. Media must be indexed first (search.indexMedia); a read, it changes nothing.

Capability `Read`. Tool name `search_footage`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | string |  | Only media dated on or after this day (YYYY-MM-DD, the file's date). |
| `limit` | integer (1..100) |  | Most hits to return (default 20). |
| `maxDuration` | number (0.0..604800.0) |  | Only media at most this long (seconds). |
| `media` | (integer \| string (1..))[], min 1 |  | Only these media items. |
| `minDuration` | number (0.0..604800.0) |  | Only media at least this long (seconds). |
| `name` | string |  | Only media whose name contains this (case-insensitive). |
| `query` | string | yes | What to find. |
| `to` | string |  | Only media dated on or before this day (YYYY-MM-DD). |
| `transcript` | boolean |  | Search what is said (default true). |
| `visual` | boolean |  | Search what the frames show (default true). |

```json
{"command":"search.footage","params":{"limit":10,"query":"a dog running on the beach"}}
```

<a id="search.indexMedia-v1"></a>

### `search.indexMedia` v1: Index Media for Search

Brings the smart-search index up to date for some or all media: keyframes of every shot (one per shot, more in long shots) through the image-text model, and the transcript's phrases. Unchanged files are skipped, so running it again is cheap. Runs locally; nothing leaves the computer.

Capability `MediaAnalysis`. Tool name `search_indexMedia`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | (integer \| string (1..))[], min 1 |  | Media items to index (default: all). |

```json
{"command":"search.indexMedia","params":{}}
```

<a id="sequence.addEdit-v1"></a>

### `sequence.addEdit` v1: Add Edit

Add edit / razor (Premiere Ctrl+K): cuts the clips under a time on the given tracks (default: the targeted tracks).

Capability `EditSequence`. Tool name `sequence_addEdit`. Aliases: `timeline.razor`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `linked` | boolean |  | Also cut linked partners. Default false. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where to cut. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `tracks` | (string \| integer (1..))[] |  | Tracks to cut. Default: the targeted tracks. |

```json
{"command":"sequence.addEdit","params":{"linked":true,"time":"00:00:12:00","tracks":["V1"]}}
```

<a id="sequence.addEditToAllTracks-v1"></a>

### `sequence.addEditToAllTracks` v1: Add Edit to All Tracks

Add edit to all tracks (Premiere Ctrl+Shift+K): cuts every unlocked track at a time.

Capability `EditSequence`. Tool name `sequence_addEditToAllTracks`. Aliases: `timeline.razorAll`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where to cut. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.addEditToAllTracks","params":{"time":"00:00:12:00"}}
```

<a id="sequence.addSubmixTrack-v1"></a>

### `sequence.addSubmixTrack` v1: Add Audio Submix Track

Adds an audio submix track (Premiere's Sequence > Add Tracks > Audio Submix Tracks): a bus that audio tracks are assigned to (track.setOutput) or send to (track.addSend). Its own effects (audio.addEffect with the submix as the target), fader and panner (track.setVolume / track.setPan), mute and solo (track.setState) apply to the sum, and it outputs to the Mix. Submixes are labelled S1, S2, ... A mono submix folds its input to mono and pans it; the format is fixed once made. Use it to put one reverb or compressor on several tracks (all the voices, the music). sequence.removeTrack deletes one (its tracks go to the Mix). One undo step.

Capability `EditSequence`. Tool name `sequence_addSubmixTrack`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `mono` \| `stereo` \| `5.1` \| `7.1` \| `adaptive` |  | The submix's channels (default stereo). |
| `name` | string |  | A name (default "Submix n"). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.addSubmixTrack","params":{"format":"stereo","name":"Voices"}}
```

<a id="sequence.addTrack-v1"></a>

### `sequence.addTrack` v1: Add Track

Adds a video track above the top video track or an audio track below the last audio track.

Capability `EditProject`. Tool name `sequence_addTrack`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `kind` | `video` \| `audio` | yes | Track kind. |
| `name` | string |  | Custom name. Default: "Video n" / "Audio n". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.addTrack","params":{"kind":"audio"}}
```

<a id="sequence.applyAudioTransition-v1"></a>

### `sequence.applyAudioTransition` v1: Apply Audio Transition

Ctrl+Shift+D: a constant-power crossfade on the edit point nearest the playhead on each targeted audio track.

Capability `EditSequence`. Tool name `sequence_applyAudioTransition`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `duration` | integer \| string |  | Length. Default 1 second. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.applyAudioTransition","params":{"time":"00:00:10:00"}}
```

<a id="sequence.applyDefaultTransitions-v1"></a>

### `sequence.applyDefaultTransitions` v1: Apply Default Transitions to Selection

Shift+D: default transitions on both ends of each clip (cross dissolve on video, constant power on audio), on the cut where a neighbour meets the clip, otherwise a fade inside it.

Capability `EditSequence`. Tool name `sequence_applyDefaultTransitions`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audioDuration` | integer \| string |  | Audio length. Default 1 second. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `clips` | (integer \| object (1..))[], min 1 | yes | The selected clips. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `videoDuration` | integer \| string |  | Video length. Default 1 second. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.applyDefaultTransitions","params":{"clips":[42,43]}}
```

<a id="sequence.applyVideoTransition-v1"></a>

### `sequence.applyVideoTransition` v1: Apply Video Transition

Ctrl+D: a cross dissolve on the edit point nearest the playhead on each targeted video track, shortened to the media there is.

Capability `EditSequence`. Tool name `sequence_applyVideoTransition`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `duration` | integer \| string |  | Length. Default 1 second. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.applyVideoTransition","params":{"time":"00:00:10:00"}}
```

<a id="sequence.beats-v1"></a>

### `sequence.beats` v1: List Beats

The beats of the sequence's beat grid as arrays in beatFields order: time, place in the bar (1 = downbeat) and index from the grid's downbeat.

Capability `Read`. Tool name `sequence_beats`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | integer \| string |  | Only beats at or after this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `limit` | integer (1..100000) |  | Most beats to return (default 500). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Only beats before this time. Default: the end of the sequence. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.beats","params":{"from":"00:00:00:00","to":"00:00:10:00"}}
```

<a id="sequence.closeGap-v1"></a>

### `sequence.closeGap` v1: Close Gap

Premiere's Sequence > Close Gap: removes the empty time between the clips (with their linked partners) on their tracks - with no clips, every gap of the unlocked tracks from the sequence's start - only where none of those tracks has material, so linked clips stay in sync; other unlocked sync-locked tracks lose the same time, and a gap where one of them has material stays. Caption cues follow. One undo step.

Capability `EditSequence`. Tool name `sequence_closeGap`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 0 |  | The clips between which to close gaps. Empty or absent: all gaps. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.closeGap","params":{}}
```

<a id="sequence.create-v1"></a>

### `sequence.create` v1: New Sequence

Makes a new, empty sequence (3 video and 3 audio tracks, 48 kHz) from a preset (sequence.presets: "1080p60", "1440p144", "2160p60", "shorts" for 1080x1920 60 fps, ...). createdIds: the sequence.

Capability `EditProject`. Tool name `sequence_create`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | The bin to put it in (bin id). Default: the root. |
| `name` | string |  | The sequence's name. Default: the preset's name. |
| `preset` | `720p23.976` \| `720p24` \| `720p25` \| `720p29.97` \| `720p30` \| `720p50` \| `720p59.94` \| `720p60` \| `1080p23.976` \| `1080p24` \| `1080p25` \| `1080p29.97` \| `1080p29.97df` \| `1080p30` \| `1080p50` \| `1080p59.94` \| `1080p59.94df` \| `1080p60` \| `1080p120` \| `1080p144` \| `1080p240` \| `1440p23.976` \| `1440p24` \| `1440p25` \| `1440p29.97` \| `1440p30` \| `1440p50` \| `1440p59.94` \| `1440p60` \| `1440p120` \| `1440p144` \| `1440p240` \| `2160p23.976` \| `2160p24` \| `2160p25` \| `2160p29.97` \| `2160p29.97df` \| `2160p30` \| `2160p50` \| `2160p59.94` \| `2160p59.94df` \| `2160p60` \| `2160p120` \| `2160p144` \| `2160p240` \| `vertical23.976` \| `vertical24` \| `vertical25` \| `vertical29.97` \| `shorts30` \| `vertical50` \| `vertical59.94` \| `shorts` \| `vertical120` \| `square1080p24` \| `square1080p25` \| `square1080p29.97` \| `square1080p30` \| `square1080p50` \| `square1080p59.94` \| `square1080p60` | yes | The preset (sequence.presets). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.create","params":{"name":"Clutch 1v4 (Short)","preset":"shorts"}}
```

<a id="sequence.creditsRoll-v1"></a>

### `sequence.creditsRoll` v1: Credits Roll from Rights Log

Makes an end-credits roll from the credit lines of the sources the sequence uses: a rolling graphic clip (Essential Graphics Roll) holding "heading" and one line per distinct credit, in the order the sources first appear (or "alphabetical"), starting at "at" (default the end of the sequence's content). Sources with a record but no credit line are reported under "missingCreditLine". The length sets the speed: default about 1.6 s a line. Edit the result like any graphic (graphic.get, graphic.update). One undo step.

Capability `EditSequence`. Tool name `sequence_creditsRoll`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string |  | Where the roll starts. Default: the end of the sequence's content. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `duration` | integer \| string |  | How long it lasts. Default about 1.6 seconds a line, at least 8 seconds. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `heading` | string |  | The roll's heading; empty for none. Default "Archive and Licensed Material". |
| `name` | string |  | The graphic's name (also the clip's). Default Credits. |
| `order` | `appearance` \| `alphabetical` |  | Default appearance. |
| `placement` | `targeted` \| `freeTrackAtOrAbove` \| `newTrack` |  | As graphic.add. Default freeTrackAtOrAbove. |
| `range` | `sequence` \| `inOut` |  | The whole sequence (default) or its In/Out marks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `skipNotRequired` | boolean |  | Leave out sources marked not required that name no source or licence. Default true. |
| `track` | string \| integer (1..) |  | A video track. Default: the first free track above the picture. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"sequence.creditsRoll","params":{"heading":"Archive Footage"}}
```

<a id="sequence.duplicate-v1"></a>

### `sequence.duplicate` v1: Duplicate Sequence

Premiere's Edit > Duplicate on a sequence in the Project panel: a copy of the whole sequence (tracks, clips, effects, markers, captions; new ids) in the same bin, named "<name> Copy" ("Copy 2", ... when taken) unless "name" is given. createdIds: the copy.

Capability `EditProject`. Tool name `sequence_duplicate`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `name` | string |  | The copy's name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.duplicate","params":{}}
```

<a id="sequence.exportFrame-v1"></a>

### `sequence.exportFrame` v1: Export Frame

Premiere's Export Frame (the Program Monitor's camera button, Ctrl+Shift+E): renders the sequence frame at "time" as an export renders it (original media, the export colour transform) at the sequence's size and writes it to "path" as DPX (10-bit), JPEG, PNG, TIFF, Targa, BMP, GIF or OpenEXR (half float, linear). With "importIntoProject" the still is added to the project as a media item (one undo step); otherwise the project does not change. Needs a host that renders frames (the app, wabcut-cli); a dry run writes nothing.

Capability `EditProject`. Tool name `sequence_exportFrame`. Aliases: `frame.export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | With importIntoProject: the bin it goes into (default the root bin). |
| `fit` | `fit` \| `fill` |  | With a size of another shape: "fit" keeps the whole frame with black bars (default), "fill" crops its centre. |
| `format` | `dpx` \| `jpeg` \| `png` \| `tiff` \| `targa` \| `bmp` \| `gif` \| `openexr` |  | The image format (Premiere's Export Frame list). Default png. |
| `height` | integer (16..16384) |  | Scale the frame to this height (with width). |
| `importIntoProject` | boolean |  | Premiere's "Import into project": the still is added to the project as a media item. |
| `maxBytes` | integer (10000..) |  | JPEG: the most the file may weigh, in bytes (YouTube thumbnails: 2000000); the quality goes down until it fits. |
| `path` | string | yes | The file to write (UTF-8 path). The format's extension is added when it has another one; an existing file is replaced. |
| `quality` | integer (1..100) |  | JPEG quality, 1-100 (default: the highest). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The frame to export (snapped to the sequence's frames). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `width` | integer (16..16384) |  | Scale the frame to this width (with height; default the frame's own size). 1280 x 720 is a YouTube thumbnail. |

```json
{"command":"sequence.exportFrame","params":{"format":"png","importIntoProject":true,"path":"C:/Stills/Thumbnail.png","time":"00:00:10:00"}}
```

<a id="sequence.exportFramesAtMarkers-v1"></a>

### `sequence.exportFramesAtMarkers` v1: Export Frames at Markers

Writes one still per sequence marker - all of them, the chapter markers, the highlight markers, or one colour - rendered as Export Frame renders them (original media, the export colour transform), named after the marker, into "folder": thumbnail candidates. A highlight marker's still is taken at its peak. With "width" / "height" (1280 x 720 for YouTube), "quality" and "maxBytes" (2000000 keeps a YouTube thumbnail under 2 MB) as sequence.exportFrame. With "importIntoProject" the stills join the project in a "Thumbnails" bin, one undo step. Needs a host that renders frames; a dry run lists the files only.

Capability `EditProject`. Tool name `sequence_exportFramesAtMarkers`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | `peak` \| `start` |  | Where the still is taken: a highlight marker's peak (default; the start for other markers) or every marker's start. |
| `binName` | string |  | With importIntoProject: the top-level bin they go into. Default "Thumbnails". |
| `colorIndex` | integer (0..7) |  | Only the markers of this colour (Premiere's 8: 0 green .. 7 cyan). |
| `fit` | `fit` \| `fill` |  | With a size of another shape: "fit" keeps the whole frame with black bars (default), "fill" crops its centre. |
| `folder` | string | yes | The folder the stills go into (UTF-8 path; it must exist). Each still is named after its marker ("Marker 3" when it has no name; " (2)" for a name used twice); an existing file is replaced. |
| `format` | `dpx` \| `jpeg` \| `png` \| `tiff` \| `targa` \| `bmp` \| `gif` \| `openexr` |  | The image format. Default jpeg. |
| `height` | integer (16..16384) |  | Scale the frame to this height (with width). |
| `importIntoProject` | boolean |  | The stills are added to the project, into a bin (made when missing), in one undo step. |
| `markers` | `all` \| `chapters` \| `highlights` |  | Which markers: every one (default), chapter markers, or the markers a highlight finder wrote. |
| `maxBytes` | integer (10000..) |  | JPEG: the most the file may weigh, in bytes (YouTube thumbnails: 2000000); the quality goes down until it fits. |
| `quality` | integer (1..100) |  | JPEG quality, 1-100 (default: the highest). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `width` | integer (16..16384) |  | Scale the frame to this width (with height; default the frame's own size). 1280 x 720 is a YouTube thumbnail. |

```json
{"command":"sequence.exportFramesAtMarkers","params":{"fit":"fill","folder":"C:/Thumbnails","format":"jpeg","height":720,"markers":"highlights","maxBytes":2000000,"width":1280}}
```

<a id="sequence.exportMarkers-v1"></a>

### `sequence.exportMarkers` v1: Export Markers

Returns the sequence's markers as text: "csv" (Premiere's columns: Marker Name, Description, In, Out, Duration, Marker Type, plus Color) or "youtube" (chapter lines for a video description, from the chapter markers, or every marker when there are none). YouTube's rules (the first chapter at 0:00, at least 3 chapters, each at least 10 s) are reported as warnings, never fixed silently. "range": "inOut" keeps the markers that start between the sequence's In and Out marks, YouTube times counted from the In mark. "chaptersOnly": chapter markers only (no text and the warning "no chapter markers" when there are none). Nothing is written to disk.

Capability `Read`. Tool name `sequence_exportMarkers`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `chaptersOnly` | boolean |  | Chapter markers only, never every marker. Default false. |
| `format` | `csv` \| `youtube` |  | The text format. Default csv. |
| `range` | `sequence` \| `inOut` |  | The whole sequence (default) or its In/Out marks, YouTube times counted from the In mark. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.exportMarkers","params":{"format":"youtube"}}
```

<a id="sequence.extendNextToPlayhead-v1"></a>

### `sequence.extendNextToPlayhead` v1: Extend Next Edit to Playhead

Shift+W: the nearest edit point after the playhead on the targeted tracks rolls back to the playhead.

Capability `EditSequence`. Tool name `sequence_extendNextToPlayhead`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.extendNextToPlayhead","params":{"time":"00:00:07:12"}}
```

<a id="sequence.extendPreviousToPlayhead-v1"></a>

### `sequence.extendPreviousToPlayhead` v1: Extend Previous Edit to Playhead

Shift+Q: the nearest edit point before the playhead on the targeted tracks rolls to the playhead (the clips on both sides change length; a clip next to a gap grows into it). Nothing else moves.

Capability `EditSequence`. Tool name `sequence_extendPreviousToPlayhead`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.extendPreviousToPlayhead","params":{"time":"00:00:07:12"}}
```

<a id="sequence.extract-v1"></a>

### `sequence.extract` v1: Extract

Extract (Premiere "'"): removes a time range from the targeted tracks and closes the gap; sync-locked tracks lose the same range so they stay in sync.

Capability `EditSequence`. Tool name `sequence_extract`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `in` | integer \| string |  | Start of the range. Default: the sequence In mark. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `out` | integer \| string |  | End of the range (exclusive). Default: the sequence Out mark. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (string \| integer (1..))[] |  | Tracks to act on. Default: the targeted tracks. |

```json
{"command":"sequence.extract","params":{"in":"00:00:04:00","out":"00:00:06:00","tracks":["V1","A1"]}}
```

<a id="sequence.goToGap-v1"></a>

### `sequence.goToGap` v1: Go to Gap

Premiere's Sequence > Go to Gap: where the next (previous) gap after (before) "time" starts - in the sequence (a time no track has a clip in, before the last clip) or in a track ("scope": "track": a gap on any of "tracks", default the targeted tracks). data.time is null when there is none; hosts move the playhead there.

Capability `Read`. Tool name `sequence_goToGap`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `direction` | `next` \| `previous` |  | Next (default) or previous. |
| `scope` | `sequence` \| `track` |  | In the sequence (default) or in tracks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where to look from (the playhead). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `tracks` | (string \| integer (1..))[] |  | Track scope: the tracks. Default: the targeted tracks. |

```json
{"command":"sequence.goToGap","params":{"direction":"next","scope":"sequence","time":"00:00:10:00"}}
```

<a id="sequence.joinThroughEdits-v1"></a>

### `sequence.joinThroughEdits` v1: Join Through Edits

Premiere's Join Through Edit (right-click a through edit) and Sequence > Join Through Edits: through edits - cuts between two pieces of the same clip that continue each other (same source, continuous source time, the same speed, effects and attributes, no transition at the cut) - at the edges of the clips (and their linked partners) go, each pair becoming one clip; with no clips, every through edit on the unlocked tracks. Refused when there is none there.

Capability `EditSequence`. Tool name `sequence_joinThroughEdits`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 0 |  | Clips whose through edits to join. Empty or absent: all. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.joinThroughEdits","params":{"clips":[42]}}
```

<a id="sequence.lift-v1"></a>

### `sequence.lift` v1: Lift

Lift (Premiere ';'): removes a time range from the targeted tracks and leaves a gap.

Capability `EditSequence`. Tool name `sequence_lift`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `in` | integer \| string |  | Start of the range. Default: the sequence In mark. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `out` | integer \| string |  | End of the range (exclusive). Default: the sequence Out mark. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (string \| integer (1..))[] |  | Tracks to act on. Default: the targeted tracks. |

```json
{"command":"sequence.lift","params":{"in":"00:00:04:00","out":"00:00:06:00"}}
```

<a id="sequence.makeSubsequence-v1"></a>

### `sequence.makeSubsequence` v1: Make Subsequence

Premiere's Sequence > Make Subsequence (Shift+U): a new sequence with copies of the clips (and their linked partners) on the same tracks, the earliest at 0, with the transitions between them and the markers and caption cues over their span; with no clips, everything on the targeted tracks between In and Out (Make Subsequence from In to Out). The sequence is unchanged. Named "<sequence> Sub_01" unless "name" is given. createdIds: the subsequence.

Capability `EditProject`. Tool name `sequence_makeSubsequence`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 0 |  | The clips. Empty or absent: In to Out on the targeted tracks. |
| `name` | string |  | The subsequence's name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.makeSubsequence","params":{"clips":[42,43]}}
```

<a id="sequence.makeVersion-v1"></a>

### `sequence.makeVersion` v1: Make Vertical or Square Version

Makes a 9:16 Shorts (1080x1920) or 1:1 square (1080x1080) version of a 16:9 gaming edit as a new sequence: with a facecam (a track named like a webcam - facecam, webcam, cam, camera -, a picture-in-picture made with layout.apply, "facecamTrack" or "facecamRegion") Shorts get the vertical split (gameplay on top, facecam below) and square versions the gameplay over a blurred fill with the facecam in a corner; without one, Auto Reframe when the host has it, else the gameplay fitted to the width over a blurred copy of itself. Captions move into the version's safe area. The 16:9 sequence is not changed. One undo step.

Capability `EditProject`. Tool name `sequence_makeVersion`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `captions` | boolean |  | Move captions into the version's safe area. |
| `corner` | `topLeft` \| `topRight` \| `bottomLeft` \| `bottomRight` |  | Square: the facecam box's corner. Default bottomRight. |
| `facecamOnTop` | boolean |  | Shorts: the facecam above the gameplay. |
| `facecamRegion` | object |  | Where the capture itself shows the webcam (no facecam track). |
| `facecamRegion.height` | number (0.01..1.0) | yes | Height, a fraction. |
| `facecamRegion.width` | number (0.01..1.0) | yes | Width, a fraction. |
| `facecamRegion.x` | number (0.0..1.0) | yes | Left, a fraction. |
| `facecamRegion.y` | number (0.0..1.0) | yes | Top, a fraction. |
| `facecamTrack` | string \| integer (1..) |  | The facecam's video track (default: detected). "V1" (bottom video track), "A2", ... or a track id. |
| `format` | `shorts` \| `square` |  | The version. Default shorts. |
| `method` | `auto` \| `layout` \| `reframe` \| `blurFill` |  | How the gameplay is laid out. Default auto. |
| `name` | string |  | The new sequence's name. Default "<name> (Shorts)" / "(Square)". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `split` | number (0.2..0.8) |  | Shorts: the gameplay's share of the height. Default 0.6. |

```json
{"command":"sequence.makeVersion","params":{"format":"shorts"}}
```

<a id="sequence.matchFrame-v1"></a>

### `sequence.matchFrame` v1: Match Frame

Match Frame (Premiere F): the clip under a time on the targeted tracks (the topmost; every track when none is targeted there), its media and the source frame shown. Like Premiere, the media's source marks become the clip's used range (setMarks false leaves them). A nested or multi-camera clip (0.7) answers with "nested": true, its source "sequence" and the "sequenceTime" it shows there instead (the app opens that sequence on that frame); no marks are set.

Capability `EditProject`. Tool name `sequence_matchFrame`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `setMarks` | boolean |  | Set the source marks to the clip's range. Default true. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `tracks` | (string \| integer (1..))[] |  | Tracks to look on. Default: the targeted tracks, else all. |

```json
{"command":"sequence.matchFrame","params":{"time":"00:00:12:05"}}
```

<a id="sequence.mediaUsage-v1"></a>

### `sequence.mediaUsage` v1: Media Usage Report

Lists every use of a source in the sequence, for archive clearances, music licensing and credits: one row per use with the media's name, path, tape name, scene / shot / take, whether it is AI-generated, the track, the record In/Out (the sequence's timecode, from its start time), the source In/Out (the media's timecode, at the clip's speed), the duration and the speed; then per source its uses, seconds on screen and seconds of the source shown. Clips of enabled video tracks and audible audio tracks, enabled clips only; a linked video and audio clip is one use. "flatten" (default true) reports through nested sequences (in the top sequence's record time) and multicam clips (their active angle); false lists a nested clip as one row. Time-remapped clips are reported as their whole range with the note "remapped". Titles, mattes and graphics are not listed. "text" is the CSV (or the JSON) to write; "report" the same as data. Nothing is written to disk. (0.7) Every row and source also carries its rights and clearance record (media.setRights): clearance, source, licence, territory, expiry, cost and credit line, and "Rights Flags" - uncleared material, a licence that has expired or expires within "soonDays" of "asOf" - and the command warns when any source needs attention.

Capability `Read`. Tool name `sequence_mediaUsage`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `asOf` | string |  | The day to judge expiry dates on, YYYY-MM-DD (usually the delivery or air date). Default today. |
| `flatten` | boolean |  | Report through nested and multicam clips. Default true. |
| `format` | `csv` \| `json` |  | The text format. Default csv. |
| `range` | `sequence` \| `inOut` |  | The whole sequence (default) or its In/Out marks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `soonDays` | integer (0..3650) |  | Flag a licence that expires within this many days of asOf (0 = never). Default 30. |

```json
{"command":"sequence.mediaUsage","params":{"format":"csv"}}
```

<a id="sequence.moveTrack-v1"></a>

### `sequence.moveTrack` v1: Move Track

Moves a video or audio track to another place in its own stack (0.7), so it becomes track number "position" (V1 / A1 = 1; a position past the last means the last), and the tracks between shift by one. The track keeps its clips, settings, links and effects; on video, the higher the track the nearer the top of the picture. A locked track can move. Submix tracks cannot.

Capability `EditProject`. Tool name `sequence_moveTrack`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `position` | integer (1..1000) | yes | The number the track gets in its stack (1 = V1 / A1). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) | yes | The track to move. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"sequence.moveTrack","params":{"position":3,"track":"V1"}}
```

<a id="sequence.nest-v1"></a>

### `sequence.nest` v1: Nest

Premiere's Nest: the clips (and, with "linked", their partners) move into a new sequence in the bin chosen for nests (bin.setNestTarget), else in the same bin as the sequence they come from, keeping their tracks, effects, links and the transitions between them, and one nested clip per kind (picture on the lowest selected video track, the mix on the lowest audio track, linked) takes their place. Returns the new sequence's id and the nested clips.

Capability `EditProject`. Tool name `sequence_nest`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | The clips to nest. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `name` | string |  | Name of the new sequence. Default: "Nested Sequence 01", 02, ... |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.nest","params":{"clips":[42,43],"name":"Interview"}}
```

<a id="sequence.newFromClip-v1"></a>

### `sequence.newFromClip` v1: New Sequence From Clip

Premiere's File > New > Sequence From Clip (the Project panel's right-click): a sequence whose settings match the item - frame size as it displays (a phone video turned 90 degrees makes a vertical sequence), frame rate (its interpretation when set), pixel aspect and audio sample rate - named after it, in its bin, with the item (between its Source Monitor marks) edited in at the start, picture on V1 and each audio stream on its own track. createdIds: the sequence.

Capability `EditProject`. Tool name `sequence_newFromClip`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) | yes | The item. A media id or exact name. |
| `name` | string |  | The sequence's name. Default: the item's. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.newFromClip","params":{"media":3}}
```

<a id="sequence.newFromClips-v1"></a>

### `sequence.newFromClips` v1: New Sequences From Clips

File > New Sequence From Clip for several items at once (the Project panel's selection). mode "each" (default): one sequence per item, each as sequence.newFromClip makes it (named after its item, in its bin, matching its picture and sound). mode "together": ONE sequence matching the first item, with every item edited in one after another ("name" names it; default the first item's name). All in one undo step. createdIds: the sequences, in the order of "media".

Capability `EditProject`. Tool name `sequence_newFromClips`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | (integer \| string (1..))[], min 1 | yes | The items. |
| `mode` | `each` \| `together` |  | One sequence per item (default), or all in one. |
| `name` | string |  | The sequence's name with mode "together". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.newFromClips","params":{"media":[3,4]}}
```

<a id="sequence.presets-v1"></a>

### `sequence.presets` v1: Sequence Presets

The sequence presets sequence.create and sequence.setSettings take (Premiere's rates for the target uses): landscape 720p, 1080p, 1440p and 4K at 23.976, 24, 25, 29.97 (also drop-frame), 30, 50, 59.94 (also drop-frame) and 60 fps, 1080p to 4K also at 120, 144 and 240 fps; vertical 1080x1920 for Shorts, TikTok and Reels ("shorts" at 60 fps, "shorts30", "vertical23.976" ...) and square 1080x1080. Every rate is exact in ticks.

Capability `Read`. Tool name `sequence_presets`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.presets","params":{}}
```

<a id="sequence.removeDeadTime-v1"></a>

### `sequence.removeDeadTime` v1: Remove Dead Time

Finds low-activity stretches of gameplay - game audio and picture motion both low (loading screens, walking, waiting) for at least "minSeconds" - and removes them with handles kept, like a silence cleanup. Without "apply" it only lists the ranges (the preview: nothing changes); review them, then call again with "apply": true (the same detection) or with the "ranges" to remove. Removing ripple-deletes on every unlocked track and the captions in one undo step; markers move with the material. Listens to the gameplay track (the lowest video track with media or a nested clip) and the unmuted audio tracks unless "tracks" says otherwise. Nested and multicam clips are measured through what they show and play (the active angle) and cut in place, staying nested / multicam. Media the host cannot measure is never cut.

Capability `EditSequence`. Tool name `sequence_removeDeadTime`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `apply` | boolean |  | Remove the ranges (default: only list them). |
| `handleSeconds` | number (0.0..5.0) |  | Kept at each end of a stretch. Default 0.3. Seconds. |
| `inOut` | boolean |  | Only look between the sequence In and Out marks. |
| `includeCurve` | boolean |  | Also return the activity curves (to draw them). |
| `minSeconds` | number (0.5..3600.0) |  | Shortest stretch that counts. Default 3. Seconds. |
| `ranges` | object[], min 1 |  | With apply: exactly these ranges (the preview's, edited) instead of detecting. |
| `ranges[].end` | integer \| string | yes | Range end (exclusive). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `ranges[].start` | integer \| string | yes | Range start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `rippleMarkers` | boolean |  | Markers move with the material. |
| `sensitivity` | number (0.0..1.0) |  | 0..1: higher finds more dead time. Default 0.5. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (string \| integer (1..))[] |  | Tracks to listen to and watch. Default: the gameplay track and the unmuted audio tracks. |

```json
{"command":"sequence.removeDeadTime","params":{"minSeconds":4,"sensitivity":0.6}}
```

<a id="sequence.removeDisabledClips-v1"></a>

### `sequence.removeDisabledClips` v1: Remove Disabled Clips

Deletes every disabled clip (clip.enable false) of the sequence's unlocked tracks, each on its own - a disabled picture's enabled sound stays - and leaves gaps; with "ripple" the time they occupied closes like Ripple Delete (refused when that would take an unselected clip off a track). Disabled clips on locked tracks stay. data.removed is how many clips went. One undo step.

Capability `EditSequence`. Tool name `sequence_removeDisabledClips`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `ripple` | boolean |  | Close the gaps they leave (Ripple Delete). Default: leave gaps. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.removeDisabledClips","params":{}}
```

<a id="sequence.removeTrack-v1"></a>

### `sequence.removeTrack` v1: Delete Track

Deletes a track and its clips (their linked partners on other tracks stay, unlinked). The track must be unlocked. A submix track ("S1"): the tracks assigned to it output to the Mix and the sends to it are removed.

Capability `EditProject`. Tool name `sequence_removeTrack`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) | yes | The track to delete. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"sequence.removeTrack","params":{"track":"V3"}}
```

<a id="sequence.removeTransitions-v1"></a>

### `sequence.removeTransitions` v1: Remove Transitions

Deletes the transitions of the clips - one goes when the clip on either side of it is listed (a fade or dip at one clip's edge belongs to that clip) - or, with no clips, every transition of the sequence's unlocked tracks. The edit points become plain cuts; clips, their effects and the other tracks stay. Linked partners are not added: list them too. Transitions on locked tracks stay. data.removed is how many went. One undo step.

Capability `EditSequence`. Tool name `sequence_removeTransitions`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 0 |  | The clips whose transitions go. Empty or absent: every transition of the sequence. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.removeTransitions","params":{}}
```

<a id="sequence.reverseMatchFrame-v1"></a>

### `sequence.reverseMatchFrame` v1: Reverse Match Frame

Reverse Match Frame (Premiere Shift+R): where a source frame is used in the sequence: the first use at or after "from", else the first one.

Capability `Read`. Tool name `sequence_reverseMatchFrame`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | integer \| string |  | Search from here (the playhead). Default: the start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `media` | integer \| string (1..) | yes | The source media. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string \| null | yes | The source frame. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `tracks` | (string \| integer (1..))[] |  | Tracks to look on. Default: all. |

```json
{"command":"sequence.reverseMatchFrame","params":{"media":3,"time":"00:00:07:00"}}
```

<a id="sequence.rippleTrimNextToPlayhead-v1"></a>

### `sequence.rippleTrimNextToPlayhead` v1: Ripple Trim Next Edit to Playhead

W: removes the time from the playhead to the next edit point on the targeted tracks and closes it.

Capability `EditSequence`. Tool name `sequence_rippleTrimNextToPlayhead`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.rippleTrimNextToPlayhead","params":{"time":"00:00:07:12"}}
```

<a id="sequence.rippleTrimPreviousToPlayhead-v1"></a>

### `sequence.rippleTrimPreviousToPlayhead` v1: Ripple Trim Previous Edit to Playhead

Q: removes the time from the previous edit point to the playhead on the targeted tracks and closes it.

Capability `EditSequence`. Tool name `sequence_rippleTrimPreviousToPlayhead`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.rippleTrimPreviousToPlayhead","params":{"time":"00:00:07:12"}}
```

<a id="sequence.setBeatGrid-v1"></a>

### `sequence.setBeatGrid` v1: Set Beat Grid

Sets the sequence's beat grid, which timeline snapping, edit.snapToBeats and edit.cutToBeats use: a tempo and the time of a downbeat, a tempo from tapped beats (at least two times), or detected beats; "clear" removes it. analysis.detectBeats sets it from the music.

Capability `EditSequence`. Tool name `sequence_setBeatGrid`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `beats` | (integer \| string)[] |  | Detected beats (music that drifts). |
| `beatsPerBar` | integer (1..32) |  | Default 4. |
| `bpm` | number (1.0..1000.0) |  | Tempo in beats per minute. |
| `clear` | boolean |  | Remove the grid. |
| `phase` | integer \| string |  | A downbeat (beat 1 of a bar). Default 0. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `taps` | (integer \| string)[], min 2 |  | Tapped beats: tempo and phase from them. |

```json
{"command":"sequence.setBeatGrid","params":{"bpm":120,"phase":"00:00:00:12"}}
```

<a id="sequence.setMasterFormat-v1"></a>

### `sequence.setMasterFormat` v1: Set Master Channel Format

The sequence's master: stereo, 5.1 or 7.1 (mono and stereo tracks through their surround panners; previewed on a stereo device and exported to stereo through an ITU-R BS.775 downmix; exports with 0 channels follow it). The result's "note" says what a change between layouts does to the sound (5.1 to 7.1 puts the surrounds in the side pair and leaves the rear pair silent). One undo step.

Capability `EditSequence`. Tool name `sequence_setMasterFormat`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `stereo` \| `5.1` \| `7.1` | yes | The master's format. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.setMasterFormat","params":{"format":"5.1"}}
```

<a id="sequence.setMasterGain-v1"></a>

### `sequence.setMasterGain` v1: Set Master Gain

Sets the master fader: dB, 0 = unity, applied to the sequence's mix after the master effects and before the meters (what export renders). With "time" it sets a keyframe there. One undo step.

Capability `EditSequence`. Tool name `sequence_setMasterGain`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `db` | number (-144.0..24.0) | yes | The level in dB (-144 = silence .. +24). |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` |  | The keyframe's interpolation. Default linear. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Put a keyframe here instead of setting a constant (the fader becomes animated). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.setMasterGain","params":{"db":-1.5}}
```

<a id="sequence.setPatches-v1"></a>

### `sequence.setPatches` v1: Set Source Patching

Checks a source patching (Premiere's source track indicators: which source track goes to which sequence track, and which are switched off) and returns it in order. Patching is view state of the app's Source Monitor, which keeps it; elsewhere nothing is stored: pass "patches" to the source edits.

Capability `Read`. Tool name `sequence_setPatches`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) | yes | The source media. A media id or exact name. |
| `patches` | object[] | yes | Source patching: which source track goes to which sequence track. |
| `patches[].enabled` | boolean |  | false switches the indicator off (Alt-click). Default true. |
| `patches[].newTrack` | string |  | Audio source tracks: the name of the track it goes to (multi-track recordings). Without "track" the edit adds an audio track with this name below the last one; an unnamed "track" is given the name. A recognised name sets the new track's role (Mic, Discord: dialogue; Music: music; Game, Alerts: effects). |
| `patches[].source` | string | yes | Source track: "V1" (the video), "A1", "A2", ... (audio streams). |
| `patches[].track` | string \| integer \| null |  | Destination track ("V2", "A1", ...), or null for none. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.setPatches","params":{"media":3,"patches":[{"source":"V1","track":"V2"},{"source":"A1","track":"A3"},{"source":"A2","track":null}]}}
```

<a id="sequence.setSettings-v1"></a>

### `sequence.setSettings` v1: Sequence Settings

Premiere's Sequence > Sequence Settings on an existing sequence. "preset" (sequence.presets) sets the frame size, timebase and drop-frame display at once; the other fields override it. Changing the timebase of a sequence with clips keeps the edit in real time and puts every edit point on the new frame grid: clip starts and ends, transitions, markers, In/Out, the work area and caption cues move to the nearest frame (a clip without media to reach it gets the frame inside instead), clips keep the picture under them, adjacent clips stay adjacent and locked tracks are left alone. Frame size changes keep each clip's Motion. Drop-frame display applies at 29.97 and 59.94 only. "label" is the sequence's label colour (Premiere's Label: the Project panel and its timeline tab). "rippleMarkers" is Markers > Ripple Sequence Markers. "rippleTrackKeyframes" (0.7) makes the keyframes of the tracks (their faders, panners, effects and sends) and of the master bus follow ripple edits like the markers do: later keys move with the material, keys inside a removed stretch go with it. "workAreaIn" and "workAreaOut" set Premiere's work area (the range used for playback and export; interchange files carry it), "clearWorkArea" removes it. One undo step.

Capability `EditProject`. Tool name `sequence_setSettings`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audioDisplay` | `audioSamples` \| `milliseconds` |  | Audio display format. |
| `audioRate` | integer (8000..384000) |  | Audio sample rate in Hz (32000, 44100, 48000, 96000 ...). |
| `clearWorkArea` | boolean |  | 0.7: remove the work area (the whole sequence again). |
| `dropFrame` | boolean |  | Drop-frame timecode display (29.97 and 59.94). |
| `fields` | `progressive` \| `upperFirst` \| `lowerFirst` |  | Fields: progressive (no fields), upper or lower field first. |
| `frameRate` | number \| object |  | Timebase. Frames per second (23.976, 25, 29.97, 59.94, 60 ...) or {"num", "den"}. |
| `height` | integer (16..16384) |  | Frame height, pixels. |
| `label` | integer (0..15) |  | The label colour, 0 to 15 (the Project panel's 16 label colours). |
| `name` | string |  | The sequence's name (not empty). |
| `pixelAspect` | number \| object |  | Pixel aspect ratio (1 square, 0.9091 D1/DV NTSC, 1.4587 PAL widescreen, 2 anamorphic ...). Frames per second (23.976, 25, 29.97, 59.94, 60 ...) or {"num", "den"}. |
| `preset` | string |  | A preset id from sequence.presets ("1080p23.976", "2160p29.97df", ...). |
| `previewFormat` | string |  | Preview file format id ("" = default; "prores", "dnxhr", "h264"). |
| `rippleMarkers` | boolean |  | Markers > Ripple Sequence Markers: sequence markers move with insert, extract, ripple delete and ripple trims. |
| `rippleTrackKeyframes` | boolean |  | Sequence > Ripple Track Keyframes: the keyframes of the tracks and the master bus move with the same ripple edits. Default off. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `startTimecode` | integer \| string |  | Start time: the timecode of the first frame ("01:00:00:00") or ticks. |
| `videoDisplay` | `timecode` \| `frames` \| `feet16mm` \| `feet35mm` |  | Video display format. |
| `vr` | object \| null |  | 0.7: the sequence is VR / 360 video of this projection and eye layout (its frame is the whole equirectangular picture): the Program Monitor can look around in it and exports write the spherical metadata. projection none (or null) makes it a flat sequence. |
| `width` | integer (16..16384) |  | Frame width, pixels. |
| `workAreaIn` | integer \| string |  | 0.7: the work area's In (Premiere's work area bar: the range used for playback and export). Pass with workAreaOut. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `workAreaOut` | integer \| string |  | 0.7: the work area's Out (exclusive). Pass with workAreaIn. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.setSettings","params":{"preset":"1080p23.976"}}
```

<a id="sequence.setTargets-v1"></a>

### `sequence.setTargets` v1: Set Track Targets

Targets exactly the given tracks (Premiere's track targeting: Lift, Extract, paste, Up/Down navigation and Match Frame use them).

Capability `EditSequence`. Tool name `sequence_setTargets`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (string \| integer (1..))[] | yes | Tracks to target; [] untargets all. |

```json
{"command":"sequence.setTargets","params":{"tracks":["V1","A1","A2"]}}
```

<a id="sequence.settings-v1"></a>

### `sequence.settings` v1: Sequence Settings Info

A sequence's settings as Premiere's Sequence Settings shows them: timebase, frame size, pixel aspect, fields, audio sample rate, drop-frame display, preview file format, video and audio display formats and the start timecode.

Capability `Read`. Tool name `sequence_settings`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.settings","params":{}}
```

<a id="sequence.summary-v1"></a>

### `sequence.summary` v1: Sequence Summary

Compact view of a sequence: settings, In/Out marks, every track (label, id, flags) with its clips as arrays in clipFields order (times are timecodes; link = link group, clips with the same value are linked), and markers. Use from/to to look at part of a long sequence.

Capability `Read`. Tool name `sequence_summary`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | integer \| string |  | Only clips that end after this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `maxClips` | integer (1..100000) |  | Most clips to list (default 500); "truncated" is set when more exist. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Only clips that start before this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `tracks` | (string \| integer (1..))[] |  | Only these tracks. Default: all. |

```json
{"command":"sequence.summary","params":{"from":"00:00:00:00","to":"00:01:00:00"}}
```

<a id="sequence.trimNextToPlayhead-v1"></a>

### `sequence.trimNextToPlayhead` v1: Trim Next Edit to Playhead

Ctrl+Alt+W: the clip under the playhead on each targeted track ends at the playhead (a regular trim of its tail; a gap is left after it).

Capability `EditSequence`. Tool name `sequence_trimNextToPlayhead`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.trimNextToPlayhead","params":{"time":"00:00:07:12"}}
```

<a id="sequence.trimPreviousToPlayhead-v1"></a>

### `sequence.trimPreviousToPlayhead` v1: Trim Previous Edit to Playhead

Ctrl+Alt+Q: the clip under the playhead on each targeted track starts at the playhead (a regular trim of its head; a gap is left before it).

Capability `EditSequence`. Tool name `sequence_trimPreviousToPlayhead`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The playhead. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"sequence.trimPreviousToPlayhead","params":{"time":"00:00:07:12"}}
```

<a id="sequence.unnest-v1"></a>

### `sequence.unnest` v1: Unnest

Resolve's Decompose: a nested clip (with its linked nested partners) is replaced by copies of the clips it shows - trimmed to what it shows, on its track and the ones above (audio: below), with their effects, links and transitions. Only when that looks and sounds exactly the same; otherwise it is refused with the reason (retimed, effects on the nested clip, track effects, a missing track, a clip in the way, ...).

Capability `EditSequence`. Tool name `sequence_unnest`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The nested clip. A clip id, or {"track", "time"} for the clip there. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sequence.unnest","params":{"clip":57}}
```

<a id="shorts.create-v1"></a>

### `shorts.create` v1: Create Shorts

Makes one new sequence per short (from shorts.find): the range cut out of the source sequence, reframed to 9:16 (or "aspect") with editable keyframes following the speaker, captioned (vertical shorts in the "Shorts" caption style unless "captionPreset" says otherwise), with the title as a hook for its first seconds; optionally cleaned of fillers and silences and normalised to -14 LUFS. The source is not changed; all the shorts of one call are one undo step. Export them afterwards (vertical 1080x1920).

Capability `EditProject`. Tool name `shorts_create`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `aspect` | string |  | Aspect of the shorts (default "9:16"). |
| `captionPreset` | string |  | The caption style: a built-in preset (Shorts, Clean, Bold Box, Podcast, Thai-friendly) or one of the project's. Default "Shorts" for vertical output, none (the default look) otherwise; "" = the default look. |
| `captions` | boolean |  | Add captions from the transcript (default true). |
| `cleanup` | boolean |  | Remove fillers and silences first (default false). |
| `cleanupSilences` | number (0.2..10.0) |  | With cleanup: the shortest pause removed, in seconds (default 0.8). |
| `fit` | `crop` \| `letterbox` \| `blur` |  | Where subjects cannot fit (default "crop"). |
| `hookTitle` | boolean |  | Show the title for the first seconds (default true). |
| `mode` | `follow` \| `faces` \| `speaker` \| `anchor` |  | What the frame follows (default "speaker"). |
| `normalize` | boolean |  | Normalise the loudness to -14 LUFS (default false). |
| `reframe` | boolean |  | Reframe to the aspect (default true). |
| `sequence` | integer (1..) |  | The source sequence; only needed when the project has several. |
| `shorts` | object[], min 1 | yes | The shorts to make (from shorts.find), at most 10. |
| `shorts[].end` | number (0.0..) | yes | End on the source sequence, in seconds. |
| `shorts[].start` | number (0.0..) | yes | Start on the source sequence, in seconds. |
| `shorts[].title` | string |  | Its title (the hook title and the sequence name). |
| `titleSeconds` | number (0.5..30.0) |  | How long the hook title shows (default 3). |

```json
{"command":"shorts.create","params":{"shorts":[{"end":58.0,"start":12.5,"title":"Why we doubled our price"}]}}
```

<a id="shorts.find-v1"></a>

### `shorts.find` v1: Find Shorts

Finds the best moments of a long sequence for vertical shorts (20-90 s each): whole sentences, a hook in the first seconds, one topic, lively delivery, faces, few fillers; optionally about a topic ("query"). Returns them best first with a score breakdown, reasons, a suggested title, the words and a start/end (seconds; ticks too) snapped to sentences and cuts, ready for shorts.create. Needs the media's transcripts. A read: it changes nothing.

Capability `Read`. Tool name `shorts_find`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `analyze` | boolean |  | Analyse faces and motion of media not analysed yet (slow for long media; default false: cached analyses only). |
| `count` | integer (1..20) |  | How many shorts (default 5). |
| `maxDuration` | number (5.0..600.0) |  | Longest short in seconds (default 90). |
| `minDuration` | number (5.0..600.0) |  | Shortest short in seconds (default 20). |
| `query` | string |  | What the shorts should be about, e.g. "the pricing discussion". |
| `rerank` | boolean |  | Also let the user's cloud chat model rank and title the best ones from their transcript text (needs consent; never for a no-cloud project; default false). |
| `sequence` | integer (1..) |  | The sequence to search (a podcast, an interview); only needed when the project has several. |
| `targetDuration` | number (5.0..600.0) |  | Preferred length in seconds (default 45). |

```json
{"command":"shorts.find","params":{"count":5,"query":"pricing"}}
```

<a id="source.addMarker-v1"></a>

### `source.addMarker` v1: Add Clip Marker

Adds a clip marker to a media item, in media time. Every clip of the media shows it where it uses that part of the source (Premiere's master clip markers).

Capability `EditProject`. Tool name `source_addMarker`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `colorIndex` | integer (0..15) |  | Marker colour index (Premiere's colours are 0-7). |
| `comment` | string |  | Marker comment. |
| `duration` | integer \| string \| null |  | Length for a range marker. Default 0. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `highlight` | object \| null |  | C.15: a highlight finder's verdict on the marked moment (the Highlights panel reads it). null removes it. |
| `media` | integer \| string (1..) | yes | The media. A media id or exact name. |
| `name` | string |  | Marker name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | string |  | Web link frame target. |
| `time` | integer \| string \| null | yes | Where the marker starts, in media time. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `type` | `comment` \| `chapter` \| `segmentation` \| `webLink` \| `source` |  | Premiere's marker type (0.7: "source" marks a citation, see sources.add). Default comment. |
| `url` | string |  | Web link URL (http or https). |

```json
{"command":"source.addMarker","params":{"media":3,"name":"Good take","time":"00:00:02:00"}}
```

<a id="source.clearMarks-v1"></a>

### `source.clearMarks` v1: Clear Source Marks

Clears a media item's source In and/or Out mark (Premiere's Source Monitor Clear In / Clear Out / Clear In and Out).

Capability `EditProject`. Tool name `source_clearMarks`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `in` | boolean |  | Clear the In. Default true. |
| `media` | integer \| string (1..) | yes | The media. A media id or exact name. |
| `out` | boolean |  | Clear the Out. Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"source.clearMarks","params":{"media":3,"out":false}}
```

<a id="source.exportFrame-v1"></a>

### `source.exportFrame` v1: Export Frame (Source Monitor)

Premiere's Export Frame from the Source Monitor: the frame of a media item at a source time, at the media's own frame size, written to "path" in the chosen format (as sequence.exportFrame); with "importIntoProject" the still joins the project (one undo step). Needs a host that renders frames.

Capability `EditProject`. Tool name `source_exportFrame`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `bin` | integer (1..) |  | With importIntoProject: the bin it goes into (default the root bin). |
| `fit` | `fit` \| `fill` |  | With a size of another shape: "fit" keeps the whole frame with black bars (default), "fill" crops its centre. |
| `format` | `dpx` \| `jpeg` \| `png` \| `tiff` \| `targa` \| `bmp` \| `gif` \| `openexr` |  | The image format (Premiere's Export Frame list). Default png. |
| `height` | integer (16..16384) |  | Scale the frame to this height (with width). |
| `importIntoProject` | boolean |  | Premiere's "Import into project": the still is added to the project as a media item. |
| `maxBytes` | integer (10000..) |  | JPEG: the most the file may weigh, in bytes (YouTube thumbnails: 2000000); the quality goes down until it fits. |
| `media` | integer \| string (1..) | yes | The media item the Source Monitor shows. A media id or exact name. |
| `path` | string | yes | The file to write (UTF-8 path). The format's extension is added when it has another one; an existing file is replaced. |
| `quality` | integer (1..100) |  | JPEG quality, 1-100 (default: the highest). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string \| null | yes | The source frame to export (the media's time). Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `width` | integer (16..16384) |  | Scale the frame to this width (with height; default the frame's own size). 1280 x 720 is a YouTube thumbnail. |

```json
{"command":"source.exportFrame","params":{"format":"jpeg","media":3,"path":"C:/Stills/Frame.jpg","time":"00:00:04:12"}}
```

<a id="source.importMarkers-v1"></a>

### `source.importMarkers` v1: Import Clip Markers

Reads a marker list (see marker.import: CSV, text lines or JSON) into a media item's clip markers, in media time ("HH:MM:SS:FF" at the media's own frame rate). Every clip of the media shows them. "offset" is added to every time; "replace" clears the media's markers first; exact copies of markers already there are left out.

Capability `EditProject`. Tool name `source_importMarkers`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `auto` \| `csv` \| `txt` \| `json` |  | The file's shape. Default auto: detected from the content. |
| `media` | integer \| string (1..) | yes | The media. A media id or exact name. |
| `offset` | integer \| string \| null |  | Added to every marker's time, in media time. Default 0. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `path` | string | yes | The marker file (UTF-8 path). |
| `replace` | boolean |  | Remove the media's markers first. Default false. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"source.importMarkers","params":{"media":3,"path":"C:/Delivery/takes.csv"}}
```

<a id="source.load-v1"></a>

### `source.load` v1: Load Source

Opens a media item as the source (Premiere's Source Monitor): its kind, duration, frame rate, stored In/Out marks and the source patching a source edit would use. The app also shows it in its Source Monitor.

Capability `Read`. Tool name `source_load`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) | yes | The media to load. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string \| null |  | Source position to show. Default: the In mark, else the start. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |

```json
{"command":"source.load","params":{"media":3,"time":"00:00:04:00"}}
```

<a id="source.removeMarker-v1"></a>

### `source.removeMarker` v1: Remove Clip Marker

Removes a media item's clip marker at an index.

Capability `EditProject`. Tool name `source_removeMarker`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `index` | integer (0..) | yes | Marker index. |
| `media` | integer \| string (1..) | yes | The media. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"source.removeMarker","params":{"index":0,"media":3}}
```

<a id="source.setMarks-v1"></a>

### `source.setMarks` v1: Set Source Marks

Sets a media item's source In/Out marks (Premiere's Source Monitor I and O). They are saved with the project and used by edit.insertFromSource / overwriteFromSource. A key left out keeps its mark; null clears it; an In at or after the Out (or an Out at or before the In) clears the other mark.

Capability `EditProject`. Tool name `source_setMarks`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `in` | integer \| string \| null |  | Source In: the first frame to use. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `media` | integer \| string (1..) | yes | The media. A media id or exact name. |
| `out` | integer \| string \| null |  | Source Out (exclusive): just past the last frame to use. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"source.setMarks","params":{"in":"00:00:02:00","media":3,"out":"00:00:06:12"}}
```

<a id="source.updateMarker-v1"></a>

### `source.updateMarker` v1: Edit Clip Marker

Changes a media item's clip marker at an index (keys left out keep their values).

Capability `EditProject`. Tool name `source_updateMarker`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `colorIndex` | integer (0..15) |  | Marker colour index (Premiere's colours are 0-7). |
| `comment` | string |  | Marker comment. |
| `duration` | integer \| string \| null |  | New length. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `highlight` | object \| null |  | C.15: a highlight finder's verdict on the marked moment (the Highlights panel reads it). null removes it. |
| `index` | integer (0..) | yes | Marker index. |
| `media` | integer \| string (1..) | yes | The media. A media id or exact name. |
| `name` | string |  | Marker name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | string |  | Web link frame target. |
| `time` | integer \| string \| null |  | New start, in media time. Ticks, or a timecode in the media's frame rate ("00:00:02:10"). |
| `type` | `comment` \| `chapter` \| `segmentation` \| `webLink` \| `source` |  | Premiere's marker type (0.7: "source" marks a citation, see sources.add). Default comment. |
| `url` | string |  | Web link URL (http or https). |

```json
{"command":"source.updateMarker","params":{"colorIndex":2,"index":0,"media":3}}
```

<a id="sources.add-v1"></a>

### `sources.add` v1: Add Source

Cites a source for a claim at a time of the sequence: a Source marker whose name is the claim, with the address, title, author, publisher, date, an archive link and a screenshot. Leave the title and address out (or pass "needed": true) to mark an uncited claim to come back to. sources.list shows them, sources.export writes the YouTube description block or the bibliography.

Capability `EditSequence`. Tool name `sources_add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `archive` | string |  | An archive link (http or https) in case the page goes away. |
| `author` | string |  | Who wrote or made it. |
| `claim` | string | yes | The claim the source backs, as the video says it. Becomes the marker's name. |
| `colorIndex` | integer (0..15) |  | Marker colour (default blue when cited, red when a source is needed). |
| `date` | string |  | When it was published (2024-02-01 is best; APA export reads it). |
| `duration` | integer \| string |  | How long the claim is on screen or in the voice-over. Default 0 (a point). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `needed` | boolean |  | The claim still wants a source (an uncited claim). Default: true when there is no title or address. |
| `note` | string |  | A note for yourself (the marker's comment). |
| `publisher` | string |  | The outlet or site. |
| `screenshot` | string |  | A screenshot of the source: a file path or address, as written. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where the claim is made (the start of the range). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `title` | string |  | The work: an article, a video, a paper, a book. |
| `url` | string |  | The source's address (http or https). |

```json
{"command":"sources.add","params":{"author":"A. Author","claim":"Unemployment fell 4 percent in 2023","date":"2024-02-01","time":"00:00:42:00","title":"Labour Market Report","url":"https://example.org/report"}}
```

<a id="sources.export-v1"></a>

### `sources.export` v1: Export Sources

The sources as text to publish: "youtube" (a description block with timestamps), "plain" (a numbered list), "markdown" or "apa" (a bibliography, one entry per distinct source). Claims that still need a source are named in the warnings and left out unless "includeUncited". Text only: the Sources panel writes the file or a PDF.

Capability `Read`. Tool name `sources_export`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `youtube` \| `plain` \| `apa` \| `markdown` |  | The form of the list (default youtube). |
| `includeUncited` | boolean |  | List the claims that need a source too, marked (default false). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sources.export","params":{"format":"apa"}}
```

<a id="sources.list-v1"></a>

### `sources.list` v1: List Sources

The sequence's sources by time: each claim with its address, reference and whether it still needs a source; counts of cited and uncited claims. Changes nothing.

Capability `Read`. Tool name `sources_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `onlyNeeded` | boolean |  | Only the claims that still need a source (default false). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sources.list","params":{}}
```

<a id="sources.lowerThirds-v1"></a>

### `sources.lowerThirds` v1: Show Citations as Lower Thirds

Puts each cited source on screen as a lower-third graphic ("Source: title - author") over its claim, on new "Sources" video tracks: for its length, or "seconds" (default 4) for a point. Claims without a source are skipped. One undo step.

Capability `EditSequence`. Tool name `sources_lowerThirds`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `seconds` | number (0.5..30.0) |  | How long a point source stays on screen, seconds (default 4). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sources.lowerThirds","params":{"seconds":5.0}}
```

<a id="sources.remove-v1"></a>

### `sources.remove` v1: Remove Source

Removes a source (its marker) by index; sources.list shows the indexes.

Capability `EditSequence`. Tool name `sources_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `index` | integer (0..) | yes | The source's marker index. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"sources.remove","params":{"index":0}}
```

<a id="sources.update-v1"></a>

### `sources.update` v1: Edit Source

Changes the fields of a source that are given (the claim, address, title, author, publisher, date, archive link, screenshot, note, its time). Giving a claim its first title or address clears "needed" unless you pass it.

Capability `EditSequence`. Tool name `sources_update`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `archive` | string |  | An archive link (http or https) in case the page goes away. |
| `author` | string |  | Who wrote or made it. |
| `claim` | string |  | The claim the source backs, as the video says it. Becomes the marker's name. |
| `colorIndex` | integer (0..15) |  | Marker colour (default blue when cited, red when a source is needed). |
| `date` | string |  | When it was published (2024-02-01 is best; APA export reads it). |
| `duration` | integer \| string |  | Its length. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `index` | integer (0..) | yes | The source's marker index (sources.list shows it). |
| `needed` | boolean |  | The claim still wants a source (an uncited claim). Default: true when there is no title or address. |
| `note` | string |  | A note for yourself (the marker's comment). |
| `publisher` | string |  | The outlet or site. |
| `screenshot` | string |  | A screenshot of the source: a file path or address, as written. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Move the claim to this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `title` | string |  | The work: an article, a video, a paper, a book. |
| `url` | string |  | The source's address (http or https). |

```json
{"command":"sources.update","params":{"index":0,"title":"Labour Market Report","url":"https://example.org/report"}}
```

<a id="spelling.check-v1"></a>

### `spelling.check` v1: Check Spelling

The misspelled words of a text in a language (default the user's), from the host's spell checker and the user's dictionary, as captions.checkSpelling lists them (byte ranges of the text). "available" is false when the host cannot check that language.

Capability `Read`. Tool name `spelling_check`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `language` | string |  | BCP 47 or ISO 639-1. Default: the user's. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `suggestions` | boolean |  | List suggestions. Default true. |
| `text` | string | yes | The text to check (UTF-8). |

```json
{"command":"spelling.check","params":{"language":"en-US","text":"Teh quick brown fox"}}
```

<a id="takes.find-v1"></a>

### `takes.find` v1: Find Takes

Finds repeated attempts of the same line (retakes and false starts) in a sequence and the recordings it uses: groups of takes, best first, each with its words, a word-by-word difference from the best one, and quality (fillers, false starts, pauses, complete sentence, level, clipping, noise floor, pace). "improvable" groups would change with takes.use best. Needs transcripts. A read: it changes nothing.

Capability `Read`. Tool name `takes_find`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `acrossRecordings` | boolean |  | Attempts in different recordings count (default true). |
| `includeSourceMedia` | boolean |  | Also look in the whole recordings the sequence uses, for takes trimmed away (default true). |
| `limit` | integer (1..500) |  | Groups listed at most (default 100). |
| `maxGap` | number (1.0..36000.0) |  | Within one recording, attempts further apart than this (seconds) are not retakes (default 180). |
| `meaning` | boolean |  | Also group paraphrased attempts by meaning with the text encoder (slower; default false). |
| `media` | (integer \| string (1..))[], min 1 |  | More media items whose transcripts hold takes. |
| `minSimilarity` | number (0.3..1.0) |  | How alike two attempts must be to count as takes of one line, 0.3..1 (default 0.7). |
| `sequence` | integer (1..) |  | The sequence; only needed when the project has several. |

```json
{"command":"takes.find","params":{"minSimilarity":0.7}}
```

<a id="takes.use-v1"></a>

### `takes.use` v1: Use Take

Makes the sequence say a line once, with the chosen take: other takes of it on the timeline are removed with their pause, and a take that is not on the timeline replaces the first one there (linked video and audio, with small handles; what follows moves to fit). "best": true does this for every group whose best take is not already the one heard. Pass the same options as takes.find (and its expectedRevision). One undo step; returns the review list.

Capability `EditSequence`. Tool name `takes_use`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `acrossRecordings` | boolean |  | Attempts in different recordings count (default true). |
| `best` | boolean |  | Use the best take of every group ("Keep best takes"). |
| `group` | integer (1..) |  | The group of takes, 1-based as takes.find lists them (with the same options). |
| `includeSourceMedia` | boolean |  | Also look in the whole recordings the sequence uses, for takes trimmed away (default true). |
| `maxGap` | number (1.0..36000.0) |  | Within one recording, attempts further apart than this (seconds) are not retakes (default 180). |
| `meaning` | boolean |  | Also group paraphrased attempts by meaning with the text encoder (slower; default false). |
| `media` | (integer \| string (1..))[], min 1 |  | More media items whose transcripts hold takes. |
| `minSimilarity` | number (0.3..1.0) |  | How alike two attempts must be to count as takes of one line, 0.3..1 (default 0.7). |
| `sequence` | integer (1..) |  | The sequence; only needed when the project has several. |
| `take` | integer (1..) |  | The take to use, 1-based (default 1: the best). |

```json
{"command":"takes.use","params":{"best":true}}
```

<a id="template.clearFieldKeys-v1"></a>

### `template.clearFieldKeys` v1: Stop Animating a Template Field

Removes every key of a template field and keeps the value it shows at "time" (a timeline time inside the clip; default the clip's start) as its value. One undo step.

Capability `EditSequence`. Tool name `template_clearFieldKeys`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `field` | string | yes | A field id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | The moment whose value stays. Default the clip's start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"template.clearFieldKeys","params":{"clip":42,"field":"accent"}}
```

<a id="template.export-v1"></a>

### `template.export` v1: Export Template

Writes a template (template.list: the project's, the user's or a built-in) to a Wabcut template file (".wabcut-template.json") another Wabcut imports with template.import. Refuses to replace a file unless "overwrite"; a dry run writes nothing.

Capability `Read`. Tool name `template_export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `library` | `project` \| `user` \| `builtin` |  | Only this library. |
| `overwrite` | boolean |  | Replace an existing file. Default false. |
| `path` | string | yes | The file to write. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `template` | string | yes | The template's id. |

```json
{"command":"template.export","params":{"path":"D:/Templates/lower-third.wabcut-template.json","template":"lower-third"}}
```

<a id="template.import-v1"></a>

### `template.import` v1: Import Motion Graphics Template

Adds a template file to the project's library (default) or the user's: a Wabcut template file (".wabcut-template.json", template.export) as it is, or a Motion Graphics template (".mogrt") made in Premiere Pro, approximately - its text Essential Properties become text fields of a lower third Wabcut lays out, the first colour the box's, checkboxes show it; the report lists what was approximated or dropped. Templates made in After Effects are refused (Wabcut cannot run After Effects compositions: export them as Lottie). Files are untrusted: the ZIP is read bounded, with no path leaving it.

Capability `EditProject`. Tool name `template_import`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `id` | string |  | Give it this id (default: the file's). |
| `library` | `project` \| `user` |  | Where it goes. Default "project". |
| `path` | string | yes | The .mogrt or .wabcut-template.json file. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"template.import","params":{"path":"D:/Templates/Lower Third.mogrt"}}
```

<a id="template.importFolder-v1"></a>

### `template.importFolder` v1: Import a Folder of Templates

Adds every Motion Graphics template (.mogrt) and Wabcut template file (.wabcut-template.json) found in a folder and its subfolders (at most 500 files, 8 levels deep, no symbolic links) to the project's library (default) or the user's. Each is filed under a template-browser folder named after where it was found: the chosen folder's own name, then its subfolders ("Lower thirds/Dark"; deeper levels share the third). A file that does not read is skipped and listed with the reason; a template with the id of one already there replaces it. The project's library changes in one undo step.

Capability `EditProject`. Tool name `template_importFolder`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `library` | `project` \| `user` |  | Where they go. Default "project". |
| `path` | string | yes | The folder. |
| `recursive` | boolean |  | Include subfolders. Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"template.importFolder","params":{"path":"D:/Templates/Lower thirds"}}
```

<a id="template.instantiate-v1"></a>

### `template.instantiate` v1: Add Template

Adds a graphic made from a template (template.list) as a clip, with field values (by field id: text a string, colour "#RRGGBB" or [r, g, b, a] 0..1 or {"brand": name}, font a family or {"family", "style", "weight"}, image a media id or name, duration a time, checkbox true/false). Its length: a "clip" duration field, else "duration", else the template's. The project's library is searched first, then the user's, then the built-ins (template.list).

Capability `EditSequence`. Tool name `template_instantiate`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string | yes | Where it starts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `duration` | integer \| string |  | How long it lasts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `library` | `project` \| `user` \| `builtin` |  | Only this library. |
| `placement` | `targeted` \| `freeTrackAtOrAbove` \| `newTrack` |  | "targeted": on "track" (or the targeted video track), overwriting what is there; the default when "track" is given. "freeTrackAtOrAbove": the first unlocked video track from "track" (or the targeted one) upwards with nothing in the graphic's time range, a new top track when none is free; the default without "track", so nothing under the playhead is overwritten. "newTrack": a new video track on top. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `template` | string | yes | The template's id. |
| `track` | string \| integer (1..) |  | A video track. Default: the first targeted unlocked video track. "V1" (bottom video track), "A2", ... or a track id. |
| `values` | object |  | Field values by field id. |

```json
{"command":"template.instantiate","params":{"at":"00:00:12:00","template":"lower-third","track":"V2","values":{"name":"Somchai Jaidee"}}}
```

<a id="template.list-v1"></a>

### `template.list` v1: Templates

The graphic templates of the project's library, the user's and the built-in ones that ship with Wabcut (C.16: gaming lower thirds, banners, callouts, stingers, end cards...; 0.4: essay and documentary name and title, location, quote, source and chapter cards that follow the brand kit): id, name, version, default length, canvas, and the fields (id, label, kind, default value) template.instantiate and template.setField take. Built-ins also give their category and "posterTime" (the design time a thumbnail shows: template.resolve draws it). With "id", that template only, with its graphic.

Capability `Read`. Tool name `template_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `id` | string |  | Only this template, with its graphic. |
| `library` | `project` \| `user` \| `builtin` \| `all` |  | Which library. Default "all". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"template.list","params":{"library":"all"}}
```

<a id="template.removeFieldKey-v1"></a>

### `template.removeFieldKey` v1: Remove a Template Field Key

Removes key `index` of a template field (graphic.get lists the keys in order). One undo step.

Capability `EditSequence`. Tool name `template_removeFieldKey`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `field` | string | yes | A field id. |
| `index` | integer (0..) | yes | Key index (0 = first). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"template.removeFieldKey","params":{"clip":42,"field":"accent","index":0}}
```

<a id="template.resolve-v1"></a>

### `template.resolve` v1: Template Draw List

The draw list of a template at a design time, without adding it to a timeline: what template browsers draw for thumbnails and hover previews (C.16). Default time: the template's poster time (everything has animated in). "values" previews field values. Same items as graphic.resolve.

Capability `Read`. Tool name `template_resolve`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `library` | `project` \| `user` \| `builtin` |  | Only this library. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `template` | string | yes | The template's id. |
| `time` | integer (0..) |  | Design time in ticks from the template's start. Default its poster time. |
| `values` | object |  | Field values by field id, as template.instantiate takes them. |

```json
{"command":"template.resolve","params":{"template":"wabcut.gaming.lowerThird","values":{"name":"Somchai"}}}
```

<a id="template.save-v1"></a>

### `template.save` v1: Save as Template

Saves a graphic clip's graphic as a template with editable fields (Premiere's Motion Graphics Templates): into the project's library (default), or the user's template library folder ("library": "user"; the host's, not a path). Saving under an id in use replaces that template and raises its version.

Capability `EditProject`. Tool name `template_save`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `description` | string |  | A description. |
| `duration` | integer \| string |  | Default clip length (default: the responsive duration, else 5 s). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `fields` | object[] |  | The editable fields; their defaults are what the graphic shows now. |
| `fields[].id` | string | yes | Stable key: [A-Za-z][A-Za-z0-9_.-]*. |
| `fields[].kind` | `text` \| `color` \| `font` \| `image` \| `duration` \| `checkbox` | yes | What it edits. |
| `fields[].label` | string |  | What the Essential Graphics panel shows. |
| `fields[].layer` | integer (0..4294967295) |  | The layer it edits (not for a "clip" duration). |
| `fields[].property` | string | yes | text: "text"; color: "fill", "stroke", "background" or "shadow"; font: "font"; image: "media"; duration: "out" (the layer's out point) or "clip" (the clip's length); checkbox: "visible". |
| `id` | string | yes | The template's id: [A-Za-z0-9][A-Za-z0-9_.-]*, at most 128 bytes. |
| `library` | `project` \| `user` |  | Where it goes. Default "project". |
| `name` | string | yes | Its name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"template.save","params":{"clip":42,"fields":[{"id":"name","kind":"text","label":"Name","layer":2,"property":"text"},{"id":"boxColor","kind":"color","label":"Box","layer":1,"property":"fill"}],"id":"lower-third","name":"Lower third"}}
```

<a id="template.setField-v1"></a>

### `template.setField` v1: Edit Template Fields

Sets field values of a template instance clip (template.list shows the fields): "field" and "value", or several at once in "values" - one undo step. A "clip" duration field trims the clip's end.

Capability `EditSequence`. Tool name `template_setField`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `field` | string |  | A field id. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `value` |  |  | The field's value (as template.instantiate takes it). |
| `values` | object |  | Field values by field id. |

```json
{"command":"template.setField","params":{"clip":42,"field":"name","value":"Malee"}}
```

<a id="template.setFieldKey-v1"></a>

### `template.setFieldKey` v1: Key a Template Field

Animates a field of a template instance clip (template.list shows the fields): from "time" (a timeline time inside the clip) the field shows "value" (as template.instantiate takes it). A colour field blends linearly towards its next key ("interpolation": "hold" steps instead); text, font, picture and checkbox fields switch at their keys. A key at the same time is replaced. Length fields cannot be keyed. One undo step.

Capability `EditSequence`. Tool name `template_setFieldKey`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `field` | string | yes | A field id. |
| `interpolation` | `linear` \| `hold` |  | A colour field's way to the next key. Default linear. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where the key goes (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `value` |  | yes | The field's value at that time. |

```json
{"command":"template.setFieldKey","params":{"clip":42,"field":"accent","time":"00:00:02:00","value":"#FF3366"}}
```

<a id="template.setFolder-v1"></a>

### `template.setFolder` v1: File a Template in a Folder

Files a template under a folder of the template browser ("Titles" or "Titles/Lower thirds": up to three levels separated by "/"); an empty "folder" takes it out of its folder. A project template changes in one undo step (its version stays); a template of the user's library is rewritten in the library folder. Built-in templates cannot be filed.

Capability `EditProject`. Tool name `template_setFolder`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `folder` | string | yes | The folder label, or "" for none. |
| `library` | `project` \| `user` |  | Where it is. Default "project". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `template` | string | yes | The template's id. |

```json
{"command":"template.setFolder","params":{"folder":"Titles/Lower thirds","template":"lower-third"}}
```

<a id="textStyle.apply-v1"></a>

### `textStyle.apply` v1: Apply Text Style

Gives text layers of a graphic clip a project text style; they follow it (updating the style restyles them).

Capability `EditSequence`. Tool name `textStyle_apply`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `layers` | (integer (1..))[], min 1 | yes | The text layers. |
| `name` | string | yes | The text style. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"textStyle.apply","params":{"clip":42,"layers":[2,3],"name":"Lower third name"}}
```

<a id="textStyle.applyRange-v1"></a>

### `textStyle.applyRange` v1: Apply Text Style to Part of a Text

Styles part of a text layer with a project text style: the characters [from, to) of the layer's text (Unicode characters, a line break counts one) take the style's look (font, weight, size, fill, italic, underline, tracking, leading, baseline shift), the rest of the text, the layer's alignment, background and shadow stay, and the layer does not follow the style. With Source Text keys it styles the text shown at "time". One undo step.

Capability `EditSequence`. Tool name `textStyle_applyRange`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `from` | integer (0..) | yes | First character of the range (0 = the first character). |
| `layer` | integer (1..) | yes | The text layer. A layer id. |
| `name` | string | yes | The text style. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | With Source Text keys: the moment whose text is styled. Default the clip's start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `to` | integer (0..) | yes | The character after the last one of the range. |

```json
{"command":"textStyle.applyRange","params":{"clip":42,"from":6,"layer":2,"name":"Highlight","to":11}}
```

<a id="textStyle.create-v1"></a>

### `textStyle.create` v1: Create Text Style

Premiere's Create Text Style: the look of a text layer (its first run's font, size, fill, tracking...; its alignment, background and shadow) saved in the project under a name; the layer follows it.

Capability `EditSequence`. Tool name `textStyle_create`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The graphic clip. A clip id, or {"track", "time"} for the clip there. |
| `layer` | integer (1..) | yes | The text layer. A layer id. |
| `name` | string | yes | The style's name (unique). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"textStyle.create","params":{"clip":42,"layer":2,"name":"Lower third name"}}
```

<a id="textStyle.list-v1"></a>

### `textStyle.list` v1: Text Styles

The project's text styles (Premiere's Text Style items): each with its look (the text JSON of a layer with one run: font, size, fill, tracking...; alignment, background; shadow) and how many text layers follow it.

Capability `Read`. Tool name `textStyle_list`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"textStyle.list","params":{}}
```

<a id="textStyle.move-v1"></a>

### `textStyle.move` v1: Reorder Text Styles

Moves a project text style to another place in the list (0 = first), the order the Library lists them in.

Capability `EditProject`. Tool name `textStyle_move`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `index` | integer (0..) | yes | New position (0 = first). |
| `name` | string | yes | The text style. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"textStyle.move","params":{"index":0,"name":"Name"}}
```

<a id="textStyle.remove-v1"></a>

### `textStyle.remove` v1: Delete Text Style

Deletes a project text style: the layers that followed it keep their look and follow nothing.

Capability `EditProject`. Tool name `textStyle_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `name` | string | yes | The text style. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"textStyle.remove","params":{"name":"Name"}}
```

<a id="textStyle.rename-v1"></a>

### `textStyle.rename` v1: Rename Text Style

Renames a project text style; the layers that follow it follow the new name.

Capability `EditProject`. Tool name `textStyle_rename`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `name` | string | yes | The text style. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | string | yes | The new name. |

```json
{"command":"textStyle.rename","params":{"name":"Lower third name","to":"Name"}}
```

<a id="textStyle.update-v1"></a>

### `textStyle.update` v1: Push to Text Style

Updates a project text style - Premiere's Push to Text Style - from a text layer's current look ("clip" and "layer") or from its JSON ("style", as textStyle.list gives it), and restyles every text layer of every graphic clip that follows it. Makes the style when there is none of that name.

Capability `EditProject`. Tool name `textStyle_update`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | A graphic clip whose text layer has the look. A clip id, or {"track", "time"} for the clip there. |
| `layer` | integer (1..) |  | That text layer. A layer id. |
| `name` | string | yes | The text style. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `style` | object |  | Or the look as JSON (textStyle.list's form, without the name). |

```json
{"command":"textStyle.update","params":{"clip":42,"layer":2,"name":"Lower third name"}}
```

<a id="timeRemap.addHold-v1"></a>

### `timeRemap.addHold` v1: Add Hold Segment

Holds the frame shown at a timeline time for a duration inside a remapped clip; the rest of its remap plays later.

Capability `EditSequence`. Tool name `timeRemap_addHold`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `duration` | integer \| string | yes | How long the frame holds. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `extendClip` | boolean |  | Lengthen the clip by the time the remap gains (or shorten it by what it loses), moving later clips like a ripple trim. Default false: the clip keeps its length. |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where the hold starts (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"timeRemap.addHold","params":{"clip":42,"duration":"00:00:01:00","extendClip":true,"time":"00:00:02:00"}}
```

<a id="timeRemap.addKey-v1"></a>

### `timeRemap.addKey` v1: Add Time Remap Key

Adds a remap key at the sampled source position of a timeline time inside a remapped clip. Holds stay holds; the new key is otherwise linear and can reshape a smooth ramp. Returns its index.

Capability `EditSequence`. Tool name `timeRemap_addKey`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where the key goes (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"timeRemap.addKey","params":{"clip":42,"time":"00:00:02:00"}}
```

<a id="timeRemap.addReverse-v1"></a>

### `timeRemap.addReverse` v1: Add Reverse Segment

From a timeline time inside a remapped clip, plays the media backwards at 100 % for a duration, then forwards again over the same frames (a boomerang); the rest of the remap plays later by twice the duration. Needs that much media before the point.

Capability `EditSequence`. Tool name `timeRemap_addReverse`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `duration` | integer \| string | yes | How long it plays backwards. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `extendClip` | boolean |  | Lengthen the clip by the time the remap gains (or shorten it by what it loses), moving later clips like a ripple trim. Default false: the clip keeps its length. |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where the reverse starts (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"timeRemap.addReverse","params":{"clip":42,"duration":"00:00:00:12","time":"00:00:02:00"}}
```

<a id="timeRemap.enable-v1"></a>

### `timeRemap.enable` v1: Time Remapping

Turns time remapping on (speed ramps, holds and reverse inside one clip; the clip shows the same frames, also beyond its ends for transitions and trims; keyframes of a clip that was not at 100 % then animate at timeline rate) or off (back to a constant speed: the one it shows if a single straight segment covers it, else 100 % from its first frame). Audio follows the remap (A.10): pitch kept (Maintain Audio Pitch, on by default when remapping starts) or varispeed; holds are silent.

Capability `EditSequence`. Tool name `timeRemap_enable`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `enabled` | boolean |  | true (default) on, false off. |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `maintainPitch` | boolean |  | Audio: keep the pitch in ramps (time-stretch) instead of varispeed. Default true when turning remapping on. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"timeRemap.enable","params":{"clip":42,"enabled":true}}
```

<a id="timeRemap.moveKey-v1"></a>

### `timeRemap.moveKey` v1: Move Time Remap Key

Moves a remap key to another timeline time. Without "ripple" it stays between its neighbours and both segments around it change speed; with "ripple" the later keys move too, so only the segment before it changes speed (Premiere's speed keyframe drag).

Capability `EditSequence`. Tool name `timeRemap_moveKey`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `extendClip` | boolean |  | Lengthen the clip by the time the remap gains (or shorten it by what it loses), moving later clips like a ripple trim. Default false: the clip keeps its length. |
| `index` | integer (0..) | yes | Remap key index (0 = first; clip.details lists them). |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `ripple` | boolean |  | Move the later keys by as much (default false). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | The new time (inside the clip). Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"timeRemap.moveKey","params":{"clip":42,"index":1,"ripple":true,"time":"00:00:03:00"}}
```

<a id="timeRemap.removeKey-v1"></a>

### `timeRemap.removeKey` v1: Remove Time Remap Key

Removes a remap key (not the only one).

Capability `EditSequence`. Tool name `timeRemap_removeKey`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `index` | integer (0..) | yes | Remap key index (0 = first; clip.details lists them). |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"timeRemap.removeKey","params":{"clip":42,"index":1}}
```

<a id="timeRemap.setInterpolation-v1"></a>

### `timeRemap.setInterpolation` v1: Set Time Remap Interpolation

Shapes the remap around keys: linear (constant speed between keys), hold (the frame holds until the next key, then jumps), or easeIn / easeOut / easeInOut / bezier for a smooth speed ramp.

Capability `EditSequence`. Tool name `timeRemap_setInterpolation`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` | yes | The new interpolation. |
| `keys` | (integer (0..))[] |  | The keys. Default: all of them. |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"timeRemap.setInterpolation","params":{"clip":42,"interpolation":"easeInOut","keys":[1,2]}}
```

<a id="timeRemap.setKey-v1"></a>

### `timeRemap.setKey` v1: Set Time Remap Key

Changes a remap key's source position, interpolation and Bezier handles together. Omitted fields keep their values. Slopes are source ticks per second of curve time (254016000000 = 100% forwards); influence is a fraction of the adjacent segment. Source bounds and monotonic segments are validated.

Capability `EditSequence`. Tool name `timeRemap_setKey`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `inInfluence` | number (0.0..1.0) |  | Bezier: arriving influence. |
| `inSlope` | number |  | Bezier: arriving slope in source ticks per second. |
| `index` | integer (0..) | yes | Remap key index (0 = first; clip.details lists them). |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` |  | The key's interpolation. |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `outInfluence` | number (0.0..1.0) |  | Bezier: leaving influence. |
| `outSlope` | number |  | Bezier: leaving slope in source ticks per second. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `source` | integer (0..9007199254740992) |  | Source position in whole ticks (not timeline time). |

```json
{"command":"timeRemap.setKey","params":{"clip":42,"index":1,"interpolation":"bezier","outSlope":254016000000}}
```

<a id="timeRemap.setSpeed-v1"></a>

### `timeRemap.setSpeed` v1: Set Segment Speed

Sets the speed of one segment of a remapped clip (between two keys; the one playing at "time", or "segment" by index): its end key and the keys after it move, so later material plays earlier or later.

Capability `EditSequence`. Tool name `timeRemap_setSpeed`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `extendClip` | boolean |  | Lengthen the clip by the time the remap gains (or shorten it by what it loses), moving later clips like a ripple trim. Default false: the clip keeps its length. |
| `linked` | boolean |  | Linked partners take part too (default true). |
| `segment` | integer (0..) |  | Segment index (key index to the next key). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speed` | number (0.001..100000.0) | yes | Speed in percent (positive; the direction stays). |
| `time` | integer \| string |  | Or a timeline time inside the segment. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"timeRemap.setSpeed","params":{"clip":42,"speed":50,"time":"00:00:01:00"}}
```

<a id="timeline.removeSilences-v1"></a>

### `timeline.removeSilences` v1: Remove Silences

Ripple-deletes a list of time ranges (e.g. silences found in a transcript) in one step. Only whole frames inside each range are removed. By default every track loses the same time so everything stays in sync; In/Out marks and track targeting are left as they were.

Capability `EditSequence`. Tool name `timeline_removeSilences`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `ranges` | object[], min 1 | yes | Ranges to remove, in sequence time, any order. |
| `ranges[].end` | integer \| string | yes | Range end (exclusive). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `ranges[].start` | integer \| string | yes | Range start. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (string \| integer (1..))[] |  | Tracks to remove the ranges from. Default: all tracks. |

```json
{"command":"timeline.removeSilences","params":{"ranges":[{"end":"00:00:02:20","start":"00:00:01:10"},{"end":"00:00:07:15","start":"00:00:07:00"}]}}
```

<a id="timeline.rippleDelete-v1"></a>

### `timeline.rippleDelete` v1: Ripple Delete

Ripple delete: removes clips and closes the time they used, or closes a gap. Sync-locked tracks lose the same time. A gap only closes when no sync-locked track has material in it.

Capability `EditSequence`. Tool name `timeline_rippleDelete`. Aliases: `edit.rippleDelete`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 |  | Clips to remove (use this or gap). |
| `gap` | object |  | A gap to close (use this or clips). |
| `gap.time` | integer \| string | yes | A time inside the gap. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `gap.track` | string \| integer (1..) | yes | Track of the gap. "V1" (bottom video track), "A2", ... or a track id. |
| `linked` | boolean |  | Include linked partners (Premiere's linked selection). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"timeline.rippleDelete","params":{"clips":[42]}}
```

<a id="title.add-v1"></a>

### `title.add` v1: New Title

Adds a text title as a clip on a video track: a title media item holding the text and font, and the clip's "wabcut.title.style" effect holding size, fill and stroke colours, stroke width, shadow, alignment, line spacing and tracking. Place, scale and rotate it with the clip's Transform (clip.setParam motion.*). "placement" chooses the track: the named one (overwriting; the default when "track" is given), the first free one at or above the named or targeted one (the default without "track": nothing under the title is overwritten), or a new track on top. createdIds: the clip, the style effect, the title media.

Capability `EditSequence`. Tool name `title_add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `at` | integer \| string | yes | Where the title starts. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `duration` | integer \| string |  | How long it lasts. Default 5 seconds. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `fontFamily` | string |  | Font family. Default "Arial". |
| `fontStyle` | string |  | Font style. Default "Regular". |
| `placement` | `targeted` \| `freeTrackAtOrAbove` \| `newTrack` |  | "targeted": on "track", overwriting what is there (the default when "track" is given). "freeTrackAtOrAbove": the first unlocked video track from "track" (or the targeted one) upwards with nothing in the title's time range, a new top track when none is free (the default without "track"). "newTrack": a new video track on top. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `style` | object |  | Style values by parameter key ("wabcut.title.style"); others take their defaults. Animate them later with clip.addKeyframe on the style effect. |
| `style.alignment` | number (0.0..2.0) |  | Alignment (amount, default 1). |
| `style.fill.alpha` | number (0.0..1.0) |  | Fill Opacity (fraction, default 1). |
| `style.fill.blue` | number (0.0..1.0) |  | Fill Blue (fraction, default 1). |
| `style.fill.green` | number (0.0..1.0) |  | Fill Green (fraction, default 1). |
| `style.fill.red` | number (0.0..1.0) |  | Fill Red (fraction, default 1). |
| `style.lineSpacing` | number (0.1..10.0) |  | Line Spacing (factor, default 1). |
| `style.shadow.angle` | number (-3600.0..3600.0) |  | Shadow Angle (deg, default 135). |
| `style.shadow.blue` | number (0.0..1.0) |  | Shadow Blue (fraction, default 0). |
| `style.shadow.distance` | number (0.0..4000.0) |  | Shadow Distance (px, default 10). |
| `style.shadow.green` | number (0.0..1.0) |  | Shadow Green (fraction, default 0). |
| `style.shadow.opacity` | number (0.0..1.0) |  | Shadow Opacity (fraction, default 0). |
| `style.shadow.red` | number (0.0..1.0) |  | Shadow Red (fraction, default 0). |
| `style.shadow.softness` | number (0.0..1000.0) |  | Shadow Softness (px, default 10). |
| `style.size` | number (1.0..2000.0) |  | Font Size (px, default 100). |
| `style.stroke.alpha` | number (0.0..1.0) |  | Stroke Opacity (fraction, default 1). |
| `style.stroke.blue` | number (0.0..1.0) |  | Stroke Blue (fraction, default 0). |
| `style.stroke.green` | number (0.0..1.0) |  | Stroke Green (fraction, default 0). |
| `style.stroke.red` | number (0.0..1.0) |  | Stroke Red (fraction, default 0). |
| `style.stroke.width` | number (0.0..500.0) |  | Stroke Width (px, default 0). |
| `style.tracking` | number (-1000.0..10000.0) |  | Tracking (amount, default 0). |
| `text` | string | yes | The text (UTF-8; a newline starts a new line). |
| `track` | string \| integer (1..) |  | A video track. Default: the first targeted unlocked video track. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"title.add","params":{"at":"00:00:10:00","style":{"size":120},"text":"Chapter One","track":"V2"}}
```

<a id="title.setFont-v1"></a>

### `title.setFont` v1: Set Title Font

Changes a title's font family and/or style.

Capability `EditSequence`. Tool name `title_setFont`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | A clip of the title. A clip id, or {"track", "time"} for the clip there. |
| `fontFamily` | string |  | Font family, e.g. "Noto Sans Thai". |
| `fontStyle` | string |  | Font style, e.g. "Bold". |
| `media` | integer \| string (1..) |  | Or the title's media item. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"title.setFont","params":{"clip":42,"fontFamily":"Noto Sans Thai","fontStyle":"Bold"}}
```

<a id="title.setText-v1"></a>

### `title.setText` v1: Edit Title Text

Replaces a title's text. Every clip of the title shows the new text.

Capability `EditSequence`. Tool name `title_setText`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) |  | A clip of the title. A clip id, or {"track", "time"} for the clip there. |
| `media` | integer \| string (1..) |  | Or the title's media item. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `text` | string | yes | The new text (UTF-8; a newline starts a new line). |

```json
{"command":"title.setText","params":{"clip":42,"text":"Chapter Two"}}
```

<a id="track.addEffect-v1"></a>

### `track.addEffect` v1: Add Video Track Effect

Adds a video effect to a video track's stack: it applies to the composite of every clip on the track (a colour grade or blur for the whole track at once), after the clips' own effects. Parameters not given take their defaults. The new instance id is returned in createdIds. One undo step.

Capability `EditSequence`. Tool name `track_addEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | `wabcut.color.basic` \| `wabcut.vignette` \| `wabcut.color.grade` \| `wabcut.blur.gaussian` \| `wabcut.sharpen` \| `wabcut.crop` \| `wabcut.dropShadow` \| `wabcut.title.style` \| `wabcut.mask` \| `wabcut.mosaic` \| `wabcut.key.trackMatte` \| `wabcut.border` \| `wabcut.key.ultra` \| `wabcut.adjust.levels` \| `wabcut.adjust.procAmp` \| `wabcut.adjust.extract` \| `wabcut.adjust.convolutionKernel` \| `wabcut.channel.invert` \| `wabcut.color.brightnessContrast` \| `wabcut.color.tint` \| `wabcut.color.changeColor` \| `wabcut.color.changeToColor` \| `wabcut.color.leaveColor` \| `wabcut.color.balance` \| `wabcut.color.videoLimiter` \| `wabcut.image.blackWhite` \| `wabcut.image.colorPass` \| `wabcut.image.colorReplace` \| `wabcut.image.gammaCorrection` \| `wabcut.key.alphaAdjust` \| `wabcut.key.luma` \| `wabcut.noise.noise` \| `wabcut.stylize.posterize` \| `wabcut.stylize.solarize` \| `wabcut.stylize.strobeLight` \| `wabcut.stylize.threshold` \| `wabcut.stylize.replicate` \| `wabcut.blur.directional` \| `wabcut.blur.camera` \| `wabcut.vr.reorient` \| `wabcut.distort.cornerPin` \| `wabcut.distort.lensDistortion` \| `wabcut.distort.magnify` \| `wabcut.distort.mirror` \| `wabcut.distort.offset` \| `wabcut.distort.spherize` \| `wabcut.distort.twirl` \| `wabcut.distort.waveWarp` \| `wabcut.distort.turbulentDisplace` \| `wabcut.distort.transform` \| `wabcut.transform.horizontalFlip` \| `wabcut.transform.verticalFlip` \| `wabcut.transform.edgeFeather` \| `wabcut.perspective.basic3d` \| `wabcut.perspective.bevelEdges` \| `wabcut.perspective.bevelAlpha` \| `wabcut.perspective.radialShadow` \| `wabcut.blur.unsharpMask` \| `wabcut.retouch.skinSmoothing` \| `wabcut.stylize.alphaGlow` \| `wabcut.stylize.glow` \| `wabcut.stylize.colorEmboss` \| `wabcut.stylize.emboss` \| `wabcut.stylize.findEdges` \| `wabcut.stylize.roughenEdges` \| `wabcut.noise.dustScratches` \| `wabcut.noise.median` \| `wabcut.key.colorKey` \| `wabcut.generate.fourColorGradient` \| `wabcut.generate.checkerboard` \| `wabcut.generate.circle` \| `wabcut.generate.grid` \| `wabcut.generate.lensFlare` \| `wabcut.generate.ramp` \| `wabcut.generate.colorama` \| `wabcut.generate.fractalNoise` \| `wabcut.generate.cellPattern` \| `wabcut.generate.lightning` \| `wabcut.generate.scribble` \| `wabcut.generate.paintBucket` \| `wabcut.transition.blockDissolve` \| `wabcut.transition.gradientWipe` \| `wabcut.transition.linearWipe` \| `wabcut.transition.radialWipe` \| `wabcut.transition.venetianBlinds` \| `wabcut.key.differenceMatte` \| `wabcut.key.imageMatte` \| `wabcut.time.echo` \| `wabcut.time.posterizeTime` \| `wabcut.video.clipName` \| `wabcut.video.simpleText` \| `wabcut.video.timecode` \| `wabcut.audio.gain` \| `wabcut.audio.eq3` \| `wabcut.audio.compressor` \| `wabcut.audio.limiter` \| `wabcut.audio.denoise` \| `wabcut.audio.enhanceSpeech` \| `wabcut.audio.filter` \| `wabcut.audio.dehum` \| `wabcut.audio.deess` \| `wabcut.audio.expander` \| `wabcut.audio.reverb` \| `wabcut.audio.width` \| `wabcut.audio.essentialSound` \| `wabcut.audio.amplify` \| `wabcut.audio.channelMixer` \| `wabcut.audio.dynamics` \| `wabcut.audio.hardLimiter` \| `wabcut.audio.multibandCompressor` \| `wabcut.audio.dynamicsProcessing` \| `wabcut.audio.singleBandCompressor` \| `wabcut.audio.tubeModeledCompressor` \| `wabcut.audio.distortion` \| `wabcut.audio.guitarSuite` \| `wabcut.audio.mastering` \| `wabcut.audio.vocalEnhancer` \| `wabcut.audio.pitchShifter` \| `wabcut.audio.graphicEq10` \| `wabcut.audio.graphicEq20` \| `wabcut.audio.graphicEq30` \| `wabcut.audio.parametricEq` \| `wabcut.audio.simpleParametricEq` \| `wabcut.audio.highpass` \| `wabcut.audio.lowpass` \| `wabcut.audio.bandpass` \| `wabcut.audio.simpleNotch` \| `wabcut.audio.bass` \| `wabcut.audio.treble` \| `wabcut.audio.notchFilter` \| `wabcut.audio.scientificFilter` \| `wabcut.audio.fftFilter` \| `wabcut.audio.stereoExpander` \| `wabcut.audio.balance` \| `wabcut.audio.channelVolume` \| `wabcut.audio.fillLeftWithRight` \| `wabcut.audio.fillRightWithLeft` \| `wabcut.audio.invert` \| `wabcut.audio.swapChannels` \| `wabcut.audio.delay` \| `wabcut.audio.analogDelay` \| `wabcut.audio.multitapDelay` \| `wabcut.audio.chorusFlanger` \| `wabcut.audio.flanger` \| `wabcut.audio.phaser` \| `wabcut.audio.convolutionReverb` \| `wabcut.audio.studioReverb` \| `wabcut.audio.surroundReverb` \| `wabcut.audio.deReverb` \| `wabcut.audio.spectralDenoise` \| `wabcut.audio.adaptiveNoiseReduction` \| `wabcut.audio.automaticClickRemover` \| `wabcut.audio.spectralRepair` \| `wabcut.blur.motion` \| `wabcut.stabilize.warp` \| `wabcut.stabilize.rollingShutter` \| `wabcut.ai.restore` \| `wabcut.ai.denoise` \| `wabcut.ai.refocus` \| `wabcut.ai.faceRetouch` \| `wabcut.ai.eyeContact` | yes | The effect id (effect.list with kind "video"). |
| `index` | integer (0..) |  | Position in the stack (0 = first). Default: last. |
| `params` | object |  | Constant parameter values by key, e.g. {"radius": 12}. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) | yes | The video track (V1, V2, ...). "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"track.addEffect","params":{"effect":"wabcut.blur.gaussian","params":{"radius":12},"track":"V1"}}
```

<a id="track.addKeyframe-v1"></a>

### `track.addKeyframe` v1: Add Video Track Effect Keyframe

Adds a keyframe to a parameter of a video track's effect at a timeline time (or replaces the one there). A constant parameter becomes animated with just this key.

Capability `EditSequence`. Tool name `track_addKeyframe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) | yes | The effect. The effect instance id (clip.details lists them). |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` |  | Shape after this key. Default linear. |
| `param` | string | yes | The effect's parameter key (effect.list). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string | yes | Where the key goes. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `track` | string \| integer (1..) | yes | The video track (V1, V2, ...). "V1" (bottom video track), "A2", ... or a track id. |
| `value` | number | yes | The value, in the parameter's unit. |

```json
{"command":"track.addKeyframe","params":{"effect":77,"param":"radius","time":"00:00:04:00","track":"V1","value":0.0}}
```

<a id="track.addSend-v1"></a>

### `track.addSend` v1: Add Track Send

A send from an audio track to a submix (Premiere's Audio Track Mixer sends; at most 5 per track): a copy of the track's sound at its own level and pan, post-fader (follows the track's fader) unless "preFader". A muted or soloed-away track is silent in its sends too. Use it for a shared reverb: send each voice to a submix with the reverb. One undo step.

Capability `EditSequence`. Tool name `track_addSend`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `level` | number (-144.0..24.0) |  | Send volume, dB (-144 = silent .. +24; 0 = unity). |
| `muted` | boolean |  | Mute the send. |
| `pan` | number (-1.0..1.0) |  | Send pan: -1 left .. 1 right. |
| `preFader` | boolean |  | Pre-Fader: ignore the track's fader and panner (default post-fader). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `submix` | integer \| string | yes | The submix track ("S1"). A label ("A1") or a track id. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |

```json
{"command":"track.addSend","params":{"level":-6,"submix":"S1","track":"A1"}}
```

<a id="track.effects-v1"></a>

### `track.effects` v1: Video Track Effect Stack

The effects on a video track: instance id, effect id, name, enabled and the parameters (constants, or {"keys": [...]} in timeline ticks).

Capability `Read`. Tool name `track_effects`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) | yes | The video track (V1, V2, ...). "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"track.effects","params":{"track":"V1"}}
```

<a id="track.mask-v1"></a>

### `track.mask` v1: Track Mask

Tracks a mask through the clip's media, frame by frame from "time" (forward or backward, to the clip's end or "frames" frames), with pyramidal Lucas-Kanade on features inside the mask and a robust fit of its motion ("model"). Its transform.* parameters get ordinary keyframes (one undo step). Stops where the picture is lost (confidence below "minConfidence"); data lists each frame's confidence. A job: it reports progress and can be cancelled.

Capability `EditSequence`. Tool name `track_mask`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `direction` | `forward` \| `backward` |  | Default forward. |
| `effect` | integer (1..) |  | The effect instance id (clip.details lists them). mask.add: omit for the clip's opacity mask (cut-outs); the others: default the effect holding the mask. |
| `frames` | integer (1..) |  | Track at most this many frames (1 = one step). Default to the clip's end. |
| `mask` | integer (1..) | yes | The mask id. |
| `minConfidence` | number (0.0..1.0) |  | Stop where the confidence falls below this. Default 0.3. |
| `model` | `translation` \| `similarity` \| `affine` |  | What the region may do: move; move, turn and scale; or any affine. Default similarity. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Where to start (inside the clip). Default the clip's first frame (backward: its last). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `workingWidth` | integer (64..4096) |  | Width of the pictures the tracker looks at. Default 640. |

```json
{"command":"track.mask","params":{"clip":42,"direction":"forward","mask":1}}
```

<a id="track.moveEffect-v1"></a>

### `track.moveEffect` v1: Reorder Video Track Effect

Moves an effect to another position in a video track's stack.

Capability `EditSequence`. Tool name `track_moveEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) | yes | The effect to move. The effect instance id (clip.details lists them). |
| `index` | integer (0..) | yes | New position (0 = first). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) | yes | The video track (V1, V2, ...). "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"track.moveEffect","params":{"effect":77,"index":0,"track":"V1"}}
```

<a id="track.removeEffect-v1"></a>

### `track.removeEffect` v1: Remove Video Track Effect

Removes an effect from a video track's stack.

Capability `EditSequence`. Tool name `track_removeEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) | yes | The effect to remove. The effect instance id (clip.details lists them). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) | yes | The video track (V1, V2, ...). "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"track.removeEffect","params":{"effect":77,"track":"V1"}}
```

<a id="track.removeKeyframe-v1"></a>

### `track.removeKeyframe` v1: Remove Video Track Effect Keyframe

Removes keyframe `index` of a video track effect's parameter; the last one leaves a constant.

Capability `EditSequence`. Tool name `track_removeKeyframe`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) | yes | The effect. The effect instance id (clip.details lists them). |
| `index` | integer (0..) | yes | Keyframe index (0 = first; track.effects lists the keys). |
| `param` | string | yes | The effect's parameter key. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) | yes | The video track (V1, V2, ...). "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"track.removeKeyframe","params":{"effect":77,"index":0,"param":"radius","track":"V1"}}
```

<a id="track.removeSend-v1"></a>

### `track.removeSend` v1: Remove Track Send

Removes send "send" (1..5) of an audio track; the later sends move up. One undo step.

Capability `EditSequence`. Tool name `track_removeSend`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `send` | integer (1..5) | yes | The send, 1..5. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |

```json
{"command":"track.removeSend","params":{"send":1,"track":"A1"}}
```

<a id="track.setEffect-v1"></a>

### `track.setEffect` v1: Set Video Track Effect

Turns an effect of a video track's stack on or off, or sets constant parameters (animate them with track.addKeyframe).

Capability `EditSequence`. Tool name `track_setEffect`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `effect` | integer (1..) | yes | The effect. The effect instance id (clip.details lists them). |
| `enabled` | boolean |  | On or off. |
| `params` | object |  | Parameter values by key. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | string \| integer (1..) | yes | The video track (V1, V2, ...). "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"track.setEffect","params":{"effect":77,"params":{"radius":20},"track":"V1"}}
```

<a id="track.setFormat-v1"></a>

### `track.setFormat` v1: Set Track Channel Format

An audio track's channel format (Premiere's mono, stereo, 5.1 and adaptive tracks, and 7.1): clips are mixed to it (mono averages, 5.1 and 7.1 to stereo are ITU-R BS.775), and it to the master's format; adaptive passes channels through one to one. One undo step.

Capability `EditSequence`. Tool name `track_setFormat`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `mono` \| `stereo` \| `5.1` \| `7.1` \| `adaptive` | yes | The format. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |

```json
{"command":"track.setFormat","params":{"format":"mono","track":"A2"}}
```

<a id="track.setOutput-v1"></a>

### `track.setOutput` v1: Set Track Output

Premiere's track output assignment: where an audio track or a submix goes, "mix" (the master) or a submix track ("S1"). A submix cannot output into itself or into a submix that feeds it. One undo step.

Capability `EditSequence`. Tool name `track_setOutput`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `output` | integer \| string | yes | "mix", or a submix track ("S1" or its id). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | integer \| string | yes | The audio track or submix ("S1"). A label ("A1") or a track id. |

```json
{"command":"track.setOutput","params":{"output":"S1","track":"A2"}}
```

<a id="track.setPan-v1"></a>

### `track.setPan` v1: Set Track Pan

Sets an audio track's panner: -1 = left .. 0 = centre .. +1 = right, a balance on the track's stereo mix (centre leaves it unchanged; towards a side the other side follows the -3 dB constant-power curve down to silence). With "time" it sets a keyframe there. One undo step.

Capability `EditSequence`. Tool name `track_setPan`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` |  | The keyframe's interpolation. Default linear. |
| `pan` | number (-1.0..1.0) | yes | The balance, -1 .. +1. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Put a keyframe here instead of setting a constant (the fader becomes animated). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |

```json
{"command":"track.setPan","params":{"pan":-0.3,"track":"A3"}}
```

<a id="track.setRecordArm-v1"></a>

### `track.setRecordArm` v1: Set Track Record Arm

Arms an audio track for voice-over recording or disarms it (the R button of Premiere's track headers and Audio Track Mixer). The app's Record (record.start) records the microphone onto the armed track (else the targeted one) at the playhead or between the sequence In and Out; recording is the user's action in the app, never a command. Allowed on locked tracks; video tracks are refused. One undo step.

Capability `EditSequence`. Tool name `track_setRecordArm`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `armed` | boolean |  | Arm (default true) or disarm. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |

```json
{"command":"track.setRecordArm","params":{"armed":true,"track":"A2"}}
```

<a id="track.setRole-v1"></a>

### `track.setRole` v1: Set Track Role

What an audio track carries: dialogue, music or effects (Premiere's Essential Sound types), or none. audio.autoDuck lowers music-role tracks under dialogue-role tracks.

Capability `EditSequence`. Tool name `track_setRole`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `role` | `none` \| `dialogue` \| `music` \| `effects` | yes | The role. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |

```json
{"command":"track.setRole","params":{"role":"dialogue","track":"A1"}}
```

<a id="track.setSend-v1"></a>

### `track.setSend` v1: Set Track Send

Changes send "send" (1..5) of an audio track: its submix, pre- or post-fader, mute, level (dB) and pan; with "time" the level and pan given become keyframes there (send automation). One undo step.

Capability `EditSequence`. Tool name `track_setSend`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `level` | number (-144.0..24.0) |  | Send volume, dB (-144 = silent .. +24; 0 = unity). |
| `muted` | boolean |  | Mute the send. |
| `pan` | number (-1.0..1.0) |  | Send pan: -1 left .. 1 right. |
| `preFader` | boolean |  | Pre-Fader (true) or Post-Fader (false). |
| `send` | integer (1..5) | yes | The send, 1..5. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `submix` | integer \| string |  | Another submix track. A label ("A1") or a track id. |
| `time` | integer \| string |  | Put the level and pan as keyframes here instead of constants. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |

```json
{"command":"track.setSend","params":{"preFader":true,"send":1,"track":"A1"}}
```

<a id="track.setState-v1"></a>

### `track.setState` v1: Set Track State

Changes track header toggles: lock, target, sync lock, mute, solo, output, name, label colour. Unlocking is how a locked track becomes editable again.

Capability `EditSequence`. Tool name `track_setState`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `colorIndex` | integer (-1..15) |  | The track's label colour, 0-15 (the clip label colours; shown as a stripe on the header and a wash on the lane), or -1 for none. |
| `enabled` | boolean |  | Track output on. |
| `locked` | boolean |  | Track lock. |
| `muted` | boolean |  | Mute (audio). |
| `name` | string |  | New name ("" for the default). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `solo` | boolean |  | Solo: while any track of a kind is soloed, only the soloed ones are heard (audio) or shown (video). |
| `syncLocked` | boolean |  | Sync lock. |
| `targeted` | boolean |  | Track targeting. |
| `track` | string \| integer (1..) | yes | The track. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"track.setState","params":{"locked":true,"track":"A2"}}
```

<a id="track.setSurroundPan-v1"></a>

### `track.setSurroundPan` v1: Set Track Surround Panner

The surround panner of a mono or stereo track on a 5.1 or 7.1 master (Premiere's track 5.1 panner): the puck x (-1 left .. +1 right) and y (-1 back .. +1 front; front centre plays stereo on L/R and mono on C) and the LFE send in dB (-144 = off). One undo step.

Capability `EditSequence`. Tool name `track_setSurroundPan`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `lfeDb` | number (-144.0..0.0) |  | The LFE send, dB. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |
| `x` | number (-1.0..1.0) |  | Left .. right. |
| `y` | number (-1.0..1.0) |  | Back .. front. |

```json
{"command":"track.setSurroundPan","params":{"track":"A1","x":-0.5,"y":0.5}}
```

<a id="track.setVolume-v1"></a>

### `track.setVolume` v1: Set Track Volume

Sets an audio track's fader (Premiere's track Volume): dB, 0 = unity, applied to the track's mix after its effects. With "time" it sets a keyframe there (track automation, in timeline time) instead of a constant; a constant replaces the track's volume keyframes. One undo step.

Capability `EditSequence`. Tool name `track_setVolume`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `db` | number (-144.0..24.0) | yes | The level in dB (-144 = silence .. +24). |
| `interpolation` | `hold` \| `linear` \| `bezier` \| `easeIn` \| `easeOut` \| `easeInOut` |  | The keyframe's interpolation. Default linear. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `time` | integer \| string |  | Put a keyframe here instead of setting a constant (the fader becomes animated). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `track` | integer \| string | yes | The audio track. A label ("A1") or a track id. |

```json
{"command":"track.setVolume","params":{"db":-6,"track":"A2"}}
```

<a id="track.transform-v1"></a>

### `track.transform` v1: Track Motion

Tracks a region of a clip ("region": {x, y, width, height} 0..1 of the frame, {"mask": id}, or {"detect": "face"} once a detector is installed) and keys a clip's Motion with it (one undo step): "follow" moves "target" with the region (position; with the similarity model also scale and rotation), "stabilize" keys the tracked clip so the region stays still. A job: it reports progress and can be cancelled.

Capability `EditSequence`. Tool name `track_transform`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clip` | integer \| object (1..) | yes | The tracked clip. A clip id, or {"track", "time"} for the clip there. |
| `direction` | `forward` \| `backward` |  | Default forward. |
| `frames` | integer (1..) |  | Track at most this many frames (1 = one step). Default to the clip's end. |
| `minConfidence` | number (0.0..1.0) |  | Stop where the confidence falls below this. Default 0.3. |
| `mode` | `follow` \| `stabilize` |  | Default follow with a target, else stabilize. |
| `model` | `translation` \| `similarity` \| `affine` |  | What the region may do: move; move, turn and scale; or any affine. Default similarity. |
| `region` | object | yes | What to track. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `target` | integer \| object (1..) |  | The clip that follows. Default the tracked clip (stabilize). A clip id, or {"track", "time"} for the clip there. |
| `time` | integer \| string |  | Where to start (inside the clip). Default the clip's first frame (backward: its last). Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `workingWidth` | integer (64..4096) |  | Width of the pictures the tracker looks at. Default 640. |

```json
{"command":"track.transform","params":{"clip":42,"region":{"height":0.2,"width":0.2,"x":0.4,"y":0.3},"target":57}}
```

<a id="trackGroup.addTracks-v1"></a>

### `trackGroup.addTracks` v1: Add Tracks to Group

Adds tracks (of the group's kind) to a track group; each moves next to the group's last track and leaves the group it was in.

Capability `EditSequence`. Tool name `trackGroup_addTracks`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `trackGroup` | integer (1..) | yes | The track group's id (sequence.summary lists them under trackGroups). |
| `tracks` | (string \| integer (1..))[] | yes | The tracks to add. |

```json
{"command":"trackGroup.addTracks","params":{"trackGroup":12,"tracks":["A3"]}}
```

<a id="trackGroup.create-v1"></a>

### `trackGroup.create` v1: Group Tracks

Groups video tracks or audio tracks (0.7): a named set with a header row in the timeline that folds its tracks into it when collapsed and mutes, solos, locks, targets and routes them together. The tracks must be of one kind and in no group yet; they are gathered at the place of the first one. Returns the group's id. A group is saved with the project.

Capability `EditSequence`. Tool name `trackGroup_create`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `colorIndex` | integer (-1..15) |  | The group's label colour, 0-15, or -1 for none (default). |
| `name` | string |  | The group's name (default "Group 1", "Group 2", ...). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (string \| integer (1..))[] | yes | The tracks to group (V1, A2, ...). |

```json
{"command":"trackGroup.create","params":{"name":"Dialogue","tracks":["A1","A2"]}}
```

<a id="trackGroup.removeTracks-v1"></a>

### `trackGroup.removeTracks` v1: Remove Tracks from Group

Takes tracks out of their track group (tracks in no group are ignored); a track from the middle of its group moves to the group's end so the group stays whole. A group left without tracks is dissolved.

Capability `EditSequence`. Tool name `trackGroup_removeTracks`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `tracks` | (string \| integer (1..))[] | yes | The tracks to take out. |

```json
{"command":"trackGroup.removeTracks","params":{"tracks":["A3"]}}
```

<a id="trackGroup.route-v1"></a>

### `trackGroup.route` v1: Route Track Group

Sets the output of every track in an audio group: "mix" (the master), "newSubmix" (adds a submix track named after the group and routes the group to it, so one fader and one effect stack work on the whole group) or an existing submix track by id.

Capability `EditSequence`. Tool name `trackGroup_route`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `output` | `mix` \| `newSubmix` |  | Where the group goes. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `submix` | integer (1..) |  | Or the id of an existing submix track. |
| `trackGroup` | integer (1..) | yes | The track group's id (sequence.summary lists them under trackGroups). |

```json
{"command":"trackGroup.route","params":{"output":"newSubmix","trackGroup":12}}
```

<a id="trackGroup.set-v1"></a>

### `trackGroup.set` v1: Track Group Settings

Renames a track group, sets its label colour, or collapses / expands it (a collapsed group shows only its header row; its tracks still play and export).

Capability `EditSequence`. Tool name `trackGroup_set`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `collapsed` | boolean |  | Fold the tracks into the group's header row (true) or show them (false). |
| `colorIndex` | integer (-1..15) |  | The label colour, 0-15, or -1 for none. |
| `name` | string |  | The new name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `trackGroup` | integer (1..) | yes | The track group's id (sequence.summary lists them under trackGroups). |

```json
{"command":"trackGroup.set","params":{"collapsed":true,"trackGroup":12}}
```

<a id="trackGroup.setTracksState-v1"></a>

### `trackGroup.setTracksState` v1: Set Track Group State

Changes the header toggles of every track in a group in one step: lock, target, sync lock, mute, solo, output (the eye), label colour.

Capability `EditSequence`. Tool name `trackGroup_setTracksState`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `colorIndex` | integer (-1..15) |  | The tracks' label colour, 0-15, or -1 for none. |
| `enabled` | boolean |  | Track output on. |
| `locked` | boolean |  | Track lock. |
| `muted` | boolean |  | Mute (audio). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `solo` | boolean |  | Solo. |
| `syncLocked` | boolean |  | Sync lock. |
| `targeted` | boolean |  | Track targeting. |
| `trackGroup` | integer (1..) | yes | The track group's id (sequence.summary lists them under trackGroups). |

```json
{"command":"trackGroup.setTracksState","params":{"muted":true,"trackGroup":12}}
```

<a id="trackGroup.ungroup-v1"></a>

### `trackGroup.ungroup` v1: Ungroup Tracks

Dissolves a track group; its tracks stay where they are, with their clips and settings.

Capability `EditSequence`. Tool name `trackGroup_ungroup`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `trackGroup` | integer (1..) | yes | The track group's id (sequence.summary lists them under trackGroups). |

```json
{"command":"trackGroup.ungroup","params":{"trackGroup":12}}
```

<a id="transcript.addSpeaker-v1"></a>

### `transcript.addSpeaker` v1: New Speaker

Adds a speaker to the project. Returns its id.

Capability `EditProject`. Tool name `transcript_addSpeaker`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `color` | string \| array, min 3 |  | Colour (default: the next of the palette). "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `name` | string | yes | Display name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"transcript.addSpeaker","params":{"color":"#4F8EF7","name":"Host"}}
```

<a id="transcript.assignSpeaker-v1"></a>

### `transcript.assignSpeaker` v1: Assign Speaker

Says who speaks over a timeline range: the words the audio clips there play (on "track" only when given) belong to "speaker", or to a new speaker called "name". It wins over diarization and stays with the media when the clips are edited.

Capability `EditProject`. Tool name `transcript_assignSpeaker`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `end` | integer \| string | yes | End of the range. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `name` | string |  | Or the name of a new speaker. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speaker` | integer (1..) |  | Speaker id. |
| `start` | integer \| string | yes | Start of the range. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `track` | string \| integer (1..) |  | Only this audio track. "V1" (bottom video track), "A2", ... or a track id. |

```json
{"command":"transcript.assignSpeaker","params":{"end":"00:00:14:00","speaker":12,"start":"00:00:10:00"}}
```

<a id="transcript.cleanup-v1"></a>

### `transcript.cleanup` v1: Remove Fillers and Silences

Removes every filler word and pause the review finds with at least minConfidence, in one undo step.

Capability `EditSequence`. Tool name `transcript_cleanup`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `fillers` | boolean |  | Look for filler words (um, uh; Thai อืม, เอ่อ ...). Default true. |
| `minConfidence` | number (0.0..1.0) |  | Lowest confidence removed (0..1). Default 0.5. |
| `padding` | number (0.0..2.0) |  | Seconds kept around the cut words (never into the next word). Default 0.04. |
| `roomTone` | number (0.0..5.0) |  | Seconds of each removed pause kept on both sides. Default 0.15. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `silenceDb` | number (-200.0..0.0) |  | A pause louder than this (dBFS) has sound in it. Default -40. |
| `silences` | number (0.0..3600.0) |  | Pauses at least this many seconds long; 0 = none. Default 0.8. |

```json
{"command":"transcript.cleanup","params":{"fillers":true,"silences":0.8}}
```

<a id="transcript.correctFromScript-v1"></a>

### `transcript.correctFromScript` v1: Correct from Script

Corrects the sequence's transcript from the script it was read from: each line is found in what was said (fuzzy, Thai too; a line with a long aside is found in parts), and where the speech follows the line, the words the recogniser got wrong take the script's spelling, casing and punctuation (names, jargon, numbers written another way). Only substitutions that sound like the script: a word the speaker skipped is never added, a word they added never removed, a word they changed ("great" for "good") is kept and listed; lines that do not match closely enough are skipped and listed. The corrections are saved with the project like transcript.correctRange (merged with corrections made before), so captions follow on captions.updateFromTranscript. One undo step; dryRun lists the changes (at most 200) without making them.

Capability `EditProject`. Tool name `transcript_correctFromScript`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `fixCase` | boolean |  | Also give names and terms the script's capitals where only those differ ("kubernetes" -> "Kubernetes"; default true). |
| `minMatch` | number (0.3..0.99) |  | How closely what was said must follow a line before its words are corrected, 0.3..0.99 (default 0.7). |
| `sequence` | integer (1..) |  | The sequence whose transcript is corrected; only needed when the project has several. |
| `split` | `auto` \| `lines` \| `sentences` |  | Lines of the script: "auto" (bullets and short lines whole, prose per sentence; default), every line, or every sentence. |
| `text` | string | yes | The script as it should read (plain text or Markdown). |

```json
{"command":"transcript.correctFromScript","params":{"text":"Today we test Wabcut with Dr. Somchai on Kubernetes."}}
```

<a id="transcript.correctRange-v1"></a>

### `transcript.correctRange` v1: Correct Words

Fixes several consecutive words at once (they read "text", split over them by spaces). Like transcript.correctWord; the words must come from one media item.

Capability `EditProject`. Tool name `transcript_correctRange`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `text` | string | yes | The corrected text (UTF-8; words separated by spaces). Empty removes the words. |
| `words` | (integer (0..1000000000))[], min 2 | yes | [first, last] word indices, inclusive. |

```json
{"command":"transcript.correctRange","params":{"text":"open source editor","words":[12,14]}}
```

<a id="transcript.correctWord-v1"></a>

### `transcript.correctWord` v1: Correct Word

Fixes a transcription mistake: the word reads "text" from now on, in the transcript the project sees and in captions generated or updated afterwards. The correction is saved with the project on top of the cached transcript, so transcribing again keeps it.

Capability `EditProject`. Tool name `transcript_correctWord`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `text` | string | yes | The corrected text (UTF-8; words separated by spaces). Empty removes the words. |
| `word` | integer (0..1000000000) | yes | Word index (transcript.words). |

```json
{"command":"transcript.correctWord","params":{"text":"Wabcut","word":12}}
```

<a id="transcript.corrections-v1"></a>

### `transcript.corrections` v1: Transcript Corrections

The project's transcript corrections and how they apply to the current transcripts: applied, conflict (the words changed since, e.g. after re-transcription; the correction still wins), restored (no words there any more; the correction brings them back) or noTranscript.

Capability `Read`. Tool name `transcript_corrections`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"transcript.corrections","params":{}}
```

<a id="transcript.customWords-v1"></a>

### `transcript.customWords` v1: List Custom Words

The project's custom words for transcription (transcript.setCustomWords).

Capability `Read`. Tool name `transcript_customWords`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"transcript.customWords","params":{}}
```

<a id="transcript.delete-v1"></a>

### `transcript.delete` v1: Delete Transcript

Throws away the transcript of media items or clips (default: every audio clip of the sequence) so the audio can be transcribed again, with another language or model. Transcripts live beside the source file in the analysis cache, not in the project: this removes the cached transcripts of those audio streams (and only transcripts). Captions already made from them, and cuts made by editing the text, stay. The words disappear from the Transcript tab at once; transcript.transcribe starts from nothing. dryRun lists what would go.

Capability `MediaAnalysis`. Tool name `transcript_delete`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 |  | Clips whose media's transcript to delete. |
| `media` | (integer \| string (1..))[], min 1 |  | Media items whose transcript to delete. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"transcript.delete","params":{"media":["Interview A"]}}
```

<a id="transcript.deleteRanges-v1"></a>

### `transcript.deleteRanges` v1: Delete Words

Deletes words (and pauses) like deleting text: ripple deletes on the tracks the words are heard on and their linked partners, cut on frame boundaries just outside the words, neighbouring deletions merged, sync-locked tracks following, locked tracks untouched. One undo step.

Capability `EditSequence`. Tool name `transcript_deleteRanges`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `gaps` | (integer (0..1000000000))[] |  | Pauses to remove: the pause after each of these words. |
| `padding` | number (0.0..2.0) |  | Seconds kept around the cut words (never into the next word). Default 0.04. |
| `roomTone` | number (0.0..5.0) |  | Seconds of each removed pause kept on both sides. Default 0.15. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `words` | ((integer (0..1000000000))[], min 2)[], min 1 |  | Word ranges to delete. |

```json
{"command":"transcript.deleteRanges","params":{"words":[[12,14],[40,40]]}}
```

<a id="transcript.export-v1"></a>

### `transcript.export` v1: Export Transcript

Premiere's Export transcript as a text file: the sequence's transcript as paragraphs (split at speaker changes and pauses longer than maxPause) with their start and end timecodes and speaker. "txt": blocks of "start - end", the speaker, the text, a blank line between (transcript files captions.import reads back); "csv": Start,End,Speaker,Text rows; "docx": the .txt layout as a Word document (timecodes grey, speakers bold), which captions.import reads back too. Without "path" the text is returned (not for docx). Refuses to replace a file unless "overwrite"; a dry run writes nothing.

Capability `Read`. Tool name `transcript_export`. Reads or writes the files its parameters name (extensions never run it).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `format` | `txt` \| `csv` \| `docx` |  | Default: the file's extension, else txt. |
| `maxPause` | number (0.0..60.0) |  | A longer pause starts a new paragraph. Default 1. |
| `overwrite` | boolean |  | Replace an existing file. Default false. |
| `path` | string |  | A .txt, .csv or .docx file. Default: return the text. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speakers` | boolean |  | Name the speakers. Default true. |

```json
{"command":"transcript.export","params":{"path":"C:/exports/interview transcript.csv"}}
```

<a id="transcript.find-v1"></a>

### `transcript.find` v1: Find in Transcript

Premiere's Find for the transcript (the arrows of Find and Replace): every match of "find" (within a word, or across consecutive words when it has spaces) as the words it spans, the time of the first and their text, in transcript order. "matchCase" and "wholeWord" as transcript.findReplace. At most 1000 matches.

Capability `Read`. Tool name `transcript_find`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `find` | string | yes | The text to find (plain text, not a pattern; spaces separate words). |
| `matchCase` | boolean |  | Match Case. Default false. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `wholeWord` | boolean |  | Whole Word. Default false. |

```json
{"command":"transcript.find","params":{"find":"wab cut"}}
```

<a id="transcript.findReplace-v1"></a>

### `transcript.findReplace` v1: Find and Replace in Transcript

Premiere's Find and Replace for the transcript: every match of "find" (within a word, or across consecutive words when it has spaces) is corrected as transcript.correctRange would - saved with the project on top of the cached transcripts, so captions generated or updated later read it - in one undo step; with "words" (a match transcript.find listed) only that match (Premiere's Replace). "matchCase" and "wholeWord" as Premiere's options (without Match Case, Unicode simple case folding). A dry run lists the changes without making them.

Capability `EditProject`. Tool name `transcript_findReplace`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `find` | string | yes | The text to find (plain text, not a pattern; spaces separate words). |
| `matchCase` | boolean |  | Match Case. Default false. |
| `replace` | string | yes | What each match becomes (empty removes the words). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `wholeWord` | boolean |  | Whole Word. Default false. |
| `words` | (integer (0..1000000000))[], min 2 |  | Replace only the match on these words: [first, last], inclusive. |

```json
{"command":"transcript.findReplace","params":{"find":"wab cut","replace":"Wabcut"}}
```

<a id="transcript.keepRanges-v1"></a>

### `transcript.keepRanges` v1: Keep Only Words

Rough cut: keeps the given word ranges and ripple-deletes everything else on the transcript's tracks, from the first to the last clip that has words. One undo step.

Capability `EditSequence`. Tool name `transcript_keepRanges`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `padding` | number (0.0..2.0) |  | Seconds kept around the cut words (never into the next word). Default 0.04. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `words` | ((integer (0..1000000000))[], min 2)[], min 1 | yes | Word ranges to keep. |

```json
{"command":"transcript.keepRanges","params":{"words":[[0,30],[95,140]]}}
```

<a id="transcript.mediaWords-v1"></a>

### `transcript.mediaWords` v1: Media Transcript Words

One media item's words in source time (what the clip says before any edit: the Transcript panel's Source mode), as arrays in wordFields order: index, text, start and end as source timecodes in the media's rate and as ticks, speaker and recognition confidence. Corrections of the project apply; words inside an audio redaction read "[bleep]". Pick a soundbite, then source.setMarks with its first word's start and its last word's end and edit.insertFromSource / overwriteFromSource. audioStream: an index into the media's audio streams (default: the first one with a transcript).

Capability `Read`. Tool name `transcript_mediaWords`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audioStream` | integer (0..64) |  | Audio stream index (0 = the first). Default: the first with a transcript. |
| `first` | integer (0..1000000000) |  | First word index. |
| `from` | integer \| string |  | Only words that end after this source time: ticks or a timecode in the media's rate. |
| `last` | integer (0..1000000000) |  | Last word index. |
| `limit` | integer (1..100000) |  | Most words to return (default 500). |
| `media` | integer \| string (1..) | yes | The media item. A media id or exact name. |
| `to` | integer \| string |  | Only words that start before this source time: ticks or a timecode in the media's rate. |

```json
{"command":"transcript.mediaWords","params":{"from":"00:01:10:00","media":3,"to":"00:01:30:00"}}
```

<a id="transcript.mergeSpeakers-v1"></a>

### `transcript.mergeSpeakers` v1: Merge Speakers

Makes speakers one: every word, assignment and caption of "from" (one id or several) becomes "into", and the merged speakers go.

Capability `EditProject`. Tool name `transcript_mergeSpeakers`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | integer \| array (1..) | yes | One speaker id, or an array of them, to merge away. |
| `into` | integer (1..) | yes | The speaker that stays. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"transcript.mergeSpeakers","params":{"from":[13,14],"into":12}}
```

<a id="transcript.moveRange-v1"></a>

### `transcript.moveRange` v1: Move Words

Moves words (a sentence, a paragraph) to the word boundary before another word: cut out with a ripple and inserted there, with their linked video. One undo step.

Capability `EditSequence`. Tool name `transcript_moveRange`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `before` | integer (0..1000000000) | yes | Insert before this word; the word count = after the last word. |
| `padding` | number (0.0..2.0) |  | Seconds kept around the cut words (never into the next word). Default 0.04. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `words` | (integer (0..1000000000))[], min 2 | yes | [first, last] word indices to move. |

```json
{"command":"transcript.moveRange","params":{"before":10,"words":[60,85]}}
```

<a id="transcript.renameSpeaker-v1"></a>

### `transcript.renameSpeaker` v1: Rename Speaker

Renames a speaker and/or changes its colour. Name it by "speaker" id, or by the diarization "label" of a media item's transcript (transcript.speakers lists both); a label without a speaker gets one.

Capability `EditProject`. Tool name `transcript_renameSpeaker`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `audioStream` | integer (0..64) |  | Its audio stream (default 0). |
| `color` | string \| array, min 3 |  | New colour. "#RRGGBB", or [red, green, blue] from 0 to 1. |
| `label` | integer (0..100000) |  | The diarization label. |
| `media` | integer \| string (1..) |  | Or the media item whose transcript has the label. A media id or exact name. |
| `name` | string |  | New name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `speaker` | integer (1..) |  | Speaker id. |

```json
{"command":"transcript.renameSpeaker","params":{"name":"Dr. Somchai","speaker":12}}
```

<a id="transcript.review-v1"></a>

### `transcript.review` v1: Review Fillers and Silences

Filler words (language-aware: um, uh, like in disfluent context; Thai อืม, เอ่อ, แบบว่า ...) and pauses longer than a threshold, as a review list: kind, word indices, time range, confidence, label and context. Remove chosen items with transcript.deleteRanges (fillers as words, silences as gaps) or all with transcript.cleanup.

Capability `Read`. Tool name `transcript_review`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `fillers` | boolean |  | Look for filler words (um, uh; Thai อืม, เอ่อ ...). Default true. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `silenceDb` | number (-200.0..0.0) |  | A pause louder than this (dBFS) has sound in it. Default -40. |
| `silences` | number (0.0..3600.0) |  | Pauses at least this many seconds long; 0 = none. Default 0.8. |

```json
{"command":"transcript.review","params":{"silences":1.0}}
```

<a id="transcript.setCustomWords-v1"></a>

### `transcript.setCustomWords` v1: Custom Words

Sets the project's custom words for transcription: names, game titles, jargon and invented terms the recogniser should spell right. They are given to the local transcriber as its vocabulary hint and to a cloud transcriber as key terms the next time audio is transcribed (transcript.transcribe; audio that already has a transcript needs "force" to be heard again). Give "words" (replaces the list; an empty list clears it) or "text" (one word or phrase per line, or separated by commas); "add" keeps the words already there and adds these. Words are trimmed, duplicates (any case) dropped; at most 200 of 80 bytes. One undo step.

Capability `EditProject`. Tool name `transcript_setCustomWords`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `add` | boolean |  | Add to the words already there instead of replacing them (default false). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `text` | string |  | Or the words as text: one per line, or separated by commas or semicolons. |
| `words` | string[] |  | The words. |

```json
{"command":"transcript.setCustomWords","params":{"words":["Malenia","Elden Ring"]}}
```

<a id="transcript.speakers-v1"></a>

### `transcript.speakers` v1: Speakers

The project's speakers (id, name, colour, words in this sequence) and the diarization labels of the sequence's transcripts with the speaker each one is (null: none yet; transcript.renameSpeaker by media and label names one).

Capability `Read`. Tool name `transcript_speakers`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"transcript.speakers","params":{}}
```

<a id="transcript.text-v1"></a>

### `transcript.text` v1: Transcript Text

What the sequence says, in timeline order, as paragraphs (split at speaker changes and pauses) with the word indices the other transcript commands take. Words inside an audio redaction read "[bleep]". Start here.

Capability `Read`. Tool name `transcript_text`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | integer \| string |  | Only paragraphs that end after this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `maxPause` | number (0.0..60.0) |  | A longer pause starts a new paragraph. Default 1. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Only paragraphs that start before this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"transcript.text","params":{}}
```

<a id="transcript.transcribe-v1"></a>

### `transcript.transcribe` v1: Transcribe Media

Transcribes the audio of media items or clips (default: every audio clip of the sequence) so the other transcript commands have words. provider: "auto" (on this computer when the local engine is built, else the user's cloud transcription model), "local", or "cloud:<model id>"; cloud transcription goes through the user's consent, spend cap and offline / no-cloud locks. Audio that already has a transcript is not transcribed again unless "force". "check": true only lists the providers and what has a transcript. Transcripts are stored per source file (the analysis cache), not in the project.

Capability `MediaAnalysis`. Tool name `transcript_transcribe`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `check` | boolean |  | Only list providers, languages and which audio has a transcript. Default false. |
| `clips` | (integer \| object (1..))[], min 1 |  | Clips whose media to transcribe. |
| `force` | boolean |  | Transcribe even audio that has a transcript (the new one replaces the old: how to redo it in another language). Default false. |
| `language` | string |  | The language spoken: "auto" (default: detected) or an ISO 639-1 code such as "he", "id", "uk", "ar", "pl", "tr", "is", "fi", "cs", "sk", "ro", "hr", "sr", "bs" ("check": true lists all of them with their own names and how well Whisper knows them). Naming it helps a short or noisy recording that detection gets wrong. |
| `media` | (integer \| string (1..))[], min 1 |  | Media items to transcribe. |
| `mixedLanguages` | boolean |  | The recording switches between languages (an interview with a translator, a streamer who changes language): with "language" "auto", every stretch of speech is transcribed in the language it is spoken in, and each transcript segment names its own (transcript.get shows them). Local transcription only, with a multilingual model (Small or larger tells languages apart best); slower, since the audio is listened to in short pieces. Ignored with a named language or "translate". Default false. |
| `provider` | string |  | "auto" (default), "local" or "cloud:<model id>". |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `translate` | boolean |  | Whisper's translate task: the transcript comes out in English, whatever the language spoken (Icelandic or Finnish interviews, say). Local transcription only, with a multilingual model (Small, Medium or Large v3 translate best). Default false. |
| `vocabulary` | string[] |  | Words the recogniser should spell right (character names, game titles, jargon). Default: the project's custom words (transcript.setCustomWords). The local transcriber uses the first ones that fit its vocabulary hint (about 480 bytes), a cloud transcriber the first 100 as key terms. [] means none. |

```json
{"command":"transcript.transcribe","params":{"language":"th","media":["Interview A"]}}
```

<a id="transcript.words-v1"></a>

### `transcript.words` v1: Transcript Words

The sequence's words in timeline order as arrays in wordFields order: index, text, timeline start and end, the audio clip and track it is heard on, speaker and recognition confidence.

Capability `Read`. Tool name `transcript_words`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `first` | integer (0..1000000000) |  | First word index. |
| `from` | integer \| string |  | Only words that end after this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `last` | integer (0..1000000000) |  | Last word index. |
| `limit` | integer (1..100000) |  | Most words to return (default 500). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `to` | integer \| string |  | Only words that start before this time. Timecode in the sequence's rate ("00:01:02:03") or ticks. |

```json
{"command":"transcript.words","params":{"first":0,"last":49}}
```

<a id="transition.add-v1"></a>

### `transition.add` v1: Add Transition

Adds a transition at a clip's head or tail: on the cut when the neighbouring clip meets that edge, otherwise inside the clip (a fade from/to black). The media must have frames beyond the cut; by default the transition is shortened to what there is. Default effect: crossDissolve on video, constantPower on audio.

Capability `EditSequence`. Tool name `transition_add`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `alignment` | `center` \| `startAtCut` \| `endAtCut` \| `custom` |  | Where the transition sits on a cut between two clips: centered (default), starting at the cut (the outgoing clip plays on), ending at it, or custom: the cut lies "offset" into it (by default where it is now). Stored on the transition: a new duration keeps it. Ignored for a one-sided transition at a clip end with no neighbour. |
| `clip` | integer \| object (1..) | yes | The clip. A clip id, or {"track", "time"} for the clip there. |
| `duration` | integer \| string |  | Transition length. Default 1 second. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `edge` | `head` \| `tail` | yes | Which edge: "head" (the clip's In point) or "tail" (its Out point). |
| `effect` | `crossDissolve` \| `additiveDissolve` \| `dipToBlack` \| `dipToColor` \| `dipToWhite` \| `filmDissolve` \| `nonAdditiveDissolve` \| `irisBox` \| `irisCross` \| `irisDiamond` \| `irisRound` \| `pagePeel` \| `pageTurn` \| `bandSlide` \| `centerSplit` \| `push` \| `slide` \| `split` \| `whip` \| `bandWipe` \| `barnDoors` \| `checkerWipe` \| `checkerBoard` \| `clockWipe` \| `gradientWipe` \| `inset` \| `paintSplatter` \| `pinwheel` \| `radialWipe` \| `randomBlocks` \| `randomWipe` \| `spiralBoxes` \| `venetianBlinds` \| `wedgeWipe` \| `wipe` \| `zigZagBlocks` \| `crossZoom` \| `zoomBlur` \| `lightLeak` \| `flash` \| `glitch` \| `animationBlock` \| `animationFade` \| `animationFlip` \| `animationFold` \| `animationPop` \| `animationPull` \| `animationSpin` \| `animationSpring` \| `animationTravel` \| `animationSurprise` \| `morphCut` \| `constantPower` \| `constantGain` \| `exponentialFade` |  | The transition (effect.list). |
| `fit` | `exact` \| `availableHandles` |  | When the media lacks frames beyond the cut: "exact" fails with how much there is, "availableHandles" shortens the transition to fit (default). |
| `offset` | integer \| string |  | With alignment "custom": how much of the transition lies before the cut. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `params` | object |  | Constant parameters, e.g. a wipe's {"angle": 90}. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"transition.add","params":{"clip":42,"duration":"00:00:00:12","edge":"tail"}}
```

<a id="transition.remove-v1"></a>

### `transition.remove` v1: Remove Transition

Deletes a transition; the edit point becomes a cut.

Capability `EditSequence`. Tool name `transition_remove`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `transition` | integer (1..) | yes | Transition id. |

```json
{"command":"transition.remove","params":{"transition":90}}
```

<a id="transition.set-v1"></a>

### `transition.set` v1: Set Transition

Changes a transition: its effect, parameters, its length on either side of the cut ("before", "after"), a new total "duration" placed by its alignment, or the alignment itself (which re-splits it; "custom" with "offset" puts the cut that far in).

Capability `EditSequence`. Tool name `transition_set`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `after` | integer \| string |  | New length after the cut. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `alignment` | `center` \| `startAtCut` \| `endAtCut` \| `custom` |  | Where the transition sits on a cut between two clips: centered (default), starting at the cut (the outgoing clip plays on), ending at it, or custom: the cut lies "offset" into it (by default where it is now). Stored on the transition: a new duration keeps it. Ignored for a one-sided transition at a clip end with no neighbour. |
| `before` | integer \| string |  | New length before the cut. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `duration` | integer \| string |  | New total length, split by alignment. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `effect` | `crossDissolve` \| `additiveDissolve` \| `dipToBlack` \| `dipToColor` \| `dipToWhite` \| `filmDissolve` \| `nonAdditiveDissolve` \| `irisBox` \| `irisCross` \| `irisDiamond` \| `irisRound` \| `pagePeel` \| `pageTurn` \| `bandSlide` \| `centerSplit` \| `push` \| `slide` \| `split` \| `whip` \| `bandWipe` \| `barnDoors` \| `checkerWipe` \| `checkerBoard` \| `clockWipe` \| `gradientWipe` \| `inset` \| `paintSplatter` \| `pinwheel` \| `radialWipe` \| `randomBlocks` \| `randomWipe` \| `spiralBoxes` \| `venetianBlinds` \| `wedgeWipe` \| `wipe` \| `zigZagBlocks` \| `crossZoom` \| `zoomBlur` \| `lightLeak` \| `flash` \| `glitch` \| `animationBlock` \| `animationFade` \| `animationFlip` \| `animationFold` \| `animationPop` \| `animationPull` \| `animationSpin` \| `animationSpring` \| `animationTravel` \| `animationSurprise` \| `morphCut` \| `constantPower` \| `constantGain` \| `exponentialFade` |  | New transition effect. |
| `fit` | `exact` \| `availableHandles` |  | When the media lacks frames beyond the cut: "exact" fails with how much there is, "availableHandles" shortens the transition to fit (default). |
| `offset` | integer \| string |  | With alignment "custom": how much of the transition lies before the cut. Timecode in the sequence's rate ("00:01:02:03") or ticks. |
| `params` | object |  | New constant parameters (replace all). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `transition` | integer (1..) | yes | Transition id (sequence.summary and clip.details list them). |

```json
{"command":"transition.set","params":{"duration":"00:00:01:00","effect":"dipToBlack","transition":90}}
```

<a id="vr.info-v1"></a>

### `vr.info` v1: VR Video Info

Whether the sequence is VR / 360 video (its projection and eye layout: sequence.setSettings "vr" sets it) and, with "media", what that item's picture is: "file" is what its spherical metadata says, "interpretation" Interpret Footage's setting (null: none), "effective" the one in use and "suits" whether the frame size fits the projection (a 360 eye is 2:1, a 180 eye 1:1).

Capability `Read`. Tool name `vr_info`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `media` | integer \| string (1..) |  | A media item. A media id or exact name. |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |

```json
{"command":"vr.info","params":{"media":3}}
```

<a id="vr.reorient-v1"></a>

### `vr.reorient` v1: VR Reorient

Adds the VR Reorient effect to 360 / VR180 clips and sets it: "yaw" turns the sphere to the right, "pitch" up, "roll" tilts it to the right (degrees; keyframe them with clip.addKeyframe), so a different part of the scene lands straight ahead. The projection and eye layout default to the clip's (its footage's, else the sequence's); a flat clip needs "projection". One undo step.

Capability `EditSequence`. Tool name `vr_reorient`.

| Parameter | Type | Required | Description |
|---|---|---|---|
| `clips` | (integer \| object (1..))[], min 1 | yes | 360 / VR180 clips. |
| `pitch` | number (-90.0..90.0) |  | Degrees up (-90 to 90). |
| `projection` | `equirect360` \| `equirect180` |  | The picture's projection. Default: the clip's. |
| `roll` | number (-3600.0..3600.0) |  | Degrees of tilt to the right (-3600 to 3600). |
| `sequence` | integer (1..) |  | Sequence id; only needed when the project has several. |
| `stereo` | `mono` \| `topBottom` \| `sideBySide` |  | The picture's eye layout. Default: the clip's. |
| `yaw` | number (-3600.0..3600.0) |  | Degrees to the right (-3600 to 3600). |

```json
{"command":"vr.reorient","params":{"clips":[42],"yaw":90}}
```

<a id="workflow.podcast-v1"></a>

### `workflow.podcast` v1: Podcast Workflow

The one-click podcast / interview edit, on this computer (local, free): sync the cameras and microphones (multicam by audio), lay out the episode (the multicam picture, each microphone on its own dialogue track), transcribe, Enhance Speech, loudness to -16 LUFS, remove fillers and long pauses, switch cameras to whoever speaks, captions, chapters from the topics (and YouTube chapters), optionally shorts. Without "run" it returns the plan (every step, on or off and why); with "run" it runs the enabled steps (or "steps"), as one undo step; "context" continues an earlier run.

Capability `EditProject`. Tool name `workflow_podcast`. Runs an AI model (extensions need MediaAnalysis for it too).

| Parameter | Type | Required | Description |
|---|---|---|---|
| `angles` | object[] |  | Who is on which camera, for the switching (default: the analysis guesses). |
| `angles[].angle` | integer (1..64) | yes | The angle: 1 = the first camera; the microphones come after the cameras. |
| `angles[].role` | `closeUp` \| `wide` \| `unknown` |  | What it shows. |
| `angles[].speaker` | string |  | Whom a close-up shows: a speaker's name ("Mic 1": the first microphone's speaker) or key. |
| `cameras` | (integer \| string)[] |  | The camera recordings (the picture angles). |
| `captionPreset` | string |  | The caption preset (default Podcast). |
| `context` | object |  | What earlier runs made (a previous result's "context", as it is). |
| `context.multicam` | integer (0..) |  | Multicam source id. |
| `context.sequence` | integer (0..) |  | Episode sequence id. |
| `context.transcribed` | (integer (0..))[] |  | What the run transcribed. |
| `context.youtubeChapters` | string |  | The YouTube chapters text. |
| `enhanceStrength` | number (0.0..100.0) |  | Enhance Speech strength 0..100 (default 70). |
| `fillers` | boolean |  | Remove filler words (default true). |
| `lufs` | number (-36.0..-5.0) |  | Loudness target (default -16, podcasts). |
| `maxChapters` | integer (2..100) |  | At most this many chapters (default 12). |
| `media` | (integer \| string)[] |  | Or all recordings: those with a picture are cameras, the rest microphones. |
| `mics` | (integer \| string)[] |  | The microphone recordings (audio only). Default: the cameras' own sound. |
| `minChapterSeconds` | number (10.0..3600.0) |  | A chapter lasts at least this long (default 60 s). |
| `name` | string |  | The episode's name (default Podcast). |
| `run` | boolean |  | Run the steps (default false: return the plan only). |
| `shorts` | boolean |  | Also make vertical shorts of the best moments (default false). |
| `shortsCount` | integer (1..10) |  | How many shorts (default 3). |
| `silences` | number (0.0..30.0) |  | Shorten pauses longer than this many seconds (default 1; 0 keeps pauses). |
| `skip` | (`multicam` \| `sequence` \| `transcribe` \| `enhance` \| `loudness` \| `cleanup` \| `autoswitch` \| `captions` \| `chapters` \| `shorts`)[] |  | Steps to leave out. |
| `steps` | (`multicam` \| `sequence` \| `transcribe` \| `enhance` \| `loudness` \| `cleanup` \| `autoswitch` \| `captions` \| `chapters` \| `shorts`)[] |  | Run only these steps (with "run"). |
| `switchPreset` | `calm` \| `interview` \| `energetic` |  | The camera switching preset (default calm). |
| `sync` | `audio` \| `timecode` \| `inPoints` \| `outPoints` \| `markers` |  | How the recordings are synced (default audio). |

```json
{"command":"workflow.podcast","params":{"media":[3,4,5,6]}}
```

