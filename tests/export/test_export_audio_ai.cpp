// The export's audio wiring end to end (export/src/Mixdown.cpp): the WAV an export writes has the project's redaction
// bleeps at the exact samples (wave-4 test gap: before, only the GPU render test exported a redaction) and the
// enhanced takes clips with Enhance Speech play (A.4), as preview renders them.

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/AudioTakes.h>
#include <wabcut/playback/audio/MediaAudioFeed.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <vector>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

constexpr Rate kRate{30, 1};

ExportSettings wavSettings() {
    auto p = findPreset("audio-wav");
    REQUIRE(p.has_value());
    return p->settings; // 24-bit PCM, 48 kHz stereo
}

MediaId mediaNamed(const TestProject& p, const std::string& name) {
    for (const MediaPtr& m : p.state.media) {
        if (m->name == name || m->locator.path.ends_with(name)) {
            return m->id;
        }
    }
    FAIL("no media " << name);
    return {};
}

/// Only A2 (the sine, timeline 0.125 .. 0.875 s from media 0.25 s) is heard, at -20 dB: the fixture is a 1 kHz sine,
/// which at the timeline's -6 dB would be the bleep itself (1 kHz at -12 dBFS) and hide it.
TestProject sineOnly() {
    TestProject p = threeLayerTimeline(640, 360, kRate, Ticks::fromSeconds(1));
    p.editSequence([](Sequence& s) {
        Track a1 = *s.audioTracks[0];
        a1.muted = true;
        s.audioTracks[0] = std::make_shared<const Track>(std::move(a1));
        Track a2 = *s.audioTracks[1];
        Clip clip = a2.clips[0];
        ClipProperties props = clip.properties();
        props.set(ParamId::VolumeDb, -20.0);
        clip.setProperties(props);
        a2.clips = ClipList{};
        a2.clips.push_back(std::move(clip));
        s.audioTracks[1] = std::make_shared<const Track>(std::move(a2));
    });
    return p;
}

constexpr double kQuantum = 1.0 / 8388608.0; // 24-bit PCM

/// The same mix through playback's render path (MediaAudioFeed + OfflineMixdown), takes and redactions resolved as the
/// player resolves them: what preview plays.
std::vector<float> previewMix(const TestProject& p, std::int64_t frames) {
    EditSession session(p.state);
    playback::MediaAudioFeed feed(session);
    playback::OfflineMixdownOptions options;
    options.redactions = playback::AudioRedactions::of(p.state);
    options.takes = playback::AudioTakes::of(p.state);
    playback::OfflineMixdown mix(p.state.sequences.front(), 2, 48000, Ticks{}, feed, options);
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    mix.render(out);
    return out;
}

} // namespace

TEST_CASE("Audio export writes the redaction bleep at the exact samples (export/src/Mixdown.cpp)",
          "[export][media][redaction]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p = sineOnly();
    const MediaId sine = mediaNamed(p, "sine_48k.wav");
    Redaction r;
    r.media = sine;
    r.source = TimeRange{ms(400), ms(200)}; // media 0.4 .. 0.6 s = timeline 0.275 .. 0.475 s
    r.audio = RedactionAudio::Bleep;
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);

    const auto dir = freshOutputDir("redaction_wav");
    const auto result = ExportJob(makeRequest(p, wavSettings(), dir / "bleeped.wav")).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    const auto audio = decodeAudio(dir / "bleeped.wav");
    REQUIRE(audio.has_value());
    REQUIRE(audio->samples.size() >= 48000u * 2);
    // Inside: exactly the bleep at the media time (independent of the clip's -6 dB), to the 24-bit step.
    std::size_t wrong = 0;
    const std::int64_t tpf = Rate{48000, 1}.ticksPerUnit();
    for (std::int64_t k = 13200 + 1; k < 22800; ++k) { // 0.275 .. 0.475 s
        const Ticks media = ms(250) + Ticks{(k - 6000) * tpf};
        const double expected = playback::AudioRedactions::bleepAt(media);
        for (int c = 0; c < 2; ++c) {
            wrong += std::fabs(audio->samples[static_cast<std::size_t>(k * 2 + c)] - expected) > 2.0 * kQuantum ? 1 : 0;
        }
    }
    CHECK(wrong == 0);
    // Outside the range and its 5 ms ramps: the sine as before, and the whole file is what preview renders.
    const std::vector<float> preview = previewMix(p, 48000);
    std::size_t differ = 0;
    for (std::size_t i = 0; i < preview.size(); ++i) {
        differ += std::fabs(audio->samples[i] - preview[i]) > 2.0 * kQuantum ? 1 : 0;
    }
    CHECK(differ == 0);
    // And the bleep really is there: without the redaction the same samples are the sine.
    TestProject plain = sineOnly();
    const std::vector<float> unredacted = previewMix(plain, 48000);
    double apart = 0.0;
    for (std::size_t k = 13300; k < 22700; ++k) {
        apart = std::max(apart, static_cast<double>(std::fabs(unredacted[k * 2] - audio->samples[k * 2])));
    }
    CHECK(apart > 0.1);
}

