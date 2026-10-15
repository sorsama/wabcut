# wabcut_export — render a sequence to a file: settings/presets, HW-first encoder selection with software
# fallback, offline audio mix through the playback path, libavformat muxing, job queue. Owner: Export.
# Public headers under include/wabcut/export/ (namespace wabcut::exporter). The production video source
# (RenderedVideoSource: playback evaluator + media decoders + render::OffscreenRenderer) needs Qt Gui (QRhi).
find_package(FFMPEG REQUIRED)
find_package(Threads REQUIRED)

add_library(wabcut_export STATIC
    src/CaptionSidecar.cpp
    src/Encode.cpp
    src/Encode.h
    src/Encoders.cpp
    src/ExportJob.cpp
    src/ExportSettings.cpp
    src/FfmpegCommon.h
    src/Mixdown.cpp
    src/Mixdown.h
    src/Proxy.cpp
    src/MediaTime.h
    src/TrimmedCopy.cpp
    src/RenderedVideoSource.cpp
    src/ReviewFrames.cpp
    src/SyntheticVideoSource.cpp
)
target_include_directories(wabcut_export PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# FFmpeg headers are SYSTEM so /W4 /WX stays about our code. They never appear in public headers.
target_include_directories(wabcut_export SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(wabcut_export PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(wabcut_export
    PUBLIC wabcut_model wabcut_media wabcut_render wabcut_command Threads::Threads
    PRIVATE wabcut_playback wabcut_audio ${FFMPEG_LIBRARIES})
wabcut_target_defaults(wabcut_export)
