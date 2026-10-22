// P.2 (E5): Sequence Settings on an existing sequence (a new timebase conforms the edit to the new frame grid), Duplicate,
// New Sequence From Clip and the extended sequence presets (SequenceSettings.h, SequencePresets.h).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequencePresets.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/Timecode.h>

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace wabcut;
using namespace wabcut::test;

namespace {

bool onFrameGrid(Ticks t, Rate rate) { return framesToTicks(ticksToFrames(t, rate), rate) == t; }

/// Every clip edge of every track on the grid of `rate`.
void requireOnGrid(const Sequence& s, Rate rate) {
    s.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            INFO("clip " << c.id.value << " " << c.start().value << ".." << c.end().value);
            CHECK(onFrameGrid(c.start(), rate));
            CHECK(onFrameGrid(c.end(), rate));
        }
    });
}

const Clip* clipById(const ProjectState& state, SequenceId seq, ClipId id) {
    const auto loc = state.findSequence(seq)->findClip(id);
    return loc ? &loc->clip() : nullptr;
}

} // namespace

TEST_CASE("Sequence presets cover Premiere's rates and frames", "[model][sequenceSettings][presets]") {
    std::set<std::string_view> ids;
    for (const SequencePreset& p : sequencePresets()) {
        INFO(p.id);
        CHECK(ids.insert(p.id).second);
        CHECK(p.frameRate.isExact());
        CHECK((!p.dropFrame || supportsDropFrame(p.frameRate)));
    }
    for (const char* id : {"720p23.976", "1080p23.976", "1080p24", "1080p25", "1080p29.97", "1080p29.97df", "1080p30",
                           "1080p50", "1080p59.94", "1080p59.94df", "1080p60", "1080p120", "1080p144", "1080p240",
                           "2160p23.976", "2160p25", "2160p29.97df", "vertical23.976", "vertical25", "shorts", "shorts30",
                           "square1080p30", "square1080p60"}) {
        INFO(id);
        CHECK(findSequencePreset(id) != nullptr);
    }
    const SequencePreset* df = findSequencePreset("1080p29.97df");
    REQUIRE(df);
    CHECK(df->frameRate == Rate{30000, 1001});
    CHECK(sequenceSettingsFor(*df).dropFrame);
    const SequencePreset* square = findSequencePreset("square1080p30");
    REQUIRE(square);
    CHECK(square->width == 1080);
    CHECK(square->height == 1080);
}

TEST_CASE("A new timebase keeps the edit in real time on the new frame grid", "[model][sequenceSettings][conform]") {
    // 25 fps: A 0-7, B 8-18 (a gap before it), C 18-21 adjacent to B; a marker and marks at odd frames.
    Timeline tl(R"(
        V1 tgt |A5aaaaa.B4bbbbbbbbC3c|
        A1 tgt |A5aaaaa.B4bbbbbbbbC3c|
    )");
    tl.linkMatching();
    ProjectState state = tl.state;
    state = ok(edit::addMarker(state, tl.seq, Marker{f(3), f(2), "kill", "", 0})).state;
    state = ok(edit::markIn(state, tl.seq, f(9))).state;
    state = ok(edit::markOut(state, tl.seq, f(13))).state;
    const ClipId b = clipAt(state, tl.seq, "V1@8");
    const Clip before = *clipById(state, tl.seq, b);

    edit::SequenceSettingsChange change;
    change.frameRate = Rate{30, 1};
    const auto r = ok(edit::setSequenceSettings(state, tl.seq, change));
    const Sequence& s = *r.state.findSequence(tl.seq);
    CHECK(s.frameRate == Rate{30, 1});
    requireOnGrid(s, Rate{30, 1});
    // Same real time, to the nearest 30 fps frame: 7/25 s -> 8/30, 8/25 -> 10/30, 18/25 -> 22/30, 21/25 -> 25/30.
    const Track& v1 = *s.videoTracks[0];
    REQUIRE(v1.clips.size() == 3);
    const Rate r30{30, 1};
    CHECK(v1.clips[0].end() == framesToTicks(8, r30));
    CHECK(v1.clips[1].start() == framesToTicks(10, r30));
    CHECK(v1.clips[1].end() == v1.clips[2].start()); // B and C stay adjacent
    CHECK(v1.clips[2].start() == framesToTicks(22, r30));
    CHECK(v1.clips[2].end() == framesToTicks(25, r30));
    // Linked audio moved with its video.
    const Track& a1 = *s.audioTracks[0];
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(a1.clips[i].timelineRange == v1.clips[i].timelineRange);
    }
    // B shows the same source where both versions of it play.
    const Clip& after = *clipById(r.state, tl.seq, b);
    const Ticks t = framesToTicks(15, r30);
    CHECK(after.sourceTimeAt(t) == before.sourceTimeAt(t));
    // Markers and marks on the grid too.
    REQUIRE(s.markers.size() == 1);
    CHECK(onFrameGrid(s.markers[0].start, r30));
    CHECK(onFrameGrid(s.markers[0].end(), r30));
    REQUIRE(s.markIn);
    REQUIRE(s.markOut);
    CHECK(onFrameGrid(*s.markIn, r30));
    CHECK(onFrameGrid(*s.markOut, r30));
    CHECK(onFrameGrid(s.duration, r30));
    REQUIRE(r.changes.sequences.size() == 1);
    CHECK(r.changes.sequences[0].settingsChanged);

    // To 23.976 and back to 25: still frame-accurate.
    edit::SequenceSettingsChange film;
    film.frameRate = Rate{24000, 1001};
    const auto f24 = ok(edit::setSequenceSettings(r.state, tl.seq, film));
    requireOnGrid(*f24.state.findSequence(tl.seq), Rate{24000, 1001});
    edit::SequenceSettingsChange back;
    back.frameRate = kRate;
    const auto f25 = ok(edit::setSequenceSettings(f24.state, tl.seq, back));
    requireOnGrid(*f25.state.findSequence(tl.seq), kRate);
}

