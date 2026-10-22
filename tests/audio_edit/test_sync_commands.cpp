// A.5 audio.syncClips through the CommandEngine, with an AudioSyncService that runs the real sync on synthetic
// recordings of the project's media: offsets and confidence, the dry run, Premiere's Synchronize (the recorder's clip
// moves into sync with the camera clip and links to it; one undo step), failures, multicam's angleOffsets, and
// multicam.create's audio sync through MulticamAudioSync (A.5 part 3), stopped at its checkpoints.

#include "CommandHarness.h"
#include "NestedTestUtil.h"
#include "SyncTestSignals.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioSync.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <map>
#include <memory>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr std::int64_t kTicksPerSample = 5'292'000; // 48 kHz
const Ticks k32{std::llround(3.2 * static_cast<double>(kTicksPerSecond))}; // the recorder starts 3.2 s after the camera

/// Single-file angles only: each media item is a synthetic recording.
class SignalSync final : public AudioSyncService {
public:
    std::map<MediaId, RecordingSignal*> recordings;
    int calls = 0;

    std::expected<Result, Error> sync(const ProjectState&, std::span<const Angle> angles, std::size_t reference,
                                      const SyncOptions& options, std::stop_token stop,
                                      const std::function<void(double)>& progress) override {
        ++calls;
        std::vector<SyncSignal*> signals;
        Result r;
        for (const Angle& a : angles) {
            signals.push_back(recordings.at(a.media.front()));
            r.order.push_back(a.media);
        }
        auto report = syncSignals(signals, reference, options, stop, progress);
        if (!report) {
            return std::unexpected(report.error());
        }
        r.report = *report;
        for (const SyncMatch& m : r.report.matches) {
            r.starts.push_back({samplesToTicks(m.offset, Rate{48000, 1})});
        }
        return r;
    }
};

std::string picture() {
    // V1/A1: the camera (media A, 20 s), A2: the recorder (media R, 16 s), both at 0; A3: another event (media U).
    return "V1 tgt |A" + std::string(499, 'a') + "|\nA1 tgt |A" + std::string(499, 'a') + "|\nA2     |R" + std::string(399, 'r') +
           "|\nA3     |U" + std::string(399, 'u') + "|";
}

/// The timeline with the recorder's media audio-only (the fixture gives every media a picture).
Timeline timeline() {
    Timeline tl(picture());
    tl.link("V1@0 A1@0");
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media('R')) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->info.video.clear();
            m = copy;
        }
    }
    return tl;
}

struct Fixture {
    Fixture()
        : event(60.0, 5), other(60.0, 6), camera(event, {0.0, 30.0, 0.4, 0.01, true}), recorder(event, {3.2, 30.0, 1.0, 0.002}),
          unrelated(other, {0.0, 30.0}), sync(std::make_shared<SignalSync>()),
          h(timeline(), policy(), ai::edit::editingCommands()) {
        sync->recordings[h.tl.media('A')] = &camera;
        sync->recordings[h.tl.media('R')] = &recorder;
        sync->recordings[h.tl.media('U')] = &unrelated;
    }
    command::EnginePolicy policy() {
        auto services = std::make_shared<AudioServices>(nullptr, nullptr);
        services->sync = sync;
        return command::EnginePolicy{command::CapabilitySet::all(), 64, services};
    }
    const Clip& clip(ClipId id) const { return h.sequence(h.state()).findClip(id)->clip(); }

    SyntheticEvent event;
    SyntheticEvent other;
    RecordingSignal camera;
    RecordingSignal recorder;
    RecordingSignal unrelated;
    std::shared_ptr<SignalSync> sync;
    CommandHarness h;
};

} // namespace

