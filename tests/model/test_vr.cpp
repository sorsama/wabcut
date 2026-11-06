// Card 38 (0.7): VR / 360 video - the sphere maths (model/Vr.h), the format on sequences, footage interpretations and probed
// streams (settings, project file round trip, hostile files), and a 360 clip making a VR sequence.

#include "TimelineFixture.h"

#include <wabcut/model/ProjectDowngrade.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/Vr.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <span>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr VrFormat k360{VrProjection::Equirectangular360, StereoLayout::Mono};
constexpr VrFormat k360TopBottom{VrProjection::Equirectangular360, StereoLayout::TopBottom};
constexpr VrFormat k360SideBySide{VrProjection::Equirectangular360, StereoLayout::SideBySide};
constexpr VrFormat k180{VrProjection::Equirectangular180, StereoLayout::Mono};

void checkVec(vr::Vec3 v, double x, double y, double z) {
    CHECK(v.x == Approx(x).margin(1e-9));
    CHECK(v.y == Approx(y).margin(1e-9));
    CHECK(v.z == Approx(z).margin(1e-9));
}

/// The item of `media` replaced by a copy with its first video stream described as `format`.
void setFileVr(ProjectState& state, MediaId media, VrFormat format) {
    for (MediaPtr& m : state.media) {
        if (m->id == media) {
            auto item = std::make_shared<MediaItem>(*m);
            item->info.video.front().vr = format;
            m = item;
        }
    }
}

} // namespace

TEST_CASE("VR: directions and pictures map onto each other (card 38)", "[model][vr]") {
    // The middle of the picture is straight ahead, the top the zenith, a quarter across the left, three quarters the right.
    checkVec(vr::directionAt(VrProjection::Equirectangular360, 0.5, 0.5), 0, 0, 1);
    checkVec(vr::directionAt(VrProjection::Equirectangular360, 0.75, 0.5), 1, 0, 0);
    checkVec(vr::directionAt(VrProjection::Equirectangular360, 0.25, 0.5), -1, 0, 0);
    checkVec(vr::directionAt(VrProjection::Equirectangular360, 0.0, 0.5), 0, 0, -1);
    checkVec(vr::directionAt(VrProjection::Equirectangular360, 0.3, 0.0), 0, 1, 0);
    checkVec(vr::directionAt(VrProjection::Equirectangular360, 0.3, 1.0), 0, -1, 0);
    // A 180 picture holds the front half across its whole width.
    checkVec(vr::directionAt(VrProjection::Equirectangular180, 1.0, 0.5), 1, 0, 0);
    checkVec(vr::directionAt(VrProjection::Equirectangular180, 0.0, 0.5), -1, 0, 0);

    // The round trip over a grid, both projections (away from the poles, where longitude means nothing).
    for (const VrProjection p : {VrProjection::Equirectangular360, VrProjection::Equirectangular180}) {
        for (int i = 1; i < 20; ++i) {
            for (int j = 1; j < 10; ++j) {
                const double u = i / 20.0;
                const double v = j / 10.0;
                const vr::EquirectUv back = vr::uvOf(p, vr::directionAt(p, u, v));
                REQUIRE(back.inside);
                CHECK(back.u == Approx(u).margin(1e-9));
                CHECK(back.v == Approx(v).margin(1e-9));
            }
        }
    }
    // Behind a 180 picture there is nothing; behind a 360 one is its edge, wrapping.
    CHECK_FALSE(vr::uvOf(VrProjection::Equirectangular180, {0, 0, -1}).inside);
    const vr::EquirectUv behind = vr::uvOf(VrProjection::Equirectangular360, {0, 0, -1});
    CHECK(behind.inside);
    CHECK((behind.u < 1e-9 || behind.u > 1.0 - 1e-9));
    // Longitude wraps: slightly to either side of straight behind lands at the two edges.
    CHECK(vr::uvOf(VrProjection::Equirectangular360, {0.01, 0, -1}).u == Approx(1.0 - 0.5 / 360.0 * 0.573).margin(0.002));
    CHECK(vr::uvOf(VrProjection::Equirectangular360, {-0.01, 0, -1}).u < 0.01);
}

