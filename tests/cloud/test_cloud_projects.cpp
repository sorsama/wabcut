// Cloud projects (docs/cloud.md): the editing lock, revisions, push / pull / fast-forward / merge, races for head.json, divergent
// lines, damaged and hostile data. Everything runs against an in-memory store (and a fake S3 for the store check); no network.

#include "FakeServers.h"
#include "TimelineFixture.h"

#include <wabcut/cloud/CloudLock.h>
#include <wabcut/cloud/CloudProject.h>
#include <wabcut/cloud/HttpStores.h>
#include <wabcut/cloud/RemoteStore.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ReviewNotes.h>
#include <wabcut/model/ReviewOps.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace wabcut;
using namespace wabcut::cloud;
using namespace wabcut::cloud::test;
namespace fs = std::filesystem;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaa....Bbbb|
    A1 tgt |Aaaa....Bbbb|
)";

Identity person(const char* user, const char* host, const char* session) { return Identity{user, host, session}; }

/// A clock the test moves.
struct Clock {
    std::int64_t now = 1793874937;
    UnixClock fn() { return [this] { return now; }; }
};

ProjectState baseProject() { return wabcut::test::Timeline(kPicture).state; }

SequenceId firstSequence(const ProjectState& s) { return s.sequences.front()->id; }

ProjectState withMarker(const ProjectState& s, const char* name, int frame) {
    Marker m;
    m.start = framesToTicks(frame, kRate25);
    m.name = name;
    return wabcut::test::ok(edit::addMarker(s, firstSequence(s), m)).state;
}

ProjectState withNote(const ProjectState& s, const char* text) {
    ReviewNote n;
    n.sequence = firstSequence(s);
    n.start = framesToTicks(2, kRate25);
    n.text = text;
    n.author = "Ann";
    auto added = edit::addReviewNote(s, n);
    REQUIRE(added);
    return added->state;
}

std::size_t markerCount(const ProjectState& s) { return s.sequences.front()->markers.size(); }

struct Machine {
    TempFolder cache;
    TempFolder folder;
    CloudProjects cloud;
    CloudLink link;
    ProjectState state;
    Machine(RemoteStore& store, const char* name, Clock& clock, const char* session)
        : cache(std::string("cache-") + name), folder(std::string("project-") + name),
          cloud(store, person(name, "pc", session), clock.fn(), systemRandom(), "0.7.0") {}

    SyncRequest request(bool mayPush = true) const {
        SyncRequest r;
        r.local = state;
        r.link = link;
        r.cacheFolder = cache.path;
        r.projectFolder = folder.path;
        r.mayPush = mayPush;
        r.pushBlockedBecause = "Pushing is blocked.";
        return r;
    }
    /// Syncs and applies the result like the app does.
    StoreResult<SyncResult> sync(bool mayPush = true) {
        auto result = cloud.sync(request(mayPush));
        if (result) {
            if (result->state) {
                state = *result->state;
            }
            if (result->kind != SyncKind::NeedsChoices && result->kind != SyncKind::PushBlocked) {
                link = result->link;
            }
        }
        return result;
    }
};

} // namespace

TEST_CASE("Cloud lock: one editor at a time renewed stale ones taken over with a warning", "[cloud][lock]") {
    MemoryStore store;
    Clock clock;
    CloudLock ann(store, "proj", person("ann", "pc-ann", "aaaaaaaaaaaaaaaa"), "0.7.0", clock.fn());
    CloudLock bob(store, "proj", person("bob", "pc-bob", "bbbbbbbbbbbbbbbb"), "0.7.0", clock.fn());

    auto first = ann.acquire();
    REQUIRE(first);
    CHECK(first->kind == LockOutcome::Kind::Acquired);
    CHECK(ann.held());

    auto refused = bob.acquire();
    REQUIRE(refused);
    CHECK(refused->kind == LockOutcome::Kind::HeldByOther);
    CHECK(refused->other.user == "ann");
    CHECK(refused->other.host == "pc-ann");
    CHECK_FALSE(bob.held());

    // renewing keeps it fresh
    clock.now += 14 * 60;
    REQUIRE(ann.refresh());
    clock.now += 14 * 60;
    CHECK(bob.acquire()->kind == LockOutcome::Kind::HeldByOther);

    // not renewed for 15 minutes: stale. Bob takes it over and is told whose it was.
    clock.now += 16 * 60;
    auto taken = bob.acquire();
    REQUIRE(taken);
    CHECK(taken->kind == LockOutcome::Kind::TookOver);
    CHECK(taken->other.user == "ann");
    CHECK(taken->warning.find("ann on pc-ann") != std::string::npos);
    CHECK(bob.held());
    // Ann's next renewal fails and she knows she lost it
    auto lost = ann.refresh();
    REQUIRE_FALSE(lost);
    CHECK(lost.error().kind == StoreFailure::PreconditionFailed);
    CHECK_FALSE(ann.held());
    // she does not delete Bob's lock when she gives up
    REQUIRE(ann.release());
    auto stored = bob.peek();
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    CHECK((*stored)->user == "bob");
    CHECK((*stored)->tookOverFrom.find("ann on pc-ann") != std::string::npos);

    REQUIRE(bob.release());
    CHECK_FALSE(bob.peek()->has_value());
    CHECK(store.peek("proj/lock.json").empty());
}

