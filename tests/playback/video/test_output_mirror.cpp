// P.2 (E6): a second program output (the full-screen / transmit window) through a playback mirror: the mirror follows
// the leader's transport, seeks, loop and sequence, decodes on its own, and presents the same frames; it never commands
// the transport.

#include "PlaybackTestSupport.h"

#include <wabcut/model/EditSession.h>
#include <wabcut/playback/video/PlaybackController.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::playback::test;
using namespace std::chrono_literals;

namespace {

constexpr const char* kClip = "h264_1080p60_aac.mp4"; // 600 frames, barcode = frame index
constexpr std::int64_t kSourceIn = 100;

/// Waits until `mailbox` presents `target` (of its current epoch).
Presented presentedAt(render::CompositeMailbox& mailbox, Ticks target) {
    Presented last;
    const std::uint64_t epoch = mailbox.epoch();
    consume(mailbox, 5000ms, [&](const std::vector<Presented>& seen) {
        last = seen.back();
        return last.epoch == epoch && last.target == target;
    });
    return last;
}

} // namespace

TEST_CASE("A playback mirror follows the leader's seeks and playback with its own decoding", "[playback][mirror]") {
    if (!haveMedia(kClip)) {
        SKIP("test media missing");
    }
    const TestProject project = makeProject({{kClip, 0, 0, 120, kSourceIn}});
    EditSession session(project.state);
    PlaybackController leader; // zero-copy off in tests either way; the mirror is a CPU-frame controller
    leader.setSession(&session);
    leader.setSequence(project.sequence);
    auto mirror = std::make_unique<PlaybackController>();
    leader.addMirror(mirror.get());
    CHECK(mirror->isMirror());
    CHECK_FALSE(leader.isMirror());
    CHECK(mirror->transport() == leader.transport()); // one clock
    CHECK(mirror->sequence() == project.sequence);

    // A seek on the leader: the mirror shows the same frame.
    const Ticks at = framesToTicks(37, kRate60);
    leader.seek(at);
    CHECK(presentedAt(*leader.mailbox(), at).barcode == kSourceIn + 37);
    CHECK(presentedAt(*mirror->mailbox(), at).barcode == kSourceIn + 37);

    // The loop reaches the mirror too (long enough not to wrap below: test_loop_playback.cpp plays wraps).
    const TimeRange loop = TimeRange::fromStartEnd(framesToTicks(10, kRate60), framesToTicks(100, kRate60));
    leader.setLoop(loop);
    CHECK(mirror->loop() == loop);

    // Playing: the mirror presents frames of the running transport, frame-accurate.
    leader.play();
    const std::vector<Presented> seen = consume(*mirror->mailbox(), 600ms);
    leader.pause();
    int frames = 0;
    for (const Presented& p : seen) {
        if (p.layers == 0) {
            continue;
        }
        ++frames;
        CHECK(p.barcode == kSourceIn + ticksToFrames(p.target, kRate60));
    }
    CHECK(frames >= 10);

    // Removed (the window closed): it no longer follows, and the leader runs on alone.
    leader.removeMirror(mirror.get());
    CHECK_FALSE(mirror->isMirror());
    mirror.reset();
    const Ticks later = framesToTicks(50, kRate60);
    leader.seek(later);
    CHECK(presentedAt(*leader.mailbox(), later).barcode == kSourceIn + 50);
}

TEST_CASE("A mirror going first leaves the leader safe", "[playback][mirror]") {
    PlaybackController leader;
    {
        PlaybackController mirror;
        leader.addMirror(&mirror);
        leader.addMirror(&mirror); // once only
        CHECK(mirror.isMirror());
    } // the mirror's destructor removes itself
    leader.seek(Ticks{kTicksPerSecond}); // starts an epoch: no mirror left to touch
    CHECK(leader.position() == Ticks{kTicksPerSecond});
}
