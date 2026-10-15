// A.8 voice-over recording in the model: a track's record arm (Track::recordArmed, the R button) and recorded media
// (MediaItem::recording: where a take was recorded, its number, the input device and the latency compensation). Both
// are written to project files only when set, so other projects stay byte-identical.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaa|
    A1 tgt |Aaaaaaaa|
    A2 lock |........|
)";

RecordingProvenance recording(const Timeline& tl) {
    RecordingProvenance r;
    r.sequence = tl.seq;
    r.track = tl.track("A2");
    r.range = TimeRange{f(2), f(4)};
    r.take = 2;
    r.device = "Microphone (USB Audio Device)"; // the OS's name
    r.latencyCompensation = Ticks{1'270'080'000};        // 5 ms
    r.createdAt = "2026-10-12T09:30:00Z";
    return r;
}

ProjectState withRecording(const ProjectState& state, MediaId id, std::optional<RecordingProvenance> r) {
    ProjectState out = state;
    for (MediaPtr& m : out.media) {
        if (m->id == id) {
            MediaItem copy = *m;
            copy.recording = std::move(r);
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
    return out;
}

} // namespace

TEST_CASE("Record arm: audio tracks only, allowed on locked tracks", "[model][voiceover]") {
    Timeline tl(kPicture);
    edit::TrackStateChange arm;
    arm.recordArmed = true;
    const edit::EditResult armed = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A2"), arm)); // A2 is locked
    CHECK(armed.state.findSequence(tl.seq)->findTrack(tl.track("A2"))->recordArmed);
    // A header change: the ChangeSet has it (the command engine commits only what the ChangeSet sees).
    CHECK_FALSE(diff(tl.state, armed.state).empty());
    const auto video = edit::setTrackState(tl.state, tl.seq, tl.track("V1"), arm);
    REQUIRE_FALSE(video);
    CHECK(video.error().message.find("video") != std::string::npos);
    // A video track armed (a hand-edited file) is invalid state.
    Sequence seq = *tl.state.findSequence(tl.seq);
    Track v1 = *seq.videoTracks[0];
    v1.recordArmed = true;
    seq.videoTracks[0] = std::make_shared<const Track>(std::move(v1));
    CHECK_FALSE(validate(tl.state.withSequence(std::make_shared<const Sequence>(std::move(seq))), Validation::Structure));
}

TEST_CASE("Recorded media: validated, bounded device name, weak sequence and track", "[model][voiceover]") {
    Timeline tl(kPicture);
    const MediaId a = tl.media('A');
    CHECK(validate(withRecording(tl.state, a, recording(tl)), Validation::Structure));
    // A removed sequence or track leaves the takes valid (the provenance is a weak reference).
    RecordingProvenance dangling = recording(tl);
    dangling.sequence = SequenceId{9999};
    dangling.track = TrackId{8888};
    CHECK(validate(withRecording(tl.state, a, dangling), Validation::Structure));
    const auto bad = [&](auto change) {
        RecordingProvenance r = recording(tl);
        change(r);
        return !validate(withRecording(tl.state, a, r), Validation::Structure);
    };
    CHECK(bad([](RecordingProvenance& r) { r.range.duration = Ticks{}; }));
    CHECK(bad([](RecordingProvenance& r) { r.range.start = Ticks{-1}; }));
    CHECK(bad([](RecordingProvenance& r) { r.take = 0; }));
    CHECK(bad([](RecordingProvenance& r) { r.device = std::string(RecordingProvenance::kMaxDeviceBytes + 1, 'm'); }));
    CHECK(bad([](RecordingProvenance& r) { r.device = "bad \xFF utf-8"; }));
    CHECK(bad([](RecordingProvenance& r) { r.createdAt = "yesterday"; }));
    CHECK_FALSE(bad([](RecordingProvenance& r) { r.device = std::string(RecordingProvenance::kMaxDeviceBytes, 'm'); }));
    CHECK_FALSE(bad([](RecordingProvenance& r) { r.device.clear(); }));
    CHECK_FALSE(bad([](RecordingProvenance& r) { r.device = "Micr\xC3\xB3" "fono"; })); // UTF-8 is fine
    // A change of the provenance is a media change.
    const ChangeSet changes = diff(tl.state, withRecording(tl.state, a, recording(tl)));
    CHECK_FALSE(changes.empty());
}

TEST_CASE("Record arm and recorded media round-trip through project files, absent until set", "[model][voiceover][io]") {
    Timeline tl(kPicture);
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    CHECK(plain->find("recordArmed") == std::string::npos);
    CHECK(plain->find("\"recording\"") == std::string::npos);

    edit::TrackStateChange arm;
    arm.recordArmed = true;
    ProjectState s = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A2"), arm)).state;
    s = withRecording(s, tl.media('A'), recording(tl));
    const auto saved = io::toJson(s);
    REQUIRE(saved);
    CHECK(saved->find("\"recordArmed\": true") != std::string::npos);
    const auto back = io::fromJson(*saved);
    REQUIRE(back);
    CHECK(back->findSequence(tl.seq)->findTrack(tl.track("A2"))->recordArmed);
    CHECK_FALSE(back->findSequence(tl.seq)->findTrack(tl.track("A1"))->recordArmed);
    const MediaItem& item = *back->findMedia(tl.media('A'));
    REQUIRE(item.recording);
    CHECK(*item.recording == recording(tl));
    CHECK(*io::toJson(*back) == *saved);

    // A file naming a device past the bound is refused (the name is the OS's text, never trusted as-is).
    std::string tooLong = *saved;
    const std::string device = "\"device\": \"" + recording(tl).device + "\"";
    const auto at = tooLong.find(device);
    REQUIRE(at != std::string::npos);
    tooLong.replace(at, device.size(), "\"device\": \"" + std::string(300, 'x') + "\"");
    CHECK_FALSE(io::fromJson(tooLong));
}