TEST_CASE("VR: yaw pitch and roll turn the sphere the way FFmpeg's convention says (card 38)", "[model][vr]") {
    const vr::Vec3 ahead{0, 0, 1};
    checkVec(vr::apply(vr::rotation(90, 0, 0), ahead), 1, 0, 0);  // yaw: what is in front goes to the right
    checkVec(vr::apply(vr::rotation(0, 90, 0), ahead), 0, 1, 0);  // pitch: up
    checkVec(vr::apply(vr::rotation(0, 0, 90), {0, 1, 0}), 1, 0, 0); // roll: the top tilts to the right
    checkVec(vr::apply(vr::rotation(0, 0, 90), ahead), 0, 0, 1);  // the roll leaves the line of sight alone
    // R = yaw * pitch * roll: the roll applies first, the yaw last.
    checkVec(vr::apply(vr::rotation(90, 90, 0), ahead), 0, 1, 0);
    checkVec(vr::apply(vr::rotation(90, 0, 90), {0, 1, 0}), 0, 0, -1);
    // Turning is orthonormal, and its transpose undoes it.
    const vr::Mat3 r = vr::rotation(37, -23, 111);
    const vr::Mat3 id = vr::multiply(r, vr::transpose(r));
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(id[i] == Approx(vr::identity()[i]).margin(1e-12));
    }
    const vr::Vec3 v = vr::apply(r, {0.3, -0.5, 0.8});
    CHECK(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) == Approx(std::sqrt(0.09 + 0.25 + 0.64)));
}

TEST_CASE("VR: Reorient moves content where the angles say and per eye (card 38)", "[model][vr]") {
    // Yaw 90: what was in front is now to the right, so the picture's right quarter shows the old middle.
    const vr::EquirectUv fromMiddle = vr::reorientUv(k360, vr::rotation(90, 0, 0), 0.75, 0.5);
    CHECK(fromMiddle.u == Approx(0.5).margin(1e-9));
    CHECK(fromMiddle.v == Approx(0.5).margin(1e-9));
    // Pitch 45 up: the old middle now sits 45 degrees above the horizon (v = 0.25).
    const vr::EquirectUv fromBelow = vr::reorientUv(k360, vr::rotation(0, 45, 0), 0.5, 0.25);
    CHECK(fromBelow.u == Approx(0.5).margin(1e-9));
    CHECK(fromBelow.v == Approx(0.5).margin(1e-9));
    // No turn: every pixel shows itself.
    for (const VrFormat f : {k360, k360TopBottom, k360SideBySide, k180}) {
        const vr::EquirectUv same = vr::reorientUv(f, vr::identity(), 0.3, 0.4);
        CHECK(same.inside);
        CHECK(same.u == Approx(0.3).margin(1e-9));
        CHECK(same.v == Approx(0.4).margin(1e-9));
    }
    // Stereoscopic pictures turn each eye in its own half and never take the other eye's pixels.
    for (int i = 1; i < 12; ++i) {
        const double u = i / 12.0;
        const vr::EquirectUv top = vr::reorientUv(k360TopBottom, vr::rotation(40, 20, 5), u, 0.2);
        const vr::EquirectUv bottom = vr::reorientUv(k360TopBottom, vr::rotation(40, 20, 5), u, 0.7);
        CHECK(top.v < 0.5);
        CHECK(bottom.v >= 0.5);
        const vr::EquirectUv left = vr::reorientUv(k360SideBySide, vr::rotation(40, 20, 5), 0.2, u);
        const vr::EquirectUv right = vr::reorientUv(k360SideBySide, vr::rotation(40, 20, 5), 0.7, u);
        CHECK(left.u < 0.5);
        CHECK(right.u >= 0.5);
    }
    // The same turn in both eyes: the right eye's result is the left eye's, one half further.
    const vr::EquirectUv a = vr::reorientUv(k360TopBottom, vr::rotation(40, 20, 5), 0.3, 0.2);
    const vr::EquirectUv b = vr::reorientUv(k360TopBottom, vr::rotation(40, 20, 5), 0.3, 0.7);
    CHECK(b.u == Approx(a.u).margin(1e-9));
    CHECK(b.v == Approx(a.v + 0.5).margin(1e-9));
    // A 180 picture turned far to one side shows nothing where the other half would be.
    CHECK_FALSE(vr::reorientUv(k180, vr::rotation(120, 0, 0), 0.5, 0.5).inside);
}

