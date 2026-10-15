// ClipList: chunked, copy-on-write clip storage. A copy shares every chunk; a change copies only the
// chunks it touches, so a one-clip edit on a 10,000-clip track leaves the other chunks shared.

#include "TimelineFixture.h"

#include <wabcut/model/ClipList.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

Clip makeClip(std::uint64_t id, std::int64_t start) {
    Clip c;
    c.id = ClipId{id};
    c.timelineRange = {Ticks{start}, Ticks{10}};
    return c;
}

std::vector<ClipId> ids(const ClipList& list) {
    std::vector<ClipId> out;
    for (const Clip& c : list) {
        out.push_back(c.id);
    }
    return out;
}

std::vector<ClipId> ids(const std::vector<Clip>& list) {
    std::vector<ClipId> out;
    for (const Clip& c : list) {
        out.push_back(c.id);
    }
    return out;
}

/// Chunks are never empty or oversized, and their start indices add up.
void requireWellFormed(const ClipList& list) {
    std::size_t total = 0;
    for (std::size_t c = 0; c < list.chunkCount(); ++c) {
        REQUIRE(list.chunkSize(c) > 0);
        REQUIRE(list.chunkSize(c) <= ClipList::kChunkMax);
        REQUIRE(list.chunkStart(c) == total);
        total += list.chunkSize(c);
    }
    REQUIRE(total == list.size());
}

std::size_t sharedChunks(const ClipList& a, const ClipList& b) {
    std::size_t shared = 0;
    for (std::size_t i = 0; i < a.chunkCount(); ++i) {
        for (std::size_t j = 0; j < b.chunkCount(); ++j) {
            if (a.chunkIdentity(i) == b.chunkIdentity(j)) {
                ++shared;
            }
        }
    }
    return shared;
}

/// A sequence with one video track of `count` two-frame clips.
Timeline longTrack(std::size_t count) {
    Timeline tl(R"(
        V1 tgt |.|
        A1     |.|
    )");
    auto seq = std::make_shared<Sequence>(tl.sequence());
    auto v1 = std::make_shared<Track>(*seq->videoTracks[0]);
    const MediaItem& a = *tl.state.findMedia(tl.media('A'));
    for (std::size_t i = 0; i < count; ++i) {
        Clip c;
        c.id = tl.state.ids.allocate<ClipId>();
        c.media = a.id;
        c.timelineRange = {f(2 * static_cast<std::int64_t>(i)), f(2)};
        c.sourceIn = f(static_cast<std::int64_t>(i % 500));
        c.setContent(a.defaultClipContent());
        v1->clips.push_back(std::move(c));
    }
    seq->videoTracks[0] = v1;
    seq->duration = seq->contentEnd();
    const IdGenerator ids = tl.state.ids;
    tl.state = tl.state.withSequence(seq);
    tl.state.ids = ids;
    requireValid(tl.state);
    return tl;
}

} // namespace

TEST_CASE("ClipList behaves like a vector under random edits", "[model][cliplist]") {
    std::mt19937 rng(20261011);
    std::vector<Clip> reference;
    ClipList list;
    std::uint64_t nextId = 1;
    const auto randomIndex = [&](std::size_t bound) {
        return std::uniform_int_distribution<std::size_t>(0, bound)(rng);
    };
    for (int step = 0; step < 4000; ++step) {
        const int op = std::uniform_int_distribution<int>(0, 9)(rng);
        if (op <= 2 || reference.empty()) {
            const Clip c = makeClip(nextId++, static_cast<std::int64_t>(step));
            list.push_back(c);
            reference.push_back(c);
        } else if (op <= 5) {
            const std::size_t at = randomIndex(reference.size());
            const Clip c = makeClip(nextId++, static_cast<std::int64_t>(step));
            list.insert(at, c);
            reference.insert(reference.begin() + static_cast<std::ptrdiff_t>(at), c);
        } else if (op == 6) {
            const std::size_t first = randomIndex(reference.size() - 1);
            const std::size_t last = std::min(reference.size(), first + randomIndex(40));
            list.erase(first, last);
            reference.erase(reference.begin() + static_cast<std::ptrdiff_t>(first),
                            reference.begin() + static_cast<std::ptrdiff_t>(last));
        } else if (op == 7) {
            std::vector<std::size_t> picked;
            for (std::size_t i = 0; i < reference.size(); ++i) {
                if (std::uniform_int_distribution<int>(0, 30)(rng) == 0) {
                    picked.push_back(i);
                }
            }
            const std::vector<Clip> taken = list.extract(picked);
            REQUIRE(taken.size() == picked.size());
            for (std::size_t k = picked.size(); k-- > 0;) {
                REQUIRE(taken[k].id == reference[picked[k]].id);
                reference.erase(reference.begin() + static_cast<std::ptrdiff_t>(picked[k]));
            }
        } else if (op == 8) {
            const std::size_t i = randomIndex(reference.size() - 1);
            list.mutableAt(i).sourceIn = Ticks{step};
            reference[i].sourceIn = Ticks{step};
        } else {
            const std::size_t first = randomIndex(reference.size() - 1);
            list.mutateRange(first, reference.size(), [&](Clip& c) { c.timelineRange.start += Ticks{3}; });
            for (std::size_t i = first; i < reference.size(); ++i) {
                reference[i].timelineRange.start += Ticks{3};
            }
        }
        if (step % 50 == 0) {
            requireWellFormed(list);
            REQUIRE(list.size() == reference.size());
            REQUIRE(ids(list) == ids(reference));
        }
    }
    requireWellFormed(list);
    REQUIRE(list.toVector() == reference);
    for (std::size_t i = 0; i < reference.size(); i += 7) {
        REQUIRE(list[i] == reference[i]);
        REQUIRE(*list.iteratorAt(i) == reference[i]);
    }
    if (!reference.empty()) {
        CHECK(list.front() == reference.front());
        CHECK(list.back() == reference.back());
    }
}

