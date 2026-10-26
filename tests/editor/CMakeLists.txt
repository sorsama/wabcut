# Editor services (owner: Graphics/playback): import, library -> timeline drops, project lifecycle (save, autosave,
# recovery), offline media and relinking, the playback session and the export seam. Uses the generated test media.
find_package(ZLIB REQUIRED) # ../media/PsdWriter.h
wabcut_add_test_suite(editor
    SOURCES
        test_activity.cpp
        test_autosave_job.cpp
        test_camera_cards.cpp # 0.2: the Media Browser, spanned clips, layered files
        test_capsule.cpp
        test_chapter_import.cpp # P0 round: OBS chapter markers on import
        test_color_editor.cpp
        test_editor.cpp
        test_image_sequence.cpp # 0.2: Import > Image Sequence
        test_path_fileset.cpp
        test_performance_probe.cpp
        test_preview_render.cpp
        test_production.cpp # 0.2 P.3: Productions, project locks, import from another project
        test_proxies.cpp
        test_recovery_scan.cpp
        test_system_spell_checker.cpp # 0.2: spell check
        test_release_import.cpp
        test_tracking.cpp
        test_wave3.cpp
    LIBS wabcut_editor wabcut_media wabcut_ai_edit ZLIB::ZLIB
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
