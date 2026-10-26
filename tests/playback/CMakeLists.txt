wabcut_add_test_suite(playback
    SOURCES
        PlaybackFixture.h
        VoiceOverBooth.h
        test_effects_render.cpp
        test_enhance_speech.cpp
        test_faders.cpp
        test_nested_audio.cpp
        test_redaction_audio.cpp
        test_redaction_cases.cpp
        test_rt_audit.cpp
        test_speed_audio.cpp
        test_timeline_player.cpp
        test_track_audio.cpp
        test_voice_over.cpp
        test_voice_over_shutdown.cpp
        test_audio_channels.cpp
        test_submix.cpp
        test_clip_preroll.cpp
        test_audio_scrub.cpp
    LIBS wabcut_playback)

# Real media through the timeline player (MediaAudioFeed). A separate suite so the pure playback tests do not
# depend on media generation: these need the clips tools/gen_test_media.py writes (the media suite's fixture).
wabcut_add_test_suite(playback_media
    SOURCES test_media_feed.cpp
    LIBS wabcut_playback wabcut_media
    PROPERTIES FIXTURES_REQUIRED wabcut_test_media LABELS media)
target_compile_definitions(wabcut_playback_media_tests PRIVATE
    WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media")

# Video side (owner: Graphics/playback): Scheduler/PlaybackController suites and the playback benchmark.
add_subdirectory(video)
