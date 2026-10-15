// transcript.transcribe through the CommandEngine with a scripted MediaTranscriber (every build): what it transcribes
// (named media, clips, else every audio clip of the sequence), per-item results, the check mode, provider errors,
// partial failures and cancel. The media-backed transcriber (ai::TranscribeService) is in test_media_services.cpp.

#include "CommandHarness.h"
#include "EditTestUtil.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaaaaaaaBbbbbbbbbbAaaaaaaaaa|
    A1 tgt |AaaaaaaaaaBbbbbbbbbbAaaaaaaaaa|
)";

class ScriptedTranscriber final : public MediaTranscriber {
public:
    std::vector<std::pair<std::uint64_t, int>> calls;
    std::vector<Options> options;
    std::set<std::uint64_t> failing;     ///< media ids whose transcription fails
    std::set<std::uint64_t> transcribed; ///< media ids with a transcript

    std::expected<Result, Error> transcribe(const MediaItem& media, int audioStream, const Options& o,
                                            const std::function<bool(double)>& progress) override {
        calls.emplace_back(media.id.value, audioStream);
        options.push_back(o);
        const bool had = transcribed.contains(media.id.value) && !o.force;
        if (!had) {
            if (auto p = resolve(o.provider); !p) {
                return std::unexpected(p.error()); // like TranscribeService: only the cache needs no provider
            }
        }
        if (progress && (!progress(0.25) || !progress(0.75))) {
            return std::unexpected(Error{wabcut::ErrorCode::Cancelled, "stopped"});
        }
        if (failing.contains(media.id.value)) {
            return std::unexpected(Error{wabcut::ErrorCode::Unsupported, "stt-model: ConsentRequired: ask the user first"});
        }
        transcribed.insert(media.id.value);
        return Result{o.provider == "auto" ? "local" : o.provider, had, 12, "th"};
    }
    bool hasTranscript(const MediaItem& media, int) const override { return transcribed.contains(media.id.value); }
    std::vector<Provider> providers() const override {
        return {{"local", "On this computer", true, ""}, {"cloud:stt", "STT", false, "no key for STT"}};
    }
    std::expected<std::string, Error> resolve(std::string_view provider) const override {
        if (provider == "auto" || provider == "local") {
            return std::string("local");
        }
        if (provider == "cloud:stt") {
            return std::string(provider);
        }
        return std::unexpected(Error{wabcut::ErrorCode::NotFound, "unknown transcription provider \"" + std::string(provider) + "\""});
    }
};

/// Stops the request at the n-th checkpoint.
class StopAt final : public command::ExecutionControl {
public:
    explicit StopAt(int n) : m_left(n) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        messages.emplace_back(message);
        if (--m_left == 0) {
            return command::CommandError{command::ErrorCode::Cancelled, "stopped by the caller", nullptr};
        }
        return std::nullopt;
    }
    std::vector<std::string> messages;

private:
    int m_left;
};

struct Fixture {
    Fixture()
        : transcriber(std::make_shared<ScriptedTranscriber>()),
          h(Timeline(kPicture),
            command::EnginePolicy{command::CapabilitySet::all(), 1024, std::make_shared<TranscribeServices>(transcriber)},
            editingCommands()) {}
    std::shared_ptr<ScriptedTranscriber> transcriber;
    CommandHarness h;
};

} // namespace

TEST_CASE("transcript.transcribe is a MediaAnalysis command that needs a transcriber", "[ai][edit][transcribe]") {
    const command::CommandSpec* spec = editingCommands().find("transcript.transcribe");
    REQUIRE(spec);
    CHECK(spec->capability == command::Capability::MediaAnalysis);
    CHECK_FALSE(spec->mutates());
    CHECK(command::validateAgainstSchema(spec->params, spec->example).empty());
    CommandHarness none(Timeline(kPicture), {}, editingCommands());
    CHECK(none.errorOf("transcript.transcribe") == "InvalidRequest");
}

TEST_CASE("transcript.transcribe: the sequence's audio once per media, then what is cached", "[ai][edit][transcribe]") {
    Fixture fx;
    const MediaId a = fx.h.tl.media('A');
    const MediaId b = fx.h.tl.media('B');
    const auto first = fx.h.ok("transcript.transcribe", Json{{"language", "th"}});
    CHECK(first.revision == 0); // nothing in the project changes
    CHECK(first.changes.empty());
    CHECK(first.data["provider"] == "local");
    CHECK(first.data["transcribed"] == 2); // A twice on the timeline, B once
    CHECK(first.data["cached"] == 0);
    CHECK(first.data["failed"] == 0);
    REQUIRE(first.data["items"].size() == 2);
    CHECK(first.data["items"][0]["media"] == a.value);
    CHECK(first.data["items"][0]["status"] == "transcribed");
    CHECK(first.data["items"][0]["words"] == 12);
    CHECK(first.data["items"][0]["language"] == "th");
    CHECK(first.data["items"][1]["media"] == b.value);
    REQUIRE(fx.transcriber->options.size() == 2);
    CHECK(fx.transcriber->options[0].language == "th");
    CHECK(fx.transcriber->options[0].provider == "auto");
    CHECK_FALSE(fx.transcriber->options[0].force);

    const auto again = fx.h.ok("transcript.transcribe", Json{{"media", Json::array({a.value})}});
    CHECK(again.data["cached"] == 1);
    CHECK(again.data["items"][0]["status"] == "cached");
    const auto forced = fx.h.ok("transcript.transcribe", Json{{"media", Json::array({a.value})}, {"force", true}});
    CHECK(forced.data["transcribed"] == 1);
    CHECK(fx.transcriber->options.back().force);
}

