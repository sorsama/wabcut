// CompositeMailbox: bounded, latest-seek-wins, presentation-time selection, hold/black policy, never
// blocking.

#include <wabcut/render/CompositeMailbox.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <mutex>
#include <thread>

using namespace wabcut;
using namespace wabcut::render;

namespace {

constexpr Ticks kFrame{kTicksPerSecond / 60};

struct ManualClock final : PresentationClock {
    std::atomic<std::int64_t> time{0};
    std::atomic<int> dir{1};
    Ticks presentationTime(std::int64_t) const noexcept override { return Ticks{time.load()}; }
    int direction() const noexcept override { return dir.load(); }
};

CompositeMailbox::Ptr composite(std::uint64_t epoch, std::int64_t frame) {
    auto c = std::make_shared<PreparedComposite>();
    c->seekEpoch = epoch;
    c->targetTime = Ticks{frame * kFrame.value};
    c->frameDuration = kFrame;
    return c;
}

} // namespace

TEST_CASE("Without a clock the newest composite wins and the last one is held", "[render][mailbox]") {
    CompositeMailbox box(4);
    auto sel = box.acquire(0);
    CHECK(sel.composite == nullptr);
    CHECK_FALSE(sel.black);
    box.publish(composite(0, 1));
    box.publish(composite(0, 2));
    sel = box.acquire(0);
    REQUIRE(sel.composite);
    CHECK(sel.composite->targetTime == Ticks{2 * kFrame.value});
    CHECK(sel.changed);
    // Nothing new: hold.
    sel = box.acquire(0);
    REQUIRE(sel.composite);
    CHECK(sel.composite->targetTime == Ticks{2 * kFrame.value});
    CHECK_FALSE(sel.changed);
    const MailboxStats s = box.stats();
    CHECK(s.published == 2);
    CHECK(s.presented == 1);
    CHECK(s.dropped == 1);
}

TEST_CASE("Bounded: a full mailbox drops its oldest composite", "[render][mailbox]") {
    CompositeMailbox box(3);
    for (int i = 0; i < 5; ++i) {
        CHECK(box.publish(composite(0, i)));
    }
    CHECK(box.pendingCount() == 3);
    CHECK(box.stats().dropped == 2);
}

TEST_CASE("Playback presents the composite due at the presentation time", "[render][mailbox]") {
    CompositeMailbox box(8);
    auto clock = std::make_shared<ManualClock>();
    box.setClock(clock);
    for (int i = 0; i < 6; ++i) {
        box.publish(composite(0, i));
    }
    clock->time = 2 * kFrame.value + kFrame.value / 2; // inside frame 2
    auto sel = box.acquire(0);
    REQUIRE(sel.composite);
    CHECK(sel.composite->targetTime == Ticks{2 * kFrame.value});
    CHECK(sel.keepRefreshing);
    CHECK(box.stats().dropped == 2); // frames 0 and 1 were superseded
    CHECK(box.pendingCount() == 3);  // 3..5 wait for their time
    // Same refresh interval again: held (a duplicate while playing).
    sel = box.acquire(0);
    CHECK_FALSE(sel.changed);
    CHECK(box.stats().repeated == 1);
    // Skip ahead past frame 4: frame 3 dropped, 4 shown and counted late? No: 4 is current at 4.5 frames.
    clock->time = 4 * kFrame.value + kFrame.value / 2;
    sel = box.acquire(0);
    CHECK(sel.composite->targetTime == Ticks{4 * kFrame.value});
    CHECK(box.stats().late == 0);
    // Frame 5 shown only after its interval ended: late.
    clock->time = 6 * kFrame.value + 1;
    sel = box.acquire(0);
    CHECK(sel.composite->targetTime == Ticks{5 * kFrame.value});
    CHECK(box.stats().late == 1);
}

