# Audio plugin hosting (A.6, docs/plugins.md; owner: Audio engine).
#
#   wabcut_plughost_common   the wire both sides share: Protocol.h (shared-memory layout), Ipc.h (processes, shared
#                            memory, events, base64)
#   wabcut-plughost          the helper process that loads CLAP and VST3 plugins, scans them and runs their editors
#   wabcut_plugins           the editor side: the plugin catalog (scanner, cache, blocklist, effect descriptors), the
#                            host processes and instances, and the plugin effect processors for the effect chains
#
# Windows only for now (Win32 processes, shared memory and windows); AU and LV2 are a macOS / Linux follow-up.

find_package(clap CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# The VST3 SDK port installs headers and static libraries without a CMake package.
find_path(WABCUT_VST3SDK_INCLUDE_DIR pluginterfaces/vst/ivstcomponent.h PATH_SUFFIXES vst3sdk REQUIRED)
set(_vst3_libs)
foreach(_lib sdk_hosting sdk sdk_common base pluginterfaces)
    find_library(WABCUT_VST3SDK_${_lib} NAMES ${_lib} PATH_SUFFIXES vst3sdk REQUIRED)
    list(APPEND _vst3_libs ${WABCUT_VST3SDK_${_lib}})
endforeach()
add_library(wabcut_vst3sdk INTERFACE)
target_include_directories(wabcut_vst3sdk SYSTEM INTERFACE ${WABCUT_VST3SDK_INCLUDE_DIR})
target_link_libraries(wabcut_vst3sdk INTERFACE ${_vst3_libs})

add_library(wabcut_plughost_common STATIC
    common/include/wabcut/plughost/Ipc.h
    common/include/wabcut/plughost/Protocol.h
    common/src/Ipc.cpp
)
target_include_directories(wabcut_plughost_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common/include)
wabcut_target_defaults(wabcut_plughost_common)

add_executable(wabcut-plughost
    helper/ClapHost.cpp
    helper/Editor.cpp
    helper/Editor.h
    helper/HostedPlugin.h
    helper/main.cpp
    helper/Vst3Host.cpp
)
target_link_libraries(wabcut-plughost PRIVATE wabcut_plughost_common clap wabcut_vst3sdk nlohmann_json::nlohmann_json avrt ole32)
wabcut_target_defaults(wabcut-plughost)

add_library(wabcut_plugins STATIC
    client/include/wabcut/plughost/PluginCatalog.h
    client/include/wabcut/plughost/PluginEffects.h
    client/include/wabcut/plughost/PluginHost.h
    client/include/wabcut/plughost/PluginSystem.h
    client/src/PluginCatalog.cpp
    client/src/PluginEffects.cpp
    client/src/PluginHost.cpp
    client/src/PluginSystem.cpp
)
target_include_directories(wabcut_plugins PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/client/include)
target_link_libraries(wabcut_plugins PUBLIC wabcut_plughost_common wabcut_audio wabcut_audio_edit wabcut_model
                                      PRIVATE nlohmann_json::nlohmann_json)
# Hosts run the helper from next to themselves: build it with the library.
add_dependencies(wabcut_plugins wabcut-plughost)
wabcut_target_defaults(wabcut_plugins)
