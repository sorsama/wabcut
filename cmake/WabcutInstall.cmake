# Install tree, third-party notices, version resources, CPack packages (portable ZIP, per-user MSI) and the install
# smoke tests. Included by the root CMakeLists.txt for full (non-core) builds, after every module directory has been
# added.
#
#   cmake --install build/win-msvc --prefix <dir>     self-contained tree (see docs/building.md)
#   cpack --config build/win-msvc/CPackConfig.cmake   portable ZIP and MSI of the same tree (docs/release.md)
#
# Windows layout: executables (wabcut, wabcut-cli and the AI helpers wabcut-infer / wabcut-embed with the modules they
# load at run time), Qt libraries (windeployqt), vcpkg runtime DLLs and the MSVC runtime in bin/; the in-app docs in
# bin/docs/; Qt plugins in plugins/ and QML modules in qml/ (found through bin/qt.conf); LICENSE.txt,
# THIRD-PARTY-NOTICES.txt and licenses/ at the top. Linux/macOS follow GNUInstallDirs (experimental).

include_guard(GLOBAL)
include(GNUInstallDirs)

# ---- Version information -------------------------------------------------------------------------------------
# WABCUT_BUILD_NUMBER is the fourth FILEVERSION field (CI passes its run number); the commit, read at configure time,
# goes into ProductVersion and the crash reports' diagnostic summary.
set(WABCUT_BUILD_NUMBER 0 CACHE STRING "Build number: the fourth Windows FILEVERSION field (CI sets it)")
if(NOT DEFINED WABCUT_BUILD_COMMIT)
    find_package(Git QUIET)
    set(WABCUT_BUILD_COMMIT "")
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE WABCUT_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
endif()
if(WABCUT_BUILD_COMMIT)
    set(WABCUT_PRODUCT_VERSION "${PROJECT_VERSION} (${WABCUT_BUILD_COMMIT})")
else()
    set(WABCUT_PRODUCT_VERSION "${PROJECT_VERSION}")
endif()

# wabcut_add_version_resource(<target> <description>): a VERSIONINFO resource (FileVersion, ProductName, ...) for a
# Windows executable, from cmake/packaging/version.rc.in. Does nothing elsewhere or for a target that does not exist.
function(wabcut_add_version_resource target description)
    if(NOT WIN32 OR NOT TARGET ${target})
        return()
    endif()
    get_target_property(output_name ${target} OUTPUT_NAME)
    if(NOT output_name)
        set(output_name ${target})
    endif()
    set(WABCUT_RC_FILENAME "${output_name}.exe")
    set(WABCUT_RC_INTERNAL_NAME "${output_name}")
    set(WABCUT_RC_DESCRIPTION "${description}")
    set(WABCUT_RC_VERSION_DOTS "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${WABCUT_BUILD_NUMBER}")
    string(REPLACE "." "," WABCUT_RC_VERSION_COMMAS "${WABCUT_RC_VERSION_DOTS}")
    set(WABCUT_RC_PRODUCT_VERSION "${WABCUT_PRODUCT_VERSION}")
    set(WABCUT_RC_FILEFLAGS 0)
    if(PROJECT_VERSION_MAJOR EQUAL 0)
        set(WABCUT_RC_FILEFLAGS VS_FF_PRERELEASE)
    endif()
    set(rc "${CMAKE_BINARY_DIR}/packaging/version/${target}.rc")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/packaging/version.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
endfunction()

# The app's crash reports and diagnostic summary carry the version with its commit.
if(TARGET wabcut)
    target_compile_definitions(wabcut PRIVATE WABCUT_PRODUCT_VERSION="${WABCUT_PRODUCT_VERSION}")
endif()

wabcut_add_version_resource(wabcut "Wabcut video editor")
wabcut_add_version_resource(wabcut-cli "Wabcut command-line tool")
wabcut_add_version_resource(wabcut-infer "Wabcut local transcription helper (whisper.cpp)")
wabcut_add_version_resource(wabcut-embed "Wabcut smart search and subject detection helper (ONNX Runtime)")

# macOS with the app: the bundle is the whole distribution (the DMG holds wabcut.app and nothing else), so every
# executable, the in-app docs, the notices and the licenses go inside it. Without the app (a CLI-only build) the tree
# follows GNUInstallDirs as on Linux.
set(WABCUT_MACOS_BUNDLE OFF)
if(APPLE AND TARGET wabcut)
    set(WABCUT_MACOS_BUNDLE ON)
