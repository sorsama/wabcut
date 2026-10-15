// C.9: version history (History.h) - checkpoints that share unchanged chunks, exact loads, names, pruning within a
// budget, crash safety and damage detection, and its speed on a large project.

#include "TimelineFixture.h"

#include <wabcut/model/History.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const std::string& tag) {
    const fs::path p = fs::temp_directory_path() / ("wabcut-c9-history-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

std::vector<std::uint64_t> idsOf(const ProjectHistory& h) {
    std::vector<std::uint64_t> out;
    for (const HistoryRevision& r : h.revisions()) {
        out.push_back(r.id);
    }
    return out;
}

constexpr std::string_view kPicture = R"(
    V2     |..Cccc..|
    V1 tgt |AaaaBbbb|
    A1 tgt |AaaaBbbb|
)";

} // namespace

TEST_CASE("History: checkpoints share unchanged chunks and load back exactly", "[model][history]") {
    Timeline tl(kPicture);
    auto h = ProjectHistory::open(freshDir("dedup"));
    REQUIRE(h);
    const auto r1 = h->checkpoint(tl.state, HistoryKind::Save, {}, "2026-10-12T10:00:00Z");
    REQUIRE(r1);
    CHECK(r1->createdAt == "2026-10-12T10:00:00Z");
    CHECK(r1->clips == 5);
    CHECK(h->objectCount() == 6); // the root, the media list, the sequence and its three tracks
    // Nothing changed: no new revision.
    CHECK(h->checkpoint(tl.state, HistoryKind::Auto)->id == r1->id);
    CHECK(h->revisions().size() == 1);
    // A trim on V1: a new root, sequence and V1; V2, A1 and the media list are shared.
    const ProjectState s2 =
        ok(edit::trimEdge(tl.state, tl.seq, tl.clip("V1@4"), edit::Edge::Tail, -f(1), edit::LinkMode::IgnoreLinks)).state;
    const auto r2 = h->checkpoint(s2, HistoryKind::Auto);
    REQUIRE(r2);
    CHECK(h->objectCount() == 9);
    CHECK(r2->addedBytes < r1->addedBytes);
    CHECK(*h->load(r1->id) == tl.state);
    CHECK(*h->load(r2->id) == s2);
    // A name sticks, across reopening the folder.
    REQUIRE(h->name(r1->id, "Client cut v2"));
    auto again = ProjectHistory::open(h->folder());
    REQUIRE(again);
    REQUIRE(again->revisions().size() == 2);
    CHECK(again->revisions()[0].label == "Client cut v2");
    CHECK(again->revisions()[0].kind == HistoryKind::Save);
    CHECK(*again->load(r2->id) == s2);
    CHECK_FALSE(again->load(99));
    CHECK_FALSE(again->name(99, "x"));
    CHECK(ProjectHistory::folderFor("C:/films/cut.wabcut") == fs::path("C:/films/cut.wabcut.history"));
}

TEST_CASE("History: pruning keeps named and recent revisions, within the size budget", "[model][history]") {
    Timeline tl(kPicture);
    HistoryOptions options;
    options.keepRecent = 3;
    auto h = ProjectHistory::open(freshDir("prune"), options);
    REQUIRE(h);
    ProjectState s = tl.state;
    std::vector<std::uint64_t> ids;
    for (int i = 0; i < 6; ++i) {
        s = ok(edit::addMarker(s, tl.seq, Marker{f(i), Ticks{}, "m" + std::to_string(i)})).state;
        ids.push_back(h->checkpoint(s, HistoryKind::Auto)->id);
        if (i == 1) {
            REQUIRE(h->name(ids[1], "Rough cut"));
        }
    }
    CHECK(idsOf(*h) == std::vector<std::uint64_t>{ids[1], ids[3], ids[4], ids[5]});
    for (const std::uint64_t id : idsOf(*h)) {
        CHECK(h->load(id));
    }
    // Every object is reached by a remaining revision: a prune frees nothing more.
    CHECK(h->prune()->objectsRemoved == 0);
    // Over budget: only the named and the newest stay.
    HistoryOptions tight;
    tight.keepRecent = 10;
    tight.maxBytes = 1;
    auto t = ProjectHistory::open(h->folder(), tight);
    REQUIRE(t);
    const auto report = t->prune();
    REQUIRE(report);
    CHECK(report->revisionsRemoved == 2);
    CHECK(report->bytesFreed > 0);
    CHECK(idsOf(*t) == std::vector<std::uint64_t>{ids[1], ids[5]});
    CHECK(*t->load(ids[5]) == s);
}

