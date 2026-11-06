// Stream C, batch C5 (model): Consolidate Duplicates (card 025) and the rights and clearance record (card 179). The
// channel names of a poly WAV (card 062) are in tests/media, Collect Files' folder layout (card 076) in tests/editor.

#include "TimelineFixture.h"

#include <wabcut/model/Auditions.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Rights.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// Gives media `letter` the file identity `hash` (and a path), so items with one hash are the same file.
ProjectState withFile(ProjectState state, MediaId id, const std::string& hash, bool offline = false) {
    for (MediaPtr& m : state.media) {
        if (m->id == id) {
            auto item = std::make_shared<MediaItem>(*m);
            item->locator.path = "D:/Footage/" + item->name + ".mp4";
            item->locator.fingerprint = SourceFingerprint{5'000'000, 1, hash};
            item->locator.offline = offline;
            m = item;
        }
    }
    return state;
}

ProjectState edited(ProjectState state, MediaId id, const std::function<void(MediaItem&)>& change) {
    for (MediaPtr& m : state.media) {
        if (m->id == id) {
            auto item = std::make_shared<MediaItem>(*m);
            change(*item);
            m = item;
        }
    }
    return state;
}

edit::ConsolidateResult consolidated(const ProjectState& state, std::span<const MediaId> only = {}) {
    auto result = edit::consolidateDuplicates(state, only);
    REQUIRE(result);
    requireValid(result->state);
    return std::move(*result);
}

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

} // namespace

TEST_CASE("Duplicate groups are items with one file; subclips and generated items are left out (025)", "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    const MediaId c = tl.media('C');
    ProjectState s = withFile(withFile(withFile(tl.state, a, "aaaaaaaaaaaaaaaa"), b, "aaaaaaaaaaaaaaaa"), c, "cccccccccccccccc");
    CHECK(edit::duplicateMediaGroups(s) == std::vector<std::vector<MediaId>>{{a, b}});

    // A subclip shares its master's file on purpose.
    auto sub = edit::makeSubclip(s, a, TimeRange{f(0), f(3)}, "Sub");
    REQUIRE(sub);
    CHECK(edit::duplicateMediaGroups(sub->state) == std::vector<std::vector<MediaId>>{{a, b}});

    // A file without a fingerprint proves nothing.
    ProjectState unknown = withFile(s, c, "aaaaaaaaaaaaaaaa");
    unknown = edited(unknown, c, [](MediaItem& m) { m.locator.fingerprint = SourceFingerprint{}; });
    CHECK(edit::duplicateMediaGroups(unknown) == std::vector<std::vector<MediaId>>{{a, b}});
    CHECK(edit::duplicateMediaGroups(tl.state).empty());
}

TEST_CASE("Consolidate Duplicates plays the keeper where a duplicate played, and removes the duplicate (025)",
          "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    tl.linkMatching();
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    const ProjectState s = withFile(withFile(tl.state, a, "1111111111111111"), b, "1111111111111111");

    const auto result = consolidated(s);
    REQUIRE(result.groups.size() == 1);
    CHECK(result.groups[0].keeper == a); // equal clip counts: the lowest id
    CHECK(result.groups[0].removed == std::vector<MediaId>{b});
    CHECK(result.groups[0].repointedClips == 2); // its video and its audio clip
    CHECK(result.removedItems() == 1);
    CHECK(result.repointedClips() == 2);
    CHECK(result.state.findMedia(b) == nullptr);
    CHECK(clipIn(result.state, tl.seq, "V1@6").media == a);
    CHECK(clipIn(result.state, tl.seq, "A1@6").media == a);
    // The timeline itself is untouched: the same clips at the same places (their names are the clips' own).
    CHECK(clipIn(result.state, tl.seq, "V1@6").timelineRange == clipIn(tl.state, tl.seq, "V1@6").timelineRange);
    CHECK(clipIn(result.state, tl.seq, "V1@6").sourceIn == clipIn(tl.state, tl.seq, "V1@6").sourceIn);
    CHECK(clipIn(result.state, tl.seq, "V1@6").name() == "B");
    CHECK(links(result.state, tl.seq) == links(tl.state, tl.seq));

    // Run again: nothing left to merge, the state stays the same revision.
    const auto again = consolidated(result.state);
    CHECK(again.groups.empty());
    CHECK(isSameRevision(again.state, result.state));
}

