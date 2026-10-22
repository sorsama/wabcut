# UI shell suite: loads the real "Wabcut" QML module (static wabcut_ui + its plugin) offscreen.
# Screenshots land in <build>/test-output/ui/ (see ShellFixture.h for the offscreen approach).
wabcut_add_test_suite(app
    SOURCES
        ShellFixture.h
        ShellFixture.cpp
        test_app_startup.cpp
        test_accessibility.cpp
        A11yAudit.h
        test_accessibility_m5n.cpp
        test_audio_ui.cpp
        test_audio_channels_ui.cpp
        test_autosave_background.cpp
        test_render_in_to_out.cpp # 0.1.1: Sequence > Render In to Out
        test_captions_workshop.cpp
        test_clip_audio.cpp
        test_clipboard_ui.cpp
        test_brand_icons.cpp
        test_design_language.cpp
        test_echo_motion.cpp # design language v3: the echo mark, its loaders and the launch intro
        test_essential_sound.cpp
        test_essential_graphics.cpp
        test_brand_lottie_ui.cpp
        test_gaming_export_ui.cpp
        test_gaming_start.cpp
        test_layout_picker.cpp
        test_highlights_panel.cpp
        test_graphics_canvas.cpp
        test_gaming_callouts.cpp
        test_extensions_ui.cpp
        test_finishing_ui.cpp
        test_effect_presets_ui.cpp
        test_spelling_ui.cpp
        test_graphics_tools_ui.cpp
        test_frame_inspector.cpp
        test_history_review.cpp
        test_home_ui.cpp
        test_i18n.cpp
        test_icons.cpp # 0.1.3: the Boxicons icon set
        test_layout.cpp
        test_library.cpp
        test_project_panel.cpp # 0.2 P.3: metadata, Search Bins, Freeform view, XMP, Productions
        test_masks_ui.cpp
        test_media_bridge.cpp
        test_mixer_automation.cpp
        test_monitor_docking.cpp # 0.1.3: the monitor in a shown window, panel docking
        test_motion_ui.cpp # P.2: Time Interpolation, Warp Stabilizer's Analyze, New Item
        test_mixer_routing.cpp
        test_clip_mixer.cpp
        test_take_recovery_ui.cpp
        test_waveform_building.cpp
        test_multicam.cpp
        test_onboarding.cpp
        test_palette_access.cpp
        test_perf_gating.cpp
        test_perf_ui.cpp
        test_plugins_ui.cpp
        test_program_timebar.cpp # 0.1.3: the Program Monitor's time bar; deferred and live gutter drags
        test_qml_guards.cpp
        test_recovery_bridge.cpp
        test_review_wave4.cpp
        test_rhi.cpp
        test_scope_view_hdr.cpp # I.9: the nits / HLG % waveform graticule
        test_color_management_ui.cpp # 0.2: the Color Management dialog and the Media color space menu
        test_screenshots.cpp
        test_settings.cpp # 0.1.3: the Settings window, theme files, the rail's layout, the AI switch
        test_shell.cpp
        test_shortcuts_ui.cpp
        test_startup_ui.cpp
        test_snapshot_lifetime.cpp
        test_source_monitor.cpp
        test_theme.cpp
        test_keymap_dispatcher.cpp
        test_timeline.cpp
        test_timeline_edit.cpp
        test_timeline_parity.cpp
        test_timeline_pro.cpp
        test_timeline_toolbar.cpp # 0.1.3: snapped drops, edit cursors, the Tracks toolbar
        test_time_remap_ui.cpp
        test_transcript_ui.cpp
        test_voice_over_ui.cpp
        test_workspaces.cpp # 0.1.3: customizable workspaces
        TimelineHarness.h
        ${CMAKE_SOURCE_DIR}/tests/model/TimelineFixture.h
        ${CMAKE_SOURCE_DIR}/tests/model/TimelineFixture.cpp
    LIBS wabcut_uiplugin wabcut_ui Qt6::Gui Qt6::Quick Qt6::QuickControls2)
