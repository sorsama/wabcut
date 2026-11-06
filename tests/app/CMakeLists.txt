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
        test_d2_text_audio_ui.cpp # font picker, spoken language, caption gallery, auto-mix
        test_d3_shell_ui.cpp # Window menu order, Gaming workspace, hidden tabs, read-only badge, window drops, presets, custom words
        test_d4_shell_ui.cpp # Open with, Save Version, Report a Bug, system menu and Snap Layouts, mixed languages
        test_d6_sources_ui.cpp # 0.7 (batch D6): Sources, Audio Description
        test_d6_ui.cpp # 0.7 (batch D6): Save for an Older Version, bilingual captions, Sources, Audio Description
        test_d5_audio_ui.cpp # 0.7: mixer menu (Show/Hide Tracks, Meter Input(s) Only, Loop), Match tab, Loudness panel and radar, captions to graphics
        test_d7_audio_ui.cpp # 0.7: Dynamics Processing in Effect Controls, the 7.1 master in the mixer
        test_d8_spectral_ui.cpp # 0.7: the Spectral tab of Audio Tools (card 047)
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
        test_effect_controls_text.cpp # 0.6: Text sections, keyframe copy and paste, several clips at once
        test_effect_controls_r08.cpp # 0.7: Nearest Neighbor Scaling and Blend Mode rows, the Blend Mode shortcuts, pinned favourites
        test_ai_video_ui.cpp # 0.7: AI Restore / AI Denoise in the shell (header, Process, Delete, export notice, screenshots)
        test_retouch_ui.cpp # 0.7: Skin Smoothing and AI Face Retouch in the shell (header, Process, undo, screenshots)
        test_eye_contact_ui.cpp # 0.7: AI Eye Contact and the Morph Cut's face detector in the shell (headers, Process, undo, screenshots)
        test_effect_shortcuts.cpp # 0.7: keys for individual effects and presets
        test_effect_presets_ui.cpp
        test_master_clip_effects_ui.cpp # 0.6 (#71): Master Clip Effects in Effect Controls and the Effects panel
        test_spelling_ui.cpp
        test_graphics_tools_ui.cpp
        test_frame_inspector.cpp
        test_history_review.cpp
        test_home_ui.cpp
        test_i18n.cpp
        test_icons.cpp # 0.1.3: the Boxicons icon set
        test_layout.cpp
        test_library.cpp
        test_library_p1.cpp # 0.6 (1D): Move to Bin, Cut / Paste, bin drags, Link Media, proxies, sizes, Properties
        test_project_panel.cpp # 0.2 P.3: metadata, Search Bins, Freeform view, XMP, Productions
        test_masks_ui.cpp
        test_track_motion_ui.cpp # 0.7 (card #83): Track Motion - the section, the monitor's box and the track job
        test_track_effects_ui.cpp # 0.7 (card #139): effects on a whole video track - the bridge, the stack, the popover
        test_vr_ui.cpp # 0.7 (card 38): VR / 360 video in the interface
        test_graphics_r08.cpp # 0.7 (cards #71 #81 #87): keyed template fields, styling part of a text, template folders and sort
        test_media_bridge.cpp
        test_mixer_automation.cpp
        test_mixer_effects.cpp # r07: the mixer's "+ Effect" menu, effect settings, the Enhance Speech slot
        test_monitor_docking.cpp # 0.1.3: the monitor in a shown window, panel docking
        test_motion_ui.cpp # P.2: Time Interpolation, Warp Stabilizer's Analyze, New Item
        test_gyro_ui.cpp # 0.7 (card #130): Warp Stabilizer's Gyro (camera metadata) method
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
        test_chart_match_ui.cpp # 0.7: the Color panel's Color Chart section and the monitor's corner handles
        test_screenshots.cpp
        test_settings.cpp # 0.1.3: the Settings window, theme files, the AI switch
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
# 0.7: the AI video effects' test doubles (a fake backend and frame source: tests/ai/video/VideoTestUtil.h).
target_include_directories(wabcut_app_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/ai)
# 0.7: the colour-chart tests' synthetic ColorChecker picture (tests/color/ChartScene.h).
target_include_directories(wabcut_app_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/color)

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

