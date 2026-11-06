// Stock media: Pexels and Pixabay (Stock.h).

#include <wabcut/cloud/Stock.h>

#include "FileUtil.h"

#include <wabcut/ai/Redirects.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/TitleOps.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>

namespace wabcut::cloud {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::size_t kMaxItems = 100;
constexpr std::size_t kMaxAnswerBytes = 8u * 1024u * 1024u;
constexpr std::uint64_t kMaxThumbBytes = 4u * 1024u * 1024u;
constexpr std::int64_t kCacheSeconds = 24 * 3600;
constexpr std::size_t kMaxCached = 64;

std::vector<std::string> allowedHosts(StockProvider provider) {
    if (provider == StockProvider::Pexels) {
        return {"videos.pexels.com", "images.pexels.com", "www.pexels.com"};
    }
    return {"cdn.pixabay.com", "pixabay.com"};
}

std::string clip(std::string value, std::size_t limit) {
    if (value.size() > limit) {
        value.resize(limit);
        while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xC0u) == 0x80u) {
            value.pop_back();
        }
        if (!value.empty() && static_cast<unsigned char>(value.back()) >= 0xC0u) {
            value.pop_back();
        }
    }
    return value;
}

/// Text from a service: valid UTF-8, no control characters, one line.
std::string cleanText(const json& j, const char* key, std::size_t limit) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    std::string value = it->get<std::string>();
    if (!edit::isValidUtf8(value)) {
        return {};
    }
    std::string out;
    for (const char c : value) {
        const auto b = static_cast<unsigned char>(c);
        out += (b < 0x20 || b == 0x7f) ? ' ' : c;
    }
    return clip(std::move(out), limit);
}

std::string idText(const json& j) {
    const auto it = j.find("id");
    if (it == j.end()) {
        return {};
    }
    std::string id;
    if (it->is_number_unsigned()) {
        id = std::to_string(it->get<std::uint64_t>());
    } else if (it->is_string()) {
        id = it->get<std::string>();
    }
    if (id.empty() || id.size() > 20 || !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return {};
    }
    return id;
}

int integerOf(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return 0;
    }
    const double v = it->get<double>();
    return v < 0 || v > 1e6 ? 0 : static_cast<int>(v);
}

/// An https address on one of the provider's own hosts, or "".
std::string allowedUrl(StockProvider provider, const std::string& url) {
    return stockHostAllowed(provider, url) ? url : std::string();
}

std::string extensionOf(const std::string& url, const std::string& fallback) {
    const auto parts = ai::parseHttpUrl(url);
    if (!parts) {
        return fallback;
    }
    std::string path = parts->path.substr(0, parts->path.find_first_of("?#"));
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.size() - dot > 6) {
        return fallback;
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == "jpeg") {
        ext = "jpg";
    }
    for (const char* ok : {"mp4", "mov", "webm", "jpg", "png"}) {
        if (ext == ok) {
            return ext;
        }
    }
    return fallback;
}

StoreError failure(StoreFailure kind, std::string message, int status = 0) { return StoreError{kind, std::move(message), status}; }

void sortBest(std::vector<StockFile>& files) {
    std::stable_sort(files.begin(), files.end(), [](const StockFile& a, const StockFile& b) {
        return static_cast<std::int64_t>(a.width) * a.height > static_cast<std::int64_t>(b.width) * b.height;
    });
}

std::string todayText(std::string_view iso) { return std::string(iso.substr(0, 10)); }

} // namespace

std::string stockProviderId(StockProvider provider) { return provider == StockProvider::Pexels ? "pexels" : "pixabay"; }
std::string stockProviderName(StockProvider provider) { return provider == StockProvider::Pexels ? "Pexels" : "Pixabay"; }

std::optional<StockProvider> stockProviderFromId(std::string_view id) {
    if (id == "pexels") {
        return StockProvider::Pexels;
    }
    if (id == "pixabay") {
        return StockProvider::Pixabay;
    }
    return std::nullopt;
}

