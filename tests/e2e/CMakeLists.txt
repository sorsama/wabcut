# End-to-end editor loop (owner: Graphics/playback, I.1): generated media -> import through ProjectController's
# ProjectFile bridge -> a 3-track sequence built with edit commands -> 2 s of playback through the real Scheduler
# (offscreen renderer) and the Null audio engine -> save -> destroy -> reopen -> move a file, relink -> export through
# ExportJob (real audio mix; synthetic video until M6 phase 2). Plus the offscreen screenshots of the full shell: the
# library with real thumbnails and the monitor mid-playback (test-output/e2e/screenshots). I.3: an H.264 4:2:2 10-bit
# 2160p60 clip gets an automatic proxy; playback is measured with and without it (test-output/e2e/proxy-playback.md).
# Labels (tests/CMakeLists.txt): the loop is `e2e`; the screenshot case is tagged [screenshot], so it is `gpu`.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WABCUT_E2E_MEDIA_DIR "${CMAKE_BINARY_DIR}/test-output/e2e/media")
set(e2e_ffmpeg_arg "")
if(WABCUT_FFMPEG_EXECUTABLE)
    set(e2e_ffmpeg_arg --ffmpeg "${WABCUT_FFMPEG_EXECUTABLE}")
endif()
add_test(NAME e2e.generate_media
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_e2e_media.py"
            --out "${WABCUT_E2E_MEDIA_DIR}" ${e2e_ffmpeg_arg})
set_tests_properties(e2e.generate_media PROPERTIES FIXTURES_SETUP wabcut_e2e_media TIMEOUT 300 LABELS e2e)

if(TARGET wabcut_ui)
    # QQuickWindowPrivate::polishItems() lays out the hidden shell before its one offscreen grab.
    find_package(Qt6 REQUIRED COMPONENTS QuickPrivate)
    wabcut_add_test_suite(e2e
        SOURCES test_e2e.cpp
        LIBS wabcut_uiplugin wabcut_ui wabcut_editor wabcut_export wabcut_command wabcut_playback wabcut_render
             wabcut_media
             Qt6::Gui Qt6::Quick Qt6::QuickPrivate Qt6::QuickControls2
        PROPERTIES FIXTURES_REQUIRED wabcut_e2e_media)
    target_compile_definitions(wabcut_e2e_tests PRIVATE
        WABCUT_E2E_MEDIA_DIR="${WABCUT_E2E_MEDIA_DIR}"
        WABCUT_INTERCHANGE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/interchange/fixtures"
        WABCUT_VERSION="${PROJECT_VERSION}"
        WABCUT_FFMPEG_EXECUTABLE="${WABCUT_FFMPEG_EXECUTABLE}")
    # I.6: the mask case pans a synthetic texture (tests/masks/SyntheticMotion.h) for the tracker.
    target_include_directories(wabcut_e2e_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/masks)
endif()
