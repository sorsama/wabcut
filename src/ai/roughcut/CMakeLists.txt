# wabcut_ai_roughcut — creator superpowers over speech (AI-11, docs/ai.md "Creator superpowers"): Thai-aware text matching,
# scripts and outlines as beats, the speech corpus and take quality, the rough cut from a script (beat alignment, take
# choice, assembly, the plan record), retake grouping and "use this take" edits, and the roughcut.* / takes.* commands.
# Pure C++ (model + command + ai_edit + ai_shorts + ai_search), so it builds and is tested in the core preset.
# Owner: AI platform engineer.
#
# The commands are part of wabcut_ai_edit's editingCommands(), and they run catalog commands (sequence.extract,
# transcript.cleanup) themselves: the static libraries depend on each other, like wabcut_ai_shorts.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_roughcut STATIC
    src/Outline.cpp
    src/RoughCut.cpp
    src/RoughCutCommands.cpp
    src/Script.cpp
    src/Speech.cpp
    src/Takes.cpp
    src/TextMatch.cpp)
target_include_directories(wabcut_ai_roughcut PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_roughcut PUBLIC wabcut_ai_edit wabcut_ai_shorts wabcut_ai_search wabcut_command wabcut_model wabcut_ai
    PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_roughcut)

# wabcut_ai_roughcut_media — take level / peak / noise floor through AudioEnergyService (AI-7). Full builds only.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_roughcut_media STATIC src/MediaTakeProbe.cpp)
    target_link_libraries(wabcut_ai_roughcut_media PUBLIC wabcut_ai_roughcut wabcut_ai_edit_media wabcut_ai_transcribe)
    wabcut_target_defaults(wabcut_ai_roughcut_media)
endif()
