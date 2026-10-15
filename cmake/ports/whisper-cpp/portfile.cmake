# Wabcut overlay for whisper.cpp (docs/ai.md, "vcpkg overlay ports"): the registry port lags at 1.8.6, and Wabcut
# needs >= 1.9 (Silero VAD whose timestamps map back to the original audio, DTW presets up to large-v3-turbo).
# Built against the ggml overlay (same v1.9.5 tarball), never the copy vendored in the source tree.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ggml-org/whisper.cpp
    REF "v${VERSION}"
    SHA512 2948873d9fb86c676d45b9fce6ba2c6a056062583c481113b737cba9d16835954d9407743a987a8065b89e55baff01862385d1831bfbd091540e9058305c888a
    HEAD_REF master
)
file(REMOVE_RECURSE "${SOURCE_PATH}/ggml")

# whisper-config.cmake checks the lib/bin directories at their pre-fixup install paths, which vcpkg moves.
vcpkg_replace_string("${SOURCE_PATH}/cmake/whisper-config.cmake.in"
    [[set_and_check(WHISPER_LIB_DIR     "@PACKAGE_WHISPER_LIB_INSTALL_DIR@")]]
    [[set(WHISPER_LIB_DIR     "@PACKAGE_WHISPER_LIB_INSTALL_DIR@")]] IGNORE_UNCHANGED)
vcpkg_replace_string("${SOURCE_PATH}/cmake/whisper-config.cmake.in"
    [[set_and_check(WHISPER_BIN_DIR     "@PACKAGE_WHISPER_BIN_INSTALL_DIR@")]]
    [[set(WHISPER_BIN_DIR     "@PACKAGE_WHISPER_BIN_INSTALL_DIR@")]] IGNORE_UNCHANGED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    DISABLE_PARALLEL_CONFIGURE # the configure step rewrites bindings/javascript/package.json
    OPTIONS
        -DWHISPER_USE_SYSTEM_GGML=ON
        -DWHISPER_ALL_WARNINGS=OFF
        -DWHISPER_BUILD_EXAMPLES=OFF
        -DWHISPER_BUILD_SERVER=OFF
        -DWHISPER_BUILD_TESTS=OFF
        -DGGML_CCACHE=OFF
        -DWHISPER_CURL=OFF
        -DWHISPER_SDL2=OFF
    MAYBE_UNUSED_VARIABLES
        GGML_CCACHE
        WHISPER_CURL
        WHISPER_SDL2
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/whisper")
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
