# The shared libraries a Linux install tree carries, and the ones it takes from the operating system. Included by the
# install step (cmake/WabcutInstall.cmake) and by the install smoke test (cmake/InstallSmokeTest.cmake); script mode
# only, no targets.
#
#   wabcut_linux_bundle_libraries(PREFIX <dir> LIB_DIR <lib> SEARCH_DIRS <dir>...)
#       Copies into <prefix>/<lib> every library an ELF file of the tree needs that is neither in the tree nor one of the
#       operating system's (WABCUT_LINUX_SYSTEM_LIBRARIES), until nothing is missing. A library is taken from
#       SEARCH_DIRS first (the Qt kit, vcpkg), else from where the build machine has it (the xcb helpers below).
#   wabcut_linux_check_libraries(PREFIX <dir> FILES <file>...)
#       Fails unless every library FILES need resolves inside PREFIX or is one of the operating system's.
#
# Why: Qt's generic deploy tool copies the QML plugins the app imports but not always the Qt libraries those plugins
# link (libQt6QuickDialogs2, libQt6QuickLayouts, libQt6QuickControls2Basic, ...), and Qt's xcb platform plugin links
# xcb helper libraries that a stock desktop does not install (libxcb-cursor0 and friends). Wabcut 0.5.2 then did not
# start on a Linux desktop, while CI, which runs with the Qt kit on LD_LIBRARY_PATH and those packages installed, did.

include_guard(GLOBAL)

# Libraries every supported desktop (Ubuntu 24.04, Fedora 40 or another distribution of that age or newer, X11 or
# Wayland) has, matched on the file name a binary asks for. They stay out of the tree: the C and C++ runtimes, the GPU
# driver stack, X11 and Wayland, fonts, D-Bus and GLib, GTK (Qt's GTK theme plugin), Kerberos, ALSA / PulseAudio.
# Everything else a binary needs is bundled - in particular every Qt library and the xcb helpers Qt's xcb platform
# plugin links (libxcb-cursor, -icccm, -image, -keysyms, -render-util, -util, -xkb, libxkbcommon-x11).
set(WABCUT_LINUX_SYSTEM_LIBRARIES
    "^ld-linux" "^libc\\.so" "^libm\\.so" "^libmvec\\.so" "^libdl\\.so" "^libpthread\\.so" "^librt\\.so"
    "^libresolv\\.so" "^libutil\\.so" "^libanl\\.so" "^libstdc\\+\\+\\.so" "^libgcc_s\\.so"
    "^libGL\\.so" "^libGLX\\.so" "^libEGL\\.so" "^libOpenGL\\.so" "^libGLdispatch\\.so" "^libGLESv2\\.so"
    "^libgbm\\.so" "^libdrm\\.so" "^libvulkan\\.so"
    "^libX11\\.so" "^libX11-xcb\\.so" "^libXext\\.so" "^libXrender\\.so" "^libXi\\.so" "^libXfixes\\.so"
    "^libXrandr\\.so" "^libXcursor\\.so" "^libXau\\.so" "^libXdmcp\\.so" "^libSM\\.so" "^libICE\\.so"
    "^libxcb\\.so" "^libxcb-(dri2|dri3|glx|present|randr|render|shape|shm|sync|xfixes|xinerama|xinput)\\.so"
    "^libwayland-(client|cursor|egl)\\.so" "^libxkbcommon\\.so"
    "^libfontconfig\\.so" "^libfreetype\\.so" "^libharfbuzz\\.so" "^libexpat\\.so" "^libpng16\\.so" "^libz\\.so"
    "^libzstd\\.so" "^libbz2\\.so" "^libbrotli(common|dec)\\.so"
    "^libdbus-1\\.so" "^lib(glib|gobject|gio|gthread|gmodule)-2\\.0\\.so" "^libsystemd\\.so" "^libudev\\.so"
    "^lib(gtk|gdk)-3\\.so" "^libgdk_pixbuf-2\\.0\\.so" "^libpango(cairo|ft2)?-1\\.0\\.so" "^libcairo(-gobject)?\\.so"
    "^libatk(-bridge)?-[12]\\.0\\.so"
    "^libgssapi_krb5\\.so" "^libkrb5\\.so" "^libk5crypto\\.so" "^libcom_err\\.so" "^libssl\\.so" "^libcrypto\\.so"
    "^libasound\\.so" "^libpulse(-simple)?\\.so" "^libsecret-1\\.so"
    "^libinput\\.so" "^libmtdev\\.so" "^libevdev\\.so" "^libts\\.so")

