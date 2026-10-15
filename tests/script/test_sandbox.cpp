// Extensions in the real wabcut-scripthost (C.12): the sandbox (no file, network or process API), the time and memory
// limits, exceptions that stay inside the extension, capability filtering, undo labels, throttled events, storage,
// prompts and notifications.

#include "ScriptTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/script/Extension.h>
#include <wabcut/script/Host.h>

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace wabcut;
using namespace wabcut::script;
using namespace wabcut::script::test;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaa..Bbbbbbbbbb|
    A1 tgt |Aaaaaaaaaa..Bbbbbbbbbb|
)";

HostLimits fast() {
    HostLimits l;
    l.callTime = 500ms;
    l.grace = 1500ms;
    return l;
}

struct Rig {
    Rig(const std::string& name, std::vector<std::string> caps, const std::string& source, HostLimits limits = fast(),
        bool expectReady = true)
        : tl(kPicture), session(tl.state) {
        dir = scratch("sandbox-" + name);
        writeExtension(dir / "ext", manifest("com.test." + name, "Test " + name, caps), {{"main.js", source}});
        auto bundle = loadExtension(dir / "ext", ExtensionScope::User);
        REQUIRE(bundle.has_value());
        setup.bundle = std::move(*bundle);
        setup.granted = command::CapabilitySet::all();
        setup.session = &session;
        setup.registry = &ai::edit::editingCommands();
        setup.storageFile = dir / "storage.json";
        setup.wabcutVersion = "0.1.0";
        setup.limits = limits;
        start(expectReady);
    }
    void start(bool expectReady = true) {
        delegate.stops.clear();
        auto started = ExtensionHost::start(setup, delegate);
        REQUIRE(started.has_value());
        host = std::move(*started);
        REQUIRE(pumpUntil(*host, delegate, [&] { return host->state() != HostState::Starting; }));
        if (expectReady) {
            INFO((host->stopReport() ? host->stopReport()->reason : std::string()));
            REQUIRE(host->state() == HostState::Ready);
        }
    }
    CallResult run(const std::string& command, Json args = Json::object()) {
        return call(*host, delegate, command, std::move(args));
    }

    wabcut::test::Timeline tl;
    EditSession session;
    fs::path dir;
    HostSetup setup;
    RecordingDelegate delegate;
    std::unique_ptr<ExtensionHost> host;
};

} // namespace

