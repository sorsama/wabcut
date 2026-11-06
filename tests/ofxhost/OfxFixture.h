#pragma once

// OpenFX host test helpers (0.7): the test plug-ins built next to the tests (plugins/TestOfx.cpp), a catalog of them, a host
// and the OpenFX effects installed into the model and the playback layer for the test's lifetime, plus the pictures the
// tests process.

#include <wabcut/ofxhost/OfxCatalog.h>
#include <wabcut/ofxhost/OfxEffects.h>
#include <wabcut/ofxhost/OfxPluginHost.h>

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Effects.h>
#include <wabcut/playback/video/ExternalEffects.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::test {

inline std::filesystem::path ofxTestDir() { return std::filesystem::path(WABCUT_TEST_OFX_DIR); }
inline std::filesystem::path ofxHostPath() { return std::filesystem::path(WABCUT_OFXHOST_PATH); }

inline std::filesystem::path ofxOutput(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ofxhost";
    std::filesystem::create_directories(dir);
    return dir / name;
}

inline constexpr const char* kGain = "plugin.ofx.org.wabcut.test.gain";
inline constexpr const char* kInvert8 = "plugin.ofx.org.wabcut.test.invert8";
inline constexpr const char* kInvert16 = "plugin.ofx.org.wabcut.test.invert16";
inline constexpr const char* kFail = "plugin.ofx.org.wabcut.test.fail";
inline constexpr const char* kGeneral = "plugin.ofx.org.wabcut.test.general";
inline constexpr const char* kCrash = "plugin.ofx.org.wabcut.test.crash";
inline constexpr const char* kHang = "plugin.ofx.org.wabcut.test.hang";

/// The tests' host options: a generous picture timeout (loaded computers); the hang test makes a host of its own.
inline ofxhost::OfxHostOptions testOfxHostOptions() {
    ofxhost::OfxHostOptions options;
    options.helper = ofxHostPath();
    options.logFile = ofxOutput("helpers.log");
    options.previewRenderTimeout = std::chrono::milliseconds(20000);
    return options;
}

/// The test plug-ins, scanned, hosted and installed as the model's and the playback layer's OpenFX effects (until destroyed).
struct TestOfx {
    std::shared_ptr<ofxhost::OfxCatalog> catalog;
    std::shared_ptr<ofxhost::OfxPluginHost> host;
    std::shared_ptr<ofxhost::OfxEffects> effects;

    explicit TestOfx(ofxhost::OfxHostOptions hostOptions = testOfxHostOptions()) {
        static std::atomic<int> serial{0};
        ofxhost::OfxCatalogOptions options;
        options.helper = ofxHostPath();
        options.folders = {ofxTestDir() / "main", ofxTestDir() / "crash", ofxTestDir() / "hang"};
        options.cacheFile = ofxOutput("catalog-" + std::to_string(serial.fetch_add(1)) + ".json");
        std::filesystem::remove(options.cacheFile);
        catalog = std::make_shared<ofxhost::OfxCatalog>(options);
        const ofxhost::OfxScanReport report = catalog->scan();
        REQUIRE(report.files == 3);
        REQUIRE(report.blocked.empty());
        host = ofxhost::OfxPluginHost::create(hostOptions);
        effects = ofxhost::OfxEffects::create(host, catalog);
        setSecondaryEffectDescriptorProvider(catalog->descriptors());
        playback::setExternalEffectProcessor(effects);
    }
    ~TestOfx() {
        playback::setExternalEffectProcessor(nullptr);
        setSecondaryEffectDescriptorProvider(nullptr);
    }

    ofxhost::OfxPluginInfo plugin(const char* effectId) const {
        auto p = catalog->find(effectId);
        REQUIRE(p);
        return *p;
    }
};

/// A float RGBA picture (premultiplied, top-down): red grows to the right, green downwards, blue constant, opaque.
inline std::vector<float> gradientPicture(int w, int h) {
    std::vector<float> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float* p = out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
            p[0] = 0.1f + 0.8f * static_cast<float>(x) / static_cast<float>(w - 1);
            p[1] = 0.1f + 0.8f * static_cast<float>(y) / static_cast<float>(h - 1);
            p[2] = 0.3f;
            p[3] = 1.0f;
        }
    }
    return out;
}

/// A float picture of one colour.
inline std::vector<float> flatPicture(int w, int h, float r, float g, float b, float a = 1.0f) {
    std::vector<float> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i] = r;
        out[i + 1] = g;
        out[i + 2] = b;
        out[i + 3] = a;
    }
    return out;
}

