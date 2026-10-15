# wabcut_render_reference - CPU reference renders, the test oracles of C.15's compositing (docs/internal/c15-parity-handoff.md):
# the 27 blend modes, the Ultra Key, the layout effects (masks, Border, Drop Shadow, Gaussian Blur, the vertical blur
# fill) and the synthetic media, in double precision on linear premultiplied pictures, plus a text golden-file format
# and an image comparison. The render owner's shaders are tested against it. Pure C++ (links only wabcut_model; no Qt,
# no GPU, no FFmpeg): it builds in the core preset. Owner: Graphics.
add_library(wabcut_render_reference STATIC
    src/Blend.cpp
    src/Golden.cpp
    src/Image.cpp
    src/Layout.cpp
    src/Synthetic.cpp
    src/UltraKey.cpp
)
target_include_directories(wabcut_render_reference PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_render_reference PUBLIC cxx_std_23)
target_link_libraries(wabcut_render_reference PUBLIC wabcut_model)
wabcut_target_defaults(wabcut_render_reference)
