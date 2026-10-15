# wabcut_ai_multicam — AI multicam (AI-13, docs/ai.md "Podcast and interview editing"): active-speaker detection from
# transcripts, microphone levels and faces (Speakers.h, Evidence.h), automatic angle switching with explained cuts
# (AutoSwitch.h), analysis.speakers and multicam.autoSwitch. Pure C++ (model + command + ai_edit + ai_vision types), so
# it builds and is tested in the core preset. Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_multicam STATIC
    src/AutoSwitch.cpp
    src/Evidence.cpp
    src/MulticamAiCommands.cpp
    src/Speakers.cpp)
target_include_directories(wabcut_ai_multicam PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_multicam PUBLIC wabcut_ai_edit wabcut_ai_vision wabcut_command wabcut_model wabcut_ai
    PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_multicam)

# wabcut_ai_multicam_media — the media signals: each angle's microphone level (decoded with wabcut_media) and the
# cameras' faces (the AI-9 subject provider). Full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_multicam_media STATIC src/MediaAngleSignals.cpp)
    target_link_libraries(wabcut_ai_multicam_media PUBLIC wabcut_ai_multicam wabcut_ai_vision_media wabcut_media)
    wabcut_target_defaults(wabcut_ai_multicam_media)
endif()