TEST_CASE("The keeper is the online item then the one with most clips then the lowest id (025)", "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Bbbb|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    ProjectState s = withFile(withFile(tl.state, a, "2222222222222222"), b, "2222222222222222");
    // B has more clips than A.
    CHECK(consolidated(s).groups[0].keeper == b);
    // Offline loses to online whatever the clips.
    s = withFile(s, b, "2222222222222222", true);
    CHECK(consolidated(s).groups[0].keeper == a);
    // Both offline: the clips decide again.
    s = withFile(s, a, "2222222222222222", true);
    CHECK(consolidated(s).groups[0].keeper == b);
}

TEST_CASE("Items read differently are not merged and are listed with the reason (025)", "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    const MediaId c = tl.media('C');
    ProjectState s = withFile(withFile(withFile(tl.state, a, "3333333333333333"), b, "3333333333333333"), c, "3333333333333333");
    s = edited(s, b, [](MediaItem& m) { m.scaling = MediaScaling::SetToFill; });
    s = edited(s, c, [](MediaItem& m) { m.interpretation = MediaInterpretation{}; m.interpretation->frameRate = Rate{30, 1}; });

    const auto result = consolidated(s);
    REQUIRE(result.groups.size() == 1);
    CHECK(result.groups[0].removed.empty());
    REQUIRE(result.groups[0].skipped.size() == 2);
    CHECK(result.groups[0].skipped[0].reason.find("scaled") != std::string::npos);
    CHECK(result.groups[0].skipped[1].reason.find("interpreted") != std::string::npos);
    CHECK(isSameRevision(result.state, s));
    CHECK(result.state.findMedia(b) != nullptr);

    // Two that match each other get a keeper of their own even when the first keeper turned them down.
    ProjectState t = withFile(withFile(withFile(tl.state, a, "4444444444444444"), b, "4444444444444444"), c, "4444444444444444");
    t = edited(t, b, [](MediaItem& m) { m.scaling = MediaScaling::SetToFill; });
    t = edited(t, c, [](MediaItem& m) { m.scaling = MediaScaling::SetToFill; });
    const auto split = consolidated(t);
    REQUIRE(split.groups.size() == 1);
    CHECK(split.groups[0].keeper == b);
    CHECK(split.groups[0].removed == std::vector<MediaId>{c});
    CHECK(split.groups[0].skipped.empty()); // B is the keeper of its own set: not "left over"
    CHECK(split.state.findMedia(c) == nullptr);
    CHECK(split.state.findMedia(a) != nullptr);
    CHECK(split.state.findMedia(b) != nullptr);
}

TEST_CASE("The keeper takes what only its duplicates had written (025)", "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    ProjectState s = withFile(withFile(tl.state, a, "5555555555555555"), b, "5555555555555555");
    s = edited(s, a, [](MediaItem& m) {
        m.comment = "keeper comment";
        m.markers.push_back(Marker{f(2), f(0), "Same", "", 0});
    });
    s = edited(s, b, [](MediaItem& m) {
        m.comment = "other comment";
        m.description = "from the duplicate";
        m.colorIndex = 5;
        m.metadata.scene = "12";
        m.metadata.good = true;
        m.markers.push_back(Marker{f(2), f(0), "Same", "", 0}); // same start and name: once
        m.markers.push_back(Marker{f(1), f(0), "Earlier", "", 0});
        m.rights = MediaRights{};
        m.rights->source = "Archive";
        m.rights->status = ClearanceStatus::Cleared;
    });
    const auto result = consolidated(s);
    REQUIRE(result.groups.size() == 1);
    const MediaItem& keeper = *result.state.findMedia(a);
    CHECK(keeper.comment == "keeper comment"); // what was written stays
    CHECK(keeper.description == "from the duplicate");
    CHECK(keeper.colorIndex == 5);
    CHECK(keeper.metadata.scene == "12");
    CHECK(keeper.metadata.good);
    REQUIRE(keeper.markers.size() == 2);
    CHECK(keeper.markers[0].name == "Earlier");
    CHECK(keeper.markers[1].name == "Same");
    REQUIRE(keeper.rights.has_value());
    CHECK(keeper.rights->source == "Archive");
    CHECK(keeper.rights->status == ClearanceStatus::Cleared);
}