StockLicence stockLicence(StockProvider provider) {
    if (provider == StockProvider::Pexels) {
        return {"Pexels License", "https://www.pexels.com/license/",
                "Free to use, also for commercial work, with or without credit. You may change the picture or video. You may not sell it "
                "unchanged, or show identifiable people in a bad light.",
                "Pexels asks for a visible link to Pexels and, where possible, a credit to the maker (\"Video by Name on Pexels\").",
                "https://www.pexels.com", "https://www.pexels.com/api/"};
    }
    return {"Pixabay Content License", "https://pixabay.com/service/license-summary/",
            "Free to use, also for commercial work, without credit. You may change the picture or video. You may not sell it unchanged, or "
            "show identifiable people in a bad light.",
            "Pixabay asks that results show where they come from. Credit to the maker is welcome, not required.", "https://pixabay.com",
            "https://pixabay.com/api/docs/"};
}

bool stockHostAllowed(StockProvider provider, std::string_view url) {
    const auto parts = ai::parseHttpUrl(url);
    if (!parts || parts->scheme != "https" || !parts->userinfo.empty()) {
        return false;
    }
    const auto hosts = allowedHosts(provider);
    return std::find(hosts.begin(), hosts.end(), parts->host) != hosts.end();
}

std::string stockQueryProblem(const StockQuery& q) {
    std::string text = q.text;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    if (text.empty()) {
        return "Type what to look for.";
    }
    if (text.size() > 100) {
        return "The search can be at most 100 characters.";
    }
    if (!edit::isValidUtf8(text)) {
        return "The search is not valid text.";
    }
    if (q.page < 1 || q.page > 100) {
        return "There are no more pages.";
    }
    return {};
}

ai::HttpRequest buildStockSearch(const StockQuery& q, const ai::Secret& key) {
    std::string text = q.text;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    const int perPage = std::clamp(q.perPage, 3, q.provider == StockProvider::Pexels ? 80 : 200);
    ai::HttpRequest r;
    r.method = "GET";
    r.timeout = std::chrono::seconds(30);
    r.idleTimeout = std::chrono::seconds(30);
    r.retrySafe = true;
    if (q.provider == StockProvider::Pexels) {
        r.url = std::string("https://api.pexels.com/v1/") + (q.kind == StockKind::Video ? "videos/search" : "search") +
                "?query=" + uriEncode(text, true) + "&per_page=" + std::to_string(perPage) + "&page=" + std::to_string(q.page);
        if (q.orientation == "landscape" || q.orientation == "portrait" || q.orientation == "square") {
            r.url += "&orientation=" + q.orientation;
        }
        r.headers.push_back({"Authorization", std::string(key.reveal())});
    } else {
        r.url = std::string("https://pixabay.com/api/") + (q.kind == StockKind::Video ? "videos/" : "") +
                "?key=" + std::string(key.reveal()) + "&q=" + uriEncode(text, true) + "&per_page=" + std::to_string(perPage) +
                "&page=" + std::to_string(q.page) + "&safesearch=true";
        if (q.kind == StockKind::Photo) {
            r.url += "&image_type=photo";
            if (q.orientation == "landscape") {
                r.url += "&orientation=horizontal";
            } else if (q.orientation == "portrait") {
                r.url += "&orientation=vertical";
            }
        }
    }
    r.headers.push_back({"Accept", "application/json"});
    return r;
}

