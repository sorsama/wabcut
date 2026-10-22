# wabcut_motion - motion estimation (P.2, docs/motion.md): dense optical flow (Dense Inverse Search, coarse to fine)
# between two grey pictures, the flow-based frame interpolation the compositor's flow mix implements (its CPU
# reference, shaders/flowmix.frag), and the deterministic parallel loop they share. Time Interpolation (Optical Flow),
# Morph Cut and the stabilizers all estimate motion here. Pure C++ (no Qt, no FFmpeg): the caller decodes and downsizes
# the pictures. Builds and is tested in the core preset.
find_package(Threads REQUIRED)

add_library(wabcut_motion STATIC
    src/Flow.cpp
    src/Geometry.cpp
    src/Image.cpp
    src/Interpolate.cpp
)
target_include_directories(wabcut_motion PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_motion PUBLIC cxx_std_23)
target_link_libraries(wabcut_motion PUBLIC Threads::Threads)
wabcut_target_defaults(wabcut_motion)