TEST_CASE("Cloud lock: a heartbeat ahead of this clock is a live lock but three days ahead is a damaged one", "[cloud][lock]") {
    MemoryStore store;
    Clock annClock;
    Clock bobClock;
    annClock.now = bobClock.now + 90; // Ann's clock runs fast
    CloudLock ann(store, "proj", person("ann", "pc-ann", "aaaaaaaaaaaaaaaa"), "0.7.0", annClock.fn());
    CloudLock bob(store, "proj", person("bob", "pc-bob", "bbbbbbbbbbbbbbbb"), "0.7.0", bobClock.fn());
    REQUIRE(ann.acquire());
    auto refused = bob.acquire();
    REQUIRE(refused);
    CHECK(refused->kind == LockOutcome::Kind::HeldByOther); // not taken over as stale
    CHECK(refused->ageSeconds == 0);
    CHECK(ann.held());
    REQUIRE(ann.refresh()); // Ann's next renewal still works: nothing was taken

    // A lock written for the year 9999 is no clock skew: it can be taken over.
    annClock.now = bobClock.now + 3 * 24 * 60 * 60;
    REQUIRE(ann.refresh());
    auto taken = bob.acquire();
    REQUIRE(taken);
    CHECK(taken->kind == LockOutcome::Kind::TookOver);
}

TEST_CASE("Cloud lock: the lock an earlier start of the same person's program left is taken at once", "[cloud][lock]") {
    MemoryStore store;
    Clock clock;
    CloudLock before(store, "proj", person("ann", "pc-ann", "aaaaaaaaaaaaaaaa"), "0.7.0", clock.fn());
    REQUIRE(before.acquire());
    // Wabcut crashed (no release); a minute later Ann starts it again on the same computer.
    clock.now += 60;
    CloudLock after(store, "proj", person("ann", "pc-ann", "cccccccccccccccc"), "0.7.0", clock.fn());
    auto taken = after.acquire();
    REQUIRE(taken);
    CHECK(taken->kind == LockOutcome::Kind::TookOver);
    CHECK(after.held());
    // Someone else on another computer still waits for it.
    CloudLock bob(store, "proj", person("bob", "pc-bob", "bbbbbbbbbbbbbbbb"), "0.7.0", clock.fn());
    CHECK(bob.acquire()->kind == LockOutcome::Kind::HeldByOther);
    // And Ann on a second computer is another editor too.
    CloudLock laptop(store, "proj", person("ann", "laptop", "dddddddddddddddd"), "0.7.0", clock.fn());
    CHECK(laptop.acquire()->kind == LockOutcome::Kind::HeldByOther);
}