namespace {

StockItem pexelsVideo(const json& v) {
    StockItem item;
    item.provider = StockProvider::Pexels;
    item.kind = StockKind::Video;
    item.id = idText(v);
    item.width = integerOf(v, "width");
    item.height = integerOf(v, "height");
    item.durationSeconds = v.contains("duration") && v["duration"].is_number() ? std::clamp(v["duration"].get<double>(), 0.0, 1e6) : 0.0;
    item.pageUrl = allowedUrl(StockProvider::Pexels, cleanText(v, "url", 512));
    item.thumbnailUrl = allowedUrl(StockProvider::Pexels, cleanText(v, "image", 512));
    if (v.contains("user") && v["user"].is_object()) {
        item.creator = cleanText(v["user"], "name", 120);
        item.creatorUrl = allowedUrl(StockProvider::Pexels, cleanText(v["user"], "url", 512));
    }
    if (v.contains("video_files") && v["video_files"].is_array()) {
        for (const json& f : v["video_files"]) {
            if (!f.is_object() || item.files.size() >= 24) {
                continue;
            }
            StockFile file;
            file.url = allowedUrl(StockProvider::Pexels, cleanText(f, "link", 1024));
            file.quality = cleanText(f, "quality", 16);
            file.width = integerOf(f, "width");
            file.height = integerOf(f, "height");
            file.fps = f.contains("fps") && f["fps"].is_number() ? std::clamp(f["fps"].get<double>(), 0.0, 1000.0) : 0.0;
            const std::string type = cleanText(f, "file_type", 40);
            file.extension = extensionOf(file.url, type == "video/webm" ? "webm" : "mp4");
            if (!file.url.empty() && file.width > 0 && file.height > 0 && (file.extension == "mp4" || file.extension == "mov" || file.extension == "webm")) {
                item.files.push_back(std::move(file));
            }
        }
    }
    item.description = item.creator.empty() ? std::string() : "by " + item.creator;
    return item;
}

StockItem pexelsPhoto(const json& p) {
    StockItem item;
    item.provider = StockProvider::Pexels;
    item.kind = StockKind::Photo;
    item.id = idText(p);
    item.width = integerOf(p, "width");
    item.height = integerOf(p, "height");
    item.pageUrl = allowedUrl(StockProvider::Pexels, cleanText(p, "url", 512));
    item.creator = cleanText(p, "photographer", 120);
    item.creatorUrl = allowedUrl(StockProvider::Pexels, cleanText(p, "photographer_url", 512));
    item.description = cleanText(p, "alt", 200);
    if (p.contains("src") && p["src"].is_object()) {
        const json& src = p["src"];
        item.thumbnailUrl = allowedUrl(StockProvider::Pexels, cleanText(src, "medium", 1024));
        const struct {
            const char* key;
            const char* quality;
        } renditions[] = {{"original", "original"}, {"large2x", "large"}, {"large", "medium"}};
        for (const auto& r : renditions) {
            StockFile file;
            file.url = allowedUrl(StockProvider::Pexels, cleanText(src, r.key, 1024));
            file.quality = r.quality;
            file.extension = extensionOf(file.url, "jpg");
            if (std::string_view(r.key) == "original") {
                file.width = item.width;
                file.height = item.height;
            } else if (item.width > 0 && item.height > 0) {
                const double scale = std::string_view(r.key) == "large2x" ? 1880.0 / std::max(item.width, 1) : 940.0 / std::max(item.width, 1);
                file.width = std::max(1, static_cast<int>(item.width * std::min(1.0, scale)));
                file.height = std::max(1, static_cast<int>(item.height * std::min(1.0, scale)));
            }
            if (!file.url.empty() && file.width > 0 && file.height > 0) {
                item.files.push_back(std::move(file));
            }
        }
    }
    return item;
}

StockItem pixabayVideo(const json& v) {
    StockItem item;
    item.provider = StockProvider::Pixabay;
    item.kind = StockKind::Video;
    item.id = idText(v);
    item.pageUrl = allowedUrl(StockProvider::Pixabay, cleanText(v, "pageURL", 512));
    item.creator = cleanText(v, "user", 120);
    const std::string userId = v.contains("user_id") && v["user_id"].is_number_unsigned() ? std::to_string(v["user_id"].get<std::uint64_t>()) : std::string();
    if (!item.creator.empty() && !userId.empty()) {
        std::string slug;
        for (const char c : item.creator) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
                slug += c;
            }
        }
        if (!slug.empty()) {
            item.creatorUrl = "https://pixabay.com/users/" + slug + "-" + userId + "/";
        }
    }
    item.description = cleanText(v, "tags", 200);
    item.durationSeconds = v.contains("duration") && v["duration"].is_number() ? std::clamp(v["duration"].get<double>(), 0.0, 1e6) : 0.0;
    std::map<std::string, std::string> thumbs;
    if (v.contains("videos") && v["videos"].is_object()) {
        for (const char* size : {"large", "medium", "small", "tiny"}) {
            const auto it = v["videos"].find(size);
            if (it == v["videos"].end() || !it->is_object()) {
                continue;
            }
            StockFile file;
            file.url = allowedUrl(StockProvider::Pixabay, cleanText(*it, "url", 1024));
            file.quality = size;
            file.width = integerOf(*it, "width");
            file.height = integerOf(*it, "height");
            const auto bytes = it->find("size");
            file.bytes = bytes != it->end() && bytes->is_number_unsigned() ? bytes->get<std::uint64_t>() : 0;
            file.extension = extensionOf(file.url, "mp4");
            thumbs[size] = allowedUrl(StockProvider::Pixabay, cleanText(*it, "thumbnail", 1024));
            if (!file.url.empty() && file.width > 0 && file.height > 0 && file.extension != "jpg" && file.extension != "png") {
                item.files.push_back(std::move(file));
            }
        }
    }
    for (const char* size : {"small", "tiny", "medium"}) { // a small picture for the grid, never the big one
        if (item.thumbnailUrl.empty() && !thumbs[size].empty()) {
            item.thumbnailUrl = thumbs[size];
        }
    }
    if (!item.files.empty()) {
        item.width = item.files.front().width;
        item.height = item.files.front().height;
    }
    return item;
}

