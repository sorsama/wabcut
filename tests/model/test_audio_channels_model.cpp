// A.9 audio channels in the model: channel maps on clips and media items (Modify > Audio Channels and its presets),
// split to mono, track channel formats and 5.1 panners, the master's format: the operations, validation, change
// detection and project files (absent until set: other projects stay byte-identical).

#include "TimelineFixture.h"

#include <wabcut/model/AudioChannelOps.h>
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
    A2     |........|
)";

std::vector<std::int32_t> sources(const AudioChannelMap& m, std::size_t channel) { return m.channels[channel].sources; }

} // namespace

TEST_CASE("Channel map presets: identity, mono, sum to mono, fill and swap", "[model][channels]") {
    const AudioChannelMap stereo = AudioChannelMap::identity(2);
    CHECK(stereo.format == ChannelFormat::Stereo);
    REQUIRE(stereo.channelCount() == 2);
    CHECK(sources(stereo, 0) == std::vector<std::int32_t>{0});
    CHECK(sources(stereo, 1) == std::vector<std::int32_t>{1});
    CHECK(AudioChannelMap::identity(1).format == ChannelFormat::Mono);
    CHECK(AudioChannelMap::identity(6).format == ChannelFormat::Surround51);
    CHECK(AudioChannelMap::identity(4).format == ChannelFormat::Adaptive);
    CHECK(AudioChannelMap::identity(4).channelCount() == 4);

    const AudioChannelMap right = AudioChannelMap::mono(1);
    CHECK(right.format == ChannelFormat::Mono);
    CHECK(sources(right, 0) == std::vector<std::int32_t>{1});
    const AudioChannelMap sum = AudioChannelMap::sumToMono(2);
    CHECK(sources(sum, 0) == std::vector<std::int32_t>{0, 1});
    const AudioChannelMap fillLeft = AudioChannelMap::fill(1); // Fill Left with Right
    CHECK(sources(fillLeft, 0) == std::vector<std::int32_t>{1});
    CHECK(sources(fillLeft, 1) == std::vector<std::int32_t>{1});
    const AudioChannelMap swapped = AudioChannelMap::swap();
    CHECK(sources(swapped, 0) == std::vector<std::int32_t>{1});
    CHECK(sources(swapped, 1) == std::vector<std::int32_t>{0});
    for (const AudioChannelMap& m : {stereo, right, sum, fillLeft, swapped, AudioChannelMap::identity(6)}) {
        CHECK_FALSE(channelMapProblem(m));
    }
    CHECK(toString(ChannelFormat::Surround51) == "5.1");
    CHECK(channelFormatFromString("adaptive") == ChannelFormat::Adaptive);
    CHECK_FALSE(channelFormatFromString("7.1"));
}

TEST_CASE("Channel maps are validated and bounded", "[model][channels]") {
    AudioChannelMap m = AudioChannelMap::identity(2);
    m.channels.pop_back();
    CHECK(channelMapProblem(m)); // stereo needs 2
    m = AudioChannelMap::identity(2);
    m.channels[0].sources = {kMaxSourceChannels};
    CHECK(channelMapProblem(m));
    m.channels[0].sources = {-1};
    CHECK(channelMapProblem(m));
    m.channels[0].sources = {0, 0};
    CHECK(channelMapProblem(m)); // twice
    m.channels[0].sources = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(channelMapProblem(m)); // more than kMaxRouteSources
    m.channels[0].sources = {};
    CHECK_FALSE(channelMapProblem(m)); // silent is fine
    m.channels[0].gainDb = ParamValue(30.0);
    CHECK(channelMapProblem(m));
    m.channels[0].gainDb = ParamValue(-6.0);
    CHECK_FALSE(channelMapProblem(m));
    AudioChannelMap wide;
    wide.format = ChannelFormat::Adaptive;
    wide.channels.resize(static_cast<std::size_t>(kMaxClipChannels + 1));
    CHECK(channelMapProblem(wide));
}