TEST_CASE("transcript.transcribe: clips, providers and check mode", "[ai][edit][transcribe]") {
    Fixture fx;
    const MediaId b = fx.h.tl.media('B');
    const auto clip = fx.h.ok("transcript.transcribe", Json{{"clips", Json::array({fx.h.clip("V1@10").value})}, {"provider", "cloud:stt"}});
    REQUIRE(fx.transcriber->calls.size() == 1);
    CHECK(fx.transcriber->calls[0] == std::pair{b.value, 0}); // a video clip: its media's first audio stream
    CHECK(clip.data["provider"] == "cloud:stt");
    CHECK(clip.data["items"][0]["provider"] == "cloud:stt");

    const auto check = fx.h.ok("transcript.transcribe", Json{{"check", true}});
    CHECK(fx.transcriber->calls.size() == 1); // nothing ran
    CHECK(check.data["provider"] == "local");
    REQUIRE(check.data["providers"].size() == 2);
    CHECK(check.data["providers"][1]["available"] == false);
    CHECK(check.data["providers"][1]["reason"] == "no key for STT");
    REQUIRE(check.data["items"].size() == 2);
    CHECK(check.data["items"][0]["hasTranscript"] == false);
    CHECK(check.data["items"][1]["hasTranscript"] == true);

    // An unknown provider: nothing can be made (B's transcript is only skipped without force).
    CHECK(fx.h.errorOf("transcript.transcribe", Json{{"provider", "whisper-cloud-9"}, {"force", true}}) == "NotFound");
    CHECK(fx.h.errorOf("transcript.transcribe", Json{{"media", Json::array({999})}}) == "NotFound");
    CHECK(fx.h.errorOf("transcript.transcribe", Json{{"force", "yes"}}) == "InvalidParams");
}

TEST_CASE("transcript.transcribe: partial and total failures, cancel", "[ai][edit][transcribe]") {
    Fixture fx;
    const MediaId a = fx.h.tl.media('A');
    const MediaId b = fx.h.tl.media('B');
    fx.transcriber->failing = {b.value};
    const auto partial = fx.h.ok("transcript.transcribe");
    CHECK(partial.data["transcribed"] == 1);
    CHECK(partial.data["failed"] == 1);
    CHECK(partial.data["items"][1]["status"] == "failed");
    CHECK(partial.data["items"][1]["error"].get<std::string>().find("ConsentRequired") != std::string::npos);
    REQUIRE(partial.warnings.size() == 1);

    // Nothing made: the command fails with the first error.
    const auto all = fx.h.run(Json{{"command", "transcript.transcribe"}, {"params", Json{{"media", Json::array({b.value})}}}});
    REQUIRE_FALSE(all.ok());
    CHECK(all.error->code == command::ErrorCode::EditRejected);
    CHECK(all.error->message.find("ConsentRequired") != std::string::npos);

    // The caller stops it during the first item (its 75% report): nothing more runs.
    fx.transcriber->failing.clear();
    fx.transcriber->calls.clear();
    StopAt stop(3); // "transcribing 'A'", 25%, 75%
    const auto stopped = fx.h.engine.executeJson(Json{{"command", "transcript.transcribe"}, {"params", Json{{"force", true}}}}, &stop);
    REQUIRE_FALSE(stopped.ok());
    CHECK(stopped.error->code == command::ErrorCode::Cancelled);
    CHECK(fx.transcriber->calls.size() == 1);
    CHECK(fx.transcriber->calls[0].first == a.value);
    bool sawProgress = false;
    for (const std::string& m : stop.messages) {
        sawProgress = sawProgress || m.find("(25%)") != std::string::npos;
    }
    CHECK(sawProgress);
}

TEST_CASE("transcript.transcribe passes the project's audio redactions of each media item", "[ai][edit][transcribe][redaction]") {
    // Review P1-3: the transcriber must know what to silence.
    Timeline tl(kPicture);
    Redaction r;
    r.media = tl.media('A');
    r.source = TimeRange::fromStartEnd(f(2), f(6));
    r.audio = RedactionAudio::Bleep;
    auto added = ::wabcut::edit::addRedaction(tl.state, r);
    REQUIRE(added);
    tl.state = std::move(added->state);
    auto transcriber = std::make_shared<ScriptedTranscriber>();
    CommandHarness h(std::move(tl),
                     command::EnginePolicy{command::CapabilitySet::all(), 1024, std::make_shared<TranscribeServices>(transcriber)},
                     editingCommands());
    h.ok("transcript.transcribe");
    REQUIRE(transcriber->options.size() == 2);
    REQUIRE(transcriber->options[0].redacted.size() == 1);
    CHECK(transcriber->options[0].redacted[0] == TimeRange::fromStartEnd(f(2), f(6)));
    CHECK(transcriber->options[1].redacted.empty()); // media B has none
}
