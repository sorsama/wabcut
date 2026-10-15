// The media-backed analysis on the real render path (OfflineMixdown + libebur128) with synthetic media: loudness of
// clips, tracks and the mix, the analysis cache, normalisation end to end, and speech from the audio level or
// transcript words driving auto-ducking.

#include "CommandHarness.h"

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/LoudnessJobs.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/audio/WavWriter.h>
#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/audio/SyntheticAudioFeed.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

Ticks s(double seconds) { return Ticks{static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond))}; }

playback::SyntheticSignal tone(double dbfs, double hz = 1000.0) {
    playback::SyntheticSignal sig;
    sig.kind = playback::SyntheticSignal::Kind::Sine;
    sig.frequency = hz;
    sig.amplitude = static_cast<float>(std::pow(10.0, dbfs / 20.0));
    return sig;
}

/// A feed that is silent except in the given (source) intervals, where it plays `inside`: speech bursts.
class BurstFeed final : public playback::AudioFeed {
public:
    BurstFeed(std::shared_ptr<playback::SyntheticAudioFeed> base, MediaId media, std::vector<TimeRange> bursts)
        : m_base(std::move(base)), m_media(media), m_bursts(std::move(bursts)) {}
    std::expected<std::size_t, Error> read(MediaId media, int stream, Ticks sourceTime, std::span<float> out, int channels,
                                           Rate rate) override {
        auto n = m_base->read(media, stream, sourceTime, out, channels, rate);
        if (n && media == m_media) {
            const std::int64_t tpf = rate.ticksPerUnit();
            for (std::size_t f = 0; f < *n; ++f) {
                const Ticks t = sourceTime + Ticks{static_cast<std::int64_t>(f) * tpf};
                const bool on = std::any_of(m_bursts.begin(), m_bursts.end(), [&](const TimeRange& b) { return b.contains(t); });
                if (!on) {
                    for (int c = 0; c < channels; ++c) {
                        out[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] *= 0.003f; // -50 dB room tone
                    }
                }
            }
        }
        return n;
    }

private:
    std::shared_ptr<playback::SyntheticAudioFeed> m_base;
    MediaId m_media;
    std::vector<TimeRange> m_bursts;
};

/// 20 s of A on A1 (a -23 dBFS 1 kHz tone) and of M on A2 (-30 dBFS 300 Hz).
std::string longPicture() {
    const std::string a = "A" + std::string(499, 'a');
    const std::string m = "M" + std::string(499, 'm');
    return "A1 dialogue |" + a + "|\nA2 music |" + m + "|";
}

struct Rig {
    Rig() : tl(longPicture()) {
        feed->setSignal(tl.media('A'), 0, tone(-23.0));
        feed->setSignal(tl.media('M'), 0, tone(-30.0, 300.0));
    }
    std::shared_ptr<RenderLoudnessAnalyzer> analyzer(std::shared_ptr<ai::AnalysisCache> cache = nullptr) {
        RenderAnalysisOptions o;
        o.cache = std::move(cache);
        auto f = feed;
        return std::make_shared<RenderLoudnessAnalyzer>([f](const ProjectState&) { return f; }, o);
    }
    Timeline tl;
    std::shared_ptr<playback::SyntheticAudioFeed> feed = std::make_shared<playback::SyntheticAudioFeed>();
};

} // namespace