inline const float* pixelAt(const std::vector<float>& picture, int w, int x, int y) {
    return picture.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
}

/// Parameter values for the gain test plug-in that change nothing (the centre mark is off the picture, the label empty).
inline nlohmann::json gainParams(double gain = 1.0) {
    return nlohmann::json{{"gain", gain},   {"offset", 0},        {"mode", 0},   {"skip", false},
                          {"tint", {1.0, 1.0, 1.0}}, {"center", {5.0, 5.0}}, {"bottomMark", false}, {"label", ""}};
}

inline ofxhost::OfxRenderRequest renderRequest(int w, int h, nlohmann::json params, bool hasAlpha = true) {
    ofxhost::OfxRenderRequest r;
    r.width = w;
    r.height = h;
    r.hasAlpha = hasAlpha;
    r.params = std::make_shared<const nlohmann::json>(std::move(params));
    return r;
}

/// An 8-bit YUV 4:2:0 frame (BT.709, limited range) of one colour, or with the top half at `y` and the bottom half at
/// `yBottom` (when it is not 0): what a decoder hands the playback layer.
inline media::FrameHandle yuvFrame(int w, int h, std::uint8_t y, std::uint8_t u, std::uint8_t v, std::uint64_t fingerprint = 7,
                                   std::int64_t pts = 0, int yBottom = -1) {
    const std::size_t luma = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const std::size_t chroma = static_cast<std::size_t>(w / 2) * static_cast<std::size_t>(h / 2);
    auto storage = std::make_shared<std::vector<std::uint8_t>>(luma + 2 * chroma);
    for (int row = 0; row < h; ++row) {
        const std::uint8_t value = yBottom >= 0 && row >= h / 2 ? static_cast<std::uint8_t>(yBottom) : y;
        std::fill_n(storage->data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(w), w, value);
    }
    std::fill_n(storage->data() + luma, chroma, u);
    std::fill_n(storage->data() + luma + chroma, chroma, v);
    media::FrameHandle f;
    f.pts = Ticks{pts};
    f.duration = Ticks{1000};
    f.streamPts = pts;
    f.width = w;
    f.height = h;
    f.format = media::PixelFormat::YUV420P;
    f.color.matrix = ColorMatrix::BT709;
    f.color.range = ColorRange::Limited;
    f.color.primaries = ColorPrimaries::BT709;
    f.color.transfer = TransferFunction::BT709;
    f.color.chromaLocation = ChromaLocation::Left;
    f.source.fingerprint = fingerprint;
    media::CpuPlanes cpu;
    cpu.planes[0] = media::Plane{storage->data(), w, w, h};
    cpu.planes[1] = media::Plane{storage->data() + luma, w / 2, w / 2, h / 2};
    cpu.planes[2] = media::Plane{storage->data() + luma + chroma, w / 2, w / 2, h / 2};
    cpu.owner = storage;
    f.payload = std::move(cpu);
    return f;
}

/// The RGBA8 pixel (x, y) of a processed picture.
inline const std::uint8_t* rgbaAt(const render::FrameView& view, int x, int y) {
    return view.planes[0].data + static_cast<std::ptrdiff_t>(y) * view.planes[0].stride + static_cast<std::ptrdiff_t>(x) * 4;
}

/// An external effect as the evaluator makes it: the gain test plug-in (or another), with `params` by key.
inline playback::ExternalEffect externalEffect(const char* effectId, std::uint64_t id,
                                               std::vector<std::pair<std::string, double>> params = {}, double timeFrames = 0.0) {
    playback::ExternalEffect e;
    e.id = EffectInstanceId{id};
    e.effect = effectId;
    e.params = std::move(params);
    e.timeFrames = timeFrames;
    return e;
}

/// The gain test plug-in's values that change nothing, by effect key, then `changed`.
inline std::vector<std::pair<std::string, double>> gainKeys(std::vector<std::pair<std::string, double>> changed = {}) {
    std::vector<std::pair<std::string, double>> keys{{"gain", 1.0},     {"offset", 0.0},   {"mode", 0.0},         {"skip", 0.0},
                                                     {"tint.r", 1.0},   {"tint.g", 1.0},   {"tint.b", 1.0},       {"center.x", 5.0},
                                                     {"center.y", 5.0}, {"bottomMark", 0.0}};
    for (auto& [key, value] : changed) {
        for (auto& entry : keys) {
            if (entry.first == key) {
                entry.second = value;
            }
        }
    }
    return keys;
}

} // namespace wabcut::test
