# wabcut_playback — playback: timeline audio (owner: Audio engine) and, later, the video Scheduler /
# PlaybackController (owner: Graphics/playback). Public headers under include/wabcut/playback/; the audio side
# lives in include/wabcut/playback/audio/ and src/audio/ so the video side can be added without conflicts.
add_library(wabcut_playback STATIC
    src/audio/AudioRedactions.cpp
    src/audio/AudioTakes.cpp
    src/audio/ChannelLayout.cpp
    src/audio/LiveFader.h
    src/audio/MasterBus.cpp
    src/audio/MasterBus.h
    src/audio/MediaAudioFeed.cpp
    src/audio/NestedMix.cpp
    src/audio/NestedMix.h
    src/audio/OfflineMixdown.cpp
    src/audio/SyntheticAudioFeed.cpp
    src/audio/TimelineAudioPlayer.cpp
    src/audio/TrackAudio.cpp
    src/audio/TrackRenderer.cpp
    src/audio/TrackStream.cpp
    src/audio/VoiceOverRecorder.cpp
    src/audio/TrackStream.h
)
target_include_directories(wabcut_playback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_playback PUBLIC wabcut_model wabcut_audio PRIVATE wabcut_media)
wabcut_target_defaults(wabcut_playback)

# Video side (owner: Graphics/playback): wabcut_playback_video, linked into wabcut_playback.
add_subdirectory(src/video)