TEST_CASE("Rights that differ are kept as the keeper has them and the conflict is reported (025 179)", "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    ProjectState s = withFile(withFile(tl.state, a, "6666666666666666"), b, "6666666666666666");
    s = edited(s, a, [](MediaItem& m) {
        m.rights = MediaRights{};
        m.rights->status = ClearanceStatus::Pending;
        m.rights->source = "Archive A";
    });
    s = edited(s, b, [](MediaItem& m) {
        m.rights = MediaRights{};
        m.rights->status = ClearanceStatus::Cleared;
        m.rights->source = "Archive B";
        m.rights->licence = "ten years";
    });
    const auto result = consolidated(s);
    REQUIRE(result.groups.size() == 1);
    const MediaItem& keeper = *result.state.findMedia(a);
    CHECK(keeper.rights->status == ClearanceStatus::Pending); // never silently cleared by a duplicate
    CHECK(keeper.rights->source == "Archive A");
    CHECK(keeper.rights->licence == "ten years"); // an empty field is filled
    REQUIRE(result.groups[0].notes.size() == 1);
    CHECK(result.groups[0].notes[0].find("rights") != std::string::npos);
}

TEST_CASE("Consolidate Duplicates re-points subclips and redactions and transcripts and auditions and provenance (025)",
          "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Aaaa|
        V2 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    const MediaId z = tl.media('C');
    ProjectState s = withFile(withFile(tl.state, a, "7777777777777777"), b, "7777777777777777");

    // A subclip of B, a redaction on B, transcript corrections of B, a derived file made from B.
    auto sub = edit::makeSubclip(s, b, TimeRange{f(0), f(3)}, "Sub of B");
    REQUIRE(sub);
    s = sub->state;
    Redaction redaction;
    redaction.media = b;
    redaction.name = "Plate";
    redaction.source = {Ticks{}, f(4)};
    redaction.region.shape = MaskShape::Rectangle;
    auto red = edit::addRedaction(s, redaction);
    REQUIRE(red);
    s = red->state;
    TranscriptEdits edits;
    edits.media = b;
    edits.audioStream = 0;
    edits.corrections.push_back(WordCorrection{TimeRange{f(0), f(1)}, "word", "fixed"});
    s.transcriptEdits.edit().push_back(edits);
    s = edited(s, z, [&](MediaItem& m) {
        DerivedProvenance d;
        d.source = b;
        d.operation = "separateStems";
        d.part = "vocals";
        d.createdAt = "2026-11-04T10:00:00Z";
        m.derived = d;
    });

    // An audition on V2 whose second take plays B.
    const ClipId second = clipAt(s, tl.seq, "V2@0");
    edit::TakeSource take;
    take.media = b;
    take.name = "B take";
    auto withTake = edit::addAuditionSource(s, tl.seq, second, take);
    REQUIRE(withTake);
    s = withTake->state;
    requireValid(s);

    const auto result = consolidated(s);
    REQUIRE(result.groups.size() == 1);
    REQUIRE(result.groups[0].keeper == a);
    const ProjectState& out = result.state;
    CHECK(out.findMedia(b) == nullptr);
    CHECK(out.findMedia(sub->id)->subclip->master == a);
    REQUIRE(out.redactions.size() == 1);
    CHECK(out.redactions[0].media == a);
    REQUIRE(out.transcriptEdits.size() == 1);
    CHECK(out.transcriptEdits[0].media == a);
    CHECK(out.findMedia(z)->derived->source == a);
    const Clip& audition = clipIn(out, tl.seq, "V2@0");
    REQUIRE(audition.audition() != nullptr);
    CHECK(audition.audition()->others.front().media == a);
    CHECK(clipIn(out, tl.seq, "V1@6").media == a);
    CHECK(clipIn(out, tl.seq, "V2@6").media == a);
}

TEST_CASE("Transcript corrections the keeper already has stay and the duplicates are reported (025)", "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    ProjectState s = withFile(withFile(tl.state, a, "8888888888888888"), b, "8888888888888888");
    TranscriptEdits mine;
    mine.media = a;
    mine.corrections.push_back(WordCorrection{TimeRange{f(0), f(1)}, "word", "mine"});
    TranscriptEdits theirs;
    theirs.media = b;
    theirs.corrections.push_back(WordCorrection{TimeRange{f(0), f(1)}, "word", "theirs"});
    s.transcriptEdits.edit().push_back(mine);
    s.transcriptEdits.edit().push_back(theirs);
    const auto result = consolidated(s);
    REQUIRE(result.state.transcriptEdits.size() == 1);
    CHECK(result.state.transcriptEdits[0].media == a);
    CHECK(result.state.transcriptEdits[0].corrections.front().text == "mine");
    REQUIRE(result.groups[0].notes.size() == 1);
    CHECK(result.groups[0].notes[0].find("transcript") != std::string::npos);
}

