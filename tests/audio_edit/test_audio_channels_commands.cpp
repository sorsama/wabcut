// A.9 audio channel commands through the CommandEngine: Modify > Audio Channels presets and maps on clips and media,
// Channel Volume, Fill Left / Right, split to mono onto separate tracks, the channel report and levels, track formats,
// the 5.1 panner and the master's format; one undo step each; capabilities.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/model/AudioChannelOps.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaa|
    A1 tgt |Aaaaaaaaaa|
    A2     |..........|
)";

CommandHarness harness(std::shared_ptr<command::CommandServices> services = nullptr) {
    Timeline tl(kPicture);
    tl.linkMatching();
    return CommandHarness(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 64, std::move(services)},
                          ai::edit::editingCommands());
}

const Clip& clipOf(CommandHarness& h, ClipId id) { return h.sequence(h.state()).findClip(id)->clip(); }

/// Reports channel 2 of every clip silent (a recording with sound on the left only).
class LeftOnlyAnalyzer final : public ChannelAnalyzer {
public:
    std::expected<std::vector<ChannelLevel>, Error> measure(const ProjectState&, const MediaItem&, int, TimeRange,
                                                             command::ExecutionControl*) override {
        ++calls;
        return std::vector<ChannelLevel>{{-3.0, -18.0}, {-144.0, -144.0}};
    }
    int calls = 0;
};

} // namespace

TEST_CASE("audio.setChannels: presets and maps on clips, one undo step, cleared with default", "[audio][edit][channels]") {
    CommandHarness h = harness();
    const ClipId a1 = h.clip("A1@0");
    const std::size_t undo = h.session.undoCount();
    auto r = h.ok("audio.setChannels", {{"clips", Json::array({a1.value})}, {"preset", "fillLeft"}});
    CHECK(h.session.undoCount() == undo + 1);
    REQUIRE(clipOf(h, a1).channelMap());
    CHECK(*clipOf(h, a1).channelMap() == AudioChannelMap::fill(1));
    CHECK(r.data["clips"][0]["map"]["channels"][0]["sources"] == Json::array({2})); // 1-based in reports
    CHECK(r.data["clips"][0]["mapFrom"] == "clip");
    // A video clip stands for its linked audio.
    h.ok("audio.setChannels", {{"clips", Json::array({h.clip("V1@0").value})}, {"preset", "swap"}});
    CHECK(*clipOf(h, a1).channelMap() == AudioChannelMap::swap());
    h.ok("audio.setChannels", {{"clips", Json::array({a1.value})}, {"preset", "mono"}, {"channel", 2}});
    CHECK(*clipOf(h, a1).channelMap() == AudioChannelMap::mono(1));
    h.ok("audio.setChannels", {{"clips", Json::array({a1.value})},
                               {"map", Json{{"format", "stereo"},
                                            {"channels", Json::array({Json{{"sources", Json::array({1, 2})}},
                                                                      Json{{"sources", Json::array({2})}, {"muted", true}, {"gainDb", -6}}})}}}});
    const AudioChannelMap& custom = *clipOf(h, a1).channelMap();
    CHECK(custom.channels[0].sources == std::vector<std::int32_t>{0, 1});
    CHECK(custom.channels[1].muted);
    CHECK(custom.channels[1].gainDb == ParamValue(-6.0));
    h.ok("audio.setChannels", {{"clips", Json::array({a1.value})}, {"preset", "default"}});
    CHECK(clipOf(h, a1).channelMap() == nullptr);
    // Refused: no preset or map, both, a bad map, an unknown preset.
    CHECK(h.errorOf("audio.setChannels", {{"clips", Json::array({a1.value})}}) == "InvalidParams");
    CHECK(h.errorOf("audio.setChannels", {{"clips", Json::array({a1.value})},
                                          {"map", Json{{"format", "stereo"}, {"channels", Json::array({Json{{"sources", Json::array({1})}}})}}}}) ==
          "InvalidParams");
    CHECK(h.errorOf("audio.setChannels", {{"clips", Json::array({a1.value})}, {"preset", "bogus"}}) == "InvalidParams");
}

