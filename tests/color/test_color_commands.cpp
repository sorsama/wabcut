// The colour commands through the CommandEngine (editingCommands(), so the registration is tested too), with a fake
// renderer: color.grade.set/get (partial params, curves, LUTs, undo/redo, dry run, errors), color.auto, color.match,
// color.matchScene (one undo step over a C.5 scene), lut.import/list, cdl.export/import and scopes.read.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/color/Cdl.h>
#include <wabcut/color/ColorCommands.h>
#include <wabcut/color/Looks.h>
#include <wabcut/color/Scopes.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::color;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

/// Renders a synthetic picture per clip: a grey card, grass and skin under a per-media cast and a per-take exposure
/// (from the clip's source In), then the clip's grade through the CPU reference.
class FakeRenderer final : public ColorServices {
public:
    int calls = 0;

    std::expected<ColorImage, Error> frame(const ProjectState& state, SequenceId sequence, const FrameRequest& request) override {
        ++calls;
        const Sequence& seq = *state.findSequence(sequence);
        const Clip* clip = nullptr;
        if (request.solo.isValid()) {
            clip = &seq.findClip(request.solo)->clip();
        } else {
            for (const TrackPtr& t : seq.videoTracks) {
                if (const auto i = t->clipIndexAt(request.time)) {
                    clip = &t->clips[*i];
                }
            }
        }
        if (!clip) {
            return std::unexpected(Error{ErrorCode::NotFound, "nothing at that time"});
        }
        const MediaItem* media = state.findMedia(clip->media);
        const char letter = media->name.empty() ? 'A' : media->name.front();
        const Rgb cast = letter == 'A' ? Rgb{1.2, 1.0, 0.75} : letter == 'B' ? Rgb{0.8, 1.0, 1.3} : Rgb{1, 1, 1};
        const double take = 1.0 + 0.25 * std::sin(static_cast<double>(clip->sourceIn.value) / 1e10);
        const GradeSpec* grade = nullptr;
        GradeSpec spec;
        for (const EffectInstance& e : clip->effects()) {
            if (e.id == request.stopBefore) {
                break;
            }
            if (e.effect == kGradeEffect && e.enabled) {
                spec = evaluateGrade(e, clip->curveTimeAt(request.time), LutLibrary::shared().resolver(state));
                grade = &spec;
            }
        }
        ColorImage image;
        image.width = 64;
        image.height = 36;
        image.rgba.resize(64 * 36 * 4);
        for (int y = 0; y < 36; ++y) {
            for (int x = 0; x < 64; ++x) {
                Rgb c = x < 16 ? Rgb{0.18, 0.18, 0.18} : x < 22 ? Rgb{0.6, 0.6, 0.6} : y < 18 ? Rgb{0.08, 0.25, 0.05} : Rgb{0.45, 0.3, 0.22};
                c = c * cast * take;
                if (grade) {
                    c = gradePixel(*grade, c);
                }
                const Rgb e = encode709(c);
                float* p = image.rgba.data() + (static_cast<std::size_t>(y) * 64 + x) * 4;
                p[0] = static_cast<float>(e[0]);
                p[1] = static_cast<float>(e[1]);
                p[2] = static_cast<float>(e[2]);
                p[3] = 1.0f;
            }
        }
        return image;
    }
};

struct ColorHarness {
    std::shared_ptr<FakeRenderer> renderer = std::make_shared<FakeRenderer>();
    CommandHarness h;

    explicit ColorHarness(std::string_view picture, bool services = true)
        : h(picture, policy(services), ai::edit::editingCommands()) {}

    command::EnginePolicy policy(bool services) {
        command::EnginePolicy p;
        if (services) {
            p.services = renderer;
        }
        return p;
    }

    const EffectInstance* grade(std::string_view ref) {
        return gradeOf(h.state()->findSequence(h.tl.seq)->findClip(h.clip(ref))->clip());
    }
    GradeParams params(std::string_view ref) { return gradeParamsAt(*grade(ref), Ticks{}); }
};

/// The grey card's Oklab chroma in the clip as rendered.
double greyChroma(FakeRenderer& r, const ProjectState& state, SequenceId seq, ClipId clip) {
    const Clip& c = state.findSequence(seq)->findClip(clip)->clip();
    const auto image = r.frame(state, seq, FrameRequest{c.start(), clip, {}});
    REQUIRE(image);
    const float* p = image->rgba.data() + (10 * 64 + 5) * 4;
    const Rgb lab = linearToOklab(decode709({p[0], p[1], p[2]}));
    return std::hypot(lab[1], lab[2]);
}

} // namespace

