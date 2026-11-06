# wabcut_ai_video — the AI video-effect runner (0.7, docs/ai.md "AI video effects"): what AI Restore (upscale, deblur,
# light denoise) and AI Denoise are built on, and what later image-to-image effects (frame interpolation, refocus, face
# retouching, eye contact) reuse.
#   - Picture: a 16-bit RGB picture (+ optional alpha) with the few pixel operations the chain needs (resize, mix).
#   - Tiling: any picture through a tile function in overlapping tiles (memory bounded by the tile, not the picture).
#   - VideoModels: the model catalogue (ModelStore entries of kind Video) and a stage's steps (denoise -> deblur -> upscale).
#   - ProcessedCache: the per-clip cache of processed pictures on disk (zlib, atomic writes, an index, an LRU of decoded ones).
#   - Backend / HelperBackend: one model run on one picture; HelperBackend talks to wabcut-embed (ONNX Runtime, DirectML with
#     the CPU as fallback, out of process, restarted after a crash, killed to cancel). Tests use a fake backend.
#   - Runner: processes a clip's frames through a chain into the cache with progress, Cancel and resume; the frame source
#     (decoder) and the backend are injected.
# Pure C++ (model + ai + ai_search), so it builds and is tested in the core preset.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_library(wabcut_ai_video STATIC
    src/FaceParse.cpp
    src/Gaze.cpp
    src/HelperBackend.cpp
    src/Picture.cpp
    src/ProcessedCache.cpp
    src/Runner.cpp
    src/Targets.cpp
    src/Tiling.cpp
    src/VideoCommands.cpp
    src/VideoModels.cpp)
target_include_directories(wabcut_ai_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_video PUBLIC wabcut_ai wabcut_ai_search wabcut_command wabcut_model
    PRIVATE nlohmann_json::nlohmann_json ZLIB::ZLIB)
wabcut_target_defaults(wabcut_ai_video)

# wabcut_ai_video_media — the decoded frames in and out (FrameHandle <-> Picture with the colour matrix of the stream), the
# decoder-backed frame source, and the SourceFrameProcessor the evaluator asks (playback and export). Full builds only; the
# core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_video_media STATIC src/FrameConvert.cpp src/MediaFrames.cpp src/MorphFaces.cpp src/SourceProcessor.cpp)
    target_link_libraries(wabcut_ai_video_media PUBLIC wabcut_ai_video wabcut_ai_vision wabcut_media wabcut_playback wabcut_render)
    wabcut_target_defaults(wabcut_ai_video_media)
endif()
