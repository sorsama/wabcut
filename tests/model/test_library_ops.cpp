// M5k: bins, library metadata and subclips (ProjectOps.h), and the restricted-subclip bounds every edit op takes from
// MediaItem::sourceBounds(): trim, ripple trim, slip, slide, speed, time remap, three-point edits and transitions each
// get one test proving a restricted subclip cannot reach outside its range, while an unrestricted one behaves exactly
// as plain media.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ReviewOps.h>

#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

const MediaItem& item(const ProjectState& state, MediaId id) { return *state.findMedia(id); }

/// A subclip of `media` covering source frames [first, first + frames).
MediaId subclipOf(ProjectState& state, MediaId media, std::int64_t first, std::int64_t frames, bool restricted,
                  std::string name = "Sub") {
    auto made = edit::makeSubclip(state, media, {f(first), f(frames)}, std::move(name), restricted);
    REQUIRE(made);
    state = std::move(made->state);
    return made->id;
}

/// Overwrites `frames` frames of `media` from source frame `in` onto V1 at timeline frame `at`.
ProjectState place(const ProjectState& state, SequenceId seq, MediaId media, std::int64_t in, std::int64_t frames,
                   std::int64_t at) {
    const Sequence& s = *state.findSequence(seq);
    edit::SourceClip source{media, {f(in), f(frames)}, {{{StreamKind::Video, 0}, s.videoTracks.front()->id}}};
    return ok(edit::overwriteEdit(state, seq, source, f(at))).state;
}

std::int64_t frames(Ticks t) { return ticksToFrames(t, kRate); }

} // namespace

// ---- Bins ------------------------------------------------------------------------------------------------------

TEST_CASE("Bins are renamed, nested, moved and deleted with their contents", "[model][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    ProjectState s = tl.state;
    auto footage = edit::addBin(s, "Footage");
    REQUIRE(footage);
    s = footage->state;
    auto day1 = edit::addBin(s, "Day 1", footage->id);
    REQUIRE(day1);
    s = day1->state;

    SECTION("rename") {
        const auto renamed = edit::renameBin(s, day1->id, "Day one");
        REQUIRE(renamed);
        CHECK(renamed->findBin(day1->id)->name == "Day one");
        CHECK(edit::renameBin(s, day1->id, "").error().code == ErrorCode::InvalidArgument);
        CHECK(edit::renameBin(s, BinId{9999}, "x").error().code == ErrorCode::NotFound);
        requireValid(*renamed);
    }
    SECTION("move media, sequences and bins; a bin never into itself") {
        const MediaId b = tl.media('B');
        const auto moved =
            edit::moveToBin(s, day1->id, std::vector<BinId>{}, std::vector<MediaId>{b}, std::vector<SequenceId>{tl.seq});
        REQUIRE(moved);
        CHECK(moved->findMedia(b)->bin == day1->id);
        CHECK(moved->findSequence(tl.seq)->bin == day1->id);
        requireValid(*moved);
        // Moving again is a no-op that shares every node.
        const auto again = edit::moveToBin(*moved, day1->id, std::vector<BinId>{}, std::vector<MediaId>{b},
                                           std::vector<SequenceId>{});
        REQUIRE(again);
        CHECK(isSameRevision(*again, *moved));
        // Footage cannot move into Day 1 (its own child), nor into itself.
        CHECK(edit::moveToBin(s, day1->id, std::vector<BinId>{footage->id}, {}, {}).error().code ==
              ErrorCode::InvalidArgument);
        CHECK(edit::moveToBin(s, footage->id, std::vector<BinId>{footage->id}, {}, {}).error().code ==
              ErrorCode::InvalidArgument);
        // Day 1 up to the root.
        const auto up = edit::moveToBin(s, BinId{}, std::vector<BinId>{day1->id}, {}, {});
        REQUIRE(up);
        CHECK(!up->findBin(day1->id)->parent.isValid());
    }
    SECTION("delete removes sub-bins and their media, unless a sequence outside uses it") {
        const MediaId a = tl.media('A'); // used on V1
        const MediaId c = tl.media('C'); // unused
        auto inBin = edit::moveToBin(s, day1->id, {}, std::vector<MediaId>{c}, {});
        REQUIRE(inBin);
        const auto removed = edit::removeBins(*inBin, std::vector<BinId>{footage->id});
        REQUIRE(removed);
        CHECK(removed->findBin(footage->id) == nullptr);
        CHECK(removed->findBin(day1->id) == nullptr);
        CHECK(removed->findMedia(c) == nullptr);
        requireValid(*removed);

        auto used = edit::moveToBin(s, day1->id, {}, std::vector<MediaId>{a}, {});
        REQUIRE(used);
        const auto refused = edit::removeBins(*used, std::vector<BinId>{footage->id});
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
        CHECK(refused.error().message.find("Fixture") != std::string::npos);
        // With the sequence inside the bin too, everything goes together.
        auto both = edit::moveToBin(*used, day1->id, {}, {}, std::vector<SequenceId>{tl.seq});
        REQUIRE(both);
        const auto all = edit::removeBins(*both, std::vector<BinId>{footage->id});
        REQUIRE(all);
        CHECK(all->findSequence(tl.seq) == nullptr);
        CHECK(all->findMedia(a) == nullptr);
        requireValid(*all);
    }
}

