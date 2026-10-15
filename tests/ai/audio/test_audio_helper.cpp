// A.4: the audio requests of the wabcut-embed protocol and AudioHelper's process handling against the scripted helper
// (tests/ai/search/fake_embed.cpp: a one-pole low-pass for enhance, four weighted copies for stems): answers, a crash
// retried once, a crash that keeps happening, a timeout, a cancel, garbage and a wrong answer. Never a model.

#include "../AiTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/audio/AudioHelper.h>
#include <wabcut/ai/audio/AudioModels.h>
#include <wabcut/ai/search/EmbedProtocol.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai::audio;
namespace embed = wabcut::ai::search::embed;

namespace {

AudioHelperOptions fakeHelper(const std::string& mode, std::vector<std::string> extra = {}) {
    AudioHelperOptions o;
    o.executable = WABCUT_FAKE_EMBED;
    o.arguments = {"--mode", mode};
    o.arguments.insert(o.arguments.end(), extra.begin(), extra.end());
    o.speechModel = "deepfilternet3";
    o.stemModel = "htdemucs.onnx";
    o.workDir = ai::test::freshDir("audio-helper");
    o.startTimeout = std::chrono::seconds(10);
    o.loadTimeout = std::chrono::seconds(10);
    o.requestTimeout = std::chrono::seconds(10);
    return o;
}

std::vector<float> tone(std::size_t frames, int channels) {
    std::vector<float> out(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = 0.5f * std::sin(static_cast<float>(i) * 0.003f);
    }
    return out;
}

std::vector<float> lowpass(const std::vector<float>& x, int channels) {
    std::vector<float> y(x.size());
    std::vector<float> s(static_cast<std::size_t>(channels), 0.0f);
    for (std::size_t i = 0; i < x.size(); ++i) {
        float& v = s[i % static_cast<std::size_t>(channels)];
        v += 0.002f * (x[i] - v);
        y[i] = v;
    }
    return y;
}

bool emptyDir(const std::filesystem::path& dir) {
    return std::filesystem::is_empty(dir);
}

} // namespace

TEST_CASE("The audio models are pinned and on the licence allow-list, like every built-in model", "[ai][audio][models]") {
    for (const ai::ModelInfo& m : ai::builtinModels()) {
        INFO(m.id);
        CHECK((ai::isAllowedModelLicense(m.license) || ai::requiresLicenseOptIn(m)));
        CHECK(m.url.starts_with("https://"));
        CHECK(m.sha256.size() == 64);
        CHECK(m.size > 0);
        CHECK_FALSE(m.licenseCard.empty());
    }
    const ai::ModelInfo& speech = speechModel();
    CHECK(speech.kind == ai::ModelKind::Audio);
    CHECK(speech.license == "MIT");
    CHECK(speech.url.find("84d57ec2c08fe08e68a13fb32a58cd7092060a0f") != std::string::npos); // a commit, not a branch
    CHECK(speech.sha256 == "c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616");
    CHECK(speechModelMembers().size() == 4);
    const ai::ModelInfo& stems = stemModel();
    CHECK(stems.license == "MIT");
    CHECK(stems.url.find("d54ed9eb60e258ea82131c6ee14578628816456a") != std::string::npos);
    CHECK(stems.size == 165612636);
    CHECK(speechModelVersion().starts_with("deepfilternet3@c94d91f70911/"));
    CHECK(stemModelVersion().starts_with("htdemucs@d05c269d0178/"));
    CHECK(stemModelSpec().stems.size() == 4);
    // Nothing installed in an empty store; the extractor is absolute.
    ai::ModelStore store(ai::test::freshDir("audio-models-store"));
    CHECK_FALSE(speechModelInstalled(store));
    CHECK(defaultExtractor().is_absolute());
    auto missing = installSpeechModel(store, nullptr, defaultExtractor());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound); // not downloaded, no transport: nothing is fetched
}

