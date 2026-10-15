# wabcut_mcp — pure C++ (no Qt, no network): a Model Context Protocol server (JSON-RPC 2.0 over a line
# transport) on the command catalog, behind `wabcut-cli mcp`. See docs/mcp.md.
find_package(Threads REQUIRED)

add_library(wabcut_mcp STATIC
    src/McpCli.cpp
    src/McpDocs.cpp
    src/McpServer.cpp
    src/McpTransport.cpp
)
target_include_directories(wabcut_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_mcp PUBLIC wabcut_command PRIVATE Threads::Threads)
wabcut_target_defaults(wabcut_mcp)