TEST_CASE("Extension scripts have no file; network or process API", "[script][sandbox]") {
    Rig rig("globals", {"Read"}, R"js(
        wabcut.ui.registerCommand('probe', 'Probe', () => {
            const types = {};
            for (const name of ['require', 'process', 'fetch', 'XMLHttpRequest', 'WebSocket', 'Qt', 'print', 'gc',
                                'importModule', 'importScripts', 'qsTr', 'Packages', 'java', 'Deno', 'Bun', 'module',
                                'exports', 'os', 'std', 'File', 'Blob', 'setTimeout']) {
                types[name] = typeof globalThis[name];
            }
            const tries = {};
            for (const [name, code] of [['import', 'import("file:///C:/Windows/win.ini")'],
                                        ['importStatement', 'import x from "./x.js"'],
                                        ['functionGlobal', 'return Object.getOwnPropertyNames(this).includes("wabcut")']]) {
                try {
                    tries[name] = String(new Function(code)());
                } catch (e) {
                    tries[name] = e.name;
                }
            }
            // Nothing reachable from the API is a Qt object (no deleteLater, signals, ...).
            const seen = new Set();
            const qtLike = [];
            const walk = (o, path) => {
                if (o === null || (typeof o !== 'object' && typeof o !== 'function') || seen.has(o)) return;
                seen.add(o);
                for (const k of Object.getOwnPropertyNames(o)) {
                    if (['deleteLater', 'objectName', 'destroyed', 'call', 'post', 'storageSet'].includes(k) &&
                        path !== 'Function.prototype') qtLike.push(path + '.' + k);
                    let v;
                    try { v = o[k]; } catch (e) { continue; }
                    walk(v, path + '.' + k);
                }
            };
            walk(wabcut, 'wabcut');
            walk(console, 'console');
            return {globals: Object.getOwnPropertyNames(globalThis), types, tries, qtLike,
                    frozen: Object.isFrozen(wabcut) && Object.isFrozen(wabcut.commands) && Object.isFrozen(console)};
        });
    )js");
    const CallResult r = rig.run("probe");
    REQUIRE(r.ok);
    for (const auto& [name, type] : r.result["types"].items()) {
        INFO(name);
        CHECK(type == "undefined");
    }
    CHECK(r.result["tries"]["import"] == "SyntaxError");          // no dynamic import()
    CHECK(r.result["tries"]["importStatement"] == "SyntaxError"); // scripts are not modules
    CHECK(r.result["tries"]["functionGlobal"] == "true");
    CHECK(r.result["qtLike"] == Json::array());
    CHECK(r.result["frozen"] == true);
    // Every global is an ECMAScript built-in, or wabcut / console / globalThis.
    const std::set<std::string> allowed{
        "Array", "ArrayBuffer", "Atomics", "Boolean", "DataView", "Date", "Error", "EvalError", "Float32Array",
        "Float64Array", "Function", "Infinity", "Int16Array", "Int32Array", "Int8Array", "JSON", "Map", "Math", "NaN",
        "Number", "Object", "Promise", "Proxy", "RangeError", "ReferenceError", "Reflect", "RegExp", "Set",
        "SharedArrayBuffer", "String", "Symbol", "SyntaxError", "TypeError", "URIError", "URL", "URLSearchParams",
        "Uint16Array", "Uint32Array", "Uint8Array", "Uint8ClampedArray", "WeakMap", "WeakSet", "WeakRef",
        "FinalizationRegistry", "AggregateError", "decodeURI", "decodeURIComponent", "encodeURI",
        "encodeURIComponent", "escape", "eval", "isFinite", "isNaN", "parseFloat", "parseInt", "undefined",
        "unescape", "globalThis", "wabcut", "console"};
    for (const Json& name : r.result["globals"]) {
        INFO(name.get<std::string>());
        CHECK(allowed.contains(name.get<std::string>()));
    }
}

TEST_CASE("A runaway script is stopped at its time limit; the extension runs on; and the editor never waits", "[script][sandbox]") {
    Rig rig("spin", {"Read"}, R"js(
        wabcut.ui.registerCommand('spin', 'Spin', () => { while (true) {} });
        wabcut.ui.registerCommand('catchSpin', 'Catch', () => { try { while (true) {} } catch (e) { return 'caught'; } });
        wabcut.ui.registerCommand('later', 'Later', () => { Promise.resolve().then(() => { for (;;) {} }); return 1; });
        wabcut.ui.registerCommand('ping', 'Ping', () => 'pong');
    )js");
    const auto started = std::chrono::steady_clock::now();
    std::optional<CallResult> spun;
    rig.host->invoke("spin", Json::object(), [&](const CallResult& r) { spun = r; });
    CHECK(std::chrono::steady_clock::now() - started < 50ms);
    std::chrono::steady_clock::duration slowestPump{};
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] {
        const auto t = std::chrono::steady_clock::now();
        rig.host->pump();
        slowestPump = std::max(slowestPump, std::chrono::steady_clock::now() - t);
        return spun.has_value();
    }));
    const auto took = std::chrono::steady_clock::now() - started;
    WARN("while (true) {} stopped after " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
         << " ms (limit 500 ms); slowest pump() meanwhile "
         << std::chrono::duration_cast<std::chrono::microseconds>(slowestPump).count() << " us");
    CHECK(slowestPump < 100ms);
    CHECK_FALSE(spun->ok);
    CHECK(spun->errorKind == "timeLimit");
    CHECK(took >= 450ms);
    CHECK(took < 2500ms);
    // The interruption cannot be caught.
    const CallResult caught = rig.run("catchSpin");
    CHECK(caught.errorKind == "timeLimit");
    // The engine runs again afterwards.
    CHECK(rig.run("ping").result == "pong");
    // A loop in a promise reaction (run by the event loop, outside any call) is stopped too.
    CHECK(rig.run("later").result == 1);
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return rig.delegate.logged("time limit"); }, 5s));
    CHECK(rig.run("ping").result == "pong");
    CHECK(rig.host->state() == HostState::Ready);
}

