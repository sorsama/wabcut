// AnalysisCache keys and invalidation, SHA-256, and ModelStore downloads through a fake transport (no network).

#include "AiTestUtil.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <fstream>
#include <random>
#include <set>

using namespace wabcut;
using namespace wabcut::ai;

namespace {

AnalysisKey key(std::uint64_t fp = 0x1234, std::string model = "whisper/base@60ed5bc3", std::string params = "abc") {
    return {fp, 1, "transcript", std::move(model), std::move(params)};
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 test vectors, streamed in any pieces", "[ai][sha256]") {
    CHECK(Sha256::hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(Sha256::hex(two) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 streamed;
    for (std::size_t i = 0; i < two.size(); i += 7) {
        streamed.update(std::string_view(two).substr(i, 7));
    }
    CHECK(streamed.finishHex() == Sha256::hex(two));
    Sha256 million;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        million.update(chunk);
    }
    CHECK(million.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Analysis cache stores and finds results by source, analyser, model and parameters", "[ai][cache]") {
    AnalysisCache cache(test::freshDir("cache"));
    REQUIRE(cache.put(key(), R"({"words":[1,2,3]})"));
    auto hit = cache.get(key());
    REQUIRE(hit);
    REQUIRE(*hit);
    CHECK(nlohmann::json::parse(**hit) == nlohmann::json::parse(R"({"words":[1,2,3]})"));

    // Any key part that changes misses: another file, stream, analyser, model version or parameter set.
    CHECK_FALSE(*cache.get(key(0x9999)));
    AnalysisKey otherStream = key();
    otherStream.streamIndex = 2;
    CHECK_FALSE(*cache.get(otherStream));
    AnalysisKey otherAnalyzer = key();
    otherAnalyzer.analyzerId = "shots";
    CHECK_FALSE(*cache.get(otherAnalyzer));
    CHECK_FALSE(*cache.get(key(0x1234, "whisper/large-v3-turbo@39422170")));
    CHECK_FALSE(*cache.get(key(0x1234, "whisper/base@60ed5bc3", "def")));

    // Replacing an entry.
    REQUIRE(cache.put(key(), R"({"v":2})"));
    CHECK(nlohmann::json::parse(**cache.get(key()))["v"] == 2);

    // The layout is source-scoped and versioned.
    const auto path = cache.pathFor(key());
    CHECK(path.parent_path().parent_path().parent_path().filename() == "0000000000001234");
    CHECK(path.parent_path().parent_path().parent_path().parent_path().filename() == "v1");
    const auto file = nlohmann::json::parse(readAll(path));
    CHECK(file["schema"] == "wabcut.analysis");
    CHECK(file["version"] == 1);
    CHECK(file["key"]["modelVersion"] == "whisper/base@60ed5bc3");
}

TEST_CASE("Analysis cache invalidation: per source, damaged files, other format versions", "[ai][cache]") {
    AnalysisCache cache(test::freshDir("cache-inv"));
    REQUIRE(cache.put(key(0xA), R"({"a":1})"));
    REQUIRE(cache.put(key(0xA, "m2"), R"({"a":2})"));
    REQUIRE(cache.put(key(0xB), R"({"b":1})"));

    SECTION("invalidating a source removes exactly its entries") {
        CHECK(*cache.invalidateSource(0xA) == 2);
        CHECK_FALSE(*cache.get(key(0xA)));
        CHECK(*cache.get(key(0xB)));
        CHECK(*cache.invalidateSource(0xA) == 0);
    }
    SECTION("a damaged entry is a miss and is removed") {
        std::ofstream(cache.pathFor(key(0xA)), std::ios::trunc) << "{truncated";
        CHECK_FALSE(*cache.get(key(0xA)));
        CHECK_FALSE(std::filesystem::exists(cache.pathFor(key(0xA))));
    }
    SECTION("an entry from another format version is a miss") {
        auto doc = nlohmann::json::parse(readAll(cache.pathFor(key(0xB))));
        doc["version"] = 99;
        std::ofstream(cache.pathFor(key(0xB)), std::ios::trunc) << doc.dump();
        CHECK_FALSE(*cache.get(key(0xB)));
    }
    SECTION("a file holding another key is a miss") {
        std::filesystem::copy_file(cache.pathFor(key(0xB)), cache.pathFor(key(0xA)),
                                   std::filesystem::copy_options::overwrite_existing);
        CHECK_FALSE(*cache.get(key(0xA)));
    }
    SECTION("bad input is refused") {
        CHECK_FALSE(cache.put(key(0), "{}"));
        CHECK_FALSE(cache.put(key(), "not json"));
    }
}

TEST_CASE("Analysis cache keys: hostile ids stay inside the cache, parameter hashes are canonical", "[ai][cache]") {
    AnalysisCache cache(test::freshDir("cache-paths"));
    const AnalysisKey hostile{0x1, 0, "../../etc", "a/b\\c:..", "x y"};
    const auto p = cache.pathFor(hostile);
    const auto rel = std::filesystem::relative(p, cache.root());
    CHECK_FALSE(rel.empty());
    CHECK(*rel.begin() != "..");
    REQUIRE(cache.put(hostile, "{}"));
    CHECK(*cache.get(hostile));
    // Two ids that sanitise to the same characters still get different files.
    CHECK(cache.pathFor({0x1, 0, "a/b", "m", "p"}) != cache.pathFor({0x1, 0, "a_b", "m", "p"}));

    const std::map<std::string, std::string> a{{"language", "th"}, {"beam", "5"}};
    std::map<std::string, std::string> b;
    b["beam"] = "5";
    b["language"] = "th";
    CHECK(hashParams(a) == hashParams(b));
    CHECK(hashParams(a).size() == 16);
    CHECK(hashParams(a) != hashParams({{"language", "en"}, {"beam", "5"}}));
    CHECK(hashParams({{"ab", "c"}}) != hashParams({{"a", "bc"}})); // no ambiguity from concatenation
}

TEST_CASE("Analysis cache exports a source's entries as one JSON sidecar", "[ai][cache]") {
    AnalysisCache cache(test::freshDir("cache-sidecar"));
    REQUIRE(cache.put(key(0xC), R"({"text":"one"})"));
    REQUIRE(cache.put(key(0xC, "other-model"), R"({"text":"two"})"));
    REQUIRE(cache.put(key(0xD), R"({"text":"elsewhere"})"));
    const auto sidecar = nlohmann::json::parse(*cache.exportSidecar(0xC));
    CHECK(sidecar["schema"] == "wabcut.analysis-sidecar");
    CHECK(sidecar["source"]["fingerprint"] == "000000000000000c");
    REQUIRE(sidecar["entries"].size() == 2);
    std::set<std::string> texts;
    for (const auto& e : sidecar["entries"]) {
        texts.insert(e["payload"]["text"].get<std::string>());
        CHECK(e["key"]["analyzer"] == "transcript");
    }
    CHECK(texts == std::set<std::string>{"one", "two"});
    CHECK(nlohmann::json::parse(*cache.exportSidecar(0xE))["entries"].empty());
}

// ---- ModelStore ------------------------------------------------------------------------------------------

namespace {

/// Serves `body` for one URL from memory; optionally ignores Range, drops the connection after `cutAfter`
/// bytes, or corrupts a byte. Counts requests. Never touches the network.
class FakeTransport final : public IDownloadTransport {
public:
    std::string url;
    std::string body;
    bool ignoreRange = false;
    std::size_t cutAfter = std::string::npos; ///< total bytes served before failing (across requests)
    bool corrupt = false;
    int requests = 0;
    std::vector<std::uint64_t> offsets;

    std::expected<Response, Error> get(const std::string& u, std::uint64_t offset,
                                       const std::function<bool(const Response&)>& onResponse,
                                       const std::function<bool(std::span<const std::byte>)>& onData,
                                       std::stop_token stop) override {
        ++requests;
        offsets.push_back(offset);
        if (u != url) {
            Response notFound{404, std::nullopt};
            onResponse(notFound);
            return notFound;
        }
        const std::size_t start = ignoreRange ? 0 : static_cast<std::size_t>(offset);
        Response r{ignoreRange || offset == 0 ? 200 : 206, body.size()};
        if (!onResponse(r)) {
            return std::unexpected(Error{ErrorCode::Cancelled, "stopped"});
        }
        std::string data = body.substr(start);
        if (corrupt && !data.empty()) {
            data[data.size() / 2] ^= 0x5A;
        }
        for (std::size_t i = 0; i < data.size();) {
            if (stop.stop_requested()) {
                return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
            }
            if (served >= cutAfter) {
                return std::unexpected(Error{ErrorCode::IoError, "connection reset"});
            }
            const std::size_t n = std::min<std::size_t>({1000, data.size() - i, cutAfter - served});
            if (!onData(std::as_bytes(std::span(data.data() + i, n)))) {
                return std::unexpected(Error{ErrorCode::Cancelled, "stopped"});
            }
            served += n;
            i += n;
        }
        return r;
    }

private:
    std::size_t served = 0;
};

ModelInfo testModel(const std::string& body) {
    ModelInfo m;
    m.id = "test-model";
    m.fileName = "ggml-test.bin";
    m.url = "https://example.invalid/ggml-test.bin";
    m.size = body.size();
    m.sha256 = Sha256::hex(body);
    m.license = "MIT";
    return m;
}

std::string randomBody(std::size_t n) {
    std::mt19937 rng(42);
    std::string s(n, '\0');
    for (char& c : s) {
        c = static_cast<char>(rng() & 0xFF);
    }
    return s;
}

} // namespace

TEST_CASE("The built-in model manifest is complete and pinned", "[ai][models]") {
    for (const std::string id : {"tiny", "base", "large-v3-turbo-q5_0", "silero-v6.2.0"}) {
        const ModelInfo* m = findModel(id);
        REQUIRE(m != nullptr);
        CHECK(m->sha256.size() == 64);
        CHECK(m->sha256.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK(m->url.starts_with("https://huggingface.co/"));
        CHECK(m->url.find("/resolve/main/") == std::string::npos); // pinned to a revision, not a branch
        CHECK(m->url.ends_with(m->fileName));
        CHECK(m->size > 100'000);
        CHECK(m->license == "MIT");
        CHECK_FALSE(m->licenseCard.empty());
    }
    CHECK(findModel("large-v3-turbo") == findModel("large-v3-turbo-q5_0"));
    CHECK(findModel("silero")->kind == ModelKind::Vad);
    CHECK(findModel("gpt-9") == nullptr);
}

TEST_CASE("ModelStore downloads, verifies and then reuses a model", "[ai][models]") {
    const std::string body = randomBody(25'000);
    const ModelInfo model = testModel(body);
    ModelStore store(test::freshDir("models"));
    FakeTransport net;
    net.url = model.url;
    net.body = body;

    CHECK_FALSE(store.isInstalled(model));
    const auto missing = store.ensure(model, nullptr);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);

    std::vector<DownloadProgress> progress;
    auto path = store.ensure(model, &net, [&](const DownloadProgress& p) { progress.push_back(p); });
    REQUIRE(path);
    CHECK(readAll(*path) == body);
    CHECK(store.isInstalled(model));
    CHECK(store.verify(model));
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back().received == body.size());
    CHECK(progress.back().total == body.size());

    REQUIRE(store.ensure(model, &net)); // installed: no second request
    CHECK(net.requests == 1);

    REQUIRE(store.remove(model));
    CHECK_FALSE(store.isInstalled(model));
}

TEST_CASE("ModelStore resumes an interrupted download with a range request", "[ai][models]") {
    const std::string body = randomBody(25'000);
    const ModelInfo model = testModel(body);
    ModelStore store(test::freshDir("models-resume"));
    FakeTransport net;
    net.url = model.url;
    net.body = body;
    net.cutAfter = 12'000;

    const auto first = store.ensure(model, &net);
    REQUIRE_FALSE(first);
    CHECK(first.error().code == ErrorCode::IoError);
    CHECK_FALSE(store.isInstalled(model));

    net.cutAfter = std::string::npos;
    auto second = store.ensure(model, &net);
    REQUIRE(second);
    CHECK(net.offsets == std::vector<std::uint64_t>{0, 12'000});
    CHECK(readAll(*second) == body);
    CHECK(store.verify(model));

    SECTION("a server that ignores the range restarts cleanly") {
        ModelStore again(test::freshDir("models-norange"));
        FakeTransport plain;
        plain.url = model.url;
        plain.body = body;
        plain.cutAfter = 9'000;
        REQUIRE_FALSE(again.ensure(model, &plain));
        plain.cutAfter = std::string::npos;
        plain.ignoreRange = true;
        auto restarted = again.ensure(model, &plain);
        REQUIRE(restarted);
        CHECK(readAll(*restarted) == body);
    }
}

TEST_CASE("ModelStore rejects corrupt or oversized downloads and stops on cancel", "[ai][models]") {
    const std::string body = randomBody(25'000);
    const ModelInfo model = testModel(body);
    ModelStore store(test::freshDir("models-bad"));
    FakeTransport net;
    net.url = model.url;
    net.body = body;

    SECTION("a corrupted byte fails verification and leaves nothing behind") {
        net.corrupt = true;
        const auto r = store.ensure(model, &net);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
        CHECK(r.error().message.find("SHA-256") != std::string::npos);
        CHECK_FALSE(store.isInstalled(model));
        CHECK_FALSE(std::filesystem::exists(store.pathFor(model)));
        net.corrupt = false;
        CHECK(store.ensure(model, &net)); // the next attempt starts over and succeeds
    }
    SECTION("more bytes than the manifest says") {
        net.body = body + "trailing";
        const auto r = store.ensure(model, &net);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("HTTP errors are reported") {
        net.url = "https://example.invalid/elsewhere";
        const auto r = store.ensure(model, &net);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("HTTP 404") != std::string::npos);
    }
    SECTION("cancellation keeps the partial file for later") {
        std::stop_source stop;
        int calls = 0;
        const auto r = store.ensure(model, &net, [&](const DownloadProgress&) {
            if (++calls == 3) {
                stop.request_stop();
            }
        }, stop.get_token());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Cancelled);
        net.offsets.clear();
        REQUIRE(store.ensure(model, &net));
        CHECK(net.offsets.front() == 3000); // resumed where it stopped
    }
    SECTION("a damaged installed file is found by verify()") {
        REQUIRE(store.ensure(model, &net));
        {
            std::fstream f(store.pathFor(model), std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(100);
            f.put('X');
        }
        CHECK_FALSE(store.verify(model));
    }
}

TEST_CASE("The user cache directory honours WABCUT_CACHE_DIR", "[ai][models]") {
    CHECK_FALSE(userCacheDir().empty());
    CHECK(ModelStore::defaultRoot().filename() == "models");
    CHECK(AnalysisCache::defaultRoot().filename() == "analysis");
}
