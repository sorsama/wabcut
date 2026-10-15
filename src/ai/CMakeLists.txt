# wabcut_ai — pure C++ (no Qt): AI capability interfaces, model registry, transcript data, analysis cache, consent/spend.
# Owners: AI platform engineer (providers/registry/secrets/consent) + Local-AI engineer (Transcript, TranscriptionService,
# AnalysisCache, ModelStore). Contract: docs/ai.md. Network goes through an injected IHttpTransport (Qt impl in src/ai_net).
find_package(nlohmann_json CONFIG REQUIRED)
add_library(wabcut_ai STATIC
    # Local-AI engineer: transcripts, analysis cache, model store, the wabcut-infer protocol and process
    src/AnalysisCache.cpp
    src/Captions.cpp
    src/ChildProcess.cpp
    src/InferProtocol.cpp
    src/ModelStore.cpp
    src/Sha256.cpp
    src/Text.cpp
    src/Timestamps.cpp
    src/TranslationManifest.cpp
    src/Transcript.cpp
    src/TranscriptJson.cpp
    src/TranscriptRedaction.cpp
    src/VadChunker.cpp
    src/WordBuilder.cpp)
target_include_directories(wabcut_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai PUBLIC wabcut_model PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai)

# AI platform engineer: errors, chat types, capability interfaces, IHttpTransport (+ SSE/NDJSON decoding, retry,
# redaction, request log), the model registry (data/models.json, compiled in), keys, consent and spend caps.
# Chat messages, tool specs and schemas are JSON values, so nlohmann_json is part of the public API.
set(wabcut_ai_models_json ${CMAKE_CURRENT_SOURCE_DIR}/data/models.json)
set(wabcut_ai_models_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/ModelsJson.cpp)
add_custom_command(
    OUTPUT ${wabcut_ai_models_cpp}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${wabcut_ai_models_json} -DOUTPUT=${wabcut_ai_models_cpp}
            -DSYMBOL=kModelsJson -P ${CMAKE_CURRENT_SOURCE_DIR}/EmbedFile.cmake
    DEPENDS ${wabcut_ai_models_json} ${CMAKE_CURRENT_SOURCE_DIR}/EmbedFile.cmake
    COMMENT "Embedding src/ai/data/models.json")
target_sources(wabcut_ai PRIVATE
    src/AiError.cpp
    src/Chat.cpp
    src/ConsentLedger.cpp
    src/Generation.cpp
    src/Provenance.cpp
    src/Http.cpp
    src/HttpDownload.cpp
    src/JsonAccess.cpp
    src/ModelRegistry.cpp
    src/Provider.cpp
    src/Redact.cpp
    src/Redirects.cpp
    src/RequestLog.cpp
    src/Secrets.cpp
    src/SpendGuard.cpp
    src/ToolSchema.cpp
    src/Transport.cpp
    ${wabcut_ai_models_cpp})
target_link_libraries(wabcut_ai PUBLIC nlohmann_json::nlohmann_json)

# wabcut_ai_transcribe — TranscriptionService (local whisper), TranscribeService (local / cloud / cache routing,
# shared by the app, the transcript.transcribe command and the CLI) and AudioEnergyService (per-range RMS/peak/speech
# jobs): they decode media (wabcut_media's AudioDecoder at 16 kHz, source fingerprints, media::Job), so they exist
# in full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_transcribe STATIC src/TranscriptionService.cpp src/TranscribeService.cpp src/AudioEnergy.cpp)
    target_link_libraries(wabcut_ai_transcribe PUBLIC wabcut_ai wabcut_media)
    wabcut_target_defaults(wabcut_ai_transcribe)
endif()

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_analysis (+ wabcut_ai_analysis_media in full builds) — shot and beat detection for editing (C.5).
# Owner: Core engine. See src/ai/analysis/CMakeLists.txt. Before `edit`, whose catalog includes its commands.
add_subdirectory(llm)
add_subdirectory(generation)
add_subdirectory(analysis)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_search (+ wabcut_ai_search_media in full builds) — smart search over footage and transcripts (AI-8).
# Owner: AI platform engineer. See src/ai/search/CMakeLists.txt. Before `edit`, whose catalog includes its commands.
add_subdirectory(search)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_vision (+ wabcut_ai_vision_media in full builds) — faces, people, tracks and saliency over time for auto
# reframe and the shorts finder (AI-9). Owner: AI platform engineer. See src/ai/vision/CMakeLists.txt.
add_subdirectory(vision)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_segment (+ wabcut_ai_segment_media in full builds) - the AI Object Mask: SAM 2.1 mattes tracked through a
# shot and the mask.objectSelect / refine / propagate commands (AI-14). Owner: AI platform engineer. See
# src/ai/segment/CMakeLists.txt. Before `edit`, whose catalog includes its commands.
add_subdirectory(segment)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_audio (+ wabcut_ai_audio_media in full builds) � Enhance Speech (DeepFilterNet3) and stem separation
# (HTDemucs) through wabcut-embed (A.4). Owner: Audio / Local-AI engineer. See src/ai/audio/CMakeLists.txt.
add_subdirectory(audio)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_reframe — auto reframe: the virtual camera, the reframed sequence and its commands (AI-9).
# Owner: AI platform engineer. See src/ai/reframe/CMakeLists.txt. Before `edit`, whose catalog includes its commands.
add_subdirectory(reframe)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_edit (+ wabcut_ai_edit_media in full builds) — transcript-first editing: text edits as catalog
# commands, filler/silence review, captions. Owner: Local-AI engineer (AI-3). See src/ai/edit/CMakeLists.txt.
add_subdirectory(edit)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_shorts — the shorts finder and shorts.find / shorts.create (AI-9). Owner: AI platform engineer.
# See src/ai/shorts/CMakeLists.txt (it and wabcut_ai_edit depend on each other).
add_subdirectory(shorts)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_highlights (+ _media in full builds) - gameplay highlights (G.2): highlights.find / toReel / toShort.
# Owner: AI platform engineer. See src/ai/highlights/CMakeLists.txt (it and wabcut_ai_edit depend on each other).
add_subdirectory(highlights)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_roughcut — rough cut from a script and retake comparison (AI-11); wabcut_ai_autozoom (+ _media in full
# builds) — auto zoom for screen recordings (AI-11). Owner: AI platform engineer. See their CMakeLists.txt.
add_subdirectory(roughcut)
add_subdirectory(autozoom)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_translate - caption translation (AI-12). Owner: AI platform engineer. See src/ai/translate/CMakeLists.txt.
add_subdirectory(translate)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_dub - AI voice-over, dubbing and narration (AI-12). Owner: AI platform engineer. See src/ai/dub/CMakeLists.txt.
add_subdirectory(dub)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_multicam / wabcut_ai_workflow - AI multicam switching and the podcast workflow (AI-13). Owner: AI platform
# engineer. See src/ai/multicam/CMakeLists.txt and src/ai/workflow/CMakeLists.txt.
add_subdirectory(multicam)
add_subdirectory(workflow)

# ---------------------------------------------------------------------------------------------------------------
# wabcut_ai_agent — the editing assistant: tool loop, staging, verification, quick actions, guarded apply (AI-5).
# Owner: AI platform engineer. See src/ai/agent/CMakeLists.txt.
add_subdirectory(agent)
