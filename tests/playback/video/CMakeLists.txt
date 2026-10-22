# Video playback tests (owner: Graphics/playback): evaluation, transports, the composite pool, and the Scheduler /
# PlaybackController end to end over the generated test media (fixture from tests/media).
wabcut_add_test_suite(playback_video
    SOURCES
        PlaybackTestSupport.h
        PlaybackTestSupport.cpp
        test_caption_style.cpp
        test_device_change.cpp
        test_evaluate.cpp
        test_finishing.cpp
        test_library_mapping.cpp # 0.2: Premiere's transitions and effects in the evaluator
        test_nested.cpp
        test_graphics_layers.cpp # C.13 graphic clips in the evaluator (I.9 Part 0.5)
        test_preview_playback.cpp # C.16 preview files (renders them with wabcut_editor's export preview renderer)
        test_proxy_playback.cpp
        test_redaction.cpp
        test_matte_redaction.cpp # AI-14: matte redactions fail closed
        test_motion.cpp # P.2: Time Interpolation, Morph Cut (synthetic media through ffmpeg's FFV1)
        test_scheduler.cpp
        ../../model/TimelineFixture.cpp
        ../../model/TimelineFixture.h
    LIBS wabcut_playback wabcut_media wabcut_audio wabcut_export wabcut_editor Qt6::Gui
    PROPERTIES FIXTURES_REQUIRED wabcut_test_media LABELS playback)
target_compile_definitions(wabcut_playback_video_tests PRIVATE
    WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media"
    WABCUT_FFMPEG_EXECUTABLE="${WABCUT_FFMPEG_EXECUTABLE}") # P.2: test_motion.cpp encodes its own media
# C.8 nested / multicam evaluation tests build their projects with the model tests' timeline pictures.
target_include_directories(wabcut_playback_video_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model)
# P.2 (E6): Premiere's Loop (transports, the controller, the scheduler over real media) and the second program output's
# playback mirror.
target_sources(wabcut_playback_video_tests PRIVATE test_loop_playback.cpp test_output_mirror.cpp)

# ---- Playback benchmark (perf harness): wabcut_playback_bench --media <dir> --config 3x1080p60 --seconds 20 ----
# Clips come from gen_perf_media.py (--set full: 6 x 1080p60 + 1 x 2160p30, 20 s). CTest runs a 2-second smoke
# of the 3-stream path over short clips so the harness keeps working; real numbers come from running it directly.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(wabcut_playback_bench bench_playback.cpp)
target_link_libraries(wabcut_playback_bench PRIVATE wabcut_playback wabcut_render wabcut_media Qt6::Gui Qt6::GuiPrivate)
if(WIN32)
    target_link_libraries(wabcut_playback_bench PRIVATE d3d11 dxgi winmm)
endif()
wabcut_target_defaults(wabcut_playback_bench)

set(WABCUT_PERF_MEDIA_DIR "${CMAKE_BINARY_DIR}/test-output/perf-media")
add_test(NAME playback.generate_perf_smoke_media
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_perf_media.py"
            --ffmpeg "${WABCUT_FFMPEG_EXECUTABLE}" --out "${WABCUT_PERF_MEDIA_DIR}" --set smoke)
set_tests_properties(playback.generate_perf_smoke_media PROPERTIES
    FIXTURES_SETUP wabcut_perf_smoke_media TIMEOUT 600 LABELS playback)
add_test(NAME playback.bench_smoke
    COMMAND wabcut_playback_bench --media "${WABCUT_PERF_MEDIA_DIR}" --config smoke --seconds 2
            --out "${CMAKE_BINARY_DIR}/test-output/playback/bench-smoke.md")
set_tests_properties(playback.bench_smoke PROPERTIES
    FIXTURES_REQUIRED wabcut_perf_smoke_media TIMEOUT 120 LABELS playback
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WABCUT_QT_BIN_DIR}")
