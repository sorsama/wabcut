// PlaybackController + Scheduler end to end over real test media (decoders on worker threads, hardware when
// present). A consumer thread plays the render thread's part. Every presented picture's burnt-in frame number
// is checked against the sequence time it claims to show, so these tests prove frame accuracy, not just
// liveness.

#include "PlaybackTestSupport.h"

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/playback/video/PlaybackController.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::playback::test;
using namespace std::chrono_literals;

namespace {

constexpr const char* kClip = "h264_1080p60_aac.mp4"; // 600 frames, barcode = frame index
constexpr std::int64_t kSourceIn = 100;               // the clip starts at source frame 100

std::int64_t frameOf(Ticks t) {
    return ticksToFrames(t, kRate60);
}

/// The source frame a sequence time shows for the standard single-clip project.
std::int64_t expectedBarcode(Ticks target) {
    return kSourceIn + frameOf(target);
}

struct Fixture {
    TestProject project;
    EditSession session;
    PlaybackController controller;

    explicit Fixture(std::vector<ClipSpec> clips = {{kClip, 0, 0, 400, kSourceIn}},
                     PlaybackOptions options = {})
        : project(makeProject(clips)), session(project.state), controller(options) {
        controller.setSession(&session);
        controller.setSequence(project.sequence);
    }

    /// Waits until a picture of the current epoch showing `target` was presented.
    Presented waitFor(Ticks target, std::chrono::milliseconds timeout = 5000ms) {
        Presented last;
        const std::uint64_t epoch = controller.mailbox()->epoch();
        consume(*controller.mailbox(), timeout, [&](const std::vector<Presented>& seen) {
            last = seen.back();
            return last.epoch == epoch && last.target == target;
        });
        return last;
    }
};

bool skipWithoutMedia() {
    return !haveMedia(kClip);
}

} // namespace

TEST_CASE("A paused seek presents the exact frame", "[playback][scheduler]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f;
    f.controller.seek(framesToTicks(37, kRate60));
    const Presented p = f.waitFor(framesToTicks(37, kRate60));
    REQUIRE(p.target == framesToTicks(37, kRate60));
    CHECK(p.barcode == 137);
    CHECK(p.layers == 1);
    const PlaybackStats s = f.controller.stats();
    INFO("decode backends: " << s.decodeBackends);
    CHECK(s.activeSessions == 1);
    CHECK_FALSE(s.decodeBackends.empty());
}

TEST_CASE("stepFrames moves exactly one frame at a time", "[playback][scheduler]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f;
    f.controller.seek(framesToTicks(37, kRate60));
    REQUIRE(f.waitFor(framesToTicks(37, kRate60)).barcode == 137);
    f.controller.stepFrames(1);
    CHECK(f.waitFor(framesToTicks(38, kRate60)).barcode == 138);
    f.controller.stepFrames(-2);
    CHECK(f.waitFor(framesToTicks(36, kRate60)).barcode == 136);
    f.controller.stepFrames(10);
    CHECK(f.waitFor(framesToTicks(46, kRate60)).barcode == 146);
    CHECK_FALSE(f.controller.isPlaying());
}

TEST_CASE("The paused picture keeps its decoder past the idle timeout, so the next step does not reopen it",
          "[playback][scheduler]") {
    // P.1: while a still is shown no new frame is asked for; its session used to be closed after sessionIdleMs, and
    // the next step, scrub or play opened the file and decoded from the keyframe again.
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    PlaybackOptions options;
    options.scheduler.sessionIdleMs = 150;
    Fixture f({{kClip, 0, 0, 400, kSourceIn}}, options);
    f.controller.seek(framesToTicks(37, kRate60));
    REQUIRE(f.waitFor(framesToTicks(37, kRate60)).barcode == 137);
    std::this_thread::sleep_for(600ms); // four idle timeouts, paused on that frame
    CHECK(f.controller.stats().activeSessions == 1);
    f.controller.stepFrames(1);
    CHECK(f.waitFor(framesToTicks(38, kRate60)).barcode == 138);
}

