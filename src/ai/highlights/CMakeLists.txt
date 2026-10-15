# wabcut_ai_highlights - gameplay highlights (G.2, docs/ai.md "Gameplay highlights"): the measurements (loudness,
# voice, pitch, laughter-like bursts, motion, flashes, picture changes), chat logs and marker files, the fusion into
# ranked moments with reasons, and highlights.find / highlights.toReel / highlights.toShort. Pure C++, core preset.
# Owner: AI platform engineer.
#
# highlights.toShort runs layout.apply, captions.generate and title.add through wabcut_ai_edit's editingCommands(),
# and editingCommands() registers the highlight commands: the two static libraries depend on each other, as with
# wabcut_ai_shorts.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_highlights STATIC
    src/ChatLog.cpp
    src/Features.cpp
    src/HighlightCommands.cpp
    src/Highlights.cpp)
target_include_directories(wabcut_ai_highlights PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_highlights PUBLIC wabcut_ai_analysis wabcut_ai_edit wabcut_ai_reframe wabcut_ai_shorts wabcut_command wabcut_model wabcut_ai
    PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_highlights)

# wabcut_ai_highlights_media - decoding (AudioDecoder, VideoFrameSource), chapters (FFmpeg), marker files next to
# recordings, and the cached MediaHighlightProvider. Full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    find_package(FFMPEG REQUIRED)
    add_library(wabcut_ai_highlights_media STATIC src/MediaHighlights.cpp)
    target_include_directories(wabcut_ai_highlights_media SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(wabcut_ai_highlights_media PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(wabcut_ai_highlights_media PUBLIC wabcut_ai_highlights wabcut_ai_analysis_media wabcut_media
        PRIVATE ${FFMPEG_LIBRARIES})
    wabcut_target_defaults(wabcut_ai_highlights_media)
endif()
