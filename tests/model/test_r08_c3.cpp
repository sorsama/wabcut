// 0.7, stream C, batch C3: sequence creation dates (card 144), the bin nested sequences are filed in (card 112), one
// sequence for several clips (card 055) and Reload Footage (card 143).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

EditSessionOptions stamped(std::vector<std::string> times) {
    EditSessionOptions options;
    options.validateEdits = true;
    options.clock = [times = std::move(times), next = std::size_t{0}]() mutable {
        return times[std::min(next++, times.size() - 1)];
    };
    return options;
}

} // namespace

TEST_CASE("A sequence a commit adds is stamped with the time it was made (144)", "[model][session][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    EditSession session(tl.state, stamped({"2026-11-04T09:30:00Z", "2026-11-05T10:00:00Z"}));
    CHECK(session.snapshot()->findSequence(tl.seq)->createdAt.empty()); // opened from a file that has no date

    // File > New Sequence: stamped by the commit, whatever edit made it.
    edit::SequenceSettings settings;
    settings.name = "Final";
    auto added = edit::addSequence(*session.snapshot(), settings);
    REQUIRE(added);
    REQUIRE(session.commit("New Sequence", added->state));
    const SequenceId final = added->id;
    CHECK(session.snapshot()->findSequence(final)->createdAt == "2026-11-04T09:30:00Z");
    // Pure operations stay pure: the state the operation returned carries no time.
    CHECK(added->state.findSequence(final)->createdAt.empty());

    // A copy is a new sequence with its own time; the first one keeps its.
    auto copy = edit::duplicateSequence(*session.snapshot(), final);
    REQUIRE(copy);
    REQUIRE(session.commit("Duplicate", copy->state));
    CHECK(session.snapshot()->findSequence(copy->id)->createdAt == "2026-11-05T10:00:00Z");
    CHECK(session.snapshot()->findSequence(final)->createdAt == "2026-11-04T09:30:00Z");
    // The sequence that was there before keeps having no date (nothing is invented for old projects).
    CHECK(session.snapshot()->findSequence(tl.seq)->createdAt.empty());

    // An edit that adds no sequence stamps nothing; undo and redo bring the same time back.
    auto renamed = edit::setSequenceSettings(*session.snapshot(), final, [] {
        edit::SequenceSettingsChange change;
        change.name = "Final v2";
        return change;
    }());
    REQUIRE(renamed);
    REQUIRE(session.commit("Rename", renamed->state));
    CHECK(session.snapshot()->findSequence(final)->createdAt == "2026-11-04T09:30:00Z");
    REQUIRE(session.undo());
    REQUIRE(session.undo());
    CHECK(session.snapshot()->findSequence(copy->id) == nullptr);
    REQUIRE(session.redo());
    CHECK(session.snapshot()->findSequence(copy->id)->createdAt == "2026-11-05T10:00:00Z");
}

TEST_CASE("A sequence added by a nest or New Sequence From Clip is stamped too (144)", "[model][session][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    EditSession session(tl.state, stamped({"2026-11-06T08:00:00Z"}));
    auto nested = edit::nestClips(*session.snapshot(), tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, "Nest");
    REQUIRE(nested);
    REQUIRE(session.commit("Nest", nested->edit.state));
    CHECK(session.snapshot()->findSequence(nested->nested)->createdAt == "2026-11-06T08:00:00Z");
}

TEST_CASE("A sequence's creation time is saved only when set; damaged text is dropped on load (144)", "[model][io][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    CHECK(plain->find("\"createdAt\"") == std::string::npos); // older builds' files: nothing new

    ProjectState dated = tl.state;
    auto seq = std::make_shared<Sequence>(*dated.findSequence(tl.seq));
    seq->createdAt = "2026-11-04T09:30:00Z";
    dated = dated.withSequence(std::move(seq));
    const auto text = io::toJson(dated);
    REQUIRE(text);
    CHECK(text->find("\"createdAt\"") != std::string::npos);
    CHECK(text->find("2026-11-04T09:30:00Z") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(loaded->findSequence(tl.seq)->createdAt == "2026-11-04T09:30:00Z");

    // Text that is not an ISO-8601 UTC time is not kept (a project file is untrusted input): the file still opens.
    std::string damaged = *text;
    const auto at = damaged.find("2026-11-04T09:30:00Z");
    REQUIRE(at != std::string::npos);
    damaged.replace(at, 20, "last tuesday, maybe");
    const auto reloaded = io::fromJson(damaged);
    REQUIRE(reloaded);
    CHECK(reloaded->findSequence(tl.seq)->createdAt.empty());

    // The model refuses such a state when something sets it.
    ProjectState bad = tl.state;
    auto broken = std::make_shared<Sequence>(*bad.findSequence(tl.seq));
    broken->createdAt = "yesterday";
    bad = bad.withSequence(std::move(broken));
    CHECK_FALSE(validate(bad, Validation::Structure));
}