TEST_CASE("1x playback presents consecutive frames on the transport clock", "[playback][scheduler]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f;
    f.controller.seek(framesToTicks(10, kRate60));
    REQUIRE(f.waitFor(framesToTicks(10, kRate60)).barcode == 110);
    f.controller.play();
    const std::uint64_t epoch = f.controller.mailbox()->epoch();
    const auto seen = consume(*f.controller.mailbox(), 700ms);
    f.controller.pause();
    std::vector<Presented> played;
    for (const Presented& p : seen) {
        if (p.epoch == epoch) {
            played.push_back(p);
        }
    }
    const PlaybackStats s = f.controller.stats();
    INFO("presented " << played.size() << ", duplicated " << s.duplicated << ", dropped " << s.dropped
                      << ", late " << s.late << ", skipped " << s.skippedLate << ", backends "
                      << s.decodeBackends);
    REQUIRE(played.size() >= 15); // ~42 expected in 700 ms; generous for a loaded machine
    for (std::size_t i = 0; i < played.size(); ++i) {
        CHECK(played[i].barcode == expectedBarcode(played[i].target));
        if (i > 0) {
            CHECK(played[i].target > played[i - 1].target);
        }
    }
    // The pictures followed the clock: the last one is within a few frames of the transport position.
    const std::int64_t lastFrame = frameOf(played.back().target);
    const std::int64_t clockFrame = frameOf(f.controller.position());
    CHECK(std::abs(clockFrame - lastFrame) <= 6);
}

TEST_CASE("J/K/L shuttle speeds step by n frames", "[playback][scheduler][perf]") { // real-time decode
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    for (int n : {2, 4, 8}) {
        INFO("speed " << n << "x");
        Fixture f;
        f.controller.seek(framesToTicks(5, kRate60));
        REQUIRE(f.waitFor(framesToTicks(5, kRate60)).barcode == 105);
        REQUIRE(f.controller.setTransportSpeed({n, 1}));
        const std::uint64_t epoch = f.controller.mailbox()->epoch();
        const auto seen = consume(*f.controller.mailbox(), 400ms);
        f.controller.pause();
        std::vector<Presented> played;
        for (const Presented& p : seen) {
            if (p.epoch == epoch) {
                played.push_back(p);
            }
        }
        const PlaybackStats s = f.controller.stats();
        INFO("presented " << played.size() << ", skipped " << s.skippedLate << ", backends "
                          << s.decodeBackends);
        REQUIRE(played.size() >= 4);
        for (std::size_t i = 0; i < played.size(); ++i) {
            CHECK(played[i].barcode == expectedBarcode(played[i].target));
            if (i > 0) {
                const std::int64_t delta = frameOf(played[i].target) - frameOf(played[i - 1].target);
                CHECK(delta > 0);
                CHECK(delta % n == 0); // reduced cadence skips whole steps, never off-grid frames
            }
        }
    }
}

TEST_CASE("Reverse playback presents frames backwards (bounded decode windows)", "[playback][scheduler][perf]") { // real-time decode
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f;
    f.controller.seek(framesToTicks(200, kRate60));
    REQUIRE(f.waitFor(framesToTicks(200, kRate60)).barcode == 300);
    REQUIRE(f.controller.setTransportSpeed({-1, 1}));
    const std::uint64_t epoch = f.controller.mailbox()->epoch();
    const auto seen = consume(*f.controller.mailbox(), 1000ms);
    f.controller.pause();
    std::vector<Presented> played;
    for (const Presented& p : seen) {
        if (p.epoch == epoch) {
            played.push_back(p);
        }
    }
    const PlaybackStats s = f.controller.stats();
    INFO("presented " << played.size() << ", skipped " << s.skippedLate << ", late " << s.late);
    REQUIRE(played.size() >= 8); // reverse decodes windows; generous for a loaded machine
    for (std::size_t i = 0; i < played.size(); ++i) {
        CHECK(played[i].barcode == expectedBarcode(played[i].target));
        if (i > 0) {
            CHECK(played[i].target < played[i - 1].target);
        }
    }
    CHECK(frameOf(played.back().target) < 200);
}

