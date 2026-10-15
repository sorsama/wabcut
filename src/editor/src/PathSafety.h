#pragma once

// Path checks for code that deletes or writes files whose paths come from project files, indexes or caches (Codex
// wave-3 P1-1, P1-4): such a path is untrusted. A file may be deleted only when Wabcut made it inside a folder it
// manages; it must not be (or reach through a link) an original.
//
// Two notions of "the same name" (I.7 portability audit):
//   * this computer's file system (key, foldName): Windows ignores case (NTFS's simple upper-case mapping), macOS's
//     default APFS ignores case and Unicode normalisation, Linux compares bytes;
//   * every file system a capsule may travel to (portableNameKey, portableName): names created side by side for another
//     computer must differ in more than case or normalisation, and be valid on Windows (no reserved device names, no
//     < > : " | ? * or control characters, no trailing dot or space).

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::editor::paths {

/// The comparison key of a path on this computer: absolute and lexically normal, with links resolved where the path
/// exists (weakly_canonical), folded like the file system compares names (foldName). Two paths with the same key name
/// the same entry. (A case-sensitive macOS volume is compared as if it ignored case: the conservative side.)
std::filesystem::path::string_type key(const std::filesystem::path& path);

/// `name` (UTF-8) as this computer's file system compares names: case-folded on Windows, case-folded and NFC-normalised
/// on macOS, unchanged on Linux.
std::string foldName(std::string_view name);

/// `name` (UTF-8) as the strictest of Windows, macOS and Linux compares names: Unicode case-folded and NFC-normalised.
/// Names a capsule (or any folder meant to move between computers) holds side by side must have different keys.
std::string portableNameKey(std::string_view name);

/// `name` (one file or folder name, UTF-8) made valid on Windows, macOS and Linux: separators, < > : " | ? * and control
/// characters become '_', trailing dots and spaces are dropped, a Windows device name (CON, PRN, AUX, NUL, COM1-9,
/// LPT1-9, also with an extension) gets '_' after its stem, and an empty result becomes "_".
std::string portableName(std::string_view name);

/// A symbolic link or a junction (a directory mount point). Other reparse points (cloud placeholders, dedup) are
/// ordinary files and folders.
bool isLink(const std::filesystem::path& path);

/// `file` is directly inside `folder` (by key), and neither the folder nor the file is a link.
bool isDirectChild(const std::filesystem::path& file, const std::filesystem::path& folder);

/// `a` and `b` are the same file: the same key, or one file system entity (a hard link, a symlink or a junction to
/// it; std::filesystem::equivalent). False when either does not exist and the keys differ.
bool sameFile(const std::filesystem::path& a, const std::filesystem::path& b);

/// What std::filesystem::equivalent compares: the volume and the file's id there. Nothing when `path` does not exist.
struct FileIdentity {
    std::uint64_t volume = 0;
    std::array<unsigned char, 16> id{};
    auto operator<=>(const FileIdentity&) const = default;
};
std::optional<FileIdentity> fileIdentity(const std::filesystem::path& path);

/// Files (a project's originals) that answer "is `file` the same file as one of them" (sameFile against each) with one
/// key and at most one identity lookup per question, where asking sameFile of every member costs a key and an
/// equivalent() each (P.1: that made a proxy refresh O(media^2) file opens). The members' identities are read once,
/// on the first question that needs them.
class FileSet {
public:
    void add(const std::filesystem::path& file);
    bool contains(const std::filesystem::path& file) const;

private:
    std::vector<std::filesystem::path> m_files;
    std::set<std::filesystem::path::string_type> m_keys;
    mutable std::optional<std::set<FileIdentity>> m_identities;
};

/// A bare file name (UTF-8): not empty, no separator, no drive or root, not "." or "..".
bool isPlainName(std::string_view name);

/// A relative path that stays inside the folder it is relative to: no root or drive, no ".." component, not empty.
bool isContainedRelative(const std::filesystem::path& relative);

} // namespace wabcut::editor::paths
