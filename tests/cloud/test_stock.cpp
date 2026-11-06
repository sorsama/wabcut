// Stock media (docs/cloud.md): Pexels and Pixabay requests and answers (recorded-style JSON written from the services' current
// documentation), licence and credit records, and the bounded download. A fake transport stands in for the network.

#include "FakeServers.h"

#include <wabcut/cloud/Stock.h>
#include <wabcut/model/Rights.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>

using namespace wabcut;
using namespace wabcut::cloud;
using namespace wabcut::cloud::test;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// A transport that answers from a table and remembers every request.
class Fake final : public ai::IHttpTransport {
public:
    struct Answer {
        int status = 200;
        ai::HttpHeaders headers;
        std::string body;
        std::string location; ///< for 3xx
    };
    std::map<std::string, Answer> answers; ///< by URL
    std::vector<Recorded> requests;
    std::optional<ai::AiError> failWith;
    std::size_t chunk = 64 * 1024;

    std::expected<ai::HttpResponse, ai::AiError> send(const ai::HttpRequest& request, const ai::HttpStreamHandlers& handlers,
                                                      std::stop_token stop) override {
        requests.push_back({request.method, request.url, request.headers, request.body, false});
        if (failWith) {
            return std::unexpected(*failWith);
        }
        if (stop.stop_requested()) {
            return std::unexpected(ai::AiError(ai::AiErrorCode::Cancelled, "cancelled"));
        }
        const auto it = answers.find(request.url);
        if (it == answers.end()) {
            return answer(request, handlers, 404, {}, "not found");
        }
        const Answer& a = it->second;
        ai::HttpHeaders headers = a.headers;
        if (!a.location.empty()) {
            headers.push_back({"Location", a.location});
        }
        if (a.status >= 200 && a.status < 300 && request.responseMode == ai::BodyMode::Raw) {
            ai::HttpResponse response;
            response.status = a.status;
            response.headers = headers;
            if (handlers.onHead && !handlers.onHead(ai::HttpResponseHead{a.status, headers})) {
                return std::unexpected(ai::AiError(ai::AiErrorCode::Cancelled, "handler stopped"));
            }
            for (std::size_t at = 0; at < a.body.size(); at += chunk) {
                const std::size_t n = std::min(chunk, a.body.size() - at);
                if (handlers.onData && !handlers.onData(std::span<const std::byte>(reinterpret_cast<const std::byte*>(a.body.data()) + at, n))) {
                    return std::unexpected(ai::AiError(ai::AiErrorCode::Cancelled, "handler stopped"));
                }
            }
            return response;
        }
        ai::HttpResponse response;
        response.status = a.status;
        response.headers = headers;
        response.body = a.body;
        return response;
    }
};

const char* kPexelsVideos = R"({"page":1,"per_page":2,"total_results":300,"next_page":"https://api.pexels.com/v1/videos/search?page=2","videos":[
 {"id":856973,"width":3840,"height":2160,"url":"https://www.pexels.com/video/ocean-waves-856973/","image":"https://images.pexels.com/videos/856973/free-video-856973.jpg","duration":23,
  "user":{"id":1,"name":"Ann Lee","url":"https://www.pexels.com/@annlee"},
  "video_files":[{"id":1,"quality":"sd","file_type":"video/mp4","width":640,"height":360,"fps":25.0,"link":"https://videos.pexels.com/video-files/856973/856973-sd_640_360_25fps.mp4"},
                 {"id":2,"quality":"hd","file_type":"video/mp4","width":1920,"height":1080,"fps":25.0,"link":"https://videos.pexels.com/video-files/856973/856973-hd_1920_1080_25fps.mp4"},
                 {"id":3,"quality":"hd","file_type":"video/mp4","width":1280,"height":720,"fps":25.0,"link":"https://evil.example.com/steal.mp4"},
                 {"id":4,"quality":"hd","file_type":"video/mp4","width":1280,"height":720,"fps":25.0,"link":"http://videos.pexels.com/insecure.mp4"}],
  "video_pictures":[]},
 {"id":2,"width":10,"height":10,"url":"https://evil.example.com/x","image":"https://evil.example.com/i.jpg","duration":1,"user":{"id":2,"name":"X"},
  "video_files":[{"id":9,"quality":"hd","file_type":"video/mp4","width":10,"height":10,"fps":1,"link":"https://evil.example.com/a.mp4"}]}]})";

