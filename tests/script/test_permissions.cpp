// Extension permissions (C.12): grants within what an extension asks for, revoking, per-project trust bound to the
// reviewed content, and crashes that switch a broken extension off.

#include "ScriptTestUtil.h"

#include <wabcut/script/Permissions.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::script;
using namespace wabcut::script::test;
using command::Capability;
using command::CapabilitySet;

namespace {

ExtensionBundle bundleIn(const fs::path& folder, const Json& m, ExtensionScope scope = ExtensionScope::User) {
    writeExtension(folder, m, {{"main.js", "// " + m["version"].get<std::string>()}});
    auto bundle = loadExtension(folder, scope);
    REQUIRE(bundle.has_value());
    return *bundle;
}

} // namespace

TEST_CASE("Grants stay within what the extension asks for; a new version asking for more needs a review", "[script][permissions]") {
    const fs::path dir = scratch("grants");
    const ExtensionBundle v1 = bundleIn(dir / "ext", manifest("com.example.ext", "Example", {"Read", "EditSequence"}));
    PermissionStore store = PermissionStore::load(dir / "permissions.json");
    CHECK(store.needsReview(v1));
    CHECK_FALSE(store.isEnabled(v1.manifest.id));
    CHECK(store.effective(v1).bits == 0);

    // The dialog lists what it asks for, with what each allows.
    const ExtensionReview review = store.review(v1);
    REQUIRE(review.requests.size() == 2);
    CHECK(review.requests[0].info.capability == Capability::Read);
    CHECK_FALSE(review.requests[0].info.explanation.empty());
    CHECK_FALSE(review.requests[1].granted);
    CHECK_FALSE(review.update);
    CHECK_FALSE(review.needsTrust);

    // More than it asks for is refused; less is fine.
    CHECK_FALSE(store.grant(v1, CapabilitySet::of({Capability::Read, Capability::EditProject})).has_value());
    CHECK_FALSE(store.grant(v1, CapabilitySet::of({Capability::Network})).has_value());
    REQUIRE(store.grant(v1, CapabilitySet::of({Capability::Read})).has_value());
    CHECK(store.isEnabled(v1.manifest.id));
    CHECK(store.effective(v1).has(Capability::Read));
    CHECK_FALSE(store.effective(v1).has(Capability::EditSequence));
    CHECK(store.needsReview(v1)); // EditSequence was asked for and not granted

    REQUIRE(store.grant(v1, CapabilitySet::of({Capability::Read, Capability::EditSequence})).has_value());
    CHECK_FALSE(store.needsReview(v1));

    // Version 2 asks for EditProject too. Its files are not the ones reviewed, so it gets nothing until the user
    // reviews it (S5-P2-4); the dialog ticks what was granted before.
    Json m2 = manifest("com.example.ext", "Example", {"Read", "EditSequence", "EditProject"});
    m2["version"] = "2.0.0";
    const ExtensionBundle v2 = bundleIn(dir / "ext", m2);
    CHECK(store.needsReview(v2));
    CHECK_FALSE(store.isReviewed(v2));
    CHECK(store.effective(v2).bits == 0);
    const ExtensionReview update = store.review(v2);
    CHECK(update.update);
    CHECK_FALSE(update.authorChanged);
    REQUIRE(update.requests.size() == 3);
    CHECK(update.requests[1].granted);
    CHECK_FALSE(update.requests[2].granted);
    CHECK(std::any_of(update.notes.begin(), update.notes.end(), [](const std::string& n) { return n.find("1.0.0") != std::string::npos; }));
    CHECK(std::any_of(update.notes.begin(), update.notes.end(), [](const std::string& n) { return n.find("changed") != std::string::npos; }));

    // Version 3 asks for less: once reviewed, it gets only what it asks for, whatever was granted before.
    Json m3 = manifest("com.example.ext", "Example", {"Read"});
    m3["version"] = "3.0.0";
    const ExtensionBundle v3 = bundleIn(dir / "ext", m3);
    CHECK(store.needsReview(v3));
    REQUIRE(store.grant(v3, CapabilitySet::of({Capability::Read})).has_value());
    CHECK_FALSE(store.needsReview(v3));
    CHECK(store.effective(v3).has(Capability::Read));
    CHECK_FALSE(store.effective(v3).has(Capability::EditSequence));
    CHECK(store.effective(v1).bits == 0); // the earlier files are not the reviewed ones any more

    // Revoke: off, no grants, and a reason.
    store.revoke(v1.manifest.id);
    CHECK_FALSE(store.isEnabled(v1.manifest.id));
    CHECK(store.effective(v1).bits == 0);
    CHECK(store.find(v1.manifest.id)->disabledReason == "permissions revoked");
}

