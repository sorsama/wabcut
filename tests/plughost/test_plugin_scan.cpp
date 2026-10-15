// The plugin scanner (A.6): one helper per plugin file, so the CLAP that crashes while scanned and the one that hangs
// go on the blocklist and nothing else is disturbed; the cache (unchanged files are not scanned again, the blocklist
// survives a restart); user folders; and real plugins on this computer (hidden test: [.real-plugins]).

#include "PlughostFixture.h"

#include <wabcut/plughost/Ipc.h>

#include <chrono>
#include <cstdio>

using namespace wabcut;
using namespace wabcut::test;

TEST_CASE("The scanner blocks plugins that crash or hang while scanned, and caches the rest", "[plughost][scan]") {
    plughost::CatalogOptions options;
    options.helper = plughostPath();
    options.folders = {testPluginDir()}; // clap/, vst3/ and blocked/ (crash and hang on scan)
    options.cacheFile = plughostOutput("scan-catalog.json");
    options.scanTimeout = std::chrono::milliseconds(3000);
    std::filesystem::remove(options.cacheFile);

    plughost::PluginCatalog catalog(options);
    const auto t0 = std::chrono::steady_clock::now();
    const plughost::ScanReport first = catalog.scan();
    const double firstSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(first.files == 4);
    CHECK(first.scanned == 4);
    CHECK(first.plugins == 2);
    CHECK(first.added == 2);
    REQUIRE(first.blocked.size() == 2);
    const auto reasonOf = [&](std::string_view file) {
        for (const auto& b : first.blocked) {
            if (b.path.filename().string() == file) {
                return b.reason;
            }
        }
        return std::string();
    };
    CHECK(reasonOf("WabcutTestCrashOnScan.clap").find("crashed while being scanned") != std::string::npos);
    CHECK(reasonOf("WabcutTestHangOnScan.clap").find("did not finish") != std::string::npos);
    WARN("first scan of 4 files (one hangs for the 3 s timeout): " << firstSeconds << " s");

    // What a scan finds.
    const auto clap = catalog.find("plugin.clap.org.wabcut.test.gain");
    REQUIRE(clap);
    CHECK(clap->name == "Wabcut Test Gain");
    CHECK(clap->vendor == "Wabcut");
    CHECK(clap->version == "1.0.0");
    CHECK(clap->category.find("audio-effect") != std::string::npos);
    CHECK(clap->params.size() == 5);
    CHECK(clap->latency == 0);
    CHECK_FALSE(clap->editor);
    bool vst3 = false;
    for (const auto& p : catalog.plugins()) {
        if (p.format == "vst3") {
            vst3 = true;
            CHECK(p.name == "Wabcut Test Gain VST3");
            CHECK(p.id.size() == 32);
            CHECK(p.category == "Fx");
            CHECK(p.params.size() == 2);
            CHECK(p.effectId() == "plugin.vst3." + p.id);
        }
    }
    CHECK(vst3);

    // Again: nothing is scanned, the blocked files are skipped.
    const auto t1 = std::chrono::steady_clock::now();
    const plughost::ScanReport second = catalog.scan();
    const double secondSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    CHECK(second.scanned == 0);
    CHECK(second.cached == 2);
    CHECK(second.skipped == 2);
    CHECK(second.plugins == 2);
    CHECK(second.added == 0);
    CHECK(secondSeconds < 1.0);

    // A restart keeps the plugins and the blocklist.
    plughost::PluginCatalog restarted(options);
    REQUIRE(restarted.load());
    CHECK(restarted.plugins().size() == 2);
    CHECK(restarted.blocked().size() == 2);
    // Unblocking a file scans it again (and it is blocked again: it still crashes).
    restarted.unblock(testPluginDir() / "blocked" / "WabcutTestCrashOnScan.clap");
    const plughost::ScanReport third = restarted.scan();
    CHECK(third.scanned == 1);
    CHECK(third.blocked.size() == 1);
    CHECK(restarted.blocked().size() == 2);
    // A rescan scans everything that is not blocked.
    CHECK(restarted.scan({}, {}, true).scanned == 2);
}

