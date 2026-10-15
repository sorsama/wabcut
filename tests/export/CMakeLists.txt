# Export tests. The end-to-end cases export a timeline of the media suite's generated clips (CTest fixture
# wabcut_test_media), verify the files with the vcpkg ffprobe and decode them back with FFmpeg.
find_package(FFMPEG REQUIRED)

find_program(WABCUT_FFPROBE_EXECUTABLE NAMES ffprobe
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/ffmpeg"
    NO_DEFAULT_PATH)
if(NOT WABCUT_FFPROBE_EXECUTABLE)
    set(WABCUT_FFPROBE_EXECUTABLE ffprobe) # from PATH
endif()

wabcut_add_test_suite(export
    SOURCES
        ExportTestUtil.cpp
        ExportTestUtil.h
        test_benchmark.cpp
        test_caption_redaction.cpp
        test_cli_render.cpp
        test_export.cpp
        test_export_audio_ai.cpp
        test_gaming_export.cpp # I.9 (gaming): 60 fps presets, target sizes under Discord's limits
        test_proxy.cpp
        test_queue.cpp
        test_rendered.cpp
        test_matte_export.cpp # AI-14: matte redactions fail closed, export == preview
        test_highlights_e2e.cpp # G.2: a recording's highlights -> a reel -> MP4, end to end
        test_settings.cpp
        test_sidecar_text.cpp
    LIBS wabcut_export wabcut_media wabcut_playback wabcut_audio wabcut_ai_highlights_media
    # Label: "export", or "gpu" for the tests tagged [gpu] (QRhi rendering, NVENC), so GPU-less CI can skip them
    # (tests/CMakeLists.txt).
    PROPERTIES FIXTURES_REQUIRED wabcut_test_media)
target_include_directories(wabcut_export_tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(wabcut_export_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(wabcut_export_tests PRIVATE ${FFMPEG_LIBRARIES})
target_compile_definitions(wabcut_export_tests PRIVATE
    WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media"
    WABCUT_FFPROBE_EXECUTABLE="${WABCUT_FFPROBE_EXECUTABLE}"
    WABCUT_CLI_EXECUTABLE="$<TARGET_FILE:wabcut-cli>")
add_dependencies(wabcut_export_tests wabcut-cli) # test_cli_render.cpp runs it

# Export UI: ExportController and the Export workspace on the real QML shell, through tests/app's ShellFixture
# (one QGuiApplication per process, Qt message capture, Main.qml created hidden). The screenshot lands in
# <build>/test-output/ui/workspace-export-job.png.
if(TARGET wabcut_ui)
    wabcut_add_test_suite(export_ui
        SOURCES
            ${CMAKE_SOURCE_DIR}/tests/app/ShellFixture.h
            ${CMAKE_SOURCE_DIR}/tests/app/ShellFixture.cpp
            ui/test_export_ui.cpp
        LIBS wabcut_uiplugin wabcut_ui wabcut_export wabcut_media Qt6::Gui Qt6::Quick Qt6::QuickControls2
        PROPERTIES FIXTURES_REQUIRED wabcut_test_media)
    target_include_directories(wabcut_export_ui_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/app ${CMAKE_SOURCE_DIR}/src/app/src ${CMAKE_SOURCE_DIR}/src/app/src/bridge)
    target_compile_definitions(wabcut_export_ui_tests PRIVATE
        WABCUT_VERSION="${PROJECT_VERSION}"
        WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media"
        # The Qt installation's QML modules (QtCore, QtQuick.Dialogs, ...), independent of what windeployqt
        # copied next to the binaries.
        WABCUT_QT_QML_DIR="${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}")
    wabcut_allow_qt_generated_code(wabcut_export_ui_tests)
endif()
