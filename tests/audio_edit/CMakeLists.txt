# Audio engine: the dialogue-first audio commands (roles, track/master effects, loudness, auto-ducking). The command
# and planner tests are pure (they run in the core preset); the analysis tests render through playback (full builds).
set(wabcut_audio_edit_test_sources
    ../model/TimelineFixture.cpp
    ../model/TimelineFixture.h
    SyncTestSignals.h
    MusicTestSignals.h
    test_audio_commands.cpp
    test_essential_sound.cpp
    test_remix.cpp
    test_audio_sync.cpp
    test_sync_commands.cpp
    test_ducking.cpp
    test_enhance_commands.cpp
    test_plugin_commands.cpp
    test_voice_over_takes.cpp
    test_audio_channels_commands.cpp
    test_streamer_audio.cpp)
set(wabcut_audio_edit_test_libs wabcut_audio_edit wabcut_ai_edit wabcut_ai_analysis)
if(NOT WABCUT_CORE_ONLY)
    list(APPEND wabcut_audio_edit_test_sources test_audio_analysis.cpp test_audio_sync_media.cpp test_remix_media.cpp
         test_streamer_audio_media.cpp)
    list(APPEND wabcut_audio_edit_test_libs wabcut_audio_edit_media)
endif()
wabcut_add_test_suite(audio_edit SOURCES ${wabcut_audio_edit_test_sources} LIBS ${wabcut_audio_edit_test_libs})
target_include_directories(wabcut_audio_edit_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command)

# wabcut-cli end to end on a WAV the test writes (full builds).
if(NOT WABCUT_CORE_ONLY)
    wabcut_add_test_suite(audio_edit_cli
        SOURCES ../model/TimelineFixture.cpp ../model/TimelineFixture.h MusicTestSignals.h test_cli_audio.cpp
        LIBS wabcut_model wabcut_media wabcut_audio_edit wabcut_ai_analysis)
    target_include_directories(wabcut_audio_edit_cli_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model)
    find_package(nlohmann_json CONFIG REQUIRED)
    target_link_libraries(wabcut_audio_edit_cli_tests PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(wabcut_audio_edit_cli_tests PRIVATE WABCUT_CLI_PATH="$<TARGET_FILE:wabcut-cli>")
    add_dependencies(wabcut_audio_edit_cli_tests wabcut-cli)
endif()