TEST_CASE("History: an interrupted checkpoint leaves the previous history intact", "[model][history]") {
    Timeline tl(kPicture);
    const fs::path dir = freshDir("crash");
    {
        auto h = ProjectHistory::open(dir);
        REQUIRE(h->checkpoint(tl.state, HistoryKind::Save));
    }
    const ProjectState s2 =
        ok(edit::trimEdge(tl.state, tl.seq, tl.clip("V1@4"), edit::Edge::Tail, -f(1), edit::LinkMode::IgnoreLinks)).state;
    {
        HistoryOptions crash;
        crash.failBeforeIndexForTesting = true; // objects written, the process "dies" before the index
        auto h = ProjectHistory::open(dir, crash);
        CHECK_FALSE(h->checkpoint(s2, HistoryKind::Auto));
    }
    // What else a dying write could leave behind.
    std::ofstream(dir / "index.json.tmp") << "{ half an ind";
    std::ofstream(dir / "objects" / "ab" / "deadbeef.tmp") << "partial";
    auto h = ProjectHistory::open(dir);
    REQUIRE(h);
    REQUIRE(h->revisions().size() == 1);
    CHECK(*h->load(h->revisions()[0].id) == tl.state);
    CHECK_FALSE(fs::exists(dir / "index.json.tmp"));
    CHECK_FALSE(fs::exists(dir / "objects" / "ab" / "deadbeef.tmp"));
    // The interrupted checkpoint's objects (root, sequence, V1) name no revision: the next prune removes them.
    CHECK(h->prune()->objectsRemoved == 3);
    CHECK(h->objectCount() == 6);
    // Writing it again works.
    REQUIRE(h->checkpoint(s2, HistoryKind::Auto));
    CHECK(*h->load(h->revisions().back().id) == s2);
}

TEST_CASE("History: damaged folders are refused, not trusted", "[model][history]") {
    Timeline tl(kPicture);
    const fs::path dir = freshDir("damage");
    auto h = ProjectHistory::open(dir);
    const auto r = h->checkpoint(tl.state, HistoryKind::Save);
    REQUIRE(r);
    // An object whose contents no longer match its name.
    const fs::path root = dir / "objects" / r->root.substr(0, 2) / r->root;
    {
        std::fstream f(root, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(20);
        f.put('\x7f');
    }
    CHECK_FALSE(h->load(r->id));
    // An index that is not ours, or from a newer version.
    std::ofstream(dir / "index.json") << R"({"format": "something-else", "revisions": []})";
    CHECK_FALSE(ProjectHistory::open(dir));
    std::ofstream(dir / "index.json") << R"({"format": "wabcut-history", "version": 2, "revisions": []})";
    const auto newer = ProjectHistory::open(dir);
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == ErrorCode::VersionMismatch);
    std::ofstream(dir / "index.json") << R"({"format": "wabcut-history", "version": 1, "revisions": [{"id": 1, "root": "../../etc"}]})";
    CHECK_FALSE(ProjectHistory::open(dir));
}

