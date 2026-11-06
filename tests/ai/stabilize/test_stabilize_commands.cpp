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
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MasterClipEffects.h>
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

// Fix round 3 (NEW-01): a Warp Stabilizer in the media's Master Clip Effects is analysed on the media, over the frames
// every clip of it shows, and keyed on the media item (one undo step), so each instance plays stabilized.
TEST_CASE("Analyze Stabilization keys a Master Clip Effect for every clip of the media", "[ai][stabilize][commands]") {
    // Two clips of A: source frames 0-9 and 40-51 (a source In's digits take frames of the picture too).
    StabilizeHarness s("V1 |Aaaaaaaaaa..........A40aaaaaaaaa|");
    const MediaId media = s.h.tl.media('A');
    const ClipId first = s.h.clip("V1@0");
    const ClipId second = s.h.clip("V1@20");
    const SequenceId seq = s.h.tl.seq;
    const auto added = s.h.session.apply("Add Master Clip Effect", [&](const ProjectState& state) {
        return edit::addEffect(state, seq, edit::EffectTarget::ofMedia(media), std::string(kWarpStabilizerEffect),
                               {EffectParam{"framing", ParamValue(0.0)}, EffectParam{"method", ParamValue(1.0)}});
    });
    REQUIRE(added);
    REQUIRE(added->createdEffects.size() == 1);
    const EffectInstanceId id = added->createdEffects.front();
    const auto master = [&]() -> const EffectInstance* {
        for (const EffectInstance& e : s.h.state()->findMedia(media)->masterEffects) {
            if (e.id == id) {
                return &e;
            }
        }
        return nullptr;
    };

    const auto result = s.h.ok("effect.analyzeStabilization", {{"clip", first.value}, {"effect", id.value}});
    CHECK(result.data["frames"] == 52); // frames 0 to 51: both clips' frames, in one contiguous analysis
    CHECK(result.createdIds == std::vector<std::uint64_t>{id.value});
    // Keyed on the media item; neither clip got an effect of its own.
    const EffectInstance* warp = master();
    REQUIRE(warp);
    REQUIRE(warp->findData("status"));
    CHECK(*warp->findData("status")->text() == "analyzed");
    REQUIRE(warp->find("transform.x"));
    CHECK(warp->find("transform.x")->isCurve());
    CHECK(s.clip("V1@0").effects().empty());
    CHECK(s.clip("V1@20").effects().empty());
    // The second clip (source frames 40-51) plays it stabilized too.
    std::vector<double> shot, steady;
    for (std::int64_t k = 40; k < 52; ++k) {
        shot.push_back(jitterX(k) * kToSource);
        steady.push_back(jitterX(k) * kToSource + at(*warp, "transform.x", k));
    }
    INFO("x spread " << spread(shot) << " -> " << spread(steady));
    CHECK(spread(steady) < 0.25 * spread(shot));
    const EffectStack played = playedEffects(*s.h.state(), s.clip("V1@20"), EffectKind::Video);
    REQUIRE(played.size() == 1);
    CHECK(played.front().id == id);
    // A Rolling Shutter Repair of the second clip's own: its own frames only.
    const std::size_t analysed = s.h.session.undoCount();
    CHECK(s.h.ok("effect.analyzeRollingShutter", {{"clip", second.value}}).data["frames"] == 12);
    s.h.session.undo();
    // Through the second clip: the same frames, the same keys (nothing changes, so no undo step).
    const ProjectSnapshot before = s.h.state();
    CHECK(s.h.ok("effect.analyzeStabilization", {{"clip", second.value}, {"effect", id.value}}).data["frames"] == 52);
    CHECK(*s.h.state()->findMedia(media) == *before->findMedia(media));
    CHECK(s.h.session.undoCount() == analysed);
    // One undo step takes the keys back; the instance stays on the media item.
    s.h.session.undo();
    warp = master();
    REQUIRE(warp);
    CHECK(warp->find("transform.x") == nullptr);
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

// ---- 0.7 ---------------------------------------------------------------------------------------------------------------

namespace {

/// The shaky content's x position (source pixels) on each of the clip's frames over its first `frames` timeline frames, and
/// the same plus the Warp Stabilizer's compensation read at that frame's curve time: what the viewer sees before and after.
struct Seen {
    std::vector<double> shot;
    std::vector<double> steady;
};

Seen seenOver(const Clip& clip, const EffectInstance& warp, std::int64_t frames) {
    Seen seen;
    for (std::int64_t j = 0; j < frames; ++j) {
        const Ticks at = clip.start() + framesToTicks(j, kRate25);
        const std::int64_t k = ticksToFrames(clip.sourceTimeAt(at), kRate25); // the source frame it shows
        // The renderer reads a clip that shows whole frames at the frame's own time (Evaluate.cpp, stabilizationOf: Frame
        // Sampling holds each source frame's compensation); a remapped clip is keyed per frame shown.
        Ticks curve = clip.curveTimeAt(at);
        curve = clip.isRemapped() ? curve : snapToFrame(curve, kRate25);
        seen.shot.push_back(jitterX(k) * kToSource);
        seen.steady.push_back(jitterX(k) * kToSource + effectParameterAt(warp, "transform.x", curve));
    }
    return seen;
}

} // namespace

// Card 107: a clip at another speed (here 200 %) is analysed over the frames it shows; its keys are at the media's times,
// which are its curve times, so each timeline frame finds the compensation of the source frame it shows.
TEST_CASE("Analyze Stabilization works on a clip at another speed (card 107)", "[ai][stabilize][commands][speed]") {
    StabilizeHarness s(kClip30);
    const ClipId clip = s.h.clip("V1@0");
    s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect},
                              {"params", {{"framing", 0}, {"method", 0}}}});
    s.h.ok("clip.speedDuration", {{"clips", Json::array({clip.value})}, {"speed", 200}});
    REQUIRE(s.clip("V1@0").speed == Speed{2, 1});
    REQUIRE(s.clip("V1@0").duration() == framesToTicks(15, kRate25));
    const auto result = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    CHECK(result.data["frames"] == 30); // every source frame the clip shows
    CHECK(s.pictures->frames == 30);
    const EffectInstance* warp = s.effect("V1@0", kWarpStabilizerEffect);
    REQUIRE(warp);
    REQUIRE(warp->find("transform.x"));
    CHECK(warp->find("transform.x")->isCurve());
    const Seen seen = seenOver(s.clip("V1@0"), *warp, 15);
    INFO("x spread " << spread(seen.shot) << " -> " << spread(seen.steady));
    CHECK(spread(seen.steady) < 0.3 * spread(seen.shot));
    // Slowed to 50 % (the same 30 source frames over 60 timeline frames): each frame is shown twice, the keys stay on the
    // frames.
    s.h.ok("clip.speedDuration", {{"clips", Json::array({clip.value})}, {"speed", 50}});
    REQUIRE(s.clip("V1@0").duration() == framesToTicks(60, kRate25));
    const auto again = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    CHECK(again.data["frames"] == 30);
    warp = s.effect("V1@0", kWarpStabilizerEffect);
    const Seen slow = seenOver(s.clip("V1@0"), *warp, 60);
    CHECK(spread(slow.steady) < 0.3 * spread(slow.shot));
    // A clip that plays backwards at another speed is keyed the same way (its curve time is the media time).
    s.h.ok("clip.speedDuration", {{"clips", Json::array({clip.value})}, {"reverse", true}});
    REQUIRE(s.clip("V1@0").reversed);
    s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    warp = s.effect("V1@0", kWarpStabilizerEffect);
    const Seen backwards = seenOver(s.clip("V1@0"), *warp, 60);
    CHECK(spread(backwards.steady) < 0.3 * spread(backwards.shot));
}