TEST_CASE("A conformed edit point that would need missing media moves the other way", "[model][sequenceSettings][conform]") {
    // B starts at 25 fps frame 7 with no media before its first frame: 7/25 s is nearest 30 fps frame 8 (earlier), which B
    // cannot reach, so the shared cut goes to frame 9 and A (which has media after it) grows to meet it.
    Timeline tl(R"(
        V1 tgt |A5aaaaaBbbbbbbbb|
    )");
    edit::SequenceSettingsChange change;
    change.frameRate = Rate{30, 1};
    const auto r = ok(edit::setSequenceSettings(tl.state, tl.seq, change));
    const Track& v1 = *r.state.findSequence(tl.seq)->videoTracks[0];
    REQUIRE(v1.clips.size() == 2);
    CHECK(v1.clips[0].end() == framesToTicks(9, Rate{30, 1}));
    CHECK(v1.clips[1].start() == framesToTicks(9, Rate{30, 1}));
    CHECK(v1.clips[1].sourceIn.value >= 0);
}

TEST_CASE("Locked tracks keep their clips when the timebase changes", "[model][sequenceSettings][conform]") {
    Timeline tl(R"(
        V2 lock |...Cccc|
        V1 tgt  |.Aaaa..|
    )");
    const Clip locked = tl.sequence().videoTracks[1]->clips[0];
    edit::SequenceSettingsChange change;
    change.frameRate = Rate{24, 1};
    const auto r = ok(edit::setSequenceSettings(tl.state, tl.seq, change));
    CHECK(r.state.findSequence(tl.seq)->videoTracks[1]->clips[0] == locked);
    const Clip& moved = r.state.findSequence(tl.seq)->videoTracks[0]->clips[0];
    CHECK(onFrameGrid(moved.start(), Rate{24, 1}));
    CHECK(onFrameGrid(moved.end(), Rate{24, 1}));
}

TEST_CASE("Sequence Settings change the frame, audio and display settings", "[model][sequenceSettings]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    edit::SequenceSettingsChange change;
    change.name = "Essay";
    change.width = 3840;
    change.height = 2160;
    change.pixelAspect = Rate{10, 11};
    change.fields = FieldOrder::UpperFirst;
    change.audioRate = Rate{96000, 1};
    change.previewFormat = "prores";
    change.videoDisplay = VideoTimeDisplay::Feet35mm;
    change.audioDisplay = AudioTimeDisplay::Milliseconds;
    change.startTimecode = Ticks::fromSeconds(3600);
    change.dropFrame = true; // 25 fps has no drop-frame timecode
    const auto r = ok(edit::setSequenceSettings(tl.state, tl.seq, change));
    const Sequence& s = *r.state.findSequence(tl.seq);
    CHECK(s.name == "Essay");
    CHECK(s.width == 3840);
    CHECK(s.pixelAspect == Rate{10, 11});
    CHECK(s.fields == FieldOrder::UpperFirst);
    CHECK(s.audioRate == Rate{96000, 1});
    CHECK(s.previewFormat == "prores");
    CHECK(s.videoDisplay == VideoTimeDisplay::Feet35mm);
    CHECK(s.audioDisplay == AudioTimeDisplay::Milliseconds);
    CHECK(s.startTimecode == Ticks::fromSeconds(3600));
    CHECK_FALSE(s.dropFrame);
    CHECK(s.videoTracks[0]->clips[0] == tl.sequence().videoTracks[0]->clips[0]); // no clip moved

    // Saved and read back.
    const auto json = io::toJson(r.state);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded->findSequence(tl.seq) == s);

    // At 29.97 drop-frame display is kept.
    edit::SequenceSettingsChange ntsc;
    ntsc.frameRate = Rate{30000, 1001};
    ntsc.dropFrame = true;
    CHECK(ok(edit::setSequenceSettings(tl.state, tl.seq, ntsc)).state.findSequence(tl.seq)->dropFrame);

    // Out-of-range values are refused.
    const auto refused = [&](auto set) {
        edit::SequenceSettingsChange bad;
        set(bad);
        return !edit::setSequenceSettings(tl.state, tl.seq, bad).has_value();
    };
    CHECK(refused([](auto& c) { c.width = 8; }));
    CHECK(refused([](auto& c) { c.frameRate = Rate{3000, 1}; }));
    CHECK(refused([](auto& c) { c.pixelAspect = Rate{1, 100}; }));
    CHECK(refused([](auto& c) { c.audioRate = Rate{1000, 1}; }));
    CHECK(refused([](auto& c) { c.previewFormat = std::string(40, 'x'); }));
    CHECK(refused([](auto& c) { c.startTimecode = Ticks::fromSeconds(25 * 3600); }));
    CHECK(refused([](auto& c) { c.name = std::string(); }));
}

