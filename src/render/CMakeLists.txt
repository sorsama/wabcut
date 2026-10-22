# wabcut_render — QRhi compositor (preview + export share it): ColorPipeline, FrameImporter, Compositor,
# OffscreenRenderer, CompositeMailbox. Shaders are Vulkan-style GLSL baked to .qsb by qt_add_shaders.
# Owner: Graphics/playback. Public headers under include/wabcut/render/.
add_library(wabcut_render STATIC
    src/Caption.cpp
    src/ColorPipeline.cpp
    src/CompositeMailbox.cpp
    src/Compositor.cpp
    src/CompositorFlow.cpp
    src/EffectLibrary.cpp
    src/FrameImporter.cpp
    src/Mask.cpp
    src/Matte.cpp
    src/GpuScopes.cpp
    src/MediaFrameAdapter.cpp
    src/OffscreenRenderer.cpp
    src/SyntheticPicture.cpp
    src/Title.cpp
    src/GraphicRaster.cpp
    src/Transform.cpp
    src/ZeroCopy.cpp
)
target_include_directories(wabcut_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# GuiPrivate: QRhi is semi-public API (pinned to Qt 6.11 behaviour, see the root CMakeLists.txt).
target_link_libraries(wabcut_render PUBLIC wabcut_model wabcut_media wabcut_color wabcut_colorspaces wabcut_motion
                                           Qt6::Gui Qt6::GuiPrivate)
wabcut_target_defaults(wabcut_render)

# Resources under :/wabcut/render/shaders/<name>.qsb. GLSL 300 es / 330 (no ES 2: integer uniforms, fwidth),
# HLSL 5.0 and MSL 1.2 are the qsb defaults otherwise.
qt_add_shaders(wabcut_render "wabcut_render_shaders"
    PREFIX "/wabcut/render"
    GLSL "300es,330"
    FILES
        shaders/layer.vert
        shaders/layer.frag
        shaders/layer_array.frag
        shaders/fx.vert
        shaders/fx.frag
        shaders/output.vert
        shaders/output.frag
)

# 0.2: the transition and effect library's family shaders (with fx.vert's uniform block; Compositor kFxFamilyShaders).
# PRECOMPILE like the grade: their DXBC is built at build time, so the first transition does not wait for D3D's compiler.
qt_add_shaders(wabcut_render "wabcut_render_library_shaders"
    PREFIX "/wabcut/render"
    GLSL "300es,330"
    PRECOMPILE
    FILES
        shaders/fx_transition.frag
        shaders/fx_color.frag
        shaders/fx_distort.frag
        shaders/fx_stylize.frag
        shaders/fx_generate.frag
)

# Masks (I.6): signed distances, coverage, the masked mix, redaction styles. PRECOMPILE like the grade (DXBC at build
# time on Windows).
qt_add_shaders(wabcut_render "wabcut_render_mask_shaders"
    PREFIX "/wabcut/render"
    GLSL "300es,330"
    PRECOMPILE
    FILES
        shaders/mask.vert
        shaders/mask.frag
)

# The colour grade (I.5): PRECOMPILE bakes the HLSL to DXBC with fxc at build time on Windows (no effect elsewhere),
# so the first graded picture does not wait for D3D's compiler (~0.6 s for grade.frag on the shader's first use).
qt_add_shaders(wabcut_render "wabcut_render_grade_shaders"
    PREFIX "/wabcut/render"
    GLSL "300es,330"
    PRECOMPILE
    FILES
        shaders/grade.vert
        shaders/grade.frag
)

# The flow mix (P.2: Time Interpolation's Optical Flow, Morph Cut), with fx.vert. PRECOMPILE like the grade.
qt_add_shaders(wabcut_render "wabcut_render_flow_shaders"
    PREFIX "/wabcut/render"
    GLSL "300es,330"
    PRECOMPILE
    FILES
        shaders/flowmix.frag
)

# Compute shaders (GPU scopes): GLSL ES 3.1 / 4.3 at least.
qt_add_shaders(wabcut_render "wabcut_render_compute_shaders"
    PREFIX "/wabcut/render"
    GLSL "310es,430"
    PRECOMPILE
    FILES
        shaders/scopes.comp
)
