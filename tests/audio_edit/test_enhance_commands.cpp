// A.4: audio.enhanceSpeech and audio.separateStems through the CommandEngine with fake local models: the take is
// recorded on the media item and the effect put first in each clip's stack (one undo step), a ready take is not
// rendered again, video clips stand for their linked audio, "render": false only applies the effect, a stop while
// rendering commits nothing; stems become new media items with their provenance, in a bin of their own.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioCommands.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using command::Json;

namespace {

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio_edit" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// Writes an (empty) take file per render, like the real one writes a WAV; counts the renders.
class FakeEnhancer final : public SpeechEnhancer {
public:
    explicit FakeEnhancer(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    std::string modelVersion() const override { return "fake-dfn@1"; }
    std::expected<Take, Error> render(const MediaItem& item, int stream, std::stop_token stop,
                                      const std::function<void(double)>& progress) override {
        ++renders;
        for (const double p : {0.25, 0.5, 1.0}) {
            if (progress) {
                progress(p);
            }
            if (stop.stop_requested()) {
                return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
            }
        }
        Take t;
        const auto file = m_dir / (item.name + "-a" + std::to_string(stream) + ".wav");
        std::ofstream(file) << "take";
        t.path = pathToUtf8(file);
        t.model = modelVersion();
        t.seconds = 10.0;
        t.elapsed = 1.0;
        t.device = "FakeCPU";
        return t;
    }
    int renders = 0;

private:
    std::filesystem::path m_dir;
};

class FakeSeparator final : public StemSeparator {
public:
    explicit FakeSeparator(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    std::string modelVersion() const override { return "fake-demucs@1"; }
    std::expected<Result, Error> separate(const MediaItem& item, int, std::stop_token, const std::function<void(double)>&) override {
        ++calls;
        Result r;
        r.model = modelVersion();
        r.seconds = 10.0;
        r.elapsed = 4.0;
        r.device = "FakeCPU";
        for (const char* part : {"drums", "bass", "other", "vocals"}) {
            Stem s;
            s.part = part;
            s.path = pathToUtf8(m_dir / (item.name + "-" + part + ".wav"));
            s.info.path = s.path;
            s.info.duration = item.info.duration;
            AudioStreamInfo a;
            a.index = 0;
            a.codec = "pcm_f32le";
            a.sampleRate = Rate{44100, 1};
            a.channels = 2;
            a.duration = item.info.duration;
            s.info.audio.push_back(a);
            s.fingerprint.size = 1234;
            s.fingerprint.sampleHash = "00112233445566ff";
            r.stems.push_back(std::move(s));
        }
        return r;
    }
    int calls = 0;

private:
    std::filesystem::path m_dir;
};

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaaaaaBbbbbbbb|
    A1 tgt |AaaaaaaaBbbbbbbb|
    A2     |Mmmmmmmmmmmmmmmm|
)";

struct Fixture {
    explicit Fixture(const std::string& name)
        : dir(scratch(name)), enhancer(std::make_shared<FakeEnhancer>(dir)), separator(std::make_shared<FakeSeparator>(dir)),
          h(Timeline(kPicture).link("V1@0 A1@0"), policy(), ai::edit::editingCommands()) {}

    command::EnginePolicy policy() {
        auto services = std::make_shared<AudioServices>(nullptr, nullptr);
        services->enhancer = enhancer;
        services->separator = separator;
        return command::EnginePolicy{command::CapabilitySet::all(), 64, services};
    }
    const Clip& clip(std::string_view ref) const { return h.sequence(h.state()).findClip(h.clip(ref))->clip(); }
    const MediaItem& media(char letter) const { return *h.state()->findMedia(h.tl.media(letter)); }