TEST_CASE("Latest seek wins: rapid scrubs end on the last target", "[playback][scheduler]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f;
    for (int i = 0; i < 20; ++i) {
        f.controller.scrub(framesToTicks((i * 37) % 350, kRate60));
        std::this_thread::sleep_for(2ms);
    }
    f.controller.scrub(framesToTicks(250, kRate60));
    const Presented p = f.waitFor(framesToTicks(250, kRate60));
    CHECK(p.target == framesToTicks(250, kRate60));
    CHECK(p.barcode == 350);
    CHECK(p.epoch == f.controller.mailbox()->epoch());
    // Nothing older shows up afterwards.
    const auto after = consume(*f.controller.mailbox(), 150ms);
    for (const Presented& q : after) {
        CHECK(q.epoch == p.epoch);
    }
}

TEST_CASE("Hidden layers are not decoded; visible ones are composited in track order",
          "[playback][scheduler]") {
    if (skipWithoutMedia() || !haveMedia("hevc_main10_420.mp4")) {
        SKIP("test media missing");
    }
    {
        // V2 covers the canvas: V1 is never decoded.
        Fixture f({{kClip, 0, 0, 100, 0}, {kClip, 1, 0, 100, 300}});
        f.controller.seek(framesToTicks(10, kRate60));
        const Presented p = f.waitFor(framesToTicks(10, kRate60));
        CHECK(p.layers == 1);
        CHECK(p.barcode == 310);
        CHECK(f.controller.stats().activeSessions == 1);
    }
    {
        // A 720p picture-in-picture over the 1080p clip: two layers, two sessions, top layer = V2.
        ClipProperties pip;
        pip.set(ParamId::Scale, 0.5);
        pip.set(ParamId::PositionX, 400);
        pip.set(ParamId::PositionY, 200);
        pip.set(ParamId::Opacity, 0.8);
        Fixture f({{kClip, 0, 0, 100, 0}, {"hevc_main10_420.mp4", 1, 0, 60, 0, pip}});
        f.controller.seek(framesToTicks(20, kRate60));
        const Presented p = f.waitFor(framesToTicks(20, kRate60));
        CHECK(p.layers == 2);
        CHECK(p.barcode == 10); // the 30 fps HEVC clip shows its frame 10 at sequence frame 20
        CHECK(f.controller.stats().activeSessions == 2);
    }
}

TEST_CASE("Playback parks on the last frame at the sequence end", "[playback][scheduler]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f({{kClip, 0, 0, 30, kSourceIn}});
    f.controller.seek(framesToTicks(20, kRate60));
    REQUIRE(f.waitFor(framesToTicks(20, kRate60)).barcode == 120);
    f.controller.play();
    const auto start = std::chrono::steady_clock::now();
    while (f.controller.isPlaying() && std::chrono::steady_clock::now() - start < 3s) {
        (void)consume(*f.controller.mailbox(), 50ms);
    }
    CHECK_FALSE(f.controller.isPlaying());
    const Ticks pos = f.controller.position();
    CHECK(pos <= framesToTicks(30, kRate60));
    CHECK(pos >= framesToTicks(28, kRate60));
}