TEST_CASE("Loudness of clips, tracks and the mix on the real render path", "[audio][edit][analysis]") {
    Rig rig;
    auto analyzer = rig.analyzer();
    const SequenceId seq = rig.tl.seq;
    const ClipId a = rig.tl.clip("A1@0");
    auto measure = [&](const ProjectState& state, const LoudnessTarget& t) {
        const auto r = analyzer->measure(state, seq, t, std::nullopt, nullptr);
        REQUIRE(r);
        return *r;
    };
    // EBU Tech 3341 case 1 on the timeline: a -23 dBFS stereo tone is -23 LUFS.
    CHECK(measure(rig.tl.state, LoudnessTarget::ofClip(a)).integratedLufs == Approx(-23.0).margin(0.1));
    CHECK(measure(rig.tl.state, LoudnessTarget::ofTrack(rig.tl.track("A1"))).integratedLufs == Approx(-23.0).margin(0.1));
    const LoudnessStats music = measure(rig.tl.state, LoudnessTarget::ofTrack(rig.tl.track("A2")));
    CHECK(music.integratedLufs == Approx(-30.0).margin(0.8)); // K-weighting is a fraction of a dB below 0 at 300 Hz
    // The mix is the power sum.
    const double sum = 10.0 * std::log10(std::pow(10.0, -2.3) + std::pow(10.0, music.integratedLufs / 10.0));
    CHECK(measure(rig.tl.state, LoudnessTarget::mix()).integratedLufs == Approx(sum).margin(0.1));
    const double peak = measure(rig.tl.state, LoudnessTarget::mix()).truePeakDbtp; // the two tones' peaks meet at most
    CHECK(peak <= 20.0 * std::log10(0.0708 + 0.0316) + 0.2);
    CHECK(peak > -23.0);

    // Effects count where they apply: a track gain in the track and the mix, not in the clip; a master gain only in
    // the mix; mute in the mix only.
    ProjectState state = test::ok(edit::addEffect(rig.tl.state, seq, edit::EffectTarget::ofTrack(rig.tl.track("A1")),
                                                  "wabcut.audio.gain", {{"gain", ParamValue(-6.0)}}))
                             .state;
    CHECK(measure(state, LoudnessTarget::ofClip(a)).integratedLufs == Approx(-23.0).margin(0.1));
    CHECK(measure(state, LoudnessTarget::ofTrack(rig.tl.track("A1"))).integratedLufs == Approx(-29.0).margin(0.1));
    edit::TrackStateChange mute;
    mute.muted = true;
    state = test::ok(edit::setTrackState(state, seq, rig.tl.track("A2"), mute)).state;
    CHECK(measure(state, LoudnessTarget::mix()).integratedLufs == Approx(-29.0).margin(0.1));
    CHECK(measure(state, LoudnessTarget::ofTrack(rig.tl.track("A2"))).integratedLufs == Approx(music.integratedLufs).margin(0.05));
    state = test::ok(edit::addEffect(state, seq, edit::EffectTarget::masterAudio(), "wabcut.audio.gain", {{"gain", ParamValue(3.0)}})).state;
    CHECK(measure(state, LoudnessTarget::mix()).integratedLufs == Approx(-26.0).margin(0.1));
    // A range.
    const auto part = analyzer->measure(rig.tl.state, seq, LoudnessTarget::ofClip(a), TimeRange{s(5.0), s(3.0)}, nullptr);
    REQUIRE(part);
    CHECK(part->duration == s(3.0));
    CHECK(part->integratedLufs == Approx(-23.0).margin(0.1));
}