# 0.7: OpenFX video plug-ins in Effect Controls and the Effects panel: the test plug-ins (tests/ofxhost builds them into
# <build>/test-ofx) with the real wabcut-ofxhost helper; no plug-in window exists to open.
if(TARGET wabcut_ofx)
    target_sources(wabcut_app_tests PRIVATE test_ofx_effect_controls.cpp)
    target_compile_definitions(wabcut_app_tests PRIVATE
        WABCUT_TEST_OFX_DIR="${CMAKE_BINARY_DIR}/test-ofx"
        WABCUT_OFXHOST_PATH="$<TARGET_FILE:wabcut-ofxhost>")
    add_dependencies(wabcut_app_tests wabcut-ofxhost wabcut_test_ofx_main wabcut_test_ofx_crash)
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
# UX rework (stream S): the editor shell's C++ services - single instance (AppShell), the window's place between runs,
# File > Save a Copy and Revert - and the title bar's menu bar (it replaced the left rail).
target_sources(wabcut_app_tests PRIVATE test_shell_services.cpp test_menu_bar.cpp)
# UX rework (stream S): the shell's feedback - the toast's action, open and save failures, the sample project's leave
# prompt, the History panel keys, Reveal in Project.
target_sources(wabcut_app_tests PRIVATE test_shell_feedback.cpp)
# UX rework (stream L): AppSettings (start-up choice, the Settings search, palette rows) and the export memory and
# finished-job details.
target_sources(wabcut_app_tests PRIVATE test_qol_settings_export.cpp)
# UX rework (stream T): typed playhead entry, gap selection, the track header menu, default transitions, Reveal in
# Project, Library drops with marks, the Source Monitor drop and marker tooltips (C++ side), and their QML in the shell.
target_sources(wabcut_app_tests PRIVATE test_timeline_qol.cpp test_timeline_qol_ui.cpp)
# 0.4 (qol round): stream E's background work (export summary, quit guard, activity pill, keep awake, taskbar) and
# stream B's keys and transport (nudge, clip volume keys, play around, paging, numpad timecode entry).
target_sources(wabcut_app_tests PRIVATE test_background_work.cpp test_keys_transport.cpp)
# 0.5 (p0 round, stream C): Scene Edit Detection (bridge, dialog, menus) and Remove dead time's review.
target_sources(wabcut_app_tests PRIVATE test_scene_detect.cpp test_dead_time_review.cpp)
# 0.5 (p0 round, stream D): OBS chapter markers on import (the summary, the preference) and the source clip transcript
# (the Transcript panel's Source mode).
target_sources(wabcut_app_tests PRIVATE test_source_transcript.cpp)
# 0.5 (p0 round, stream A): caption tracks as timeline lanes (drawing, selection, trims, moves, splits, deletes, the
# headers in the shell) and the waveforms of nested, multicam and merged audio clips (NestedPeaks).
target_sources(wabcut_app_tests PRIVATE test_caption_lanes.cpp test_nested_peaks.cpp test_nested_mix_peaks.cpp)
# 0.5 (p0 round, stream B): the Audio Meters panel and audio scrubbing in the shell.
target_sources(wabcut_app_tests PRIVATE test_audio_meters.cpp)
# 0.5 (p0 round, stream E): issue #10 - the Graphics card setting and the Performance check's GPU row - and the opt-in
# updates (the signed manifest, the verified download and the install after closing, against a loopback fake server;
# no installer ever runs: the launcher is injected).
target_sources(wabcut_app_tests PRIVATE test_gpu_choice_ui.cpp test_updates.cpp)
# 0.6 (P1 round, stream 1A): timeline editing - Square corners, edit points, markers, non-ripple Q / W, track header
# Shift-clicks and Sync Lock, the Clip Mismatch Warning and the out-of-sync tags.
target_sources(wabcut_app_tests PRIVATE test_p1_timeline.cpp)
# 0.6 (card #74): auditions - the Take Selector, Clip > Auditions, the timeline badge and the Program Monitor's take strip.
target_sources(wabcut_app_tests PRIVATE test_auditions_ui.cpp)
# Platform round (macOS first-class): Finder's file-open events reach the window (FileOpenRelay).
target_sources(wabcut_app_tests PRIVATE test_file_open_relay.cpp)
find_package(Qt6 REQUIRED COMPONENTS Network)
target_link_libraries(wabcut_app_tests PRIVATE Qt6::Network) # the fake update server (QTcpServer)
if(WIN32)
    target_link_libraries(wabcut_app_tests PRIVATE bcrypt) # test_updates.cpp signs its manifests with a test key (CNG)