TEST_CASE("color.grade.set: partial parameters, stages, curves and LUTs in one undo step", "[color][commands]") {
    ColorHarness c(R"(
        V1 tgt |AaaaBbbb|
    )");
    const ClipId a = c.h.clip("V1@0");
    const ClipId b = c.h.clip("V1@4");
    const auto first = c.h.ok("color.grade.set", Json{{"clips", Json::array({a.value, b.value})},
                                                      {"params", Json{{"basic.exposure", 0.5}, {"basic.saturation", 120}}},
                                                      {"stages", Json{{"vignette", false}}},
                                                      {"curves", Json{{"master", Json::array({Json::array({0, 0}), Json::array({0.5, 0.6}), Json::array({1, 1})})}}},
                                                      {"look", "builtin:teal-orange"}});
    CHECK(first.committed);
    CHECK(c.h.session.undoCount() == 1);
    REQUIRE(c.grade("V1@0"));
    REQUIRE(c.grade("V1@4"));
    CHECK(c.params("V1@0").exposure == 0.5);
    CHECK(c.params("V1@4").saturation == 120);
    CHECK_FALSE(c.params("V1@0").vignette);
    CHECK(c.grade("V1@0")->findData("curves.master")->points()->size() == 3);
    CHECK(*c.grade("V1@0")->findData("creative.look")->text() == "builtin:teal-orange");
    // A second set changes only what it names, on the same instance.
    const EffectInstanceId id = c.grade("V1@0")->id;
    c.h.ok("color.grade.set", Json{{"clip", a.value}, {"params", Json{{"basic.contrast", -20}}}, {"look", ""}});
    CHECK(c.grade("V1@0")->id == id);
    CHECK(c.params("V1@0").exposure == 0.5);
    CHECK(c.params("V1@0").contrast == -20);
    CHECK_FALSE(c.grade("V1@0")->findData("creative.look"));
    CHECK(c.h.state()->findSequence(c.h.tl.seq)->findClip(a)->clip().effects().size() == 1);
    // get
    const auto got = c.h.ok("color.grade.get", Json{{"clip", a.value}});
    CHECK(got.data["params"]["basic.contrast"] == -20);
    CHECK(got.data["curves"]["master"].size() == 3);
    CHECK(got.data["active"] == true);
    // undo / redo
    REQUIRE(c.h.session.undo());
    CHECK(c.params("V1@0").contrast == 0);
    REQUIRE(c.h.session.undo());
    CHECK_FALSE(c.grade("V1@0"));
    REQUIRE(c.h.session.redo());
    CHECK(c.params("V1@0").exposure == 0.5);
    // Errors and dry runs.
    CHECK(c.h.errorOf("color.grade.set", Json{{"clip", a.value}, {"params", Json{{"basic.nope", 1}}}}) == "InvalidParams");
    CHECK(c.h.errorOf("color.grade.set", Json{{"clip", a.value}, {"params", Json{{"basic.exposure", 9}}}}) == "EditRejected");
    CHECK(c.h.errorOf("color.grade.set", Json{{"clip", a.value}, {"look", "builtin:nope"}}) == "InvalidParams");
    CHECK(c.h.errorOf("color.grade.set", Json{{"clip", a.value}, {"inputLut", "asset:999"}}) == "InvalidParams");
    CHECK(c.h.errorOf("color.grade.set", Json{{"clip", a.value}, {"curves", Json{{"purple", Json::array()}}}}) == "InvalidParams");
    CHECK(c.h.errorOf("color.grade.set", Json{{"clip", a.value}}) == "InvalidParams");
    const auto before = c.h.session.revision();
    const auto dry = c.h.run(Json{{"command", "color.grade.set"}, {"dryRun", true}, {"params", Json{{"clip", b.value}, {"params", Json{{"basic.tint", 30}}}}}});
    REQUIRE(dry.ok());
    CHECK(dry.dryRun);
    CHECK(c.h.session.revision() == before);
}

TEST_CASE("color.auto neutralises the clip's cast with editable Basic parameters", "[color][commands]") {
    ColorHarness c(R"(
        V1 tgt |AaaaBbbb|
    )");
    const ClipId a = c.h.clip("V1@0");
    const ClipId b = c.h.clip("V1@4");
    const double beforeA = greyChroma(*c.renderer, *c.h.state(), c.h.tl.seq, a);
    const auto result = c.h.ok("color.auto", Json{{"clips", Json::array({a.value, b.value})}});
    CHECK(result.data["clips"].size() == 2);
    CHECK(c.h.session.undoCount() == 1);
    const GradeParams pa = c.params("V1@0");
    const GradeParams pb = c.params("V1@4");
    CHECK(pa.temperature < -5); // A is warm: cooled
    CHECK(pb.temperature > 5);  // B is cool: warmed
    const double afterA = greyChroma(*c.renderer, *c.h.state(), c.h.tl.seq, a);
    CHECK(afterA < beforeA / 3.0);
    CHECK(afterA < 0.015);
    // Running Auto again on the graded clip starts from its input, so it lands on the same values.
    c.h.ok("color.auto", Json{{"clip", a.value}});
    CHECK(c.params("V1@0").temperature == Approx(pa.temperature).margin(1.0));
    // Without a renderer: a clear refusal.
    ColorHarness bare(R"(
        V1 tgt |Aaaa|
    )",
                      false);
    CHECK(bare.h.errorOf("color.auto", Json{{"clip", bare.h.clip("V1@0").value}}) == "InvalidRequest");
}