TEST_CASE("Cloud lock: two editors starting together a damaged lock an unreachable store", "[cloud][lock]") {
    Clock clock;
    SECTION("the one who writes second finds the other's lock") {
        MemoryStore store;
        CloudLock ann(store, "proj", person("ann", "pc-ann", "aaaaaaaaaaaaaaaa"), "", clock.fn());
        CloudLock bob(store, "proj", person("bob", "pc-bob", "bbbbbbbbbbbbbbbb"), "", clock.fn());
        bool once = true;
        store.beforeWrite = [&](std::string_view op, std::string_view path) {
            if (once && op == "put" && path == "proj/lock.json") {
                once = false;
                REQUIRE(bob.acquire()); // Bob's create-if-absent lands just before Ann's
            }
        };
        auto outcome = ann.acquire();
        REQUIRE(outcome);
        CHECK(outcome->kind == LockOutcome::Kind::HeldByOther);
        CHECK(outcome->other.user == "bob");
        CHECK_FALSE(ann.held());
        CHECK(bob.held());
    }
    SECTION("a damaged lock file counts as stale") {
        MemoryStore store;
        REQUIRE(store.put("proj/lock.json", "not json at all", Precondition::none()));
        CloudLock bob(store, "proj", person("bob", "pc-bob", "bbbbbbbbbbbbbbbb"), "", clock.fn());
        auto outcome = bob.acquire();
        REQUIRE(outcome);
        CHECK(outcome->kind == LockOutcome::Kind::TookOver);
        CHECK(bob.held());
    }
    SECTION("a hostile lock file is bounded") {
        CHECK_FALSE(parseLock(std::string(100 * 1024, ' ')).has_value());
        CHECK_FALSE(parseLock("{\"format\":\"wabcut-cloud-lock\",\"session\":\"\"}").has_value());
        auto info = parseLock("{\"format\":\"wabcut-cloud-lock\",\"session\":\"abc\",\"user\":\"" + std::string(5000, 'u') + "\"}");
        REQUIRE(info);
        CHECK(info->user.size() == 256);
    }
    SECTION("a store that cannot be reached is an error, not a lock") {
        MemoryStore store;
        store.failNext("get", "", StoreError{StoreFailure::Network, "no connection"});
        CloudLock ann(store, "proj", person("ann", "pc-ann", "aaaaaaaaaaaaaaaa"), "", clock.fn());
        auto outcome = ann.acquire();
        REQUIRE_FALSE(outcome);
        CHECK(outcome.error().kind == StoreFailure::Network);
        CHECK_FALSE(ann.held());
    }
}

TEST_CASE("A cloud project: create open on another computer push pull", "[cloud][project]") {
    MemoryStore store;
    Clock clock;
    Machine ann(store, "ann", clock, "aaaaaaaaaaaaaaaa");
    Machine bob(store, "bob", clock, "bbbbbbbbbbbbbbbb");
    ann.state = baseProject();

    auto created = ann.cloud.create(ann.state, "Film", "loc1", ann.cache.path);
    REQUIRE(created);
    ann.link = created->link;
    CHECK(created->revision.revision == 1);
    CHECK(created->revision.parentSha.empty());
    CHECK(created->revision.savedBy == "ann on pc");
    CHECK(created->link.projectId.size() == 26);
    CHECK(store.peek(created->link.projectId + "/head.json").find("\"revision\": 1") != std::string::npos);

    auto listed = bob.cloud.list();
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK((*listed)[0].projectId == created->link.projectId);
    CHECK((*listed)[0].head.sha256 == created->revision.sha256);

    // Bob opens it: the same project, its media offline until he finds it
    std::size_t quarantined = 99;
    auto opened = bob.cloud.open(created->link.projectId, "loc1", bob.folder.path, bob.cache.path, &quarantined);
    REQUIRE(opened);
    bob.state = opened->first;
    bob.link = opened->second;
    CHECK(quarantined == 0);
    CHECK(io::toJson(normalizeForSync(bob.state)).value() == io::toJson(normalizeForSync(ann.state)).value());
    for (const MediaPtr& m : bob.state.media) {
        CHECK(m->locator.offline);
    }
    CHECK(bob.link.baseRevision == 1);

    // nothing changed anywhere
    CHECK(ann.sync()->kind == SyncKind::UpToDate);
    CHECK(bob.sync()->kind == SyncKind::UpToDate);

    // Ann edits and pushes
    ann.state = withMarker(ann.state, "Title card", 10);
    auto pushed = ann.sync();
    REQUIRE(pushed);
    CHECK(pushed->kind == SyncKind::Pushed);
    REQUIRE(pushed->pushed);
    CHECK(pushed->pushed->revision == 2);
    CHECK(pushed->pushed->parentSha == created->revision.sha256);
    CHECK(ann.link.baseRevision == 2);
    CHECK(ann.sync()->kind == SyncKind::UpToDate);

    // Bob's unchanged copy fast-forwards, keeps his own media paths, and gets the new revision for his history
    for (const MediaPtr& m : bob.state.media) {
        CHECK(m->locator.path.find("D:/footage/") == 0); // (the fixture's paths came with the project)
    }
    auto pulled = bob.sync();
    REQUIRE(pulled);
    CHECK(pulled->kind == SyncKind::FastForwarded);
    CHECK(markerCount(bob.state) == 1);
    REQUIRE(pulled->newRevisions.size() == 1);
    CHECK(pulled->newRevisions[0].revision == 2);
    CHECK_FALSE(pulled->changes.empty());
    CHECK(bob.link.baseRevision == 2);
    CHECK(bob.sync()->kind == SyncKind::UpToDate);

    // the cloud's own history
    auto revisions = bob.cloud.revisions(created->link.projectId);
    REQUIRE(revisions);
    REQUIRE(revisions->size() == 2);
    CHECK((*revisions)[0].revision == 2);
    CHECK((*revisions)[1].revision == 1);
    auto fetched = bob.cloud.fetch(created->link.projectId, (*revisions)[1], bob.cache.path);
    REQUIRE(fetched);
    CHECK(markerCount(*fetched) == 0);
    CHECK(markerCount(bob.state) == 1);
}

