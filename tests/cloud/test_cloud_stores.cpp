// The cloud library's storage layer (docs/cloud.md): helpers, AWS Signature V4 against the published examples, and one contract
// every store must keep - memory, folder, WebDAV and S3 (the last two over fake servers that check what real servers check).

#include "FakeServers.h"

#include <wabcut/ai/Redact.h>
#include <wabcut/cloud/HttpStores.h>
#include <wabcut/cloud/RemoteStore.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <memory>

using namespace wabcut;
using namespace wabcut::cloud;
using namespace wabcut::cloud::test;
namespace fs = std::filesystem;

TEST_CASE("Cloud helpers: hashes encodings and names", "[cloud]") {
    // RFC 4231 test case 2 and the RFC 4648 base32 / base64 vectors
    CHECK(toHex(hmacSha256("Jefe", "what do ya want for nothing?")) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(base32Lower("foobar") == "mzxw6ytboi");
    CHECK(base32Lower("fo") == "mzxq");
    CHECK(base64Encode("foobar") == "Zm9vYmFy");
    CHECK(base64Encode("fo") == "Zm8=");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    CHECK(uriEncode("a b/c~d", true) == "a%20b%2Fc~d");
    CHECK(uriEncode("a b/c", false) == "a%20b/c");
    CHECK(*uriDecode("a%20b%2Fc") == "a b/c");
    CHECK_FALSE(uriDecode("bad%2").has_value());
    CHECK_FALSE(uriDecode("nul%00").has_value());

    CHECK(validSegment("head.json"));
    CHECK(validSegment("0001-ab12.wabcut"));
    CHECK_FALSE(validSegment(".hidden"));
    CHECK_FALSE(validSegment(".."));
    CHECK_FALSE(validSegment("a b"));
    CHECK_FALSE(validSegment("a\\b"));
    CHECK_FALSE(validSegment(""));
    CHECK(validPath("reviews/abc/index.html"));
    CHECK_FALSE(validPath("reviews//index.html"));
    CHECK_FALSE(validPath("../x"));
    CHECK_FALSE(validPath("a/../b"));
    CHECK_FALSE(validPath("/abs"));
    CHECK_FALSE(validPath("a/b/c/d/e/f/g/h/i"));
    CHECK(joinPath("a", "b") == "a/b");
    CHECK(joinPath("", "b") == "b");
    CHECK(baseName("a/b/c.json") == "c.json");
    CHECK(parentOf("a/b/c.json") == "a/b");
    CHECK(parentOf("c.json").empty());

    // tokens: 128 bits = 26 characters, different each time, only lower-case letters and 2-7
    const RandomBytes random = systemRandom();
    const std::string a = newToken(random);
    const std::string b = newToken(random);
    CHECK(a.size() == 26);
    CHECK(a != b);
    CHECK(a.find_first_not_of("abcdefghijklmnopqrstuvwxyz234567") == std::string::npos);
}

TEST_CASE("Cloud helpers: UTC times", "[cloud]") {
    CHECK(formatIsoUtc(0) == "1970-01-01T00:00:00Z");
    CHECK(formatIsoUtc(1369353600) == "2013-05-24T00:00:00Z");
    CHECK(*parseIsoUtc("2013-05-24T00:00:00Z") == 1369353600);
    CHECK(*parseIsoUtc("2013-05-24") == 1369353600);
    CHECK(*parseIsoUtc("2026-11-05T09:30:15Z") == *parseIsoUtc("2026-11-05T09:30:00Z") + 15);
    CHECK(parseIsoUtc(formatIsoUtc(1793874937)).value() == 1793874937);
    CHECK_FALSE(parseIsoUtc("nonsense").has_value());
    CHECK_FALSE(parseIsoUtc("2026-13-05").has_value());
    CHECK_FALSE(parseIsoUtc("2026-11-05T25:00:00Z").has_value());
}

TEST_CASE("S3 Signature V4 matches the examples AWS publishes", "[cloud][s3]") {
    const std::string empty = sha256Hex("");
    SigV4Request get;
    get.method = "GET";
    get.canonicalUri = "/test.txt";
    get.headers = {{"host", "examplebucket.s3.amazonaws.com"}, {"range", "bytes=0-9"}, {"x-amz-content-sha256", empty}, {"x-amz-date", "20130524T000000Z"}};
    get.payloadHash = empty;
    get.region = "us-east-1";
    get.amzDate = "20130524T000000Z";
    get.accessKey = "AKIAIOSFODNN7EXAMPLE";
    get.secretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    const SigV4Result got = signV4(get);
    CHECK(got.signature == "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    CHECK(got.signedHeaders == "host;range;x-amz-content-sha256;x-amz-date");
    CHECK(got.authorization.find("Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request") != std::string::npos);

    SigV4Request put = get;
    put.method = "PUT";
    put.canonicalUri = "/test%24file.text";
    put.payloadHash = sha256Hex("Welcome to Amazon S3.");
    put.headers = {{"date", "Fri, 24 May 2013 00:00:00 GMT"}, {"host", "examplebucket.s3.amazonaws.com"}, {"x-amz-content-sha256", put.payloadHash},
                   {"x-amz-date", "20130524T000000Z"}, {"x-amz-storage-class", "REDUCED_REDUNDANCY"}};
    CHECK(signV4(put).signature == "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");

    SigV4Request list = get;
    list.canonicalUri = "/";
    list.canonicalQuery = "max-keys=2&prefix=J";
    list.headers = {{"host", "examplebucket.s3.amazonaws.com"}, {"x-amz-content-sha256", empty}, {"x-amz-date", "20130524T000000Z"}};
    CHECK(signV4(list).signature == "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");

    // the same published example, through the store: a presigned link (virtual-hosted style, fixed clock)
    FakeS3 never("examplebucket", "x", "y");
    S3Config config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "examplebucket";
    config.pathStyle = false;
    S3Store store(never, config, ai::Secret("AKIAIOSFODNN7EXAMPLE"), ai::Secret("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"),
                  [] { return std::int64_t{1369353600}; });
    REQUIRE(store.problem().empty());
    const auto link = store.presignGet("test.txt", 86400);
    REQUIRE(link);
    CHECK(link->find("https://examplebucket.s3.amazonaws.com/test.txt?X-Amz-Algorithm=AWS4-HMAC-SHA256") == 0);
    CHECK(link->find("X-Amz-Expires=86400") != std::string::npos);
    CHECK(link->find("X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404") != std::string::npos);
    CHECK_FALSE(store.presignGet("test.txt", 0).has_value());
    CHECK_FALSE(store.presignGet("test.txt", 604801).has_value());
    CHECK_FALSE(store.presignGet("../x", 60).has_value());
}

namespace {

struct Rig {
    std::string name;
    std::unique_ptr<RemoteStore> store;
    std::function<std::size_t()> requestCount;
};

/// Every kind of store the library has, ready to use.
std::vector<Rig> allStores(TempFolder& folder, FakeDav& dav, FakeS3& s3, FakeS3& s3Virtual) {
    std::vector<Rig> rigs;
    rigs.push_back({"memory", std::make_unique<MemoryStore>(), {}});
    rigs.push_back({"folder", std::make_unique<FolderStore>(folder.path / "store"), {}});
    WebDavConfig dc;
    dc.url = "https://dav.example.com/dav/wabcut";
    dc.user = "ann";
    rigs.push_back({"webdav", std::make_unique<WebDavStore>(dav, dc, ai::Secret("correct horse battery")), [&dav] { return dav.requests.size(); }});
    S3Config sc;
    sc.endpoint = "https://s3.example.com";
    sc.bucket = "reviews";
    sc.prefix = "wabcut";
    rigs.push_back({"s3 path style", std::make_unique<S3Store>(s3, sc, ai::Secret("AKIATESTKEY0001"), ai::Secret("secret-secret-secret-1234"), [] { return std::int64_t{1793874937}; }),
                    [&s3] { return s3.requests.size(); }});
    S3Config vc = sc;
    vc.pathStyle = false;
    vc.prefix = "";
    rigs.push_back({"s3 virtual hosted", std::make_unique<S3Store>(s3Virtual, vc, ai::Secret("AKIATESTKEY0001"), ai::Secret("secret-secret-secret-1234"), [] { return std::int64_t{1793874937}; }),
                    [&s3Virtual] { return s3Virtual.requests.size(); }});
    return rigs;
}

} // namespace

TEST_CASE("Every store keeps the same contract: write read list conditions delete", "[cloud]") {
    TempFolder folder("contract");
    FakeDav dav("/dav/wabcut", "ann", "correct horse battery");
    FakeS3 s3("reviews", "AKIATESTKEY0001", "secret-secret-secret-1234");
    FakeS3 s3v("reviews", "AKIATESTKEY0001", "secret-secret-secret-1234", "us-east-1", false);
    for (Rig& rig : allStores(folder, dav, s3, s3v)) {
        INFO(rig.name);
        RemoteStore& store = *rig.store;
        // nothing yet
        auto missing = store.stat("head.json");
        REQUIRE(missing);
        CHECK_FALSE(missing->has_value());
        auto notThere = store.get("head.json", 1000);
        REQUIRE_FALSE(notThere);
        CHECK(notThere.error().kind == StoreFailure::NotFound);
        auto empty = store.list("");
        REQUIRE(empty);
        CHECK(empty->empty());

        // create-if-absent: the first wins, the second is refused
        auto first = store.put("proj/lock.json", "{\"who\":\"ann\"}", Precondition::absent(), "application/json");
        REQUIRE(first);
        CHECK_FALSE(first->empty());
        auto second = store.put("proj/lock.json", "{\"who\":\"bob\"}", Precondition::absent(), "application/json");
        REQUIRE_FALSE(second);
        CHECK(second.error().kind == StoreFailure::PreconditionFailed);
        auto read = store.get("proj/lock.json", 1000);
        REQUIRE(read);
        CHECK(read->data == "{\"who\":\"ann\"}");
        CHECK(read->etag == *first);

        // compare-and-swap: the right ETag wins once, the stale one is refused
        auto swapped = store.put("proj/lock.json", "{\"who\":\"ann\",\"n\":2}", Precondition::matches(*first));
        REQUIRE(swapped);
        CHECK(*swapped != *first);
        auto stale = store.put("proj/lock.json", "{\"who\":\"bob\"}", Precondition::matches(*first));
        REQUIRE_FALSE(stale);
        CHECK(stale.error().kind == StoreFailure::PreconditionFailed);
        auto onMissing = store.put("proj/none.json", "x", Precondition::matches("\"abc\""));
        REQUIRE_FALSE(onMissing);
        CHECK(onMissing.error().kind == StoreFailure::PreconditionFailed);
        CHECK(store.get("proj/lock.json", 1000)->data == "{\"who\":\"ann\",\"n\":2}");

        // stat and list
        REQUIRE(store.put("proj/revisions/00000001-aa.wabcut", "PROJECT ONE", Precondition::none()));
        REQUIRE(store.put("proj/revisions/00000002-bb.wabcut", "PROJECT TWO", Precondition::none()));
        auto entry = store.stat("proj/revisions/00000002-bb.wabcut");
        REQUIRE(entry);
        REQUIRE(entry->has_value());
        CHECK((*entry)->size == 11);
        CHECK_FALSE((*entry)->etag.empty());
        auto top = store.list("proj");
        REQUIRE(top);
        std::sort(top->begin(), top->end(), [](const StoreEntry& a, const StoreEntry& b) { return a.path < b.path; }); // (stores differ in order)
        REQUIRE(top->size() == 2);
        CHECK((*top)[0].path == "proj/lock.json");
        CHECK_FALSE((*top)[0].folder);
        CHECK((*top)[1].path == "proj/revisions");
        CHECK((*top)[1].folder);
        auto revisions = store.list("proj/revisions");
        REQUIRE(revisions);
        CHECK(revisions->size() == 2);
        auto all = store.listAll("proj");
        REQUIRE(all);
        CHECK(all->size() == 3);

        // a too-large answer is refused, not read
        auto tooBig = store.get("proj/revisions/00000001-aa.wabcut", 5);
        REQUIRE_FALSE(tooBig);
        CHECK(tooBig.error().kind == StoreFailure::TooLarge);

        // files in and out
        TempFolder work("work");
        writeAll(work.path / "up.bin", std::string(300000, 'z') + "end");
        auto uploaded = store.putFile("proj/up.bin", work.path / "up.bin", Precondition::absent(), "application/octet-stream");
        REQUIRE(uploaded);
        auto downloaded = store.getToFile("proj/up.bin", work.path / "down" / "down.bin", 1u << 20);
        REQUIRE(downloaded);
        CHECK(readAll(work.path / "down" / "down.bin") == std::string(300000, 'z') + "end");
        auto refused = store.getToFile("proj/up.bin", work.path / "small.bin", 100);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().kind == StoreFailure::TooLarge);
        CHECK_FALSE(fs::exists(work.path / "small.bin"));

        // names that are not ours never reach the storage
        for (const char* bad : {"../escape.json", "a/../b.json", "/abs.json", "has space.json", "proj//x.json", ".hidden"}) {
            auto r = store.put(bad, "x", Precondition::none());
            REQUIRE_FALSE(r);
            CHECK(r.error().kind == StoreFailure::Invalid);
        }
        CHECK_FALSE(store.get("../x", 10));

        // delete: a missing object is not an error; a tree goes whole
        REQUIRE(store.remove("proj/lock.json"));
        REQUIRE(store.remove("proj/lock.json"));
        CHECK_FALSE((*store.stat("proj/lock.json")).has_value());
        auto removed = store.removeTree("proj");
        REQUIRE(removed);
        CHECK(*removed == 3);
        auto left = store.listAll("");
        REQUIRE(left);
        CHECK(left->empty());
    }
}

TEST_CASE("A store reports what went wrong", "[cloud]") {
    SECTION("memory store failures can be injected and counted") {
        MemoryStore store;
        store.failNext("get", "", StoreError{StoreFailure::Network, "no connection"});
        auto first = store.get("a.json", 10);
        REQUIRE_FALSE(first);
        CHECK(first.error().kind == StoreFailure::Network);
        CHECK(store.get("a.json", 10).error().kind == StoreFailure::NotFound);
        CHECK(store.calls("get") == 2);
        CHECK(toError(StoreError{StoreFailure::NotFound, "x"}).code == ErrorCode::NotFound);
    }
    SECTION("wrong WebDAV password") {
        FakeDav dav("/dav/wabcut", "ann", "right");
        WebDavConfig config;
        config.url = "https://dav.example.com/dav/wabcut/";
        config.user = "ann";
        WebDavStore store(dav, config, ai::Secret("wrong"));
        auto r = store.put("a.json", "x", Precondition::none());
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == StoreFailure::Unauthorized);
        CHECK(r.error().message.find("wrong") == std::string::npos);
    }
    SECTION("wrong S3 secret key: the signature does not match, and the message holds no secret") {
        FakeS3 s3("reviews", "AKIATESTKEY0001", "the-real-secret-key-0001");
        S3Config config;
        config.endpoint = "https://s3.example.com";
        config.bucket = "reviews";
        S3Store store(s3, config, ai::Secret("AKIATESTKEY0001"), ai::Secret("a-different-secret-key"));
        auto r = store.get("a.json", 100);
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == StoreFailure::Unauthorized);
        CHECK(r.error().message.find("a-different-secret-key") == std::string::npos);
    }
    SECTION("settings are checked before anything is sent") {
        FakeDav dav("/d", "u", "p");
        WebDavConfig plain;
        plain.url = "http://nas.example.com/dav";
        plain.user = "ann";
        WebDavStore http(dav, plain, ai::Secret("secret-password"));
        CHECK_FALSE(http.problem().empty());
        CHECK(http.put("a.json", "x", Precondition::none()).error().kind == StoreFailure::Invalid);
        plain.allowInsecure = true;
        CHECK(WebDavStore(dav, plain, ai::Secret("secret-password")).problem().empty());
        plain.allowInsecure = false;
        plain.url = "http://localhost:8080/dav";
        CHECK(WebDavStore(dav, plain, ai::Secret("secret-password")).problem().empty()); // this computer
        plain.url = "https://ann:pw@cloud.example.com/dav";
        CHECK_FALSE(WebDavStore(dav, plain, ai::Secret("secret-password")).problem().empty());
        plain.url = "https://cloud.example.com/dav?x=1";
        CHECK_FALSE(WebDavStore(dav, plain, ai::Secret("secret-password")).problem().empty());
        plain.url = "ftp://cloud.example.com/dav";
        CHECK_FALSE(WebDavStore(dav, plain, ai::Secret("secret-password")).problem().empty());
        CHECK(endpointProblem("https://s3.example.com", false).empty());
        CHECK_FALSE(endpointProblem("http://192.168.1.5:9000", false).empty());

        FakeS3 s3("b", "k", "s");
        S3Config c;
        c.endpoint = "https://s3.example.com/with/path";
        c.bucket = "b";
        CHECK_FALSE(S3Store(s3, c, ai::Secret("k"), ai::Secret("s")).problem().empty());
        c.endpoint = "https://s3.example.com";
        c.bucket = "";
        CHECK_FALSE(S3Store(s3, c, ai::Secret("k"), ai::Secret("s")).problem().empty());
        c.bucket = "b";
        c.prefix = "bad prefix";
        CHECK_FALSE(S3Store(s3, c, ai::Secret("k"), ai::Secret("s")).problem().empty());
        c.prefix = "a/b";
        CHECK(S3Store(s3, c, ai::Secret("k"), ai::Secret("s")).problem().empty());
    }
}