TEST_CASE("VR: the viewer looks where the angles say (card 38)", "[model][vr]") {
    const vr::Mat3 level = vr::identity();
    // Straight ahead: the middle of the picture.
    const vr::EquirectUv centre = vr::viewUv(k360, level, 90.0, 16.0 / 9.0, 0.5, 0.5);
    CHECK(centre.u == Approx(0.5).margin(1e-9));
    CHECK(centre.v == Approx(0.5).margin(1e-9));
    // 90 degrees across: the right edge of the view is 45 degrees to the right, a eighth of the sphere.
    CHECK(vr::viewUv(k360, level, 90.0, 16.0 / 9.0, 1.0, 0.5).u == Approx(0.625).margin(1e-9));
    CHECK(vr::viewUv(k360, level, 90.0, 16.0 / 9.0, 0.0, 0.5).u == Approx(0.375).margin(1e-9));
    // Looking along yaw 90 shows what lies to the right; pitch 45 what lies above.
    CHECK(vr::viewUv(k360, vr::rotation(90, 0, 0), 90.0, 1.0, 0.5, 0.5).u == Approx(0.75).margin(1e-9));
    CHECK(vr::viewUv(k360, vr::rotation(0, 45, 0), 90.0, 1.0, 0.5, 0.5).v == Approx(0.25).margin(1e-9));
    // A narrower field of view sees less: the edge of a 45 degree view is 22.5 degrees off.
    CHECK(vr::viewUv(k360, level, 45.0, 1.0, 1.0, 0.5).u == Approx(0.5 + 22.5 / 360.0).margin(1e-9));
    // Stereoscopic pictures are viewed through the left eye (or the right).
    const vr::EquirectUv leftEye = vr::viewUv(k360TopBottom, level, 90.0, 1.0, 0.5, 0.5, 0);
    const vr::EquirectUv rightEye = vr::viewUv(k360TopBottom, level, 90.0, 1.0, 0.5, 0.5, 1);
    CHECK(leftEye.v == Approx(0.25).margin(1e-9));
    CHECK(rightEye.v == Approx(0.75).margin(1e-9));
    CHECK(vr::viewUv(k360SideBySide, level, 90.0, 1.0, 0.5, 0.5, 1).u == Approx(0.75).margin(1e-9));
    // A 180 picture seen from the side has an edge.
    CHECK_FALSE(vr::viewUv(k180, vr::rotation(100, 0, 0), 60.0, 1.0, 0.5, 0.5).inside);
}

TEST_CASE("VR: names and combinations and sizes (card 38)", "[model][vr]") {
    for (const VrProjection p : {VrProjection::None, VrProjection::Equirectangular360, VrProjection::Equirectangular180}) {
        CHECK(vrProjectionFromString(toString(p)) == p);
    }
    for (const StereoLayout l : {StereoLayout::Mono, StereoLayout::TopBottom, StereoLayout::SideBySide}) {
        CHECK(stereoLayoutFromString(toString(l)) == l);
    }
    CHECK_FALSE(vrProjectionFromString("cubemap"));
    CHECK_FALSE(stereoLayoutFromString("anaglyph"));
    CHECK_FALSE(vrFormatProblem(k360));
    CHECK_FALSE(vrFormatProblem(VrFormat{}));
    CHECK(vrFormatProblem(VrFormat{VrProjection::None, StereoLayout::TopBottom}));
    CHECK(vrEyeSize(k360, 3840, 1920) == std::array<int, 2>{3840, 1920});
    CHECK(vrEyeSize(k360TopBottom, 3840, 3840) == std::array<int, 2>{3840, 1920});
    CHECK(vrEyeSize(k360SideBySide, 7680, 1920) == std::array<int, 2>{3840, 1920});
    CHECK(vrAspectSuits(k360, 3840, 1920));
    CHECK_FALSE(vrAspectSuits(k360, 1920, 1080));
    CHECK(vrAspectSuits(k360TopBottom, 3840, 3840));
    CHECK(vrAspectSuits(k180, 2048, 2048));
    CHECK(vrAspectSuits(VrFormat{}, 1920, 1080)); // flat footage always suits itself
}