// ---- Metadata --------------------------------------------------------------------------------------------------

TEST_CASE("Media attributes: name, label, comment, description and poster frame", "[model][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa....|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');

    SECTION("rename: new clips take the new name, placed clips keep theirs") {
        edit::MediaAttributeChange change;
        change.name = "Interview";
        const auto renamed = edit::setMediaAttributes(tl.state, std::vector<MediaId>{a}, change);
        REQUIRE(renamed);
        CHECK(item(*renamed, a).name == "Interview");
        CHECK(clipIn(*renamed, tl.seq, "V1@0").name() == "A");
        const ProjectState placed = place(*renamed, tl.seq, a, 0, 4, 4);
        CHECK(clipIn(placed, tl.seq, "V1@4").name() == "Interview");
        CHECK(edit::setMediaAttributes(tl.state, std::vector<MediaId>{a}, {.name = std::string()}).error().code ==
              ErrorCode::InvalidArgument);
    }
    SECTION("label many at once; new clips take the label") {
        const auto labelled =
            edit::setMediaAttributes(tl.state, std::vector<MediaId>{a, b}, {.colorIndex = 7});
        REQUIRE(labelled);
        CHECK(item(*labelled, a).colorIndex == 7);
        CHECK(item(*labelled, b).colorIndex == 7);
        CHECK(clipIn(*labelled, tl.seq, "V1@0").colorIndex == 0); // placed before
        const ProjectState placed = place(*labelled, tl.seq, b, 0, 4, 4);
        CHECK(clipIn(placed, tl.seq, "V1@4").colorIndex == 7);
        CHECK(edit::setMediaAttributes(tl.state, std::vector<MediaId>{a}, {.colorIndex = 16}).error().code ==
              ErrorCode::InvalidArgument);
    }
    SECTION("comment, description and their bound") {
        const auto noted = edit::setMediaAttributes(tl.state, std::vector<MediaId>{a},
                                                    {.comment = "Best take", .description = "Wide, golden hour"});
        REQUIRE(noted);
        CHECK(item(*noted, a).comment == "Best take");
        CHECK(item(*noted, a).description == "Wide, golden hour");
        const std::string tooLong(MediaItem::kMaxTextBytes + 1, 'x');
        CHECK(edit::setMediaAttributes(tl.state, std::vector<MediaId>{a}, {.comment = tooLong}).error().code ==
              ErrorCode::InvalidArgument);
    }
    SECTION("poster frame inside the media, and cleared") {
        const auto poster = edit::setMediaAttributes(tl.state, std::vector<MediaId>{a},
                                                     {.posterFrame = std::optional<Ticks>(f(250))});
        REQUIRE(poster);
        CHECK(item(*poster, a).posterFrame == f(250));
        CHECK(edit::setMediaAttributes(tl.state, std::vector<MediaId>{a},
                                       {.posterFrame = std::optional<Ticks>(f(1000))})
                  .error()
                  .code == ErrorCode::InvalidArgument);
        const auto cleared = edit::setMediaAttributes(*poster, std::vector<MediaId>{a},
                                                      {.posterFrame = std::optional<Ticks>()});
        REQUIRE(cleared);
        CHECK(!item(*cleared, a).posterFrame);
    }
    SECTION("unknown media and no-op changes") {
        CHECK(edit::setMediaAttributes(tl.state, std::vector<MediaId>{MediaId{9999}}, {.colorIndex = 1}).error().code ==
              ErrorCode::NotFound);
        const auto same = edit::setMediaAttributes(tl.state, std::vector<MediaId>{a}, {.colorIndex = 0});
        REQUIRE(same);
        CHECK(isSameRevision(*same, tl.state));
    }
}