TEST_CASE("color.match and color.matchScene: one undo step for the whole scene", "[color][commands]") {
    // V1: one take of A cut into three shots (C.5 clip.splitAtShots), then B.
    ColorHarness c(R"(
        V1 tgt |AaaaA4aaA8aaBbbb|
    )");
    const auto scene = sceneClips(*c.h.state()->findSequence(c.h.tl.seq), c.h.clip("V1@4"));
    CHECK(scene == std::vector<ClipId>{c.h.clip("V1@0"), c.h.clip("V1@4"), c.h.clip("V1@8")});
    // Match B to A's middle shot.
    const auto matched = c.h.ok("color.match", Json{{"clip", c.h.clip("V1@12").value}, {"reference", Json{{"clip", c.h.clip("V1@4").value}}}});
    CHECK(matched.data["clips"][0]["after"]["a"].get<double>() ==
          Approx(matched.data["clips"][0]["reference"]["a"].get<double>()).margin(0.01));
    REQUIRE(c.h.session.undo());
    // The scene: the other two shots of the take, one undo step; B untouched.
    const auto before = c.h.session.undoCount();
    const auto dry = c.h.run(Json{{"command", "color.matchScene"}, {"dryRun", true}, {"params", Json{{"reference", c.h.clip("V1@4").value}}}});
    REQUIRE(dry.ok());
    CHECK(dry.changes.sequences.front().modifiedClips.size() == 2);
    const auto sceneResult = c.h.ok("color.matchScene", Json{{"reference", c.h.clip("V1@4").value}});
    CHECK(sceneResult.data["clips"].size() == 2);
    CHECK(c.h.session.undoCount() == before + 1);
    CHECK(c.grade("V1@0"));
    CHECK(c.grade("V1@8"));
    CHECK_FALSE(c.grade("V1@4"));
    CHECK_FALSE(c.grade("V1@12"));
    REQUIRE(c.h.session.undo());
    CHECK_FALSE(c.grade("V1@0"));
    CHECK_FALSE(c.grade("V1@8"));
    // Other scopes.
    CHECK(c.h.ok("color.matchScene", Json{{"reference", c.h.clip("V1@4").value}, {"scope", "track"}}).data["clips"].size() == 3);
    CHECK(c.h.errorOf("color.matchScene", Json{{"reference", c.h.clip("V1@12").value}}) == "EditRejected"); // B is alone
}

TEST_CASE("lut.import / lut.list: an asset with a reference the grade uses", "[color][commands][lut]") {
    ColorHarness c(R"(
        V1 tgt |Aaaa|
    )");
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "color";
    std::filesystem::create_directories(dir);
    const auto file = dir / "Command Look.cube";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << writeCube(*builtinLut("builtin:bleach-bypass"));
    }
    const auto imported = c.h.ok("lut.import", Json{{"path", file.generic_string()}});
    const std::string reference = imported.data["reference"];
    CHECK(reference.starts_with("asset:"));
    CHECK(imported.data["size3d"] == 33);
    CHECK(c.h.state()->assets.size() == 1);
    // The same file again: the existing asset, no new undo step.
    const auto undos = c.h.session.undoCount();
    CHECK(c.h.ok("lut.import", Json{{"path", file.generic_string()}}).data["existing"] == true);
    CHECK(c.h.session.undoCount() == undos);
    c.h.ok("color.grade.set", Json{{"clip", c.h.clip("V1@0").value}, {"look", reference}});
    const auto list = c.h.ok("lut.list");
    CHECK(list.data["builtins"].size() == builtinLuts().size());
    REQUIRE(list.data["assets"].size() == 1);
    CHECK(list.data["assets"][0]["usedBy"] == 1);
    CHECK(c.h.errorOf("lut.import", Json{{"path", (dir / "missing.cube").generic_string()}}) == "NotFound");
    const auto notCube = dir / "notes.txt";
    {
        std::ofstream(notCube) << "hello";
    }
    CHECK(c.h.errorOf("lut.import", Json{{"path", notCube.generic_string()}}) == "InvalidParams");
    const auto broken = dir / "broken.cube";
    {
        std::ofstream(broken) << "LUT_3D_SIZE 2\n0 0 0\n";
    }
    CHECK(c.h.errorOf("lut.import", Json{{"path", broken.generic_string()}}) == "EditRejected");
}

TEST_CASE("cdl.export / cdl.import round-trip the wheels", "[color][commands][cdl]") {
    ColorHarness c(R"(
        V1 tgt |AaaaBbbb|
    )");
    const ClipId a = c.h.clip("V1@0");
    const ClipId b = c.h.clip("V1@4");
    c.h.ok("color.grade.set", Json{{"clip", a.value},
                                   {"params", Json{{"wheels.lift.red", 0.05}, {"wheels.gain.blue", 1.2}, {"wheels.gamma.green", 0.9},
                                                   {"wheels.offset.red", -0.01}, {"wheels.saturation", 1.1}}}});
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "color";
    std::filesystem::create_directories(dir);
    const auto file = dir / "wheels.cc";
    std::filesystem::remove(file);
    c.h.ok("cdl.export", Json{{"clip", a.value}, {"path", file.generic_string()}});
    CHECK(c.h.errorOf("cdl.export", Json{{"clip", a.value}, {"path", file.generic_string()}}) == "EditRejected"); // exists
    c.h.ok("cdl.export", Json{{"clip", a.value}, {"path", file.generic_string()}, {"overwrite", true}});
    CHECK(c.h.errorOf("cdl.export", Json{{"clips", Json::array({a.value, b.value})}, {"path", file.generic_string()}, {"overwrite", true}}) ==
          "InvalidParams"); // .cc holds one
    c.h.ok("cdl.import", Json{{"clip", b.value}, {"path", file.generic_string()}});
    const Cdl ca = cdlOf(c.params("V1@0"));
    const Cdl cb = cdlOf(c.params("V1@4"));
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(cb.slope[i] == Approx(ca.slope[i]).margin(1e-6));
        CHECK(cb.offset[i] == Approx(ca.offset[i]).margin(1e-6));
        CHECK(cb.power[i] == Approx(ca.power[i]).margin(1e-6));
    }
    CHECK(cb.saturation == Approx(1.1));
    CHECK(c.h.errorOf("cdl.import", Json{{"clip", b.value}, {"path", (dir / "x.txt").generic_string()}}) == "InvalidParams");
}