TEST_CASE("audio.setMediaChannels changes media items: a project edit", "[audio][edit][channels][capability]") {
    CommandHarness h = harness();
    const MediaId a = h.tl.media('A');
    h.ok("audio.setMediaChannels", {{"media", Json::array({a.value})}, {"preset", "sumToMono"}});
    REQUIRE(h.state()->findMedia(a)->channels);
    CHECK(*h.state()->findMedia(a)->channels == AudioChannelMap::sumToMono(2));
    auto report = h.ok("audio.channels", {{"clips", Json::array({h.clip("A1@0").value})}});
    CHECK(report.data["clips"][0]["mapFrom"] == "media");
    const auto& registry = ai::edit::editingCommands();
    CHECK(registry.find("audio.setMediaChannels")->capability == command::Capability::EditProject);
    CHECK(registry.find("audio.setChannels")->capability == command::Capability::EditSequence);
    CHECK(registry.find("audio.channelLevels")->capability == command::Capability::MediaAnalysis);
    CHECK(registry.find("audio.channels")->capability == command::Capability::Read);
    Timeline tl(kPicture);
    CommandHarness sequenceOnly(std::move(tl),
                                command::EnginePolicy{command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence}), 16, nullptr},
                                ai::edit::editingCommands());
    CHECK(sequenceOnly.errorOf("audio.setMediaChannels", {{"media", Json::array({a.value})}, {"preset", "swap"}}) == "PermissionDenied");
}

TEST_CASE("audio.setChannelVolume sets a channel's volume or a keyframe of it", "[audio][edit][channels]") {
    CommandHarness h = harness();
    const ClipId a1 = h.clip("A1@0");
    h.ok("audio.setChannelVolume", {{"clips", Json::array({a1.value})}, {"channel", 2}, {"db", -9}});
    const AudioChannelMap* map = clipOf(h, a1).channelMap();
    REQUIRE(map);
    AudioChannelMap expected = AudioChannelMap::identity(2);
    expected.channels[1].gainDb = ParamValue(-9.0);
    CHECK(*map == expected);
    h.ok("audio.setChannelVolume", {{"clips", Json::array({a1.value})}, {"channel", 1}, {"db", -3}, {"time", f(5).value}});
    const ParamValue& left = clipOf(h, a1).channelMap()->channels[0].gainDb;
    REQUIRE(left.isCurve());
    CHECK(left.curve()->keys.size() == 1);
    CHECK(left.curve()->keys[0].value == -3.0);
    CHECK(h.errorOf("audio.setChannelVolume", {{"clips", Json::array({a1.value})}, {"channel", 3}, {"db", 0}}) == "InvalidParams");
}

TEST_CASE("audio.fillChannel: Fill Left with Right / Fill Right with Left", "[audio][edit][channels]") {
    CommandHarness h = harness();
    const ClipId a1 = h.clip("A1@0");
    h.ok("audio.fillChannel", {{"clips", Json::array({a1.value})}, {"from", "left"}}); // left-only audio: both from the left
    CHECK(*clipOf(h, a1).channelMap() == AudioChannelMap::fill(0));
    h.ok("audio.fillChannel", {{"clips", Json::array({a1.value})}, {"from", "right"}, {"to", "left"}});
    CHECK(*clipOf(h, a1).channelMap() == AudioChannelMap::fill(1));
    CHECK(h.errorOf("audio.fillChannel", {{"clips", Json::array({a1.value})}, {"from", "left"}, {"to", "left"}}) == "InvalidParams");
}

