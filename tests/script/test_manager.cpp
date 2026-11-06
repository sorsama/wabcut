// The extension manager (C.12): what the app's extension panel, permission dialog and palette are built on. The
// extensions run in tests/script/fake_scripthost.cpp here (its default mode answers every command with "ok"), so the
// flow is tested in core builds too.

#include "ScriptTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/command/Registry.h>
#include <wabcut/script/Manager.h>
#include <wabcut/script/Storage.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <cstdio>
#include <iterator>
#ifdef _WIN32
#include <share.h>
#endif

using namespace wabcut;
using namespace wabcut::script;
using namespace wabcut::script::test;
using namespace std::chrono_literals;
using command::Capability;
using command::CapabilitySet;

namespace {

class Recorder : public ManagerDelegate {
public:
    void wake() override {
        {
            std::lock_guard lock(m_mutex);
            m_woken = true;
        }
        m_cv.notify_all();
    }
    void changed() override { ++changes; }
    void faulted(const ExtensionInfo& e, const StopReport& r, bool disabled) override {
        faults.push_back({e.id, r.reason, disabled});
    }
    void wait(std::chrono::milliseconds max) {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, max, [this] { return m_woken; });
        m_woken = false;
    }
    struct Fault {
        std::string id;
        std::string reason;
        bool disabled = false;
    };
    int changes = 0;
    std::vector<Fault> faults;

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_woken = false;
};

struct ManagerRig {
    ManagerRig() : tl("    V1 tgt |Aaaa|\n    A1 tgt |Aaaa|\n"), session(tl.state) {
        // A folder per test case: ctest runs the cases as parallel processes, which must not share one.
        const std::size_t test = std::hash<std::string>{}(Catch::getResultCapture().getCurrentTestName());
        root = scratch("manager-" + std::to_string(test % 1'000'000'007));
        paths = extensionPathsUnder(root / "appdata");
#ifdef _WIN32
        paths.helper = ai::currentExecutableDir() / "wabcut_fake_scripthost.exe";
#else
        paths.helper = ai::currentExecutableDir() / "wabcut_fake_scripthost";
#endif
        project = root / "film" / "film.wabcut";
        fs::create_directories(project.parent_path());
    }
    fs::path user(const std::string& id, std::vector<std::string> caps = {"Read"}) {
        Json m = manifest(id, "Ext " + id.substr(id.rfind('.') + 1), caps);
        m["commands"] = Json::array({{{"id", "go"}, {"title", "Go"}}});
        return writeExtension(paths.userExtensions / id, m, {{"main.js", "// " + id}});
    }
    fs::path inProject(const std::string& id) {
        Json m = manifest(id, "Project " + id.substr(id.rfind('.') + 1), {"Read"});
        return writeExtension(projectExtensionsRoot(project) / id, m, {{"main.js", "// project " + id}});
    }
    std::unique_ptr<ExtensionManager> open() {
        auto m = std::make_unique<ExtensionManager>(paths, "0.1.0", delegate);
        m->attach(session, command::builtinCommands(), nullptr, project);
        return m;
    }
    ExtensionStatus statusOf(ExtensionManager& m, const std::string& id) {
        const auto info = m.info(id);
        REQUIRE(info.has_value());
        return info->status;
    }
    bool waitFor(ExtensionManager& m, const std::function<bool()>& done, std::chrono::milliseconds timeout = 15s) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() > end) {
                return false;
            }
            delegate.wait(20ms);
            m.pump();
        }
        return true;
    }
    std::optional<CallResult> run(ExtensionManager& m, const std::string& paletteId) {
        std::optional<CallResult> result;
        auto started = m.run(paletteId, Json::object(), [&](const CallResult& r) { result = r; });
        if (!started) {
            return CallResult{false, {}, "notRunnable", started.error().message};
        }
        waitFor(m, [&] { return result.has_value(); });
        return result;
    }

    wabcut::test::Timeline tl;
    EditSession session;
    fs::path root;
    fs::path project;
    ExtensionPaths paths;
    Recorder delegate;
};

} // namespace