TEST_CASE("lut.export writes the grade as a 33-point cube that reproduces it", "[color][commands][lut]") {
    ColorHarness c(R"(
        V1 tgt |AaaaBbbb|
    )");
    const ClipId a = c.h.clip("V1@0");
    c.h.ok("color.grade.set", Json{{"clip", a.value},
                                   {"params", Json{{"basic.exposure", 0.4}, {"basic.contrast", 20}, {"basic.saturation", 130},
                                                   {"creative.fadedFilm", 15}, {"wheels.gain.blue", 1.1},
                                                   {"vignette.amount", -1.5}}},
                                   {"curves", Json{{"master", Json::array({Json::array({0, 0}), Json::array({0.5, 0.6}),
                                                                           Json::array({1, 1})})}}},
                                   {"look", "builtin:teal-orange"}});
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "color";
    std::filesystem::create_directories(dir);
    const auto file = dir / "graded # shot.cube";
    std::filesystem::remove(file);
    // A dry run writes nothing.
    REQUIRE(c.h.run(Json{{"command", "lut.export"}, {"params", Json{{"clip", a.value}, {"path", file.generic_string()}}}, {"dryRun", true}}).ok());
    CHECK_FALSE(std::filesystem::exists(file));
    const auto exported = c.h.ok("lut.export", Json{{"clip", a.value}, {"path", file.generic_string()}, {"title", "Shot #1 \"final\""}});
    CHECK(exported.data["size"] == 33);
    CHECK(exported.data["leftOut"] == Json::array({"Vignette"}));
    CHECK(c.h.errorOf("lut.export", Json{{"clip", a.value}, {"path", file.generic_string()}}) == "EditRejected"); // exists
    CHECK(c.h.errorOf("lut.export", Json{{"clip", c.h.clip("V1@4").value}, {"path", (dir / "b.cube").generic_string()}}) == "NotFound");
    CHECK(c.h.errorOf("lut.export", Json{{"clip", a.value}, {"path", (dir / "b.txt").generic_string()}}) == "InvalidParams");

    // It reads back, and applying it gives what the grade gives (without the vignette) to within the lattice's error.
    auto lut = loadCube(file);
    REQUIRE(lut);
    CHECK(lut->size3d == 33);
    CHECK(lut->title == "Shot 1 final");
    GradeSpec spec = evaluateGrade(*c.grade("V1@0"), Ticks{}, LutLibrary::shared().resolver(*c.h.state()));
    spec.params.vignette = false;
    // Trilinear interpolation between 33 points: a few code values at worst where the curves bend, far less on average.
    double worst = 0;
    double sum = 0;
    for (int i = 0; i < 400; ++i) {
        const Rgb code{std::fmod(i * 0.6180339887, 1.0), std::fmod(i * 0.3247179572, 1.0), std::fmod(i * 0.5698402910, 1.0)};
        const Rgb want = encode709(gradePixel(spec, decode709(code)));
        const Rgb got = applyLut(*lut, code);
        for (std::size_t k = 0; k < 3; ++k) {
            const double e = std::abs(std::clamp(want[k], 0.0, 1.0) - got[k]);
            worst = std::max(worst, e);
            sum += e;
        }
    }
    INFO("worst " << worst << ", mean " << sum / 1200.0);
    CHECK(worst < 0.03);
    CHECK(sum / 1200.0 < 0.005);
    // An untouched grade is the identity.
    const Lut identity = gradeToLut(GradeSpec{}, 17, "x");
    for (int r = 0; r < 17; r += 4) {
        const Rgb e = identity.entry3d(r, 16 - r, r / 2);
        CHECK(e[0] == Approx(r / 16.0).margin(1e-6));
        CHECK(e[1] == Approx((16 - r) / 16.0).margin(1e-6));
    }
}
TEST_CASE("scopes.read: a verdict an agent can use", "[color][commands][scopes]") {
    ColorHarness c(R"(
        V1 tgt |Aaaa|
    )");
    const auto read = c.h.ok("scopes.read", Json{{"time", tc(1)}});
    CHECK(read.data.contains("exposure"));
    CHECK(read.data["cast"] != "neutral"); // A is warm
    CHECK(read.data["luma"]["median"].get<double>() > 0.0);
    // Darken it a lot: underexposed.
    c.h.ok("color.grade.set", Json{{"clip", c.h.clip("V1@0").value}, {"params", Json{{"basic.exposure", -3}}}});
    CHECK(c.h.ok("scopes.read", Json{{"clip", c.h.clip("V1@0").value}}).data["exposure"] == "underexposed");
    CHECK_FALSE(c.h.ok("scopes.read", Json{{"time", tc(1)}}).committed);
}

TEST_CASE("A scene of multicam clips is one angle's adjacent pieces (0.1.2)", "[color][commands][multicam]") {
    // Nested clips have no media item: comparing Clip::media made every multicam piece one scene.
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        V2     |Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )");
    edit::MulticamSpec spec;
    spec.angles = {tl.media('A'), tl.media('B')};
    spec.sync = MulticamSync::Manual;
    const auto mc = edit::createMulticam(tl.state, spec);
    REQUIRE(mc.has_value());
    ProjectState state = mc->state;
    // Angle 0 over 0-10 and 10-20 (two edits), angle 1 over 20-30, on V1 of the edit.
    const auto place = [&](std::size_t angle, int from) {
        edit::SourceClip src;
        src.sequence = mc->id;
        src.sourceRange = TimeRange::fromStartEnd(f(from), f(from + 10));
        src.patches = {edit::SourcePatch{StreamRef{StreamKind::Video, 0}, tl.track("V1")}};
        src.videoAngle = AngleSet::of(angle);
        state = ok(edit::overwriteEdit(state, tl.seq, src, f(from))).state;
    };
    place(0, 0);
    place(0, 10);
    place(1, 20);
    const Sequence& seq = *state.findSequence(tl.seq);
    const Track& v1 = *seq.findTrack(tl.track("V1"));
    REQUIRE(v1.clips.size() == 3);
    CHECK(sceneClips(seq, v1.clips[0].id) == std::vector<ClipId>{v1.clips[0].id, v1.clips[1].id});
    CHECK(sceneClips(seq, v1.clips[2].id) == std::vector<ClipId>{v1.clips[2].id});
}

