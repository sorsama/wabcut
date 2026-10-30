# CTest `install.smoke` (label: install), run as `cmake -P`:
#   1. `cmake --install` the build tree into PREFIX (a scratch directory inside the build tree),
#   2. with PATH reduced to the operating system's own directories (no Qt, vcpkg or build directories), run
#      `wabcut-cli --version`, start the AI helpers, and resolve every DLL the GUI executable, the helpers and the
#      modules they load need from the tree alone (on Linux: every library of the executables, the Qt libraries, the
#      platform plugins and the QML plugins from the tree or the operating system; the GUI is never launched),
#   3. render a tiny project headlessly with the installed CLI (a title added with `wabcut-cli cmd`, then
#      `wabcut-cli render` through the real renderer and encoder),
#   4. check the licenses and notices, and report the tree's size.
# Inputs: BUILD_DIR, PREFIX, VERSION, CONFIG (may be empty), EXPECT_APP (1 when the GUI app is built),
#         EXPECT_HELPERS (the helper executables built: wabcut-infer, wabcut-embed), SMOKE_PROJECT (the tiny project),
#         DUMPBIN (MSVC's dumpbin.exe, to read DLL imports on Windows).
# cmake/packaging/PackageSmokeTest.ps1 runs steps 2 and 3 again on the installed MSI and the unpacked portable ZIP.

cmake_minimum_required(VERSION 3.28...4.3)

foreach(var BUILD_DIR PREFIX VERSION)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "InstallSmokeTest.cmake: ${var} is not set")
    endif()
endforeach()

# ---- 1. Install ---------------------------------------------------------------------------------------------
file(REMOVE_RECURSE "${PREFIX}")
set(config_args "")
if(CONFIG)
    set(config_args --config "${CONFIG}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}" ${config_args}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${rc}):\n${out}\n${err}")
endif()

# ---- 2. Run from the tree with a minimal environment ----------------------------------------------------------
if(CMAKE_HOST_WIN32)
    file(TO_CMAKE_PATH "$ENV{SystemRoot}" system_root)
    set(ENV{PATH} "${system_root}/System32;${system_root}")
    set(exe_suffix ".exe")
else()
    set(ENV{PATH} "/usr/bin:/bin")
    unset(ENV{LD_LIBRARY_PATH})
    unset(ENV{DYLD_LIBRARY_PATH})
    set(exe_suffix "")
endif()
foreach(var QT_PLUGIN_PATH QML_IMPORT_PATH QML2_IMPORT_PATH QT_QPA_PLATFORM_PLUGIN_PATH)
    unset(ENV{${var}})
endforeach()

# macOS with the app: everything is inside the bundle (cmake/WabcutInstall.cmake, WABCUT_MACOS_BUNDLE).
if(CMAKE_HOST_APPLE AND EXPECT_APP)
    set(bin_dir "${PREFIX}/wabcut.app/Contents/MacOS")
else()
    set(bin_dir "${PREFIX}/bin")
endif()
set(cli "${bin_dir}/wabcut-cli${exe_suffix}")
if(NOT EXISTS "${cli}")
    message(FATAL_ERROR "missing ${cli}")
endif()
execute_process(COMMAND "${cli}" --version RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "wabcut-cli --version failed (${rc}) with PATH=$ENV{PATH}:\n${out}\n${err}")
endif()
if(NOT out MATCHES "wabcut-cli ${VERSION}")
    message(FATAL_ERROR "unexpected wabcut-cli --version output:\n${out}")
endif()
message(STATUS "wabcut-cli --version from the install tree:\n${out}")

# The AI helpers start from the tree: wabcut-infer loads ggml's compute backends from its directory; wabcut-embed
# loads ONNX Runtime and answers the protocol's hello.
set(scratch "${PREFIX}-scratch")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}")
if("wabcut-infer" IN_LIST EXPECT_HELPERS)
    set(infer "${bin_dir}/wabcut-infer${exe_suffix}")
    execute_process(COMMAND "${infer}" --version RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "^wabcut-infer \\(whisper\\.cpp")
        message(FATAL_ERROR "wabcut-infer --version failed (${rc}):\n${out}\n${err}")
    endif()
    execute_process(COMMAND "${infer}" --devices RESULT_VARIABLE rc OUTPUT_VARIABLE devices ERROR_VARIABLE err
        TIMEOUT 120)
    string(TOLOWER "${devices}" devices_lower)
    if(NOT rc EQUAL 0 OR NOT devices_lower MATCHES "cpu")
        message(FATAL_ERROR "wabcut-infer found no CPU compute backend in the tree (${rc}):\n${devices}\n${err}")
    endif()
    message(STATUS "wabcut-infer from the install tree: ${out}compute devices:\n${devices}")
