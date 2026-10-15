// Review wave 5 (security): the editor's side of an extension against a hostile helper or script.
//   S5-P2-1  a call has a hard time limit that nothing the helper sends can reset (logs, storage writes, requests);
//            storage writes cost what they store; a command that runs an AI model never blocks the editor's pump
//   S5-P2-2  extension text cannot pose as the editor (invisible and bidi characters, reserved names); prompts only
//            while a user-started call runs, at most five a minute
//   S5-P2-3  events reach only extensions that may read the project, and never the project's full path

#include "ScriptTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/command/Schema.h>
#include <wabcut/script/Extension.h>
#include <wabcut/script/Host.h>
#include <wabcut/script/Manifest.h>
#include <wabcut/script/Protocol.h>

#include <catch2/catch_test_macros.hpp>

#include <thread>

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

HostLimits quick() {
    HostLimits l;
    l.callTime = 300ms;
    l.grace = 300ms;
    return l;
}

/// An extension in the fake helper (tests/script/fake_scripthost.cpp), its mode the last part of its id.
struct Fake {
    Fake(const std::string& mode, HostLimits limits = quick(), const command::CommandRegistry* registry = nullptr)
        : tl("    V1 tgt |Aaaa|\n    A1 tgt |Aaaa|\n"), session(tl.state) {
        dir = scratch("wave5-" + mode);
        // A model command needs MediaAnalysis asked for and granted (the host grants no more than the manifest asks for).
        const std::vector<std::string> asks =
            mode == "model" ? std::vector<std::string>{"Read", "MediaAnalysis"} : std::vector<std::string>{"Read"};
        writeExtension(dir / "ext", manifest("com.test." + mode, "Fake " + mode, asks), {{"main.js", "// fake"}});
        auto bundle = loadExtension(dir / "ext", ExtensionScope::User);
        REQUIRE(bundle.has_value());
        HostSetup setup;
        setup.bundle = std::move(*bundle);
        setup.granted = command::CapabilitySet::all();
        setup.session = &session;
        setup.registry = registry ? registry : &command::builtinCommands();
        setup.helper = fakeHelper();
        setup.storageFile = dir / "storage.json";
        setup.limits = limits;
        auto started = ExtensionHost::start(std::move(setup), delegate);
        REQUIRE(started.has_value());
        host = std::move(*started);
        REQUIRE(pumpUntil(*host, delegate, [&] { return host->state() != HostState::Starting; }, 10s));
        REQUIRE(host->state() == HostState::Ready);
    }
    bool running() const { return host->state() == HostState::Ready; }

    wabcut::test::Timeline tl;
    EditSession session;
    fs::path dir;
    RecordingDelegate delegate;
    std::unique_ptr<ExtensionHost> host;
};

} // namespace

// ---- S5-P2-1 ----------------------------------------------------------------------------------------------------------

TEST_CASE("A helper that only logs is cancelled at its time limit and then ended (S5-P2-1)", "[script][security]") {
    Fake f("chatty");
    std::optional<CallResult> result;
    f.host->invoke("go", Json::object(), [&](const CallResult& r) { result = r; });
    // callTime + grace to cancel, another grace to end it: well under 3 s however many lines it sends meanwhile.
    REQUIRE(pumpUntil(*f.host, f.delegate, [&] { return !f.running(); }, 3s));
    CHECK(f.host->state() == HostState::Crashed);
    REQUIRE(f.host->stopReport().has_value());
    CHECK(f.host->stopReport()->timeLimit);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->ok);
}