TEST_CASE("Library metadata survives a save and load", "[model][library][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    ProjectState s = tl.state;
    const MediaId a = tl.media('A');
    s = *edit::setMediaAttributes(s, std::vector<MediaId>{a},
                                  {.colorIndex = 3, .comment = "c", .description = "d",
                                   .posterFrame = std::optional<Ticks>(f(12))});
    const MediaId sub = subclipOf(s, a, 100, 20, false, "Take 2");
    requireValid(s);
    const auto text = io::toJson(s);
    REQUIRE(text);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(*loaded == s);
    CHECK(item(*loaded, sub).subclip == SubclipSpec{a, {f(100), f(20)}, false});

    SECTION("projects without the fields are unchanged") {
        const auto plain = io::toJson(tl.state);
        REQUIRE(plain);
        CHECK(plain->find("\"description\"") == std::string::npos); // clips write their own colorIndex
        CHECK(plain->find("subclip") == std::string::npos);
        CHECK(plain->find("posterFrame") == std::string::npos);
    }
    SECTION("a dangling master is valid; a range past a shorter file loads, warns and clamps") {
        ProjectState orphan = s;
        std::erase_if(orphan.media, [&](const MediaPtr& m) { return m->id == a; });
        orphan.sequences.clear();
        requireValid(orphan);

        ProjectState shorter = s;
        for (MediaPtr& m : shorter.media) {
            if (m->id == sub) {
                MediaItem relinked = *m;
                relinked.info.duration = f(110);
                for (auto& v : relinked.info.video)
                    v.duration = f(110);
                for (auto& au : relinked.info.audio)
                    au.duration = f(110);
                relinked.subclip->restricted = true;
                m = std::make_shared<const MediaItem>(std::move(relinked));
            }
        }
        const auto reloaded = io::fromJson(*io::toJson(shorter));
        REQUIRE(reloaded);
        CHECK(validate(*reloaded, Validation::Structure));
        const auto warnings = subclipRangeWarnings(*reloaded);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().find("Take 2") != std::string::npos);
        const SourceBounds bounds = item(*reloaded, sub).sourceBounds({StreamKind::Video, 0});
        CHECK(bounds.start == f(100));
        CHECK(bounds.end == f(110));
    }
    SECTION("bad values are rejected on load") {
        std::string bad = *text;
        const auto at = bad.find("\"colorIndex\": 3");
        REQUIRE(at != std::string::npos);
        bad.replace(at, 15, "\"colorIndex\": 40");
        CHECK_FALSE(io::fromJson(bad));
    }
}

// ---- Subclips --------------------------------------------------------------------------------------------------

