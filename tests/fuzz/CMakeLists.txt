# Parser fuzz harnesses (C.10; tests/fuzz/README.md). One executable per untrusted input format: a libFuzzer fuzzer
# with WABCUT_FUZZ (the win-msvc-asan preset, cmake/WabcutSanitizers.cmake), else the harness on the portable driver
# (FuzzMain.cpp). Either way ctest runs each one on its seeds plus WABCUT_FUZZ_REGRESSION_RUNS deterministic mutations
# (label "fuzz"): the seeds and every reproducer of a fixed crash (seeds/<harness>/regression-*) keep passing.
#
# Seeds: small hand-written files in seeds/<harness>/, the test fixtures of the format's own suite, and corpora that
# wabcut_fuzz_seeds generates from the model into <build>/fuzz/seeds (ctest fixture fuzz_seeds). libFuzzer adds the
# inputs it finds to <build>/fuzz/corpus/<harness> and writes crashes to <build>/fuzz/artifacts/<harness>/.

set(WABCUT_FUZZ_REGRESSION_RUNS 1000 CACHE STRING "Mutations each fuzz harness runs in ctest after its seeds")
set(fuzz_seeds_src ${CMAKE_CURRENT_SOURCE_DIR}/seeds)
set(fuzz_work ${CMAKE_BINARY_DIR}/fuzz)
set(fuzz_fixtures ${CMAKE_SOURCE_DIR}/tests/interchange/fixtures)

# The every-shape project and the large one (tests/model/ScaleProject.h), shared by harnesses and the seed tool.
add_library(wabcut_fuzz_support STATIC ../model/ScaleProject.cpp ../model/ScaleProject.h FuzzSupport.h FuzzRegistry.h)
target_include_directories(wabcut_fuzz_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/model)
target_link_libraries(wabcut_fuzz_support PUBLIC wabcut_model)
wabcut_target_defaults(wabcut_fuzz_support)

# wabcut_add_fuzzer(<name> SOURCES <files> LIBS <targets> [SEEDS <files or folders>] [MAX_LEN <n>] [DEFINITIONS <defs>])
#   -> executable fuzz_<name> and the ctest test fuzz.<name>.
function(wabcut_add_fuzzer name)
    cmake_parse_arguments(ARG "" "MAX_LEN" "SOURCES;LIBS;SEEDS;DEFINITIONS" ${ARGN})
    set(target fuzz_${name})
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE wabcut_fuzz_support ${ARG_LIBS})
    target_compile_definitions(${target} PRIVATE ${ARG_DEFINITIONS})
    wabcut_target_defaults(${target})
    if(WABCUT_FUZZ)
        target_compile_options(${target} PRIVATE ${WABCUT_FUZZER_COMPILE_OPTIONS})
        target_link_options(${target} PRIVATE ${WABCUT_FUZZER_LINK_OPTIONS})
    else()
        target_sources(${target} PRIVATE FuzzMain.cpp)
    endif()
    if(NOT ARG_MAX_LEN)
        set(ARG_MAX_LEN 16384)
    endif()
    file(MAKE_DIRECTORY ${fuzz_work}/corpus/${name} ${fuzz_work}/artifacts/${name} ${fuzz_work}/seeds/${name})
    set(seeds "")
    if(EXISTS ${fuzz_seeds_src}/${name})
        list(APPEND seeds ${fuzz_seeds_src}/${name})
    endif()
    # libFuzzer takes folders only: single seed files (fixtures of other suites) are copied into one.
    if(ARG_SEEDS)
        file(REMOVE_RECURSE ${fuzz_work}/staged/${name})
        file(COPY ${ARG_SEEDS} DESTINATION ${fuzz_work}/staged/${name})
        list(APPEND seeds ${fuzz_work}/staged/${name})
    endif()
    # The first folder is libFuzzer's working corpus (it writes new inputs there), never a source folder.
    add_test(NAME fuzz.${name}
        COMMAND ${target} -runs=${WABCUT_FUZZ_REGRESSION_RUNS} -seed=1 -max_len=${ARG_MAX_LEN} -timeout=20
                -rss_limit_mb=2048 -artifact_prefix=${fuzz_work}/artifacts/${name}/
                ${fuzz_work}/corpus/${name} ${fuzz_work}/seeds/${name} ${seeds})
    set_tests_properties(fuzz.${name} PROPERTIES LABELS fuzz TIMEOUT 600 FIXTURES_REQUIRED fuzz_seeds)
