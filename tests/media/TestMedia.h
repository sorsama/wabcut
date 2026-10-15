#pragma once

// Shared helpers for the media tests: generated clip paths, the frame-index barcode, process resource probes.

#include <wabcut/media/FrameHandle.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#if defined(_WIN32)
// clang-format off: psapi.h needs windows.h first
#include <windows.h>
#include <psapi.h>
// clang-format on
#elif defined(__linux__)
#include <fstream>
#include <malloc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

template <> struct Catch::StringMaker<wabcut::ColorInfo> {
    static std::string convert(const wabcut::ColorInfo& c) {
        return "{matrix " + std::to_string(static_cast<int>(c.matrix)) + ", range " +
               std::to_string(static_cast<int>(c.range)) + ", primaries " +
               std::to_string(static_cast<int>(c.primaries)) + ", transfer " +
               std::to_string(static_cast<int>(c.transfer)) + ", chroma " +
               std::to_string(static_cast<int>(c.chromaLocation)) + "}";
    }
};

namespace wabcut::media::test {

/// UTF-8 path of a clip made by tools/gen_test_media.py (the CTest fixture puts them in
/// WABCUT_TEST_MEDIA_DIR).
inline std::string mediaPath(std::string_view name) {
    const std::u8string p = (std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name).generic_u8string();
    return std::string(reinterpret_cast<const char*>(p.data()), p.size());
}

inline bool haveMedia(std::string_view name) {
    return std::filesystem::is_regular_file(std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name);
}

/// Scratch directory for a test (emptied first).
inline std::filesystem::path scratchDir(std::string_view name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "media-scratch" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

/// Hardware assertions are on by default on Windows; CI machines without a GPU set WABCUT_TEST_NO_GPU=1.
inline bool gpuExpected() {
#if defined(_WIN32)
    const char* noGpu = std::getenv("WABCUT_TEST_NO_GPU");
    return !(noGpu && std::string_view(noGpu) == "1");
#else
    return false;
#endif
}

/// Luma of pixel (x, y), scaled to 8 bits; -1 without CPU planes.
inline int luma8(const FrameHandle& f, int x, int y) {
    if (f.cpu() == nullptr || x < 0 || y < 0 || x >= f.width ||
        y >= f.height) { // no assertion: worker threads
        return -1;
    }
    const Plane& p = f.cpu()->planes[0];
    const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
    switch (f.format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
    case PixelFormat::YUV422P:
    case PixelFormat::YUV444P:
    case PixelFormat::YUVA420P:
        return row[x];
    case PixelFormat::P010: {
        std::uint16_t v = 0;
        std::memcpy(&v, row + 2 * x, 2);
        return v >> 8; // value lives in the high 10 bits
    }
    case PixelFormat::YUV420P10:
    case PixelFormat::YUV422P10:
    case PixelFormat::YUV444P10:
    case PixelFormat::YUVA444P10: {
        std::uint16_t v = 0;
        std::memcpy(&v, row + 2 * x, 2);
        return v >> 2;
    }
    case PixelFormat::RGBA8:
        return row[4 * x + 1]; // green
    default:
        return -1;
    }
}

/// Frame number written by gen_test_media.py: 16 cells of 32x32 pixels in the top-left corner, bit b at x =
/// 32b. Narrow clips only carry the cells that fit in their width.
inline std::int64_t frameIndexOf(const FrameHandle& f) {
    std::int64_t index = 0;
    for (int bit = 0; bit < 16 && bit * 32 + 16 < f.width; ++bit) {
        if (luma8(f, bit * 32 + 16, 16) > 128) {
            index |= std::int64_t{1} << bit;
        }
    }
    return index;
}

/// Committed private memory of this process in bytes (0 where unsupported).
inline std::size_t processPrivateBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof counters)) {
        return counters.PrivateUsage;
    }
    return 0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::size_t size = 0;
    std::size_t resident = 0;
    statm >> size >> resident;
    return resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

/// Bytes in live (allocated, not freed) blocks of the C heap that FFmpeg's av_malloc() draws from. Unlike
/// private bytes it ignores memory the heap or a GPU driver merely keeps around, so it is the signal for leak
/// tests. 0 where unsupported.
inline std::size_t heapLiveBytes() {
#if defined(_WIN32)
    HANDLE heap = GetProcessHeap(); // the UCRT allocates from the process heap
    if (!HeapLock(heap)) {
        return 0;
    }
    std::size_t total = 0;
    PROCESS_HEAP_ENTRY entry{};
    while (HeapWalk(heap, &entry)) {
        if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) != 0) {
            total += entry.cbData;
        }
    }
    HeapUnlock(heap);
    return total;
#elif defined(__linux__)
    return mallinfo2().uordblks;
#else
    return 0;
#endif
}

/// CPU time (user + kernel, all threads) consumed by this process so far, in seconds (0 where unsupported).
inline double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    const auto ticks = [](FILETIME f) { return (std::uint64_t{f.dwHighDateTime} << 32) | f.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7; // 100 ns units
#elif defined(__linux__)
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
#else
    return 0.0;
#endif
}

/// GPU memory this process has committed on every adapter (local + non-local segments, as the OS accounts it:
/// textures, decoder surfaces, CUDA allocations), in bytes. The leak signal for hardware decoding: unlike private
/// bytes it does not include the host-side pools a GPU driver grows and trims on its own. Windows (DXGI); -1
/// where unsupported.
long long processGpuMemoryBytes(); // GpuMemory.cpp

/// Open OS handles of this process (Windows; 0 elsewhere).
inline std::size_t processHandleCount() {
#if defined(_WIN32)
    DWORD count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return count;
#else
    return 0;
#endif
}

} // namespace wabcut::media::test