endif()
if("wabcut-embed" IN_LIST EXPECT_HELPERS)
    file(WRITE "${scratch}/embed-hello.ndjson" "{\"type\":\"hello\",\"protocol\":1}\n{\"type\":\"shutdown\"}\n")
    execute_process(COMMAND "${bin_dir}/wabcut-embed${exe_suffix}"
        INPUT_FILE "${scratch}/embed-hello.ndjson"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "\"engine\":\"onnxruntime\"")
        message(FATAL_ERROR "wabcut-embed did not start from the tree (${rc}):\n${out}\n${err}")
    endif()
    message(STATUS "wabcut-embed from the install tree: ${out}")
endif()

if(CMAKE_HOST_WIN32)
    if(NOT EXISTS "${DUMPBIN}")
        message(FATAL_ERROR "DUMPBIN is not set to MSVC's dumpbin.exe: '${DUMPBIN}'")
    endif()
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL dumpbin)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${DUMPBIN}")
    string(TOLOWER "${PREFIX}/" tree_lower)
    string(TOLOWER "${system_root}/" system_lower)

    # Every DLL `executables` and `libraries` import must come from the tree or from Windows itself.
    function(check_closure what)
        cmake_parse_arguments(ARG "" "" "EXECUTABLES;LIBRARIES" ${ARGN})
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES ${ARG_EXECUTABLES}
            LIBRARIES ${ARG_LIBRARIES}
            DIRECTORIES "${PREFIX}/bin"
            RESOLVED_DEPENDENCIES_VAR resolved
            UNRESOLVED_DEPENDENCIES_VAR unresolved
            CONFLICTING_DEPENDENCIES_PREFIX conflict
            PRE_EXCLUDE_REGEXES "^[Aa][Pp][Ii]-[Mm][Ss]-" "^[Ee][Xx][Tt]-[Mm][Ss]-"
            # Windows' own DLLs are not scanned further (their optional imports are not ours to ship).
            POST_EXCLUDE_REGEXES ".*[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*")
        if(unresolved)
            message(FATAL_ERROR "DLLs of ${what} not found in the install tree or the OS: ${unresolved}")
        endif()
        # A DLL found in two places (the MSVC runtime: bin/ for the executables, System32 for a plugin in plugins/)
        # is loaded once, from bin/, before any plugin. Each candidate must still be in the tree or the OS.
        foreach(name IN LISTS conflict_FILENAMES)
            list(APPEND resolved ${conflict_${name}})
        endforeach()
        foreach(dll IN LISTS resolved)
            file(TO_CMAKE_PATH "${dll}" dll_path)
            string(TOLOWER "${dll_path}" dll_lower)
            string(FIND "${dll_lower}" "${tree_lower}" in_tree)
            string(FIND "${dll_lower}" "${system_lower}" in_system)
            if(NOT in_tree EQUAL 0 AND NOT in_system EQUAL 0)
                message(FATAL_ERROR "${what} would load ${dll} from outside the install tree")
            endif()
        endforeach()
    endfunction()

    if(EXPECT_APP)
        set(app "${PREFIX}/bin/wabcut${exe_suffix}")
        if(NOT EXISTS "${app}")
            message(FATAL_ERROR "missing ${app}")
        endif()
        check_closure("wabcut.exe" EXECUTABLES "${app}" LIBRARIES "${PREFIX}/plugins/platforms/qwindows.dll")
        # Qt's deployment layout: libraries next to the executable, bin/qt.conf pointing at plugins/ and qml/.
        # The D3D / DirectX shader compilers come from the Windows SDK (see WabcutInstall.cmake).
        foreach(required bin/Qt6Core.dll bin/Qt6Quick.dll bin/qt.conf plugins/platforms/qwindows.dll
                         qml/QtQuick/Controls bin/d3dcompiler_47.dll bin/dxcompiler.dll bin/dxil.dll)
            if(NOT EXISTS "${PREFIX}/${required}")
                message(FATAL_ERROR "the install tree lacks ${required}")
            endif()
        endforeach()
        # Version information (Explorer > Properties > Details): read back from the resource with dumpbin is not
        # possible, so check that the resource compiler embedded the strings.
        file(STRINGS "${app}" product_strings ENCODING UTF-16LE REGEX "^Wabcut video editor$")
        if(NOT product_strings)
            message(FATAL_ERROR "wabcut.exe has no version information (FileDescription)")
        endif()
    endif()
    foreach(helper IN LISTS EXPECT_HELPERS)
        set(modules "")
        if(helper STREQUAL "wabcut-infer")
            file(GLOB modules "${PREFIX}/bin/ggml-cpu-*.dll" "${PREFIX}/bin/ggml-vulkan.dll")
            if(NOT modules)
                message(FATAL_ERROR "the install tree has no ggml compute backends (bin/ggml-cpu-*.dll)")
            endif()
        elseif(helper STREQUAL "wabcut-embed")
            set(modules "${PREFIX}/bin/onnxruntime_providers_shared.dll")
        endif()
        check_closure("${helper}.exe" EXECUTABLES "${PREFIX}/bin/${helper}${exe_suffix}" LIBRARIES ${modules})
    endforeach()
    if(NOT EXISTS "${PREFIX}/bin/docs/roadmap.md")
        message(FATAL_ERROR "the install tree lacks the Home screen's docs (bin/docs/)")
    endif()