StockItem pixabayPhoto(const json& p) {
    StockItem item;
    item.provider = StockProvider::Pixabay;
    item.kind = StockKind::Photo;
    item.id = idText(p);
    item.pageUrl = allowedUrl(StockProvider::Pixabay, cleanText(p, "pageURL", 512));
    item.creator = cleanText(p, "user", 120);
    const std::string userId = p.contains("user_id") && p["user_id"].is_number_unsigned() ? std::to_string(p["user_id"].get<std::uint64_t>()) : std::string();
    if (!item.creator.empty() && !userId.empty()) {
        std::string slug;
        for (const char c : item.creator) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
                slug += c;
            }
        }
        if (!slug.empty()) {
            item.creatorUrl = "https://pixabay.com/users/" + slug + "-" + userId + "/";
        }
    }
    item.description = cleanText(p, "tags", 200);
    item.thumbnailUrl = allowedUrl(StockProvider::Pixabay, cleanText(p, "webformatURL", 1024));
    const int webW = integerOf(p, "webformatWidth");
    const int webH = integerOf(p, "webformatHeight");
    StockFile large;
    large.url = allowedUrl(StockProvider::Pixabay, cleanText(p, "largeImageURL", 1024));
    large.quality = "large";
    large.extension = extensionOf(large.url, "jpg");
    if (webW > 0 && webH > 0) { // largeImageURL is the picture scaled to 1280 on its longer side
        const double scale = 1280.0 / std::max(webW, webH);
        large.width = static_cast<int>(webW * scale);
        large.height = static_cast<int>(webH * scale);
    }
    if (!large.url.empty() && large.width > 0) {
        item.files.push_back(std::move(large));
    }
    StockFile web;
    web.url = item.thumbnailUrl;
    web.quality = "medium";
    web.extension = extensionOf(web.url, "jpg");
    web.width = webW;
    web.height = webH;
    if (!web.url.empty() && web.width > 0) {
        item.files.push_back(std::move(web));
    }
    item.width = integerOf(p, "imageWidth") > 0 ? integerOf(p, "imageWidth") : (item.files.empty() ? 0 : item.files.front().width);
    item.height = integerOf(p, "imageHeight") > 0 ? integerOf(p, "imageHeight") : (item.files.empty() ? 0 : item.files.front().height);
    return item;
}

} // namespace

