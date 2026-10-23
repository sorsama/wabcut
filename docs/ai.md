# Wabcut AI architecture (contract, draft rev 1)

Goal: AI that makes editing dramatically faster. Every feature is **optional**, **local-first where feasible**
(free, private, offline) and **bring-your-own-key** for cloud providers. AI never edits silently: every AI edit is a
normal, previewable, undoable `EditSession` transaction.

> Status: architecture contract. Concrete model/provider choices are filled in from the research brief
> (`docs/internal/ai-research.md`) and may change as the field moves; the contracts below must not depend on a specific vendor.

## Principles
1. **User in control.**
   - AI proposes; the user disposes. Agentic edits arrive as a *plan* (a list of edit steps plus a preview `ChangeSet`). Applying it
     is one undo step. Nothing is applied without an explicit action, except features the user enabled as automatic, such as auto-captions on import.
2. **Local-first, cloud-optional.**
   - Each capability has a local implementation where a good open model exists (transcription, VAD, scene detection,
     beat detection, embeddings, segmentation, denoise). Cloud providers are opt-in per capability.
   - Without any key, Wabcut is still a complete editor with local AI.
3. **Bring your own key, stored safely.**
   - API keys live only in the OS secret store (Windows Credential Manager / macOS Keychain / libsecret). They never go into the
     project file, settings files, logs, crash reports or `wabcut-cli` output. Keys can be tested, rotated and removed in Preferences.
4. **Privacy and consent.**
   - Before the first cloud call per provider×capability, show what will be uploaded (e.g. "audio of 3 clips, 12 min")
     and to whom. "Always allow" is remembered per provider×capability.
   - A global "Offline mode" disables all network AI.
5. **Same job system.**
   - AI work runs on the Job queue (cancellable, prioritized below playback), reports progress and respects the memory/GPU budgets.
   - Results are cached by source fingerprint plus model id/version, so re-running is free.
6. **Deterministic data, non-deterministic models.** AI outputs (transcripts, shot lists, embeddings, masks) are stored as
   versioned project data with provenance (model, provider, version, settings, date) so edits remain reproducible after a
   model changes.

## Foundation: typed edit-command protocol (prerequisite for the agent, scripting and MCP)
`EditSession::apply(label, fn)` with C++ lambdas is right for in-process edits, but AI/scripting/MCP need **durable, typed,
validated commands** (independent product review, 2026-10-11):
- `wabcut/command/` (pure C++): a `CommandSchema` registry where each command has a stable name + version, typed JSON-schema params,
  a capability scope (read / edit-sequence / edit-project / media-analysis / network), and a mapping onto model EditOps.
- A request carries `{command, version, params, expectedRevision, idempotencyKey, dryRun}`. `dryRun` returns the proposed `ChangeSet`
  plus a preview snapshot without committing. A commit is rejected if `expectedRevision` is stale (or recomputed explicitly).
  Every external edit is **validated** (invariants and locks) even in release builds.
- The same command catalog backs the AI agent tools, `wabcut-cli`, the scripting API (later) and a local **MCP server** (so external agents
  such as Claude Desktop/Code can drive Wabcut with consent). Model output never becomes executed code. It only becomes typed commands.
- Keymap CommandIds and edit commands share names where they overlap, so "what the agent did" reads like menu actions.

## Resource governor
AI jobs run under the same budgets as playback: per-job RAM/VRAM ceilings, cancellation, resumable analysis, artifact
caching by fingerprint, and **automatic pause of optional analysis while playback or a drag gesture is active**. Cloud jobs
show the selected media/ranges, destination, an estimated cost ceiling, and what will be uploaded.

## Semantic data model (persisted, versioned)
- `Transcript`: words with source-time anchors (media fingerprint + stream + source Ticks), clip-instance mapping, speaker,
  confidence, language, and an edited/approved flag per word. It survives retimes through TimeMap, and segmentation is language-aware
  (Thai has no spaces: use a dictionary/ICU word segmenter; never split on whitespace).
- `CaptionTrack`: language, cues (words → lines), style references (brand styles), editable timing, and SRT/VTT import/export.
  Kinetic caption presets render through the compositor (deterministic export), never through QML animations.
- Analysis artifacts (shots, beats, embeddings, masks) are stored with provenance and invalidated by source fingerprint.

## Decisions from the research brief (docs/internal/ai-research.md, verified 2026-10-11)
- **Three layers.**
  - `ai`: pure C++ types, capability interfaces, model registry and tool specs, with an injected `IHttpTransport`.
  - `ai_net`: Qt network transport with SSE/NDJSON, retry with `retry-after`, cancellation and redaction, plus four wire adapters (Anthropic Messages, OpenAI Responses,
    Chat Completions, which covers Ollama/LM Studio/llama-server/OpenRouter/Groq/Mistral, and Gemini Interactions, the only native video input).
  - `ai_local`: whisper.cpp ≥ 1.9, ONNX Runtime 1.31 (built with telemetry OFF) and llama.cpp. Heavy local inference runs **out of process**
    (a `wabcut-infer` helper / bundled llama-server on 127.0.0.1 with a random port and API key), so a driver crash or OOM can't take down the editor.
- **Model registry as data** (`models.json`): id, provider, endpoint, capabilities (`toolsOnChat`, `toolsOnResponses`,
  `strictSchema`, `videoInput`, `wordTimestamps`, `diarization`), price, **deprecation date**, license and SHA-256/URL for local weights.
  It ships with the app and can be refreshed from a signed file. Providers churn monthly (Sora API closed, Veo-on-Gemini-key retiring, GPT-6 tools
  only via Responses), so nothing is hard-wired to one vendor.
- **GPU backends:** default to DirectML/Windows ML, Core ML and **Vulkan**. CUDA/TensorRT stay a user-installed optional download
  (linking proprietary NVIDIA libraries into GPL binaries needs a §7 additional permission, so get legal review before the first binary release).
  Vulkan headers and shader tools come from vcpkg, not a system SDK.
- **vcpkg overlay ports** owned by the AI module (registry ports lag and the ggml port disables SIMD): whisper-cpp 1.9.x, ggml
  (`GGML_BACKEND_DL` + `GGML_CPU_ALL_VARIANTS`), onnxruntime 1.31 (no telemetry), llama-cpp, qtkeychain 0.17.
- **Bundle only tiny permissive models** (Silero VAD, RNNoise, TransNetV2). Everything else downloads on first use with a license card
  and SHA-256 verification. A license allow-list in CI rejects NC/AGPL models: no Ultralytics YOLO, RMBG, Wav2Lip, ProPainter,
  CrisperWhisper, MobileCLIP, InsightFace, DEIMv2 or madmom.
- **Analysis cache is source-scoped:** keyed by (source fingerprint, stream, analyzer id + model version + params), reusable
  across projects, exportable as an open JSON sidecar.
- **Agent reliability is the top product risk** (Timeline-Bench: ≤27% of real tasks solved, 93% false success claims). The agent works
  only through typed commands, stages a ChangeSet diff (ghost clips / strike-through transcript), runs deterministic verification and
  never bypasses undo. The default cloud model is Claude Sonnet 5.5 (`claude-sonnet-5-5`), with Opus 5.5 for "deep" tasks and GPT-6.1 / Gemini 3.8
  Flash / local Qwen3.5 or Gemma 4 as options.
- **MCP:** `wabcut-cli mcp` (JSON-RPC over stdio, hand-rolled on nlohmann_json, since there is no official C++ SDK) serves the same command catalog,
  so Claude Desktop/Code, Codex and others can drive Wabcut (Resolve 21.1 already ships MCP).
- **Compliance:** per-clip AI provenance plus **C2PA Content Credentials on export** (c2pa-rs, MIT/Apache) for EU AI Act Art. 50 (in force
  2026-08-02). Voice-clone consent prompts. Keys are BYO only (fal/ElevenLabs forbid shared keys). Runway needs a "Powered by Runway" credit.
- **Keys:** QtKeychain 0.17 overlay port, an env-var override, no plaintext key storage (session-only if no keychain), redacted everywhere.
- **No telemetry**, a project-level "Offline / no-cloud" lock, a local redacted request log and per-provider monthly spend caps.

### First 8 AI features (build order)
| # | Feature | Local default | BYOK cloud |
|---|---|---|---|
| 1 | Transcription + Transcript panel + captions (SRT/VTT, words, speakers) | whisper.cpp large-v3-turbo (Vulkan), Silero VAD, sherpa-onnx diarization; Parakeet TDT v3 fast mode | Deepgram Nova-3, ElevenLabs Scribe v2, AssemblyAI, Mistral Voxtral |
| 2 | Text-based editing + filler/silence removal (review list) | word map + Silero VAD + energy | — |
| 3 | Audio cleanup: denoise, loudness, auto-duck | RNNoise → DeepFilterNet3; libebur128 | — |
| 4 | Animated/styled captions + translation | compositor text layer | user's LLM / DeepL |
| 5 | Shot detection + semantic search | TransNetV2 + detectors; SigLIP 2 + hnswlib + SQLite FTS5 | provider vision |
| 6 | Agent assistant + MCP | llama-server (Qwen3.5 / Gemma 4) | Claude Sonnet/Opus 5.5, GPT-6.1, Gemini 3.8 Flash |
| 7 | Auto-reframe + shorts finder | YuNet + RT-DETRv2 + ByteTrack + Light-ASD | LLM rubric scoring |
| 8 | Beat detection + cut-to-music | Beat This! (ONNX), aubio fallback | — |

## Module layout
```
src/ai/            wabcut_ai (pure C++ core, no Qt):
  include/wabcut/ai/Capability.h     capability enums + typed requests/results
  include/wabcut/ai/Provider.h       Provider interface + registry
  include/wabcut/ai/Transcript.h     Transcript data model (words, timing, speakers, confidence)
  include/wabcut/ai/EditPlan.h       agent plans: steps -> model EditOps, preview ChangeSet
  include/wabcut/ai/Secrets.h        SecretStore interface (OS backends live in the app layer)
  include/wabcut/ai/TranscribeService.h  local / cloud / cache transcription routing (wabcut_ai_transcribe)
  include/wabcut/ai/AudioEnergy.h        per-range RMS / peak / speech jobs (wabcut_ai_transcribe)
  edit/              wabcut_ai_edit: transcript-first editing (SequenceTranscript, TextEdits, Review,
                     CaptionBuilder, transcript.* commands); see "Text-based editing"
  analysis/          wabcut_ai_analysis: shot and beat detection, analysis.* commands; see "Media analysis"
  agent/             wabcut_ai_agent: the editing assistant (tool loop, staging, verification, guarded apply,
                     quick actions); see "Editing assistant"
  search/            wabcut_ai_search: smart search (keyframes, SigLIP 2 tokenizer, wabcut-embed protocol, SQLite FTS5 +
                     hnswlib index, hybrid ranking, search.* commands); see "Smart search"
  vision/            wabcut_ai_vision: subject analysis (YuNet / YOLOX decoding, tracking, saliency, the sampler,
                     HelperDetector); see "Auto reframe"
  reframe/           wabcut_ai_reframe: the L1 virtual camera, reframed sequences, analysis.subjects / reframe.*
  shorts/            wabcut_ai_shorts: the shorts finder, the guarded re-rank, shorts.find / shorts.create; see
                     "Shorts finder"
  providers/local/   whisper.cpp, ONNX Runtime models, DSP (VAD, beats, scene detect)
  providers/cloud/   HTTP JSON clients (OpenAI-compatible, Anthropic, Gemini, Deepgram, ElevenLabs, ...)
src/app/src/bridge/AiController.*    QML bridge: settings, consent, jobs, results -> UI
```
HTTP: one small `HttpClient` interface in `wabcut_ai`. The app injects a Qt Network (QNetworkAccessManager) implementation and
tests inject a fake. Streaming uses SSE/chunked responses. TLS comes from the platform via Qt.

## Capabilities (typed, provider-agnostic)
| Capability | Input | Output | Local default | Cloud options (BYOK) |
|---|---|---|---|---|
| `Transcribe` | audio PCM (16 kHz mono) + language hint | `Transcript` with word timestamps, speakers, confidence | whisper.cpp | OpenAI / Deepgram / AssemblyAI / ElevenLabs / Gemini |
| `DetectSpeech` (VAD) | audio | speech segments | Silero VAD (ONNX) or whisper VAD | — |
| `DetectShots` | video frames (decimated) | cut points + confidence | histogram/adaptive detector (DSP), TransNetV2 (ONNX) optional | — |
| `DetectBeats` | audio | beats/downbeats/tempo | DSP onset/tempo tracker | — |
| `EmbedFrames` / `EmbedText` | frames / query text | vectors | SigLIP-class model (ONNX) | provider embeddings |
| `Segment` (mask/roto) | frame + prompt point/box | mask (+ tracked masks) | SAM-class model (ONNX) | — |
| `EnhanceSpeech` | audio | cleaned audio | DeepFilterNet-class model | provider audio APIs |
| `SeparateStems` | audio | voice/music/fx stems | Demucs-class model (heavy, optional download) | — |
| `Chat` / `Agent` (tool calling) | messages + tools + context | tool calls / text (streamed) | llama.cpp / OpenAI-compatible local server | Anthropic / OpenAI / Gemini / OpenAI-compatible |
| `DescribeVideo` (vision) | frames/clip + prompt | text/structured | local VLM (optional) | Gemini (native video) / Claude / OpenAI vision |
| `Speak` (TTS) / `Translate` | text | audio / text | — | ElevenLabs / OpenAI / others |
| `GenerateVideo` / `ExtendClip` | prompt + image/clip | video file | — | Veo / Runway / Luma / Kling / fal / Replicate (as available) |

Every result type carries `Provenance{provider, model, modelVersion, settingsHash, createdAt}`.

## Provider contract

Implemented in AI-1 (2026-10-11). Pure C++ in `src/ai` (`wabcut_ai`, no Qt); the network layer is in `src/ai_net`;
the app bridge is `AiController`.

### Layers
| Layer | Where | What |
|---|---|---|
| Types and policy | `wabcut/ai/*.h` (`wabcut_ai`) | `AiError`, chat types, capability interfaces, `IHttpTransport`, model registry, keys, consent, spend, request log, `ProviderRegistry`, `AiPlatform` |
| Wire adapters | `wabcut/ai_net/Adapters.h` (`wabcut_ai_adapters`, no Qt) | Anthropic Messages, OpenAI Responses, Chat Completions, Gemini Interactions; Deepgram, ElevenLabs Scribe, AssemblyAI |
| Qt transport | `wabcut/ai_net/Network.h` (`wabcut_ai_net`) | `QtHttpTransport` (QNetworkAccessManager on its own thread), `NetworkStack` = retry → request log → Qt |
| App | `src/app/src/bridge/AiController.*`, `qml/panels/ai/` | QML singleton, QtKeychain store, Settings > AI, consent prompt |

### Errors (`AiError.h`)
`std::expected<T, AiError>` everywhere. `AiError{code, message, retryAfter, httpStatus, providerCode}`:
- The contract's codes: NoCredentials, Unauthorized, RateLimited (with `retryAfter`), QuotaExceeded, PayloadTooLarge, Network,
  ProviderError, Cancelled, ModelMissing, Deprecated.
- Added for the platform: InvalidRequest (400 / failed validation), Unsupported (e.g. video on a frames-only model), ConsentRequired,
  Offline (offline mode or the project lock) and SpendCapExceeded.
- `toError()` maps an AiError onto `wabcut::Error` at boundaries that speak only that. Messages are redacted.

### Canonical types (`Chat.h`, `Capability.h`, `ToolSchema.h`)
- **Messages.** A `ChatMessage` is a role (User / Assistant) plus parts: `TextPart`, `ImagePart` (bytes or URL), `VideoPart` (a provider
  file URI or a local file to upload), `ToolCall`, `ToolResult` and `ReasoningPart`. A ReasoningPart is provider state that must be
  replayed verbatim (Anthropic thinking signatures, OpenAI encrypted reasoning, Gemini thought steps); adapters drop other providers'
  parts.
- **Tools.** A `ToolSpec` is {name, description, parameters, strict}. Parameters use the **portable JSON-Schema subset**: what the command
  catalog emits, and `checkPortableSchema()` enforces it. `adaptSchema()` rewrites it per provider's strict mode:
  - Anthropic: bounds move into the description.
  - OpenAI strict: every property is required, and optional ones become nullable.
  - Gemini: no `additionalProperties`.
  `toolSpecsFromCommandExport()` turns `command::exportTools(JsonSchema)` into ToolSpecs, and every command of the catalog checks as portable.
- **Requests.** A `ChatRequest` carries {system, messages, tools, toolChoice, responseFormat, maxOutputTokens, temperature, stopSequences}.
  Models that reject forced tool choice or a custom temperature have flags in the registry, and adapters adapt to them (auto + strict
  tools; no temperature).
- **Responses.** A `ChatResponse` carries {message, stopReason, usage, model, id}. Streaming `ChatEvent`s are TextDelta,
  ToolCallStarted, ToolCallArgumentsDelta, ReasoningDelta and UsageUpdate, each with the part index.
- **Interfaces.** `IChatModel::chat(request, onEvent, stop)`, `ITranscriber::transcribe(TranscribeRequest, progress, stop)`,
  `IEmbedder`, `ISpeechSynth`, `ITranslator` and `IMediaGenerator` (create / poll / fetch).
  - Calls are synchronous and cancellable through `std::stop_token`. `AiJob<T>` / `startAiJob(executor, work)` runs them as background
    jobs, which is the contract's JobHandle.
  - `TranscribeResult` = {the AI-2 `Transcript`, duration, usage, requestId}. Adapters normalize provider seconds and milliseconds to
    source `Ticks` (plus `origin`), speakers to 0, 1, … and ISO 639-3 / `en_us` codes to ISO 639-1.
- **Money.** Integer micro-USD (`MicroUsd`). `Usage` holds tokens (input, output, cached, cache writes, reasoning), audio milliseconds and
  characters.

### Transport (`Http.h`, `Transport.h`, `Redact.h`)
- **`IHttpTransport::send`.** `send(request, handlers, stop)` blocks a job thread until the exchange ends.
  - Request bodies are text, a file streamed from disk, or a multipart form.
  - 2xx bodies are delivered buffered or decoded by the shared `StreamDecoder`: SSE events (comments skipped, CR/LF/CRLF, multi-line
    data) or NDJSON lines.
  - Error statuses come back as responses with buffered bodies, and `classifyHttpError()` maps them per provider body. For example,
    Anthropic `enforced_spend_limit_reached` and OpenAI `insufficient_quota` 429s are QuotaExceeded, not RateLimited.
- **`RetryingTransport`.** Retries Network errors that happen before any 2xx head, and 408/429/5xx/529 responses that are retryable.
  - It waits for `Retry-After` / `retry-after-ms` (up to 120 s; a longer hint returns RateLimited with the hint).
  - Otherwise it uses exponential backoff with ±20% jitter (1 s, 2 s, 4 s; at most 3 retries).
- **`LoggingTransport` + `RequestLog`.** Every exchange is recorded with method, redacted URL, status, sizes, duration and a redacted,
  truncated request summary (base64 elided).
  - Every guarded call is recorded too, with usage and cost.
  - The log is a bounded ring kept on this computer, appended as JSON lines.
- **`Redactor`.** Masks the auth headers of every provider, Bearer/Token/Key schemes, `?key=` and `upload_id=`, `sk-…` / `AIza…` key
  shapes, and every key KeyRing loaded.

### Registry (`ModelRegistry.h`, `src/ai/data/models.json`)
- **The data.** `models.json` is compiled into the library. Providers carry {id, api, baseUrl, auth scheme, env var, key check, local,
  configurableBaseUrl, extra headers, consoleUrl, termsUrl, dataPolicy}. Models carry {id, provider, api, endpoint, capabilities,
  prices, deprecation, retirement, license}. Every entry has `verified` and `source`.
- **Capabilities.** chat / transcribe / embed / speak / translate / generateMedia, plus vision, toolsOnChat, toolsOnResponses,
  strictSchema, structuredOutput, forcedToolChoice, temperature, streaming, videoInput (none / frames / native), wordTimestamps and
  diarization.
- **Prices.** Price rules have optional `from` dates (Gemini 3.8 Flash doubles on 2027-01-01) and `aboveInputTokens` tiers (Haiku 5.5
  over 100K, GPT-6.x over 272K). `costOf(model, usage, date)` rounds up.
- **Seeded and verified on 2026-10-11:**
  - Chat: Claude Sonnet / Opus / Haiku 5.5 and Fable 5.1; GPT-6.1 Sol, GPT-6 Astra and GPT-6 Luna (Responses); Gemini 3.8 Flash,
    3.1 Pro (preview) and 3.5 Flash-Lite (Interactions).
  - Transcription: Deepgram Nova-3, ElevenLabs Scribe v2 and AssemblyAI Universal-3.5 Pro.
  - OpenAI-compatible providers: OpenRouter, Groq, Mistral, Ollama, LM Studio and llama-server (port 9931).
  - **Unverified:** `voxtral-mini-latest` (Mistral). Its word-timestamp response shape is undocumented and its docs disagree on the
    auth header, so it has no adapter yet.
- **User entries.** Users add model ids to local and OpenAI-compatible providers (`addModel`) or whole custom providers.

### Providers and the platform (`Provider.h`)
- `ProviderRegistry` maps api ids to adapter factories (`net::registerCloudAdapters()` registers all seven).
  - It keeps the preference per capability and base-URL overrides for configurable providers.
  - `resolve(capability)` returns the usable models in preference order, skipping missing, deprecated, adapter-less, keyless and
    offline-blocked ones.
- `AiPlatform::chat(model, CallOptions)` / `transcriber(...)` return guarded capability objects. Every call runs, in order:
  1. Check the offline and project locks and consent (`allowOnce` = "Allow once").
  2. Reserve the pre-flight cost ceiling against the provider's monthly cap.
  3. Run the call.
  4. Settle the actual cost from the response's usage (or release it on failure).
  5. Log the call.
- `preflightChat` / `preflightTranscription` give the UI the consent state, the cost estimate and the remaining budget.

### Keys (`Secrets.h`)
- `Secret` is move-only, wiped on destruction and shown masked (`••••abcd`).
- `SecretStore` backends:
  - `MemorySecretStore`: session only; the fallback when no keychain service exists.
  - `PlaintextFileSecretStore`: explicit opt-in only.
  - QtKeychain 0.17 in the app: Windows Credential Manager `provider:<id>@Wabcut`, macOS Keychain, Secret Service. It is an overlay
    port built against the app's Qt, behind `WABCUT_WITH_CLOUD_AI` / vcpkg feature `ai-cloud`.
- `KeyRing` reads `WABCUT_<PROVIDER>_API_KEY` first, then the store, and caches the result. It registers every key with the Redactor.
- `validateKey()` makes each provider's cheap authenticated call, and keys are checked on save. QSettings holds only {hasKey, last4,
  validatedAt}.

### Consent and spend (`ConsentLedger.h`, `SpendGuard.h`)
- **Consent.** Recorded per provider × capability ("Always allow"); "Allow once" is per call.
  - Offline mode and the project lock block every cloud call. Neither consent nor "Allow once" overrides them.
  - The project lock is a project setting (`ProjectState::aiNoCloud`, see "Security"); the ledger reads the ACTIVE
    project's value at every check through `setProjectLockSource()`.
  - Loopback-local providers need no consent and work offline. A "local" provider pointed at another host counts as cloud.
  - `makeConsentPrompt()` gives the prompt its content: what is sent, the destination host, the provider's terms link and data
    policy, and the cost.
- **Spend.** Monthly caps per provider are enforced on this computer. The estimate is conservative: input ≈ bytes / 4, plus 1,600
  tokens per image, and the full `maxOutputTokens` as the ceiling. Reservations make concurrent jobs respect the cap. Totals persist
  as JSON.
  A call that fails or is cancelled still counts what it may have cost (see "Security", spend accounting).

### Security (AI-6)
The second-wave review (docs/internal/reviews-codex-wave2.md) found five AI-platform issues. Each is fixed with regression tests;
the rules below hold for every adapter and every caller.

