#pragma once

// A file's modification time (std::filesystem::file_time_type) on the system clock. std::chrono::clock_cast (C++20)
// where the standard library has it (MSVC's, libstdc++), and file_clock::to_sys, which does the same conversion, where
// it does not yet (libc++ as of LLVM 23). Header-only; wabcut_target_defaults() puts src/platform/include on every
// Wabcut target's include path.

#include <chrono>
#include <filesystem>

namespace wabcut::platform {

inline auto toSystemClock(std::filesystem::file_time_type time) {
#if defined(_LIBCPP_VERSION)
    return std::chrono::file_clock::to_sys(time);
#else
    return std::chrono::clock_cast<std::chrono::system_clock>(time);
#endif
}

} // namespace wabcut::platform