TEST_CASE("A call waiting on a promise that never settles is cancelled", "[script][sandbox]") {
    Rig rig("never", {"Read"}, R"js(
        wabcut.ui.registerCommand('never', 'Never', () => new Promise(() => {}));
        wabcut.ui.registerCommand('ping', 'Ping', () => 'pong');
    )js");
    const CallResult r = rig.run("never");
    CHECK(r.errorKind == "cancelled");
    CHECK(rig.run("ping").result == "pong");
}

TEST_CASE("An extension that runs out of memory ends its own process; not the editor", "[script][sandbox]") {
    HostLimits limits;
    limits.callTime = 20s;
    limits.memoryBytes = std::size_t{160} * 1024 * 1024;
    Rig rig("memory", {"Read"}, R"js(
        wabcut.ui.registerCommand('grow', 'Grow', () => {
            const keep = [];
            for (;;) { keep.push(new Array(100000).fill(1.5)); }
        });
        wabcut.ui.registerCommand('ping', 'Ping', () => 'pong');
    )js",
            limits);
    const CallResult r = rig.run("grow");
    CHECK_FALSE(r.ok);
    INFO(r.errorKind << ": " << r.error);
    WARN("memory cap 160 MiB: " << (rig.host->state() == HostState::Crashed ? "the helper process ended: " : "refused: ")
                                 << (rig.host->stopReport() ? rig.host->stopReport()->reason : r.error));
    if (rig.host->state() == HostState::Crashed) {
        CHECK(rig.host->stopReport()->memoryLimit);
        CHECK(rig.host->stopReport()->reason.find("memory limit") != std::string::npos);
    } else {
        CHECK(rig.host->state() == HostState::Ready); // the engine refused the allocation itself
    }
    // A fresh start works.
    rig.start();
    CHECK(rig.run("ping").result == "pong");
}

TEST_CASE("Exceptions stay inside the extension", "[script][sandbox]") {
    Rig rig("throws", {"Read"}, R"js(
        wabcut.ui.registerCommand('throws', 'Throws', () => { throw new RangeError('out of beats'); });
        wabcut.ui.registerCommand('rejects', 'Rejects', () => Promise.reject(new Error('later')));
        wabcut.ui.registerCommand('cyclic', 'Cyclic', () => { const o = {}; o.o = o; return o; });
        wabcut.ui.registerCommand('ping', 'Ping', () => 'pong');
        wabcut.events.on('commandApplied', () => { throw new Error('listener broke'); });
    )js");
    const CallResult thrown = rig.run("throws");
    CHECK(thrown.errorKind == "exception");
    CHECK(thrown.error.find("RangeError: out of beats") != std::string::npos);
    CHECK(thrown.error.find("main.js") != std::string::npos); // where it happened
    CHECK(rig.run("rejects").error.find("later") != std::string::npos);
    CHECK(rig.run("cyclic").errorKind == "result");
    rig.host->publish(Event{EventKind::CommandApplied, Json{{"label", "x"}}, 0});
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return rig.delegate.logged("listener broke"); }, 5s));
    CHECK(rig.run("ping").result == "pong");
}

TEST_CASE("Scripts that do not load fail to start; with the place", "[script][sandbox]") {
    Rig syntax("syntax", {"Read"}, "wabcut.ui.registerCommand('a', 'A', () => {;\n", fast(), false);
    CHECK(syntax.host->state() == HostState::Failed);
    CHECK(syntax.host->stopReport()->reason.find("main.js:") != std::string::npos);

    Rig loop("loadloop", {"Read"}, "for (;;) {}", fast(), false);
    CHECK(loop.host->state() == HostState::Failed);
    CHECK(loop.host->stopReport()->reason.find("time limit") != std::string::npos);
}

