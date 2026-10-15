// A.4 Enhance Speech playback (AudioTakes.h): a clip with an enabled wabcut.audio.enhanceSpeech effect plays its media's
// enhanced take at the same source time - sample-exact against the take at full strength, blended by Strength, before
// the clip's level and its redactions (a bleep is never enhanced) - and its source when the effect is off or the take
// is missing; preview == export; recording a take while playing is heard within the lead window. The synthetic feed
// plays a take path as a signal of its own (a sawtooth: every sample unique, so any misalignment shows).

#include "PlaybackFixture.h"

#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/AudioTakes.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;
using Catch::Approx;

namespace {

SyntheticSignal level(float value) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Constant;
    s.amplitude = value;
    return s;
}

SyntheticSignal saw() {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 7.0;
    s.amplitude = 0.4f;
    s.channelScale = 0.5f;
    return s;
}

std::filesystem::path takeFile(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "playback" / "takes";
    std::filesystem::create_directories(dir);
    const auto file = dir / (name + ".wav");
    std::ofstream(file) << "take"; // exists: AudioTakes plays it; the synthetic feed makes up its samples
    return file;
}

/// Records the enhanced take of media `letter` (stream 0), played by the feed as `signal`.
void recordTake(Project& p, char letter, const std::filesystem::path& file, SyntheticSignal signal, std::uint64_t generation = 1) {
    p.feed->setTake(pathToUtf8(file), signal);
    AudioTake t;
    t.stream = 0;
    t.kind = std::string(kEnhanceSpeechTake);
    t.path = pathToUtf8(file);
    t.model = "test@1";
    t.generation = generation;
    auto s = edit::setAudioTake(p.state, p.mediaId(letter), t);
    REQUIRE(s);
    p.state = std::move(*s);
}

EffectInstance enhance(Project& p, ParamValue strength, bool enabled = true) {
    EffectInstance e;
    e.id = p.state.ids.allocate<EffectInstanceId>();
    e.effect = "wabcut.audio.enhanceSpeech";
    e.enabled = enabled;
    e.set("strength", std::move(strength));
    return e;
}

void addTrack(Project& p, std::vector<Clip> clips) {
    Track track;
    track.id = p.state.ids.allocate<TrackId>();
    track.kind = TrackKind::Audio;
    for (Clip& c : clips) {
        track.clips.push_back(std::move(c));
    }
    Sequence seq = *p.state.sequences.front();
    seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
    p.state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
}

Clip clipWith(Project& p, const ClipSpec& spec, EffectStack effects) {
    Clip c = p.makeClip(spec);
    c.setEffects(std::move(effects));
    return c;
}

/// The export path, takes and redactions resolved as export resolves them, in odd block sizes.
std::vector<float> exported(const Project& p, Ticks start, std::size_t count, TrackRenderStats* stats = nullptr) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = Ticks{};
    options.redactions = AudioRedactions::of(p.state);
    options.takes = AudioTakes::of(p.state);
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, start, *p.feed, options);
    std::vector<float> out(count * 2);
    for (std::size_t done = 0; done < count;) {
        const std::size_t n = std::min<std::size_t>(count - done, 300 + done % 777);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    if (stats != nullptr) {
        *stats = mix.stats();
    }
    return out;
}

std::size_t at(Ticks t) { return static_cast<std::size_t>(t.value / kTicksPerFrame); }

/// The take's sample on `channel` for timeline frame k of a clip at 0 with source in `sourceIn`.
float takeAt(Project& p, const std::filesystem::path& file, Ticks sourceIn, std::size_t k, int channel) {
    (void)p;
    (void)file;
    const std::int64_t n = ticksToSamples(sourceIn, k48k) + static_cast<std::int64_t>(k);
    return SyntheticAudioFeed::evaluate(saw(), n, channel, k48k);
}

} // namespace