// Card 107: a time-remapped clip (a slow first segment here) used to be refused. Its frames are analysed once and the
// compensation is keyed per frame the clip shows, in the clip's curve time (which runs with the timeline).
TEST_CASE("Analyze Stabilization works on a time-remapped clip (card 107)", "[ai][stabilize][commands][speed][remap]") {
    StabilizeHarness s(kClip30);
    const ClipId clip = s.h.clip("V1@0");
    s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect},
                              {"params", {{"framing", 0}, {"method", 0}}}});
    s.h.ok("timeRemap.enable", {{"clip", clip.value}});
    s.h.ok("timeRemap.addKey", {{"clip", clip.value}, {"time", "00:00:00:10"}});
    s.h.ok("timeRemap.setSpeed", {{"clip", clip.value}, {"time", "00:00:00:04"}, {"speed", 50}, {"extendClip", true}});
    REQUIRE(s.clip("V1@0").isRemapped());
    REQUIRE(s.clip("V1@0").duration() == framesToTicks(40, kRate25)); // 10 frames at 50 % then 20 at 100 %
    const auto result = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    CHECK(result.data["frames"] == 30);
    CHECK(result.data["lostFrames"] == 0);
    CHECK(s.pictures->frames == 30); // each source frame the remap shows is read once
    const EffectInstance* warp = s.effect("V1@0", kWarpStabilizerEffect);
    REQUIRE(warp);
    REQUIRE(warp->find("transform.x"));
    CHECK(warp->find("transform.x")->isCurve());
    CHECK(*warp->findData("status")->text() == "analyzed");
    // Frame Sampling shows whole frames: each output frame is compensated for the frame it shows, including the repeats of
    // the slow segment (output frames 0 and 1 both show source frame 0 but are keyed at their own curve times).
    const Clip& remapped = s.clip("V1@0");
    const Seen seen = seenOver(remapped, *warp, 40);
    INFO("x spread " << spread(seen.shot) << " -> " << spread(seen.steady));
    CHECK(spread(seen.steady) < 0.3 * spread(seen.shot));
    // The keys sit at curve times inside the clip (sourceIn + its timeline length), not at the media's frame times.
    for (const Keyframe& key : warp->find("transform.x")->curve()->keys) {
        CHECK(key.time >= remapped.sourceIn);
        CHECK(key.time < remapped.sourceIn + remapped.duration());
    }
    // A hold (a freeze inside the remap) is keyed too.
    s.h.ok("timeRemap.addHold", {{"clip", clip.value}, {"time", "00:00:01:00"}, {"duration", "00:00:00:03"}});
    s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", warp->id.value}});
    warp = s.effect("V1@0", kWarpStabilizerEffect);
    const Clip& held = s.clip("V1@0");
    const Seen heldSeen = seenOver(held, *warp, ticksToFrames(held.duration(), kRate25)); // the hold may change the length
    CHECK(spread(heldSeen.steady) < 0.3 * spread(heldSeen.shot));
    CHECK(s.h.errorOf("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", 999}}) == "NotFound");
}

