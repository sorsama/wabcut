#pragma once

// An extension's own key-value store (wabcut.storage in scripts): JSON values under short keys, kept in one file per
// extension in the user's data folder. The editor owns the file; the script host keeps a copy and sends every change
// over, and the editor checks the bounds again (the host runs untrusted code).

#include <wabcut/model/Error.h>
#include <wabcut/script/Manifest.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut::script {

inline constexpr std::size_t kMaxStorageKeys = 256;
inline constexpr std::size_t kMaxStorageKeyBytes = 128;
inline constexpr std::size_t kMaxStorageValueBytes = 64 * 1024; ///< one value, as compact JSON
inline constexpr std::size_t kMaxStorageBytes = 1024 * 1024;    ///< every key and value

/// Why `key` = `value` cannot be stored in `values` (an object of the current entries), or nullopt.
std::optional<std::string> checkStorageEntry(const Json& values, std::string_view key, const Json& value);

class KeyValueStore {
public:
    KeyValueStore() = default;
    /// Reads `file` (missing or damaged: empty). The values are re-checked against the bounds.
    static KeyValueStore load(const std::filesystem::path& file);
    std::expected<void, Error> save() const;

    const Json& values() const { return m_values; }
    std::optional<Json> get(std::string_view key) const;
    /// Errors: InvalidArgument with the bound that was hit.
    std::expected<void, Error> set(std::string_view key, Json value);
    bool remove(std::string_view key);
    void clear();
    bool dirty() const { return m_dirty; }
    /// Every key and value, as compact JSON (what kMaxStorageBytes bounds).
    std::size_t bytes() const { return m_bytes; }

private:
    std::filesystem::path m_file;
    Json m_values = Json::object();
    // A running total and each entry's size, so a write costs what it stores, never a pass over the store (S5-P2-1).
    std::map<std::string, std::size_t, std::less<>> m_entryBytes;
    std::size_t m_bytes = 0;
    mutable bool m_dirty = false;
};

/// <storage folder>/<extension id>.json
std::filesystem::path storageFileFor(const std::filesystem::path& storageFolder, std::string_view extensionId);

} // namespace wabcut::script