- **Server-returned URLs are checked before anything is sent to them (P1-1).**
  - `parseHttpUrl()` is a strict parser: http(s) only, no whitespace, control characters or backslashes, a valid
    host and port.
  - Gemini's Files API returns the resumable `x-goog-upload-url`. The API key and the user's video go there only if
    it is HTTPS on `generativelanguage.googleapis.com` (or the configured HTTPS base host), with no userinfo, the
    default port and an `/upload/` path. Otherwise the call fails closed with ProviderError ("refused to upload the
    video…"), naming only the host and never the session id.
  - Resource ids that a server returns and that go into URL paths are validated too: the Gemini file name
    (`files/<id>`) and the AssemblyAI transcript id.
- **External JSON is read with type-checked accessors (P1-2).**
  - `JsonAccess.h` provides `jsonString`, `jsonInt`, `jsonNumber`, `jsonBool`, `jsonObject`, `jsonArray`, `jsonAt`
    and `jsonIdOr`. They never throw: a field of the wrong type is simply absent.
  - Every provider field the adapters read (chat streams, Files API, transcription responses) and the local
    settings/cache loaders use them.
  - Three catch boundaries back this up:
    - `ChatAccumulator` never throws. A delta for a missing or wrong part is a ProviderError, and `guardEvent()` turns
      any exception into one.
    - `StreamDecoder` catches handler exceptions and reports them as `error()`.
    - `QtHttpTransport` wraps every network-thread callback (`readyRead`, upload progress, finish, `start`). An
      exception aborts the reply, and the exchange completes exactly once.
- **Streams are bounded (P2-9).** A stream past a bound is aborted with PayloadTooLarge; a flooding helper ends with
  `ReadStatus::Overflow` and is killed.

  | What | Limit |
  |---|---|
  | `StreamLimits`: a whole SSE/NDJSON body | 64 MiB |
  | `StreamLimits`: one line | 1 MiB |
  | `StreamLimits`: one SSE event | 4 MiB |
  | One response's text + reasoning | 8 MiB |
  | One tool call's arguments | 1 MiB |
  | Parts of one response | 1,024 |
  | Helper (`ChildProcess`) output: one line | 16 MiB |
  | Helper output: unread | 64 MiB |

  Raw downloads are bounded by the model store's manifest size instead.
- **Spend accounting survives failures (P2-10).**
  - Every guarded call runs through a per-object `TrackingTransport`, which records whether the request was sent, its
    body left this machine, or a 2xx head arrived.
  - `GuardedChat` keeps the streamed usage and output.
  - A reservation is released only for a call proven unprocessed: never sent, or refused with an error status before
    any output.
  - Otherwise it is settled with:
    - the reported usage, plus any output after the last report (bytes / 4);
    - or, for an answered call without usage, the input estimate plus the streamed output;
    - or, for a request sent but never answered, the input estimate. A transcription upload settles its expected
      cost.
  - `Settlement` is RAII: an exception in the middle of a call still settles its reservation.
- **The project no-cloud lock belongs to the project (P1-4).**
  - It is stored as `ProjectState::aiNoCloud`. ProjectIO writes `"aiNoCloud": true` only when it is set, so unlocked
    projects are byte-identical and older files open unlocked. It is saved with the project and its autosave
    recovery copies.
  - Changing it is an edit: one undo step, with `ChangeSet::aiLockChanged`.
  - `AiController` binds the consent ledger to the active session (`setProjectLockSource`), so every guarded call
    reads the ACTIVE project's lock. That covers chat, the editing assistant, cloud transcription and Gemini video.
  - Reopened, recovered or switched-to projects decide for themselves, and recorded consent never overrides a lock.
- **Audit (AI-6).** Redaction:
  - HTTP and stream errors are redacted in `classifyHttpError`, Qt network errors in `networkError`.
  - The request log stores redacted URLs and headers. Guarded-call log details are redacted.
  - Keys never reach QSettings.
  - The agent transcript redacts every string, including the staged batch's tool parameters (`redactedJson`).
  - The app writes no crash dumps of its own.

  Consent on media uploads:
  - Agent tools run locally. Analysis and transcript reads use the local cache, and the agent sends text only.
  - Gemini video uploads happen inside a guarded chat call.
  - Cloud transcription goes through `AiPlatform::transcriber()`. `TranscriptBridge::transcribe(scope, allowOnce)` sets
    "Allow once" only from the consent dialog's answer (it used to set it for every job) and refuses up front without
    consent.
  - The project lock and offline mode block all three, whatever consent or "Allow once" says.
- **Review wave 3** (docs/internal/reviews-codex-wave3.md):
  - **Model arguments (P1-2).** The agent checks a tool call's arguments against the command's schema before it
    describes or runs it. A mismatch goes back to the model as an `InvalidParams` tool result. Descriptions use
    type-checked accessors and never throw. `EditAgent::run`, and the app's worker thread around it, turn anything
    unexpected into a Failed outcome, and the live project is never touched.
  - **Every dispatch is authorized (P1-5).** A guarded call checks consent, offline mode and the project lock again
    before EACH request it sends, not once per call. `TrackingTransport` checks on every send, and
    `RetryingTransport` checks before every retry through `HttpRequest::beforeDispatch`. This covers Gemini's
    upload start, the upload and the polls. The call is also bound to the project it started on:
    `ConsentLedger::projectIdentity()`, which the app bumps whenever the session is reset (a new, opened or
    recovered project), must not change. A lock turned on, or another project opened, mid-operation therefore stops
    the next request before it leaves.
  - **Verification scope (P2-9).** "No edits outside" compares what each clip still shows outside the scope:
    - every stretch of every clip outside the scope must still be on its track, with the same rendering (media,
      speed, direction, enabled, content) and the same moment of its source;
    - before the scope it must be in place; after it, it may move by one ripple offset per track.
    Partial trims inside the scope pass. A clip removed or changed as a whole because it overlaps the scope fails, and
    so does content that only another instance of the same source still shows.
### Local models
- Local models are downloaded on demand (not bundled) into a user-cache model store (AI-2: `ModelStore`). Each download is verified by
  SHA-256 and shows its license, and the store is browsable / removable in Preferences.
- Bundling is decided case by case after a license check.
- `IHttpTransport` (with Range headers and `BodyMode::Raw`) is the HTTP seam `ModelStore`'s `IDownloadTransport` can switch to.

### Testing
- Recorded-fixture contract tests in `tests/ai/fixtures/<provider>/` (suite `ai_net`):
  - Request snapshots; SSE streams replayed in small chunks through the real `StreamDecoder`.
  - Tool calls, reasoning replay, errors, and 429 with Retry-After.
- The Qt transport runs against a loopback server.
- Live smoke tests run only with `WABCUT_LIVE_<PROVIDER>_KEY` and never in CI.
- Platform policy tests run on a fake transport (suite `ai`), and `AiController` and screenshots are in suite `ai_ui`.

## Flagship features (MVP AI wave)
1. **Transcript-first editing.**
   - Transcribe clips or a sequence, then edit the video by editing the text: delete words → ripple-delete the matching ranges; drag
     paragraphs → reorder; search the transcript; click a word → playhead.
   - Filler-word and long-silence removal are one action with a preview.
   - Captions are generated from the transcript as a caption track, with styles including animated/kinetic captions.
2. **AI command bar (agent).**
   - Press `Ctrl/Cmd+K`-style palette (the binding is chosen not to collide with Premiere's Add Edit; e.g. `Ctrl+Space`) and describe the edit: "remove
     silences over 0.8 s on A1", "make a 30 s highlight", "add captions in Thai", "cut on the beat of the music on A3".
   - The LLM sees a compact sequence/transcript summary and calls **tools** that map 1:1 to model EditOps and AI capabilities.
   - The result is an `EditPlan` previewed as a diff (ghost clips on the timeline) and applied as a single undo step.
   - Works with Claude, OpenAI, Gemini or a local OpenAI-compatible server.
3. **Smart search.** Natural-language search over all footage ("drone shot of the coast at sunset", "where she says
   'launch'") combining frame embeddings and transcripts, with hits as subclips you can drop on the timeline.
4. **Auto shot detection + beat sync.** Split long clips at detected cuts, and snap and auto-cut to detected beats (BPM
   timeline idea for everyone).
5. **Audio cleanup.** Enhance Speech (denoise/dereverb), auto-ducking of music under dialogue, and loudness normalization to
   platform targets (YouTube/TikTok/broadcast).
6. **Smart reframe** for vertical/square deliverables via subject tracking (detector + smoothing), keyframing the transform.
7. **Object mask / background removal** via prompt-point segmentation with tracking, feeding the compositor as a mask.
8. **Generative B-roll / extend (BYOK, optional).** Generate or extend shots via the user's generative-video provider, with
   provenance; results land in the bin as ordinary media.

## Transcription service

AI-7: one way to turn media audio into a `Transcript`, shared by the Transcript panel (`TranscriptBridge`), the
`transcript.transcribe` command (wabcut-cli `cmd`/`mcp`, the app) and `wabcut-cli transcribe`. `ai::TranscribeService`
(`src/ai/include/wabcut/ai/TranscribeService.h`, library `wabcut_ai_transcribe`, full builds: it decodes media).

**Routing.** source (media file + container stream) → identity (`media::sourceFingerprint` + the decoder's stream) →
- the analysis cache: the newest `"transcript"` entry of the stream, any provider and settings (`AnalysisCache::newest`).
  A cached transcript needs no provider, so a build without local AI still serves what the app or a cloud model made;
- else the provider (`TranscribeJobOptions::provider`):
  - `"local"`: whisper.cpp in the wabcut-infer helper (`TranscriptionService`: 16 kHz extraction, Silero VAD chunks,
    cancel / timeout / crash isolation), when the helper was built;
  - `"cloud:<model id>"`: the model's guarded `ITranscriber` from `AiPlatform::transcriber` (offline mode and the project's
    no-cloud lock, consent, monthly spend cap, request log, on every request). `allowOnce` is the host's "Allow once"
    answer; it never overrides the locks and is never a command parameter;
  - `"auto"`: local when built, else the user's preferred cloud transcription model, else `NotFound` with how to set one up;
  - any id added with `addProvider()` (tests, a host's own engine).
- → stored in the analysis cache under the source fingerprint (cloud: `cloud/<model>`, params model + language; local:
  TranscriptionService's own key; added providers: `custom/<id>`), with `provenance.params["provider"]`, where
  `CachedTranscriptProvider` and every transcript command find it. `useCache = false` (force) re-transcribes and replaces it.

**Cloud uploads.** The audio is extracted once to raw 16 kHz mono float32 and uploaded as 16 kHz mono 16-bit WAV. Audio longer
than one request (`cloudChunk`, 10 min, and the model's `maxAudioSeconds` / `maxUploadBytes`) is cut at pauses: speech from
the helper's Silero VAD when built, else from the level (`energySpeech`: 10 ms RMS frames 12 dB over the noise floor), stretches
split at pauses of 0.3 s or more and packed into requests up to the limit (a stretch longer than the limit alone is cut evenly).
Each request carries `origin` (its first sample's source time), so the words come back in source time; the parts are joined
in order with times kept non-decreasing at the seams.

**Jobs.** `transcribe()` returns a `media::Job<TranscribeOutcome>` (own thread, progress 0..1, `cancel()` stops the helper or
the upload); `run()` is the same on the calling thread. `TranscribeOutcome` says what ran (`provider`), whether it came from
the cache, the device local whisper ran on and a `note` for the user when it fell back to the CPU (`cpuFallbackNote`: no Vulkan
driver or device, or a build without the Vulkan backend).

Local extraction, cloud uploads and VAD energy inputs each reserve a job directory before writing audio. The
directory is claimed atomically across processes and removed on success, cancellation or extraction failure;
concurrent editor and CLI jobs never reuse an upload path. Cloud policy is checked again for each chunk.

**`transcript.transcribe`** (Capability `MediaAnalysis`; `TranscriptCommands.h`). Params: `media` (media items), `clips`, or
neither for every audio clip of the sequence (each media stream once); `provider`; `language`; `force`; `check` (only list the
providers with availability and which audio has a transcript). The result has one row per media stream (`transcribed`,
`cached` or `failed` with the error) and the counts; it fails only when nothing could be made. Nothing in the project changes
(transcripts are analysis data). Hosts provide a `MediaTranscriber` as `TranscribeServices`; full builds have
`ServiceMediaTranscriber` over TranscribeService (`MediaServices.h`).
- `wabcut-cli cmd` serves it with local transcription and the cache only (a headless request cannot answer a consent prompt).
- `wabcut-cli mcp` serves it (and `audio.loudness`) only with `--allow-analysis`; network commands are never served there.
- The editing assistant does not get it (its tools are Read + EditSequence).

```
wabcut-cli transcribe <media> [--provider auto|local|cloud:<model>] [--allow-cloud] [--language th|en|auto]
                              [--model base|large-v3-turbo|tiny] [--json|--srt|--vtt] [--force | --no-cache] [--providers]
```
`--allow-cloud` is the consent for that file (keys come from `WABCUT_<PROVIDER>_API_KEY`); `--providers` lists what is
available and why not.

**Audio energy (`AudioEnergy.h`).** `ai::AudioEnergyService` measures a list of ranges (media time) of a media audio stream as
one job: RMS and peak in dBFS (`-inf` for digital silence) and, when asked and the helper is built, the fraction of each range
Silero VAD calls speech. Ranges are decoded in time order with one decoder at 16 kHz mono; results are kept per range in
memory and per request in the analysis cache (`"energy"`, `energy-1`). Silence detection uses it through
`ServiceEnergyProbe` (an `AudioEnergyProbe`): `review()` hands every pause to `AudioEnergyProbe::prefetch()` first, so all of a
media file's pauses are measured in one pass and the per-pause `rmsDb()` calls answer from memory. The Clean-up review can
start `measure()` as a background job and run the review when it finishes. wabcut-cli's transcript services use it.
RMS and peak accumulate in one-second blocks, including when streaming PCM to the VAD, so a long range does not
require an equally long in-memory sample vector. Damaged energy-cache records are treated as cache misses.

## Text-based editing

Transcript-first editing core (AI-3): `src/ai/edit`, library `wabcut_ai_edit`. It is pure C++ on the model, command and ai
libraries, and deterministic. Its media-backed services are in `wabcut_ai_edit_media` (full builds only). The Transcript
panel (UI) builds on it later.

**Transcript ↔ timeline (`SequenceTranscript`).**
- Transcripts belong to a media audio stream, in source time. A sequence is heard through its audio clips: a word is on the
  timeline when its midpoint lies inside a clip's source window, at the position the clip's speed and direction give it.
  Trimmed-away words are gone; a word cut by an edit point is clamped to its clip.
- Words are listed in timeline order (start, then A1..An). Each carries its clip, track, link group, source range, speaker
  and confidence.
- Video clips contribute nothing: a linked A/V pair is heard through its audio. Selecting words selects the whole link group
  (`clipsFor`). A link group holding the same media stream twice (a stereo pair split over two tracks) is heard once.
- `update(next, changeSet)` keeps the map current after an edit. Clips that only moved shift their words, changed clips are
  mapped again, and track, link or media changes rebuild it. An incremental update always equals a full build (tested).
- Perf (RelWithDebInfo, `[perf]` test), with 10,000 words (1 hour) on 200 linked clips:
  - build: 0.8 ms (budget 50 ms);
  - update after a ripple delete that shifts 100 clips: 0.2 ms (budget 5 ms).

**Text edits → commands (`TextEdits.h`).** Edits are planned on the transcript and emitted as a `CommandBatch` of catalog
commands (`sequence.extract`; a move adds `sequence.addEdit` and an insert-mode `clip.move`). They are therefore validated,
previewable with `dryRun`, and one undo step.
- *Delete words:*
  - Cuts fall on frame boundaries just outside the words: 40 ms padding each side by default, never reaching into a kept
    neighbour (the frame holding a neighbour's edge stays).
  - Deletions that overlap, or that have no kept word between them (and are ≤ 1 s apart), merge into one cut.
  - Pauses can be removed too, keeping `roomTone` (150 ms) next to each word.
  - Cuts go on the tracks the words are heard on plus their linked partners' tracks. Sync-locked tracks ripple with them;
    other tracks stay. Words heard on a locked track cannot be deleted.
- *Keep ranges* (rough cut): everything outside the kept words is deleted, from the first to the last clip that has words.
- *Move range*: lift and insert at the word boundary before another word, then close the place the words left.

**Review (`Review.h`).**
- *Fillers* come from a language-aware lexicon of data files, `src/ai/edit/data/fillers.<lang>.txt`. These are compiled in;
  add a file to add a language, or pass `ReviewOptions::lexicons`.
  - `en`: um, uh, er, ah, hmm…; *in context only*: like, you know, I mean, sort of, kind of, basically.
  - `th`: อืม, เอ่อ, เออ, อ่า; *in context only*: แบบ, แบบว่า, คือ, ก็คือ, ประมาณว่า, อ่ะ.
  - Contextual entries count only when disfluent: a pause ≥ 150 ms beside them, a trailing comma, or said twice.
  - Matching ignores case and surrounding punctuation. Unspaced languages (Thai) match across word units.
  - Confidence:

    | Filler | Confidence |
    |---|---|
    | Lexicon filler | 0.9 |
    | Contextual filler | 0.6, or 0.75 with pauses on both sides |
    | ASR confidence < 0.4 | −0.1 |
    | Shorter than 60 ms | −0.2 |
- *Silences*: gaps between words longer than a threshold (0.8 s by default), checked against the audio energy (an
  `AudioEnergyProbe`; `DecoderEnergyProbe` decodes at 16 kHz mono). Silero VAD can replace the probe later.

  | Gap audio | Confidence |
  |---|---|
  | Quiet (≤ −40 dBFS) | 0.9 |
  | Louder (laughter, music) | 0.3 |
  | Not measured | 0.7 |
- Output is a review list: kind, word indices, range, confidence, label, and a context snippet. `deleteRequestFor(items)`
  turns chosen items into a delete, with room tone on removed pauses.

**Captions (`CaptionBuilder.h`).**
- Cues have at most `maxLines` lines of `maxCharsPerLine` display cells (Thai combining vowels and tone marks take none),
  and last at most `maxDuration`.
- Cues break at pauses, speaker changes, and sentence ends once half full. Lines break between words, as balanced as
  possible, preferring breaks after punctuation.
- Thai joins its word units without spaces and breaks only between units, never at a space that is not there.
- The track is stored in the project as a minimal caption track: `Sequence::captionTracks`, schema 4, with
  `edit::setCaptionTrack` / `removeCaptionTrack`.
- `toSrt` / `toVtt` match `ai::toSrt` / `ai::toVtt` exactly (numbering, ms rounding, VTT escaping) but keep the cue's line
  breaks.

**Commands (`TranscriptCommands.h`, registered in `editingCommands()` = built-in catalog + these).**

| Command | Access | Purpose |
|---|---|---|
| `transcript.transcribe` | MediaAnalysis | Transcribe media / clips / the sequence (see "Transcription service") |
| `transcript.text` | Read | Paragraphs with word indices |
| `transcript.words` | Read | Word table |
| `transcript.review` | Read | Fillers and silences |
| `transcript.deleteRanges` | Edit | Delete words and gaps, with padding and room tone |
| `transcript.keepRanges` | Edit | Rough cut |
| `transcript.moveRange` | Edit | Move words before another word |
| `transcript.cleanup` | Edit | Remove all fillers and silences ≥ `minConfidence` |
| `captions.generate` | Edit | Make a caption track |

- `captions.get` and `captions.remove` are built in, and `sequence.summary` lists caption tracks.
- Transcripts reach the commands through `command::CommandServices` (`EnginePolicy::services`, `McpServerOptions::services`)
  as `TranscriptServices{provider, energy probe}`. The command library itself stays free of AI code.
- `wabcut-cli` (`cmd`, `mcp`, `tools`) serves `editingCommands()`. Transcripts are found by source fingerprint in the
  analysis cache that transcription fills (`CachedTranscriptProvider`, newest entry per stream).

```
wabcut-cli transcript-edit <project> --remove-fillers --remove-silences 0.8 [--min-confidence 0.5] [--dry-run] [--save]
wabcut-cli transcript-edit <project> --review
wabcut-cli captions <project> --sequence <id> --srt out.srt [--vtt out.vtt] [--max-chars 42] [--save]
  both: [--cache <dir>] [--transcript <media id or name>[:<stream>]=<transcript.json>]
```

## Dialogue-first audio

AI feature 5 (audio cleanup) and the audio items of Wave A (AI-4). The DSP is in `wabcut_audio` (`src/audio/include/wabcut/audio/dsp`,
`effects`), the render path in `wabcut_playback` (TrackRenderer, OfflineMixdown, the master bus; `src/playback/src/audio/README.md`),
and the commands in `wabcut_audio_edit` (`src/audio/edit`; pure, so they also build in the core preset), with the media-backed
analysis in `wabcut_audio_edit_media`.

- **Audio effects in the render path.** Gain, 3-band EQ (cookbook shelves and a peaking band), compressor (soft knee, log-domain
  attack/release), true-peak limiter (BS.1770 4x detection, lookahead, no overshoot) and Reduce Noise (RNNoise 0.2, 48 kHz) run per
  clip, per track and on the master bus. Curves are evaluated on a fixed 32-frame grid from the render origin, gain ramps sample by
  sample, so fades and automation are sample-accurate and preview and export produce the same samples. Clip and track stacks run on
  the player's feeder thread ahead of the callback (lookahead effects read ahead instead of adding latency); the master stack runs in
  the callback as an immutable program swapped by one atomic pointer. Export calls the same code (OfflineMixdown).
- **Enhance Speech and stems (A.4).** The offline Enhance Speech effect (`wabcut.audio.enhanceSpeech`: the media's
  DeepFilterNet3 take, blended by Strength before the clip's level and redactions) and stem separation (HTDemucs):
  `audio.enhanceSpeech`, `audio.separateStems`. See "Enhance Speech and stem separation (A.4)" below.
- **Audio sync (A.5).** `audio.syncClips`: recordings of one event lined up by their audio (dual-system sound, multicam),
  with confidence and clock drift. See "Audio sync (A.5)" below.
- **Nested and multicam audio (A.5 part 3).** Nested sequences play their mix (with the nested master stage) and multicam clips
  their active angles, crossfading at angle switches, in preview and export alike. See "Nested-sequence and multicam audio"
  below.
- **Audio plugins (A.6).** CLAP and VST3 effects on clips, tracks and the master, run in the crash-isolated `wabcut-plughost`
  helper; `plugins.list` / `plugins.scan`, plugin effects through `clip.addEffect` / `audio.addEffect` ("plugin"). See
  "Audio plugins (A.6)" below and [plugins.md](plugins.md).
- **Essential Sound and Music Remix (A.7).** `audio.essentialSound` tags clips as dialogue, music, sfx or ambience and applies
  a preset as ordinary, editable effects (new: filter, DeHum, DeEsser, expander, reverb, stereo width); `audio.remix`
  retimes music to a length by rearranging its bars. See "Essential Sound (A.7)" and "Music Remix (A.7)" below.
- **Voice-over recording (A.8).** Premiere's Voice-over Record: the microphone onto the armed track at the playhead or between
  In and Out, with count-in, pre-/post-roll, input monitoring and latency compensation; repeated passes stack as takes.
  `track.setRecordArm`, `record.takes`, `record.useTake`; recording itself is the app's (no command, no agent). See
  "Voice-over recording (A.8)" below.
- **Audio channels and 5.1 (A.9).** Modify > Audio Channels on clips and media items (presets: Fill Left / Right, swap,
  sum to mono, split to mono; per-channel volume), mono / stereo / 5.1 / adaptive tracks, a 5.1 master with a VBAP
  panner and LFE send, the ITU-R BS.775 downmix for stereo preview and export, 5.1 AAC / AC-3 export and 5.1 loudness.
  See "Audio channels and 5.1 (A.9)" below.
- **Track roles.** `Track::role` (dialogue, music, effects), `track.setRole`.
- **Loudness.** `audio.loudness` measures the mix (what export writes), a track or a clip on its own through the real render path
  (libebur128: integrated, LRA, true peak, max momentary/short-term), cached in the AnalysisCache by a fingerprint of everything that
  changes the audio. `audio.normalize` targets YouTube / Spotify (-14 LUFS), podcasts / Apple Podcasts (-16), broadcast EBU R128
  (-23), ATSC (-24) or a custom value: a Gain at the top of the clip, track or master stack, measured again and refined until within
  0.1 LU (so compressors and limiters after it count); `"limit": true` first adds the master true-peak limiter at the preset's
  ceiling (`audio.masterLimiter` on its own). One undo step.
- **Auto-ducking.** `audio.autoDuck` finds speech on dialogue-role tracks (transcript words where the media has a transcript, else
  the rendered level: 10 ms RMS, 12 dB above the noise floor, hysteresis), builds the envelope (down by `amount` over `attack` before
  each phrase, back over `release` after `hold`, short pauses bridged) and writes plain linear volume keyframes on the music-role
  clips, replacing their keys in the range like Premiere's Auto Ducking. One undo step; `dryRun` returns the preview state.
  Silero VAD (AI-2's helper) is a possible third source; transcripts and level cover the MVP. `"preset": "gameUnderVoice"`
  (A.10) ducks a gaming recording's game audio and music under the streamer's mic, with tracks found by name; see "Streamer
  voice and Game under voice (A.10)" below.
- **Variable-speed audition.** Forward 1/4x to 4x (J/K/L 2x and 4x, slow shuttle) is heard, pitch preserved: the player mixes
  speed x frames of timeline per device block and a streaming WSOLA (20 ms window, +-5 ms similarity search) stretches it. Preview
  only. Reverse and 8x stay muted.

### Track faders, pan and the master fader (A.3)

Premiere's track Volume and Panner and the master fader, keyframable as track automation.

**Design decision: intrinsic parameters stored like a clip's, not entries in the effect stacks.** The track fader and
panner are `Track::volume` / `Track::pan` and the master fader is `Sequence::masterVolume`, each a `ParamValue` (a constant
or a keyframe curve in timeline time). They are the parameters of the intrinsic `wabcut.volume` / `wabcut.panner`
descriptors (`ParamId::VolumeDb`: dB, 0 = unity, -144..+24; `ParamId::Pan`: -1..+1), so they keep their units, ranges and
defaults. They are edited through the existing target-taking parameter and keyframe operations
(`edit::setParameter / addKeyframe / moveKeyframe / removeKeyframe / setKeyframe / setAnimated` with
`EffectTarget::ofTrack(track)` or `EffectTarget::masterAudio()` and `ParamRef::of(ParamId::VolumeDb | ParamId::Pan)`). That
is how clips already hold Volume/Panner (`ClipProperties`, never a stack entry).

This reuses everything an intrinsic effect in `Track::effects` would: curves and their evaluation, the generalised curve
operations, IO (the clip parameter JSON), undo (pure state to state operations) and the commands. It avoids what a stack entry
costs:
- `effectStackProblem` rejects an intrinsic effect in a stack, so older builds would fail to load such a project instead of
  ignoring a key;
- every stack consumer (the UI's effect lists, `audio.effects`, reorder, remove, enable, the effect chains) would have to
  skip, pin or special-case the entry;
- its fixed post-stack evaluation would contradict the stack order;
- the UI and agents would first have to find its instance id.

The plain-field alternative is what this is, typed as a `ParamValue` so that automation comes free.

**Evaluation.**
- The track fader and panner come after the track's effect stack. Export (OfflineMixdown) and nested sequences apply them
  in TrackRenderer. The live player applies them in its audio callback (since A.8, for live overrides): the feeder
  renders the tracks pre-fader and publishes each track's fader program (a copy of the two curves, one atomic pointer,
  retired once the callback is past it, like the master program).
- The master fader runs in the master bus program in the callback, after the master effects and before the master meter
  and soft clip. The program carries a copy of the curve, published with one atomic pointer like the master stack;
  `ParamValue::valueAt` does not allocate.
- All three use one helper (`playback/src/audio/Fader.h`): values at the 32-frame parameter-grid points, linear ramps
  between, sample by sample. So ramps are sample-accurate and independent of block size. The player's grid counts from
  timeline 0, so export from the start equals preview sample for sample (`[playback][faders][export]`).
- A fader edit is heard from the next block (a new program; nothing queued is rendered again). A master edit swaps the
  program (the next block).
- **Live overrides (automation passes).** `TimelineAudioPlayer::setLiveTrackMix(track, gainDb, pan)` (`TrackId{}`: the
  master) replaces the evaluated fader and/or panner in the callback while set, heard from the next block: Touch, Latch
  and Write passes hear the dragged fader at once. Every change, and the way in and out, is a linear ramp over 10 ms (no
  zipper noise). The values are atomics (any thread). `clearLiveMix()`, `stop()` and a stopped transport clear them (the
  pass ends with playback, as in Premiere), and so does a slot that starts playing another track. The model is never
  touched: the UI writes the pass as keyframes when it ends. Tested: heard on the next block, monotonic within one
  ramp step, exact after 480 frames, pan, master, clear and the stop (`[playback][faders][live]`), and the RT audit with
  live moves every block.
- The loudness analysis cache keys include the faders.

**Pan law.** The track pan is a balance on the track's stereo mix: `audio::stereoBalance`, the unity-centre balance on the
constant-power taper.
- Centre leaves the mix untouched.
- Towards a side the far channel follows sqrt(2)·cos/sin((pan + 1)·pi/4), down to silence at the hard position (0.541,
  -5.3 dB, at pan ±0.5).
- The near channel stays at unity.

**Mute and solo** are Premiere's:
- a muted track, or one whose output is off, is silent;
- while any track is soloed only soloed tracks play;
- mute beats solo (a soloed and muted track is silent and still silences the others);
- no track is solo-safe.

**Project files.** `"volume"` / `"pan"` on a track and `"masterVolume"` on a sequence use the clip parameter format and are
written only when not 0 dB / centre. The schema stays 8, like AI-9's `derivedFrom` and I.6's masks, so projects without
faders are byte-identical. A build from before A.3 ignores the keys and plays the tracks at 0 dB, centre.

**Commands** (EditSequence, one undo step each):

| Command | What it does |
|---|---|
| `track.setVolume {track, db, time?, interpolation?}` | Sets the track fader. With `time` it adds a keyframe there; without, a constant replaces the automation. |
| `track.setPan {track, pan, time?, interpolation?}` | Sets the track panner, the same way. |
| `sequence.setMasterGain {db, time?, interpolation?}` | Sets the master fader, the same way. |
| `audio.addKeyframe` / `audio.moveKeyframe` / `audio.removeKeyframe {target, param: "volume" or "pan", ...}` | With no `effect`, edits the fader keys of a track (`"A1"`) or of `"master"`. |
| `audio.effects {target}` | Lists the fader after the stack: `volume` (and `pan` for tracks). |

`audio.autoDuck` now returns its plan in `data`, in a dry run too:
- per clip, `keyframes`: each key's timeline `time` / `ticks`, `curveTicks`, `db`, `interpolation` and the index of the
  `region` it comes from (null for the clip's own level);
- per clip, `regions`: the indices it uses;
- per region, `ticks`: `speech`, `down`, `upStart`, `up`.

So agents, CLI and MCP clients can inspect the plan, and the UI can draw ghosts. The old `keys` count is kept.

**The assistant** gets these as tools. `AgentOptions::analysisCommands` (default `audio.loudness`) lets it read the
staged mix's loudness, and AgentController provides the audio services. So "mix the music 6 dB under the dialogue and hit
-14 LUFS" stages `track.setVolume`, then `audio.normalize`, and checks with `audio.loudness`.

### Live loudness (A.3)

`audio::LoudnessTap` (`wabcut/audio/LoudnessTap.h`) measures EBU R128 / BS.1770-4 loudness off the audio thread:
- **On the audio thread:** `push()` copies each rendered block into a lock-free SPSC frame ring (1 s; a full ring drops and
  counts).
- **On a worker, every 20 ms:** it drains the ring into libebur128 (the same `dsp::LoudnessMeter` the offline analysis uses:
  K-weighting, 400 ms momentary and 3 s short-term windows, gated integration, 4x true peak) and publishes a
  `LoudnessReading` through a SeqLock. The reading holds momentary, short-term, integrated, true peak, the maxima, frames,
  dropped frames and resets.

TimelineAudioPlayer feeds it what leaves the player (post master effects, master fader and soft clip: what export renders;
shuttle audition is not measured) and exposes it:
- `loudness()`, from any thread;
- `resetLoudness()`;
- `setLoudnessEnabled(bool)`;
- `play()` from pause starts a new integration ("since play start").

In manual-feed mode (tests) `feedOnce()` drains it. The RT audit counts no allocations or locks in the callback with fader,
pan and master automation and the tap running.

**Accuracy:** the live readings equal an offline libebur128 measurement of the same samples. On a 10 s programme the
integrated, momentary, short-term and true peak values matched to within 0.1 LU / dB (see the test output line
`[loudness] live tap vs libebur128`), and the frame-exact tap unit test matches too.

### Wiring the Audio Track Mixer (note for the timeline engineer)

`AudioMixerBridge` today works against `faderAvailable = false` and the last offline analysis. With A.3:
- **Fader.**
  - `setGainDb(target, dB)`: run `track.setVolume {track, db}` (or `sequence.setMasterGain {db}` for `"master"`) through
    the session's command engine, or call
    `edit::setParameter(state, seq, EffectTarget::ofTrack(id) | masterAudio(), ParamRef::of(ParamId::VolumeDb), ParamValue(dB))`
    and commit.
  - For automation-on-drag, pass `time` = the playhead (`track.setVolume {track, db, time}` adds a key).
  - Read values from `Track::volume` / `Sequence::masterVolume` (`valueAt(playhead)` for a curve).
  - One undo step per commit: coalesce a drag into one commit on release (or commit live and merge, as the timeline does
    for trims).
- **Pan.** `setPan(track, pan)`: `track.setPan {track, pan}`, or `edit::setParameter(... ParamRef::of(ParamId::Pan) ...)`.
  Read `Track::pan`.
- **Mute / solo.** Unchanged (`track.setState {muted, solo, enabled}`); the player and export apply Premiere's rules above.
- **Meters.** Track and master peak meters stay `TimelineAudioPlayer::trackMeter(i)` / `masterMeter()` (post fader). The
  live loudness meter is `TimelineAudioPlayer::loudness()` (`momentaryLufs`, `shortTermLufs`, `integratedLufs`,
  `truePeakDbtp`, ...), polled at the bridge's ~30 Hz with `resetLoudness()` for the reset button. Compare with
  `targetLufs` / `targetTruePeak`, and call `setLoudnessEnabled(metersActive)` to save the worker's work when hidden.
- **Ghosts for Auto-Duck.** `audio.autoDuck` with `dryRun: true` gives `data.clips[].keyframes` (timeline ticks + dB) and
  `data.regions[].ticks`.
- `faderAvailable` can become true, `faderNote` empty.

### Enhance Speech and stem separation (A.4)

Premiere's Enhance Speech and Resolve's Voice Isolation: **Enhance Speech** gives dialogue a studio sound (noise, hum and room
removed) with DeepFilterNet3. **Separate Stems** splits music or a mixed clip into vocals, drums, bass and other with HTDemucs.
Both run on this computer, offline (never real time), out of process in `wabcut-embed`. RNNoise stays the real-time Reduce
Noise effect.

**Models.** Both are on the licence allow-list and downloaded on first use. ModelStore (`ModelKind::Audio`) checks every byte
against the pinned SHA-256 and shows the licence card first. Neither is bundled or downloaded in tests.

| Model | Source (pinned) | Licence | Bytes | SHA-256 |
|---|---|---|---:|---|
| DeepFilterNet3 (`deepfilternet3`) | [`models/DeepFilterNet3_onnx.tar.gz`](https://github.com/Rikorose/DeepFilterNet/tree/84d57ec2c08fe08e68a13fb32a58cd7092060a0f/models) at commit `84d57ec` (2023-05-23, "Update final DeepFilterNet3 model") | Code and weights MIT OR Apache-2.0 (the repository's LICENSE covers `models/`); used under MIT | 7983136 | `c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616` |
| HTDemucs (`htdemucs`) | [`StemSplitio/htdemucs-onnx`](https://huggingface.co/StemSplitio/htdemucs-onnx/tree/d54ed9eb60e258ea82131c6ee14578628816456a) `htdemucs_fp16weights.onnx` | MIT: Demucs (Meta; the repository releases code and weights under MIT) and the ONNX export ([StemSplit/demucs-onnx](https://github.com/StemSplit/demucs-onnx), MIT) | 165612636 | `d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a` |

- **The DeepFilterNet3 archive.** It is extracted with the system's tar (`%SystemRoot%\System32\tar.exe`, bsdtar). Each graph is
  then checked by size and SHA-256 before it is installed in `<models>/deepfilternet3-84d57ec/`:

  | File | Bytes | SHA-256 |
  |---|---:|---|
  | `enc.onnx` | 1954042 | `7c5399d3da8a50ebef1c1a0ae421b33376aa5e45d0e92df16da7e83c9c131916` |
  | `erb_dec.onnx` | 3292397 | `ab669a1d10afe20911728b33053a452071042317a90581092b325da7b2f9d895` |
  | `df_dec.onnx` | 3340803 | `23114ce3b0f6464b763ee62f7bb8aab6b2a129a21eabd5bcfe59413db05f278a` |
  | `config.ini` | 2067 | `415eb925d44990d938fb739f514aa3662c1ec0ea836cff044fa1291b82cb4290` |

- **The Demucs licence.** One third-party re-upload labels the htdemucs weights CC-BY-NC. The official repository states nothing
  but MIT, so this is recorded but does not change the allow-list decision. A model for dialogue / music / effects separation
  (cinematic demixing) was looked for; none with permissive code and weights and an ONNX export was found, so it is not offered.

**DSP (`wabcut/ai/audio/DeepFilter.h`).** The networks run in the helper; the signal processing around them is ported from libDF
(`lib.rs` / `tract.rs` at the model's commit):

- the STFT: 960-point Vorbis window, 480 hop, libDF's normalisation;
- the 32-band ERB features and the 96-bin complex features, with libDF's exponential normalisation states;
- the ERB gains;
- the deep filter: order 5, the network's frame t applied to spectrum t - 2;
- the overlap-add synthesis.

It is offline: whole blocks go to the causal networks at once (the same computation as libDF's frame-by-frame tract run), and the
delay is compensated, so output sample n is input sample n. Both stages always run, as deep-filter's default thresholds make
them, with no post-filter.

Checked against the reference `deep-filter.exe` (v0.5.6, `-D`, run locally, not shipped) on a 48 kHz noisy-speech fixture:
- the outputs differ by 72.6 dB below the signal (about the reference's 16-bit output precision);
- lag 0.

**Protocol.** `EmbedProtocol.h` gains these messages:
- `load {speech, stems}`: the speech model's directory, the stem model's file;
- `enhance_audio {audio, output}` and `separate_audio {audio, output}`: raw float32 blocks in files;
- the `audio` answer.

The helper reads every channel on its own and writes the result next to the block. `AudioHelper` (`wabcut/ai/audio/AudioHelper.h`)
has EmbedHelper's crash, timeout and cancel handling:
- a crash while a block is processed is retried once (on a new helper, which loads the model again);
- a timeout or a cancel is not retried;
- the helper runs below normal priority.

**AI helpers and the CPU (0.2).** Every AI helper process - wabcut-infer (transcription), wabcut-embed (analysis, search,
segmentation, detection, audio models) and the local model server - starts as an AI helper (`ChildProcessOptions::aiHelper`):
below normal priority and, on Windows, a hard CPU rate cap on its job object (`JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP`), so
background analysis leaves playback its share of the processors. Preferences > AI > Models > "AI helpers' CPU": No limit,
75 % (the default), 50 % or 25 % of the machine (`ai/helperCpuLimit`, `setHelperCpuPolicy`); it applies to helpers started
afterwards. POSIX: nice +10 only (no cap yet). The export CPU limit (Export queue > Limit CPU use) is separate: it caps the
encoder's threads. Test: `tests/ai/test_process.cpp` "AI helpers run below normal priority under the helper policy's CPU cap".

**Jobs (`wabcut/ai/audio/AudioJobs.h`, `Plans.h`).**
- **Enhance Speech.**
  - Decodes the stream with `media::AudioDecoder` at 48 kHz from source time 0, the decoder playback reads source and take with.
  - Sends 30 s chunks with a 4 s pre-roll (the recurrent state and the normalisations settle) and a 100 ms post-roll; consecutive
    chunks crossfade over 50 ms, so seams do not click.
  - Every request has the same length (the last one is padded), so DirectML compiles its graphs once.
  - Writes a float WAV take, `<size>-<sample hash>-a<stream>-<model>.wav`. The cache key is the source file plus the model and
    processing version (`deepfilternet3@c94d91f70911/dsp1`).
  - Strength is not part of the key: libDF's attenuation limit is exactly a linear blend of the enhanced and noisy spectra, so of
    the two signals, and the effect does that blend in real time.
  - A second request for the same file and model finds the take and renders nothing.
- **Stem separation.**
  - Decodes at 44.1 kHz stereo and sends 7.8 s segments with Demucs's quarter overlap (linear crossfades that sum to one).
  - Writes `drums/bass/other/vocals.wav`.
  - Refuses with Unsupported when the computer has less than 2.5 GiB of free RAM (`stemDevice`; one segment needs about 1.1 GB on
    ONNX Runtime's CPU path), before anything is downloaded or started.
- **Where they go.** Takes go to `<user cache>/enhanced` and stems to `<user cache>/stems` (`LocalAudioOptions` takes other
  directories).
- **The resource governor.** A host hook, `waitIdle`, holds a job between requests while playback or an export runs.

**The take in the project.** `MediaItem::audioTakes` records `AudioTake {stream, kind "enhanceSpeech", path, model, source
fingerprint, generation}`. It is set by `edit::setAudioTake`, saved only when present (old projects stay byte-identical), and
validated.

**The effect.** `wabcut.audio.enhanceSpeech` has one parameter, `strength` (a Fraction: 0..1, shown as 0..100 %). The take is
resolved per snapshot by `playback::AudioTakes`: the file must exist and its source fingerprint must match the item's file.
`TrackRenderer` then does, per clip:
1. Reads the take at the clip's source time (`AudioFeed::readTake`; `MediaAudioFeed` opens it with AudioDecoder) and blends it into
   the source by Strength on the 32-frame parameter grid.
2. Applies the clip's Volume and Panner.
3. Applies the redactions: a mute or a bleep replaces the enhanced sound, never the other way round.
4. Runs the clip's effects, then the envelope.

The effect therefore acts on the source wherever it sits in the stack (`audio.enhanceSpeech` puts it first).

- **At full strength** the output is the take, sample for sample.
- **Bypass.** With the effect off, a missing file or a stale take, the clip plays its source. The `takeErrors` counter counts takes
  the feed could not read.
- **Export == preview.** OfflineMixdown (export, `export/src/Mixdown.cpp`) and loudness analysis resolve the same table; the
  loudness cache key includes the takes of the media in scope.
- **Refill.** Recording or re-rendering a take refills only the tracks that play that media (`AudioTakes::changedMedia`), within
  the lead window.

**Commands.**
- **`audio.enhanceSpeech {clips, strength 0..100 (default 100), render (default true)}`** (EditProject since A.8: it records
  the takes on the media items; one undo step).
  - Renders the missing or stale takes of the clips' media streams, each once, and records them.
  - Puts the effect first in each clip's stack, or updates it.
  - Video clips stand for their linked audio.
  - `render: false` only applies the effect (the "bypassed until rendered" state).
  - `data.takes[]` reports rendered / reused, the speed and the device; `data.clips[].take` is `ready`, `missing` or `stale`.
- **`audio.separateStems {clip | media [+ stream], model "htdemucs"}`** (EditProject, one undo step).
  - Adds the four stems as media items named `<source> (<part>)` in a bin `<source> stems` next to the source.
  - Each has `MediaItem::derived = {source, "separateStems", model, part, createdAt}`, saved and validated.
- **Hosts.** The app's assistant (`AgentController::services()`) and wabcut-cli (`cmd` / MCP) provide the jobs.
  - In the app, first-use downloads go through the AI network stack (offline mode refuses them).
  - The CLI downloads nothing: a headless request cannot show the licence card.
  - Both are MCP edit tools (`audio_enhanceSpeech`, `audio_separateStems`: served with `--allow-edit`, not on a read-only
    server) and no consent is needed (local). They change media items, so the editing assistant does not get them
    (A.8 audit): Essential Sound's Enhance Speech applies the effect, and the clips play the original until
    `audio.enhanceSpeech` renders the take.
  - The assistant can run `audio.enhanceSpeech` by default ("clean up the voice on all interview clips"). `audio.separateStems`
    needs EditProject (Settings > AI); the additive-command guard is about added sequences and was left as it is.

**Measured (RTX 3060 12 GB, Core i5-12600K, ONNX Runtime 1.24.4).**
- **Enhance Speech quality.** On 5 dB-SNR speech (pink-ish noise + 120 Hz hum):
  - SNR 5.0 → 14.7 dB and SI-SDR 5.0 → 15.7 dB over 60 s;
  - SNR 15.0 dB and SI-SDR 15.8 dB over 300 s.
- **Enhance Speech speed** (the whole job: decode, chunks, IPC, WAV):

  | Programme | DirectML | CPU |
  |---|---:|---:|
  | 300 s | 11.4× real time | 13.8× real time |
  | 60 s | 3.7× real time (DirectML's one-time graph compile dominates) | 8.8× real time |

  A warm 11 s block takes the helper 0.27–0.34 s on DirectML (32–41×).
- **Stem separation.** CPU 2.0–2.6× real time per 7.8 s segment in the helper, 0.55× real time for a 20 s job including model load.
  DirectML runs out of memory on the export's inverse STFT, a transposed convolution with a 4096-sample kernel; that happens even
  with 10 GB free, and with graph fusion off or basic optimisation. Stem separation therefore runs on the CPU unless `device` is
  `gpu`.

**Tests.**
- **ai suite, `[audio]`.** The DSP:
  - perfect reconstruction with no delay;
  - an identity network gives back its input;
  - gains and the deep filter's frames;
  - the normalisation states.

  The plans:
  - chunk tiling;
  - crossfades summing to one;
  - a model with memory joins without a click with pre-roll, and clicks without.

  The protocol and AudioHelper against the scripted helper:
  - answers;
  - a crash retried once, a repeated crash, a timeout, a cancel, garbage, a wrong answer;
  - no leftover files.

  The jobs on decoded WAVs:
  - the take equals the model over the whole stream;
  - 44.1 kHz input;
  - stems sum to the mix;
  - reuse;
  - the memory refusal.

  The real models are tested only with `WABCUT_TEST_AUDIO_MODELS` and `WABCUT_TEST_SPEECH_WAV`.
- **playback.**
  - The take is sample-exact at full strength, and preview == export.
  - The blend is exact, also keyframed.
  - Bypass when the effect is off, the take is missing or stale, or the take is unreadable.
  - The bleep after enhancement.
  - A live take record is heard in the lead window.
  - Reversed clips.
  - With the real decoder (playback_media), the float-WAV take plays sample-exact.
- **export.** The WAV export plays the take as preview does. Both export tests fail with the Mixdown.cpp wiring removed.
- **audio_edit.** The commands with fake models.
- **model.** Takes and derived provenance: ops, validation, IO.

**Note for the UI engineer (context menu, badge, downloads).**
- **Context menu.** In `ClipContextMenu.qml`, add "Enhance Speech" and "Separate Stems…" (audio clips, or video clips with linked
  audio). Run `audio.enhanceSpeech {clips: [the selection's audio clips]}` and `audio.separateStems {clip}` on a worker (the
  `BridgeJob` / `JobControl` pattern), with progress from the "(NN%)" checkpoint messages.
- **Before the first run.** If `ai::audio::speechModelInstalled(store)` is false (or `!store.isInstalled(stemModel())`), show the
  model's `licenseCard` and download it with `installSpeechModel` / `store.ensure` over `HttpDownloadTransport(m_ai->transport())`
  (offline mode refuses). A command whose model is not there fails with NotFound.
- **Badge.** On a clip whose stack has `wabcut.audio.enhanceSpeech`, show the take's state. `audio.enhanceSpeech` returns it per
  clip (`data.clips[].take`). Without running anything, `playback::AudioTakes::of(state)->find(media, stream, "enhanceSpeech")` is
  null when the take is missing or stale. Missing or stale gets the badge and a "Render enhancement" action, which runs
  `audio.enhanceSpeech {clips}` again; the effect stays, and the take is rendered and recorded.
- **Strength.** Strength is the effect's `strength` parameter, 0..1, shown ×100 like Reduce Noise's Amount. Toggling the effect is
  the A/B. Add the id to `AudioMixerBridge`'s `kAudioEffects` / `shortEffectName` ("Enhance") / `effectSummary` lists.
- **Takes directory.** Set `LocalAudioOptions::takesDirectory` to the project's media cache (`<project>.wabcut-media/enhanced`) so
  takes travel with the project. The CacheInspector's cache folders should list `enhanced` and `stems`, never deleting a file a
  project uses (the `AudioTake` paths, the stems' locators).
- **Governor.** Wire `LocalAudioOptions::waitIdle` to `ResourceGovernor::waitIdle`.

**Limits.**
- The take is per media stream; a clip's source range is not rendered on its own, so the first render of a long file takes the
  whole file.
- Multichannel files are enhanced channel by channel (independent masks); dual-mono interviews come out best.
- At a 44.1 kHz device rate both the source and the 48 kHz take are resampled by AudioDecoder (sub-sample alignment, not
  bit-exact; 48 kHz is exact).
- Linux and macOS are untested (the archive step uses `/usr/bin/tar` there).

### Audio sync (A.5)

Recordings of one event, lined up by their audio:
- dual-system sound: a camera's scratch audio and a separate recorder (Premiere's **Synchronize** by audio, Resolve's Auto Sync
  Audio);
- several cameras: multicam's audio sync method.

The algorithm is `wabcut/audio/edit/AudioSync.h`. It is pure C++ on an abstract reader, so it is tested in the core preset.

**How it works.** Coarse to fine:
1. **The onset envelope.** Each recording is read once as mono at 8 kHz and reduced to a 1 kHz onset envelope. Per millisecond,
   the band energy above 150 Hz in dB, minus the mean of the previous 30 ms, kept where positive. Gain, hum and a different
   microphone change little of it.
2. **The coarse match.** GCC-PHAT of each envelope against the reference's, over every lag that leaves `minOverlap` (5 s) in
   common. The edges are cosine-tapered so recordings' starts do not match each other.
   - The match's peak-to-sidelobe ratio (PSR) gives its confidence: `clamp((PSR - 8) / 12, 0, 1)`.
   - In the tests, unrelated audio stays below the threshold, and the fine windows of real matches reach 35–700 σ.
   - Too little overlap, silence or no correlation fails with a reason. A failed recording is reported, never placed at a guess.
3. **The fine match.** Five 4 s windows across the overlap are read at 48 kHz, high-passed and tapered. GCC-PHAT over 100 Hz to
   8 kHz runs within ±0.5 s of the coarse lag, with parabolic peak interpolation.
   - The windows are measured from the middle outwards, each predicted by the line through the ones before it, so drift is
     followed.
   - A peak on the search's edge, a window below 6 σ (silence there) and a window more than 1 ms off the line are left out.
   - The line gives the offset at the overlap's middle (and at its start) and the clock drift in ppm.
   - The drift is called significant from 2 ppm when it is three times its own error. The speed that cancels it,
     `1 + ppm / 1e6`, is proposed, never applied: a retimed audio clip would be silent today.
4. **Confidence.** The coarse confidence times the share of fine windows that agree.

**Chaining.** A recording that does not overlap the reference is synced through another synced one (its offsets add up).

**Threads.** Envelopes and coarse matches run in parallel, up to 4 threads; the fine pass runs one pair at a time.

**Cancel and progress.** Cancel works through the stop token. Progress: envelopes 0–70 %, coarse 70–85 %, fine to 100 %.

**Media (`makeMediaAudioSync`, in `wabcut_audio_edit_media`).**
- Files are decoded with AudioDecoder as mono at the pass's rate (any source rate; the decoder resamples).
- **Split recordings.** An angle may be several files, a camera that split its recording. They play back to back in recording
  order: the files' modification time, then their names (`keepOrder` keeps the listed order). Each file's start on the
  reference's clock is reported.
- **Caching.** Results are cached in the AnalysisCache (`audio.sync`, `sync1`), keyed by every file's fingerprint, stream and
  duration, the reference and the options.
- **Wiring.** `makeMediaAudioServices` provides it, so the app's assistant and wabcut-cli have it.

**Command: `audio.syncClips {clips | media, reference?, apply?, link?, minConfidence?, minOverlap?}`** (EditSequence).
- **What it returns.** Every recording's offset (ticks and seconds), confidence, overlap and drift (with the speed proposal), or
  why it did not sync.
- **The reference.** The one named, else the first clip with a picture.
- **Split recordings.** `media` entries may be arrays: a split recording.
- **`apply`, with clips: Premiere's Synchronize.**
  - The other clips move to `start_ref - in_ref + in_clip + offset`. Linked partners move with them, and the reference's own
    link group stays.
  - If something would start before 0, everything moves later together.
  - With `link` (default on), the synced clips without a picture (the external audio) join the reference's link group with its
    picture and scratch audio.
  - Retimed or reversed clips are reported and not moved.
  - One undo step; dryRun previews it.

**Multicam (C.8).** `multicam.create {"sync": "audio"}` runs this analysis.
- `angleOffsets(service, state, angles)` gives where each angle's audio stream 0 starts relative to angles[0]'s. It fails with
  the angle's name and reason.
- `MulticamAudioSync` (in `AudioSync.h`) is C.8's `AudioSyncAnalyzer` on it.
- **Wiring.** The app's assistant services and wabcut-cli bundle it as `command::MulticamServices`, next to the audio services
  and on the same analysis.
- **Cancel and progress.** The seam's `offsets(state, angles, AnalysisControl{stop, progress})` overload lets
  `multicam.create` report the analysis through its checkpoints ("syncing the angles by audio (40%)"). A stop there cancels
  the command, and nothing is created.

**Measured (Core i5-12600K, other builds running).**
- **Four 60-minute angles synced in 7.1 s** (target < 30 s), from AAC 128 kb/s `.m4a` files cut from one synthetic programme:
  - one angle is 44.1 kHz with a 6 kHz low-pass;
  - the others are high-passed at 120 or 200 Hz;
  - gains 0.6–1.5 and their own white noise.

  Offset errors: 0, -2 and 0 samples at 48 kHz. The -2 is the 44.1 kHz low-passed angle: the filter really delays the sound by
  about a sample. Confidence 1.0, drift 0.00 ppm.
- **The algorithm alone**, on four synthetic hour-long angles generated on the fly: 13.6–16.7 s, which includes computing the
  synthetic sound itself (the file benchmark, decoding included, is faster).
- **A 40 s camera** (48 kHz stereo, room echoes, noise) **and a 44.1 kHz mono recorder**: 0.4 s, error 0 samples.
- **Synthetic pairs.** Within 1 sample through noise, a room (echoes at 23–97 ms), gains from 0.2 to 3 and partial overlap
  (10 s of 40).
- **Drift.** A recorder 40 ppm fast measures 40 ± 2 ppm over 5 minutes, with the speed proposal 1.00004.

**Tests.**
- **audio_edit `[sync]`:**
  - accuracy cases;
  - partial overlap; too little overlap, unrelated audio and silence fail;
  - drift;
  - chaining, reference choice, cancel;
  - the command: offsets, dry run, Synchronize with link, the recorder as reference, failures, undo, `angleOffsets`.
- **`[sync][media]`:** real WAV files through AudioDecoder (48 kHz and 44.1 kHz), the split camera in recording order, the cache,
  cancel.
- **Benchmarks.** `WABCUT_TEST_SYNC_BENCH_DIR` with `angle1..4.m4a` and `offsets.txt`. The hidden test `[.sync-bench-source]`
  writes the source, and `ffmpeg` cuts the angles: start, gain, filters, noise, rate.

### Nested-sequence and multicam audio (A.5 part 3)

A clip of a sequence (C.8: `Clip::sequence`, `docs/internal/c8-nested-multicam-handoff.md`) plays that sequence's sound.
- **Where.** TrackRenderer, so the player's feeder, OfflineMixdown (export, loudness, speech) and `wabcut-cli render` all play
  it, sample for sample alike.
- **How.** The renderer reads the clip's source from a nested mix (`playback/src/audio/NestedMix`) instead of the AudioFeed. Then
  come the clip's own Volume and Panner, its effects, and its envelope (cut fades, transitions), as for media.

**A nested clip: Premiere's default nest, one stereo clip.**
1. **The tracks.** The tracks the resolver reads there play (`nestedTracksRead`): output on, not muted, only the soloed ones
   while any is soloed.
2. **Each track.** Each renders through its own TrackRenderer: clips, clip and track effects, transitions, cut fades, the track
   fader and pan. Nested clips inside recurse, to `kMaxNestingDepth`.
3. **The sum.** The tracks are summed.
4. **The nested master stage.** The nested sequence's master effects run (with their latency read ahead, like a track stack),
   then its master fader.
   - **Decision: the nested master bus applies.** Premiere puts a nested sequence's Mix track (its effects and fader) into
     the nest, and the C.8 handoff agrees. A limiter on a nested scene's master therefore limits the scene wherever it is
     nested.
   - **No soft clip.** The player's soft-clip guard is the device output's alone, so it does not run on a nested master.
5. **The format.** The nested mix renders at the parent's rate and channel count; the nested sequence's own `audioRate` is not
   used.

**A multicam clip** sums its active angles' audio tracks (`Clip::angles`), each with its effects and fader. It reads them
whatever their mute, solo and output flags.
- **No master stage.**
- **The sync.** The angles' sync is where their clips lie in the source (`MulticamAngle::offset`). A clip therefore plays every
  angle at the same moment.
- **Angle switches.** Where a multicam clip is cut to other angles, the two angles crossfade instead of the 2 ms cut fades: a
  `multicam.switchAngle` cut (same source, contiguous at 100 %, no transition there).
  - The crossfade is `TrackRendererOptions::angleCrossfade`, 10 ms centred on the cut.
  - Both angles play through it. The ramps add up to exactly 1, so a switch never clicks or dips.
  - Switching to angles that share a track keeps that track's sound unchanged.

**Time.**
- **Speed.** Nested time follows `Clip::sourceTimeAt` exactly, in integer ticks. Nested audio plays at 100 % only (media clips
  play at any speed since A.10): a remapped nested clip is heard in its `unitySpeedRanges`, with the P2-21 fades at their
  edges. Other speeds are silent.
- **Reverse.** A reversed nested clip plays its nested mix backwards. The mix is rendered forwards in 1 s pieces of nested time
  (aligned to the clip), each from a restart. Its samples therefore do not depend on how the reads are cut.
- **The ends.** Before the nested sequence's start and past its `duration`, the clip is silent. Disabled clips are silent, and
  so are clips of an unknown source kind.
- **State.** A nested mix carries its effects on while it is read continuously. Any other read restarts it: the inner
  renderers reset and pre-roll, and a master stack with effects pre-rolls `maxPreroll`. The samples depend only on where
  playback started, so preview and export agree from the same start.

**Redactions.** Source-anchored redactions apply to the media inside by media time, wherever it is nested. A redaction that
excepts the leaf clip or any clip leading to it (`AudioRedactions::apply`'s path) does not apply: excepting a nested clip
excepts its content, as in `resolveLeaves`.

**Off the audio thread.** The nested mixes run inside the renderers on the feeder, export or analysis thread. The callback
still only reads the track rings.
- **The player.** It hands each snapshot to its renderers (`TrackRenderer::setProject`). A sequence a track nests
  (`sequenceContains`) can change or go away. The track then refills from the edit cutover, so a nested edit is heard within
  one lead window.
- **Redactions and takes.** Their refills look through nested clips (`mediaHeardOn`).
- **The analysis cache.** Loudness cache keys hash a nested clip's mix: the tracks it reads, the master stage, the length.

**Tests.**
- **playback `[nested]`, `[multicam]`:**
  - levels, track rules and the nested master;
  - silence past the nested end;
  - nested in nested, with the resolver;
  - a remapped and a reversed nested clip;
  - multicam offsets and the switch crossfade (and that a hard switch clicks);
  - redactions inside nested media, with resolveLeaves and the player;
  - preview == export bit for bit, from the start and from a seek;
  - a nested edit heard during playback.
- **The RT audit** (`test_rt_audit.cpp`). Nested and multicam playback through a nested edit, an angle switch, a seek and a
  loop: no allocation and no lock in the callback.
- **audio_edit:**
  - nested loudness and its cache key;
  - `multicam.create` by audio through `MulticamAudioSync`, and cancelled at a checkpoint;
  - **`wabcut-cli` end to end:** two WAVs of one event are synced by audio into a multicam source, then cut in, switched,
    nested and rendered in sync to the sample.

### Audio plugins (A.6)

CLAP and VST3 audio effects run in **wabcut-plughost**, one helper process per plugin binary. A crash disables that plugin's
instances with a badge (they are bypassed, latency kept), never the editor. The full design is in [plugins.md](plugins.md).

**What the assistant and scripts see:**
- **`plugins.list`** (Read): the plugins by effect id (`plugin.vst3.<class id>`, `plugin.clap.<plugin id>`), with their
  parameters (`p<id>` keys, ranges, automatable), the blocklist and the folders.
- **`plugins.scan`** (MediaAnalysis): scans the plugin folders, each file in its own helper (a crash or a hang goes on the
  blocklist). The assistant never gets it: it runs plugin code.
- **`clip.addEffect` / `audio.addEffect`**, with `"plugin"`: a plugin effect on clips, a track or the master. Its parameters
  are keyframed with the usual keyframe commands.

**In the engine:**
- **Fixed blocks.** Plugins get fixed 256-frame blocks aligned to the chain's stream, so export == preview for deterministic
  plugins.
- **Delay compensation.** The plugin's latency, plus one block of pipeline (4096 frames on the master), is compensated to the
  sample.
- **Deadlines.** On the audio callback the master never waits: a late block is silence and is counted.
- **Parameters** go as sample-accurate events on the parameter grid.
- **State** is kept with the effect as base64 parts (at most 45 KB).
- **Missing plugins.** A plugin missing on another computer is kept and bypassed with a "missing plugin" badge.

**Measured.** 10 instances cost 4–6 µs per instance per 256-frame block. Eight free VST3 effects installed on the
development computer scan and process (plugins.md).

### Essential Sound (A.7)

Premiere's Essential Sound panel as a command, `audio.essentialSound` (`src/audio/edit` `EssentialSound.h`). Nothing is a
black box: a preset builds an ordinary effect stack that the user can edit, keyframe, disable or delete.

**The clip's type.**
- **The record.** A clip's type lives in the first effect of its stack, `wabcut.audio.essentialSound`. It does nothing to the
  sound; its data holds `type` (dialogue, music, sfx, ambience), `preset` and the `options` applied as JSON (for the panel).
  It is a registry row, so no model field was added, and it saves, undoes and copies like any effect.
- **Its effects.** Each effect the preset built follows the record and is marked with its panel section (data
  `essentialSound`: `loudness`, `repair.rumble`, `repair.hum`, `repair.noise`, `repair.enhance`, `repair.gate`,
  `repair.reverb`, `repair.deess`, `clarity.band`, `clarity.eq`, `clarity.dynamics`, `creative.reverb`, `width`,
  `clarity.limiter`).
- **Applying again** replaces the marked effects and keeps the clip's other effects after them. `"type": "none"` removes
  Essential Sound.
- **Clips or whole tracks.** `"clips"` names clips (video clips stand for their linked audio); `"tracks"` (A.10) takes
  every clip on those audio tracks, so the mic track of a recording is one request. Both may be given; each clip counts once.
- **Track roles are reused.** Without `"type"`, a clip keeps its recorded type, else takes its track's role (effects → sfx).
  Applying a type gives a track without a role the type's role (`setTrackRole`, default true), so `audio.autoDuck` and the
  next request find it.

**Presets** (`options` change any setting):

| Type | Presets |
|---|---|
| Dialogue (-23 LUFS) | `default` (loudness), `streamerVoice` (-16 LUFS, A.10: see "Streamer voice and Game under voice" below), `cleanUpNoisy`, `podcastVoice`, `phone`, `radio`, `reduceRoomEcho`, `removeHum`, `smallRoom`, `largeRoom`, `outside` |
| Music (-25 LUFS) | `default`, `background` (ducked under dialogue), `wide` |
| SFX (-25 LUFS) | `default`, `background` (-32) |
| Ambience (-30 LUFS) | `default`, `underDialogue` (ducked), `wide` |

**The settings and what they build**, in stack order:
- **Loudness** (Premiere's Auto-Match): a Gain first, then the clip on its own (with its new stack) is measured on the real
  render path and the Gain moved until within 0.5 LU of the target. `audio.normalize` finds the same Gain, after the record.
  Since A.10 each step after the first divides the error by the slope the last step measured: above a compressor's
  threshold a dB of Gain moves the loudness by only 1/ratio dB, and plain steps would creep up on the target.
- **Repair** (dialogue):
  - `rumble`: a 24 dB/oct high-pass, 80 Hz typically;
  - `hum`: DeHum at 50 or 60 Hz and the next 3 harmonics;
  - `noise`: Reduce Noise (RNNoise), or `enhance`: Enhance Speech (DeepFilterNet3, a rendered take, as `audio.enhanceSpeech`);
  - `gate` (A.10): Noise Gate, an expander with a high ratio and a 1 ms opening, right after the noise reduction (the floor it
    closes on is lower there): threshold 12 - 8 x gate dB under the voice's loudness, ratio 2 + 8 x gate, range 12 + 28 x
    gate dB, release 200 - 80 x gate ms;
  - `reverb`: an expander that pulls room tails and noise between words down;
  - `deess`: the de-esser.
- **Clarity** (dialogue): an EQ preset (`podcastVoice`, `phone`, `radio`, `streamerVoice`: a band filter, then a 3-band EQ
  scaled by `eqAmount`), then Dynamics (a compressor; with `makeup` (A.10) it gives back what it takes from a voice at the
  target), and with `limiter` (A.10) a true-peak limiter at `limiterCeiling` (default -1 dBTP) as the last effect of the stack.
- **Creative**: a reverb preset (`smallRoom`, `largeRoom`, `hall`, `plate`, `outside`), its mix by `amount`.
- **Levels follow the target** (A.10). The gate, Reduce Reverb, the de-esser and the compressor come after the loudness Gain,
  so they hear the clip at about its target. Their thresholds are set for dialogue at -23 LUFS and move with `targetLufs` (a
  voice at -16 LUFS gets them 7 dB higher). Without loudness the clip's level is unknown and they stay as set for -23. Every
  preset at -23 LUFS builds the stack it built in A.7.
- **Music and ambience**: `ducking` (volume keyframes under the dialogue's speech, `audio.autoDuck`'s envelope; the dialogue is
  dialogue-role tracks and tracks with dialogue clips), `width` (Stereo Width), and for music `duration` (Music Remix, below:
  the pieces share the stack).
- **SFX**: `pan`, the clip's own panner.

**The new effects** (registry rows plus `wabcut/audio/dsp` `Repair.h` and `Space.h`; own implementations, RT-safe, preview ==
export sample for sample):

| Effect | What it is |
|---|---|
| `wabcut.audio.filter` | Butterworth low cut / high cut, 12–48 dB/oct; off at 20 Hz / 20 kHz |
| `wabcut.audio.dehum` | depth-blended notches (exactly -N dB, q 12) at the mains frequency and its harmonics |
| `wabcut.audio.deess` | split-band de-esser: the band above a frequency goes down by 3/4 of its level over the threshold |
| `wabcut.audio.expander` | downward expander (ratio, range, attack, release) |
| `wabcut.audio.reverb` | 8-line feedback delay network (Jot): predelay, 4 all-pass diffusers, Householder feedback, absorption filters for RT60 and damping |
| `wabcut.audio.width` | mid/side width 0..2 with a mono bass; the mono fold-down never changes |

**Measured** (`wabcut_audio_tests "[essential]"`, and `"Essential Sound DSP numbers"` for the report):
- **DeHum at 30 dB:** -30.0 to -30.1 dB at each of the 4 harmonics, at 50 and at 60 Hz; a 1 kHz programme moves 0.002–0.004 dB.
- **De-esser** (threshold -30 dB, 10 dB maximum) on synthetic sibilance: the band goes down by 6.5 dB; the voice (220 Hz) by 0.01 dB.
  Between the bursts the output is the input, bit for bit.
- **Reverb:** the measured RT60 is 0.615 / 1.512 / 3.009 s against 0.6 / 1.5 / 3.0 s asked. The left/right tail correlation is
  0.001–0.005. Damping shortens the highs; width 0 gives a mono tail. The output is the same whatever the block size.
- **Width:** L + R of the output equals L + R of the input (error < 1e-6) at every width.
- **Filter:** -3.01 dB at the cut and -24.1 dB an octave below at 24 dB/oct.
- **Cost** per second of stereo 48 kHz audio: filter 1.4 ms, DeHum 0.9 ms, de-esser 1.8 ms, expander 0.6 ms, reverb 2.0 ms,
  width 0.3 ms (each under 0.2 % of real time).
- **The audio callback** with all of them on clip, track and master stacks, through live edits, a new reverb, a seek and a loop:
  no allocation and no lock (`[playback][rt][essential]`).

### Streamer voice and Game under voice (A.10)

Two one-click presets for gaming recordings (OBS, ShadowPlay, Game Bar), where the microphone and the game arrive on their
own tracks ("Mic", "Game", "Discord", "Music"). Both are ordinary commands: one undo step each, `dryRun` previews them, and
what they build stays editable.

**Streamer voice** is the dialogue preset `streamerVoice` of `audio.essentialSound`:
`{"tracks": ["A1"], "type": "dialogue", "preset": "streamerVoice"}` applies it to every clip of the mic track. Its stack, in
order (each effect marked with its section):

| Section | Effect | Setting |
|---|---|---|
| `loudness` | Gain | matched to -16 LUFS |
| `repair.rumble` | Filter | low cut 80 Hz, 24 dB/oct |
| `repair.noise` | Reduce Noise (RNNoise) | amount 0.8 |
| `repair.gate` | Expander as a gate (gate 0.5) | threshold -24 dBFS (8 dB under the voice's loudness), ratio 6, range 26 dB, attack 1 ms, release 160 ms |
| `repair.deess` | DeEsser (0.45) | threshold -20.9 dB, at most 9.4 dB, from 5.5 kHz |
| `clarity.band` | Filter | low cut 90 Hz, 24 dB/oct |
| `clarity.eq` | 3-band EQ `streamerVoice` | low shelf -2.5 dB under 300 Hz (mud), +3.5 dB at 3.2 kHz (presence, Q 1), high shelf +2 dB from 10 kHz (air) |
| `clarity.dynamics` | Compressor (0.6, `makeup`) | threshold -16.6 dB, ratio 4.2, attack 5 ms, release 120 ms, makeup 7.3 dB |
| `clarity.limiter` | Limiter | ceiling -1 dBTP, release 50 ms, lookahead 5 ms |

- **Why -16 LUFS.** The upload is normalised to YouTube's -14 LUFS integrated (`audio.normalize` `youtube`). The voice is the
  loudest part of the mix; at -16 LUFS on its own it leaves about 2 LU for the game under and between the sentences, so the
  final normalisation moves the mix by a dB or two, not the 9 dB a voice at Auto-Match's -23 would need.
- **Why these thresholds.** Everything after the Gain hears the voice at about -16 LUFS: the thresholds are the dialogue
  formulas moved up 7 dB ("Levels follow the target" above). The gate closes about 17 dB under the voice's peaks, where OBS's
  noise gate closes for a typical mic, and well over a fan's floor. The de-esser comes before the EQ's presence lift.
- **Why makeup.** Without it the loudness Gain has to push the voice far into the compressor to reach the target (each dB of
  Gain adds only 1/4.2 dB there), and the noise floor the gate hears goes up with it. With it the Gain stays near the
  difference between the clip and the target.
- **Enhance Speech** (DeepFilterNet3) is the heavier denoise: `"options": {"repair": {"enhance": true, "noise": 0}}` uses it
  instead of RNNoise. It plays a take that `audio.enhanceSpeech` renders with the model (until then the clip plays the
  original, with a warning), so the preset keeps RNNoise and works at once, offline, on any computer.
- **Editable afterwards** like every Essential Sound preset. The effects are ordinary; `options` (`repair.gate`,
  `clarity.limiter`, `clarity.limiterCeiling`, `clarity.makeup`, ...) rebuild only the marked effects, and the clip's own
  effects stay after them.

**Game under voice** is `audio.autoDuck` with `"preset": "gameUnderVoice"`:
- **The tracks** (`chooseGameVoiceTracks`, `src/audio/edit` `GameAudio.h`). The voice is the tracks named "Mic", "Microphone",
  "Voice" or "Commentary", else the dialogue-role tracks. The game is the tracks named "Game", "Game Audio", "Desktop Audio"
  or "Gameplay", else the effects-role tracks. The music tracks (role or name) duck too, unless `"includeMusic": false`. Names
  match in any case and with a number or a note after the word ("Mic 2", "Mic/Aux", "Game (Track 1)"). A voice track is never
  ducked. A "Discord" track next to a mic track is neither: it is not ducked and the game does not duck under it (pass
  `"dialogue"` for that). `"dialogue"`, `"music"` and the timing parameters still win. The report names the tracks used
  (`dialogue`, `music`: label and name) and the `preset`.
- **The timing** (`gameUnderVoiceDucking`, `Ducking.h`):
  - -10 dB: the game is what the video is about, so it goes down less than a music bed (-12) and stays present;
  - 0.12 s attack: a short lead-in, ending where the speech starts; a voice reacting to the game follows it closely;
  - 0.35 s hold: down through the gaps between words and a breath;
  - 0.8 s release: back fast enough to hear the game between sentences, slowly enough not to pump;
  - speech from 0.15 s: clicks, coughs and short laughs on an open mic do not take the game down.

  With the attack and the hold, a pause shorter than 1.27 s stays down.
- **Order.** Either order works. Speech found on the processed mic ends a little later, because the detector hears the
  gate's release, so the game comes back a little later too.
- **For the UI engineer.** The Essential Sound panel does not have controls for `repair.gate`, `clarity.makeup`,
  `clarity.limiter` and `clarity.limiterCeiling` yet. When another setting moves, the preset's values are kept, and the
  section switches (Repair, Clarity) cover the gate and the limiter. The EQ menu shows `streamerVoice` by its id until it gets
  a name. One-click entries: "Streamer voice" on a mic track's menu runs `audio.essentialSound` with `"tracks"`; "Game under
  voice" runs `audio.autoDuck` with the preset (dryRun first for the ghost keyframes).

**Measured** (`wabcut_audio_edit_tests "[streamer]"`, `test_streamer_audio_media.cpp`). The fixture is a synthetic 24 s OBS-like
recording. The mic has commentary in three phrases (voiced speech with formants and syllables, "s" sounds) over a fan floor
at -45 dBFS. The game has a bed at about -19 dBFS with explosions. The intelligibility proxy is the mic-to-game ratio inside
the sentences, each track rendered on its own, broadband and 300 Hz - 4 kHz. Measured 2026-10-13 (the test's `[streamer]`
lines):
- the mic: -18.7 LUFS, true peak -1.87 dBTP -> Streamer voice -15.6 LUFS (target -16), true peak -1.00 dBTP (ceiling -1); the
  loudness Gain -1.2 dB, found in one pass;
- the noise floor in the pauses: -46.7 dBFS -> -68.5 dBFS (Reduce Noise + the gate take 28.9 dB of it); the voice over the
  floor 25.7 dB -> 50.6 dB;
- sibilance 5-8 kHz in the "s" sounds: against the voice band 3.1 dB -> 1.7 dB through the chain; the de-esser alone takes
  only 0.2 dB, because Reduce Noise (before it) already removes most of these synthetic s-sounds (the test asks the chain for
  at least 1 dB less sibilance against the voice and the de-esser for none added);
- the mic-to-game ratio in the sentences: recording -2.7 dB, Streamer voice 0.5 dB, plus the duck (-10 dB) 10.5 dB; in the
  voice band 2.6 dB -> 16.1 dB; the game outside the ducks +0.00 dB;
- the duck timing, speech found on the recording: fully down at the first word (+0 ms), back up 10 ms before last word +
  hold + release, in all three phrases; found on the processed mic: fully down at the first word, back up 140-160 ms later
  (the gate's release lengthens the detected speech); no extra duck at the clip's start.

### Music Remix (A.7)

Premiere's Remix as a command, `audio.remix` (`src/audio/edit` `Remix.h`; the media analysis in `MediaRemix.cpp`). A music
clip is retimed to a target length by playing its own bars in another order, never by stretching it.

**How it works:**
1. **Beats and bars:** ai/analysis's beat tracker (C.5), on the stream decoded as mono at 22 050 Hz.
2. **Beat-synchronous features:** per beat, a chroma vector (harmony) and 13 MFCCs (timbre) of frames that start on the beat
   and stay inside it, and its level.
3. **Jump costs:** a jump from beat a to beat j costs the mean feature distance of the bar before and the bar after them.
   Jumps off the bar grid cost more. A join's confidence is 1 − cost / the typical cost of unrelated beats.
4. **The intro and the outro:** section boundaries come from the contrast of the mean timbre and level of the four bars
   before and after each bar line. The intro runs up to the first boundary, the outro from the last one. Both are played once,
   as they are.
5. **The plan:** the jump sequence nearest the target (within half a beat when the music allows) at the lowest cost: an
   exhaustive search of one jump, a loop taken several times, and either with one more jump, plus a beam search for the rest.
6. **The cuts** sit just before the landing beat, on a 1/300 s grid: a whole sample at 44.1, 48, 88.2 and 96 kHz. Each
   piece therefore plays its music a whole number of samples from where the clip played it.

**The result:**
- **As edits** (default): the clip becomes its pieces on its track. The first keeps the clip's id; they share its name, level
  and effects. Each join gets a constant-gain crossfade (an eighth of a beat, 10–50 ms) centred on the cut, so it ends on the
  attack. One undo step; dryRun previews it.
- **As a take** (`audio.remixRender`, same parameters): one float WAV rendered from the file in one-second chunks, cached by
  the file and the plan, added as a media item derived from the song ("remix"). The clip plays it from its start. It adds
  media, so it is a project edit (EditProject: the MCP scopes, the assistant's allow-list and read-only MCP follow the
  capability); `audio.remix` itself only edits the timeline. Until A.8 this was `audio.remix` with `"output": "take"`.
- **The report:** every join (where, from which beat to which, confidence, on the bar or not, its transition), the intro and
  the outro, the tempo, the error, and the analysis time. A join under 35 % confidence, or a result more than a beat off, adds
  a warning.

**Targets:** `duration` (seconds), `end` (a timeline time), or `fit: "video"` (the end of the last video clip: "make the music
fit the video length"). `keepIntro` / `keepOutro` default to true.

**Refusals** (nothing changes):
- a target shorter than the intro and the outro together;
- a clip at another speed, reversed, remapped, nested or linked;
- a remix that would run into the next clip on the track;
- no music analysis on the host.

**Measured** (a synthetic 80 s song with a known structure, `MusicTestSignals.h`):
- **Accuracy:** remixes to 30, 45, 60, 79, 100, 150 and 300 s all land within one beat. Every join is on a detected beat, and
  at most one per plan is off the bar grid. The intro (8 s) and the outro (from 72 s) are found and kept.
- **Seamless joins:** where the song repeats (64, 112, 144 s, 48 s), every join's confidence is above 0.8, and most are
  0.99 or more.
- **No clicks:** around each join, the spectral flux is no higher than the landing beat's own onset in the original.
- **Through `wabcut-cli`:** a 48 kHz song remixed to 48 s and rendered is, outside the crossfades, sample for sample its pieces'
  music.
- **Speed:**
  - a 5-minute track takes 0.84 s in memory (beats 0.5 s, features 0.3 s, plan 0.08 s);
  - a 5-minute 44.1 kHz stereo WAV takes 1.2–2.4 s with decoding;
  - a cached analysis takes 0.02 s;
  - `wabcut-cli audio.remix` of an 80 s song takes 0.6–0.9 s, process start included.

**For the UI engineer** (the Essential Sound panel and the Remix handle; they follow Premiere):
- **The panel** edits the clip's record and the effects marked with its sections.
  - Selecting a clip shows its type (`recordedSoundType`). The type buttons (Dialogue, Music, SFX, Ambience, Clear) and the
    preset menu (`essentialSoundPresets(type)`) run `audio.essentialSound`.
  - The section switches and sliders map to `options` (repair.noise, clarity.eq, ...). Moving one re-runs the command with
    the options from the record's `options` data. Editing the marked effect directly (Effect Controls) also works.
  - Badges: a marked effect the user disabled shows its section off.
- **The Remix handle** on a music clip (Premiere's Remix tool):
  - Dragging the clip's end proposes a length. On release, run `audio.remix` with `dryRun` and `duration` to draw the joins
    (the report's `at`, with a zigzag mark, coloured by confidence).
  - Then commit it. "Fit to video" is `fit: "video"`, and a menu item renders the take (`audio.remixRender`).
- **Remix analysis** of a long song takes a second or two the first time (it is cached after that): show progress from the
  command's checkpoints.

**Limits:**
- The section detector relies on timbre and level changes: songs whose sections differ only in harmony get the default intro
  and outro (the first and last two bars).
- Meters other than the tracker's beats per bar are taken as they come.
- The take is the plan rendered from the file. A clip's volume keyframes stay at their source times in the take (a warning
  says so).

### Voice-over recording (A.8)

Premiere's Voice-over Record: the microphone recorded onto an armed audio track at the playhead, or between the sequence In
and Out (punch-in / punch-out), with a count-in, pre-roll and post-roll playback, input monitoring and latency compensation.
Each pass is a new WAV and a new clip. Passes over the same range stack as takes.

**Where it lives:**
- `wabcut_audio`: `AudioInput.h` (a capture device through miniaudio, or the Null backend fed with `push()`), `WavWriter.h`
  (the crash-safe WAV writer and `recoverWav`).
- `wabcut_playback`: `VoiceOverRecorder.h` (one pass: the transport, the output during the pass, the writer, placement).
- `wabcut_audio_edit`: `VoiceOver.h` (what a pass leaves in the project, the takes switcher) and the commands.
- The app: `VoiceOverBridge` (the QML singleton `VoiceOver`). Its QML is the UI engineer's (notes below).
- The video side: `PlaybackController::setStopAtEnd(bool)` (`Scheduler::setStopAtEnd`), off during a pass so playback runs
  past the sequence end, restored however the pass ends. A small additive hook for the Graphics/playback owner's review.

**A pass** (`VoiceOverOptions`):
1. **Count-in.** The transport starts at punchIn − preroll − countIn seconds (it may start before 0). The program is muted,
   and a beep (880 Hz, 100 ms, −12 dBFS) sounds on each second when `countInBeep` is set. `status().countInRemaining` is the
   3, 2, 1 to show. 0 to 10 seconds; default 3.
2. **Pre-roll.** From punchIn − preroll the program plays (default 2 s), so the speaker hears the lead-in.
3. **Recording.** The input heard while the transport presented [punchIn, punchOut) is the take. Without a punch-out it
   runs until `stop()`.
4. **Post-roll.** After the punch-out the program plays on (default 1 s), then the transport stops (state `finished`),
   once the input past the punch-out has come in.
5. **`stop()`** writes what is left, finishes the WAV, pauses the transport and restores the engine's source. A pass stopped
   before the punch-in records nothing and deletes its empty file. Only `cancel()` deletes a take: destroying the
   recorder, closing the project or quitting during a pass ends it as `stop()` does and keeps the take (review
   A5-P1-7: the window's close, the project's close and the bridge's destructor each finish a running pass and place
   its take while the playback session still exists).
6. **The program during a pass** is the player's, wrapped (count-in mute, monitor) through
   `TimelineAudioPlayer::wrapEngineSource()`: a change of the master's format waits for the end of the pass instead of
   leaving the pass playing a source nobody feeds (A5-P2-3).

**Placement and latency.** A captured frame's place on the timeline is computed, not assumed:
- AudioInput keeps a capture clock line: frame f was delivered at offset + f / rate, the offset the earliest arrival over the
  session (callbacks run late, never early, so the minimum filters their jitter). Frames lost to a full ring are recorded as
  gaps, so the frames after a gap keep their stream index.
- The microphone heard frame f at its delivery time less the input latency: the device buffer miniaudio reports (internal
  period × periods) plus the calibration.
- The audio engine's transport map gives the timeline position presented (heard) at that host time, the output latency
  included. Snapped to the sample grid, that is the frame's place; the take starts at its first frame's place.
- **Measuring the rest (loopback).** `loopback: true` plays a 2-sample click every 250 ms from the punch-in instead of the
  program, with the output wired (or the microphone held) to the input. `LoopbackResult` gives the residual (where the clicks
  came back less where they were played: the latency nobody reported), the reported output and input latencies and the round
  trip. Add the residual to the calibration (the app does).
- `VoiceOverResult::latencyCompensation` (the input latency plus the calibration) is kept in the take's provenance.

**Takes** (`VoiceOver.h`):
- `placeVoiceOverTake` puts a finished pass in the project, as one undo step ("Voice-over Record"):
  - a media item in the Voice-over bin (made when missing), named "Voice-over A2 Take 3", with `MediaItem::recording` set
    (the sequence, track and range, the take number, the input device, the latency compensation, the time);
  - a clip of it overwritten onto the track over its range, so the newest take is the one heard.
- A slot's takes are the recorded media of the same sequence and track whose ranges overlap. The earlier takes stay in the
  bin.
- `voiceOverTakesOf` lists them, newest first, with the one the clip plays marked. `useVoiceOverTake` puts another take of
  the slot over its recorded range (an overwrite), with the clip's level, pan and effects. This is AI-11's takes idea (one
  line, several attempts, "use this take") for recorded passes. AI-11's `takes.find` compares attempts by transcript; a
  recorded slot needs no transcript.

**Crash safety.** The take is a canonical WAV (48 kHz, 24-bit PCM by default; 16-bit and 32-bit float too) written as it is
recorded. Every half second the header's RIFF and data sizes are rewritten to cover what was written and the file is
flushed, so after a crash the file is a valid WAV up to the last sync. Takes go to the project's own folder,
`<project>.wabcut-media/Voice-over` (takes recorded before stay where they were), with a `<take>.journal` sidecar written
when the take starts: its format and where it was being recorded (sequence, track, punch-in, device).
- `recoverWav` repairs only a file with its journal (of the same format): it rewrites the sizes and cuts a frame the crash
  cut in half. It refuses links, junctions, hard-linked files and files over 4 GiB, and opens the file read-only unless it
  repairs (reviews A5-P2-12, S5-P2-11: a foreign WAV with a trailing LIST chunk was "repaired" and truncated in place).
  `inspectWav` checks without writing.
- When a project opens, the app lists (read-only, links not followed) the takes in its Voice-over folder that no media item
  uses and whose names are the recorder's: `VoiceOver.recoveredTakes`. `placeRecoveredTake(path)` repairs the one the user
  picks (in place: only our own file's sizes and the cut frame, what the next sync would have written) and puts it back
  where its journal says it was recorded; `dismissRecoveredTakes()` stops offering them (A5-P2-13).

**Real time.** Nothing on either audio callback blocks, locks or allocates:
- The capture callback copies frames into AudioInput's lock-free SPSC rings (the capture ring, 8 s; the monitor ring,
  0.25 s), updates the meters and the clock line (atomics, a seqlock), and records a gap when the ring is full.
- The output callback runs the pass's source over the player's: program mute, the beeps or clicks, and input monitoring from
  the monitor ring. It reads the newest frames and keeps the backlog to one input period, so the monitor latency stays
  bounded: the input buffer + one period + the output latency (`status().monitorLatency`).
- A writer thread (every 5 ms) moves captured frames to the WavWriter. The RT audit test covers recording with monitoring,
  the count-in beeps and playback (`[playback][rt][voiceover]`).

**Privacy.** The microphone opens only on the user's own action in the app (Record, the input meter in Preferences), and
`VoiceOver.inputOpen` is true exactly while it is open, for the recording indicator. Recording is not a command: no agent,
script or MCP client can start it. The input device's name is the OS's text: kept as valid UTF-8, at most 256 bytes, and
never treated as instructions.

**Commands** (`src/audio/edit` `VoiceOverCommands.cpp`):
- **`track.setRecordArm {track, armed (default true)}`** (EditSequence, one undo step): Premiere's R button in the track
  headers and the Audio Track Mixer. Allowed on locked tracks; video tracks are refused.
- **`record.takes {clip}`** (Read): the slot's takes, newest first (take, media, name, start, seconds, device, createdAt,
  active).
- **`record.useTake {clip, take}`** (EditSequence, one undo step; dryRun previews it).
- `record.start`, `record.stop` and `record.toggle` are app-level keymap commands (Application scope, no default keys, as in
  Premiere; docs/keymap.md). They are not in the command registry.
- **The assistant** can arm tracks and switch takes, and cannot record: there is no tool for it.

**Capability audit (A.8).** Commands that create or change project-level things declare EditProject; the MCP scopes, the
assistant's allow-list and read-only MCP depend on it:
- `audio.remix` (pieces on the timeline) is EditSequence; its rendered take moved to `audio.remixRender` (EditProject).
- `audio.enhanceSpeech` records takes on media items: now EditProject. Essential Sound's Enhance Speech only applies the
  effect.
- `audio.separateStems` was already EditProject.
- Tests: the capability map, a sequence-only engine refusing `audio.remixRender` and `audio.enhanceSpeech`, and the
  assistant's tools.

**Model.** `Track::recordArmed` and `MediaItem::recording` (`RecordingProvenance`) are additive, written to project files
only when set (no schema bump: other projects stay byte-identical) and validated:
- a positive range from 0, a take from 1, a UTF-8 device name of at most 256 bytes, an ISO-8601 UTC time;
- a video track cannot be armed;
- the sequence and track are weak references: a take whose track was deleted is valid. Merges remap them.

**Measured** (`playback` suite, the Null engine and input with known latencies; `[playback][voiceover]`):
- **Placement:** with 10 ms of output latency (480 samples) and 5 ms of input latency (240), both reported, a take
  punched in at 2 s starts at exactly 2 s (0 samples off), and all 48 000 samples of its second are bit for bit the
  program the microphone heard at their places (32-bit float takes; a 24-bit take differs only by its quantisation).
- **Unreported latency:** an input that takes 37 samples (0.77 ms) more than it reports places takes 37 samples late. The
  loopback pass measures exactly 37 samples from 7 clicks (a round trip of 757 samples, 15.77 ms), and with that
  calibration the next take is exact again.
- **Punch-in/out:** a 1 s range gives exactly 48 000 frames, a 0.5 s range at the sequence start (the count-in from
  −1.5 s) exactly 24 000. A manual stop keeps the exact start and everything heard until the stop.
- **Monitoring:** the monitor latency is the input buffer + one period + the output latency: 240 + 256 + 480 samples
  (20.3 ms) in the test. On a real device it is that device's buffers and granted period.
- **Real time:** no allocation and no lock on either callback through a pass with monitoring, the beeps and odd block
  sizes (`[playback][rt][voiceover]`).
- The `[audio-device]` test (a real input, at most 2 s) was built but not run here: it opens the microphone, which needs
  the person at the computer to agree. Run it with `wabcut_playback_tests "[audio-device][voiceover]"`.

**Limits:**
- The input and output clocks are taken to run at one rate. A take keeps its length; drift between two devices over a long
  take is not corrected (the take is placed by its first frame).
- One track and one input per pass; the app records mono (the recorder and the WAV take stereo).
- The input runs at the engine's rate (48 kHz); miniaudio converts from the device's rate. The reported input latency is the
  device buffer: the converter and the driver's own delay are what the loopback measures.
- In the app the transport is the playback session's: a user seek during a pass is not guarded (the UI should stop the pass
  first).

**Tests:**
- `audio` (`test_voice_over_io.cpp`): AudioInput on the Null backend (order, clock line, gaps after a full ring, meters,
  monitoring) and the WAV writer (16/24/32-bit, syncs, recovery after a cut mid-write, a file copied while open).
- `playback` (`test_voice_over.cpp`, `test_rt_audit.cpp`): placement to the sample with output and input latency, the
  loopback measurement and its calibration, punch-in/out and manual stop, a count-in before 0, monitoring, the RT audit;
  `[audio-device]` records from a real input (bounded, skipped without one).
- `audio_edit` (`test_voice_over_takes.cpp`): the take in the project, takes stacking and switching, the device name bound,
  the commands; `test_essential_sound.cpp`: the capability audit.
- `model` (`test_voice_over_model.cpp`): record arm, validation, project files. `ai` (`test_agent.cpp`): the assistant
  cannot record. `keymap`: the record commands. `playback_video` (`test_scheduler.cpp`): stop-at-end off and on.

**For the UI engineer** (the `VoiceOver` singleton; Premiere's Voice-over Record):
- **Record arm (R)** in the Audio Track Mixer strips (MixerStrip.qml's `recordToggle` placeholder) and the audio track
  headers: `VoiceOver.setRecordArm(trackKey, armed)` (track.setRecordArm, undoable). The armed state is `Track::recordArmed`:
  the strip and header models need it added.
- **Record:** the track header's microphone button and the Program Monitor's record button run `VoiceOver.toggle()`. Route
  the keymap's `record.toggle`, `record.start` and `record.stop` (Main.qml's `commandTriggered`) to `toggle()`, `start()`
  and `stop()`. Space during a pass stops it and keeps the take (the bridge watches the transport). Esc can `cancel()`.
- **Count-in overlay** in the Program Monitor: while `state == "countIn"`, the big `countIn` number (3, 2, 1). While
  `"recording"`, a red dot, "Recording" and `recordedSeconds`; `"preroll"` and `"postroll"` dimmed.
- **Recording indicator:** whenever `inputOpen`, a red microphone in the header bar (also for the Preferences meter). Hover:
  `inputName`.
- **Input meter:** `levels` (`peakDb`, `rmsDb`, `clipped`) at ~30 Hz while `inputOpen`, on the armed strip's meter (Premiere
  meters the input on armed tracks; `audioMixer.meterInputsOnly`) and in Preferences.
- **Preferences > Audio > Voice-over:**
  - the input device (`inputDevices` after `refreshDevices()`, `inputDevice`; "" = the system default);
  - "Test input" (`openInput()` / `closeInput()`) with the meter;
  - `options`: count-in seconds, beep, pre-roll, post-roll, monitor input, monitor gain. Persist them with Settings;
  - latency: "Measure" runs `measureLatency()` (tell the user to hold the microphone to the speakers, or connect the output
    to the input). `latencyMeasured(roundTripMs, clicks)` reports it; `latency.calibrationMs` is set (persist it too).
- **Takes switcher:** on a clip whose `takesOf(clipKey)` is not empty, a "Takes" chip on the clip (and in Effect Controls'
  header) listing take n, time and device, the active one checked; choosing one runs `useTake(clipKey, n)`.
- **After a pass:** `takeRecorded(clipKey)`: select the clip. `lastError`: a toast.
- **Interrupted takes:** when `recoveredTakes.length > 0` (it fills by itself when a project opens), a prompt or a row:
  "Place" runs `placeRecoveredTake(path)` (the take goes back where it was recorded), "Dismiss" runs
  `dismissRecoveredTakes()`. Main.qml's `onClosing` already stops a running pass (`VoiceOver.stop()`) before the save
  prompt.

### Audio channels and 5.1 (A.9)

Premiere's Modify > Audio Channels, its Fill Left / Fill Right / Swap Channels / Channel Volume effects, mono, stereo,
5.1 and adaptive tracks, and a 5.1 master with a panner. It is essential for real interview and podcast media: two
microphones recorded on the left and right channels of one file.

**Model** (`wabcut/model/AudioChannels.h`, `AudioChannelOps.h`; additive, written only when set, so other projects save
byte-identical):
- `ChannelFormat`: mono, stereo, 5.1 (L, R, C, LFE, Ls, Rs, the SMPTE / WAV / FFmpeg order), adaptive (channels pass
  through one to one).
- `AudioChannelMap` on a clip (`ClipContent::channels`) or a media item (`MediaItem::channels`): the clip's format and,
  for each clip channel, the source channels that feed it (averaged when several), a mute, and a volume in dB (a
  constant or keyframes in the clip's curve time: Premiere's Channel Volume). A clip without a map uses its item's;
  without either it plays as before A.9.
  - Presets: `identity` (each source channel as it is), `mono(c)`, `sumToMono`, `fill(c)` (Fill Left with Right is
    `fill(1)`), `swap`.
  - Bounds: 32 clip channels, source channels below 64, 8 sources per channel, volume -144..+24 dB. Files past them
    are refused.
- `Track::format` (stereo by default) and `Track::surround` (the 5.1 panner: x, y, the LFE send).
- `Sequence::masterFormat`: stereo or 5.1.
- Ops: `setClipChannels`, `setMediaChannels`, `setMasterFormat`, `splitClipToMono` (the clip plays channel 1, linked
  copies on the given free tracks play the others), and `TrackStateChange::format` / `surround`.
- The ChangeSet reports a map change as an audio change of the clip, and a format or panner change as a track header
  change (`Track::sameHeader`). The player refills what they change.

**Engine** (`playback/audio/ChannelLayout.h`, `TrackRenderer`):
- **The plain path** (no map, a stereo track, a stereo or mono bus, or a 5.1 bus before the track's 5.1 panner)
  decodes the stream to the bus (or the track's two channels) as before: bit-identical.
- **The routed path** reads the source at its own channel count (MediaItem's probed channels), applies the map
  (averages, mutes, per-channel volume on the parameter grid), then one matrix: the clip's format to the track's, then
  the track's to the bus's.
- **On a 5.1 bus a mono or stereo track renders at its own channel count**: its clips, their effects and the track's
  effects run on one or two channels, then its 5.1 panner places the result on the bus, then the fader sets the level
  (Premiere's order; review A5-P2-10). Stereo effects and plug-ins see the track's channels, never the puck's spread.
  5.1 and adaptive tracks and the master run at six: there the reverb leaves the LFE dry (its tail goes to L / Ls and
  R / Rs, the mid to C) and stereo width acts on L and R.
  - mono → stereo: -3 dB on each side, what FFmpeg's resampler does for a mono file;
  - stereo → mono: the mean;
  - 5.1 → stereo: ITU-R BS.775, L' = L + 0.7071 C + 0.7071 Ls, R' = R + 0.7071 C + 0.7071 Rs, LFE dropped;
  - mono → 5.1: C; stereo → 5.1: L, R; adaptive: channel to channel.
- **The 5.1 panner:** two-dimensional VBAP over the five full-range speakers (L -30, R +30, C 0, Ls -110, Rs +110
  degrees). Its x, y and LFE send are taken on the 32-frame parameter grid (timeline frames) and interpolated between,
  like every automated audio parameter, so a moving puck does not step and preview equals export (A5-P2-1).
  - The puck's direction is atan2(x, y). Its distance from the centre is the focus: at the rim, the nearest speaker
    pair (or one speaker); at the centre, all five equally.
  - The power sums to 1. Stereo is placed at the direction -30 / +30 degrees, so the default puck (front centre)
    plays stereo on L and R and mono on C exactly.
  - The LFE send adds the inputs' mean to the LFE. The track's stereo panner does not apply on a 5.1 bus.
  - A stereo track keeps its stereo image: a mono clip on it (as `audio.splitToMono` leaves them) plays at the puck's
    direction -30 / +30 degrees, spread over the speakers around it. Make the track mono (`track.setFormat`) to put a
    mic on one speaker.
- **The mix follows the master's format.** A 5.1 master mixes in 6 channels:
  - the player converts the mix to the device's channel count in the callback (preallocated, RT-safe), the ITU
    downmix on a stereo device;
  - the downmix sums up to 2.41 times (+7.7 dB): when the mix is downmixed, the master's soft clip guard runs once,
    after the downmix, on what is delivered, in the player and in OfflineMixdown (A5-P2-2). The meters and the live
    loudness measure the 5.1 mix (before the guard);
  - OfflineMixdown converts it to the channels asked for. Export uses `AudioSettings::channels` 0 ("as the master");
    AAC, AC-3 (FFmpeg's native encoder, new) and PCM all take 5.1;
  - switching the master's format while playing rebuilds the player's source (the old one is kept until the engine
    lets go of it).
- **Loudness:** a 5.1 mix is measured in 5.1, in the analysis (`audio.loudness`) and the live tap: libebur128 weighs
  Ls / Rs 1.41 (+1.5 dB) and ignores the LFE.

**Commands** (`src/audio/edit` `AudioChannelCommands.cpp`; channel numbers are 1-based, as Premiere shows them):

| Command | Capability | What |
|---|---|---|
| `audio.setChannels {clips, preset \| map}` | EditSequence | Modify > Audio Channels on clips: presets `fillLeft`, `fillRight`, `swap`, `sumToMono`, `mono` + `channel`, `stereo`, `identity`, `default` (no map), or a map `{format, channels: [{sources, muted, gainDb}]}` |
| `audio.setMediaChannels {media, preset \| map}` | EditProject | the same on media items. The brief's `audio.setChannels(media)` is its own command, because it changes media items (A.8's capability rule) |
| `audio.setChannelVolume {clips, channel, db, time?}` | EditSequence | Channel Volume, a constant or a keyframe |
| `audio.fillChannel {clips, from, to?}` | EditSequence | Fill Left with Right / Fill Right with Left |
| `audio.splitToMono {clip, tracks?}` | EditSequence | one linked mono clip per source channel, on the free audio tracks below (new tracks when needed) |
| `audio.channels {clips?}` | Read | each clip's source channels and map (its own, its item's, none), the tracks' formats and panners, the master |
| `audio.channelLevels {clips}` | MediaAnalysis | peak / RMS per source channel (at most 120 s of the clip) and `silentChannels` (30 dB under the loudest) |
| `track.setFormat {track, format}` | EditSequence | mono, stereo, 5.1, adaptive |
| `track.setSurroundPan {track, x, y, lfeDb}` | EditSequence | the track's 5.1 panner |
| `sequence.setMasterFormat {format}` | EditSequence | stereo or 5.1 |

**The assistant:**
- "Split the two mics into separate tracks" is `audio.splitToMono`.
- "Fix the left-only audio" is `audio.channelLevels` (a MediaAnalysis read the assistant may run, like
  `audio.loudness`), then `audio.fillChannel` from the side with sound.
- Both are staged edits, tested with a scripted model. `audio.setMediaChannels` is not among its tools.

**Interchange:**
- FCP7: a clip playing one channel of a multichannel stream gets a mono map of that channel (Carried; it was
  Approximated, "plays the whole stream"). Stereo pairs stay one stereo clip.
- FCPXML: `audio-channel-source` becomes the clip's map (Carried): `srcCh` and `outCh` with L, R (also R, L: a swap),
  C or the six 5.1 names; `active="0"` mutes. Several components become one mono channel of their sources averaged
  (Approximated).
- OTIO has no channel field: unchanged (Approximated, stream 1).

**Limits:**
- A route averages its sources (sum to mono at half level of the two, never louder than either); Final Cut mixes
  components at full level.
- A stereo track panned where both of its sides fall between the same two speakers (behind the listener: Ls and Rs
  are 140 degrees apart) plays both sides on them, so material that is the same on both sides comes out up to 3 dB
  louder there. A mono track (track.setFormat mono) keeps its power everywhere.
- No 7.1 master. An adaptive track on a 5.1 master passes its first six channels.
- The live fader override's pan applies on a stereo master only.

**Tests:**
- `model`: presets, validation, ops, split to mono, track formats, master format, project files byte-identical
  without the fields.
- `playback`: every preset sample for sample on synthetic multichannel sources, media maps, keyframed per-channel
  volume, track formats and the 5.1-to-stereo coefficients, the panner's speakers and constant power, a 5.1 master
  with the LFE send and its downmix, preview == offline (stereo and 5.1 masters), a master switched to 5.1 while
  playing; the RT audit with a 5.1 master on a stereo device.
- `audio` (5.1 loudness against libebur128, the LFE ignored, surrounds +1.5 dB), `audio_edit` (the commands, the
  capability, the channel analyzer on a real file, 5.1 loudness in the analysis), `ai` (both assistant requests),
  `interchange` (FCP7 and FCPXML fixtures), `export` (5.1 AAC and AC-3 by ffprobe, the stereo downmix, the settings).

**For the UI engineer:**
- **Modify > Audio Channels dialog** (clip context menu and the Library's item menu, Premiere's layout):
  - the clip format (mono / stereo / 5.1 / adaptive);
  - a grid of clip channels by source channels (checkboxes; a column per source channel of the item), a mute per clip
    channel;
  - quick buttons for the presets.
  - It runs `audio.setChannels` (clips) or `audio.setMediaChannels` (items); `audio.channels` fills it.
- **Clip menu > Audio Channels:**
  - Fill Left with Right / Fill Right with Left (`audio.fillChannel`), Swap Channels (preset `swap`), Split to Mono
    (`audio.splitToMono`).
  - Channel Volume in Effect Controls: two sliders with keyframe toggles (`audio.setChannelVolume` with `time`).
  - "Find silent channels" can run `audio.channelLevels` and badge clips whose `silentChannels` is not empty.
- **Track format chooser:** in the track header's context menu and Add Tracks (mono / stereo / 5.1 / adaptive,
  `track.setFormat`). Show the format on the header like Premiere's track icon.
- **The 5.1 panner:** when the master is 5.1 (Sequence Settings > Audio > Master: stereo / 5.1, through
  `sequence.setMasterFormat`), the Audio Track Mixer's pan knob on mono and stereo tracks becomes a puck in a square
  (x, y) with an LFE knob (`track.setSurroundPan`). The master meter shows six channels (`masterMeter()` has them).
- **Export dialog:** Audio Channels: "As the master" (0), Mono, Stereo, 5.1 (6), and AC-3 in the codec list.

## AI analyses through multicam and nested clips (0.2)

A multicam clip (a podcast's cameras) or a nested clip (a gaming nest) shows and plays what its sequence holds, so the
analyses read that, not a media item of their own. Two patterns, as transcript editing (C.17b), Split at Shots and
the colour commands (0.1.2) already do:

- **Media the clip plays, through the time maps.** `mediaPlayedBy(state, clip)` (Resolve.h) lists the recordings a
  clip plays: a multicam clip its active angles' media only, a nested clip what the tracks it reads hold, through every
  level. The host measures those once each; the analysis then reads them back at each time through `resolveLeaves()`
  (the active angle at the angles' sync, the nest's speed, reverse and remap, every level).
- **The picture as one picture.** A nested or multicam video clip is one picture, its nested sequence's canvas
  (`reframe::nestedPictureTrack`: the active angle's video track, or a nest's only video track with clips). Results
  found in the recording there move into the nested time and onto the canvas through that clip's Motion
  (`reframe::canvasPlacement`: Position, Scale, Anchor; rotation is not followed).

The result always lands on the multicam or nested clip itself: it is never flattened (that broke the podcast workflow
once), cuts are made in place so angles stay switchable, keyframes go on the clip. One undo step per command; dry runs
are unchanged.

| Command | Through multicam / nested | How |
|---|---|---|
| `highlights.find` | yes | Measures the recordings the clips play (`mediaPlayedBy`); each 100 ms sample of a nested or multicam clip takes the signals of its leaves at their media time (the top picture's visual signal, the strongest audio), keywords and OBS markers moved to where the clip shows them. A clip of the sequence played flat, as the active angle or inside a nest gives the same moments. |
| `sequence.removeDeadTime` | yes | Activity of the recordings inside (the busiest leaf per sample, `resolveLeaves`); a nested clip counts as the gameplay track. The ripple delete cuts the nested / multicam clips in place: the pieces stay nested / multicam clips with their angles. |
| `reframe.createSequence`, `shorts.create` (its reframe) | yes, one picture track | The camera keys go on the nested / multicam clip, framing its canvas; the subjects are those of the media its picture track shows, moved into nested time and canvas. A nest with layers (several video tracks with clips) stays centred, with a warning. `shorts.find` scores from the transcript, which reads through these clips (C.17b), and its energy and face / shot-rhythm signals read the recordings the clips play and show (`resolveLeaves` at each part's ends): a podcast flat, as a multicam clip or in a nest gets the same shorts. |
| `audio.syncClips` | yes | Syncs by the recording the clip plays (a multicam clip: its active audio angle; a picture: the shown media's sound), found at the clip's start (or a quarter further in, for an angle that starts late); Synchronize moves the whole clip by that recording's media time at its start. Retimed levels are not moved (a warning). |
| `audio.channelLevels` | yes | One entry per recording the clip plays at its start (with `media`, `name`): a multicam clip's active audio angles' streams, a nest's clips. |
| `autozoom.apply` | yes, one recording | The activity (sidecar, `cursor` or detected frames) is the recording's own, read over the part the clip shows and moved into nested time and onto the canvas; the zoom keys go on the clip. Refused with the reason for layers, several clips in the window, or a retimed recording inside. |
| `mask.objectSelect`, `mask.refine`, `mask.propagate` | yes (0.2) | They segment what the clip shows: its nested sequence rendered by the export renderer at nested time with only the tracks the clip shows (`NestedFrameSource`; a multicam clip's active angle). The mask goes on the nested / multicam clip itself; its matte names the nested sequence (`MaskMatte::sequence`, schema 18) at nested time, the clip's curve time, so preview and export need nothing new. The nest's edit points stop the tracking like shot cuts. After an angle switch the mask stays where it was on the frame (as a tracked mask in Premiere) and `mask.propagate` re-tracks the angle shown. A host that renders no nested pictures refuses with the reason. |

Tests (synthetic media, 2-angle multicam and a nest each): `tests/model/test_gaming_ops.cpp` "Dead time reads through
multicam and nested clips", `tests/command/test_gaming_commands.cpp` "sequence.removeDeadTime measures a multicam clip's
active angle and cuts it in place in one undo step", `tests/ai/highlights/test_highlights.cpp` "highlights.find reads through a multicam clip and a
nested clip", `tests/ai/reframe/test_reframe.cpp` "Auto Reframe follows the subjects of a multicam clip's active angle and
of a nested clip", `tests/audio_edit/test_sync_commands.cpp` "audio.syncClips syncs a multicam clip and a nested clip",
`tests/audio_edit/test_audio_channels_commands.cpp` "audio.channelLevels measures what a multicam clip and a nested clip
play", `tests/ai/creator/test_autozoom.cpp` "autozoom.apply zooms a multicam clip and a nested clip",
`tests/ai/shorts/test_shorts.cpp` "Shorts hear and see a podcast through a multicam clip and a nest",
`tests/ai/segment/test_object_mask_commands.cpp` "mask.objectSelect on a nested clip..." and "mask.objectSelect on a multicam
clip: the active angle, never flattened; an angle switch keeps the mask", `tests/ai/segment/test_segment_media.cpp`
"NestedFrameSource: a multicam clip's active angle at nested time".

## Editing assistant

The AI editing assistant (AI-5) lives in `src/ai/agent`, library `wabcut_ai_agent`. It is pure C++ (model, command, ai and
ai_edit) and builds in the core preset. The app side is `AgentController` (`src/app/src/bridge`) plus
`qml/panels/ai/AssistantPanel.qml`. It is built so that a wrong model cannot change the project behind the user's back:

- every edit goes to a staging copy;
- deterministic checks decide what the report says;
- the user's Accept applies the whole edit as one undo step.

```
EditAgent.h     AgentRequest/AgentOutcome, EditAgent::run (the tool loop), applyStaged (the guarded apply)
Verification.h  Constraints, parseConstraints (from the request text), verify -> VerificationReport
QuickActions.h  the panel's one-click requests: prompt + constraints
Preview.h       stagedPreview: cuts (base time), new content, new markers, for the timeline preview
Describe.h      describeCall / describeChanges: tool calls and ChangeSets as human-readable steps
```

### The loop (`EditAgent::run`, a worker thread)

- **Tools.**
  - The model gets the command catalog: `editingCommands()`, which is the built-in commands plus the transcript commands.
  - It is exported with `exportTools(..., JsonSchema, allowed)` and converted with `toolSpecsFromCommandExport`.
  - The allowed capabilities are Read + EditSequence by default; Preferences can add EditProject.
  - Read tools:
    - `sequence_summary` and `clip_details`;
    - transcript slices through `transcript_text`, `transcript_words` and `transcript_review`;
    - media search by name through `media_list` and `clip_find`;
    - beats and shots through the C.5 `analysis.*` commands, plus `sequence.beats`.
  - A call without `sequence` gets the request's sequence. An edit of another sequence is refused.
  - Strict tool modes send `null` for properties that were left out (OpenAI). These nulls are dropped before the command
    runs, unless the catalog schema allows null.
- **Staging.**
  - The run branches from `EditSession::stateForSave()`, which keeps the id high-water mark, at a known revision.
  - Every mutating call runs on that staging `EditSession` through a `CommandEngine`. The engine's own checks apply:
    schema, references, validate(), locked tracks.
  - The live session is never touched.
  - The calls that changed something are recorded in order as one `CommandBatch`. The cumulative change is
    `diff(base, staged)`.
  - Read calls see the staged state, so the model reads its own edits back.
- **Tool results.**
  - Reads return the command's data, cut at 24,000 characters with a note to narrow the request.
  - Edits return `{ok, changed: "2 clips removed, 1 changed on V1, A1; length …", sequenceLength, createdIds, warnings, data}`.
  - Failures return `{error: {code, message, details}}` with `isError`, so the model can fix its call. The recorded fixtures show
    each adapter's wire form.
- **Verification.**
  - When the model stops calling tools, `verify()` checks the staged result against the base. The check ids are stable:
    - `invariants`: validate().
    - `changes`: something was staged, or no edit was needed.
    - `duration`: within the target, ± max(1 s, 5%), or under a maximum.
    - `protected`: the source content of protected ranges is still on the same track. Moving is fine.
    - `scope`: no content edit outside the referenced range or tracks. Ripple shifts are not edits.
    - `onlyMarkers`: marker and caption actions changed no clip.
    - `tracks`: no track added or removed unless asked.
    - `flashFrames`: no new clip shorter than 2 frames.
    - `gaps`: no new gap inside a track unless asked ("lift", "leave gaps").
    - `locked`: locked tracks untouched.
  - Constraints come from quick actions, or from unambiguous phrases in the request, which `parseConstraints` reads:
    - "a 30-second highlight";
    - "keep it under 2 minutes";
    - "don't touch 00:01:00:00-00:01:30:00";
    - "between 0:10 and 0:20";
    - "only on A1".
  - A bare "less than 2 s" is not read as a length: it usually describes pauses.
  - Failed checks go back to the model once, as a user message listing them. After that the outcome reports what the checks
    found. **The status is never the model's claim:**
    - `Staged`: all checks passed.
    - `VerificationFailed`: checks still fail.
    - `NoChanges`: the model answered without editing.
- **Limits.**
  - `maxSteps` (16 model calls) and `maxToolCalls` (64).
  - A token budget.
  - A per-run cost budget, US$1 by default. Before each call, `estimateChatCost` must leave the expected cost inside the
    budget; after it, the actual `costOf(usage)` is added.
  - The guarded model's monthly SpendGuard cap, which ends the run as `BudgetExceeded`.
  - Cancellation through a `stop_token`. It also stops a running command at its checkpoints.
  - A timeout, enforced by a watchdog that fires the same stop.
  - A run that stops early still checks what it staged. Accept stays possible except after Cancelled or Failed.
- **Prompt.**
  - The system prompt is a short block of fixed rules plus a compact project summary: sequence settings, tracks with clip
    counts and the first clips, markers, caption tracks and media names.
  - It is built once per (revision, sequence), so every call of a run, and of later runs on the same revision, sends the same
    prefix.
  - `ChatRequest::cachePrompt` makes the Anthropic adapter put `cache_control` breakpoints on the last tool, the system prompt
    and the end of the conversation. OpenAI and Gemini cache long prefixes automatically.
  - The user message restates the constraints the result will be checked against.
- **Transcript.**
  - Each run keeps a redacted JSON record: prompt, system prompt, tool names, every message, steps, verification, usage, cost
    and the batch.
  - Text goes through `Redactor`. Reasoning signatures and image bytes are dropped.
  - The app writes it to `<AppLocalData>/ai/agent/agent-<time>.json`. It is never uploaded.

### Accept (`applyStaged`, GUI thread)

- If the live session is still at the base revision, the batch runs with `expectedRevision` as ONE undo step, labelled
  "AI: <request>". The result must equal the staged preview (same state and ids); if not, it is undone and reported.
- If the user edited meanwhile, the batch is dry-run on the current state. It is applied, as `Rebased`, only if:
  - it removes and changes the same clips;
  - it adds as many clips as the preview did;
  - the checks still pass on the current state.

  Otherwise it is `Refused` ("run the request again") and nothing changes. Word indices of transcript commands are
  re-resolved by the dry run, so a transcript that changed underneath refuses rather than cutting the wrong words.

### Refine and quick actions

- **Refine** continues the outcome: same base, its staged state and conversation, and a batch that grows. Accept still
  applies everything as one undo step. Constraints merge: a new length or range replaces the old one, and the rest is kept.
- **Quick actions** (`QuickActions.h`) are fixed prompts with constraints:

  | Action | Checks |
  |---|---|
  | Remove silences longer than X s | `transcript.cleanup` with no filler removal; finding nothing is a fine answer |
  | Remove filler words | |
  | Make an N-second highlight | `targetDuration` = N s |
  | Add markers at topic changes | marker only |
  | Generate captions | caption only |
  | Cut to the beat of track A3 | `analysis.detectBeats` on the music clip, then `edit.snapToBeats`; clip edits only on the other tracks (`onlyTracks`) |

  "Cut to the beat" is listed when the host has analysis services and the sequence has an audio track with clips. The app
  picks the last such track, where a music bed usually is.

### App (`AgentController`, `AssistantPanel.qml`)

- **Opening.** The palette's "Ask AI…" row hands its query to `AgentController.open()`. The palette closes and records the
  hand-off in `mode`. The panel is a non-modal popup at the top right, so the timeline stays in view.
- **Panel layout.**
  - A prompt box: Enter sends.
  - Quick actions.
  - The phase and the streamed reasoning summary.
  - Steps as edits, for example "Remove pauses longer than 1 s". Errors are marked; a step shows a spinner while it runs.
- **Result.**
  - A ✓/✗ report of the checks, the change summary, the model's answer and the cost.
  - Reject · Refine · Accept.
- **Data-flow badge.**
  - It shows "model · provider", or "on this computer" for local providers.
  - It says what is sent: the request, a sequence summary and the transcript excerpts read, never media files.
  - The first use of a cloud provider opens AI-1's consent prompt (`AiConsentDialog`). Nothing is sent before an answer.
- **Model.** Runs use the user's chat model through `AiPlatform::chat` (guarded: locks, consent, spend cap, request log).
- **Transcripts, beats and shots.** They come from the analysis cache by source fingerprint, bundled like `wabcut-cli`
  (`command::ServiceBundle`):
  - `CachedTranscriptProvider` + `DecoderEnergyProbe`;
  - `AnalysisServices(MediaAnalysisProvider)`, which analyses on the worker when the cache has nothing.
- **Preview.**
  - The staged edit shows over the CURRENT timeline. `TimelineView::setExternalPreview()` is a self-contained block in
    `TimelineViewPreview.cpp`. It draws a read-only overlay child item and never touches gestures or the model.
  - Cuts are struck through and hatched (Theme.danger), new content is a dashed ghost clip, new markers are dashed lines.
  - The ranges come from `stagedPreview()`: source content per track, so ripple shifts and splits are not shown as changes.
  - If the timeline changes while a result is shown, the preview is removed and Accept re-checks (rebase or refuse).

### Tests

- **`ai` suite** (core preset):
  - `test_agent.cpp` uses a scripted model. It covers:
    - an end-to-end "remove silences" run that produces the same timeline as `transcript.cleanup` run directly;
    - staging isolation;
    - tool errors fed back;
    - a wrong plan caught by verification and never reported as success;
    - its fix after the feedback;
    - rebase and refuse on a stale revision;
    - cost and token budgets, the spend cap, the step limit, cancel and timeout;
    - the redacted transcript, Refine, quick actions and step wording;
    - "Cut to the beat" through C.5 beat detection (`MapAnalysisProvider`), with the music track left unchanged.
  - `test_agent_verification.cpp` covers every check, `parseConstraints` and `stagedPreview`.
- **`ai_net`**: `test_agent_providers.cpp` replays one recorded session per chat adapter (`tests/ai/fixtures/agent/<provider>`):
  read → invalid call → error fed back → fixed call → done. It checks each wire form of the error result, reasoning replay,
  cache usage and Anthropic's cache breakpoints. One optional live session runs with `WABCUT_LIVE_ANTHROPIC_KEY`.
- **`ai_ui`**: `test_agent_ui.cpp` runs on the real Main.qml with a scripted model. It covers:
  - the consent and data-flow badge;
  - palette → panel → staged preview → Accept as one undo step;
  - a failed check that is shown, not hidden.

  Screenshots: `assistant-panel-staged.png`, `assistant-panel-failed-check.png`.

## Media analysis: shots and beats

Scene-edit detection and beat-synced editing (C.5, AI features #5a and #8): `src/ai/analysis`, library
`wabcut_ai_analysis` (pure C++, deterministic, CPU only). Decoding is in `wabcut_ai_analysis_media` (full builds).
All of it is our own implementation. The algorithms follow the sources below, and no third-party code is used, so there
is nothing to add to the notices.

**Shot detection (`ShotDetection.h`)** runs on small frames (160 px wide, every frame) decoded by `media::VideoDecoder`,
hardware when available. The detectors follow PySceneDetect's (BSD-3-Clause; reimplemented from its documentation):

| Detector | Rule |
|---|---|
| `ContentDetector` | Mean absolute HSV difference between consecutive frames ≥ 27 |
| `AdaptiveDetector` | That value ÷ the mean of 2 neighbours each side ≥ 3, and the value itself ≥ 15 |
| `ThresholdDetector` | Mean intensity < 12 between a fade out and a fade in; the cut goes in the middle |

`Combined`, the default, runs all three and adds:
- **Flash filter:** a change that returns to the previous picture within 4 frames is ignored at both of its edges, so
  strobes and flashes are not cuts.
- **Dissolve detector:** over a 12-frame window, the middle frame being the average of the window's end frames, which
  differ by ≥ 25, is a blend. The cut goes half way through the run of blends. Pans and zooms are not blends.
- **Hue/saturation stability:** hue only counts between saturated, lit pixels, saturation only between lit ones. Fades
  and grey pictures otherwise look like cuts.
- **Minimum shot length:** cuts closer than 15 frames keep the strongest (hard cut > fade > dissolve).

The output is `ShotCut{time (start of the new shot's first frame), frame, kind cut/fade/dissolve, confidence}`.

**Beat detection (`BeatDetection.h`)** decodes audio as mono 22,050 Hz floats:
1. Log-mel spectral flux: 40 bands, 100 frames a second, high-passed.
2. Tempo from the autocorrelation in 60–200 BPM, with a log-normal prior around 120 BPM. The twice-as-fast tempo wins
   when the half lag is as periodic, so 174 is not heard as 87.
3. Beats by dynamic programming after Ellis (2007).
4. End beats that are off the tempo or weak are trimmed (librosa's ½ RMS rule).
5. Each beat moves to the waveform attack near it (1 ms steps), so beats are sample-exact Ticks.
6. The tempo is fitted to the beats (least squares).
7. Downbeats: the place in the bar whose beats are loudest just after the attack, with low frequencies counting double.

`BeatTracker` is the interface (`DspBeatTracker`, id `dsp-1`). The learned Beat This! tracker (ONNX via ai_local) will
plug in under its own id later.

**Accuracy and speed** (tests in `tests/ai/analysis`):

| Material | Result |
|---|---|
| FFmpeg fixtures | Hard cuts exact at frames 50/100/150/200 of a 1080p H.264 file; the xfade dissolve over 50–75 found at 64; a fade through black at 56; two white flashes, no cut, and the real cut at 90 |
| 1080p shot detection speed | 26–35× realtime (software decoding ≈ 32×, CUDA readback ≈ 26×) |
| Click tracks at 90 / 120 / 128 / 174 BPM | Tempo exact to 0.001 BPM, beats within 0.2 ms of the clicks, accented downbeats found |
| Synthesized drum loop at 105 BPM | 105.001 BPM, beats within 3 ms |
| Beat analysis speed | About 1000× realtime |

**Cache.** Results are stored in the `AnalysisCache` (AI-2's), keyed by source fingerprint, container stream, analyser
(`shots` / `beats`), analyser version and a hash of the settings. A second request is instant and is shared by every
project using the file. The JSON forms are `wabcut.shots` and `wabcut.beats` v1 (`AnalysisJson.h`).

**Beat grid (model).** `Sequence::beatGrid` (schema 6) holds:
- the tempo in thousandths of a BPM (exact);
- a downbeat (`phase`);
- beats per bar;
- optional detected beats, for music that drifts.

`edit::setBeatGrid` sets it. `BeatGrid::beatsIn` / `nearestBeat` / `beatAtOrAfter` give beats with their place in the
bar. Timeline snapping uses grid beats as targets (`SnapOptions::beats`, up to a bar past the end).

**Commands** (typed, dry-run, one undo step, also MCP tools):

| Command | Library | What it does |
|---|---|---|
| `analysis.detectShots` | `wabcut_ai_analysis` | Shot markers in a clip (cyan "Shot n"), or the list for a media item |
| `clip.splitAtShots` | `wabcut_ai_analysis` | Razor at the cuts, linked partners too |
| `analysis.detectBeats` | `wabcut_ai_analysis` | Markers labelled 1-2-3-4 (downbeats red) and the beat grid; the clip's speed scales the tempo |
| `sequence.setBeatGrid` | built-in | Set from a BPM and phase, from tapped beats (least squares), or clear |
| `sequence.beats` | built-in | List beats |
| `edit.snapToBeats` | built-in | Trim clip edges to the nearest beat within a tolerance (a quarter beat by default); edges that cannot reach are reported |
| `edit.cutToBeats` | built-in | beat-synced assembly: clips' media (from their In) and bin items, one per group of N beats from the first beat after the playhead |

The analysis commands get their analyses through `AnalysisServices`, which hosts combine with the transcript services in
a `command::ServiceBundle`. `editingCommands()` serves them.

```
wabcut-cli analyze <media> [--shots] [--beats] [--json] [--detector combined|content|adaptive|threshold]
                   [--threshold <n>] [--min-bpm <n>] [--max-bpm <n>] [--cache <dir> | --no-cache]
```

**Sources:**
- PySceneDetect documentation, "Detection Methods" (ContentDetector, AdaptiveDetector, ThresholdDetector), BSD-3-Clause.
- D. P. W. Ellis, "Beat Tracking by Dynamic Programming", J. New Music Research 36(1), 2007.
- librosa's onset strength and beat trimming conventions (ISC); ideas only, no code.

## Smart search

Smart search finds the moments of a project's footage that show or say what a query describes ("a drone shot over the
coast", "where she mentions the budget", Thai too) and returns timecoded source ranges (AI-8, research C.8). It runs
entirely on this computer: the only network access is the one-time model download.

**Pieces.** `src/ai/search` builds `wabcut_ai_search` (pure C++, core preset) and `wabcut_ai_search_media` (full
builds). The model runs in the helper `src/ai_local/embed` (`wabcut-embed`, built with `WABCUT_WITH_ONNX`).

| Part | What it does |
|---|---|
| `Keyframes.h` | Shot-aware keyframes. Each C.5 shot gets one keyframe in its middle. A shot longer than 5 s is cut into equal stretches of at most 5 s, each with its own keyframe. A hit is the stretch its keyframe stands for. |
| `Embedding.h` | Vectors (L2-normalised; cosine = dot product) and their base64 float32 encoding. `resizeBilinear` is Pillow's bilinear filter, byte for byte, which is what `SiglipImageProcessor` uses. |
| `Tokenizer.h` | SigLIP 2's Gemma BPE from `tokenizer.json`, streamed (SAX) rather than parsed into a DOM. Text is lowercased (SigLIP 2 is trained on lowercased text), `" "`→`"▁"`, byte fallback, `<eos>`, padded to 64. It accepts only SigLIP 2's normalizer and pre-tokenizer. |
| `EmbedProtocol.h`, `HelperEmbedder.h` | NDJSON to `wabcut-embed`: `hello`, `load`, `embed_images` (a raw RGB batch file), `embed_text` and `shutdown`. The helper starts on first use and keeps its models loaded. A crash, a timeout, a protocol breach or a cancel kills it; that call fails cleanly and the next call restarts it. |
| `SearchIndex.h` | SQLite (`index.sqlite`): sources, keyframes (vectors as BLOBs) and transcript phrases, with an FTS5 table using `tokenize='trigram'`. hnswlib (`visual.hnsw`, inner product) is a cache of the vectors. Rebuilt when missing, damaged, out of date or not matching its SHA-256 in the database. |
| `TextQuery.h` | FTS5 query building (below). |
| `Ranking.h` | Hybrid ranking (below). |
| `FootageSearch.h` | Indexing (a job) and searching (a read). |
| `SearchCommands.h` | `search.footage` (Read) and `search.indexMedia` (MediaAnalysis). |
| `MediaSearchAccess.h` | Media access for full builds: fingerprints, C.5 shots, keyframes decoded with `media::VideoDecoder` at 384 px (sample aspect and rotation applied) and cached transcripts. `openFootageSearch()` is the editor's setup, shared by the app and the CLI. |

**Keyed by source fingerprint.** The index is shared by every project in the user cache (`<cache>/search/v1`). An
unchanged file is indexed once. An edited file has a new fingerprint and is indexed anew; the old entry is never
searched by the project and `prune()` reclaims it. Keyframe embeddings are also stored in the AnalysisCache under
(`search-keyframes`, fingerprint, video stream, model version, settings hash). A wiped index, or another machine
sharing the cache, therefore restores the embeddings without decoding a frame or running the model. The model version
`siglip2-base-patch16-224/q8-<vision>-<text>/pp1` covers the weights' digests and the preprocessing. An index opened
with another model version drops its vectors and keeps the phrases. A transcript is re-indexed when its key changes
(the words, their times and the language).

**Searching.** A search runs over the media the filters allow: clip name, file date, duration and specific media
items.
- **Visual:** the query's text vector against the keyframe vectors. Below 20,000 allowed keyframes this is an exact
  scan; above that it uses the filtered HNSW graph. Matches under a cosine of 0.06 are dropped. SigLIP 2's own
  calibration (logit scale 112.7, bias −16.77) puts a 50 % match at 0.149 and a 1 % match at 0.108, and unrelated
  pictures near 0.
- **Transcript:** the query's terms against the phrases (FTS5, bm25).
- **Fusion:** the two lists are merged by Reciprocal Rank Fusion (k = 60): each candidate scores `1/(60 + rank)` in
  its own list, so cosines and bm25 never need a common scale. A phrase that overlaps a visual hit of the same file
  joins it: the scores add and the hit spans both. "Seen and heard" therefore ranks first.

A hit carries the media, its source range, a thumbnail time (the keyframe, or the first matched word), the score, the
snippet and the matched words.

**Thai and other scripts.** The trigram tokenizer matches any substring of three or more characters, so no word
segmenter is needed.
- Phrases of languages written without spaces (th, lo, km, my, zh, ja, ...) are indexed twice: with spaces between the
  words and joined without them. "แมววิ่ง" therefore finds the words แมว + วิ่ง.
- Terms of one or two characters (รถ) cannot use the trigram index. They are matched with `LIKE '%term%'` instead,
  and only when the query has no longer term. This is the documented fallback.
- Queries are case-insensitive.

**ONNX Runtime without a source build.** The overlay port `cmake/ports/onnxruntime-prebuilt` installs Microsoft's
official ONNX Runtime 1.24.4 binaries (MIT), pinned by vcpkg's SHA-512 and by Microsoft's published SHA-256. It
requires vcpkg feature `ai-onnx`, enabled by the CMake option `WABCUT_WITH_ONNX`. It defaults to ON wherever Microsoft
publishes binaries, which is everywhere except Intel Macs: the download is small and the version pinned.

| Platform | Package | Providers | Download | Installed |
|---|---|---|---|---|
| Windows x64/arm64 | NuGet `Microsoft.ML.OnnxRuntime.DirectML` (the last DirectML release); SHA-512 = NuGet's catalog hash | DirectML + CPU | 12.5 MB | `onnxruntime.dll` 17.3 MB + `onnxruntime_providers_shared.dll` 22 KB |
| Linux x64 | GitHub `onnxruntime-linux-x64-1.24.4.tgz` | CPU | 8.2 MB | |
| Linux aarch64 | GitHub `onnxruntime-linux-aarch64-1.24.4.tgz` | CPU | 7.2 MB | |
| macOS arm64 | GitHub `onnxruntime-osx-arm64-1.24.4.tgz` | CPU + Core ML | 30.9 MB | |

- The DirectML runtime itself (the `Microsoft.AI.DirectML` redistributable) is proprietary and is not shipped.
  `onnxruntime.dll` uses Windows' own `System32\DirectML.dll`, present since Windows 10 1903.
- `wabcut-embed` tries DirectML (`device` `auto`/`gpu`). If DirectML cannot create the session, or fails while
  running, the helper falls back to the CPU provider.
- Telemetry: `wabcut-embed` calls `Ort::Env::DisableTelemetryEvents()` right after creating the environment, before
  any session exists. Official Windows builds otherwise send session events to Microsoft's ETW provider.

**The model.** SigLIP 2 base, patch 16, 224 px (`google/siglip2-base-patch16-224`, Apache-2.0, with its Gemma
tokenizer vocabulary), in onnx-community's 8-bit ONNX export (revision `ba1f3b08`).

| File | Size | Notes |
|---|---|---|
| vision encoder | 94.6 MB | `pixel_values` [N,3,224,224] → `pooler_output` [N,768] |
| text encoder | 283.4 MB | `input_ids` [N,64] → `pooler_output` [N,768] |
| `tokenizer.json` | 34.4 MB | |

- They are ModelStore entries (`siglip2-base-vision`, `-text`, `-tokenizer`). Each is SHA-256 verified and comes with
  the license card the app shows before downloading.
- Preprocessing follows `SiglipImageProcessor`: a 224 × 224 bilinear resize (Pillow), then (x/255 − 0.5)/0.5 in NCHW.
- Without the helper or the model, smart search still indexes and searches transcripts, and says that visual search
  is off and why.

**Commands and the assistant.**
- `search.footage(query, limit, media, name, from, to, minDuration, maxDuration, visual, transcript)` is a read: it
  changes nothing and is open to read-only clients. It returns `hits[]` with `media`, `sourceIn`/`sourceOut` (ticks,
  ready for `clip.insert`), `start`/`end` (s), `score`, `similarity`, `transcript`, `snippet` and `matchedWords`.
- `search.indexMedia(media?)` is a MediaAnalysis job: it reports progress through the command checkpoints and skips
  unchanged files.
- wabcut-cli (`cmd`, `mcp`) opens the search on first use (`src/cli/Search.cpp`).
- The editing assistant gets `search.footage` as a tool, because it is a Read command of the catalog. Its services
  include the Library's search (`AgentController::services`).

**App.** The Library has a search bar with a **Smart** toggle (`SmartSearchBar.qml`).
- Smart off: typing filters the library by name.
- Smart on: the query goes to `SmartSearch` (`SearchBridge`), and the grid is replaced by `SearchResultsPanel.qml`.
  Turning Smart on indexes, in the background, whatever is not indexed yet.
- Searches run on a worker thread; the newest query wins.
- Each result card (`SearchHitCard.qml`) shows the matched frame (`image://wabcut-search/<media>/<ticks>`, decoded off
  the GUI thread), the source-range badge, the clip name, sparkles/captions icons for "seen"/"said", and the snippet
  with the matched words in bold.
- Dragging a card onto the timeline (`application/x-wabcut-search-hit`) edits in just that range, as one undo step:
  it overwrites (Ctrl/Cmd inserts) and snapping applies, like library drops (`SmartSearch.dropHit` →
  `editor::MediaDrop::sourceRange`).
- The pill above the results shows "Indexing 3/12" with progress, then "Indexed 12/12"; click it to index or stop.
- When the helper is built but the model is missing, the panel offers the download (412 MB, once, with the license
  card), through the AI network stack. Offline mode refuses it.

**Performance** (this workstation, RelWithDebInfo).
- **Searching** (`[benchmark]` test; 1,000 indexed shots of 768 dimensions and 2,000 phrases over 50 files): a median
  of 6.9 ms (p95 7.4 ms). That includes the FTS5 query, the exact vector scan, fusion and hit mapping. Encoding the
  query text adds 11 ms (CPU) to 16 ms (DirectML), so a search is about 20–25 ms from typing pause to results.
- **The model** (the `[model]` smoke test, the 8-bit models):
  - DirectML: 1.3 s to start the helper, load the image model and embed the first pictures; 41 ms per keyframe when
    indexing (batches of 16).
  - CPU (all cores): 0.43 s and 49 ms per keyframe.
  - `auto` therefore uses DirectML, which also leaves the CPU free for editing. The app runs the helper with half the
    cores when it falls back to the CPU.

**Tests.** None of them download a model.
- `tests/ai/search` (the `ai` suite) runs on fixture embeddings, a colour per shot:
  - resampling against Pillow references;
  - the tokenizer: a hand-made `tokenizer.json`, plus the real one against the tokenizers library's ids when
    `WABCUT_TEST_SIGLIP_DIR` is set;
  - the protocol bounds and keyframe planning;
  - FTS query building with Thai, Thai search across words, and the LIKE fallback;
  - persistence, invalidation by fingerprint and by model version, damaged database and graph files, and the exact
    scan against the graph;
  - RRF fusion, `FootageSearch` end to end (filters, re-indexing only what changed, embeddings restored from the
    AnalysisCache, transcripts without a model, cancellation), the commands through the engine, and the latency
    benchmark;
  - `HelperEmbedder` against a scripted helper (`fake_embed`): crash, crash-once and restart, hang (timeout), cancel,
    garbage, wrong dimension or count, wrong protocol, a missing model or executable.
- An optional smoke test runs the real SigLIP 2 in `wabcut-embed` (red, green and blue pictures against three
  captions) when `WABCUT_TEST_SIGLIP_DIR` names the downloaded files.
- `test_search_ui.cpp` (the `ai_ui` suite) drives the Library on the real Main.qml and saves
  `smart-search.png` / `smart-search-library.png`.

## Auto reframe

AI-9 makes a sequence again at another aspect (9:16 for shorts, 1:1, 4:5, 16:9) with its subjects kept in frame. Nothing is
rendered: every video clip of the new sequence gets ordinary Position X/Y and Scale keyframes that the editor can
change like any other.

### Subject analysis (`wabcut/ai/vision`)
| Piece | What it does |
|---|---|
| `Detection.h` | The networks' input and output in pure C++, tested on fixture tensors. **YuNet** (OpenCV Zoo `face_detection_yunet_2023mar`, MIT) takes a 640 × 640 BGR 0..255 picture letterboxed into its top-left corner over black. Per stride 8/16/32 it outputs a class and an objectness score (score = √(cls·obj)), a box (grid offset, log size) and five landmarks, decoded like OpenCV's `FaceDetectorYN`. **YOLOX-Tiny** (Megvii 0.1.1rc0, Apache-2.0) takes a 416 × 416 BGR 0..255 picture over grey 114, *not* normalised: the released weights find nothing with the mean/std normalisation the demo code of that tag still applies (checked against onnxruntime on a photo). Its 3549 × 85 raw predictions are decoded like `demo_postprocess` (score = obj · cls), and only the person class is kept. Greedy per-class NMS follows. |
| `Tracker.h` | A ByteTrack-style tracker (the method, reimplemented): it uses an alpha-beta predictor and matches high-score detections first, then lets low-score ones extend leftover tracks. New tracks are tentative until their second hit, and a confirmed track survives 15 missed samples. Matching is greedy IoU, so it is deterministic. Tracks reset at every shot cut. |
| `Saliency.h` | The fallback when nothing is detected: spectral-residual saliency (Hou & Zhang 2007) on a 64-pixel grey picture, plus motion energy. Each sample gets one salient box; a flat picture gets none. |
| `Subjects.h` | `analyzeSubjects()` samples each C.5 shot at 5 fps, or 10 fps right after the picture moved (mean grey change ≥ 0.035). It batches pictures to the detector and tracks faces and people per shot. Each face's lower-half change between samples is its *activity*, the speaking cue. The result (`SubjectAnalysis`) is compact JSON, cached in the AnalysisCache by source fingerprint, stream, detector version (`yunet-2023mar-8f2383e4+yolox-tiny-427cc366/v1`) and sampling settings. `SubjectProvider::cached()` reads a cache entry without ever analysing. |
| `HelperDetector.h` | The detectors run in **wabcut-embed**, which also serves smart search. `EmbedHelper` is that process's shared session: hello and protocol check, deadlines, crash/garbage/cancel handling, restarts. The protocol gained `detect` / `detections` (≤ 256 rows per picture, 15-number face rows, 6-number object rows) and detector paths in `load`. The helper runs DirectML when it can and falls back to the CPU per session. |
| `MediaSubjects.h` | Media-backed (full builds): frames at 640 px from `search::VideoPictureReader` (sample aspect and rotation applied, hardware decoding), shots from the cached `MediaAnalyzer`, `openSubjectDetector()` (the downloaded detectors, or salient regions only, with the reason). |

The models are in the manifest as `ModelKind::Detection`: `yunet-2023mar` (232,589 B, SHA-256 `8f2383e4…52fa4`, Hugging Face `opencv/face_detection_yunet` at a pinned revision) and `yolox-tiny` (20,219,662 B, `427cc366…b0f7`, the GitHub release asset). Each is downloaded on first use after its license card, verified by SHA-256, and listed in THIRD-PARTY-NOTICES.

### The virtual camera (`reframe/CameraPath.h`)
Per shot, the window centre follows an AutoFlip-style L1-optimal path on a uniform grid (10 fps). It minimises:

  Σ wᵢ·dt·|xᵢ − dᵢ| + λ₁ Σ|Δx| + (λ₂/dt) Σ|Δ²x| + (λ₃/dt²) Σ|Δ³x|,  subject to lᵢ ≤ xᵢ ≤ uᵢ

The L1 terms make the path piecewise still, then constant-velocity, then constant-acceleration. That is a camera operator's pan, not a follow that jitters. The data term pulls the window towards the subject. The hard bounds are the *safe region*: the subject box plus a margin, within the picture.

It is solved by over-relaxed ADMM on a banded system (bandwidth 3, Cholesky factored once) with soft-thresholds. Each block's step is set to λ/τ, where τ is the size its differences take (1% of the picture per sample for the first difference, smaller for higher ones). That converges in about 300 iterations; uniform steps needed about 12,000. The smoothness slider scales λ by 10^(2s−1).

`fitCurve()` then writes sparse keyframes. It uses Bezier keys whose in/out tangents are the path's one-sided slopes, and adds the worst sample until the model's own `Curve::valueAt` stays within the tolerance (1 px) and inside the bounds at every sample. A still path becomes one key, so a still subject gives no motion at all. The last key of each shot is Hold and the next shot's first key sits exactly on the cut, so the framing jumps on the cut's frame.

### Planning and the sequence (`reframe/Reframe.h`)
The window is the largest one of the target aspect: for 16:9 → 9:16, full height and 31.6% of the width. Modes:
- **follow:** the shot's main face track (present longest, biggest, surest), else the head of its main person, else the salient region (soft, no bounds).
- **faces:** the union of the shot's steady faces.
- **speaker:** the face with the most mouth activity, smoothed over ±0.6 s. It switches only when another face is 1.3× more active for 0.6 s.
- **anchor:** a fixed point.

Where the subjects cannot fit, **crop** keeps the window full. **letterbox** zooms out per shot until they fit. **blur** zooms out the same way and adds a "Blur fill" track below: the same clips covering the frame with a Gaussian blur.

`applyPlan()` writes Position = −scale·R·(centre − picture centre) in display pixels and Scale = frame height / window height, and sets Anchor to 0. Rotation is kept, and keyframe times are media time, like the editor's. `duplicateSequence()` renews every id (tracks, clips, link groups, transitions, effect instances, caption tracks), and `reframeSequence()` records `Sequence::derivedFrom` (`reframe/1` plus the parameters as JSON). That record is optional in the project file, bounded (64 B generator, 16 KiB params) and part of `==`/diff. A source deleted since is not an error: `reframe.rerun` reports "source missing".

### Commands
| Command | Capability | Notes |
|---|---|---|
| `analysis.subjects` | MediaAnalysis | faces/people/salient over a media item; cached; progress through checkpoints |
| `reframe.createSequence` | EditProject | aspect, mode, fit, smoothness, anchor, margin, size, name; one undo step; dryRun returns the preview state and, per clip, the window path and subjects sampled for drawing |
| `reframe.rerun` | EditProject | makes a reframed sequence again from its (edited) source with its recorded or new parameters, in place (id, name, bin kept) |

Linked live updates (a reframe following its source as it is edited) are a follow-up; `reframe.rerun` is the manual form.

### App
The sequence menu (top bar) has **Reframe…**, which opens the self-contained `ReframeDialog`. It offers aspect presets, Subject / All faces / Speaker, Crop / Letterbox / Blur fill and a smoothness slider. Its preview draws source frames (`image://wabcut-frames`) dimmed outside the crop window, with the subject box, and a scrubber steps through each clip's path. Subject analysis runs as a job with a progress pill. **Create sequence** commits the dry run and switches to the new sequence. Without the detectors the dialog says so, shows their license cards, and offers the 21 MB download.

### Measured (RTX 3060, NVDEC decoding; a 20 s 1080p 30 fps clip panning over ~45 faces)
| Detectors on | Analysis time | Real time | Samples/s | Detectors alone (1280 × 719 photo) |
|---|---|---|---|---|
| CPU (ONNX Runtime) | 16.5 s | 1.21× | 6.1 | 4.7 pictures/s (172 ms) |
| DirectML | 4.2 s | 4.71× | 23.6 | 30.2 pictures/s (18.7 ms) |

On the photo both find 57 faces and 9 people. `wabcut-cli subjects <file> --device cpu|auto` measures a file. The optional `[render]` test reframes the clip with the real detectors and renders a 9:16 frame through `wabcut-cli render`.

## Shorts finder

`shorts.find` returns the 20–90 s moments of a long sequence that stand on their own as vertical shorts. They are ranked locally and explained.

1. **Sentences** come from the sequence transcript: they end at punctuation (Thai and CJK forms included), at a 0.7 s pause, at a speaker change, or at 15 s.
2. **Topics** are found TextTiling-style. Cohesion between blocks of three sentences uses TF-IDF cosine (or the host's text embedder), and the deepest valleys become boundaries.
3. **Candidates** are every run of whole sentences of the right length. Each is scored from 0 to 1 on:
   - *structure:* starts and ends a topic;
   - *hook:* a question, hook phrases or numbers, dense speech in the first 3 s;
   - *topic:* no boundary inside, cohesive;
   - *energy:* RMS above the sequence's level; loud pauses count as laughter or applause;
   - *visual:* faces, shot rhythm and motion, from cached subject analyses;
   - *clean:* filler and silence density, from the transcript review;
   - *length:* close to 45 s;
   - *query:* matches "about …".

   Text features rank every candidate. The media probes look only at the best 40, so 78 minutes of transcript (8,295 words, 900 sentences, 11,884 candidates) takes about 12 ms.
4. The best non-overlapping candidates (≤ 20% overlap) are kept. Each is snapped outwards to a cut (a clip boundary or shot cut) within 0.5 s without taking in a neighbouring word, else padded by 0.15 s and frame-aligned. Each gets a title: the matching or most telling sentence near its start, without leading fillers.

**Cloud re-rank (optional, bring your own key).** `rerank: true` sends the best 10 candidates' *transcript text only* (no file names, media or pictures) to the user's chat model through `AiPlatform::chat`. That path enforces consent, the offline and no-cloud locks and the spend cap, and records the call in the request log. The model answers with JSON scores, titles and reasons, blended 50/50 with the local score. A no-cloud project skips it before any call. Any failure (no consent, offline, a bad answer) leaves the local ranking and says so.

**`shorts.create`** (EditProject) makes one new sequence per chosen short (at most 10 per call). It cuts the range out of the source with `cutSequence` (clips trimmed, transitions and captions inside kept, lonely link partners unlinked), then runs, on a nested engine:
- optional `transcript.cleanup`;
- the reframe (default: speaker);
- `captions.generate` (24 characters per line on 9:16);
- the title as a hook on its own top track for its first 3 s;
- optional `audio.normalize` to −14 LUFS.

All the shorts of one call are one undo step, and each records `derivedFrom` (`shorts/1`, its range and options). The Shorts panel's **Export all** queues `vertical-1080x1920` exports (H.264, or HEVC through the `videoCodec` override).

**The assistant** may use `reframe.createSequence` and `shorts.create` without the EditProject capability, because they only add sequences. Other project commands stay out of reach, and so does `reframe.rerun`. A request may make at most 10 sequences. Verification adds two checks: *others* (no other sequence or media item changed or was removed) and *newSequences*. So "make 5 shorts from this podcast about the pricing discussion" runs `shorts.find` (query: pricing) and then `shorts.create`, staged and verified.

**App.** The **Shorts** browser panel (Window menu) has the query, a count, the re-rank toggle (when a cloud model is set up and the project is not no-cloud) and the create options. Each candidate is a card with:
- a strip of six frames, looping (still under reduced motion);
- range and length;
- score bars;
- reasons;
- an editable title;
- Include and Create.

**Create N shorts** stages them, **Apply** commits them, and **Export all** queues them.

### Tests (`ai` suite, core preset; `ai_ui` for the app)
- `test_vision_detection.cpp`: letterboxing, both input tensors, YuNet and YOLOX decoding on fixture tensors, NMS, the tracker (ids through occlusion, low-score extension, reset at cuts, maxLost), saliency, the detect protocol.
- `test_vision_subjects.cpp`: the sampler on scripted videos (per-shot sampling, denser on motion, track reset at cuts, speaking activity, saliency only, cancel, the cache JSON); `HelperDetector` against the scripted `fake_embed` (colour blocks are faces, people and cars), with crash, crash-once and restart, hang, cancel, garbage, malformed rows, wrong counts, protocol, a missing model or executable; the pinned, licensed manifest entries.
- `test_camera_path.cpp`: a still subject gives a still camera (one key), the subject always stays inside the safe region (path and keyframes), smoothness bounds the travel and ignores jitter, still / steady / still, convergence, Hold at cuts.
- `test_reframe.cpp`: 9:16 with sparse keys and a jump exactly at the cut, no motion for a still subject, centred without analysis, the anchor, crop / letterbox / blur fill, speaker switching, the id-renewing copy (links, transitions, captions), options and sizes, and the commands (dryRun preview, one undo step, rerun after a trim and with new options, source missing, permissions).
- `test_derived_from.cpp`: optional and byte-identical when absent, equality and diff, round trip, bounds on load and in validate.
- `test_shorts.cpp`: sentences (Thai too) and topics, a deterministic, explained ranking (the hook topic first, the messy one least clean, no overlap), the query, snapping to a cut, energy and visual signals, the re-rank (order, titles, text only, a bad answer), consent / no-cloud through `AiPlatform` (no call without consent or with the lock; logged otherwise), `shorts.create` (cut, 1080 × 1920, captions, hook title, reframed; one undo; cleanup; the cap; permissions), `cutSequence`, and the 78-minute benchmark.
- `test_shorts_agent.cpp`: the assistant makes two shorts, verified and applied without EditProject; the cap; other project commands refused; the *others* check.
- `test_vision_model.cpp` (optional): with `WABCUT_TEST_DETECT_DIR` / `WABCUT_TEST_DETECT_IMAGE` / `WABCUT_TEST_SUBJECT_VIDEO`, the real YuNet and YOLOX on CPU and DirectML (SHA-256 checked, faces, people, throughput).
- `test_shorts_ui.cpp` (`ai_ui`): the Shorts panel and the Reframe dialog on the real Main.qml; saves `shorts-panel.png`, `shorts-window.png` and `reframe-dialog.png`.

## Creator superpowers (AI-11)

Three features for people who record themselves: a **rough cut from a script**, **retake comparison**, and **auto zoom** for screen recordings. All of them work on this computer from transcripts and frames, stage one edit with a preview, and apply it as one undo step. Libraries: `wabcut_ai_roughcut` (`src/ai/roughcut`) and `wabcut_ai_autozoom` (`src/ai/autozoom`), with media services in `wabcut_ai_roughcut_media` / `wabcut_ai_autozoom_media`.

### Text matching (`roughcut/TextMatch.h`)
- **Units.** Spaced scripts become words: lower-cased, numbers as digits ("ten" = "10"), English contractions as their words ("we're" = "we are", "don't" = "do not"; possessives stay one word). Thai, Lao, Khmer, Myanmar and CJK become character clusters (a base letter with its vowels and tone marks), so a Thai script without spaces matches a Thai transcript however the recogniser segmented it.
- **Similarity.** Exact, fuzzy for words of 4+ letters (edit distance, stems such as "edit"/"editing"), and lenient on a missed Thai mark (same base letter).
- **Alignment.** A semi-global alignment finds the line inside a stretch of transcript: words dropped or added cost a little, fillers in the transcript cost nothing. Its quality is coverage × precision (0..1).

### Rough cut from a script (`roughcut/Script.h`, `RoughCut.h`)
1. **The script** is pasted text or a `.txt` / `.md` file (≤ 256 KiB, ≤ 400 lines). Markdown headings are sections (a marker each); bullets are outline beats; `Name:` speaker labels are dropped; `[B-roll: …]` and `(cut to …)` lines are cues for the line before; prose is split into sentences; Thai lines stay whole.
2. **Retrieval.** Every transcript sentence of the chosen media is indexed by IDF-weighted features. For each line, the best-matching sentences seed windows of up to four consecutive sentences, scored by the exact cosine of their summed vectors (two takes of a line in one window score no higher than one). Windows that overlap better ones keep only the sentences those do not hold.
3. **Alignment.** The line is aligned inside each window (with three words of slack each side). What is left of the window on either side is searched again, because takes said one after another can share a window. Fillers at the edges are trimmed off the take. Takes below `minMatch` (0.45) go; alternates within 0.3 of the best match stay (at most 4).
4. **Judging takes** (`assessSpan`, `scoreTake`): 0.40 match, 0.20 clean (fillers, false starts, pauses of 0.7 s or more, and a filler or a restart such as "it cuts a, it cuts a…" right before the words), 0.15 audio (level against the median take, signal over the noise floor, clipping, from `AudioEnergyService`), 0.15 complete sentences (starts after a sentence end or a pause, ends at one), 0.05 recogniser confidence, 0.05 pace (close to the speaker's median). `prefer` picks *best* (score), *cleanest*, *first* or *last* recorded. Confidence is 0.8 × match + 0.2 × the margin over the runner-up.
5. **Assembly.** A new sequence (the frame size and rate of `fromSequence`, else of the first take's video) gets the chosen takes in script order, linked V1/A1, frame-aligned, trimmed to the words with 0.15 s handles that never reach into a neighbouring word. Section markers, optional B-roll from smart search on V2 ("B-roll (suggested)") and optional `transcript.cleanup` follow. The plan (every beat's takes) is recorded in `derivedFrom` (`roughcut/1`, ≤ 16 KiB), so `roughcut.swapTake` can swap a take later.
6. **Outline bullets** ("- pricing") may share no word with what was said. With the host's text encoder, the sentences closest in meaning are candidates too, and a match can be carried by meaning alone. Optionally (`interpret`), the user's **cloud chat model** places loose bullets first: transcript *text only*, through `AiPlatform::chat`, so consent, the offline and no-cloud locks and the spend cap apply and the call is logged. Without it everything is local.

### Retake comparison (`roughcut/Takes.h`)
`findTakes` groups repeated attempts of a line: sentences of the sequence (and, by default, of the whole recordings it uses, plus any `media` given) that align with each other (similarity ≥ 0.7, ≥ 3 words, within 180 s in one recording; across recordings by default). Pairs are verified both ways. A short fragment that starts a longer attempt ("So the new…") is a false start of it. Substituted content words keep look-alike lines apart ("the first version yesterday" vs "the second version today"). Groups are complete-link. Each take has its words, a word diff against the best take, the quality figures above (fillers, false starts, pauses, clipping, level over the noise floor) and its pace. `planUseTake` / `applyTakeEdits` keep the chosen take, remove the others from the sequence (or replace the take on the timeline with one from a recording), keep links and handles, and restore In/Out marks; "keep best takes" does every improvable group as one batch with a review list.

### Auto zoom for screen recordings (`autozoom/Activity.h`, `AutoZoom.h`)
- **Cursor sidecar** (preferred when present): `<file>.cursor.json` or `<file>.cursor.csv` next to the recording, or its content passed as `cursor`.
  - JSON: `{"format":"wabcut.cursor","version":1,"width":1920,"height":1080,"offset":0,"events":[{"t":1.25,"x":640,"y":360,"type":"move|click|down|up|key|scroll"}]}`. Positions are pixels of `width` × `height`, or 0..1 without a size; `t` is seconds, plus `offset`; at most 2,000,000 events.
  - CSV: `t,x,y[,event]`, an optional header row; `,` `;` or tab.
  - OBS writes no cursor log of its own, so OBS recordings use detection (or a log from a cursor-logging plug-in converted to this format).
- **Local detection** (no sidecar): frames at 10 fps, reduced to 640 px wide luma, changed 8×8 blocks grouped into blobs; blocks that change most of the time (video, animation) are ignored. The cursor is followed as a pair of small changes (where it was, where it is now). A UI change near the resting cursor within 0.7 s is a click (one per 0.6 s); small changes again and again in one place are typing; a change of 40% of the picture is a scene change.
- **The path.** Events become segments (nearby in time and space; a click, typing, a scroll, or the cursor resting). The zoom fits the region with a margin (≤ `maxZoom`, default ×2), starts 0.4 s before the first event and holds `hold` (1.5 s) after the last. The AI-9 L1 camera-path optimiser (`reframe::solveCameraPath`) solves the zoom, then the window centre inside the bounds that zoom allows; its data weights make eases last about `transition` (0.6 s), and idle stretches (2.5 s) pull it back to ×1. The first sample is always ×1.
- **Keyframes.** Editable Position/Scale keys: Scale = zoom × the clip's own scale, Position = zoom × (base position − window centre), at the starts and ends of moves (`easeInOut` or `linear`) or a simplified path (`smooth`). Moves are at least `transition` long. Earlier auto-zoom keys are replaced and the clip's own placement is recovered from the first key, so a rerun is idempotent. The frame never shows outside the picture.
- **Out of scope:** cursor smoothing, cursor highlight and click ripples (they need a cursor-free recording, i.e. the recorder's cooperation).

### Commands, MCP and the assistant
| Command | Capability | What it does |
|---|---|---|
| `roughcut.fromScript` | EditProject (additive) | text or file, media / fromSequence, split, prefer, minMatch, cleanup, broll, markers, interpret, name; returns the alignment report |
| `roughcut.swapTake` | EditSequence | sequence, beat, take (1-based, as the report numbers them) |
| `takes.find` | Read | sequence, limit, minSimilarity, maxGap, acrossRecordings, includeSourceMedia, media, meaning |
| `takes.use` | EditSequence | group + take, or `best: true` for every improvable group |
| `autozoom.apply` | EditSequence | clip, maxZoom, hold, idle, transition, easing, smoothness, clicks, typing, dwell, detect (auto / sidecar / frames), fps, cursor, sidecar, preview |

`file` and `sidecar` (paths) are read only where the host allows it (`wabcut-cli cmd`). MCP clients pass content (`text`, `cursor`) instead. The assistant gets the same tools; it may run `roughcut.fromScript` without EditProject because it only adds a sequence, and it may then edit the sequence it made in the same request. So "make a first cut from this script and pick the cleanest takes" runs `roughcut.fromScript` (prefer cleanest), then `roughcut.swapTake` or `takes.use` on the new cut, staged, verified and applied as one undo step.

### App
- **Rough cut sheet** (`qml/panels/creator/RoughCutSheet.qml`, Sequence menu): paste or open a script, choose the scope and the take preference, clean-up, B-roll, and the cloud interpretation (only offered with a set-up model and never for a no-cloud project). **Make the cut** previews; the report lists every line with its chosen take, confidence and alternates; clicking an alternate swaps it (a staged edit); **Create sequence** applies and opens it.
- **Takes panel** (`qml/panels/creator/TakesPanel.qml`, Window menu): groups with the takes' words, the diff against the best (added words underlined, missing ones struck through), quality chips and pace. **Play** auditions a take in place (timeline takes in the Program Monitor, others in the Source Monitor), **Use this take** stages one edit, **Keep best takes** stages them all with a review list; the ghost shows on the timeline.
- **Auto zoom dialog** (`qml/dialogs/AutoZoomDialog.qml`, clip menu): the zoom window over the recording's frames, the activity near the playhead, the zoom over time (scrub or click), max zoom, hold, motion, and what to zoom in on. **Add zoom** applies.
- The singletons are `RoughCut`, `Takes` and `AutoZoom` (`src/app/src/bridge/CreatorBridge.*`). Each runs its command on a scratch copy and commits only on apply, guarded by the revision it was made at.

### Measured (2026-10-12, this machine, `ai_creator` suite, Release)
- Rough cut of 9,302 words (87 min of speech, 2 recordings) from a 40-line script: 40/40 lines on the right clean take; corpus + plan 47–75 ms (plan alone 38–63 ms), against a 2 s budget.
- English 30-line fixture (retakes, fillers, asides, a word missing from every fifth line): 30/30 on a clean take of the right line. Thai fixture (script without spaces, transcript in recogniser words): 6/6.
- Retake grouping fixture: precision 1.0, recall 1.0 (6 of 6 true pairs).
- Auto zoom analysis at 1080p: 220–320 frames/s (reduction to 640 px + detection, 3–4.5 ms a frame), so a recording is analysed many times faster than real time at 10 fps.
- The zoom path on the synthetic recording: ×2 on the button and the text field, back to ×1 when idle, max zoom speed 2.5/s, max acceleration 15.7/s², never outside the frame, identical on rerun.

### Limits
- Outline bullets match by meaning only with a real sentence encoder; the smart-search SigLIP text tower is an image-text encoder and a weak paraphrase model, so loose outlines are best placed with `interpret` (cloud, opt-in) or written closer to what was said.
- Detection finds clicks from the UI's reaction; a click that changes nothing is invisible without a sidecar. Busy backgrounds (video playing in the recording) are ignored as background motion.
- The local assistant (AI-10) is not used here; it is refused below its RAM floor (6.4 GiB free; 4.4–5.2 GiB were free on this machine). Live cloud reliability for `interpret` is unmeasured.

### Tests (`ai_creator` suite; `ai_ui` for the app)
- `creator/test_text_match.cpp`: units (contractions, numbers, Thai clusters), similarity, alignment with fillers and dropped words, feature bags, the word diff, script parsing.
- `creator/test_roughcut.cpp`: every beat found and the cleanest take chosen with alternates; deterministic plans and preferences; the assembled cut (order, trims with handles, linked V/A, markers, the record); the commands (dry run, one undo step, swap, permissions, files where allowed, clean-up); Thai; outline by meaning; the guarded cloud interpretation (consent, no-cloud, text only); the 30-line accuracy fixture and the 60-minute benchmark.
- `creator/test_takes.cpp`: grouping precision and recall, look-alike lines apart, diffs and quality, "use this take" and "keep best takes" (heard once, links kept, one undo step).
- `creator/test_autozoom.cpp`: sidecars (JSON, CSV, bad input), detection on the synthetic recording, the zoom path (targets in view, smoothness bounds, idle zoom-out, inside the frame, idempotent rerun), the command (dry run with preview, sidecar, refusals), the analysis throughput.
- `creator/test_creator_agent.cpp`: the assistant makes a cut and swaps a take, verified and applied as one undo step; it may not read files or edit sequences it did not make.
- `test_creator_ui.cpp` (`ai_ui`): the sheet, the panel and the dialog; saves `rough-cut-report.png`, `takes-panel.png` and `auto-zoom-preview.png`.

## Going global (AI-12)

Caption translation, AI voice-over (dubbing) and script narration, so one edit can be published in many languages. Thai is first-class: Thai captions break between words, read at Thai speed (17 characters a second, combining marks not counted), get the "Thai-friendly" style, and Thai has a local translation model and Thai-speaking voices. Libraries: `wabcut_ai_translate` and `wabcut_ai_mt` (`src/ai/translate`), `wabcut_ai_dub` and `wabcut_ai_dub_media` (`src/ai/dub`), the `translate_text` job of `wabcut-embed` (`src/ai_local/embed/TranslateModels.*`) and the three voice adapters (`src/ai_net/src/SpeechSynths.cpp`). Prices, model ids, voices and API shapes below are **as of 2026-10-12**.

### Caption translation (`translate/CaptionTranslation.h`, `Segmentation.h`, `captions.translate`)
1. **Source text.** The corrections layer applies first (`transcript.correctWord`), and every word inside an audio redaction reads `[bleep]`. A cue made from the transcript takes the corrected text whole; an edited cue gets each corrected or redacted word replaced where its recognised spelling appears.
2. **`[bleep]` is never content.** A cloud model gets each cue whole with the bleeps as markers and must answer with exactly as many (the system prompt says so). A cue whose answer lost or added one, or came back empty, is translated again piece by piece (the text between bleeps). A local model only ever gets the pieces. So a translation can never put the redacted word back.
3. **Translators.** *This computer*: OPUS-MT (below), nothing leaves the machine. *Your AI*: the user's chat model through `AiPlatform::chat` (consent, the spend cap, the offline and no-cloud locks, the request log). It gets caption text only, as JSON `{"entries":[{"id","text"}]}` with a structured-output schema, plus a context line (what the text is, the tone, the glossary, the line limits). A **glossary** (≤ 200 terms; a term mapped to itself stays untranslated) and a **tone** (neutral, casual, formal, playful) apply to cloud translations. Terms a translation missed are reported, not silently fixed. `provider` is `auto` (local where the pair's model is installed, else the cloud unless the project is no-cloud), `local` or `cloud`. `estimate: true` prices a cloud run batch by batch without sending anything.
4. **Re-segmentation to the target's reading speed** (`resegment`). Each translated cue keeps its timing when it reads in time and fits its lines. Otherwise, in order: (1) extend into the neighbouring gaps (≤ 0.5 s past the end, ≤ 0.25 s before the start, two frames between cues, ≤ 7 s); (2) merge with a neighbour of the same speaker less than a second away when the two read in time together, or move words across the boundary ("rebalance"); (3) break into lines with the host's `LineBreaker` (Thai, Lao, Khmer, Burmese between words: Qt's `QTextBoundaryFinder` in the app, Uniscribe in the CLI on Windows) and split what does not fit two lines into more cues, each timed by its share of the text; (4) extend cues shorter than 20 frames. What still reads too fast is kept and flagged; nothing is dropped.
5. **Reading-speed limits** (adult, from the Netflix timed-text style guides, checked 2026-10-12 for English, Thai and Japanese; the others use the guides' common values): English 20 cps / 42 per line; Thai 17 cps / 35 per line (vowel and tone marks above and below not counted); Japanese 4 cps / 13 full-width per line; Chinese 9 cps / 16; Korean 12 / 16; most other languages 17 / 42; 2 lines; at least 20 frames a cue. `readingSpeed` overrides the speed.
6. **The tracks.** One new caption track per language, named "<source> (<native name>)" ("English (ไทย)"), in that language, with the source's style (Thai, Lao, Khmer, Burmese: the Thai-friendly preset's font and line spacing). `replace` (default) reuses the track of an earlier translation into the same language. All languages are one edit, one undo step.

### Local translation models (OPUS-MT, optional downloads)
| Pair | Model (Helsinki-NLP, ONNX by Xenova, 8-bit) | Licence | Size |
|---|---|---|---|
| English → Thai, Japanese, Vietnamese, Indonesian, Lao, Khmer, Burmese | opus-mt-en-mul (target token `>>tha<<`, `>>jpn<<`, …) | Apache-2.0 | 118 MB |
| Thai → English | opus-mt-th-en | Apache-2.0 | 117 MB |
| English → Spanish / French / German / Chinese (Simplified, Traditional) | opus-mt-en-es / -en-fr / -en-de / -en-zh (`>>cmn_Hans<<`, `>>cmn_Hant<<`) | Apache-2.0 (en-de: CC-BY-4.0) | 111–119 MB |
| Japanese, Korean, Chinese, Spanish, French, German → English | opus-mt-ja-en / -ko-en / -zh-en / -es-en / -fr-en / -de-en | Apache-2.0 (zh-en: CC-BY-4.0) | 111–119 MB |

- Every file (encoder, decoder without the key/value cache, tokenizer, config) is pinned to a Hugging Face revision with its size and SHA-256 (`src/ai/src/TranslationManifest.cpp`, kind `Translation`, in `opus-mt-<pair>/` of the model store). The licence allow-list now includes **CC-BY-4.0** (attribution only); the two CC-BY models carry the attribution in their cards and in THIRD-PARTY-NOTICES. NC licences stay excluded: NLLB-200 (CC-BY-NC-4.0) is not offered. M2M100 (MIT) was set aside because its tokenizer needs SentencePiece's normalisation tables.
- **Quality.** There is no English → Thai OPUS-MT model of its own: `en-mul` covers many languages with one small model, so its Thai (and Japanese) is rough. The dialog marks these chips "local" and suggests the cloud model where quality matters. Thai → English (`th-en`) is a dedicated model.
- **Runtime.** `wabcut-embed` loads one translation model at a time (`load` with `translation`), encodes once and decodes greedily (the decoder runs on the whole prefix for each token, the pad token is never chosen, a run of 8 identical tokens ends the line), on DirectML when available, else the CPU (a GPU failure switches to the CPU once). The tokenizer (`MarianTokenizer`) reads the export's `tokenizer.json`: SentencePiece unigram pieces by Viterbi, "▁" word marks, `</s>` at the end, the target token first. `LocalTranslator` is the `ITranslator` over it (16 texts a request).

### Voice-over, dubbing and narration (`dub/Dub.h`, `TimeFit.h`, `Pipeline.h`; `dub.create`, `narration.create`)
- **Lines.** Dub: the cues of a caption track (usually a translated one), each cue's slot its timing. `[bleep]` is never voiced: the line leaves it out, and a cue that is only a bleep is not spoken. Narration: a script's lines (`roughcut::parseScript`: headings, bullets, `[B-roll]` cues dropped), on the beats of a rough cut (AI-11) when the counts match, else one after another from `at`, `gap` apart.
- **Time fit** (`fitLines`). A line longer than its slot is sped up, pitch kept, by at most `maxStretch` (15 %, the audio engine's WSOLA stretcher, rendered offline: clips at speed ≠ 1 play silent); what still does not fit starts earlier into free time (≤ 0.5 s), and a line that runs into the next slot is flagged and pushes the next one later. Lines shorter than their slot play at their own speed. The report lists every line: as generated, sped up (speed), moved (why) or flagged.
- **The edit** (`assembleDub`), one undo step: the speech as ONE generated audio file (the lines one after another, 50 ms apart, mono 48 kHz float WAV in `<cache>/generated/dub`, with an XMP provenance sidecar), a media item with `generated` provenance (`kind` "audio", `operation` "dub" / "narration", provider, model, SHA-256 of the text, time), a new audio track "Dub (<native name>)" / "Narration" with the **Dialogue** role, a clip per line, and the original dialogue (Dialogue and unassigned tracks; for narration music too) ducked under the new lines with `audio.autoDuck`'s envelope (−18 dB for a dub, −12 dB for narration; `duck`). The timeline draws its "AI" badge on the clips (AI-10).
- **Cost and the cap.** `estimate: true` plans and prices the run without sending anything, anywhere (`estimateSpeechCost` per line: per character for ElevenLabs; per text token plus per minute of audio for OpenAI and Gemini, the audio at 14 characters a second, its ceiling at 6). A run needs `maxCost`: it is refused when the estimate's ceiling is above it, and the run stops before any line that could take it past it (the worst case of the lines already sent counts). The platform's monthly cap and consent (capability Speak) apply to every request, which is never retried by the transport (a line is never billed twice).
- **Who may run it.** Paid generation follows AI-10: standalone CLI/MCP and the assistant cannot spend. In the app the assistant's `dub.create` / `narration.create` open the Dub dialog prefilled; the user sees the voice, the estimate, sets the cap and answers the consent prompt. A no-cloud project refuses both commands (there is no permissive local voice, see below).
- **Voice cloning** (opt-in, ElevenLabs instant voice cloning): `cloneVoice` with `rightsConfirmed` ("I have the speaker's permission") and the project `speaker`. Refused for a speaker with any redacted speech in the sequence, with less than 5 s of their speech, or with another provider. Up to 90 s of the speaker's own transcribed speech from one recording is written to a temporary WAV, uploaded once (`POST /v1/voices/add`), used for the run, and the voice is deleted again (`DELETE /v1/voices/{id}`) when the run ends, also on failure; the sample file is removed.

### Voice providers (BYOK; as of 2026-10-12)
| Model (registry id) | API | Price | Notes |
|---|---|---|---|
| ElevenLabs v4 (`eleven_v4`) | `POST /v1/text-to-speech/{voice_id}/with-timestamps`, `xi-api-key`; `{text, model_id}` → `audio_base64` (MP3 44.1 kHz) + character alignment | $0.08 / 1K characters | 90+ languages incl. Thai |
| ElevenLabs v3 (`eleven_v3`, the default) | as above | $0.08 / 1K characters | 74 languages incl. Thai; 5,000 characters a request |
| ElevenLabs Flash v2.5 (`eleven_flash_v2_5`) | as above | $0.04 / 1K characters | 32 languages, **no Thai** (refused for th, lo, km, my) |
| OpenAI `gpt-4o-mini-tts` | `POST /v1/audio/speech`, bearer; `{model, input ≤ 4096, voice, instructions, response_format: "wav"}` | $0.60 / 1M text tokens + $12 / 1M audio tokens (≈ $0.015 / min) | 13 voices (marin and cedar recommended); `/audio/speech` models retire **2027-01-06** |
| Gemini 3.8 Flash TTS (`gemini-3.8-flash-tts`) | `POST /v1beta/interactions`, `x-goog-api-key`; `{model, input:[{type:user_input, content:[{type:text, text, annotations:[{type:speech_metadata, style}]}]}], response_format:{type:audio, mime_type:audio/wav}, generation_config:{speech_config:[{voice}]}}` | $0.50 / 1M text tokens + $9 / 1M audio tokens (25 a second) through 2026-12-31; $1.00 / $18 from 2027-01-01 | GA 2026-09-22; prebuilt voices (Kore, Puck, Charon, …; 150+ at `/v1beta/voices`) |

`style` is passed as OpenAI's `instructions` and Gemini's speech annotation; ElevenLabs takes the delivery from the voice. Sources: elevenlabs.io/pricing/api and the API reference (text-to-speech with timestamps; the Python SDK's `voices/ivc` client for cloning), developers.openai.com (models/gpt-4o-mini-tts, audio speech reference), ai.google.dev (speech generation, pricing page of 2026-10-09).

**No local voice.** Piper's voices need eSpeak NG for phonemes (GPL-3.0) and the Thai MMS-TTS model is CC-BY-NC-4.0, so no text-to-speech model fits the allow-list today: voice-over and narration are BYOK only.

### Commands, MCP and the assistant
| Command | Capability | What it does |
|---|---|---|
| `captions.translate` | EditSequence | captionTrack, languages (1–10), provider (auto / local / cloud), sourceLanguage, glossary, tone, notes, readingSpeed, replace, estimate; returns per language the track and the re-segmentation report |
| `dub.create` | EditProject (the assistant: proposes) | captionTrack, language, model, voice, style, maxStretch, duck, name, cloneVoice + rightsConfirmed + speaker, maxCost, estimate |
| `narration.create` | EditProject (the assistant: proposes) | text, onBeats, at, gap, language, model, voice, style, maxStretch, duck, name, maxCost, estimate |

The CLI and MCP translate with the installed local models (nothing is downloaded headless; no cloud: a headless request cannot answer a consent prompt) and estimate voice-overs. "Make Thai and Japanese subtitles and a Thai voice-over" in the assistant runs `captions.translate` (local, or the cloud model the user already allowed) as a staged edit that is verified like any other, and `dub.create` on the new Thai track, which opens the prefilled Dub dialog with the cost for the user to approve.

### App
- **Translate captions** (`qml/dialogs/TranslateDialog.qml`, singleton `TranslateCaptions`): the caption track, language chips (Thai first; each says "local", "↓ 118 MB" or "cloud"), who translates (Auto / This computer / the user's model, only with one set up and never for a no-cloud project), tone, glossary ("term = translation" lines), notes. **Translate** stages the tracks (the consent prompt first when the cloud model is used); the report per language shows cues re-timed, merged and split, cues still too fast and warnings, and the corrections and bleeps kept. **Add tracks** applies. **Download** fetches a pair's model through the AI network stack.
- **Dub** (`qml/dialogs/DubDialog.qml`, singleton `Dub`): Dub captions or Narration from a script; the voice model (price, set up or not), voice chips (or an ElevenLabs library voice id), delivery, **Hear it** (one short line, ≤ $0.05), max speed-up, ducking, the estimate and the cap, cloning with the rights box. **Generate** (consent first) stages the speech with the timing report (every line's dot: as spoken, sped up or moved, flagged); **Add to timeline** applies it.
- Entry points (the hooks patch): Captions page "Translate…" and "Dub…", Sequence menu "Narration from Script…", and the assistant's proposals.

### Measured (2026-10-12, this machine)
- **Local translation, English → Thai** (opus-mt-en-mul, 8-bit, `ai_global_media` benchmark, 10 cooking-video captions, Release): **1.7–2.0 cues/s on the CPU** (500–590 ms a cue, 163 tokens in all, greedy, no key/value cache). "Today we are cooking green curry." → "วันนี้เรากำลังกินคาร์รี่เขียว" (rough: en-mul). Thai → English (opus-mt-th-en): "สวัสดีครับ วันนี้เราจะทำแกงเขียวหวาน" → "Hello, we're making a green curry today."
- **DirectML** (ONNX Runtime 1.24.4, RTX 3060): creating a session for these 8-bit dynamic-quantized graphs crashes the helper (access violation), so translation defaults to the CPU; asking for the GPU restarts the helper on the CPU after the crash (the benchmark's "auto" run: 1.7 cues/s on the CPU). The fp32 exports were not tried (2× the download).
- Downloads: opus-mt-en-mul 118 MB and opus-mt-th-en 117 MB through the AI network stack, hash-checked. Hugging Face now redirects to `us.aws.cdn.hf.co` (its CDN's Xet bridge); the download redirect allow-list gained it (and `eu.aws.cdn.hf.co`), without which no Hugging Face model could be downloaded.
- `wabcut-cli cmd … captions.translate {"languages":["th"],"provider":"local"}` on a three-cue fixture: "Welcome back to the market." → "ยินดีต้อนรับกลับไปที่ตลาด", "Thanks for watching!" → "ขอบคุณที่ดู!", the bleeped cue kept its `[bleep]`; the new track got Noto Sans Thai at 1.3 line spacing.
- Voice-over runs were measured on the scripted provider only (the UI test: three Thai cues, two sped up ×1.15, one flagged and the next pushed 0.78 s). No live provider call was made for this report.

### Limits
- en-mul's Thai and Japanese are rough (one small model for many languages); the dialog labels those chips "local" and the cloud model is the better choice where quality matters. Local translation has no glossary or tone.
- The decoder runs without its key/value cache (O(n²) per line): fine for captions, slow for long text. A cached decoder (`decoder_with_past`) would be about 3× faster.
- A line that is longer than its slot even at +15 % is moved and flagged, not rewritten; shortening the translation is the user's call (or ask the cloud model again with a tighter reading speed).
- ElevenLabs' character alignment is read but not yet used (karaoke captions for the dub are a follow-up). Voices are the built-in lists plus a typed ElevenLabs voice id; the providers' voice catalogues are not fetched.
- The Translate and Dub entry points in the Captions page, the Sequence menu and Main.qml are a hooks patch for the coordinator (not in this branch).

### Tests
- `ai_global` (core): re-segmentation (English kept, Thai split between words within 17 cps and 35 per line, Japanese flagged, merges), `[bleep]` kept as a marker and retried piece by piece, never sent to a local model, glossary misses, the chat translator's JSON, `captions.translate` (corrections before translation, redacted words never sent, one undo step, replace, dry run, no-cloud refusal, the estimate sends nothing), the OPUS-MT tokenizer, the pairs' pinned allow-listed files, `LocalTranslator` on the scripted helper, time fit (stretch bounds, leads, pushes, flags), dub and narration lines, the run (one file, provenance, the hard cap, failures), cloning (deleted after success and failure, never for a redacted speaker, only with the rights confirmed), `dub.create` / `narration.create` (the Dialogue track, ducking, one undo step, refusals: no cap, above the cap, no approval host, no-cloud).
- `ai_generation_net`: the three voice adapters on recorded response shapes (alignment, WAV length, interactions), quota, cancel, consent, the no-cloud lock, the run's cap, cloning's multipart upload and delete, translation by the chat model through the guard.
- `ai_global_media` (full builds): FFmpeg decoding of a provider's WAV, the WSOLA stretch keeps the pitch, the dub's WAV probes as audio, clone samples; the OPUS-MT benchmark on the real helper (`WABCUT_TEST_TRANSLATION_MODELS=1`, downloads two models once; never in CI).
- `ai_ui`: the Translate captions and Dub dialogs; saves `translate-captions.png`, `dub-timing-report.png` and `dub-track-badge.png`.
- `ai_net` (`test_net_live_speech.cpp`): optional live smoke tests, one short Thai line per provider, only with `WABCUT_LIVE_ELEVENLABS_KEY` / `WABCUT_LIVE_OPENAI_KEY` / `WABCUT_LIVE_GEMINI_KEY` set (never in CI; the key is never printed or logged). Not run for this report: the adapters follow the documented shapes of 2026-10-12 and are tested on recorded fixtures.

## Podcast and interview editing (AI-13)

Automatic camera switching for multicam recordings and a one-click podcast / interview workflow, all on this computer
and free. Libraries: `wabcut_ai_multicam` (`src/ai/multicam`: who speaks when, the switching rules, `analysis.speakers`,
`multicam.autoSwitch`), `wabcut_ai_multicam_media` (the media signals) and `wabcut_ai_workflow` (`src/ai/workflow`:
`workflow.podcast` and its steps).

### Who speaks when (`multicam/Speakers.h`, `Evidence.h`)
- **Evidence**, all in the multicam source's time, each optional: the transcripts of the angles' audio (the C.7 project
  speakers when assigned, else each recording's speaker labels; the same word heard by two microphones is kept once, for
  the louder one), each angle's microphone level (dBFS per 50 ms, decoded at 16 kHz), and each camera's faces with their
  lower-face motion (AI-9's YuNet subjects; a lip cue). No extra model.
- **Speakers.** Transcript speakers; a microphone that hears one speaker ≥ 70 % of its words is that speaker's own mic;
  without a transcript every dedicated microphone is a speaker (a camera's own sound is the room's when there are
  dedicated microphones; without them, every microphone); with neither, every camera that mostly sees one face.
- **Fusion** per 100 ms: transcript 0.5, own mic 0.35 (normalised between its 15th and 95th percentile; a mic more than
  0.25 below the loudest other one counts at 30 %: bleed), face 0.15 (the speaker's close-up's lip motion), over the
  cues a speaker has; hysteresis (on 0.5, off 0.35), gaps under 0.3 s bridged, runs under 0.25 s dropped.
- **Segments** cover the source: one speaker, crosstalk (two or more), non-speech (a loud mic without words: laughter)
  and silence, each with a confidence. A listener's quick "wow" or "right?" (≤ 3 words, ≤ 1.2 s) while someone else holds
  the floor is a **reaction**, not a turn (no crosstalk, no cut), and so is laughter on their own mic.
- **Angles.** A camera that sees two or more faces ≥ 35 % of the time is the wide shot; one face ≥ 40 % is a close-up of
  the speaker whose score its face's motion follows best (Pearson), else of the speaker whose own mic is in the angle.
  The user's assignments (angle → close-up of a speaker, or wide) always win and are marked as theirs.

### The switching rules (`multicam/AutoSwitch.h`)
| Rule | Interview (default) | Podcast calm | Energetic |
|---|---|---|---|
| The speaker's close-up, held at least | 2 s | 3 s | 1 s |
| Cut before they start (J-cut lead) | 0.25 s | 0.3 s | 0.15 s |
| Wide for crosstalk lasting | 0.6 s | 1 s | 0.4 s |
| Wide after a pause of | 3 s | 4 s | 2 s |
| Variety: a wide shot after N s on one close-up, for M s | 15 / 3 s | 25 / 4 s | 8 / 2 s |
| Reaction shots (listener's laughter or quick reaction ≥ strength) | off | off | on (0.5, 1 s) |
| Open on the wide shot for | 3 s | 4 s | 2 s |

Always: wide for laughter, never mid-word (a cut moves to the nearest gap between words within 0.4 s; the ones that
cannot are counted and reported). A cut that comes too soon after the last waits for the minimum hold, unless a later
wish supersedes it. Every rule is a parameter of `multicam.autoSwitch`. Every cut carries its reason and a sentence:
"Cut to Cam B: Guest starts talking (0.25 s early)", "Wide on Cam C: Host and Guest talk over each other", "Wide on Cam
C: variety after 15 s on Host", "Cut to Cam B: Guest laughs", "(moved 0.12 s to a pause between words)".

The result is **ordinary angle cuts** (`multicam.switchAngle`'s): the multicam clip split where the angle changes, each
piece showing its angle, one undo step; switch any piece by hand afterwards. The picture switches; the sound stays the
microphones' mix (`"target": "both"` switches the sound too). A clip already cut into pieces (after a clean-up) is
switched piece by piece.

### The podcast workflow (`workflow/Podcast.h`, `workflow.podcast`)
1. **Sync** — `multicam.create` of the cameras and microphones, synced by audio (A.5).
2. **Lay out** — a new sequence: the multicam picture on V1 (the first camera's frame size and rate), each microphone on
   its own Dialogue track, in sync. Microphones as ordinary clips, because transcripts, clean-up, captions, Enhance Speech
   and loudness work on leaf clips (transcripts are not mapped through a multicam clip's audio).
3. **Transcribe** — `transcript.transcribe` of the microphones on this computer (whisper.cpp), unless they have
   transcripts.
4. **Enhance Speech** — `audio.enhanceSpeech` on the microphones (A.4, strength 70).
5. **Loudness** — `audio.normalize` of the mix to −16 LUFS (−14 for YouTube).
6. **Clean-up** — `transcript.cleanup`: fillers and pauses over 1 s, cut on every track (the picture too).
7. **Switch cameras** — `multicam.autoSwitch` with the preset (Podcast calm by default).
8. **Captions** — `captions.generateFromTranscript` with the "Podcast" style.
9. **Chapters** — chapter markers at the strongest topic changes (AI-9's topic segmentation of the transcript's
   sentences; the first at 0:00, each ≥ 60 s, at most 12, titled from their first sentence) and the YouTube chapters
   text (`sequence.exportMarkers`).
10. **Shorts** — off by default: `shorts.find` and `shorts.create` (AI-9).

Every step runs through the editing catalog with the host's services, so it does exactly what its command does. The
plan says for every step whether it is on and why not ("unticked", "already made", "already transcribed", "no audio sync
here", "no Enhance Speech here"); the cost is "local, free". `workflow.podcast` without `run` returns the plan; with `run`
it runs the enabled (or the listed `steps`) as one undo step and returns what each step did and a `context` (the
multicam source and the episode) to continue from. Clean-up, captions and chapters are off when there will be no
transcripts. A step that fails after others keeps them (still one undo step), is reported as "failed" and stops the run.

**Resuming.** The app applies each step as its own undo step, then checkpoints the project in its version history (C.9,
`<project>.history`) and writes the run's journal (`<project>.history/podcast-workflow.json`: the options, the context,
each step's status and the history revision after it, written atomically). After a crash, Resume reads the journal; when
the reopened project has lost the steps (it was not saved), the last checkpoint is restored as one undo step, and the run
continues with the next step.

### Commands, MCP and the assistant
| Command | Capability | What it does |
|---|---|---|
| `analysis.speakers` | MediaAnalysis (the assistant: an allowed read) | clip or multicam, angles (your assignments), segments, faces (default true); returns speakers (seconds, own mic), angles (role, whom, confidence, reason, a face), segments, reactions, crosstalk / silence / non-speech seconds |
| `multicam.autoSwitch` | EditSequence | clip, preset, every rule (seconds, booleans), target (video / both), faces, angles; returns the cuts (time, angle, reason, explanation, moved), the plan in source time and statistics; dryRun previews |
| `workflow.podcast` | EditProject (the assistant: an additive command) | cameras + mics (or media, split by picture), name, sync, enhanceStrength, lufs, fillers, silences, switchPreset, angles, captionPreset, maxChapters, minChapterSeconds, shorts, shortsCount, skip, steps, context, run |

`wabcut-cli cmd` and `mcp` serve both with the media signals (the faces from the subject cache and detector, the levels
decoded from the files). In the assistant, "switch cameras to whoever is talking" reads `analysis.speakers` and stages
`multicam.autoSwitch` on the request's multicam clip; "edit this 3-camera podcast" stages `workflow.podcast` (it adds the
multicam source and the episode; verification checks that nothing else changed). Both are verified and applied as one
undo step, and cost nothing.

### App
- **Auto switch** (`qml/dialogs/AutoSwitchDialog.qml`, singleton `AutoSwitch`): the cameras with what each one sees (the
  face it found, cropped from its frame) and whom the analysis thinks it shows, with a guess's confidence and reason;
  the mapping (Auto / a speaker / Wide) per camera; the style (Podcast calm / Interview / Energetic) and every rule as a
  slider or switch; the cut list with each cut's reason and a strip of the shots in their cameras' colours; the cuts as
  ghosts on the timeline. **Switch angles** applies them as one undo step.
- **Podcast workflow** (`qml/panels/creator/PodcastWorkflowSheet.qml`, singleton `PodcastWorkflow`): the recordings
  (import files, or one click on the project's media; camera or mic), the style (episode name, switching, captions,
  loudness, fillers, pauses), and the checklist of steps: tick or untick, **Preview** (staged, the timeline ghost),
  **Apply** (its own undo step), **Skip**; **Run all** runs the rest in turn; the progress bar shows the steps done and the
  step at work; **Resume the last run** after a crash; the YouTube chapters to copy.
- Entry points (the hooks patch): the clip menu's "Auto Switch…" on a multicam clip, Sequence > "Podcast Workflow…",
  and the dialogs' loaders in Main.qml.

### Measured (2026-10-12, this machine, Release)
- **Speaker analysis and switching** (`ai_multicam` benchmark: a 60-minute conversation on 3 cameras and 2 microphones,
  the signals in memory, 36,000 frames per speaker): fusing the evidence 0.07–0.10 s; `multicam.autoSwitch` through the
  engine (analysis, plan and 451 cuts on the clip) 0.08–0.10 s.
- **Microphone levels** (`ai_multicam_media`: a 10-minute 48 kHz WAV decoded at 16 kHz): 0.11–0.18 s, 3,300–5,600× real
  time, so the two microphones of a 60-minute podcast take about 2 s the first time (then kept for the session).
- **Faces** are AI-9's subject analysis (cached on disk after the first run): 4.7× real time per camera with DirectML,
  1.2× on the CPU (RTX 3060, the crowded clip under "Auto reframe"). Extrapolated, a 60-minute 3-camera podcast takes
  about 38 min with DirectML and about 2.5 h on the CPU the first time: hence `"faces": false` (the dialog's "Look at the
  cameras' faces"), with the angles' roles from the user's assignments and the microphones.
- **End to end on real files** (`ai_multicam_media`: three FFmpeg camera files started 1.5 s late and 0.8 s early, two
  microphone WAVs, 30 s): the whole workflow (audio sync, layout, loudness rendered and measured, switching on the
  decoded levels, captions, chapters) in 1.6–2.8 s; the cameras' offsets found by their sound within 50 ms.
- **wabcut-cli** on that project without transcripts or a face detector: `analysis.speakers` in 2.4 s (process start
  included) with the two speakers, their turns and the crosstalk at the right times; `workflow.podcast` with `run`
  (transcription and Enhance Speech unticked, `angles` given): synced, laid out, loudness −21.3 → −16.0 LUFS, switched;
  clean-up, captions and chapters off ("no transcripts").
- **MCP**: `analysis_speakers` with `--allow-analysis`; the plan and the explained cuts come back in `edit_batch`'s steps
  (a single edit tool returns its change summary).

### Limits
- Who speaks is decided per 100 ms from the cues there are. With only one room microphone and no transcript speaker
  labels, the speakers come from the faces' lip motion alone (weaker); transcribe with speaker labels, give each person a
  microphone, or assign the angles.
- Transcripts are not mapped through a multicam clip's audio (`SequenceTranscript` reads leaf clips): the workflow lays
  the microphones out as ordinary clips for that reason, and `analysis.speakers` reads the transcripts of the multicam
  source's angles directly.
- Face detection is AI-9's (YuNet in wabcut-embed): its cost dominates a first analysis of long camera files and is
  cached on disk afterwards; without the detector the speakers come from the transcripts and microphones.
- The workflow's transcription runs whisper.cpp on this computer with the installed model; the cloud stays with the
  Transcript panel (its consent prompt). Resuming needs a saved project (the history and journal live next to it).
- The Podcast workflow sheet has no camera mapping of its own: its switching step guesses (faces, microphones); correct
  it with Auto switch on the episode's picture (one more undo step). `workflow.podcast` takes `angles` for that.
- The Auto switch and Podcast workflow entry points in the clip menu, the Sequence menu and Main.qml are a hooks patch
  for the coordinator (not in this branch); `ai_ui` checks them once it is applied.

### Tests
- `ai_multicam` (core): the speaker timeline of a scripted 3-camera, 2-microphone conversation (turns, crosstalk,
  silence, a "Wow!" reaction that is not crosstalk, laughter on the listener's mic, close-ups and the wide shot), each cue
  alone, bleed and duplicate words, the user's assignments; the rules on deterministic timelines (minimum hold, the lead,
  never mid-word and the warning when it must, wide for crosstalk but not for 0.3 s of it, laughter, variety and back,
  the opening, reaction shots, presets); `multicam.autoSwitch` through the engine (dry run, one undo step of ordinary
  angle cuts, a cut switched by hand afterwards, `analysis.speakers` changes nothing); the workflow's plan and reasons,
  each step its own undo step and in sync after the clean-up, undone one by one, resumed from its journal and a history
  checkpoint, chapters from topics, `workflow.podcast` as one undo step and its refusals; the assistant ("switch cameras
  to whoever is talking", "edit this 3-camera podcast"); the 60-minute benchmark.
- `ai_multicam_media` (full builds): microphone levels from real WAV files and the decode speed; the workflow end to end
  on FFmpeg-made camera files (synced by sound, switched on the decoded levels).
- `ai_ui`: the Auto switch dialog and the Podcast workflow sheet (steps applied one by one, a crash and Resume, the rest
  run); saves `auto-switch-dialog.png`, `podcast-workflow-sheet.png` and `podcast-workflow-done.png`; the hooks once
  applied.

## AI Object Mask (AI-14)

Click a person or an object on a frame (or draw a box around it, or name it) and get a mask that follows it through the
shot - Premiere's AI Object Mask, Resolve's Magic Mask, Final Cut's Magnetic Mask - on this computer: no picture leaves
it. The mask is an ordinary I.6 mask of a new kind, `matte` ([masks.md](masks.md#object-masks-maskshapematte-ai-14)), so it
works wherever masks work: on any effect (blur the background, grade only the subject), as the clip's opacity (remove the
background) and as a source-anchored redaction (blur a face the AI follows). Libraries: `wabcut_ai_segment`
(`src/ai/segment`, pure C++, core preset: mattes, propagation, the helper client, subjects, the commands) and
`wabcut_ai_segment_media` (full builds: decoded pictures, the services for a project, the compositor's matte source).

### The model: SAM 2.1 Hiera-Tiny, its full video tracker
Meta's Segment Anything 2.1, the smallest variant (Hiera-Tiny, 38.9 M parameters), **code and weights Apache-2.0**
([facebook/sam2.1-hiera-tiny](https://huggingface.co/facebook/sam2.1-hiera-tiny) at `de431c4043854a71d8101e17995dfe596bf101a5`).
Not just the image model: the ONNX export by square-zero-labs of Hugging Face transformers' `Sam2VideoModel`
([square-zero-labs/sam2.1-tiny-video-onnx](https://huggingface.co/square-zero-labs/sam2.1-tiny-video-onnx), Apache-2.0,
revision `3b2984dd865f6e9d2cc6aed0be6a5a5c2eb352ce`) has the memory encoder, the memory attention and the object pointers,
so the object is followed by SAM 2's own video propagation (no per-frame fallback was needed). The exporter validated it
against PyTorch's `propagate_in_video_iterator`: worst per-frame IoU 0.9967. fp32, 1024 x 1024 input. Six files, 190 MB,
downloaded once on request (the licence card first), every file checked by SHA-256 (`src/ai/src/ModelStore.cpp`):

| file | bytes | SHA-256 |
|---|---|---|
| `onnx/vision_encoder.onnx` | 134 335 567 | `aa7a8542942f042e235a993a1ab0ccf5f049918500577802a7f10ec1b39bb873` |
| `onnx/mask_decoder.onnx` | 17 794 355 | `0461896de3db00936fe1643506f129d71cb6d5d2ae15754811756b7ea1b070c6` |
| `onnx/memory_attention.onnx` | 32 259 165 | `791e648ce8f5ef91ad00ba06e83066ff261ae5a645f0b042b4f46a89fd054baf` |
| `onnx/memory_encoder.onnx` | 5 616 496 | `580d246c109de88838f600ba7c1c0d03d1fe267f7641b06f8c88c5f0dc5834cd` |
| `onnx/pointer_tpos.onnx` | 67 289 | `7e71df1d75dba09bc18dd4ae745c2a2cceebdd14d84eadea2fe65d0205f101fc` |
| `constants.json` | 9 922 | `172edc70e892aab0aef6caedf97b8e4091fbf6c5d26dac5fd9ea69d093d927d3` |

Considered and not used: diffusionstudio's fp16 export at 512 x 512 (83 MB, faster, coarser edges).

### In wabcut-embed (`src/ai_local/embed/SegmentModels.h`)
`Load.segmentation` opens the five graphs (DirectML, else the CPU; a DirectML error while segmenting retries that frame
on the CPU). The `segment_frame` request carries one 1024 x 1024 picture (the stored frame, stretched) through a raw RGB
file, and the matte comes back through another (8-bit alpha at the requested size, the 1024 logits resampled
bilinearly then a sigmoid): `mode: "prompt"` starts an object from points (labels 1 on, 0 off; a box as its corners,
labels 2 and 3), `mode: "track"` segments the next picture from the object's memory - the prompted frame's and the six
most recent memories, fifteen recent object pointers, temporal positions from `frame` / `frames`, padded as the export
expects. The encoder's features of the last picture are kept, so refining clicks and the two passes from a prompt skip
the expensive graph. `WABCUT_EMBED_TIMINGS=1` logs each graph's time.

`HelperSegmenter` (the client, `segment/HelperSegmenter.h`) is the `Segmenter` the app and wabcut-cli use. When the GPU
provider takes the helper down (an access violation is no exception) the helper restarts on the CPU, the prompt is
replayed into it and the frame retried (the recent memory restarts from the prompt); a helper that keeps crashing ends
the run after three restarts. A hung helper times out, a stop request cancels.

### Propagation (`segment/Propagation.h`)
The prompt snaps to its frame on the media's grid; the run goes forward, backward or both (both: the backward pass
prompts again) through the clip's media range or In / Out, and stops at the **shot cuts around the prompt** (C.5,
the analysis cache's) unless `acrossShots`. "Forward" is timeline direction: on a reversed clip the model runs the
other way through the media. Each frame keeps its matte, whether the model sees the object (object score > 0), its
bounding box and a **confidence** = the decoder's predicted IoU x sigmoid(object score). A job: progress per frame,
cancellable between frames.

### Mattes and their cache (`segment/Matte.h`)
A matte is 512 px on the long side at the frame's display aspect (512 x 288 for 16:9), run-length encoded (value byte,
LEB128 run length; a few kilobytes a frame), kept in the **analysis cache** (analyzer `matte`, JSON with base64 RLE,
bounded on read) under the media's source fingerprint, the stream, the model version
(`sam2.1-tiny-video-aa7a8542-0461896d-v1`) and a hash of the prompts, the span and how the run went. That key is the
track's identity, which the mask names (`MaskMatte::track`). A request made before (a dry run's apply, undo and redo,
the same clicks again) reads the mattes back instead of running the model. The compositor's `render::MatteSource`
(`CacheMatteSource`, installed by the app, wabcut-cli and export) turns a matte into a signed-distance field (an exact
Euclidean distance transform of the binarised matte, in frame heights) - the same input every mask shape gives the I.6
pipeline, so feather, choke, invert, the combination and the GPU / CPU parity are unchanged.

### Commands (EditSequence; docs/commands.md)
| command | what it does |
|---|---|
| `mask.objectSelect` | `points` [{x, y, positive}] and / or `box` on the clip's frame at `time` - or `subject`: `"face"` or any of the 80 COCO classes (`"person"`, `"car"`, `"dog"`, ...) with an optional `colour` (red, orange, yellow, green, cyan, blue, purple, pink, white, black, grey) and `pick` (largest, center, left, right) - segmented and tracked (`propagate`: none / forward / backward / both, default both; `acrossShots`; `in` / `out`); the matte mask goes on `effect`, or without it on the clip's opacity (a cut-out); `feather` (default 0.004 frame heights), `expansion` (negative chokes), `inverted`, `mode`, `name` |
| `mask.refine` | more clicks or a new box on a frame of an object mask: the frame's prompt grows and the object is re-tracked from there over the frames the mask covers (both ways on a prompted frame, else onwards) |
| `mask.propagate` | tracks it further from its last prompt: after `propagate: "none"`, across a cut, past In / Out |

One undo step each; dryRun previews. The data says what happened: the track, frames, lost frames, the confidence (lowest,
mean and a strip of at most 120 values), where the run stopped (`stoppedAtCutBefore` / `After`, timecodes), the device
and its speed, and whether the cache answered. Remapped clips and frame holds are refused (their mask curve time is not
media time). Subjects come from AI-9's detectors in wabcut-embed, asked for every COCO class (`Detect.allClasses`;
people-only stays the default, so reframe's cache keys do not change); a colour picks the box whose middle reads most as
that colour.

### The assistant
`mask.*` are EditSequence: the assistant stages them like any edit, verified and applied as one undo step. "Blur the
background behind the speaker": `clip.addEffect` Gaussian Blur, then `mask.objectSelect` with `subject: "person"`,
that effect and `inverted: true`. "Isolate the red car and brighten it": Basic Correction, then `subject: "car"`,
`colour: "red"`. Redactions stay out of its reach (`redaction.*` are EditProject, not in its additive list); people make
AI-tracked redactions themselves in the Source Monitor.

### In the app
`ObjectMasks` (src/app/src/bridge/ObjectMaskBridge.h): Effect Controls' mask section has the **Object** tool (sparkles):
armed, a click on the program monitor is a positive point (the minus toggle, or Alt-click, a negative one) and a drag a
box; the first prompt makes the mask on the frame at the playhead, the next ones refine it. **Track** backward / both
ways / forward runs a job with its progress, cancel and a per-frame confidence strip; *Across shot cuts* goes on past
them. The **matte preview** shows the object tinted (colour) or the matte in grey (alpha) over the monitor, placed
through the clip's Motion. The Source Monitor's redaction popover has *Track an object (AI)*: the redaction drag then
marks an object, tracked from the source position through In..Out, and the redaction's region is its matte mask. SAM
2.1 is downloaded from the Object tool (its card first); the model stays loaded between clicks.

### Measured (i5-12600K + RTX 3060, a shared machine; test-output of the smoke test)
`ai_segment_media`'s opt-in smoke test (`WABCUT_TEST_SAM2_DIR`) runs the real model through wabcut-embed on a synthetic
shot drawn at 1920 x 1080 (a red object moving over a textured background), 30 tracked frames:

| device | frames / s at the 1080p working resolution | per frame (model) | IoU against the drawn object |
|---|---|---|---|
| DirectML | 2.7 - 2.9 | encoder 95 ms, memory attention 155 ms, decoder 11 ms, memory encoder 6 ms | mean 0.980, worst 0.960 |
| CPU | 0.2 | encoder 1.7 s, memory attention 2.1 s, memory encoder 0.3 - 0.6 s | the same |

End to end the way the app and wabcut-cli run it (the six files installed through `ModelStore::ensure`, which checked
every size and SHA-256 against the pinned manifest; `mask.objectSelect` through the engine on a decoded 1080p H.264 file
of the same scene, 40 frames, both ways from frame 5): 22.7 s in all on DirectML including the model's load, 1.8
frames / s while tracking (decoding, resizing and both passes included), IoU mean 0.983, worst 0.967.

The IoU is against synthetic ground truth (no natural-video fixture with ground truth ships); the export's own
validation against PyTorch is the quality reference above. The source resolution does not change the model's cost: every
frame is resized to 1024 x 1024.

### Tests
`ai_segment` (core: the matte codec and every malformed input, the cache key, tracks as JSON, the store, propagation
and its cuts / directions / cancel / failures, the protocol, HelperSegmenter against the scripted helper - a GPU crash
at load, a crash mid-run, one that keeps crashing, a hung one, cancel - the commands through the engine and the
assistant), `ai_segment_media` (decoded pictures forward and backward, the matte source - effects get the frame's matte,
redactions fail closed - mask.objectSelect end to end on a real file, the gated real-model test), `ai_ui`
(test_object_mask_ui.cpp: the Object tool, refine, track, the preview, an AI-tracked redaction failing closed in
thumbnails), and the render / playback / export suites' matte cases ([masks.md](masks.md#object-masks-maskshapematte-ai-14)).

### Limits
- The CPU fallback is slow (about 5 s a frame): fine for a few frames and refinement, long for a whole shot.
- One object per mask; select again for another (masks combine as usual).
- A correction on a frame re-tracks onwards from it with that frame's prompt only; frames before it keep their mattes.
- Mattes are 512 px on the long side: fine edges (hair) are softer than the picture.
- The mattes live in this computer's analysis cache. Another computer, or a cleared cache, shows the mask's bounding box
  (and a redaction its box grown) until the object is selected again there.
- Remapped clips and frame holds are refused.

## UI integration
- An "AI" status pill shows Local / provider name / Offline. Settings > AI (`settings/AiSettingsPage`, 0.1.3; it replaced
  the AI preferences dialog) has a per-capability provider choice, a key manager (test/rotate/remove), a model store,
  consent history and the offline toggle, under the tabs Providers and keys / Models / Privacy / Usage / Local assistant.
- **The "AI features" switch (0.1.3):** the master switch at the top of Settings > AI (`Preferences.aiEnabled`). Off
  forces offline mode on, so no request leaves the computer whatever the consent or the per-feature choice, and hides
  the AI entry points (the Window menu's Generate panel, Rough Cut and Narration from Script in the sequence menu; Generate
  says "AI is off" if it is already open). The offline setting from before is kept in `Preferences.aiOfflineBefore`
  and comes back when the switch goes on again. Local tools that need no provider (transcription, Enhance Speech,
  smart search) keep working: the switch is about what leaves the computer and the generative entry points.
- **A model by name (0.1.3):** "Assistant model" takes a provider and a model id typed by hand (a model newer than
  `models.json`, a fine-tune, an OpenAI-compatible server's name). `AiController.addModel` adds it to the registry as a
  user model and makes it the assistant's model; its chip removes it again. A custom base URL per provider stays in
  Providers and keys.
- There's a Transcript panel (Library tab or dockable) and a command bar overlay (spring pop-in). Plan previews render as ghost clips with accept/reject.
- Editing actions share the CLI/MCP catalog. Host-dependent generation and managed-runtime commands require the services described in AI-10 below.

## Testing
- Core logic (transcript ↔ timeline mapping, plan → EditOps, filler/silence detection, beat/shot detection on synthetic
  signals) is unit-tested deterministically.
- Providers have contract tests against recorded HTTP fixtures; live calls are opt-in, gated on an env key, and never run in CI.
- Local models: smoke tests with tiny fixtures; the heavy models are skipped unless the model has been downloaded.

## AI-10: managed local assistant and generated media (as of 2026-10-11)

The default assistant selection is `qwen3-4b-instruct-2507` (since AI-15: the tier this computer runs best, see
"AI-15" below), running through an out-of-process
`llama-server`. Settings > AI > Local assistant shows license cards, download bytes, estimated
RAM/VRAM fit and start/stop. Models and runtimes are downloaded only on an explicit Download action
while online; an offline session never downloads missing files. Existing transcription, semantic
search and subject/reframe helpers remain local. Offline mode and the project no-cloud lock also
block generative media, including the next poll/download of an in-flight job.

The managed runtime currently targets Windows x64. It binds only `127.0.0.1`, selects an OS-assigned
random port, uses a fresh 256-bit API token, authenticates its model-readiness request, and passes
`--jinja --parallel 1 --ctx-size 8192`. No runtime source build is part of installation. Runtime
archives and GGUF weights pass ModelStore SHA-256 verification. Extracted runtime files live in a
fresh cache directory; source archives and models are retained for offline restarts. CUDA requires
an explicit license acceptance in the runtime planner; the UI currently selects Vulkan or CPU and
does not expose that CUDA opt-in. Multi-adapter/unknown GPU budgets fall back to CPU rather than
assuming which device llama.cpp will use.

The conservative launch estimates are **GGUF bytes + 4 GiB available RAM**, and, for GPU offload,
**GGUF bytes + 3 GiB GPU budget**. These are admission estimates, not measured peak allocations.
The helper runs below normal CPU priority with at most four threads, unloads after two idle minutes,
and stops on playback/export pressure. GPU scheduling has no hard real-time guarantee: stopping
the helper and retaining reserves is the policy, not a proof of zero playback latency. The local
agent advertises a smaller tool catalog, caps each answer at 1,024 tokens and refuses oversized
context rather than silently dropping project constraints. Phi is available as chat-only until its
tool template is verified.

### Pinned downloads and model cards

All sizes are exact compressed/download bytes, not installed size. GGUF sources below were checked
against their Hugging Face repository revisions, model-card licenses and LFS SHA-256 metadata on
2026-10-11. Only `Apache-2.0` and `MIT` are admitted for local LLM weights, including cached entries.
These licenses do not guarantee model output accuracy or grant rights to arbitrary source footage.

| Model | GGUF source revision | License | Bytes | SHA-256 |
|---|---|---|---:|---|
| Qwen3 4B Instruct 2507 Q4_K_M | [Unsloth](https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/tree/a06e946bb6b655725eafa393f4a9745d460374c9) | Apache-2.0; [original card](https://huggingface.co/Qwen/Qwen3-4B-Instruct-2507) | 2497281120 | `3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597` |
| Qwen3 8B Q4_K_M | [Official Qwen](https://huggingface.co/Qwen/Qwen3-8B-GGUF/tree/7c41481f57cb95916b40956ab2f0b139b296d974) | Apache-2.0 | 5027783488 | `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785` |
| Phi-4 mini instruct Q4_K_M | [Unsloth](https://huggingface.co/unsloth/Phi-4-mini-instruct-GGUF/tree/78eb92a46fc37e6b524df991ed9aca9bc6aa7b80) | MIT; [original card](https://huggingface.co/microsoft/Phi-4-mini-instruct) | 2491874272 | `88c00229914083cd112853aab84ed51b87bdf6b9ce42f532d8c85c7c63b1730a` |

Runtime: [official llama.cpp b11429 release](https://github.com/ggml-org/llama.cpp/releases/tag/b11429),
published 2026-10-05, MIT. Digests/sizes are from the release API, checked 2026-10-11.

| Archive | Bytes | SHA-256 |
|---|---:|---|
| `llama-b11429-bin-win-cpu-x64.zip` | 19398918 | `1283323272b04cd07905816a597a0da810918102de958f4ff6f7bbaa70ed2efe` |
| `llama-b11429-bin-win-vulkan-x64.zip` | 33337769 | `1bfe78ad9168b79fa02bf67f6af9f5e17a966d824d77238517f7bef12ac73b36` |
| `llama-b11429-bin-win-cuda-12.4-x64.zip` | 264478423 | `dd6df685c1024e6aa55ca55ad35038692d5e284d4dc895e7a646c3245d3d14ff` |
| `cudart-llama-bin-win-cuda-12.4-x64.zip` | 391443627 | `8c79a9b226de4b3cacfd1f83d24f962d0773be79f1e7b75c6af4ded7e32ae1d6` |

The separate cudart bundle uses the NVIDIA CUDA EULA, **not** MIT/Apache model terms. It is not
silently downloaded by the UI. License notices are also in THIRD-PARTY-NOTICES.md and the in-app cards.
The authoritative CLI flags/tool-calling contract is the
[pinned server README](https://github.com/ggml-org/llama.cpp/blob/b11429/tools/server/README.md).

### BYOK Generate and Generative Extend

Generate offers a prompt, model/provider, duration/aspect, pre-run estimate, an exact per-run hard
cap, explicit upload/spend consent, progress, and a result preview. Accept imports/places the result
as one undo step; project changes since the preview prevent applying it. No provider key is stored
in a project, sidecar or QML. AI-10 removes the editor's plaintext key-store fallback;
it uses the OS keychain or an ephemeral session key. Existing plaintext files are not read or migrated automatically. Existing environment overrides remain useful
for opt-in smoke tests and are never printed.

The approved API subset and prices below were checked against official docs on **2026-10-11**;
Google and Runway schemas were additionally checked through the coordinator's Context7 results.
`models.json` is the versioned price registry; estimates are not a live provider invoice. Unknown
options/tiers fail closed. Prices do not include taxes or a provider's minimum account purchase.

| Provider/model | API subset | Output and estimate |
|---|---|---|
| Gemini `veo-3.1-fast-generate-preview` | `predictLongRunning`; poll named operation; authenticated video URI download | 720p, 16:9/9:16, 4/6/8 seconds; $0.10 per generated second |
| Runway `gen4.5` | `/v1/text_to_video` or `/v1/image_to_video`; `/v1/tasks/{id}`; DELETE cancellation | 1280:720 or 720:1280, integer 2–10 seconds; 12 credits/sec at $0.01/credit = $0.12/sec |
| Runway `gen4_image` | `/v1/text_to_image`; optional reference image | 720p landscape/portrait/square; 5 credits = $0.05/image |

Sources: [Google video generation](https://ai.google.dev/gemini-api/docs/veo),
[Google pricing](https://ai.google.dev/gemini-api/docs/pricing),
[Runway API](https://docs.dev.runwayml.com/api/),
[Runway pricing](https://docs.dev.runwayml.com/guides/pricing/),
[Runway generated outputs](https://docs.dev.runwayml.com/assets/outputs/), and the
[official generated SDK](https://github.com/runwayml/sdk-python/tree/main/src/runwayml).
Runway uses `X-Runway-Version: 2024-11-06`. Google `durationSeconds` is a string, and conditioning
uses `image.inlineData.{mimeType,data}`. No undocumented `sampleCount` option is sent.

Google rounds a five-second request up to six billable seconds; the preview shows that price and
conforming trims the imported result to the requested duration. Neither provider submission is
retried automatically: a network ambiguity must not submit a duplicate paid job. Every dispatch
rechecks consent/no-cloud and the project context. Reservations enforce both the per-run cap and
monthly limit; an uncertain charged failure is conservatively recorded. Google has no cancellation
API in this subset, so cancelling local polling does not promise to stop provider work or charges.
Download hosts are restricted; Runway credentials never accompany its CDN download. The current
Runway allow-list contains the documented `dnznrvs05pmza.cloudfront.net` host only; other CDN hosts
and cross-origin redirects fail closed pending verification.

Extend conditions on the decoded source boundary frame. Tail output follows that frame; head output
is generated outward then reversed, clearly labeled in the panel. This is boundary-conditioned
synthesis, not a guarantee of physically correct continuation or the providers' native video-extension
feature. Source effects/compositing and generated audio are not carried into the extension. The media
layer decodes, scales, resamples the exact sequence frame clock and encodes MP4; size/rate/duration
are checked before staging. Occupied or negative head ranges and locked tracks are refused before
spend. Existing linked partners are preserved when linking the extension to its original. B-roll can
remain in the Library or be placed on a new video track. Still generation remains a Library asset.

**Sound effects (0.2, `gen.sfx`).** Premiere's Generative Media makes sound effects from a prompt; so does Generate,
through the user's own ElevenLabs key (checked against the official docs on 2026-10-16):

| Provider/model | API subset | Output and estimate |
|---|---|---|
| ElevenLabs `eleven_text_to_sound_v2` | `POST /v1/sound-generation?output_format=mp3_44100_128` with `text`, `model_id`, `duration_seconds` (0.5-30, or left out: the model picks the length), `prompt_influence`, `loop`; the answer is the MP3 | MP3 44.1 kHz; $0.12 per minute of sound ([pricing](https://elevenlabs.io/pricing/api)); a length the model picks is estimated at 30 s; the spend ledger is settled at the length that came back (the MP3's frames counted), never above the approved estimate |

The panel shows Seconds (0: the model picks) and Seamless loop for a sound model; the result is imported as audio media
with provenance (`kind` "audio", `operation` "text-to-sound-effect", the prompt's hash only) and can go on a new audio
track. The answer is checked to be an MP3 (an error page with status 200 is refused). The request is never retried, and
consent, offline mode and the project's no-cloud lock, the keychain-only key, the per-run hard cap and the monthly cap
are the ones every generation goes through (`AiPlatform::generate`). `gen.sfx` proposes it to the user like `gen.broll`:
the assistant cannot authorize a charge. Tests: `tests/ai/test_net_generation.cpp` "Sound effects: ..." and "A sound
effect is booked at the length that came back...". Not done: other sound providers.

Commands registered in the shared command/MCP catalog:
`llm.local.status/start/stop/models`, `gen.extend`, `gen.broll`, `gen.image`, `gen.sfx`, `provenance.report`.
In the app, agent `gen.*` calls **prefill an approval proposal**, without uploads or paid work; the user
reviews the exact Generate form and then accepts its staged result. CLI/MCP manages a separate local runtime for the lifetime of its process, with the same pinned
cache, memory gate and idle unload. It refuses paid generation rather than accepting model-supplied
consent. The headless generation approval/execution host is a remaining integration step; catalog
registration alone is not generation execution.

### Provenance and the C2PA decision

Generated media has additive `MediaItem.generated` metadata: provider/model, SHA-256 of the UTF-8
prompt, UTC date, kind, operation and weak source-media reference. Absence preserves existing project
serialization. Provider/model names are limited to 128 bytes, hashes to 64 hex digits, and dates to
valid ISO UTC values. Each imported generated file gets an XMP sidecar with the IPTC
`trainedAlgorithmicMedia` source type and Wabcut AI flag. Plaintext prompts are not retained in these
provenance fields (the existing local agent transcript may contain the user's prompt).

Decision: **unsigned JSON sidecar assertions, not embedded/signed Content Credentials**.
The [c2pa-c](https://github.com/contentauth/c2pa-c) C bindings wrap c2pa-rs and are dual Apache-2.0/MIT.
The official release API for `v0.26.13` returned **zero downloadable release assets** on 2026-10-11.
No trusted pinned Windows binary was available there; a Rust/C++ signing build was deferred under the
shared low-memory constraint. Added native signing binary/dependency size: **0 bytes**. Runtime/GGUF
download sizes are listed above; they are independent of C2PA. No speculative compiled C2PA size is
reported because it was not built.

GUI and CLI exports write `<output>.provenance.json` after successful export, with `claim_generator`,
`c2pa.actions`, an `edited with Wabcut` declaration and a custom segment assertion. Export ranges clip
and rebase the segment times. A new unsigned report also replaces a prior sidecar on a non-generated
re-export so stale AI flags do not survive. Sidecar failure warns and does not change export success.
The report lists enabled direct media clips; it is not an occlusion analysis or recursive nested-sequence
provenance resolver. `wabcut:unsigned: true` prevents mistaking it for a signature. It is a C2PA-style
manifest input structure; a validator cannot verify asset binding, identity or a signature yet.

Remaining signing step: pin and package c2pa-c for each platform, map assertions to the selected C2PA
spec version, hash/bind the final MP4, invoke a signer and verify with c2patool. Development can use an
explicitly labeled self-signed test certificate; distribution needs a user-supplied supported signing
certificate/key (or remote signer) and certificate-chain/trust configuration. Private signing keys
must stay in a keychain/signer. No test certificate, private key or trust claim ships in this fallback.

### AI-10 validation boundaries

`ai` covers model/governor, fake helper lifecycle, generation staging/links/undo, metadata validation
and provenance serialization. `ai_generation_net` covers scripted provider submit/poll/download,
auth scoping, no-cloud/consent/spend refusal, cancellation, malformed/quota responses, retry policy
and the managed Chat Completions adapter. `ai_generation_media` exercises a synthetic 24fps video
conformed to 25fps with resized output and reversed head. `ai_ui` includes offscreen screenshots
of the Generate estimate and the local model page. The default suites never download a model or spend provider credits. The separate `ai_llm_live`
suite requires `WABCUT_LIVE_LOCAL_LLM=1` and preinstalled verified assets for inference; paid smoke
requires `WABCUT_LIVE_GENERATION=1` plus a dedicated `WABCUT_LIVE_GEMINI_GENERATION_KEY` or
`WABCUT_LIVE_RUNWAY_GENERATION_KEY`. Both refuse to run when `CI` is set. Paid smoke caps are
$0.40 for a four-second Veo clip and $0.05 for one Runway image; these smokes have not been run here.

The local/cloud comparison replays one identical marker/repair task through each adapter: read the
sequence, receive a missing-time error, correct it, then finish with a staged marker. Fixture success
measures parser/tool-loop compatibility only. **Real local model reliability: unmeasured because resource admission refused launch;
real cloud model reliability: unmeasured (paid calls not authorized).** Do not label fixture replay
as a model benchmark. Final suite counts and any real local evaluation belong in the dispatch report;
source test presence alone does not mean a suite ran.

Verified on 2026-10-11 after merging master `33f8a1c`: the core `ai` executable passed
221 cases and 90,659 assertions, with three optional model/ONNX cases skipped;
`ai_generation_net` passed 10 cases and 205 assertions. The marker/repair fixture passed
1/1 task through the managed local adapter and 1/1 through the cloud adapter, each using
four recorded model responses. These numbers describe deterministic fixture integration,
not live model reliability or a statistically representative benchmark.


Full Windows acceptance on 2026-10-11, source `38aace4` (master base `33f8a1c`, frozen by
coordinator): `ai` 255 passed / 6 optional skips, 91,284 assertions; `ai_net` 41 cases /
21,090 assertions; `ai_generation_net` 10 cases / 205 assertions; `ai_generation_media`
1 case / 18 assertions; full `ai_ui` 14 cases / 689 assertions. After adding recovery-dialog
dismissal to screenshot capture only, the generation UI subset passed 2 cases / 101 assertions.
The optional skips require missing Whisper/SigLIP/detector binaries or model fixtures, not provider keys.

The hidden UI workflow checks refusal without consent, one fixture-provider submission after consent,
no project mutation before Accept, generated metadata import, and a single undo. A real CLI render of
that imported synthetic asset produced 50 frames, 160x90 at 25fps, two seconds, using libx264 and the
D3D11 renderer: MP4 16,381 bytes, unsigned segment manifest 1,108 bytes, source XMP 642 bytes.
No paid provider was invoked. Generate cost and local-model screenshots are in the dispatch artifacts;
the timeline AI badge hook/screenshot remains an explicitly accepted coordinator integration follow-up.

The Qwen 4B GGUF and official CPU archive were downloaded and SHA-256 reverified locally. Running
only `llama-server --version` returned `0.6.0-dev (build 11429, commit d81235049)`, Clang 20.1.8,
Windows x86_64; this does not exercise inference. Available RAM during the quiet-slot check was
4.43–5.24 GiB, below the model's 6.326 GiB admission requirement (6,792,248,416 bytes) and the
coordinator's 6.4 GiB evaluation floor. The actual model page reported
`not enough free RAM after reserving memory for playback` and disabled Start. No weights were
loaded, no model process remains, and the playback reserve was not weakened.

To rerun the bounded three-task local evaluation when memory permits, from this worktree in PowerShell:

```powershell
. ./tools/dev-env.ps1
$env:PATH = "$PWD/build/win-msvc/vcpkg_installed/x64-windows-release/bin;$env:PATH"
$env:WABCUT_CACHE_DIR = 'G:/Wabcut-AI10-cache'
$env:WABCUT_LIVE_GENERATION = '0'
$env:WABCUT_LIVE_LOCAL_LLM = '1'
& ./build/win-msvc/bin/wabcut_ai_llm_live_tests.exe '[eval]' --reporter compact
```

Do not run in CI or below the RAM floor. The test rehashes cached weights/runtime, uses CPU with
at most four threads and 8,192 context tokens, limits each task to five model steps/four minutes,
and unloads on exit. It grades actual EditAgent staged state: an Intro marker at frame 5 after a
sequence read, an Ending marker at frame 12 without changing clips, and a summary-only task with
no edits. Results include tool errors, exact prompts in test source, task status and timing in
`build/win-msvc/test-output/ai-local-eval.json`. Three tasks are a small smoke evaluation, not a
representative reliability benchmark. Until this is run, report no local success percentage.

## AI-15: local assistant tiers and constrained tool calls (as of 2026-10-13)

AI-15 was cut back on 2026-10-13, when Wabcut became gaming-first and AI work moved to G.2 (gameplay highlights).
What shipped is what the assistant needs to run on a small local model: the tiers, constrained tool calls and the
eval harness. The live per-tier measurement and signed Content Credentials are deferred (see the end of this
section).

### Local assistant tiers

Three allow-listed instruct models in GGUF, one per size, admitted by what each needs on this computer
now (`src/ai/llm/include/wabcut/ai/llm/Tiers.h`):

| Tier | Model (GGUF, Q4_K_M) | Licence | Weights | Context | KV cache (f16) |
|---|---|---|---|---|---|
| small | Qwen3 1.7B (`unsloth/Qwen3-1.7B-GGUF` @ `d7f544e`, base `Qwen/Qwen3-1.7B` @ `70d244c`) | Apache-2.0 | 1,107,409,472 B | 6,144 | 672 MiB |
| medium | Qwen3 4B Instruct 2507 (`unsloth/Qwen3-4B-Instruct-2507-GGUF` @ `a06e946`) | Apache-2.0 | 2,497,281,120 B | 8,192 | 1.13 GiB |
| large | Qwen3 8B (`Qwen/Qwen3-8B-GGUF` @ `7c41481`) | Apache-2.0 | 5,027,783,488 B | 8,192 | 1.13 GiB |

SHA-256 (the files ModelStore verifies): small `b139949c5bd74937ad8ed8c8cf3d9ffb1e99c866c823204dc42c0d91fa181897`,
medium `3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597`, large
`d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`. Qwen's own Qwen3-1.7B GGUF
repository ships only Q8_0 (1.8 GB), so the small tier uses Unsloth's Q4_K_M of the same weights; its model
card states Apache-2.0 like the base model. The 1.7B and 8B models are Qwen3 hybrid-thinking models: the
runtime starts them with `--reasoning off`, so they answer directly (no hidden thinking tokens to pay for).
Qwen2.5 1.5B/3B were considered: Qwen2.5-3B is under the Qwen Research licence (not allow-listed) and the
Qwen3 1.7B replaces Qwen2.5 1.5B at the same footprint.

**Admission** (`admit()`): the weights, the KV cache at the tier's context (layers x 2 x KV heads x head size x
2 bytes x tokens), 512 MiB of llama.cpp compute buffers, 512 MiB for the process and a 1 GiB reserve for the
editor. On the GPU the weights, cache and buffers must fit in its free memory with a 512 MiB margin (the
process and the reserve in RAM); on the CPU all of it must fit in free RAM. Unknown memory fails closed. GPU
threads: clamp(CPU threads / 4, 1, 4); CPU threads: clamp(CPU threads / 2, 1, 8). **Recommendation**
(`recommend()`): the largest tier that fits on the GPU; without a usable GPU the largest that fits in RAM and
still answers at a usable speed (the medium tier on the CPU only with 8+ threads; the large tier never on the
CPU by default). Settings > AI > Local assistant and `llm.local.models` show each tier's fit, its RAM / VRAM
and the recommended one; `llm.local.start` without a model starts the recommended tier.

**Tool calls on small models.** The local tiers decode with llama-server's `json_schema` (a GBNF grammar):
every reply is either `{"name": <one of the offered tools>, "parameters": {...}}` or `{"answer": "..."}`, so the
model cannot invent a tool or emit broken JSON (`src/ai/agent/ConstrainedTools.h`; tolerant parsing and one
repair turn for what still goes wrong). The tool list is slimmed for small context windows: `ToolRouter`
scores the catalog against the request (words and synonyms, command families, a timecode-range hint) and
offers 8 tools at 6k context, 12 at 8k and 16 at 16k, plus the always-on read tools.

### Eval harness

`tests/ai/eval`: 56 realistic editing requests over fixture projects (trim, ripple, markers, captions, audio,
colour, multicam, library), each with a deterministic success check on the staged ChangeSet and the verified
state. A run reports the success rate, the valid tool-call rate, latency per step, tokens per second and the
server's memory. `eval` label: an oracle model and a near-miss server run everywhere (they test the harness and
the constrained decoding, not a model). `perf` label: the real tiers, opt-in, never CI
(`WABCUT_EVAL_MODELS`, `WABCUT_EVAL_LLAMA`, `WABCUT_EVAL_TIERS`, `WABCUT_EVAL_BACKEND`, `WABCUT_EVAL_MODES`),
skipped without models. No media leaves the computer: llama-server listens on 127.0.0.1 and the fixtures hold no
media.

**No per-tier numbers yet.** The live runs need a quiet machine, and they were deferred with the gaming-first
re-scope before one could be scheduled, so this section claims no success rates. The cloud reference was not
measured either: no cloud keys are configured for Wabcut here, and the eval never uses keys it was not given.

### Content Credentials (AI-15, in 0.2)

Signed C2PA Content Credentials (c2pa-rs 0.91.2's C library, the pinned prebuilt release; a per-user signing
identity with its key in Windows Credential Manager, or the user's own certificate; a manifest with actions,
source-hash ingredients, AI segments and redactions; an export finalize step; `wabcut-cli credentials verify`),
built in AI-15 and parked for the gaming-first 0.1, are in exports since 0.2, with Premiere's Export Settings >
Content Credentials (the producer, a thumbnail of the export, a "do not train" request): docs/export.md "Content
Credentials". Exports that are not signed (off, or a failure to sign) keep the unsigned sidecar of the AI-10
decision above; a signed export gets none. Tests: `tests/ai/c2pa` (the identity, the keychain, the manifest's
contents, signature, trust, tamper detection, keys never in a file or a log; the producer and training request),
`tests/export/test_content_credentials.cpp` (signed MP4 exports that the c2pa reader validates - with the
producer, the thumbnail and the training assertion - and that fail after one changed byte; a failure to sign
leaves a complete unsigned file).

## Gameplay highlights (G.2, as of 2026-10-13)

The best moments of a gaming recording (OBS, ShadowPlay, Game Bar, Medal), found on this computer, with the
reasons in words: no model, no cloud, nothing uploaded (`src/ai/highlights`). Every signal is a measurement,
compared with what is usual for the same recording.

| Signal | Measured from | What counts |
|---|---|---|
| game audio | each audio stream, 16 kHz mono, 100 ms steps: RMS over 400 ms | the level above its 60 s moving median (+6 dB starts, +18 dB is full) |
| mic | the same, plus voicing and pitch (normalised autocorrelation, 70-500 Hz) and 3-7 Hz loudness bursts | the voice louder than the speaker's usual speaking level, higher than their usual pitch, shouting (both), laughter-like bursts |
| visual | the picture decoded small (160 px wide, every frame) | motion above its moving median, brightness flashes, bursts of picture changes (cuts, respawns, menus) |
| chat | a Twitch (TwitchDownloader) / YouTube (yt-dlp live_chat.json) / chat-downloader log, imported once per recording | the message rate over its moving median, emote and keyword hits (KEKW, POGGERS, "clip it", ... and your own words), counted 6 s earlier (viewers react late) |
| markers | chapter markers in the recording (OBS "Add Chapter Marker" in hybrid MP4 / MKV) and a marker file next to it (`<file>.markers.txt / .csv / .json`, `<stem>_markers.*`) | a hotkey pressed after the play lifts the 15 s before it |

With several audio streams (OBS multi-track), the most voice-like one is the mic and the others are game audio;
one stream is "mixed" (game spikes from its level, the voice from its voiced parts). `audioRoles` overrides
this per stream.

**Fusion.** Each signal becomes an activation (0..1). The fused score is their weighted mean (mic 1.2, game 1.0,
chat 1.0, visual 0.8; only the signals present count), plus 0.15 for each further signal that agrees (>= 0.35),
plus 0.3 near a marker, smoothed over 1.5 s. Peaks over `minScore` (0.25) at least `minGap` (20 s) apart
become moments. Each moment keeps the build-up (4-12 s before the peak) and 2-6 s after it, and moments never
overlap. **Confidence** rises with the number of agreeing signals: high, medium or low. **Reasons** quote the
measurements: "game audio +13 dB over the moments before", "you shouted (+11 dB, pitch +4 semitones)", "you
laughed", "3 flashes, fast action (2.6x the usual motion)", "chat 6x busier (38 messages in 5 s), 'KEKW' x14",
"OBS chapter marker 'clutch'".

**Commands** (docs/commands.md):
- `highlights.find` marks the moments on the sequence (or a media item's clip markers): "Highlight 1: You shouted +
  Game audio spike", with the score, confidence and reasons in the comment. It is one undo step, and earlier
  highlight markers are replaced. The result has, per moment, the range, the peak (the thumbnail time), the score,
  the confidence, the reasons and the activation of each signal.
- `highlights.importChat` keeps a chat log with a recording's analyses. It is the one command that reads a file
  the caller names, so the assistant never runs it itself (S5-P1-1).
- `highlights.toReel` makes a new sequence from the marked (or given) moments: handles, time or score order, a count
  or length budget, transitions with audio crossfades, an optional slow-motion part around each peak (50 % clip
  speed: its audio plays slowed) and, with `music`, cuts on its beats (every `beatsPerCut` beats) with the music under the reel.
  It is one undo step.
- `highlights.toShort` makes one moment a 9:16 sequence through shorts.create: blurred fill by default, so the whole
  game view stays visible, plus captions, a hook title and loudness. The facecam/gameplay layouts arrive with the
  core's `layout.apply`.

The assistant can run all of these except importChat. On a small local model, the tool router offers them for
requests about highlights, reels, montages, moments, clutches and TikToks.

**Measured once, cached.** The measurements (per audio stream and the picture) are AnalysisCache entries by
source fingerprint ("highlights.audio" / "highlights.video"). Another run, with other signals, another chat log or
other settings, decodes nothing. An imported chat ("highlights.chat") and the moments found in a recording
("highlights") are cached the same way. The moments are markers with C.15's `Marker::highlight` (score, confidence,
reasons, peak, source "highlights"). C.16's dead-time removal (`sequence.removeDeadTime` in wabcut-cli) reads a
measured recording's activity from these measurements (`cachedActivity`: the loudest stream's level and the picture's
motion, 10 a second) instead of decoding it again, and the assistant may make vertical or square versions
(`sequence.makeVersion`, an additive command like highlights.toShort).

### Accuracy (synthetic gameplay)

Measured with the real feature extraction, not with handed-in activations.

**The synthetic gameplay fixture** (`tests/ai/highlights/SyntheticGameplay.h`) is 8 minutes long, with 9 scripted moments and 8 decoys:
- Most moments mix the signals; some leave signals out: no chat, no picture, a shout over a quiet game, a laugh
  with only chat.
- The decoys are lone game sounds (+8 dB), lone flashes and short bursts of chat.

On it, precision and recall are both **1.00**:
- with all signals,
- with audio + picture,
- with audio only.

All 9 moments are found and no decoy is picked.

**A decoded MKV written by FFmpeg** (`tests/ai/highlights/test_highlights_media.cpp`) is 150 s long. It has a game
track and a mic track, two OBS chapter markers and a marker file next to it, with 4 moments and 3 decoys.

On it, precision and recall are **1.00**. Measuring the two audio streams and the picture took 4.5 s, about 33x real
time on this machine while other builds ran. The second run reads the cache and decodes nothing.

End to end (`tests/export/test_highlights_e2e.cpp`), the test starts from a 120 s 720p30 OBS-style MKV with game and
mic tracks and chapter markers:
- highlights.find finds 3 of 3 moments and no decoy;
- highlights.toReel makes a 26 s reel with crossfades and slow-motion peaks;
- the reel is rendered to a YouTube 1080p MP4 in 13.7 s and checked with ffprobe.

These numbers come from synthetic recordings. They show that the pipeline finds what it is built to find. They do not
show how well it matches a player's taste on real, noisier gameplay: music, voice chat, constant explosions. No
labelled real recordings were available to measure on.