TEST_CASE("A helper that keeps asking the editor never runs past the call's hard limit (S5-P2-1)", "[script][security]") {
    Fake f("busy");
    std::optional<CallResult> result;
    const auto start = std::chrono::steady_clock::now();
    f.host->invoke("go", Json::object(), [&](const CallResult& r) { result = r; });
    // Each request is answered at once, so the script is never idle long; still the whole call has a hard limit
    // (kCallHardLimitFactor x callTime), after which it is cancelled and then ended.
    REQUIRE(pumpUntil(*f.host, f.delegate, [&] { return !f.running(); }, 6s));
    CHECK(f.host->stopReport()->timeLimit);
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("Storage writes cost what they store: ten thousand at once are handled quickly (S5-P2-1)", "[script][security]") {
    Fake f("storageflood", HostLimits{});
    const auto start = std::chrono::steady_clock::now();
    const auto flooded = [&] {
        return std::ranges::any_of(f.host->commands(), [](const RegisteredCommand& c) { return c.id == "flooded"; });
    };
    REQUIRE(pumpUntil(*f.host, f.delegate, [&] { return flooded() || !f.running(); }, 60s));
    const auto took = std::chrono::steady_clock::now() - start;
    INFO("took " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms");
    CHECK(took < 3s);
    CHECK(f.running());
}

TEST_CASE("A command that runs an AI model never blocks the editor's pump (S5-P2-1)", "[script][security]") {
    command::CommandRegistry registry = command::builtinCommands();
    REQUIRE(registry
                .add(command::runsModel(
                    {"slow.model", 1, "Slow Model", "A test command that takes as long as a model run would.",
                     command::Capability::Read,
                     command::schema::object({{"sequence", command::schema::integer("Sequence id.", 1)}}),
                     command::Json::object(),
                     {},
                     [](command::ExecutionContext& c) {
                         std::this_thread::sleep_for(2s);
                         command::CommandOutput out;
                         out.state = c.state();
                         out.data = command::Json{{"slept", true}};
                         return out;
                     }}))
                .has_value());
    Fake f("model", HostLimits{}, &registry);
    std::optional<CallResult> result;
    f.host->invoke("go", Json::object(), [&](const CallResult& r) { result = r; });
    std::chrono::steady_clock::duration longest{};
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!result && std::chrono::steady_clock::now() < end) {
        f.delegate.wait(20ms);
        const auto before = std::chrono::steady_clock::now();
        f.host->pump();
        longest = std::max(longest, std::chrono::steady_clock::now() - before);
    }
    REQUIRE(result.has_value());
    INFO("longest pump " << std::chrono::duration_cast<std::chrono::milliseconds>(longest).count() << " ms");
    INFO(result->result.dump());
    CHECK(longest < 500ms);
    // Without a host that runs it elsewhere, the request is refused (and says so) instead of freezing the editor.
    CHECK(result->result.contains("error"));
}

TEST_CASE("A model command runs where its owner runs long work and the call waits for it (S5-P2-1)", "[script][security]") {
    command::CommandRegistry registry = command::builtinCommands();
    REQUIRE(registry
                .add(command::runsModel({"slow.model", 1, "Slow Model", "A test command standing in for a model run.",
                                         command::Capability::Read, command::schema::object({}), command::Json::object(),
                                         {},
                                         [](command::ExecutionContext& c) {
                                             command::CommandOutput out;
                                             out.state = c.state();
                                             out.data = command::Json{{"slept", true}};
                                             return out;
                                         }}))
                .has_value());
    Fake f("model", quick(), &registry);
    f.delegate.acceptLongCommands = true;
    std::optional<CallResult> result;
    f.host->invoke("go", Json::object(), [&](const CallResult& r) { result = r; });
    REQUIRE(pumpUntil(*f.host, f.delegate, [&] { return !f.delegate.longCommands.empty(); }, 5s));
    LongCommand job = std::move(f.delegate.longCommands.front());
    CHECK(job.policy.undoLabelPrefix == "Fake model"); // the extension's own policy
    CHECK_FALSE(job.policy.allowFileCommands);
    CHECK(job.policy.allowed.has(command::Capability::Read));
    CHECK(job.policy.allowModelCommands);
    CHECK_FALSE(job.policy.allowed.has(command::Capability::EditSequence)); // only what the manifest asked for

    // The job takes longer than callTime + grace twice over: the call is neither cancelled nor ended meanwhile.
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < until) {
        f.delegate.wait(20ms);
        f.host->pump();
    }
    CHECK(f.running());
    CHECK_FALSE(result.has_value());

    command::CommandEngine engine(f.session, registry, job.policy);
    job.finish(command::toJson(engine.executeJson(job.document)));
    REQUIRE(pumpUntil(*f.host, f.delegate, [&] { return result.has_value(); }, 5s));
    CHECK(result->ok);
    CHECK(result->result["result"]["data"]["slept"] == true);
}

// ---- S5-P2-2 ----------------------------------------------------------------------------------------------------------

TEST_CASE("Extension names may not hide text or pose as the editor (S5-P2-2)", "[script][security]") {
    const auto nameIssue = [](const std::string& name) {
        Json m = manifest("com.example.name", name, {"Read"});
        auto parsed = parseManifest(m.dump(), std::nullopt);
        return parsed ? std::string() : parsed.error().summary();
    };
    CHECK(nameIssue("Beat Markers").empty());
    CHECK(nameIssue("Caf\xC3\xA9 Tools").empty()); // accents are fine
    for (const char* hidden : {"Beat\xE2\x80\xA8Markers",      // U+2028 line separator
                               "Beat\xE2\x80\xA9Markers",      // U+2029 paragraph separator
                               "Beat\xD8\x9CMarkers",          // U+061C Arabic letter mark
                               "Beat\xE2\x81\xA0Markers",      // U+2060 word joiner
                               "Beat\xC2\xADMarkers",          // U+00AD soft hyphen
                               "Beat\xF3\xA0\x81\x81Markers",  // U+E0041 tag A
                               "Beat\xEF\xB8\x8FMarkers",      // U+FE0F variation selector
                               "Beat\xEE\x80\x80Markers",      // U+E000 private use
                               "Beat\xE1\xA0\x8EMarkers"}) {   // U+180E Mongolian vowel separator
        INFO(hidden);
        CHECK_FALSE(nameIssue(hidden).empty());
    }
    for (const char* reserved : {"Wabcut", "wabcut tools", "WABCUT", "W\xD0\xB0" "bcut Helper", // Cyrillic a
                                 "\xEF\xBC\xB7" "abcut", // fullwidth W
                                 "Adobe Premiere Helper", "Premiere Pro Bridge"}) {
        INFO(reserved);
        CHECK(nameIssue(reserved).find("reserved") != std::string::npos);
    }
    // Text an extension sends at run time (notifications, titles) gets the same treatment: such characters are replaced.
    auto notify = parseHostMessage(R"({"type": "notify", "level": "info", "message": "a)" "\xE2\x80\xA8" R"(b"})");
    REQUIRE(notify.has_value());
    CHECK(notify->body["message"].get<std::string>().find("\xE2\x80\xA8") == std::string::npos);
}

