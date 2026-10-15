# wabcut_masks - masks, tracking and redactions without pixels (I.6, docs/masks.md): the region tracker (pyramidal
# Lucas-Kanade + RANSAC on luma pictures the host supplies) and the mask / track / redaction commands (registered into
# editingCommands()). No Qt, no GPU, no decoder: it builds in the core preset. The GPU passes live in wabcut_render
# (Mask.h, shaders/mask.frag); the host's TrackingServices decode the pictures (wabcut_editor). Owner: Graphics.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_masks STATIC
    src/MaskCommands.cpp
    src/Tracker.cpp
)
target_include_directories(wabcut_masks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_masks PUBLIC cxx_std_23)
target_link_libraries(wabcut_masks PUBLIC wabcut_model wabcut_command PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_masks)
