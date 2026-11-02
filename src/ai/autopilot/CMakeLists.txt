# wabcut_ai_autopilot - the creator autopilot (P1 #62, docs/creator-autopilot.md): auto B-roll from the library for what
# is said, a publishing kit (title ideas, description, chapters, tags, thumbnail candidates as files) and pacing match
# (a reference video's cut rhythm, cuts for the sequence), and the autopilot.* commands.
# Pure C++ (model + command + ai_edit + ai_shorts + ai_search + ai_analysis + ai_workflow), so it builds and is tested in
# the core preset. Owner: AI platform engineer.
#
# The commands are part of wabcut_ai_edit's editingCommands(): the static libraries depend on each other, like
# wabcut_ai_roughcut.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_autopilot STATIC
    src/AutopilotCommands.cpp
    src/Broll.cpp
    src/Pacing.cpp
    src/PublishingKit.cpp
    src/Words.cpp)
target_include_directories(wabcut_ai_autopilot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_autopilot PUBLIC wabcut_ai_edit wabcut_ai_shorts wabcut_ai_search wabcut_ai_analysis
    wabcut_ai_workflow wabcut_command wabcut_model wabcut_ai PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_autopilot)