TEST_CASE("color.management.* and color.inputSpace.*: the project's colour management and a clip's input colour space (I.9)",
          "[color][commands][colormanagement]") {
    ColorHarness c(R"(
        V1 tgt |Aaaaaaaa|
    )");
    // The default, and what there is to choose from.
    const auto get = c.h.ok("color.management.get");
    CHECK(get.data["mode"] == "rec709");
    CHECK(get.data["name"] == "Direct Rec.709 (SDR)");
    CHECK(get.data["hdr"] == false);
    CHECK(get.data["managed"] == false);
    CHECK(get.data["autoDetectLog"] == true);
    CHECK(get.data["modes"].size() == 6); // Premiere's Color Setup presets
    CHECK(get.data["modes"][0]["name"] == "Direct Rec.709 (SDR)");
    CHECK_FALSE(get.data["modes"][0]["description"].get<std::string>().empty());
    CHECK(get.data["inputSpaces"].size() == allInputColorSpaces().size());

    // Set: one undo step, only the given fields change.
    const auto set = c.h.ok("color.management.set", Json{{"mode", "hdr-pq"}, {"peakNits", 4000}});
    CHECK(set.data["mode"] == "hdr-pq");
    CHECK(set.data["hdr"] == true);
    CHECK(set.data["peakNits"] == 4000.0);
    CHECK(set.data["referenceWhiteNits"] == 203.0);
    CHECK(c.h.state()->colorManagement.mode == ColorManagementMode::HdrPq);
    CHECK(c.h.session.undoCount() == 1);
    CHECK(c.h.errorOf("color.management.set", Json{{"mode", "hdr-xx"}}) == "InvalidParams");
    CHECK(c.h.errorOf("color.management.set", Json{{"referenceWhiteNits", 500}}) == "InvalidParams"); // schema bounds
    c.h.ok("color.management.set", Json{{"peakNits", 250}});                                               // above 203: valid
    CHECK_FALSE(c.h.errorOf("color.management.set", Json{{"peakNits", 150}}).empty());                    // below the white
    CHECK_FALSE(c.h.errorOf("color.management.set", Json{{"referenceWhiteNits", 300}}).empty());          // not below the peak

    // A clip's input colour space: auto from the tags, an override, back to auto.
    const ClipId a = c.h.clip("V1@0");
    const auto before = c.h.ok("color.inputSpace.get", Json{{"clip", a.value}});
    CHECK(before.data["clips"][0]["override"].is_null());
    CHECK(before.data["clips"][0]["auto"] == "rec709");
    const auto logged = c.h.ok("color.inputSpace.set", Json{{"clips", Json::array({a.value})}, {"space", "s-log3-cine"}});
    CHECK(logged.data["clips"][0]["effective"] == "s-log3-cine");
    CHECK(logged.warnings.empty()); // an HDR project: the input space applies
    CHECK(c.h.sequence(c.h.state()).findClip(a)->clip().inputColorSpace() == InputColorSpace::SLog3SGamut3Cine);
    CHECK(c.h.errorOf("color.inputSpace.set", Json{{"clip", a.value}, {"space", "red-log"}}) == "InvalidParams");
    const auto back = c.h.ok("color.inputSpace.set", Json{{"clip", a.value}, {"space", "auto"}});
    CHECK(back.data["clips"][0]["override"].is_null());

    // Direct Rec.709 converts the clip (tone-mapped on input): no warning. With colour management disabled the
    // override is kept but says it does nothing there.
    c.h.ok("color.management.set", Json{{"mode", "rec709"}});
    const auto direct = c.h.ok("color.inputSpace.set", Json{{"clip", a.value}, {"space", "v-log"}});
    CHECK(direct.warnings.empty());
    c.h.ok("color.management.set", Json{{"mode", "off"}, {"autoDetectLog", false}});
    CHECK(c.h.state()->colorManagement.mode == ColorManagementMode::Disabled);
    CHECK_FALSE(c.h.state()->colorManagement.autoDetectLog);
    const auto ignored = c.h.ok("color.inputSpace.set", Json{{"clip", a.value}, {"space", "logc4"}});
    CHECK(ignored.warnings.size() == 1);
    const auto wide = c.h.ok("color.management.set", Json{{"mode", "wide-gamut"}});
    CHECK(wide.data["name"] == "Wide Gamut (No Tone Mapping)");
    CHECK(wide.data["managed"] == true);
}