const char* kPexelsPhotos = R"({"page":1,"per_page":1,"total_results":8000,"photos":[
 {"id":4147875,"width":6000,"height":4000,"url":"https://www.pexels.com/photo/food-dinner-lunch-meal-4147875/","photographer":"Daria <b>S</b>","photographer_url":"https://www.pexels.com/@daria",
  "alt":"A plate of food","src":{"original":"https://images.pexels.com/photos/4147875/pexels-photo-4147875.jpeg","large2x":"https://images.pexels.com/photos/4147875/pexels-photo-4147875.jpeg?w=940","large":"https://images.pexels.com/photos/4147875/pexels-photo-4147875.jpeg?w=650","medium":"https://images.pexels.com/photos/4147875/m.jpeg"}}]})";

const char* kPixabayVideos = R"({"total":42,"totalHits":42,"hits":[
 {"id":125,"pageURL":"https://pixabay.com/videos/id-125/","type":"film","tags":"flowers, yellow, blossom","duration":12,
  "videos":{"large":{"url":"","width":0,"height":0,"size":0,"thumbnail":""},
            "medium":{"url":"https://cdn.pixabay.com/video/2015/08/08/125-135736646_medium.mp4","width":1920,"height":1080,"size":6615235,"thumbnail":"https://cdn.pixabay.com/video/2015/08/08/125-135736646_medium.jpg"},
            "small":{"url":"https://cdn.pixabay.com/video/2015/08/08/125-135736646_small.mp4","width":1280,"height":720,"size":3562083,"thumbnail":"https://cdn.pixabay.com/video/2015/08/08/125-135736646_small.jpg"},
            "tiny":{"url":"https://cdn.pixabay.com/video/2015/08/08/125-135736646_tiny.mp4","width":960,"height":540,"size":1030736,"thumbnail":"https://cdn.pixabay.com/video/2015/08/08/125-135736646_tiny.jpg"}},
  "views":4462,"downloads":1464,"user_id":1281706,"user":"Coverr-Free-Footage"}]})";

const char* kPixabayPhotos = R"({"total":4692,"totalHits":500,"hits":[
 {"id":195893,"pageURL":"https://pixabay.com/en/blossom-bloom-flower-195893/","type":"photo","tags":"blossom, bloom, flower","previewURL":"https://cdn.pixabay.com/photo/2013/10/15/09/12/flower-195893_150.jpg",
  "webformatURL":"https://pixabay.com/get/35bbf209e13e39d2_640.jpg","webformatWidth":640,"webformatHeight":360,"largeImageURL":"https://pixabay.com/get/ed6a99fd0a76647_1280.jpg","user_id":48777,"user":"Josch13"}]})";

StockItem firstItem(StockProvider provider, StockKind kind, const char* body) {
    auto page = parseStockSearch(provider, kind, 200, body, {});
    REQUIRE(page);
    REQUIRE_FALSE(page->items.empty());
    return page->items.front();
}

} // namespace

