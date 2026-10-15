#pragma once

// Shared by the parser fuzz harnesses (tests/fuzz/README.md). Each harness defines LLVMFuzzerTestOneInput: libFuzzer's
// entry point, which the portable driver (FuzzMain.cpp) calls too. A harness returns normally for any input it
// rejects; a broken invariant aborts through WABCUT_FUZZ_CHECK so both drivers record the input as a crash.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace wabcut::fuzz {

inline std::string_view text(const std::uint8_t* data, std::size_t size) {
    return {reinterpret_cast<const char*>(data), size};
}

[[noreturn]] inline void fail(const char* file, int line, const char* condition, std::string_view detail) {
    const int shown = static_cast<int>(std::min<std::size_t>(detail.size(), 4000));
    std::fprintf(stderr, "\n==FUZZ== invariant broken at %s:%d: %s\n%.*s\n", file, line, condition, shown, detail.data());
    std::fflush(stderr);
    std::abort();
}

#ifdef _WIN32
/// The C runtime's invalid-parameter handler ends the process with __fastfail by default, which neither driver can
/// record; abort instead, so the input is written as a crash. (The hardened STL of win-msvc-asan aborts by itself.)
inline const bool kInvalidParameterAborts = [] {
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {
        std::fprintf(stderr, "\n==FUZZ== a C runtime or STL precondition failed\n");
        std::fflush(stderr);
        std::abort();
    });
    return true;
}();
#endif

} // namespace wabcut::fuzz

/// Aborts with `detail` (a std::string or string_view) when `condition` is false.
#define WABCUT_FUZZ_CHECK(condition, detail)                                                                          \
    do {                                                                                                              \
        if (!(condition)) {                                                                                           \
            ::wabcut::fuzz::fail(__FILE__, __LINE__, #condition, (detail));                                           \
        }                                                                                                             \
    } while (false)