TEST_CASE("Clip and media channel maps: set, clear, the clip's wins, audio clips only", "[model][channels]") {
    Timeline tl(kPicture);
    const ClipId audio = tl.clip("A1@0");
    const ClipId video = tl.clip("V1@0");
    const MediaId media = tl.media('A');
    CHECK(edit::effectiveChannelMap(tl.state.findSequence(tl.seq)->findClip(audio)->clip(), tl.state.findMedia(media)) == nullptr);

    const ClipId one[] = {audio};
    const edit::EditResult set = ok(edit::setClipChannels(tl.state, tl.seq, one, AudioChannelMap::fill(0)));
    const Clip& mapped = set.state.findSequence(tl.seq)->findClip(audio)->clip();
    REQUIRE(mapped.channelMap());
    CHECK(*mapped.channelMap() == AudioChannelMap::fill(0));
    // The change is an audio change of that clip (the player refills it).
    const ChangeSet changes = diff(tl.state, set.state);
    REQUIRE_FALSE(changes.empty());

    auto withMedia = edit::setMediaChannels(set.state, media, AudioChannelMap::swap());
    REQUIRE(withMedia);
    const MediaItem* item = withMedia->findMedia(media);
    REQUIRE(item->channels);
    CHECK(*edit::effectiveChannelMap(mapped, item) == AudioChannelMap::fill(0)); // the clip's own wins
    const edit::EditResult cleared = ok(edit::setClipChannels(*withMedia, tl.seq, one, std::nullopt));
    const Clip& plain = cleared.state.findSequence(tl.seq)->findClip(audio)->clip();
    CHECK(plain.channelMap() == nullptr);
    CHECK(*edit::effectiveChannelMap(plain, cleared.state.findMedia(media)) == AudioChannelMap::swap()); // then the item's

    const ClipId pictures[] = {video};
    CHECK_FALSE(edit::setClipChannels(tl.state, tl.seq, pictures, AudioChannelMap::swap()));
    AudioChannelMap bad = AudioChannelMap::swap();
    bad.channels.pop_back();
    CHECK_FALSE(edit::setClipChannels(tl.state, tl.seq, one, bad));
    CHECK_FALSE(edit::setMediaChannels(tl.state, MediaId{99999}, AudioChannelMap::swap()));
    Timeline locked(R"(
        V1 tgt |Aaaaaaaa|
        A1 lock |Aaaaaaaa|
    )");
    const ClipId lockedClip[] = {locked.clip("A1@0")};
    CHECK_FALSE(edit::setClipChannels(locked.state, locked.seq, lockedClip, AudioChannelMap::swap()));
}

TEST_CASE("Split to mono: the clip plays channel 1, a linked copy channel 2 on its own track", "[model][channels]") {
    Timeline tl(kPicture);
    tl.linkMatching();
    const ClipId audio = tl.clip("A1@0");
    const int channels[] = {0, 1};
    const TrackId a2[] = {tl.track("A2")};
    const edit::EditResult r = ok(edit::splitClipToMono(tl.state, tl.seq, audio, channels, a2));
    const Sequence& seq = *r.state.findSequence(tl.seq);
    const Clip& first = seq.findClip(audio)->clip();
    REQUIRE(seq.findTrack(tl.track("A2"))->clips.size() == 1);
    const Clip& second = seq.findTrack(tl.track("A2"))->clips[0];
    REQUIRE(first.channelMap());
    REQUIRE(second.channelMap());
    CHECK(*first.channelMap() == AudioChannelMap::mono(0));
    CHECK(*second.channelMap() == AudioChannelMap::mono(1));
    CHECK(second.timelineRange == first.timelineRange);
    CHECK(second.sourceIn == first.sourceIn);
    CHECK(second.media == first.media);
    CHECK(first.linkGroup.isValid());
    CHECK(second.linkGroup == first.linkGroup);
    CHECK(seq.findClip(tl.clip("V1@0"))->clip().linkGroup == first.linkGroup); // the picture stays linked to both
    CHECK(r.created.size() == 1);
    // An occupied target is refused; so is the clip's own track.
    Timeline busy(R"(
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
        A2     |..Bbbb..|
    )");
    const TrackId busyA2[] = {busy.track("A2")};
    CHECK_FALSE(edit::splitClipToMono(busy.state, busy.seq, busy.clip("A1@0"), channels, busyA2));
    const TrackId own[] = {tl.track("A1")};
    CHECK_FALSE(edit::splitClipToMono(tl.state, tl.seq, audio, channels, own));
}

