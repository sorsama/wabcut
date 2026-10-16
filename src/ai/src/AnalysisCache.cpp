#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/JsonAccess.h>

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/model/MediaLocator.h>

#include "JsonUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <thread>

namespace wabcut::ai {

using nlohmann::json;

namespace {

/// Entries stored by this process (AnalysisCache::writeCount).
std::atomic<std::uint64_t> g_writes{0};

constexpr std::string_view kEntrySchema = "wabcut.analysis";
constexpr std::string_view kSidecarSchema = "wabcut.analysis-sidecar";

/// A path component: [A-Za-z0-9._-] kept, everything else '_', at most 64 characters (plus a hash suffix when it
/// had to be changed or shortened, so different ids never share a name).
std::string component(std::string_view s) {
    std::string out;
    bool changed = s.empty();
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        out += ok ? c : '_';
        changed = changed || !ok;
    }
    if (out == "." || out == "..") {
        changed = true;
    }
    if (out.size() > 64 || changed) {
        out = out.substr(0, 48) + "~" + Sha256::hex(s).substr(0, 12);
    }
    return out;
}

json keyToJson(const AnalysisKey& key) {
    return {{"fingerprint", detail::hex64(key.sourceFingerprint)}, {"stream", key.streamIndex},
            {"analyzer", key.analyzerId}, {"modelVersion", key.modelVersion}, {"paramsHash", key.paramsHash}};
}

std::optional<AnalysisKey> keyFromJson(const json& j) {
    try {
        AnalysisKey key;
        const auto fp = detail::parseHex64(j.at("fingerprint").get<std::string>());
        if (!fp) {
            return std::nullopt;
        }
        key.sourceFingerprint = *fp;
        key.streamIndex = j.at("stream").get<int>();
        key.analyzerId = j.at("analyzer").get<std::string>();
        key.modelVersion = j.at("modelVersion").get<std::string>();
        key.paramsHash = j.at("paramsHash").get<std::string>();
        return key;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

/// A parsed entry file whose schema and version match, or nothing.
std::optional<json> readEntry(const std::filesystem::path& p) {
    const auto text = readFile(p);
    if (!text) {
        return std::nullopt;
    }
    json doc = json::parse(*text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || jsonStringOr(doc, "schema") != kEntrySchema ||
        jsonIntOr(doc, "version", 0) != AnalysisCache::kFormatVersion || !doc.contains("key") || !doc.contains("payload")) {
        return std::nullopt;
    }
    return std::make_optional(std::move(doc));
}

std::string uniqueSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return std::to_string(tid) + "-" + std::to_string(counter.fetch_add(1));
}

} // namespace

std::string hashParams(const std::map<std::string, std::string>& params) {
    Sha256 h;
    for (const auto& [k, v] : params) { // std::map: sorted, so the form is canonical
        h.update(std::to_string(k.size()) + ":" + k + "=" + std::to_string(v.size()) + ":" + v + ";");
    }
    return h.finishHex().substr(0, 16);
}

AnalysisCache::AnalysisCache(std::filesystem::path root) : m_root(std::move(root)) {}

std::filesystem::path AnalysisCache::defaultRoot() { return userCacheDir() / "analysis"; }

std::filesystem::path AnalysisCache::pathFor(const AnalysisKey& key) const {
    return m_root / ("v" + std::to_string(kFormatVersion)) / detail::hex64(key.sourceFingerprint) /
           std::to_string(key.streamIndex) / component(key.analyzerId) /
           (component(key.modelVersion) + "-" + component(key.paramsHash) + ".json");
}

std::expected<std::optional<std::string>, Error> AnalysisCache::get(const AnalysisKey& key) const {
    const std::filesystem::path p = pathFor(key);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        return std::optional<std::string>{};
    }
    const auto doc = readEntry(p);
    if (!doc) {
        std::filesystem::remove(p, ec); // damaged or from another format version: never trust it
        return std::optional<std::string>{};
    }
    const auto stored = keyFromJson(doc->at("key"));
    if (!stored || *stored != key) {
        return std::optional<std::string>{}; // a different key that maps to the same file name
    }
    return std::optional<std::string>(doc->at("payload").dump(-1, ' ', false, json::error_handler_t::replace));
}