TEST_CASE("Edits to different parts merge; both sides get both", "[cloud][project]") {
    MemoryStore store;
    Clock clock;
    Machine ann(store, "ann", clock, "aaaaaaaaaaaaaaaa");
    Machine bob(store, "bob", clock, "bbbbbbbbbbbbbbbb");
    ann.state = baseProject();
    auto created = ann.cloud.create(ann.state, "Film", "loc1", ann.cache.path);
    REQUIRE(created);
    ann.link = created->link;
    bob.state = ann.state;
    bob.link = created->link;

    ann.state = withMarker(ann.state, "Ann's marker", 10);
    REQUIRE(ann.sync()->kind == SyncKind::Pushed);
    bob.state = withNote(bob.state, "Bob's note");
    auto merged = bob.sync();
    REQUIRE(merged);
    CHECK(merged->kind == SyncKind::MergedAndPushed);
    CHECK(markerCount(bob.state) == 1);
    CHECK(bob.state.reviewNotes.size() == 1);
    REQUIRE(merged->pushed);
    CHECK(merged->pushed->revision == 3);
    CHECK(merged->pushed->parentRevision == 2);
    CHECK(merged->pushed->secondParentSha.empty());

    auto ann2 = ann.sync();
    REQUIRE(ann2);
    CHECK(ann2->kind == SyncKind::FastForwarded);
    CHECK(markerCount(ann.state) == 1);
    CHECK(ann.state.reviewNotes.size() == 1);
    CHECK(ann.sync()->kind == SyncKind::UpToDate);
}

TEST_CASE("The same thing changed on both sides asks; nothing is overwritten until the user chooses", "[cloud][project]") {
    MemoryStore store;
    Clock clock;
    Machine ann(store, "ann", clock, "aaaaaaaaaaaaaaaa");
    Machine bob(store, "bob", clock, "bbbbbbbbbbbbbbbb");
    ann.state = baseProject();
    auto created = ann.cloud.create(ann.state, "Film", "loc1", ann.cache.path);
    REQUIRE(created);
    ann.link = created->link;
    bob.state = ann.state;
    bob.link = created->link;

    ann.state = withMarker(ann.state, "From Ann", 10);
    REQUIRE(ann.sync()->kind == SyncKind::Pushed);
    const std::string headBefore = store.peek(created->link.projectId + "/head.json");
    bob.state = withMarker(bob.state, "From Bob", 8);

    auto asked = bob.sync();
    REQUIRE(asked);
    CHECK(asked->kind == SyncKind::NeedsChoices);
    REQUIRE_FALSE(asked->conflicts.empty());
    CHECK_FALSE(asked->state.has_value());
    CHECK(store.peek(created->link.projectId + "/head.json") == headBefore); // nothing was written
    CHECK(markerCount(bob.state) == 1);
    CHECK(bob.state.sequences.front()->markers[0].name == "From Bob");

    // Bob picks his own for every conflict: his version goes up as the next revision
    SyncRequest mine = bob.request();
    for (const MergeConflict& c : asked->conflicts) {
        mine.choices[c.id] = MergeChoice::Mine;
    }
    auto kept = bob.cloud.sync(mine);
    REQUIRE(kept);
    CHECK(kept->kind == SyncKind::MergedAndPushed);
    REQUIRE(kept->state);
    CHECK(kept->state->sequences.front()->markers[0].name == "From Bob");
    CHECK(kept->pushed->revision == 3);

    // Ann takes the cloud's on a second round by asking for all of theirs
    ann.state = withMarker(ann.state, "Ann again", 6);
    SyncRequest theirs = ann.request();
    theirs.takeAllTheirs = true;
    auto taken = ann.cloud.sync(theirs);
    REQUIRE(taken);
    CHECK(taken->kind == SyncKind::MergedAndPushed);
    REQUIRE(taken->state);
    CHECK(taken->state->sequences.front()->markers[0].name == "From Bob");
}

