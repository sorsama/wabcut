# Sanitizer and fuzzing builds (C.10; docs/building.md, "Sanitizers and fuzzing"). Included by the root CMakeLists.txt
# before the dependencies, so fetched dependencies (the win-msvc-asan preset builds them from source) are instrumented
# the same way as Wabcut's own code.
#
#   WABCUT_SANITIZE=address  every target built with AddressSanitizer (MSVC /fsanitize=address; GCC/Clang also UBSan)
#   WABCUT_FUZZ=ON           the parser harnesses in tests/fuzz become libFuzzer executables; everything else gets the
#                            coverage instrumentation libFuzzer needs (MSVC: /fsanitize-coverage). Needs ASan.
#
# Without WABCUT_FUZZ the harnesses still build, on a small portable driver (tests/fuzz/FuzzMain.cpp) that ctest runs on
# the seeds plus a fixed number of deterministic mutations.

include_guard(GLOBAL)

set(WABCUT_SANITIZE "" CACHE STRING "Sanitizer build: empty (none) or 'address'")
set_property(CACHE WABCUT_SANITIZE PROPERTY STRINGS "" address)
option(WABCUT_FUZZ "Build the fuzz harnesses (tests/fuzz) as libFuzzer executables; needs WABCUT_SANITIZE=address" OFF)

if(WABCUT_SANITIZE STREQUAL "address")
    if(MSVC)
        add_compile_options(/fsanitize=address)
        # MSVC has no UBSan. The nearest: the STL's hardened preconditions (an index out of range, an empty optional or
        # expected dereferenced, ...) and tombstones in destroyed containers. Both keep the ABI, so the whole tree
        # (the fetched dependencies included) gets them. A failed check calls abort() instead of __fastfail, so ctest
        # and libFuzzer see it (libFuzzer then writes the input).
        add_compile_definitions(_MSVC_STL_HARDENING=1 _MSVC_STL_DESTRUCTOR_TOMBSTONES=1 _MSVC_STL_USE_ABORT_AS_DOOM_FUNCTION)
        # Those aborting checks make the optimizer report unreachable code (C4702) inside third-party headers
        # (nlohmann_json's binary_reader, inlined into ai/search), which /external:W0 does not silence: so
        # wabcut_target_defaults() turns C4702 off in this build (WabcutCompilerOptions.cmake). The regular builds keep it.
        # ASan cannot link incrementally (LNK4300 otherwise).
        add_link_options(/INCREMENTAL:NO)
        # The ASan runtime next to the executables, so they also run outside a developer prompt (ctest, the IDE).
        get_filename_component(_wabcut_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(_wabcut_dll clang_rt.asan_dynamic-x86_64.dll clang_rt.asan_dbg_dynamic-x86_64.dll)
            if(EXISTS "${_wabcut_cl_dir}/${_wabcut_dll}")
                file(COPY "${_wabcut_cl_dir}/${_wabcut_dll}" DESTINATION "${CMAKE_BINARY_DIR}/bin")
            endif()
        endforeach()
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
    message(STATUS "Wabcut: AddressSanitizer build")
elseif(NOT WABCUT_SANITIZE STREQUAL "")
    message(FATAL_ERROR "WABCUT_SANITIZE must be empty or 'address' (got '${WABCUT_SANITIZE}')")
endif()

if(WABCUT_FUZZ)
    if(NOT WABCUT_SANITIZE STREQUAL "address")
        message(FATAL_ERROR "WABCUT_FUZZ needs WABCUT_SANITIZE=address (libFuzzer runs with ASan)")
    endif()
    if(MSVC)
        # Coverage feedback everywhere (8-bit edge counters, comparison and division operands); the harnesses add
        # /fsanitize=fuzzer (tests/fuzz). Executables that are not fuzzers link the coverage runtime's default callbacks.
        add_compile_options(/fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
                            /fsanitize-coverage=trace-cmp /fsanitize-coverage=trace-div)
        add_link_options(/DEFAULTLIB:sancov.lib)
        set(WABCUT_FUZZER_COMPILE_OPTIONS /fsanitize=fuzzer)
        set(WABCUT_FUZZER_LINK_OPTIONS "")
    else()
        add_compile_options(-fsanitize=fuzzer-no-link)
        set(WABCUT_FUZZER_COMPILE_OPTIONS -fsanitize=fuzzer)
        set(WABCUT_FUZZER_LINK_OPTIONS -fsanitize=fuzzer)
    endif()
    message(STATUS "Wabcut: libFuzzer harnesses (tests/fuzz)")
endif()