TEST_CASE("History: hostile files cannot make it allocate, overflow ids or recurse (C.10)", "[model][history]") {
    Timeline tl(kPicture);
    const fs::path dir = freshDir("hostile");
    auto h = ProjectHistory::open(dir);
    REQUIRE(h);
    const auto r = h->checkpoint(tl.state, HistoryKind::Save);
    REQUIRE(r);
    // An object whose header claims 1 GiB over a few bytes of data: refused at once, nothing that size allocated (the
    // text grows only as data inflates).
    const fs::path root = dir / "objects" / r->root.substr(0, 2) / r->root;
    {
        std::ofstream f(root, std::ios::binary | std::ios::trunc);
        f.write("WCH1", 4);
        const std::uint64_t claimed = std::uint64_t{1} << 30;
        for (int i = 0; i < 8; ++i) {
            f.put(static_cast<char>((claimed >> (8 * i)) & 0xFF));
        }
        f.write("\x78\x9c\x03\x00\x00\x00\x00\x01", 8); // zlib of nothing
    }
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(h->load(r->id));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    // Revision ids that would overflow, or zero.
    const std::string key(64, 'a');
    std::ofstream(dir / "index.json") << R"({"format": "wabcut-history", "version": 1, "nextRevision": 18446744073709551615, "revisions": []})";
    CHECK_FALSE(ProjectHistory::open(dir));
    std::ofstream(dir / "index.json") << R"({"format": "wabcut-history", "version": 1, "revisions": [{"id": 0, "root": ")" + key + R"("}]})";
    CHECK_FALSE(ProjectHistory::open(dir));
    // An index nested 100,000 deep.
    std::ofstream(dir / "index.json") << R"({"format": "wabcut-history", "version": 1, "revisions": [], "x": )" +
                                             std::string(100'000, '[') + std::string(100'000, ']') + "}";
    CHECK_FALSE(ProjectHistory::open(dir));
}

TEST_CASE("History: 5,000 clips checkpoint, dedup and load fast", "[model][history][perf]") {
    // 10 linked V/A track pairs of 250 ten-frame clips each.
    Timeline tl(R"(
        V1 tgt |A|
        A1 tgt |A|
    )");
    ProjectState s = tl.state;
    auto seq = std::make_shared<Sequence>(*s.findSequence(tl.seq));
    for (int t = 0; t < 10; ++t) {
        auto v = std::make_shared<Track>();
        v->id = s.ids.allocate<TrackId>();
        v->kind = TrackKind::Video;
        auto a = std::make_shared<Track>();
        a->id = s.ids.allocate<TrackId>();
        a->kind = TrackKind::Audio;
        for (int i = 0; i < 250; ++i) {
            const MediaId media = tl.media(static_cast<char>('A' + i % 26));
            const LinkGroupId group = s.ids.allocate<LinkGroupId>();
            for (Track* track : {v.get(), a.get()}) {
                Clip c;
                c.id = s.ids.allocate<ClipId>();
                c.media = media;
                c.stream = {track == v.get() ? StreamKind::Video : StreamKind::Audio, 0};
                c.timelineRange = {f(i * 10), f(10)};
                c.sourceIn = f(i % 40);
                c.linkGroup = group;
                c.setContent(s.findMedia(media)->defaultClipContent());
                track->clips.push_back(std::move(c));
            }
        }
        seq->videoTracks.push_back(std::move(v));
        seq->audioTracks.push_back(std::move(a));
    }
    seq->duration = f(2500);
    s = s.withSequence(seq);
    requireValid(s);

    auto h = ProjectHistory::open(freshDir("perf"));
    REQUIRE(h);
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto t0 = Clock::now();
    const auto first = h->checkpoint(s, HistoryKind::Save);
    const auto t1 = Clock::now();
    REQUIRE(first);
    CHECK(first->clips == 5002);
    // One trim later: only that track, its sequence and the root are written (and only they are serialised).
    const ClipId clip = s.findSequence(tl.seq)->videoTracks[5]->clips[100].id;
    const ProjectState edited = ok(edit::trimEdge(s, tl.seq, clip, edit::Edge::Tail, -f(1), edit::LinkMode::IgnoreLinks)).state;
    const auto t2 = Clock::now();
    const auto second = h->checkpoint(edited, HistoryKind::Auto);
    const auto t3 = Clock::now();
    REQUIRE(second);
    const auto loaded = h->load(second->id);
    const auto t4 = Clock::now();
    REQUIRE(loaded);
    CHECK(*loaded == edited);
    std::printf("history, 5,002 clips: first checkpoint %.1f ms (%llu bytes), next checkpoint %.1f ms (%llu bytes), "
                "load %.1f ms, folder %llu bytes\n",
                ms(t0, t1), static_cast<unsigned long long>(first->addedBytes), ms(t2, t3),
                static_cast<unsigned long long>(second->addedBytes), ms(t3, t4),
                static_cast<unsigned long long>(h->diskBytes()));
    CHECK(second->addedBytes * 5 < first->addedBytes);
    CHECK(ms(t2, t3) < 200.0 * timingSlack());
    CHECK(ms(t0, t1) < 2000.0 * timingSlack());
    CHECK(ms(t3, t4) < 2000.0 * timingSlack());
}
