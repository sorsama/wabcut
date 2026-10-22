// P.2: effect.analyzeStabilization and effect.analyzeRollingShutter through the CommandEngine (editingCommands(), so
// the registration is tested too), with fake pictures: a texture shaken by a known jitter per frame (and an optional
// pan). Warp Stabilizer is added and keyed in one undo step, the keys cancel the jitter, the settings change the result
// (Position keys no rotation, Perspective keys corner pins, No Motion locks), the analysis is cached per media
// fingerprint (a second run reads no frame), progress is reported, and bad targets are refused. Rolling Shutter Repair
// keys the picture's motion per frame.

#include "CommandHarness.h"
#include "../../masks/SyntheticMotion.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/stabilize/StabilizeCommands.h>
#include <wabcut/masks/MaskCommands.h>
#include <wabcut/model/MotionOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;
namespace fs = std::filesystem;

namespace {

constexpr int kWorkingWidth = 320; // the fake's pictures (the fixture's media is 1920 x 1080: 6 source px per working px)
constexpr double kToSource = 1920.0 / kWorkingWidth;

/// The jitter of frame k, working pixels (deterministic, zero-mean-ish, up to about 3 px).
double jitterX(std::int64_t k) { return 2.0 * std::sin(1.7 * static_cast<double>(k)) + std::sin(4.1 * static_cast<double>(k) + 1.0); }
double jitterY(std::int64_t k) { return 1.5 * std::sin(2.3 * static_cast<double>(k) + 0.5) + 0.7 * std::sin(5.3 * static_cast<double>(k)); }

class ShakyPictures final : public masks::TrackingServices {
public:
    double panX = 0.0; ///< working pixels per frame
    int frames = 0;
    std::vector<masks::TrackProgress> reports;

    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                     int maxWidth) override {
        ++frames;
        const VideoStreamInfo& v = state.findMedia(media)->info.video.at(static_cast<std::size_t>(stream));
        if (time.value < 0 || time >= v.duration) {
            return std::unexpected(Error{ErrorCode::NotFound, "past the end"});
        }
        const std::int64_t k = ticksToFrames(time, v.frameRate);
        const int w = std::min(maxWidth, kWorkingWidth);
        const int h = w * v.height / v.width;
        const double dx = panX * static_cast<double>(k) + jitterX(k);
        const double dy = jitterY(k);
        masks::TrackingFrame f;
        f.luma = masks::test::render(m_texture, w, h, masks::Motion{1, 0, -dx, 0, 1, -dy});
        f.start = framesToTicks(k, v.frameRate);
        f.duration = framesToTicks(1, v.frameRate);
        return f;
    }
    void progress(const masks::TrackProgress& p) override { reports.push_back(p); }

private:
    masks::test::Texture m_texture{23};
};

/// The fixture with media A fingerprinted (so the analysis can be cached).
Timeline fingerprinted(std::string_view picture) {
    Timeline tl(picture);
    for (MediaPtr& m : tl.state.media) {
        if (m && m->id == tl.media('A')) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->locator.fingerprint.size = 123456789;
            copy->locator.fingerprint.sampleHash = "00000000deadbeef";
            m = std::move(copy);
        }
    }
    return tl;
}

struct StabilizeHarness {
    std::shared_ptr<ShakyPictures> pictures = std::make_shared<ShakyPictures>();
    fs::path cacheDir;
    CommandHarness h;

    explicit StabilizeHarness(std::string_view picture, bool services = true, bool cache = false)
        : cacheDir(cacheFolder()), h(fingerprinted(picture), policy(pictures, services, cache ? cacheDir : fs::path()),
                                     ai::edit::editingCommands()) {}

    static fs::path cacheFolder() {
        static int n = 0;
        const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "ai" / "stabilize" / ("cache" + std::to_string(n++));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        return dir;
    }
    static command::EnginePolicy policy(const std::shared_ptr<ShakyPictures>& pictures, bool services, const fs::path& cache) {
        command::EnginePolicy p;
        if (!services) {
            return p;
        }
        std::vector<std::shared_ptr<command::CommandServices>> parts{pictures};
        if (!cache.empty()) {
            parts.push_back(std::make_shared<ai::stabilize::StabilizationServices>(std::make_shared<ai::AnalysisCache>(cache)));
        }
        p.services = std::make_shared<command::ServiceBundle>(std::move(parts));
        return p;
    }
    const Clip& clip(std::string_view ref) { return h.state()->findSequence(h.tl.seq)->findClip(h.clip(ref))->clip(); }
    const EffectInstance* effect(std::string_view ref, std::string_view id) {
        for (const EffectInstance& e : clip(ref).effects()) {
            if (e.effect == id) {
                return &e;
            }
        }
        return nullptr;
    }
};

double at(const EffectInstance& e, std::string_view key, std::int64_t frame) {
    return effectParameterAt(e, key, framesToTicks(frame, kRate25));
}

/// Standard deviation of v.
double spread(const std::vector<double>& v) {
    double mean = 0.0;
    for (const double x : v) {
        mean += x;
    }
    mean /= static_cast<double>(v.size());
    double s = 0.0;
    for (const double x : v) {
        s += (x - mean) * (x - mean);
    }
    return std::sqrt(s / static_cast<double>(v.size()));
}