TEST_CASE("Loudness results are cached by what changes the audio", "[audio][edit][analysis][cache]") {
    Rig rig;
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio-edit-cache";
    std::filesystem::remove_all(dir);
    auto cache = std::make_shared<ai::AnalysisCache>(dir);
    auto analyzer = rig.analyzer(cache);
    const auto target = LoudnessTarget::ofTrack(rig.tl.track("A1"));
    const auto first = analyzer->measure(rig.tl.state, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(first);
    CHECK(analyzer->renders() == 1);
    // Same audio (an unrelated change: A2's volume, a renamed clip): a hit.
    ProjectState other = rig.tl.state;
    {
        ClipProperties props;
        props.set(ParamId::VolumeDb, -10.0);
        edit::ClipAttributeChange change;
        change.properties = props;
        other = test::ok(edit::setClipAttributes(other, rig.tl.seq, std::vector<ClipId>{rig.tl.clip("A2@0")}, change)).state;
    }
    const auto second = analyzer->measure(other, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(second);
    CHECK(analyzer->cacheHits() == 1);
    CHECK(analyzer->renders() == 1);
    CHECK(second->integratedLufs == first->integratedLufs);
    CHECK(second->truePeakDbtp == first->truePeakDbtp);
    // A fresh analyzer on the same cache directory finds it too.
    CHECK(rig.analyzer(std::make_shared<ai::AnalysisCache>(dir))->measure(rig.tl.state, rig.tl.seq, target, std::nullopt, nullptr));
    // A change to the track's audio: a miss.
    const ProjectState louder = test::ok(edit::addEffect(rig.tl.state, rig.tl.seq, edit::EffectTarget::ofTrack(rig.tl.track("A1")),
                                                         "wabcut.audio.gain", {{"gain", ParamValue(2.0)}}))
                                    .state;
    const auto third = analyzer->measure(louder, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(third);
    CHECK(analyzer->renders() == 2);
    CHECK(third->integratedLufs == Approx(first->integratedLufs + 2.0).margin(0.05));
}

TEST_CASE("audio.normalize end to end: the mix through a compressor to -16 LUFS", "[audio][edit][analysis]") {
    Rig rig;
    ProjectState start = test::ok(edit::addEffect(rig.tl.state, rig.tl.seq, edit::EffectTarget::ofTrack(rig.tl.track("A1")),
                                                  "wabcut.audio.compressor",
                                                  {{"threshold", ParamValue(-26.0)}, {"ratio", ParamValue(3.0)}}))
                              .state;
    rig.tl.state = start;
    auto services = std::make_shared<AudioServices>(rig.analyzer(), nullptr);
    CommandHarness h(rig.tl, command::EnginePolicy{command::CapabilitySet::all(), 16, services}, ai::edit::editingCommands());
    const auto r = h.ok("audio.normalize", {{"preset", "podcast"}, {"limit", true}, {"maxPasses", 6}});
    const Json& result = r.data["results"][0];
    INFO(result.dump());
    CHECK(result["after"]["integratedLufs"].get<double>() == Approx(-16.0).margin(0.1));
    CHECK(result["after"]["truePeakDbtp"].get<double>() <= -1.0 + 0.02);
    // Measured independently, the committed project is at -16.
    const auto check = rig.analyzer()->measure(*h.state(), rig.tl.seq, LoudnessTarget::mix(), std::nullopt, nullptr);
    REQUIRE(check);
    CHECK(check->integratedLufs == Approx(-16.0).margin(0.1));
}

TEST_CASE("Speech from the audio level, and from transcript words, drives auto-ducking", "[audio][edit][analysis][duck]") {
    Rig rig;
    // Speech bursts on A1's media at 2-4 s, 4.5-6 s and 12-14 s, room tone elsewhere.
    const std::vector<TimeRange> bursts{TimeRange::fromStartEnd(s(2.0), s(4.0)), TimeRange::fromStartEnd(s(4.5), s(6.0)),
                                        TimeRange::fromStartEnd(s(12.0), s(14.0))};
    auto feed = std::make_shared<BurstFeed>(rig.feed, rig.tl.media('A'), bursts);
    FeedFactory feeds = [feed](const ProjectState&) { return feed; };

    SECTION("energy") {
        RenderSpeechDetector detector(feeds, nullptr);
        const std::vector<TrackId> dialogue{rig.tl.track("A1")};
        const auto speech = detector.speech(rig.tl.state, rig.tl.seq, dialogue, SpeechSource::Auto, std::nullopt, nullptr);
        REQUIRE(speech);
        // 4 s to 4.5 s is shorter than the 250 ms bridge? No (500 ms): three runs, each within a frame of the burst.
        REQUIRE(speech->size() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(std::fabs(((*speech)[i].start - bursts[i].start).toSeconds()) <= 0.011);
            CHECK(std::fabs(((*speech)[i].end() - bursts[i].end()).toSeconds()) <= 0.011);
        }
        // Through the command: music dips under each phrase (the first two bridged), level elsewhere.
        auto services = std::make_shared<AudioServices>(nullptr, std::make_shared<RenderSpeechDetector>(feeds, nullptr));
        CommandHarness h(rig.tl, command::EnginePolicy{command::CapabilitySet::all(), 16, services}, ai::edit::editingCommands());
        const auto r = h.ok("audio.autoDuck", {{"amount", -12}});
        CHECK(r.data["regions"].size() == 2);
        const Clip& music = h.sequence(h.state()).findClip(h.clip("A2@0"))->clip();
        const ParamValue& v = music.properties()[ParamId::VolumeDb];
        CHECK(v.valueAt(music.curveTimeAt(s(1.0))) == 0.0);
        CHECK(v.valueAt(music.curveTimeAt(s(3.0))) == -12.0);
        CHECK(v.valueAt(music.curveTimeAt(s(4.25))) == -12.0); // bridged
        CHECK(v.valueAt(music.curveTimeAt(s(9.0))) == 0.0);
        CHECK(v.valueAt(music.curveTimeAt(s(13.0))) == -12.0);
    }
    SECTION("transcript words win where there is a transcript") {
        auto transcripts = std::make_shared<ai::edit::MapTranscriptProvider>();
        auto t = std::make_shared<ai::Transcript>();
        t->language = "en";
        for (const auto& [from, to] : std::vector<std::pair<double, double>>{{7.0, 7.4}, {7.5, 8.0}, {16.0, 16.6}}) {
            ai::TranscriptWord w;
            w.text = "word";
            w.t0 = s(from);
            w.t1 = s(to);
            t->words.push_back(w);
        }
        ai::TranscriptSegment seg;
        seg.t0 = s(7.0);
        seg.t1 = s(16.6);
        seg.firstWord = 0;
        seg.wordCount = 3;
        t->segments.push_back(seg);
        transcripts->add(rig.tl.media('A'), 0, t);
        RenderSpeechDetector detector(feeds, transcripts);
        const std::vector<TrackId> dialogue{rig.tl.track("A1")};
        const auto speech = detector.speech(rig.tl.state, rig.tl.seq, dialogue, SpeechSource::Auto, std::nullopt, nullptr);
        REQUIRE(speech);
        // The words (7-8 s, 16-16.6 s) replace the level over the transcribed clip.
        REQUIRE(speech->size() == 2);
        CHECK((*speech)[0] == TimeRange::fromStartEnd(s(7.0), s(8.0)));
        CHECK((*speech)[1] == TimeRange::fromStartEnd(s(16.0), s(16.6)));
        const auto energy = detector.speech(rig.tl.state, rig.tl.seq, dialogue, SpeechSource::Energy, std::nullopt, nullptr);
        REQUIRE(energy);
        CHECK(energy->size() == 3);
    }
}

TEST_CASE("Speech levels: floor, hysteresis and bridging", "[audio][edit][analysis]") {
    const Ticks frame{kTicksPerSecond / 100};
    std::vector<double> levels(300, -55.0);
    for (std::size_t i = 50; i < 120; ++i) {
        levels[i] = -20.0;
    }
    for (std::size_t i = 80; i < 90; ++i) {
        levels[i] = -48.0; // a 100 ms dip: bridged
    }
    for (std::size_t i = 200; i < 260; ++i) {
        levels[i] = -25.0 + (i % 2 == 0 ? 3.0 : -3.0);
    }
    const auto ranges = speechFromLevels(levels, Ticks{}, frame, EnergySpeechOptions{});
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0] == TimeRange::fromStartEnd(Ticks{50 * frame.value}, Ticks{120 * frame.value}));
    CHECK(ranges[1] == TimeRange::fromStartEnd(Ticks{200 * frame.value}, Ticks{260 * frame.value}));
    CHECK(speechFromLevels(std::vector<double>(100, -200.0), Ticks{}, frame, EnergySpeechOptions{}).empty());
}

TEST_CASE("Loudness jobs run in the background with progress and cancel", "[audio][edit][analysis][jobs]") {
    Rig rig;
    audio::edit::LoudnessJobs jobs(rig.analyzer());
    auto job = jobs.measure(rig.tl.state, rig.tl.seq, LoudnessTarget::mix());
    const auto& result = job.wait();
    REQUIRE(result);
    CHECK(job.progress() == 1.0f);
    CHECK(result->hasIntegrated());

    // A long programme (an hour of tone), cancelled at once: it stops early with Cancelled.
    test::Timeline longer("A1 |A" + std::string(25 * 3600 - 1, 'a') + "|", {{'A', 25 * 3600}});
    rig.feed->setSignal(longer.media('A'), 0, tone(-20.0));
    auto slow = jobs.measure(longer.state, longer.seq, LoudnessTarget::mix());
    slow.cancel();
    const auto& stopped = slow.wait();
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == ErrorCode::Cancelled);
}

