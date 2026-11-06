# Capture tests (0.7, docs/capture.md): the pure parts, the recorder fed with chosen times, the session over the fake backend in
# real time, the system backend (skipped without a device). No hardware is needed for any of them but the device test, which is
# skipped when nothing is connected. Outputs land in <build>/test-output/capture/.
find_package(FFMPEG REQUIRED)

wabcut_add_test_suite(capture
    SOURCES
        CaptureTestUtil.cpp
        CaptureTestUtil.h
        test_capture_device.cpp
        test_capture_files.cpp
        test_capture_mf_file.cpp # Windows: the Media Foundation source against a generated clip (no camera opened)
        test_capture_recorder.cpp
        test_capture_session.cpp
    LIBS wabcut_capture wabcut_export wabcut_model)
target_include_directories(wabcut_capture_tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_include_directories(wabcut_capture_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/capture/src) # SystemBackends.h (private)
target_compile_definitions(wabcut_capture_tests PRIVATE WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media")
target_link_directories(wabcut_capture_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(wabcut_capture_tests PRIVATE ${FFMPEG_LIBRARIES})