target_compile_definitions(wabcut_app_tests PRIVATE
    # Main.qml shows Qt.application.version; the app sets it from the project version, so the tests do too.
    WABCUT_VERSION="${PROJECT_VERSION}"
    # test_i18n scans the QML sources for untranslated user-visible strings.
    WABCUT_APP_QML_DIR="${CMAKE_SOURCE_DIR}/src/app/qml"
    # M5f: the Home screen imports interchange fixtures; the Shortcuts editor imports the keymap's .kys fixtures.
    WABCUT_INTERCHANGE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/interchange/fixtures"
    WABCUT_KEYMAP_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/keymap/data"
    # 0.2: the Media Browser tests preview, ingest and transcode generated clips (tools/gen_test_media.py; skipped
    # when they were not generated).
    WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media")

# KeymapDispatcher bridge tests (owner: Keymap): the bridge header lives in src/app/src/bridge, the scene in this
# directory.
target_include_directories(wabcut_app_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app/src)
target_link_libraries(wabcut_app_tests PRIVATE wabcut_keymap)
target_compile_definitions(wabcut_app_tests PRIVATE WABCUT_APP_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# M5b gesture tests reuse the model tests' ASCII timeline pictures (tests/model/TimelineFixture).
target_include_directories(wabcut_app_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model)
# I.6: the mask UI tests track synthetic moving pictures (tests/masks/SyntheticMotion.h).
target_include_directories(wabcut_app_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/masks)

# M5i: the whole QML tree stays qmllint clean. This runs qmllint with the response file of Qt's generated
# wabcut_ui_qmllint target (the module's files, import paths and resources), and any warning fails the test.
if(TARGET Qt6::qmllint)
    set(wabcut_qmllint_test "qmllint: the wabcut_ui QML module has no warnings")
    add_test(NAME "${wabcut_qmllint_test}"
        COMMAND $<TARGET_FILE:Qt6::qmllint> "@${CMAKE_BINARY_DIR}/src/app/.rcc/qmllint/wabcut_ui.rsp"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/app")
    set_tests_properties("${wabcut_qmllint_test}" PROPERTIES
        LABELS app
        TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "Warning: |Error: ")
endif()

# M5m: the plugin browser tests scan the A.6 test plugins (tests/plughost builds them into <build>/test-plugins) with the
# real wabcut-plughost helper; plugin editor windows are never opened in tests.
if(TARGET wabcut_plugins)
    target_compile_definitions(wabcut_app_tests PRIVATE
        WABCUT_TEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/test-plugins"
        WABCUT_PLUGHOST_PATH="$<TARGET_FILE:wabcut-plughost>")
    add_dependencies(wabcut_app_tests wabcut-plughost wabcut_test_clap_gain wabcut_test_clap_crash wabcut_test_clap_hang
                     wabcut_test_vst3_gain)
endif()

# I.7 crash safety (owner: Graphics/integration): a helper process that installs the crash handler like main() and then
# crashes on purpose; test_crash_safety.cpp checks its report, dump and redaction and the next start's notice.
add_executable(wabcut_crash_helper crash_helper.cpp)
target_link_libraries(wabcut_crash_helper PRIVATE wabcut_crash)
wabcut_target_defaults(wabcut_crash_helper)
target_sources(wabcut_app_tests PRIVATE test_crash_safety.cpp)
# I.7 first-run performance check: the PerformanceCheck bridge and FirstRunCheckDialog in the window.
target_sources(wabcut_app_tests PRIVATE test_first_run_check.cpp)
add_dependencies(wabcut_app_tests wabcut_crash_helper)
target_compile_definitions(wabcut_app_tests PRIVATE WABCUT_CRASH_HELPER="$<TARGET_FILE:wabcut_crash_helper>")
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(wabcut_app_tests PRIVATE nlohmann_json::nlohmann_json wabcut_crash)

# M5n: the extension tests run the real wabcut-scripthost, and the crash test the script suite's fake helper, from bin/.
target_link_libraries(wabcut_app_tests PRIVATE wabcut_script)
if(TARGET wabcut-scripthost)
    add_dependencies(wabcut_app_tests wabcut-scripthost)
endif()
if(TARGET wabcut_fake_scripthost)
    add_dependencies(wabcut_app_tests wabcut_fake_scripthost)
endif()

# M5o gaming: moments (Fork S) - slow-mo, ramps, freeze + replay, fast forward and punch-in presets, their menu and
# popover.
target_sources(wabcut_app_tests PRIVATE test_moments_ui.cpp)

# Wave-5 review (A.8 voice-over): the VoiceOver bridge on the Null engine and the Null input, no window.
target_sources(wabcut_app_tests PRIVATE test_voice_over_bridge.cpp)

# P.2 (E5): Premiere's editing and sequence commands (the Editing bridge, its menus, dialogs and History panel).
target_sources(wabcut_app_tests PRIVATE test_editing_ui.cpp)
# 0.2 media formats: Field Options, Import Layered File and the Media Browser.
target_sources(wabcut_app_tests PRIVATE test_media_formats_ui.cpp)
find_package(ZLIB REQUIRED) # ../media/PsdWriter.h
target_link_libraries(wabcut_app_tests PRIVATE ZLIB::ZLIB)
# P.2 (E6): the Source and Program Monitors (direct manipulation of Motion; Loop, Play In to Out and the monitor keys).
target_sources(wabcut_app_tests PRIVATE test_motion_overlay.cpp test_monitors.cpp)