TEST_CASE("Losing the race for head.json starts again from what the other editor saved", "[cloud][project]") {
    MemoryStore store;
    Clock clock;
    Machine ann(store, "ann", clock, "aaaaaaaaaaaaaaaa");
    Machine bob(store, "bob", clock, "bbbbbbbbbbbbbbbb");
    ann.state = baseProject();
    auto created = ann.cloud.create(ann.state, "Film", "loc1", ann.cache.path);
    REQUIRE(created);
    ann.link = created->link;
    bob.state = ann.state;
    bob.link = created->link;
    const std::string id = created->link.projectId;

    // Bob pushes; just before his write to head.json Ann pushes her own edit
    bob.state = withNote(bob.state, "Bob's note");
    ann.state = withMarker(ann.state, "Ann's marker", 10);
    bool once = true;
    store.beforeWrite = [&](std::string_view op, std::string_view path) {
        if (once && op == "put" && path == id + "/head.json") {
            once = false;
            REQUIRE(ann.sync()->kind == SyncKind::Pushed);
        }
    };
    auto result = bob.sync();
    REQUIRE(result);
    CHECK(result->kind == SyncKind::MergedAndPushed);
    CHECK(markerCount(bob.state) == 1);
    CHECK(bob.state.reviewNotes.size() == 1);
    // numbering never reused a number: Bob's lost attempt took 2 (its file stays), Ann's push is 3, Bob's merge is 4
    REQUIRE(result->pushed);
    CHECK(result->pushed->revision == 4);
    auto leftover = bob.cloud.unmerged(id);
    REQUIRE(leftover);
    REQUIRE(leftover->size() == 1);
    CHECK((*leftover)[0].revision == 2);
    CHECK((*leftover)[0].savedBy == "bob on pc");
}

TEST_CASE("Two computers that wrote the same revision are merged not overwritten", "[cloud][project]") {
    // What a sync client does with two head.json files: the second one wins, the first computer's revision is left orphaned.
    MemoryStore store;
    Clock clock;
    Machine ann(store, "ann", clock, "aaaaaaaaaaaaaaaa");
    Machine bob(store, "bob", clock, "bbbbbbbbbbbbbbbb");
    ann.state = baseProject();
    auto created = ann.cloud.create(ann.state, "Film", "loc1", ann.cache.path);
    REQUIRE(created);
    ann.link = created->link;
    bob.state = ann.state;
    bob.link = created->link;
    const std::string id = created->link.projectId;
    const std::string head1 = store.peek(id + "/head.json");

    ann.state = withMarker(ann.state, "Ann's marker", 10);
    REQUIRE(ann.sync()->kind == SyncKind::Pushed); // revision 2 by Ann
    REQUIRE(store.put(id + "/head.json", head1, Precondition::none())); // the sync client brings back the old head.json
    bob.state = withNote(bob.state, "Bob's note");
    auto bobPush = bob.sync();
    REQUIRE(bobPush);
    CHECK(bobPush->kind == SyncKind::Pushed);
    CHECK(bobPush->pushed->revision == 3); // 2 is taken by Ann's file: max + 1

    // Ann's computer: head is Bob's revision 3, she is on her revision 2 and has not changed anything since
    auto annSync = ann.sync();
    REQUIRE(annSync);
    CHECK(annSync->kind == SyncKind::MergedAndPushed); // not a fast-forward: her revision is not an ancestor of the head
    CHECK(markerCount(ann.state) == 1);
    CHECK(ann.state.reviewNotes.size() == 1);
    REQUIRE(annSync->pushed);
    CHECK(annSync->pushed->revision == 4);
    CHECK(annSync->pushed->parentRevision == 3);
    CHECK_FALSE(annSync->pushed->secondParentSha.empty()); // her own line is recorded as merged in
    auto leftover = ann.cloud.unmerged(id);
    REQUIRE(leftover);
    CHECK(leftover->empty());
}