StoreResult<StockPage> parseStockSearch(StockProvider provider, StockKind kind, int status, std::string_view body, const ai::HttpHeaders& headers) {
    if (status == 401 || status == 403) {
        return std::unexpected(failure(StoreFailure::Unauthorized, "The service refused the key. Check it in the Stock panel's settings.", status));
    }
    if (status == 429) {
        return std::unexpected(failure(StoreFailure::Network,
                                       "The service says there were too many searches. Wait a little (Pexels allows 200 an hour, Pixabay 100 a minute).", status));
    }
    if (status < 200 || status >= 300) {
        return std::unexpected(failure(StoreFailure::Network, "The service did not answer the search (HTTP " + std::to_string(status) + ").", status));
    }
    if (body.size() > kMaxAnswerBytes || !io::jsonNestingWithin(body, 16)) {
        return std::unexpected(failure(StoreFailure::Other, "The service's answer is not usable."));
    }
    const json doc = json::parse(body, nullptr, false);
    if (!doc.is_object()) {
        return std::unexpected(failure(StoreFailure::Other, "The service's answer is not usable."));
    }
    StockPage page;
    const char* listKey = provider == StockProvider::Pexels ? (kind == StockKind::Video ? "videos" : "photos") : "hits";
    const auto list = doc.find(listKey);
    if (list == doc.end() || !list->is_array()) {
        return std::unexpected(failure(StoreFailure::Other, "The service's answer is not usable."));
    }
    for (const json& entry : *list) {
        if (!entry.is_object() || page.items.size() >= kMaxItems) {
            continue;
        }
        StockItem item = provider == StockProvider::Pexels ? (kind == StockKind::Video ? pexelsVideo(entry) : pexelsPhoto(entry))
                                                           : (kind == StockKind::Video ? pixabayVideo(entry) : pixabayPhoto(entry));
        if (item.id.empty() || item.files.empty()) {
            continue;
        }
        sortBest(item.files);
        page.items.push_back(std::move(item));
    }
    page.page = std::max(1, integerOf(doc, "page"));
    page.perPage = integerOf(doc, "per_page");
    if (provider == StockProvider::Pexels) {
        page.total = integerOf(doc, "total_results");
        page.hasMore = doc.contains("next_page") && doc["next_page"].is_string();
    } else {
        page.total = integerOf(doc, "totalHits");
    }
    if (const auto remaining = ai::findHeader(headers, "x-ratelimit-remaining")) {
        char* end = nullptr;
        const std::string value(*remaining);
        const long n = std::strtol(value.c_str(), &end, 10);
        page.quotaRemaining = end != value.c_str() && n >= 0 && n < 100'000'000 ? static_cast<int>(n) : -1;
    }
    return page;
}

std::string stockCreditLine(const StockItem& item) {
    const char* what = item.kind == StockKind::Video ? "Video" : (item.provider == StockProvider::Pexels ? "Photo" : "Image");
    const std::string who = item.creator.empty() ? std::string("an unnamed contributor") : item.creator;
    return std::string(what) + " by " + who + " on " + stockProviderName(item.provider);
}

MediaRights stockRights(const StockItem& item, std::string_view downloadedOn) {
    const StockLicence licence = stockLicence(item.provider);
    MediaRights r;
    r.source = stockProviderName(item.provider);
    r.licence = licence.name;
    r.reference = item.pageUrl.empty() ? stockProviderName(item.provider) + " " + item.id : item.pageUrl;
    r.territory = "Worldwide";
    r.creditLine = stockCreditLine(item);
    r.status = ClearanceStatus::Cleared;
    r.notes = "Downloaded from " + stockProviderName(item.provider) + " on " + todayText(downloadedOn) + ". Licence: " + licence.name + " (" +
              licence.url + "). " + licence.attribution;
    if (rightsProblem(r)) { // a field too long: cut the free text, keep the facts
        r.notes = clip(r.notes, MediaRights::kMaxNotesBytes);
        r.reference = clip(r.reference, MediaRights::kMaxFieldBytes);
        r.creditLine = clip(r.creditLine, MediaRights::kMaxFieldBytes);
    }
    return r;
}

std::string stockFileName(const StockItem& item, const StockFile& file) {
    std::string quality;
    for (const char c : file.quality) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            quality += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (quality.empty()) {
        quality = std::to_string(file.width);
    }
    return stockProviderId(item.provider) + "-" + item.id + "-" + quality + "." + file.extension;
}

bool stockMagicMatches(std::string_view extension, std::string_view b) {
    const auto at = [&](std::size_t i) { return i < b.size() ? static_cast<unsigned char>(b[i]) : 0u; };
    if (extension == "mp4" || extension == "mov") {
        return b.size() >= 12 && b.substr(4, 4) == "ftyp";
    }
    if (extension == "webm") {
        return b.size() >= 4 && at(0) == 0x1A && at(1) == 0x45 && at(2) == 0xDF && at(3) == 0xA3;
    }
    if (extension == "jpg") {
        return b.size() >= 3 && at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF;
    }
    if (extension == "png") {
        return b.size() >= 8 && at(0) == 0x89 && b.substr(1, 3) == "PNG";
    }
    return false;
}

// ---- the client ----

StockClient::StockClient(ai::IHttpTransport& transport, UnixClock clock) : m_transport(transport), m_clock(std::move(clock)) {}