namespace {

/// A face (a skin box at the centre) over a background, per media: A neutral skin on blue, B a greenish skin on orange.
class FaceRenderer final : public ColorServices {
public:
    std::expected<ColorImage, Error> frame(const ProjectState& state, SequenceId sequence, const FrameRequest& request) override {
        const Sequence& seq = *state.findSequence(sequence);
        const Clip& clip = seq.findClip(request.solo)->clip();
        const char letter = state.findMedia(clip.media)->name.front();
        GradeSpec spec;
        bool graded = false;
        for (const EffectInstance& e : clip.effects()) {
            if (e.id == request.stopBefore) {
                break;
            }
            if (e.effect == kGradeEffect && e.enabled) {
                spec = evaluateGrade(e, clip.curveTimeAt(request.time), LutLibrary::shared().resolver(state));
                graded = true;
            }
        }
        ColorImage image;
        image.width = 64;
        image.height = 36;
        image.rgba.resize(64 * 36 * 4);
        for (int y = 0; y < 36; ++y) {
            for (int x = 0; x < 64; ++x) {
                const bool face = x >= 24 && x < 40 && y >= 6 && y < 30;
                Rgb c = letter == 'A' ? (face ? Rgb{0.45, 0.3, 0.22} : Rgb{0.05, 0.1, 0.5})
                                      : (face ? Rgb{0.36, 0.33, 0.19} : Rgb{0.5, 0.25, 0.05});
                if (graded) {
                    c = gradePixel(spec, c);
                }
                const Rgb e = encode709(c);
                float* p = image.rgba.data() + (static_cast<std::size_t>(y) * 64 + x) * 4;
                p[0] = static_cast<float>(e[0]), p[1] = static_cast<float>(e[1]), p[2] = static_cast<float>(e[2]), p[3] = 1.0f;
            }
        }
        return image;
    }
};

/// The face box, or none.
class FakeFaces final : public FaceFinder {
public:
    bool none = false;
    int calls = 0;
    std::expected<std::vector<FaceBox>, Error> faces(const ColorImage&) override {
        ++calls;
        if (none) {
            return std::vector<FaceBox>{};
        }
        return std::vector<FaceBox>{FaceBox{24.0 / 64, 6.0 / 36, 16.0 / 64, 24.0 / 36}};
    }
};

/// How far the clip's skin (graded) is from the reference's, in Oklab.
double skinError(FaceRenderer& r, const ProjectState& state, SequenceId seq, ClipId clip) {
    const Clip& c = state.findSequence(seq)->findClip(clip)->clip();
    const auto image = r.frame(state, seq, FrameRequest{c.start(), clip, {}});
    REQUIRE(image);
    const float* p = image->rgba.data() + (18 * 64 + 32) * 4;
    const Rgb lab = linearToOklab(decode709({p[0], p[1], p[2]}));
    const Rgb ref = linearToOklab({0.45, 0.3, 0.22});
    return std::sqrt(std::pow(lab[0] - ref[0], 2) + std::pow(lab[1] - ref[1], 2) + std::pow(lab[2] - ref[2], 2));
}

} // namespace

TEST_CASE("color.match with faceDetection matches the faces' skin tones first (Premiere's Face Detection, 0.2)",
          "[color][commands][faces]") {
    auto renderer = std::make_shared<FaceRenderer>();
    auto faces = std::make_shared<FakeFaces>();
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{renderer, faces});
    CommandHarness h(R"(
        V1 tgt |AaaaBbbb|
    )", policy, ai::edit::editingCommands());
    const ClipId a = h.clip("V1@0"), b = h.clip("V1@4");
    const Json reference{{"clip", a.value}};

    // Off by default: the whole frame counts, the backgrounds pull the skin off.
    const auto whole = h.ok("color.match", Json{{"clip", b.value}, {"reference", reference}});
    CHECK(faces->calls == 0);
    CHECK_FALSE(whole.data["clips"][0].contains("faces"));
    const double wholeError = skinError(*renderer, *h.state(), h.tl.seq, b);
    REQUIRE(h.session.undo());

    const auto byFaces = h.ok("color.match", Json{{"clip", b.value}, {"reference", reference}, {"faceDetection", true}});
    CHECK(faces->calls == 2); // the reference and the clip
    CHECK(byFaces.data["referenceFaces"] == 1);
    CHECK(byFaces.data["clips"][0]["faces"] == 1);
    const double faceError = skinError(*renderer, *h.state(), h.tl.seq, b);
    INFO("skin error: whole frame " << wholeError << ", face detection " << faceError);
    CHECK(faceError < wholeError * 0.6);
    CHECK(h.session.undoCount() == 1); // one undo step

    // No face on a picture: a warning, the whole picture counts.
    REQUIRE(h.session.undo());
    faces->none = true;
    const auto noFace = h.ok("color.matchScene", Json{{"reference", a.value}, {"clips", Json::array({b.value})}, {"faceDetection", true}});
    CHECK(noFace.data["referenceFaces"] == 0);
    REQUIRE_FALSE(noFace.warnings.empty());
    CHECK(noFace.warnings.front().find("no face") != std::string::npos);

    // Without a detector the host cannot do it: refused with the reason, nothing changed.
    CommandHarness bare(R"(
        V1 tgt |AaaaBbbb|
    )", command::EnginePolicy{command::CapabilitySet::all(), 1024, renderer}, ai::edit::editingCommands());
    const auto refused = bare.run(Json{{"command", "color.match"},
                                       {"params", Json{{"clip", bare.clip("V1@4").value}, {"reference", Json{{"clip", bare.clip("V1@0").value}}},
                                                       {"faceDetection", true}}}});
    REQUIRE(refused.error);
    CHECK(refused.error->message.find("Face Detection needs the face detector") != std::string::npos);
    CHECK(bare.session.undoCount() == 0);
}