TEST_CASE("ClipList searches by start time", "[model][cliplist]") {
    ClipList list;
    for (std::int64_t i = 0; i < 1000; ++i) {
        list.push_back(makeClip(static_cast<std::uint64_t>(i + 1), i * 10));
    }
    CHECK(list.lowerBoundStart(Ticks{0}) == 0);
    CHECK(list.lowerBoundStart(Ticks{5}) == 1);
    CHECK(list.lowerBoundStart(Ticks{640}) == 64);   // a chunk boundary
    CHECK(list.upperBoundStart(Ticks{640}) == 65);
    CHECK(list.lowerBoundStart(Ticks{9990}) == 999);
    CHECK(list.lowerBoundStart(Ticks{9991}) == 1000);
    CHECK(list.upperBoundStart(Ticks{-1}) == 0);
    CHECK(ClipList{}.lowerBoundStart(Ticks{0}) == 0);
}

TEST_CASE("Copies share chunks until a change copies just the chunks it touches", "[model][cliplist][sharing]") {
    ClipList original;
    for (std::int64_t i = 0; i < 1000; ++i) {
        original.push_back(makeClip(static_cast<std::uint64_t>(i + 1), i * 10));
    }
    ClipList copy = original;
    CHECK(sharedChunks(original, copy) == original.chunkCount());

    copy.mutableAt(500).sourceIn = Ticks{1};
    CHECK(sharedChunks(original, copy) == original.chunkCount() - 1);
    CHECK(original[500].sourceIn == Ticks{}); // the original is untouched

    copy.mutableAt(501).sourceIn = Ticks{2}; // same chunk, now owned: no further copy
    CHECK(sharedChunks(original, copy) == original.chunkCount() - 1);

    ClipList tail = original;
    tail.mutateRange(900, tail.size(), [](Clip& c) { c.timelineRange.start += Ticks{5}; });
    const std::size_t touched = original.chunkCount() - sharedChunks(original, tail);
    CHECK(touched == (original.size() - 1) / ClipList::kChunkTarget - 900 / ClipList::kChunkTarget + 1);
}

TEST_CASE("A one-clip edit on a 10,000-clip track shares every other chunk", "[model][cliplist][sharing]") {
    Timeline tl = longTrack(10'000);
    const ClipList& before = tl.sequence().videoTracks[0]->clips;
    const ClipId middle = before[5'000].id;
    const auto shared = [&](const ProjectState& after) {
        return sharedChunks(before, after.findSequence(tl.seq)->videoTracks[0]->clips);
    };

    SECTION("trim") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, middle, edit::Edge::Tail, f(-1)));
        CHECK(r.state.findSequence(tl.seq)->videoTracks[0]->clips.chunkCount() == before.chunkCount());
        CHECK(shared(r.state) == before.chunkCount() - 1);
    }
    SECTION("slip") {
        const auto r = ok(edit::slip(tl.state, tl.seq, std::vector<ClipId>{middle}, f(1)));
        CHECK(shared(r.state) == before.chunkCount() - 1);
    }
    SECTION("clip attributes") {
        edit::ClipAttributeChange change;
        change.enabled = false;
        const auto r = ok(edit::setClipAttributes(tl.state, tl.seq, std::vector<ClipId>{middle}, change));
        CHECK(shared(r.state) == before.chunkCount() - 1);
    }
    SECTION("ripple delete near the end only rewrites the last chunks") {
        const ClipId nearEnd = before[9'990].id;
        const auto r = ok(edit::rippleDeleteClips(tl.state, tl.seq, std::vector<ClipId>{nearEnd}));
        // The last chunk changes and, now undersized, merges into the one before it.
        CHECK(shared(r.state) >= before.chunkCount() - 2);
    }
    SECTION("the other tracks are not copied at all") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, middle, edit::Edge::Tail, f(-1)));
        CHECK(r.state.findSequence(tl.seq)->audioTracks[0] == tl.sequence().audioTracks[0]);
    }
}
