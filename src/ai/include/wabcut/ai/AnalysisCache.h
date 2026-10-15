#pragma once

// Source-scoped analysis cache (docs/ai.md, "Analysis cache is source-scoped"): results of analysers such as
// transcription, keyed by (source fingerprint, stream, analyser id + model version + parameters hash), stored as
// versioned JSON files in the user cache and therefore shared by every project that uses the same media. A
// source's entries can be exported as one open JSON sidecar.
//
// Layout: <root>/v1/<fingerprint hex>/<stream>/<analyser>/<model version>-<params hash>.json, each file
//   {"schema":"wabcut.analysis","version":1,"key":{...},"createdAt":"...","payload":{...}}
// Entries are written atomically (temporary file + rename). A file whose key, schema or version does not match
// is a miss, and so is a damaged one (which is removed). A changed file gets a new fingerprint, so its old
// entries are simply never found again; invalidateSource() reclaims them.

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut::ai {

struct AnalysisKey {
    std::uint64_t sourceFingerprint = 0; ///< media::sourceFingerprint
    int streamIndex = -1;
    std::string analyzerId;   ///< e.g. "transcript"
    std::string modelVersion; ///< e.g. "whisper.cpp-1.9.5/base@60ed5bc3..." (any text; sanitised for the path)
    std::string paramsHash;   ///< hashParams() of every setting that changes the result

    bool operator==(const AnalysisKey&) const = default;
};

/// Stable 16-hex-digit hash of key/value settings (SHA-256 of the canonical, sorted form).
std::string hashParams(const std::map<std::string, std::string>& params);

class AnalysisCache {
public:
    static constexpr int kFormatVersion = 1;

    explicit AnalysisCache(std::filesystem::path root);
    /// <user cache>/analysis (see userCacheDir() in ModelStore.h).
    static std::filesystem::path defaultRoot();
    /// How many entries this process has stored (any root): a lookup that remembers a miss checks again when it
    /// changes, so a transcript made a moment ago is found.
    static std::uint64_t writeCount();

    const std::filesystem::path& root() const { return m_root; }
    std::filesystem::path pathFor(const AnalysisKey& key) const;

    /// The payload (JSON text) stored for `key`, or nothing. Errors: IoError only for an unreadable cache.
    std::expected<std::optional<std::string>, Error> get(const AnalysisKey& key) const;
    /// Stores `payloadJson` (must be valid JSON) for `key`, replacing an earlier entry. Errors: InvalidArgument
    /// (not JSON, key without fingerprint or analyser), IoError.
    std::expected<void, Error> put(const AnalysisKey& key, std::string_view payloadJson);
    std::expected<void, Error> remove(const AnalysisKey& key);

    /// The newest valid entry of one analyser for a source stream, whatever its model version and parameters (e.g.
    /// "any transcript of this audio"): its key and payload, or nothing.
    struct Entry {
        AnalysisKey key;
        std::string payload;
    };
    std::optional<Entry> newest(std::uint64_t fingerprint, int streamIndex, std::string_view analyzerId) const;
    /// Removes every entry of a source; returns how many files went.
    std::expected<std::size_t, Error> invalidateSource(std::uint64_t fingerprint);

    /// Every entry of a source as one document:
    ///   {"schema":"wabcut.analysis-sidecar","version":1,"source":{"fingerprint":"..."},
    ///    "entries":[{"key":{...},"createdAt":"...","payload":{...}}, ...]}
    std::expected<std::string, Error> exportSidecar(std::uint64_t fingerprint) const;

private:
    std::filesystem::path m_root;
};

} // namespace wabcut::ai
