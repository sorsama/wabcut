// Source-anchored redactions follow the source material, not the Library item they were drawn on (review wave 5
// E5-P1-1 and A5-P1-3; model/RedactionMedia.h): a redaction on a master covers the clips of its subclips (a subclip
// plays the master's file at the master's media times) and the reverse, separated stems share their source's
// redactions (sample-aligned), a remix does not (it rearranges time: the remix command refuses redacted stretches
// instead), and a removed master still anchors what was made from it. Every matching site of the resolver uses it.

#include "TimelineFixture.h"

#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Resolve.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kEmpty = R"(
    V1 tgt |....................|
    A1 tgt |....................|
)";

Redaction redactionOf(MediaId media, const char* name, TimeRange source) {
    Redaction r;
    r.media = media;
    r.name = name;
    r.source = source;
    r.audio = RedactionAudio::Bleep;
    r.region.shape = MaskShape::Rectangle;
    r.region.set("size.x", 0.2);
    r.region.set("size.y", 0.2);
    return r;
}

/// Edits `length` frames of `media` from media frame `in` onto V1 and A1 at timeline frame `at`.
ProjectState placed(const Timeline& tl, const ProjectState& state, MediaId media, std::int64_t in, std::int64_t length,
                    std::int64_t at) {
    edit::SourceClip clip;
    clip.media = media;
    clip.sourceRange = {f(in), f(length)};
    clip.patches = {{{StreamKind::Video, 0}, tl.track("V1")}, {{StreamKind::Audio, 0}, tl.track("A1")}};
    return ok(edit::overwriteEdit(state, tl.seq, clip, f(at))).state;
}

std::vector<RedactionId> redactionsAt(const ProjectState& state, SequenceId seq, StreamKind kind, std::int64_t frame) {
    const auto leaves = resolveSequenceLeaves(state, seq, kind, f(frame));
    return leaves.empty() ? std::vector<RedactionId>{} : leaves.front().redactions;
}

} // namespace

TEST_CASE("Redactions follow the source: a master's cover its subclips' clips, and the reverse (review wave 5 E5-P1-1)",
          "[model][redaction][subclip]") {
    Timeline tl(kEmpty);
    const MediaId master = tl.media('A');
    const auto sub = edit::makeSubclip(tl.state, master, TimeRange{f(2), f(8)}, "A, the good take");
    REQUIRE(sub);
    // A subclip of the subclip still plays the master's file.
    const auto subsub = edit::makeSubclip(sub->state, sub->id, TimeRange{f(3), f(4)}, "A, the best bit");
    REQUIRE(subsub);
    ProjectState s = subsub->state;
    CHECK(redactionRoot(s, master) == master);
    CHECK(redactionRoot(s, sub->id) == master);
    CHECK(redactionRoot(s, subsub->id) == master);

    // A face blurred (and its words bleeped) on the master, at media frames 4..6.
    const auto face = edit::addRedaction(s, redactionOf(master, "Face", TimeRange{f(4), f(2)}));
    REQUIRE(face);
    s = face->state;
    CHECK(redactionsCovering(s, sub->id).size() == 1);
    CHECK(redactionsCovering(s, subsub->id).size() == 1);

    // A clip of the subclip plays media frames 2..10 from timeline 0: covered at timeline 2..4, picture and sound.
    s = placed(tl, s, sub->id, 2, 8, 0);
    CHECK(redactionsAt(s, tl.seq, StreamKind::Video, 1).empty());
    CHECK(redactionsAt(s, tl.seq, StreamKind::Video, 2) == std::vector<RedactionId>{face->id});
    CHECK(redactionsAt(s, tl.seq, StreamKind::Audio, 3) == std::vector<RedactionId>{face->id});
    CHECK(redactionsAt(s, tl.seq, StreamKind::Video, 4).empty());

    // A removed master still anchors its subclips: their clips stay covered (fail closed).
    auto gone = edit::removeMedia(s, master);
    REQUIRE(gone);
    CHECK(gone->findMedia(master) == nullptr);
    CHECK(redactionRoot(*gone, sub->id) == master);
    CHECK(redactionsAt(*gone, tl.seq, StreamKind::Video, 2) == std::vector<RedactionId>{face->id});

    // The reverse: a plate blurred while the Source Monitor shows the subclip covers the master's clips too.
    const auto plate = edit::addRedaction(s, redactionOf(subsub->id, "Plate", TimeRange{f(12), f(3)}));
    REQUIRE(plate);
    ProjectState both = placed(tl, plate->state, master, 10, 6, 12); // media 10..16 at timeline 12..18
    CHECK(redactionsAt(both, tl.seq, StreamKind::Video, 13).empty());                              // media 11
    CHECK(redactionsAt(both, tl.seq, StreamKind::Video, 14) == std::vector<RedactionId>{plate->id}); // media 12
    CHECK(redactionsAt(both, tl.seq, StreamKind::Audio, 16) == std::vector<RedactionId>{plate->id}); // media 14
    CHECK(redactionsAt(both, tl.seq, StreamKind::Audio, 17).empty());                              // media 15
    CHECK(redactionCovers(both, *both.findRedaction(plate->id), master));
}

TEST_CASE("Redactions follow the source: separated stems share it; a remix does not (review wave 5 A5-P1-3)",
          "[model][redaction][stems]") {
    Timeline tl(kEmpty);
    const MediaId source = tl.media('A');
    ProjectState s = tl.state;
    const auto bleep = edit::addRedaction(s, redactionOf(source, "Name", TimeRange{f(5), f(2)}));
    REQUIRE(bleep);
    s = bleep->state;
    const auto derived = [&](const char* operation, const char* part) {
        MediaItem item = *s.findMedia(source);
        item.name = std::string("A (") + part + ")";
        item.locator.path = std::string("C:/stems/a_") + part + ".wav";
        item.subclip.reset();
        item.derived = DerivedProvenance{source, operation, "htdemucs", part, "2026-10-12T00:00:00Z"};
        auto added = edit::addMedia(s, std::move(item));
        REQUIRE(added);
        s = added->state;
        return added->id;
    };
    const MediaId vocals = derived("separateStems", "vocals");
    const MediaId remix = derived("remix", "30 s");
    CHECK(derivationKeepsTime("separateStems"));
    CHECK_FALSE(derivationKeepsTime("remix"));
    CHECK(redactionRoot(s, vocals) == source);
    CHECK(redactionRoot(s, remix) == remix);
    // The vocals stem in place of the original: its clips hear the bleep at the same media times.
    s = placed(tl, s, vocals, 0, 10, 0);
    CHECK(redactionsAt(s, tl.seq, StreamKind::Audio, 5) == std::vector<RedactionId>{bleep->id});
    CHECK(redactionsAt(s, tl.seq, StreamKind::Audio, 7).empty());
    CHECK(redactionsCovering(s, remix).empty());
}