    std::filesystem::path dir;
    std::shared_ptr<FakeEnhancer> enhancer;
    std::shared_ptr<FakeSeparator> separator;
    CommandHarness h;
};

} // namespace

TEST_CASE("audio.enhanceSpeech renders each media stream once, records the take and puts the effect first",
          "[audio][edit][enhance]") {
    Fixture f("enhance");
    // A clip with an effect already: Enhance Speech goes before it.
    f.h.ok("clip.addEffect", {{"clips", Json::array({f.h.clip("A1@0").value})}, {"effect", "wabcut.audio.eq3"}});
    const std::size_t undo = f.h.session.undoCount();
    auto r = f.h.ok("audio.enhanceSpeech", {{"clips", Json::array({f.h.clip("V1@0").value, f.h.clip("A1@8").value})}, {"strength", 80}});
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(f.enhancer->renders == 2); // media A (by V1's linked audio) and B
    for (const char* ref : {"A1@0", "A1@8"}) {
        const Clip& c = f.clip(ref);
        REQUIRE_FALSE(c.effects().empty());
        CHECK(c.effects().front().effect == "wabcut.audio.enhanceSpeech");
        CHECK(c.effects().front().enabled);
        REQUIRE(c.effects().front().find("strength"));
        CHECK(*c.effects().front().find("strength") == ParamValue(0.8));
    }
    CHECK(f.clip("A1@0").effects().size() == 2); // the EQ stays, after it
    CHECK(f.clip("V1@0").effects().empty());     // the picture is not touched
    const AudioTake* take = f.media('A').audioTake(0, kEnhanceSpeechTake);
    REQUIRE(take);
    CHECK(take->model == "fake-dfn@1");
    CHECK(take->generation == 1);
    CHECK(std::filesystem::exists(pathFromUtf8(take->path)));
    REQUIRE(r.data["takes"].size() == 2);
    CHECK(r.data["takes"][0]["rendered"] == true);
    CHECK(r.data["takes"][0]["device"] == "FakeCPU");
    CHECK(r.data["clips"][0]["take"] == "ready");
    CHECK(r.data["model"] == "fake-dfn@1");

    // Again with another strength: the takes are ready, nothing is rendered, the effects are updated in place.
    r = f.h.ok("audio.enhanceSpeech", {{"clips", Json::array({f.h.clip("A1@0").value})}, {"strength", 40}});
    CHECK(f.enhancer->renders == 2);
    CHECK(r.data["takes"][0]["rendered"] == false);
    CHECK(f.clip("A1@0").effects().size() == 2);
    CHECK(*f.clip("A1@0").effects().front().find("strength") == ParamValue(0.4));

    // The take file gone (a cleared cache): "missing", rendered again with the next generation.
    std::filesystem::remove(pathFromUtf8(f.media('A').audioTake(0, kEnhanceSpeechTake)->path));
    r = f.h.ok("audio.enhanceSpeech", {{"clips", Json::array({f.h.clip("A1@0").value})}});
    CHECK(f.enhancer->renders == 3);
    CHECK(f.media('A').audioTake(0, kEnhanceSpeechTake)->generation == 2);
    CHECK(*f.clip("A1@0").effects().front().find("strength") == ParamValue(1.0)); // default 100 %

    // Undo takes back the effect and the take record together.
    f.h.session.undo();
    CHECK(f.media('A').audioTake(0, kEnhanceSpeechTake)->generation == 1);
}

TEST_CASE("audio.enhanceSpeech: render false applies the effect only; errors and a stop commit nothing",
          "[audio][edit][enhance]") {
    Fixture f("enhance-plan");
    auto r = f.h.ok("audio.enhanceSpeech", {{"clips", Json::array({f.h.clip("A2@0").value})}, {"render", false}});
    CHECK(f.enhancer->renders == 0);
    CHECK(f.clip("A2@0").effects().front().effect == "wabcut.audio.enhanceSpeech");
    CHECK(r.data["clips"][0]["take"] == "missing");
    CHECK_FALSE(r.warnings.empty()); // plays the original until rendered
    CHECK(f.media('M').audioTakes.empty());

    const std::size_t undo = f.h.session.undoCount();
    CHECK(f.h.errorOf("audio.enhanceSpeech", {{"clips", Json::array({f.h.clip("A2@0").value})}, {"strength", 150}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.enhanceSpeech", {{"clips", Json::array({999999})}}) == "NotFound");
    CHECK(f.h.errorOf("audio.enhanceSpeech", {{"clips", Json::array({f.h.clip("V1@8").value})}}) == "InvalidParams"); // no linked audio
    CHECK(f.h.session.undoCount() == undo);

    // A host without local models: rendering is refused, applying is not.
    CommandHarness bare(Timeline(kPicture), command::EnginePolicy{command::CapabilitySet::all(), 64,
                                                                  std::make_shared<AudioServices>(nullptr, nullptr)},
                        ai::edit::editingCommands());
    CHECK(bare.errorOf("audio.enhanceSpeech", {{"clips", Json::array({bare.clip("A1@0").value})}}) == "InvalidRequest");
    bare.ok("audio.enhanceSpeech", {{"clips", Json::array({bare.clip("A1@0").value})}, {"render", false}});

    // A stop while it renders: Cancelled, nothing committed.
    struct StopAt final : command::ExecutionControl {
        std::optional<command::CommandError> checkpoint(std::string_view message) override {
            if (message.find("(50%)") != std::string_view::npos) {
                return command::CommandError{command::ErrorCode::Cancelled, "stopped", nullptr};
            }
            return std::nullopt;
        }
    } control;
    command::CommandRequest req;
    req.command = "audio.enhanceSpeech";
    req.params = Json{{"clips", Json::array({f.h.clip("A1@0").value})}};
    const auto stopped = f.h.engine.execute(req, &control);
    REQUIRE_FALSE(stopped.ok());
    CHECK(stopped.error->code == command::ErrorCode::Cancelled);
    CHECK(f.media('A').audioTakes.empty());
    CHECK(f.clip("A1@0").effects().empty());
}

TEST_CASE("audio.separateStems adds the stems as media with provenance, in a bin named after the source",
          "[audio][edit][stems]") {
    Fixture f("stems");
    const std::size_t undo = f.h.session.undoCount();
    const std::size_t mediaBefore = f.h.state()->media.size();
    auto r = f.h.ok("audio.separateStems", {{"clip", f.h.clip("A2@0").value}});
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(f.separator->calls == 1);
    const ProjectSnapshot state = f.h.state();
    REQUIRE(state->media.size() == mediaBefore + 4);
    REQUIRE(r.data["stems"].size() == 4);
    CHECK(r.data["stems"][3]["part"] == "vocals");
    CHECK(r.data["model"] == "fake-demucs@1");
    const MediaItem& vocals = *state->findMedia(MediaId{r.data["stems"][3]["media"].get<std::uint64_t>()});
    CHECK(vocals.name == "M (vocals)");
    REQUIRE(vocals.derived);
    CHECK(vocals.derived->source == f.h.tl.media('M'));
    CHECK(vocals.derived->operation == "separateStems");
    CHECK(vocals.derived->model == "fake-demucs@1");
    CHECK(vocals.derived->part == "vocals");
    CHECK(isIso8601Utc(vocals.derived->createdAt));
    const Bin* bin = state->findBin(vocals.bin);
    REQUIRE(bin);
    CHECK(bin->name == "M stems");
    // The provenance is saved and read back.
    const auto json = io::toJson(*state);
    REQUIRE(json);
    const auto back = io::fromJson(*json);
    REQUIRE(back);
    CHECK(back->findMedia(vocals.id)->derived == vocals.derived);

    CHECK(f.h.errorOf("audio.separateStems", {{"clip", f.h.clip("A2@0").value}, {"model", "spleeter"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.separateStems", Json::object()) == "InvalidParams");
    f.h.ok("audio.separateStems", {{"media", f.h.tl.media('B').value}});
    CHECK(f.separator->calls == 2);
}
