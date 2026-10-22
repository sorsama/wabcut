// 0.2 (P.3): the Project panel's model - clip metadata (Scene, Shot, Take, Log Note, Good, Client, Tape Name), Search
// Bins, the Freeform view and bringing items in from another project (ProjectOps.h, Merge.h importProjectItems).

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

ProjectState roundTrip(const ProjectState& state) {
    const auto text = io::toJson(state);
    REQUIRE(text);
    auto back = io::fromJson(*text);
    if (!back) {
        FAIL("reading it back failed: " << back.error().message);
    }
    CHECK(io::toJson(*back).value() == *text);
    return std::move(*back);
}

template <class T>
T okValue(std::expected<T, Error> result) {
    if (!result) {
        FAIL("operation failed: " << result.error().message);
    }
    return std::move(*result);
}

/// `state` with media `id` pointing at `path` (the fixture's media have no file).
ProjectState withFile(ProjectState state, MediaId id, std::string path, std::string hash = "00112233aabbccdd") {
    for (MediaPtr& m : state.media) {
        if (m->id == id) {
            MediaItem item = *m;
            item.locator.path = std::move(path);
            item.locator.fingerprint = SourceFingerprint{1234, 0, std::move(hash)};
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    return state;
}

FreeformPlacement placed(FreeformPlacement::Kind kind, std::uint64_t id, double x, double y, double size = 1.0) {
    FreeformPlacement p;
    p.kind = kind;
    p.id = id;
    p.x = x;
    p.y = y;
    p.size = size;
    return p;
}

} // namespace

// ---- Clip metadata ---------------------------------------------------------------------------------------------

TEST_CASE("Clip metadata is set in one edit, bounded, and saved only when set", "[model][library][metadata]") {
    Timeline tl("    V1 tgt |Aaaa|\n");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    const std::string plain = io::toJson(tl.state).value();
    CHECK(plain.find("\"metadata\"") == std::string::npos);

    edit::MediaAttributeChange change;
    change.scene = "12";
    change.shot = "3";
    change.take = "2";
    change.logNote = "Laughs at the end";
    change.client = "Northwind";
    change.good = true;
    change.tapeName = "A001";
    const ProjectState s = okValue(edit::setMediaAttributes(tl.state, std::vector<MediaId>{a, b}, change));
    requireValid(s);
    for (const MediaId id : {a, b}) {
        const MediaItem& m = *s.findMedia(id);
        CHECK(m.metadata.scene == "12");
        CHECK(m.metadata.shot == "3");
        CHECK(m.metadata.take == "2");
        CHECK(m.metadata.logNote == "Laughs at the end");
        CHECK(m.metadata.client == "Northwind");
        CHECK(m.metadata.good);
        REQUIRE(m.timecode);
        CHECK(m.timecode->reelName == "A001");
    }
    const ChangeSet changes = diff(tl.state, s);
    CHECK(changes.modifiedMedia.size() == 2);

    // Saved and read back; the file says it.
    const ProjectState back = roundTrip(s);
    CHECK(back.findMedia(a)->metadata == s.findMedia(a)->metadata);
    CHECK(io::toJson(s).value().find("\"logNote\": \"Laughs at the end\"") != std::string::npos);

    // Clearing the tape name drops an otherwise empty Modify > Timecode.
    edit::MediaAttributeChange clear;
    clear.tapeName = "";
    clear.good = false;
    const ProjectState cleared = okValue(edit::setMediaAttributes(s, std::vector<MediaId>{a}, clear));
    CHECK_FALSE(cleared.findMedia(a)->timecode.has_value());
    CHECK_FALSE(cleared.findMedia(a)->metadata.good);

    // Bounds.
    edit::MediaAttributeChange tooLong;
    tooLong.scene = std::string(ClipMetadata::kMaxFieldBytes + 1, 'x');
    CHECK(edit::setMediaAttributes(s, std::vector<MediaId>{a}, tooLong).error().code == ErrorCode::InvalidArgument);
    edit::MediaAttributeChange notUtf8;
    notUtf8.logNote = std::string("\xC3\x28");
    CHECK(edit::setMediaAttributes(s, std::vector<MediaId>{a}, notUtf8).error().code == ErrorCode::InvalidArgument);
    // A file with an over-long field does not load.
    std::string text = io::toJson(s).value();
    const std::string from = "\"scene\": \"12\"";
    text.replace(text.find(from), from.size(), "\"scene\": \"" + std::string(ClipMetadata::kMaxFieldBytes + 1, 'y') + "\"");
    CHECK_FALSE(io::fromJson(text).has_value());
}

// ---- Search Bins -----------------------------------------------------------------------------------------------

TEST_CASE("A search bin keeps its query, holds nothing and round-trips", "[model][library][searchbin]") {
    Timeline tl("    V1 tgt |Aaaa|\n");
    BinSearch query;
    query.text = "interview";
    query.field = "scene";
    query.type = "video";
    query.label = 3;
    query.good = true;
    query.from = "2026-10-01";
    auto created = okValue(edit::addSearchBin(tl.state, "Good interviews", query));
    ProjectState s = created.state;
    requireValid(s);
    const Bin* bin = s.findBin(created.id);
    REQUIRE(bin);
    REQUIRE(bin->isSearch());
    CHECK(*bin->search == query);

    // Nothing goes into it.
    CHECK(edit::moveToBin(s, created.id, std::vector<BinId>{}, std::vector<MediaId>{tl.media('A')}, std::vector<SequenceId>{})
              .error()
              .code == ErrorCode::InvalidArgument);
    CHECK(edit::addBin(s, "Inside", created.id).error().code == ErrorCode::InvalidArgument);
    MediaItem stray = *s.findMedia(tl.media('A'));
    stray.bin = created.id;
    CHECK(edit::addMedia(s, stray).error().code == ErrorCode::InvalidArgument);
    // A hand-made state with an item inside it is invalid.
    {
        ProjectState bad = s;
        MediaItem moved = *bad.media.front();
        moved.bin = created.id;
        bad.media.front() = std::make_shared<const MediaItem>(std::move(moved));
        CHECK_FALSE(validate(bad).has_value());
    }
    // It moves like a bin and is deleted alone.
    auto folder = okValue(edit::addBin(s, "Selects"));
    s = okValue(edit::moveToBin(folder.state, folder.id, std::vector<BinId>{created.id}, std::vector<MediaId>{},
                                std::vector<SequenceId>{}));
    CHECK(s.findBin(created.id)->parent == folder.id);

    // Edit Search Bin.
    BinSearch other;
    other.text = "B";
    const ProjectState edited = okValue(edit::setBinSearch(s, created.id, other));
    CHECK(edited.findBin(created.id)->search->text == "B");
    CHECK(diff(s, edited).binsChanged);
    CHECK(edit::setBinSearch(s, folder.id, other).error().code == ErrorCode::InvalidArgument);

    // Saved and read back with every field.
    const ProjectState back = roundTrip(s);
    CHECK(*back.findBin(created.id)->search == query);
    // Bad queries are refused, also in files.
    BinSearch badField;
    badField.field = "password";
    CHECK(edit::addSearchBin(tl.state, "x", badField).error().code == ErrorCode::InvalidArgument);
    BinSearch badDate;
    badDate.to = "16/10/2026";
    CHECK(edit::addSearchBin(tl.state, "x", badDate).error().code == ErrorCode::InvalidArgument);
    std::string text = io::toJson(s).value();
    const std::string fieldText = "\"field\": \"scene\"";
    text.replace(text.find(fieldText), fieldText.size(), "\"field\": \"nope\"");
    CHECK_FALSE(io::fromJson(text).has_value());
}

// ---- Freeform view ---------------------------------------------------------------------------------------------

TEST_CASE("Freeform arrangements keep places, stacks and saved layouts", "[model][library][freeform]") {
    Timeline tl("    V1 tgt |AaaaBbbb|\n");
    const MediaId a = tl.media('A');
    const MediaId b = tl.media('B');
    auto binMade = okValue(edit::addBin(tl.state, "Bin"));
    ProjectState s = binMade.state;
    using K = FreeformPlacement::Kind;

    FreeformArrangement arrangement;
    arrangement.scale = 1.5;
    arrangement.items = {placed(K::Media, a.value, 10, 20, 2.0), placed(K::Media, b.value, 12, 22),
                         placed(K::Bin, binMade.id.value, 400, 0), placed(K::Sequence, tl.seq.value, 0, 300),
                         placed(K::Media, 999999, 0, 0)}; // not in the bin: dropped
    s = okValue(edit::setFreeformArrangement(s, BinId{}, arrangement));
    requireValid(s);
    const FreeformView* view = s.findFreeform(BinId{});
    REQUIRE(view);
    CHECK(view->current.scale == 1.5);
    REQUIRE(view->current.items.size() == 4);
    CHECK(view->current.items[1].id == b.value); // B over A: a stack
    CHECK(view->current.items[0].size == 2.0);
    CHECK(diff(binMade.state, s).freeformChanged);
    CHECK(edit::setFreeformArrangement(s, BinId{}, view->current).value() == s); // no change

    // Save, change, restore, delete.
    s = okValue(edit::saveFreeformLayout(s, BinId{}, "Storyboard"));
    FreeformArrangement moved = s.findFreeform(BinId{})->current;
    moved.items[0].x = 900;
    s = okValue(edit::setFreeformArrangement(s, BinId{}, moved));
    CHECK(s.findFreeform(BinId{})->current.items[0].x == 900);
    s = okValue(edit::restoreFreeformLayout(s, BinId{}, "storyboard")); // names ignore case
    CHECK(s.findFreeform(BinId{})->current.items[0].x == 10);
    CHECK(edit::restoreFreeformLayout(s, BinId{}, "Nope").error().code == ErrorCode::NotFound);
    const ProjectState back = roundTrip(s);
    CHECK(back.freeform == s.freeform);
    s = okValue(edit::removeFreeformLayout(s, BinId{}, "STORYBOARD"));
    CHECK(s.findFreeform(BinId{})->layouts.empty());

    // Out of range values are refused, in edits and in files.
    FreeformArrangement wrong = arrangement;
    wrong.items[0].size = 9.0;
    CHECK(edit::setFreeformArrangement(s, BinId{}, wrong).error().code == ErrorCode::InvalidArgument);
    std::string text = io::toJson(back).value();
    const std::string scale = "\"scale\": 1.5";
    text.replace(text.find(scale), scale.size(), "\"scale\": 50.0");
    CHECK_FALSE(io::fromJson(text).has_value());

    // A search bin has no Freeform view; deleting a bin deletes its view.
    auto search = okValue(edit::addSearchBin(s, "Find", BinSearch{}));
    CHECK(edit::setFreeformArrangement(search.state, search.id, {}).error().code == ErrorCode::InvalidArgument);
    s = okValue(edit::setFreeformArrangement(s, binMade.id, FreeformArrangement{}));
    REQUIRE(s.findFreeform(binMade.id));
    s = okValue(edit::removeBins(s, std::vector<BinId>{binMade.id}));
    CHECK_FALSE(s.findFreeform(binMade.id));
    requireValid(s);
}

TEST_CASE("Reset to Grid and Align to Grid place items on the grid of the clip size", "[model][library][freeform]") {
    using K = FreeformPlacement::Kind;
    const std::vector<FreeformPlacement> items{placed(K::Bin, 5, 0, 0), placed(K::Sequence, 6, 0, 0),
                                               placed(K::Media, 7, 0, 0), placed(K::Media, 8, 0, 0)};
    const FreeformArrangement grid = edit::freeformGrid(items, 3, 2.0);
    CHECK(grid.scale == 2.0);
    REQUIRE(grid.items.size() == 4);
    CHECK(grid.items[1].x == edit::kFreeformCellWidth * 2.0);
    CHECK(grid.items[2].x == edit::kFreeformCellWidth * 4.0);
    CHECK(grid.items[3].x == 0.0);
    CHECK(grid.items[3].y == edit::kFreeformCellHeight * 2.0);

    FreeformArrangement loose;
    loose.items = {placed(K::Media, 7, edit::kFreeformCellWidth * 0.6, 10), placed(K::Media, 8, 5, 5)};
    const FreeformArrangement aligned = edit::alignFreeformToGrid(loose);
    CHECK(aligned.items[0].x == edit::kFreeformCellWidth);
    CHECK(aligned.items[0].y == 0.0);
    CHECK(aligned.items[1].x == 0.0);
    const FreeformPlacement only = placed(K::Media, 8, 0, 0);
    const FreeformArrangement one = edit::alignFreeformToGrid(loose, std::span(&only, 1));
    CHECK(one.items[0].x == loose.items[0].x); // not selected: stays
    CHECK(one.items[1].x == 0.0);

    // The bin's items, in the Library's unsorted order: bins, sequences, media, each by name.
    Timeline tl("    V1 tgt |AaaaBbbb|\n");
    auto bin = okValue(edit::addBin(tl.state, "Zed"));
    const std::vector<FreeformPlacement> of = edit::freeformItemsOf(bin.state, BinId{});
    REQUIRE(of.size() >= 3);
    CHECK(of.front().kind == K::Bin);
    CHECK(of[1].kind == K::Sequence);
    CHECK(of[2].kind == K::Media);
}

TEST_CASE("Renumbering ids keeps Freeform placements and clip groups pointing at their items", "[model][library][freeform][merge]") {
    Timeline tl("    V1 tgt |AaaaBbbb|\n");
    using K = FreeformPlacement::Kind;
    FreeformArrangement a;
    a.items = {placed(K::Media, tl.media('A').value, 1, 2)};
    ProjectState s = okValue(edit::setFreeformArrangement(tl.state, BinId{}, a));
    const ProjectState renumbered = renumberIdsFrom(s, 1, 1000);
    const FreeformView* view = renumbered.findFreeform(BinId{});
    REQUIRE(view);
    const MediaItem* moved = renumbered.findMedia(MediaId{view->current.items.front().id});
    REQUIRE(moved);
    CHECK(moved->name == s.findMedia(tl.media('A'))->name);
    requireValid(renumbered);
}

// ---- Import from another project -------------------------------------------------------------------------------

TEST_CASE("Importing a sequence brings what it plays and reuses media the project has", "[model][library][import]") {
    // The other project: a sequence of A and B, a nested sequence playing C, an unused D.
    Timeline other(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    ProjectState source = other.state;
    source = withFile(source, other.media('A'), "D:/Footage/A.mov", "aaaa");
    source = withFile(source, other.media('B'), "E:/Elsewhere/B.mov", "bbbb");
    auto nest = edit::nestClips(source, other.seq, std::vector<ClipId>{other.clip("V1@4")}, "Nest");
    REQUIRE(nest);
    source = nest->edit.state;
    requireValid(source);

    // This project already has A's file.
    Timeline here("    V1 tgt |Zzzz|\n");
    ProjectState target = withFile(here.state, here.media('A'), "D:/Footage/A.mov", "aaaa");
    const std::size_t mediaBefore = target.media.size();

    edit::ProjectImportSelection selection;
    selection.sequences = {other.seq};
    edit::ProjectImportOptions options;
    options.binName = "From Doc";
    const edit::ProjectImportResult result = okValue(edit::importProjectItems(target, source, selection, options));
    requireValid(result.state);
    CHECK(result.reusedMedia == 1);
    CHECK(result.sequences.size() == 2); // the sequence and the nest it plays
    REQUIRE(result.bin.isValid());
    CHECK(result.state.findBin(result.bin)->name == "From Doc");
    // Every copied sequence went into the new bin; the copies play this project's A.
    for (const SequenceId id : result.sequences) {
        const Sequence* seq = result.state.findSequence(id);
        REQUIRE(seq);
        CHECK(seq->bin == result.bin);
    }
    const Sequence* main = nullptr;
    for (const SequenceId id : result.sequences) {
        if (result.state.findSequence(id)->name == source.findSequence(other.seq)->name) {
            main = result.state.findSequence(id);
        }
    }
    REQUIRE(main);
    bool playsA = false;
    bool playsNest = false;
    main->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            playsA = playsA || c.media == here.media('A');
            if (c.isNested()) {
                playsNest = std::find(result.sequences.begin(), result.sequences.end(), c.sequence) != result.sequences.end();
            }
        }
    });
    CHECK(playsA);
    CHECK(playsNest);
    // B (and whatever the nest plays) were copied; the unused media of the other project were not.
    CHECK(result.state.media.size() == mediaBefore + result.media.size());
    CHECK(std::none_of(result.media.begin(), result.media.end(),
                       [&](MediaId m) { return result.state.findMedia(m)->locator.path == "D:/Footage/A.mov"; }));
    // Every new id is above the old generator.
    for (const MediaId m : result.media) {
        CHECK(m.value >= target.ids.next);
    }

    // Without reuse every needed item is copied.
    options.reuseMedia = false;
    const edit::ProjectImportResult copied = okValue(edit::importProjectItems(target, source, selection, options));
    CHECK(copied.reusedMedia == 0);
    CHECK(copied.media.size() == result.media.size() + 1);
}

