wabcut_add_test_suite(audio
    SOURCES
        test_ring_buffer.cpp
        test_seqlock.cpp
        test_gain.cpp
        test_mixer.cpp
        test_sources.cpp
        test_time_math.cpp
        test_transport_map.cpp
        test_transport_engine.cpp
        test_clock.cpp
        test_engine.cpp
        test_overlay.cpp
        test_device.cpp
        test_device_status.cpp
        DspTestUtil.h
        test_denoise.cpp
        test_dsp_dynamics.cpp
        test_dynamics_processing.cpp # 0.7 (card 044): the graph-based multiband dynamics
        test_dsp_essential.cpp
        test_dsp_report.cpp
        test_dsp_filters.cpp
        test_dsp_fft.cpp
        test_effects_amplitude.cpp
        test_effects_filter.cpp
        test_effects_time.cpp
        test_effects_catalog.cpp
        test_loudness.cpp
        test_match.cpp # 0.7 (cards 045, 104, 141): spectra, EQ Match curve, level accumulator, radar inputs
        test_spectral_repair.cpp # 0.7 (card 047): Spectral Repair edits, DSP, effect and spectrogram
        test_speed_render.cpp
        test_time_stretch.cpp
        test_voice_over_io.cpp
        test_voice_over_recovery.cpp
    LIBS wabcut_audio wabcut_audio_edit)
# test_device_status.cpp drives the internal DeviceIo / RenderCore directly.
target_include_directories(wabcut_audio_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/audio/src)
# A.9: the 5.1 loudness test compares with libebur128 called directly.
target_include_directories(wabcut_audio_tests SYSTEM PRIVATE ${EBUR128_INCLUDE_DIR})
target_link_libraries(wabcut_audio_tests PRIVATE ${EBUR128_LIBRARY})
