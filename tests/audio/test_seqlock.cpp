#include <wabcut/audio/SeqLock.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace wabcut::audio;

namespace {
struct Payload {
    std::uint64_t a = 0;
    std::uint64_t notA = ~std::uint64_t{0};
    double half = 0.0;
    std::uint64_t pad[5]{};
    std::uint64_t tripleA = 0;
};
} // namespace

TEST_CASE("SeqLock publishes and reads back a struct", "[audio][seqlock]") {
    SeqLock<Payload> lock;
    CHECK(lock.load().a == 0);
    Payload p;
    p.a = 42;
    p.notA = ~42ULL;
    p.half = 21.0;
    p.tripleA = 126;
    lock.store(p);
    const Payload q = lock.load();
    CHECK(q.a == 42);
    CHECK(q.half == 21.0);
    Payload r;
    CHECK(lock.tryLoad(r));
    CHECK(r.tripleA == 126);
    CHECK(lock.version() == 2); // the initial value plus one store
}

TEST_CASE("SeqLock readers never observe a torn write", "[audio][seqlock][stress]") {
    SeqLock<Payload> lock;
    std::atomic<bool> done{false};
    std::atomic<bool> torn{false};
    std::atomic<bool> backwards{false};
    std::atomic<std::uint64_t> successfulTries{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            std::uint64_t last = 0;
            std::uint64_t tries = 0;
            while (!done.load(std::memory_order_relaxed)) {
                Payload p;
                if (r == 0) {
                    if (!lock.tryLoad(p)) {
                        continue;
                    }
                    ++tries;
                } else {
                    p = lock.load();
                }
                if (p.notA != ~p.a || p.tripleA != 3 * p.a || p.half != static_cast<double>(p.a) / 2.0) {
                    torn.store(true);
                }
                if (p.a < last) {
                    backwards.store(true);
                }
                last = p.a;
            }
            // A tryLoad reader may lose every race against a writer that never pauses (seqlocks favour the
            // writer; in the engine, transport writes are rare). With the writer finished it must succeed.
            if (r == 0) {
                Payload p;
                if (lock.tryLoad(p) && p.a == 2'000'000) {
                    ++tries;
                }
            }
            successfulTries.fetch_add(tries);
        });
    }

    for (std::uint64_t i = 1; i <= 2'000'000; ++i) {
        Payload p;
        p.a = i;
        p.notA = ~i;
        p.half = static_cast<double>(i) / 2.0;
        p.tripleA = 3 * i;
        lock.store(p);
    }
    done.store(true);
    for (auto& t : readers) {
        t.join();
    }
    CHECK_FALSE(torn.load());
    CHECK_FALSE(backwards.load());
    CHECK(successfulTries.load() > 0);
    CHECK(lock.load().a == 2'000'000);
}
