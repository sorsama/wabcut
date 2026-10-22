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
    src/Animations.cpp
    src/BeatGrid.cpp
    src/Blend.cpp
    src/BuiltinTemplates.cpp
    ${embedded_templates}
    src/CaptionOps.cpp
    src/CaptionStyle.cpp
    src/CaptionText.cpp
    src/ChangeSet.cpp
    src/Clip.cpp
    src/ClipGroupOps.cpp
    src/FadeOps.cpp
    src/SelectionOps.cpp
    src/Clipboard.cpp
    src/ClipList.cpp
    src/ColorManagement.cpp
    src/ColorManagementOps.cpp
    src/DeadTime.cpp
    src/EditOps.cpp
    src/EditingOps.cpp
    src/EditSession.cpp
    src/FieldOptions.cpp
    src/FrameScaling.cpp
    src/GamingOps.cpp
    src/GamingVersions.cpp
    src/GraphicJson.cpp
    src/GraphicOps.cpp
    src/GraphicResolve.cpp
    src/Graphics.cpp
    src/Effects.cpp
    src/EffectPresets.cpp
    src/EffectsLibrary.cpp
    src/EffectsAudioAmplitude.cpp
    src/EffectsAudioFilter.cpp
    src/EffectsAudioTime.cpp
    src/History.cpp
    src/JsonIo.h
    src/KeyframeOps.cpp
    src/Layouts.cpp
    src/MaskOps.cpp
    src/Masks.cpp
    src/ImageSequence.cpp
    src/LayeredFile.cpp
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
    src/RawSourceSettings.cpp
    src/RedactedSpeech.cpp
    src/RedactionMedia.cpp
    src/Resolve.cpp
    src/SequencePresets.cpp
    src/SequenceSettings.cpp
    src/ReviewNotes.cpp
    src/SequenceEditor.cpp
    src/SourceMarkOps.cpp
    src/MarkerOps.cpp
    src/SequenceEditor.h
    src/Snap.cpp
    src/SpannedClip.cpp
    src/SnapshotReleaser.cpp
    src/Spelling.cpp
    src/Synthetic.cpp
    src/CaseFolding.inc
    src/TextFind.cpp
    src/SpeedOps.cpp
    src/EffectsMotion.cpp
    src/TimeInterpolation.cpp
    src/SubmixOps.cpp
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

# P.2 (E6, monitors): Motion set as one edit (Position, Scale, Rotation, Anchor Point at one curve time): the Program
# Monitor's direct manipulation and clip.setMotion.
target_sources(wabcut_model PRIVATE src/MotionOps.cpp)