TEST_CASE("Extensions run only the commands their grants allow", "[script][sandbox][security]") {
    const std::string source = R"js(
        wabcut.ui.registerCommand('try', 'Try', (args) => {
            try {
                return {ok: true, result: wabcut.commands.run(args.command, args.params || {})};
            } catch (e) {
                return {ok: false, code: e.code, name: e.name, details: e.details};
            }
        });
        wabcut.ui.registerCommand('list', 'List', () => wabcut.commands.list().filter((c) => c.allowed).map((c) => c.name));
        wabcut.ui.registerCommand('query', 'Query', (args) => {
            try { return wabcut.project.query(args.command, {}); } catch (e) { return e.name + ': ' + e.message; }
        });
    )js";
    SECTION("Read only: queries yes, edits no (whatever the host offers)") {
        Rig rig("readonly", {"Read"}, source);
        CHECK_FALSE(rig.host->granted().has(command::Capability::EditSequence));
        const CallResult summary = rig.run("try", {{"command", "project.summary"}});
        CHECK(summary.result["ok"] == true);
        const CallResult edit = rig.run("try", {{"command", "marker.add"}, {"params", {{"time", 2}}}});
        CHECK(edit.result["ok"] == false);
        CHECK(edit.result["code"] == "PermissionDenied");
        CHECK(edit.result["name"] == "CommandError");
        CHECK(rig.session.revision() == 0);
        const Json allowed = rig.run("list").result;
        CHECK(std::find(allowed.begin(), allowed.end(), "project.summary") != allowed.end());
        CHECK(std::find(allowed.begin(), allowed.end(), "marker.add") == allowed.end());
        // project.query runs read commands only.
        CHECK(rig.run("query", {{"command", "marker.clearAll"}}).result.get<std::string>().find("read commands only") !=
              std::string::npos);
    }
    SECTION("No grant reaches the files a command's parameters name") {
        Rig rig("nofiles", {"Read", "EditSequence", "EditProject", "MediaAnalysis"}, source);
        for (const char* name : {"review.exportPackage", "history.export", "lut.import", "project.merge", "plugins.scan"}) {
            INFO(name);
            const CallResult r = rig.run("try", {{"command", name}, {"params", {{"path", "C:/Windows/win.ini"}}}});
            CHECK(r.result["code"] == "PermissionDenied");
            CHECK(r.result["details"]["touchesFiles"] == true);
        }
        const Json allowed = rig.run("list").result;
        CHECK(std::find(allowed.begin(), allowed.end(), "review.exportPackage") == allowed.end());
        CHECK(std::find(allowed.begin(), allowed.end(), "bin.create") != allowed.end());
    }
    SECTION("Commands that run a model need MediaAnalysis on top of their own capability") {
        Rig rig("nomodel", {"Read", "EditSequence"}, source);
        const CallResult translate = rig.run("try", {{"command", "captions.translate"}, {"params", {{"languages", {"th"}}}}});
        CHECK(translate.result["code"] == "PermissionDenied");
        CHECK(translate.result["details"]["runsModel"] == true);
        const Json allowed = rig.run("list").result;
        CHECK(std::find(allowed.begin(), allowed.end(), "marker.add") != allowed.end());
        CHECK(std::find(allowed.begin(), allowed.end(), "captions.translate") == allowed.end());
        CHECK(std::find(allowed.begin(), allowed.end(), "transcript.transcribe") == allowed.end());
    }
}