TEST_CASE("Stock: Pexels answers", "[cloud][stock]") {
    const auto page = parseStockSearch(StockProvider::Pexels, StockKind::Video, 200, kPexelsVideos, {{"X-Ratelimit-Remaining", "19684"}});
    REQUIRE(page);
    CHECK(page->total == 300);
    CHECK(page->hasMore);
    CHECK(page->quotaRemaining == 19684);
    REQUIRE(page->items.size() == 1); // the second video has no file on a Pexels host
    const StockItem& v = page->items[0];
    CHECK(v.id == "856973");
    CHECK(v.creator == "Ann Lee");
    CHECK(v.durationSeconds == 23.0);
    CHECK(v.pageUrl == "https://www.pexels.com/video/ocean-waves-856973/");
    CHECK(v.thumbnailUrl.find("https://images.pexels.com/") == 0);
    // files on other hosts and plain http are dropped; the rest is largest first
    REQUIRE(v.files.size() == 2);
    CHECK(v.files[0].width == 1920);
    CHECK(v.files[0].quality == "hd");
    CHECK(v.files[0].extension == "mp4");
    CHECK(v.files[1].width == 640);
    CHECK(stockFileName(v, v.files[0]) == "pexels-856973-hd.mp4");
    CHECK(stockCreditLine(v) == "Video by Ann Lee on Pexels");

    const auto photos = parseStockSearch(StockProvider::Pexels, StockKind::Photo, 200, kPexelsPhotos, {});
    REQUIRE(photos);
    REQUIRE(photos->items.size() == 1);
    const StockItem& p = photos->items[0];
    CHECK(p.creator == "Daria <b>S</b>"); // text stays text; the panel shows it as plain text
    CHECK(p.description == "A plate of food");
    REQUIRE(p.files.size() == 3);
    CHECK(p.files[0].quality == "original");
    CHECK(p.files[0].width == 6000);
    CHECK(p.files[0].extension == "jpg");
    CHECK(stockCreditLine(p) == "Photo by Daria <b>S</b> on Pexels");
}

TEST_CASE("Stock: Pixabay answers", "[cloud][stock]") {
    const StockItem v = firstItem(StockProvider::Pixabay, StockKind::Video, kPixabayVideos);
    CHECK(v.id == "125");
    CHECK(v.creator == "Coverr-Free-Footage");
    CHECK(v.creatorUrl == "https://pixabay.com/users/Coverr-Free-Footage-1281706/");
    CHECK(v.durationSeconds == 12.0);
    REQUIRE(v.files.size() == 3); // "large" is empty for this video
    CHECK(v.files[0].quality == "medium");
    CHECK(v.files[0].width == 1920);
    CHECK(v.files[0].bytes == 6615235);
    CHECK(v.thumbnailUrl == "https://cdn.pixabay.com/video/2015/08/08/125-135736646_small.jpg");
    CHECK(stockFileName(v, v.files[0]) == "pixabay-125-medium.mp4");
    CHECK(stockCreditLine(v) == "Video by Coverr-Free-Footage on Pixabay");

    const StockItem p = firstItem(StockProvider::Pixabay, StockKind::Photo, kPixabayPhotos);
    CHECK(p.id == "195893");
    REQUIRE(p.files.size() == 2);
    CHECK(p.files[0].quality == "large");
    CHECK(p.files[0].width == 1280);
    CHECK(p.files[0].height == 720);
    CHECK(p.description == "blossom, bloom, flower");
    CHECK(stockCreditLine(p) == "Image by Josch13 on Pixabay");
}

TEST_CASE("Stock: what each answer status means", "[cloud][stock]") {
    CHECK(parseStockSearch(StockProvider::Pexels, StockKind::Video, 401, "", {}).error().kind == StoreFailure::Unauthorized);
    CHECK(parseStockSearch(StockProvider::Pixabay, StockKind::Video, 403, "", {}).error().kind == StoreFailure::Unauthorized);
    const auto limited = parseStockSearch(StockProvider::Pixabay, StockKind::Video, 429, "API rate limit exceeded", {});
    REQUIRE_FALSE(limited);
    CHECK(limited.error().status == 429);
    CHECK(limited.error().message.find("too many searches") != std::string::npos);
    CHECK_FALSE(parseStockSearch(StockProvider::Pexels, StockKind::Video, 500, "", {}));
    CHECK_FALSE(parseStockSearch(StockProvider::Pexels, StockKind::Video, 200, "not json", {}));
    CHECK_FALSE(parseStockSearch(StockProvider::Pexels, StockKind::Video, 200, "{\"photos\":[]}", {})); // wrong list for the kind
    CHECK_FALSE(parseStockSearch(StockProvider::Pexels, StockKind::Video, 200, std::string(2000, '['), {}));
    // an item with an id that is not digits, or without files, is skipped
    const auto odd = parseStockSearch(StockProvider::Pexels, StockKind::Video, 200,
                                      R"({"videos":[{"id":"../x","video_files":[]},{"id":7,"width":1,"height":1,"video_files":[]}]})", {});
    REQUIRE(odd);
    CHECK(odd->items.empty());
}

