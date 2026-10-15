# Editor services (owner: Graphics/playback): import, library -> timeline drops, project lifecycle (save, autosave,
# recovery), offline media and relinking, the playback session and the export seam. Uses the generated test media.
wabcut_add_test_suite(editor
    SOURCES
        test_activity.cpp
        test_autosave_job.cpp
        test_capsule.cpp
        test_color_editor.cpp
        test_editor.cpp
        test_path_fileset.cpp
        test_performance_probe.cpp
        test_preview_render.cpp
        test_proxies.cpp
        test_recovery_scan.cpp
        test_release_import.cpp
        test_tracking.cpp
        test_wave3.cpp
    LIBS wabcut_editor wabcut_media wabcut_ai_edit
    PROPERTIES FIXTURES_REQUIRED wabcut_test_media)
target_compile_definitions(wabcut_editor_tests PRIVATE
    WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media"
    WABCUT_INTERCHANGE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/interchange/fixtures"
    WABCUT_FFMPEG_EXECUTABLE="${WABCUT_FFMPEG_EXECUTABLE}")
target_include_directories(wabcut_editor_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/masks)
# I.7: the path rules (src/editor/src/PathSafety.h, internal to the library) are tested directly.
target_include_directories(wabcut_editor_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/editor/src)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(wabcut_editor_tests PRIVATE nlohmann_json::nlohmann_json)
