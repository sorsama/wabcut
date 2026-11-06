// 0.7 (card #130): Warp Stabilizer's Method "Gyro (camera metadata)" through the CommandEngine: effect.analyzeStabilization
// reads the clip's gyro through GyroServices (a fake here), keys the compensation as the corner pin the picture-based
// Perspective method keys (one undo step, saved and loaded back), says what it found (camera, sync, field of view), measures
// the picture to sync the gyro with Auto-sync (fake pictures shaken like the gyro, the gyro's clock behind by 12 ms) and
// caches that measurement, goes stale when a Gyro setting changes, is part of Analyze All, and refuses with a reason when the
// file has no gyro.

#include "CommandHarness.h"
#include "GyroShake.h"
#include "../../masks/SyntheticMotion.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/stabilize/GyroMotion.h>
#include <wabcut/ai/stabilize/StabilizeCommands.h>
#include <wabcut/gyro/Gyro.h>
#include <wabcut/gyro/Rotation.h>
#include <wabcut/masks/MaskCommands.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <optional>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using namespace wabcut::test::gyro_shake;
using Catch::Approx;
using command::Json;
namespace fs = std::filesystem;

namespace {

constexpr int kSourceWidth = 1920; // the fixture's media
constexpr int kSourceHeight = 1080;
constexpr double kSourceFocal = 960.0; // a 90 degree field of view across 1920 pixels
constexpr int kWorkingWidth = 320;     // the fake pictures' width
constexpr double kWorkingFocal = 160.0;

class FakeGyro final : public gyro::GyroServices {
public:
    std::optional<gyro::GyroTrack> track;
    std::string failure = "this file carries no camera gyro data";
    int reads = 0;
    std::expected<gyro::GyroTrack, Error> read(const ProjectState&, MediaId, int) override {
        ++reads;
        if (!track) {
            return std::unexpected(Error{ErrorCode::NotFound, failure});
        }
        return *track;
    }
};

/// Pictures shaken the way the gyro says: the content moves by (-f * ay, f * ax) and rolls by -az at each frame.
class ShakenPictures final : public masks::TrackingServices {
public:
    int frames = 0;
    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time, int maxWidth) override {
        ++frames;
        const VideoStreamInfo& v = state.findMedia(media)->info.video.at(static_cast<std::size_t>(stream));
        if (time.value < 0 || time >= v.duration) {
            return std::unexpected(Error{ErrorCode::NotFound, "past the end"});
        }
        const std::int64_t k = ticksToFrames(time, v.frameRate);
        const int w = std::min(maxWidth, kWorkingWidth);
        const int h = w * v.height / v.width;
        const Vec3 a = shakeAngles(static_cast<double>(k) / 25.0);
        const double dx = -kWorkingFocal * a.y;
        const double dy = kWorkingFocal * a.x;
        const double rollDegrees = -a.z * 180.0 / std::numbers::pi;
        masks::TrackingFrame f;
        f.luma = masks::test::render(m_texture, w, h, masks::test::similarity(-rollDegrees, 1.0, -dx, -dy, (w - 1) / 2.0, (h - 1) / 2.0));
        f.start = framesToTicks(k, v.frameRate);
        f.duration = framesToTicks(1, v.frameRate);
        return f;
    }

private:
    masks::test::Texture m_texture{23};
};

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

struct GyroHarness {
    std::shared_ptr<FakeGyro> gyro = std::make_shared<FakeGyro>();
    std::shared_ptr<ShakenPictures> pictures = std::make_shared<ShakenPictures>();
    CommandHarness h;

    explicit GyroHarness(std::string_view picture, bool withPictures = false, bool withGyro = true, const fs::path& cache = {})
        : h(fingerprinted(picture), policy(gyro, pictures, withPictures, withGyro, cache), ai::edit::editingCommands()) {}

    static command::EnginePolicy policy(const std::shared_ptr<FakeGyro>& gyro, const std::shared_ptr<ShakenPictures>& pictures,
                                        bool withPictures, bool withGyro, const fs::path& cache) {
        std::vector<std::shared_ptr<command::CommandServices>> parts;
        if (withGyro) {
            parts.push_back(gyro);
        }
        if (withPictures) {
            parts.push_back(pictures);
        }
        if (!cache.empty()) {
            parts.push_back(std::make_shared<ai::stabilize::StabilizationServices>(std::make_shared<ai::AnalysisCache>(cache)));
        }
        command::EnginePolicy p;
        p.services = std::make_shared<command::ServiceBundle>(std::move(parts));
        return p;
    }
    static fs::path cacheFolder() {
        static int n = 0;
        const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "ai" / "stabilize" / ("gyro-cache" + std::to_string(n++));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        return dir;
    }
    const Clip& clip(std::string_view ref) { return h.state()->findSequence(h.tl.seq)->findClip(h.clip(ref))->clip(); }
    const EffectInstance* warp(std::string_view ref) {
        for (const EffectInstance& e : clip(ref).effects()) {
            if (e.effect == kWarpStabilizerEffect) {
                return &e;
            }
        }
        return nullptr;
    }
};