// Card 128: Analyze All.
TEST_CASE("Analyze All analyses each stabilizer once, redoes the stale ones and skips what cannot be analysed (card 128)",
          "[ai][stabilize][commands][analyzeAll]") {
    StabilizeHarness s("V1 |Aaaaaaaaaa..A40aaaaaaaaa|");
    const ClipId first = s.h.clip("V1@0");
    const ClipId second = s.h.clip("V1@12");
    const Json position = {{"framing", 0}, {"method", 0}};
    const Json defaults = Json(Json::value_t::object);
    const auto add = [&](ClipId clip, std::string_view effect, const Json& params) {
        return s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", effect}, {"params", params}})
            .createdIds.at(0);
    };
    const std::uint64_t warpA = add(first, kWarpStabilizerEffect, position);
    const std::uint64_t warpB = add(second, kWarpStabilizerEffect, position);
    const std::size_t undo = s.h.session.undoCount();

    // Nothing is analysed yet: both are, over every frame of both clips (10 and 12: a source In's digits take frames of the
    // picture), with one progress count and one undo step.
    const auto all = s.h.ok("effect.analyzeAll");
    CHECK(all.data["analyzed"] == 2);
    CHECK(all.data["alreadyAnalyzed"] == 0);
    CHECK(all.data["frames"] == 22);
    CHECK(all.data["skipped"].empty());
    CHECK(s.pictures->frames == 22);
    REQUIRE_FALSE(s.pictures->reports.empty());
    CHECK(s.pictures->reports.back().frame == 22); // one bar over both effects
    CHECK(s.pictures->reports.back().total == 22);
    CHECK(s.h.session.undoCount() == undo + 1);
    for (const char* ref : {"V1@0", "V1@12"}) {
        const EffectInstance* warp = s.effect(ref, kWarpStabilizerEffect);
        REQUIRE(warp);
        REQUIRE(warp->findData("status"));
        CHECK(*warp->findData("status")->text() == "analyzed");
        CHECK(warp->find("transform.x")->isCurve());
    }

    // Run again: everything is current, so nothing is read and nothing changes.
    const int read = s.pictures->frames;
    const auto again = s.h.ok("effect.analyzeAll");
    CHECK(again.data["analyzed"] == 0);
    CHECK(again.data["alreadyAnalyzed"] == 2);
    CHECK(s.pictures->frames == read);

    // A setting changed after the analysis (Smoothness): that one is out of date; the other stays.
    s.h.ok("clip.setEffect", {{"clip", first.value}, {"effect", warpA}, {"params", {{"smoothness", 300}}}});
    const auto stale = s.h.ok("effect.analyzeAll");
    CHECK(stale.data["analyzed"] == 1);
    CHECK(stale.data["alreadyAnalyzed"] == 1);
    CHECK(stale.data["effects"][0]["effect"] == warpA);
    CHECK(s.pictures->frames == read + 10);

    // The clip trimmed longer after the analysis: its keys stop short of the new frames, so it is analysed again.
    s.h.ok("clip.trim", {{"clip", second.value}, {"edge", "tail"}, {"delta", "00:00:00:02"}});
    const auto trimmed = s.h.ok("effect.analyzeAll");
    CHECK(trimmed.data["analyzed"] == 1);
    CHECK(trimmed.data["effects"][0]["effect"] == warpB);

    // reanalyze redoes every one; kind limits it; an effect that is switched off is listed, not analysed.
    CHECK(s.h.ok("effect.analyzeAll", {{"reanalyze", true}}).data["analyzed"] == 2);
    const std::uint64_t shutter = add(first, kRollingShutterRepairEffect, defaults);
    const auto onlyShutter = s.h.ok("effect.analyzeAll", {{"kind", "shutter"}});
    CHECK(onlyShutter.data["analyzed"] == 1);
    CHECK(onlyShutter.data["effects"][0]["effect"] == shutter);
    CHECK(onlyShutter.data["effects"][0]["kind"] == "shutter");
    s.h.ok("clip.setEffect", {{"clip", second.value}, {"effect", warpB}, {"enabled", false}});
    const auto off = s.h.ok("effect.analyzeAll", {{"reanalyze", true}});
    CHECK(off.data["analyzed"] == 2); // warpA and the shutter repair
    REQUIRE(off.data["skipped"].size() == 1);
    CHECK(off.data["skipped"][0]["effect"] == warpB);
    CHECK_FALSE(off.warnings.empty());
    // Without the media's frames it is refused like the single analysis.
    StabilizeHarness without("V1 |Aaaaaaaaaa|", false);
    without.h.ok("clip.addEffect", {{"clips", Json::array({without.h.clip("V1@0").value})}, {"effect", kWarpStabilizerEffect}});
    CHECK(without.h.errorOf("effect.analyzeAll") == "InvalidRequest");
}

