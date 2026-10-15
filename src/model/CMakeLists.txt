# wabcut_model — pure C++ (no Qt): time, timeline data model, edit engine, undo, project IO.
# Owner: Core engine. Public headers under include/wabcut/model/, private sources under src/.
find_package(nlohmann_json CONFIG REQUIRED)

# C.16: the built-in graphic templates (templates/builtin/*.wabcut-template.json, the editable source of truth) are
# compiled in, so no host needs a resource path (BuiltinTemplates.h). The glob re-runs at build time when files come
# or go; the generated source is only rewritten when its content changes.
file(GLOB WABCUT_BUILTIN_TEMPLATES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/templates/builtin/*.wabcut-template.json)
list(SORT WABCUT_BUILTIN_TEMPLATES)
set(embedded_templates ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedTemplates.cpp)
add_custom_command(
    OUTPUT ${embedded_templates}
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${embedded_templates}
        -DTEMPLATE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/templates/builtin
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedTemplates.cmake
    DEPENDS ${WABCUT_BUILTIN_TEMPLATES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedTemplates.cmake
    COMMENT "Embedding the built-in graphic templates"
    VERBATIM)

add_library(wabcut_model STATIC
    src/BeatGrid.cpp
    src/Blend.cpp
    src/BuiltinTemplates.cpp
    ${embedded_templates}
    src/CaptionOps.cpp
    src/CaptionStyle.cpp
    src/CaptionText.cpp
    src/ChangeSet.cpp
    src/Clip.cpp
    src/Clipboard.cpp
    src/ClipList.cpp
    src/DeadTime.cpp
    src/EditOps.cpp
    src/EditSession.cpp
    src/GamingOps.cpp
    src/GamingVersions.cpp
    src/GraphicJson.cpp
    src/GraphicOps.cpp
    src/GraphicResolve.cpp
    src/Graphics.cpp
    src/Effects.cpp
    src/History.cpp
    src/JsonIo.h
    src/KeyframeOps.cpp
    src/Layouts.cpp
    src/MaskOps.cpp
    src/Masks.cpp
    src/MediaLocator.cpp
    src/Merge.cpp
    src/MoveOps.cpp
    src/Nesting.cpp
    src/Parameters.cpp
    src/PreviewCache.cpp
    src/ProjectDiff.cpp
    src/ProjectIO.cpp
    src/AudioChannelOps.cpp
    src/AudioChannels.cpp
    src/ProjectOps.cpp
    src/ProjectState.cpp
    src/RedactedSpeech.cpp
    src/RedactionMedia.cpp
    src/Resolve.cpp
    src/SequencePresets.cpp
    src/ReviewNotes.cpp
    src/SequenceEditor.cpp
    src/SourceMarkOps.cpp
    src/MarkerOps.cpp
    src/SequenceEditor.h
    src/Snap.cpp
    src/SnapshotReleaser.cpp
    src/Synthetic.cpp
    src/SpeedOps.cpp
    src/TimeRemap.cpp
    src/TimeRemapOps.cpp
    src/Timecode.cpp
    src/TitleOps.cpp
    src/TranscriptOps.cpp
    src/Transcripts.cpp
    src/TransitionOps.cpp
    src/TrimOps.cpp
)
target_include_directories(wabcut_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_model PUBLIC cxx_std_23)
# JSON is an implementation detail of ProjectIO; no public header includes it.
find_package(ZLIB REQUIRED)
target_link_libraries(wabcut_model PRIVATE nlohmann_json::nlohmann_json ZLIB::ZLIB)
wabcut_target_defaults(wabcut_model)
