# Wabcut overlay for ggml (docs/ai.md, "vcpkg overlay ports").
#
# Why an overlay: the registry port pins ggml 0.24 and builds the CPU backend with AVX/AVX2/AVX512/FMA/F16C forced
# OFF (a slow, lowest-common-denominator CPU path). Here ggml is built the way upstream ships it:
#   * GGML_BACKEND_DL: every backend is a DLL loaded at run time (ggml_backend_load_all_from_path), so one build
#     runs everywhere and skips backends whose driver is missing ("probe, don't assume").
#   * GGML_CPU_ALL_VARIANTS: one CPU backend per x86-64 level (SSE4.2 ... AVX-512 / AVX-VNNI); ggml loads the best
#     one the machine supports.
#   * vulkan feature: the GPU backend for Windows/Linux without CUDA. Vulkan headers, the loader, SPIR-V headers and
#     glslc (shaderc) all come from vcpkg ports; no Vulkan SDK is needed.
#
# Source: the ggml directory of the whisper.cpp v1.9.5 release (ggml 0.26.0), so this port always matches the
# whisper-cpp overlay next to it (whisper.cpp vendors the exact ggml it was tested with). The download is shared
# with the whisper-cpp port. When bumping, update both ports together.

if(NOT VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    message(FATAL_ERROR "The Wabcut ggml overlay needs dynamic linkage (GGML_BACKEND_DL loads backends as DLLs).")
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH WHISPER_SOURCE_PATH
    REPO ggml-org/whisper.cpp
    REF v1.9.5
    SHA512 2948873d9fb86c676d45b9fce6ba2c6a056062583c481113b737cba9d16835954d9407743a987a8065b89e55baff01862385d1831bfbd091540e9058305c888a
    HEAD_REF master
)
set(SOURCE_PATH "${WHISPER_SOURCE_PATH}/ggml")

# whisper.cpp vendors ggml without the standalone pkg-config template; use upstream ggml's.
if(NOT EXISTS "${SOURCE_PATH}/ggml.pc.in")
    file(WRITE "${SOURCE_PATH}/ggml.pc.in" [[prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
includedir=${prefix}/include
libdir=${prefix}/@CMAKE_INSTALL_LIBDIR@

Name: ggml
Description: The GGML Tensor Library for Machine Learning
Version: @GGML_VERSION@
Cflags: -I${includedir}
Libs: -L${libdir} -lggml -lggml-base
]])
endif()

set(FEATURE_OPTIONS "")
if("vulkan" IN_LIST FEATURES)
    # ggml builds vulkan-shaders-gen for the build machine itself (ExternalProject), and the glslc used is the one
    # vcpkg built for the target: both have to run here, so this is for native-architecture builds.
    list(APPEND FEATURE_OPTIONS
        -DGGML_VULKAN=ON
        "-DVulkan_GLSLC_EXECUTABLE=${CURRENT_INSTALLED_DIR}/tools/shaderc/glslc${VCPKG_TARGET_EXECUTABLE_SUFFIX}"
    )
else()
    list(APPEND FEATURE_OPTIONS -DGGML_VULKAN=OFF)
endif()

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    list(APPEND FEATURE_OPTIONS -DGGML_CPU_ALL_VARIANTS=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DGGML_BACKEND_DL=ON
        -DGGML_NATIVE=OFF
        -DGGML_CCACHE=OFF
        -DGGML_OPENMP=OFF
        -DGGML_BUILD_NUMBER=1
        -DGGML_BUILD_TESTS=OFF
        -DGGML_BUILD_EXAMPLES=OFF
        -DGGML_CUDA=OFF
        -DGGML_HIP=OFF
        -DGGML_SYCL=OFF
        -DGGML_METAL=OFF
        -DGGML_OPENCL=OFF
        -DGGML_BLAS=OFF
        ${FEATURE_OPTIONS}
    MAYBE_UNUSED_VARIABLES
        GGML_BUILD_NUMBER
        GGML_CPU_ALL_VARIANTS
        GGML_METAL
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME ggml CONFIG_PATH "lib/cmake/ggml")
vcpkg_fixup_pkgconfig()

# The headers pick dllimport by GGML_SHARED / GGML_BACKEND_SHARED; every consumer of this DLL build needs it.
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/ggml.h" "#ifdef GGML_SHARED" "#if 1")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/ggml-backend.h" "#ifdef GGML_BACKEND_SHARED" "#if 1")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

# Backend modules (ggml-cpu-*.dll, ggml-vulkan.dll) are loaded at run time and have no import libraries.
set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${WHISPER_SOURCE_PATH}/LICENSE") # MIT, "The ggml authors" (covers the vendored ggml)