TEST_CASE("Stock: the request carries the key only where the service wants it", "[cloud][stock]") {
    StockQuery q;
    q.text = "cats & dogs/ocean";
    q.page = 2;
    q.perPage = 40;
    SECTION("Pexels: in the Authorization header, never in the address") {
        const ai::HttpRequest r = buildStockSearch(q, ai::Secret("PEXELS-KEY-0123456789"));
        CHECK(r.method == "GET");
        CHECK(r.url == "https://api.pexels.com/v1/videos/search?query=cats%20%26%20dogs%2Focean&per_page=40&page=2");
        CHECK(r.url.find("PEXELS-KEY") == std::string::npos);
        CHECK(ai::findHeader(r.headers, "authorization").value_or("") == "PEXELS-KEY-0123456789");
        q.kind = StockKind::Photo;
        q.orientation = "landscape";
        q.perPage = 500;
        const ai::HttpRequest p = buildStockSearch(q, ai::Secret("PEXELS-KEY-0123456789"));
        CHECK(p.url == "https://api.pexels.com/v1/search?query=cats%20%26%20dogs%2Focean&per_page=80&page=2&orientation=landscape");
    }
    SECTION("Pixabay: as the key parameter, with safe search on") {
        q.provider = StockProvider::Pixabay;
        const ai::HttpRequest r = buildStockSearch(q, ai::Secret("PIXABAY-KEY-0123456789"));
        CHECK(r.url == "https://pixabay.com/api/videos/?key=PIXABAY-KEY-0123456789&q=cats%20%26%20dogs%2Focean&per_page=40&page=2&safesearch=true");
        CHECK_FALSE(ai::findHeader(r.headers, "authorization").has_value());
        q.kind = StockKind::Photo;
        q.orientation = "portrait";
        q.perPage = 1;
        const ai::HttpRequest p = buildStockSearch(q, ai::Secret("PIXABAY-KEY-0123456789"));
        CHECK(p.url.find("https://pixabay.com/api/?key=") == 0);
        CHECK(p.url.find("&per_page=3&") != std::string::npos);
        CHECK(p.url.find("&image_type=photo&orientation=vertical") != std::string::npos);
    }
    SECTION("queries are checked") {
        q.text = "";
        CHECK_FALSE(stockQueryProblem(q).empty());
        q.text = std::string(101, 'a');
        CHECK_FALSE(stockQueryProblem(q).empty());
        q.text = "ok";
        q.page = 0;
        CHECK_FALSE(stockQueryProblem(q).empty());
        q.page = 1;
        CHECK(stockQueryProblem(q).empty());
        q.text = "bad\xFF";
        CHECK_FALSE(stockQueryProblem(q).empty());
    }
}

TEST_CASE("Stock: a download is recorded with its licence and credit", "[cloud][stock]") {
    const auto page = parseStockSearch(StockProvider::Pexels, StockKind::Video, 200, kPexelsVideos, {});
    REQUIRE(page);
    const MediaRights r = stockRights(page->items[0], "2026-11-05T09:30:00Z");
    CHECK(r.source == "Pexels");
    CHECK(r.licence == "Pexels License");
    CHECK(r.reference == "https://www.pexels.com/video/ocean-waves-856973/");
    CHECK(r.creditLine == "Video by Ann Lee on Pexels");
    CHECK(r.status == ClearanceStatus::Cleared);
    CHECK(r.territory == "Worldwide");
    CHECK(r.notes.find("2026-11-05") != std::string::npos);
    CHECK(r.notes.find("https://www.pexels.com/license/") != std::string::npos);
    CHECK_FALSE(rightsProblem(r).has_value());

    const StockItem px = firstItem(StockProvider::Pixabay, StockKind::Video, kPixabayVideos);
    const MediaRights pr = stockRights(px, "2026-11-05T09:30:00Z");
    CHECK(pr.licence == "Pixabay Content License");
    CHECK(pr.creditLine == "Video by Coverr-Free-Footage on Pixabay");
    CHECK_FALSE(rightsProblem(pr).has_value());

    // a very long creator name still gives a record that can be saved
    StockItem long1 = page->items[0];
    long1.creator = std::string(120, 'n');
    long1.pageUrl = "https://www.pexels.com/video/" + std::string(600, 'x') + "/";
    CHECK_FALSE(rightsProblem(stockRights(long1, "2026-11-05")).has_value());

    CHECK(stockLicence(StockProvider::Pexels).attribution.find("link to Pexels") != std::string::npos);
    CHECK(stockLicence(StockProvider::Pixabay).attribution.find("where they come from") != std::string::npos);
}