/// How much of the shake on one axis is left after `key` compensates it: the spread of (content position + key) over
/// the clip's 30 frames as a share of the content position's.
double leftOver(const EffectInstance& e, std::string_view key, double (*jitter)(std::int64_t)) {
    std::vector<double> shot, steady;
    for (std::int64_t k = 0; k < 30; ++k) {
        shot.push_back(jitter(k) * kToSource);
        steady.push_back(jitter(k) * kToSource + at(e, key, k));
    }
    return spread(steady) / spread(shot);
}

constexpr std::string_view kClip30 = "V1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|"; // 30 frames

} // namespace

TEST_CASE("Analyze Stabilization adds and keys a Warp Stabilizer in one undo step", "[ai][stabilize][commands]") {
    StabilizeHarness s(kClip30);
    const ClipId clip = s.h.clip("V1@0");
    // Position, Scale, Rotation and Stabilize Only, so the compensation is the motion's opposite with no auto-scale.
    const auto added = s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect},
                                                  {"params", {{"framing", 0}, {"method", 1}}}});
    REQUIRE(added.createdIds.size() == 1);
    const auto result = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    CHECK(result.data["frames"] == 30);
    CHECK(result.data["lostFrames"] == 0);
    CHECK(result.data["cached"] == false);
    CHECK(result.createdIds == std::vector<std::uint64_t>{added.createdIds[0]});
    CHECK(s.pictures->frames == 30);
    REQUIRE_FALSE(s.pictures->reports.empty());
    CHECK(s.pictures->reports.back().frame == 30);
    CHECK(s.pictures->reports.back().total == 30);
    const EffectInstance* warp = s.effect("V1@0", kWarpStabilizerEffect);
    REQUIRE(warp);
    REQUIRE(warp->find("transform.x"));
    CHECK(warp->find("transform.x")->isCurve());
    REQUIRE(warp->findData("status"));
    CHECK(*warp->findData("status")->text() == "analyzed");
    // The compensation cancels the jitter: the content's position plus the key is (nearly) still.
    std::vector<double> shotX, shotY, steadyX, steadyY;
    for (std::int64_t k = 0; k < 30; ++k) {
        shotX.push_back(jitterX(k) * kToSource);
        shotY.push_back(jitterY(k) * kToSource);
        steadyX.push_back(jitterX(k) * kToSource + at(*warp, "transform.x", k));
        steadyY.push_back(jitterY(k) * kToSource + at(*warp, "transform.y", k));
    }
    INFO("x spread " << spread(shotX) << " -> " << spread(steadyX) << ", y spread " << spread(shotY) << " -> " << spread(steadyY));
    CHECK(spread(steadyX) < 0.25 * spread(shotX));
    CHECK(spread(steadyY) < 0.25 * spread(shotY));
    CHECK(std::abs(at(*warp, "transform.rotation", 10)) < 0.2);
    CHECK(at(*warp, "transform.scale", 10) == Approx(1.0).margin(0.01));
    // Undo takes the keys back in one step; the instance the user added stays.
    s.h.session.undo();
    warp = s.effect("V1@0", kWarpStabilizerEffect);
    REQUIRE(warp);
    CHECK(warp->find("transform.x") == nullptr);
}