TEST_CASE("Consolidate Duplicates can be limited to the groups of some items and refuses unknown ones (025)",
          "[model][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc..Dddd|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    const MediaId c = tl.media('C');
    const MediaId d = tl.media('D');
    ProjectState s = withFile(withFile(withFile(withFile(tl.state, a, "9999999999999999"), b, "9999999999999999"), c,
                                       "abababababababab"), d, "abababababababab");
    const std::vector<MediaId> only{c};
    const auto result = consolidated(s, only);
    REQUIRE(result.groups.size() == 1);
    CHECK(result.groups[0].keeper == c);
    CHECK(result.state.findMedia(d) == nullptr);
    CHECK(result.state.findMedia(b) != nullptr); // the other group is left alone

    const std::vector<MediaId> unknown{MediaId{9999}};
    const auto refused = edit::consolidateDuplicates(s, unknown);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::NotFound);
}

TEST_CASE("Consolidate Duplicates is one undo step (025)", "[model][v07][library][session]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    EditSession session(withFile(withFile(tl.state, a, "cdcdcdcdcdcdcdcd"), b, "cdcdcdcdcdcdcdcd"));
    auto merged = edit::consolidateDuplicates(*session.snapshot());
    REQUIRE(merged);
    REQUIRE(session.commit("Consolidate Duplicates", merged->state));
    CHECK(session.snapshot()->findMedia(b) == nullptr);
    REQUIRE(session.undo());
    CHECK(session.snapshot()->findMedia(b) != nullptr);
    CHECK(clipIn(*session.snapshot(), tl.seq, "V1@6").media == b);
    REQUIRE(session.redo());
    CHECK(session.snapshot()->findMedia(b) == nullptr);
}

// ---- Rights (179) ----------------------------------------------------------------------------------------------------

TEST_CASE("Dates and costs and flags of a rights record (179)", "[model][v07][rights]") {
    CHECK(isIsoCalendarDate("2028-02-29"));
    CHECK_FALSE(isIsoCalendarDate("2027-02-29"));
    CHECK_FALSE(isIsoCalendarDate("2027-13-01"));
    CHECK_FALSE(isIsoCalendarDate("27-1-1"));
    CHECK_FALSE(isIsoCalendarDate("2027-01-01x"));
    CHECK(isoDateToDays("1970-01-01") == 0);
    CHECK(isoDateFromDays(*isoDateToDays("2026-12-31") + 1) == "2027-01-01");

    CHECK(parseCostMinor("250") == 25000);
    CHECK(parseCostMinor("250.5") == 25050);
    CHECK(parseCostMinor("1,250.00") == 125000);
    CHECK(parseCostMinor("1.250,00") == 125000);
    CHECK(parseCostMinor("1 250") == 125000);
    CHECK(parseCostMinor("12.") == 1200);
    CHECK(parseCostMinor("0.07") == 7);
    CHECK_FALSE(parseCostMinor("").has_value());
    CHECK_FALSE(parseCostMinor("abc").has_value());
    CHECK_FALSE(parseCostMinor("-5").has_value());
    CHECK_FALSE(parseCostMinor("12.3456").has_value());
    CHECK_FALSE(parseCostMinor("1,2,3").has_value());
    CHECK_FALSE(parseCostMinor("99999999999999999999").has_value());
    CHECK(formatCostMinor(125000) == "1250.00");
    CHECK(formatCostMinor(7) == "0.07");

    MediaRights r;
    CHECK(assessRights(r, "2026-12-01").needsAttention() == false); // an empty record has nothing to flag
    r.source = "Archive";
    CHECK(assessRights(r, "2026-12-01").statusUnset); // details but nobody said
    r.status = ClearanceStatus::Pending;
    CHECK(assessRights(r, "2026-12-01").uncleared());
    r.status = ClearanceStatus::Cleared;
    CHECK_FALSE(assessRights(r, "2026-12-01").needsAttention());
    r.expiry = "2026-12-10";
    auto a = assessRights(r, "2026-12-01", 30);
    CHECK(a.expiresSoon);
    CHECK(a.daysLeft == 9);
    CHECK(rightsFlagText(r, a) == "Licence expires in 9 days");
    CHECK_FALSE(assessRights(r, "2026-12-01", 5).expiresSoon);
    CHECK_FALSE(assessRights(r, "2026-12-10", 0).expired); // the last day is still a day it may be shown
    a = assessRights(r, "2026-12-11");
    CHECK(a.expired);
    CHECK(rightsFlagText(r, a) == "Licence expired on 2026-12-10");
    a = assessRights(r, "2026-12-09");
    CHECK(rightsFlagText(r, a) == "Licence expires in 1 day");
}

