add_executable(wabcut_fake_llama fake_llama.cpp)
wabcut_target_defaults(wabcut_fake_llama)
if(WIN32)
    target_link_libraries(wabcut_fake_llama PRIVATE ws2_32)
endif()
# Local-AI engineer: Transcript, TranscriptionService, AnalysisCache, ModelStore (docs/ai.md). Never downloads a model.
add_executable(wabcut_fake_infer fake_infer.cpp)
target_link_libraries(wabcut_fake_infer PRIVATE wabcut_ai)
wabcut_target_defaults(wabcut_fake_infer)

set(wabcut_ai_test_sources
    test_generation.cpp
    test_llm_lifecycle.cpp
    test_tiers.cpp # AI-15: local assistant tiers, admission and launch
    AiTestUtil.h
    test_cache_store.cpp
    test_chunks.cpp
    test_process.cpp
    test_protocol.cpp
    test_transcript.cpp
    test_words.cpp)
set(wabcut_ai_test_libs wabcut_ai wabcut_ai_llm)
# AI platform engineer: providers, registry, keys, consent, spend, HTTP plumbing (no network: FakeTransport).
list(APPEND wabcut_ai_test_sources
    FakeTransport.h
    test_platform_http.cpp
    test_platform_policy.cpp
    test_platform_registry.cpp
    test_security.cpp) # AI-6: security and robustness regressions (core part)
list(APPEND wabcut_ai_test_libs wabcut_command) # every command of the catalog must be a portable tool
if(NOT WABCUT_CORE_ONLY) # TranscriptionService decodes media: full builds only
    list(APPEND wabcut_ai_test_sources test_real_infer.cpp test_transcribe_service.cpp test_transcription_service.cpp)
    list(APPEND wabcut_ai_test_libs wabcut_ai_transcribe)
endif()

# AI-3 transcript-first editing (wabcut_ai_edit): mapping, text edits, review, captions, the transcript commands.
# Timeline pictures come from the model suite's fixture, the command harness from the command suite.
list(APPEND wabcut_ai_test_sources
    ../model/TimelineFixture.cpp
    ../model/TimelineFixture.h
    EditTestUtil.h
    test_caption_builder.cpp
    test_caption_transcript.cpp
    test_review.cpp
    test_sequence_transcript.cpp
    test_text_edits.cpp
    test_transcribe_command.cpp
    test_transcript_commands.cpp
    test_transcript_review.cpp
    test_profanity.cpp)
list(APPEND wabcut_ai_test_libs wabcut_ai_edit)
if(NOT WABCUT_CORE_ONLY) # the decoder-backed probe, the analysis-cache provider and wabcut-cli
    list(APPEND wabcut_ai_test_sources test_media_services.cpp)
    list(APPEND wabcut_ai_test_libs wabcut_ai_edit_media)
endif()

# C.5 media analysis (wabcut_ai_analysis): shot and beat detection, the analysis and rhythm commands. The media
# tests decode FFmpeg-generated fixtures (analysis/gen_analysis_media.py): ctest generates them once in a fixture step
# before any ai test, so parallel runs (ctest -j N) never race on them; run outside ctest, the tests generate them on
# demand (the script is safe in several processes at once).
list(APPEND wabcut_ai_test_sources
    analysis/AnalysisTestUtil.h
    analysis/test_analysis_commands.cpp
    analysis/test_beat_detection.cpp
    analysis/test_shot_detection.cpp)
list(APPEND wabcut_ai_test_libs wabcut_ai_analysis)
set(wabcut_ai_test_properties "")
if(NOT WABCUT_CORE_ONLY)
    list(APPEND wabcut_ai_test_sources analysis/test_media_analysis.cpp)
    list(APPEND wabcut_ai_test_libs wabcut_ai_analysis_media)
    # FFmpeg from the vcpkg tree, run through Python.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(WABCUT_ANALYSIS_FFMPEG NAMES ffmpeg
        HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/ffmpeg")
    set(analysis_ffmpeg_arg "")
    if(WABCUT_ANALYSIS_FFMPEG)
        set(analysis_ffmpeg_arg --ffmpeg "${WABCUT_ANALYSIS_FFMPEG}")
    endif()
    add_test(NAME ai.generate_analysis_media
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/analysis/gen_analysis_media.py"
                --out "${CMAKE_BINARY_DIR}/test-output/ai/analysis-media" ${analysis_ffmpeg_arg})
    set_tests_properties(ai.generate_analysis_media PROPERTIES
        FIXTURES_SETUP wabcut_analysis_media
        TIMEOUT 600
        LABELS ai)
    set(wabcut_ai_test_properties FIXTURES_REQUIRED wabcut_analysis_media)