TEST_CASE("Make Subclip copies the file, marks its range and refuses what has no time", "[model][library][subclip]") {
    Timeline tl(R"(
        V1 tgt |....|
    )");
    ProjectState s = tl.state;
    const MediaId a = tl.media('A');
    Marker inside;
    inside.start = f(105);
    inside.name = "inside";
    Marker outside = inside;
    outside.start = f(300);
    outside.name = "outside";
    s = ok(edit::setMediaMarkers(s, a, {inside, outside})).state;
    s = *edit::setMediaAttributes(s, std::vector<MediaId>{a}, {.colorIndex = 5});

    const MediaId sub = subclipOf(s, a, 100, 20, true, "");
    const MediaItem& m = item(s, sub);
    CHECK(m.name == "A.Subclip");
    CHECK(m.locator == item(s, a).locator);
    CHECK(m.info == item(s, a).info);
    CHECK(m.colorIndex == 5);
    CHECK(m.markIn == f(100));
    CHECK(m.markOut == f(120));
    REQUIRE(m.markers.size() == 1);
    CHECK(m.markers.front().name == "inside");
    requireValid(s);

    SECTION("a subclip of a restricted subclip stays inside it") {
        CHECK(edit::makeSubclip(s, sub, {f(90), f(20)}, "x").error().code == ErrorCode::InvalidArgument);
        const auto nested = edit::makeSubclip(s, sub, {f(105), f(10)}, "x");
        REQUIRE(nested);
        CHECK(nested->state.findMedia(nested->id)->subclip->master == sub);
    }
    SECTION("ranges outside the file, empty ranges, stills and titles are refused") {
        CHECK(edit::makeSubclip(s, a, {f(990), f(20)}, "x").error().code == ErrorCode::InvalidArgument);
        CHECK(edit::makeSubclip(s, a, {f(10), Ticks{}}, "x").error().code == ErrorCode::InvalidArgument);
        MediaItem still;
        still.name = "Still";
        still.info.isStillImage = true;
        still.info.video.push_back({});
        auto withStill = edit::addMedia(s, still);
        REQUIRE(withStill);
        CHECK(edit::makeSubclip(withStill->state, withStill->id, {f(0), f(5)}, "x").error().code ==
              ErrorCode::InvalidArgument);
        MediaItem title;
        title.name = "Title";
        title.generator = GeneratorSpec{std::string(kTitleGenerator), "Hello", "Arial", "Regular"};
        auto withTitle = edit::addMedia(s, title);
        REQUIRE(withTitle);
        CHECK(edit::makeSubclip(withTitle->state, withTitle->id, {f(0), f(5)}, "x").error().code ==
              ErrorCode::InvalidArgument);
    }
    SECTION("editing a subclip clamps its marks; restricting refuses while a clip lies outside") {
        const ProjectState placed = place(s, tl.seq, sub, 100, 4, 0);
        const auto widened = edit::setSubclip(placed, sub, {f(90), f(40)}, false);
        REQUIRE(widened);
        const auto slipped = ok(edit::slip(*widened, tl.seq, std::vector<ClipId>{clipAt(*widened, tl.seq, "V1@0")},
                                           f(-8)));
        CHECK(clipIn(slipped.state, tl.seq, "V1@0").sourceIn == f(92));
        const auto refused = edit::setSubclip(slipped.state, sub, {f(100), f(20)}, true);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
        const auto narrowed = edit::setSubclip(s, sub, {f(110), f(5)}, true);
        REQUIRE(narrowed);
        CHECK(item(*narrowed, sub).markIn == f(110));
        CHECK(item(*narrowed, sub).markOut == f(115));
    }
}

// ---- Restricted subclips in every edit op ------------------------------------------------------------------------

namespace {

/// A subclip of A covering source [100, 120), restricted or not, with a clip of source [105, 115) on V1 at frame 20.
struct SubclipRig {
    Timeline tl{R"(
        V1 tgt |............................................................|
    )"};
    ProjectState state;
    MediaId sub;

    explicit SubclipRig(bool restricted) {
        state = tl.state;
        sub = subclipOf(state, tl.media('A'), 100, 20, restricted);
        state = place(state, tl.seq, sub, 105, 10, 20);
        requireValid(state);
    }
    ClipId clip() const { return clipAt(state, tl.seq, "V1@20"); }
    const Clip& at(const ProjectState& s, std::string_view ref) const { return clipIn(s, tl.seq, ref); }
};

} // namespace

