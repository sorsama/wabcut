// Review wave 5, S5-P2-6: nothing wabcut-plughost writes may end the editor, and what plug-ins print never reaches the
// control channel. Before the fix:
//   * the host's reader thread called message["id"].get<std::uint64_t>() on every line that parsed as an object:
//     {"id":"x"} threw json::type_error on that thread and std::terminate ended the editor (here: this test binary);
//   * a line that never ended grew the editor's buffer without bound (the request then timed out instead);
//   * the scanner took the first line a scan printed as its answer, and a field of the wrong type in it threw on the
//     scan thread;
//   * the helper's stdout was the plug-ins' too: a printf from process() (no newline) glued onto the next reply, which
//     was lost - the request timed out after 20 s and the instance looked hung.
// The fake helper (fake_plughost.cpp) writes such lines before every reply and malformed scan answers.

#include "PlughostFixture.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

std::filesystem::path fakeHelperPath() { return std::filesystem::path(WABCUT_FAKE_PLUGHOST_PATH); }

/// A plugin for the fake helper: its id says what the helper does with "create" (fake_plughost.cpp).
plughost::PluginInfo fakePlugin(const std::string& mode) {
    plughost::PluginInfo p;
    p.format = "clap";
    p.id = mode;
    p.name = "Fake " + mode;
    p.vendor = "Wabcut";
    p.path = plughostOutput("fake-" + mode + ".clap"); // never loaded: the fake helper only answers
    return p;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

/// The CLAP test plugin's longer state blob (TestGainClap.cpp's StateBlob), here with process() printing to stdout.
std::string chattyState(double gainDb) {
    struct Blob {
        std::uint32_t magic;
        double gain, latency, crash, stall, stateMs, chatter;
    } blob;
    static_assert(sizeof(Blob) == 56, "TestGainClap.cpp's StateBlob");
    std::memset(&blob, 0, sizeof blob);
    blob.magic = 0x57544731;
    blob.gain = gainDb;
    blob.chatter = 1.0;
    return plughost::base64Encode(&blob, sizeof blob);
}

} // namespace

TEST_CASE("Malformed control lines from the helper are dropped and every request still gets its own reply",
          "[plughost][ipc]") {
    plughost::HostOptions options;
    options.helper = fakeHelperPath();
    options.logFile = plughostOutput("fake-helper.log");
    options.requestTimeout = std::chrono::milliseconds(30000);
    auto host = plughost::PluginHost::create(options);

    SECTION("a create the helper refuses: its own error, at once") {
        const auto t0 = std::chrono::steady_clock::now();
        const auto made = host->instantiate(fakePlugin("refuse"), 48000.0);
        REQUIRE_FALSE(made.has_value());
        CHECK(made.error().message.find("the fake helper hosts no plugins") != std::string::npos);
        CHECK(secondsSince(t0) < 10.0); // the replies came: no request waited for its timeout
        CHECK(host->processCount() == 1); // nothing it wrote ended the channel
    }
    SECTION("an instance whose replies carry fields of the wrong type") {
        const auto t0 = std::chrono::steady_clock::now();
        auto made = host->instantiate(fakePlugin("accept"), 48000.0);
        REQUIRE(made.has_value());
        plughost::PluginInstance& instance = **made;
        CHECK(instance.running());
        CHECK_FALSE(instance.hasEditor()); // "editor": "yes" reads as the fallback
        const auto state = instance.saveState(); // "state": 12
        REQUIRE(state.has_value());
        CHECK(state->empty());
        const auto values = instance.parameterValues(); // one well-formed entry among four that are not
        REQUIRE(values.has_value());
        REQUIRE(values->size() == 1);
        CHECK((*values)[0].first == 3u);
        CHECK((*values)[0].second == 0.5);
        CHECK(secondsSince(t0) < 10.0);
        CHECK(host->processCount() == 1);
    }
    SECTION("a line that never ends: the helper is given up as crashed, not waited for") {
        const auto made = host->instantiate(fakePlugin("endless"), 48000.0);
        REQUIRE_FALSE(made.has_value());
        // Before the fix the editor buffered the line without bound and the request timed out (30 s here).
        CHECK(made.error().message.find("did not answer in time") == std::string::npos);
        CHECK(made.error().message.find("crashed") != std::string::npos);
        CHECK(host->processCount() == 0);
        // The host carries on: the next instance gets a new helper.
        const auto again = host->instantiate(fakePlugin("refuse"), 48000.0);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().message.find("the fake helper hosts no plugins") != std::string::npos);
    }
}

