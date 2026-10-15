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
