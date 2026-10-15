#include <wabcut/script/Storage.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <fstream>
#include <sstream>

namespace wabcut::script {

namespace fs = std::filesystem;

namespace {

std::size_t entryBytes(std::string_view key, const Json& value) { return key.size() + value.dump().size(); }

std::size_t totalBytes(const Json& values) {
    std::size_t total = 0;
    for (const auto& [key, value] : values.items()) {
        total += entryBytes(key, value);
    }
    return total;
}

/// The bounds for `key` = `value` (`valueBytes` of JSON) in a store of `keys` entries and `total` bytes, where `key`
/// now takes `replacedBytes` (nullopt: a new key).
std::optional<std::string> checkEntry(std::size_t keys, std::size_t total, std::optional<std::size_t> replacedBytes,
                                      std::string_view key, const Json& value, std::size_t valueBytes) {
    if (key.empty() || key.size() > kMaxStorageKeyBytes) {
        return "a storage key must be 1 to " + std::to_string(kMaxStorageKeyBytes) + " bytes long";
    }
    if (value.is_discarded() || value.is_binary()) {
        return std::string("a stored value must be JSON");
    }
    if (valueBytes > kMaxStorageValueBytes) {
        return "a stored value may be at most " + std::to_string(kMaxStorageValueBytes / 1024) + " KiB of JSON";
    }
    if (!replacedBytes && keys >= kMaxStorageKeys) {
        return "an extension may store at most " + std::to_string(kMaxStorageKeys) + " keys";
    }
    if (total - replacedBytes.value_or(0) + key.size() + valueBytes > kMaxStorageBytes) {
        return "an extension may store at most " + std::to_string(kMaxStorageBytes / 1024) + " KiB";
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> checkStorageEntry(const Json& values, std::string_view key, const Json& value) {
    const auto it = values.find(std::string(key));
    const std::optional<std::size_t> replaced =
        it == values.end() ? std::nullopt : std::optional<std::size_t>(entryBytes(key, *it));
    const std::size_t valueBytes = value.is_discarded() || value.is_binary() ? 0 : value.dump().size();
    return checkEntry(values.size(), totalBytes(values), replaced, key, value, valueBytes);
}

KeyValueStore KeyValueStore::load(const fs::path& file) {
    KeyValueStore store;
    store.m_file = file;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return store;
    }
    std::string text(kMaxStorageBytes * 2 + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    if (text.size() > kMaxStorageBytes * 2 || !io::jsonNestingWithin(text)) {
        return store;
    }
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object()) {
        return store;
    }
    for (const auto& [key, value] : json.items()) {
        (void)store.set(key, value); // an entry past the bounds is dropped
    }
    store.m_dirty = false;
    return store;
}

std::expected<void, Error> KeyValueStore::save() const {
    if (m_file.empty()) {
        return {};
    }
    std::error_code ec;
    fs::create_directories(m_file.parent_path(), ec);
    auto written = io::writeFileAtomically(m_file, m_values.dump());
    if (written) {
        m_dirty = false;
    }
    return written;
}

std::optional<Json> KeyValueStore::get(std::string_view key) const {
    const auto it = m_values.find(std::string(key));
    if (it == m_values.end()) {
        return std::nullopt;
    }
    return std::optional<Json>(std::in_place, *it);
}

std::expected<void, Error> KeyValueStore::set(std::string_view key, Json value) {
    const std::size_t valueBytes = value.is_discarded() || value.is_binary() ? 0 : value.dump().size();
    const auto old = m_entryBytes.find(key);
    const std::optional<std::size_t> replaced =
        old == m_entryBytes.end() ? std::nullopt : std::optional<std::size_t>(old->second);
    if (auto why = checkEntry(m_values.size(), m_bytes, replaced, key, value, valueBytes)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(*why)});
    }
    m_bytes = m_bytes - replaced.value_or(0) + key.size() + valueBytes;
    m_entryBytes.insert_or_assign(std::string(key), key.size() + valueBytes);
    m_values[std::string(key)] = std::move(value);
    m_dirty = true;
    return {};
}

bool KeyValueStore::remove(std::string_view key) {
    const bool removed = m_values.erase(std::string(key)) > 0;
    if (const auto it = m_entryBytes.find(key); it != m_entryBytes.end()) {
        m_bytes -= it->second;
        m_entryBytes.erase(it);
    }
    m_dirty = m_dirty || removed;
    return removed;
}

void KeyValueStore::clear() {
    m_dirty = m_dirty || !m_values.empty();
    m_values = Json::object();
    m_entryBytes.clear();
    m_bytes = 0;
}

fs::path storageFileFor(const fs::path& storageFolder, std::string_view extensionId) {
    return storageFolder / pathFromUtf8(std::string(extensionId) + ".json");
}

} // namespace wabcut::script