TEST_CASE("Stock: searching goes through the transport is cached for a day and says why it failed", "[cloud][stock]") {
    Fake net;
    std::int64_t now = 1793874937;
    StockClient client(net, [&] { return now; });
    const ai::Secret key("PEXELS-KEY-0123456789");
    StockQuery q;
    q.text = "ocean";
    const std::string url = buildStockSearch(q, key).url;
    net.answers[url] = {200, {{"X-Ratelimit-Remaining", "199"}}, kPexelsVideos, {}};

    auto first = client.search(q, key);
    REQUIRE(first);
    CHECK(first->items.size() == 1);
    CHECK(first->quotaRemaining == 199);
    REQUIRE(net.requests.size() == 1);
    CHECK(net.requests[0].url.find("PEXELS-KEY") == std::string::npos);

    // the same search again within a day sends nothing (Pixabay requires it); a day later it asks again
    REQUIRE(client.search(q, key));
    CHECK(net.requests.size() == 1);
    now += 24 * 3600 + 1;
    REQUIRE(client.search(q, key));
    CHECK(net.requests.size() == 2);

    // failures
    net.answers[url].status = 401;
    now += 100000;
    auto refused = client.search(q, key);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().kind == StoreFailure::Unauthorized);
    net.failWith = ai::AiError(ai::AiErrorCode::Offline, "Offline / no-cloud mode blocks this AI network request");
    q.text = "another";
    auto offline = client.search(q, key);
    REQUIRE_FALSE(offline);
    CHECK(offline.error().kind == StoreFailure::Offline);
    net.failWith.reset();
    CHECK(client.search(q, ai::Secret()).error().kind == StoreFailure::Unauthorized);
    q.text = "";
    CHECK(client.search(q, key).error().kind == StoreFailure::Invalid);

    // checking a key
    net.answers.clear();
    StockQuery check;
    check.kind = StockKind::Photo;
    check.text = "nature";
    check.perPage = 3;
    net.answers[buildStockSearch(check, key).url] = {200, {}, kPexelsPhotos, {}};
    CHECK(client.checkKey(StockProvider::Pexels, key));
    net.answers[buildStockSearch(check, key).url].status = 401;
    CHECK(client.checkKey(StockProvider::Pexels, key).error().kind == StoreFailure::Unauthorized);
}

TEST_CASE("Stock: pictures for the panel come without the key and only if they are pictures", "[cloud][stock]") {
    Fake net;
    StockClient client(net);
    const StockItem item = firstItem(StockProvider::Pexels, StockKind::Video, kPexelsVideos);
    net.answers[item.thumbnailUrl] = {200, {}, std::string("\xFF\xD8\xFF\xE0", 4) + std::string(100, 'j'), {}};
    auto ok = client.thumbnail(item);
    REQUIRE(ok);
    CHECK(ok->size() == 104);
    REQUIRE(net.requests.size() == 1);
    CHECK_FALSE(ai::findHeader(net.requests[0].headers, "authorization").has_value());

    net.answers[item.thumbnailUrl].body = "<html>not a picture</html>";
    CHECK_FALSE(client.thumbnail(item));
    net.answers[item.thumbnailUrl].body = std::string("\xFF\xD8\xFF", 3) + std::string(5 * 1024 * 1024, 'j');
    auto big = client.thumbnail(item);
    REQUIRE_FALSE(big);
    CHECK(big.error().kind == StoreFailure::TooLarge);
    StockItem wrongHost = item;
    wrongHost.thumbnailUrl = "https://evil.example.com/x.jpg";
    const std::size_t sent = net.requests.size();
    CHECK_FALSE(client.thumbnail(wrongHost));
    CHECK(net.requests.size() == sent);
}