TEST_CASE("Permissions persist; and a damaged file grants nothing", "[script][permissions]") {
    const fs::path dir = scratch("persist");
    const ExtensionBundle b = bundleIn(dir / "ext", manifest("com.example.persist", "Persist", {"Read", "MediaAnalysis"}));
    const ExtensionBundle p = bundleIn(dir / "film" / ".wabcut" / "extensions" / "p",
                                       manifest("com.example.project", "Project", {"Read"}), ExtensionScope::Project);
    const fs::path file = dir / "settings" / "extension-permissions.json";
    {
        PermissionStore store = PermissionStore::load(file);
        REQUIRE(store.grant(b, CapabilitySet::of({Capability::Read, Capability::MediaAnalysis})).has_value());
        store.trust(dir / "film" / "film.wabcut", p);
        store.recordCrash(b.manifest.id, "a test");
        REQUIRE(store.save().has_value());
    }
    PermissionStore loaded = PermissionStore::load(file);
    CHECK(loaded.effective(b).has(Capability::MediaAnalysis));
    CHECK(loaded.find(b.manifest.id)->crashes == 1);
    CHECK(loaded.isTrusted(dir / "film" / "film.wabcut", p));
    CHECK(loaded.toJson() == PermissionStore::load(file).toJson());

    // Damaged: moved aside, never reused.
    writeFile(file, "{\"format\": \"wabcut-extension-permissions\", \"version\": 1, \"extensions\": {\"x\": {}}}");
    PermissionStore damaged = PermissionStore::load(file);
    CHECK_FALSE(damaged.isEnabled(b.manifest.id));
    CHECK(fs::exists(fs::path(file.string() + ".bad")));
    CHECK_FALSE(fs::exists(file));

    // A grant of Network in the file is refused too.
    writeFile(file, Json{{"format", "wabcut-extension-permissions"},
                         {"version", 1},
                         {"extensions", {{"com.example.persist", {{"enabled", true}, {"granted", {"Network"}}}}}},
                         {"trustedProjectExtensions", Json::array()}}
                        .dump());
    CHECK_FALSE(PermissionStore::load(file).isEnabled(b.manifest.id));
}

TEST_CASE("Trust in a per-project extension holds for that project and that exact content", "[script][permissions]") {
    const fs::path dir = scratch("trust");
    const fs::path film = dir / "film" / "film.wabcut";
    const fs::path other = dir / "copy" / "film.wabcut";
    const fs::path folder = dir / "film" / ".wabcut" / "extensions" / "p";
    const ExtensionBundle p = bundleIn(folder, manifest("com.example.project", "Project", {"Read"}), ExtensionScope::Project);
    PermissionStore store;
    CHECK(store.review(p, film).needsTrust);
    CHECK_FALSE(store.review(p, film).notes.empty()); // "comes with the project"
    store.trust(film, p);
    CHECK(store.isTrusted(film, p));
    CHECK_FALSE(store.review(p, film).needsTrust);
    CHECK_FALSE(store.isTrusted(other, p)); // a copy of the project elsewhere starts untrusted

    // A teammate changes the script: the trust lapses.
    writeFile(folder / "main.js", "wabcut.commands.run('timeline.rippleDelete', {});");
    auto changed = loadExtension(folder, ExtensionScope::Project);
    REQUIRE(changed.has_value());
    CHECK_FALSE(store.isTrusted(film, *changed));

    store.distrust(film, p.manifest.id);
    CHECK_FALSE(store.isTrusted(film, p));
}

