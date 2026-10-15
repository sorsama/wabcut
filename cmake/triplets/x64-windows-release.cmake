# Release-only dynamic triplet: halves dependency build time (FFmpeg/x264/x265).
# Wabcut's Windows presets build RelWithDebInfo, which links the release CRT.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