endif()

# AI-5 editing assistant (wabcut_ai_agent): the tool loop with a scripted model, staging, verification, apply.
list(APPEND wabcut_ai_test_sources
    AgentTestUtil.h
    test_agent.cpp
    test_constrained_tools.cpp # AI-15: tool routing and constrained tool calls
    test_library_agent.cpp
    test_graphics_agent.cpp
    test_agent_files.cpp
    test_agent_verification.cpp)
list(APPEND wabcut_ai_test_libs wabcut_ai_agent)

# AI-8 smart search (wabcut_ai_search): fixture embeddings and media, a scripted wabcut-embed (fake_embed), never a
# model download; the real-model smoke test runs only when WABCUT_TEST_SIGLIP_DIR names the downloaded model.
add_executable(wabcut_fake_embed search/fake_embed.cpp)
target_link_libraries(wabcut_fake_embed PRIVATE wabcut_ai_search)
wabcut_target_defaults(wabcut_fake_embed)
list(APPEND wabcut_ai_test_sources
    search/SearchTestUtil.h
    search/test_search_basics.cpp
    search/test_search_index.cpp
    search/test_search_service.cpp)
list(APPEND wabcut_ai_test_libs wabcut_ai_search)

# AI-9 subject analysis (wabcut_ai_vision): detector decoding on fixture tensors, tracking, saliency, the sampling
# pipeline on scripted videos and HelperDetector against the scripted wabcut-embed; never a model download. Auto
# reframe (wabcut_ai_reframe): the camera path on synthetic trajectories, the reframed sequence and its commands. The
# shorts finder (wabcut_ai_shorts) on a synthetic podcast, its guarded cloud re-rank and shorts.create.
list(APPEND wabcut_ai_test_sources
    vision/VisionTestUtil.h
    vision/test_vision_detection.cpp
    vision/test_vision_subjects.cpp
    reframe/test_derived_from.cpp
    reframe/test_camera_path.cpp
    reframe/test_reframe.cpp
    shorts/ShortsTestUtil.h
    shorts/test_shorts.cpp
    shorts/test_shorts_agent.cpp
    vision/test_vision_model.cpp) # the real detectors: WABCUT_TEST_DETECT_DIR (never downloaded here)
list(APPEND wabcut_ai_test_libs wabcut_ai_vision wabcut_ai_reframe wabcut_ai_shorts)

# G.2 gameplay highlights (wabcut_ai_highlights): the measurements on synthetic sound and pictures, chat logs and
# marker files, the fusion (precision / recall on synthetic gameplay with scripted moments and decoys) and the
# highlights.* commands.
list(APPEND wabcut_ai_test_sources highlights/SyntheticGameplay.h highlights/test_highlights.cpp)
list(APPEND wabcut_ai_test_libs wabcut_ai_highlights)
if(NOT WABCUT_CORE_ONLY) # decoded gameplay: an MKV that FFmpeg writes, measured, cached and searched
    list(APPEND wabcut_ai_test_sources highlights/test_highlights_media.cpp)
    list(APPEND wabcut_ai_test_libs wabcut_ai_highlights_media)
endif()
if(NOT WABCUT_CORE_ONLY) # decoded subject analysis (the 1080p throughput figure)
    list(APPEND wabcut_ai_test_libs wabcut_ai_vision_media)
    set(wabcut_ai_test_vision_media ON)
endif()

