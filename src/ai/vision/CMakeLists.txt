# wabcut_ai_vision — subject analysis for auto reframe and the shorts finder (AI-9, docs/ai.md "Auto reframe"): YuNet
# and YOLOX input/output decoding, ByteTrack-style tracking, spectral-residual saliency, the shot-aware adaptive
# sampling pipeline and HelperDetector (the detectors run out of process in wabcut-embed, like smart search's SigLIP 2).
# Pure C++ (model + ai + ai_analysis + ai_search), so it builds and is tested in the core preset.
# Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_vision STATIC
    src/ColorFaces.cpp
    src/Detection.cpp
    src/HelperDetector.cpp
    src/Saliency.cpp
    src/Subjects.cpp
    src/Tracker.cpp)
target_include_directories(wabcut_ai_vision PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_vision PUBLIC wabcut_ai wabcut_ai_analysis wabcut_ai_search wabcut_model wabcut_color
    PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_vision)

# wabcut_ai_vision_media — MediaSubjectProvider: decoded frames (search::VideoPictureReader), cached C.5 shots and
# cached subject analyses of the project's files. Full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_vision_media STATIC src/MediaSubjects.cpp)
    target_link_libraries(wabcut_ai_vision_media PUBLIC wabcut_ai_vision wabcut_ai_search_media wabcut_ai_analysis_media wabcut_media)
    wabcut_target_defaults(wabcut_ai_vision_media)
endif()