TEST_CASE("S3 requests are signed and never carry the secret anywhere else", "[cloud][s3]") {
    FakeS3 s3("reviews", "AKIATESTKEY0001", "secret-secret-secret-1234");
    S3Config config;
    config.endpoint = "https://s3.example.com:9443";
    config.bucket = "reviews";
    config.prefix = "team/wabcut";
    S3Store store(s3, config, ai::Secret("AKIATESTKEY0001"), ai::Secret("secret-secret-secret-1234"), [] { return std::int64_t{1793874937}; });
    REQUIRE(store.put("p/head.json", "{\"revision\":1}", Precondition::absent(), "application/json"));
    REQUIRE(store.get("p/head.json", 100));
    REQUIRE(store.list("p"));
    REQUIRE(store.remove("p/head.json"));
    REQUIRE_FALSE(s3.requests.empty());
    for (const Recorded& r : s3.requests) {
        CHECK(ai::findHeader(r.headers, "authorization").has_value());
        CHECK(ai::findHeader(r.headers, "x-amz-content-sha256").has_value());
        CHECK(ai::findHeader(r.headers, "x-amz-date").has_value());
        CHECK(r.url.find("secret-secret") == std::string::npos);
        CHECK(r.url.find("https://s3.example.com:9443/reviews") == 0); // path style: the bucket, then the key (or the listing query)
    }
    // the create-if-absent write says so
    CHECK(ai::findHeader(s3.requests[0].headers, "if-none-match").value_or("") == "*");
    // the key and the secret appear only inside the Authorization header (the Credential holds the key id, never the secret)
    CHECK(s3.everythingSent().find("secret-secret-secret-1234") == std::string::npos);
}