# A.4 local audio AI (wabcut_ai_audio): DeepFilterNet3's signal processing, the chunk plans and the helper protocol
# against the scripted wabcut-embed; the jobs on decoded fixtures (full builds); the real models only when
# WABCUT_TEST_AUDIO_MODELS names them (never downloaded here).
list(APPEND wabcut_ai_test_sources audio/test_deepfilter.cpp audio/test_audio_plans.cpp audio/test_audio_helper.cpp)
list(APPEND wabcut_ai_test_libs wabcut_ai_audio)
if(NOT WABCUT_CORE_ONLY)
    list(APPEND wabcut_ai_test_sources audio/test_audio_jobs.cpp audio/test_audio_models.cpp)
    list(APPEND wabcut_ai_test_libs wabcut_ai_audio_media)
endif()

# AI-15 signed Content Credentials (wabcut_ai_c2pa): the signing identity and its keychain entry, the manifest of an
# export, signing, verification, trust and tamper detection with the pinned c2pa-rs library (skipped without it).
list(APPEND wabcut_ai_test_sources c2pa/test_c2pa.cpp)
list(APPEND wabcut_ai_test_libs wabcut_ai_c2pa)

wabcut_add_test_suite(ai SOURCES ${wabcut_ai_test_sources} LIBS ${wabcut_ai_test_libs}
    PROPERTIES ${wabcut_ai_test_properties})
target_include_directories(wabcut_ai_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/ai/c2pa/src) # Der.h (the test checks it)

# Video stabilization (C.15 parked, finished in P.2; wabcut_ai_stabilize): global motion analysis on synthetic shaky
# clips (the masks suite's textures, ../masks/SyntheticMotion.h), Gaussian and L1 smoothing, No Motion, framing, the
# rolling shutter's scan share, the analysis JSON, and effect.analyzeStabilization / effect.analyzeRollingShutter
# through the CommandEngine with fake pictures. Its own suite (label ai_stabilize) so it runs without the AI helpers.
wabcut_add_test_suite(ai_stabilize
    SOURCES
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        stabilize/test_stabilize.cpp
        stabilize/test_stabilize_commands.cpp
    LIBS wabcut_ai_stabilize wabcut_ai_edit
    PROPERTIES LABELS ai_stabilize)
