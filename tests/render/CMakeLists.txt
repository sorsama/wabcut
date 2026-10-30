# Render tests: colour math (CPU), the compositor on the real GPU through a headless QRhi (D3D11 on Windows; no window
# is ever created), the texture pool, the mailbox and async offscreen rendering. The decoded-clip test uses the
# test-media fixture (tests/media) and ffmpeg for its swscale reference. Images land in <build>/test-output/render/.
wabcut_add_test_suite(render
    SOURCES
        RenderTestSupport.h
        RenderTestSupport.cpp
        EffectsReference.h
        EffectsReference.cpp
        test_color_math.cpp
        test_compositor.cpp
        test_effects.cpp
        test_flowmix.cpp # P.2: the flow mix (Optical Flow, Morph Cut) and the stabilizers' layer warp
        test_transitions.cpp # 0.2: Premiere's transition library
        test_library_effects.cpp # 0.2: Premiere's video effect library
        test_grade.cpp
        test_masks.cpp
        test_nested.cpp
        test_matte_masks.cpp # AI-14: matte masks (render/Matte.h)
        test_scopes_gpu.cpp
        test_color_management.cpp # I.9: colour management (ACES, HDR) against the CPU plans
        test_decoded_clip.cpp
        test_mailbox.cpp
        test_offscreen_async.cpp
        test_texture_pool.cpp
        test_captions.cpp
        test_titles.cpp
        test_graphics.cpp # C.13 motion graphics rasterised (I.9 Part 0.5)
        test_hdr_capture.cpp # I.9 (gaming): HDR game captures tone-mapped into SDR projects
    LIBS wabcut_render wabcut_render_reference wabcut_media wabcut_interchange Qt6::Gui
    PROPERTIES FIXTURES_REQUIRED wabcut_test_media LABELS render)
target_compile_definitions(wabcut_render_tests PRIVATE
    WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media"
    WABCUT_FFMPEG_EXECUTABLE="${WABCUT_FFMPEG_EXECUTABLE}"
    WABCUT_LOTTIE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/interchange/fixtures/lottie"
    WABCUT_RENDER_GOLDENS="${CMAKE_CURRENT_SOURCE_DIR}/goldens"
    WABCUT_REFERENCE_GOLDENS="${CMAKE_SOURCE_DIR}/tests/render_reference/goldens")

# The shaders' uniform blocks match from stage to stage, as OpenGL requires (no GPU needed).
add_test(NAME render.shader_blocks
    COMMAND "${CMAKE_COMMAND}" -DSHADER_DIR=${CMAKE_SOURCE_DIR}/src/render/shaders
            -P ${CMAKE_CURRENT_SOURCE_DIR}/ShaderBlocks.cmake)
set_tests_properties(render.shader_blocks PROPERTIES LABELS render)

# The program monitor on the real scene graph (QQuickRhiItem, offscreen window grab, the shell's QML module).
if(TARGET wabcut_ui)
    wabcut_add_test_suite(monitor
        SOURCES test_program_monitor.cpp
        LIBS wabcut_uiplugin wabcut_ui wabcut_playback wabcut_media Qt6::Gui Qt6::Quick Qt6::QuickControls2
        PROPERTIES FIXTURES_REQUIRED wabcut_test_media LABELS render)
    target_compile_definitions(wabcut_monitor_tests PRIVATE
        WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media"
        WABCUT_VERSION="${PROJECT_VERSION}")
endif()

# Zero-copy spike (report-only, docs/zero-copy.md): D3D11VA decoder surfaces sampled by QRhi on the same device.
# Uses FFmpeg directly (test-only). Exit 77 = not applicable on this machine (no D3D11 / hardware decoder).
if(WIN32)
    find_package(FFMPEG REQUIRED)
    add_executable(wabcut_zero_copy_spike spike/zero_copy_spike.cpp)
    target_include_directories(wabcut_zero_copy_spike SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(wabcut_zero_copy_spike PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(wabcut_zero_copy_spike PRIVATE wabcut_render wabcut_media Qt6::Gui Qt6::GuiPrivate
        ${FFMPEG_LIBRARIES} d3d11)
    target_compile_definitions(wabcut_zero_copy_spike PRIVATE
        WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media"
        WABCUT_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output")
    wabcut_target_defaults(wabcut_zero_copy_spike)
    qt_add_shaders(wabcut_zero_copy_spike "wabcut_zero_copy_spike_shaders"
        PREFIX "/wabcut/spike"
        BASE spike
        FILES spike/nv12_array.vert spike/nv12_array.frag)
    add_test(NAME render.zero_copy_spike COMMAND wabcut_zero_copy_spike)
    # Label gpu: D3D11 video decoding needs a real GPU (a GPU-less runner's software device has none).
    set_tests_properties(render.zero_copy_spike PROPERTIES
        FIXTURES_REQUIRED wabcut_test_media SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS gpu
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${WABCUT_QT_BIN_DIR}")
endif()
