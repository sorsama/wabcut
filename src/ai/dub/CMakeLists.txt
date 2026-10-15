# wabcut_ai_dub — AI voice-over, dubbing and narration (AI-12, docs/ai.md "Going global"): the lines to speak (caption
# cues, script lines, rough-cut beats), fitting them to their slots, the run through the user's voice provider (BYOK,
# AiPlatform::speech: consent, spend cap, request log), the one-step edit (Dub track with the Dialogue role, the
# original dialogue ducked), and dub.create / narration.create. Pure C++ (model + command + ai), so it builds and is
# tested in the core preset. Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_dub STATIC
    src/Dub.cpp
    src/DubCommands.cpp
    src/Pipeline.cpp
    src/TimeFit.cpp
    src/Voices.cpp)
target_include_directories(wabcut_ai_dub PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_dub PUBLIC wabcut_ai_edit wabcut_ai_roughcut wabcut_ai_translate wabcut_audio_edit wabcut_command
    wabcut_model wabcut_ai PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_dub)

# wabcut_ai_dub_media — the real SpeechAudio: FFmpeg decoding of the providers' audio, the audio engine's WSOLA
# stretcher, float WAV files. Full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_dub_media STATIC src/MediaSpeechAudio.cpp)
    target_link_libraries(wabcut_ai_dub_media PUBLIC wabcut_ai_dub wabcut_ai_audio_media wabcut_audio wabcut_media)
    wabcut_target_defaults(wabcut_ai_dub_media)
endif()
