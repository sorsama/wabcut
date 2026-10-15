# wabcut_audio_edit — the dialogue-first audio commands (AI-4): track roles, track/master effect stacks, loudness
# (measure, normalize, master true-peak limiter) and auto-ducking; Essential Sound and Music Remix's planner (A.7); voice-over takes and record-arm (A.8). Pure C++ (model + command): it builds in the core
# preset and registers into editingCommands(). Owner: Audio engine. The analysis behind it is the host's
# (AudioServices, wabcut/audio/edit/AudioAnalysis.h).
add_library(wabcut_audio_edit STATIC
    src/AudioChannelCommands.cpp
    src/AudioCommands.cpp
    src/AudioCommandHelpers.h
    src/EssentialSoundCommands.cpp
    src/PluginCommands.cpp
    src/AudioSync.cpp
    src/Ducking.cpp
    src/EssentialSound.cpp
    src/GameAudio.cpp
    src/Loudness.cpp
    src/Remix.cpp
    src/SyncFft.h
    src/VoiceOver.cpp
    src/VoiceOverCommands.cpp)
target_include_directories(wabcut_audio_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_audio_edit PUBLIC wabcut_command wabcut_model)
wabcut_target_defaults(wabcut_audio_edit)

# wabcut_audio_edit_media — the media-backed AudioServices: loudness on the real render path (OfflineMixdown +
# libebur128, cached in the AnalysisCache) and speech from transcripts or the audio level. Full builds only.
if(NOT WABCUT_CORE_ONLY)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_library(wabcut_audio_edit_media STATIC src/LoudnessJobs.cpp src/MediaAnalysis.cpp src/MediaAudioSync.cpp src/MediaRemix.cpp)
    # A.7: Music Remix tracks beats with ai/analysis's DspBeatTracker.
    target_link_libraries(wabcut_audio_edit_media PUBLIC wabcut_audio_edit wabcut_playback wabcut_ai wabcut_ai_edit wabcut_media
                                                  PRIVATE wabcut_audio wabcut_ai_analysis nlohmann_json::nlohmann_json)
    wabcut_target_defaults(wabcut_audio_edit_media)
endif()