// Card 128: the sequences nested in the sequence are included (their clips keep their own effects).
TEST_CASE("Analyze All reaches a stabilizer inside a nested sequence (card 128)", "[ai][stabilize][commands][analyzeAll]") {
    StabilizeHarness s("V1 |Aaaaaaaaaa|");
    const ClipId clip = s.h.clip("V1@0");
    const std::uint64_t own = s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect},
                                                         {"params", {{"framing", 0}, {"method", 0}}}})
                                  .createdIds.at(0);
    s.h.ok("sequence.nest", {{"clips", Json::array({clip.value})}, {"name", "Inner"}});
    const std::int64_t outer = s.h.tl.seq.value; // two sequences now: the command names which
    const auto result = s.h.ok("effect.analyzeAll", {{"sequence", outer}});
    CHECK(result.data["analyzed"] == 1);
    CHECK(result.data["effects"][0]["effect"] == own);
    // Without the nested sequences the outer one has nothing of its own to analyse.
    const auto outerOnly = s.h.ok("effect.analyzeAll", {{"sequence", outer}, {"nested", false}, {"reanalyze", true}});
    CHECK(outerOnly.data["analyzed"] == 0);
}

// Card 129: Smoothness can be keyframed. Frames before the key that raises it are left as they were shot (0 %), the frames
// after it are steadied, moving a key makes the analysis out of date, and re-smoothing reads no frame again.
TEST_CASE("Analyze Stabilization follows a keyed Smoothness (card 129)", "[ai][stabilize][commands][keyed]") {
    StabilizeHarness s(kClip30, true, true);
    const ClipId clip = s.h.clip("V1@0");
    const std::uint64_t warp =
        s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect},
                                  {"params", {{"framing", 0}, {"method", 0}}}})
            .createdIds.at(0);
    // Smoothness 0 % until frame 15 (a hold), then 100 % (one second).
    const auto key = [&](std::int64_t frame, double value, const char* interpolation) {
        s.h.ok("clip.addKeyframe", {{"clip", clip.value}, {"effect", warp}, {"param", "smoothness"},
                                     {"time", framesToTicks(frame, kRate25).value}, {"value", value},
                                     {"interpolation", interpolation}});
    };
    key(0, 0.0, "hold");
    key(15, 100.0, "linear");
    REQUIRE(s.effect("V1@0", kWarpStabilizerEffect)->find("smoothness")->isCurve());

    const auto first = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", warp}});
    CHECK(first.data["cached"] == false);
    const EffectInstance* effect = s.effect("V1@0", kWarpStabilizerEffect);
    REQUIRE(effect);
    CHECK(effect->find("smoothness")->isCurve()); // the analysis leaves the setting (and its keys) alone
    // Where Smoothness is 0 the picture stays exactly as it was shot: the compensation is nothing.
    for (std::int64_t k = 0; k < 15; ++k) {
        INFO("frame " << k);
        CHECK(at(*effect, "transform.x", k) == Approx(0.0).margin(0.06));
        CHECK(at(*effect, "transform.y", k) == Approx(0.0).margin(0.06));
    }
    // Where it is 100 % the shake is gone: far less of it is left than as shot.
    std::vector<double> shot, steady;
    for (std::int64_t k = 15; k < 30; ++k) {
        shot.push_back(jitterX(k) * kToSource);
        steady.push_back(jitterX(k) * kToSource + at(*effect, "transform.x", k));
    }
    INFO("shake " << spread(shot) << " -> " << spread(steady));
    CHECK(spread(steady) < 0.4 * spread(shot));

    // Nothing changed since the analysis: it is up to date, so Analyze All leaves it alone.
    const std::size_t undo = s.h.session.undoCount();
    CHECK(s.h.ok("effect.analyzeAll").data["alreadyAnalyzed"] == 1); // up to date: nothing to do
    CHECK(s.h.session.undoCount() == undo);

    // Moving a key makes the analysis out of date; the cached motion re-smooths it without reading a frame.
    s.h.ok("clip.setKeyframe", {{"clip", clip.value}, {"effect", warp}, {"param", "smoothness"}, {"index", 0}, {"value", 100.0}});
    const int read = s.pictures->frames;
    const auto stale = s.h.ok("effect.analyzeAll");
    CHECK(stale.data["analyzed"] == 1);
    CHECK(stale.data["alreadyAnalyzed"] == 0);
    CHECK(s.pictures->frames == read); // the motion was cached
    effect = s.effect("V1@0", kWarpStabilizerEffect);
    std::vector<double> shotFirst, steadyFirst;
    for (std::int64_t k = 0; k < 15; ++k) {
        shotFirst.push_back(jitterX(k) * kToSource);
        steadyFirst.push_back(jitterX(k) * kToSource + at(*effect, "transform.x", k));
    }
    CHECK(spread(steadyFirst) < 0.4 * spread(shotFirst)); // now steadied there too
    // One undo step (the Analyze) restores the earlier keys.
    s.h.session.undo();
    effect = s.effect("V1@0", kWarpStabilizerEffect);
    CHECK(at(*effect, "transform.x", 3) == Approx(0.0).margin(0.06));
}