TEST_CASE("User extensions run once reviewed and granted; revoking stops them", "[script][manager]") {
    ManagerRig rig;
    rig.user("com.test.ok", {"Read", "EditSequence"});
    auto m = rig.open();
    REQUIRE(m->list().size() == 1);
    CHECK(rig.statusOf(*m, "com.test.ok") == ExtensionStatus::NeedsReview);
    CHECK(m->paletteCommands().empty());
    CHECK_FALSE(m->run("ext:com.test.ok/go", Json::object(), {}).has_value());

    auto review = m->review("com.test.ok");
    REQUIRE(review.has_value());
    REQUIRE(review->requests.size() == 2);
    CHECK_FALSE(m->enable("com.test.ok", CapabilitySet::of({Capability::EditProject})).has_value());
    REQUIRE(m->enable("com.test.ok", CapabilitySet::of({Capability::Read})).has_value());
    REQUIRE(rig.waitFor(*m, [&] { return rig.statusOf(*m, "com.test.ok") == ExtensionStatus::Running; }));
    CHECK(m->info("com.test.ok")->granted.has(Capability::Read));
    CHECK(m->info("com.test.ok")->needsReview); // EditSequence was asked for, not granted

    const auto palette = m->paletteCommands();
    REQUIRE(palette.size() == 1);
    CHECK(palette[0].paletteId == "ext:com.test.ok/go");
    CHECK(palette[0].title == "Ext ok: Go");
    const auto result = rig.run(*m, "ext:com.test.ok/go");
    REQUIRE(result.has_value());
    CHECK(result->ok);
    CHECK(result->result == "ok");

    // The grants were saved: a new manager (the next app start) runs it straight away.
    m.reset();
    m = rig.open();
    REQUIRE(rig.waitFor(*m, [&] { return rig.statusOf(*m, "com.test.ok") == ExtensionStatus::Running; }));

    m->revoke("com.test.ok");
    CHECK(rig.statusOf(*m, "com.test.ok") == ExtensionStatus::Disabled);
    CHECK(m->info("com.test.ok")->disabledReason == "permissions revoked");
    CHECK(m->host("com.test.ok") == nullptr);
    CHECK(m->paletteCommands().empty());
}

TEST_CASE("Per-project extensions run only after the user trusts that content", "[script][manager][security]") {
    ManagerRig rig;
    const fs::path folder = rig.inProject("com.test.shared");
    auto m = rig.open();
    CHECK(rig.statusOf(*m, "com.test.shared") == ExtensionStatus::NeedsReview);
    auto review = m->review("com.test.shared");
    REQUIRE(review.has_value());
    CHECK(review->scope == ExtensionScope::Project);
    CHECK(review->needsTrust);
    // Granted but not trusted: it does not run.
    REQUIRE(m->enable("com.test.shared", CapabilitySet::of({Capability::Read})).has_value());
    CHECK(rig.statusOf(*m, "com.test.shared") == ExtensionStatus::NeedsTrust);
    CHECK(m->host("com.test.shared") == nullptr);
    CHECK(m->paletteCommands().empty());
    CHECK_FALSE(m->run("ext:com.test.shared/go", Json::object(), {}).has_value());

    REQUIRE(m->trust("com.test.shared").has_value());
    REQUIRE(rig.waitFor(*m, [&] { return rig.statusOf(*m, "com.test.shared") == ExtensionStatus::Running; }));

    // A teammate changes the script: trust lapses and it stops.
    writeFile(folder / "main.js", "// changed");
    m->rescan();
    CHECK(rig.statusOf(*m, "com.test.shared") == ExtensionStatus::NeedsTrust);
    CHECK(m->host("com.test.shared") == nullptr);

    // A project extension cannot take the place of an installed one.
    rig.user("com.test.same");
    rig.inProject("com.test.same");
    m->rescan();
    const auto list = m->list();
    const auto shadowed = std::find_if(list.begin(), list.end(), [](const ExtensionInfo& e) {
        return e.scope == ExtensionScope::Project && e.status == ExtensionStatus::Invalid;
    });
    REQUIRE(shadowed != list.end());
    CHECK(shadowed->loadError.find("already uses the id") != std::string::npos);
    CHECK(m->info("com.test.same")->scope == ExtensionScope::User);
}