TEST_CASE("setMediaRights changes fields of several items in one edit and refuses what cannot be kept (179)",
          "[model][v07][rights]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    const std::vector<MediaId> both{a, b};
    edit::MediaRightsChange change;
    change.source = "Newsreel";
    change.status = ClearanceStatus::Pending;
    change.costMinor = std::optional<std::int64_t>(5000);
    change.currency = "USD";
    auto set = edit::setMediaRights(tl.state, both, change);
    REQUIRE(set);
    requireValid(*set);
    CHECK(set->findMedia(a)->rights->source == "Newsreel");
    CHECK(set->findMedia(b)->rights->costMinor == 5000);

    // Only the fields named change; an empty text clears one; clearing everything removes the record.
    edit::MediaRightsChange more;
    more.licence = "Ten years";
    auto more2 = edit::setMediaRights(*set, std::vector<MediaId>{a}, more);
    REQUIRE(more2);
    CHECK(more2->findMedia(a)->rights->source == "Newsreel");
    CHECK(more2->findMedia(a)->rights->licence == "Ten years");
    CHECK_FALSE(more2->findMedia(b)->rights->licence == "Ten years");
    edit::MediaRightsChange clear;
    clear.source = "";
    clear.licence = "";
    clear.currency = "";
    clear.status = ClearanceStatus::Unset;
    clear.costMinor.emplace(); // an empty amount
    auto cleared = edit::setMediaRights(*more2, std::vector<MediaId>{a}, clear);
    REQUIRE(cleared);
    CHECK_FALSE(cleared->findMedia(a)->rights.has_value());

    // Setting what is already set changes nothing: no new revision, no undo step.
    auto same = edit::setMediaRights(*set, both, change);
    REQUIRE(same);
    CHECK(isSameRevision(*same, *set));

    // Refusals: a bad date, a bad currency, a cost out of range, an unknown item, control characters, too long.
    edit::MediaRightsChange bad;
    bad.expiry = "next year";
    CHECK_FALSE(edit::setMediaRights(tl.state, both, bad));
    bad = {};
    bad.currency = "usd";
    CHECK_FALSE(edit::setMediaRights(tl.state, both, bad));
    bad = {};
    bad.costMinor = std::optional<std::int64_t>(-1);
    CHECK_FALSE(edit::setMediaRights(tl.state, both, bad));
    bad = {};
    bad.source = std::string("a\x01" "b");
    CHECK_FALSE(edit::setMediaRights(tl.state, both, bad));
    bad = {};
    bad.licence = std::string(MediaRights::kMaxFieldBytes + 1, 'x');
    CHECK_FALSE(edit::setMediaRights(tl.state, both, bad));
    bad = {};
    bad.source = "x";
    const std::vector<MediaId> unknown{MediaId{777}};
    auto missing = edit::setMediaRights(tl.state, unknown, bad);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(tl.state.findMedia(a)->rights == std::nullopt); // nothing changed by the refusals
}

TEST_CASE("A subclip without a record of its own has its master's rights (179)", "[model][v07][rights]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const MediaId a = tl.media('A');
    auto sub = edit::makeSubclip(tl.state, a, TimeRange{f(0), f(2)}, "Part");
    REQUIRE(sub);
    edit::MediaRightsChange change;
    change.creditLine = "Courtesy of the Archive";
    auto set = edit::setMediaRights(sub->state, std::vector<MediaId>{a}, change);
    REQUIRE(set);
    const MediaRights* inherited = effectiveRights(*set, *set->findMedia(sub->id));
    REQUIRE(inherited != nullptr);
    CHECK(inherited->creditLine == "Courtesy of the Archive");
    CHECK(inherited == &*set->findMedia(a)->rights); // the same record, not a copy
    // Its own record wins.
    edit::MediaRightsChange own;
    own.creditLine = "Own line";
    auto ownSet = edit::setMediaRights(*set, std::vector<MediaId>{sub->id}, own);
    REQUIRE(ownSet);
    CHECK(effectiveRights(*ownSet, *ownSet->findMedia(sub->id))->creditLine == "Own line");
    CHECK(effectiveRights(tl.state, *tl.state.findMedia(a)) == nullptr);
}