TEST_CASE("Audio export plays the enhanced take of clips with Enhance Speech, as preview does (A.4)",
          "[export][media][enhance]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    TestProject p = sineOnly();
    const MediaId sine = mediaNamed(p, "sine_48k.wav");
    // A take of the sine's stream from source time 0: its samples halved and inverted (a float WAV).
    const auto dir = freshOutputDir("enhance_wav");
    const auto takeFile = dir / "sine-enhanced.wav";
    {
        EditSession session(p.state);
        playback::MediaAudioFeed feed(session);
        std::vector<float> samples(48000u * 5 * 2);
        REQUIRE(feed.read(sine, 0, Ticks{}, samples, 2, Rate{48000, 1}));
        for (float& s : samples) {
            s *= -0.5f;
        }
        std::ofstream out(takeFile, std::ios::binary);
        const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        const auto bytes = static_cast<std::uint32_t>(samples.size() * 4);
        out.write("RIFF", 4);
        u32(36 + bytes);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(3);
        u16(2);
        u32(48000);
        u32(48000 * 8);
        u16(8);
        u16(32);
        out.write("data", 4);
        u32(bytes);
        out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(bytes));
    }
    AudioTake take;
    take.stream = 0;
    take.kind = std::string(kEnhanceSpeechTake);
    take.path = pathToUtf8(takeFile);
    take.model = "test@1";
    take.generation = 1;
    auto withTake = edit::setAudioTake(p.state, sine, take);
    REQUIRE(withTake);
    p.state = std::move(*withTake);
    p.editSequence([&](Sequence& s) {
        Track a2 = *s.audioTracks[1];
        Clip clip = a2.clips[0];
        EffectInstance e;
        e.id = p.state.ids.allocate<EffectInstanceId>();
        e.effect = "wabcut.audio.enhanceSpeech";
        e.set("strength", ParamValue(1.0));
        clip.setEffects({e});
        a2.clips = ClipList{};
        a2.clips.push_back(std::move(clip));
        s.audioTracks[1] = std::make_shared<const Track>(std::move(a2));
    });

    const auto result = ExportJob(makeRequest(p, wavSettings(), dir / "enhanced.wav")).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    const auto audio = decodeAudio(dir / "enhanced.wav");
    REQUIRE(audio.has_value());
    const std::vector<float> preview = previewMix(p, 48000);
    std::size_t differ = 0;
    for (std::size_t i = 0; i < preview.size(); ++i) {
        differ += std::fabs(audio->samples[i] - preview[i]) > 2.0 * kQuantum ? 1 : 0;
    }
    CHECK(differ == 0); // export == preview
    // The take plays (not the source): the clip's -20 dB on the inverted, halved sine.
    const std::vector<float> plain = previewMix(sineOnly(), 48000);
    const std::size_t k = 30000; // 0.625 s, inside A2
    CHECK(std::fabs(audio->samples[k * 2] - (-0.5f * plain[k * 2])) < 4.0 * kQuantum + 1e-6);
}