TEST_CASE("An extension that keeps crashing is switched off after three crashes", "[script][manager]") {
    ManagerRig rig;
    rig.user("com.test.crash");
    auto m = rig.open();
    REQUIRE(m->enable("com.test.crash", CapabilitySet::of({Capability::Read})).has_value());
    for (int i = 1; i <= kMaxCrashes; ++i) {
        INFO("crash " << i);
        const auto r = rig.run(*m, "ext:com.test.crash/go"); // starts it again after the last crash
        REQUIRE(r.has_value());
        CHECK_FALSE(r->ok);
        REQUIRE(rig.waitFor(*m, [&] { return static_cast<int>(rig.delegate.faults.size()) == i; }));
    }
    CHECK(rig.delegate.faults.back().disabled);
    CHECK(rig.statusOf(*m, "com.test.crash") == ExtensionStatus::Disabled);
    CHECK(m->info("com.test.crash")->disabledReason.find("crashed 3 times") != std::string::npos);
    CHECK_FALSE(m->run("ext:com.test.crash/go", Json::object(), {}).has_value());
    // The user switches it on again (re-enabling resets the count).
    REQUIRE(m->enable("com.test.crash", CapabilitySet::of({Capability::Read})).has_value());
    CHECK(m->info("com.test.crash")->crashes == 0);
}

TEST_CASE("Installing copies only the extension's own files; uninstalling removes it; its grants and storage", "[script][manager]") {
    ManagerRig rig;
    const fs::path source = rig.root / "download" / "beats";
    Json mf = manifest("com.test.installed", "Installed", {"Read"});
    mf["scripts"] = Json::array({"lib/util.js"});
    writeExtension(source, mf, {{"main.js", "// main"}, {"lib/util.js", "// util"}, {"secrets.txt", "not copied"}});
    auto m = rig.open();
    auto review = m->reviewFolder(source);
    REQUIRE(review.has_value());
    CHECK(review->manifest.name == "Installed");
    auto installed = m->install(source);
    REQUIRE(installed.has_value());
    CHECK(*installed == "com.test.installed");
    const fs::path target = rig.paths.userExtensions / "com.test.installed";
    CHECK(fs::exists(target / "wabcut-extension.json"));
    CHECK(fs::exists(target / "lib" / "util.js"));
    CHECK_FALSE(fs::exists(target / "secrets.txt"));
    CHECK(rig.statusOf(*m, "com.test.installed") == ExtensionStatus::NeedsReview); // installing grants nothing
    REQUIRE(m->enable("com.test.installed", CapabilitySet::of({Capability::Read})).has_value());
    writeFile(storageFileFor(rig.paths.storage, "com.test.installed"), "{}");

    REQUIRE(m->uninstall("com.test.installed").has_value());
    CHECK_FALSE(fs::exists(target));
    CHECK_FALSE(fs::exists(storageFileFor(rig.paths.storage, "com.test.installed")));
    CHECK_FALSE(m->info("com.test.installed").has_value());
    CHECK(m->permissions().find("com.test.installed") == nullptr);
    // A broken folder is listed, not run.
    writeFile(rig.paths.userExtensions / "broken" / "wabcut-extension.json", "{}");
    m->rescan();
    const auto list = m->list();
    REQUIRE(list.size() == 1);
    CHECK(list[0].status == ExtensionStatus::Invalid);
    CHECK_FALSE(list[0].loadError.empty());
}

