# wabcut_ai_stabilize - video stabilization (C.15 parked, finished in P.2; docs/motion.md "Warp Stabilizer"): the global
# motion of a clip's pictures (features on a grid of cells, the I.6 pyramidal Lucas-Kanade tracker with a
# forward-backward check, RANSAC fits of a translation, a similarity or a homography, tracked against a keyframe so
# errors do not pile up), its cacheable JSON, the smoothing of the camera path (Gaussian, or auto reframe's L1-optimal
# path) into a compensating transform per frame with the crop / auto-scale framing, and the analysis commands
# (effect.analyzeStabilization, effect.analyzeRollingShutter, registered into editingCommands()). Pure C++ (no Qt, no
# FFmpeg): the host's masks::TrackingServices decode and downsize the pictures. Builds and is tested in the core preset.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_stabilize STATIC
    src/Analyze.cpp
    src/Homography.cpp
    src/Smooth.cpp
    src/StabilizeCommands.cpp
    src/StabilizeJson.cpp)
target_include_directories(wabcut_ai_stabilize PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_stabilize PUBLIC wabcut_masks wabcut_model wabcut_ai wabcut_command
    PRIVATE wabcut_ai_reframe nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_stabilize)