endfunction()

# Generated seeds (Wabcut's own formats), before the fuzz tests.
add_executable(wabcut_fuzz_seeds make_seeds.cpp)
target_include_directories(wabcut_fuzz_seeds PRIVATE ${CMAKE_SOURCE_DIR}/src/model/src)
target_link_libraries(wabcut_fuzz_seeds PRIVATE wabcut_fuzz_support wabcut_command)
wabcut_target_defaults(wabcut_fuzz_seeds)
add_test(NAME fuzz.seeds COMMAND wabcut_fuzz_seeds ${fuzz_work}/seeds)
set_tests_properties(fuzz.seeds PROPERTIES LABELS fuzz FIXTURES_SETUP fuzz_seeds)

# --- Wabcut's own formats (model, command) ---------------------------------------------------------------------------
wabcut_add_fuzzer(project_io SOURCES fuzz_project_io.cpp LIBS wabcut_model MAX_LEN 65536)
wabcut_add_fuzzer(history SOURCES fuzz_history.cpp LIBS wabcut_model MAX_LEN 65536)
target_include_directories(fuzz_history PRIVATE ${CMAKE_SOURCE_DIR}/src/model/src)
wabcut_add_fuzzer(review_package SOURCES fuzz_review_package.cpp LIBS wabcut_command MAX_LEN 65536)
wabcut_add_fuzzer(command SOURCES fuzz_command.cpp LIBS wabcut_command)
wabcut_add_fuzzer(mcp SOURCES fuzz_mcp.cpp LIBS wabcut_mcp)
wabcut_add_fuzzer(clipboard SOURCES fuzz_clipboard.cpp LIBS wabcut_model MAX_LEN 65536)
wabcut_add_fuzzer(graphic SOURCES fuzz_graphic.cpp LIBS wabcut_model MAX_LEN 65536)
# C.15: preview cache indexes, and projects with blend modes, synthetic items and adjustment layers through the composite
# plan, the render bar and content hashes (seeds: wabcut_fuzz_seeds' preview/).
wabcut_add_fuzzer(preview SOURCES fuzz_preview.cpp LIBS wabcut_model MAX_LEN 262144)
wabcut_add_fuzzer(extension SOURCES fuzz_extension.cpp LIBS wabcut_script MAX_LEN 65536)

# --- Other editors' timelines (interchange) ---------------------------------------------------------------------------
wabcut_add_fuzzer(fcp7 SOURCES fuzz_import.cpp LIBS wabcut_interchange DEFINITIONS WABCUT_FUZZ_FORMAT=Fcp7Xml MAX_LEN 65536
    SEEDS ${fuzz_fixtures}/fcp7_23976_mac.xml ${fuzz_fixtures}/premiere_2997df.xml ${fuzz_fixtures}/malformed_truncated.xml)
wabcut_add_fuzzer(fcpxml SOURCES fuzz_import.cpp LIBS wabcut_interchange DEFINITIONS WABCUT_FUZZ_FORMAT=FcpXml MAX_LEN 65536
    SEEDS ${fuzz_fixtures}/fcpx_1_10_23976.fcpxml "${fuzz_fixtures}/Wedding Teaser.fcpxmld/Info.fcpxml")
wabcut_add_fuzzer(otio SOURCES fuzz_import.cpp LIBS wabcut_interchange DEFINITIONS WABCUT_FUZZ_FORMAT=Otio MAX_LEN 65536
    SEEDS ${fuzz_fixtures}/resolve_timeline.otio ${fuzz_fixtures}/malformed.otio)
