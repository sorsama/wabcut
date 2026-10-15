# Shared compiler settings and toolchain checks.
#   wabcut_target_defaults(<target>)              every Wabcut target: warnings, conformance, UTF-8, src/platform/include
#   wabcut_windows_executable(<target>)           a Windows executable: UTF-8 code page, DLL search order
#   wabcut_allow_qt_generated_code(<targets...>)  targets that compile Qt-generated code (qmlcachegen, ...)
#   wabcut_check_toolchain()                      C++23 library features, Debug vs release-only triplet

include_guard(GLOBAL)

# Windows executables (docs/building.md, "UTF-8 code page" and "DLL search order"); every one gets this through
# wabcut_target_defaults(), other target types are left alone.
#   * The activeCodePage manifest (cmake/utf8.manifest): the narrow Win32 and CRT calls and
#     std::filesystem::path::string() use UTF-8 instead of the ANSI code page, which cannot represent most names (Thai
#     on an English Windows, emoji anywhere) and makes path::string() throw. CMake merges a .manifest source with the
#     linker's own manifest (mt.exe).
#   * The shipped executables (wabcut and wabcut-*: the app, the CLI, the helpers) resolve their static imports from
#     their own folder and System32 only, never the current directory or PATH: /DEPENDENTLOADFLAG:0xA00
#     (LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32; 0x800 alone would drop their own folder,
#     where Qt, FFmpeg and ONNX Runtime are). Tests and tools keep the default: in the build tree they find Qt through
#     the PATH ctest prepends. Run-time loads are hardened in main() (wabcut/platform/DllSearch.h).
function(wabcut_windows_executable target)
    if(NOT MSVC)
        return()
    endif()
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "EXECUTABLE")
        return()
    endif()
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.manifest")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND (target STREQUAL "wabcut" OR target MATCHES "^wabcut-"))
        target_link_options(${target} PRIVATE /DEPENDENTLOADFLAG:0xA00)
    endif()
endfunction()

function(wabcut_target_defaults target)
    wabcut_windows_executable(${target})
    # Header-only platform helpers (DLL search order, helper processes): visible to every target, also those that link
    # nothing of Wabcut's (wabcut_plughost_common, the fake helpers).
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/platform/include")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            # cl's conforming preprocessor; clang-cl's always conforms and rejects the flag (unused argument).
            target_compile_options(${target} PRIVATE /Zc:preprocessor)
        else()
            # clang-cl: /W4 is -Wall -Wextra (see the note on missing-field-initializers below).
            target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
        endif()
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        if(WABCUT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        # The hardened STL of a sanitizer build aborts on a failed check, and the optimizer then reports unreachable
        # code in inlined third-party headers (WabcutSanitizers.cmake).
        if(WABCUT_SANITIZE)
            target_compile_options(${target} PRIVATE /wd4702)
        endif()
    else()
        # -Wextra's missing-field-initializers flags `Keyframe{t, value}`: brace initialization that leaves the
        # remaining members to their default member initializers, which the code base uses on purpose throughout.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
                                                 -Wno-missing-field-initializers)
        if(WABCUT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# C4702 ("unreachable code") is emitted by the MSVC optimizer for Qt headers inlined into qmlcachegen output and
# other Qt-generated sources. It ignores /external:W0, so it cannot be scoped to third-party headers; instead
# only the targets that compile such code switch it off. The pure-C++ core keeps the full /W4 /WX.
function(wabcut_allow_qt_generated_code)
    if(NOT MSVC)
        return()
    endif()
    foreach(target IN LISTS ARGN)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE /wd4702)
        endif()
    endforeach()
endfunction()

function(wabcut_check_toolchain)
    # C++23 library features the code base relies on. CMAKE_CXX_STANDARD applies to the check (CMP0067).
    include(CheckCXXSourceCompiles)
    # std::jthread / std::stop_token and floating-point std::from_chars are used throughout (the AI, media and render
    # libraries, the core too); libc++ ships them complete from 20 only, so Apple Clang (Xcode 16) cannot build Wabcut
    # yet (I.7 portability audit).
    check_cxx_source_compiles([=[
        #include <charconv>
        #include <expected>
        #include <format>
        #include <stop_token>
        #include <string>
        #include <thread>
        #if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
        #error "std::expected is missing"
        #endif
        #if !defined(__cpp_lib_jthread)
        #error "std::jthread is missing"
        #endif
        int main() {
            const std::expected<int, std::string> value{1};
            double parsed = 0.0;
            const char text[] = "2.5";
            std::from_chars(text, text + 3, parsed);
            std::stop_source stop;
            std::jthread worker([](std::stop_token token) { (void)token.stop_requested(); });
            return *value + static_cast<int>(std::format("{}", 0).size()) - 2 + (parsed == 2.5 ? 0 : 1) +
                   (stop.stop_requested() ? 1 : 0);
        }
    ]=] WABCUT_HAS_CXX23_LIBRARY)
    if(NOT WABCUT_HAS_CXX23_LIBRARY)
        message(FATAL_ERROR
            "Wabcut needs a C++23 compiler and standard library with std::expected, std::format, std::jthread and "
            "floating-point std::from_chars (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} does not provide "
            "them all). Use MSVC 19.36+ (Visual Studio 2022 17.6 or later), GCC 13+, or Clang 17+ with libstdc++ 13+, "
            "or LLVM 20+ with its libc++ (on macOS: Homebrew's llvm, not Apple Clang). See docs/building.md.")
    endif()

    # The Windows triplet x64-windows-release builds every dependency in release mode only. A Debug build would
    # link the debug CRT/STL against release-CRT dependencies (mismatched _ITERATOR_DEBUG_LEVEL and heaps).
    if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-release$")
        set(fix "Configure RelWithDebInfo (the win-msvc preset) for debugging, or use the opt-in win-msvc-debug "
                "preset, which builds all dependencies with the stock x64-windows triplet. See docs/building.md.")
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(FATAL_ERROR "CMAKE_BUILD_TYPE=Debug cannot be used with the release-only vcpkg triplet "
                                "'${VCPKG_TARGET_TRIPLET}': the debug CRT would be mixed with release-CRT dependencies. ${fix}")
        endif()
        if(CMAKE_CONFIGURATION_TYPES AND "Debug" IN_LIST CMAKE_CONFIGURATION_TYPES)
            message(FATAL_ERROR "The multi-config generator offers a Debug configuration, which cannot be used with "
                                "the release-only vcpkg triplet '${VCPKG_TARGET_TRIPLET}'. Pass "
                                "-DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo;Release. ${fix}")
        endif()
    endif()
endfunction()

# Third-party headers (FFmpeg, miniaudio) are noisy: include them as SYSTEM so /W4 doesn't fire on them.
