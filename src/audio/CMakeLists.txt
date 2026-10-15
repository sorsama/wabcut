# wabcut_audio — miniaudio device output, float32 mixer, master clock (drives A/V sync).
# Owner: Audio engine. Public headers under include/wabcut/audio/. See README.md for the threading model.
#
# The vcpkg `miniaudio` port is header-only: it installs miniaudio.h and no CMake package. The implementation
# is compiled exactly once, in src/miniaudio_impl.cpp (MINIAUDIO_IMPLEMENTATION). The header is an include of
# wabcut_audio's private sources only, as SYSTEM, so /W4 /WX never fires on third-party code and consumers of
# wabcut_audio never see miniaudio types.
find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)
find_package(Threads REQUIRED)
# DSP dependencies: libebur128 (MIT; loudness) installs a library and pkg-config file only; RNNoise (BSD-3; Reduce
# Noise) comes from the overlay port in cmake/ports.
find_path(EBUR128_INCLUDE_DIR NAMES ebur128.h REQUIRED)
find_library(EBUR128_LIBRARY NAMES ebur128 REQUIRED)
find_package(unofficial-rnnoise CONFIG REQUIRED)

add_library(wabcut_audio STATIC
    src/AudioEngine.cpp
    src/AudioInput.cpp
    src/DeviceIds.h
    src/DeviceIo.cpp
    src/LevelMeter.cpp
    src/LoudnessTap.cpp
    src/Mixer.cpp
    src/OfflineRenderer.cpp
    src/RealTime.cpp
    src/RenderCore.cpp
    src/Sources.cpp
    src/TransportMap.cpp
    src/WavWriter.cpp
    src/miniaudio_impl.cpp
    # DSP: the audio effects (preview and export), loudness, noise reduction, variable-speed audition.
    src/dsp/Biquad.cpp
    src/dsp/Compressor.cpp
    src/dsp/Denoiser.cpp
    src/dsp/Equalizer.cpp
    src/dsp/Limiter.cpp
    src/dsp/Loudness.cpp
    src/dsp/Repair.cpp
    src/dsp/Space.cpp
    src/dsp/SpeedRender.cpp
    src/dsp/TimeStretch.cpp
    src/effects/EffectChain.cpp
)
target_include_directories(wabcut_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(wabcut_audio SYSTEM PRIVATE ${MINIAUDIO_INCLUDE_DIR} ${EBUR128_INCLUDE_DIR})
target_compile_features(wabcut_audio PUBLIC cxx_std_23)
# Only the device layer of miniaudio is used; these must be identical in every TU that includes miniaudio.h.
target_compile_definitions(wabcut_audio PRIVATE
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
target_link_libraries(wabcut_audio PUBLIC wabcut_model PRIVATE Threads::Threads ${EBUR128_LIBRARY} unofficial::rnnoise::rnnoise)
if(APPLE)
    target_link_libraries(wabcut_audio PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
    # miniaudio loads ALSA/PulseAudio/JACK at runtime with dlopen.
    target_link_libraries(wabcut_audio PRIVATE ${CMAKE_DL_LIBS} m)
endif()
wabcut_target_defaults(wabcut_audio)