TEST_CASE("Prompts only while a user-started call runs; at most five a minute (S5-P2-2)", "[script][security]") {
    Fake f("prompter", HostLimits{});
    // The helper asked for a prompt right after loading, with no call running: refused, never shown.
    std::this_thread::sleep_for(300ms);
    f.host->pump();
    CHECK(f.delegate.prompts.empty());

    std::optional<CallResult> result;
    f.host->invoke("go", Json::object(), [&](const CallResult& r) { result = r; });
    std::size_t answered = 0;
    REQUIRE(pumpUntil(*f.host, f.delegate, [&] {
        while (answered < f.delegate.prompts.size()) {
            f.host->answerPrompt(f.delegate.prompts[answered++].prompt, std::nullopt); // the user cancels each
        }
        return result.has_value();
    }, 10s));
    CHECK(result->result == "prompted");
    CHECK(f.delegate.prompts.size() == 5); // the sixth and seventh were cancelled without being shown
}

// ---- S5-P2-3 ----------------------------------------------------------------------------------------------------------

TEST_CASE("Events reach only extensions that may read the project and never its full path (S5-P2-3)", "[script][security]") {
    if (!fs::exists(scriptHostPath())) {
        SKIP("the real wabcut-scripthost is not built (core preset)");
    }
    const std::string source = R"js(
        const got = {};
        for (const kind of ['selectionChanged', 'playheadMoved', 'commandApplied', 'projectSaved'])
            wabcut.events.on(kind, (data) => { got[kind] = data; });
        wabcut.ui.registerCommand('got', 'Got', () => got);
    )js";
    const auto received = [&](const std::string& name, command::CapabilitySet granted) {
        wabcut::test::Timeline tl("    V1 tgt |Aaaa|\n    A1 tgt |Aaaa|\n");
        EditSession session(tl.state);
        const fs::path dir = scratch("wave5-events-" + name);
        writeExtension(dir / "ext", manifest("com.test.events" + name, "Events " + name, {"Read"}), {{"main.js", source}});
        auto bundle = loadExtension(dir / "ext", ExtensionScope::User);
        REQUIRE(bundle.has_value());
        HostSetup setup;
        setup.bundle = std::move(*bundle);
        setup.granted = granted;
        setup.session = &session;
        setup.registry = &command::builtinCommands();
        setup.wabcutVersion = "0.1.0";
        RecordingDelegate delegate;
        auto started = ExtensionHost::start(std::move(setup), delegate);
        REQUIRE(started.has_value());
        ExtensionHost& host = **started;
        REQUIRE(pumpUntil(host, delegate, [&] { return host.state() != HostState::Starting; }));
        REQUIRE(host.state() == HostState::Ready);
        host.publish(Event{EventKind::SelectionChanged, Json{{"clips", Json::array({7})}}, 0});
        host.publish(Event{EventKind::PlayheadMoved, Json{{"time", 12}}, 0});
        host.publish(Event{EventKind::CommandApplied, Json{{"label", "Rename 'Interview with Somchai'"}}, 0});
        host.publish(Event{EventKind::ProjectSaved, Json{{"file", "C:/Users/someone/Films/film.wabcut"}}, 0});
        std::this_thread::sleep_for(300ms);
        host.pump();
        REQUIRE(pumpUntil(host, delegate, [&] { return !host.nextDeadline(); }, 2s));
        return call(host, delegate, "got").result;
    };
    CHECK(received("none", command::CapabilitySet{}) == Json::object()); // no grants: no events at all
    const Json read = received("read", command::CapabilitySet::of({command::Capability::Read}));
    CHECK(read["playheadMoved"]["time"] == 12);
    CHECK(read["projectSaved"]["file"] == "film.wabcut"); // the name, not where it lives (the user's account name)
}