TEST_CASE("Enhance Speech plays the take sample-exact at full strength, in preview and export alike",
          "[playback][enhance][export]") {
    Project p;
    p.media('A', level(0.5f));
    const auto file = takeFile("exact");
    recordTake(p, 'A', file, saw());
    addTrack(p, {clipWith(p, {'A', ms(0), ms(800), ms(250)}, {enhance(p, ParamValue(1.0))})});
    const std::size_t n = at(ms(800));
    const auto out = exported(p, Ticks{}, n);
    std::size_t wrong = 0;
    for (std::size_t k = 0; k < n; ++k) {
        wrong += out[k * 2] != takeAt(p, file, ms(250), k, 0) || out[k * 2 + 1] != takeAt(p, file, ms(250), k, 1) ? 1 : 0;
    }
    CHECK(wrong == 0);

    Rig rig(p);
    rig.player->play();
    rig.pullFor(800);
    REQUIRE(rig.stream.size() >= out.size());
    CHECK(std::equal(out.begin(), out.end(), rig.stream.begin())); // preview == export
    CHECK(rig.player->stats().underrunFrames == 0);
    CHECK(p.feed->takeReads() > 0);
}

TEST_CASE("Enhance Speech blends by Strength on the parameter grid; the clip's level comes after it",
          "[playback][enhance]") {
    Project p;
    p.media('A', level(0.5f));
    const auto file = takeFile("blend");
    recordTake(p, 'A', file, saw());
    SECTION("a constant strength") {
        addTrack(p, {clipWith(p, {'A', ms(0), ms(500), ms(0), -6.0}, {enhance(p, ParamValue(0.25))})});
        const auto out = exported(p, Ticks{}, at(ms(500)));
        const float gain = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
        for (std::size_t k = 0; k < at(ms(500)); k += 37) {
            const float blended = 0.5f * 0.75f + takeAt(p, file, Ticks{}, k, 0) * 0.25f;
            CHECK(out[k * 2] == Approx(blended * gain).margin(1e-6));
        }
    }
    SECTION("keyframed strength: preview == export whatever the block sizes") {
        Curve curve;
        curve.keys = {{ms(0), 0.0}, {ms(400), 1.0}};
        addTrack(p, {clipWith(p, {'A', ms(0), ms(500), ms(0)}, {enhance(p, ParamValue(curve))})});
        const auto out = exported(p, Ticks{}, at(ms(500)));
        CHECK(out[0] == Approx(0.5f).margin(1e-4));                                   // the source at the start
        CHECK(out[at(ms(450)) * 2] == takeAt(p, file, Ticks{}, at(ms(450)), 0));      // the take after the last key
        CHECK(out[at(ms(200)) * 2] == Approx(0.25f + 0.5f * takeAt(p, file, Ticks{}, at(ms(200)), 0)).margin(2e-3));
        Rig rig(p);
        rig.player->play();
        rig.pullFor(500);
        CHECK(std::equal(out.begin(), out.end(), rig.stream.begin()));
    }
}

TEST_CASE("Enhance Speech bypasses: effect off, no take, a missing file, a take the feed cannot read",
          "[playback][enhance]") {
    Project p;
    p.media('A', level(0.5f));
    SECTION("the effect disabled plays the source") {
        recordTake(p, 'A', takeFile("off"), saw());
        addTrack(p, {clipWith(p, {'A', ms(0), ms(200), ms(0)}, {enhance(p, ParamValue(1.0), false)})});
        const auto out = exported(p, Ticks{}, at(ms(200)));
        CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.5f; }));
    }
    SECTION("no take recorded: the source") {
        addTrack(p, {clipWith(p, {'A', ms(0), ms(200), ms(0)}, {enhance(p, ParamValue(1.0))})});
        const auto out = exported(p, Ticks{}, at(ms(200)));
        CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.5f; }));
        CHECK(AudioTakes::of(p.state) == nullptr);
    }
    SECTION("the take's file gone (a cleared cache): left out of the table, the source plays") {
        const auto file = takeFile("gone");
        recordTake(p, 'A', file, saw());
        std::filesystem::remove(file);
        addTrack(p, {clipWith(p, {'A', ms(0), ms(200), ms(0)}, {enhance(p, ParamValue(1.0))})});
        CHECK(AudioTakes::of(p.state) == nullptr);
        const auto out = exported(p, Ticks{}, at(ms(200)));
        CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.5f; }));
    }
    SECTION("a take rendered from another file: stale, the source plays") {
        recordTake(p, 'A', takeFile("stale"), saw());
        MediaItem item = *p.state.findMedia(p.mediaId('A'));
        item.locator.fingerprint = SourceFingerprint{2000, 0, "ffffffffffffffff"};
        item.audioTakes[0].source = SourceFingerprint{1000, 0, "0000000000000000"};
        p.state.media[0] = std::make_shared<const MediaItem>(std::move(item));
        CHECK(AudioTakes::of(p.state) == nullptr);
    }
    SECTION("a take the feed cannot read: counted, the source plays") {
        const auto file = takeFile("unreadable");
        recordTake(p, 'A', file, saw());
        addTrack(p, {clipWith(p, {'A', ms(0), ms(200), ms(0)}, {enhance(p, ParamValue(1.0))})});
        auto table = AudioTakes::of(p.state);
        REQUIRE(table);
        // A take path the feed does not know (as a decoder that fails to open it).
        std::vector<AudioTakes::Take> takes = table->takes();
        takes[0].path += ".unknown";
        OfflineMixdownOptions options;
        options.renderer.edgeFade = Ticks{};
        options.takes = std::make_shared<const AudioTakes>(takes);
        OfflineMixdown mix(p.state.sequences.front(), 2, 48000, Ticks{}, *p.feed, options);
        std::vector<float> out(at(ms(200)) * 2);
        mix.render(out);
        CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.5f; }));
        CHECK(mix.stats().takeErrors > 0);
    }
}