TEST_CASE("Audio messages of the wabcut-embed protocol round-trip and keep to their bounds", "[ai][audio][protocol]") {
    embed::Load load;
    load.id = "1";
    load.speech = "C:/models/deepfilternet3";
    load.stems = "C:/models/htdemucs.onnx";
    auto back = embed::decodeRequest(embed::encode(embed::Request{load}));
    REQUIRE(back);
    const auto* l = std::get_if<embed::Load>(&*back);
    REQUIRE(l);
    CHECK(l->speech == load.speech);
    CHECK(l->stems == load.stems);

    const embed::AudioBlock block{"C:/tmp/in.f32", 2, 48000, 48000};
    back = embed::decodeRequest(embed::encode(embed::Request{embed::EnhanceAudio{"2", block, "C:/tmp/out.f32"}}));
    REQUIRE(back);
    const auto* e = std::get_if<embed::EnhanceAudio>(&*back);
    REQUIRE(e);
    CHECK(e->audio.channels == 2);
    CHECK(e->audio.frames == 48000);
    CHECK(e->output == std::filesystem::path("C:/tmp/out.f32"));
    back = embed::decodeRequest(embed::encode(embed::Request{embed::SeparateAudio{"3", block, "C:/tmp/stems.f32"}}));
    REQUIRE(back);
    CHECK(std::holds_alternative<embed::SeparateAudio>(*back));

    auto reply = embed::decodeResponse(embed::encode(embed::Response{embed::AudioDone{"2", 48000, 2, 4, 12.5, "DirectML"}}));
    REQUIRE(reply);
    const auto* done = std::get_if<embed::AudioDone>(&*reply);
    REQUIRE(done);
    CHECK(done->outputs == 4);
    CHECK(done->device == "DirectML");

    // Out of bounds: too many channels or frames, no output, a reply of zero frames.
    CHECK_FALSE(embed::decodeRequest(R"({"type":"enhance_audio","id":"4","audio":{"path":"a","channels":9,"frames":10,"rate":48000},"output":"b"})"));
    CHECK_FALSE(embed::decodeRequest(R"({"type":"enhance_audio","id":"4","audio":{"path":"a","channels":1,"frames":99999999,"rate":48000},"output":"b"})"));
    CHECK_FALSE(embed::decodeRequest(R"({"type":"separate_audio","id":"4","audio":{"path":"a","channels":2,"frames":10,"rate":44100}})"));
    CHECK_FALSE(embed::decodeResponse(R"({"type":"audio","id":"4","frames":0,"channels":1,"outputs":1})"));

    // Blocks: written and read back exactly; a short file is an error.
    const auto dir = ai::test::freshDir("audio-blocks");
    const std::vector<float> samples = tone(100, 2);
    auto written = embed::writeAudioBlock(dir / "b.f32", samples, 2, 48000);
    REQUIRE(written);
    CHECK(written->frames == 100);
    auto read = embed::readAudioBlock(*written);
    REQUIRE(read);
    CHECK(*read == samples);
    CHECK_FALSE(embed::readAudioBlock(*written, 2));
    CHECK_FALSE(embed::writeAudioBlock(dir / "c.f32", std::vector<float>(3, 0.0f), 2, 48000)); // not whole frames
}

TEST_CASE("AudioHelper enhances and separates through the helper, leaving no block files", "[ai][audio][helper]") {
    AudioHelperOptions options = fakeHelper("normal");
    AudioHelper helper(options);
    const std::vector<float> input = tone(4800, 2);
    auto enhanced = helper.enhance(input, 2, {});
    REQUIRE(enhanced);
    CHECK(*enhanced == lowpass(input, 2));
    CHECK(helper.device() == "FakeCPU");
    CHECK(helper.inferenceMilliseconds() > 0.0);

    auto stems = helper.separate(input, 44100, 4, {});
    REQUIRE(stems);
    REQUIRE(stems->size() == 4 * input.size());
    for (std::size_t i = 0; i < input.size(); i += 101) {
        float sum = 0.0f;
        for (std::size_t s = 0; s < 4; ++s) {
            sum += (*stems)[s * input.size() + i];
        }
        CHECK(std::fabs(sum - input[i]) < 1e-6f);
    }
    CHECK(helper.starts() == 1);
    CHECK(emptyDir(options.workDir));
    CHECK_FALSE(helper.enhance(std::vector<float>(3, 0.0f), 2, {})); // not whole frames
}

TEST_CASE("AudioHelper failures: a crash is retried once, a repeated crash, a timeout, garbage and a wrong answer fail "
          "cleanly",
          "[ai][audio][helper]") {
    const std::vector<float> input = tone(960, 1);
    {
        const auto marker = ai::test::freshDir("audio-crash-once") / "crashed";
        AudioHelper helper(fakeHelper("crash-once", {"--marker", marker.string()}));
        auto out = helper.enhance(input, 1, {});
        REQUIRE(out); // the restarted helper answered
        CHECK(*out == lowpass(input, 1));
        CHECK(helper.starts() == 2);
    }
    {
        AudioHelper helper(fakeHelper("crash-on-embed"));
        auto out = helper.enhance(input, 1, {});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::DecoderError);
        CHECK(helper.starts() == 2); // retried once, then gave up
    }
    {
        AudioHelperOptions o = fakeHelper("hang-on-embed");
        o.requestTimeout = std::chrono::milliseconds(400);
        AudioHelper helper(o);
        const auto started = std::chrono::steady_clock::now();
        auto out = helper.enhance(input, 1, {});
        REQUIRE_FALSE(out);
        CHECK(out.error().message.find("did not answer") != std::string::npos);
        CHECK(helper.starts() == 1); // a timeout is not retried
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    }
    {
        AudioHelper helper(fakeHelper("garbage"));
        auto out = helper.enhance(input, 1, {});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::DecoderError);
    }
    {
        AudioHelper helper(fakeHelper("wrong-count"));
        auto out = helper.enhance(input, 1, {});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::DecoderError);
        CHECK_FALSE(helper.separate(tone(960, 2), 44100, 4, {})); // three stems for four
    }
    {
        AudioHelper helper(fakeHelper("fail-load"));
        auto out = helper.enhance(input, 1, {});
        REQUIRE_FALSE(out);
        CHECK(out.error().message.find("no such model file") != std::string::npos);
    }
    {
        AudioHelperOptions o = fakeHelper("normal");
        o.speechModel.clear();
        AudioHelper helper(o);
        auto out = helper.enhance(input, 1, {});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::NotFound);
    }
}

TEST_CASE("AudioHelper cancel: a stop ends the request at once and kills the helper", "[ai][audio][helper]") {
    AudioHelper helper(fakeHelper("hang-on-embed"));
    std::stop_source stop;
    std::jthread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    auto out = helper.enhance(tone(960, 1), 1, stop.get_token());
    REQUIRE_FALSE(out);
    CHECK(out.error().code == ErrorCode::Cancelled);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    CHECK(helper.starts() == 1);
}
