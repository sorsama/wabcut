#include <wabcut/model/MediaLocator.h>

#include <array>
#include <chrono>
#include <fstream>
#include <system_error>
#include <vector>

namespace wabcut {

namespace {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

void fnv1a(std::uint64_t& hash, const char* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= kFnvPrime;
    }
}

std::string toHex(std::uint64_t v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[v & 0xFu];
        v >>= 4;
    }
    return out;
}

bool readAt(std::ifstream& in, std::uint64_t offset, std::vector<char>& buffer, std::size_t size) {
    buffer.resize(size);
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(buffer.data(), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(in.gcount()) == size;
}

} // namespace

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path pathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::expected<SourceFingerprint, Error> fingerprintFile(const std::filesystem::path& file) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read the size of " + pathToUtf8(file)});
    }
    const auto written = std::filesystem::last_write_time(file, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read the modification time of " + pathToUtf8(file)});
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot open " + pathToUtf8(file)});
    }

    std::uint64_t hash = kFnvOffset;
    std::vector<char> buffer;
    const std::uint64_t total = size;
    if (total <= 2 * kFingerprintSampleBytes) {
        if (!readAt(in, 0, buffer, static_cast<std::size_t>(total))) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(file)});
        }
        fnv1a(hash, buffer.data(), buffer.size());
    } else {
        if (!readAt(in, 0, buffer, kFingerprintSampleBytes)) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(file)});
        }
        fnv1a(hash, buffer.data(), buffer.size());
        if (!readAt(in, total - kFingerprintSampleBytes, buffer, kFingerprintSampleBytes)) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(file)});
        }
        fnv1a(hash, buffer.data(), buffer.size());
    }

    SourceFingerprint fp;
    fp.size = static_cast<std::int64_t>(size);
#if defined(_LIBCPP_VERSION)
    // libc++ has no std::chrono::clock_cast yet; its file clock converts to the system clock itself.
    const auto system = std::chrono::file_clock::to_sys(written);
#else
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
#endif
    fp.mtimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count();
    fp.sampleHash = toHex(hash);
    return fp;
}

std::string relativeMediaPath(const std::filesystem::path& projectFile, const std::filesystem::path& media) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::absolute(projectFile, ec).parent_path();
    if (ec) {
        return {};
    }
    const std::filesystem::path target = std::filesystem::absolute(media, ec);
    if (ec || target.root_name() != base.root_name()) {
        return {};
    }
    const std::filesystem::path relative = target.lexically_relative(base);
    if (relative.empty()) {
        return {};
    }
    const std::u8string generic = relative.generic_u8string();
    return {reinterpret_cast<const char*>(generic.data()), generic.size()};
}

std::optional<std::filesystem::path> resolveMediaPath(const MediaLocator& locator,
                                                      const std::filesystem::path& projectFile) {
    std::error_code ec;
    if (!locator.path.empty()) {
        const std::filesystem::path absolutePath = pathFromUtf8(locator.path);
        if (std::filesystem::is_regular_file(absolutePath, ec)) {
            return absolutePath;
        }
    }
    if (!locator.relativePath.empty()) {
        const std::filesystem::path candidate =
            (std::filesystem::absolute(projectFile, ec).parent_path() / pathFromUtf8(locator.relativePath))
                .lexically_normal();
        if (!ec && std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace wabcut
