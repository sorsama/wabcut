// ProjectState: immutability, structural sharing, ids, lookups and invariant validation.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectState.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// Replaces track `label` of the fixture's sequence with `mutate(copy of the track)`.
template <class Fn>
ProjectState mutateTrack(const Timeline& tl, std::string_view label, Fn&& mutate) {
    auto seq = std::make_shared<Sequence>(tl.sequence());
    const TrackId id = tl.track(label);
    for (auto* list : {&seq->videoTracks, &seq->audioTracks}) {
        for (auto& t : *list) {
            if (t->id == id) {
                auto copy = std::make_shared<Track>(*t);
                mutate(*copy);
                t = std::move(copy);
            }
        }
    }
    return tl.state.withSequence(std::move(seq));
}

std::string problems(const ProjectState& state, Validation level = Validation::Strict) {
    const auto valid = validate(state, level);
    return valid ? std::string() : valid.error().message;
}

} // namespace

TEST_CASE("An edit shares every untouched node with the previous revision", "[model][state]") {
    Timeline tl(R"(
        V2 |Cccc|
        V1 |AaBb|
        A1 |AaBb|
    )");
    const ProjectState before = tl.state;
    const std::vector<TrackId> v1{tl.track("V1")};
    const auto r = ok(edit::addEdit(tl.state, tl.seq, f(1), v1));

    const Sequence& oldSeq = *before.findSequence(tl.seq);
    const Sequence& newSeq = *r.state.findSequence(tl.seq);
    CHECK(&oldSeq != &newSeq);
    CHECK(newSeq.videoTracks[1] == oldSeq.videoTracks[1]); // V2 shared
    CHECK(newSeq.audioTracks[0] == oldSeq.audioTracks[0]); // A1 shared
    CHECK(newSeq.videoTracks[0] != oldSeq.videoTracks[0]); // V1 is a new node
    CHECK(r.state.media == before.media);                  // media nodes shared

    // The old revision is untouched.
    CHECK(oldSeq.videoTracks[0]->clips.size() == 2);
    CHECK(newSeq.videoTracks[0]->clips.size() == 3);
    CHECK(before == tl.state);
    CHECK_FALSE(r.state == before);
}

TEST_CASE("Ids are unique across kinds and never reused", "[model][state]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    ProjectState s = tl.state;
    const std::vector<ClipId> first{tl.clip("V1@0")};
    s = ok(edit::deleteClips(s, tl.seq, first)).state;
    s = ok(edit::addEditToAllTracks(s, tl.seq, f(6))).state;

    std::unordered_set<std::uint64_t> seen;
    const auto claim = [&](std::uint64_t id) {
        CHECK(id != 0);
        CHECK(id < s.ids.next);
        CHECK(seen.insert(id).second);
    };
    for (const auto& m : s.media) {
        claim(m->id.value);
    }
    for (const auto& seq : s.sequences) {
        claim(seq->id.value);
        seq->forEachTrack([&](const Track& t) {
            claim(t.id.value);
            for (const Clip& c : t.clips) {
                claim(c.id.value);
                CHECK(c.id != first[0]); // the deleted clip's id did not come back
            }
        });
    }
}

TEST_CASE("Lookups on tracks and sequences", "[model][state]") {
    Timeline tl(R"(
        V1 |..Aaaa..Bb|
        A1 |..........|
    )");
    const Track& v1 = *tl.sequence().videoTracks[0];
    CHECK_FALSE(v1.clipIndexAt(f(1)));
    CHECK(v1.clipIndexAt(f(2)) == 0u);
    CHECK(v1.clipIndexAt(f(5) + Ticks{f(1).value - 1}) == 0u); // last tick of the clip
    CHECK_FALSE(v1.clipIndexAt(f(6)));                         // half-open: the end is outside
    CHECK(v1.firstClipAtOrAfter(f(3)) == 1u);
    CHECK(v1.end() == f(10));

    const Sequence& s = tl.sequence();
    CHECK(s.contentEnd() == f(10));
    const auto loc = s.findClip(tl.clip("V1@8"));
    REQUIRE(loc);
    CHECK(loc->index == 1u);
    CHECK(loc->track == &v1);
    CHECK(s.trackPosition(tl.track("A1"))->kind == TrackKind::Audio);
    CHECK_FALSE(s.findClip(ClipId{987654}));
    CHECK_FALSE(s.markedRange());
    CHECK(tl.state.findMedia(MediaId{987654}) == nullptr);
    CHECK(tl.state.findSequence(SequenceId{987654}) == nullptr);
}

