# wabcut_capture - capture from a camera, a capture card or (where the system exposes it) a tape deck (0.7, docs/capture.md).
# The device layer behind CaptureBackend (CaptureBackend.h): Media Foundation on Windows, a fake backend for the tests and
# screenshots, "not available" elsewhere (V4L2 / AVFoundation are a later step); a recorder that writes the pictures and the
# sound to a file with the export's encoders while they arrive (CaptureRecorder.h, over wabcut_export's CaptureWriter); the
# files of a capture: names, the journal and the repair of a capture the program stopped in the middle of (CaptureFiles.h).
# Qt-free. Owner: Project panel / Library.
find_package(FFMPEG REQUIRED)
find_package(Threads REQUIRED)

add_library(wabcut_capture STATIC
    src/AudioInputSource.cpp
    src/CaptureBackend.cpp
    src/CaptureFiles.cpp
    src/CapturePresets.cpp
    src/CaptureRecorder.cpp
    src/CaptureSession.cpp
    src/CaptureTypes.cpp
    src/FakeBackend.cpp
    src/Preview.cpp
    src/SystemBackends.h
)
if(WIN32)
    target_sources(wabcut_capture PRIVATE src/MediaFoundationBackend.cpp)
endif()
target_include_directories(wabcut_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# FFmpeg headers are SYSTEM so /W4 /WX stays about our code. They never appear in public headers.
target_include_directories(wabcut_capture SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(wabcut_capture PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(wabcut_capture
    PUBLIC wabcut_model wabcut_export
    PRIVATE wabcut_audio ${FFMPEG_LIBRARIES} Threads::Threads)
if(WIN32)
    target_link_libraries(wabcut_capture PRIVATE mfplat mf mfreadwrite mfuuid ole32)
endif()
wabcut_target_defaults(wabcut_capture)