TEST_CASE("Every edit an extension makes is one undo step named after it", "[script][sandbox]") {
    Rig rig("undo", {"Read", "EditSequence"}, R"js(
        wabcut.ui.registerCommand('one', 'One', () => wabcut.commands.run('marker.add', {time: 2, name: 'Here'}).revision);
        wabcut.ui.registerCommand('many', 'Many', () => wabcut.commands.batch([
            {command: 'marker.add', params: {time: 4}}, {command: 'marker.add', params: {time: 6}}],
            {label: 'Add Two Markers'}).revision);
        wabcut.ui.registerCommand('preview', 'Preview', () => {
            const r = wabcut.commands.run('marker.add', {time: 8}, {dryRun: true});
            return {dryRun: r.dryRun, committed: r.committed};
        });
    )js");
    CHECK(rig.run("one").result == 1);
    CHECK(rig.session.undoLabel() == "Test undo: Add Marker");
    CHECK(rig.run("many").result == 2);
    CHECK(rig.session.undoLabel() == "Test undo: Add Two Markers");
    CHECK(rig.run("preview").result == Json{{"dryRun", true}, {"committed", false}});
    CHECK(rig.session.revision() == 2);
    REQUIRE(rig.session.undo().has_value());
    CHECK(rig.session.snapshot()->findSequence(rig.tl.seq)->markers.size() == 1);
}

TEST_CASE("Editor events reach the listeners throttled; and only the kinds they listen to", "[script][sandbox]") {
    Rig rig("events", {"Read"}, R"js(
        let received = 0, folded = 0, last = null, saves = 0;
        const stop = wabcut.events.on('playheadMoved', (data, info) => { received++; folded += info.coalesced; last = data.time; });
        wabcut.events.on('projectSaved', () => { saves++; });
        wabcut.ui.registerCommand('count', 'Count', () => ({received, folded, last, saves}));
        wabcut.ui.registerCommand('stop', 'Stop', () => { stop(); return true; });
    )js");
    for (int i = 0; i < 50; ++i) {
        rig.host->publish(Event{EventKind::PlayheadMoved, Json{{"time", i}}, 0});
        rig.host->publish(Event{EventKind::SelectionChanged, Json{{"clips", Json::array()}}, 0}); // not listened to
        rig.host->pump();
    }
    std::this_thread::sleep_for(300ms);
    rig.host->pump();
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return !rig.host->nextDeadline(); }, 2s));
    const CallResult counts = rig.run("count");
    CHECK(counts.result["received"].get<int>() <= 4);
    CHECK(counts.result["received"].get<int>() + counts.result["folded"].get<int>() == 50);
    CHECK(counts.result["last"] == 49); // the latest wins
    // Unsubscribed: nothing more arrives.
    CHECK(rig.run("stop").result == true);
    rig.host->publish(Event{EventKind::PlayheadMoved, Json{{"time", 99}}, 0});
    std::this_thread::sleep_for(150ms);
    rig.host->pump();
    CHECK(rig.run("count").result["last"] == 49);
}

TEST_CASE("Extension storage persists between runs and keeps its bounds", "[script][sandbox]") {
    const std::string source = R"js(
        wabcut.ui.registerCommand('set', 'Set', (args) => { wabcut.storage.set(args.key, args.value); return wabcut.storage.keys(); });
        wabcut.ui.registerCommand('get', 'Get', (args) => wabcut.storage.get(args.key, 'none'));
        wabcut.ui.registerCommand('big', 'Big', () => {
            try { wabcut.storage.set('big', 'x'.repeat(70000)); return 'stored'; } catch (e) { return e.name; }
        });
        wabcut.ui.registerCommand('remove', 'Remove', (args) => wabcut.storage.remove(args.key));
    )js";
    Rig rig("storage", {"Read"}, source);
    CHECK(rig.run("set", {{"key", "every"}, {"value", {{"n", 4}}}}).result == Json::array({"every"}));
    CHECK(rig.run("get", {{"key", "every"}}).result == Json{{"n", 4}});
    CHECK(rig.run("get", {{"key", "missing"}}).result == "none");
    CHECK(rig.run("big").result == "RangeError");
    rig.host->stop();
    REQUIRE(fs::exists(rig.dir / "storage.json"));
    rig.start();
    CHECK(rig.run("get", {{"key", "every"}}).result == Json{{"n", 4}});
    CHECK(rig.run("remove", {{"key", "every"}}).result == true);
    rig.host->stop();
    rig.start();
    CHECK(rig.run("get", {{"key", "every"}}).result == "none");
}

