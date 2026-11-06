# wabcut_export — render a sequence to a file: settings/presets, HW-first encoder selection with software
# fallback, offline audio mix through the playback path, libavformat muxing, job queue, and the clip audio and reference
# video of AAF / OMF export (P.2: AudioPostHost.h, over wabcut_interchange). Owner: Export.
# Public headers under include/wabcut/export/ (namespace wabcut::exporter). The production video source
# (RenderedVideoSource: playback evaluator + media decoders + render::OffscreenRenderer) needs Qt Gui (QRhi).
find_package(FFMPEG REQUIRED)
find_package(Threads REQUIRED)

add_library(wabcut_export STATIC
    src/AudioPostHost.cpp
    src/CaptionSidecar.cpp
    src/Encode.cpp
    src/Encode.h
    src/Encoders.cpp
    src/ExportCommandServices.cpp
    src/ExportEffects.cpp
    src/ExportJob.cpp
    src/ExportLoudness.cpp
    src/ExportLoudness.h
    src/ExportPresets.cpp
    src/ExportSettings.cpp
    src/Extras.h
    src/FrameEffects.cpp
    src/FrameEffects.h
    src/LongGop.cpp
    src/LongGop.h
    src/MetadataWriter.cpp
    src/MetadataWriter.h
    src/PreExportCheck.cpp
    src/SmartRender.cpp
    src/SmartRender.h
    src/SubtitleStream.cpp
    src/SubtitleStream.h
    src/WatchFolders.cpp
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
    PUBLIC wabcut_model wabcut_media wabcut_render wabcut_command wabcut_interchange Threads::Threads
    PRIVATE wabcut_playback wabcut_audio ${FFMPEG_LIBRARIES})
wabcut_target_defaults(wabcut_export)
# P.2 (E6): Export Frame - a sequence frame rendered as an export and written as DPX, JPEG, PNG, TIFF, Targa, BMP, GIF or
# OpenEXR (StillExport.h), and the command services behind sequence.exportFrame / source.exportFrame.
target_sources(wabcut_export PRIVATE src/StillExport.cpp)
# 0.6 (#73): broadcast delivery - MXF OP1a / OP-Atom, XDCAM HD422 and AVC-Intra 100, embedded CEA-608 / 708 captions in
# the picture stream, an SMPTE 436M track or a QuickTime c608 track (ClosedCaptions.h).
target_sources(wabcut_export PRIVATE src/ClosedCaptions.cpp src/Delivery.cpp src/Delivery.h src/MxfDelivery.cpp
                                     src/MxfDelivery.h)
# 0.6 (#52): dialogue / music / effects stems with the master, by track role (Stems.h).
target_sources(wabcut_export PRIVATE src/Stems.cpp)
# 0.7 (card "Capture video from tape, capture card or camera"): the file side of a live capture (CaptureWriter.h): the export's
# encoders fed picture by picture, into a file that survives the program stopping.
target_sources(wabcut_export PRIVATE src/CaptureWriter.cpp)
# 0.7 (card "Dolby Vision 8.4 and HDR10+ dynamic metadata"): HDR10+ measured from the rendered frames and written as T.35 SEI
# into PQ HEVC output, and the licence gate for Dolby Vision (DynamicMetadata.h).
target_sources(wabcut_export PRIVATE src/DynamicMetadata.cpp src/HdrDynamic.cpp src/HdrDynamic.h)