wabcut_add_fuzzer(edl SOURCES fuzz_import.cpp LIBS wabcut_interchange DEFINITIONS WABCUT_FUZZ_FORMAT=Edl
    SEEDS ${fuzz_fixtures}/avid_25fps.edl ${fuzz_fixtures}/promo_2997df.edl ${fuzz_fixtures}/malformed.edl)
# X.1: timeline export of any project that loads, read back by the importers (seeds: wabcut_fuzz_seeds' export/).
wabcut_add_fuzzer(export SOURCES fuzz_export.cpp LIBS wabcut_interchange MAX_LEN 65536)
file(GLOB fuzz_lottie_seeds ${fuzz_fixtures}/lottie/*.json)
wabcut_add_fuzzer(lottie SOURCES fuzz_lottie.cpp LIBS wabcut_interchange MAX_LEN 65536 SEEDS ${fuzz_lottie_seeds})
# P.2: caption files (every format's reader on each input) and the readers of the AAF / OMF containers.
file(GLOB fuzz_caption_seeds ${fuzz_fixtures}/captions/*)
wabcut_add_fuzzer(captions SOURCES fuzz_captions.cpp LIBS wabcut_interchange MAX_LEN 65536 SEEDS ${fuzz_caption_seeds})
wabcut_add_fuzzer(audio_post SOURCES fuzz_audio_post.cpp LIBS wabcut_interchange MAX_LEN 1048576
    SEEDS ${fuzz_fixtures}/export/audiopost.aaf ${fuzz_fixtures}/export/audiopost.omf)
target_include_directories(fuzz_audio_post PRIVATE ${CMAKE_SOURCE_DIR}/src/interchange/src)
# 0.2: Motion Graphics templates (.mogrt made in Premiere Pro) and the bounded ZIP reader; effect preset files and the
# user's effects library (seeds: seeds/mogrt, seeds/effect_presets - the first byte picks the input).
wabcut_add_fuzzer(mogrt SOURCES fuzz_mogrt.cpp LIBS wabcut_interchange MAX_LEN 65536)
wabcut_add_fuzzer(effect_presets SOURCES fuzz_effect_presets.cpp LIBS wabcut_model MAX_LEN 65536)
wabcut_add_fuzzer(psd SOURCES fuzz_psd.cpp LIBS wabcut_media MAX_LEN 65536) # 0.2 layered Photoshop files
file(GLOB fuzz_prproj_seeds ${fuzz_fixtures}/prproj/*.prproj)
wabcut_add_fuzzer(prproj SOURCES fuzz_import.cpp LIBS wabcut_interchange DEFINITIONS WABCUT_FUZZ_FORMAT=PremiereProject
    MAX_LEN 65536 SEEDS ${fuzz_prproj_seeds})

# --- Harnesses for other owners' parsers (findings go to them through the coordinator) -------------------------------
file(GLOB fuzz_kys_seeds ${CMAKE_SOURCE_DIR}/tests/keymap/data/*.kys)
wabcut_add_fuzzer(kys SOURCES fuzz_kys.cpp LIBS wabcut_keymap MAX_LEN 65536 SEEDS ${fuzz_kys_seeds})
wabcut_add_fuzzer(cube SOURCES fuzz_cube.cpp LIBS wabcut_color)
wabcut_add_fuzzer(cdl SOURCES fuzz_cdl.cpp LIBS wabcut_color)
file(GLOB fuzz_sse_seeds ${CMAKE_SOURCE_DIR}/tests/ai/fixtures/*/*.sse ${CMAKE_SOURCE_DIR}/tests/ai/fixtures/*/*.json)
wabcut_add_fuzzer(ai_sse SOURCES fuzz_ai_sse.cpp LIBS wabcut_ai_adapters MAX_LEN 65536 SEEDS ${fuzz_sse_seeds})
target_include_directories(fuzz_ai_sse PRIVATE ${CMAKE_SOURCE_DIR}/tests/ai)