TEST_CASE("validate() reports every broken invariant", "[model][state][validate]") {
    Timeline tl(R"(
        V1 |AaaaBbbb|
        A1 |Aaaa....|
    )",
                {{'B', 4}});
    REQUIRE(problems(tl.state).empty());

    SECTION("overlapping clips") {
        const auto s = mutateTrack(tl, "V1", [](Track& t) { t.clips.mutableAt(1).timelineRange.start = f(3); });
        CHECK(problems(s).find("overlap") != std::string::npos);
    }
    SECTION("unsorted clips") {
        const auto s = mutateTrack(tl, "V1", [](Track& t) { std::swap(t.clips.mutableAt(0), t.clips.mutableAt(1)); });
        CHECK(problems(s).find("out of order") != std::string::npos);
    }
    SECTION("zero-length clip") {
        const auto s = mutateTrack(tl, "A1", [](Track& t) { t.clips.mutableAt(0).timelineRange.duration = {}; });
        CHECK(problems(s).find("no duration") != std::string::npos);
    }
    SECTION("negative start and source In") {
        const auto s = mutateTrack(tl, "A1", [](Track& t) {
            t.clips.mutableAt(0).timelineRange.start = f(-1);
            t.clips.mutableAt(0).sourceIn = f(-1);
        });
        CHECK(problems(s).find("starts before 0") != std::string::npos);
        CHECK(problems(s).find("before its source media") != std::string::npos);
    }
    SECTION("duplicate id and id beyond the generator") {
        const auto s = mutateTrack(tl, "V1", [&](Track& t) {
            t.clips.mutableAt(0).id = ClipId{tl.state.ids.next + 5};
            t.clips.mutableAt(1).id = t.clips[0].id;
        });
        const std::string p = problems(s);
        CHECK(p.find("used twice") != std::string::npos);
        CHECK(p.find("not below the id generator") != std::string::npos);
    }
    SECTION("missing media and wrong stream kind") {
        const auto s = mutateTrack(tl, "V1", [](Track& t) {
            t.clips.mutableAt(0).media = MediaId{999999};
            t.clips.mutableAt(1).stream.kind = StreamKind::Audio;
        });
        const std::string p = problems(s);
        CHECK(p.find("missing media") != std::string::npos);
        CHECK(p.find("wrong kind") != std::string::npos);
    }
    SECTION("a link group with one clip") {
        const auto s = mutateTrack(tl, "V1", [&](Track& t) { t.clips.mutableAt(0).linkGroup = LinkGroupId{tl.state.ids.next - 1}; });
        CHECK(problems(s).find("single clip") != std::string::npos);
    }
    SECTION("playing past the end of the source media is only rejected in Strict mode") {
        const auto s = mutateTrack(tl, "V1", [](Track& t) { t.clips.mutableAt(1).sourceIn = f(1); });
        CHECK(problems(s, Validation::Strict).find("past the end") != std::string::npos);
        CHECK(problems(s, Validation::Structure).empty());
    }
    SECTION("non-finite and out-of-range properties") {
        const auto s = mutateTrack(tl, "V1", [](Track& t) {
            ClipProperties nan;
            nan.set(ParamId::Scale, std::numeric_limits<double>::quiet_NaN());
            t.clips.mutableAt(0).setProperties(nan);
            ClipProperties opaque;
            opaque.set(ParamId::Opacity, 1.5);
            t.clips.mutableAt(1).setProperties(opaque);
        });
        const std::string p = problems(s);
        CHECK(p.find("motion.scale") != std::string::npos);
        CHECK(p.find("opacity.opacity") != std::string::npos);
    }
    SECTION("marks and markers") {
        auto seq = std::make_shared<Sequence>(tl.sequence());
        seq->markIn = f(5);
        seq->markOut = f(5);
        seq->markers = {{f(4), {}, "", "", 0}, {f(2), {}, "", "", 0}};
        const std::string p = problems(tl.state.withSequence(seq));
        CHECK(p.find("In mark at or after its Out mark") != std::string::npos);
        CHECK(p.find("markers out of order") != std::string::npos);
    }
    SECTION("bins: missing parents and cycles") {
        ProjectState s = tl.state;
        const BinId a = s.ids.allocate<BinId>();
        const BinId b = s.ids.allocate<BinId>();
        s.bins = {{a, b, "a"}, {b, a, "b"}, {s.ids.allocate<BinId>(), BinId{999999}, "orphan"}};
        const std::string p = problems(s);
        CHECK(p.find("cycle") != std::string::npos);
        CHECK(p.find("missing parent") != std::string::npos);
    }
    SECTION("wrong track kind and unsupported rates") {
        auto seq = std::make_shared<Sequence>(tl.sequence());
        seq->frameRate = Rate{13, 1};
        auto track = std::make_shared<Track>(*seq->audioTracks[0]);
        track->kind = TrackKind::Video;
        seq->audioTracks[0] = track;
        const std::string p = problems(tl.state.withSequence(seq));
        CHECK(p.find("unsupported frame rate") != std::string::npos);
        CHECK(p.find("wrong kind") != std::string::npos);
    }
}

// AI-12: generated speech (voice-over, dubbing, narration) is generated media of kind "audio".
TEST_CASE("Generated provenance accepts video, image and audio kinds only", "[model][state][validate]") {
    Timeline tl("V1 |Aaaa|");
    const auto withKind = [&](std::string kind) {
        auto media = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media('A')));
        GeneratedProvenance g;
        g.provider = "elevenlabs";
        g.model = "eleven_v4";
        g.promptHash = std::string(64, 'a');
        g.createdAt = "2026-10-12T12:00:00Z";
        g.kind = std::move(kind);
        g.operation = "text-to-speech";
        media->generated = g;
        ProjectState state = tl.state;
        for (MediaPtr& m : state.media) {
            if (m->id == media->id) {
                m = media;
            }
        }
        return problems(state);
    };
    CHECK(withKind("audio").empty());
    CHECK(withKind("video").empty());
    CHECK(withKind("image").empty());
    CHECK(withKind("music").find("generated kind") != std::string::npos);
}

TEST_CASE("isSameRevision compares node identity, operator== compares values", "[model][state]") {
    Timeline tl(R"(
        V1 |Aa|
    )");
    const ProjectState copy = tl.state;
    CHECK(isSameRevision(copy, tl.state));

    // Same values in a new node: equal, but a different revision.
    const ProjectState rebuilt = tl.state.withSequence(std::make_shared<Sequence>(tl.sequence()));
    CHECK(rebuilt == tl.state);
    CHECK_FALSE(isSameRevision(rebuilt, tl.state));
}