std::expected<void, Error> AnalysisCache::put(const AnalysisKey& key, std::string_view payloadJson) {
    if (key.sourceFingerprint == 0 || key.analyzerId.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "analysis key needs a source fingerprint and an analyser"});
    }
    json payload = json::parse(payloadJson, nullptr, false);
    if (payload.is_discarded()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "analysis payload is not JSON"});
    }
    const json doc{{"schema", kEntrySchema},
                   {"version", kFormatVersion},
                   {"key", keyToJson(key)},
                   {"createdAt", detail::utcNowIso8601()},
                   {"payload", std::move(payload)}};
    const std::filesystem::path p = pathFor(key);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot create " + pathToUtf8(p.parent_path()) + ": " + ec.message()});
    }
    std::filesystem::path temp = p;
    temp += ".tmp-" + uniqueSuffix();
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << doc.dump(-1, ' ', false, json::error_handler_t::replace);
        if (!out.flush()) {
            std::filesystem::remove(temp, ec);
            return std::unexpected(Error{ErrorCode::IoError, "cannot write " + pathToUtf8(temp)});
        }
    }
    std::filesystem::rename(temp, p, ec); // replaces an existing entry
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error{ErrorCode::IoError, "cannot store " + pathToUtf8(p) + ": " + ec.message()});
    }
    g_writes.fetch_add(1, std::memory_order_relaxed);
    return {};
}

std::uint64_t AnalysisCache::writeCount() { return g_writes.load(std::memory_order_relaxed); }

std::optional<AnalysisCache::Entry> AnalysisCache::newest(std::uint64_t fingerprint, int streamIndex,
                                                         std::string_view analyzerId) const {
    const std::filesystem::path dir =
        pathFor(AnalysisKey{fingerprint, streamIndex, std::string(analyzerId), "m", "p"}).parent_path();
    std::error_code ec;
    std::optional<Entry> best;
    std::filesystem::file_time_type bestTime{};
    std::filesystem::path bestPath;
    for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
        if (file.path().extension() != ".json") {
            continue;
        }
        const auto time = file.last_write_time(ec);
        if (best && (time < bestTime || (time == bestTime && file.path() < bestPath))) {
            continue;
        }
        const auto doc = readEntry(file.path());
        if (!doc) {
            continue;
        }
        const auto key = keyFromJson(doc->at("key"));
        if (!key || key->sourceFingerprint != fingerprint || key->streamIndex != streamIndex || key->analyzerId != analyzerId) {
            continue;
        }
        best = Entry{*key, doc->at("payload").dump(-1, ' ', false, json::error_handler_t::replace)};
        bestTime = time;
        bestPath = file.path();
    }
    return best;
}

std::expected<void, Error> AnalysisCache::remove(const AnalysisKey& key) {
    std::error_code ec;
    std::filesystem::remove(pathFor(key), ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, ec.message()});
    }
    return {};
}

std::expected<std::size_t, Error> AnalysisCache::invalidateSource(std::uint64_t fingerprint) {
    const std::filesystem::path dir = m_root / ("v" + std::to_string(kFormatVersion)) / detail::hex64(fingerprint);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return std::size_t{0};
    }
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        files += entry.is_regular_file(ec) ? 1 : 0;
    }
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot remove " + pathToUtf8(dir) + ": " + ec.message()});
    }
    return files;
}

std::expected<std::string, Error> AnalysisCache::exportSidecar(std::uint64_t fingerprint) const {
    const std::filesystem::path dir = m_root / ("v" + std::to_string(kFormatVersion)) / detail::hex64(fingerprint);
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
    }
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot list " + pathToUtf8(dir) + ": " + ec.message()});
    }
    std::ranges::sort(files); // deterministic order
    json entries = json::array();
    for (const std::filesystem::path& p : files) {
        auto doc = readEntry(p);
        if (!doc) {
            continue;
        }
        const auto key = keyFromJson(doc->at("key"));
        if (!key || key->sourceFingerprint != fingerprint) {
            continue;
        }
        entries.push_back({{"key", std::move((*doc)["key"])},
                           {"createdAt", doc->value("createdAt", std::string{})},
                           {"payload", std::move((*doc)["payload"])}});
    }
    const json sidecar{{"schema", kSidecarSchema},
                       {"version", kFormatVersion},
                       {"source", {{"fingerprint", detail::hex64(fingerprint)}}},
                       {"entries", std::move(entries)}};
    return sidecar.dump(2, ' ', false, json::error_handler_t::replace);
}

} // namespace wabcut::ai
