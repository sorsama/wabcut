// A.8 voice-over takes: what a recording pass leaves in the project (a media item in the Voice-over bin with its
// recording provenance, a clip on the armed track), repeated passes over one range stacking as takes (the newest one
// heard), the takes switcher, and the commands (track.setRecordArm, record.takes, record.useTake).

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/VoiceOver.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    A2     |....................|
)";

RecordedTake take(const std::string& name, Ticks start, Ticks duration) {
    RecordedTake t;
    t.path = "C:/Projects/Film/Voice-over/" + name + ".wav";
    t.fingerprint.size = 44 + duration.value / 5'292'000 * 3;
    t.fingerprint.sampleHash = "0123456789abcdef";
    t.start = start;
    t.duration = duration;
    t.device = "Microphone (USB Audio Device)";
    t.latencyCompensation = Ticks{1'270'080'000};
    t.createdAt = "2026-10-12T09:30:00Z";
    return t;
}

const MediaItem& mediaOf(const ProjectState& state, SequenceId seq, ClipId clip) {
    return *state.findMedia(state.findSequence(seq)->findClip(clip)->clip().media);
}

} // namespace

TEST_CASE("A recorded take becomes a media item in the Voice-over bin and a clip on the track", "[audio][edit][voiceover]") {
    Timeline tl(kPicture);
    const TrackId a2 = tl.track("A2");
    const edit::EditResult r = ok(placeVoiceOverTake(tl.state, tl.seq, a2, take("take1", f(4), f(8))));
    REQUIRE(r.placed.size() == 1);
    REQUIRE(r.createdMedia.isValid());
    const MediaItem& item = *r.state.findMedia(r.createdMedia);
    CHECK(item.name == "Voice-over A2 Take 1");
    CHECK(item.locator.path == "C:/Projects/Film/Voice-over/take1.wav");
    REQUIRE(item.info.audio.size() == 1);
    CHECK(item.info.audio[0].codec == "pcm_s24le");
    CHECK(item.info.audio[0].channels == 1);
    CHECK(item.info.duration == f(8));
    REQUIRE(item.recording);
    CHECK(item.recording->sequence == tl.seq);
    CHECK(item.recording->track == a2);
    CHECK(item.recording->range == TimeRange{f(4), f(8)});
    CHECK(item.recording->take == 1);
    CHECK(item.recording->device == "Microphone (USB Audio Device)");
    CHECK(item.recording->latencyCompensation == Ticks{1'270'080'000});
    const Bin* bin = nullptr;
    for (const Bin& b : r.state.bins) {
        if (b.id == item.bin) {
            bin = &b;
        }
    }
    REQUIRE(bin);
    CHECK(bin->name == "Voice-over");
    const Clip& clip = r.state.findSequence(tl.seq)->findClip(r.placed[0])->clip();
    CHECK(clip.start() == f(4));
    CHECK(clip.duration() == f(8));
    CHECK(clip.stream == StreamRef{StreamKind::Audio, 0});
    CHECK(r.state.findSequence(tl.seq)->findClip(r.placed[0])->track->id == a2);

    // A take placed to the sample (not on a frame): 2 s and 37 samples at 48 kHz.
    const Ticks sampleAccurate = Ticks::fromSeconds(2) + Ticks{37 * 5'292'000};
    const edit::EditResult exact = ok(placeVoiceOverTake(tl.state, tl.seq, a2, take("exact", sampleAccurate, Ticks::fromSeconds(1))));
    CHECK(exact.state.findSequence(tl.seq)->findClip(exact.placed[0])->clip().start() == sampleAccurate);

    // Refused: a video track, an empty take, a locked track.
    CHECK_FALSE(placeVoiceOverTake(tl.state, tl.seq, tl.track("V1"), take("v", f(0), f(4))));
    CHECK_FALSE(placeVoiceOverTake(tl.state, tl.seq, a2, take("empty", f(0), Ticks{})));
    Timeline locked(R"(
        V1 tgt |Aaaaaaaa|
        A1 lock |........|
    )");
    CHECK_FALSE(placeVoiceOverTake(locked.state, locked.seq, locked.track("A1"), take("l", f(0), f(4))));
}

TEST_CASE("Passes over the same range stack as takes: the newest is heard, the others stay in the bin", "[audio][edit][voiceover]") {
    Timeline tl(kPicture);
    const TrackId a2 = tl.track("A2");
    ProjectState s = ok(placeVoiceOverTake(tl.state, tl.seq, a2, take("t1", f(4), f(8)))).state;
    edit::EditResult second = ok(placeVoiceOverTake(s, tl.seq, a2, take("t2", f(4), f(8))));
    s = second.state;
    // Take 2 replaced take 1 on the track; one Voice-over bin holds both.
    const Track& track = *s.findSequence(tl.seq)->findTrack(a2);
    REQUIRE(track.clips.size() == 1);
    CHECK(mediaOf(s, tl.seq, track.clips[0].id).name == "Voice-over A2 Take 2");
    int voBins = 0;
    for (const Bin& b : s.bins) {
        voBins += b.name == "Voice-over" ? 1 : 0;
    }
    CHECK(voBins == 1);
    // A third pass over part of the range is take 3 of the same slot; a pass elsewhere starts a slot of its own.
    const edit::EditResult third = ok(placeVoiceOverTake(s, tl.seq, a2, take("t3", f(6), f(4))));
    s = third.state;
    const edit::EditResult elsewhere = ok(placeVoiceOverTake(s, tl.seq, a2, take("e1", f(14), f(4))));
    s = elsewhere.state;
    CHECK(s.findMedia(elsewhere.createdMedia)->recording->take == 1);
    CHECK(s.findMedia(elsewhere.createdMedia)->name == "Voice-over A2 Take 1");

    const std::vector<VoiceOverTake> takes = voiceOverTakesOf(s, tl.seq, third.placed[0]);
    REQUIRE(takes.size() == 3);
    CHECK(takes[0].take == 3);
    CHECK(takes[0].active);
    CHECK(takes[1].take == 2);
    CHECK_FALSE(takes[1].active);
    CHECK(takes[2].take == 1);
    CHECK(takes[2].range == TimeRange{f(4), f(8)});
    CHECK(takes[2].device == "Microphone (USB Audio Device)");
    CHECK(voiceOverTakesOf(s, tl.seq, elsewhere.placed[0]).size() == 1);
    CHECK(voiceOverTakesOf(s, tl.seq, tl.clip("A1@0")).empty()); // not a recorded take

    // Use take 1: its media over its recorded range, with the clip's level and effects.
    {
        Sequence seq = *s.findSequence(tl.seq);
        for (TrackPtr& t : seq.audioTracks) {
            if (t->id != a2) {
                continue;
            }
            Track copy = *t;
            for (std::size_t i = 0; i < copy.clips.size(); ++i) {
                if (copy.clips[i].id == third.placed[0]) {
                    ClipProperties props = copy.clips[i].properties();
                    props.set(ParamId::VolumeDb, -6.0);
                    copy.clips.mutableAt(i).setProperties(props);
                }
            }
            t = std::make_shared<const Track>(std::move(copy));
        }
        s = s.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }
    const edit::EditResult used = ok(useVoiceOverTake(s, tl.seq, third.placed[0], 1));
    REQUIRE(used.placed.size() == 1);
    const Clip& now = used.state.findSequence(tl.seq)->findClip(used.placed[0])->clip();
    CHECK(now.start() == f(4));
    CHECK(now.duration() == f(8));
    CHECK(used.state.findMedia(now.media)->recording->take == 1);
    CHECK(now.name() == "Voice-over A2 Take 1");
    CHECK(now.properties()[ParamId::VolumeDb].constant() == -6.0);
    const std::vector<VoiceOverTake> after = voiceOverTakesOf(used.state, tl.seq, used.placed[0]);
    REQUIRE(after.size() == 3);
    CHECK(after[2].active); // take 1 is the one heard now
    CHECK_FALSE(after[0].active);
    // Errors: no such take, not a recorded clip.
    CHECK(useVoiceOverTake(s, tl.seq, third.placed[0], 7).error().code == ErrorCode::InvalidArgument);
    CHECK(useVoiceOverTake(s, tl.seq, tl.clip("A1@0"), 1).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("The device name is the OS's text: kept valid UTF-8 and bounded", "[audio][edit][voiceover]") {
    Timeline tl(kPicture);
    RecordedTake t = take("odd", f(0), f(4));
    t.device = std::string(1000, 'x') + "\xFF";
    t.device.insert(3, "\xC3"); // a cut-off sequence
    const edit::EditResult r = ok(placeVoiceOverTake(tl.state, tl.seq, tl.track("A2"), t));
    const std::string& kept = r.state.findMedia(r.createdMedia)->recording->device;
    CHECK(kept.size() == RecordingProvenance::kMaxDeviceBytes);
    CHECK(kept.find('\xC3') == std::string::npos);
    CHECK(validate(r.state, Validation::Strict));
}

TEST_CASE("track.setRecordArm, record.takes and record.useTake through the engine", "[audio][edit][voiceover][commands]") {
    CommandHarness h(Timeline(kPicture), command::EnginePolicy{command::CapabilitySet::all(), 64, nullptr}, ai::edit::editingCommands());
    const TrackId a2 = h.tl.track("A2");
    const std::size_t undo = h.session.undoCount();
    auto armed = h.ok("track.setRecordArm", {{"track", "A2"}});
    CHECK(armed.data["track"] == "A2");
    CHECK(armed.data["armed"] == true);
    CHECK(h.sequence(h.state()).findTrack(a2)->recordArmed);
    CHECK(h.session.undoCount() == undo + 1);
    h.ok("track.setRecordArm", {{"track", "A2"}, {"armed", false}});
    CHECK_FALSE(h.sequence(h.state()).findTrack(a2)->recordArmed);
    CHECK_FALSE(h.errorOf("track.setRecordArm", {{"track", "V1"}}).empty());

    // Two passes (the app's record.stop commits each like this).
    ProjectState s = *h.state();
    const edit::EditResult first = ok(placeVoiceOverTake(s, h.tl.seq, a2, take("c1", f(4), f(8))));
    const edit::EditResult second = ok(placeVoiceOverTake(first.state, h.tl.seq, a2, take("c2", f(4), f(8))));
    REQUIRE(h.session.commit("Voice-over Record", second.state));
    const ClipId heard = second.placed[0];

    auto listed = h.ok("record.takes", {{"clip", heard.value}});
    REQUIRE(listed.data["takes"].size() == 2);
    CHECK(listed.data["takes"][0]["take"] == 2);
    CHECK(listed.data["takes"][0]["active"] == true);
    CHECK(listed.data["takes"][1]["take"] == 1);
    CHECK(listed.data["takes"][1]["name"] == "Voice-over A2 Take 1");
    CHECK(listed.data["takes"][1]["device"] == "Microphone (USB Audio Device)");
    CHECK(listed.data["takes"][1]["start"].is_string());
    const std::size_t before = h.session.undoCount();
    auto used = h.ok("record.useTake", {{"clip", heard.value}, {"take", 1}});
    CHECK(h.session.undoCount() == before + 1);
    const ClipId now = ClipId{used.data["clip"].get<std::uint64_t>()};
    CHECK(mediaOf(*h.state(), h.tl.seq, now).recording->take == 1);
    CHECK(h.errorOf("record.useTake", {{"clip", now.value}, {"take", 5}}) == "InvalidParams");
    CHECK(h.errorOf("record.takes", {{"clip", h.clip("A1@0").value}}) == "InvalidParams");
    CHECK(h.errorOf("record.takes", {{"clip", 999999}}) == "NotFound");
}