if(NOT CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "linux+elf")
endif()
if(NOT CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
endif()

# Every ELF file (executable, library, Qt or QML plugin) under `root`, symbolic links left out.
function(wabcut_linux_elf_files out root)
    file(GLOB_RECURSE candidates LIST_DIRECTORIES false "${root}/*")
    set(found "")
    foreach(path IN LISTS candidates)
        if(IS_SYMLINK "${path}")
            continue()
        endif()
        file(READ "${path}" magic LIMIT 4 HEX)
        if(magic STREQUAL "7f454c46")
            list(APPEND found "${path}")
        endif()
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# The libraries `files` need that resolve outside `root` or not at all, as "<name>|<resolved path or empty>". A name the
# tree has in `libdir` counts as in the tree: the executables and the Qt libraries find libdir through their RUNPATH, and
# the dynamic loader reuses a library already loaded under that name, so a bundled library without a RUNPATH of its own
# (an xcb helper copied from the build machine) gets its siblings from there too - libQt6XcbQpa loads every one of them
# directly.
function(_wabcut_linux_outside out root libdir)
    file(GET_RUNTIME_DEPENDENCIES
        LIBRARIES ${ARGN}
        PRE_EXCLUDE_REGEXES ${WABCUT_LINUX_SYSTEM_LIBRARIES}
        RESOLVED_DEPENDENCIES_VAR resolved
        UNRESOLVED_DEPENDENCIES_VAR unresolved
        CONFLICTING_DEPENDENCIES_PREFIX conflict)
    foreach(name IN LISTS conflict_FILENAMES)
        list(APPEND resolved ${conflict_${name}})
    endforeach()
    set(outside "")
    foreach(path IN LISTS resolved)
        string(FIND "${path}" "${root}/" at)
        get_filename_component(name "${path}" NAME)
        if(NOT at EQUAL 0 AND NOT EXISTS "${root}/${libdir}/${name}")
            list(APPEND outside "${name}|${path}")
        endif()
    endforeach()
    foreach(name IN LISTS unresolved)
        if(NOT EXISTS "${root}/${libdir}/${name}")
            list(APPEND outside "${name}|")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES outside)
    set(${out} "${outside}" PARENT_SCOPE)
endfunction()

function(wabcut_linux_bundle_libraries)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "PREFIX;LIB_DIR" "SEARCH_DIRS")
    # CMAKE_INSTALL_PREFIX has no DESTDIR (file(INSTALL) adds it); the files are read where they really are.
    set(root "$ENV{DESTDIR}${arg_PREFIX}")
    get_filename_component(root "${root}" REALPATH)
    set(copied "")
    set(missing "")
    foreach(pass RANGE 9)
        wabcut_linux_elf_files(files "${root}")
        _wabcut_linux_outside(outside "${root}" "${arg_LIB_DIR}" ${files})
        set(progress FALSE)
        foreach(entry IN LISTS outside)
            string(FIND "${entry}" "|" bar)
            string(SUBSTRING "${entry}" 0 ${bar} name)
            math(EXPR bar "${bar} + 1")
            string(SUBSTRING "${entry}" ${bar} -1 found)
            set(source "")
            # The Qt kit and vcpkg first: a distribution's own Qt (or ICU) of another version must not stand in.
            foreach(dir IN LISTS arg_SEARCH_DIRS)
                if(EXISTS "${dir}/${name}")
                    set(source "${dir}/${name}")
                    break()
                endif()
            endforeach()
            if(NOT source)
                set(source "${found}")
            endif()
            if(NOT source)
                list(APPEND missing "${name}")
                continue()
            endif()
            # The library under the name binaries ask for, with the file it links to.
            file(INSTALL "${source}" DESTINATION "${arg_PREFIX}/${arg_LIB_DIR}" FOLLOW_SYMLINK_CHAIN)
            list(APPEND copied "${name}")
            set(progress TRUE)
        endforeach()
        if(NOT progress)
            break()
        endif()
    endforeach()
    if(copied)
        list(JOIN copied ", " copied)
        message(STATUS "Bundled the libraries Qt's deployment left out: ${copied}")
    endif()
    if(missing)
        # An optional plugin's library the build machine does not have; the install smoke test fails if the app needs it.
        list(REMOVE_DUPLICATES missing)
        list(JOIN missing ", " missing)
        message(WARNING "Libraries found neither in the Qt kit, vcpkg nor on this machine, not bundled: ${missing}")
    endif()
endfunction()

function(wabcut_linux_check_libraries)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "PREFIX;LIB_DIR" "FILES")
    if(NOT arg_LIB_DIR)
        set(arg_LIB_DIR lib)
    endif()
    if(NOT arg_FILES)
        message(FATAL_ERROR "wabcut_linux_check_libraries: no files to check in ${arg_PREFIX}")
    endif()
    get_filename_component(root "${arg_PREFIX}" REALPATH)
    _wabcut_linux_outside(outside "${root}" "${arg_LIB_DIR}" ${arg_FILES})
    if(outside)
        list(TRANSFORM outside REPLACE "\\|$" " (not found)")
        list(TRANSFORM outside REPLACE "\\|" " from ")
        list(JOIN outside "\n    " outside)
        message(FATAL_ERROR "The install tree needs libraries that are neither in it nor part of the operating system "
                            "(cmake/packaging/linux/BundleLibraries.cmake):\n    ${outside}")
    endif()
endfunction()