TEST_CASE("Analyze Stabilization adds the effect when the clip has none and settings change the result",
          "[ai][stabilize][commands]") {
    StabilizeHarness s(kClip30, true, true);
    const ClipId clip = s.h.clip("V1@0");
    // No instance yet: one is added at the top of the stack, with Premiere's defaults (Subspace Warp, here a corner pin
    // per frame; Stabilize, Crop, Auto-scale).
    s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", "wabcut.blur.gaussian"}});
    const auto first = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    REQUIRE(s.clip("V1@0").effects().size() == 2);
    CHECK(s.clip("V1@0").effects().front().effect == kWarpStabilizerEffect);
    const EffectInstanceId id{first.createdIds.at(0)};
    const EffectInstance* warp = s.effect("V1@0", kWarpStabilizerEffect);
    REQUIRE(warp);
    CHECK(warp->id == id);
    const double autoScale = at(*warp, "autoScale", 0);
    CHECK(autoScale > 1.0); // the jitter uncovers borders: the auto scale hides them
    CHECK(autoScale < 1.1);
    REQUIRE(warp->find("corner.tl.x"));
    CHECK(warp->find("corner.tl.x")->isCurve());
    CHECK(at(*warp, "transform.scale", 10) == 1.0);
    // One undo step removes the instance with its keys.
    s.h.session.undo();
    CHECK(s.clip("V1@0").effects().size() == 1);
    s.h.session.redo();
    // Position: no rotation or scale keyed. Its analysis (a translation per frame) is another cache entry: the frames
    // are read again.
    const int read = s.pictures->frames;
    s.h.ok("clip.setEffect", {{"clip", clip.value}, {"effect", id.value}, {"params", {{"method", 0}, {"framing", 0}}}});
    auto position = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", id.value}});
    CHECK(position.data["cached"] == false);
    CHECK(s.pictures->frames == read + 30);
    warp = s.effect("V1@0", kWarpStabilizerEffect);
    CHECK(at(*warp, "transform.rotation", 12) == 0.0);
    CHECK(at(*warp, "transform.scale", 12) == 1.0);
    CHECK(leftOver(*warp, "transform.x", jitterX) < 0.25);
    CHECK(leftOver(*warp, "transform.y", jitterY) < 0.25);
    // Smoothness changes re-smooth from the cached analysis: no frame is read.
    s.h.ok("clip.setEffect", {{"clip", clip.value}, {"effect", id.value}, {"params", {{"smoothness", 200}}}});
    position = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", id.value}});
    CHECK(position.data["cached"] == true);
    CHECK(s.pictures->frames == read + 30);
    // No Motion locks every frame to the first.
    s.h.ok("clip.setEffect", {{"clip", clip.value}, {"effect", id.value}, {"params", {{"result", 1}}}});
    s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", id.value}});
    warp = s.effect("V1@0", kWarpStabilizerEffect);
    for (const std::int64_t k : {5, 17, 29}) {
        INFO("frame " << k);
        CHECK(jitterX(k) * kToSource + at(*warp, "transform.x", k) == Approx(jitterX(0) * kToSource + at(*warp, "transform.x", 0)).margin(1.5));
    }
    // Perspective keys the corner pins and leaves the transform alone.
    s.h.ok("clip.setEffect", {{"clip", clip.value}, {"effect", id.value}, {"params", {{"method", 2}, {"result", 0}}}});
    s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", id.value}});
    warp = s.effect("V1@0", kWarpStabilizerEffect);
    CHECK(warp->find("corner.tl.x")->isCurve());
    CHECK(at(*warp, "transform.x", 12) == 0.0);
    CHECK(leftOver(*warp, "corner.tl.x", jitterX) < 0.25);
    CHECK(leftOver(*warp, "corner.br.y", jitterY) < 0.25);
    // Position, Scale, Rotation with Stabilize, Crop, Auto-scale: the auto scale is in the keyed scale.
    s.h.ok("clip.setEffect", {{"clip", clip.value}, {"effect", id.value}, {"params", {{"method", 1}, {"framing", 2}}}});
    s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", id.value}});
    warp = s.effect("V1@0", kWarpStabilizerEffect);
    const double scaled = at(*warp, "autoScale", 0);
    CHECK(scaled > 1.0);
    CHECK(at(*warp, "transform.scale", 10) == Approx(scaled).epsilon(0.01));
    CHECK(at(*warp, "corner.tl.x", 10) == 0.0);
}

TEST_CASE("Analyze Stabilization refuses what it cannot analyse", "[ai][stabilize][commands]") {
    StabilizeHarness without(kClip30, false);
    CHECK(without.h.errorOf("effect.analyzeStabilization", {{"clip", without.h.clip("V1@0").value}}) == "InvalidRequest");
    StabilizeHarness s("V1 |Aaaaaaaaaa|\nA1 |Aaaaaaaaaa|");
    CHECK(s.h.errorOf("effect.analyzeStabilization", {{"clip", s.h.clip("A1@0").value}}) == "InvalidParams");
    CHECK(s.h.errorOf("effect.analyzeStabilization", {{"clip", 9999}}) == "NotFound");
    CHECK(s.h.errorOf("effect.analyzeStabilization", {{"clip", s.h.clip("V1@0").value}, {"effect", 77}}) == "NotFound");
    // A dry run analyses and previews, but changes nothing.
    const auto dry = s.h.run(Json{{"command", "effect.analyzeStabilization"}, {"params", {{"clip", s.h.clip("V1@0").value}}},
                                  {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(s.clip("V1@0").effects().empty());
}

TEST_CASE("Analyze Rolling Shutter keys the picture's motion per frame", "[ai][stabilize][commands]") {
    StabilizeHarness s(kClip30);
    s.pictures->panX = 4.0; // a pan: 24 source px a frame, plus the jitter
    const ClipId clip = s.h.clip("V1@0");
    const auto result = s.h.ok("effect.analyzeRollingShutter", {{"clip", clip.value}});
    CHECK(result.data["frames"] == 30);
    const EffectInstance* shutter = s.effect("V1@0", kRollingShutterRepairEffect);
    REQUIRE(shutter);
    for (const std::int64_t k : {5, 15, 25}) {
        INFO("frame " << k);
        // Central difference of the content's position.
        const double vx = (4.0 * 2.0 + jitterX(k + 1) - jitterX(k - 1)) / 2.0 * kToSource;
        const double vy = (jitterY(k + 1) - jitterY(k - 1)) / 2.0 * kToSource;
        CHECK(at(*shutter, "motion.x", k) == Approx(vx).margin(2.0));
        CHECK(at(*shutter, "motion.y", k) == Approx(vy).margin(2.0));
    }
    s.h.session.undo();
    CHECK(s.effect("V1@0", kRollingShutterRepairEffect) == nullptr);
}