elseif(EXPECT_APP)
    # Qt's deployment layout (cmake/WabcutInstall.cmake): the libraries, the platform plugin and the QML modules the
    # app imports. The CLI run above already loaded the Qt libraries from the tree.
    if(CMAKE_HOST_APPLE)
        set(contents "wabcut.app/Contents")
        set(required ${contents}/MacOS/wabcut ${contents}/Frameworks/QtCore.framework
                     ${contents}/Frameworks/QtQuick.framework ${contents}/PlugIns/platforms/libqcocoa.dylib
                     ${contents}/Resources/qml/QtQuick/Controls ${contents}/Resources/qt.conf
                     ${contents}/Resources/docs/roadmap.md ${contents}/Info.plist)
    else()
        set(required bin/wabcut bin/qt.conf lib/libQt6Core.so.6 lib/libQt6Quick.so.6
                     plugins/platforms/libqxcb.so plugins/platforms/libqoffscreen.so qml/QtQuick/Controls
                     bin/docs/roadmap.md
                     share/applications/wabcut.desktop share/icons/hicolor/256x256/apps/wabcut.png)
    endif()
    foreach(path IN LISTS required)
        if(NOT EXISTS "${PREFIX}/${path}")
            message(FATAL_ERROR "the install tree lacks ${path}")
        endif()
    endforeach()
    if(NOT CMAKE_HOST_APPLE)
        # The Wayland platform plugin, for sessions without XWayland (its name changed in Qt 6.10).
        file(GLOB wayland_platform "${PREFIX}/plugins/platforms/libqwayland*.so")
        if(NOT wayland_platform)
            message(FATAL_ERROR "the install tree lacks the Wayland platform plugin (plugins/platforms/libqwayland*.so)")
        endif()
        # Every library the executables, the Qt libraries, the platform, image, TLS and input plugins and every QML
        # plugin load comes from the tree or is part of the operating system - checked by name, not by what this machine
        # happens to have installed (a Qt kit, libxcb-cursor0), which is how 0.5.2 shipped without the Qt libraries of its
        # QML plugins (cmake/packaging/linux/BundleLibraries.cmake). The optional plugins (GTK theme, network
        # information, EGLFS) load on demand and are left out.
        include("${CMAKE_CURRENT_LIST_DIR}/packaging/linux/BundleLibraries.cmake")
        set(checked "")
        foreach(dir bin lib plugins/platforms plugins/xcbglintegrations plugins/wayland-decoration-client
                    plugins/wayland-graphics-integration-client plugins/wayland-shell-integration plugins/imageformats
                    plugins/iconengines plugins/tls plugins/platforminputcontexts qml)
            if(IS_DIRECTORY "${PREFIX}/${dir}")
                wabcut_linux_elf_files(files "${PREFIX}/${dir}")
                list(APPEND checked ${files})
            endif()
        endforeach()
        wabcut_linux_check_libraries(PREFIX "${PREFIX}" FILES ${checked})
        list(LENGTH checked checked_count)
        message(STATUS "${checked_count} ELF files load only libraries from the tree or the operating system")
    endif()
endif()