TEST_CASE("Restricted subclips bound regular and ripple trims", "[model][library][subclip]") {
    SECTION("restricted: the head stops at the subclip's In, the tail at its Out") {
        SubclipRig rig(true);
        const auto head = ok(edit::trimEdge(rig.state, rig.tl.seq, rig.clip(), edit::Edge::Head, f(-50)));
        CHECK(frames(rig.at(head.state, "V1@15").sourceIn) == 100);
        const auto tail = ok(edit::trimEdge(rig.state, rig.tl.seq, rig.clip(), edit::Edge::Tail, f(50)));
        CHECK(frames(rig.at(tail.state, "V1@20").duration()) == 15);
        const auto ripple = ok(edit::rippleTrim(rig.state, rig.tl.seq, rig.clip(), edit::Edge::Tail, f(50)));
        CHECK(frames(rig.at(ripple.state, "V1@20").sourceOut()) == 120);
    }
    SECTION("unrestricted: exactly as plain media (the timeline start and the file end)") {
        SubclipRig rig(false);
        const auto head = ok(edit::trimEdge(rig.state, rig.tl.seq, rig.clip(), edit::Edge::Head, f(-50)));
        CHECK(frames(rig.at(head.state, "V1@0").sourceIn) == 85);
        const auto tail = ok(edit::trimEdge(rig.state, rig.tl.seq, rig.clip(), edit::Edge::Tail, f(50)));
        CHECK(frames(rig.at(tail.state, "V1@20").duration()) == 60);
    }
}

TEST_CASE("Restricted subclips bound slip", "[model][library][subclip]") {
    SubclipRig restricted(true);
    const auto later = ok(edit::slip(restricted.state, restricted.tl.seq, std::vector<ClipId>{restricted.clip()}, f(50)));
    CHECK(frames(restricted.at(later.state, "V1@20").sourceIn) == 110);
    const auto earlier =
        ok(edit::slip(restricted.state, restricted.tl.seq, std::vector<ClipId>{restricted.clip()}, f(-50)));
    CHECK(frames(restricted.at(earlier.state, "V1@20").sourceIn) == 100);

    SubclipRig free(false);
    const auto freeLater = ok(edit::slip(free.state, free.tl.seq, std::vector<ClipId>{free.clip()}, f(50)));
    CHECK(frames(free.at(freeLater.state, "V1@20").sourceIn) == 155);
}

TEST_CASE("Restricted subclips bound slide through their neighbours", "[model][library][subclip]") {
    // P: the subclip's last 10 frames ([110, 120)) at 0; C (media C) at 10; N (media N) at 20.
    const auto rig = [](bool restricted) {
        Timeline tl(R"(
            V1 tgt |..........C40cccccccN40nnnnnnn..........|
        )");
        ProjectState s = tl.state;
        const MediaId sub = subclipOf(s, tl.media('A'), 100, 20, restricted);
        s = place(s, tl.seq, sub, 110, 10, 0);
        requireValid(s);
        return std::pair{tl, s};
    };
    {
        auto [tl, s] = rig(true);
        // Sliding C later needs P's tail to grow past 120: nothing moves.
        const auto r = edit::slide(s, tl.seq, clipAt(s, tl.seq, "V1@10"), f(5));
        REQUIRE(r);
        CHECK(isSameRevision(r->state, s));
    }
    {
        auto [tl, s] = rig(false);
        const auto r = ok(edit::slide(s, tl.seq, clipAt(s, tl.seq, "V1@10"), f(5)));
        CHECK(frames(clipIn(r.state, tl.seq, "V1@0").sourceOut()) == 125);
        CHECK(frames(clipIn(r.state, tl.seq, "V1@15").start()) == 15);
    }
}

