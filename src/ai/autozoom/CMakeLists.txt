# wabcut_ai_autozoom — auto zoom for screen recordings (AI-11, docs/ai.md "Creator superpowers"): cursor sidecars and
# cursor logs, local activity detection on frames (cursor, clicks, typing, scene changes), the zoom path through AI-9's
# L1 camera optimiser, Position / Scale keyframes, and autozoom.apply. Pure C++ (model + command + ai_reframe), so it
# builds and is tested in the core preset. Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_autozoom STATIC
    src/Activity.cpp
    src/AutoZoom.cpp
    src/AutoZoomCommands.cpp)
target_include_directories(wabcut_ai_autozoom PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_autozoom PUBLIC wabcut_ai_reframe wabcut_command wabcut_model PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_autozoom)

# wabcut_ai_autozoom_media — frames of the recording (search::VideoPictureReader) and the sidecar next to its file.
# Full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_autozoom_media STATIC src/MediaAutoZoom.cpp)
    target_link_libraries(wabcut_ai_autozoom_media PUBLIC wabcut_ai_autozoom wabcut_ai_search_media wabcut_ai_edit_media)
    wabcut_target_defaults(wabcut_ai_autozoom_media)
endif()
