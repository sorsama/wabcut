# CMake build of RNNoise 0.2 for the Wabcut overlay port (replaces upstream's autotools build; same sources).
cmake_minimum_required(VERSION 3.20)
project(rnnoise VERSION 0.2 LANGUAGES C)

add_library(rnnoise
    src/celt_lpc.c
    src/denoise.c
    src/kiss_fft.c
    src/nnet.c
    src/nnet_default.c
    src/parse_lpcnet_weights.c
    src/pitch.c
    src/rnn.c
    src/rnnoise_data.c
    src/rnnoise_tables.c)
target_include_directories(rnnoise
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE src)
target_compile_definitions(rnnoise PRIVATE RNNOISE_BUILD)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(rnnoise PRIVATE DLL_EXPORT)
endif()
if(MSVC)
    target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif()
if(UNIX)
    target_link_libraries(rnnoise PRIVATE m)
endif()

# x86-64: run-time dispatch to SSE4.1 / AVX2 kernels, as upstream's --enable-x86-rtcd. SSE2 is the x86-64 baseline;
# MSVC does not define __SSE2__, which vec.h needs to pick its SIMD path.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|x64)$")
    target_sources(rnnoise PRIVATE src/x86/x86_dnn_map.c src/x86/x86cpu.c src/x86/nnet_sse4_1.c src/x86/nnet_avx2.c)
    target_compile_definitions(rnnoise PRIVATE RNN_ENABLE_X86_RTCD)
    if(MSVC)
        target_compile_definitions(rnnoise PRIVATE __SSE2__)
        set_source_files_properties(src/x86/nnet_sse4_1.c PROPERTIES COMPILE_DEFINITIONS OPUS_X86_MAY_HAVE_SSE4_1)
        set_source_files_properties(src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS /arch:AVX2)
    else()
        target_compile_definitions(rnnoise PRIVATE CPU_INFO_BY_ASM)
        set_source_files_properties(src/x86/nnet_sse4_1.c PROPERTIES COMPILE_OPTIONS -msse4.1)
        set_source_files_properties(src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx;-mfma;-mavx2")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS rnnoise EXPORT unofficial-rnnoise-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/rnnoise.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT unofficial-rnnoise-targets NAMESPACE unofficial::rnnoise::
    DESTINATION share/unofficial-rnnoise FILE unofficial-rnnoise-config.cmake)