TEST_CASE("Importing bins brings their contents and keeps their tree", "[model][library][import]") {
    Timeline other("    V1 tgt |AaaaBbbb|\n");
    ProjectState source = other.state;
    auto footage = okValue(edit::addBin(source, "Footage"));
    auto day = okValue(edit::addBin(footage.state, "Day 1", footage.id));
    source = okValue(edit::moveToBin(day.state, day.id, std::vector<BinId>{}, std::vector<MediaId>{other.media('C')},
                                     std::vector<SequenceId>{}));
    // C's file is not in this project (the fixture's media share paths, so give it its own).
    source = withFile(source, other.media('C'), "E:/Elsewhere/C.mov", "cccc");
    Timeline here("    V1 tgt |Zzzz|\n");
    auto into = okValue(edit::addBin(here.state, "Imports"));

    edit::ProjectImportSelection selection;
    selection.bins = {footage.id};
    edit::ProjectImportOptions options;
    options.into = into.id;
    const edit::ProjectImportResult result = okValue(edit::importProjectItems(into.state, source, selection, options));
    requireValid(result.state);
    REQUIRE(result.bins.size() == 2);
    const Bin* top = result.state.findBin(result.bins[0]);
    const Bin* inner = result.state.findBin(result.bins[1]);
    REQUIRE(top);
    REQUIRE(inner);
    CHECK(top->name == "Footage");
    CHECK(top->parent == into.id);
    CHECK(inner->parent == top->id);
    REQUIRE(result.media.size() == 1);
    CHECK(result.state.findMedia(result.media.front())->bin == inner->id);
    CHECK(result.sequences.empty());
    CHECK(result.reusedMedia == 0);
    // Imported again, C's file is in the project now: no second item of it.
    const edit::ProjectImportResult again = okValue(edit::importProjectItems(result.state, source, selection, options));
    CHECK(again.media.empty());
    CHECK(again.reusedMedia == 1);

    // Errors: an id the other project does not have, nothing selected, a search bin as the target.
    edit::ProjectImportSelection missing;
    missing.sequences = {SequenceId{424242}};
    CHECK(edit::importProjectItems(here.state, source, missing).error().code == ErrorCode::NotFound);
    CHECK(edit::importProjectItems(here.state, source, {}).error().code == ErrorCode::InvalidArgument);
    auto search = okValue(edit::addSearchBin(here.state, "Find", BinSearch{}));
    edit::ProjectImportOptions intoSearch;
    intoSearch.into = search.id;
    CHECK(edit::importProjectItems(search.state, source, selection, intoSearch).error().code == ErrorCode::InvalidArgument);

    // Everything: the whole project.
    edit::ProjectImportSelection all;
    all.everything = true;
    const edit::ProjectImportResult whole = okValue(edit::importProjectItems(here.state, source, all));
    requireValid(whole.state);
    CHECK(whole.sequences.size() == source.sequences.size());
    CHECK(whole.bins.size() == source.bins.size());
}

