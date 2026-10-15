// The editor's side against a broken or compromised helper (C.12): garbage, floods, hangs, crashes and forged
// requests end only that extension, never block the editor, and never get past the extension's grants. The helper is
// tests/script/fake_scripthost.cpp, so this runs in core builds too.

#include "ScriptTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/command/Registry.h>
#include <wabcut/script/Extension.h>
#include <wabcut/script/Host.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::script;
using namespace wabcut::script::test;
using namespace std::chrono_literals;

namespace {

fs::path fakeHelper() {
#ifdef _WIN32
    return ai::currentExecutableDir() / "wabcut_fake_scripthost.exe";
#else
    return ai::currentExecutableDir() / "wabcut_fake_scripthost";
#endif
}

struct FakeRig {
    explicit FakeRig(const std::string& mode, std::vector<std::string> caps = {"Read"}, HostLimits limits = {})
        : tl("    V1 tgt |Aaaa|\n    A1 tgt |Aaaa|\n"), session(tl.state) {
        const fs::path dir = scratch("fake-" + mode);
        writeExtension(dir / "ext", manifest("com.test." + mode, "Fake " + mode, caps), {{"main.js", "// fake"}});
        auto bundle = loadExtension(dir / "ext", ExtensionScope::User);
        REQUIRE(bundle.has_value());
        HostSetup setup;
        setup.bundle = std::move(*bundle);
        setup.granted = command::CapabilitySet::all();
        setup.session = &session;
        setup.registry = &command::builtinCommands();
        setup.helper = fakeHelper();
        setup.limits = limits;
        auto started = ExtensionHost::start(std::move(setup), delegate);
        REQUIRE(started.has_value());
        host = std::move(*started);
    }
    bool ended(std::chrono::milliseconds timeout = 20s) {
        return pumpUntil(*host, delegate,
                         [&] { return host->state() != HostState::Starting && host->state() != HostState::Ready; },
                         timeout);
    }
    bool ready() {
        return pumpUntil(*host, delegate, [&] { return host->state() != HostState::Starting; }) &&
               host->state() == HostState::Ready;
    }

    wabcut::test::Timeline tl;
    EditSession session;
    RecordingDelegate delegate;
    std::unique_ptr<ExtensionHost> host;
};

} // namespace

TEST_CASE("A helper that breaks the protocol is ended; and says why", "[script][host]") {
    FakeRig garbage("garbage");
    REQUIRE(garbage.ended());
    CHECK(garbage.host->state() == HostState::Crashed);
    CHECK(garbage.host->stopReport()->reason.find("protocol") != std::string::npos);
    REQUIRE(garbage.delegate.stops.size() == 1);

    FakeRig huge("huge");
    REQUIRE(huge.ended());
    CHECK(huge.host->state() == HostState::Crashed);
    CHECK(huge.host->stopReport()->reason.find("larger than") != std::string::npos);

    // A prompt the editor could not show is a broken helper too (the real one refuses it as a script error).
    FakeRig badPrompt("badprompt");
    REQUIRE(badPrompt.ended());
    CHECK(badPrompt.delegate.prompts.empty());
}

TEST_CASE("A helper that floods the editor is ended without the editor reading it all", "[script][host]") {
    FakeRig flood("flood");
    std::this_thread::sleep_for(1500ms); // the editor is busy elsewhere: the lines pile up
    REQUIRE(flood.ended());
    CHECK(flood.host->state() == HostState::Crashed);
    CHECK(flood.host->stopReport()->reason.find("more than the editor reads") != std::string::npos);
    CHECK(flood.host->log().size() <= 500); // the editor keeps a bounded log
}

TEST_CASE("A helper that hangs in native code is cancelled; then killed; the editor never waits", "[script][host]") {
    HostLimits limits;
    limits.callTime = 300ms;
    limits.grace = 300ms;
    FakeRig hang("hang", {"Read"}, limits);
    REQUIRE(hang.ready());
    std::optional<CallResult> result;
    const auto before = std::chrono::steady_clock::now();
    hang.host->invoke("go", Json::object(), [&](const CallResult& r) { result = r; });
    CHECK(std::chrono::steady_clock::now() - before < 50ms); // invoke() returns at once
    std::chrono::steady_clock::duration slowestPump{};
    REQUIRE(pumpUntil(*hang.host, hang.delegate, [&] {
        const auto t = std::chrono::steady_clock::now();
        hang.host->pump();
        slowestPump = std::max(slowestPump, std::chrono::steady_clock::now() - t);
        return result.has_value();
    }));
    CHECK(slowestPump < 100ms); // ... and so does pump(), while the extension hangs
    CHECK_FALSE(result->ok);
    CHECK(result->errorKind == "stopped");
    CHECK(hang.host->state() == HostState::Crashed);
    CHECK(hang.host->stopReport()->timeLimit);
    CHECK(std::chrono::steady_clock::now() - before < 5s);
}

TEST_CASE("A crashing helper fails its calls and ends only itself", "[script][host]") {
    FakeRig crash("crash");
    REQUIRE(crash.ready());
    const CallResult r = call(*crash.host, crash.delegate, "go");
    CHECK_FALSE(r.ok);
    CHECK(r.errorKind == "stopped");
    CHECK(crash.host->state() == HostState::Crashed);
    CHECK(crash.host->stopReport()->reason.find("process ended") != std::string::npos);
    // Later calls fail at once.
    std::optional<CallResult> late;
    crash.host->invoke("go", Json::object(), [&](const CallResult& x) { late = x; });
    REQUIRE(late.has_value());
    CHECK(late->errorKind == "stopped");
}

TEST_CASE("A helper that never gets ready; or exits; fails to start", "[script][host]") {
    HostLimits limits;
    limits.startTime = 500ms;
    FakeRig nostart("nostart", {"Read"}, limits);
    REQUIRE(nostart.ended());
    CHECK(nostart.host->state() == HostState::Failed);
    CHECK(nostart.host->stopReport()->reason.find("did not finish loading") != std::string::npos);

    FakeRig exits("exit");
    REQUIRE(exits.ended());
    CHECK(exits.host->state() == HostState::Failed);
}

TEST_CASE("A forged request gets no more than the extension's grants", "[script][host][security]") {
    // The manifest asks for Read only; the host is offered everything; the helper asks for an edit anyway.
    FakeRig forge("forge", {"Read"});
    REQUIRE(forge.ready());
    CHECK_FALSE(forge.host->granted().has(command::Capability::EditSequence));
    const auto revision = forge.session.revision();
    const CallResult edit = call(*forge.host, forge.delegate, "go",
                                 Json{{"document", {{"command", "marker.add"}, {"params", {{"time", 2}}}}}});
    REQUIRE(edit.ok);
    CHECK(edit.result["result"]["error"]["code"] == "PermissionDenied");
    CHECK(forge.session.revision() == revision);
    const CallResult read = call(*forge.host, forge.delegate, "go",
                                 Json{{"document", {{"command", "project.summary"}}}});
    CHECK(read.result["result"].contains("data"));
}