TEST_CASE("A scan answer with fields of the wrong type is an error for its file and never the end of the scan",
          "[plughost][scan][ipc]") {
    const std::filesystem::path folder = plughostOutput("fake-scan");
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const std::vector<std::string> names{"latency-string", "param-id-string", "name-number", "ok-string", "plugins-object"};
    for (const std::string& name : names) {
        std::ofstream(folder / (name + ".clap"), std::ios::binary) << "not a plugin";
    }
    plughost::CatalogOptions options;
    options.helper = fakeHelperPath();
    options.folders = {folder};
    options.cacheFile = plughostOutput("fake-scan.json");
    options.scanTimeout = std::chrono::milliseconds(10000);
    std::filesystem::remove(options.cacheFile);
    plughost::PluginCatalog catalog(options);

    plughost::ScanReport report;
    CHECK_NOTHROW(report = catalog.scan());
    CHECK(report.files == 5);
    CHECK(report.scanned == 5);
    CHECK(report.plugins == 0);
    CHECK(report.blocked.empty()); // the helper exited normally: an answer that is wrong, not a crash
    REQUIRE(report.errors.size() == names.size());
    const auto errorOf = [&](const std::string& name) {
        for (const std::string& e : report.errors) {
            if (e.find(name + ".clap") != std::string::npos) {
                return e;
            }
        }
        return std::string();
    };
    for (const std::string& name : names) {
        INFO(name);
        CHECK_FALSE(errorOf(name).empty());
    }
    CHECK(errorOf("latency-string").find("malformed") != std::string::npos);
    CHECK(errorOf("param-id-string").find("malformed") != std::string::npos);
    CHECK(errorOf("name-number").find("malformed") != std::string::npos);
    CHECK(errorOf("ok-string").find("malformed") != std::string::npos);
    CHECK(errorOf("plugins-object").find("lists no plugins") != std::string::npos);
    CHECK(catalog.plugins().empty());

    // On the thread the app scans on (PluginBridge: startScan) too: it reports and ends normally.
    // (The callback owns the promise: the worker may still be returning from it when the future is ready.)
    auto finished = std::make_shared<std::promise<plughost::ScanReport>>();
    std::future<plughost::ScanReport> result = finished->get_future();
    catalog.startScan([finished](const plughost::ScanReport& r) { finished->set_value(r); }, true);
    REQUIRE(result.wait_for(std::chrono::seconds(60)) == std::future_status::ready);
    CHECK(result.get().errors.size() == names.size());
}

TEST_CASE("A plugin that prints from process() leaves the control channel intact and is not hung", "[plughost][ipc]") {
    TestPlugins plugins;
    const std::filesystem::path log = plughostOutput("helpers.log"); // TestPlugins' helper log
    std::error_code ec;
    const std::uintmax_t logBefore = std::filesystem::exists(log, ec) ? std::filesystem::file_size(log, ec) : 0;

    // 1 s through the plugin: every process() call prints a line without a newline.
    audio::EffectChain chain(2, 48000);
    const EffectStack stack{pluginEffect(plugins.clap(), 951, {}, chattyState(-6.0))};
    chain.bind(stack);
    const int delay = chain.latencyFrames();
    const std::vector<float> in = stereoTone(48000);
    const auto out = runChain(chain, in);
    std::size_t wrong = 0;
    for (int s = delay; s < 48000; ++s) {
        const float expected = in[static_cast<std::size_t>(s - delay) * 2] * dbGain(-6.0);
        wrong += std::fabs(out[static_cast<std::size_t>(s) * 2] - expected) > 1e-6f ? 1 : 0;
    }
    CHECK(wrong == 0);
    const auto instance = plugins.effects->instanceFor(EffectInstanceId{951});
    REQUIRE(instance);

    // Control requests after 187 blocks of chatter: each gets its reply at once (before the fix: lost, 20 s each).
    const auto t0 = std::chrono::steady_clock::now();
    const auto state = instance->saveState();
    REQUIRE(state.has_value());
    CHECK(*state == chattyState(-6.0));
    const auto values = instance->parameterValues();
    REQUIRE(values.has_value());
    CHECK(values->size() == 5);
    const auto another = plugins.host->instantiate(plugins.clap(), 48000.0, clapState(-3.0, 0.0));
    REQUIRE(another.has_value());
    CHECK(secondsSince(t0) < 10.0);
    CHECK(instance->health() == plughost::InstanceHealth::Running);
    CHECK(instance->missedBlocks() == 0);
    CHECK(plugins.host->processCount() == 1); // the same helper answered everything

    // What the plugin printed went to the helper's log instead.
    std::ifstream in2(log, std::ios::binary);
    REQUIRE(in2.good());
    in2.seekg(static_cast<std::streamoff>(logBefore));
    const std::string added((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    CHECK(added.find("chatter from process()") != std::string::npos);
}