// ---- Follow-up: Status and camera fields, import provenance, several projects in one session --------------------

TEST_CASE("Status and the camera fields are clip metadata too", "[model][library][metadata]") {
    Timeline tl("    V1 tgt |Aaaa|\n");
    const MediaId a = tl.media('A');
    edit::MediaAttributeChange change;
    change.status = "Approved";
    change.cameraAngle = "Wide";
    change.cameraLabel = "A cam";
    change.cameraModel = "FX6";
    change.shotDate = "2026-10-16T09:30:00";
    change.shotLocation = "Kitchen";
    const ProjectState s = okValue(edit::setMediaAttributes(tl.state, std::vector<MediaId>{a}, change));
    requireValid(s);
    const ClipMetadata& md = s.findMedia(a)->metadata;
    CHECK(md.status == "Approved");
    CHECK(md.cameraModel == "FX6");
    CHECK_FALSE(md.isEmpty());
    const ProjectState back = roundTrip(s);
    CHECK(back.findMedia(a)->metadata == md);
    edit::MediaAttributeChange tooLong;
    tooLong.shotLocation = std::string(ClipMetadata::kMaxFieldBytes + 1, 'x');
    CHECK(edit::setMediaAttributes(s, std::vector<MediaId>{a}, tooLong).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Imported copies remember their source project and item", "[model][library][import]") {
    Timeline other("    V1 tgt |Aaaa|\n");
    ProjectState source = withFile(other.state, other.media('A'), "E:/Other/A.mov", "aaaaaaaaaaaaaaaa");
    Timeline here("    V1 tgt |Zzzz|\n");
    edit::ProjectImportSelection selection;
    selection.sequences = {other.seq};
    edit::ProjectImportOptions options;
    options.sourceProject = "D:/Productions/Doc/Interviews.wabcut";
    const edit::ProjectImportResult result = okValue(edit::importProjectItems(here.state, source, selection, options));
    REQUIRE(result.media.size() == 1);
    REQUIRE(result.sequences.size() == 1);
    const MediaItem& copy = *result.state.findMedia(result.media.front());
    REQUIRE(copy.importSource);
    CHECK(copy.importSource->project == options.sourceProject);
    CHECK(copy.importSource->id == other.media('A').value); // the id over there, not renumbered
    const Sequence& seq = *result.state.findSequence(result.sequences.front());
    REQUIRE(seq.importSource);
    CHECK(seq.importSource->id == other.seq.value);
    const ProjectState back = roundTrip(result.state);
    CHECK(back.findMedia(copy.id)->importSource == copy.importSource);
    CHECK(back.findSequence(seq.id)->importSource == seq.importSource);
    // Without a source path nothing is recorded.
    options.sourceProject.clear();
    const edit::ProjectImportResult plain = okValue(edit::importProjectItems(here.state, source, selection, options));
    CHECK_FALSE(plain.state.findMedia(plain.media.front())->importSource.has_value());
}

TEST_CASE("A parked project comes back with its undo and redo history", "[model][session][park]") {
    Timeline first("    V1 tgt |AaaaBbbb|\n");
    Timeline second("    V1 tgt |Cccc|\n");
    EditSession session(first.state);
    std::vector<ChangeKind> kinds;
    session.addListener([&](const ChangeEvent& e) { kinds.push_back(e.kind); });
    REQUIRE(session.apply("Rename", [&](const ProjectState& s) {
        edit::MediaAttributeChange c;
        c.name = "Interview";
        return edit::setMediaAttributes(s, std::vector<MediaId>{first.media('A')}, c);
    }));
    REQUIRE(session.apply("Label", [&](const ProjectState& s) {
        edit::MediaAttributeChange c;
        c.colorIndex = 3;
        return edit::setMediaAttributes(s, std::vector<MediaId>{first.media('A')}, c);
    }));
    REQUIRE(session.undo());
    const ProjectSnapshot edited = session.snapshot();

    // Park the first project, work on the second.
    EditSession::Parked parked = session.park(second.state);
    CHECK(parked.undoCount() == 1);
    CHECK(parked.redoCount() == 1);
    CHECK(parked.snapshot() == edited);
    CHECK_FALSE(session.canUndo());
    CHECK(kinds.back() == ChangeKind::Reset);
    REQUIRE(session.apply("Label C", [&](const ProjectState& s) {
        edit::MediaAttributeChange c;
        c.colorIndex = 5;
        return edit::setMediaAttributes(s, std::vector<MediaId>{second.media('C')}, c);
    }));
    EditSession::Parked secondParked = session.park();

    // Back to the first: the same snapshot, its undo and redo steps.
    session.restore(std::move(parked));
    CHECK(session.snapshot() == edited);
    CHECK(kinds.back() == ChangeKind::Reset);
    CHECK(session.undoLabel() == "Rename");
    CHECK(session.redoLabel() == "Label");
    REQUIRE(session.redo());
    CHECK(session.snapshot()->findMedia(first.media('A'))->colorIndex == 3);
    // Exchanged in one step: one Reset, from the first to the second, and the first is parked again.
    const std::size_t before = kinds.size();
    const ProjectSnapshot firstNow = session.snapshot();
    EditSession::Parked firstAgain = session.exchange(std::move(secondParked));
    CHECK(kinds.size() == before + 1);
    CHECK(firstAgain.snapshot() == firstNow);
    CHECK(session.undoLabel() == "Label C");
    secondParked = session.exchange(std::move(firstAgain));
    CHECK(session.snapshot() == firstNow);
    // And the second one, with its own step.
    session.restore(std::move(secondParked));
    CHECK(session.snapshot()->findMedia(second.media('C'))->colorIndex == 5);
    CHECK(session.undoLabel() == "Label C");
    REQUIRE(session.undo());
    CHECK(session.snapshot()->findMedia(second.media('C'))->colorIndex == 0);
}