TEST_CASE("Damaged and hostile cloud data is refused without changing anything", "[cloud][project]") {
    MemoryStore store;
    Clock clock;
    Machine ann(store, "ann", clock, "aaaaaaaaaaaaaaaa");
    ann.state = baseProject();
    auto created = ann.cloud.create(ann.state, "Film", "loc1", ann.cache.path);
    REQUIRE(created);
    ann.link = created->link;
    const std::string id = created->link.projectId;
    const std::string head = store.peek(id + "/head.json");

    SECTION("head.json missing next to revisions is damage, not a new project") {
        REQUIRE(store.remove(id + "/head.json"));
        auto r = ann.cloud.head(id);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("missing") != std::string::npos);
        ann.state = withMarker(ann.state, "x", 5);
        CHECK_FALSE(ann.sync());
        CHECK(store.peek(id + "/head.json").empty()); // nothing was written
    }
    SECTION("a revision whose bytes changed on the storage") {
        auto h = ann.cloud.head(id);
        REQUIRE(h);
        std::string bytes = store.peek(id + "/" + h->file);
        bytes[bytes.size() / 2] = bytes[bytes.size() / 2] == 'x' ? 'y' : 'x';
        REQUIRE(store.put(id + "/" + h->file, bytes, Precondition::none()));
        TempFolder other("other-cache");
        auto fetched = ann.cloud.fetch(id, *h, other.path);
        REQUIRE_FALSE(fetched);
        CHECK(fetched.error().message.find("checksum") != std::string::npos);
        // and no file of it was left in the cache
        std::size_t files = 0;
        for (const auto& e : fs::directory_iterator(other.path)) {
            (void)e;
            ++files;
        }
        CHECK(files == 0);
    }
    SECTION("a head that points somewhere else, or is huge, or is not one") {
        CHECK_FALSE(parseRevisionJson(std::string(100 * 1024, ' ')));
        CHECK_FALSE(parseRevisionJson("{}"));
        std::string evil = head;
        const std::string name = "revisions/";
        evil.replace(evil.find(name), name.size(), "../../x/");
        CHECK_FALSE(parseRevisionJson(evil));
        std::string badSha = head;
        badSha.replace(badSha.find("\"sha256\": \"") + 11, 1, "Z");
        CHECK_FALSE(parseRevisionJson(badSha));
        REQUIRE(store.put(id + "/head.json", evil, Precondition::none()));
        CHECK_FALSE(ann.cloud.head(id));
    }
    SECTION("a project saved by a newer Wabcut") {
        auto h = ann.cloud.head(id);
        REQUIRE(h);
        RevisionMeta newer = *h;
        newer.schema = io::kSchemaVersion + 1;
        REQUIRE(store.put(id + "/head.json", revisionJson(newer, true), Precondition::none()));
        ann.state = withMarker(ann.state, "x", 5);
        auto r = ann.sync();
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("newer Wabcut") != std::string::npos);
    }
    SECTION("pushing is blocked when the caller says so, and nothing goes up") {
        ann.state = withMarker(ann.state, "x", 5);
        const std::size_t puts = store.calls("put");
        auto r = ann.sync(false);
        REQUIRE(r);
        CHECK(r->kind == SyncKind::PushBlocked);
        CHECK(r->notes.size() == 1);
        CHECK(store.calls("put") == puts);
    }
    SECTION("the link file and the cloud's names are checked") {
        TempFolder dir("link");
        const fs::path project = dir.path / "Film.wabcut";
        CHECK(cloudLinkPath(project).filename() == "Film.wabcut-cloud");
        REQUIRE(writeCloudLink(project, ann.link));
        auto read = readCloudLink(project);
        REQUIRE(read);
        CHECK(*read == ann.link);
        writeAll(cloudLinkPath(project), "{\"format\":\"wabcut-cloud-link\",\"projectId\":\"../../x\"}");
        CHECK_FALSE(readCloudLink(project));
        CHECK_FALSE(ann.cloud.head("../x"));
    }
}

