# wabcut_ai_workflow — the podcast / interview workflow (AI-13, docs/ai.md "Podcast and interview editing"): its plan,
# its steps (each through the editing catalog), chapters from the transcript's topics, the journal that resumes a run
# after a crash, and workflow.podcast. Pure C++ (command + ai_edit + ai_shorts + ai_multicam), core preset.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_workflow STATIC
    src/Podcast.cpp
    src/WorkflowCommands.cpp)
target_include_directories(wabcut_ai_workflow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_workflow PUBLIC wabcut_ai_multicam wabcut_ai_shorts wabcut_ai_edit wabcut_audio_edit wabcut_command
    wabcut_model wabcut_ai PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_workflow)
