# wabcut_editor — editor services that close the edit loop (I.1): media/timeline import, project lifecycle
# (save, autosave, recovery), offline media and relinking, library -> timeline drops, the playback session (video +
# timeline audio on the audio engine's clock) and the export seam. Plain C++ over the engine modules; the app's
# bridges (src/app/src/bridge) only adapt it to QML. Owner: Graphics/playback. Public headers under
# include/wabcut/editor/.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_editor STATIC
    src/Activity.cpp
    src/CacheInspector.cpp
    src/CameraCards.cpp
    src/Capsule.cpp
    src/ColorFrames.cpp
    src/Export.cpp
    src/MediaFormats.cpp
    src/MediaImport.cpp
    src/PathSafety.cpp
    src/PathSafety.h
    src/PerformanceProbe.cpp
    src/PlaybackSession.cpp
    src/PreviewRender.cpp
    src/ProjectDocument.cpp
    src/Production.cpp
    src/ProxyManager.cpp
    src/Recovery.cpp
    src/SourceEdit.cpp
    src/SystemSpellChecker.cpp
    src/Tracking.cpp
)
target_include_directories(wabcut_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_editor
    PUBLIC wabcut_model wabcut_interchange wabcut_audio wabcut_playback wabcut_export wabcut_masks
    PRIVATE wabcut_media nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_editor)
# 0.2 graphics: the system spell checker (SystemSpellChecker.h) is Windows' Spelling Checking API, a COM API.
if(WIN32)
    target_link_libraries(wabcut_editor PRIVATE ole32)
endif()