TEST_CASE("audio.syncClips reports offsets and confidence; the dry run and Synchronize move and link the recorder",
          "[audio][edit][sync][commands]") {
    Fixture f;
    const ClipId cameraVideo = f.h.clip("V1@0");
    const ClipId cameraAudio = f.h.clip("A1@0");
    const ClipId recorderClip = f.h.clip("A2@0");
    const Json params{{"clips", Json::array({cameraVideo.value, recorderClip.value})}};

    // Offsets only: nothing changes; the reference is the clip with a picture.
    auto r = f.h.ok("audio.syncClips", params);
    CHECK(r.data["reference"] == 0);
    REQUIRE(r.data["recordings"].size() == 2);
    const Json& rec = r.data["recordings"][1];
    CHECK(rec["synced"] == true);
    CHECK(std::llabs(rec["offsetTicks"].get<std::int64_t>() - k32.value) <= kTicksPerSample);
    CHECK(rec["confidence"].get<double>() >= 0.5);
    CHECK(f.clip(recorderClip).start() == Ticks{});

    // Synchronize, as a dry run first: the preview has the recorder 3.2 s later, the session does not.
    Json apply = params;
    apply["apply"] = true;
    const std::uint64_t revision = f.h.engine.revision();
    const auto preview = f.h.run(Json{{"command", "audio.syncClips"}, {"params", apply}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(f.h.engine.revision() == revision);
    REQUIRE(preview.preview);
    const Ticks moved = preview.preview->findSequence(f.h.tl.seq)->findClip(recorderClip)->clip().start();
    CHECK(std::llabs(moved.value - k32.value) <= kTicksPerSample);
    CHECK(f.clip(recorderClip).start() == Ticks{});

    // For real: one undo step, the recorder in sync and linked with the camera's picture and scratch audio.
    const std::size_t undo = f.h.session.undoCount();
    r = f.h.ok("audio.syncClips", apply);
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(std::llabs(f.clip(recorderClip).start().value - k32.value) <= kTicksPerSample);
    CHECK(f.clip(cameraVideo).start() == Ticks{});
    REQUIRE(f.clip(recorderClip).linkGroup.isValid());
    CHECK(f.clip(recorderClip).linkGroup == f.clip(cameraVideo).linkGroup);
    CHECK(f.clip(cameraAudio).linkGroup == f.clip(cameraVideo).linkGroup);
    CHECK(r.data["linked"] == Json::array({recorderClip.value}));
    f.h.session.undo();
    CHECK(f.clip(recorderClip).start() == Ticks{});
    CHECK_FALSE(f.clip(recorderClip).linkGroup.isValid());
}

TEST_CASE("audio.syncClips syncs a multicam clip and a nested clip by the recording they play (0.2)", "[audio][edit][sync][commands][multicam]") {
    Fixture f;
    const ClipId recorderClip = f.h.clip("A2@0");
    const SequenceId seq = f.h.tl.seq;

    SECTION("a two-angle multicam clip of the camera (and another event) playing the camera from its 2 s") {
        PlacedMulticam mc;
        REQUIRE(f.h.session.apply("Multicam", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
            mc = placeMulticam(s, seq, {f.h.tl.media('A'), f.h.tl.media('U')}, {Ticks{}, Ticks{}},
                               TimeRange::fromStartEnd(Ticks::fromSeconds(2), Ticks::fromSeconds(20)), Ticks{});
            return mc.state;
        }));
        const Json params{{"sequence", seq.value}, {"clips", Json::array({mc.video.value, recorderClip.value})}, {"apply", true}};
        const ProjectSnapshot before = f.h.state();
        const auto preview = f.h.run(Json{{"command", "audio.syncClips"}, {"params", params}, {"dryRun", true}});
        REQUIRE(preview.ok());
        CHECK(*f.h.state() == *before);
        const std::size_t undo = f.h.session.undoCount();
        const auto r = f.h.ok("audio.syncClips", params);
        CHECK(f.h.session.undoCount() == undo + 1);
        CHECK(r.data["reference"] == 0); // the multicam clip has the picture
        CHECK(r.data["recordings"][0]["media"][0]["media"] == f.h.tl.media('A').value);
        // The recorder starts 3.2 s into the event; the clip shows the camera from its 2 s: 1.2 s in.
        CHECK(std::llabs(f.clip(recorderClip).start().value - (k32 - Ticks::fromSeconds(2)).value) <= kTicksPerSample);
        const Clip& camera = f.clip(mc.video);
        CHECK(camera.start() == Ticks{});
        CHECK(camera.sequence == mc.multicam); // still the multicam clip, its angle kept
        CHECK(camera.angles == AngleSet::of(0));
        CHECK(f.clip(recorderClip).linkGroup == camera.linkGroup);
        f.h.session.undo();
        CHECK(*f.h.state() == *before);
    }

    SECTION("a nested clip of the camera") {
        edit::NestResult nest;
        REQUIRE(f.h.session.apply("Nest", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
            const std::vector<ClipId> camera{f.h.clip("V1@0"), f.h.clip("A1@0")};
            auto nested = edit::nestClips(s, seq, camera, "Camera");
            if (!nested) {
                return std::unexpected(nested.error());
            }
            nest = *nested;
            return nest.edit.state;
        }));
        REQUIRE(f.clip(nest.clips.front()).isNested());
        const auto r = f.h.ok("audio.syncClips", {{"sequence", seq.value}, {"clips", Json::array({nest.clips.front().value, recorderClip.value})}, {"apply", true}});
        CHECK(r.data["recordings"][1]["synced"] == true);
        CHECK(std::llabs(f.clip(recorderClip).start().value - k32.value) <= kTicksPerSample);
        CHECK(f.clip(nest.clips.front()).sequence == nest.nested);
    }
}