endif()

# Already normalized (no "./"), so CMake 3.31+ (policy CMP0177) installs them unchanged.
if(WIN32)
    set(WABCUT_INSTALL_DOCDIR ".")
    set(WABCUT_INSTALL_LICENSEDIR "licenses")
elseif(WABCUT_MACOS_BUNDLE)
    set(WABCUT_INSTALL_DOCDIR "wabcut.app/Contents/Resources")
    set(WABCUT_INSTALL_LICENSEDIR "wabcut.app/Contents/Resources/licenses")
else()
    set(WABCUT_INSTALL_DOCDIR "${CMAKE_INSTALL_DOCDIR}")
    set(WABCUT_INSTALL_LICENSEDIR "${CMAKE_INSTALL_DOCDIR}/licenses")
endif()

set(wabcut_vcpkg_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

set(wabcut_install_targets wabcut-cli)
if(TARGET wabcut)
    list(APPEND wabcut_install_targets wabcut)
endif()
# The out-of-process AI helpers. The app and the CLI start them from their own directory (currentExecutableDir()),
# so they go to bin/ as well.
foreach(helper wabcut-infer wabcut-embed)
    if(TARGET ${helper})
        list(APPEND wabcut_install_targets ${helper})
    endif()
endforeach()
if(TARGET wabcut-plughost)
    list(APPEND wabcut_install_targets wabcut-plughost) # A.6: the audio plugin helper, next to the app and the CLI
endif()
if(TARGET wabcut-scripthost)
    list(APPEND wabcut_install_targets wabcut-scripthost) # C.12: runs extensions (Qt Qml), next to the app and the CLI
endif()

# ---- Executables and their non-Qt runtime libraries -------------------------------------------------------
if(MSVC)
    # dumpbin (next to link.exe) reads the DLL imports; recording its full path lets `cmake --install`, CPack and
    # the smoke test run outside the developer shell.
    get_filename_component(wabcut_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(WABCUT_DUMPBIN dumpbin HINTS "${wabcut_linker_dir}" REQUIRED)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL dumpbin)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${WABCUT_DUMPBIN}")
endif()
if(WIN32)
    install(TARGETS ${wabcut_install_targets}
        RUNTIME_DEPENDENCY_SET wabcut_runtime
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # vcpkg DLLs (FFmpeg, x264, x265, dav1d, libvpl, pugixml, ...), found transitively. Qt is deployed by
    # windeployqt below, the MSVC runtime by InstallRequiredSystemLibraries, and the OS libraries stay out.
    install(RUNTIME_DEPENDENCY_SET wabcut_runtime
        DIRECTORIES "${wabcut_vcpkg_prefix}/bin"
        PRE_EXCLUDE_REGEXES
            "^[Aa][Pp][Ii]-[Mm][Ss]-" "^[Ee][Xx][Tt]-[Mm][Ss]-" "^[Qq]t6"
            "^([Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee]|[Mm][Ss][Vv][Cc][Pp]|[Cc][Oo][Nn][Cc][Rr][Tt])[0-9_]*\\.dll$"
        POST_EXCLUDE_REGEXES ".*[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
        DESTINATION ${CMAKE_INSTALL_BINDIR})

    # App-local MSVC runtime, so the tree runs without the VC++ redistributable. Windows 10+ ships the UCRT.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
    include(InstallRequiredSystemLibraries)
elseif(WABCUT_MACOS_BUNDLE)
    # macOS (preview builds from CI, docs/release.md "Platform status"): the app starts its helpers from its own folder
    # (currentExecutableDir()), so the CLI and the helpers sit next to it in wabcut.app/Contents/MacOS; macdeployqt
    # (below) puts the Qt frameworks and the other shared libraries in Contents/Frameworks.
    foreach(target IN LISTS wabcut_install_targets)
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
    endforeach()
    set(wabcut_bundle_tools ${wabcut_install_targets})
    list(REMOVE_ITEM wabcut_bundle_tools wabcut)
    install(TARGETS wabcut BUNDLE DESTINATION .)
    install(TARGETS ${wabcut_bundle_tools} RUNTIME DESTINATION "wabcut.app/Contents/MacOS")
else()
    # Linux and a CLI-only macOS build (preview builds from CI, docs/release.md "Platform status"): shared libraries
    # next to the executables or in ../lib.
    foreach(target IN LISTS wabcut_install_targets)
        if(APPLE)
            set_target_properties(${target} PROPERTIES INSTALL_RPATH "@executable_path;@executable_path/../lib")
        else()
            set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
        endif()
    endforeach()
    install(TARGETS ${wabcut_install_targets} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
# Where the executables are: the app starts the helpers from its own folder, and the helpers load their modules (below)
# from theirs.
if(WABCUT_MACOS_BUNDLE)
    set(wabcut_helper_dirs "wabcut.app/Contents/MacOS")
    set(wabcut_library_dir "wabcut.app/Contents/Frameworks")
else()
    set(wabcut_helper_dirs ${CMAKE_INSTALL_BINDIR})
    set(wabcut_library_dir ${CMAKE_INSTALL_BINDIR})
endif()

# Modules the helpers load at run time (nothing imports them, so the dependency set above cannot see them). The same
# files src/ai_local copies next to the helpers in the build tree.
if(TARGET wabcut-infer)
    # ggml's compute backends (GGML_BACKEND_DL): one CPU module per instruction-set level, the Vulkan GPU backend and
    # the Vulkan loader it imports. whisper.dll, ggml.dll and ggml-base.dll are imports and come with the set above.
    file(GLOB wabcut_ggml_modules
        "${wabcut_vcpkg_prefix}/bin/ggml-cpu-*.dll" "${wabcut_vcpkg_prefix}/bin/ggml-vulkan.dll"
        "${wabcut_vcpkg_prefix}/bin/vulkan-1.dll"
        "${wabcut_vcpkg_prefix}/lib/libggml-cpu-*.so" "${wabcut_vcpkg_prefix}/lib/libggml-vulkan.so"
        "${wabcut_vcpkg_prefix}/lib/libggml-cpu-*.dylib" "${wabcut_vcpkg_prefix}/lib/libggml-metal.dylib"
        "${wabcut_vcpkg_prefix}/lib/libggml-blas.dylib")
    foreach(dir IN LISTS wabcut_helper_dirs)
        if(wabcut_ggml_modules)
            install(FILES ${wabcut_ggml_modules} DESTINATION ${dir})
        endif()
    endforeach()
    install(FILES "${CMAKE_SOURCE_DIR}/src/ai_local/THIRD_PARTY_NOTICES.md"
        DESTINATION ${WABCUT_INSTALL_LICENSEDIR} RENAME ai-local-helper.md)
endif()
if(TARGET wabcut-embed)
    # onnxruntime.dll is an import (above); onnxruntime_providers_shared.dll is loaded by it at run time.
    file(GLOB wabcut_ort_modules "${wabcut_vcpkg_prefix}/bin/onnxruntime_providers_shared.dll"
                                 "${wabcut_vcpkg_prefix}/lib/libonnxruntime_providers_shared.*")
    if(NOT WIN32)
        # The prebuilt ONNX Runtime is a shared library on every platform (vcpkg's Linux/macOS triplets are static).
        file(GLOB wabcut_ort_libraries "${wabcut_vcpkg_prefix}/lib/libonnxruntime.so*" "${wabcut_vcpkg_prefix}/lib/libonnxruntime*.dylib")
        list(APPEND wabcut_ort_modules ${wabcut_ort_libraries})
    endif()
    if(wabcut_ort_modules)
        install(FILES ${wabcut_ort_modules} DESTINATION ${wabcut_library_dir})
    endif()
endif()

# The documents the Home screen links ("Learn", "What's new"): HomeBridge looks for docs/ next to the executable, and
# in a macOS bundle in Contents/Resources/docs.
if(WABCUT_MACOS_BUNDLE)
    set(wabcut_docs_dir "wabcut.app/Contents/Resources/docs")
else()
    set(wabcut_docs_dir "${CMAKE_INSTALL_BINDIR}/docs")
endif()
install(FILES "${CMAKE_SOURCE_DIR}/docs/interchange.md" "${CMAKE_SOURCE_DIR}/docs/keymap.md"
              "${CMAKE_SOURCE_DIR}/docs/roadmap.md" "${CMAKE_SOURCE_DIR}/docs/release.md"
              "${CMAKE_SOURCE_DIR}/docs/getting-started.md" "${CMAKE_SOURCE_DIR}/docs/release-notes-0.1.md"
    DESTINATION ${wabcut_docs_dir})

# Linux desktop integration: a launcher entry and the app icon in the hicolor theme (the AppImage uses them too).
if(UNIX AND NOT APPLE AND TARGET wabcut)
    install(FILES "${CMAKE_CURRENT_LIST_DIR}/packaging/linux/wabcut.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES "${CMAKE_SOURCE_DIR}/src/app/resources/brand/icons/wabcut-${size}.png"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME wabcut.png)
    endforeach()
endif()

# ---- Qt runtime (libraries, plugins, QML modules) -----------------------------------------------------------
if(TARGET wabcut AND WIN32)
    # Same options as the build-tree POST_BUILD windeployqt in src/app; --qmldir lets it find the QML imports of the
    # statically linked Wabcut module. The compiler runtime comes from InstallRequiredSystemLibraries.
    qt_generate_deploy_app_script(
        TARGET wabcut
        OUTPUT_SCRIPT wabcut_deploy_script
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR
        DEPLOY_TOOL_OPTIONS --qmldir "${CMAKE_SOURCE_DIR}/src/app/qml" --no-compiler-runtime)
elseif(TARGET wabcut)
    # Linux and macOS: the QML modules the app imports (Qt's qmlimportscanner over src/app, the scan Qt already runs
    # for the wabcut target), then the Qt libraries and plugins the app, those QML plugins, the CLI and the helpers
    # need. Linux: lib/, plugins/, qml/ and bin/qt.conf (file(GET_RUNTIME_DEPENDENCIES); the OS's own libraries stay
    # out). macOS: macdeployqt fills Contents/Frameworks, PlugIns and Resources/qml and points every executable in
    # Contents/MacOS at them. ONNX Runtime is installed above, next to the helper that loads it.
    set(wabcut_deploy_tools "")
    foreach(target IN LISTS wabcut_install_targets)
        if(NOT target STREQUAL "wabcut")
            list(APPEND wabcut_deploy_tools "${wabcut_helper_dirs}/$<TARGET_FILE_NAME:${target}>")
        endif()
    endforeach()
    if(WABCUT_MACOS_BUNDLE)
        set(wabcut_deploy_app "$<TARGET_FILE_NAME:wabcut>.app")
    else()
        set(wabcut_deploy_app "$<TARGET_FILE:wabcut>")
    endif()
    qt_generate_deploy_script(
        TARGET wabcut
        OUTPUT_SCRIPT wabcut_deploy_script
        CONTENT "
qt_deploy_qml_imports(TARGET wabcut PLUGINS_FOUND plugins_found)
qt_deploy_runtime_dependencies(
    EXECUTABLE \"${wabcut_deploy_app}\"
    ADDITIONAL_EXECUTABLES ${wabcut_deploy_tools}
    ADDITIONAL_MODULES \${plugins_found}
    PRE_EXCLUDE_REGEXES onnxruntime
    GENERATE_QT_CONF
    NO_TRANSLATIONS)
")
endif()
if(TARGET wabcut)
    if(WIN32)
        # windeployqt copies the D3D and DirectX shader compilers (d3dcompiler_47.dll, dxcompiler.dll, dxil.dll)
        # from %WindowsSdkDir%\Redist\D3D, and silently skips them when that variable is not set. Record the SDK of
        # the configure (developer) shell, so the tree does not depend on the shell `cmake --install` or cpack run
        # in. Defaults to the SDK that holds the resource compiler.
        file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" wabcut_sdk_default)
        string(REGEX REPLACE "/+$" "" wabcut_sdk_default "${wabcut_sdk_default}")
        if(NOT wabcut_sdk_default AND CMAKE_RC_COMPILER MATCHES "^(.*)/bin/[^/]+/[^/]+/rc\\.exe$")
            set(wabcut_sdk_default "${CMAKE_MATCH_1}")
        endif()
        set(WABCUT_WINDOWS_SDK_DIR "${wabcut_sdk_default}" CACHE PATH "Windows SDK root, for windeployqt's D3D redist")
        if(NOT EXISTS "${WABCUT_WINDOWS_SDK_DIR}/Redist/D3D")
            message(WARNING "No Windows SDK D3D redist under WABCUT_WINDOWS_SDK_DIR ('${WABCUT_WINDOWS_SDK_DIR}'): "
                            "the install tree will lack d3dcompiler_47.dll, dxcompiler.dll and dxil.dll.")
        endif()
        file(TO_NATIVE_PATH "${WABCUT_WINDOWS_SDK_DIR}/" wabcut_sdk_native)
        string(REPLACE "\\" "\\\\" wabcut_sdk_native "${wabcut_sdk_native}")
        install(CODE "set(ENV{WindowsSdkDir} \"${wabcut_sdk_native}\")")
    endif()
    install(SCRIPT ${wabcut_deploy_script})
endif()

# ---- Licenses and third-party notices -----------------------------------------------------------------------
# Version of a vcpkg port from its SPDX document ("3.12.0#2" -> "3.12.0"), or "unknown".
function(_wabcut_vcpkg_port_version port out)
    set(version "unknown")
    set(spdx "${wabcut_vcpkg_prefix}/share/${port}/vcpkg.spdx.json")
    if(EXISTS "${spdx}")
        file(READ "${spdx}" json)
        string(JSON version ERROR_VARIABLE json_error GET "${json}" packages 0 versionInfo)
        if(json_error)
            set(version "unknown")
        endif()
        string(REGEX REPLACE "#[0-9]+$" "" version "${version}")
    endif()
    set(${out} "${version}" PARENT_SCOPE)
endfunction()

set(WABCUT_VERSION ${PROJECT_VERSION})
set(WABCUT_QT_VERSION "${Qt6_VERSION}")
set(wabcut_license_ports ffmpeg x264 x265 dav1d libvpl amd-amf ffnvcodec miniaudio nlohmann-json pugixml zlib spdlog
                         fmt catch2 qtkeychain libebur128 rnnoise sqlite3 hnswlib onnxruntime-prebuilt whisper-cpp ggml
                         vulkan-loader clap-cleveraudio vst3sdk svt-av1 fastfeat libvpx opus)
foreach(port IN LISTS wabcut_license_ports)
    _wabcut_vcpkg_port_version(${port} version)
    string(MAKE_C_IDENTIFIER "V_${port}" var)
    set(${var} "${version}")
    if(EXISTS "${wabcut_vcpkg_prefix}/share/${port}/copyright")
        install(FILES "${wabcut_vcpkg_prefix}/share/${port}/copyright"
            DESTINATION ${WABCUT_INSTALL_LICENSEDIR} RENAME ${port}.txt)
    endif()
endforeach()

# FFmpeg's configure line and license, as reported by the ffmpeg tool of the same vcpkg build.
set(FFMPEG_CONFIGURATION "        (not available: the ffmpeg tool was not found in ${wabcut_vcpkg_prefix}/tools/ffmpeg)")
set(FFMPEG_LICENSE "GNU General Public License version 2 or later")
find_program(WABCUT_FFMPEG_TOOL NAMES ffmpeg HINTS "${wabcut_vcpkg_prefix}/tools/ffmpeg" NO_DEFAULT_PATH)
if(WABCUT_FFMPEG_TOOL)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_prepend:${wabcut_vcpkg_prefix}/bin" --
                "${WABCUT_FFMPEG_TOOL}" -hide_banner -buildconf
        OUTPUT_VARIABLE buildconf ERROR_VARIABLE buildconf_err RESULT_VARIABLE buildconf_rc)
    if(buildconf_rc EQUAL 0)
        string(REGEX MATCHALL "--[^\r\n]+" flags "${buildconf}")
        set(FFMPEG_CONFIGURATION "")
        foreach(flag IN LISTS flags)
            string(STRIP "${flag}" flag)
            if(flag MATCHES "^--prefix=")
                set(flag "--prefix=<vcpkg package directory>")
            endif()
            string(APPEND FFMPEG_CONFIGURATION "        ${flag}\n")
        endforeach()
        string(REGEX REPLACE "\n$" "" FFMPEG_CONFIGURATION "${FFMPEG_CONFIGURATION}")
        if(buildconf MATCHES "--enable-version3")
            set(FFMPEG_LICENSE "GNU General Public License version 3 or later")
        endif()
        if(NOT buildconf MATCHES "--enable-gpl")
            set(FFMPEG_LICENSE "GNU Lesser General Public License version 2.1 or later")
        endif()
    endif()
endif()

configure_file(${CMAKE_CURRENT_LIST_DIR}/THIRD-PARTY-NOTICES.txt.in ${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt @ONLY)
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION ${WABCUT_INSTALL_DOCDIR} RENAME LICENSE.txt)
install(FILES "${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" DESTINATION ${WABCUT_INSTALL_DOCDIR})
install(FILES "${CMAKE_SOURCE_DIR}/src/app/resources/icons/LICENSE"
    DESTINATION ${WABCUT_INSTALL_LICENSEDIR} RENAME boxicons.txt)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/licenses/LGPL-3.0.txt"
    DESTINATION ${WABCUT_INSTALL_LICENSEDIR} RENAME qt-LGPL-3.0.txt)
# The bundled fonts (design language v3, and v2's faces kept for content): SIL OFL 1.1.
install(FILES "${CMAKE_SOURCE_DIR}/src/app/resources/fonts/OFL-Manrope.txt"
              "${CMAKE_SOURCE_DIR}/src/app/resources/fonts/OFL-JetBrains-Mono.txt"
              "${CMAKE_SOURCE_DIR}/src/app/resources/fonts/OFL-IBM-Plex.txt"
              "${CMAKE_SOURCE_DIR}/src/app/resources/fonts/OFL-Bai-Jamjuree.txt"

    DESTINATION ${WABCUT_INSTALL_LICENSEDIR})

# Last: record which runtime files the tree actually contains, so the notices can be audited against it.
set(wabcut_listing_code [[
    set(_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
    file(GLOB_RECURSE _libraries RELATIVE "${_root}" "${_root}/*.dll" "${_root}/*.so*" "${_root}/*.dylib")
    list(SORT _libraries)
    list(JOIN _libraries "\n    " _listing)
    file(APPEND "${_root}/@WABCUT_INSTALL_DOCDIR@/THIRD-PARTY-NOTICES.txt"
        "\nRuntime libraries in this distribution:\n    ${_listing}\n")
]])
string(CONFIGURE "${wabcut_listing_code}" wabcut_listing_code @ONLY) # fills in @WABCUT_INSTALL_DOCDIR@ only
install(CODE "${wabcut_listing_code}")

# ---- CPack: portable ZIP and per-user MSI (cmake/packaging/WabcutPackaging.cmake) ---------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/packaging/WabcutPackaging.cmake)

# ---- Smoke tests -----------------------------------------------------------------------------------------------
if(WABCUT_BUILD_TESTS)
    set(wabcut_smoke_helpers "")
    foreach(helper wabcut-infer wabcut-embed)
        if(TARGET ${helper})
            list(APPEND wabcut_smoke_helpers ${helper})
        endif()
    endforeach()
    # install.smoke: install into a scratch prefix, run the CLI and the helpers with a minimal PATH, check every DLL
    # comes from the tree, render a tiny project headlessly.
    add_test(NAME install.smoke
        COMMAND "${CMAKE_COMMAND}"
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DPREFIX=${CMAKE_BINARY_DIR}/install-smoke
            -DCONFIG=$<CONFIG>
            -DVERSION=${PROJECT_VERSION}
            -DEXPECT_APP=$<TARGET_EXISTS:wabcut>
            "-DEXPECT_HELPERS=${wabcut_smoke_helpers}"
            -DSMOKE_PROJECT=${CMAKE_CURRENT_LIST_DIR}/packaging/smoke/tiny.wabcut
            -DDUMPBIN=${WABCUT_DUMPBIN}
            -P ${CMAKE_CURRENT_LIST_DIR}/InstallSmokeTest.cmake)
    set_tests_properties(install.smoke PROPERTIES LABELS install TIMEOUT 900 RESOURCE_LOCK wabcut_install)
    # install.package (Windows): build the portable ZIP and the MSI with cpack, report their sizes and contents, run
    # the unpacked ZIP, and install the MSI per user into a temporary folder, run it, uninstall it and check that
    # only program files went (cmake/packaging/PackageSmokeTest.ps1).
    if(WIN32)
        find_program(WABCUT_POWERSHELL NAMES pwsh powershell REQUIRED)
        add_test(NAME install.package
            COMMAND "${WABCUT_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_LIST_DIR}/packaging/PackageSmokeTest.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}"
                -CPack "${CMAKE_CPACK_COMMAND}"
                -Version "${PROJECT_VERSION}"
                -PackageBaseName "${WABCUT_PACKAGE_BASENAME}"
                -UpgradeCode "${WABCUT_WIX_UPGRADE_GUID}"
                -SmokeProject "${CMAKE_CURRENT_LIST_DIR}/packaging/smoke/tiny.wabcut"
                -Generators "${WABCUT_PACKAGE_GENERATORS}")
        set_tests_properties(install.package PROPERTIES LABELS install TIMEOUT 1800 RESOURCE_LOCK wabcut_install
            SKIP_RETURN_CODE 77)
    endif()
endif()
