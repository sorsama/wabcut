# Performance benchmarks (P.1, owner: Audio / performance lead; docs/performance.md). Every test is tagged [perf] (CTest
# label `perf`, or `gpu-perf` when it also needs the GPU) and writes test-output/perf/<benchmark>.json; a gated metric
# that costs more than 20 % over this machine's baseline (baselines/<machine id>.json) fails after one retry. Run them
# on a quiet machine: `ctest --preset win-msvc -L perf -R "^Benchmark"`.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(nlohmann_json CONFIG REQUIRED)

set(WABCUT_PERF_BENCH_MEDIA_DIR "${CMAKE_BINARY_DIR}/test-output/perf-bench-media")
set(perf_ffmpeg_arg "")
if(WABCUT_FFMPEG_EXECUTABLE)
    set(perf_ffmpeg_arg --ffmpeg "${WABCUT_FFMPEG_EXECUTABLE}")
endif()
add_test(NAME perf.generate_media
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_perf_bench_media.py"
            --out "${WABCUT_PERF_BENCH_MEDIA_DIR}" ${perf_ffmpeg_arg})
set_tests_properties(perf.generate_media PROPERTIES FIXTURES_SETUP wabcut_perf_media TIMEOUT 600 LABELS perf)

# A.10: the long-VOD and high-frame-rate recordings (a 2-hour 1440p60 H.264 MKV with 3 audio tracks, a 30-minute
# 1080p240 HEVC MP4 with 2; about 33 GB, out of git). The "Benchmark longvod" cases skip while they are missing. Their
# generator is a CTest test only when asked for (-DWABCUT_PERF_LONGVOD_FIXTURE=ON), so no `ctest -L perf` or `-LE perf`
# run writes 33 GB by accident; otherwise run tests/perf/gen_longvod_media.py (docs/performance.md, "Long VODs and high
# frame rates").
option(WABCUT_PERF_LONGVOD_FIXTURE "Add the long-VOD recordings' generator (about 33 GB) as CTest test perf.generate_longvod_media" OFF)
set(WABCUT_PERF_LONGVOD_DIR "${CMAKE_BINARY_DIR}/test-output/perf-longvod-media")
if(WABCUT_PERF_LONGVOD_FIXTURE)
    add_test(NAME perf.generate_longvod_media
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_longvod_media.py"
                --out "${WABCUT_PERF_LONGVOD_DIR}" ${perf_ffmpeg_arg})
    set_tests_properties(perf.generate_longvod_media PROPERTIES
        FIXTURES_SETUP wabcut_longvod_media TIMEOUT 7200 LABELS "perf;longvod")
endif()

if(TARGET wabcut_ui AND TARGET wabcut)
    wabcut_add_test_suite(perf
        SOURCES
            PerfHarness.h
            PerfHarness.cpp
            PerfGpu.cpp
            PerfProjects.h
            PerfProjects.cpp
            bench_analysis.cpp
            bench_export.cpp
            bench_longvod.cpp
            bench_playback.cpp
            bench_project.cpp
            bench_startup.cpp
            bench_timeline.cpp
            ${CMAKE_SOURCE_DIR}/tests/model/ScaleProject.h
            ${CMAKE_SOURCE_DIR}/tests/model/ScaleProject.cpp
            # The real shell offscreen (the timeline and memory benchmarks), as the app suite runs it.
            ${CMAKE_SOURCE_DIR}/tests/app/ShellFixture.h
            ${CMAKE_SOURCE_DIR}/tests/app/ShellFixture.cpp
            ${CMAKE_SOURCE_DIR}/tests/app/TimelineHarness.h
        LIBS wabcut_uiplugin wabcut_ui wabcut_editor wabcut_export wabcut_command wabcut_model wabcut_media
             wabcut_ai_analysis_media wabcut_ai_search wabcut_ai_transcribe
             nlohmann_json::nlohmann_json Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2
        PROPERTIES FIXTURES_REQUIRED wabcut_perf_media TIMEOUT 900)
    target_include_directories(wabcut_perf_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/app ${CMAKE_SOURCE_DIR}/src/app/src)
    target_compile_definitions(wabcut_perf_tests PRIVATE
        WABCUT_VERSION="${PROJECT_VERSION}"
        WABCUT_PERF_BASELINE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/baselines"
        WABCUT_PERF_BUDGETS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/budgets.json"
        WABCUT_PERF_MEDIA_DIR="${WABCUT_PERF_BENCH_MEDIA_DIR}"
        # bench_longvod.cpp: the recordings perf.generate_longvod_media writes (the cases skip without them).
        WABCUT_PERF_LONGVOD_DIR="${WABCUT_PERF_LONGVOD_DIR}"
        WABCUT_APP_EXE="$<TARGET_FILE:wabcut>"
        # The platform the startup benchmark runs the app on (StartupProbe; QT_QPA_PLATFORM overrides it).
        WABCUT_PERF_STARTUP_PLATFORM="offscreen"
        WABCUT_QT_PLUGIN_DIR="${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
        # The analysis benchmark's helpers (protocol and pipeline without a model).
        WABCUT_FAKE_EMBED="$<TARGET_FILE:wabcut_fake_embed>"
        WABCUT_FAKE_INFER="$<TARGET_FILE:wabcut_fake_infer>")
    add_dependencies(wabcut_perf_tests wabcut wabcut_fake_embed wabcut_fake_infer)
    if(TARGET wabcut-infer)
        target_compile_definitions(wabcut_perf_tests PRIVATE WABCUT_REAL_INFER="$<TARGET_FILE:wabcut-infer>")
        add_dependencies(wabcut_perf_tests wabcut-infer)
    endif()
    wabcut_allow_qt_generated_code(wabcut_perf_tests)
endif()

# The sampling profiler (docs/performance.md, "Profiling"): no installation needed.
if(WIN32)
    add_executable(wabcut_perf_sampler tools/sampler.cpp)
    target_compile_definitions(wabcut_perf_sampler PRIVATE UNICODE _UNICODE)
    target_link_libraries(wabcut_perf_sampler PRIVATE dbghelp psapi winmm)
    wabcut_target_defaults(wabcut_perf_sampler)
endif()