TEST_CASE("Prompts; notifications and palette commands go through the editor", "[script][sandbox]") {
    Rig rig("ui", {"Read"}, R"js(
        wabcut.ui.registerCommand('ask', 'Ask', () =>
            wabcut.ui.prompt({title: 'How many?', fields: [{id: 'n', type: 'number', label: 'N', value: 2, min: 1, max: 9}]})
                .then((answer) => ({answer})));
        wabcut.ui.registerCommand('badForm', 'Bad form', () => {
            try { wabcut.ui.prompt({title: 'x', fields: [{id: 'f', type: 'file', label: 'File'}]}); return 'shown'; }
            catch (e) { return e.name; }
        });
        wabcut.ui.registerCommand('shout', 'Shout', (args) => {
            for (let i = 0; i < args.times; i++) wabcut.ui.notify('note ' + i, {level: i === 0 ? 'warning' : 'info'});
            wabcut.ui.toast('done');
            return true;
        });
        wabcut.ui.registerCommand('badId', 'x', () => {
            try { wabcut.ui.registerCommand('../x', 'X', () => 1); return 'registered'; } catch (e) { return e.name; }
        });
    )js");
    REQUIRE(rig.host->commands().size() == 4);
    CHECK(rig.host->commands()[0].title == "Ask");
    CHECK(rig.delegate.commandsChangedCount >= 4);

    // A prompt, answered.
    std::optional<CallResult> asked;
    rig.host->invoke("ask", Json::object(), [&](const CallResult& r) { asked = r; });
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return !rig.delegate.prompts.empty(); }));
    const PromptRequest p = rig.delegate.prompts.back();
    CHECK(p.form["title"] == "How many?");
    CHECK_FALSE(rig.host->answerPrompt(p.prompt, Json{{"n", 42}})); // outside the field's bounds: not sent
    std::this_thread::sleep_for(std::chrono::milliseconds(2500)); // the user takes a while: no time limit meanwhile
    CHECK(rig.host->answerPrompt(p.prompt, Json{{"n", 3}}));
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return asked.has_value(); }));
    CHECK(asked->result == Json{{"answer", {{"n", 3}}}});
    CHECK_FALSE(rig.host->answerPrompt(p.prompt, Json{{"n", 3}})); // answered already

    // Cancelled.
    asked.reset();
    rig.host->invoke("ask", Json::object(), [&](const CallResult& r) { asked = r; });
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return rig.delegate.prompts.size() == 2; }));
    CHECK(rig.host->answerPrompt(rig.delegate.prompts.back().prompt, std::nullopt));
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return asked.has_value(); }));
    CHECK(asked->result == Json{{"answer", nullptr}});

    CHECK(rig.run("badForm").result == "TypeError");
    CHECK(rig.run("badId").result == "TypeError");

    // Notifications, and their rate limit (30 a minute).
    CHECK(rig.run("shout", {{"times", 3}}).result == true);
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return rig.delegate.notifications.size() == 4; }));
    CHECK(rig.delegate.notifications[0].level == "warning");
    CHECK(rig.delegate.notifications[3].level == "toast");
    CHECK(rig.run("shout", {{"times", 40}}).result == true);
    pumpUntil(*rig.host, rig.delegate, [] { return false; }, 300ms);
    CHECK(rig.delegate.notifications.size() <= 30);

    // A prompt still open when the extension stops is closed.
    rig.host->invoke("ask", Json::object(), [](const CallResult&) {});
    REQUIRE(pumpUntil(*rig.host, rig.delegate, [&] { return rig.delegate.prompts.size() == 3; }));
    rig.host->stop();
    CHECK(rig.delegate.closedPrompts.size() == 1);
}