// ---- 0.4: color.grade.copy (Apply Grade) ------------------------------------------------------------------------------

namespace {

const Clip& clipIn(const ColorHarness& c, ClipId id) { return c.h.state()->findSequence(c.h.tl.seq)->findClip(id)->clip(); }

std::vector<const EffectInstance*> gradesOf(const Clip& clip) {
    std::vector<const EffectInstance*> out;
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == kGradeEffect) {
            out.push_back(&e);
        }
    }
    return out;
}

/// A1's grade: exposure 0.5, a master curve, a look and an animated saturation (100 -> 140 over its first 3 frames).
void gradeSource(ColorHarness& c, ClipId a1) {
    c.h.ok("color.grade.set", Json{{"clip", a1.value},
                                   {"params", Json{{"basic.exposure", 0.5}}},
                                   {"curves", Json{{"master", Json::array({Json::array({0, 0}), Json::array({0.5, 0.62}), Json::array({1, 1})})}}},
                                   {"look", "builtin:teal-orange"}});
    const ProjectSnapshot snap = c.h.state();
    const Clip& clip = snap->findSequence(c.h.tl.seq)->findClip(a1)->clip();
    const EffectInstanceId grade = gradeOf(clip)->id;
    Curve saturation;
    saturation.keys.push_back(Keyframe{clip.curveTimeAt(clip.start()), 100.0, Interpolation::Linear, {}, {}});
    saturation.keys.push_back(Keyframe{clip.curveTimeAt(clip.start() + f(3)), 140.0, Interpolation::Linear, {}, {}});
    const SequenceId seq = c.h.tl.seq;
    REQUIRE(c.h.session.apply("Saturation", [&](const ProjectState& s) {
        return edit::setParameter(s, seq, a1, edit::ParamRef::ofEffect(grade, "basic.saturation"), ParamValue(saturation));
    }));
}

/// The two clips' grades look the same at the same times from their starts (keys included).
void checkSameGrade(const Clip& from, const EffectInstance& a, const Clip& to, const EffectInstance& b) {
    CHECK(a.enabled == b.enabled);
    CHECK(a.data == b.data);
    for (int frame = 0; frame < 4; ++frame) {
        INFO("frame " << frame);
        const GradeParams pa = gradeParamsAt(a, from.curveTimeAt(from.start() + f(frame)));
        const GradeParams pb = gradeParamsAt(b, to.curveTimeAt(to.start() + f(frame)));
        CHECK(pb.exposure == Approx(pa.exposure));
        CHECK(pb.saturation == Approx(pa.saturation));
        CHECK(pb.contrast == Approx(pa.contrast));
    }
    const ParamValue* sa = a.find("basic.saturation");
    const ParamValue* sb = b.find("basic.saturation");
    REQUIRE(sa);
    REQUIRE(sb);
    REQUIRE(sa->curve());
    REQUIRE(sb->curve());
    CHECK(sb->curve()->keys.size() == sa->curve()->keys.size());
}

} // namespace

TEST_CASE("color.grade.copy replaces the targets grade in one undo step", "[color][commands][gradecopy]") {
    // A1, A2 and A3 are clips of media A (A2 already graded), B1 of media B.
    ColorHarness c(R"(
        V1 tgt |AaaaA4aaBbbbA8aa|
    )");
    const ClipId a1 = c.h.clip("V1@0");
    const ClipId a2 = c.h.clip("V1@4");
    const ClipId b1 = c.h.clip("V1@8");
    c.h.ok("color.grade.set", Json{{"clip", a2.value}, {"params", Json{{"basic.exposure", -1.0}, {"basic.contrast", 30}}}});
    const EffectInstanceId a2Grade = c.grade("V1@4")->id;
    gradeSource(c, a1);

    const auto before = c.h.session.undoCount();
    const auto copied = c.h.ok("color.grade.copy", Json{{"from", a1.value}, {"clips", Json::array({a2.value, b1.value})}});
    CHECK(c.h.session.undoCount() == before + 1);
    CHECK(copied.data["changed"] == 2);
    CHECK(copied.data["from"] == a1.value);
    CHECK(copied.data["scope"] == "clips");
    REQUIRE(copied.data["clips"].size() == 2);
    CHECK(copied.data["clips"][0]["result"] == "replaced");
    CHECK(copied.data["clips"][1]["result"] == "added");

    // Each target has exactly one grade equal to A1's, keys included; A2 keeps its instance (id and place).
    const ProjectSnapshot snap = c.h.state();
    const Sequence& seq = *snap->findSequence(c.h.tl.seq);
    const Clip& from = seq.findClip(a1)->clip();
    for (const ClipId id : {a2, b1}) {
        INFO(id.value);
        const Clip& to = seq.findClip(id)->clip();
        const auto grades = gradesOf(to);
        REQUIRE(grades.size() == 1);
        checkSameGrade(from, *gradeOf(from), to, *grades.front());
    }
    CHECK(c.grade("V1@4")->id == a2Grade);
    CHECK(c.params("V1@4").contrast == Approx(0.0)); // replaced, not merged
    CHECK_FALSE(c.grade("V1@12"));                  // A3 was not named

    // One undo gives A2 its old grade back and takes B1's away.
    REQUIRE(c.h.session.undo());
    CHECK(c.params("V1@4").exposure == Approx(-1.0));
    CHECK(c.params("V1@4").contrast == Approx(30.0));
    CHECK_FALSE(c.grade("V1@8"));

    // A dry run changes nothing.
    const auto revision = c.h.session.revision();
    const auto dry = c.h.run(Json{{"command", "color.grade.copy"}, {"dryRun", true},
                                  {"params", Json{{"from", a1.value}, {"clips", Json::array({a2.value})}}}});
    REQUIRE(dry.ok());
    CHECK(c.h.session.revision() == revision);
}