TEST_CASE("Reverse playback presents frames in descending order", "[render][mailbox]") {
    CompositeMailbox box(8);
    auto clock = std::make_shared<ManualClock>();
    clock->dir = -1;
    box.setClock(clock);
    for (int i = 10; i >= 6; --i) {
        box.publish(composite(0, i));
    }
    clock->time = 9 * kFrame.value + kFrame.value / 2; // inside frame 9 travelling backwards
    auto sel = box.acquire(0);
    REQUIRE(sel.composite);
    CHECK(sel.composite->targetTime == Ticks{9 * kFrame.value});
    CHECK(box.stats().dropped == 1); // frame 10 already passed
}

TEST_CASE("Latest seek wins; Black seeks blank, HoldPrevious seeks hold", "[render][mailbox]") {
    CompositeMailbox box(8);
    box.publish(composite(0, 1));
    REQUIRE(box.acquire(0).composite);

    // Scrub: the old picture stays until the new epoch delivers; stale composites never show.
    const std::uint64_t scrub = box.beginSeek(SeekDisplay::HoldPrevious);
    CHECK_FALSE(box.publish(composite(0, 2))); // stale epoch rejected
    auto sel = box.acquire(0);
    REQUIRE(sel.composite);
    CHECK(sel.composite->seekEpoch == 0);
    CHECK_FALSE(sel.black);
    box.publish(composite(scrub, 7));
    sel = box.acquire(0);
    CHECK(sel.composite->seekEpoch == scrub);

    // Jump: black until something of the new epoch is decoded.
    const std::uint64_t jump = box.beginSeek(SeekDisplay::Black);
    sel = box.acquire(0);
    CHECK(sel.composite == nullptr);
    CHECK(sel.black);
    CHECK(sel.changed);
    CHECK(box.stats().blackFrames == 1);
    box.publish(composite(jump, 20));
    sel = box.acquire(0);
    REQUIRE(sel.composite);
    CHECK_FALSE(sel.black);
    CHECK(sel.composite->targetTime == Ticks{20 * kFrame.value});
    CHECK(box.stats().stale >= 1);
}

TEST_CASE("Pending composites of an older epoch are discarded by beginSeek", "[render][mailbox]") {
    CompositeMailbox box(8);
    box.publish(composite(0, 1));
    box.publish(composite(0, 2));
    box.beginSeek(SeekDisplay::HoldPrevious);
    CHECK(box.pendingCount() == 0);
    CHECK(box.stats().stale == 2);
    CHECK(box.acquire(0).composite == nullptr);
}

TEST_CASE("acquire() never waits for the mailbox lock", "[render][mailbox]") {
    // Deterministic contention: another thread sits inside acquire() (holding the lock) in a clock that
    // blocks.
    struct BlockingClock final : PresentationClock {
        mutable std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        Ticks presentationTime(std::int64_t) const noexcept override {
            entered = true;
            while (!release.load()) {
                std::this_thread::yield();
            }
            return {};
        }
        int direction() const noexcept override { return 1; }
    };
    CompositeMailbox box(4);
    box.publish(composite(0, 0));
    auto clock = std::make_shared<BlockingClock>();
    box.setClock(clock);
    std::thread holder([&] { (void)box.acquire(0); });
    while (!clock->entered.load()) {
        std::this_thread::yield();
    }
    // The render thread gets an answer at once: the current picture (none yet) and a request to refresh
    // again.
    const auto sel = box.acquire(0);
    CHECK(sel.composite == nullptr);
    CHECK(sel.keepRefreshing);
    CHECK(box.stats().contended == 1);
    clock->release = true;
    holder.join();
    CHECK(box.stats().presented == 1);
}

TEST_CASE("The publish callback fires once per accepted composite", "[render][mailbox]") {
    CompositeMailbox box(4);
    int calls = 0;
    box.setPublishCallback([&] { ++calls; });
    box.publish(composite(0, 1));
    box.beginSeek(SeekDisplay::Black);
    box.publish(composite(0, 2)); // stale
    CHECK(calls == 1);
}
