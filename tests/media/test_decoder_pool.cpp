#include "TestMedia.h"

#include <wabcut/media/DecoderPool.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <latch>
#include <mutex>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;
using namespace std::chrono_literals;

namespace {

void requireClips(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (!haveMedia(n)) {
            SKIP("test clip " << n << " was not generated");
        }
    }
}

FrameRequest frameAt(const char* clip, Ticks t, DecodePriority priority = DecodePriority::Preview) {
    FrameRequest r;
    r.path = mediaPath(clip);
    r.time = t;
    r.priority = priority;
    return r;
}

std::int64_t indexOf(const Job<FrameHandle>& job) {
    const auto& result = job.wait();
    REQUIRE(result.has_value());
    return frameIndexOf(*result);
}

} // namespace

TEST_CASE("DecoderPool: sequential requests reuse the session at their cursor", "[media][pool]") {
    requireClips({"h264_1080p60_aac.mp4"});
    DecoderPool pool;
    for (std::int64_t i = 0; i < 30; ++i) {
        CHECK(indexOf(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", framesToTicks(i, kRate60)))) == i);
    }
    DecoderPoolStats s = pool.stats();
    CHECK(s.sessionsCreated == 1);
    CHECK(s.sequentialReuses == 29);
    CHECK(s.completed == 30);

    // A far jump gets its own session (under the cap), so the first one keeps its cursor...
    CHECK(indexOf(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", framesToTicks(500, kRate60)))) == 500);
    CHECK(pool.stats().sessionsCreated == 2);
    // ...and continuing either stream reuses the matching session without opening another.
    CHECK(indexOf(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", framesToTicks(30, kRate60)))) == 30);
    CHECK(indexOf(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", framesToTicks(501, kRate60)))) == 501);
    s = pool.stats();
    CHECK(s.sessionsCreated == 2);
    CHECK(s.sequentialReuses == 31);
}

TEST_CASE("DecoderPool: the session cap is respected", "[media][pool]") {
    requireClips({"h264_1080p60_aac.mp4", "vfr.mp4", "rotated_90.mp4", "prores_422.mov"});
    DecoderPoolConfig config;
    config.maxVideoSessions = 2;
    config.workerThreads = 3;
    DecoderPool pool(config);
    const char* clips[] = {"h264_1080p60_aac.mp4", "vfr.mp4", "rotated_90.mp4", "prores_422.mov"};
    std::vector<Job<FrameHandle>> jobs;
    for (int round = 0; round < 3; ++round) {
        for (const char* clip : clips) {
            jobs.push_back(pool.requestFrame(frameAt(clip, framesToTicks(round * 5, kRate30))));
        }
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const auto& result = jobs[i].wait();
        REQUIRE(result.has_value());
        const int round = static_cast<int>(i / 4);
        const char* clip = clips[i % 4];
        // Frame index at round*5 frames of 30 fps: the 60 fps clip shows frame 2x, ProRes (23.976) its own.
        const std::int64_t expected = std::string_view(clip) == "h264_1080p60_aac.mp4" ? round * 10
                                      : std::string_view(clip) == "prores_422.mov"
                                          ? ticksToFrames(framesToTicks(round * 5, kRate30), kRate23_976)
                                          : round * 5;
        CHECK(frameIndexOf(*result) == expected);
    }
    const DecoderPoolStats s = pool.stats();
    CHECK(s.peakVideoSessions <= 2);
    CHECK(s.videoSessions <= 2);
    CHECK(s.sessionsClosed >= 2);
    CHECK(s.completed == jobs.size());
}

TEST_CASE("DecoderPool: higher priorities are served first, equal ones in order", "[media][pool]") {
    requireClips({"h264_1080p60_aac.mp4", "ramp_48k.wav"});
    // Declared before the pool so they outlive its workers; the latch makes the last callback observable.
    std::mutex mutex;
    std::vector<std::string> order;
    std::latch allRecorded(6);
    const auto record = [&](std::string name) {
        return [&, name](const auto&) {
            {
                std::lock_guard lock(mutex);
                order.push_back(name);
            }
            allRecorded.count_down();
        };
    };
    DecoderPoolConfig config;
    config.workerThreads = 1; // one worker makes the service order observable
    DecoderPool pool(config);
    pool.pause();
    std::vector<Job<FrameHandle>> frames;
    frames.push_back(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", Ticks{0}, DecodePriority::Background),
                                       record("background-1")));
    frames.push_back(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", Ticks{0}, DecodePriority::Prefetch),
                                       record("prefetch")));
    frames.push_back(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", Ticks{0}, DecodePriority::Preview),
                                       record("preview-1")));
    AudioRequest audio;
    audio.path = mediaPath("ramp_48k.wav");
    audio.frames = 480;
    auto audioJob = pool.requestAudio(audio, record("audio"));
    frames.push_back(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", Ticks{0}, DecodePriority::Background),
                                       record("background-2")));
    frames.push_back(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", Ticks{0}, DecodePriority::Preview),
                                       record("preview-2")));

    std::this_thread::sleep_for(50ms);
    CHECK(pool.stats().completed == 0); // paused
    pool.resume();
    for (const auto& j : frames) {
        REQUIRE(j.wait().has_value());
    }
    REQUIRE(audioJob.wait().has_value());
    allRecorded.wait(); // Job::wait() can return before the completion callback has run
    std::lock_guard lock(mutex);
    CHECK(order == std::vector<std::string>{"preview-1", "preview-2", "audio", "prefetch", "background-1",
                                            "background-2"});
}

TEST_CASE("DecoderPool: stale seeks are cancelled by epoch", "[media][pool]") {
    requireClips({"h264_1080p60_aac.mp4"});
    DecoderPool pool;
    pool.pause();

    std::vector<Job<FrameHandle>> old;
    for (int i = 0; i < 3; ++i) {
        FrameRequest r = frameAt("h264_1080p60_aac.mp4", framesToTicks(100 * i, kRate60));
        r.client = 1;
        r.epoch = 1;
        old.push_back(pool.requestFrame(r));
    }
    FrameRequest otherClient = frameAt("h264_1080p60_aac.mp4", framesToTicks(7, kRate60));
    otherClient.client = 2;
    otherClient.epoch = 1;
    auto unaffected = pool.requestFrame(otherClient);
    auto withdrawn = pool.requestFrame(frameAt("h264_1080p60_aac.mp4", framesToTicks(9, kRate60)));

    pool.setEpoch(1, 2); // the user seeked again: everything of epoch 1 is stale
    for (const auto& j : old) {
        REQUIRE(j.isFinished()); // cancelled at once, without decoding
        REQUIRE_FALSE(j.wait().has_value());
        CHECK(j.wait().error().code == ErrorCode::Cancelled);
    }
    CHECK(pool.epoch(1) == 2);

    FrameRequest late = frameAt("h264_1080p60_aac.mp4", framesToTicks(1, kRate60));
    late.client = 1;
    late.epoch = 1; // submitted after the seek with the old epoch
    auto rejected = pool.requestFrame(late);
    REQUIRE(rejected.isFinished());
    CHECK(rejected.wait().error().code == ErrorCode::Cancelled);

    FrameRequest current = frameAt("h264_1080p60_aac.mp4", framesToTicks(42, kRate60));
    current.client = 1;
    current.epoch = 2;
    auto fresh = pool.requestFrame(current);
    withdrawn.cancel(); // Job::cancel() on a queued request

    pool.resume();
    CHECK(indexOf(fresh) == 42);
    CHECK(indexOf(unaffected) == 7); // other clients keep their own epochs
    REQUIRE_FALSE(withdrawn.wait().has_value());
    CHECK(withdrawn.wait().error().code == ErrorCode::Cancelled);
    CHECK(pool.stats().cancelled == 5);
}

TEST_CASE("DecoderPool: software decode threads stay within the global budget", "[media][pool]") {
    requireClips({"h264_1080p60_aac.mp4", "vfr.mp4", "rotated_90.mp4", "prores_422.mov"});
    DecoderPoolConfig config;
    config.maxVideoSessions = 4;
    config.softwareThreadBudget = 8;
    config.workerThreads = 4;
    config.decoderOptions.allowHardware = false;
    DecoderPool pool(config);
    std::vector<Job<FrameHandle>> jobs;
    for (const char* clip : {"h264_1080p60_aac.mp4", "vfr.mp4", "rotated_90.mp4", "prores_422.mov"}) {
        jobs.push_back(pool.requestFrame(frameAt(clip, Ticks{0})));
    }
    for (const auto& j : jobs) {
        REQUIRE(j.wait().has_value());
    }
    const DecoderPoolStats s = pool.stats();
    CHECK(s.videoSessions == 4);
    CHECK(s.softwareThreadBudget == 8);
    CHECK(s.softwareThreadsInUse == 8); // 4 software sessions x a share of 2
    CHECK(s.peakSoftwareThreads <= 8);

    pool.closeIdleSessions();
    CHECK(pool.stats().videoSessions == 0);
    CHECK(pool.stats().softwareThreadsInUse == 0);
}

TEST_CASE("DecoderPool: audio blocks are sample-exact and sequential", "[media][pool]") {
    requireClips({"ramp_48k.wav"});
    DecoderPool pool;
    std::int64_t position = 0;
    for (int block = 0; block < 10; ++block) {
        AudioRequest r;
        r.path = mediaPath("ramp_48k.wav");
        r.time = samplesToTicks(position, kRate48kHz);
        r.frames = 1000;
        const auto& result = pool.requestAudio(r).wait();
        REQUIRE(result.has_value());
        CHECK(result->result.framesFromStream == 1000);
        for (std::int64_t i = 0; i < 1000; i += 97) {
            const std::int64_t n = position + i;
            CHECK(result->interleaved[static_cast<std::size_t>(2 * i)] ==
                  static_cast<float>(static_cast<int>(n % 65536) - 32768) / 32768.0f);
        }
        position += 1000;
    }
    CHECK(pool.stats().audioSessions == 1);
    CHECK(pool.stats().sequentialReuses == 9);

    AudioRequest tail;
    tail.path = mediaPath("ramp_48k.wav");
    tail.time = samplesToTicks(5 * 48000 - 100, kRate48kHz);
    tail.frames = 1000;
    const auto& end = pool.requestAudio(tail).wait();
    REQUIRE(end.has_value());
    CHECK(end->result.framesFromStream == 100);
    CHECK(end->result.endOfStream);

    AudioRequest empty;
    empty.path = mediaPath("ramp_48k.wav");
    CHECK(pool.requestAudio(empty).wait().error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("DecoderPool: errors and shutdown", "[media][pool]") {
    requireClips({"h264_1080p60_aac.mp4"});
    std::vector<Job<FrameHandle>> pending;
    {
        DecoderPool pool;
        auto missing = pool.requestFrame(frameAt("missing.mp4", Ticks{0}));
        CHECK(missing.wait().error().code == ErrorCode::NotFound);
        CHECK(pool.stats().failed == 1);
        CHECK(pool.stats().videoSessions == 0); // a session that failed to open is not kept

        pool.pause();
        for (int i = 0; i < 4; ++i) {
            pending.push_back(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", framesToTicks(i, kRate60))));
        }
    } // destroying the pool cancels what is still queued
    for (const auto& j : pending) {
        REQUIRE(j.isFinished());
        CHECK(j.wait().error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("DecoderPool: concurrent clients get exact frames", "[media][pool]") {
    requireClips({"h264_1080p60_aac.mp4"});
    DecoderPoolConfig config;
    config.maxVideoSessions = 3;
    config.workerThreads = 3;
    DecoderPool pool(config);
    std::vector<std::thread> clients;
    std::atomic<int> wrong{0};
    for (int c = 0; c < 4; ++c) {
        clients.emplace_back([&, c] {
            for (std::int64_t i = 0; i < 20; ++i) {
                const std::int64_t target = c * 120 + i;
                const auto& result =
                    pool.requestFrame(frameAt("h264_1080p60_aac.mp4", framesToTicks(target, kRate60))).wait();
                if (!result || frameIndexOf(*result) != target) {
                    ++wrong;
                }
            }
        });
    }
    for (auto& t : clients) {
        t.join();
    }
    CHECK(wrong.load() == 0);
    const DecoderPoolStats s = pool.stats();
    CHECK(s.completed == 80);
    CHECK(s.peakVideoSessions <= 3);
}

TEST_CASE("DecoderPool: an owner's jumps seek its own idle session; another owner's session is never shared",
          "[media][pool]") {
    // P.1: a scrub (one layer jumping) used to open a new session for every backward or far jump while under the cap.
    requireClips({"h264_1080p60_aac.mp4"});
    DecoderPool pool;
    const auto at = [](std::int64_t frame, std::uint64_t owner) {
        FrameRequest r = frameAt("h264_1080p60_aac.mp4", framesToTicks(frame, kRate60));
        r.owner = owner;
        return r;
    };
    // Owner 1 plays a little, then scrubs back and forth: one session, seeking, every frame exact.
    for (std::int64_t i = 0; i < 10; ++i) {
        CHECK(indexOf(pool.requestFrame(at(i, 1))) == i);
    }
    for (const std::int64_t f : {400, 120, 550, 10, 300}) {
        CHECK(indexOf(pool.requestFrame(at(f, 1))) == f);
    }
    DecoderPoolStats s = pool.stats();
    CHECK(s.sessionsCreated == 1);
    CHECK(s.seekReuses == 5);

    // Owner 2 (another layer of the same file), just ahead of owner 1's cursor: a session of its own, not owner 1's.
    CHECK(indexOf(pool.requestFrame(at(305, 2))) == 305);
    CHECK(pool.stats().sessionsCreated == 2);
    // Both scrub, interleaved: each seeks its own; nothing new opens; frames stay exact.
    for (std::int64_t k = 0; k < 6; ++k) {
        const std::int64_t a = (50 + 97 * k) % 590;
        const std::int64_t b = (580 - 89 * k) % 590;
        CHECK(indexOf(pool.requestFrame(at(a, 1))) == a);
        CHECK(indexOf(pool.requestFrame(at(b, 2))) == b);
    }
    s = pool.stats();
    CHECK(s.sessionsCreated == 2);
    CHECK(s.seekReuses >= 5 + 6); // owner 2 jumps back every time; owner 1 runs forward within the reuse window

    // Requests without an owner keep the old rules (the first test case above).
    CHECK(indexOf(pool.requestFrame(frameAt("h264_1080p60_aac.mp4", framesToTicks(306, kRate60)))) == 306);
}

TEST_CASE("DecoderPool: a 240 fps source at 60 fps times delivers every fourth frame and counts the skipped ones",
          "[media][pool]") {
    // A.10: a 240 fps game capture in a 60 fps sequence. Every source frame is decoded (P-frames need their
    // predecessors), only the frame each request asks for is delivered; the pool's stats show the difference.
    requireClips({"h264_240fps.mp4"});
    DecoderPoolConfig config;
    config.decoderOptions.allowHardware = false; // software decoding: exact counts, nothing to read back
    DecoderPool pool(config);
    for (std::int64_t n = 0; n < 120; ++n) {
        FrameRequest r = frameAt("h264_240fps.mp4", framesToTicks(n, kRate60));
        r.owner = 1;
        CHECK(indexOf(pool.requestFrame(r)) == 4 * n);
    }
    const DecoderPoolStats s = pool.stats();
    INFO("decoded " << s.videoFramesDecoded << ", delivered " << s.videoFramesDelivered);
    CHECK(s.sessionsCreated == 1);
    CHECK(s.sequentialReuses == 119);
    CHECK(s.videoFramesDelivered == 120);
    CHECK(s.videoFramesDecoded >= 4 * 119 + 1); // every source frame up to the last one delivered
    CHECK(s.videoFramesDecoded <= 480);
    CHECK(s.videoFramesReadBack == 0);
    CHECK(s.videoFramesConverted == 0);
}
