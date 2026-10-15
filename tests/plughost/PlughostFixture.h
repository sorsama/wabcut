#pragma once

// Plugin-host test helpers: the test plugins built next to the tests (WabcutTestGain.clap / .vst3, and two CLAP
// files that crash or hang while scanned), a catalog of them, a host and the plugin effects installed into the
// engine for the test's lifetime.

#include <wabcut/plughost/Ipc.h>
#include <wabcut/plughost/PluginCatalog.h>
#include <wabcut/plughost/PluginEffects.h>
#include <wabcut/plughost/PluginHost.h>

#include <wabcut/audio/effects/EffectChain.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::test {

inline std::filesystem::path testPluginDir() { return std::filesystem::path(WABCUT_TEST_PLUGIN_DIR); }
inline std::filesystem::path plughostPath() { return std::filesystem::path(WABCUT_PLUGHOST_PATH); }

inline std::filesystem::path plughostOutput(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "plughost";
    std::filesystem::create_directories(dir);
    return dir / name;
}

/// The CLAP test plugin's state blob (TestGainClap.cpp): gain dB, latency frames, crash, stall ms.
inline std::string clapState(double gainDb, double latency, double crash = 0.0, double stall = 0.0) {
    struct Blob {
        std::uint32_t magic;
        double gain, latency, crash, stall;
    } blob;
    std::memset(&blob, 0, sizeof blob);
    blob.magic = 0x57544731;
    blob.gain = gainDb;
    blob.latency = latency;
    blob.crash = crash;
    blob.stall = stall;
    return plughost::base64Encode(&blob, sizeof blob);
}

/// The VST3 test plugin's component state blob plus an empty controller state (Vst3Host's "WV31" chunk).
inline std::string vst3State(double gainNormalised, double crash, std::int32_t latency) {
    std::vector<std::uint8_t> component(20);
    std::memcpy(component.data(), &gainNormalised, 8);
    std::memcpy(component.data() + 8, &crash, 8);
    std::memcpy(component.data() + 16, &latency, 4);
    std::vector<std::uint8_t> chunk{'W', 'V', '3', '1'};
    const auto append = [&](const std::vector<std::uint8_t>& part) {
        const auto n = static_cast<std::uint32_t>(part.size());
        for (int k = 0; k < 4; ++k) {
            chunk.push_back(static_cast<std::uint8_t>(n >> (8 * k)));
        }
        chunk.insert(chunk.end(), part.begin(), part.end());
    };
    append(component);
    append({});
    return plughost::base64Encode(chunk.data(), chunk.size());
}

/// The tests' host options: plugins load on the thread that creates their processor and a block may take 3 s ahead of
/// playback too (tests compare from the first sample on loaded computers). Loading on a worker and the player's short
/// block deadline have tests of their own with the defaults.
inline plughost::HostOptions testHostOptions() {
    plughost::HostOptions options;
    options.deferInstantiation = false;
    options.playbackBlockTimeout = options.blockTimeout;
    return options;
}

/// The test plugins, scanned, hosted and installed as the engine's plugin effects (until destroyed).
struct TestPlugins {
    std::shared_ptr<plughost::PluginCatalog> catalog;
    std::shared_ptr<plughost::PluginHost> host;
    std::shared_ptr<plughost::PluginEffects> effects;

    explicit TestPlugins(plughost::HostOptions hostOptions = testHostOptions()) {
        static std::atomic<int> serial{0};
        plughost::CatalogOptions options;
        options.helper = plughostPath();
        options.folders = {testPluginDir() / "clap", testPluginDir() / "vst3"};
        options.cacheFile = plughostOutput("catalog-" + std::to_string(serial.fetch_add(1)) + ".json");
        std::filesystem::remove(options.cacheFile);
        catalog = std::make_shared<plughost::PluginCatalog>(options);
        const plughost::ScanReport report = catalog->scan();
        REQUIRE(report.plugins == 2);
        hostOptions.helper = plughostPath();
        hostOptions.logFile = plughostOutput("helpers.log");
        host = plughost::PluginHost::create(hostOptions);
        effects = plughost::PluginEffects::create(host, catalog);
        plughost::installPluginEffects(effects);
    }
    ~TestPlugins() {
        audio::setExternalEffectFactory(nullptr);
        setEffectDescriptorProvider(nullptr);
    }

    plughost::PluginInfo clap() const {
        auto p = catalog->find("plugin.clap.org.wabcut.test.gain");
        REQUIRE(p);
        return *p;
    }
    plughost::PluginInfo vst3() const {
        for (const auto& p : catalog->plugins()) {
            if (p.format == "vst3") {
                return p;
            }
        }
        FAIL("no VST3 test plugin");
        return {};
    }
};

/// A plugin effect instance with `params` (key, constant or curve) and `state` (base64).
inline EffectInstance pluginEffect(const plughost::PluginInfo& plugin, std::uint64_t id, std::vector<EffectParam> params = {},
                                   const std::string& state = {}) {
    EffectInstance e = plughost::makePluginEffect(plugin, EffectInstanceId{id});
    for (EffectParam& p : params) {
        e.set(std::move(p.key), std::move(p.value));
    }
    if (!state.empty()) {
        REQUIRE(plughost::setPluginState(e, state));
    }
    return e;
}

/// Runs `chain` over `input` (interleaved stereo) in pieces of `piece` frames from chain frame 0.
inline std::vector<float> runChain(audio::EffectChain& chain, std::vector<float> input, int piece = 333) {
    const auto frames = static_cast<int>(input.size() / 2);
    for (int done = 0; done < frames;) {
        const int n = std::min(piece, frames - done);
        chain.process(input.data() + static_cast<std::size_t>(done) * 2, n, done, audio::CurveClock{});
        done += n;
    }
    return input;
}

/// Interleaved stereo test signal: a 440 Hz sine left, 660 Hz right.
inline std::vector<float> stereoTone(int frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    for (int k = 0; k < frames; ++k) {
        out[static_cast<std::size_t>(k) * 2] = static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 440.0 * k / 48000.0));
        out[static_cast<std::size_t>(k) * 2 + 1] = static_cast<float>(0.4 * std::sin(2.0 * 3.141592653589793 * 660.0 * k / 48000.0));
    }
    return out;
}

inline float dbGain(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

} // namespace wabcut::test