TEST_CASE("color.grade.copy scope media reaches every clip of the source and skips locked tracks", "[color][commands][gradecopy]") {
    ColorHarness c(R"(
        V2 lock |....A2aa........|
        V1 tgt  |AaaaA4aaBbbbA8aa|
    )");
    const ClipId a1 = c.h.clip("V1@0");
    const ClipId a2 = c.h.clip("V1@4");
    const ClipId b1 = c.h.clip("V1@8");
    const ClipId a3 = c.h.clip("V1@12");
    const ClipId locked = c.h.clip("V2@4");
    gradeSource(c, a1);

    const auto result = c.h.ok("color.grade.copy", Json{{"from", a1.value}, {"scope", "media"}});
    CHECK(result.data["scope"] == "media");
    CHECK(result.data["changed"] == 2);
    std::map<std::uint64_t, std::string> outcome;
    for (const Json& r : result.data["clips"]) {
        outcome[r["clip"].get<std::uint64_t>()] = r["result"].get<std::string>();
    }
    CHECK(outcome.size() == 3); // A2, A3 and the A clip on the locked V2; never B1 or A1 itself
    CHECK(outcome[a2.value] == "added");
    CHECK(outcome[a3.value] == "added");
    CHECK(outcome[locked.value] == "skipped");
    CHECK_FALSE(outcome.contains(b1.value));
    CHECK_FALSE(outcome.contains(a1.value));
    CHECK(c.grade("V1@4"));
    CHECK(c.grade("V1@12"));
    CHECK_FALSE(c.grade("V1@8"));
    CHECK_FALSE(c.grade("V2@4"));
    // A source no other clip shows: refused, nothing changed.
    c.h.ok("color.grade.set", Json{{"clip", b1.value}, {"params", Json{{"basic.exposure", 0.2}}}});
    const auto undos = c.h.session.undoCount();
    CHECK(c.h.errorOf("color.grade.copy", Json{{"from", b1.value}, {"scope", "media"}}) == "EditRejected");
    CHECK(c.h.session.undoCount() == undos);
}

TEST_CASE("color.grade.copy refuses a source without a grade and keeps the target masks", "[color][commands][gradecopy]") {
    ColorHarness c(R"(
        V1 tgt |AaaaA4aaBbbb|
        A1 tgt |AaaaA4aaBbbb|
    )");
    const ClipId a1 = c.h.clip("V1@0");
    const ClipId a2 = c.h.clip("V1@4");
    const ClipId b1 = c.h.clip("V1@8");
    const ClipId audio = c.h.clip("A1@4");
    // No grade to copy.
    const auto undos = c.h.session.undoCount();
    CHECK(c.h.errorOf("color.grade.copy", Json{{"from", b1.value}, {"clips", Json::array({a2.value})}}) == "EditRejected");
    CHECK(c.h.session.undoCount() == undos);

    // A2's grade has a local mask (drawn for that shot): it stays when A1's grade replaces the grade.
    c.h.ok("color.grade.set", Json{{"clip", a1.value}, {"params", Json{{"basic.exposure", 0.3}}}});
    CHECK(c.h.errorOf("color.grade.copy", Json{{"from", a1.value}}) == "InvalidParams"); // no clips, no scope media
    CHECK(c.h.errorOf("color.grade.copy", Json{{"from", a1.value}, {"scope", "track"}}) == "InvalidParams");
    c.h.ok("color.grade.set", Json{{"clip", a2.value}, {"params", Json{{"basic.exposure", -0.4}}}});
    c.h.ok("mask.add", Json{{"clip", a2.value}, {"effect", c.grade("V1@4")->id.value}, {"shape", "ellipse"}});
    REQUIRE(c.grade("V1@4")->masks.size() == 1);
    c.h.ok("color.grade.copy", Json{{"from", a1.value}, {"clips", Json::array({a2.value})}});
    CHECK(c.params("V1@4").exposure == Approx(0.3));
    CHECK(c.grade("V1@4")->masks.size() == 1);
    // The same again changes nothing: refused, no undo step.
    const auto same = c.h.session.undoCount();
    CHECK(c.h.errorOf("color.grade.copy", Json{{"from", a1.value}, {"clips", Json::array({a2.value})}}) == "EditRejected");
    CHECK(c.h.session.undoCount() == same);
    // Audio clips and the source itself are skipped; with nothing else to change, refused.
    const auto skipped = c.h.run(Json{{"command", "color.grade.copy"},
                                      {"params", Json{{"from", a1.value}, {"clips", Json::array({audio.value, a1.value})}}}});
    REQUIRE_FALSE(skipped.ok());
    CHECK(skipped.error->code == command::ErrorCode::EditRejected);

    // replace: false stacks another grade, as Paste Attributes does.
    c.h.ok("color.grade.copy", Json{{"from", a1.value}, {"clips", Json::array({b1.value})}});
    CHECK(gradesOf(clipIn(c, b1)).size() == 1);
    const auto stacked = c.h.ok("color.grade.copy", Json{{"from", a1.value}, {"clips", Json::array({b1.value})}, {"replace", false}});
    CHECK(stacked.data["clips"][0]["result"] == "added");
    CHECK(gradesOf(clipIn(c, b1)).size() == 2);
}
