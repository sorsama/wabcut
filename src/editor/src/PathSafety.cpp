#include "PathSafety.h"

#include <wabcut/model/MediaLocator.h>

#include <QString>

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace wabcut::editor::paths {

namespace {

/// Unicode case folding and NFC: how the strictest file system (and macOS's default APFS) compares names.
std::string caseFoldedNfc(std::string_view utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()))
        .normalized(QString::NormalizationForm_C)
        .toCaseFolded()
        .toStdString();
}

std::filesystem::path::string_type folded(std::filesystem::path::string_type text) {
#if defined(_WIN32)
    if (!text.empty()) {
        // The simple upper-case mapping, as NTFS compares names.
        CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
    }
#elif defined(__APPLE__)
    text = caseFoldedNfc(text); // APFS (default): case- and normalisation-insensitive
#endif
    // Linux: case-sensitive file systems compare the bytes.
    return text;
}

std::filesystem::path normalised(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
    if (ec || p.empty()) {
        ec.clear();
        p = std::filesystem::absolute(path, ec);
        if (ec) {
            p = path;
        }
    }
    return p.lexically_normal();
}

} // namespace

std::filesystem::path::string_type key(const std::filesystem::path& path) {
    return folded(normalised(path).native());
}

std::string foldName(std::string_view name) {
    return pathToUtf8(std::filesystem::path(folded(pathFromUtf8(std::string(name)).native())));
}

std::string portableNameKey(std::string_view name) {
    return caseFoldedNfc(name);
}

std::string portableName(std::string_view name) {
    std::string out;
    out.reserve(name.size() + 1);
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out += u < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos ? '_' : c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    if (out.empty()) {
        return "_";
    }
    // A Windows device name, alone or before an extension ("CON", "con.wav", "LPT1.txt"), cannot be a file there.
    std::string stem = out.substr(0, out.find('.'));
    std::ranges::transform(stem, stem.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    static constexpr std::array<std::string_view, 4> kDevices{"CON", "PRN", "AUX", "NUL"};
    const bool numbered = stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9';
    if (numbered || std::ranges::find(kDevices, std::string_view(stem)) != kDevices.end()) {
        out.insert(stem.size(), "_");
    }
    return out;
}

bool isLink(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return false;
    }
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW(path.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return true; // a reparse point we cannot inspect: treat it as a link
    }
    FindClose(find);
    return data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
#else
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec));
#endif
}

bool isDirectChild(const std::filesystem::path& file, const std::filesystem::path& folder) {
    if (file.empty() || folder.empty() || isLink(folder) || isLink(file)) {
        return false;
    }
    return key(file.parent_path()) == key(folder) && key(folder / file.filename()) == key(file);
}

bool sameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    if (key(a) == key(b)) {
        return true;
    }
    std::error_code ec;
    const bool same = std::filesystem::equivalent(a, b, ec);
    return !ec && same;
}

std::optional<FileIdentity> fileIdentity(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    FileIdentity out;
#ifdef _WIN32
    // As the C++ library's equivalent() does: the file (or folder) opened for its attributes, links followed.
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    FILE_ID_INFO id{};
    bool ok = GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id) != FALSE;
    if (ok) {
        out.volume = id.VolumeSerialNumber;
        std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), out.id.begin());
    } else {
        BY_HANDLE_FILE_INFORMATION info{};
        ok = GetFileInformationByHandle(h, &info) != FALSE;
        out.volume = info.dwVolumeSerialNumber;
        const std::uint64_t index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        for (int i = 0; i < 8; ++i) {
            out.id[static_cast<std::size_t>(i)] = static_cast<unsigned char>(index >> (8 * i));
        }
    }
    CloseHandle(h);
    if (!ok) {
        return std::nullopt;
    }
#else
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    out.volume = static_cast<std::uint64_t>(st.st_dev);
    const auto inode = static_cast<std::uint64_t>(st.st_ino);
    for (int i = 0; i < 8; ++i) {
        out.id[static_cast<std::size_t>(i)] = static_cast<unsigned char>(inode >> (8 * i));
    }
#endif
    return out;
}

void FileSet::add(const std::filesystem::path& file) {
    if (file.empty()) {
        return; // sameFile() is never true for an empty path
    }
    m_files.push_back(file);
    m_keys.insert(key(file));
    m_identities.reset();
}

bool FileSet::contains(const std::filesystem::path& file) const {
    if (file.empty()) {
        return false;
    }
    if (m_keys.contains(key(file))) {
        return true;
    }
    const auto identity = fileIdentity(file); // equivalent() needs both files to exist
    if (!identity) {
        return false;
    }
    if (!m_identities) {
        m_identities.emplace();
        for (const std::filesystem::path& f : m_files) {
            if (const auto id = fileIdentity(f)) {
                m_identities->insert(*id);
            }
        }
    }
    return m_identities->contains(*identity);
}

bool isPlainName(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    if (name.find_first_of("/\\:") != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path p = pathFromUtf8(std::string(name));
    return p.filename() == p && !p.has_root_path();
}

bool isContainedRelative(const std::filesystem::path& relative) {
    if (relative.empty() || relative.has_root_name() || relative.has_root_directory() || relative.is_absolute()) {
        return false;
    }
    for (const std::filesystem::path& part : relative.lexically_normal()) {
        if (part == "..") {
            return false;
        }
    }
    const std::filesystem::path normal = relative.lexically_normal();
    return !normal.empty() && normal != ".";
}

} // namespace wabcut::editor::paths
