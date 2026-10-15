#include "Background.h"
#include "FfmpegUtil.h"

#include <wabcut/media/Fingerprint.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <tuple>

namespace wabcut::media {

namespace {

constexpr std::size_t kSampleBytes = 64 * 1024;

struct MemoKey {
    std::string path;
    std::int64_t size = 0;
    std::int64_t mtimeNs = 0;
    auto operator<=>(const MemoKey&) const = default;
};

class FingerprintMemo {
public:
    std::optional<std::uint64_t> find(const MemoKey& key) {
        std::lock_guard lock(m_mutex);
        const auto it = m_entries.find(key);
        return it == m_entries.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
    }
    void store(MemoKey key, std::uint64_t fingerprint) {
        std::lock_guard lock(m_mutex);
        if (m_entries.size() >= 4096) {
            m_entries.clear(); // bounded; recomputing is cheap
        }
        m_entries.emplace(std::move(key), fingerprint);
    }

private:
    std::mutex m_mutex;
    std::map<MemoKey, std::uint64_t> m_entries;
};

FingerprintMemo& memo() {
    static auto* instance = new FingerprintMemo(); // never destroyed: background jobs may run during exit
    return *instance;
}

template <typename T> std::uint64_t mix(std::uint64_t hash, const T& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    return detail::fnv1a({bytes, sizeof(T)}, hash);
}

} // namespace

std::expected<std::uint64_t, Error> sourceFingerprint(const std::string& utf8Path) {
    const std::filesystem::path path = detail::pathFromUtf8(utf8Path);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(
            Error{ec == std::errc::no_such_file_or_directory ? ErrorCode::NotFound : ErrorCode::IoError,
                  "Cannot stat " + utf8Path + ": " + ec.message()});
    }
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot stat " + utf8Path + ": " + ec.message()});
    }
    // Nanoseconds since 1970 (file_clock's own epoch is 1601 on Windows, whose nanoseconds overflow int64).
    const std::int64_t mtimeNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::clock_cast<std::chrono::system_clock>(mtime).time_since_epoch())
            .count();
    MemoKey key{utf8Path, static_cast<std::int64_t>(size), mtimeNs};
    if (const auto known = memo().find(key)) {
        return *known;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + utf8Path});
    }
    std::array<char, kSampleBytes> buffer{};
    const auto readAt = [&](std::uint64_t offset, std::size_t count) -> std::expected<std::size_t, Error> {
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(buffer.data(), static_cast<std::streamsize>(count));
        if (static_cast<std::size_t>(in.gcount()) != count) {
            return std::unexpected(Error{ErrorCode::IoError, "Short read from " + utf8Path});
        }
        return count;
    };

    std::uint64_t hash = mix(0xcbf29ce484222325ULL, static_cast<std::uint64_t>(size));
    hash = mix(hash, mtimeNs);
    const std::size_t head = static_cast<std::size_t>(std::min<std::uint64_t>(size, kSampleBytes));
    auto got = readAt(0, head);
    if (!got) {
        return std::unexpected(got.error());
    }
    hash = detail::fnv1a({reinterpret_cast<const std::uint8_t*>(buffer.data()), head}, hash);
    if (size > kSampleBytes) {
        // The last 64 KiB (overlapping the head for files shorter than 128 KiB).
        const std::uint64_t tailOffset = size - kSampleBytes;
        got = readAt(tailOffset, kSampleBytes);
        if (!got) {
            return std::unexpected(got.error());
        }
        hash = detail::fnv1a({reinterpret_cast<const std::uint8_t*>(buffer.data()), kSampleBytes}, hash);
    }
    if (hash == 0) {
        hash = 1; // 0 is reserved for "unknown"
    }
    memo().store(std::move(key), hash);
    return hash;
}

} // namespace wabcut::media
