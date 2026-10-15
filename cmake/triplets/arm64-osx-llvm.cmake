# vcpkg triplet for macOS arm64 built with Homebrew's LLVM 20+ (cmake/toolchains/macos-llvm.cmake), the compiler Wabcut
# itself uses there: static libraries like vcpkg's arm64-osx, release only. CI-ready, not run locally.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.0)
# Release only (as the Windows triplet): RelWithDebInfo links the release libraries; halves the FFmpeg build.
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/macos-llvm.cmake")
# The onnxruntime-prebuilt port installs Microsoft's shared library as it is.
if(PORT STREQUAL "onnxruntime-prebuilt")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