TEST_CASE("Stock: downloading is bounded checked and atomic", "[cloud][stock]") {
    TempFolder folder("stock");
    Fake net;
    StockClient client(net);
    StockItem item = firstItem(StockProvider::Pexels, StockKind::Video, kPexelsVideos);
    const StockFile file = item.files[0];
    const std::string mp4 = std::string("\0\0\0\x20" "ftypisom", 12) + std::string(300 * 1024, 'v');

    SECTION("the file arrives whole, with progress") {
        net.answers[file.url] = {200, {{"Content-Length", std::to_string(mp4.size())}}, mp4, {}};
        std::uint64_t last = 0;
        auto got = client.download(item, file, folder.path / "Stock", [&](std::uint64_t done, std::uint64_t total) {
            last = done;
            CHECK(total == mp4.size());
            return true;
        });
        REQUIRE(got);
        CHECK(got->file.filename() == "pexels-856973-hd.mp4");
        CHECK(got->bytes == mp4.size());
        CHECK(last == mp4.size());
        CHECK(readAll(got->file) == mp4);
        REQUIRE(net.requests.size() == 1);
        CHECK_FALSE(ai::findHeader(net.requests[0].headers, "authorization").has_value()); // no key reaches the CDN
    }
    SECTION("a redirect between the service's own hosts is followed") {
        net.answers[file.url] = {302, {}, "", "https://videos.pexels.com/video-files/moved.mp4"};
        net.answers["https://videos.pexels.com/video-files/moved.mp4"] = {200, {}, mp4, {}};
        auto got = client.download(item, file, folder.path);
        REQUIRE(got);
        CHECK(readAll(got->file) == mp4);
    }
    SECTION("a redirect to any other host is refused and nothing is kept") {
        net.answers[file.url] = {302, {}, "", "https://evil.example.com/malware.mp4"};
        net.answers["https://evil.example.com/malware.mp4"] = {200, {}, mp4, {}};
        auto got = client.download(item, file, folder.path);
        REQUIRE_FALSE(got);
        for (const Recorded& r : net.requests) {
            CHECK(r.url.find("evil") == std::string::npos);
        }
        CHECK_FALSE(fs::exists(folder.path / "pexels-856973-hd.mp4"));
    }
    SECTION("a file that is not what its name says is discarded") {
        net.answers[file.url] = {200, {}, "MZ\x90\x00 this is a program, not a video, however it is named " + std::string(1000, 'x'), {}};
        auto got = client.download(item, file, folder.path);
        REQUIRE_FALSE(got);
        CHECK(got.error().message.find("not a mp4") != std::string::npos);
        CHECK_FALSE(fs::exists(folder.path / "pexels-856973-hd.mp4"));
        for (const auto& e : fs::directory_iterator(folder.path)) {
            CHECK(e.path().string().find(".part") == std::string::npos);
        }
    }
    SECTION("addresses are checked before anything is sent") {
        StockFile evil = file;
        evil.url = "https://evil.example.com/a.mp4";
        CHECK(client.download(item, evil, folder.path).error().kind == StoreFailure::Invalid);
        evil.url = "http://videos.pexels.com/a.mp4";
        CHECK(client.download(item, evil, folder.path).error().kind == StoreFailure::Invalid);
        evil.url = "https://user:pw@videos.pexels.com/a.mp4";
        CHECK(client.download(item, evil, folder.path).error().kind == StoreFailure::Invalid);
        StockFile odd = file;
        odd.extension = "exe";
        CHECK(client.download(item, odd, folder.path).error().kind == StoreFailure::Invalid);
        CHECK(net.requests.empty());
    }
    SECTION("a file that says it is huge is refused") {
        net.answers[file.url] = {200, {{"Content-Length", "9999999999999"}}, mp4, {}};
        auto got = client.download(item, file, folder.path);
        REQUIRE_FALSE(got);
        CHECK(got.error().kind == StoreFailure::TooLarge);
        CHECK_FALSE(fs::exists(folder.path / "pexels-856973-hd.mp4"));
    }
    SECTION("cancelling leaves nothing") {
        net.chunk = 4096;
        net.answers[file.url] = {200, {}, mp4, {}};
        auto got = client.download(item, file, folder.path, [](std::uint64_t done, std::uint64_t) { return done < 20000; });
        REQUIRE_FALSE(got);
        CHECK(got.error().kind == StoreFailure::Cancelled);
        CHECK_FALSE(fs::exists(folder.path / "pexels-856973-hd.mp4"));
        for (const auto& e : fs::directory_iterator(folder.path)) {
            CHECK(e.path().string().find(".part") == std::string::npos);
        }
    }
    SECTION("a file downloaded before is used again") {
        StockFile known = file;
        known.bytes = mp4.size();
        writeAll(folder.path / "pexels-856973-hd.mp4", mp4);
        auto got = client.download(item, known, folder.path);
        REQUIRE(got);
        CHECK(net.requests.empty());
    }
    SECTION("pictures: the first bytes must be a picture") {
        const auto photos = parseStockSearch(StockProvider::Pexels, StockKind::Photo, 200, kPexelsPhotos, {});
        REQUIRE(photos);
        const StockItem p = photos->items[0];
        net.answers[p.files[0].url] = {200, {}, std::string("\xFF\xD8\xFF\xE1", 4) + std::string(4000, 'p'), {}};
        auto got = client.download(p, p.files[0], folder.path);
        REQUIRE(got);
        CHECK(got->file.filename() == "pexels-4147875-original.jpg");
    }
}

