# wabcut_ai_analysis — media analysis for editing (C.5, docs/ai.md "Media analysis"): shot (scene-cut) detection
# after PySceneDetect's detectors, beat/tempo detection (onset strength, tempo prior, Ellis dynamic programming),
# their JSON (cache payloads) and the analysis.* / clip.splitAtShots commands. Pure C++ (model + command + ai).
# Owner: Core engine.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_analysis STATIC
    src/AnalysisCommands.cpp
    src/AnalysisJson.cpp
    src/BeatDetection.cpp
    src/Fft.h
    src/ShotDetection.cpp)
target_include_directories(wabcut_ai_analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_analysis PUBLIC wabcut_ai wabcut_command wabcut_model
    PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_analysis)

# wabcut_ai_analysis_media — decoding for the analysers (VideoFrameSource, readMonoAudio), the cached MediaAnalyzer
# and the commands' MediaAnalysisProvider. Full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_analysis_media STATIC src/MediaAnalysis.cpp)
    target_link_libraries(wabcut_ai_analysis_media PUBLIC wabcut_ai_analysis wabcut_media)
    wabcut_target_defaults(wabcut_ai_analysis_media)
endif()
