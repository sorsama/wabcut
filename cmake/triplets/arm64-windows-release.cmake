# Release-only dynamic triplet for Windows on ARM64 (MSVC ARM64 toolset): the arm64 twin of x64-windows-release, so the
# dependencies of the ARM64 preview build in half the time. Wabcut's win-msvc-arm64 preset builds RelWithDebInfo, which
# links the release CRT. CI-ready, not run locally (.github/workflows/windows-arm64.yml, release.yml).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