TEST_CASE("audio.syncClips: the recorder as the reference moves the camera, earlier audio shifts everything, failures",
          "[audio][edit][sync][commands]") {
    Fixture f;
    const ClipId cameraVideo = f.h.clip("V1@0");
    const ClipId recorderClip = f.h.clip("A2@0");
    // The recorder as the reference: the camera (and its linked audio) would move 3.2 s earlier, before 0, so both
    // move together instead: the recorder 3.2 s later, the camera stays.
    auto r = f.h.ok("audio.syncClips", {{"clips", Json::array({cameraVideo.value, recorderClip.value})},
                                        {"reference", recorderClip.value},
                                        {"apply", true},
                                        {"link", false}});
    CHECK(std::llabs(f.clip(recorderClip).start().value - k32.value) <= kTicksPerSample);
    CHECK(std::llabs(f.clip(cameraVideo).start().value) <= kTicksPerSample);
    CHECK_FALSE(f.clip(recorderClip).linkGroup.isValid());

    // Unrelated audio: reported, not moved, a warning.
    const ClipId stranger = f.h.clip("A3@0");
    r = f.h.ok("audio.syncClips", {{"clips", Json::array({cameraVideo.value, stranger.value})}, {"apply", true}});
    CHECK(r.data["recordings"][1]["synced"] == false);
    CHECK_FALSE(r.data["recordings"][1]["problem"].get<std::string>().empty());
    CHECK_FALSE(r.warnings.empty());
    CHECK(f.clip(stranger).start() == Ticks{});

    CHECK(f.h.errorOf("audio.syncClips", {{"clips", Json::array({cameraVideo.value})}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.syncClips", Json::object()) == "InvalidParams");
    CHECK(f.h.errorOf("audio.syncClips", {{"clips", Json::array({cameraVideo.value, recorderClip.value})}, {"reference", 999}}) ==
          "InvalidParams");
    CHECK(f.h.errorOf("audio.syncClips", {{"media", Json::array({f.h.tl.media('A').value, f.h.tl.media('R').value})}, {"apply", true}}) ==
          "InvalidParams");
    // Media mode reports offsets.
    r = f.h.ok("audio.syncClips", {{"media", Json::array({f.h.tl.media('A').value, f.h.tl.media('R').value})}});
    CHECK(r.data["recordings"][1]["synced"] == true);

    // Multicam's seam: angle offsets in ticks, failure with the angle's name.
    const std::vector<MediaId> angles{f.h.tl.media('A'), f.h.tl.media('R')};
    auto offsets = angleOffsets(*f.sync, *f.h.state(), angles);
    REQUIRE(offsets);
    CHECK((*offsets)[0] == Ticks{});
    CHECK(std::llabs((*offsets)[1].value - k32.value) <= kTicksPerSample);
    const std::vector<MediaId> bad{f.h.tl.media('A'), f.h.tl.media('U')};
    auto failed = angleOffsets(*f.sync, *f.h.state(), bad);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().message.find("angle 2") != std::string::npos);

    // No sync service: refused.
    CommandHarness bare(Timeline(picture()), command::EnginePolicy{command::CapabilitySet::all(), 64,
                                                                   std::make_shared<AudioServices>(nullptr, nullptr)},
                        ai::edit::editingCommands());
    CHECK(bare.errorOf("audio.syncClips", {{"clips", Json::array({bare.clip("V1@0").value, bare.clip("A2@0").value})}}) ==
          "InvalidRequest");
}

TEST_CASE("multicam.create syncs angles by audio through MulticamAudioSync; a stop at its checkpoints cancels it",
          "[audio][edit][sync][commands][multicam]") {
    Fixture f;
    // The hosts' bundle: the audio services and multicam's audio sync on the same analysis.
    auto audio = std::make_shared<AudioServices>(nullptr, nullptr);
    audio->sync = f.sync;
    auto bundle = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        audio, std::make_shared<command::MulticamServices>(std::make_shared<MulticamAudioSync>(f.sync))});
    CommandHarness h(timeline(), command::EnginePolicy{command::CapabilitySet::all(), 64, bundle}, ai::edit::editingCommands());
    const Json params{{"media", Json::array({h.tl.media('A').value, h.tl.media('R').value})}, {"sync", "audio"}};
    const auto r = h.ok("multicam.create", params);
    const Sequence* source = h.state()->findSequence(SequenceId{r.data["multicam"].get<std::uint64_t>()});
    REQUIRE(source != nullptr);
    REQUIRE(source->multicam);
    REQUIRE(source->multicam->angles.size() == 2);
    CHECK(source->multicam->sync == MulticamSync::Audio);
    // The recorder started 3.2 s after the camera: its angle lies 3.2 s later in the source, to the sample.
    CHECK(source->multicam->angles[0].offset == Ticks{});
    CHECK(std::llabs(source->multicam->angles[1].offset.value - k32.value) <= kTicksPerSample);

    // The analysis reports through the request's checkpoints; a stop there cancels it and nothing is created.
    struct StopAtSync final : command::ExecutionControl {
        int seen = 0;
        std::optional<command::CommandError> checkpoint(std::string_view message) override {
            if (message.find("syncing the angles by audio") == std::string_view::npos) {
                return std::nullopt;
            }
            ++seen;
            return command::CommandError{command::ErrorCode::Cancelled, "stopped", nullptr};
        }
    } stop;
    const std::size_t sequences = h.state()->sequences.size();
    const auto cancelled = h.engine.executeJson(Json{{"command", "multicam.create"}, {"params", params}}, &stop);
    REQUIRE_FALSE(cancelled.ok());
    CHECK(std::string(command::toString(cancelled.error->code)) == "Cancelled");
    CHECK(stop.seen >= 1);
    CHECK(h.state()->sequences.size() == sequences);
    // An analysis that cannot sync the angles fails the command with its reason.
    CHECK(h.errorOf("multicam.create", {{"media", Json::array({h.tl.media('A').value, h.tl.media('U').value})}, {"sync", "audio"}}) !=
          "");
}