TEST_CASE("Rights are saved with the project only when set and read back; damaged ones are refused (179)", "[model][v07][rights][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = tl.media('A');
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    CHECK(plain->find("\"rights\"") == std::string::npos); // older builds open the project: no new key
    CHECK(plain->find("\"channelNames\"") == std::string::npos);

    edit::MediaRightsChange change;
    change.source = "Newsreel Archive";
    change.licence = "Worldwide";
    change.reference = "INV-7";
    change.territory = "UK";
    change.expiry = "2030-01-31";
    change.costMinor = std::optional<std::int64_t>(99);
    change.currency = "EUR";
    change.creditLine = "Courtesy of Newsreel";
    change.notes = "line one\nline two";
    change.status = ClearanceStatus::FairUse;
    auto set = edit::setMediaRights(tl.state, std::vector<MediaId>{a}, change);
    REQUIRE(set);
    const auto json = io::toJson(*set);
    REQUIRE(json);
    CHECK(json->find("\"fairUse\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(loaded->findMedia(a)->rights == set->findMedia(a)->rights);
    CHECK(*loaded == *set);

    // A status a newer build wrote reads as unset; a field past its bound is refused; a bad date fails validation.
    std::string text = *json;
    const auto replaceFirst = [](std::string& whole, const std::string& from, const std::string& to) {
        const auto at = whole.find(from);
        REQUIRE(at != std::string::npos);
        whole.replace(at, from.size(), to);
    };
    std::string newer = text;
    replaceFirst(newer, "\"fairUse\"", "\"licenceFree\"");
    const auto lenient = io::fromJson(newer);
    REQUIRE(lenient);
    CHECK(lenient->findMedia(a)->rights->status == ClearanceStatus::Unset);
    std::string longText = text;
    replaceFirst(longText, "\"Newsreel Archive\"", "\"" + std::string(MediaRights::kMaxFieldBytes + 1, 'x') + "\"");
    CHECK_FALSE(io::fromJson(longText));
    std::string badDate = text;
    replaceFirst(badDate, "2030-01-31", "soon");
    CHECK_FALSE(io::fromJson(badDate));
    std::string negative = text;
    replaceFirst(negative, "\"cost\": 99", "\"cost\": -5");
    CHECK_FALSE(io::fromJson(negative));
}

TEST_CASE("Channel names of an audio stream are saved only when set and bounded and read back (062)", "[model][v07][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const MediaId a = tl.media('A');
    ProjectState s = edited(tl.state, a, [](MediaItem& m) {
        m.info.audio.front().channels = 4;
        m.info.audio.front().channelNames = {"Boom", "", "Lav \xC3\xA9", "Mix"};
    });
    requireValid(s);
    const auto json = io::toJson(s);
    REQUIRE(json);
    CHECK(json->find("\"channelNames\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(loaded->findMedia(a)->info.audio.front().channelNames == s.findMedia(a)->info.audio.front().channelNames);

    // Not UTF-8, or too many, or too long: validate refuses the state, the reader refuses the file.
    ProjectState bad = edited(s, a, [](MediaItem& m) { m.info.audio.front().channelNames[0] = "\xFF\xFE"; });
    CHECK_FALSE(validate(bad));
    bad = edited(s, a, [](MediaItem& m) { m.info.audio.front().channelNames.assign(kMaxChannelNames + 1, "x"); });
    CHECK_FALSE(validate(bad));
    bad = edited(s, a, [](MediaItem& m) { m.info.audio.front().channelNames[0] = std::string(kMaxChannelNameLength + 1, 'x'); });
    CHECK_FALSE(validate(bad));
    std::string longName = *json;
    const auto at = longName.find("Boom");
    REQUIRE(at != std::string::npos);
    longName.replace(at, 4, std::string(kMaxChannelNameLength + 1, 'x'));
    CHECK_FALSE(io::fromJson(longName));
}
