# OpenFX video plug-ins (0.7, docs/plugins.md): the helper, the catalog and the effects that run on a clip's frame, with test
# plug-ins built here (full Windows builds, with the audio plug-in host).
if(NOT TARGET wabcut_ofx)
    return()
endif()

find_package(nlohmann_json CONFIG REQUIRED)
set(_ofx_dir ${CMAKE_BINARY_DIR}/test-ofx)

# The test plug-ins (plugins/TestOfx.cpp), each in a bundle as OpenFX lays them out on Windows:
#   <folder>/<name>.ofx.bundle/Contents/Win64/<name>.ofx
function(_wabcut_test_ofx target name folder)
    add_library(${target} MODULE plugins/TestOfx.cpp)
    set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".ofx" OUTPUT_NAME ${name}
                                               LIBRARY_OUTPUT_DIRECTORY ${_ofx_dir}/${folder}/${name}.ofx.bundle/Contents/Win64
                                               ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implib/${target})
    target_link_libraries(${target} PRIVATE wabcut_openfx_headers)
    wabcut_target_defaults(${target})
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()
_wabcut_test_ofx(wabcut_test_ofx_main WabcutTestOfx main)
_wabcut_test_ofx(wabcut_test_ofx_crash WabcutTestOfxCrash crash WABCUT_TEST_OFX_CRASH)
_wabcut_test_ofx(wabcut_test_ofx_hang WabcutTestOfxHang hang WABCUT_TEST_OFX_HANG)
_wabcut_test_ofx(wabcut_test_ofx_crashscan WabcutTestOfxCrashScan blocked WABCUT_TEST_OFX_CRASH_ON_SCAN)

wabcut_add_test_suite(ofxhost
    SOURCES
        OfxFixture.h
        test_ofx_effects.cpp
        test_ofx_isolation.cpp
        test_ofx_pipeline.cpp
        test_ofx_render.cpp
        test_ofx_scan.cpp
        test_ofx_sdk.cpp # opt-in: plug-ins built from the OpenFX SDK's own examples (hidden: [.sdk])
    LIBS wabcut_ofx wabcut_playback wabcut_media wabcut_render nlohmann_json::nlohmann_json Qt6::Gui)
target_compile_definitions(wabcut_ofxhost_tests PRIVATE
    WABCUT_TEST_OFX_DIR="${_ofx_dir}"
    WABCUT_OFXHOST_PATH="$<TARGET_FILE:wabcut-ofxhost>")
add_dependencies(wabcut_ofxhost_tests wabcut-ofxhost wabcut_test_ofx_main wabcut_test_ofx_crash wabcut_test_ofx_hang
                 wabcut_test_ofx_crashscan)