TEST_CASE("Stock: magic bytes and hosts", "[cloud][stock]") {
    CHECK(stockMagicMatches("mp4", std::string("\0\0\0\x18" "ftypmp42", 12)));
    CHECK(stockMagicMatches("mov", std::string("\0\0\0\x14" "ftypqt  ", 12)));
    CHECK_FALSE(stockMagicMatches("mp4", "MZ........ftyp"));
    CHECK(stockMagicMatches("webm", std::string("\x1A\x45\xDF\xA3", 4)));
    CHECK(stockMagicMatches("jpg", std::string("\xFF\xD8\xFF", 3)));
    CHECK(stockMagicMatches("png", std::string("\x89PNG\r\n\x1A\n", 8)));
    CHECK_FALSE(stockMagicMatches("png", "GIF89a......"));
    CHECK_FALSE(stockMagicMatches("exe", "MZ"));

    CHECK(stockHostAllowed(StockProvider::Pexels, "https://videos.pexels.com/x.mp4"));
    CHECK(stockHostAllowed(StockProvider::Pixabay, "https://cdn.pixabay.com/x.mp4"));
    CHECK_FALSE(stockHostAllowed(StockProvider::Pexels, "https://cdn.pixabay.com/x.mp4"));
    CHECK_FALSE(stockHostAllowed(StockProvider::Pexels, "https://videos.pexels.com.evil.example/x.mp4"));
    CHECK_FALSE(stockHostAllowed(StockProvider::Pexels, "https://evilpexels.com/x.mp4"));
    CHECK_FALSE(stockHostAllowed(StockProvider::Pexels, "https://videos.pexels.com@evil.example/x.mp4"));
    CHECK_FALSE(stockHostAllowed(StockProvider::Pexels, "http://videos.pexels.com/x.mp4"));
    CHECK_FALSE(stockHostAllowed(StockProvider::Pexels, "file:///C:/x.mp4"));

    CHECK(stockProviderId(StockProvider::Pixabay) == "pixabay");
    CHECK(stockProviderFromId("pexels") == StockProvider::Pexels);
    CHECK_FALSE(stockProviderFromId("shutterstock").has_value());
}
