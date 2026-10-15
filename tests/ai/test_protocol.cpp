// The wabcut-infer NDJSON protocol codec.

#include <wabcut/ai/InferProtocol.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;

TEST_CASE("Protocol requests round-trip", "[ai][protocol]") {
    const std::vector<infer::Request> requests = {
        infer::Hello{},
        infer::LoadModel{"1", std::filesystem::path(u8"C:/models/ggml-base.bin"), false, 1},
        infer::LoadVad{"2", std::filesystem::path(u8"C:/Users/ผู้ใช้/models/silero.bin")},
        infer::VadRequest{"3", {std::filesystem::path(u8"/tmp/pcm.f32"), 160000, 320000}, 0.4f, 200, 80, 20},
        infer::TranscribeRequest{"4", {std::filesystem::path(u8"/tmp/pcm.f32"), 0, 16000}, "th", 6, 1, true, "Wabcut"},
        infer::Cancel{"4"},
        infer::Shutdown{},
    };
    for (const infer::Request& r : requests) {
        const std::string line = infer::encode(r);
        CHECK(line.find('\n') == std::string::npos);
        auto back = infer::decodeRequest(line);
        REQUIRE(back);
        CHECK(back->index() == r.index());
        CHECK(infer::encode(*back) == line);
    }
    const auto t = std::get<infer::TranscribeRequest>(*infer::decodeRequest(infer::encode(requests[4])));
    CHECK(t.language == "th");
    CHECK(t.pcm.count == 16000);
    CHECK(t.vad);
    const auto v = std::get<infer::LoadVad>(*infer::decodeRequest(infer::encode(requests[2])));
    CHECK(v.path == std::filesystem::path(u8"C:/Users/ผู้ใช้/models/silero.bin"));
}

TEST_CASE("Protocol responses round-trip, token bytes included", "[ai][protocol]") {
    infer::Segment seg;
    seg.id = "7";
    seg.language = "th";
    seg.segment = {10, 250, {{" Hi", 10, 40, 0.9f}, {"\xE0\xB8", 40, 50, 0.5f}, {"\x94", 50, 60, 0.5f}}, true};
    const std::vector<infer::Response> responses = {
        infer::HelloReply{1, "whisper.cpp", "1.9.5", {{"Vulkan0", "NVIDIA GeForce RTX 3060", "gpu"}}},
        infer::Loaded{"1", "Vulkan0", "base"},
        infer::Progress{"7", 42},
        seg,
        infer::VadResult{"3", {{12, 250}, {300, 410}}},
        infer::Result{"7", "en", 3, {{"wallMs", 812.5}}},
        infer::Failure{"7", "cancelled", "stopped"},
    };
    for (const infer::Response& r : responses) {
        const std::string line = infer::encode(r);
        auto back = infer::decodeResponse(line);
        REQUIRE(back);
        CHECK(back->index() == r.index());
        CHECK(infer::encode(*back) == line);
    }
    const auto s = std::get<infer::Segment>(*infer::decodeResponse(infer::encode(seg)));
    REQUIRE(s.segment.tokens.size() == 3);
    CHECK(s.segment.tokens[1].bytes == "\xE0\xB8"); // a cut-off UTF-8 sequence survives the JSON transport
    CHECK(s.segment.speakerTurnNext);
    CHECK(s.language == "th");
    CHECK(infer::responseId(responses[0]).empty());
    CHECK(infer::responseId(responses[2]) == "7");
}

TEST_CASE("Protocol decoding rejects malformed lines", "[ai][protocol]") {
    CHECK_FALSE(infer::decodeRequest("not json"));
    CHECK_FALSE(infer::decodeRequest("[]"));
    CHECK_FALSE(infer::decodeRequest(R"({"type":"warp"})"));
    CHECK_FALSE(infer::decodeRequest(R"({"type":"load_model"})"));
    CHECK_FALSE(infer::decodeRequest(R"({"type":"vad","id":"1","pcm":{"path":"x","offset":-1,"count":5}})"));
    CHECK_FALSE(infer::decodeResponse(R"({"type":"segment","id":"1","t0":0,"t1":1,"tokens":[{"b":"zz","t0":0,"t1":1}]})"));
    CHECK_FALSE(infer::decodeResponse(R"({"type":"progress","id":"1"})"));
    CHECK(infer::fromHex("e0b8") == std::string("\xE0\xB8"));
    CHECK_FALSE(infer::fromHex("abc"));
    CHECK(infer::toHex(std::string("\x00\xFF", 2)) == "00ff");
}
