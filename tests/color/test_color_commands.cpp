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
#include <wabcut/model/Nesting.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

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