TEST_CASE("Installing over an extension runs nothing new until the user enables it again (S5-P2-4)", "[script][manager][security]") {
    ManagerRig rig;
    rig.user("com.test.a", {"Read", "EditSequence"});
    auto m = rig.open();
    REQUIRE(m->enable("com.test.a", CapabilitySet::of({Capability::Read, Capability::EditSequence})).has_value());
    REQUIRE(rig.waitFor(*m, [&] { return m->host("com.test.a") && m->host("com.test.a")->state() == HostState::Ready; }));

    // Another extension claiming the same id (someone else's download) is installed over it.
    const fs::path other = rig.root / "download" / "a2";
    Json mf = manifest("com.test.a", "Ext a", {"Read", "EditSequence"});
    mf["author"] = "Someone else";
    mf["commands"] = Json::array({{{"id", "go"}, {"title", "Go"}}});
    writeExtension(other, mf, {{"main.js", "// a different script"}});
    REQUIRE(m->install(other).has_value());
    // Nothing of it runs, with the old grants or any, until the dialog's enable() records what the user chose.
    CHECK(m->host("com.test.a") == nullptr);
    CHECK(rig.statusOf(*m, "com.test.a") == ExtensionStatus::NeedsReview);
    REQUIRE(m->enable("com.test.a", CapabilitySet::of({Capability::Read})).has_value());
    CHECK(m->host("com.test.a") != nullptr);
}

// ---- Card 164: an update never takes the working version away before the new one is whole -------------------------------