StoreResult<StockPage> StockClient::search(const StockQuery& query, const ai::Secret& key, std::stop_token stop) {
    if (const std::string problem = stockQueryProblem(query); !problem.empty()) {
        return std::unexpected(failure(StoreFailure::Invalid, problem));
    }
    if (key.empty()) {
        return std::unexpected(failure(StoreFailure::Unauthorized, "Enter your " + stockProviderName(query.provider) + " key in the Stock panel's settings."));
    }
    // The cache key never holds the API key.
    const std::string cacheKey = stockProviderId(query.provider) + "|" + (query.kind == StockKind::Video ? "v" : "p") + "|" + query.orientation + "|" +
                                 std::to_string(query.page) + "|" + std::to_string(query.perPage) + "|" + query.text;
    const std::int64_t now = m_clock();
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_cache.find(cacheKey);
        if (it != m_cache.end() && now - it->second.at < kCacheSeconds) {
            return it->second.page;
        }
    }
    auto response = m_transport.send(buildStockSearch(query, key), {}, stop);
    if (!response) {
        return std::unexpected(fromTransport(response.error()));
    }
    auto page = parseStockSearch(query.provider, query.kind, response->status, response->body, response->headers);
    if (page) {
        page->page = std::max(page->page, query.page);
        std::lock_guard lock(m_mutex);
        if (m_cache.size() >= kMaxCached) {
            m_cache.erase(m_cache.begin());
        }
        m_cache[cacheKey] = Cached{now, *page};
    }
    return page;
}

StoreResult<void> StockClient::checkKey(StockProvider provider, const ai::Secret& key, std::stop_token stop) {
    StockQuery q;
    q.provider = provider;
    q.kind = StockKind::Photo;
    q.text = "nature";
    q.perPage = 3;
    if (key.empty()) {
        return std::unexpected(failure(StoreFailure::Unauthorized, "Enter a key first."));
    }
    auto response = m_transport.send(buildStockSearch(q, key), {}, stop);
    if (!response) {
        return std::unexpected(fromTransport(response.error()));
    }
    auto page = parseStockSearch(provider, q.kind, response->status, response->body, response->headers);
    if (!page) {
        return std::unexpected(page.error());
    }
    return {};
}

StoreResult<std::string> StockClient::thumbnail(const StockItem& item, std::stop_token stop) {
    if (!stockHostAllowed(item.provider, item.thumbnailUrl)) {
        return std::unexpected(failure(StoreFailure::Invalid, "There is no picture for this item."));
    }
    ai::HttpRequest r;
    r.method = "GET";
    r.url = item.thumbnailUrl;
    r.timeout = std::chrono::seconds(30);
    r.idleTimeout = std::chrono::seconds(20);
    r.headers.push_back({"Accept", "image/*"});
    std::string data;
    bool tooLarge = false;
    r.responseMode = ai::BodyMode::Raw;
    ai::HttpStreamHandlers handlers;
    handlers.onData = [&](std::span<const std::byte> bytes) {
        if (data.size() + bytes.size() > kMaxThumbBytes) {
            tooLarge = true;
            return false;
        }
        data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    };
    auto response = m_transport.send(r, handlers, stop);
    if (!response) {
        return std::unexpected(tooLarge ? failure(StoreFailure::TooLarge, "The picture is larger than allowed.") : fromTransport(response.error()));
    }
    if (!response->ok()) {
        return std::unexpected(fromStatus(response->status, "Fetching the picture"));
    }
    const bool jpeg = data.size() > 3 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xD8;
    const bool png = data.size() > 8 && static_cast<unsigned char>(data[0]) == 0x89 && data.compare(1, 3, "PNG") == 0;
    const bool webp = data.size() > 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0;
    if (!jpeg && !png && !webp) {
        return std::unexpected(failure(StoreFailure::Other, "That is not a picture."));
    }
    return data;
}