constexpr std::string_view kClip30 = "V1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|"; // 30 frames

double corner(const EffectInstance& e, std::string_view key, std::int64_t frame) {
    return effectParameterAt(e, key, framesToTicks(frame, kRate25));
}

/// Where the world ray (0.25, 0.1, 1) lands in frame `frame` of the 1920 x 1080 picture, for a camera at `angles`.
masks::Point2 landing(const Vec3& angles) {
    const auto r = gyro::toMatrix(orientationOf(angles));
    const double dx = 0.25, dy = 0.1, dz = 1.0;
    const double cx = r[0] * dx + r[3] * dy + r[6] * dz; // R^T * ray
    const double cy = r[1] * dx + r[4] * dy + r[7] * dz;
    const double cz = r[2] * dx + r[5] * dy + r[8] * dz;
    return {(kSourceWidth - 1) / 2.0 + kSourceFocal * cx / cz, (kSourceHeight - 1) / 2.0 + kSourceFocal * cy / cz};
}

/// The compensation the effect keys at `frame`, as a homography of the source picture.
ai::stabilize::Homography keyedHomography(const EffectInstance& e, std::int64_t frame) {
    const double w = kSourceWidth, h = kSourceHeight;
    const std::array<masks::Point2, 4> from{{{-0.5, -0.5}, {w - 0.5, -0.5}, {w - 0.5, h - 0.5}, {-0.5, h - 0.5}}};
    std::array<masks::Point2, 4> to = from;
    static constexpr std::array<std::string_view, 4> kNames{"tl", "tr", "br", "bl"};
    for (std::size_t k = 0; k < 4; ++k) {
        to[k].x += corner(e, "corner." + std::string(kNames[k]) + ".x", frame);
        to[k].y += corner(e, "corner." + std::string(kNames[k]) + ".y", frame);
    }
    const auto result = ai::stabilize::homographyFromCorners(from, to);
    REQUIRE(result);
    return *result;
}

const Json kGyroParams = {{"method", 4}, {"gyroSync", 0}, {"gyroFov", 90}, {"framing", 0}};

/// How far a world point lands from where the pan alone would put it, over the media's first `frames` frames: before the keys
/// (the shake) and after them. Returns {shaken, stabilized} in pixels (means).
std::pair<double, double> landingErrors(const EffectInstance& warp, std::int64_t frames) {
    double raw = 0.0, fixed = 0.0;
    for (std::int64_t k = 0; k < frames; ++k) {
        const double t = static_cast<double>(k) / 25.0;
        const masks::Point2 shaken = landing(shakeAngles(t));
        const masks::Point2 ideal = landing(panAngles(t));
        const masks::Point2 after = keyedHomography(warp, k).map(shaken);
        raw += std::hypot(shaken.x - ideal.x, shaken.y - ideal.y);
        fixed += std::hypot(after.x - ideal.x, after.y - ideal.y);
    }
    return {raw / static_cast<double>(frames), fixed / static_cast<double>(frames)};
}

} // namespace

