# ONNX Runtime 1.24.4 from Microsoft's official release binaries (MIT). Nothing is compiled: building ORT from source
# takes hours and far more memory than a workstation build may use, and only the runtime is needed (wabcut-embed,
# docs/ai.md "Smart search").
#
#   Windows x64/arm64  NuGet Microsoft.ML.OnnxRuntime.DirectML 1.24.4: the DirectML and CPU execution providers in one
#                      onnxruntime.dll. 1.24.4 is the last ORT release with a DirectML package. The DirectML runtime
#                      itself (Microsoft.AI.DirectML, a proprietary redistributable) is NOT shipped: onnxruntime.dll
#                      loads the DirectML.dll of Windows 10 1903+ (System32), and wabcut-embed falls back to the CPU
#                      provider when DirectML can not start.
#   Linux x64/aarch64  GitHub release onnxruntime-linux-<arch>-1.24.4.tgz (CPU provider).
#   macOS arm64        GitHub release onnxruntime-osx-arm64-1.24.4.tgz (CPU and Core ML providers).
#
# Every archive is pinned twice: vcpkg's SHA-512 (for NuGet this is the package hash the NuGet catalog publishes) and
# the SHA-256 Microsoft publishes for the GitHub release assets (for NuGet: of the same, catalog-verified file).
# Installs onnxruntime-prebuilt::onnxruntime (see usage).
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

set(ORT_VERSION "${VERSION}")
if(VCPKG_TARGET_IS_WINDOWS)
    set(ORT_URL "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${ORT_VERSION}/microsoft.ml.onnxruntime.directml.${ORT_VERSION}.nupkg")
    set(ORT_FILE "microsoft.ml.onnxruntime.directml.${ORT_VERSION}.nupkg")
    set(ORT_SHA512 6633b2bf8f79be17d55e84c9a76bad4729fc8abd53148bc28f407d24ff106460574b4a05c7e958ed9099a927675528505fa12dc588b64f754eb585dc9814d5e0)
    set(ORT_SHA256 57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681)
    set(ORT_ONE_LEVEL NO_REMOVE_ONE_LEVEL)
elseif(VCPKG_TARGET_IS_LINUX AND VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(ORT_FILE "onnxruntime-linux-x64-${ORT_VERSION}.tgz")
    set(ORT_SHA512 74a781d161720be528e46c46a30ed3c9f37f51a33253176a5d5c29d80a7726c3380b194560868802e8cd3141184a3f169cc293bd2a3a4819318f023148d69a20)
    set(ORT_SHA256 3a211fbea252c1e66290658f1b735b772056149f28321e71c308942cdb54b747)
elseif(VCPKG_TARGET_IS_LINUX AND VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(ORT_FILE "onnxruntime-linux-aarch64-${ORT_VERSION}.tgz")
    set(ORT_SHA512 993b4a93094fab76e3596b6137f0a98e595dd0995b8ebdf500d04caa23ebb17d7b7ec369cb279af5867559d2eb82ab30a5e40d1cb20c0f89f4acfcc20ef04721)
    set(ORT_SHA256 866109a9248d057671a039b9d725be4bd86888e3754140e6701ec621be9d4d7e)
elseif(VCPKG_TARGET_IS_OSX AND VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(ORT_FILE "onnxruntime-osx-arm64-${ORT_VERSION}.tgz")
    set(ORT_SHA512 cb076a1bf339614cf488f8508df9041251ded57e2f1a0c65c01247e868cae14c57410775cd64c15b6bc97642aab9603373f5af7db796a21de74fae863081bdd1)
    set(ORT_SHA256 93787795f47e1eee369182e43ed51b9e5da0878ab0346aecf4258979b8bba989)
else()
    message(FATAL_ERROR "${PORT}: Microsoft publishes no ONNX Runtime ${ORT_VERSION} binaries for this target")
endif()
if(NOT DEFINED ORT_URL)
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_FILE}")
endif()

vcpkg_download_distfile(ARCHIVE URLS "${ORT_URL}" FILENAME "${ORT_FILE}" SHA512 ${ORT_SHA512})
file(SHA256 "${ARCHIVE}" ORT_ACTUAL_SHA256)
if(NOT ORT_ACTUAL_SHA256 STREQUAL ORT_SHA256)
    message(FATAL_ERROR "${PORT}: ${ORT_FILE} has SHA-256 ${ORT_ACTUAL_SHA256}, expected ${ORT_SHA256}")
endif()
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" ${ORT_ONE_LEVEL})

set(ORT_CONFIGS "${CURRENT_PACKAGES_DIR}")
if(NOT VCPKG_BUILD_TYPE STREQUAL "release")
    # Microsoft ships release binaries only; debug builds link the same ones (C API: no CRT objects cross the DLL).
    list(APPEND ORT_CONFIGS "${CURRENT_PACKAGES_DIR}/debug")
    set(VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK enabled)
endif()

if(VCPKG_TARGET_IS_WINDOWS)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(ORT_NATIVE "${SOURCE_PATH}/runtimes/win-arm64/native")
    else()
        set(ORT_NATIVE "${SOURCE_PATH}/runtimes/win-x64/native")
    endif()
    file(GLOB ORT_HEADERS "${SOURCE_PATH}/build/native/include/*.h")
    foreach(dir IN LISTS ORT_CONFIGS)
        file(INSTALL "${ORT_NATIVE}/onnxruntime.lib" DESTINATION "${dir}/lib")
        file(INSTALL "${ORT_NATIVE}/onnxruntime.dll" "${ORT_NATIVE}/onnxruntime_providers_shared.dll" DESTINATION "${dir}/bin")
    endforeach()
    set(ORT_IMPORTED_LOCATION "bin/onnxruntime.dll")
    set(ORT_IMPORTED_IMPLIB "lib/onnxruntime.lib")
    set(ORT_RUNTIME_FILES "bin/onnxruntime.dll;bin/onnxruntime_providers_shared.dll")
else()
    file(GLOB ORT_HEADERS "${SOURCE_PATH}/include/*.h")
    if(VCPKG_TARGET_IS_OSX)
        set(ORT_LIBS "${SOURCE_PATH}/lib/libonnxruntime.${ORT_VERSION}.dylib")
        set(ORT_IMPORTED_LOCATION "lib/libonnxruntime.${ORT_VERSION}.dylib")
    else()
        # The library and its libonnxruntime.so.1 / libonnxruntime.so links (file(INSTALL) keeps links as links).
        file(GLOB ORT_LIBS "${SOURCE_PATH}/lib/libonnxruntime.so*" "${SOURCE_PATH}/lib/libonnxruntime_providers_shared.so")
        set(ORT_IMPORTED_LOCATION "lib/libonnxruntime.so.${ORT_VERSION}")
    endif()
    foreach(dir IN LISTS ORT_CONFIGS)
        file(INSTALL ${ORT_LIBS} DESTINATION "${dir}/lib")
    endforeach()
    set(ORT_IMPORTED_IMPLIB "")
    set(ORT_RUNTIME_FILES "${ORT_IMPORTED_LOCATION}")
endif()
file(INSTALL ${ORT_HEADERS} DESTINATION "${CURRENT_PACKAGES_DIR}/include/onnxruntime")

configure_file("${CMAKE_CURRENT_LIST_DIR}/onnxruntime-prebuilt-config.cmake.in"
               "${CURRENT_PACKAGES_DIR}/share/${PORT}/onnxruntime-prebuilt-config.cmake" @ONLY)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
# MIT licence plus Microsoft's notices for the code ORT links in.
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${SOURCE_PATH}/ThirdPartyNotices.txt")
