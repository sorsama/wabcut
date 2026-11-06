// AI video effects (0.7): which clips have AI effects and what Process has to make - the chain (Auto resolved against the media
// and the sequence, the media's Master Clip Effects included), the cache identity, and the part of the media a clip uses.

#include "VideoTestUtil.h"

#include <wabcut/ai/video/Targets.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;

namespace {

std::shared_ptr<MediaItem> mediaOf(ProjectState& s, int w, int h, int rotation = 0, const char* hash = "00112233aabbccdd") {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "clip.mp4";
    item->locator.fingerprint.size = 4000;
    item->locator.fingerprint.sampleHash = hash;
    VideoStreamInfo v;
    v.index = 3; // the container's index of the stream
    v.width = w;
    v.height = h;
    v.frameRate = Rate{25, 1};
    v.rotationDegrees = rotation;
    v.duration = Ticks::fromSeconds(10);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

EffectInstance restore(ProjectState& s, double scale = 2.0, double deblur = 50.0) {
    EffectInstance e;
    e.id = s.ids.allocate<EffectInstanceId>();
    e.effect = std::string(kAiRestoreEffect);
    e.set("scale", ParamValue(scale));
    e.set("deblur", ParamValue(deblur));
    return e;
}

Clip clipOf(ProjectState& s, MediaId media, std::int64_t startSeconds, std::int64_t lengthSeconds, std::int64_t sourceInSeconds) {
    Clip c;
    c.id = s.ids.allocate<ClipId>();
    c.media = media;
    c.timelineRange = {Ticks::fromSeconds(startSeconds), Ticks::fromSeconds(lengthSeconds)};
    c.sourceIn = Ticks::fromSeconds(sourceInSeconds);
    return c;
}

Sequence sequenceOf(ProjectState& s, std::vector<std::vector<Clip>> tracks, int height = 1080) {
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.width = height * 16 / 9;
    seq.height = height;
    for (auto& clips : tracks) {
        Track t;
        t.id = s.ids.allocate<TrackId>();
        t.clips = ClipList(std::move(clips));
        seq.videoTracks.push_back(std::make_shared<const Track>(std::move(t)));
    }
    return seq;
}

} // namespace

TEST_CASE("A clip's target: chain, cache identity, and the part of the media it uses", "[ai][video][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    s.media = {media};
    Clip clip = clipOf(s, media->id, 4, 2, 1);
    clip.setEffects({restore(s)});
    const Sequence seq = sequenceOf(s, {{clip}});
    const Clip& placed = seq.videoTracks.front()->clips.front();
    const auto t = clipTarget(s, seq, placed, 0);
    REQUIRE(t.has_value());
    CHECK(t->clip == placed.id);
    CHECK(t->media == media->id);
    CHECK(t->streamIndex == 0);
    CHECK(t->containerStream == 3);
    CHECK(t->path == "clip.mp4");
    CHECK(t->width == 960);
    CHECK(t->height == 540);
    CHECK(t->fps == 25.0);
    REQUIRE(t->chain.stages.size() == 1);
    CHECK(t->chain.stages[0].scale == 2);
    CHECK(t->identity.valid());
    CHECK(t->identity == identityOf(*media, 0, t->chain));
    CHECK(t->range.start == Ticks::fromSeconds(1));
    CHECK(t->range.duration == Ticks::fromSeconds(2));
    CHECK(t->expected == 50);
    CHECK(t->timeline.start == Ticks::fromSeconds(4));
    CHECK(t->frame == framesToTicks(1, Rate{25, 1}));
}

TEST_CASE("A target reaches only the media that exists: the range is clamped, a freeze frame is one picture", "[ai][video][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    s.media = {media};
    Clip longClip = clipOf(s, media->id, 0, 5, 8); // asks for 8 s .. 13 s of a 10 s file
    longClip.setEffects({restore(s)});
    const Sequence seq = sequenceOf(s, {{longClip}});
    const auto t = clipTarget(s, seq, seq.videoTracks.front()->clips.front(), 0);
    REQUIRE(t.has_value());
    CHECK(t->range.end() <= Ticks::fromSeconds(10));
    CHECK(t->expected >= 49);
    CHECK(t->expected <= 51);
    CHECK(framesIn(TimeRange{Ticks{}, Ticks::fromSeconds(1)}, 25.0) == 25);
    CHECK(framesIn(TimeRange{Ticks{}, Ticks{1}}, 25.0) == 1);
    CHECK(framesIn(TimeRange{}, 25.0) == 0);
    CHECK(framesIn(TimeRange{Ticks{}, Ticks::fromSeconds(1)}, 0.0) == 0);
}

TEST_CASE("A turned phone clip is judged by its displayed size; a clip that cannot have AI effects has no target", "[ai][video][targets]") {
    ProjectState s;
    auto turned = mediaOf(s, 1920, 1080, 90); // a portrait clip stored sideways: shown 1080 x 1920
    auto still = mediaOf(s, 960, 540);
    still->info.isStillImage = true;
    auto title = std::make_shared<MediaItem>();
    title->id = s.ids.allocate<MediaId>();
    title->generator = GeneratorSpec{};
    title->info.video.push_back(VideoStreamInfo{});
    s.media = {turned, still, title};
    Clip a = clipOf(s, turned->id, 0, 2, 0);
    a.setEffects({restore(s, 0.0)}); // Auto
    Clip b = clipOf(s, still->id, 2, 2, 0);
    b.setEffects({restore(s)});
    Clip c = clipOf(s, title->id, 4, 2, 0);
    c.setEffects({restore(s)});
    Clip d = clipOf(s, turned->id, 6, 2, 0);
    d.stream.index = 4; // no such stream
    d.setEffects({restore(s)});
    const Sequence seq = sequenceOf(s, {{a, b, c, d}});
    const auto& clips = seq.videoTracks.front()->clips;
    const auto turnedTarget = clipTarget(s, seq, clips[0], 0);
    REQUIRE(turnedTarget.has_value());
    CHECK(turnedTarget->width == 1080);
    CHECK(turnedTarget->height == 1920);
    CHECK(turnedTarget->chain.stages[0].scale == 1); // 1920 shown lines on a 1080 canvas: not enlarged
    CHECK_FALSE(clipTarget(s, seq, clips[1], 0)); // a still image
    CHECK_FALSE(clipTarget(s, seq, clips[2], 0)); // a title
    CHECK_FALSE(clipTarget(s, seq, clips[3], 0)); // a stream that does not exist
}

TEST_CASE("An offline media has a target without a path; settings that change nothing give an empty chain", "[ai][video][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    media->locator.offline = true;
    s.media = {media};
    Clip clip = clipOf(s, media->id, 0, 2, 0);
    clip.setEffects({restore(s, 1.0, 0.0)}); // 1x, no deblur: nothing to do
    const Sequence seq = sequenceOf(s, {{clip}});
    const auto t = clipTarget(s, seq, seq.videoTracks.front()->clips.front(), 0);
    REQUIRE(t.has_value());
    CHECK(t->path.empty());
    CHECK(t->chain.empty());
    CHECK_FALSE(t->identity.valid());
    CHECK(sequenceTargets(s, seq, std::nullopt).empty());
}

TEST_CASE("The media's Master Clip Effects count for every clip of it", "[ai][video][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    media->masterEffects = {restore(s)};
    s.media = {media};
    Clip clip = clipOf(s, media->id, 0, 2, 0);
    const Sequence seq = sequenceOf(s, {{clip}});
    const auto t = clipTarget(s, seq, seq.videoTracks.front()->clips.front(), 0);
    REQUIRE(t.has_value());
    REQUIRE(t->chain.stages.size() == 1);
    CHECK(t->chain.stages[0].effect == kAiRestoreEffect);
}

TEST_CASE("Sequence targets: tracks that are off are skipped, clips of one media with one chain are one job over the union",
          "[ai][video][targets]") {
    ProjectState s;
    auto media = mediaOf(s, 960, 540);
    auto other = mediaOf(s, 960, 540, 0, "ffeeddccbbaa9988");
    s.media = {media, other};
    const EffectInstance fx = restore(s);
    Clip first = clipOf(s, media->id, 0, 2, 0);
    first.setEffects({fx});
    Clip second = clipOf(s, media->id, 4, 2, 5);
    second.setEffects({fx});
    Clip third = clipOf(s, other->id, 8, 2, 0);
    third.setEffects({fx});
    Clip plain = clipOf(s, media->id, 12, 2, 0);
    Clip hidden = clipOf(s, media->id, 0, 2, 0);
    hidden.setEffects({fx});
    Sequence seq = sequenceOf(s, {{first, second, third, plain}, {hidden}});
    {
        Track off = *seq.videoTracks[1];
        off.enabled = false;
        seq.videoTracks[1] = std::make_shared<const Track>(std::move(off));
    }
    const auto targets = sequenceTargets(s, seq, std::nullopt);
    REQUIRE(targets.size() == 2); // media (two clips merged) and other
    CHECK(targets[0].media == media->id);
    CHECK(targets[0].range.start == Ticks{});
    CHECK(targets[0].range.end() == Ticks::fromSeconds(7)); // 0..2 s and 5..7 s of the media
    CHECK(targets[0].expected == 175);
    CHECK(targets[1].media == other->id);
    CHECK(targets[0].identity != targets[1].identity); // another file: its own pictures

    // Only what an exported range reaches.
    const auto some = sequenceTargets(s, seq, TimeRange{Ticks{Ticks::fromSeconds(15).value / 2}, Ticks::fromSeconds(3)});
    REQUIRE(some.size() == 1);
    CHECK(some[0].media == other->id);
}
