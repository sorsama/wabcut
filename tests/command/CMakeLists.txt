wabcut_add_test_suite(command
    SOURCES
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        CommandHarness.h
        McpHarness.h
        test_caption_commands.cpp
        test_client_hooks.cpp
        test_clipboard_commands.cpp
        test_project_panel_commands.cpp
        test_collaboration_commands.cpp
        test_cli.cpp
        test_docs.cpp
        test_edit_commands.cpp
        test_editing_commands.cpp
        test_effects_library_commands.cpp
        test_engine.cpp
        test_file_commands_p2.cpp
        test_finishing_commands.cpp
        test_timeline_commands.cpp
        test_gaming_commands.cpp
        test_graphics_commands.cpp
        test_vr_ograf_commands.cpp # 0.7 (cards 38 and 178)
        test_graphics_parity_commands.cpp
        test_spelling_commands.cpp
        test_interchange_commands.cpp
        test_parity_commands.cpp
        test_mcp_files.cpp
        test_marker_commands.cpp
        test_sources_commands.cpp # 0.7 (card 177): sources and citations
        test_motion_commands.cpp
        test_mcp_docs.cpp
        test_mcp_golden.cpp
        test_mcp_parity.cpp
        test_mcp_protocol.cpp
        test_mcp_tools.cpp
        test_python_api.cpp # 0.7 (C8): the Python package's generated files, edit_transaction, the audit trail
        test_query_commands.cpp
        test_registry.cpp
        test_nesting_commands.cpp
        test_r08_c3.cpp # 0.7, stream C, batch C3
        test_a6_commands.cpp # 0.7 (batch A6): clip.lock, sequence.moveTrack, Match Frame on a nest
        test_r08_c5.cpp # 0.7 (C5): consolidate duplicates, rights log, credits roll
        test_a7_commands.cpp # 0.7 (batch A7): marker.removeMany / moveMany / duplicate, clip.setNote, clip.pinKeyframe, ripple track keyframes
        test_a8_commands.cpp # 0.7 (batch A8): clip.restoreUnrendered
        test_a10_commands.cpp # 0.7 (batch A10): keyframes.ease, Motion Blur as an effect
        test_track_group_commands.cpp # 0.7 (batch A9, card 124): trackGroup.*
        test_audition_commands.cpp # 0.6 (card #74)
        test_remap_commands.cpp
        test_requests.cpp
        test_rhythm_commands.cpp
        test_schema.cpp
        test_soak.cpp
        test_source_commands.cpp
        test_title_commands.cpp
        test_tool_export.cpp
    LIBS wabcut_command wabcut_mcp wabcut_ai_edit wabcut_interchange)
target_include_directories(wabcut_command_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model)
target_compile_definitions(wabcut_command_tests PRIVATE
    WABCUT_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs"
    WABCUT_PYTHON_PACKAGE_DIR="${CMAKE_SOURCE_DIR}/python/wabcut"
    WABCUT_KEYMAP_DIR="${CMAKE_SOURCE_DIR}/src/keymap/keymaps"
    WABCUT_COMMAND_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
# The stdio tests spawn wabcut-cli mcp: full builds only (core builds have no wabcut-cli, see tests/CMakeLists.txt).
if(TARGET wabcut-cli)
    target_sources(wabcut_command_tests PRIVATE test_mcp_stdio.cpp test_python_package.cpp)
    target_compile_definitions(wabcut_command_tests PRIVATE WABCUT_CLI_PATH="$<TARGET_FILE:wabcut-cli>")
    add_dependencies(wabcut_command_tests wabcut-cli)
endif()

# P.2 (E6): the Source and Program Monitors' commands (clip.setMotion, ...).
target_sources(wabcut_command_tests PRIVATE test_monitor_commands.cpp)
# P1 round (#25, stream 2C): sequence.mediaUsage, the media usage report.
target_sources(wabcut_command_tests PRIVATE test_media_usage_commands.cpp)
# P1 round (#57, stream 3C): caption tracks in timeline hand-offs (report rows, interchange.export captionSidecars).
target_sources(wabcut_command_tests PRIVATE test_caption_handoffs.cpp)

# 0.7 (card 035): review.importComments, the comments a review link brings back.
target_sources(wabcut_command_tests PRIVATE test_review_comments_command.cpp)
