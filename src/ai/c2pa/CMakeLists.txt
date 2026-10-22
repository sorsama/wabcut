# wabcut_ai_c2pa - signed C2PA Content Credentials (AI-15, docs/ai.md "Content Credentials"): the manifest of an
# export (actions, ingredients with source hashes, AI-generated segments, redactions), a per-user signing identity
# (a local root and an ECDSA P-256 signing certificate made with Windows CNG; the private key in the OS keychain), and
# signing / verification with c2pa-rs's C library.
#
# c2pa-rs (contentauth/c2pa-rs, MIT OR Apache-2.0) is never compiled here: like ONNX Runtime, the official release
# binaries are used, pinned by URL and SHA-256 (release c2pa-v0.91.2, 2026-10-06; the release API's asset digest).
# Only the header, the DLL and its import library are extracted from the archive (it also carries a 290 MB static
# library). Offline builds: WABCUT_C2PA_ARCHIVE names an already downloaded copy (the SHA-256 is checked all the same).
# Other platforms (and WABCUT_WITH_C2PA=OFF) build the library without the SDK: signing then reports "unsupported" and
# exports keep the unsigned provenance sidecar.
set(wabcut_c2pa_default OFF)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64" AND NOT WABCUT_CORE_ONLY)
    set(wabcut_c2pa_default ON)
endif()
option(WABCUT_WITH_C2PA "Sign C2PA Content Credentials into exports (prebuilt c2pa-rs C library, Windows x64)" ${wabcut_c2pa_default})
set(WABCUT_C2PA_ARCHIVE "" CACHE FILEPATH "A downloaded c2pa-v0.91.2-x86_64-pc-windows-msvc.zip (offline builds)")

add_library(wabcut_ai_c2pa STATIC
    src/ContentCredentials.cpp
    src/Der.cpp
    src/Identity.cpp
    src/Keychain.cpp
    src/Sdk.cpp)
target_include_directories(wabcut_ai_c2pa PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_c2pa PUBLIC wabcut_ai wabcut_model)
if(WIN32)
    target_link_libraries(wabcut_ai_c2pa PRIVATE bcrypt advapi32)
endif()
wabcut_target_defaults(wabcut_ai_c2pa)

if(WABCUT_WITH_C2PA)
    set(c2pa_version "0.91.2")
    set(c2pa_file "c2pa-v${c2pa_version}-x86_64-pc-windows-msvc.zip")
    set(c2pa_url "https://github.com/contentauth/c2pa-rs/releases/download/c2pa-v${c2pa_version}/${c2pa_file}")
    set(c2pa_sha256 "418a8ffa9ce97b655e20bfa89e40bf848d01e5d239c0685d4c7149c05d7e2dfd")
    set(c2pa_root "${CMAKE_BINARY_DIR}/_deps/c2pa-${c2pa_version}")
    set(c2pa_archive "${c2pa_root}/${c2pa_file}")
    if(WABCUT_C2PA_ARCHIVE)
        set(c2pa_archive "${WABCUT_C2PA_ARCHIVE}")
    elseif(NOT EXISTS "${c2pa_archive}")
        message(STATUS "Wabcut: downloading c2pa-rs ${c2pa_version} (C library, 95 MB)")
        file(DOWNLOAD "${c2pa_url}" "${c2pa_archive}.part" EXPECTED_HASH SHA256=${c2pa_sha256} STATUS c2pa_status TLS_VERIFY ON)
        list(GET c2pa_status 0 c2pa_code)
        if(NOT c2pa_code EQUAL 0)
            file(REMOVE "${c2pa_archive}.part")
            message(FATAL_ERROR "Wabcut: could not download ${c2pa_url}: ${c2pa_status}. Set WABCUT_C2PA_ARCHIVE to a "
                                "downloaded copy or WABCUT_WITH_C2PA=OFF.")
        endif()
        file(RENAME "${c2pa_archive}.part" "${c2pa_archive}")
    endif()
    file(SHA256 "${c2pa_archive}" c2pa_actual)
    if(NOT c2pa_actual STREQUAL c2pa_sha256)
        message(FATAL_ERROR "Wabcut: ${c2pa_archive} has SHA-256 ${c2pa_actual}, expected ${c2pa_sha256}")
    endif()
    if(NOT EXISTS "${c2pa_root}/sdk/lib/c2pa_c.dll" OR NOT EXISTS "${c2pa_root}/sdk/include/c2pa.h")
        file(ARCHIVE_EXTRACT INPUT "${c2pa_archive}" DESTINATION "${c2pa_root}/sdk"
             PATTERNS include/c2pa.h lib/c2pa_c.dll lib/c2pa_c.dll.lib)
    endif()
    add_library(wabcut_c2pa_sdk SHARED IMPORTED GLOBAL)
    set_target_properties(wabcut_c2pa_sdk PROPERTIES
        IMPORTED_LOCATION "${c2pa_root}/sdk/lib/c2pa_c.dll"
        IMPORTED_IMPLIB "${c2pa_root}/sdk/lib/c2pa_c.dll.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${c2pa_root}/sdk/include")
    target_link_libraries(wabcut_ai_c2pa PRIVATE wabcut_c2pa_sdk)
    target_compile_definitions(wabcut_ai_c2pa PRIVATE WABCUT_HAVE_C2PA=1 WABCUT_C2PA_VERSION="${c2pa_version}")
    # Delay-loaded: the 24 MB DLL is mapped only when an export is signed or checked, and a missing DLL makes signing
    # unavailable instead of stopping the program from starting (Sdk.cpp loads it explicitly first).
    target_link_options(wabcut_ai_c2pa INTERFACE "/DELAYLOAD:c2pa_c.dll")
    target_link_libraries(wabcut_ai_c2pa INTERFACE delayimp)
    # Next to every executable (they all build into bin/); installed by cmake/WabcutInstall.cmake.
    set(WABCUT_C2PA_DLL "${c2pa_root}/sdk/lib/c2pa_c.dll" CACHE INTERNAL "c2pa_c.dll of the pinned release")
    add_custom_target(wabcut_c2pa_runtime
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WABCUT_C2PA_DLL}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/c2pa_c.dll"
        COMMENT "Copying c2pa_c.dll (c2pa-rs ${c2pa_version}) to bin/")
    add_dependencies(wabcut_ai_c2pa wabcut_c2pa_runtime)
    message(STATUS "Wabcut: Content Credentials signing with c2pa-rs ${c2pa_version} (prebuilt)")
else()
    set(WABCUT_C2PA_DLL "" CACHE INTERNAL "c2pa_c.dll of the pinned release")
    message(STATUS "Wabcut: Content Credentials signing off (WABCUT_WITH_C2PA=OFF): exports keep the unsigned sidecar")
endif()