TEST_CASE("Analyze Stabilization reads a keyed Smoothness through a time remap (card 129)", "[ai][stabilize][commands][keyed][remap]") {
    StabilizeHarness s(kClip30);
    const ClipId clip = s.h.clip("V1@0");
    const std::uint64_t warp =
        s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect},
                                  {"params", {{"framing", 0}, {"method", 0}}}})
            .createdIds.at(0);
    s.h.ok("timeRemap.enable", {{"clip", clip.value}});
    s.h.ok("timeRemap.addKey", {{"clip", clip.value}, {"time", "00:00:00:10"}});
    s.h.ok("timeRemap.setSpeed", {{"clip", clip.value}, {"time", "00:00:00:04"}, {"speed", 50}, {"extendClip", true}});
    REQUIRE(s.clip("V1@0").isRemapped());
    // Smoothness 0 % for the first 20 output frames, then 100 %: output frame 20 shows source frame 15 (10 frames at 50 %
    // show 5 source frames, then 100 % speed).
    for (const auto& [frame, value] : {std::pair<std::int64_t, double>{0, 0.0}, {20, 100.0}}) {
        s.h.ok("clip.addKeyframe", {{"clip", clip.value}, {"effect", warp}, {"param", "smoothness"},
                                     {"time", framesToTicks(frame, kRate25).value}, {"value", value}, {"interpolation", "hold"}});
    }
    const auto result = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", warp}});
    CHECK(result.data["frames"] == 30);
    const EffectInstance* effect = s.effect("V1@0", kWarpStabilizerEffect);
    REQUIRE(effect);
    const Seen seen = seenOver(s.clip("V1@0"), *effect, 40);
    REQUIRE(seen.shot.size() == 40);
    const std::vector<double> shotEarly(seen.shot.begin(), seen.shot.begin() + 18);
    const std::vector<double> steadyEarly(seen.steady.begin(), seen.steady.begin() + 18);
    const std::vector<double> shotLate(seen.shot.begin() + 22, seen.shot.end());
    const std::vector<double> steadyLate(seen.steady.begin() + 22, seen.steady.end());
    INFO("early " << spread(shotEarly) << " -> " << spread(steadyEarly) << ", late " << spread(shotLate) << " -> " << spread(steadyLate));
    CHECK(spread(steadyEarly) > 0.8 * spread(shotEarly)); // left as shot
    CHECK(spread(steadyLate) < 0.5 * spread(shotLate));   // steadied
}
