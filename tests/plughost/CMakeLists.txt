# Audio plugin hosting (A.6, docs/plugins.md): the helper, the catalog and the plugin effects, with test plugins built
# here (full Windows builds with WABCUT_WITH_AUDIO_PLUGINS).
if(NOT TARGET wabcut_plugins)
    return()
endif()

find_package(clap CONFIG REQUIRED)
set(_plugin_dir ${CMAKE_BINARY_DIR}/test-plugins)

# A CLAP effect (gain, latency, crash, stall), and two that crash or hang while they are scanned.
function(_wabcut_test_clap target name folder)
    add_library(${target} MODULE plugins/clap/TestGainClap.cpp)
    set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME ${name}
                                               LIBRARY_OUTPUT_DIRECTORY ${_plugin_dir}/${folder})
    target_link_libraries(${target} PRIVATE clap)
    wabcut_target_defaults(${target})
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()
_wabcut_test_clap(wabcut_test_clap_gain WabcutTestGain clap)
_wabcut_test_clap(wabcut_test_clap_crash WabcutTestCrashOnScan blocked WABCUT_TEST_CRASH_ON_SCAN)
_wabcut_test_clap(wabcut_test_clap_hang WabcutTestHangOnScan blocked WABCUT_TEST_HANG_ON_SCAN)

# A VST3 effect built like the SDK's AGain example (gain, latency, crash).
add_library(wabcut_test_vst3_gain MODULE plugins/vst3/TestGainVst3.cpp)
set_target_properties(wabcut_test_vst3_gain PROPERTIES PREFIX "" SUFFIX ".vst3" OUTPUT_NAME WabcutTestGain
                                                       LIBRARY_OUTPUT_DIRECTORY ${_plugin_dir}/vst3)
target_link_libraries(wabcut_test_vst3_gain PRIVATE wabcut_vst3sdk)
wabcut_target_defaults(wabcut_test_vst3_gain)

wabcut_add_test_suite(plughost
    SOURCES
        PlughostFixture.h
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        test_plughost_channel.cpp
        test_plughost_protocol.cpp
        test_plugin_cli.cpp
        test_plugin_effects.cpp
        test_plugin_export_warnings.cpp
        test_plugin_isolation.cpp
        test_plugin_latency_change.cpp
        test_plugin_loading.cpp
        test_plugin_master_restart.cpp
        test_plugin_scan.cpp
        test_plugin_rt.cpp
        test_plugin_surround.cpp
    LIBS wabcut_plugins wabcut_playback wabcut_media wabcut_export)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(wabcut_plughost_tests PRIVATE nlohmann_json::nlohmann_json)
# Review S5-P2-6: a stand-in helper that writes malformed control lines before its replies, and malformed scan answers.
add_executable(wabcut_fake_plughost fake_plughost.cpp)
target_link_libraries(wabcut_fake_plughost PRIVATE wabcut_plughost_common nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_fake_plughost)
target_compile_definitions(wabcut_plughost_tests PRIVATE WABCUT_FAKE_PLUGHOST_PATH="$<TARGET_FILE:wabcut_fake_plughost>")
add_dependencies(wabcut_plughost_tests wabcut_fake_plughost)
target_include_directories(wabcut_plughost_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model)
target_compile_definitions(wabcut_plughost_tests PRIVATE
    WABCUT_TEST_PLUGIN_DIR="${_plugin_dir}"
    WABCUT_PLUGHOST_PATH="$<TARGET_FILE:wabcut-plughost>"
    WABCUT_CLI_PATH="$<TARGET_FILE:wabcut-cli>")
add_dependencies(wabcut_plughost_tests wabcut-plughost wabcut-cli wabcut_test_clap_gain wabcut_test_clap_crash wabcut_test_clap_hang
                 wabcut_test_vst3_gain)
