# Media tests. tools/gen_test_media.py runs as a CTest fixture, so `ctest` generates the clips into
# build/<preset>/test-output/media (skipping what is already up to date) before any media test runs.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(ZLIB REQUIRED) # PsdWriter.h

set(WABCUT_TEST_MEDIA_DIR "${CMAKE_BINARY_DIR}/test-output/media")

# The ffmpeg.exe of the vcpkg dependency tree; the script also finds it on its own.
find_program(WABCUT_FFMPEG_EXECUTABLE NAMES ffmpeg
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/ffmpeg"
    NO_DEFAULT_PATH)
set(ffmpeg_arg "")
if(WABCUT_FFMPEG_EXECUTABLE)
    set(ffmpeg_arg --ffmpeg "${WABCUT_FFMPEG_EXECUTABLE}")
endif()

add_test(NAME media.generate_test_media
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_test_media.py"
            --out "${WABCUT_TEST_MEDIA_DIR}" ${ffmpeg_arg})
set_tests_properties(media.generate_test_media PROPERTIES
    FIXTURES_SETUP wabcut_test_media
    TIMEOUT 900
    LABELS media)

wabcut_add_test_suite(media
    SOURCES
        GpuMemory.cpp
        test_alpha.cpp
        test_audio_decoder.cpp
        test_camera_log.cpp # 0.2: camera log auto-detection
        test_container_tags.cpp # 0.2 P.3: the Metadata panel File section
        test_decoder_pool.cpp
        test_ffmpeg_info.cpp
        test_fingerprint.cpp
        test_frame_cache.cpp
        test_frame_handle.cpp
        test_leaks.cpp
        test_media_formats.cpp # 0.2: layered PSD, spanned clips, camera RAW, field order
        test_native_output.cpp
        test_probe.cpp
        test_stream_time.cpp
        test_thumbnails.cpp
        test_video_decoder.cpp
        test_waveform.cpp
    LIBS wabcut_media ZLIB::ZLIB
    PROPERTIES FIXTURES_REQUIRED wabcut_test_media LABELS media)
if(WIN32)
    # test_leaks reads the process's GPU memory through DXGI (GpuMemory.cpp).
    target_link_libraries(wabcut_media_tests PRIVATE dxgi)
endif()
target_compile_definitions(wabcut_media_tests PRIVATE WABCUT_TEST_MEDIA_DIR="${WABCUT_TEST_MEDIA_DIR}")
if(WIN32)
    target_link_libraries(wabcut_media_tests PRIVATE d3d11) # test_native_output.cpp creates a shared device
endif()

# 0.2 camera RAW helpers (media/RawHelper.h): a fake helper the tests find through WABCUT_RAW_HELPERS.
add_executable(wabcut_fake_raw_helper fake_raw_helper.cpp)
set_target_properties(wabcut_fake_raw_helper PROPERTIES OUTPUT_NAME wabcut-raw-fakeraw
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-raw-helpers")
wabcut_target_defaults(wabcut_fake_raw_helper)
add_dependencies(wabcut_media_tests wabcut_fake_raw_helper)
target_compile_definitions(wabcut_media_tests PRIVATE WABCUT_TEST_RAW_HELPERS="${CMAKE_BINARY_DIR}/test-raw-helpers")