TEST_CASE("Restricted subclips bound Speed/Duration", "[model][library][subclip]") {
    edit::SpeedChange twice;
    twice.speed = Speed{2, 1};
    twice.keepDuration = true; // 10 frames at 200 % read source [105, 125)
    SubclipRig restricted(true);
    const auto refused = edit::setSpeed(restricted.state, restricted.tl.seq, std::vector<ClipId>{restricted.clip()}, twice);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::InvalidArgument);

    SubclipRig free(false);
    const auto r = ok(edit::setSpeed(free.state, free.tl.seq, std::vector<ClipId>{free.clip()}, twice));
    CHECK(frames(free.at(r.state, "V1@20").sourceOut()) == 125);
}

TEST_CASE("Restricted subclips bound time remapping", "[model][library][subclip]") {
    SubclipRig restricted(true);
    const auto on = ok(edit::setTimeRemapEnabled(restricted.state, restricted.tl.seq, restricted.clip(), true));
    const Curve& remap = *restricted.at(on.state, "V1@20").timeRemap();
    REQUIRE(remap.keys.size() == 2);
    CHECK(remap.keys.front().value == static_cast<double>(f(100).value)); // the subclip, not the whole file
    CHECK(remap.keys.back().value == static_cast<double>(f(120).value));
    edit::TimeRemapKeyChange past;
    past.source = f(130);
    CHECK(edit::setTimeRemapKey(on.state, restricted.tl.seq, restricted.clip(), 1, past).error().code ==
          ErrorCode::InvalidArgument);
    edit::TimeRemapKeyChange before;
    before.source = f(90);
    CHECK(edit::setTimeRemapKey(on.state, restricted.tl.seq, restricted.clip(), 0, before).error().code ==
          ErrorCode::InvalidArgument);

    SubclipRig free(false);
    const auto freeOn = ok(edit::setTimeRemapEnabled(free.state, free.tl.seq, free.clip(), true));
    const Curve& freeRemap = *free.at(freeOn.state, "V1@20").timeRemap();
    CHECK(freeRemap.keys.front().value == 0.0);
    CHECK(freeRemap.keys.back().value == static_cast<double>(f(1000).value));
    ok(edit::setTimeRemapKey(freeOn.state, free.tl.seq, free.clip(), 1, past));
}

TEST_CASE("Three-point edits of a subclip default to its range and respect the restriction", "[model][library][subclip]") {
    Timeline tl(R"(
        V1 tgt |....................|
    )");
    for (const bool restricted : {true, false}) {
        ProjectState s = tl.state;
        const MediaId sub = subclipOf(s, tl.media('A'), 100, 20, restricted);
        const Sequence& seq = *s.findSequence(tl.seq);
        const auto patches = edit::defaultSourcePatches(seq, item(s, sub));
        std::vector<edit::SourcePatch> video{patches.front()};
        // Cleared marks: the subclip's range either way (Premiere).
        const auto unmarked = edit::resolveThreePointEdit(s, tl.seq, sub, video, {}, f(0));
        REQUIRE(unmarked);
        CHECK(unmarked->sourceRange.start == f(100));
        CHECK(unmarked->sourceRange.duration == f(20));
        // Marks outside the range: refused only when restricted.
        const auto outside = edit::resolveThreePointEdit(s, tl.seq, sub, video, {f(80), f(130)}, f(0));
        CHECK(outside.has_value() == !restricted);
        // An explicit source range outside it, too.
        const edit::SourceClip early{sub, {f(90), f(5)}, video};
        CHECK(edit::insertEdit(s, tl.seq, early, f(0)).has_value() == !restricted);
    }
}