TEST_CASE("S3 listings follow continuation pages", "[cloud][s3]") {
    FakeS3 s3("reviews", "AKIATESTKEY0001", "secret-secret-secret-1234");
    S3Config config;
    config.endpoint = "https://s3.example.com";
    config.bucket = "reviews";
    S3Store store(s3, config, ai::Secret("AKIATESTKEY0001"), ai::Secret("secret-secret-secret-1234"));
    for (int i = 0; i < 7; ++i) {
        REQUIRE(store.put("many/file" + std::to_string(i) + ".json", "x", Precondition::none()));
    }
    s3.pageSize = 3;
    s3.requests.clear();
    auto listed = store.list("many");
    REQUIRE(listed);
    CHECK(listed->size() == 7);
    CHECK(s3.requests.size() == 3);
}

TEST_CASE("WebDAV: folders are made as needed hostile listings are ignored", "[cloud][webdav]") {
    FakeDav dav("/dav/wabcut", "ann", "pw-pw-pw-pw");
    WebDavConfig config;
    config.url = "https://dav.example.com/dav/wabcut";
    config.user = "ann";
    WebDavStore store(dav, config, ai::Secret("pw-pw-pw-pw"));
    REQUIRE(store.put("a/b/c/file.json", "x", Precondition::none()));
    CHECK(dav.folders.contains("a"));
    CHECK(dav.folders.contains("a/b"));
    CHECK(dav.folders.contains("a/b/c"));
    // a second write to the same folder makes no MKCOL again
    std::size_t mkcols = 0;
    for (const Recorded& r : dav.requests) {
        mkcols += r.method == "MKCOL" ? 1 : 0;
    }
    CHECK(mkcols == 3);
    REQUIRE(store.put("a/b/c/second.json", "y", Precondition::none()));
    std::size_t after = 0;
    for (const Recorded& r : dav.requests) {
        after += r.method == "MKCOL" ? 1 : 0;
    }
    CHECK(after == 3);

    // hrefs that leave the store's root, climb with .., or are not valid names never become entries
    dav.extraEntries =
        "<d:response><d:href>/etc/passwd</d:href><d:propstat><d:prop><d:getetag>\"x\"</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>/dav/wabcut/a/b/c/../../../../secret.txt</d:href><d:propstat><d:prop><d:getetag>\"x\"</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>https://evil.example.com/dav/wabcut/a/b/c/evil.json</d:href><d:propstat><d:prop><d:getetag>\"x\"</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>/dav/wabcut/a/b/c/bad%20name.json</d:href><d:propstat><d:prop><d:getetag>\"x\"</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>/dav/wabcut/a/b/c/%2e%2e%2fup.json</d:href><d:propstat><d:prop><d:getetag>\"x\"</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
    auto listed = store.list("a/b/c");
    REQUIRE(listed);
    std::set<std::string> names;
    for (const StoreEntry& e : *listed) {
        names.insert(e.path);
    }
    CHECK(names == std::set<std::string>{"a/b/c/file.json", "a/b/c/second.json", "a/b/c/evil.json"});
    // (evil.json is under the store's own path on another host: only the path counts, so the name is the one we asked for)
}

