# wabcut_media — FFmpeg probe/decode (HW + SW fallback), frame cache, thumbnails, waveform peaks, proxies.
# Owner: Media engine. Public headers under include/wabcut/media/. No Qt in this module.
find_package(FFMPEG REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED) # PSD ZIP channels (Psd.cpp)
find_package(nlohmann_json CONFIG REQUIRED) # RAW helpers' headers (RawHelper.cpp)

add_library(wabcut_media STATIC
    src/AudioDecoder.cpp
    src/Background.cpp
    src/CameraLog.cpp
    src/Chapters.cpp # P0 round: OBS chapter markers on import
    src/ContainerTags.cpp
    src/DecoderPool.cpp
    src/FfmpegInfo.cpp
    src/FfmpegUtil.cpp
    src/Fingerprint.cpp
    src/FrameCache.cpp
    src/FrameHandle.cpp
    src/GpuInfo.cpp
    src/Log.cpp
    src/Probe.cpp
    src/Psd.cpp
    src/RawDevelop.cpp
    src/RawHelper.cpp
    src/StreamInfo.cpp
    src/StreamTime.cpp
    src/ThumbnailService.cpp
    src/VideoDecoder.cpp
    src/WaveformPeaks.cpp
)
target_include_directories(wabcut_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# FFmpeg headers are SYSTEM so /W4 /WX stays about our code. They never appear in public headers.
target_include_directories(wabcut_media SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(wabcut_media PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(wabcut_media PUBLIC wabcut_model Threads::Threads PRIVATE ${FFMPEG_LIBRARIES} ZLIB::ZLIB nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(wabcut_media PRIVATE dxgi) # GPU vendor detection (GpuInfo.cpp)
endif()
wabcut_target_defaults(wabcut_media)