endif()
target_compile_definitions(wabcut_app_tests PRIVATE WABCUT_UPDATE_PUBLIC_KEY_DEFAULT="${WABCUT_UPDATE_PUBLIC_KEY}")
# Issue #10: the hybrid-GPU driver hints are exports of wabcut.exe itself (src/app/src/main.cpp); this reads them back
# from the executable's PE export table, with their values (build `wabcut` and `wabcut_pe_exports_check`).
if(WIN32 AND TARGET wabcut)
    add_executable(wabcut_pe_exports_check pe_exports_check.cpp)
    wabcut_target_defaults(wabcut_pe_exports_check)
    set(wabcut_pe_exports_test "wabcut.exe exports the hybrid GPU driver hints")
    add_test(NAME "${wabcut_pe_exports_test}"
        COMMAND wabcut_pe_exports_check $<TARGET_FILE:wabcut> NvOptimusEnablement=1 AmdPowerXpressRequestHighPerformance=1)
    set_tests_properties("${wabcut_pe_exports_test}" PROPERTIES LABELS app TIMEOUT 60)
endif()
# 0.6 (P1 round, stream 1E): the shell - menu rows that grey out with their reasons, tooltips, Find in the active
# panel and the panel group keys, saved workspace layouts and the lock, the History panel, update notices and What's New.
target_sources(wabcut_app_tests PRIVATE test_shell_p1.cpp)
# P1 round, stream 3E: interface size and window fit (#47), cache and proxy folders (#48), screen-reader audit (#61).
target_sources(wabcut_app_tests PRIVATE test_settings_3e.cpp test_screen_reader_p1.cpp)
# 0.6 (P1 round, stream 1C): export - estimates for every format, vertical presets, size limits, offline media before
# an export and Retry, low disk space, YouTube chapters to the clipboard, Export Frame sizes and frames at markers.
target_sources(wabcut_app_tests PRIVATE test_export_p1.cpp)
# 0.6 (P1 round, stream 2D): the Library's usage (Find in Timeline), deleting media in use with its clips, twirl-down
# bins; Home's Rename / Duplicate / Versions / Move to Recycle Bin, recovery of a project that fails to open, and Home's
# Import folder with bins.
target_sources(wabcut_app_tests PRIVATE test_library_usage.cpp test_home_files.cpp)
# 0.6 (P1 round, stream 2E): the Transcript tab's speakers (#36), the voice-over script prompter (#53) and the loudness
# QC: where the loudest moments and the true-peak overs are (#54).
target_sources(wabcut_app_tests PRIVATE test_audio_text_p1.cpp)
# 0.6 (P1 round, #37): Correct the transcript from your script - the sheet, Apply as one undo step, Update captions.
target_sources(wabcut_app_tests PRIVATE test_script_correction_ui.cpp)
# 0.6 (P1 round, task D1): #19 - Toggle Proxies and Playback Resolution as commands, View menu rows and the Source
# Monitor's settings (no separate transport button: the quality pill shows the state); #50 - Install Motion Graphics
# Template... and Export template... from the Graphics menu, Callouts > Browse and a template's menu, and template
# files dropped or imported rather than taken as media.
target_sources(wabcut_app_tests PRIVATE test_monitor_proxies_p1.cpp test_template_files_p1.cpp)
# 0.6 (P1 round, stream 3B): Paused Resolution, the Program Monitor edit overlay, captions picked in the monitor.
target_sources(wabcut_app_tests PRIVATE test_monitors_3b.cpp)
# 0.6 (P1 round, stream 2C): the queue across restarts, batch exports, the media usage report.
target_sources(wabcut_app_tests PRIVATE test_export_2c.cpp)
# 0.6 (P1 round, task D5): #35 - the four caption keys (Ctrl+Alt+A / C, Ctrl+Alt+Up / Down), their reasons in the palette,
# the Sequence > Captions menu and a new caption's text selected in the Captions tab.
target_sources(wabcut_app_tests PRIVATE test_captions_keys_p1.cpp)
# 0.6 (P1 round, stream 3A): Make Shorts / square version as a job with progress and Cancel (#55), with the facecam
# region and split the layout picker remembers (#51).
target_sources(wabcut_app_tests PRIVATE test_gaming_versions.cpp)
# 0.6 (P1 round, stream 3A, #62): the creator autopilot - autopilot.broll / publishKit / pacing through the catalog on
# fixtures, and the Creator Autopilot sheet (screenshots, dark and light).
target_sources(wabcut_app_tests PRIVATE AutopilotTestUtil.h test_autopilot.cpp test_autopilot_ui.cpp)
# 0.6 (P1 round, stream 3C): dialogue / music / effects stems with the master (#52); caption tracks with timeline
# hand-offs and Export all tracks (#57); the Audio stems row and Export Timeline's Captions row (screenshots).
target_sources(wabcut_app_tests PRIVATE test_delivery_p1.cpp)
# 0.6 (P1 round, stream 2A): Render Selection, Delete Render Files (In to Out), render files as a cache and the cache
# limits kept automatically (#7, #58; screenshots, dark and light).
target_sources(wabcut_app_tests PRIVATE test_render_files.cpp)
# 0.6 (P1 round, task D9, #8): the beat tools - Sequence > Beat Grid and Clip > Detect Beats..., the Timeline Display
# beat grid with its ruler ticks, the palette rows' reasons, and Detect / Snap / Cut / Clear each one undo step.
target_sources(wabcut_app_tests PRIVATE test_beats_ui.cpp)
# 0.7 (stream A, batch A2): middle-button pan and held H / Z, video solo and the track keys, Add Marker from In/Out,
# the edit overlay keys, Snap to, clip duration and audio track names on clips, Add tracks when editing, Track Select Backward.
target_sources(wabcut_app_tests PRIVATE test_timeline_a2.cpp)
# 0.7 (batch A3): the Markers panel's Import and Copy Frame in the real shell (the rest of the batch's tests sit in
# test_monitors.cpp, test_onboarding.cpp, test_shortcuts_ui.cpp, test_workspaces.cpp, test_i18n.cpp and the command / keymap suites).
target_sources(wabcut_app_tests PRIVATE test_a3_ui.cpp)
# 0.7 (C2): the Streaming loudness chip, the default export folder, masters of used subclips, the export queue folder
# during progress, Collect Files' size estimate on a worker.
target_sources(wabcut_app_tests PRIVATE test_r08_c2.cpp)
# 0.7 (C3): Date created and column layouts, the nest bin, New Sequence per Clip, Reload Footage, the Library list
# cells' Tab / Enter stepping, the Collect Files dialog's Handles and Sequences, Ingest > Copy and Create Proxies.
target_sources(wabcut_app_tests PRIVATE test_r08_c3.cpp)
# 0.7 (C4): the Library list's thumbnails, the export history, the pre-export check dialog, the newer codecs' notes.
target_sources(wabcut_app_tests PRIVATE test_r08_c4.cpp)