TEST_CASE("Analyze Stabilization with Method Gyro keys the camera's rotation in one undo step and says what it used", "[ai][stabilize][gyro][commands]") {
    GyroHarness s(kClip30);
    s.gyro->track = gyroOfShake(-0.5, 2.0, 400.0);
    const ClipId clip = s.h.clip("V1@0");
    const auto added = s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect}, {"params", kGyroParams}});
    REQUIRE(added.createdIds.size() == 1);
    const auto result = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    CHECK(result.data["frames"] == 30);
    CHECK(result.data["lostFrames"] == 0);
    CHECK(s.gyro->reads == 1);
    REQUIRE(result.data.contains("gyro"));
    CHECK(result.data["gyro"]["camera"] == "GoPro HERO-Test");
    CHECK(result.data["gyro"]["synced"] == false);
    CHECK(result.data["gyro"]["fieldOfViewFrom"] == "set");
    CHECK(result.data["gyro"]["fieldOfView"].get<double>() == Approx(90.0));
    CHECK(result.data["gyro"]["framesOutsideGyro"] == 0);

    const EffectInstance* warp = s.warp("V1@0");
    REQUIRE(warp);
    REQUIRE(warp->find("corner.tl.x"));
    CHECK(warp->find("corner.tl.x")->isCurve()); // a corner pin per frame, the way Perspective keys it
    CHECK_FALSE(warp->find("transform.x")->isCurve());
    REQUIRE(warp->findData("gyro"));
    const std::string note = *warp->findData("gyro")->text();
    CHECK(note.find("camera=GoPro HERO-Test") != std::string::npos);
    CHECK(note.find("fovFrom=set") != std::string::npos);
    CHECK(note.find("sync=off") != std::string::npos);
    CHECK(*warp->findData("status")->text() == "analyzed");

    // The keys cancel the shake: a point of the world lands where the pan alone would put it.
    const auto [rawError, fixedError] = landingErrors(*warp, 30);
    INFO("mean error shaken " << rawError << " px, stabilized " << fixedError << " px");
    CHECK(rawError > 10.0);
    CHECK(fixedError < 0.5 * rawError);

    // Undo takes the keys back in one step; the instance the user added stays.
    s.h.session.undo();
    warp = s.warp("V1@0");
    REQUIRE(warp);
    CHECK(warp->find("corner.tl.x") == nullptr);
    CHECK(warp->findData("gyro") == nullptr);
    s.h.session.redo();
    warp = s.warp("V1@0");
    REQUIRE(warp);
    REQUIRE(warp->find("corner.tl.x"));

    // Saved and loaded back, the keys and the note are the same.
    const auto json = io::toJson(*s.h.state());
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    const EffectInstance* again = nullptr;
    for (const EffectInstance& e : loaded->findSequence(s.h.tl.seq)->findClip(s.h.clip("V1@0"))->clip().effects()) {
        if (e.effect == kWarpStabilizerEffect) {
            again = &e;
        }
    }
    REQUIRE(again);
    CHECK(again->find("corner.tl.x")->isCurve());
    CHECK(*again->findData("gyro")->text() == note);
    CHECK(corner(*again, "corner.tr.y", 12) == Approx(corner(*warp, "corner.tr.y", 12)));
}

TEST_CASE("Method Gyro works on a clip at another speed: the keys stay on the source frames (the analysis of card 107)",
          "[ai][stabilize][gyro][commands][speed]") {
    GyroHarness s(kClip30);
    s.gyro->track = gyroOfShake(-0.5, 2.0, 400.0);
    const ClipId clip = s.h.clip("V1@0");
    s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect}, {"params", kGyroParams}});
    for (const int percent : {200, 50}) {
        INFO("speed " << percent << " %");
        s.h.ok("clip.speedDuration", {{"clips", Json::array({clip.value})}, {"speed", percent}});
        const auto result = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
        CHECK(result.data["frames"] == 30); // every source frame the clip shows
        CHECK(result.data["gyro"]["framesOutsideGyro"] == 0);
        const EffectInstance* warp = s.warp("V1@0");
        REQUIRE(warp);
        const auto [rawError, fixedError] = landingErrors(*warp, 30);
        CHECK(fixedError < 0.5 * rawError);
    }
}

TEST_CASE("A Gyro setting that changes makes the analysis out of date; Analyze All redoes it", "[ai][stabilize][gyro][commands]") {
    GyroHarness s(kClip30);
    s.gyro->track = gyroOfShake(-0.5, 2.0, 400.0);
    const ClipId clip = s.h.clip("V1@0");
    const auto added = s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect}, {"params", kGyroParams}});
    const EffectInstanceId id{added.createdIds.at(0)};
    s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    const auto need = [&] {
        for (const auto& e : ai::stabilize::surveyStabilizers(*s.h.state(), s.h.tl.seq)) {
            if (e.effect == id) {
                return e.need;
            }
        }
        return ai::stabilize::StabilizerNeed::Skipped;
    };
    CHECK(need() == ai::stabilize::StabilizerNeed::Current);
    s.h.ok("clip.setEffect", {{"clip", clip.value}, {"effect", id.value}, {"params", {{"gyroOffset", 5}}}});
    CHECK(need() == ai::stabilize::StabilizerNeed::OutOfDate);
    const auto all = s.h.ok("effect.analyzeAll", Json::object());
    CHECK(all.data["analyzed"] == 1);
    CHECK(need() == ai::stabilize::StabilizerNeed::Current);
    CHECK(s.gyro->reads == 2);
    // The offset moved the gyro's reading: the note says so.
    CHECK(s.warp("V1@0")->findData("gyro")->text()->find("offsetMs=5.0") != std::string::npos);
}