TEST_CASE("Three crashes in a row switch an extension off; a clean run or enabling it again starts over", "[script][permissions]") {
    const fs::path dir = scratch("crashes");
    const ExtensionBundle b = bundleIn(dir / "ext", manifest("com.example.crashy", "Crashy", {"Read"}));
    PermissionStore store;
    REQUIRE(store.grant(b, CapabilitySet::of({Capability::Read})).has_value());
    CHECK(store.recordCrash(b.manifest.id, "boom") == 1);
    store.recordCleanRun(b.manifest.id);
    CHECK(store.find(b.manifest.id)->crashes == 0);
    store.recordCrash(b.manifest.id, "boom");
    store.recordCrash(b.manifest.id, "boom");
    CHECK(store.isEnabled(b.manifest.id));
    CHECK(store.recordCrash(b.manifest.id, "the memory limit") == kMaxCrashes);
    CHECK_FALSE(store.isEnabled(b.manifest.id));
    CHECK(store.find(b.manifest.id)->disabledReason.find("crashed 3 times") != std::string::npos);
    CHECK(store.find(b.manifest.id)->disabledReason.find("the memory limit") != std::string::npos);
    store.setEnabled(b.manifest.id, true);
    CHECK(store.isEnabled(b.manifest.id));
    CHECK(store.find(b.manifest.id)->crashes == 0);
    CHECK(store.find(b.manifest.id)->disabledReason.empty());
}

TEST_CASE("Capability lists read and print like the CLI's --grant", "[script][permissions]") {
    const auto set = parseCapabilityList("read, editSequence,MEDIAANALYSIS");
    REQUIRE(set.has_value());
    CHECK(capabilityList(*set) == "Read, EditSequence, MediaAnalysis");
    CHECK_FALSE(parseCapabilityList("Read,Root").has_value());
    CHECK(parseCapabilityList("")->bits == 0);
    CHECK(describe(Capability::MediaAnalysis).explanation.find("consent") != std::string::npos);
}

TEST_CASE("Grants hold for the reviewed files in their folder only; another author starts from nothing (S5-P2-4)", "[script][permissions][security]") {
    const fs::path dir = scratch("bound");
    const ExtensionBundle a = bundleIn(dir / "ext", manifest("com.example.bound", "Bound", {"Read"}));
    PermissionStore store;
    REQUIRE(store.grant(a, CapabilitySet::of({Capability::Read})).has_value());
    CHECK(store.isReviewed(a));
    CHECK(store.effective(a).has(Capability::Read));

    // The very same files in another folder are not the copy the user reviewed.
    const ExtensionBundle copy = bundleIn(dir / "copy", manifest("com.example.bound", "Bound", {"Read"}));
    CHECK(copy.contentHash == a.contentHash);
    CHECK_FALSE(store.isReviewed(copy));
    CHECK(store.needsReview(copy));
    CHECK(store.effective(copy).bits == 0);

    // Someone else's extension under the same id: nothing is ticked for the user, and the dialog says why.
    Json other = manifest("com.example.bound", "Bound", {"Read"});
    other["author"] = "Someone else";
    const ExtensionBundle b = bundleIn(dir / "ext", other);
    const ExtensionReview r = store.review(b);
    CHECK(r.authorChanged);
    REQUIRE(r.requests.size() == 1);
    CHECK_FALSE(r.requests[0].granted);
    CHECK(std::any_of(r.notes.begin(), r.notes.end(), [](const std::string& n) { return n.find("Someone else") != std::string::npos; }));
    CHECK(store.effective(b).bits == 0);

    // A grant recorded before grants were bound to the files: the extension is reviewed again before it runs.
    Json json = store.toJson();
    json["extensions"]["com.example.bound"].erase("reviewedHash");
    const auto older = PermissionStore::fromJson(json);
    REQUIRE(older.has_value());
    CHECK(older->needsReview(a));
    CHECK(older->effective(a).bits == 0);
}
