# Video side of wabcut_playback (owner: Graphics/playback): Scheduler, PlaybackController, TransportClock,
# decode sessions. A separate static target so the audio side's sources and this one never touch the same lines;
# wabcut_playback links it, so consumers keep linking wabcut_playback.
add_library(wabcut_playback_video STATIC
    CaptionRedaction.cpp
    CompositePool.cpp
    DecodeSession.cpp
    DecodeSession.h
    Downscale.cpp
    Downscale.h
    FrameSource.h
    PoolFrameSource.cpp
    PoolFrameSource.h
    Evaluate.cpp
    MotionAnalysis.cpp
    PlaybackController.cpp
    Scheduler.cpp
    TransportClock.cpp
)
target_include_directories(wabcut_playback_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(wabcut_playback_video PUBLIC wabcut_model wabcut_media wabcut_audio wabcut_render)
wabcut_target_defaults(wabcut_playback_video)

# INTERFACE works whether wabcut_playback is an INTERFACE or a STATIC library.
target_link_libraries(wabcut_playback INTERFACE wabcut_playback_video)