# 0.7 (C5): Consolidate Duplicates, the rights and clearance log, poly WAV channel names, Collect Files layout.
target_sources(wabcut_app_tests PRIVATE test_r08_c5.cpp)
# 0.7 (batch A4): Go to Gap in Track selecting the gap, Select Label Group and the named label menus, track label colours
# (header bar, lane wash, menu), the Menu key / Shift+F10 on the timeline's menus, and slip with the trim tool.
target_sources(wabcut_app_tests PRIVATE test_timeline_a4.cpp)
# 0.7 (batch A5): the settings bundle (export / import / reset), where autosaves and backups go, the taskbar jump list, and the
# Settings > General rows and dialogs for them; the trim-mode check carried over from A4 (Shift adds an edit point).
target_sources(wabcut_app_tests PRIVATE test_a5.cpp test_timeline_a5.cpp)
# 0.7 (batch A6): Alt-drag then Ctrl inserts the copy, the fx badge menu (Show Clip Keyframes), Lock Clip, dragging track headers
# to reorder tracks, and Match Frame into a nest.
target_sources(wabcut_app_tests PRIVATE test_timeline_a6.cpp)

# 0.7 (card "Background rendering while idle", stream C, batch C6): the idle renderer in the app - the options and their
# settings, Sequence > Background Rendering, the background tasks list with Stop, Delete Render Files holding it until the
# next edit, and a manual render taking over (screenshots, dark and light).
target_sources(wabcut_app_tests PRIVATE test_background_render.cpp)
# 0.7 (card "Dolby Vision 8.4 and HDR10+ dynamic metadata"): the Export drawer's Dynamic metadata row (screenshots, dark and light).
target_sources(wabcut_app_tests PRIVATE test_dynamic_metadata_ui.cpp)

