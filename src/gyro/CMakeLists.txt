# wabcut_gyro - the gyroscope metadata cameras record in their files (0.7, Warp Stabilizer's "Gyro (camera metadata)"
# method; docs/motion.md "Stabilize with the camera's gyro"): the GoPro GPMF and Sony rtmd parsers (untrusted binary
# input, bounds checked), the camera-frame convention, quaternions and the integral of the angular velocity. Pure C++
# (no Qt, no FFmpeg): wabcut_media hands over a data track's packets, wabcut_ai_stabilize turns the rotation into the
# Warp Stabilizer's camera path. Builds in the core preset.
add_library(wabcut_gyro STATIC
    src/Gpmf.cpp
    src/Gyro.cpp
    src/Rotation.cpp
    src/SonyRtmd.cpp)
target_include_directories(wabcut_gyro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_gyro PUBLIC cxx_std_23)
target_link_libraries(wabcut_gyro PUBLIC wabcut_model wabcut_command)
wabcut_target_defaults(wabcut_gyro)
