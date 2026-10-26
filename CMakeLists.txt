cmake_minimum_required(VERSION 3.28)

# Overlay ports (cmake/ports) for vcpkg, set before project(): rnnoise (a CMake build; the stock port does not support
# Windows), amd-amf 1.5.3#1 (vcpkg's fix of 2026-10-20: AMD replaced the release's header archive, so the baseline's
# hash no longer matches; drop it with the next baseline) and the optional AI ports below.
list(APPEND VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ports")

# Local AI (docs/ai.md): whisper.cpp + ggml from the overlay ports in cmake/ports, built into the out-of-process
# helper wabcut-infer. Decided before project(), where the vcpkg toolchain installs the manifest. ON by default in
# full Windows builds: the chain (ggml with all CPU variants and the Vulkan shaders, shaderc, glslang, SPIRV-Tools,
# the Vulkan loader) restores from a vcpkg binary cache in seconds, and a cold build of it took 19.6 min and 2.1 GB at
# -j 1 (docs/building.md). OFF by default elsewhere (not measured yet) and in core builds; -DWABCUT_WITH_LOCAL_AI=OFF
# keeps the dependency set without it. A build dir configured before keeps its cached value.
if(CMAKE_HOST_WIN32 AND NOT WABCUT_CORE_ONLY)
    set(_wabcut_local_ai_default ON)
else()
    set(_wabcut_local_ai_default OFF)
endif()
option(WABCUT_WITH_LOCAL_AI "Build local AI (whisper.cpp transcription helper wabcut-infer)" ${_wabcut_local_ai_default})
if(WABCUT_WITH_LOCAL_AI)
    list(APPEND VCPKG_MANIFEST_FEATURES "ai-local")
    list(APPEND VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ports")
endif()
# Smart search (docs/ai.md, "Smart search"): the wabcut-embed helper runs SigLIP 2 on ONNX Runtime 1.24.4, taken from
# Microsoft's official release binaries by the overlay port cmake/ports/onnxruntime-prebuilt (nothing is compiled; a
# 12.5 MB download on Windows, 8 MB on Linux, 31 MB on macOS). ON wherever Microsoft publishes binaries (not Intel
# Macs). OFF: transcript search still works, visual search reports that it is not built.
cmake_host_system_information(RESULT wabcut_host_arch QUERY OS_PLATFORM) # before project(): the host's
if(CMAKE_HOST_APPLE AND NOT wabcut_host_arch MATCHES "arm64|aarch64")
    set(wabcut_onnx_default OFF)
else()
    set(wabcut_onnx_default ON)
endif()
option(WABCUT_WITH_ONNX "Build the smart-search embedding helper wabcut-embed (prebuilt ONNX Runtime)" ${wabcut_onnx_default})
if(WABCUT_WITH_ONNX AND NOT WABCUT_CORE_ONLY)
    list(APPEND VCPKG_MANIFEST_FEATURES "ai-onnx")
endif()
# Cloud AI key storage (docs/ai.md, "Keys"): provider keys in the OS keychain through QtKeychain 0.17, an overlay port
# built against the app's Qt (tools/dev-env.ps1 passes WABCUT_QT_DIR into the vcpkg build). OFF keeps keys for the
# session only (and in WABCUT_<PROVIDER>_API_KEY environment variables); the cloud adapters are built either way.
option(WABCUT_WITH_CLOUD_AI "Store AI provider keys in the OS keychain (QtKeychain)" ON)
if(WABCUT_WITH_CLOUD_AI AND NOT WABCUT_CORE_ONLY)
    list(APPEND VCPKG_MANIFEST_FEATURES "ai-cloud")
    list(APPEND VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ports")
    list(REMOVE_DUPLICATES VCPKG_OVERLAY_PORTS)
endif()

# Audio plugin hosting (A.6, docs/plugins.md): CLAP and VST3 effects run in the out-of-process helper
# wabcut-plughost (src/plughost), so a crashing plugin never takes the editor down. The SDKs come from the vcpkg
# feature "audio-plugins" (the clap headers and the VST3 SDK, both MIT). ON by default in full Windows builds (the
# helper's process, shared memory and windowing code is Win32 for now; AU and LV2 are a macOS/Linux follow-up).
if(CMAKE_HOST_WIN32 AND NOT WABCUT_CORE_ONLY)
    set(_wabcut_plugins_default ON)
else()
    set(_wabcut_plugins_default OFF)
endif()
option(WABCUT_WITH_AUDIO_PLUGINS "Host CLAP and VST3 audio plugins (out-of-process helper wabcut-plughost)" ${_wabcut_plugins_default})
if(WABCUT_WITH_AUDIO_PLUGINS AND NOT WABCUT_CORE_ONLY)
    list(APPEND VCPKG_MANIFEST_FEATURES "audio-plugins")
endif()

project(Wabcut
    VERSION 0.5.0
    DESCRIPTION "Open-source cross-platform non-linear video editor"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(WABCUT_CORE_ONLY "Build only the pure C++ core (model, command, keymap, interchange, ai) and its tests: no Qt, FFmpeg or miniaudio" OFF)
if(WABCUT_CORE_ONLY AND WABCUT_WITH_LOCAL_AI)
    message(FATAL_ERROR "WABCUT_WITH_LOCAL_AI needs a full build (transcription decodes media): turn off WABCUT_CORE_ONLY")
endif()
option(WABCUT_BUILD_APP "Build the Qt Quick desktop application" ON)
option(WABCUT_BUILD_TESTS "Build unit and integration tests" ON)
# OFF by default so downstream packagers with newer compilers are not broken by new warnings; every preset
# turns it ON, and CI builds with it.
option(WABCUT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(WABCUT_FETCH_DEPENDENCIES "Download missing pure-C++ dependencies (nlohmann_json, pugixml, zlib, Catch2) with FetchContent" OFF)

# The Qt version Wabcut is developed and tested against (see docs/building.md).
set(WABCUT_TESTED_QT_VERSION 6.11.3)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(WabcutCompilerOptions)
wabcut_check_toolchain()
# WABCUT_SANITIZE / WABCUT_FUZZ (the win-msvc-asan preset): before the dependencies, so they are instrumented too.
include(WabcutSanitizers)

if(WABCUT_FETCH_DEPENDENCIES)
    include(WabcutDependencies)
endif()

if(WABCUT_CORE_ONLY)
    message(STATUS "Wabcut: core-only build (model, command, keymap, interchange, ai) without Qt, FFmpeg or miniaudio")
    set(WABCUT_BUILD_APP OFF)
else()
    # GuiPrivate is intentional: QRhi (GPU abstraction for the monitor/compositor) is a semi-public Qt API.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick QuickControls2 ShaderTools Svg)
    if(Qt6_VERSION VERSION_LESS 6.11)
        message(FATAL_ERROR "Wabcut needs Qt 6.11 or later (tested with ${WABCUT_TESTED_QT_VERSION}); found Qt "
                            "${Qt6_VERSION} in ${Qt6_DIR}. Point WABCUT_QT_DIR / CMAKE_PREFIX_PATH at a Qt "
                            "${WABCUT_TESTED_QT_VERSION} kit. See docs/building.md.")
    endif()
    qt_standard_project_setup(REQUIRES 6.11)
    # QRhi / QQuickRhiItem have no source or binary compatibility guarantee across Qt minor releases.
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.12)
        message(WARNING "Wabcut is tested with Qt ${WABCUT_TESTED_QT_VERSION}; Qt ${Qt6_VERSION} may break QRhi "
                        "code (private API).")
    endif()
    message(STATUS "Wabcut: Qt ${Qt6_VERSION} (tested with ${WABCUT_TESTED_QT_VERSION}) from ${Qt6_DIR}")
endif()

# Engine modules. Each module owns its directory; see docs/architecture.md for the contracts.
add_subdirectory(src/model)      # pure C++: time, timeline data model, edit engine, undo, project IO
add_subdirectory(src/command)    # pure C++: typed, validated edit-command protocol (AI agent, CLI, scripting, MCP)
add_subdirectory(src/keymap)     # pure C++: command registry, panel-scoped per-OS keymaps
add_subdirectory(src/interchange) # pure C++: one-click import from Premiere/Resolve/FCP (XML, OTIO, EDL, prproj)
add_subdirectory(src/ai)          # pure C++: AI interfaces, model registry, transcripts, analysis cache (docs/ai.md)
add_subdirectory(src/audio/edit)  # pure C++: dialogue-first audio commands (roles, loudness, ducking); core too
add_subdirectory(src/render/color) # pure C++: colour grade core - LUTs, grade math, CDL, scopes stats, Auto/Match, colour commands; core too
add_subdirectory(src/render/masks) # pure C++: region tracker and the mask / track / redaction commands (I.6); core too
add_subdirectory(src/render/motion) # pure C++: dense optical flow and flow interpolation (P.2); core too
add_subdirectory(src/render/reference) # pure C++: CPU reference renders (C.16), the GPU shaders' test oracles; core too
add_subdirectory(src/script)      # extensions (C.12): manifests, permissions, the editor's side in pure C++ (core too);
                                  # the wabcut-scripthost helper (QJSEngine) in full builds
if(NOT WABCUT_CORE_ONLY)
    add_subdirectory(src/media)      # FFmpeg: probe, decode (HW + SW fallback), frame cache, thumbnails, peaks
    add_subdirectory(src/audio)      # miniaudio: device, mixer, master clock
    add_subdirectory(src/render)     # QRhi compositor + shaders
    add_subdirectory(src/playback)   # playback controller: JKL, scheduling, A/V sync
    if(WABCUT_WITH_AUDIO_PLUGINS AND WIN32)
        add_subdirectory(src/plughost) # audio plugins (CLAP, VST3): the wabcut-plughost helper and the editor side
    endif()
    add_subdirectory(src/export)     # encoders/muxer, presets
    add_subdirectory(src/editor)     # editor services: import, project lifecycle, relink, playback session (I.1);
                                     # links audio, playback, export and media, so full builds only
    add_subdirectory(src/cli)        # wabcut-cli: headless probe/render used by tests
    if(WABCUT_WITH_LOCAL_AI OR WABCUT_WITH_ONNX)
        add_subdirectory(src/ai_local) # out-of-process helpers: wabcut-infer (whisper.cpp), wabcut-embed (ONNX)
    endif()

endif()

add_subdirectory(src/ai_net) # wire adapters also build in core; Qt transport only in full builds

if(WABCUT_BUILD_APP)
    add_subdirectory(src/app)
    # The QML module and the app compile qmlcachegen/moc output (see WabcutCompilerOptions.cmake).
    wabcut_allow_qt_generated_code(wabcut_ui wabcut_uiplugin wabcut)
endif()

if(WABCUT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install tree, third-party notices, CPack ZIP and the install smoke test (full builds only).
if(NOT WABCUT_CORE_ONLY)
    include(WabcutInstall)
endif()