TEST_CASE("With stop-at-end off (a voice-over pass) playback runs past the sequence end; back on, it stops",
          "[playback][scheduler][voiceover]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f({{kClip, 0, 0, 30, kSourceIn}});
    CHECK(f.controller.stopAtEnd());
    f.controller.setStopAtEnd(false);
    CHECK_FALSE(f.controller.stopAtEnd());
    f.controller.seek(framesToTicks(20, kRate60));
    REQUIRE(f.waitFor(framesToTicks(20, kRate60)).barcode == 120);
    f.controller.play();
    const auto start = std::chrono::steady_clock::now();
    while (f.controller.position() < framesToTicks(60, kRate60) && std::chrono::steady_clock::now() - start < 3s) {
        (void)consume(*f.controller.mailbox(), 50ms);
    }
    CHECK(f.controller.isPlaying());
    CHECK(f.controller.position() >= framesToTicks(60, kRate60)); // a second past the end of the 30-frame sequence
    // Restored: the transport, already past the end, stops on the next pass.
    f.controller.setStopAtEnd(true);
    const auto restored = std::chrono::steady_clock::now();
    while (f.controller.isPlaying() && std::chrono::steady_clock::now() - restored < 2s) {
        (void)consume(*f.controller.mailbox(), 50ms);
    }
    CHECK_FALSE(f.controller.isPlaying());
}

TEST_CASE("Invalid speeds are rejected; zero pauses", "[playback][controller]") {
    PlaybackController controller;
    CHECK_FALSE(controller.setTransportSpeed({1, 0}));
    CHECK_FALSE(controller.setTransportSpeed({33, 1}));
    CHECK(controller.setTransportSpeed({-1, 4})); // K-held J: slow reverse
    CHECK(controller.isPlaying());
    CHECK(controller.transportSpeed() == TransportSpeed{-1, 4});
    CHECK(controller.setTransportSpeed({0, 1}));
    CHECK_FALSE(controller.isPlaying());
    controller.togglePlay();
    CHECK(controller.transportSpeed() == TransportSpeed{1, 1});
    controller.togglePlay();
    CHECK_FALSE(controller.isPlaying());
}

TEST_CASE("Video follows the audio engine's master clock", "[playback][scheduler][audio]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    audio::AudioEngine engine;
    audio::AudioDeviceConfig config;
    config.backend = audio::AudioBackend::NullRealtime;
    REQUIRE(engine.start(config).has_value());
    Fixture f;
    f.controller.setTransport(makeEngineTransport(engine));
    f.controller.seek(framesToTicks(60, kRate60));
    REQUIRE(f.waitFor(framesToTicks(60, kRate60)).barcode == 160);
    f.controller.play();
    const std::uint64_t epoch = f.controller.mailbox()->epoch();
    const auto seen = consume(*f.controller.mailbox(), 600ms);
    const Ticks enginePosition = engine.clock();
    f.controller.pause();
    std::vector<Presented> played;
    for (const Presented& p : seen) {
        if (p.epoch == epoch) {
            played.push_back(p);
        }
    }
    INFO("transport " << f.controller.stats().transport << ", presented " << played.size());
    REQUIRE(played.size() >= 10);
    for (const Presented& p : played) {
        CHECK(p.barcode == expectedBarcode(p.target));
    }
    // The last picture is the one the audio clock is at (within display latency + a few frames).
    CHECK(std::abs(frameOf(enginePosition) - frameOf(played.back().target)) <= 6);
    CHECK(f.controller.stats().transport.find("audio engine") != std::string::npos);
    engine.stop();
}

namespace {

/// `state` with clip `id` changed by `change` (a new sequence and track node; everything else shared).
ProjectState withClip(const ProjectState& state, SequenceId sequenceId, ClipId id,
                      const std::function<void(Clip&)>& change) {
    auto seq = std::make_shared<Sequence>(*state.findSequence(sequenceId));
    for (std::vector<TrackPtr>* tracks : {&seq->videoTracks, &seq->audioTracks}) {
        for (TrackPtr& trackPtr : *tracks) {
            if (const auto index = trackPtr->indexOf(id)) {
                Track track = *trackPtr;
                std::vector<Clip> clips(track.clips.begin(), track.clips.end());
                change(clips[*index]);
                track.clips = ClipList(std::move(clips));
                trackPtr = std::make_shared<const Track>(std::move(track));
            }
        }
    }
    return state.withSequence(seq);
}

} // namespace