# ---- 3. A headless render with the installed CLI -------------------------------------------------------------
if(SMOKE_PROJECT)
    set(project "${scratch}/tiny.wabcut")
    configure_file("${SMOKE_PROJECT}" "${project}" COPYONLY)
    execute_process(
        COMMAND "${cli}" cmd "${project}" --save
                --json [[{"command":"title.add","params":{"at":0,"duration":"00:00:01:00","text":"Wabcut"}}]]
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "wabcut-cli cmd title.add failed (${rc}):\n${out}\n${err}")
    endif()
    set(movie "${scratch}/tiny.mp4")
    # Windows renders through the real renderer (D3D11, or WARP without a GPU). The Linux and macOS CI runners have no
    # GPU and no display: there the CLI encodes its synthetic picture, which still runs the installed FFmpeg, encoder and
    # muxer.
    set(picture "")
    if(NOT CMAKE_HOST_WIN32)
        set(picture --test-pattern)
    endif()
    execute_process(
        COMMAND "${cli}" render "${project}" "${movie}" --encoder libx264 ${picture}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
    if(NOT rc EQUAL 0 OR NOT EXISTS "${movie}")
        message(FATAL_ERROR "wabcut-cli render from the install tree failed (${rc}):\n${out}\n${err}")
    endif()
    file(SIZE "${movie}" movie_bytes)
    if(movie_bytes LESS 1000 OR NOT "${out}${err}" MATCHES "video: libx264, 25 frames")
        message(FATAL_ERROR "wabcut-cli render wrote ${movie_bytes} bytes:\n${out}\n${err}")
    endif()
    message(STATUS "headless render from the install tree: ${movie_bytes} bytes\n${out}${err}")
endif()

# ---- 4. Licenses, notices and size --------------------------------------------------------------------------
if(CMAKE_HOST_WIN32)
    set(doc_dir "${PREFIX}")
elseif(CMAKE_HOST_APPLE AND EXPECT_APP)
    set(doc_dir "${PREFIX}/wabcut.app/Contents/Resources")
else()
    set(doc_dir "${PREFIX}/share/doc/Wabcut")
endif()
foreach(required LICENSE.txt THIRD-PARTY-NOTICES.txt licenses/ffmpeg.txt licenses/x264.txt licenses/x265.txt
                 licenses/qt-LGPL-3.0.txt licenses/boxicons.txt licenses/opencolorio.txt)
    if(NOT EXISTS "${doc_dir}/${required}")
        message(FATAL_ERROR "the install tree lacks ${required}")
    endif()
endforeach()
if("wabcut-infer" IN_LIST EXPECT_HELPERS)
    foreach(required licenses/whisper-cpp.txt licenses/ggml.txt licenses/vulkan-loader.txt
                     licenses/ai-local-helper.md)
        if(NOT EXISTS "${doc_dir}/${required}")
            message(FATAL_ERROR "the install tree lacks ${required}")
        endif()
    endforeach()
endif()
file(READ "${doc_dir}/THIRD-PARTY-NOTICES.txt" notices)
foreach(expected "--enable-gpl" "Runtime libraries in this distribution:")
    string(FIND "${notices}" "${expected}" at)
    if(at LESS 0)
        message(FATAL_ERROR "THIRD-PARTY-NOTICES.txt does not mention '${expected}'")
    endif()
endforeach()
string(FIND "${notices}" "@V_" unfilled)
if(NOT unfilled LESS 0)
    message(FATAL_ERROR "THIRD-PARTY-NOTICES.txt has an unfilled placeholder")
endif()

file(REMOVE_RECURSE "${scratch}")
file(GLOB_RECURSE files LIST_DIRECTORIES false "${PREFIX}/*")
# Symbolic links (a macOS framework's Versions/Current, Linux's libQt6Core.so.6) are not counted twice, and a link
# to a folder has no size.
set(regular "")
foreach(f IN LISTS files)
    if(NOT IS_SYMLINK "${f}" AND NOT IS_DIRECTORY "${f}")
        list(APPEND regular "${f}")
    endif()
endforeach()
set(files ${regular})
list(LENGTH files file_count)
set(total 0)
foreach(f IN LISTS files)
    file(SIZE "${f}" size)
    math(EXPR total "${total} + ${size}")
endforeach()
math(EXPR total_mib "${total} / 1048576")
message(STATUS "install tree: ${file_count} files, ${total} bytes (${total_mib} MiB) in ${PREFIX}")