TEST_CASE("Duplicate copies a sequence or a project item with Premiere's names", "[model][duplicate]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    const auto first = edit::duplicateSequence(tl.state, tl.seq);
    REQUIRE(first);
    requireValid(first->state);
    const Sequence& copy = *first->state.findSequence(first->id);
    CHECK(copy.name == "Fixture Copy");
    CHECK(copy.bin == tl.sequence().bin);
    REQUIRE(copy.videoTracks[0]->clips.size() == 2);
    CHECK(copy.videoTracks[0]->clips[0].id != tl.sequence().videoTracks[0]->clips[0].id); // new ids
    CHECK(copy.videoTracks[0]->clips[0].timelineRange == tl.sequence().videoTracks[0]->clips[0].timelineRange);
    const auto second = edit::duplicateSequence(first->state, tl.seq);
    REQUIRE(second);
    CHECK(second->state.findSequence(second->id)->name == "Fixture Copy 2");
    CHECK_FALSE(edit::duplicateSequence(tl.state, SequenceId{999999}));

    const MediaId a = tl.media('A');
    const auto item = edit::duplicateMedia(tl.state, a);
    REQUIRE(item);
    requireValid(item->state);
    const MediaItem& dup = *item->state.findMedia(item->id);
    CHECK(dup.name == "A Copy");
    CHECK(dup.locator == tl.state.findMedia(a)->locator);
    CHECK(dup.info == tl.state.findMedia(a)->info);
    CHECK(item->state.findSequence(tl.seq)->videoTracks[0]->clips[0].media == a); // clips keep the original
    const auto named = edit::duplicateMedia(tl.state, a, "Take 2");
    REQUIRE(named);
    CHECK(named->state.findMedia(named->id)->name == "Take 2");
}

TEST_CASE("New Sequence From Clip matches the item and edits it in", "[model][newSequenceFromClip]") {
    Timeline tl(R"(
        V1 tgt |....|
    )");
    ProjectState state = tl.state;
    // A 59.94 fps phone video turned 90 degrees, with one audio stream at 44.1 kHz.
    MediaItem phone;
    phone.name = "IMG_0042.MOV";
    phone.locator.path = "D:/footage/IMG_0042.MOV";
    phone.info.duration = Ticks::fromSeconds(4);
    VideoStreamInfo v;
    v.width = 1920;
    v.height = 1080;
    v.rotationDegrees = 90;
    v.frameRate = Rate{60000, 1001};
    v.duration = Ticks::fromSeconds(4);
    phone.info.video.push_back(v);
    AudioStreamInfo a;
    a.index = 1;
    a.channels = 2;
    a.sampleRate = Rate{44100, 1};
    a.duration = Ticks::fromSeconds(4);
    phone.info.audio.push_back(a);
    auto added = edit::addMedia(state, phone);
    REQUIRE(added);
    const auto made = edit::newSequenceFromClip(added->state, added->id);
    REQUIRE(made);
    requireValid(made->state);
    const Sequence& s = *made->state.findSequence(made->id);
    CHECK(s.name == "IMG_0042.MOV");
    CHECK(s.width == 1080); // as it displays
    CHECK(s.height == 1920);
    CHECK(s.frameRate == Rate{60000, 1001});
    CHECK(s.audioRate == Rate{44100, 1});
    REQUIRE(s.videoTracks[0]->clips.size() == 1);
    CHECK(s.videoTracks[0]->clips[0].media == added->id);
    CHECK(s.videoTracks[0]->clips[0].start() == Ticks{});
    CHECK(s.videoTracks[0]->clips[0].duration() == Ticks::fromSeconds(4));
    REQUIRE(s.audioTracks[0]->clips.size() == 1);
    CHECK(s.audioTracks[0]->clips[0].linkGroup == s.videoTracks[0]->clips[0].linkGroup);

    // From the fixture's 25 fps media: two audio streams on A1 and A2.
    const auto fromA = edit::newSequenceFromClip(tl.state, tl.media('A'), "A edit");
    REQUIRE(fromA);
    const Sequence& sa = *fromA->state.findSequence(fromA->id);
    CHECK(sa.name == "A edit");
    CHECK(sa.frameRate == kRate);
    CHECK(sa.audioTracks.size() == 3);
    CHECK(sa.audioTracks[1]->clips.size() == 1);
    CHECK_FALSE(edit::newSequenceFromClip(tl.state, MediaId{999999}));
}

