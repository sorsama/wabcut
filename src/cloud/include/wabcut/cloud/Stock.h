#pragma once

// Stock media (0.7, docs/cloud.md): search free videos and pictures on Pexels and Pixabay with the user's own API key, see where
// each comes from and under what licence, and download one into a project with its licence and credit recorded (model/Rights.h).
//
// Both services publish an API for exactly this and ask for the same in return - show where the results come from, and credit the
// maker where possible - which the panel does. Facts checked against the services' current documentation (https://www.pexels.com/api/documentation/,
// https://pixabay.com/api/docs/):
//   * Pexels: GET https://api.pexels.com/v1/videos/search and /v1/search, the key in the Authorization header, at most 80 results
//     per page, 200 requests an hour. The credit line asked for is "Photo/Video by <name> on Pexels" with a link.
//   * Pixabay: GET https://pixabay.com/api/ and /api/videos/, the key as the `key` parameter, 100 requests per 60 seconds,
//     search results must be cached for 24 hours, image addresses may be used to display results but not kept - files are
//     downloaded (this client never stores a remote address for later use).
// Network: everything goes through IHttpTransport (so proxies, TLS, the redacted request log, offline mode and the project's no-cloud
// lock apply). The key is sent to the service's API host only - never to a CDN. Downloads follow redirects only between the
// services' own hosts, accept https only, check the file's first bytes and write atomically.

#include <wabcut/ai/Http.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/cloud/Common.h>
#include <wabcut/cloud/RemoteStore.h>
#include <wabcut/model/Rights.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::cloud {

enum class StockProvider : std::uint8_t { Pexels, Pixabay };
enum class StockKind : std::uint8_t { Video, Photo };

std::string stockProviderId(StockProvider provider);          ///< "pexels" | "pixabay" (also the keychain entry: "stock:pexels")
std::string stockProviderName(StockProvider provider);        ///< "Pexels" | "Pixabay"
std::optional<StockProvider> stockProviderFromId(std::string_view id);

/// What a provider's licence says, in words for the panel (shown with every item).
struct StockLicence {
    std::string name;       ///< "Pexels License"
    std::string url;        ///< the licence page
    std::string summary;    ///< one or two sentences
    std::string attribution; ///< what the provider asks for
    std::string homepage;   ///< where "Photos provided by ..." links
    std::string keyUrl;     ///< where a free key is made
};
StockLicence stockLicence(StockProvider provider);

/// One downloadable rendition.
struct StockFile {
    std::string url;
    std::string quality;     ///< "hd", "large", "original", ...
    std::string extension;   ///< "mp4", "jpg", "png", "webm", "mov"
    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::uint64_t bytes = 0; ///< 0 when the service does not say
};

struct StockItem {
    StockProvider provider = StockProvider::Pexels;
    StockKind kind = StockKind::Video;
    std::string id;          ///< digits
    std::string description; ///< tags or alt text, cleaned
    std::string creator;
    std::string creatorUrl;  ///< https on the provider's site, or empty
    std::string pageUrl;     ///< the item's page on the provider's site
    int width = 0;
    int height = 0;
    double durationSeconds = 0.0;
    std::string thumbnailUrl; ///< a small picture of it (fetch with StockClient::thumbnail; never stored)
    std::vector<StockFile> files; ///< best (largest) first
};

struct StockPage {
    std::vector<StockItem> items;
    int page = 1;
    int perPage = 0;
    int total = 0;           ///< all matches the service reports
    bool hasMore = false;
    int quotaRemaining = -1; ///< requests left in the service's window, -1 when it did not say
};

struct StockQuery {
    StockProvider provider = StockProvider::Pexels;
    StockKind kind = StockKind::Video;
    std::string text;
    int page = 1;
    int perPage = 24;
    std::string orientation; ///< "", "landscape", "portrait", "square"
};

/// Why a query cannot be sent (empty text, over 100 bytes, not UTF-8, page out of range), or "".
std::string stockQueryProblem(const StockQuery& query);
/// The request for a query. The key goes where the provider wants it; nothing else carries it.
ai::HttpRequest buildStockSearch(const StockQuery& query, const ai::Secret& key);
/// Reads a search answer. Items without a usable file or from a host outside the provider's own are left out.
StoreResult<StockPage> parseStockSearch(StockProvider provider, StockKind kind, int status, std::string_view body, const ai::HttpHeaders& headers);

/// The hosts files and pictures may come from.
bool stockHostAllowed(StockProvider provider, std::string_view url);

/// "Video by Ann Lee on Pexels".
std::string stockCreditLine(const StockItem& item);
/// The rights record for a downloaded item (status cleared; the licence, the page, the credit and the date in words).
MediaRights stockRights(const StockItem& item, std::string_view downloadedOn);

/// The file's name in the project: "pexels-856973-hd.mp4".
std::string stockFileName(const StockItem& item, const StockFile& file);
/// True when the first bytes fit the extension (MP4 / MOV "ftyp", WebM EBML, JPEG, PNG).
bool stockMagicMatches(std::string_view extension, std::string_view firstBytes);

struct StockDownloaded {
    std::filesystem::path file;
    std::uint64_t bytes = 0;
};

class StockClient {
public:
    explicit StockClient(ai::IHttpTransport& transport, UnixClock clock = systemClock());

    /// A search; answers are kept 24 hours (Pixabay's rule, applied to both), so the same search again sends nothing.
    StoreResult<StockPage> search(const StockQuery& query, const ai::Secret& key, std::stop_token stop = {});
    /// Whether the key works (a three-result search that is not cached).
    StoreResult<void> checkKey(StockProvider provider, const ai::Secret& key, std::stop_token stop = {});
    /// The small picture of an item (JPEG / PNG / WebP, at most 4 MiB). No key is sent.
    StoreResult<std::string> thumbnail(const StockItem& item, std::stop_token stop = {});
    /// Downloads `file` into `folder` under stockFileName(); an existing file of that name is reused when its size matches.
    StoreResult<StockDownloaded> download(const StockItem& item, const StockFile& file, const std::filesystem::path& folder,
                                          const Progress& progress = {}, std::stop_token stop = {});

    static constexpr std::uint64_t kMaxDownloadBytes = std::uint64_t{4} << 30;

private:
    ai::IHttpTransport& m_transport;
    UnixClock m_clock;
    struct Cached {
        std::int64_t at = 0;
        StockPage page;
    };
    std::mutex m_mutex;
    std::map<std::string, Cached> m_cache;
};

} // namespace wabcut::cloud