StoreResult<StockDownloaded> StockClient::download(const StockItem& item, const StockFile& file, const fs::path& folder, const Progress& progress,
                                                   std::stop_token stop) {
    if (!stockHostAllowed(item.provider, file.url)) {
        return std::unexpected(failure(StoreFailure::Invalid, "That address is not one of " + stockProviderName(item.provider) + "'s."));
    }
    const bool knownExtension = file.extension == "mp4" || file.extension == "mov" || file.extension == "webm" || file.extension == "jpg" ||
                                file.extension == "png";
    if (item.id.empty() || !knownExtension) {
        return std::unexpected(failure(StoreFailure::Invalid, "This kind of file is not supported."));
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        return std::unexpected(failure(StoreFailure::Other, "Cannot make the folder for the download."));
    }
    const fs::path target = folder / pathFromUtf8(stockFileName(item, file));
    if (file.bytes > 0 && fs::is_regular_file(target, ec) && fs::file_size(target, ec) == file.bytes) {
        return StockDownloaded{target, file.bytes}; // downloaded before
    }
    if (file.bytes > kMaxDownloadBytes) {
        return std::unexpected(failure(StoreFailure::TooLarge, "The file is larger than 4 GB."));
    }
    if (file.bytes > 0) {
        const auto space = fs::space(folder, ec);
        if (!ec && space.available < file.bytes + (file.bytes / 10) + (16u << 20)) {
            return std::unexpected(failure(StoreFailure::TooLarge, "There is not enough free space on the drive for this download."));
        }
    }
    auto out = detail::AtomicFile::create(target);
    if (!out) {
        return std::unexpected(out.error());
    }
    // From the provider's host to the provider's hosts; the key is not part of the request.
    ai::RedirectAllowList allow;
    const auto hosts = allowedHosts(item.provider);
    for (const std::string& from : hosts) {
        allow.rules.push_back({from, hosts});
    }
    ai::HttpRequest r;
    r.method = "GET";
    r.url = file.url;
    r.followRedirects = false;
    r.responseMode = ai::BodyMode::Raw;
    r.timeout = std::chrono::hours(3);
    r.idleTimeout = std::chrono::seconds(60);
    std::stop_source linked;
    const std::stop_callback forward(stop, [&linked] { linked.request_stop(); });
    std::string firstBytes;
    std::uint64_t done = 0;
    std::uint64_t total = file.bytes;
    bool tooLarge = false;
    bool badWrite = false;
    ai::HttpStreamHandlers handlers;
    handlers.onHead = [&](const ai::HttpResponseHead& head) {
        if (const auto length = ai::findHeader(head.headers, "content-length")) {
            const std::string text(*length);
            const unsigned long long declared = std::strtoull(text.c_str(), nullptr, 10);
            if (declared > kMaxDownloadBytes) {
                tooLarge = true;
                return false;
            }
            total = declared;
        }
        return true;
    };
    handlers.onData = [&](std::span<const std::byte> bytes) {
        done += bytes.size();
        if (done > kMaxDownloadBytes) {
            tooLarge = true;
            return false;
        }
        if (firstBytes.size() < 16) {
            firstBytes.append(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(bytes.size(), 16 - firstBytes.size()));
            if (firstBytes.size() >= 12 && !stockMagicMatches(file.extension, firstBytes)) {
                return false; // not the kind of file it says
            }
        }
        if (!(*out)->write(reinterpret_cast<const char*>(bytes.data()), bytes.size())) {
            badWrite = true;
            return false;
        }
        if (progress && !progress(done, total)) {
            linked.request_stop();
            return false;
        }
        return true;
    };
    auto response = ai::sendFollowingRedirects(m_transport, std::move(r), handlers, linked.get_token(), allow);
    if (tooLarge) {
        return std::unexpected(failure(StoreFailure::TooLarge, "The file is larger than 4 GB."));
    }
    if (badWrite) {
        return std::unexpected(failure(StoreFailure::Other, "The file could not be written (is the disk full?)"));
    }
    if (!response) {
        if (linked.stop_requested()) {
            return std::unexpected(failure(StoreFailure::Cancelled, "cancelled"));
        }
        if (firstBytes.size() >= 12 && !stockMagicMatches(file.extension, firstBytes)) {
            return std::unexpected(failure(StoreFailure::Other, "The download is not a " + file.extension + " file; it was discarded."));
        }
        return std::unexpected(fromTransport(response.error()));
    }
    if (!response->ok()) {
        return std::unexpected(fromStatus(response->status, "The download"));
    }
    if (done == 0 || !stockMagicMatches(file.extension, firstBytes)) {
        return std::unexpected(failure(StoreFailure::Other, "The download is not a " + file.extension + " file; it was discarded."));
    }
    if (auto committed = (*out)->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return StockDownloaded{target, done};
}

} // namespace wabcut::cloud