TEST_CASE("Paths of another computer's network share are never kept; machine-local fields are not changes", "[cloud][project]") {
    SECTION("quarantine") {
        ProjectState s = baseProject();
        auto copy = std::make_shared<MediaItem>(*s.media[0]);
        copy->locator.path = "\\\\attacker\\share\\clips\\evil.mov";
        copy->locator.proxy = ProxyMedia{"//attacker/share/proxy.mp4", {}};
        s.media[0] = copy;
        auto ok2 = std::make_shared<MediaItem>(*s.media[1]);
        ok2->locator.path = "\\\\?\\D:\\footage\\fine.mov"; // the local-device prefix is not a share
        s.media[1] = ok2;
        const std::size_t changed = quarantineForeignPaths(s, fs::path("C:/Users/me/Project"));
        CHECK(changed == 1);
        CHECK(s.media[0]->locator.offline);
        CHECK(s.media[0]->locator.path.find("attacker") == std::string::npos);
        CHECK(s.media[0]->locator.path.find("evil.mov") != std::string::npos);
        CHECK_FALSE(s.media[0]->locator.proxy.has_value());
        CHECK(s.media[1]->locator.path == "\\\\?\\D:\\footage\\fine.mov");
        CHECK(io::toJson(s).value().find("attacker") == std::string::npos);
    }
    SECTION("the extended UNC prefix, mixed separators and a rooted relative path are quarantined too") {
        ProjectState s = baseProject();
        REQUIRE(s.media.size() >= 2);
        const char* const hostile[] = {"\\\\?\\UNC\\evil\\share\\a.mp4", "\\\\.\\unc\\evil\\share\\a.mp4", "/\\evil\\share\\a.mp4",
                                       "\\/evil\\share\\a.mp4", "//evil/share/a.mp4"};
        for (const char* path : hostile) {
            INFO(path);
            auto copy = std::make_shared<MediaItem>(*s.media[0]);
            copy->locator.path = path;
            copy->locator.relativePath.clear();
            s.media[0] = copy;
            CHECK(quarantineForeignPaths(s, fs::path("C:/Users/me/Project")) == 1);
            CHECK(s.media[0]->locator.path.find("evil") == std::string::npos);
            CHECK(s.media[0]->locator.offline);
        }
        // A relative path that is rooted, on a share or climbs out of the folder is dropped; a normal one stays.
        const char* const rooted[] = {"\\\\evil\\share\\a.mp4", "/etc/a.mp4", "C:\\a.mp4", "..\\..\\a.mp4", "Media/../../a.mp4",
                                      "\\?\\UNC\\evil\\share\\a.mp4"};
        for (const char* relative : rooted) {
            INFO(relative);
            auto copy = std::make_shared<MediaItem>(*s.media[1]);
            copy->locator.path = "D:/footage/ok.mov";
            copy->locator.relativePath = relative;
            s.media[1] = copy;
            CHECK(quarantineForeignPaths(s, fs::path("C:/Users/me/Project")) == 1);
            CHECK(s.media[1]->locator.relativePath.empty());
            CHECK(s.media[1]->locator.path == "D:/footage/ok.mov");
        }
        auto fine = std::make_shared<MediaItem>(*s.media[1]);
        fine->locator.relativePath = "Media/clips/ok.mov";
        s.media[1] = fine;
        CHECK(quarantineForeignPaths(s, fs::path("C:/Users/me/Project")) == 0);
        CHECK(s.media[1]->locator.relativePath == "Media/clips/ok.mov");
    }
    SECTION("a pulled project from a share arrives quarantined") {
        MemoryStore store;
        Clock clock;
        Machine ann(store, "ann", clock, "aaaaaaaaaaaaaaaa");
        Machine bob(store, "bob", clock, "bbbbbbbbbbbbbbbb");
        ann.state = baseProject();
        auto copy = std::make_shared<MediaItem>(*ann.state.media[0]);
        copy->locator.path = "\\\\studio\\media\\a.mov";
        ann.state.media[0] = copy;
        auto created = ann.cloud.create(ann.state, "Film", "loc1", ann.cache.path);
        REQUIRE(created);
        std::size_t quarantined = 0;
        auto opened = bob.cloud.open(created->link.projectId, "loc1", bob.folder.path, bob.cache.path, &quarantined);
        REQUIRE(opened);
        CHECK(quarantined == 1);
        CHECK(io::toJson(opened->first).value().find("\\\\\\\\studio") == std::string::npos);
    }
    SECTION("normalising") {
        ProjectState a = baseProject();
        ProjectState b = a;
        auto copy = std::make_shared<MediaItem>(*b.media[0]);
        copy->locator.path = "E:/elsewhere/A.mov";
        copy->locator.relativePath = "../A.mov";
        copy->locator.offline = true;
        copy->locator.fingerprint.mtimeNs = 12345;
        copy->locator.proxy = ProxyMedia{"E:/proxies/A.mp4", {}};
        b.media[0] = copy;
        CHECK(io::toJson(a).value() != io::toJson(b).value());
        CHECK(io::toJson(normalizeForSync(a)).value() == io::toJson(normalizeForSync(b)).value());
        // another file (another fingerprint) is a real change
        auto other = std::make_shared<MediaItem>(*b.media[0]);
        other->locator.fingerprint.size = 777;
        b.media[0] = other;
        CHECK(io::toJson(normalizeForSync(a)).value() != io::toJson(normalizeForSync(b)).value());
    }
    SECTION("restoring this computer's own paths after a merge, and the lock only ever turns on") {
        ProjectState mine = baseProject();
        mine.aiNoCloud = true;
        ProjectState theirs = baseProject();
        auto theirsCopy = std::make_shared<MediaItem>(*theirs.media[0]);
        theirsCopy->locator.path = "Z:/their/path.mov";
        theirs.media[0] = theirsCopy;
        const ProjectState merged = restoreLocalState(normalizeForSync(theirs), mine, theirs);
        CHECK(merged.media[0]->locator.path == mine.media[0]->locator.path);
        CHECK(merged.aiNoCloud);
        // an item only the cloud has keeps the cloud's path but is offline
        ProjectState fewer = mine;
        fewer.media.pop_back();
        const ProjectState more = restoreLocalState(normalizeForSync(theirs), fewer, theirs);
        CHECK(more.media.back()->locator.offline);
    }
}