TEST_CASE("Enhance Speech then redactions: a bleep replaces the enhanced sound exactly, never enhanced itself",
          "[playback][enhance][redaction]") {
    Project p;
    p.media('A', level(0.5f));
    const auto file = takeFile("redacted");
    recordTake(p, 'A', file, saw());
    addTrack(p, {clipWith(p, {'A', ms(0), ms(1000), ms(0)}, {enhance(p, ParamValue(1.0))})});
    Redaction r;
    r.media = p.mediaId('A');
    r.source = TimeRange::fromStartEnd(ms(400), ms(600));
    r.audio = RedactionAudio::Bleep;
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);
    const auto out = exported(p, Ticks{}, at(ms(1000)));
    std::size_t wrongInside = 0;
    for (std::size_t k = at(ms(400)); k < at(ms(600)); ++k) {
        wrongInside += out[k * 2] != AudioRedactions::bleepAt(frames(static_cast<std::int64_t>(k))) ? 1 : 0;
    }
    CHECK(wrongInside == 0);
    // Outside the range and its ramps: the take.
    CHECK(out[at(ms(300)) * 2] == takeAt(p, file, Ticks{}, at(ms(300)), 0));
    CHECK(out[at(ms(700)) * 2] == takeAt(p, file, Ticks{}, at(ms(700)), 0));
}

TEST_CASE("Enhance Speech live: recording a take while playing is heard within the lead window; reversed clips play "
          "it backwards",
          "[playback][enhance]") {
    Project p;
    p.media('A', level(0.5f));
    addTrack(p, {clipWith(p, {'A', ms(0), ms(3000), ms(0)}, {enhance(p, ParamValue(1.0))})});
    Rig rig(p);
    rig.player->play();
    rig.pullFor(300);
    CHECK(rig.stream[at(ms(250)) * 2] == 0.5f); // no take yet: the source
    const auto file = takeFile("live");
    recordTake(p, 'A', file, saw());
    REQUIRE(rig.session.commit("record the take", p.state));
    rig.pullFor(700);
    // Within the lead window (the player refills the track from the cutover), the take plays.
    const std::size_t k = at(ms(900));
    CHECK(rig.stream[k * 2] == takeAt(p, file, Ticks{}, k, 0));

    Project q;
    q.media('A', level(0.5f));
    const auto rfile = takeFile("reversed");
    recordTake(q, 'A', rfile, saw());
    Clip c = clipWith(q, {'A', ms(0), ms(300), ms(1000)}, {enhance(q, ParamValue(1.0))});
    c.reversed = true;
    addTrack(q, {c});
    const auto out = exported(q, Ticks{}, at(ms(300)));
    const std::int64_t last = ticksToSamples(ms(1300), k48k) - 1; // the clip's last source sample plays first
    std::size_t wrong = 0;
    for (std::size_t j = 0; j < at(ms(300)); ++j) {
        wrong += out[j * 2] != SyntheticAudioFeed::evaluate(saw(), last - static_cast<std::int64_t>(j), 0, k48k) ? 1 : 0;
    }
    CHECK(wrong == 0);
}
