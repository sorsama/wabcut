# wabcut_keymap — pure C++ (no Qt): command registry, panel-scoped per-OS keymaps, J/K/L gesture, .kys import.
# Owner: Keymap. Public headers under include/wabcut/keymap/, default tables under keymaps/ (compiled in).
find_package(nlohmann_json CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

# The JSON tables are the editable source of truth; a build step embeds them so the engine needs no loose files.
set(WABCUT_KEYMAP_WINDOWS_TABLE ${CMAKE_CURRENT_SOURCE_DIR}/keymaps/premiere-windows.json)
set(WABCUT_KEYMAP_MACOS_TABLE ${CMAKE_CURRENT_SOURCE_DIR}/keymaps/premiere-macos.json)
set(embedded_keymaps ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedKeymaps.cpp)
add_custom_command(
    OUTPUT ${embedded_keymaps}
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${embedded_keymaps}
        -DWINDOWS_TABLE=${WABCUT_KEYMAP_WINDOWS_TABLE}
        -DMACOS_TABLE=${WABCUT_KEYMAP_MACOS_TABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedKeymaps.cmake
    DEPENDS ${WABCUT_KEYMAP_WINDOWS_TABLE} ${WABCUT_KEYMAP_MACOS_TABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedKeymaps.cmake
    COMMENT "Embedding Premiere keymap tables"
    VERBATIM)

add_library(wabcut_keymap STATIC
    src/CommandRegistry.cpp
    src/DefaultKeymaps.cpp
    src/Key.cpp
    src/KeyDispatcher.cpp
    src/KeymapManager.cpp
    src/KysImport.cpp
    src/Platform.cpp
    src/Scope.cpp
    src/TransportGesture.cpp
    ${embedded_keymaps})
target_include_directories(wabcut_keymap
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(wabcut_keymap PUBLIC cxx_std_23)
target_link_libraries(wabcut_keymap
    PUBLIC wabcut_model
    PRIVATE nlohmann_json::nlohmann_json pugixml::pugixml)
wabcut_target_defaults(wabcut_keymap)
