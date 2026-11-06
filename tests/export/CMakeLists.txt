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
        test_audio_post_host.cpp # P.2: AAF / OMF audio rendered from real media, and the CLI
        test_caption_files_export.cpp # P.2: caption sidecar formats and 608 embedded in H.264
        test_caption_redaction.cpp
        test_caption_text_rules.cpp # card 114: case, punctuation and accents in caption files
        test_cli_render.cpp
        test_content_credentials.cpp # AI-15: signed C2PA Content Credentials in MP4 exports
        test_export.cpp
        test_export_audio_ai.cpp
        test_export_parity.cpp # 0.2: Premiere's formats, presets, effects, metadata, queue, watch folders, smart render
        test_gaming_export.cpp # I.9 (gaming): 60 fps presets, target sizes under Discord's limits
        test_media_formats_export.cpp # 0.2: deinterlaced and spanned media through the export
        test_hdr_export.cpp # I.9: PQ / HLG exports, HDR10 metadata, colour-managed preview == export
        test_proxy.cpp
        test_queue.cpp
        test_rendered.cpp
        test_matte_export.cpp # AI-14: matte redactions fail closed, export == preview
        test_highlights_e2e.cpp # G.2: a recording's highlights -> a reel -> MP4, end to end
        test_settings.cpp
        test_sidecar_text.cpp
    LIBS wabcut_export wabcut_media wabcut_playback wabcut_audio wabcut_ai_highlights_media wabcut_ai_c2pa
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
# P.2 (E6): Export Frame's stills (every format, OpenEXR by the spec) and sequence.exportFrame / source.exportFrame.
target_sources(wabcut_export_tests PRIVATE test_still_export.cpp)
target_link_libraries(wabcut_export_tests PRIVATE wabcut_editor)
# 0.6 (#72): the output colour space and SDR Conform in exports and wabcut-cli render.
target_sources(wabcut_export_tests PRIVATE test_sdr_conform_export.cpp)
# 0.6 (#73): MXF OP1a / OP-Atom, XDCAM HD422, AVC-Intra 100, embedded CEA-608 / 708 (SMPTE 436M, SEI, c608).
target_sources(wabcut_export_tests PRIVATE test_mxf_export.cpp)
# 0.6 (#52): dialogue / music / effects stems with the master, by track role.
target_sources(wabcut_export_tests PRIVATE test_stems_export.cpp)
# 0.7 (C1): the sequence's pixel aspect ratio in exports, preset ids, output names with dots, the target-size audio estimate
# and second pass, UTF-8 safe cuts, the OP-Atom file set moved into place as one unit (private headers of src/export/src).
target_sources(wabcut_export_tests PRIVATE test_r08_c1.cpp)
target_include_directories(wabcut_export_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/export/src)

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
# 0.6 (#73): MXF delivery and Embedded captions in the Export workspace (summary, job report, drawer screenshots).
if(TARGET wabcut_export_ui_tests)
    target_sources(wabcut_export_ui_tests PRIVATE ui/test_mxf_export_ui.cpp)
endif()
# 0.7 (C2): the Streaming (-14 LUFS) loudness standard.
target_sources(wabcut_export_tests PRIVATE test_r08_c2.cpp)
# 0.7 (C4): H.264 / HEVC smart rendering (whole GOPs copied), the pre-export check, the newer codecs.
target_sources(wabcut_export_tests PRIVATE test_r08_c4.cpp)
# 0.7 (card "Dolby Vision 8.4 and HDR10+ dynamic metadata"): the HDR10+ plan, SEI insertion and its round trip through FFmpeg's MP4
# muxer and decoder, the settings rules, the Dolby Vision licence gate, and the hardware-encoder end-to-end cases.
target_sources(wabcut_export_tests PRIVATE test_hdr_dynamic.cpp)
# 0.7 (A7): a soloed video track is the only one the export's used-media check and the pre-export check read.
target_sources(wabcut_export_tests PRIVATE test_r08_a7.cpp)
# 0.7 (card 38): the spherical metadata of VR / 360 exports.
target_sources(wabcut_export_tests PRIVATE test_vr_export.cpp)
# 0.7 (card 034): OpenFX video plug-ins in the export - the effect is in the file and the export matches the program monitor,
# with the test plug-ins of tests/ofxhost (built into <build>/test-ofx) and the real wabcut-ofxhost helper.
if(TARGET wabcut_ofx)
    find_package(nlohmann_json CONFIG REQUIRED)
    target_sources(wabcut_export_tests PRIVATE test_ofx_export.cpp)
    target_link_libraries(wabcut_export_tests PRIVATE wabcut_ofx nlohmann_json::nlohmann_json)
    target_include_directories(wabcut_export_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/ofxhost)
    target_compile_definitions(wabcut_export_tests PRIVATE
        WABCUT_TEST_OFX_DIR="${CMAKE_BINARY_DIR}/test-ofx"
        WABCUT_OFXHOST_PATH="$<TARGET_FILE:wabcut-ofxhost>")
    add_dependencies(wabcut_export_tests wabcut-ofxhost wabcut_test_ofx_main wabcut_test_ofx_crash wabcut_test_ofx_hang)
endif()