TEST_CASE("Edits re-render the paused picture only when they touch it", "[playback][scheduler][changes]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    // V1: [0, 100) shows source 100.., [200, 300) shows source 400..
    Fixture f({{kClip, 0, 0, 100, kSourceIn}, {kClip, 0, 200, 100, 400}});
    const ClipId first = f.project.clips[0];
    const ClipId second = f.project.clips[1];
    f.controller.seek(framesToTicks(37, kRate60));
    REQUIRE(f.waitFor(framesToTicks(37, kRate60)).barcode == 137);
    const std::uint64_t published = f.controller.stats().published;

    // An edit elsewhere on the timeline: the still stays, nothing is re-published.
    REQUIRE(f.session
                .commit("dim the second clip", withClip(*f.session.snapshot(), f.project.sequence, second,
                                                        [](Clip& c) {
                                                            ClipProperties p = c.properties();
                                                            p.set(ParamId::Opacity, 0.5);
                                                            c.setProperties(p);
                                                        }))
                .has_value());
    std::this_thread::sleep_for(300ms);
    CHECK(f.controller.stats().published == published);
    CHECK(f.controller.scheduler().stats().invalidations == 0);

    // An edit under the playhead (slip the first clip by 10 frames): the still is evaluated again.
    REQUIRE(
        f.session
            .commit("slip", withClip(*f.session.snapshot(), f.project.sequence, first,
                                     [](Clip& c) { c.sourceIn = c.sourceIn + framesToTicks(10, kRate60); }))
            .has_value());
    Presented p;
    consume(*f.controller.mailbox(), 3000ms, [&](const std::vector<Presented>& seen) {
        p = seen.back();
        return p.barcode == 147;
    });
    CHECK(p.target == framesToTicks(37, kRate60));
    CHECK(p.barcode == 147);
    CHECK(f.controller.scheduler().stats().invalidations >= 1);

    // Undo is just another snapshot change.
    REQUIRE(f.session.undo().has_value());
    consume(*f.controller.mailbox(), 3000ms, [&](const std::vector<Presented>& seen) {
        p = seen.back();
        return p.barcode == 137;
    });
    CHECK(p.barcode == 137);
}