TEST_CASE("One bin is where nested sequences are filed; Nest puts the new sequence there (112)", "[model][nest][v07]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    auto firstBin = edit::addBin(tl.state, "Nests");
    REQUIRE(firstBin);
    auto secondBin = edit::addBin(firstBin->state, "Other");
    REQUIRE(secondBin);
    ProjectState state = secondBin->state;
    CHECK_FALSE(edit::nestBinOf(state).isValid());

    // Without a nest bin the new sequence lies beside the one it comes from (the root bin here).
    auto beside = edit::nestClips(state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, "Beside");
    REQUIRE(beside);
    CHECK_FALSE(beside->edit.state.findSequence(beside->nested)->bin.isValid());

    auto set = edit::setNestBin(state, firstBin->id, true);
    REQUIRE(set);
    CHECK(edit::nestBinOf(*set) == firstBin->id);
    auto filed = edit::nestClips(*set, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, "Filed");
    REQUIRE(filed);
    CHECK(filed->edit.state.findSequence(filed->nested)->bin == firstBin->id);
    requireValid(filed->edit.state);

    // One bin at most: choosing another takes it from the first.
    auto moved = edit::setNestBin(*set, secondBin->id, true);
    REQUIRE(moved);
    CHECK(edit::nestBinOf(*moved) == secondBin->id);
    CHECK_FALSE(moved->findBin(firstBin->id)->nestTarget);
    // Choosing the one that already is changes nothing (no undo step: the same bins).
    auto same = edit::setNestBin(*moved, secondBin->id, true);
    REQUIRE(same);
    CHECK(same->bins == moved->bins);
    // Off.
    auto off = edit::setNestBin(*moved, secondBin->id, false);
    REQUIRE(off);
    CHECK_FALSE(edit::nestBinOf(*off).isValid());
    auto stays = edit::setNestBin(*off, secondBin->id, false);
    REQUIRE(stays);
    CHECK(stays->bins == off->bins);

    // Errors: no such bin; a search bin holds nothing.
    CHECK_FALSE(edit::setNestBin(state, BinId{999}, true));
    auto search = edit::addSearchBin(state, "Everything", BinSearch{});
    REQUIRE(search);
    CHECK_FALSE(edit::setNestBin(search->state, search->id, true));

    // Deleting the bin takes the choice with it: Nest goes back to lying beside the sequence.
    auto gone = edit::removeBins(*set, std::vector<BinId>{firstBin->id});
    REQUIRE(gone);
    CHECK_FALSE(edit::nestBinOf(*gone).isValid());
}

