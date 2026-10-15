# wabcut_ai_shorts — the shorts finder (AI-9, docs/ai.md "Shorts finder"): sentences, TextTiling topics, local scoring
# with an explainable breakdown (hook, structure, topic, energy, visual, clean, length, query), snapping to sentences
# and cuts, titles, the optional guarded cloud re-rank, and shorts.find / shorts.create. Pure C++, core preset.
# Owner: AI platform engineer.
#
# shorts.create runs catalog commands (transcript.cleanup, captions.generate, title.add, audio.normalize) on each new
# short, so it uses wabcut_ai_edit's editingCommands(), and editingCommands() registers the shorts commands: the two
# static libraries depend on each other, which CMake links correctly (it repeats them on the link line).
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_shorts STATIC
    src/Rerank.cpp
    src/Shorts.cpp
    src/ShortsCommands.cpp)
target_include_directories(wabcut_ai_shorts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_shorts PUBLIC wabcut_ai_edit wabcut_ai_reframe wabcut_ai_vision wabcut_command wabcut_model wabcut_ai
    PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_shorts)
