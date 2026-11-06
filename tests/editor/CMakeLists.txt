# Editor services (owner: Graphics/playback): import, library -> timeline drops, project lifecycle (save, autosave,
# recovery), offline media and relinking, the playback session and the export seam. Uses the generated test media.
find_package(ZLIB REQUIRED) # ../media/PsdWriter.h
wabcut_add_test_suite(editor
    SOURCES
        test_activity.cpp
        test_audiopost_import.cpp # 0.6: AAF / OMF import (card #68)
        test_autosave_job.cpp
        test_camera_cards.cpp # 0.2: the Media Browser, spanned clips, layered files
        test_capsule.cpp
        test_chapter_import.cpp # P0 round: OBS chapter markers on import
        ../model/TimelineFixture.cpp # 0.7: timeline pictures for the idle render plan tests
        ../model/TimelineFixture.h
        test_color_editor.cpp
        test_editor.cpp
        test_gyro_service.cpp # 0.7 (card #130): probing and reading a clip's camera gyro
        test_idle_render.cpp # 0.7: background rendering while idle (card "Background rendering while idle")
        test_image_sequence.cpp # 0.2: Import > Image Sequence
        test_path_fileset.cpp
        test_performance_probe.cpp
        test_preview_render.cpp
        test_production.cpp # 0.2 P.3: Productions, project locks, import from another project
        test_production_search.cpp # 0.7 (card 094): search across all projects of a production
        test_proxies.cpp
        test_recovery_scan.cpp
        test_system_spell_checker.cpp # 0.2: spell check
        test_release_import.cpp
        test_tracking.cpp
        test_vector_import.cpp # 0.7 (card #86): SVG, Illustrator and EPS import and play as vector stills
        test_wave3.cpp
    LIBS wabcut_editor wabcut_media wabcut_ai_edit ZLIB::ZLIB
    PROPERTIES FIXTURES_REQUIRED wabcut_test_media)
target_compile_definitions(wabcut_editor_tests PRIVATE
    WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media"
    WABCUT_INTERCHANGE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/interchange/fixtures"
    WABCUT_FFMPEG_EXECUTABLE="${WABCUT_FFMPEG_EXECUTABLE}")
target_include_directories(wabcut_editor_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/masks ${CMAKE_SOURCE_DIR}/tests/model)
# I.7: the path rules (src/editor/src/PathSafety.h, internal to the library) are tested directly.
target_include_directories(wabcut_editor_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/editor/src)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(wabcut_editor_tests PRIVATE nlohmann_json::nlohmann_json)

# 0.7 (card 029): autosaves and .bakN copies in the Auto-Save folder or a chosen root, how many are kept, recovery in all places.
target_sources(wabcut_editor_tests PRIVATE test_backup_policy.cpp)
