// AI video effects (0.7) on the command surface: effect.aiStatus (a read) and effect.processAi (a job) through the engine on a
// project whose clips have AI Restore / AI Denoise, with a fake backend and frame source - no model, no media. The state of a
// clip, a refusal without the helper or the models, processing into the cache (resumable, cancellable, dry run), no undo step.

#include "VideoTestUtil.h"

#include "CommandHarness.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/VideoCommands.h>
#include <wabcut/model/FinishingOps.h>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace wabcut;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;
using wabcut::test::CommandHarness;
using wabcut::test::Timeline;
using Json = command::Json;

namespace {

command::CommandRegistry& registry() {
    static command::CommandRegistry r = [] {
        command::CommandRegistry out;
        registerVideoCommands(out);
        return out;
    }();
    return r;
}

class FakeFiles final : public IFrameSource {
public:
    std::expected<std::optional<SourceFrame>, Error> frameAt(Ticks time, const std::stop_token& stop) override {
        return m_source.frameAt(time, stop);
    }

private:
    FakeSource m_source{100000, 16, 12};
};

void installModels(const std::filesystem::path& root, const AiChain& chain) {
    const ai::ModelStore store(root);
    for (const ai::ModelInfo* info : modelFilesOf(chain)) {
        REQUIRE(info != nullptr);
        const std::filesystem::path file = store.pathFor(*info);
        std::filesystem::create_directories(file.parent_path());
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << 'x';
        }
        std::filesystem::resize_file(file, info->size);
        std::filesystem::path marker = file;
        marker += ".verified";
        std::ofstream(marker, std::ios::trunc) << info->sha256;
    }
}

struct Fixture {
    std::shared_ptr<ProcessedCache> cache;
    std::shared_ptr<FakeBackend> backend = std::make_shared<FakeBackend>();
    std::filesystem::path models;
    std::unique_ptr<CommandHarness> harness;
    EffectInstanceId effect;

    explicit Fixture(const std::string& name, bool withServices = true, bool withModels = true) {
        cache = std::make_shared<ProcessedCache>(freshDir(name + "-cache"));
        models = freshDir(name + "-models");
        Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
        tl.linkMatching();
        const ClipId clip = wabcut::test::clipAt(tl.state, tl.seq, "V1@0");
        const SequenceId seq = tl.seq;
        auto added = edit::addEffect(tl.state, seq, std::span(&clip, 1), std::string(kAiRestoreEffect));
        REQUIRE(added.has_value());
        tl.state = std::move(added->state);
        effect = tl.state.findSequence(seq)->findClip(clip)->clip().effects().front().id;
        std::shared_ptr<command::CommandServices> services;
        if (withServices) {
            services = std::make_shared<VideoServices>(cache, backend, [](const std::string&, int) -> std::unique_ptr<IFrameSource> {
                return std::make_unique<FakeFiles>();
            }, models);
        }
        command::EnginePolicy policy{command::CapabilitySet::all(), 16, services};
        harness = std::make_unique<CommandHarness>(std::move(tl), policy, registry());
        AiChain chain;
        AiStage stage;
        stage.effect = std::string(kAiRestoreEffect);
        stage.deblur = 50;
        chain.stages.push_back(stage);
        if (withModels) {
            installModels(models, chain);
        }
    }
};

}

TEST_CASE("effect.aiStatus reports the chain, the models and the pictures of each clip", "[ai][video][commands]") {
    Fixture f("cmd-status");
    auto result = f.harness->ok("effect.aiStatus");
    const auto& data = result.data;
    REQUIRE(data.contains("clips"));
    REQUIRE(data["clips"].size() == 1);
    const auto& clip = data["clips"][0];
    CHECK(clip["chain"] == "Restore");
    CHECK(clip["track"] == "V1");
    CHECK(clip["state"] == "notProcessed");
    CHECK(clip["frames"].get<int>() > 10);
    CHECK(clip["processed"] == 0);
    REQUIRE(clip["models"].size() == 1);
    CHECK(clip["models"][0]["name"] == "NAFNet deblurring");
    CHECK(clip["models"][0]["installed"] == true);
    CHECK(clip["models"][0]["license"] == "MIT");
    CHECK(data["pending"] == 1);
    // One clip by id, and a clip with nothing to do is refused with a reason.
    auto one = f.harness->ok("effect.aiStatus", Json{{"clip", f.harness->clip("V1@0").value}});
    CHECK(one.data["count"] == 1);
    CHECK(f.harness->errorOf("effect.aiStatus", Json{{"clip", f.harness->clip("A1@0").value}}) == "NotFound");
}

TEST_CASE("effect.aiStatus without the services says what is missing", "[ai][video][commands]") {
    Fixture f("cmd-status-bare", false, false);
    auto result = f.harness->ok("effect.aiStatus");
    const auto& clip = result.data["clips"][0];
    CHECK(clip["state"] == "noHelper");
    CHECK(clip["models"][0]["installed"] == false);
    CHECK(f.harness->errorOf("effect.processAi") == "InvalidRequest"); // this host cannot process
}

TEST_CASE("effect.processAi makes the pictures into the cache, resumes, and is no undo step", "[ai][video][commands]") {
    Fixture f("cmd-process");
    const std::size_t undoBefore = f.harness->session.undoCount();
    auto result = f.harness->ok("effect.processAi");
    const int frames = result.data["clips"][0]["frames"].get<int>();
    CHECK(result.data["processed"] == frames);
    CHECK(result.data["skipped"] == 0);
    CHECK(result.data["clips"][0]["chain"] == "Restore");
    CHECK(static_cast<int>(f.backend->calls.size()) == frames);
    CHECK(f.harness->session.undoCount() == undoBefore); // derived data: not an edit
    auto status = f.harness->ok("effect.aiStatus");
    CHECK(status.data["clips"][0]["state"] == "ready");
    CHECK(status.data["pending"] == 0);
    // Again: nothing to do.
    auto again = f.harness->ok("effect.processAi");
    CHECK(again.data["processed"] == 0);
    CHECK(again.data["message"].get<std::string>().find("already processed") != std::string::npos);
    CHECK(static_cast<int>(f.backend->calls.size()) == frames);
}

TEST_CASE("effect.processAi refuses without the models, on a dry run only lists, and a cancel keeps what was made", "[ai][video][commands]") {
    {
        Fixture f("cmd-process-nomodel", true, false);
        auto result = f.harness->run(Json{{"command", "effect.processAi"}, {"params", Json::object()}});
        REQUIRE_FALSE(result.ok());
        CHECK(result.error->code == command::ErrorCode::InvalidRequest);
        CHECK(result.error->message.find("NAFNet") != std::string::npos);
        CHECK(f.backend->calls.empty());
    }
    {
        Fixture f("cmd-process-dry");
        auto dry = f.harness->run(Json{{"command", "effect.processAi"}, {"params", Json::object()}, {"dryRun", true}});
        REQUIRE(dry.ok());
        CHECK(dry.data["dryRun"] == true);
        CHECK(dry.data["clips"].size() == 1);
        CHECK(f.backend->calls.empty());
    }
    {
        Fixture f("cmd-process-clip");
        const Json params{{"clip", f.harness->clip("V1@0").value}};
        auto result = f.harness->ok("effect.processAi", params);
        CHECK(result.data["clips"].size() == 1);
        CHECK(f.harness->errorOf("effect.processAi", Json{{"clip", f.harness->clip("A1@0").value}}) == "NotFound");
    }
}
