# wabcut-cli — headless tool (probe, render, import, edit commands, LLM tool definitions, MCP server) used by
# integration tests, CI, scripts and AI agents. No QML.
add_executable(wabcut-cli main.cpp Extensions.cpp LocalLlm.cpp Analyze.cpp Scopes.cpp Search.cpp Subjects.cpp Transcribe.cpp
    TranscriptEdit.cpp)
target_link_libraries(wabcut-cli PRIVATE wabcut_model wabcut_media wabcut_interchange wabcut_command wabcut_mcp
    wabcut_export wabcut_editor wabcut_ai_edit_media wabcut_ai_analysis_media wabcut_ai_search_media wabcut_ai_vision_media
    wabcut_audio_edit_media wabcut_ai_audio_media wabcut_ai_transcribe wabcut_ai_net wabcut_ai_llm wabcut_ai_roughcut_media
    wabcut_ai_autozoom_media wabcut_ai_multicam_media wabcut_ai_segment_media wabcut_ai_highlights_media wabcut_script
    wabcut_ai_c2pa Qt6::Core)
# Extensions (C.12): `ext run` starts the wabcut-scripthost helper next to the CLI.
add_dependencies(wabcut-cli wabcut-scripthost)
# Only main.cpp names the version, so a version bump rebuilds one file.
set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS "WABCUT_VERSION=\"${PROJECT_VERSION}\"")
if(WIN32)
    target_link_libraries(wabcut-cli PRIVATE shell32) # CommandLineToArgvW: UTF-8 arguments for transcribe
endif()
wabcut_target_defaults(wabcut-cli)

# Audio plugins (A.6): plugin effects in renders and commands, plugins.list / plugins.scan; the wabcut-plughost helper
# runs next to the CLI.
if(TARGET wabcut_plugins)
    target_link_libraries(wabcut-cli PRIVATE wabcut_plugins)
    target_compile_definitions(wabcut-cli PRIVATE WABCUT_WITH_AUDIO_PLUGINS=1)
    add_dependencies(wabcut-cli wabcut-plughost)
endif()

# Local AI (WABCUT_WITH_LOCAL_AI): transcribe's local provider runs the wabcut-infer helper next to the CLI and
# downloads models over HTTP (wabcut_ai_local).
if(WABCUT_WITH_LOCAL_AI)
    target_link_libraries(wabcut-cli PRIVATE wabcut_ai_local)
    target_compile_definitions(wabcut-cli PRIVATE WABCUT_WITH_LOCAL_AI=1)
    add_dependencies(wabcut-cli wabcut-infer)
endif()