TEST_CASE("The display format shows sequence times as timecode, frames or feet and frames", "[model][sequenceSettings]") {
    Sequence s;
    s.frameRate = Rate{24, 1};
    const Ticks t = framesToTicks(1234, s.frameRate);
    CHECK(edit::formatSequenceTime(s, t) == ticksToTimecode(t, s.frameRate, false));
    s.videoDisplay = VideoTimeDisplay::Frames;
    CHECK(edit::formatSequenceTime(s, t) == "1234");
    CHECK(edit::formatSequenceTime(s, t + Ticks{1}) == "1234"); // the frame that contains it
    CHECK(edit::formatSequenceTime(s, Ticks{-1}) == "-1");
    s.videoDisplay = VideoTimeDisplay::Feet35mm; // 16 frames a foot
    CHECK(edit::formatSequenceTime(s, t) == "0077+02");
    s.videoDisplay = VideoTimeDisplay::Feet16mm; // 40 frames a foot
    CHECK(edit::formatSequenceTime(s, t) == "0030+34");
    // Drop-frame timecode where the sequence uses it.
    s.videoDisplay = VideoTimeDisplay::Timecode;
    s.frameRate = Rate{30000, 1001};
    s.dropFrame = true;
    CHECK(edit::formatSequenceTime(s, framesToTicks(1800, s.frameRate)) == "00:01:00;02");
}
TEST_CASE("Show Audio Time Units formats times as audio samples or milliseconds", "[model][sequenceSettings][audioUnits]") {
    Sequence s;
    s.audioRate = kRate48kHz;
    // 1 h 2 min 3 s and 9600 samples (0.2 s).
    const Ticks t = Ticks::fromSeconds(3723) + framesToTicks(9600, s.audioRate);
    CHECK(edit::formatSequenceAudioTime(s, t) == "01:02:03:09600");
    CHECK(edit::formatSequenceAudioTime(s, Ticks{}) == "00:00:00:00000");
    CHECK(edit::formatSequenceAudioTime(s, framesToTicks(47999, s.audioRate)) == "00:00:00:47999");
    // One sample less than a frame boundary is still that sample (sample-exact, not frame-rounded).
    CHECK(edit::formatSequenceAudioTime(s, framesToTicks(1, s.audioRate)) == "00:00:00:00001");
    s.audioRate = Rate{44100, 1};
    CHECK(edit::formatSequenceAudioTime(s, framesToTicks(44099, s.audioRate)) == "00:00:00:44099");
    s.audioDisplay = AudioTimeDisplay::Milliseconds;
    CHECK(edit::formatSequenceAudioTime(s, t) == "01:02:03.200");
    CHECK(edit::formatSequenceAudioTime(s, Ticks::fromSeconds(1) + Ticks{kTicksPerSecond / 1000 * 5}) == "00:00:01.005");
    CHECK(edit::formatSequenceAudioTime(s, -Ticks::fromSeconds(2)) == "-00:00:02.000");
}

TEST_CASE("A sequence's label colour is a Sequence Settings field, checked, saved and read back",
          "[model][sequenceSettings][label]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    CHECK(tl.state.findSequence(tl.seq)->colorIndex == 0);
    edit::SequenceSettingsChange label;
    label.colorIndex = 7;
    const auto labelled = edit::setSequenceSettings(tl.state, tl.seq, label);
    REQUIRE(labelled);
    CHECK(labelled->state.findSequence(tl.seq)->colorIndex == 7);
    CHECK_FALSE(labelled->changes.empty());
    requireValid(labelled->state);
    // Saved and read back.
    const std::string saved = io::toJson(labelled->state).value();
    const auto back = io::fromJson(saved);
    REQUIRE(back);
    CHECK(back->findSequence(tl.seq)->colorIndex == 7);
    CHECK(io::toJson(*back).value() == saved);
    // Out of range: refused, nothing changes.
    label.colorIndex = 16;
    CHECK_FALSE(edit::setSequenceSettings(tl.state, tl.seq, label));
    label.colorIndex = -1;
    CHECK_FALSE(edit::setSequenceSettings(tl.state, tl.seq, label));
}