TEST_CASE("Stores are checked for conditional writes before a project is trusted to them", "[cloud][project]") {
    SECTION("a good store passes") {
        MemoryStore store;
        const StoreCheck check = checkStore(store);
        CHECK(check.reachable);
        CHECK(check.conditionalWrites);
        CHECK(check.problems.empty());
        CHECK(store.listAll("")->empty()); // nothing left behind
    }
    SECTION("folder, WebDAV and S3 pass") {
        TempFolder folder("check");
        FolderStore fs1(folder.path / "root");
        CHECK(checkStore(fs1).conditionalWrites);
        FakeDav dav("/dav", "ann", "password-1234");
        WebDavConfig dc;
        dc.url = "https://dav.example.com/dav";
        dc.user = "ann";
        WebDavStore webdav(dav, dc, ai::Secret("password-1234"));
        CHECK(checkStore(webdav).conditionalWrites);
        FakeS3 s3("b", "AKIATESTKEY0001", "secret-secret-secret-1234");
        S3Config sc;
        sc.endpoint = "https://s3.example.com";
        sc.bucket = "b";
        S3Store s3store(s3, sc, ai::Secret("AKIATESTKEY0001"), ai::Secret("secret-secret-secret-1234"));
        CHECK(checkStore(s3store).conditionalWrites);
    }
    SECTION("a store that ignores conditions is found out") {
        struct Careless final : RemoteStore {
            MemoryStore inner;
            std::string describe() const override { return "careless"; }
            std::string kind() const override { return "careless"; }
            StoreResult<std::optional<StoreEntry>> stat(std::string_view p, std::stop_token s) override { return inner.stat(p, s); }
            StoreResult<std::vector<StoreEntry>> list(std::string_view p, std::stop_token s) override { return inner.list(p, s); }
            StoreResult<Fetched> get(std::string_view p, std::uint64_t m, std::stop_token s) override { return inner.get(p, m, s); }
            StoreResult<std::string> getToFile(std::string_view p, const fs::path& f, std::uint64_t m, const Progress& pr, std::stop_token s) override {
                return inner.getToFile(p, f, m, pr, s);
            }
            StoreResult<std::string> put(std::string_view p, std::string_view d, const Precondition&, std::string_view c, std::stop_token s) override {
                return inner.put(p, d, Precondition::none(), c, s); // last writer wins
            }
            StoreResult<std::string> putFile(std::string_view p, const fs::path& f, const Precondition&, std::string_view c, const Progress& pr,
                                             std::stop_token s) override {
                return inner.putFile(p, f, Precondition::none(), c, pr, s);
            }
            StoreResult<void> remove(std::string_view p, std::stop_token s) override { return inner.remove(p, s); }
            StoreResult<void> makeFolder(std::string_view p, std::stop_token s) override { return inner.makeFolder(p, s); }
        } careless;
        const StoreCheck check = checkStore(careless);
        CHECK(check.reachable);
        CHECK_FALSE(check.conditionalWrites);
        CHECK(check.problems.size() == 2);
    }
    SECTION("a store that cannot be written is reported") {
        MemoryStore store;
        store.failNext("put", "", StoreError{StoreFailure::Unauthorized, "denied", 403});
        const StoreCheck check = checkStore(store);
        CHECK_FALSE(check.reachable);
        REQUIRE(check.problems.size() == 1);
        CHECK(check.problems[0].find("denied") != std::string::npos);
    }
}