TEST_CASE("User folders are scanned and remembered", "[plughost][scan]") {
    const auto cache = plughostOutput("user-folders.json");
    std::filesystem::remove(cache);
    plughost::CatalogOptions options;
    options.helper = plughostPath();
    options.folders = {testPluginDir() / "vst3"};
    options.cacheFile = cache;
    {
        plughost::PluginCatalog catalog(options);
        catalog.addUserFolder(testPluginDir() / "clap");
        const auto report = catalog.scan();
        CHECK(report.plugins == 2);
    }
    plughost::PluginCatalog again(options);
    REQUIRE(again.load());
    REQUIRE(again.userFolders().size() == 1);
    CHECK(again.userFolders()[0].filename() == "clap");
    again.removeUserFolder(testPluginDir() / "clap");
    CHECK(again.scan().plugins == 1); // the CLAP folder's plugin is gone with its folder
}

TEST_CASE("Real plugins on this computer scan and process (when there are any)", "[.real-plugins]") {
    plughost::CatalogOptions options;
    options.helper = plughostPath();
    options.cacheFile = plughostOutput("real-catalog.json");
    std::filesystem::remove(options.cacheFile);
    plughost::PluginCatalog catalog(options);
    const auto report = catalog.scan();
    std::printf("scanned %d files in the standard folders: %d plugins, %zu blocked, %zu errors\n", report.files, report.plugins,
                report.blocked.size(), report.errors.size());
    for (const auto& b : report.blocked) {
        std::printf("  blocked: %s (%s)\n", b.path.string().c_str(), b.reason.c_str());
    }
    for (const auto& e : report.errors) {
        std::printf("  error: %s\n", e.c_str());
    }
    auto host = plughost::PluginHost::create({plughostPath(), plughostOutput("real-helpers.log")});
    for (const auto& p : catalog.plugins()) {
        std::printf("  %s %s | %s %s | %zu params | latency %d | editor %s | %s\n", p.format.c_str(), p.name.c_str(), p.vendor.c_str(),
                    p.version.c_str(), p.params.size(), p.latency, p.editor ? "yes" : "no", p.category.c_str());
        if (!p.effect || !p.problem.empty()) {
            continue;
        }
        // One second of a tone through it: it must come back, and not as silence.
        auto instance = host->instantiate(p, 48000.0);
        if (!instance) {
            std::printf("    cannot instantiate: %s\n", instance.error().message.c_str());
            continue;
        }
        plughost::PluginInstance& i = **instance;
        std::uint64_t submitted = 0;
        double energy = 0.0;
        const auto start = std::chrono::steady_clock::now();
        const std::vector<float> tone = stereoTone(48000);
        for (int block = 0; block < 48000 / plughost::kBlockFrames; ++block) {
            plughost::BlockSlot& slot = i.shared().ring[submitted % plughost::kSlots];
            slot.block = submitted;
            slot.frames = plughost::kBlockFrames;
            slot.flags = submitted == 0 ? plughost::kSlotReset : 0u;
            slot.eventCount = 0;
            for (int k = 0; k < plughost::kBlockFrames; ++k) {
                slot.input[0][k] = tone[static_cast<std::size_t>(block * plughost::kBlockFrames + k) * 2];
                slot.input[1][k] = tone[static_cast<std::size_t>(block * plughost::kBlockFrames + k) * 2 + 1];
            }
            i.shared().submitted.store(++submitted, std::memory_order_release);
            i.signalWork();
            while (i.shared().completed.load(std::memory_order_acquire) < submitted && i.running()) {
                i.waitDone(std::chrono::milliseconds(50));
            }
            for (int k = 0; k < plughost::kBlockFrames; ++k) {
                energy += static_cast<double>(slot.output[0][k]) * slot.output[0][k];
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("    1 s of tone: %s, rms %.4f, latency %d, %.1f us per block\n",
                    i.running() ? "processed" : std::string(i.problem()).c_str(), std::sqrt(energy / 48000.0), i.latency(),
                    1000.0 * ms / (48000.0 / plughost::kBlockFrames));
        CHECK(i.running());
    }
}
