# Extensions and scripting (C.12, docs/extensions.md).
#
#   wabcut_script      pure C++ (core builds too): manifests, extension folders, permissions and trust, storage,
#                      events, the scripthost protocol, and the editor's side of a running extension (ExtensionHost,
#                      ExtensionManager) over the typed command engine
#   wabcut-scripthost  the helper process that runs one extension's JavaScript in a QJSEngine (full builds: Qt Qml)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_script STATIC
    include/wabcut/script/Docs.h
    include/wabcut/script/Events.h
    include/wabcut/script/Extension.h
    include/wabcut/script/Headless.h
    include/wabcut/script/Host.h
    include/wabcut/script/Manager.h
    include/wabcut/script/Manifest.h
    include/wabcut/script/Permissions.h
    include/wabcut/script/Protocol.h
    include/wabcut/script/Storage.h
    src/Docs.cpp
    src/Events.cpp
    src/Extension.cpp
    src/Headless.cpp
    src/Host.cpp
    src/Manager.cpp
    src/Manifest.cpp
    src/Permissions.cpp
    src/Protocol.cpp
    src/Storage.cpp
)
target_include_directories(wabcut_script PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wabcut_script PUBLIC cxx_std_23)
# wabcut_ai: ChildProcess (the helper's process and job object) and Sha256 (content hashes).
target_link_libraries(wabcut_script PUBLIC wabcut_command wabcut_model nlohmann_json::nlohmann_json PRIVATE wabcut_ai)
wabcut_target_defaults(wabcut_script)

if(TARGET Qt6::Core)
    find_package(Qt6 REQUIRED COMPONENTS Qml)
    add_executable(wabcut-scripthost host/main.cpp host/ScriptHost.cpp host/ScriptHost.h)
    qt_add_resources(wabcut-scripthost "wabcut_scripthost_prelude" PREFIX "/wabcut" BASE host FILES host/prelude.js)
    target_link_libraries(wabcut-scripthost PRIVATE wabcut_script Qt6::Core Qt6::Qml)
    wabcut_target_defaults(wabcut-scripthost)
endif()
