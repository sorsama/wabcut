# wabcut_ai_reframe — auto reframe (AI-9, docs/ai.md "Auto reframe"): the L1-optimal virtual camera (ADMM) and sparse
# keyframe fitting, per-shot planning over subject analyses (follow / faces / speaker / anchor; crop, letterbox or blur
# fill), the reframed sequence copy with editable Transform keyframes and its derivation record, and the
# analysis.subjects / reframe.* commands. Pure C++ (model + command + ai_vision), so it builds and is tested in the
# core preset. Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_reframe STATIC
    src/CameraPath.cpp
    src/Reframe.cpp
    src/ReframeCommands.cpp)
target_include_directories(wabcut_ai_reframe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_reframe PUBLIC wabcut_ai_vision wabcut_command wabcut_model wabcut_ai
    PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_reframe)