TEST_CASE("A clip without gyro data is refused with the reason, and a host without the gyro says so", "[ai][stabilize][gyro][commands]") {
    GyroHarness s(kClip30);
    s.gyro->failure = "DJI records its gyro in a metadata track whose layout DJI has not published";
    const ClipId clip = s.h.clip("V1@0");
    s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect}, {"params", kGyroParams}});
    const auto refused = s.h.run(Json{{"command", "effect.analyzeStabilization"}, {"params", {{"clip", clip.value}}}});
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error->message.find("no gyro data") != std::string::npos);
    CHECK(refused.error->message.find("DJI") != std::string::npos);
    CHECK(s.warp("V1@0")->find("corner.tl.x") == nullptr); // nothing was keyed

    GyroHarness bare(kClip30, false, false);
    const ClipId other = bare.h.clip("V1@0");
    bare.h.ok("clip.addEffect", {{"clips", Json::array({other.value})}, {"effect", kWarpStabilizerEffect}, {"params", kGyroParams}});
    const auto none = bare.h.run(Json{{"command", "effect.analyzeStabilization"}, {"params", {{"clip", other.value}}}});
    REQUIRE_FALSE(none.ok());
    CHECK(none.error->message.find("camera metadata") != std::string::npos);

    // A gyro that does not reach the clip at all.
    GyroHarness late(kClip30);
    late.gyro->track = gyroOfShake(30.0, 31.0, 400.0);
    const ClipId third = late.h.clip("V1@0");
    late.h.ok("clip.addEffect", {{"clips", Json::array({third.value})}, {"effect", kWarpStabilizerEffect}, {"params", kGyroParams}});
    const auto outside = late.h.run(Json{{"command", "effect.analyzeStabilization"}, {"params", {{"clip", third.value}}}});
    REQUIRE_FALSE(outside.ok());
    CHECK(outside.error->message.find("does not cover") != std::string::npos);
}

TEST_CASE("Auto-sync measures the pictures, finds the gyro's offset and the field of view, and caches the measurement",
          "[ai][stabilize][gyro][commands][sync]") {
    const fs::path cache = GyroHarness::cacheFolder();
    GyroHarness s(kClip30, true, true, cache);
    s.gyro->track = gyroOfShake(-1.0, 3.0, 400.0, 0.012); // the gyro's clock runs 12 ms behind the picture's
    const ClipId clip = s.h.clip("V1@0");
    // Method Gyro with Auto-sync on (the default) and the field of view left to Auto.
    const auto added = s.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", kWarpStabilizerEffect},
                                                  {"params", {{"method", 4}, {"framing", 0}}}});
    const auto first = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}});
    INFO(first.data.dump());
    CHECK(s.pictures->frames == 30); // the one stretch it measured
    REQUIRE(first.data.contains("gyro"));
    CHECK(first.data["gyro"]["synced"] == true);
    CHECK(first.data["gyro"]["fieldOfViewFrom"] == "picture");
    CHECK(first.data["gyro"]["offsetMs"].get<double>() == Approx(12.0).margin(5.0));
    CHECK(first.data["gyro"]["fieldOfView"].get<double>() == Approx(90.0).epsilon(0.2));
    CHECK(first.data["gyro"]["match"].get<double>() > 0.7);
    const EffectInstance* warp = s.warp("V1@0");
    REQUIRE(warp);
    CHECK(warp->findData("gyro")->text()->find("sync=picture") != std::string::npos);

    // Again: the measurement of the pictures comes from the cache, no frame is decoded.
    const int decoded = s.pictures->frames;
    const auto second = s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", added.createdIds.at(0)}});
    CHECK(s.pictures->frames == decoded);
    CHECK(second.data["gyro"]["offsetMs"] == first.data["gyro"]["offsetMs"]);

    // Back to the picture's own motion: the camera note goes.
    s.h.ok("clip.setEffect", {{"clip", clip.value}, {"effect", added.createdIds.at(0)}, {"params", {{"method", 1}}}});
    s.h.ok("effect.analyzeStabilization", {{"clip", clip.value}, {"effect", added.createdIds.at(0)}});
    warp = s.warp("V1@0");
    REQUIRE(warp);
    CHECK(warp->findData("gyro") == nullptr);
}
