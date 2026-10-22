# wabcut_ai_segment — the AI Object Mask (AI-14, docs/ai.md "AI Object Mask", docs/masks.md "Object masks"): mattes
# (run-length encoded, signed distances, tracks in the analysis cache), propagation through a shot with stops at its
# cuts, SAM 2.1 in wabcut-embed (HelperSegmenter), subjects from the AI-9 detector, and mask.objectSelect /
# mask.refine / mask.propagate. Pure C++ (model + command + ai + ai_search + ai_vision), so it builds and is tested in
# the core preset. Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_segment STATIC
    src/FaceRedaction.cpp
    src/HelperSegmenter.cpp
    src/Matte.cpp
    src/ObjectMask.cpp
    src/ObjectMaskCommands.cpp
    src/Propagation.cpp)
target_include_directories(wabcut_ai_segment PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_segment PUBLIC wabcut_ai wabcut_ai_analysis wabcut_ai_search wabcut_ai_vision wabcut_command wabcut_model
    PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_segment)

# wabcut_ai_segment_media — the media's pictures (wabcut_media's decoder), source fingerprints, the services for a
# project and the compositor's matte source (CacheMatteSource implements wabcut_render's MatteSource). Full builds only;
# the core stays FFmpeg- and Qt-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_segment_media STATIC src/MatteCache.cpp src/MediaSegment.cpp)
    target_link_libraries(wabcut_ai_segment_media PUBLIC wabcut_ai_segment wabcut_ai_search_media wabcut_ai_edit_media wabcut_ai_vision
        wabcut_media wabcut_render wabcut_export PRIVATE wabcut_editor)
    wabcut_target_defaults(wabcut_ai_segment_media)
endif()
