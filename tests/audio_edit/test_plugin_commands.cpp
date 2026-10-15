// plugins.list, plugins.scan and plugin effects through the existing effect commands (A.6), with a fake plugin library
// and plugin descriptors (pure: the core preset runs these too; the real catalog and helper are tested in plughost).

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/PluginLibrary.h>
#include <wabcut/model/Effects.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr const char* kGainId = "plugin.clap.org.example.gain";

class FakeLibrary final : public PluginLibrary {
public:
    PluginScanOptions lastScan;
    int scans = 0;

    std::vector<PluginListing> plugins() const override {
        PluginListing gain;
        gain.effect = kGainId;
        gain.format = "clap";
        gain.name = "Example Gain";
        gain.vendor = "Example";
        gain.version = "2.0";
        gain.category = "audio-effect";
        gain.latency = 64;
        gain.params = {{"p1", "Gain", -60.0, 12.0, 0.0, true}, {"p2", "Mode", 0.0, 3.0, 0.0, false}};
        PluginListing eq;
        eq.effect = "plugin.vst3.00112233445566778899AABBCCDDEEFF";
        eq.format = "vst3";
        eq.name = "Big EQ";
        eq.vendor = "Somebody";
        eq.editor = true;
        return {gain, eq};
    }
    std::vector<BlockedPluginListing> blocked() const override { return {{"C:\\VST3\\Broken.vst3", "crashed while being scanned"}}; }
    std::vector<std::string> folders() const override { return {"C:\\Program Files\\Common Files\\VST3"}; }
    std::expected<PluginScanSummary, Error> scan(const PluginScanOptions& options, std::stop_token,
                                                const std::function<void(double)>& progress) override {
        lastScan = options;
        ++scans;
        if (progress) {
            progress(0.5);
            progress(1.0);
        }
        PluginScanSummary s;
        s.files = 3;
        s.scanned = 2;
        s.cached = 1;
        s.plugins = 2;
        s.added = 1;
        s.blocked = {{"C:\\VST3\\Hang.vst3", "did not finish scanning within 30 s"}};
        return s;
    }
};

/// The descriptor a scanned plugin gives the model (the catalog's provider does the same for real plugins).
class FakeDescriptors final : public EffectDescriptorProvider {
public:
    FakeDescriptors() {
        m_params = {EffectParamInfo{"p1", "Gain", ParamUnit::Amount, 0.0, -60.0, 12.0, true},
                    EffectParamInfo{"p2", "Mode", ParamUnit::Amount, 0.0, 0.0, 3.0, false}};
        m_descriptor.id = kGainId;
        m_descriptor.displayName = "Example Gain";
        m_descriptor.category = "Audio Plugins/Example";
        m_descriptor.kind = EffectKind::Audio;
        m_descriptor.params = m_params;
        m_descriptor.capabilities = EffectCapability::AudioStateful;
    }
    const EffectDescriptor* find(std::string_view id) const override { return id == kGainId ? &m_descriptor : nullptr; }

private:
    std::vector<EffectParamInfo> m_params;
    EffectDescriptor m_descriptor;
};

struct Fixture {
    Fixture() : library(std::make_shared<FakeLibrary>()), h(Timeline("A1 |A" + std::string(49, 'a') + "|"), policy(), ai::edit::editingCommands()) {
        setEffectDescriptorProvider(std::make_shared<FakeDescriptors>());
    }
    ~Fixture() { setEffectDescriptorProvider(nullptr); }
    command::EnginePolicy policy() {
        return command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<PluginServices>(library)};
    }
    std::shared_ptr<FakeLibrary> library;
    CommandHarness h;
};

} // namespace

TEST_CASE("plugins.list lists the plugins with their parameters, the blocklist and the folders", "[audio][edit][plugins]") {
    Fixture f;
    auto r = f.h.ok("plugins.list");
    REQUIRE(r.data["plugins"].size() == 2);
    CHECK(r.data["plugins"][0]["effect"] == kGainId);
    CHECK(r.data["plugins"][0]["latency"] == 64);
    CHECK(r.data["plugins"][0]["parameterCount"] == 2);
    CHECK_FALSE(r.data["plugins"][0].contains("params"));
    CHECK(r.data["blocked"][0]["reason"] == "crashed while being scanned");
    CHECK(r.data["folders"].size() == 1);
    r = f.h.ok("plugins.list", {{"query", "EQ"}, {"params", true}});
    REQUIRE(r.data["plugins"].size() == 1);
    CHECK(r.data["plugins"][0]["name"] == "Big EQ");
    r = f.h.ok("plugins.list", {{"format", "clap"}, {"params", true}});
    REQUIRE(r.data["plugins"].size() == 1);
    CHECK(r.data["plugins"][0]["params"][1]["key"] == "p2");
    CHECK(r.data["plugins"][0]["params"][1]["automatable"] == false);
}