TEST_CASE("audio.splitToMono: the two mics onto separate tracks, linked, one undo step", "[audio][edit][channels]") {
    CommandHarness h = harness();
    const ClipId a1 = h.clip("A1@0");
    const std::size_t undo = h.session.undoCount();
    auto r = h.ok("audio.splitToMono", {{"clip", a1.value}});
    CHECK(h.session.undoCount() == undo + 1);
    REQUIRE(r.data["clips"].size() == 2);
    CHECK(r.data["clips"][0]["track"] == "A1");
    CHECK(r.data["clips"][1]["track"] == "A2");
    const Sequence& seq = h.sequence(h.state());
    const Clip& first = seq.findClip(a1)->clip();
    const Clip& second = seq.findClip(ClipId{r.data["clips"][1]["clip"].get<std::uint64_t>()})->clip();
    CHECK(*first.channelMap() == AudioChannelMap::mono(0));
    CHECK(*second.channelMap() == AudioChannelMap::mono(1));
    CHECK(first.linkGroup == second.linkGroup);
    CHECK(seq.findClip(h.clip("V1@0"))->clip().linkGroup == first.linkGroup);
    // With A2 taken, a new track is made.
    Timeline busy(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
        A2     |..Bbbb....|
    )");
    CommandHarness b(std::move(busy), command::EnginePolicy{}, ai::edit::editingCommands());
    auto split = b.ok("audio.splitToMono", {{"clip", b.clip("A1@0").value}});
    CHECK(split.data["clips"][1]["track"] == "A3");
    CHECK(b.sequence(b.state()).audioTracks.size() == 3);
    // The clip's own track cannot take another channel; the track count must match the channels.
    CommandHarness again = harness();
    CHECK(again.errorOf("audio.splitToMono", {{"clip", again.clip("A1@0").value}, {"tracks", Json::array({"A1"})}}) == "EditRejected");
    CHECK(again.errorOf("audio.splitToMono", {{"clip", again.clip("A1@0").value}, {"tracks", Json::array({"A2", "V1"})}}) ==
          "InvalidParams");
}

TEST_CASE("audio.channels and audio.channelLevels report what plays and which channels have sound", "[audio][edit][channels]") {
    auto analyzer = std::make_shared<LeftOnlyAnalyzer>();
    auto services = std::make_shared<AudioServices>(nullptr, nullptr);
    services->channels = analyzer;
    CommandHarness h = harness(services);
    const ClipId a1 = h.clip("A1@0");
    auto report = h.ok("audio.channels");
    CHECK(report.data["master"] == "stereo");
    CHECK(report.data["tracks"][0]["format"] == "stereo");
    REQUIRE(report.data["clips"].size() == 1);
    CHECK(report.data["clips"][0]["sourceChannels"] == 2);
    CHECK(report.data["clips"][0]["mapFrom"] == "none");
    auto levels = h.ok("audio.channelLevels", {{"clips", Json::array({a1.value})}});
    REQUIRE(levels.data["clips"][0]["channels"].size() == 2);
    CHECK(levels.data["clips"][0]["silentChannels"] == Json::array({2}));
    CHECK(analyzer->calls == 1);
    CommandHarness none = harness();
    CHECK(none.errorOf("audio.channelLevels", {{"clips", Json::array({none.clip("A1@0").value})}}) == "InvalidRequest");
}

TEST_CASE("track.setFormat, track.setSurroundPan and sequence.setMasterFormat", "[audio][edit][channels][surround]") {
    CommandHarness h = harness();
    h.ok("track.setFormat", {{"track", "A1"}, {"format", "5.1"}});
    CHECK(h.sequence(h.state()).findTrack(h.tl.track("A1"))->format == ChannelFormat::Surround51);
    CHECK_FALSE(h.errorOf("track.setFormat", {{"track", "V1"}, {"format", "mono"}}).empty());
    CHECK(h.errorOf("track.setFormat", {{"track", "A1"}, {"format", "7.1"}}) == "InvalidParams");
    auto pan = h.ok("track.setSurroundPan", {{"track", "A2"}, {"x", -0.5}, {"lfeDb", -12}});
    const SurroundPan& s = h.sequence(h.state()).findTrack(h.tl.track("A2"))->surround;
    CHECK(s.x == ParamValue(-0.5));
    CHECK(s.y == ParamValue(1.0)); // unchanged
    CHECK(s.lfeDb == ParamValue(-12.0));
    CHECK(pan.data["lfeDb"] == -12.0);
    const std::size_t undo = h.session.undoCount();
    h.ok("sequence.setMasterFormat", {{"format", "5.1"}});
    CHECK(h.sequence(h.state()).masterFormat == ChannelFormat::Surround51);
    CHECK(h.session.undoCount() == undo + 1);
    CHECK(h.errorOf("sequence.setMasterFormat", {{"format", "mono"}}) == "InvalidParams");
}
