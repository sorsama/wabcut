# wabcut_ai_audio — local AI audio (A.4, docs/ai.md "Enhance Speech" and "Stem separation"): DeepFilterNet3's signal
# processing (the networks run out of process in wabcut-embed), the chunking and stem overlap-add plans, the helper
# clients and the audio models' manifest entries. Pure C++ (model + ai + ai_search), so it builds and is tested in the
# core preset. Owner: Audio / Local-AI engineer.
add_library(wabcut_ai_audio STATIC
    src/AudioHelper.cpp
    src/AudioModels.cpp
    src/DeepFilter.cpp
    src/Fft.h
    src/Plans.cpp)
target_include_directories(wabcut_ai_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_audio PUBLIC wabcut_ai wabcut_ai_llm wabcut_ai_search wabcut_model)
wabcut_target_defaults(wabcut_ai_audio)

# wabcut_ai_audio_media — the Enhance Speech and stem separation jobs: decoding (wabcut_media's AudioDecoder), the
# chunked helper requests, the takes and stems as float WAV files. Full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_audio_media STATIC src/AudioJobs.cpp src/WavFile.cpp)
    target_link_libraries(wabcut_ai_audio_media PUBLIC wabcut_ai_audio wabcut_audio_edit wabcut_media)
    wabcut_target_defaults(wabcut_ai_audio_media)
endif()