TEST_CASE("The nest bin is saved only when set and reads back (112)", "[model][io][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    auto bin = edit::addBin(tl.state, "Nests");
    REQUIRE(bin);
    const auto plain = io::toJson(bin->state);
    REQUIRE(plain);
    CHECK(plain->find("nestTarget") == std::string::npos);
    auto set = edit::setNestBin(bin->state, bin->id, true);
    REQUIRE(set);
    const auto text = io::toJson(*set);
    REQUIRE(text);
    CHECK(text->find("\"nestTarget\"") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(edit::nestBinOf(*loaded) == bin->id);
}

TEST_CASE("New Sequence From Clips makes one sequence holding every clip in turn (055)", "[model][newSequenceFromClip][v07]") {
    Timeline tl(R"(
        V1 tgt |....|
    )");
    const std::vector<MediaId> media{tl.media('A'), tl.media('B'), tl.media('C')};
    auto made = edit::newSequenceFromClips(tl.state, media);
    REQUIRE(made);
    requireValid(made->state);
    const Sequence& s = *made->state.findSequence(made->id);
    CHECK(s.name == tl.state.findMedia(media[0])->name); // named after the first
    REQUIRE(s.videoTracks[0]->clips.size() == 3);
    // One after another from 0, each as long as its media.
    Ticks at{};
    for (std::size_t i = 0; i < 3; ++i) {
        const Clip& c = s.videoTracks[0]->clips[i];
        CHECK(c.media == media[i]);
        CHECK(c.start() == at);
        at = c.end();
    }
    CHECK(s.videoTracks[0]->clips[1].start() == s.videoTracks[0]->clips[0].end());

    auto named = edit::newSequenceFromClips(tl.state, std::vector<MediaId>{media[0], media[1]}, "Selects");
    REQUIRE(named);
    CHECK(named->state.findSequence(named->id)->name == "Selects");
    // One clip is New Sequence From Clip; nothing, or a missing item, is refused.
    auto one = edit::newSequenceFromClips(tl.state, std::vector<MediaId>{media[0]});
    REQUIRE(one);
    CHECK(one->state.findSequence(one->id)->videoTracks[0]->clips.size() == 1);
    CHECK_FALSE(edit::newSequenceFromClips(tl.state, std::vector<MediaId>{}));
    CHECK_FALSE(edit::newSequenceFromClips(tl.state, std::vector<MediaId>{media[0], MediaId{99999}}));
}

namespace {

/// `item`'s file as another program left it: longer or shorter, content changed.
edit::ReloadedFootage reread(const ProjectState& state, MediaId id, Ticks duration, const std::string& hash) {
    edit::ReloadedFootage r;
    r.media = id;
    r.file = *state.findMedia(id);
    r.file.info.duration = duration;
    for (VideoStreamInfo& v : r.file.info.video) {
        v.duration = duration;
        v.frameCount = -1;
    }
    for (AudioStreamInfo& a : r.file.info.audio) {
        a.duration = duration;
    }
    r.file.locator.fingerprint.size = 123456;
    r.file.locator.fingerprint.sampleHash = hash;
    return r;
}

} // namespace

TEST_CASE("Reload Footage reads a changed file again in place (143)", "[model][editing][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
    )");
    const MediaId a = tl.media('A');
    ProjectState state = tl.state;
    {
        // A proxy made of the old file, a made-offline flag and some state the reload must keep.
        auto item = std::make_shared<MediaItem>(*state.findMedia(a));
        item->locator.fingerprint.size = 1000;
        item->locator.fingerprint.sampleHash = "00000000000000aa";
        item->locator.proxy = ProxyMedia{"D:/proxies/a.mov", {}};
        item->comment = "keep me";
        item->markIn = f(2);
        item->madeOffline = true;
        item->locator.offline = true;
        for (MediaPtr& m : state.media) {
            if (m->id == a)
                m = item;
        }
    }
    const Ticks before = state.findMedia(a)->info.duration;
    const auto longer = reread(state, a, before + f(500), "00000000000000bb");
    auto out = edit::reloadFootage(state, std::vector<edit::ReloadedFootage>{longer});
    REQUIRE(out);
    requireValid(out->state);
    REQUIRE(out->reloaded.size() == 1);
    CHECK(out->warnings.empty());
    const MediaItem& item = *out->state.findMedia(a);
    CHECK(item.info.duration == before + f(500)); // the new length shows
    CHECK(item.locator.fingerprint.sampleHash == "00000000000000bb");
    CHECK_FALSE(item.locator.offline); // it is read from the disk: linked
    CHECK_FALSE(item.madeOffline);
    CHECK_FALSE(item.locator.proxy.has_value()); // made of the old file
    CHECK(item.comment == "keep me");
    CHECK(item.markIn == f(2));
    CHECK(item.name == state.findMedia(a)->name);
    CHECK(render(out->state, tl.seq) == render(state, tl.seq)); // the clips stay as they were

    // The same file read again: no change at all (the state is the same object graph).
    ProjectState settled = out->state;
    const auto again = edit::reloadFootage(settled, std::vector<edit::ReloadedFootage>{longer});
    REQUIRE(again);
    CHECK(again->reloaded.empty());
    CHECK(again->state.media == settled.media);

    // Same content but offline: it comes back online (a reload also links).
    auto offline = edit::makeOffline(settled, std::vector<MediaId>{a});
    REQUIRE(offline);
    const auto back = edit::reloadFootage(*offline, std::vector<edit::ReloadedFootage>{longer});
    REQUIRE(back);
    CHECK(back->reloaded.size() == 1);
    CHECK_FALSE(back->state.findMedia(a)->madeOffline);
    CHECK_FALSE(back->state.findMedia(a)->locator.offline);
}

TEST_CASE("Reload Footage warns about clips past a shorter file and refuses a missing stream (143)", "[model][editing][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
    )");
    const MediaId a = tl.media('A');
    // The file is now 3 frames long: the 8-frame clips run past its end. They stay and are reported.
    const auto shorter = reread(tl.state, a, f(3), "00000000000000cc");
    auto out = edit::reloadFootage(tl.state, std::vector<edit::ReloadedFootage>{shorter});
    REQUIRE(out);
    CHECK(out->reloaded.size() == 1);
    REQUIRE_FALSE(out->warnings.empty());
    CHECK(out->warnings.front().find("plays past the end") != std::string::npos);
    CHECK(render(out->state, tl.seq) == render(tl.state, tl.seq));

    // The new file has no picture stream the V1 clip plays: nothing is reloaded.
    auto noPicture = reread(tl.state, a, f(1000), "00000000000000dd");
    noPicture.file.info.video.clear();
    const auto refused = edit::reloadFootage(tl.state, std::vector<edit::ReloadedFootage>{noPicture});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("video stream 1") != std::string::npos);

    // Titles have no file; an unknown item is NotFound.
    CHECK_FALSE(edit::reloadFootage(tl.state, std::vector<edit::ReloadedFootage>{{MediaId{99999}, *tl.state.findMedia(a)}}));
}

TEST_CASE("Replace Footage keeps refusing a clip that would play past the new end (143 refactor)", "[model][editing][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    MediaItem shortFile = *tl.state.findMedia(tl.media('A'));
    shortFile.locator.path = "D:/footage/short.mov";
    shortFile.info.duration = f(3);
    for (VideoStreamInfo& v : shortFile.info.video)
        v.duration = f(3);
    for (AudioStreamInfo& a : shortFile.info.audio)
        a.duration = f(3);
    const auto replaced = edit::replaceFootage(tl.state, tl.media('A'), shortFile);
    REQUIRE_FALSE(replaced);
    CHECK(replaced.error().message.find("in the new file") != std::string::npos);
}