namespace {

/// A downloaded extension folder with the id of the installed one: main.js says `version`; `extra` is a second script.
fs::path download(ManagerRig& rig, const std::string& name, const std::string& version, bool extra) {
    const fs::path source = rig.root / "download" / name;
    Json mf = manifest("com.test.upd", "Updated", {"Read"});
    if (extra) {
        mf["scripts"] = Json::array({"lib/util.js"});
    }
    std::map<std::string, std::string> files{{"main.js", "// " + version}};
    if (extra) {
        files["lib/util.js"] = "// util " + version;
    }
    return writeExtension(source, mf, files);
}

std::string textOf(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::size_t foldersIn(const fs::path& root) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        n += it->is_directory() ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("Updating an extension swaps the whole new version in and leaves no staging or old folder", "[script][manager]") {
    ManagerRig rig;
    auto m = rig.open();
    REQUIRE(m->install(download(rig, "v1", "v1", true)).has_value());
    const fs::path target = rig.paths.userExtensions / "com.test.upd";
    CHECK(textOf(target / "main.js") == "// v1");
    CHECK(fs::exists(target / "lib" / "util.js"));

    // v2 has no lib/util.js: nothing of v1 is left behind, and nothing of v2 is missing.
    REQUIRE(m->install(download(rig, "v2", "v2", false)).has_value());
    CHECK(textOf(target / "main.js") == "// v2");
    CHECK_FALSE(fs::exists(target / "lib" / "util.js"));
    CHECK(foldersIn(rig.paths.userExtensions) == 1); // no "com.test.upd.wabcut-new" / ".wabcut-old"
    REQUIRE(m->list().size() == 1);
    CHECK(rig.statusOf(*m, "com.test.upd") != ExtensionStatus::Invalid);
    CHECK(m->info("com.test.upd")->manifest->name == "Updated");
}

TEST_CASE("What a crashed update left beside an extension is never listed and the next install cleans it up", "[script][manager]") {
    ManagerRig rig;
    auto m = rig.open();
    REQUIRE(m->install(download(rig, "v1", "v1", false)).has_value());
    const fs::path target = rig.paths.userExtensions / "com.test.upd";
    // A crash in the middle of the copy (a manifest only), and in the middle of the swap (the old version aside).
    writeFile(rig.paths.userExtensions / "com.test.upd.wabcut-new" / "wabcut-extension.json", textOf(target / "wabcut-extension.json"));
    fs::copy(target, rig.paths.userExtensions / "com.test.upd.wabcut-old", fs::copy_options::recursive);
    m->rescan();
    REQUIRE(m->list().size() == 1); // neither is an extension of its own (no "already uses the id" duplicates)
    CHECK(rig.statusOf(*m, "com.test.upd") != ExtensionStatus::Invalid);

    REQUIRE(m->install(download(rig, "v2", "v2", false)).has_value());
    CHECK(textOf(target / "main.js") == "// v2");
    CHECK(foldersIn(rig.paths.userExtensions) == 1);

    // The crash between the two renames: the installed version is only the one waiting aside. The next install brings it
    // back before it replaces it (the update may fail, and then it is what is installed).
    fs::rename(target, rig.paths.userExtensions / "com.test.upd.wabcut-old");
    m->rescan();
    CHECK(m->list().empty());
    REQUIRE(m->install(download(rig, "v3", "v3", false)).has_value());
    CHECK(textOf(target / "main.js") == "// v3");
    CHECK(foldersIn(rig.paths.userExtensions) == 1);
}

// Review: an id spelled like the staging or the aside folder is untrusted package input. It used to install "successfully"
// into a folder that is never listed, and installing the id it looks like cleaned that folder away.
TEST_CASE("An extension whose id is spelled like an install folder is refused and the extension it looks like is untouched",
          "[script][manager]") {
    ManagerRig rig;
    auto m = rig.open();
    REQUIRE(m->install(download(rig, "v1", "v1", false)).has_value());
    const fs::path target = rig.paths.userExtensions / "com.test.upd";
    for (const char* suffix : {".wabcut-new", ".wabcut-old"}) {
        INFO(suffix);
        const std::string id = std::string("com.test.upd") + suffix;
        const fs::path folder =
            writeExtension(rig.root / "download" / ("look" + std::string(suffix)), manifest(id, "Lookalike", {"Read"}),
                           {{"main.js", "// lookalike"}});
        const auto refused = m->install(folder);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message.find("reverse-DNS") != std::string::npos);
        CHECK_FALSE(fs::exists(rig.paths.userExtensions / id)); // nothing was written
        CHECK(textOf(target / "main.js") == "// v1");
    }
    CHECK(foldersIn(rig.paths.userExtensions) == 1);
    REQUIRE(m->list().size() == 1);
    CHECK(m->list().front().id == "com.test.upd");
}

#ifdef _WIN32
// The update that cannot swap (an antivirus or an editor holds a file of the installed version): the installed version is
// untouched, still listed and runnable, and says so with an error. Before, remove_all() had already deleted what it could.
TEST_CASE("An update that cannot replace the installed version leaves it whole and running", "[script][manager]") {
    ManagerRig rig;
    auto m = rig.open();
    REQUIRE(m->install(download(rig, "v1", "v1", true)).has_value());
    REQUIRE(m->enable("com.test.upd", CapabilitySet::of({Capability::Read})).has_value());
    REQUIRE(rig.waitFor(*m, [&] { return rig.statusOf(*m, "com.test.upd") == ExtensionStatus::Running; }));
    const fs::path target = rig.paths.userExtensions / "com.test.upd";

    // A file in the installed folder that something else holds open without sharing (an antivirus scan, a text editor): the
    // folder cannot be renamed. (Not a script: the manager must still be able to read the extension afterwards.)
    writeFile(target / "notes.txt", "kept");
    std::FILE* held = _wfsopen((target / "notes.txt").c_str(), L"rb", _SH_DENYRW);
    REQUIRE(held != nullptr);
    const auto failed = m->install(download(rig, "v2", "v2", false));
    std::fclose(held);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ErrorCode::IoError);
    CHECK(textOf(target / "main.js") == "// v1");
    CHECK(textOf(target / "lib" / "util.js") == "// util v1");
    CHECK(textOf(target / "notes.txt") == "kept");
    CHECK(fs::exists(target / "wabcut-extension.json"));
    CHECK(foldersIn(rig.paths.userExtensions) == 1); // the half-written new version is gone
    // The manager still lists the working version, valid, and it runs again.
    REQUIRE(m->list().size() == 1);
    CHECK(m->info("com.test.upd")->loadError.empty());
    CHECK(rig.waitFor(*m, [&] { return rig.statusOf(*m, "com.test.upd") == ExtensionStatus::Running; }));

    // Once the file is free the same update installs.
    REQUIRE(m->install(download(rig, "v2", "v2", false)).has_value());
    CHECK(textOf(target / "main.js") == "// v2");
}
#endif