TEST_CASE("A redaction marked in the source re-renders the paused picture (I.6)", "[playback][scheduler][changes][redaction]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f;
    f.controller.seek(framesToTicks(20, kRate60));
    REQUIRE(f.waitFor(framesToTicks(20, kRate60)).barcode == 120);
    const std::uint64_t before = f.controller.scheduler().stats().invalidations;
    // Media frames 110..130 are marked: the picture under the playhead (frame 120) must be evaluated again.
    Redaction r;
    r.media = f.session.snapshot()->media.front()->id;
    r.source = TimeRange::fromStartEnd(framesToTicks(110, kRate60), framesToTicks(130, kRate60));
    r.style = RedactionStyle::Solid;
    ProjectState next = *f.session.snapshot();
    r.id = next.ids.allocate<RedactionId>();
    next.redactions.push_back(r);
    REQUIRE(f.session.commit("redact", next).has_value());
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (f.controller.scheduler().stats().invalidations == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(f.controller.scheduler().stats().invalidations > before);
}

TEST_CASE("Burned-in captions: the paused picture is evaluated again after audio edits and speaker changes (review "
          "wave 5 E5-P2-11)",
          "[playback][scheduler][changes][captions]") {
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f;
    // A caption with its speaker's name over the still, the media's sound on A1 and a bleep in it.
    ProjectState next = *f.session.snapshot();
    const MediaId media = next.media.front()->id;
    const SpeakerId alice = next.ids.allocate<SpeakerId>();
    next.speakers.push_back(Speaker{alice, "Alice", 0x3366FF});
    Sequence seq = *next.findSequence(f.project.sequence);
    CaptionTrack captions;
    captions.id = next.ids.allocate<CaptionTrackId>();
    captions.enabled = true;
    captions.style.showSpeakers = true;
    CaptionCue cue{Ticks{}, framesToTicks(100, kRate60), "hello there"};
    cue.speaker = alice;
    captions.cues = {cue};
    seq.captionTracks = {captions};
    Track audio;
    audio.id = next.ids.allocate<TrackId>();
    audio.kind = TrackKind::Audio;
    Clip sound;
    sound.id = next.ids.allocate<ClipId>();
    sound.media = media;
    sound.stream = StreamRef{StreamKind::Audio, 0};
    sound.timelineRange = TimeRange{Ticks{}, framesToTicks(100, kRate60)};
    sound.sourceIn = framesToTicks(kSourceIn, kRate60);
    audio.clips = ClipList({sound});
    seq.audioTracks.push_back(std::make_shared<const Track>(std::move(audio)));
    next = next.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    Redaction bleep;
    bleep.id = next.ids.allocate<RedactionId>();
    bleep.media = media;
    bleep.source = TimeRange::fromStartEnd(framesToTicks(110, kRate60), framesToTicks(130, kRate60));
    bleep.audio = RedactionAudio::Bleep;
    next.redactions.push_back(bleep);
    REQUIRE(f.session.commit("captions", next).has_value());
    f.controller.scheduler().setCaptions(CaptionSettings{});
    f.controller.seek(framesToTicks(20, kRate60));
    REQUIRE(f.waitFor(framesToTicks(20, kRate60)).barcode == 120);
    const auto evaluatedAgain = [&](std::uint64_t before) {
        const auto deadline = std::chrono::steady_clock::now() + 3000ms;
        while (f.controller.scheduler().stats().invalidations == before && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        return f.controller.scheduler().stats().invalidations > before;
    };

    // The speaker renamed: the name drawn before the words changes.
    std::uint64_t before = f.controller.scheduler().stats().invalidations;
    ProjectState renamed = *f.session.snapshot();
    renamed.speakers.front().name = "Alice Smith";
    REQUIRE(f.session.commit("rename speaker", renamed).has_value());
    CHECK(evaluatedAgain(before)); // the still kept the old name before

    // The audio clip slipped (an audio-only edit): the bleeped speech moves under the caption.
    before = f.controller.scheduler().stats().invalidations;
    const ClipId soundId = sound.id;
    REQUIRE(f.session
                .commit("slip the sound", withClip(*f.session.snapshot(), f.project.sequence, soundId,
                                                   [](Clip& c) { c.sourceIn = c.sourceIn + framesToTicks(50, kRate60); }))
                .has_value());
    CHECK(evaluatedAgain(before)); // the still kept the old caption text before
}

TEST_CASE("Edits ahead of the playhead refresh the lookahead while playing",
          "[playback][scheduler][changes][perf]") { // real-time decode
    if (skipWithoutMedia()) {
        SKIP("test media missing");
    }
    Fixture f({{kClip, 0, 0, 400, kSourceIn}});
    const ClipId clip = f.project.clips[0];
    f.controller.seek(framesToTicks(10, kRate60));
    REQUIRE(f.waitFor(framesToTicks(10, kRate60)).barcode == 110);
    f.controller.play();
    (void)consume(*f.controller.mailbox(), 200ms);
    // Slip the playing clip: frames shown afterwards come from the new source range.
    REQUIRE(
        f.session
            .commit("slip", withClip(*f.session.snapshot(), f.project.sequence, clip,
                                     [](Clip& c) { c.sourceIn = c.sourceIn + framesToTicks(100, kRate60); }))
            .has_value());
    const auto seen = consume(*f.controller.mailbox(), 500ms);
    f.controller.pause();
    REQUIRE(seen.size() >= 5);
    // Once the slipped source shows up, every later picture comes from it (sequence frame + sourceIn + 100);
    // only composites already on their way (the lookahead) may show the old source first.
    const auto isSlipped = [](const Presented& q) {
        return q.barcode == frameOf(q.target) + kSourceIn + 100;
    };
    const auto firstSlipped = std::find_if(seen.begin(), seen.end(), isSlipped);
    REQUIRE(firstSlipped != seen.end());
    CHECK(firstSlipped - seen.begin() <= 8);
    for (auto it = firstSlipped; it != seen.end(); ++it) {
        CHECK(isSlipped(*it));
    }
    CHECK(f.controller.scheduler().stats().invalidations >= 1);
}

TEST_CASE("A 240 fps source in a 60 fps sequence shows every fourth frame and delivers only those",
          "[playback][scheduler]") {
    // A.10: a 240 fps game capture in a 60 fps sequence. The decoder walks every source frame (P-frames need their
    // predecessors); the pool delivers, and the scheduler caches and composites, only the frames the sequence shows,
    // forward and in reverse.
    constexpr const char* kHighRate = "h264_240fps.mp4"; // 480 frames at 240 fps, one GOP, barcode = frame index
    if (!haveMedia(kHighRate)) {
        SKIP("test media missing");
    }
    media::DecoderPoolConfig config;
    config.decoderOptions.allowHardware = false; // exact counts: software decoding reads nothing back
    config.workerThreads = 2;
    const auto pool = std::make_shared<media::DecoderPool>(config);
    PlaybackOptions options;
    options.scheduler.pool = pool;
    Fixture f({{kHighRate, 0, 0, 110, 0}}, options); // source in 0: sequence frame n shows source frame 4n
    const auto playedIn = [](const std::vector<Presented>& seen, std::uint64_t epoch) {
        std::vector<Presented> out;
        for (const Presented& p : seen) {
            if (p.epoch == epoch) {
                out.push_back(p);
            }
        }
        return out;
    };

    f.controller.seek(framesToTicks(10, kRate60));
    CHECK(f.waitFor(framesToTicks(10, kRate60)).barcode == 40);
    f.controller.stepFrames(1);
    CHECK(f.waitFor(framesToTicks(11, kRate60)).barcode == 44);

    // Forward at 1x.
    f.controller.play();
    std::uint64_t epoch = f.controller.mailbox()->epoch();
    std::vector<Presented> played = playedIn(consume(*f.controller.mailbox(), 600ms), epoch);
    f.controller.pause();
    REQUIRE(played.size() >= 8);
    for (const Presented& p : played) {
        INFO("sequence frame " << frameOf(p.target));
        CHECK(p.barcode == 4 * frameOf(p.target));
    }
    const media::DecoderPoolStats forward = pool->stats();
    INFO("forward: decoded " << forward.videoFramesDecoded << ", delivered " << forward.videoFramesDelivered);
    CHECK(forward.videoFramesDelivered >= played.size());
    CHECK(forward.videoFramesDecoded >= 3 * forward.videoFramesDelivered);
    CHECK(forward.videoFramesReadBack == 0);
    CHECK(forward.videoFramesConverted == 0);

    // Reverse at 1x: the windows step by the sequence's frames (1/60 s), not the stream's (1/240 s), so about one
    // frame is delivered per picture instead of four.
    f.controller.seek(framesToTicks(100, kRate60));
    CHECK(f.waitFor(framesToTicks(100, kRate60)).barcode == 400);
    const media::DecoderPoolStats before = pool->stats();
    REQUIRE(f.controller.setTransportSpeed({-1, 1}));
    epoch = f.controller.mailbox()->epoch();
    played = playedIn(consume(*f.controller.mailbox(), 800ms), epoch);
    f.controller.pause();
    REQUIRE(played.size() >= 4);
    for (std::size_t i = 0; i < played.size(); ++i) {
        INFO("sequence frame " << frameOf(played[i].target));
        CHECK(played[i].barcode == 4 * frameOf(played[i].target));
        if (i > 0) {
            CHECK(played[i].target < played[i - 1].target);
        }
    }
    const std::uint64_t delivered = pool->stats().videoFramesDelivered - before.videoFramesDelivered;
    INFO("reverse: presented " << played.size() << ", delivered " << delivered);
    CHECK(delivered <= 2 * played.size() + 2 * 24); // a window of 24 sequence frames, and the next one begun
}