TEST_CASE("Restricted subclips bound transition handles and Strict validation", "[model][library][subclip]") {
    // P plays the subclip's last 10 frames [110, 120) at 0; B follows at 10.
    const auto rig = [](bool restricted) {
        Timeline tl(R"(
            V1 tgt |..........B40bbbbbbbbb|
        )");
        ProjectState s = tl.state;
        const MediaId sub = subclipOf(s, tl.media('A'), 100, 20, restricted);
        s = place(s, tl.seq, sub, 110, 10, 0);
        return std::tuple{tl, s, sub};
    };
    edit::TransitionSpec spec;
    spec.before = f(2);
    spec.after = f(2); // P plays 2 frames past its Out: source [120, 122)
    {
        auto [tl, s, sub] = rig(true);
        spec.outgoing = clipAt(s, tl.seq, "V1@0");
        spec.incoming = clipAt(s, tl.seq, "V1@10");
        CHECK(edit::addTransition(s, tl.seq, tl.track("V1"), spec).error().code == ErrorCode::InvalidArgument);
        // A state that breaks the restriction (built by hand) fails Strict validation, not Structure.
        ProjectState broken = s;
        for (MediaPtr& m : broken.media) {
            if (m->id == sub) {
                MediaItem narrow = *m;
                narrow.subclip->range = {f(112), f(8)};
                m = std::make_shared<const MediaItem>(std::move(narrow));
            }
        }
        CHECK_FALSE(validate(broken, Validation::Strict));
        CHECK(validate(broken, Validation::Structure));
    }
    {
        auto [tl, s, sub] = rig(false);
        spec.outgoing = clipAt(s, tl.seq, "V1@0");
        spec.incoming = clipAt(s, tl.seq, "V1@10");
        ok(edit::addTransition(s, tl.seq, tl.track("V1"), spec));
    }
}

TEST_CASE("Restricted subclip source marks stay inside the range", "[model][library][subclip]") {
    Timeline tl(R"(
        V1 tgt |....|
    )");
    ProjectState s = tl.state;
    const MediaId sub = subclipOf(s, tl.media('A'), 100, 20, true);
    CHECK(edit::setSourceMarks(s, sub, {f(90), f(110)}).error().code == ErrorCode::InvalidArgument);
    ok(edit::setSourceMarks(s, sub, {f(104), f(110)}));
    ProjectState free = tl.state;
    const MediaId freeSub = subclipOf(free, tl.media('A'), 100, 20, false);
    ok(edit::setSourceMarks(free, freeSub, {f(90), f(130)}));
}

TEST_CASE("Deleting a bin drops its sequences' review notes and keeps a sequence another one plays (E5-P2-6)", "[model][library]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
    )");
    const auto noteOn = [](SequenceId seq) {
        return ReviewNote{{}, seq, f(1), {}, std::nullopt, "C", "too long", "", ReviewStatus::Open, {}};
    };
    SECTION("a sequence with review notes goes with its notes") {
        edit::SequenceSettings settings;
        settings.name = "Alt cut";
        settings.frameRate = kRate;
        auto m = edit::addSequence(tl.state, settings);
        REQUIRE(m);
        const ProjectState longer = ok(edit::setSequenceDuration(m->state, m->id, f(10))).state;
        auto bin = edit::addBin(longer, "Old cuts");
        REQUIRE(bin);
        auto noted = edit::addReviewNote(bin->state, noteOn(m->id));
        REQUIRE(noted);
        auto moved = edit::moveToBin(noted->state, bin->id, {}, {}, std::vector<SequenceId>{m->id});
        REQUIRE(moved);
        const auto removed = edit::removeBins(*moved, std::vector<BinId>{bin->id});
        REQUIRE(removed);
        CHECK(removed->findSequence(m->id) == nullptr);
        CHECK(removed->reviewNotes.empty());
        requireValid(*removed);
    }
    SECTION("a sequence played by one outside the bins is refused with a reason") {
        const ClipId a = tl.clip("V1@0");
        auto nest = edit::nestClips(tl.state, tl.seq, std::span(&a, 1), "N");
        REQUIRE(nest);
        auto bin = edit::addBin(nest->edit.state, "Old cuts");
        REQUIRE(bin);
        auto moved = edit::moveToBin(bin->state, bin->id, {}, {}, std::vector<SequenceId>{nest->nested});
        REQUIRE(moved);
        const auto refused = edit::removeBins(*moved, std::vector<BinId>{bin->id});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
        CHECK(refused.error().message.find("'N' is used in sequence") != std::string::npos);
    }
}