target_include_directories(wabcut_ai_stabilize_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command)
target_compile_definitions(wabcut_ai_stabilize_tests PRIVATE WABCUT_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output")
add_dependencies(wabcut_ai_tests wabcut_fake_infer wabcut_fake_embed wabcut_fake_llama)
if(WIN32)
    target_link_libraries(wabcut_ai_tests PRIVATE ws2_32) # review wave 4 P2-2: an impostor loopback listener
endif()
target_compile_definitions(wabcut_ai_tests PRIVATE WABCUT_FAKE_LLAMA="$<TARGET_FILE:wabcut_fake_llama>")
if(wabcut_ai_test_vision_media)
    target_compile_definitions(wabcut_ai_tests PRIVATE WABCUT_TEST_VISION_MEDIA=1
                                                       WABCUT_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output")
endif()
target_compile_definitions(wabcut_ai_tests PRIVATE WABCUT_FAKE_INFER="$<TARGET_FILE:wabcut_fake_infer>"
                                                   WABCUT_FAKE_EMBED="$<TARGET_FILE:wabcut_fake_embed>")
if(TARGET wabcut-embed)
    add_dependencies(wabcut_ai_tests wabcut-embed)
    target_compile_definitions(wabcut_ai_tests PRIVATE WABCUT_EMBED_HELPER="$<TARGET_FILE:wabcut-embed>")
endif()
target_include_directories(wabcut_ai_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command)
if(NOT WABCUT_CORE_ONLY)
    target_compile_definitions(wabcut_ai_tests PRIVATE WABCUT_CLI_PATH="$<TARGET_FILE:wabcut-cli>")
    add_dependencies(wabcut_ai_tests wabcut-cli)
    # The analysis fixtures, for runs outside ctest (see above).
    target_compile_definitions(wabcut_ai_tests PRIVATE
        WABCUT_PYTHON="${Python3_EXECUTABLE}"
        WABCUT_FFMPEG="${WABCUT_ANALYSIS_FFMPEG}"
        WABCUT_ANALYSIS_MEDIA_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/analysis/gen_analysis_media.py")
endif()
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(wabcut_ai_tests PRIVATE nlohmann_json::nlohmann_json)
# The search index tests plant graph files with their hash in the index database (sqlite3).
find_package(SQLite3 REQUIRED)
if(TARGET SQLite3::SQLite3)
    target_link_libraries(wabcut_ai_tests PRIVATE SQLite3::SQLite3)
else()
    target_link_libraries(wabcut_ai_tests PRIVATE SQLite::SQLite3)
endif()
if(WABCUT_WITH_LOCAL_AI)
    add_dependencies(wabcut_ai_tests wabcut-infer)
    target_compile_definitions(wabcut_ai_tests PRIVATE WABCUT_REAL_INFER="$<TARGET_FILE:wabcut-infer>")
endif()

# AI-11 creator superpowers (wabcut_ai_roughcut, wabcut_ai_autozoom): text matching and scripts, the rough cut from a
# script (English and Thai fixtures, accuracy, the 60-minute benchmark), retake grouping and "use this take", auto zoom on
# a synthetic screen recording, the commands through the engine and the assistant. Its own suite (label ai_creator), so
# the feature builds and runs without relinking the large ai suite. No model is downloaded, no cloud call is made.
wabcut_add_test_suite(ai_creator
    SOURCES
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        AgentTestUtil.h
        EditTestUtil.h
        FakeTransport.h
        creator/CreatorTestUtil.h
        creator/test_autozoom.cpp
        creator/test_creator_agent.cpp
        creator/test_roughcut.cpp
        creator/test_takes.cpp
        creator/test_text_match.cpp
    LIBS wabcut_ai_roughcut wabcut_ai_autozoom wabcut_ai_agent wabcut_ai_edit)
target_include_directories(wabcut_ai_creator_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command
    ${CMAKE_SOURCE_DIR}/tests/ai)
target_link_libraries(wabcut_ai_creator_tests PRIVATE nlohmann_json::nlohmann_json)

# AI-12 going global (wabcut_ai_translate, wabcut_ai_mt, wabcut_ai_dub): re-segmentation to each language's reading
# speed (Thai between words), "[bleep]" kept, corrections before translation, the OPUS-MT tokenizer and the local
# translator on the scripted helper, time-fit, dub / narration runs and their one-step edit, the commands and their
# refusals. Its own suite (label ai_global): no model is downloaded and no cloud call is made.
wabcut_add_test_suite(ai_global
    SOURCES
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        AgentTestUtil.h
        EditTestUtil.h
        global/GlobalTestUtil.h
        global/test_dub.cpp
        global/test_global_agent.cpp
        global/test_marian_tokenizer.cpp
        global/test_translation.cpp
    LIBS wabcut_ai_dub wabcut_ai_translate wabcut_ai_mt wabcut_ai_agent wabcut_ai_edit)
target_include_directories(wabcut_ai_global_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command
    ${CMAKE_SOURCE_DIR}/tests/ai)
target_link_libraries(wabcut_ai_global_tests PRIVATE nlohmann_json::nlohmann_json)
add_dependencies(wabcut_ai_global_tests wabcut_fake_embed)
target_compile_definitions(wabcut_ai_global_tests PRIVATE WABCUT_FAKE_EMBED="$<TARGET_FILE:wabcut_fake_embed>")

# AI-13 podcast and interview editing (wabcut_ai_multicam, wabcut_ai_workflow): the speaker timeline from transcripts,
# microphone levels and faces (crosstalk, silence, reactions; each cue alone), the switching rules (minimum hold, never
# mid-word, wide on crosstalk, variety, lead, reactions, presets), multicam.autoSwitch as one undo step of ordinary
# angle cuts, the podcast workflow step by step (skippable, undoable, resumable from history), the commands and the
# assistant, and the 60-minute benchmark. Its own suite (label ai_multicam): no model, no media, no cloud.
wabcut_add_test_suite(ai_multicam
    SOURCES
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        AgentTestUtil.h
        EditTestUtil.h
        multicam/MulticamTestUtil.h
        multicam/PodcastTestUtil.h
        multicam/test_autoswitch.cpp
        multicam/test_multicam_agent.cpp
        multicam/test_multicam_perf.cpp
        multicam/test_podcast.cpp
        multicam/test_speakers.cpp
    LIBS wabcut_ai_workflow wabcut_ai_multicam wabcut_ai_agent wabcut_ai_edit)
target_include_directories(wabcut_ai_multicam_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command
    ${CMAKE_SOURCE_DIR}/tests/ai)
target_link_libraries(wabcut_ai_multicam_tests PRIVATE nlohmann_json::nlohmann_json)

# AI-13 on media: microphone levels decoded from real WAV files and their decode speed; the podcast workflow end to end
# on real files (cameras made with FFmpeg, synced by sound, switched on the decoded levels). Full builds.
if(TARGET wabcut_ai_multicam_media)
    wabcut_add_test_suite(ai_multicam_media
        SOURCES
            ../model/TimelineFixture.cpp
            ../model/TimelineFixture.h
            multicam/MulticamTestUtil.h
            multicam/test_media_signals.cpp
            multicam/test_podcast_media.cpp
        LIBS wabcut_ai_multicam_media wabcut_ai_workflow wabcut_ai_edit wabcut_audio_edit_media wabcut_media)
    target_include_directories(wabcut_ai_multicam_media_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command
        ${CMAKE_SOURCE_DIR}/tests/ai)
    target_link_libraries(wabcut_ai_multicam_media_tests PRIVATE nlohmann_json::nlohmann_json)
    if(WABCUT_ANALYSIS_FFMPEG)
        target_compile_definitions(wabcut_ai_multicam_media_tests PRIVATE WABCUT_MULTICAM_FFMPEG="${WABCUT_ANALYSIS_FFMPEG}")
    endif()
endif()

# AI-14 AI Object Mask (wabcut_ai_segment): the matte codec, tracks and the cache key, propagation through a shot
# (stopping at its cuts), SAM 2.1 through a scripted wabcut-embed (crash, timeout, cancel), mask.objectSelect /
# mask.refine / mask.propagate through the engine (dry run, cache, undo, subjects), and the assistant. Its own suite
# (label ai_segment): no model, no media, no cloud.
wabcut_add_test_suite(ai_segment
    SOURCES
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        AgentTestUtil.h
        segment/SegmentTestUtil.h
        segment/test_helper_segmenter.cpp
        segment/test_matte.cpp
        segment/test_object_mask_commands.cpp
        segment/test_propagation.cpp
    LIBS wabcut_ai_segment wabcut_ai_agent wabcut_ai_edit)
target_include_directories(wabcut_ai_segment_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command
    ${CMAKE_SOURCE_DIR}/tests/ai)
target_link_libraries(wabcut_ai_segment_tests PRIVATE nlohmann_json::nlohmann_json)
add_dependencies(wabcut_ai_segment_tests wabcut_fake_embed)
target_compile_definitions(wabcut_ai_segment_tests PRIVATE WABCUT_FAKE_EMBED="$<TARGET_FILE:wabcut_fake_embed>")

# AI-14 on media: decoded pictures for the model (forward and backward), the compositor's matte source (redactions fail
# closed), mask.objectSelect end to end on a real file; and the opt-in smoke test of the real SAM 2.1 tiny
# (WABCUT_TEST_SAM2_DIR): IoU and frames per second at 1080p on DirectML and the CPU. Full builds.
if(TARGET wabcut_ai_segment_media)
    wabcut_add_test_suite(ai_segment_media
        SOURCES
            ../model/TimelineFixture.cpp
            ../model/TimelineFixture.h
            segment/SegmentTestUtil.h
            segment/test_segment_media.cpp
        LIBS wabcut_ai_segment_media wabcut_ai_edit wabcut_media)
    target_include_directories(wabcut_ai_segment_media_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command
        ${CMAKE_SOURCE_DIR}/tests/ai)
    target_link_libraries(wabcut_ai_segment_media_tests PRIVATE nlohmann_json::nlohmann_json)
    add_dependencies(wabcut_ai_segment_media_tests wabcut_fake_embed)
    target_compile_definitions(wabcut_ai_segment_media_tests PRIVATE WABCUT_FAKE_EMBED="$<TARGET_FILE:wabcut_fake_embed>")
    if(TARGET wabcut-embed)
        add_dependencies(wabcut_ai_segment_media_tests wabcut-embed)
        target_compile_definitions(wabcut_ai_segment_media_tests PRIVATE WABCUT_EMBED_HELPER="$<TARGET_FILE:wabcut-embed>")
    endif()
    if(WABCUT_ANALYSIS_FFMPEG)
        target_compile_definitions(wabcut_ai_segment_media_tests PRIVATE WABCUT_SEGMENT_FFMPEG="${WABCUT_ANALYSIS_FFMPEG}")
    endif()
endif()

# AI-12 on media: FFmpeg decoding of the providers' speech, the WSOLA stretch, the dub's WAV; and the opt-in OPUS-MT
# benchmark on the real wabcut-embed (WABCUT_TEST_TRANSLATION_MODELS=1). Full builds.
if(TARGET wabcut_ai_dub_media)
    wabcut_add_test_suite(ai_global_media SOURCES global/GlobalTestUtil.h global/test_global_media.cpp
        LIBS wabcut_ai_dub_media wabcut_ai_mt)
    target_include_directories(wabcut_ai_global_media_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/ai)
    if(TARGET wabcut-embed)
        add_dependencies(wabcut_ai_global_media_tests wabcut-embed)
        target_compile_definitions(wabcut_ai_global_media_tests PRIVATE WABCUT_EMBED_HELPER="$<TARGET_FILE:wabcut-embed>")
    endif()
    if(TARGET wabcut_ai_net)
        target_link_libraries(wabcut_ai_global_media_tests PRIVATE wabcut_ai_net Qt6::Core)
        target_compile_definitions(wabcut_ai_global_media_tests PRIVATE WABCUT_NET_DOWNLOAD=1)
    endif()
endif()

# AI platform engineer: wire-adapter contract tests on recorded fixtures (tests/ai/fixtures), the Qt transport against
# a local loopback server, and opt-in live smoke tests (WABCUT_LIVE_<PROVIDER>_KEY; never set in CI). Full builds.
if(TARGET wabcut_ai_net)
    # AI-5: the editing assistant over each chat adapter on recorded sessions (fixtures/agent), on Timeline pictures.
    wabcut_add_test_suite(ai_net
        SOURCES
            ../model/TimelineFixture.cpp
            ../model/TimelineFixture.h
            FakeTransport.h
            NetTestSupport.h
            test_agent_providers.cpp
            test_net_chat.cpp
            test_net_live.cpp
            test_net_live_speech.cpp # AI-12: the voice providers (WABCUT_LIVE_*_KEY)
            test_net_security.cpp
            test_net_qt_transport.cpp
            test_net_redirects.cpp # review wave 4, P1-4: CDN redirects of downloads
            test_net_transcribers.cpp
        LIBS wabcut_ai_net wabcut_ai_agent Qt6::Core Qt6::Network)
    target_compile_definitions(wabcut_ai_net_tests PRIVATE WABCUT_AI_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    target_include_directories(wabcut_ai_net_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command)
endif()

# AI platform engineer: the AiController bridge and Preferences > AI screenshots, on the UI shell fixture of
# tests/app (compiled here unchanged). Screenshots land in <build>/test-output/ui/.
if(TARGET wabcut_uiplugin)
    # AI-5: the assistant panel (AgentController) on the real Main.qml with a scripted model.
    wabcut_add_test_suite(ai_ui
        SOURCES
            ${CMAKE_SOURCE_DIR}/tests/app/ShellFixture.cpp
            ${CMAKE_SOURCE_DIR}/tests/app/ShellFixture.h
            AgentTestUtil.h
            test_agent_ui.cpp
            test_ai_lock_ui.cpp
            test_ai_ui.cpp
            test_generation_ui.cpp
            search/SearchTestUtil.h
            test_search_ui.cpp # AI-8: smart search in the Library
            shorts/ShortsTestUtil.h
            test_shorts_ui.cpp # AI-9: the Shorts panel and the Reframe dialog
            creator/CreatorTestUtil.h
            creator/ScreenRecording.h
            test_creator_ui.cpp # AI-11: the Rough cut sheet, the Takes panel and the Auto zoom dialog
            global/GlobalTestUtil.h
            test_global_ui.cpp # AI-12: the Translate captions and Dub dialogs
            multicam/MulticamTestUtil.h
            multicam/PodcastTestUtil.h
            test_multicam_ui.cpp # AI-13: the Auto switch dialog and the Podcast workflow sheet
            segment/SegmentTestUtil.h
            test_object_mask_ui.cpp # AI-14: the Object tool, its matte preview, AI-tracked redactions
            ../model/TimelineFixture.cpp
            ../model/TimelineFixture.h
        LIBS wabcut_uiplugin wabcut_ui wabcut_ai_shorts wabcut_ai_roughcut wabcut_ai_autozoom wabcut_ai_edit wabcut_ai_dub wabcut_ai_translate
            wabcut_ai_multicam wabcut_ai_workflow
            Qt6::Gui Qt6::Quick
            Qt6::QuickControls2)
    target_include_directories(wabcut_ai_ui_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/app ${CMAKE_SOURCE_DIR}/src/app/src
        ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/ai)
    target_compile_definitions(wabcut_ai_ui_tests PRIVATE WABCUT_VERSION="${PROJECT_VERSION}")
endif()

wabcut_add_test_suite(ai_generation_net SOURCES FakeTransport.h test_net_generation.cpp test_net_local.cpp
    test_net_speech.cpp # AI-12: the text-to-speech adapters, voice cloning, translation by the chat model
    ../model/TimelineFixture.cpp LIBS wabcut_ai_adapters wabcut_ai_agent wabcut_ai_translate)
target_compile_definitions(wabcut_ai_generation_net_tests PRIVATE WABCUT_AI_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_include_directories(wabcut_ai_generation_net_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model)
if(TARGET wabcut_ai_generation_media)
    wabcut_add_test_suite(ai_generation_media SOURCES FakeTransport.h test_generation_conform.cpp LIBS wabcut_ai_generation_media)
endif()
if(TARGET wabcut_ai_net)
    wabcut_add_test_suite(ai_llm_live SOURCES test_llm_live.cpp test_generation_live.cpp ../model/TimelineFixture.cpp
        LIBS wabcut_ai_net wabcut_ai_llm wabcut_ai_agent Qt6::Core)
    target_include_directories(wabcut_ai_llm_live_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model)
endif()

# AI-15: the editing-assistant eval (tests/ai/eval, docs/ai.md "Local assistant tiers"): 56 requests over fixture
# projects with deterministic checks of the staged result. The oracle model and a near-miss llama-server run everywhere
# (label "eval"); the real local tiers and the cloud reference are opt-in [perf] runs for a quiet machine (label "perf",
# never CI, skipped without WABCUT_EVAL_MODELS / WABCUT_EVAL_LLAMA).
set(wabcut_eval_sources
    ../model/TimelineFixture.cpp
    ../model/TimelineFixture.h
    FakeTransport.h
    NetTestSupport.h
    eval/EvalHarness.h
    eval/EvalHarness.cpp
    eval/EvalCases.cpp
    eval/test_eval.cpp)
set(wabcut_eval_libs wabcut_ai_adapters wabcut_ai_agent wabcut_ai_llm)
if(TARGET wabcut_ai_net)
    list(APPEND wabcut_eval_sources eval/test_eval_live.cpp)
    list(APPEND wabcut_eval_libs wabcut_ai_net Qt6::Core)
endif()
wabcut_add_test_suite(eval SOURCES ${wabcut_eval_sources} LIBS ${wabcut_eval_libs} PROPERTIES TIMEOUT 21600)
target_compile_definitions(wabcut_eval_tests PRIVATE WABCUT_AI_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_include_directories(wabcut_eval_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(wabcut_eval_tests PRIVATE psapi)
endif()