TEST_CASE("plugins.scan passes its options on and reports what went on the blocklist", "[audio][edit][plugins]") {
    Fixture f;
    const auto r = f.h.ok("plugins.scan", {{"rescan", true}, {"addFolders", Json::array({"D:\\Plugins"})}, {"unblock", Json::array({"C:\\VST3\\Broken.vst3"})}});
    CHECK(f.library->scans == 1);
    CHECK(f.library->lastScan.rescan);
    CHECK(f.library->lastScan.addFolders == std::vector<std::string>{"D:\\Plugins"});
    CHECK(f.library->lastScan.unblock == std::vector<std::string>{"C:\\VST3\\Broken.vst3"});
    CHECK(r.data["scanned"] == 2);
    CHECK(r.data["blocked"].size() == 1);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("blocklist") != std::string::npos);
    // Without plugin hosting: refused.
    CommandHarness bare(Timeline("A1 |A" + std::string(9, 'a') + "|"), command::EnginePolicy{}, ai::edit::editingCommands());
    CHECK(bare.errorOf("plugins.list") == "InvalidRequest");
    CHECK(bare.errorOf("plugins.scan") == "InvalidRequest");
}

TEST_CASE("Plugin effects go on tracks and clips through audio.addEffect and clip.addEffect, keyframed like any effect",
          "[audio][edit][plugins]") {
    Fixture f;
    f.h.ok("audio.addEffect", {{"target", "A1"}, {"plugin", kGainId}, {"params", {{"p1", -6.0}}}});
    const ProjectSnapshot added = f.h.state();
    REQUIRE(f.h.sequence(added).audioTracks[0]->effects.size() == 1);
    const EffectInstance e = f.h.sequence(added).audioTracks[0]->effects[0];
    CHECK(e.effect == kGainId);
    REQUIRE(e.find("p1") != nullptr);
    CHECK(e.find("p1")->constant() == -6.0);
    // Its parameters are validated and keyframed through its descriptor.
    CHECK(f.h.errorOf("audio.addEffect", {{"target", "A1"}, {"plugin", kGainId}, {"params", {{"p1", 40.0}}}}) != "");
    f.h.ok("audio.addKeyframe", {{"target", "A1"}, {"effect", e.id.value}, {"param", "p1"}, {"time", 0}, {"value", -12.0}});
    CHECK(f.h.sequence(f.h.state()).audioTracks[0]->effects[0].find("p1")->isCurve());
    CHECK(f.h.errorOf("audio.addKeyframe", {{"target", "A1"}, {"effect", e.id.value}, {"param", "p2"}, {"time", 0}, {"value", 1.0}}) != "");
    // On a clip.
    f.h.ok("clip.addEffect", {{"clips", Json::array({f.h.clip("A1@0").value})}, {"plugin", kGainId}});
    CHECK(f.h.sequence(f.h.state()).audioTracks[0]->clips[0].effects().size() == 1);
    // A plugin this computer does not have, both names, or neither: refused.
    CHECK(f.h.errorOf("audio.addEffect", {{"target", "A1"}, {"plugin", "plugin.vst3.FFFF"}}) == "NotFound");
    CHECK(f.h.errorOf("clip.addEffect", {{"clips", Json::array({f.h.clip("A1@0").value})}, {"plugin", "plugin.clap.nothing"}}) == "NotFound");
    CHECK(f.h.errorOf("audio.addEffect", {{"target", "A1"}, {"plugin", kGainId}, {"effect", "wabcut.audio.gain"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.addEffect", {{"target", "A1"}}) == "InvalidParams");
    // Built-in effects still go in by "effect".
    f.h.ok("audio.addEffect", {{"target", "A1"}, {"effect", "wabcut.audio.gain"}});
}