# 0.7 (card 094, stream C, batch C7): search across all projects of a production in the Production panel - typing, the type
# and "search in" choices, Import and Browse on a result, the empty state, a narrow window (screenshots, dark and light).
target_sources(wabcut_app_tests PRIVATE test_production_search.cpp)
# 0.7 (batch A7): several markers at once (ruler, panel, bridge), clip notes, pinned keyframes in Effect Controls, Ripple Track Keyframes,
# and the pacing strip.
target_sources(wabcut_app_tests PRIVATE test_a7_ui.cpp)
# 0.7 (batch A8): the Premiere keys that did nothing (card 024), Render and Replace for nested sequences (card 069).
target_sources(wabcut_app_tests PRIVATE test_a8.cpp)
# 0.7 (batch A9, card 124): track groups - the lanes' header rows and folding, the controller's group steps, the keys, the header rows, menus and
# the Audio Track Mixer's group strip in the shell (dark and light screenshots).
target_sources(wabcut_app_tests PRIVATE test_track_groups_ui.cpp)
# 0.7 (batch A9, card 110): the video output to another display - the planner (picture placement, cadence, Match Refresh Rate), the
# output following its display through plugging and unplugging, the sync offset, and the Video Output dialog and output window in the shell.
target_sources(wabcut_app_tests PRIVATE test_video_output.cpp)
# 0.7 (batch A10, card 96): the Graph Editor - its engine (curves, legend, the shared key selection, dragging keys and handles, numeric
# entry, easing presets, adding keys, the views) and the panel in the shell (both themes, the speed graph, real pointer gestures).
target_sources(wabcut_app_tests PRIVATE test_a10_ui.cpp)

# 0.7 (card 050, stream C, batch C8): Python scripts - File > Run Python Script... (docs/python.md): the script header and the
# remembered permissions, finding Python, a script run against the open project over the private local socket (one undo step,
# nothing on failure or Stop), and the dialog (screenshots, dark and light).
target_sources(wabcut_app_tests PRIVATE test_scripts.cpp)
target_compile_definitions(wabcut_app_tests PRIVATE WABCUT_PYTHON_FOLDER="${CMAKE_SOURCE_DIR}/python")
# 0.7 (card 078): File > Capture… - the Capture bridge and panel over the fake capture backend.
target_sources(wabcut_app_tests PRIVATE test_capture_ui.cpp)
target_link_libraries(wabcut_app_tests PRIVATE wabcut_capture)

# 0.7 (card 035): cloud projects, review links and stock media over fake servers (no network): the bridges, the dialog and the panel.
target_sources(wabcut_app_tests PRIVATE test_cloud_ui.cpp)
target_link_libraries(wabcut_app_tests PRIVATE wabcut_cloud)
