# Wabcut 0.1 and Premiere Pro: what matches, what doesn't

This compares Wabcut 0.1.0 "First Cut" (tag `v0.1.0`) with Premiere Pro on Windows. The Premiere version is
26.5, released September 2026, as listed on Adobe's
[What's new in Premiere](https://helpx.adobe.com/premiere/desktop/whats-new/whats-new.html) page. It covers Wabcut's
three target uses: **gaming** videos (long-form and Shorts), **video essays** and **documentaries**.

How to read it:

- Premiere's feature areas come from Adobe's own help pages, linked in each row.
- Each Wabcut status is backed by evidence from this repository: a Catch2 test name (with its file under `tests/`) or a
  step of the 0.1 release run (`docs/internal/release-checklist.md`, "RC.2", workflows **a** gaming, **b** Shorts, **c** video
  essay, **d** documentary).
- **Equal** means Wabcut does the same job for the three uses, and the evidence shows it working. It does not mean
  every option matches; differences that matter are in the notes.
- **Better** is used only where the row names a concrete advantage.
- **Partial** names what is missing. **Missing** means not in 0.1.
- In the AI rows, Equal means the feature exists and passes its tests. Real-footage quality has not been benchmarked
  against Premiere's (see the 0.1 known issues).

Adobe's help site refused automated page fetches (HTTP 403) on 2026-10-13. The Premiere side was therefore taken from
the same help pages as a search engine indexed them that day. Re-open a link before quoting it externally.

## Summary

| Status | Count |
|---|---|
| Equal | 22 |
| Better | 6 |
| Partial | 9 |
| Missing | 3 |
| **Feature areas compared** | **40** |

| Use | Verdict |
|---|---|
| Gaming | Wabcut's strongest case: the 0.1 release run edited and exported a whole long-form video and a Short (RC.2 a, b). Wabcut adds a local highlight finder, gameplay dead-time removal and facecam layouts. Premiere has smoother slow motion (Optical Flow) and a much larger effects library. |
| Video essay | Close to equal for the core loop of script, voice-over, transcript, captions, B-roll search, ducking and loudness (RC.2 c). Premiere is ahead on its motion-graphics ecosystem (MOGRT, Adobe Stock) and its effects library. |
| Documentary | Multicam, text-based editing through multicam clips, colour, privacy redaction and an FCP7 XML round trip all work (RC.2 d). Premiere is ahead on HDR, on AAF/OMF hand-off to audio post, on broadcast caption formats and on Frame.io review. |

Wabcut is also free and open source (GPL-3.0-or-later), with no account. Premiere's generative features use monthly
[generative credits](https://helpx.adobe.com/firefly/using/generative-credits.html).

## Editing and trimming

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Insert / overwrite, three-point edits, source patching ([three-point edits](https://www.adobe.com/learn/premiere-pro/web/three-point-edits)); lift / extract, add edit, ripple delete | **Equal** | `edit.insertFromSource`, `edit.overwriteFromSource`, `clip.insert`, `clip.overwrite`, `sequence.lift`, `sequence.extract`, `sequence.addEdit`, `timeline.rippleDelete`; Premiere's [default keys](https://helpx.adobe.com/premiere/desktop/get-started/keyboard-shortcuts/default-keyboard-shortcuts.html) (`,` `.` `;` `'` Ctrl+K, all mapped in `docs/keymap.md`) | `tests/command/test_edit_commands.cpp` "clip.insert and clip.overwrite edit media in at a time", "Lift and extract act on a range of the chosen tracks", "Delete and ripple delete take clips or a gap"; RC.2 a "Cuts with Premiere ops" |
| Ripple, roll, slip, slide, trim mode ([trimming](https://www.adobe.com/learn/premiere-pro/web/trimming-timeline-premiere-cc), [trimming clips](https://helpx.adobe.com/premiere-pro/using/trimming-clips1.html)) | **Equal** | `clip.rippleTrim`, `clip.roll`, `clip.slip`, `clip.slide`, Q/W; Trim view in the Program Monitor with J/K/L | `tests/command/test_edit_commands.cpp` "Trim, ripple, roll, slip, slide and move by a delta"; `tests/model/test_edit_trim.cpp` "Ripple edit trims an edge and ripples later material on every sync-locked track"; `tests/app/test_timeline_pro.cpp` "Dynamic trimming: J/K/L play from the edit point and the trim follows playback" |
| Nested sequences ([nesting](https://www.adobe.com/learn/premiere-pro/web/premiere-pro-nesting-sequence-cc)) | **Equal** | `sequence.nest`, `sequence.unnest`, `clip.openNested` | `tests/command/test_nesting_commands.cpp` "sequence.nest and sequence.unnest through the engine, one undo step each" |
| Speed/Duration, Time Remapping, Optical Flow slow motion ([slow motion](https://www.adobe.com/learn/premiere-pro/web/slow-motion)) | **Partial**: no Optical Flow or frame blending (video speed uses the nearest frame); the Rate Stretch tool (R) works since 0.1.2 | `clip.speedDuration`, `timeRemap.*`, `clip.frameHold`, `clip.instantReplay`; audio keeps its pitch | `tests/model/test_speed.cpp` "Reversed clips play their media backwards and stay exact under edits"; `tests/app/test_time_remap_ui.cpp` "Time remap: command-backed speed, ramp halves, hold and reverse each undo atomically"; RC.2 a "Speed ramp with audible audio (50 %)" |

## Effects, masks and graphics

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Video effects and transitions library (Film Impact effects and transitions added in 26.2, [What's new](https://helpx.adobe.com/premiere/desktop/whats-new/whats-new.html)) | **Partial**: a much smaller built-in set (dissolves, dips, wipes, transform, crop, blur, keying, blend modes, adjustment layers) and no third-party video plug-ins (no OpenFX) | `clip.addEffect`, `transition.add`, `sequence.applyDefaultTransitions`, keyframes with Bezier handles | `tests/render/test_effects.cpp` "Transitions: dissolve, dips and wipes match their CPU reference", "Effects: crop is relative to a transformed layer's own frame"; `tests/model/test_keyframes.cpp` "Each interpolation shapes its segment" |
| Masks with tracking, AI Object Mask ([object masking](https://helpx.adobe.com/uk/chl-internal/object-masking.html)) | **Equal** | `mask.add`, `track.mask`, `mask.objectSelect`, `mask.refine`, `mask.propagate`; Object tool | `tests/masks/test_mask_commands.cpp` "track.mask keys the mask's transform frame by frame; backward; to a count; lost on a cut"; `tests/ai/segment/test_object_mask_commands.cpp` "mask.objectSelect: a click makes a matte mask that follows the object to the cut; dry run, cache, undo" |
| Blurring faces or objects for privacy (an example use of [object masking](https://helpx.adobe.com/uk/chl-internal/object-masking.html), applied per clip) | **Better**: a redaction is marked once in *source* time and follows the footage into every use (trimmed, sped up, reversed, reused, subclipped), in preview, export, thumbnails and burn-in. Audio bleeps also reach transcripts, captions, translations and dubs. It fails closed where a tracked matte is missing. Wabcut has no automatic face detection: you mark the region or click the object. | `redaction.add`, `redaction.override` | `tests/playback/video/test_redaction.cpp` "Redaction: every derived cut shows the marked source frames redacted, and only those"; `tests/model/test_redaction_media.cpp` "Redactions follow the source: a master's cover its subclips' clips, and the reverse (review wave 5 E5-P1-1)"; `tests/ai/global/test_translation.cpp` "A redaction never writes source-language words into a translated caption track"; RC.2 d "Redaction of a region" |
| Essential Graphics, Motion Graphics templates (.mogrt) ([install templates](https://helpx.adobe.com/premiere-pro-next/add-text-images/insert-text/install-motion-graphics-templates.html), [Adobe Stock templates](https://helpx.adobe.com/premiere-pro-next/add-text-images/insert-text/use-motion-graphics-templates-from-adobe-stock.html)) | **Partial**: Wabcut's own templates (12 built in, save your own) and Lottie import; no .mogrt import, no After Effects templates, no Adobe Stock | `graphic.add`, `title.add`, `template.save`, `template.instantiate`, `lottie.import`; Essential Graphics panel | `tests/app/test_essential_graphics.cpp` "Essential Graphics: Save as template keeps the chosen fields in the project's library or the user's"; `tests/model/test_graphic_ops.cpp` "Instantiating a template fills its fields and sizes the clip"; RC.2 a callout template, RC.2 c lower third |

## Colour

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Lumetri basic correction, curves, colour wheels, LUTs ([Lumetri overview](https://www.adobe.com/learn/premiere-pro/web/lumetri-color-overview), [wheels and curves](https://helpx.adobe.com/premiere-pro/how-to/color-wheels-curves.html)) | **Equal** | `color.grade.set`, `lut.import` (.cube), `cdl.import` / `cdl.export`; Color panel (white balance, curves, wheels, HSL) | `tests/color/test_color_commands.cpp` "color.grade.set: partial parameters, stages, curves and LUTs in one undo step"; `tests/color/test_grade_math.cpp` "Curves: monotone cubics through the points, exact end points, no overshoot"; `tests/color/test_lut.cpp` "Cube: 3D with TITLE, comments, CRLF, BOM and a domain" |
| Lumetri scopes ([Lumetri overview](https://www.adobe.com/learn/premiere-pro/web/lumetri-color-overview)) | **Equal** | GPU scopes (waveform, parade, vectorscope, histogram), `scopes.read` | `tests/color/test_scopes.cpp` "Scopes: colour bars land on the vectorscope targets; parades split the channels" |
| Colour match ([color match clips](https://helpx.adobe.com/premiere-pro/how-to/color-match-clips.html)) | **Partial**: matches a clip or a whole scene to a reference, but has no face-priority option. Since 0.1.2 auto colour, colour match and match scene also work on multicam clips. | `color.match`, `color.matchScene`, `color.auto` | `tests/color/test_color_commands.cpp` "color.match and color.matchScene: one undo step for the whole scene"; `tests/color/test_color_commands.cpp` (0.1.2 multicam cases); RC.2 d "Auto colour on the angles in the multicam source" (PASS) |
| HDR workflows and colour management, Rec. 2100 PQ / HLG ([HDR workflows](https://helpx.adobe.com/premiere-pro/using/hdr-workflows.html)) | **Missing**: HDR grading (ACES/OCIO) is deferred; PQ and HLG captures are tone-mapped to SDR on import | none | `tests/color/test_hdr.cpp` "HDR: PQ and HLG captures read into SDR, other transfers left alone" (the SDR fallback only) |

## Audio

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Essential Sound presets, loudness Auto-Match ([create an audio mix](https://helpx.adobe.com/premiere-pro/how-to/create-audio-mix.html)) | **Equal** | `audio.essentialSound`, `audio.normalize` (platform targets), `audio.masterLimiter`; Essential Sound panel | `tests/audio/test_loudness.cpp` "Integrated loudness: EBU Tech 3341 cases 1-4"; `tests/audio_edit/test_audio_commands.cpp` "audio.normalize hits the preset by measuring and refining, in one undo step"; RC.2 a "Streamer voice on the Mic track", "Loudness for YouTube" (-14.02 LUFS) |
| Auto ducking ([automatic audio ducking](https://helpx.adobe.com/premiere-pro/how-to/automatic-audio-ducking.html), [video](https://helpx.adobe.com/premiere-pro/using/video/auto-ducking-video.html)) | **Equal** | `audio.autoDuck` | `tests/audio_edit/test_audio_commands.cpp` "audio.autoDuck writes music volume keyframes under speech: one undo step, previewable, repeatable"; RC.2 a "Game under voice", RC.2 c "Music ducked under the narration" |
| Enhance Speech ([Enhance Speech](https://helpx.adobe.com/premiere-pro/using/enhance-speech.html)) | **Equal**: local in both (Wabcut uses DeepFilterNet3) | `audio.enhanceSpeech`; also `audio.separateStems` | `tests/ai/audio/test_audio_jobs.cpp` "Enhance Speech job: the take is the model over the whole stream, chunk seams included, sample-aligned"; `tests/ai/audio/test_audio_models.cpp` "Enhance Speech with DeepFilterNet3: SNR improvement and speed (WABCUT_TEST_AUDIO_MODELS)" (opt-in) |
| Remix: music retimed to a target length ([Remix considerations](https://helpx.adobe.com/premiere/desktop/add-audio-effects/apply-audio-effects/remix-tool-considerations-in-premiere.html)) | **Equal** | `audio.remix`, `audio.remixRender`; Remix tool (drag the music clip's end) | `tests/audio_edit/test_essential_sound.cpp` "audio.remix puts the music's pieces on its track with constant-gain joins, within a beat of the target"; `tests/app/test_essential_sound.cpp` "Remix tool: dragging a music clip's end proposes a length; nothing changes until a remix is applied" |
| Audio Track Mixer, EQ, dynamics, noise reduction, VST3 plug-ins ([create an audio mix](https://helpx.adobe.com/premiere-pro/how-to/create-audio-mix.html)) | **Equal**: VST3 and CLAP run in a separate process | `track.setVolume`, `track.setPan`, `audio.addEffect`, `plugins.scan`; Audio Track Mixer with automation | `tests/app/test_mixer_automation.cpp` "Mixer faders are A.3's track Volume and Panner and the master fader: one undo step per drag"; `tests/audio/test_denoise.cpp` "Reduce Noise lowers steady noise"; `tests/plughost/test_plughost_protocol.cpp` "CLAP and VST3 plugins process in the helper with their latency compensated by the chain" |

## Captions and transcripts

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Speech to Text transcription ([Speech to Text](https://helpx.adobe.com/premiere-pro/using/speech-to-text.html)) | **Equal**: on this computer in both (Wabcut uses whisper.cpp on the GPU; cloud transcribers only with your own key) | `transcript.transcribe` | `tests/ai/test_transcribe_service.cpp` "TranscribeService: local transcription through the helper, then the analysis cache"; RC.2 b, c and d transcript steps |
| Captions: create, edit, style, animated captions (single-word animated captions added in 26.3, [What's new](https://helpx.adobe.com/premiere/desktop/whats-new/whats-new.html)) | **Equal** | `captions.generateFromTranscript`, `captions.updateCue`, `captions.splitCue`, `captions.setStyle`, `captions.applyPreset`, `captions.translate` | `tests/playback/video/test_caption_style.cpp` "Caption style: pop, fade and word-by-word animations"; `tests/app/test_transcript_ui.cpp` "Captions: generate from the transcript, edit a cue's text and timing, export SRT and VTT"; RC.2 b "Word-by-word captions (Shorts style)" |
| Caption files: import, SCC / MCC / STL / DFXP / SRT export, burn-in ([Speech to Text](https://helpx.adobe.com/premiere-pro/using/speech-to-text.html), [working with captions](https://helpx.adobe.com/premiere-pro/using/working-with-captions.html)) | **Partial**: SRT and VTT export and burn-in only; no caption file import, no broadcast formats | Export: SRT / VTT sidecars, burn-in | `tests/export/test_rendered.cpp` "Rendered export burns in captions exactly as the preview draws them; SRT and VTT sidecars" |
| Text-Based Editing, filler words ([overview](https://helpx.adobe.com/premiere/desktop/edit-projects/edit-video-using-text-based-editing/overview-of-text-based-editing.html)) | **Equal**: also edits through multicam and nested clips, which stay multicam | `transcript.deleteRanges`, `transcript.keepRanges`, `transcript.moveRange`, `transcript.cleanup` | `tests/ai/test_transcript_commands.cpp` "transcript.deleteRanges previews, cuts in one undo step, and undoes exactly"; RC.2 d "Transcript edit: deleting words ripples", "Angle switching after the text cut" |
| Paper Edit: a sequence built from transcript lines (26.5, [What's new](https://helpx.adobe.com/premiere/desktop/whats-new/whats-new.html)) | **Equal**: a rough cut from a script, aligned to the transcripts (Thai included), with takes to swap | `roughcut.fromScript`, `roughcut.swapTake` | `tests/ai/creator/test_roughcut.cpp` "Rough cut of a 60-minute transcript from a 40-line script", "Thai lines are found in Thai transcripts without word boundaries in the script"; `tests/ai/test_creator_ui.cpp` "Rough cut sheet: the alignment report, a swapped take and the cut as one undo step; screenshot" |

## Multicam

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Multi-camera source sequence, sync by timecode or audio, angle switching ([create a multicam source sequence](https://helpx.adobe.com/premiere-pro/using/create-multi-camera-source-sequence.html)) | **Equal**: Wabcut also auto-switches angles on the microphones' levels (podcast workflow) | `multicam.create`, `multicam.setSync`, `multicam.switchAngle`, `multicam.autoSwitch`, `audio.syncClips`; angle grid with live switching | `tests/model/test_nesting.cpp` "Multicam sources sync their angles by timecode, marks, markers, audio or by hand"; `tests/app/test_multicam.cpp` "Live switching: the camera keys while playing are recorded and sent at stop as one undo step"; `tests/ai/multicam/test_podcast_media.cpp` "The podcast workflow on real files: synced by sound, switched on the microphones' levels"; RC.2 d "Multicam synced by audio" |

## Export, interchange and media

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Export presets, formats, hardware encoding ([formats exported directly](https://helpx.adobe.com/premiere-pro/using/formats-exported-directly-premiere-pro.html)) | **Equal**: presets for YouTube up to 4K60, Shorts / TikTok / Reels 1080x1920@60, Twitch and Discord file sizes; H.264 / HEVC / AV1 / ProRes; NVENC, AMF and Quick Sync | Export workspace | `tests/export/test_gaming_export.cpp` "Gaming presets: 60 fps YouTube, Shorts and Twitch, and Discord's sizes"; `tests/export/test_export.cpp` "ProRes 422 HQ MOV export: ffprobe, frame-exact 10-bit video, sample-exact 24-bit PCM", "Automatic encoder selection uses NVENC on an NVIDIA machine"; RC.2 a and b exports |
| Adobe Media Encoder queue ([encode and export with Media Encoder](https://helpx.adobe.com/media-encoder/desktop/encoding-and-exporting/encode-export-video-audio.html)) | **Partial**: an in-app export queue, but no separate encoder application, watch folders or multi-preset drops | Export queue | `tests/export/test_queue.cpp` "ExportQueue runs jobs in order and reports their status" |
| AAF, OMF, EDL and Final Cut Pro XML export ([formats exported directly](https://helpx.adobe.com/premiere-pro/using/formats-exported-directly-premiere-pro.html)) | **Partial**: no AAF or OMF, and timeline export (FCP7 XML, FCPXML, OTIO, EDL) is command-line only in 0.1. Import is broader: Premiere (FCP7 XML, `.prproj`), Resolve, Final Cut, OTIO and EDL, with a Carried / Approximated / Dropped report. | `interchange.export`, `wabcut-cli export-timeline`; File > Import | `tests/interchange/test_export.cpp` "Export: FCP7 XML brings cuts, links, gaps, markers and track settings back unchanged (X.1)"; `tests/interchange/test_fcp7.cpp` "interchange: FCP7 XML from Premiere: the import report"; RC.2 d "Timeline export to FCP7 XML", "Re-import report" |
| Content Credentials in Export Settings (26.2, [What's new](https://helpx.adobe.com/premiere/desktop/whats-new/whats-new.html)) | **Missing**: signed C2PA export was built and tested but parked for after 0.1 (branch `SorGuayteawLord/ai15-c2pa-parked`) | none in 0.1 | none in 0.1 |
| Ingest and proxy workflow ([ingest and proxies](https://helpx.adobe.com/premiere-pro-next/organize-media/ingest-proxy-workflow/ingest-and-proxy-workflow.html)) | **Equal**: proxies made automatically for what cannot play in real time | Proxy manager | `tests/editor/test_proxies.cpp` "Proxy manager: Auto proxies what cannot play in real time; the save carries the provenance"; `tests/export/test_proxy.cpp` "Proxy: ProRes Proxy at half resolution maps frame for frame onto the source" |

## AI features

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Generative Extend ([Generative Extend](https://helpx.adobe.com/premiere-pro/using/video/generative-extend-video.html)) | **Equal**: through your own video-generation provider key, with the price shown before it runs. Only the reviewed, redacted boundary frame is sent. Live provider quality is not benchmarked in 0.1. | `gen.extend` | `tests/ai/test_generation.cpp` "Generated extensions preserve links, round trip provenance, and undo once"; `tests/ai/test_generation_ui.cpp` "Generative Extend sends the reviewed, redacted boundary frame only" |
| Generative Media Tool: video and sound effects from text prompts (26.5, [What's new](https://helpx.adobe.com/premiere/desktop/whats-new/whats-new.html)) | **Partial**: B-roll video and images from prompts through your key; no generated sound effects | `gen.broll`, `gen.image` | `tests/ai/test_generation_ui.cpp` "Generate app workflow requires consent and acceptance before a single undoable import"; `tests/ai/test_generation_conform.cpp` "Generated video conforms frame clock, dimensions, duration and reversed head" |
| Media Intelligence and the Search panel ([media intelligence](https://helpx.adobe.com/premiere-pro/using/media-intelligence-and-search-panel.html), [FAQ](https://helpx.adobe.com/premiere-pro/using/media-intelligence-and-search-panel-faq.html)) | **Equal**: local in both; finds what is shown and what is said | `search.indexMedia`, `search.footage`; Library smart search | `tests/ai/search/test_search_service.cpp` "FootageSearch indexes shots and transcripts and finds what is shown and said"; `tests/ai/test_search_ui.cpp` "Smart search in the Library: index, result cards, drop a hit with its range; screenshots"; `tests/ai/search/test_search_index.cpp` "Search latency over 1,000 indexed shots" |
| Auto Reframe ([Auto Reframe](https://helpx.adobe.com/premiere-pro/using/auto-reframe.html)) | **Equal**: follows the subject and cuts with the shots. For gaming, `sequence.makeVersion` also makes facecam-aware 9:16 or 1:1 versions. | `reframe.createSequence`, `sequence.makeVersion`, `layout.apply`; Reframe dialog | `tests/ai/reframe/test_reframe.cpp` "Reframing to 9:16 follows the subject with sparse keyframes and cuts with the shots"; `tests/command/test_gaming_commands.cpp` "sequence.makeVersion adds a Shorts or square sequence in one undo step (C.16)"; RC.2 b "9:16 layout" |
| Scene Edit Detection ([Scene Edit Detection](https://helpx.adobe.com/premiere-pro/using/scene-edit-detection.html)) | **Equal**: marks the cuts or razors at them | `analysis.detectShots`, `clip.splitAtShots` | `tests/ai/analysis/test_analysis_commands.cpp` "analysis.detectShots marks the cuts inside the clip", "clip.splitAtShots razors at the cuts, linked partners too" |
| Morph Cut ([Morph Cut](https://helpx.adobe.com/premiere-pro/kb/morph-cut.html)) | **Missing** | none | none |
| How generative AI is paid for: Firefly [generative credits](https://helpx.adobe.com/firefly/using/generative-credits.html) per plan ([features and credits](https://helpx.adobe.com/creative-cloud/apps/generative-ai/creative-cloud-generative-ai-features.html)) | **Better**: every AI feature runs locally or with your own key (Anthropic, OpenAI, Gemini, Deepgram, ElevenLabs...). There are no credits. Each call shows an exact price first, under a spend cap, and a per-project "no cloud" lock that consent never overrides. | Preferences > AI; consent ledger | `tests/ai/test_net_generation.cpp` "Generation refuses no consent offline no-cloud missing cap and exceeded cap before dispatch"; `tests/ai/test_ai_lock_ui.cpp` "AI no-cloud lock: saved with the project, reopened, recovered, per project; consent never overrides it"; `tests/ai/test_platform_policy.cpp` "AiPlatform guards every call: consent, spend cap, usage settlement, request log" |
| Deleting pauses in Text-Based Editing ([overview](https://helpx.adobe.com/premiere/desktop/edit-projects/edit-video-using-text-based-editing/overview-of-text-based-editing.html)) | **Better** for gaming: `sequence.removeDeadTime` finds stretches where game audio *and* picture motion are both low (loading screens, walking, waiting), which speech pauses miss. It previews the ranges first. Silence and filler removal for speech is there too. | `sequence.removeDeadTime`, `timeline.removeSilences`, `transcript.cleanup` | `tests/model/test_gaming_ops.cpp` "Dead time is found where game audio and motion are both low and kept where unmeasured (C.16)"; `tests/command/test_gaming_commands.cpp` "sequence.removeDeadTime previews the quiet stretches then removes them in one undo step (C.16)" |

## Wabcut-only features for the target uses

Adobe's pages linked above document no counterpart to these. Each is marked Better for the reason given.

| Area | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Gameplay highlight finder | **Better**: runs locally, with no upload and no credits. It uses game-audio spikes, mic excitement and laughter, visual intensity, OBS chapter markers and an imported Twitch / YouTube chat log, and each moment says which signals fired. One step makes a reel or a Short. Precision and recall were 1.00 on synthetic and decoded test recordings; real gameplay has not been benchmarked. | `highlights.find`, `highlights.importChat`, `highlights.toReel`, `highlights.toShort` | `tests/ai/highlights/test_highlights.cpp` "Synthetic gameplay: the scripted moments are found and the decoys mostly not", "A moment says why: the reasons name the signals that fired"; `tests/ai/highlights/test_highlights_media.cpp` "Decoded gameplay: an MKV with game and mic tracks and chapters is measured and its moments found"; `tests/export/test_highlights_e2e.cpp` "Gaming end to end: a recording's highlights become a reel and a Short that export to MP4" |
| Editing assistant | **Better**: it can run on a local model with no account. Every action is a typed command that is previewed, verified and undoable as one step, under cost and token budgets. | Assistant panel | `tests/ai/test_agent_providers.cpp` "Agent session over Chat Completions (a local model): errors as tool messages"; `tests/ai/test_agent.cpp` "Agent limits: cost and token budgets, the spend cap, steps, cancellation and the timeout"; `tests/ai/highlights/test_highlights.cpp` "The assistant makes a highlight reel of the best moments as one undo step" |
| MCP server for external AI agents | **Better**: free and local, read-only by default; file commands only inside folders you allow | `wabcut-cli mcp` | `tests/command/test_mcp_stdio.cpp` "wabcut-cli mcp over real stdio: read-only session"; `tests/command/test_mcp_files.cpp` "MCP lists no file commands and refuses them by name, touching nothing (S5-P1-1)" |

## Collaboration

| Premiere feature (Adobe docs) | Wabcut | Command / UI | Evidence |
|---|---|---|---|
| Frame.io review links, comments, collaborators ([share for review](https://helpx.adobe.com/premiere-pro-next/export-and-share/collaborate-with-others/share-projects-for-review.html)) | **Partial**: local only. Version history, review notes with rendered frames, review packages and project merge; no cloud links or shared projects. | `history.*`, `review.*`, `project.merge` | `tests/command/test_collaboration_commands.cpp` "review: notes, threads, packages and markers through the engine", "project.merge stages conflicts, applies choices and merges disjoint edits" |

## Where Premiere is still ahead

- **Platforms and maturity.** Wabcut 0.1 is Windows only. It is not code-signed and has no auto-update. Neither 1–3 hour
  recordings nor 120–240 fps sources have been benchmarked. Premiere has had decades of production use on long, heavy
  projects.
- **HDR.** Premiere grades in Rec. 2100 PQ / HLG with colour management
  ([HDR workflows](https://helpx.adobe.com/premiere-pro/using/hdr-workflows.html)). Wabcut tone-maps HDR captures to
  SDR.
- **Hand-off to audio post and broadcast.** Wabcut has no AAF or OMF export, and timeline export is command-line only.
  It has no SCC / MCC / STL / DFXP captions and no caption file import. Documentaries that go to a mixer or a
  broadcaster still need Premiere (or another tool) for that step.
- **Ecosystem.** Premiere has third-party video effect plug-ins (Wabcut has no OpenFX), .mogrt templates from After
  Effects and Adobe Stock, a far larger built-in effects library, and Frame.io review with shared links.
- **Motion.** Premiere has Optical Flow slow motion. Wabcut uses the nearest frame. High
  frame-rate captures slow down cleanly, but 30/60 fps footage slowed far down will stutter. The pen, hand, zoom and
  type tool keys (P, H, Z, T) are reserved but not active.
- **Missing AI features:** Morph Cut, Content Credentials on export (parked), generated sound effects, a
  face-priority colour match, and automatic face detection for redaction.
- **Measured AI quality.** Premiere's AI features ship after Adobe's own evaluation. In Wabcut 0.1, real-footage
  quality for the local models (search, reframe, Enhance Speech, highlights) and live cloud-provider runs have not
  been benchmarked. The numbers above come from synthetic or opt-in tests.

## Where Wabcut is ahead

- **Price.** Free and open source, with no account. AI runs locally or with your own key, with no credits, an exact
  price before each cloud call, and a per-project "no cloud" lock.
- **Privacy.** Redaction is anchored in the source, so it follows the footage everywhere, audio, captions and dubs
  included.
- **Gaming.** A local, explainable highlight finder, gameplay dead-time removal, facecam layouts and versions, and
  presets for Discord file sizes.
- **Automation.** An editing assistant whose actions are all undoable commands, and a local MCP server.
- **Coming from other editors.** A Carried / Approximated / Dropped report when importing from Premiere, Resolve and
  Final Cut.
