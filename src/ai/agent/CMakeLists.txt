# wabcut_ai_agent — the AI editing assistant (AI-5, docs/ai.md "Editing assistant"): a tool-use loop over IChatModel
# that edits a staging branch of the project with the command catalog, deterministic verification of the staged
# result, quick actions and the guarded apply onto the live session. Pure C++ (model + command + ai + ai_edit), so
# it builds in the core preset. Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)
add_library(wabcut_ai_agent STATIC
    src/ConstrainedTools.cpp # AI-15: tool calls as one JSON-schema constrained reply per step
    src/Describe.cpp
    src/EditAgent.cpp
    src/Preview.cpp
    src/QuickActions.cpp
    src/ToolRouter.cpp # AI-15: the tools a request is about, for small contexts
    src/Verification.cpp)
target_include_directories(wabcut_ai_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_agent PUBLIC wabcut_ai wabcut_ai_edit wabcut_command wabcut_model nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_agent)