TEST_CASE("Track formats, 5.1 panners and the master's format", "[model][channels]") {
    Timeline tl(kPicture);
    edit::TrackStateChange mono;
    mono.format = ChannelFormat::Mono;
    const edit::EditResult m = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A1"), mono));
    CHECK(m.state.findSequence(tl.seq)->findTrack(tl.track("A1"))->format == ChannelFormat::Mono);
    CHECK_FALSE(diff(tl.state, m.state).empty()); // a header change the player sees
    CHECK_FALSE(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), mono));
    edit::TrackStateChange pan;
    pan.surround = SurroundPan{ParamValue(-0.5), ParamValue(0.5), ParamValue(-12.0)};
    const edit::EditResult p = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A1"), pan));
    CHECK(p.state.findSequence(tl.seq)->findTrack(tl.track("A1"))->surround.x == ParamValue(-0.5));
    pan.surround = SurroundPan{ParamValue(2.0), ParamValue(0.5), ParamValue(-12.0)};
    CHECK_FALSE(edit::setTrackState(tl.state, tl.seq, tl.track("A1"), pan));
    pan.surround = SurroundPan{ParamValue(0.0), ParamValue(0.5), ParamValue(6.0)};
    CHECK_FALSE(edit::setTrackState(tl.state, tl.seq, tl.track("A1"), pan));

    const edit::EditResult surround = ok(edit::setMasterFormat(tl.state, tl.seq, ChannelFormat::Surround51));
    CHECK(surround.state.findSequence(tl.seq)->masterFormat == ChannelFormat::Surround51);
    CHECK_FALSE(diff(tl.state, surround.state).empty());
    CHECK_FALSE(edit::setMasterFormat(tl.state, tl.seq, ChannelFormat::Adaptive));
    CHECK_FALSE(edit::setMasterFormat(tl.state, tl.seq, ChannelFormat::Mono));

    // Hand-edited states the validator refuses.
    Sequence seq = *tl.state.findSequence(tl.seq);
    Track v1 = *seq.videoTracks[0];
    v1.format = ChannelFormat::Surround51;
    seq.videoTracks[0] = std::make_shared<const Track>(std::move(v1));
    CHECK_FALSE(validate(tl.state.withSequence(std::make_shared<const Sequence>(seq)), Validation::Structure));
    seq = *tl.state.findSequence(tl.seq);
    seq.masterFormat = ChannelFormat::Adaptive;
    CHECK_FALSE(validate(tl.state.withSequence(std::make_shared<const Sequence>(seq)), Validation::Structure));
}

TEST_CASE("Channels round-trip through project files; projects without them save byte-identical", "[model][channels][io]") {
    Timeline tl(kPicture);
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    // ("channels" and "format" also name media info fields, so the new keys are checked where they are unique.)
    for (const char* key : {"\"surround\"", "\"masterFormat\"", "\"sources\""}) {
        INFO(key);
        CHECK(plain->find(key) == std::string::npos);
    }
    const auto reloaded = io::fromJson(*plain);
    REQUIRE(reloaded);
    CHECK(*io::toJson(*reloaded) == *plain);

    AudioChannelMap map = AudioChannelMap::fill(0);
    map.channels[1].muted = true;
    Curve keys;
    keys.keys = {Keyframe{f(0), 0.0}, Keyframe{f(4), -12.0}};
    map.channels[0].gainDb = ParamValue(keys);
    const ClipId one[] = {tl.clip("A1@0")};
    ProjectState s = ok(edit::setClipChannels(tl.state, tl.seq, one, map)).state;
    s = *edit::setMediaChannels(s, tl.media('A'), AudioChannelMap::sumToMono(2));
    edit::TrackStateChange change;
    change.format = ChannelFormat::Adaptive;
    change.surround = SurroundPan{ParamValue(0.25), ParamValue(-0.5), ParamValue(-6.0)};
    s = ok(edit::setTrackState(s, tl.seq, tl.track("A1"), change)).state;
    s = ok(edit::setMasterFormat(s, tl.seq, ChannelFormat::Surround51)).state;
    const auto saved = io::toJson(s);
    REQUIRE(saved);
    CHECK(saved->find("\"masterFormat\": \"5.1\"") != std::string::npos);
    const auto back = io::fromJson(*saved);
    REQUIRE(back);
    CHECK(*back == s);
    CHECK(*io::toJson(*back) == *saved);
    const Clip& clip = back->findSequence(tl.seq)->findClip(tl.clip("A1@0"))->clip();
    REQUIRE(clip.channelMap());
    CHECK(*clip.channelMap() == map);
    CHECK(back->findSequence(tl.seq)->findTrack(tl.track("A1"))->format == ChannelFormat::Adaptive);

    // Files past the bounds or with unknown formats are refused.
    std::string broken = *saved;
    broken.replace(broken.find("\"masterFormat\": \"5.1\""), std::string("\"masterFormat\": \"5.1\"").size(), "\"masterFormat\": \"7.1\"");
    CHECK_FALSE(io::fromJson(broken));
    std::string tooMany = *saved;
    const std::string sourcesKey = "\"sources\": [";
    const auto at = tooMany.find(sourcesKey);
    REQUIRE(at != std::string::npos);
    tooMany.insert(at + sourcesKey.size(), "70, ");
    CHECK_FALSE(io::fromJson(tooMany));
}
