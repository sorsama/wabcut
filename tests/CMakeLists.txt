# Test harness. Each module owns tests/<module>/CMakeLists.txt and calls wabcut_add_test_suite();
# suites are picked up automatically — no need to edit this file.
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

if(TARGET Qt6::Core)
    set(WABCUT_QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
endif()

# Tests that need hardware the CI runners lack get CTest labels, so CI can run `ctest -LE "gpu|audio-device"`.
# The mapping is by Catch2 tag, so suites need no changes; new hardware tests should use [gpu] or [audio-device].
#   gpu:          [gpu], [hw] (hardware video decode), [screenshot] (offscreen grabs on the real RHI backend),
#                 [qml][rhi] (the GPU status pill reads the live adapter)
#   audio-device: [audio-device], [device] (real output devices)
#   perf:         [perf] — strict timing thresholds (frame budgets, latency, nothing dropped). They need a quiet
#                 machine: `ctest -L perf` when nothing else runs, `ctest -LE perf` while other builds/tests do. The
#                 functional tests always assert what must happen and record their timings instead (e.g.
#                 test-output/e2e/timing.md); a [perf] test measures once more when its first attempt misses. A [perf]
#                 test that also needs hardware is labelled gpu-perf or audio-device-perf, so both `-LE perf`
#                 and the CI hardware exclusion match it. Suite labels must never overwrite these classifications.
# Each entry is a Catch2 test spec of AND-ed tags; a test matching several groups gets the first label.
set(WABCUT_TEST_LABEL_TAGS
    gpu "[gpu]"
    gpu "[hw]"
    gpu "[screenshot]"
    gpu "[qml][rhi]"
    audio-device "[audio-device]"
    audio-device "[device]"
    perf "[perf]")

# Catch2 spec that matches the tests NOT matched by any spec in `specs` (each an AND of [tags]): the negation of
# an OR of AND-groups, expanded into Catch2's OR-of-ANDs form.
function(_wabcut_negated_spec out specs)
    set(terms "")
    foreach(spec IN LISTS specs)
        string(REGEX MATCHALL "\\[[^]]+\\]" tags "${spec}")
        set(next "")
        foreach(tag IN LISTS tags)
            if(terms STREQUAL "")
                list(APPEND next "~${tag}")
            else()
                foreach(term IN LISTS terms)
                    list(APPEND next "${term}~${tag}")
                endforeach()
            endif()
        endforeach()
        set(terms "${next}")
    endforeach()
    list(JOIN terms "," joined)
    set(${out} "${joined}" PARENT_SCOPE)
endfunction()

# wabcut_add_test_suite(<name> SOURCES <files...> LIBS <targets...> [PROPERTIES <prop> <value> ...])
#   ->  executable wabcut_<name>_tests. Each discovered test gets one CTest label: its hardware label above if a
#   tag matches, else the suite name (Catch2's discovery cannot pass a multi-value LABELS property). PROPERTIES
#   are applied to every discovered test (e.g. FIXTURES_REQUIRED).
function(wabcut_add_test_suite name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;PROPERTIES" ${ARGN})
    set(target wabcut_${name}_tests)
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain ${ARG_LIBS})
    target_compile_definitions(${target} PRIVATE
        WABCUT_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
        WABCUT_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output")
    wabcut_target_defaults(${target})

    set(discover_args DISCOVERY_MODE PRE_TEST)
    # Every test gets a 300 s ceiling so a hung test (e.g. a real audio device that never reopens) fails instead of
    # blocking the whole run; suites can override TIMEOUT through PROPERTIES.
    set(common_props TIMEOUT 300)
    if(WABCUT_QT_BIN_DIR)
        list(APPEND discover_args DL_PATHS "${WABCUT_QT_BIN_DIR}")
        list(APPEND common_props ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WABCUT_QT_BIN_DIR}")
    endif()

    # One discovery per hardware label (tests matching its tags), then one for everything else.
    set(labelled_specs "")
    set(label_index 0)
    list(LENGTH WABCUT_TEST_LABEL_TAGS pairs)
    while(label_index LESS pairs)
        list(GET WABCUT_TEST_LABEL_TAGS ${label_index} label)
        math(EXPR spec_index "${label_index} + 1")
        list(GET WABCUT_TEST_LABEL_TAGS ${spec_index} spec)
        _wabcut_negated_spec(exclude_earlier "${labelled_specs}")
        set(test_spec "${spec}")
        if(exclude_earlier)
            # Only tests no earlier group claimed: AND the spec into every term of the earlier groups' negation.
            string(REPLACE "," ";" earlier_terms "${exclude_earlier}")
            set(combined "")
            foreach(term IN LISTS earlier_terms)
                list(APPEND combined "${spec}${term}")
            endforeach()
            list(JOIN combined "," test_spec)
        endif()
        if(label STREQUAL "perf")
            catch_discover_tests(${target} ${discover_args} TEST_SPEC "${test_spec}"
                PROPERTIES ${common_props} ${ARG_PROPERTIES} LABELS ${label})
        else()
            # Catch's discovery property list cannot represent multiple LABELS values reliably. Split each
            # hardware group on [perf], retaining both classifications in a regex-matchable single label.
            foreach(timing IN ITEMS functional perf)
                if(timing STREQUAL "perf")
                    set(timing_tag "[perf]")
                    set(timing_label "${label}-perf")
                else()
                    set(timing_tag "~[perf]")
                    set(timing_label "${label}")
                endif()
                string(REPLACE "," ",${timing_tag}" timing_spec "${test_spec}")
                catch_discover_tests(${target} ${discover_args} TEST_SPEC "${timing_tag}${timing_spec}"
                    PROPERTIES ${common_props} ${ARG_PROPERTIES} LABELS ${timing_label})
            endforeach()
        endif()
        list(APPEND labelled_specs "${spec}")
        math(EXPR label_index "${label_index} + 2")
    endwhile()
    _wabcut_negated_spec(rest_spec "${labelled_specs}")
    catch_discover_tests(${target} ${discover_args} TEST_SPEC "${rest_spec}"
        PROPERTIES LABELS ${name} ${common_props} ${ARG_PROPERTIES})
endfunction()

# The command suite in core builds (the win-msvc-asan preset): everything but the stdio tests, which spawn wabcut-cli
# (it links the FFmpeg media layer). OFF by default until the other compilers of the core gate have built it.
option(WABCUT_CORE_COMMAND_TESTS "Build the command suite in core-only builds too (without the wabcut-cli stdio tests)" OFF)
if(WABCUT_CORE_ONLY)
    set(wabcut_test_suites model keymap interchange ai audio_edit color masks motion render_reference script)
    if(WABCUT_CORE_COMMAND_TESTS)
        list(APPEND wabcut_test_suites command)
    endif()
else()
    set(wabcut_test_suites model keymap media audio render playback plughost export interchange command ai audio_edit color masks motion render_reference script app editor e2e perf)
endif()
# The parser fuzz harnesses (tests/fuzz/README.md): libFuzzer executables with WABCUT_FUZZ, else ctest regression runs.
list(APPEND wabcut_test_suites fuzz)
foreach(suite IN LISTS wabcut_test_suites)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${suite}/CMakeLists.txt)
        add_subdirectory(${suite})
    endif()
endforeach()

# The app tests compile qmlcachegen output through the QML module (see WabcutCompilerOptions.cmake).
wabcut_allow_qt_generated_code(wabcut_app_tests)

# Exception outside Qt-generated code: tests/audio/test_clock.cpp keeps a `return {};` after FAIL(), which throws,
# and MSVC's optimizer reports it as unreachable (C4702). Only that file is exempt, until the test drops the dead
# return.
if(MSVC AND TARGET wabcut_audio_tests)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/audio/test_clock.cpp
        TARGET_DIRECTORY wabcut_audio_tests PROPERTIES COMPILE_OPTIONS /wd4702)
endif()
