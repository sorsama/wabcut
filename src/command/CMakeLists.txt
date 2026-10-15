# wabcut_command — pure C++ (no Qt, no network): the typed, validated edit-command protocol used by AI
# agents, wabcut-cli, scripting and MCP. Commands are thin adapters over wabcut_model's edit operations.
# Public headers under include/wabcut/command/, private sources under src/.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_command STATIC
    src/Catalog.h
    src/CatalogCaptions.cpp
    src/CatalogClipboard.cpp
    src/CatalogCollaboration.cpp
    src/CatalogEdit.cpp
    src/CatalogFinishing.cpp
    src/CatalogGraphics.cpp
    src/CatalogInterchange.cpp
    src/CatalogLayout.cpp
    src/CatalogGaming.cpp
    src/CatalogParity.cpp
    src/CatalogLibrary.cpp
    src/CatalogQuery.cpp
    src/CatalogRhythm.cpp
    src/CatalogMarkers.cpp
    src/CatalogNesting.cpp
    src/CatalogSource.cpp
    src/Cli.cpp
    src/Command.cpp
    src/Engine.cpp
    src/ExecutionContext.cpp
    src/Registry.cpp
    src/ReviewPackage.cpp
    src/Schema.cpp
    src/ToolExport.cpp
)
target_include_directories(wabcut_command PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_command PUBLIC cxx_std_23)
# Requests, results and schemas are JSON values, so nlohmann_json is part of the public API.
target_link_libraries(wabcut_command PUBLIC wabcut_model nlohmann_json::nlohmann_json)
# lottie.import (C.13) reads Lottie files through the interchange importers.
target_link_libraries(wabcut_command PRIVATE wabcut_interchange)
wabcut_target_defaults(wabcut_command)

# The MCP server (wabcut-cli mcp).
add_subdirectory(mcp)

# Regenerates the generated docs: wabcut_command_docgen docs/commands.md docs/mcp.md docs/extensions.md
add_executable(wabcut_command_docgen tools/docgen.cpp)
target_link_libraries(wabcut_command_docgen PRIVATE wabcut_command wabcut_mcp wabcut_ai_edit wabcut_script)
wabcut_target_defaults(wabcut_command_docgen)