TEST_CASE("VR: a sequence's format is a setting and saved only when VR and read back (card 38)", "[model][vr][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    // Flat: nothing is written.
    const auto flat = io::toJson(tl.state);
    REQUIRE(flat);
    CHECK(flat->find("\"vr\"") == std::string::npos);
    CHECK(flat->find("projection") == std::string::npos);

    edit::SequenceSettingsChange change;
    change.vr = k360TopBottom;
    auto set = edit::setSequenceSettings(tl.state, tl.seq, change);
    REQUIRE(set);
    CHECK(set->state.findSequence(tl.seq)->vr == k360TopBottom);
    CHECK(set->state.findSequence(tl.seq)->vr.isStereo());
    const auto json = io::toJson(set->state);
    REQUIRE(json);
    CHECK(json->find("\"projection\": \"equirect360\"") != std::string::npos);
    CHECK(json->find("\"stereo\": \"topBottom\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(loaded->findSequence(tl.seq)->vr == k360TopBottom);
    CHECK(*loaded == set->state);

    // Mono writes no layout; a flat setting writes nothing again.
    change.vr = k180;
    const auto mono = edit::setSequenceSettings(set->state, tl.seq, change);
    REQUIRE(mono);
    const auto monoJson = io::toJson(mono->state);
    REQUIRE(monoJson);
    CHECK(monoJson->find("\"projection\": \"equirect180\"") != std::string::npos);
    CHECK(monoJson->find("\"topBottom\"") == std::string::npos); // a mono picture writes no layout
    CHECK(monoJson->find("\"sideBySide\"") == std::string::npos);
    change.vr = VrFormat{};
    const auto cleared = edit::setSequenceSettings(mono->state, tl.seq, change);
    REQUIRE(cleared);
    CHECK(io::toJson(cleared->state)->find("projection") == std::string::npos);

    // A stereoscopic layout on a flat picture is refused.
    change.vr = VrFormat{VrProjection::None, StereoLayout::SideBySide};
    CHECK_FALSE(edit::setSequenceSettings(tl.state, tl.seq, change));

    // Saving for an older release drops it and says so.
    const auto old = io::toJsonForRelease(set->state, "0.6");
    REQUIRE(old);
    CHECK(old->json.find("projection") == std::string::npos);
    bool reported = false;
    for (const io::DowngradeLoss& loss : old->losses) {
        reported = reported || loss.id == "vr360";
    }
    CHECK(reported);
}

TEST_CASE("VR: hostile project files are refused (card 38)", "[model][vr][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    edit::SequenceSettingsChange change;
    change.vr = k360SideBySide;
    const auto json = io::toJson(edit::setSequenceSettings(tl.state, tl.seq, change)->state);
    REQUIRE(json);
    const auto replaced = [&](const std::string& from, const std::string& to) {
        std::string text = *json;
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    };
    CHECK(io::fromJson(*json));
    CHECK_FALSE(io::fromJson(replaced("\"equirect360\"", "\"cubemap\"")));
    CHECK_FALSE(io::fromJson(replaced("\"equirect360\"", "5")));
    CHECK_FALSE(io::fromJson(replaced("\"sideBySide\"", "\"anaglyph\"")));
    CHECK_FALSE(io::fromJson(replaced("\"equirect360\"", "\"none\"")));  // a layout needs a projection
    CHECK_FALSE(io::fromJson(replaced("\"equirect360\"", "\"" + std::string(5000, 'x') + "\"")));
}

TEST_CASE("VR: footage is VR by what the file says unless Interpret Footage says otherwise (card 38)", "[model][vr][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const MediaId media = tl.media('A');
    ProjectState state = tl.state;
    CHECK_FALSE(mediaVr(*state.findMedia(media)).isVr());
    setFileVr(state, media, k360TopBottom);
    CHECK(mediaVr(*state.findMedia(media)) == k360TopBottom);

    // Interpret Footage overrides the file, including "this is flat".
    MediaInterpretation interpretation;
    interpretation.vr = k180;
    auto changed = edit::interpretFootage(state, std::span(&media, 1), interpretation);
    REQUIRE(changed);
    CHECK(mediaVr(*changed->state.findMedia(media)) == k180);
    interpretation.vr = VrFormat{};
    auto flat = edit::interpretFootage(state, std::span(&media, 1), interpretation);
    REQUIRE(flat);
    CHECK_FALSE(mediaVr(*flat->state.findMedia(media)).isVr());
    CHECK(flat->state.findMedia(media)->interpretation.has_value()); // the "flat" answer is kept: it is not "empty"
    // Both fields survive the project file.
    for (const ProjectState* s : {&state, &changed->state, &flat->state}) {
        const auto json = io::toJson(*s);
        REQUIRE(json);
        const auto loaded = io::fromJson(*json);
        REQUIRE(loaded);
        CHECK(*loaded == *s);
        CHECK(mediaVr(*loaded->findMedia(media)) == mediaVr(*s->findMedia(media)));
    }
    // A stereoscopic layout on a flat interpretation is refused when the file is read.
    auto json = io::toJson(changed->state);
    REQUIRE(json);
    const std::string from = "\"projection\": \"equirect180\"";
    const auto at = json->find(from);
    REQUIRE(at != std::string::npos);
    std::string broken = *json;
    broken.replace(at, from.size(), "\"projection\": \"none\", \"stereo\": \"topBottom\"");
    CHECK_FALSE(io::fromJson(broken));
}

TEST_CASE("VR: a sequence made from a 360 clip is a VR sequence (card 38)", "[model][vr]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    const MediaId media = tl.media('A');
    ProjectState state = tl.state;
    setFileVr(state, media, k360TopBottom);
    const auto made = edit::newSequenceFromClip(state, media);
    REQUIRE(made);
    CHECK(made->state.findSequence(made->id)->vr == k360TopBottom);
    // A flat clip makes a flat sequence.
    const auto flat = edit::newSequenceFromClip(tl.state, media);
    REQUIRE(flat);
    CHECK_FALSE(flat->state.findSequence(flat->id)->vr.isVr());
    // A copy of a VR sequence is VR too.
    edit::SequenceSettingsChange change;
    change.vr = k360;
    const auto vrSequence = edit::setSequenceSettings(tl.state, tl.seq, change);
    REQUIRE(vrSequence);
    const auto copy = edit::duplicateSequence(vrSequence->state, tl.seq);
    REQUIRE(copy);
    CHECK(copy->state.findSequence(copy->id)->vr == k360);
}
