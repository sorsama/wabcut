# macOS with Homebrew's LLVM (clang 20+ and its libc++), for Wabcut and, through the arm64-osx-llvm triplet, for every
# vcpkg port: one compiler and one C++ standard library in the process. Apple Clang (Xcode 16) ships a libc++ without
# std::jthread / std::stop_token and floating-point std::from_chars, which Wabcut uses (docs/release.md, "Portability").
#
#   LLVM_ROOT   the LLVM prefix, e.g. $(brew --prefix llvm). vcpkg passes it into port builds with VCPKG_KEEP_ENV_VARS.
#
# CI-ready, not run locally (.github/workflows/unix-full.yml, core.yml).

set(_wabcut_llvm "$ENV{LLVM_ROOT}")
if(NOT _wabcut_llvm)
    foreach(candidate /opt/homebrew/opt/llvm /usr/local/opt/llvm)
        if(EXISTS "${candidate}/bin/clang++")
            set(_wabcut_llvm "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${_wabcut_llvm}/bin/clang++")
    message(FATAL_ERROR "macos-llvm.cmake: set LLVM_ROOT to an LLVM 20+ prefix (brew install llvm; LLVM_ROOT=$(brew --prefix llvm))")
endif()

# A native build (no CMAKE_SYSTEM_NAME: not cross-compiling); architecture and deployment target from the triplet when
# vcpkg builds a port, else what the macos-clang preset uses.
if(DEFINED VCPKG_OSX_ARCHITECTURES AND VCPKG_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "${VCPKG_OSX_ARCHITECTURES}" CACHE STRING "")
elseif(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
endif()
if(DEFINED VCPKG_OSX_DEPLOYMENT_TARGET AND VCPKG_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${VCPKG_OSX_DEPLOYMENT_TARGET}" CACHE STRING "")
elseif(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "")
endif()

set(CMAKE_C_COMPILER "${_wabcut_llvm}/bin/clang" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_wabcut_llvm}/bin/clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_wabcut_llvm}/bin/llvm-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_wabcut_llvm}/bin/llvm-ranlib" CACHE FILEPATH "" FORCE)
# Wabcut links LLVM's own libc++ (below), not the system's: libc++'s availability markup, which describes the system
# library (floating-point std::from_chars "introduced in macOS 26.0", std::to_chars in 13.3), does not apply.
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -D_LIBCPP_DISABLE_AVAILABILITY")
# Link LLVM's libc++ (not the SDK's older one) and find it at run time.
set(_wabcut_libcxx "-L${_wabcut_llvm}/lib/c++ -L${_wabcut_llvm}/lib -Wl,-rpath,${_wabcut_llvm}/lib/c++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_wabcut_libcxx}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_wabcut_libcxx}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_wabcut_libcxx}")
