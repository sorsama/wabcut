# wabcut — Qt Quick desktop application (UI shell, QML bridges to the engine).
# Owner: UI/UX (QML) + feature owners for their bridge types. QML module URI: Wabcut.
#
# The QML module lives in the static library `wabcut_ui` so the app and the tests (tests/app) load the very
# same module. Consumers link the generated plugin target `wabcut_uiplugin` and import it with
# Q_IMPORT_QML_PLUGIN(WabcutPlugin). C++ bridge types live in src/bridge/ and are listed under SOURCES.

qt_add_library(wabcut_ui STATIC)

set_source_files_properties(
    qml/Preferences.qml
    qml/theme/Theme.qml
    qml/panels/workspace/Workspaces.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(wabcut_ui
    URI Wabcut
    VERSION 1.0
    SOURCES
        src/RhiPolicy.h
        src/RhiPolicy.cpp
        src/UiScale.h
        src/UiScale.cpp
        src/bridge/GenerateController.h
        src/bridge/GenerateController.cpp
        src/bridge/AudioMixerBridge.h
        src/bridge/AudioMixerBridge.cpp
        src/bridge/BridgeIds.h
        src/bridge/BundledFonts.h
        src/bridge/BundledFonts.cpp
        src/bridge/CaptionsBridge.h
        src/bridge/CaptionsBridge.cpp
        src/bridge/ClipAccessModel.h
        src/bridge/ClipAccessModel.cpp
        src/bridge/TimeRemapBridge.h
        src/bridge/TimeRemapBridge.cpp
        src/bridge/TimelineViewSpeed.cpp
        src/bridge/CommandBridge.h
        src/bridge/CommandBridge.cpp
        src/bridge/CommandPalette.h
        src/bridge/CommandPalette.cpp
        src/bridge/DemoProject.h
        src/bridge/DemoProject.cpp
        src/bridge/EffectControls.h
        src/bridge/EffectControls.cpp
        src/bridge/HomeBridge.h
        src/bridge/HomeBridge.cpp
        src/bridge/MediaBridge.h
        src/bridge/MediaBridge.cpp
        src/bridge/MeterBar.h
        src/bridge/MeterBar.cpp
        src/bridge/ParameterUi.h
        src/bridge/ParameterUi.cpp
        src/bridge/PeakSource.h
        src/bridge/PeakSource.cpp
        src/bridge/ProjectController.h
        src/bridge/ProjectController.cpp
        src/bridge/ProjectControllerEditing.cpp
        src/bridge/ProjectControllerTimeline.cpp
        src/bridge/ProjectControllerFinishing.cpp
        src/bridge/RendererInfo.h
        src/bridge/RendererInfo.cpp
        src/bridge/ShortcutsBridge.h
        src/bridge/ShortcutsBridge.cpp
        src/bridge/SliceShape.h
        src/bridge/SvgArt.h
        src/bridge/SvgArt.cpp
        src/bridge/SliceShape.cpp
        src/bridge/SourceMonitorBridge.h
        src/bridge/SourceMonitorBridge.cpp
        src/bridge/SourceWaveform.h
        src/bridge/SourceWaveform.cpp
        src/bridge/ProgramToolsBridge.h
        src/bridge/ProgramToolsBridge.cpp
        # M5i: the pro-grade timeline
        src/bridge/FilmstripSource.h
        src/bridge/FilmstripSource.cpp
        # I.9: nested filmstrips, sequence thumbnails
        src/bridge/NestedPictures.h
        src/bridge/NestedPictures.cpp
        src/bridge/MarkersBridge.h
        src/bridge/MarkersBridge.cpp
        src/bridge/SequenceTabsBridge.h
        src/bridge/SequenceTabsBridge.cpp
        src/bridge/StillFrame.h
        src/bridge/StillFrame.cpp
        src/bridge/TimelineAnalysis.h
        src/bridge/TimelineAnalysis.cpp
        src/bridge/TimelineViewDisplay.cpp
        src/bridge/TrimBridge.h
        src/bridge/TrimBridge.cpp
        # M5j: onboarding
        src/bridge/KeyboardTrainerBridge.h
        src/bridge/KeyboardTrainerBridge.cpp
        src/bridge/ProgramMonitor.h
        src/bridge/ProgramMonitor.cpp
        src/bridge/SequenceTimeBar.h # 0.1.3: the Program Monitor's time bar (ProgramTimeBar.qml)
        src/bridge/SequenceTimeBar.cpp
        src/bridge/TimelineEditing.h
        src/bridge/TimelineEditing.cpp
        src/bridge/TimelineGeometry.h
        src/bridge/TimelineGeometry.cpp
        src/bridge/TimelineOverview.h
        src/bridge/TimelineOverview.cpp
        src/bridge/TimelineView.h
        src/bridge/TimelineView.cpp
        src/bridge/TimelineViewInput.cpp
        src/bridge/TimelineViewFade.cpp
        src/bridge/TimelineViewRender.cpp
        src/bridge/TimelineViewVolume.cpp
        src/bridge/Titles.h
        src/bridge/Titles.cpp
        src/bridge/TranscriptBridge.h
        src/bridge/TranscriptBridge.cpp
        src/bridge/TranscriptReview.cpp
        # M5k: the Library (bins, list view, subclips, Find) and Explain this frame
        src/bridge/FrameInspector.h
        src/bridge/FrameInspector.cpp
        src/bridge/LibraryBridge.h
        src/bridge/LibraryBridge.cpp
        src/bridge/LibraryModel.h
        src/bridge/LibraryModel.cpp
        src/bridge/PosterFrames.h
        src/bridge/PosterFrames.cpp
        # M5l: multicam, nested sequences, mixer automation and live loudness, Enhance Speech / stems / sync
        src/bridge/AppAudioServices.h
        src/bridge/AppAudioServices.cpp
        src/bridge/ClipAudioBridge.h
        src/bridge/ClipAudioBridge.cpp
        src/bridge/CommandJob.h
        src/bridge/CommandJob.cpp
        src/bridge/MixerAutomation.h
        src/bridge/MixerAutomation.cpp
        src/bridge/MulticamEditBridge.h
        src/bridge/MulticamEditBridge.cpp
        src/bridge/MulticamGrid.h
        src/bridge/MulticamGrid.cpp
        src/bridge/NestingBridge.h
        src/bridge/NestingBridge.cpp
    QML_FILES
        qml/Main.qml
        qml/Preferences.qml
        qml/theme/Theme.qml
        qml/mock/MockProject.qml
        qml/audio/AudioToolsPanel.qml
        qml/audio/EffectSlots.qml
        qml/audio/Fader.qml
        qml/audio/AutomationButton.qml
        qml/audio/LiveLoudness.qml
        qml/audio/MasterStrip.qml
        qml/audio/MixerStrip.qml
        qml/audio/PanKnob.qml
        qml/components/BrandLogo.qml
        qml/components/EchoMark.qml
        qml/components/EchoLoader.qml
        qml/components/LoaderPill.qml
        qml/components/ContextMenu.qml
        qml/components/ContextMenuItem.qml
        qml/components/ContextMenuSeparator.qml
        qml/components/DockHandle.qml
        qml/components/DockHolder.qml
        qml/components/DockSlot.qml
        qml/components/DockTabs.qml
        qml/components/PanelTab.qml
        qml/components/TabFlare.qml
        qml/components/FocusRing.qml
        qml/components/Icon.qml
        qml/components/IconButton.qml
        qml/components/KeyframeShape.qml
        qml/components/NumberField.qml
        qml/components/Panel.qml
        qml/components/PillButton.qml
        qml/components/PillTabs.qml
        qml/components/Popover.qml
        qml/components/PropSlider.qml
        qml/components/SectionHeader.qml
        qml/components/BeamProgress.qml
        qml/components/EmptyState.qml
        qml/components/FieldInput.qml
        qml/components/Toast.qml
        qml/components/SliceMark.qml
        qml/components/SliceRect.qml
        qml/components/StatusPill.qml
        qml/components/Swatch.qml
        qml/components/Toggle.qml
        qml/components/ThinScrollBar.qml
        qml/components/Tooltip.qml
        qml/components/ValueSlider.qml
        qml/home/HomeCard.qml
        qml/home/ContinueCard.qml
        qml/home/HomeNav.qml
        qml/home/ProjectThumb.qml
        qml/home/HomeScreen.qml
        qml/home/ImportCard.qml
        qml/home/NewProjectCard.qml
        qml/home/RecentProjects.qml
        qml/panels/AudioMixerPanel.qml
        qml/panels/CaptionsPage.qml
        qml/panels/CleanupPage.qml
        qml/panels/ClipContextMenu.qml
        qml/panels/ChoiceMenu.qml
        qml/panels/EffectControlsRow.qml
        qml/panels/EffectsPanel.qml
        qml/panels/TitleStylePanel.qml
        qml/panels/ExportPanel.qml
        qml/panels/InspectorPanel.qml
        qml/panels/TimeRemapControls.qml
        qml/panels/KeyframeMenu.qml
        qml/panels/LeftRail.qml
        qml/panels/LibraryPanel.qml
        qml/panels/FrameInspectorPanel.qml
        qml/panels/multicam/MulticamView.qml
        qml/panels/library/BinBar.qml
        qml/panels/library/FindFiltersPopover.qml
        qml/panels/library/LibraryCard.qml
        qml/panels/library/LibraryItemMenu.qml
        qml/panels/library/LibraryListView.qml
        qml/panels/library/SubclipPopover.qml
        qml/panels/library/LibraryToolsMenu.qml
        # 0.2 (P.3): the Freeform view, Search Bins, the Metadata and Production panels
        qml/panels/library/FreeformMenu.qml
        qml/panels/library/LibraryFreeformView.qml
        qml/panels/library/SearchBinPopover.qml
        qml/panels/MetadataPanel.qml
        qml/panels/MetadataPropertyMenu.qml
        qml/panels/ProductionPanel.qml
        qml/panels/ProductionProjectMenu.qml
        qml/panels/MarkersPanel.qml
        qml/panels/MediaBrowserPanel.qml
        qml/panels/MarkerEditor.qml
        qml/panels/MarkersExportMenu.qml
        qml/panels/SequenceTabBar.qml
        qml/panels/TimelineDisplayPopover.qml
        qml/panels/TimelineListView.qml
        qml/onboarding/BeamSpark.qml
        qml/onboarding/CoachMarks.qml
        qml/onboarding/Keycap.qml
        qml/onboarding/KeyboardTrainer.qml
        qml/onboarding/LaunchIntro.qml
        qml/onboarding/Onboarding.qml
        qml/onboarding/WelcomeDialog.qml
        qml/onboarding/WhatsNewDialog.qml
        qml/panels/TrimView.qml
        qml/panels/MonitorPanel.qml
        qml/panels/ProgramTimeBar.qml # 0.1.3: the Program Monitor's time bar (SequenceTimeBar)
        qml/panels/MonitorSwitch.qml
        qml/panels/SliceTransition.qml
        qml/panels/WorkbenchSplitter.qml
        qml/panels/DockManager.qml
        qml/panels/FloatingDock.qml
        qml/panels/WorkspaceTabs.qml
        qml/panels/workspace/Workspaces.qml
        qml/panels/workspace/WorkspaceTabMenu.qml
        qml/panels/workspace/WorkspaceAddMenu.qml
        qml/panels/workspace/WorkspaceListMenu.qml
        qml/panels/FitClipDialog.qml
        qml/panels/RecentSourcesMenu.qml
        qml/panels/SafeMarginsOverlay.qml
        qml/panels/SourceMonitorPanel.qml
        qml/panels/PaletteOverlay.qml
        qml/panels/RenameClipPopover.qml
        qml/panels/SpeedDurationDialog.qml
        qml/panels/TimelineLanes.qml
        qml/panels/TimelineMinimap.qml
        qml/panels/TimelinePanel.qml
        qml/panels/TimelineZoomToolArea.qml
        qml/panels/TimelineRuler.qml
        qml/panels/TopBar.qml
        qml/panels/TrackHeader.qml
        qml/panels/TranscriptPage.qml
        qml/panels/TranscriptPanel.qml
        qml/panels/WindowControls.qml
        qml/panels/WindowFrame.qml
        qml/shortcuts/KeyboardShortcutsDialog.qml
        qml/shortcuts/KeyboardView.qml
        qml/shortcuts/OptionsMenu.qml
        qml/shortcuts/ShortcutCommandList.qml
    RESOURCES
        # The echo mark, app icon and the empty-state line illustrations (design language v3; drawn for Wabcut).
        resources/brand/wabcut-mark.svg
        resources/brand/wabcut-mark-small.svg
        resources/brand/wabcut-mark-mono.svg

        resources/brand/wabcut-app-icon.svg
        resources/illustrations/empty-library.svg
        resources/illustrations/empty-timeline.svg
        resources/illustrations/empty-transcript.svg
        resources/illustrations/empty-search.svg
        resources/illustrations/empty-effects.svg
        resources/illustrations/empty-markers.svg
        # The Wabcut type system (design language v2): IBM Plex Sans / Sans Thai / Mono and Bai Jamjuree, SIL OFL 1.1
        # (THIRD-PARTY-NOTICES.md; registered by BundledFonts).
        resources/fonts/Manrope-Regular.ttf
        resources/fonts/Manrope-Medium.ttf
        resources/fonts/Manrope-SemiBold.ttf
        resources/fonts/Manrope-Bold.ttf
        resources/fonts/Manrope-ExtraBold.ttf
        resources/fonts/JetBrainsMono-Regular.ttf
        resources/fonts/JetBrainsMono-Medium.ttf
        resources/fonts/JetBrainsMono-SemiBold.ttf
        resources/fonts/JetBrainsMono-Bold.ttf
        resources/fonts/IBMPlexSans-Regular.ttf
        resources/fonts/IBMPlexSans-Medium.ttf
        resources/fonts/IBMPlexSans-SemiBold.ttf
        resources/fonts/IBMPlexSansThai-Regular.ttf
        resources/fonts/IBMPlexSansThai-Medium.ttf
        resources/fonts/IBMPlexSansThai-SemiBold.ttf
        resources/fonts/IBMPlexSansThai-Bold.ttf
        resources/fonts/IBMPlexMono-Regular.ttf
        resources/fonts/IBMPlexMono-Medium.ttf
        resources/fonts/IBMPlexMono-SemiBold.ttf
        resources/fonts/BaiJamjuree-Medium.ttf
        resources/fonts/BaiJamjuree-SemiBold.ttf
        resources/fonts/OFL-IBM-Plex.txt
        resources/fonts/OFL-Bai-Jamjuree.txt
        resources/fonts/OFL-Manrope.txt
        resources/fonts/OFL-JetBrains-Mono.txt
        # 0.1.3: Boxicons (MIT, see resources/icons/LICENSE) under Wabcut's logical icon names, written by
        # tools/icons/import_boxicons.py from tools/icons/boxicons-map.json (`add <bx-name> <name>` lists a new one here).
        resources/icons/LICENSE
        resources/icons/accessibility.svg
        resources/icons/align-center.svg
        resources/icons/align-left.svg
        resources/icons/align-right.svg
        resources/icons/arrow-left-right.svg
        resources/icons/arrow-right-to-line.svg
        resources/icons/arrow-right.svg
        resources/icons/audio-lines.svg
        resources/icons/blend.svg
        resources/icons/book-open.svg
        resources/icons/bookmark.svg
        resources/icons/captions.svg
        resources/icons/check.svg
        resources/icons/chevron-down.svg
        resources/icons/chevron-left.svg
        resources/icons/chevron-right.svg
        resources/icons/clapperboard.svg
        resources/icons/clipboard-paste.svg
        resources/icons/clock.svg
        resources/icons/copy.svg
        resources/icons/crop.svg
        resources/icons/crosshair.svg
        resources/icons/diamond.svg
        resources/icons/download.svg
        resources/icons/droplet.svg
        resources/icons/external-link.svg
        resources/icons/eye-off.svg
        resources/icons/eye.svg
        resources/icons/film.svg
        resources/icons/circle-help.svg
        resources/icons/contrast.svg
        resources/icons/list.svg
        resources/icons/zap.svg
        resources/icons/file-down.svg
        resources/icons/fold-vertical.svg
        resources/icons/repeat.svg
        resources/icons/sliders-horizontal.svg
        resources/icons/unfold-vertical.svg
        resources/icons/fold-horizontal.svg
        resources/icons/folder-open.svg
        resources/icons/gauge.svg
        resources/icons/grip-vertical.svg
        resources/icons/hand.svg
        resources/icons/headphones.svg
        resources/icons/house.svg
        resources/icons/import.svg
        resources/icons/keyboard.svg
        resources/icons/library.svg
        resources/icons/link.svg
        resources/icons/lock-open.svg
        resources/icons/lock.svg
        resources/icons/magnet.svg
        resources/icons/minimize-2.svg
        resources/icons/minus.svg
        resources/icons/moon.svg
        resources/icons/mouse-pointer-2.svg
        resources/icons/move-horizontal.svg
        resources/icons/move-vertical.svg
        resources/icons/music.svg
        resources/icons/pause.svg
        resources/icons/pencil.svg
        resources/icons/play.svg
        resources/icons/plus.svg
        resources/icons/power.svg
        resources/icons/redo-2.svg
        resources/icons/rotate-ccw.svg
        resources/icons/rotate-cw.svg
        resources/icons/save.svg
        resources/icons/scaling.svg
        resources/icons/scissors.svg
        resources/icons/search.svg
        resources/icons/settings.svg
        resources/icons/skip-back.svg
        resources/icons/skip-forward.svg
        resources/icons/sparkles.svg
        resources/icons/spell-check.svg
        resources/icons/spline.svg
        resources/icons/square.svg
        resources/icons/sun.svg
        resources/icons/timer.svg
        resources/icons/trash-2.svg
        resources/icons/triangle-alert.svg
        resources/icons/type.svg
        resources/icons/undo-2.svg
        resources/icons/unfold-horizontal.svg
        resources/icons/unlink.svg
        resources/icons/upload.svg
        resources/icons/volume-2.svg
        resources/icons/volume-x.svg
        resources/icons/x.svg
        resources/icons/zoom-in.svg
        resources/icons/zoom-out.svg
        resources/icons/camera.svg
        resources/icons/step-back.svg
        resources/icons/step-forward.svg
        # Drawn for Wabcut (M5h; 2px lines next to Boxicons since 0.1.3): Premiere's monitor buttons.
        resources/icons/mark-in.svg
        resources/icons/mark-out.svg
        resources/icons/go-to-in.svg
        resources/icons/go-to-out.svg
        resources/icons/play-in-out.svg
        resources/icons/insert-edit.svg
        resources/icons/overwrite-edit.svg
        resources/icons/lift-edit.svg
        resources/icons/extract-edit.svg
        resources/icons/safe-margins.svg
        # M5k: the Library.
        resources/icons/columns-3.svg
        resources/icons/ellipsis.svg
        resources/icons/folder.svg
        resources/icons/folder-plus.svg
        resources/icons/layers.svg
        resources/icons/message-square.svg
        resources/icons/mic.svg
        resources/icons/funnel.svg
        resources/icons/image.svg
        resources/icons/layout-grid.svg
        resources/icons/palette.svg
        resources/icons/arrow-left-to-line.svg
        # 0.2 (P.3): the Metadata and Production panels, Search Bins and the Freeform view (Boxicons).
        resources/icons/folder-search.svg
        resources/icons/folders.svg
        resources/icons/shapes.svg
        resources/icons/tag.svg
)

# The QML files and resources of the feature blocks below are collected by wabcut_ui_qml_sources() and added to the
# module in one qt_target_qml_sources() call at the end of this file. Each qt_target_qml_sources() call makes .qrc files
# of its own and qmlcachegen gets every .qrc of the module on its command line, which Windows caps at 8191 characters:
# with one call per block (27 of them in 0.2) a build folder of ordinary length went past it, and qmlcachegen then
# printed its usage and wrote nothing.
function(wabcut_ui_qml_sources)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "QML_FILES;RESOURCES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "wabcut_ui_qml_sources: unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    set_property(GLOBAL APPEND PROPERTY WABCUT_UI_QML_FILES ${arg_QML_FILES})
    set_property(GLOBAL APPEND PROPERTY WABCUT_UI_QML_RESOURCES ${arg_RESOURCES})
endfunction()

# src/ for RhiPolicy.h (used by main.cpp and tests), src/bridge/ for the QML-registered bridge headers.
target_include_directories(wabcut_ui PUBLIC src src/bridge)
target_link_libraries(wabcut_ui PUBLIC
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    Qt6::GuiPrivate   # rhi/qrhi.h (QRhi::driverInfo) for RendererInfo
    wabcut_model      # ProjectController / TimelineView read and edit the model
    wabcut_media      # waveform peaks and library thumbnails (MediaBridge)
    wabcut_command    # typed edit commands for the command palette (CommandBridge)
    wabcut_playback   # ProgramMonitor: PlaybackController, CompositeMailbox, Compositor (wabcut_render)
)
target_compile_definitions(wabcut_ui PRIVATE WABCUT_VERSION="${PROJECT_VERSION}"
    # Home screen "Learn / What's new" links: the docs next to the app, else these (development builds).
    WABCUT_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs")
wabcut_target_defaults(wabcut_ui)

qt_add_executable(wabcut WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/StartupProbe.h
    src/StartupProbe.cpp # P.1: startup milestones for the benchmarks (inert unless WABCUT_PERF_STARTUP is set)
)
# The app icon (design language v2), rendered from resources/brand/wabcut-app-icon.svg (tests/app/test_brand_icons.cpp).
if(WIN32)
    target_sources(wabcut PRIVATE wabcut.rc)
elseif(APPLE)
    set(WABCUT_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand/icons/wabcut.icns")
    target_sources(wabcut PRIVATE "${WABCUT_ICNS}")
    set_source_files_properties("${WABCUT_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # Info.plist: name, version, icon, and the reason macOS shows when voice-over recording first asks for the
    # microphone (without NSMicrophoneUsageDescription macOS ends the app instead of asking).
    set_target_properties(wabcut PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/cmake/packaging/macos/Info.plist.in"
        MACOSX_BUNDLE_ICON_FILE wabcut.icns
        MACOSX_BUNDLE_BUNDLE_NAME Wabcut
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.sorsama.wabcut
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Wabcut contributors, GPL-3.0-or-later")
endif()

target_link_libraries(wabcut PRIVATE
    wabcut_uiplugin
    wabcut_ui         # RhiPolicy (startup backend policy)
    Qt6::Quick
    Qt6::QuickControls2
    wabcut_model
    wabcut_media
)
target_compile_definitions(wabcut PRIVATE WABCUT_VERSION="${PROJECT_VERSION}")
wabcut_target_defaults(wabcut)

# Make build/<preset>/bin/wabcut.exe runnable straight from the build tree.
if(WIN32)
    add_custom_command(TARGET wabcut POST_BUILD
        COMMAND $<TARGET_FILE:Qt6::windeployqt> --no-translations --no-compiler-runtime
                --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/qml $<TARGET_FILE:wabcut>
        COMMENT "windeployqt wabcut"
        VERBATIM)
endif()

# Keymap bridge (owner: Keymap): the KeymapDispatcher singleton and the KeymapScope attached type, over the
# Qt-free wabcut_keymap engine. See docs/keymap.md, "App integration".
target_sources(wabcut_ui PRIVATE src/bridge/KeymapDispatcher.h src/bridge/KeymapDispatcher.cpp)
# qmltyperegistrar includes registered headers by file name.
target_include_directories(wabcut_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge)
target_link_libraries(wabcut_ui PUBLIC wabcut_keymap)

# Editor bridges (owner: Graphics/playback, I.1, I.3): project lifecycle, import, relink, playback and media
# readiness (proxies, preview quality, caches) and Collect Files (I.4) over wabcut_editor (singletons `ProjectFile`,
# `Playback`, `MediaReadiness` and `Collect`), and the dialogs under qml/dialogs/.
target_sources(wabcut_ui PRIVATE
    src/bridge/CaptionStyleMapping.h
    src/bridge/CollectFilesBridge.h
    src/bridge/CollectFilesBridge.cpp
    src/bridge/MediaReadinessBridge.h
    src/bridge/MediaReadinessBridge.cpp
    src/bridge/PlaybackBridge.h
    src/bridge/PlaybackBridge.cpp
    src/bridge/ProjectLifecycle.h
    src/bridge/ProjectLifecycle.cpp
    src/bridge/RedactedPictures.h
    src/bridge/RedactedPictures.cpp
    # 0.2 (P.3): Productions and other open projects (singleton `Production`, editor/Production.h)
    src/bridge/ProductionBridge.h
    src/bridge/ProductionBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/dialogs/CollectFilesDialog.qml
        qml/dialogs/EnhanceSpeechDialog.qml
        qml/dialogs/MulticamCreateDialog.qml
        qml/dialogs/NestDialog.qml
        qml/dialogs/StemsDialog.qml
        qml/dialogs/SyncReportDialog.qml
        qml/dialogs/ImportReportDialog.qml
        qml/dialogs/AudioGainDialog.qml
        qml/dialogs/ImageSequenceDialog.qml
        qml/dialogs/ImportLayeredFileDialog.qml
        qml/dialogs/ColorManagementDialog.qml
        qml/dialogs/ContentCredentialsSettings.qml
        qml/dialogs/AudioPostOptions.qml
        qml/dialogs/ExportTimelineDialog.qml
        qml/dialogs/MediaCacheSettings.qml
        qml/dialogs/RecoveryDialog.qml
        qml/dialogs/RelinkDialog.qml
        qml/dialogs/UnsavedChangesDialog.qml
        qml/dialogs/WabcutDialog.qml)
target_link_libraries(wabcut_ui PUBLIC wabcut_editor)

# Color workspace (owner: Graphics, I.5, docs/color.md): the Color panel (singleton `Color`), the scopes (singleton
# `Scopes`, the ScopeView item, fed by the program monitor's GPU scopes) and their QML under qml/panels/color/.
target_sources(wabcut_ui PRIVATE
    src/bridge/ColorBridge.h
    src/bridge/ColorBridge.cpp
    src/bridge/ScopesBridge.h
    src/bridge/ScopesBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/color/ColorCompareOverlay.qml
        qml/panels/color/ColorPanel.qml
        qml/panels/color/ColorWheel.qml
        qml/panels/color/CurveEditor.qml
        qml/panels/color/GradeSection.qml
        qml/panels/color/GradeSlider.qml
        qml/panels/color/LutMenu.qml
        qml/panels/color/MediaColorSpaceMenu.qml
        qml/panels/color/ScopesPanel.qml)
target_link_libraries(wabcut_ui PUBLIC wabcut_color)

# Masks, tracking and redactions (owner: Graphics, I.6, docs/masks.md): the `Masks` singleton (masks of the Effect
# Controls clip, the monitor tools and handles, the tracker as a job, the Source Monitor's redactions) and its QML
# under qml/panels/masks/ (Effect Controls' mask section, the program monitor's mask overlay, the Source Monitor's
# redaction overlay and list).
target_sources(wabcut_ui PRIVATE
    src/bridge/MaskBridge.h
    src/bridge/MaskBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/masks/ConfidenceStrip.qml
        qml/panels/masks/MaskOverlay.qml
        qml/panels/masks/MaskSection.qml
        qml/panels/masks/RedactionOverlay.qml
        qml/panels/masks/RedactionsPopover.qml
    RESOURCES
        resources/icons/circle.svg)
target_link_libraries(wabcut_ui PUBLIC wabcut_masks)
# AI Object Mask (owner: AI platform, AI-14, docs/masks.md "Object masks"): the ObjectMasks singleton (ObjectMaskBridge)
# over wabcut_ai_segment_media - prompts on the program monitor, refine, tracking as a job, the matte preview
# (image://wabcut-matte), AI-tracked redactions in the Source Monitor, the SAM 2.1 download, the compositor's matte
# source - and its QML under qml/panels/masks/ (the Object tool of Effect Controls' mask section, the prompt overlay).
target_sources(wabcut_ui PRIVATE src/bridge/ObjectMaskBridge.h src/bridge/ObjectMaskBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/masks/ObjectMaskControls.qml
        qml/panels/masks/ObjectPromptOverlay.qml)
target_link_libraries(wabcut_ui PUBLIC wabcut_ai_segment_media)
# Export bridge (owner: Export): the ExportController singleton and its job model over wabcut_export, and the
# Export workspace's sub-panels. See docs/export.md, "App integration".
target_sources(wabcut_ui PRIVATE src/bridge/ExportController.h src/bridge/ExportController.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/export/ExportJobPill.qml
        qml/panels/export/CaptionExportChooser.qml
        qml/panels/export/ExportSettingsDrawer.qml
        qml/panels/export/PresetCard.qml
        qml/panels/export/ExportGroupLabel.qml
        qml/panels/export/ExportFilterChips.qml
        # 0.2 (Premiere parity): Quick Export, the queue (Media Encoder), presets, effects and metadata.
        qml/panels/export/ExportEffectsSection.qml
        qml/panels/export/ExportFormatPresets.qml
        qml/panels/export/ExportPresetManager.qml
        qml/panels/export/ExportPresetMenu.qml
        qml/panels/export/ExportQueueDialog.qml
        qml/panels/export/QuickExportButton.qml
)
target_link_libraries(wabcut_ui PUBLIC wabcut_export)

# AI bridge (owner: AI platform): the AiController singleton over wabcut_ai / wabcut_ai_net, and the AI pages in
# qml/panels/ai/ (0.1.3: shown in Settings > AI, qml/settings/AiSettingsPage.qml). With WABCUT_WITH_CLOUD_AI, keys live in the OS keychain
# through QtKeychain (overlay port cmake/ports/qtkeychain); otherwise for the session only.
target_sources(wabcut_ui PRIVATE src/bridge/AiController.h src/bridge/AiController.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/ai/AiBadge.qml
        qml/panels/ai/AiConsentDialog.qml
        qml/panels/ai/AiModelsPage.qml
        qml/panels/ai/LocalLlmPage.qml
        qml/panels/ai/GeneratePanel.qml
        qml/panels/ai/AiPrivacyPage.qml
        qml/panels/ai/AiProvidersPage.qml
        qml/panels/ai/AiTextField.qml
        qml/panels/ai/AiUsagePage.qml)
target_link_libraries(wabcut_ui PUBLIC wabcut_ai_net)
# Transcript panel (M5e, Timeline/UI): transcript-first editing and captions (wabcut_ai_edit + its media services),
# and local transcription through TranscriptionService when the wabcut-infer helper is built.
target_link_libraries(wabcut_ui PUBLIC wabcut_ai_edit_media wabcut_ai_transcribe)
# Audio workspace (M5g): the audio.* commands' media-backed loudness and speech analysis.
target_link_libraries(wabcut_ui PUBLIC wabcut_audio_edit_media)
# Audio plugins (A.6, docs/plugins.md): the plugin catalog, its background scan and plugin effects; the wabcut-plughost
# helper runs next to the app.
if(TARGET wabcut_plugins)
    target_link_libraries(wabcut_ui PUBLIC wabcut_plugins)
    target_compile_definitions(wabcut_ui PUBLIC WABCUT_WITH_AUDIO_PLUGINS=1)
    add_dependencies(wabcut_ui wabcut-plughost)
endif()
if(WABCUT_WITH_CLOUD_AI)
    find_package(Qt6Keychain CONFIG REQUIRED)
    target_link_libraries(wabcut_ui PRIVATE Qt6Keychain::Qt6Keychain)
    target_compile_definitions(wabcut_ui PRIVATE WABCUT_WITH_QTKEYCHAIN=1)
endif()

# Editing assistant (owner: AI platform, AI-5): the AgentController singleton over wabcut_ai_agent (tool loop,
# staging, verification, guarded apply), the assistant panel opened from the palette's "Ask AI…" row, and the
# timeline's read-only external preview (TimelineViewPreview.cpp, a self-contained block of TimelineView).
# Transcripts, beats and shots come from the analysis cache (wabcut_ai_edit_media, wabcut_ai_analysis_media).
target_sources(wabcut_ui PRIVATE
    src/bridge/AgentController.h
    src/bridge/AgentController.cpp
    src/bridge/TimelineViewPreview.cpp)
wabcut_ui_qml_sources(QML_FILES qml/panels/ai/AssistantPanel.qml)
target_link_libraries(wabcut_ui PUBLIC wabcut_ai_agent PRIVATE wabcut_ai_edit_media wabcut_ai_analysis_media)
# G.2: highlights.find measures recordings through the analysis cache (wabcut_ai_highlights_media).
target_link_libraries(wabcut_ui PRIVATE wabcut_ai_highlights_media)

# Smart search (owner: AI platform, AI-8): the SmartSearch singleton (SearchBridge) over wabcut_ai_search_media —
# background indexing, hybrid search, result thumbnails (image://wabcut-search), hit drops with their source range — and
# the Library's search bar with the Smart toggle and its results (qml/panels/search/).
target_sources(wabcut_ui PRIVATE src/bridge/SearchBridge.h src/bridge/SearchBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/search/SearchHitCard.qml
        qml/panels/search/SearchResultsPanel.qml
        qml/panels/search/SmartSearchBar.qml)
target_link_libraries(wabcut_ui PRIVATE wabcut_ai_search_media)

# Auto reframe and the shorts finder (owner: AI platform, AI-9): the Shorts and Reframe singletons (ShortsBridge) over
# wabcut_ai_shorts / wabcut_ai_reframe / wabcut_ai_vision_media, frames for their previews (image://wabcut-frames), the
# Shorts browser panel (qml/panels/shorts/) and the Reframe dialog of the sequence menu.
target_sources(wabcut_ui PRIVATE src/bridge/ShortsBridge.h src/bridge/ShortsBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/dialogs/ReframeDialog.qml
        qml/panels/shorts/ShortCard.qml
        qml/panels/shorts/ShortsPanel.qml)
target_link_libraries(wabcut_ui PRIVATE wabcut_ai_shorts wabcut_ai_vision_media wabcut_audio_edit_media wabcut_ai_audio_media)
target_link_libraries(wabcut_ui PUBLIC wabcut_ai_llm)

target_link_libraries(wabcut_ui PUBLIC wabcut_ai_generation_media)

# Crash safety (owner: Graphics/integration, I.7; docs/release.md "Crash reports"): the handler main() installs (local
# minidumps and redacted reports, never uploaded) and the next start's report scanner, Qt-free in wabcut_crash; the
# "Wabcut closed unexpectedly" notice (singleton `CrashNotice`, qml/dialogs/CrashNoticeDialog.qml).
find_package(nlohmann_json CONFIG REQUIRED)
add_library(wabcut_crash STATIC
    src/crash/CrashHandler.h
    src/crash/CrashHandler.cpp
    src/crash/CrashReports.h
    src/crash/CrashReports.cpp
    src/crash/SystemInfo.h
    src/crash/SystemInfo.cpp)
target_include_directories(wabcut_crash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/crash)
target_link_libraries(wabcut_crash PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_crash)
target_sources(wabcut_ui PRIVATE src/bridge/CrashNotice.h src/bridge/CrashNotice.cpp)
wabcut_ui_qml_sources(QML_FILES qml/dialogs/CrashNoticeDialog.qml)
target_link_libraries(wabcut_ui PUBLIC wabcut_crash)
# AI-15: signed C2PA Content Credentials in exports (ExportController; src/ai/c2pa).
target_link_libraries(wabcut_ui PUBLIC wabcut_ai_c2pa)
target_link_libraries(wabcut PRIVATE wabcut_crash)

# The first-run performance check (owner: Graphics/integration, I.7; docs/release.md "First-run performance check"):
# singleton `PerformanceCheck` over editor::runPerformanceProbe(), qml/dialogs/FirstRunCheckDialog.qml.
target_sources(wabcut_ui PRIVATE src/bridge/PerformanceCheck.h src/bridge/PerformanceCheck.cpp)
wabcut_ui_qml_sources(QML_FILES qml/dialogs/FirstRunCheckDialog.qml)

# Creator superpowers (owner: AI platform, AI-11): the RoughCut, Takes and AutoZoom singletons (CreatorBridge) over
# wabcut_ai_roughcut / wabcut_ai_autozoom and their media services — the "Rough cut" sheet, the Takes panel and the
# Auto zoom dialog of the clip menu (qml/panels/creator/, qml/dialogs/AutoZoomDialog.qml).
target_sources(wabcut_ui PRIVATE src/bridge/CreatorBridge.h src/bridge/CreatorBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/dialogs/AutoZoomDialog.qml
        qml/panels/creator/RoughCutSheet.qml
        qml/panels/creator/TakesPanel.qml)
target_link_libraries(wabcut_ui PRIVATE wabcut_ai_roughcut_media wabcut_ai_autozoom_media)

# Going global (owner: AI platform, AI-12): the TranslateCaptions and Dub singletons (GlobalBridge) over
# wabcut_ai_translate / wabcut_ai_mt / wabcut_ai_dub_media — the "Translate captions" dialog of the Captions workspace and
# the "Dub" dialog (voice-over and narration), qml/dialogs/TranslateDialog.qml and DubDialog.qml.
target_sources(wabcut_ui PRIVATE src/bridge/GlobalBridge.h src/bridge/GlobalBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/dialogs/DubDialog.qml
        qml/dialogs/TranslateDialog.qml)
target_link_libraries(wabcut_ui PRIVATE wabcut_ai_dub_media wabcut_ai_mt)

# Podcast and interview editing (owner: AI platform, AI-13): the AutoSwitch and PodcastWorkflow singletons
# (MulticamBridge) over wabcut_ai_multicam_media / wabcut_ai_workflow — the "Auto switch" dialog of a multicam clip and
# the "Podcast workflow" sheet, qml/dialogs/AutoSwitchDialog.qml and qml/panels/creator/PodcastWorkflowSheet.qml.
target_sources(wabcut_ui PRIVATE src/bridge/MulticamBridge.h src/bridge/MulticamBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/dialogs/AutoSwitchDialog.qml
        qml/panels/creator/PodcastWorkflowSheet.qml)
target_link_libraries(wabcut_ui PRIVATE wabcut_ai_multicam_media wabcut_ai_workflow)

# Voice-over recording (owner: Audio engineer, A.8, docs/ai.md "Voice-over recording"): the VoiceOver singleton
# (VoiceOverBridge) over wabcut_playback's VoiceOverRecorder and wabcut_audio's AudioInput - the input devices and
# meter, the recording pass, the takes switcher. Its QML (record buttons, count-in overlay, takes, Preferences > Audio
# input) is the UI engineer's.
target_sources(wabcut_ui PRIVATE src/bridge/VoiceOverBridge.h src/bridge/VoiceOverBridge.cpp)
target_link_libraries(wabcut_ui PRIVATE wabcut_audio wabcut_audio_edit)

# M5m (owner: Timeline/UI): the caption workshop, version history and review notes (C.9), audio plugins (A.6),
# Essential Sound and Remix (A.7), voice-over recording's UI (A.8), clipboard and Paste/Remove Attributes (C.11).
target_sources(wabcut_ui PRIVATE
    src/bridge/HistoryBridge.h
    src/bridge/HistoryBridge.cpp
    src/bridge/ReviewBridge.h
    src/bridge/ReviewBridge.cpp
    src/bridge/ReviewFrames.h
    src/bridge/ReviewFrames.cpp
    src/bridge/PluginBridge.h
    src/bridge/PluginBridge.cpp
    src/bridge/EssentialSoundBridge.h
    src/bridge/EssentialSoundBridge.cpp
    src/bridge/ClipboardBridge.h
    src/bridge/ClipboardBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/review/ReviewPanel.qml
        qml/panels/review/ReviewNotesPage.qml
        qml/panels/review/ReviewVersionsPage.qml
        qml/panels/review/ReviewNoteCard.qml
        qml/panels/review/VersionCompareView.qml
        qml/panels/review/ReviewMarkOverlay.qml
        qml/panels/plugins/PluginBrowser.qml
        qml/panels/plugins/PluginRow.qml
        qml/panels/plugins/PluginBadge.qml
        qml/dialogs/AudioPluginsSettings.qml
        qml/panels/EssentialSoundPanel.qml
        qml/panels/essential/EssentialSection.qml
        qml/panels/essential/RemixPopover.qml
        qml/panels/essential/RemixSummary.qml
        qml/panels/captions/CaptionStylePanel.qml
        qml/panels/captions/CaptionCueRow.qml
        qml/panels/captions/CaptionColorRow.qml
        qml/audio/RecordArmButton.qml
        qml/audio/InputMeter.qml
        qml/audio/VoiceOverSync.qml
        qml/panels/VoiceOverOverlay.qml
        qml/panels/TakesSubmenu.qml
        qml/dialogs/AudioSettings.qml
        qml/dialogs/AttributesDialog.qml
)

# M5n (owner: Timeline/UI): startup timings (UiProfile, WABCUT_UI_PROFILE) and the lazy panels; extensions (C.12: the
# Extensions singleton over wabcut_script's ExtensionManager, the wabcut-scripthost helper next to the app); audio
# channels and 5.1 (A.9); the accessibility and onboarding refresh.
target_sources(wabcut_ui PRIVATE
    src/bridge/UiProfile.h
    src/bridge/UiProfile.cpp
    src/bridge/AudioChannelsBridge.h
    src/bridge/AudioChannelsBridge.cpp
    src/bridge/ExtensionsBridge.h
    src/bridge/ExtensionsBridge.cpp
)
target_link_libraries(wabcut_ui PRIVATE wabcut_script)
if(TARGET wabcut-scripthost)
    add_dependencies(wabcut_ui wabcut-scripthost)
endif()
wabcut_ui_qml_sources(
    QML_FILES
        qml/components/LazyPanel.qml
        qml/dialogs/AudioChannelsDialog.qml
        qml/audio/ChannelVolumeSection.qml
        qml/audio/SurroundPanner.qml
        qml/panels/AudioChannelsSubmenu.qml
        qml/panels/TrackHeaderMenu.qml
        qml/dialogs/ExtensionsManager.qml
        qml/dialogs/ExtensionPermissionDialog.qml
        qml/dialogs/ExtensionPromptDialog.qml
        qml/panels/extensions/ExtensionRow.qml
        qml/panels/extensions/ExtensionPromptHost.qml
    RESOURCES
        resources/icons/puzzle.svg
        resources/icons/shield-check.svg
)

# P.2 (motion): Time Interpolation in the clip menu, the Morph Cut / optical flow analysis banner, Warp Stabilizer's
# and Rolling Shutter Repair's Analyze (the `Stabilizer` singleton, StabilizerBridge), the Library's New Item submenu
# (Bars and Tone, Universal Counting Leader, ...).
target_sources(wabcut_ui PRIVATE
    src/bridge/StabilizerBridge.h
    src/bridge/StabilizerBridge.cpp)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/StabilizerStatus.qml
        qml/panels/TimeInterpolationSubmenu.qml
        qml/panels/library/NewItemSubmenu.qml
)
target_link_libraries(wabcut_ui PUBLIC wabcut_ai_stabilize)

# M5o (owner: Timeline/UI): motion graphics (C.13) - Essential Graphics (`Graphics`: templates, layers, presets, pins;
# its Qt text layout and the thumbnail painter, a UI preview), the on-canvas handles (`GraphicCanvas`), the brand kit
# (`Brand`) and Lottie import (`GraphicsImport`).
target_sources(wabcut_ui PRIVATE
    src/bridge/GraphicText.h
    src/bridge/GraphicText.cpp
    src/bridge/GraphicPainter.h
    src/bridge/GraphicPainter.cpp
    src/bridge/GraphicsBridge.h
    src/bridge/GraphicsBridge.cpp
    src/bridge/GraphicCanvasBridge.h
    src/bridge/GraphicCanvasBridge.cpp
    src/bridge/BrandBridge.h
    src/bridge/BrandBridge.cpp
    src/bridge/GraphicsImportBridge.h
    src/bridge/GraphicsImportBridge.cpp
    src/bridge/GamingExport.h
    src/bridge/GamingExport.cpp
    src/bridge/LayoutBridge.h
    src/bridge/LayoutBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/graphics/EssentialGraphicsPanel.qml
        qml/panels/graphics/GraphicsBrowsePage.qml
        qml/panels/graphics/GraphicsEditPage.qml
        qml/panels/graphics/GraphicLayerProperties.qml
        qml/panels/graphics/GraphicTemplateCard.qml
        qml/panels/graphics/GraphicTemplateMenu.qml
        qml/panels/graphics/GraphicChoicePill.qml
        qml/panels/graphics/GraphicColorRow.qml
        qml/panels/graphics/GraphicParamRow.qml
        qml/panels/graphics/GraphicGroupLabel.qml
        qml/panels/graphics/GraphicCanvasOverlay.qml
        qml/panels/graphics/GraphicTextEditor.qml
        qml/dialogs/GraphicTemplateDialog.qml
        qml/dialogs/BrandKitDialog.qml
        qml/panels/export/GamingPresetCard.qml
        qml/panels/export/GamingPresets.qml
        qml/panels/layout/LayoutPicker.qml
        qml/panels/layout/LayoutSchematic.qml
        qml/panels/layout/LayoutMonitorOverlay.qml
    RESOURCES
        resources/icons/chevron-up.svg
)

# M5o gaming: moments (Fork S) - one-click speed ramps / slow-mo and zoom punch-ins (`Moments`: on the time remapping
# commands and Motion keyframes, one undo step each), the clip menu's Moments submenu and the Tracks header's popover.
target_sources(wabcut_ui PRIVATE
    src/bridge/MomentsBridge.h
    src/bridge/MomentsBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/MomentsSubmenu.qml
        qml/panels/MomentsPopover.qml
)

# M5o gaming: new gaming video (Fork H) - the Home screen's start from a game recording (`GamingStart`: the probe off
# the GUI thread, the project at the recording's size and nominal rate, one named audio track per included stream,
# proxies when suggested) and its card.
target_sources(wabcut_ui PRIVATE
    src/bridge/GamingStartBridge.h
    src/bridge/GamingStartBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/home/GamingVideoCard.qml
    RESOURCES
        resources/icons/gamepad-2.svg
)

# M5o gaming: highlights (Fork Q) - the Highlights panel on G.2 highlights.importChat / find / toReel / toShort
# (`Highlights`: the chat log import and the find as command jobs with progress and Cancel, the ranked moments with
# accept / reject, jump and preview, the reel and the short), its cards and option popovers.
target_sources(wabcut_ui PRIVATE
    src/bridge/HighlightsBridge.h
    src/bridge/HighlightsBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/highlights/HighlightsPanel.qml
        qml/panels/highlights/HighlightCard.qml
        qml/panels/highlights/HighlightFindOptions.qml
        qml/panels/highlights/HighlightReelOptions.qml
        qml/panels/highlights/HighlightShortOptions.qml
)

# 0.1.3 Settings window (WP-S): one window for every preference, hosted by the left rail (its Settings button,
# File > Settings…, Ctrl+,): General, Appearance (themes, custom colours, theme files through ThemeFiles), Interface
# (title bar status, the rail's entries), Timeline & editing, Playback & performance, Audio, AI, Keyboard, Plugins &
# extensions. It replaced the rail's Settings popover, the title bar's Appearance popover and Preferences > AI.
target_sources(wabcut_ui PRIVATE
    src/bridge/ThemeFiles.h
    src/bridge/ThemeFiles.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/settings/SettingsWindow.qml
        qml/settings/SettingsPage.qml
        qml/settings/SettingsSection.qml
        qml/settings/SettingsRow.qml
        qml/settings/GeneralSettingsPage.qml
        qml/settings/AppearanceSettingsPage.qml
        qml/settings/ThemeCard.qml
        qml/settings/ThemeColorEditor.qml
        qml/settings/ColorPickerPopover.qml
        qml/settings/PickerStrip.qml
        qml/settings/InterfaceSettingsPage.qml
        qml/settings/RailCustomizer.qml
        qml/settings/TimelineSettingsPage.qml
        qml/settings/PerformanceSettingsPage.qml
        qml/settings/AiSettingsPage.qml
)

# Timeline toolbar and edit cursors (0.1.3, owner: Timeline): the Tracks panel's tool palette (Premiere's tool groups
# with flyouts), the labelled mode toggles (Snap, Linked) and the view menu (qml/panels/timeline/); the edit cursors
# drawn from Boxicons glyphs (EditCursors: TimelineView's hover and drag cursors, the CursorArea QML type) and their
# glyphs (resources/cursors, MIT, see the LICENSE there).
target_sources(wabcut_ui PRIVATE
    src/bridge/EditCursors.h
    src/bridge/EditCursors.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/panels/timeline/ToolGroupButton.qml
        qml/panels/timeline/ToolFlyout.qml
        qml/panels/timeline/ToggleChip.qml
        qml/panels/timeline/CheckMenu.qml
    RESOURCES
        resources/cursors/LICENSE
        resources/cursors/bx-hand.svg
        resources/cursors/bxf-arrow-from-left-stroke.svg
        resources/cursors/bxf-arrow-from-right-stroke.svg
        resources/cursors/bxf-bracket.svg
        resources/cursors/bxf-cursor-pen.svg
        resources/cursors/bxf-cut.svg
        resources/cursors/bxf-finger-up.svg
        resources/cursors/bxf-hand.svg
        resources/cursors/bxf-move-horizontal.svg
        resources/cursors/bxf-move-vertical.svg
        resources/cursors/bxf-move.svg
        resources/cursors/bxf-music.svg
        resources/cursors/bxf-plus-big.svg
        resources/cursors/bxf-search-big-minus.svg
        resources/cursors/bxf-search-big-plus.svg
        resources/cursors/bxf-stopwatch.svg
        resources/cursors/bxf-swap-horizontal.svg
)

# P.2 (E5): Premiere's editing and sequence commands in the app (Editing: Video Options, Replace With Clip, Join Through
# Edits and Make Subsequence in the clip menu; Duplicate, New Sequence From Clip, Sequence Settings, Merge Clips, Modify,
# Replace Footage, Make Offline and Edit Original in the Library's; Preferences > Media and Timeline; the History panel)
# and their dialogs under qml/editing/ (EditingHost: one element in Main.qml).
target_sources(wabcut_ui PRIVATE
    src/bridge/EditingBridge.h
    src/bridge/EditingBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/editing/EditingChoice.qml
        qml/editing/EditingHost.qml
        qml/editing/EditingPreferencesDialog.qml
        qml/editing/FieldOptionsDialog.qml
        qml/editing/FrameHoldOptionsDialog.qml
        qml/editing/HistoryPanel.qml
        qml/editing/InterpretFootageDialog.qml
        qml/editing/MergeClipsDialog.qml
        qml/editing/RawSourceSettingsDialog.qml
        qml/editing/ReplaceWithClipSubmenu.qml
        qml/editing/SequenceSettingsDialog.qml
        qml/editing/TimecodeDialog.qml
        qml/editing/VideoOptionsSubmenu.qml
)

# p02 audio (owner: Audio engine): Premiere's Audio Track Mixer routing - submix strips, track output assignment and
# sends (AudioMixerRouting.cpp, the `AudioMixer` singleton's routing half); the Audio Clip Mixer (`ClipMixer`).
target_sources(wabcut_ui PRIVATE
    src/bridge/AudioMixerRouting.cpp
    src/bridge/ClipMixerBridge.h
    src/bridge/ClipMixerBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/audio/OutputMenu.qml
        qml/audio/SendSlots.qml
        qml/audio/SubmixStrip.qml
        qml/audio/ClipMixerStrip.qml
        qml/panels/captions/ProfanitySection.qml
        qml/panels/TakeRecoveryHost.qml
)

# P.2 (E6): Premiere's Source and Program Monitor features under qml/monitor/: direct manipulation of Motion in the
# Program Monitor (`ProgramMotion`: the bounding box, handles, anchor point and motion path; MotionOverlay, one element
# in MonitorPanel.qml); the monitors' settings and keys (`Monitors`: Loop, Play In to Out, Nudge Selected Object).
target_sources(wabcut_ui PRIVATE
    src/bridge/MonitorBridge.h
    src/bridge/MonitorBridge.cpp
    src/bridge/MonitorGuides.h
    src/bridge/MonitorGuides.cpp
    src/bridge/MotionBridge.h
    src/bridge/MotionBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/monitor/AddGuideDialog.qml
        qml/monitor/DroppedFrameIndicator.qml
        qml/monitor/ExportFrameDialog.qml
        qml/monitor/GuidesOverlay.qml
        qml/monitor/GuideTemplatesDialog.qml
        qml/monitor/LoopButton.qml
        qml/monitor/MonitorHost.qml
        qml/monitor/MonitorOverlays.qml
        qml/monitor/MonitorRulers.qml
        qml/monitor/MonitorSettingsMenu.qml
        qml/monitor/MotionOverlay.qml
        qml/monitor/ProgramOutputWindow.qml
        qml/monitor/SourceSettingsMenu.qml
        qml/monitor/TransparencyGrid.qml
    RESOURCES
        resources/monitor/transparency-grid.svg
)
# 0.2 graphics, captions and effects workflow (Premiere parity, docs/effects.md, docs/graphics.md): effect presets, the
# Effects panel's Presets / Lumetri Presets bins, custom bins and favourites (`EffectPresets`, the user's effects
# library shared with the assistant) and their menus and dialogs under qml/panels/effects/.
target_sources(wabcut_ui PRIVATE
    src/bridge/EffectPresetsBridge.h
    src/bridge/EffectPresetsBridge.cpp
    src/bridge/GraphicToolsBridge.h
    src/bridge/GraphicToolsBridge.cpp
)
# 0.2 graphics: spell check in captions and text (model/Spelling.h, editor/SystemSpellChecker.h): the `Spelling`
# singleton and the SpellHighlighter element (SpellingBridge), the right-click menu of a misspelled word and the
# Captions page's Check Spelling dialog.
target_sources(wabcut_ui PRIVATE
    src/bridge/SpellingBridge.h
    src/bridge/SpellingBridge.cpp
)
wabcut_ui_qml_sources(
    QML_FILES
        qml/components/SpellingMenu.qml
        qml/panels/captions/CaptionsSpellingDialog.qml
        qml/panels/captions/CaptionsFindReplaceDialog.qml
        qml/panels/color/ColorPanelMenu.qml
        qml/panels/effects/EffectItemMenu.qml
        qml/panels/effects/EffectNameDialog.qml
        qml/panels/effects/EffectSectionMenu.qml
        qml/panels/effects/EffectsPanelMenu.qml
        qml/panels/effects/SavePresetDialog.qml
        qml/panels/graphics/GraphicToolsSection.qml
        qml/panels/graphics/TextStyleItems.qml
)

# Every block's QML files and resources (wabcut_ui_qml_sources above), in one call.
get_property(wabcut_ui_qml_files GLOBAL PROPERTY WABCUT_UI_QML_FILES)
get_property(wabcut_ui_qml_resources GLOBAL PROPERTY WABCUT_UI_QML_RESOURCES)
qt_target_qml_sources(wabcut_ui
    QML_FILES ${wabcut_ui_qml_files}
    RESOURCES ${wabcut_ui_qml_resources}
)