TEST_CASE("Loudness measures what export renders: redaction mutes and bleeps count, and change the cache key",
          "[audio][edit][analysis][redaction]") {
    // Review P2-10: before the fix the measurement ignored redactions (and a cached value outlived a new one).
    Rig rig;
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio-edit-redaction-cache";
    std::filesystem::remove_all(dir);
    auto analyzer = rig.analyzer(std::make_shared<ai::AnalysisCache>(dir));
    const auto target = LoudnessTarget::ofTrack(rig.tl.track("A1"));
    const auto plain = analyzer->measure(rig.tl.state, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(plain);
    CHECK(plain->integratedLufs == Approx(-23.0).margin(0.1));

    // Mute all of A's audio: the track exports silence, so it measures silence.
    Redaction r;
    r.media = rig.tl.media('A');
    r.source = TimeRange::fromStartEnd(Ticks{}, s(30.0));
    r.audio = RedactionAudio::Mute;
    auto muted = edit::addRedaction(rig.tl.state, r);
    REQUIRE(muted);
    const auto silent = analyzer->measure(muted->state, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(silent);
    CHECK(analyzer->renders() == 2); // not the cached value of the unredacted audio
    CHECK_FALSE(silent->hasIntegrated());
    // A redaction of media the track does not play changes nothing (still a hit).
    Redaction other;
    other.media = rig.tl.media('M');
    other.source = TimeRange::fromStartEnd(Ticks{}, s(1.0));
    other.audio = RedactionAudio::Bleep;
    auto both = edit::addRedaction(muted->state, other);
    REQUIRE(both);
    REQUIRE(analyzer->measure(both->state, rig.tl.seq, target, std::nullopt, nullptr));
    CHECK(analyzer->renders() == 2);
    // A bleep is louder than the -23 dBFS tone it replaces.
    r.audio = RedactionAudio::Bleep;
    auto bleeped = edit::addRedaction(rig.tl.state, r);
    REQUIRE(bleeped);
    const auto bleep = analyzer->measure(bleeped->state, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(bleep);
    CHECK(bleep->integratedLufs > plain->integratedLufs + 5.0);
}

TEST_CASE("Loudness cache keys include a clip's time remap curve (review P2-23)", "[audio][edit][analysis][cache]") {
    // Before the fix the key ignored the remap curve: a cached value outlived a new curve.
    Rig rig;
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio-edit-remap-cache";
    std::filesystem::remove_all(dir);
    auto analyzer = rig.analyzer(std::make_shared<ai::AnalysisCache>(dir));
    const auto target = LoudnessTarget::ofTrack(rig.tl.track("A1"));
    const auto plain = analyzer->measure(rig.tl.state, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(plain);
    CHECK(plain->integratedLufs == Approx(-23.0).margin(0.1));
    CHECK(analyzer->renders() == 1);
    // Remap the clip to a hold: no 100 % span is left, so its audio is silent.
    const auto remapped = [&](Curve curve) {
        Sequence seq = *rig.tl.state.findSequence(rig.tl.seq);
        for (TrackPtr& t : seq.audioTracks) {
            if (t->id == rig.tl.track("A1")) {
                Track copy = *t;
                Clip& clip = copy.clips.mutableAt(0);
                ClipContent content = clip.content();
                content.timeRemap = std::move(curve);
                clip.setContent(std::move(content));
                t = std::make_shared<const Track>(std::move(copy));
            }
        }
        return rig.tl.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    };
    const ProjectState hold = remapped(Curve{{Keyframe{Ticks{}, 0.0}, Keyframe{s(20.0), 0.0}}});
    const auto silent = analyzer->measure(hold, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(silent);
    CHECK(analyzer->renders() == 2); // not the cached value of the 1x clip
    CHECK_FALSE(silent->hasIntegrated());
    // The same curve again is a hit; another curve is not.
    REQUIRE(analyzer->measure(hold, rig.tl.seq, target, std::nullopt, nullptr));
    CHECK(analyzer->renders() == 2);
    const ProjectState straight =
        remapped(Curve{{Keyframe{Ticks{}, 0.0}, Keyframe{s(20.0), static_cast<double>(s(20.0).value)}}});
    const auto back = analyzer->measure(straight, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(back);
    CHECK(analyzer->renders() == 3);
    CHECK(back->integratedLufs == Approx(-23.0).margin(0.1));
}

TEST_CASE("Loudness measures a nested clip's mix, and an edit inside the nested sequence changes the cache key (C.8)",
          "[audio][edit][analysis][cache][nested]") {
    Rig rig;
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio-edit-nested-cache";
    std::filesystem::remove_all(dir);
    auto analyzer = rig.analyzer(std::make_shared<ai::AnalysisCache>(dir));
    // Premiere's Nest of A1's clip: A1 now plays a nested clip whose sequence holds the tone.
    const ClipId a = rig.tl.clip("A1@0");
    auto nest = edit::nestClips(rig.tl.state, rig.tl.seq, std::vector<ClipId>{a}, "Dialogue");
    REQUIRE(nest);
    const ProjectState nested = nest->edit.state;
    const auto target = LoudnessTarget::ofTrack(rig.tl.track("A1"));
    const auto plain = analyzer->measure(nested, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(plain);
    CHECK(plain->integratedLufs == Approx(-23.0).margin(0.1));
    CHECK(analyzer->renders() == 1);
    // A gain on the nested sequence's track: the parent did not change, its sound did.
    const Sequence& inner = *nested.findSequence(nest->nested);
    ProjectState quieter = test::ok(edit::addEffect(nested, nest->nested, edit::EffectTarget::ofTrack(inner.audioTracks[0]->id),
                                                    "wabcut.audio.gain", {{"gain", ParamValue(-6.0)}}))
                               .state;
    const auto lower = analyzer->measure(quieter, rig.tl.seq, target, std::nullopt, nullptr);
    REQUIRE(lower);
    CHECK(analyzer->renders() == 2); // not the cached value of the louder nest
    CHECK(lower->integratedLufs == Approx(-29.0).margin(0.1));
    // The same state again is a hit.
    REQUIRE(analyzer->measure(quieter, rig.tl.seq, target, std::nullopt, nullptr));
    CHECK(analyzer->renders() == 2);
}

TEST_CASE("Loudness of a 5.1 master is measured in 5.1: surround channels weigh +1.5 dB (A.9)", "[audio][edit][analysis][channels]") {
    Rig rig;
    auto analyzer = rig.analyzer();
    const SequenceId seq = rig.tl.seq;
    auto measure = [&](const ProjectState& state, const LoudnessTarget& t) {
        const auto r = analyzer->measure(state, seq, t, std::nullopt, nullptr);
        REQUIRE(r);
        return r->integratedLufs;
    };
    const double stereoMix = measure(rig.tl.state, LoudnessTarget::mix());
    ProjectState surround = test::ok(edit::setMasterFormat(rig.tl.state, seq, ChannelFormat::Surround51)).state;
    // Front-centre panners: the tracks play on L and R as before, the same loudness.
    CHECK(measure(surround, LoudnessTarget::mix()) == Approx(stereoMix).margin(0.05));
    // A mono music track: on C at the front, on Ls and Rs (half the power each) behind the listener, +1.5 dB in the 5.1
    // measurement.
    edit::TrackStateChange mono;
    mono.format = ChannelFormat::Mono;
    surround = test::ok(edit::setTrackState(surround, seq, rig.tl.track("A2"), mono)).state;
    const double front = measure(surround, LoudnessTarget::ofTrack(rig.tl.track("A2")));
    edit::TrackStateChange rear;
    rear.surround = SurroundPan{ParamValue(0.0), ParamValue(-1.0), ParamValue(-144.0)};
    surround = test::ok(edit::setTrackState(surround, seq, rig.tl.track("A2"), rear)).state;
    CHECK(measure(surround, LoudnessTarget::ofTrack(rig.tl.track("A2"))) == Approx(front + 10.0 * std::log10(1.41)).margin(0.1));
}

TEST_CASE("Channel levels of a real file: a recording with sound on the left only (A.9)", "[audio][edit][analysis][channels]") {
    const auto dir = std::filesystem::temp_directory_path() / "wabcut-tests" / "audio-channels";
    std::filesystem::create_directories(dir);
    const auto file = dir / "left-only.wav";
    {
        auto wav = audio::WavWriter::create(file, audio::WavFormat{48000, 2, 16});
        REQUIRE(wav);
        std::vector<float> block(2 * 48000);
        for (std::size_t f = 0; f < 48000; ++f) {
            block[2 * f] = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(f) / 48000.0));
            block[2 * f + 1] = 0.0f;
        }
        REQUIRE((*wav)->write(block));
        REQUIRE((*wav)->finish());
    }
    Timeline tl("A1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    ProjectState state = tl.state;
    for (MediaPtr& m : state.media) {
        if (m->id == tl.media('A')) {
            MediaItem copy = *m;
            copy.locator.path = pathToUtf8(file);
            copy.info.path = copy.locator.path;
            copy.info.audio.front().channels = 2;
            copy.info.audio.front().index = 0; // the WAV's only stream
            copy.info.audio.resize(1);
            copy.info.video.clear();
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
    const auto analyzer = makeMediaChannelAnalyzer();
    const auto levels = analyzer->measure(state, *state.findMedia(tl.media('A')), 0, TimeRange{Ticks{}, Ticks::fromSeconds(1)}, nullptr);
    INFO((levels ? std::string() : levels.error().message));
    REQUIRE(levels);
    REQUIRE(levels->size() == 2);
    CHECK((*levels)[0].peakDb == Approx(20.0 * std::log10(0.25)).margin(0.1));
    CHECK((*levels)[0].rmsDb == Approx(20.0 * std::log10(0.25 / std::sqrt(2.0))).margin(0.2));
    CHECK((*levels)[1].peakDb == -144.0);
    // Through the command: channel 2 is reported silent.
    auto services = std::make_shared<AudioServices>(nullptr, nullptr);
    services->channels = analyzer;
    Timeline withFile("A1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    withFile.state = state;
    CommandHarness h(std::move(withFile), command::EnginePolicy{command::CapabilitySet::all(), 8, services}, ai::edit::editingCommands());
    auto r = h.ok("audio.channelLevels", {{"clips", Json::array({h.clip("A1@0").value})}});
    CHECK(r.data["clips"][0]["silentChannels"] == Json::array({2}));
}