TEST_CASE("WebDAV answers that are not XML or are huge are refused", "[cloud][webdav]") {
    struct Canned : ai::IHttpTransport {
        std::string body;
        std::expected<ai::HttpResponse, ai::AiError> send(const ai::HttpRequest& r, const ai::HttpStreamHandlers& h, std::stop_token) override {
            return answer(r, h, 207, {}, body);
        }
    } canned;
    WebDavConfig config;
    config.url = "https://dav.example.com/dav";
    config.user = "ann";
    WebDavStore store(canned, config, ai::Secret("pw-pw-pw-pw"));
    canned.body = "<<<not xml";
    CHECK_FALSE(store.list(""));
    // entities that would expand are not expanded; the document simply has no entries
    canned.body = "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;&lol;&lol;\">]>"
                  "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>&lol2;</d:href></d:response></d:multistatus>";
    auto listed = store.list("");
    if (listed) {
        CHECK(listed->empty());
    }
    canned.body = std::string(20u * 1024u * 1024u, ' ');
    CHECK_FALSE(store.list(""));
}

TEST_CASE("Folder store: nothing outside the root links are not followed odd names are reported", "[cloud]") {
    TempFolder folder("folder");
    FolderStore store(folder.path / "root");
    REQUIRE(store.put("proj/head.json", "{}", Precondition::none()));
    CHECK(store.localPath("proj/head.json") == (folder.path / "root" / "proj" / "head.json").lexically_normal());
    CHECK(store.localPath("../outside").empty());
    CHECK(store.localPath("a/../../outside").empty());
    CHECK(store.localPath("").lexically_normal() == (folder.path / "root").lexically_normal());
    // what a sync client leaves behind when two computers wrote the same file
    writeAll(folder.path / "root" / "proj" / "head (conflicted copy 2026-11-05).json", "{}");
    writeAll(folder.path / "root" / "proj" / ".DS_Store", "x");
    writeAll(folder.path / "root" / "proj" / "desktop.ini", "x");
    const auto odd = store.oddNames("proj");
    REQUIRE(odd.size() == 1);
    CHECK(odd[0] == "head (conflicted copy 2026-11-05).json");
    // and the listing does not offer them as objects
    auto listed = store.list("proj");
    REQUIRE(listed);
    CHECK(listed->size() == 2); // head.json and desktop.ini (a valid name); the conflicted copy and .DS_Store are not offered
    // a failed write leaves no temporary file behind
    CHECK_FALSE(store.put("proj/head.json", "{}", Precondition::absent()));
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(folder.path / "root" / "proj")) {
        files += e.path().extension() == ".json" || e.path().filename() == ".DS_Store" || e.path().filename() == "desktop.ini" ? 1 : 0;
        CHECK(e.path().string().find(".part") == std::string::npos);
    }
    CHECK(files == 4);
    // removing the last file of a folder removes the folder (a revoked review link leaves nothing)
    REQUIRE(store.put("reviews/abc/index.html", "<html>", Precondition::none()));
    REQUIRE(store.remove("reviews/abc/index.html"));
    CHECK_FALSE(fs::exists(folder.path / "root" / "reviews"));
}

TEST_CASE("Signed S3 links and temporary credentials are redacted wherever a URL or header is logged", "[cloud][s3]") {
    const std::string url = "https://examplebucket.s3.amazonaws.com/test.txt?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
                            "&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&X-Amz-SignedHeaders=host&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404";
    const std::string redacted = ai::Redactor::global().redactUrl(url);
    CHECK(redacted.find("aeeed9bb") == std::string::npos);
    CHECK(redacted.find("AKIAIOSFODNN7EXAMPLE") == std::string::npos);
    CHECK(redacted.find("examplebucket.s3.amazonaws.com/test.txt") != std::string::npos);
    CHECK(ai::isSecretHeader("x-amz-security-token"));
    CHECK(ai::isSecretHeader("Authorization"));
    const ai::HttpHeaders headers{{"Authorization", "AWS4-HMAC-SHA256 Credential=AKIA.../Signature=abc"}, {"x-amz-date", "20130524T000000Z"}};
    const ai::HttpHeaders safe = ai::Redactor::global().redactHeaders(headers);
    CHECK(safe[0].value.find("Signature=abc") == std::string::npos);
    CHECK(safe[1].value == "20130524T000000Z");
}
