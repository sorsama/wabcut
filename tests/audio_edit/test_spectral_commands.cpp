// 0.7 (card 047): the Spectral Repair commands through the CommandEngine: each edit is added to the clip's Spectral Repair effect
// (made on the first edit, at the start of the stack) in one undo step with a name that says what it did, the options (FFT size,
// listen, bypass), removing one edit or all of them (the effect goes with the last), the refusals, the list, and the edits
// surviving a save and load of the project with undo and redo.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioCommands.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/SpectralRepair.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A2 |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
)";

struct Fixture {
    Fixture()
        : tl(kPicture), h(tl.linkMatching(), command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<AudioServices>(nullptr, nullptr)},
                          ai::edit::editingCommands()) {}

    const Sequence& seq() const { return h.sequence(h.state()); }
    ClipId dialogue() const { return h.clip("A1@0"); }
    const Clip& clipOf(ClipId id) const { return seq().findClip(id)->clip(); }
    const EffectInstance* repairOf(ClipId id) const {
        for (const EffectInstance& e : clipOf(id).effects()) {
            if (e.effect == kSpectralRepairEffect) {
                return &e;
            }
        }
        return nullptr;
    }
    std::vector<SpectralEdit> edits(ClipId id) const {
        const EffectInstance* e = repairOf(id);
        return e != nullptr ? unpackSpectralEdits(*e) : std::vector<SpectralEdit>{};
    }

    Timeline tl;
    CommandHarness h;
};

Json rectParams(ClipId clip) {
    return Json{{"clip", clip.value}, {"start", 1.5}, {"end", 2.25}, {"low", 700}, {"high", 3200}};
}

} // namespace

TEST_CASE("audio.spectralAttenuate makes the clip's Spectral Repair on the first edit, in one undo step", "[audio][edit][spectral]") {
    Fixture f;
    const ClipId clip = f.dialogue();
    CHECK(f.repairOf(clip) == nullptr);
    const std::size_t undo = f.h.session.undoCount();
    Json params = rectParams(clip);
    params["gainDb"] = -30;
    const auto r = f.h.ok("audio.spectralAttenuate", params);
    CHECK(f.h.session.undoCount() == undo + 1);
    const EffectInstance* repair = f.repairOf(clip);
    REQUIRE(repair != nullptr);
    CHECK(repair->enabled);
    CHECK(f.clipOf(clip).effects().front().effect == kSpectralRepairEffect); // at the start of the stack
    REQUIRE(r.createdIds.size() == 1);
    CHECK(r.createdIds[0] == repair->id.value);
    CHECK(r.data["edits"] == 1);
    const std::vector<SpectralEdit> edits = f.edits(clip);
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].kind == SpectralKind::Attenuate);
    CHECK(edits[0].shape == SpectralShape::Rectangle);
    CHECK(edits[0].gainDb == Approx(-30.0));
    CHECK(edits[0].start == Approx(1.5));
    CHECK(edits[0].high == Approx(3200.0));
    // Keyframe-free: the effect has no animated parameter, and the name of the undo step says what was done.
    for (const EffectParam& p : repair->params) {
        CHECK_FALSE(p.value.isCurve());
    }
    CHECK(f.h.session.undoLabel() == "Attenuate Spectral Selection");

    // A second edit goes into the same effect.
    const auto second = f.h.ok("audio.spectralAttenuate", rectParams(clip));
    CHECK(second.createdIds.empty());
    CHECK(f.edits(clip).size() == 2);
    CHECK(f.clipOf(clip).effects().size() == 1);

    // Undo and redo.
    REQUIRE(f.h.session.undo());
    CHECK(f.edits(clip).size() == 1);
    REQUIRE(f.h.session.undo());
    CHECK(f.repairOf(clip) == nullptr);
    REQUIRE(f.h.session.redo());
    CHECK(f.edits(clip).size() == 1);
}

TEST_CASE("Heal, noise floor and tone edits are added with their shapes and parameters", "[audio][edit][spectral]") {
    Fixture f;
    const ClipId clip = f.dialogue();
    const auto points = Json::array({Json{{"time", 1.0}, {"hz", 400}}, Json{{"time", 2.0}, {"hz", 400}}, Json{{"time", 1.5}, {"hz", 2500}}});
    f.h.ok("audio.spectralHeal", {{"clip", clip.value}, {"shape", "lasso"}, {"points", points}, {"amount", 80}, {"logFrequency", false}});
    f.h.ok("audio.spectralNoiseFloor", {{"clip", clip.value}, {"shape", "brush"}, {"points", Json::array({Json{{"time", 3.0}, {"hz", 1000}}})},
                                        {"radiusSeconds", 0.2}, {"radiusOctaves", 0.5}});
    const auto tone = f.h.ok("audio.spectralRemoveTone", {{"clip", clip.value}, {"start", 0}, {"end", 8}, {"hz", 50}, {"harmonics", 4}, {"widthHz", 8}});
    CHECK(f.h.session.undoLabel() == "Remove Tone");
    CHECK(tone.data["edits"] == 3);
    const std::vector<SpectralEdit> edits = f.edits(clip);
    REQUIRE(edits.size() == 3);
    CHECK(edits[0].kind == SpectralKind::Heal);
    CHECK(edits[0].shape == SpectralShape::Lasso);
    CHECK(edits[0].path.size() == 3);
    CHECK(edits[0].amount == Approx(80.0));
    CHECK_FALSE(edits[0].logFrequency);
    CHECK(edits[1].kind == SpectralKind::NoiseFloor);
    CHECK(edits[1].shape == SpectralShape::Brush);
    CHECK(edits[1].radiusSeconds == Approx(0.2));
    CHECK(edits[1].radiusOctaves == Approx(0.5));
    CHECK(edits[2].kind == SpectralKind::Tone);
    CHECK(edits[2].harmonics == 4);
    CHECK(edits[2].widthHz == Approx(8.0));
    CHECK(edits[2].gainDb == Approx(-60.0)); // the default depth
    REQUIRE(edits[2].path.size() == 1);
    CHECK(edits[2].path[0].hz == Approx(50.0));
    // A video clip stands for its linked audio.
    const ClipId video = f.h.clip("V1@0");
    const auto viaVideo = f.h.ok("audio.spectralAttenuate", rectParams(video));
    CHECK(viaVideo.data["clip"].get<std::uint64_t>() == clip.value);
    CHECK(f.edits(clip).size() == 4);
}

TEST_CASE("A long heal warns that it only looks about a second around", "[audio][edit][spectral]") {
    Fixture f;
    const ClipId clip = f.dialogue();
    const auto short1 = f.h.ok("audio.spectralHeal", {{"clip", clip.value}, {"start", 1.0}, {"end", 1.5}, {"low", 200}, {"high", 2000}});
    CHECK(short1.warnings.empty());
    const auto long1 = f.h.ok("audio.spectralHeal", {{"clip", clip.value}, {"start", 1.0}, {"end", 4.0}, {"low", 200}, {"high", 2000}});
    CHECK_FALSE(long1.warnings.empty());
}

TEST_CASE("The spectral commands refuse what makes no sense and change nothing", "[audio][edit][spectral]") {
    Fixture f;
    const ClipId clip = f.dialogue();
    const std::size_t undo = f.h.session.undoCount();
    CHECK(f.h.errorOf("audio.spectralAttenuate", {{"clip", clip.value}, {"start", 2.0}, {"end", 1.0}, {"low", 700}, {"high", 3200}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralAttenuate", {{"clip", clip.value}, {"start", 1.0}, {"end", 2.0}, {"low", 3200}, {"high", 700}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralAttenuate", {{"clip", clip.value}, {"shape", "star"}, {"start", 1.0}, {"end", 2.0}, {"low", 700}, {"high", 3200}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralAttenuate", {{"clip", clip.value}, {"shape", "lasso"}, {"points", Json::array({Json{{"time", 1}, {"hz", 400}}})}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralAttenuate", {{"clip", clip.value}, {"start", 1.0}, {"end", 2.0}, {"low", 700}, {"high", 3200}, {"gainDb", 6}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralRemoveTone", {{"clip", clip.value}, {"start", 0}, {"end", 8}}) == "InvalidParams"); // no frequency
    CHECK(f.h.errorOf("audio.spectralRemoveTone", {{"clip", clip.value}, {"start", 0}, {"end", 8}, {"hz", 50}, {"harmonics", 40}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralHeal", {{"start", 1.0}, {"end", 2.0}, {"low", 700}, {"high", 3200}}) == "InvalidParams"); // no clip
    CHECK(f.h.errorOf("audio.spectralRepairRemove", {{"clip", clip.value}, {"index", 0}}) == "EditRejected"); // nothing to remove yet
    CHECK(f.h.errorOf("audio.spectralRepairOptions", {{"clip", clip.value}, {"listen", "selection"}}) == "EditRejected");
    CHECK(f.h.session.undoCount() == undo);
    CHECK(f.repairOf(clip) == nullptr);

    f.h.ok("audio.spectralAttenuate", rectParams(clip));
    CHECK(f.h.errorOf("audio.spectralRepairRemove", {{"clip", clip.value}, {"index", 5}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralRepairRemove", {{"clip", clip.value}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralRepairOptions", {{"clip", clip.value}, {"fftSize", 1000}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralRepairOptions", {{"clip", clip.value}, {"listen", "everything"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.spectralRepairOptions", {{"clip", clip.value}}) == "InvalidParams");
    CHECK(f.edits(clip).size() == 1);
}

TEST_CASE("audio.spectralRepairRemove takes off one edit or all of them and the effect goes with the last", "[audio][edit][spectral]") {
    Fixture f;
    const ClipId clip = f.dialogue();
    for (int i = 0; i < 3; ++i) {
        Json p = rectParams(clip);
        p["start"] = 1.0 + i;
        p["end"] = 1.5 + i;
        f.h.ok("audio.spectralAttenuate", p);
    }
    const std::size_t undo = f.h.session.undoCount();
    const auto one = f.h.ok("audio.spectralRepairRemove", {{"clip", clip.value}, {"index", 1}});
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(f.h.session.undoLabel() == "Remove Spectral Edit");
    CHECK(one.data["removed"] == 1);
    const std::vector<SpectralEdit> left = f.edits(clip);
    REQUIRE(left.size() == 2);
    CHECK(left[0].start == Approx(1.0));
    CHECK(left[1].start == Approx(3.0)); // the middle one went
    f.h.ok("audio.spectralRepairRemove", {{"clip", clip.value}, {"index", 0}});
    f.h.ok("audio.spectralRepairRemove", {{"clip", clip.value}, {"index", 0}});
    CHECK(f.repairOf(clip) == nullptr);
    // All at once, and undone as one.
    f.h.ok("audio.spectralAttenuate", rectParams(clip));
    f.h.ok("audio.spectralHeal", rectParams(clip));
    const std::size_t before = f.h.session.undoCount();
    f.h.ok("audio.spectralRepairRemove", {{"clip", clip.value}, {"all", true}});
    CHECK(f.h.session.undoCount() == before + 1);
    CHECK(f.repairOf(clip) == nullptr);
    REQUIRE(f.h.session.undo());
    CHECK(f.edits(clip).size() == 2);
}

TEST_CASE("audio.spectralRepairOptions sets the FFT size the listen mode and the bypass in one undo step", "[audio][edit][spectral]") {
    Fixture f;
    const ClipId clip = f.dialogue();
    f.h.ok("audio.spectralHeal", rectParams(clip));
    const std::size_t undo = f.h.session.undoCount();
    f.h.ok("audio.spectralRepairOptions", {{"clip", clip.value}, {"fftSize", 4096}, {"listen", "removed"}, {"enabled", false}});
    CHECK(f.h.session.undoCount() == undo + 1);
    const EffectInstance* repair = f.repairOf(clip);
    REQUIRE(repair != nullptr);
    CHECK_FALSE(repair->enabled);
    CHECK(repair->find("fftSize")->constant() == 2.0);
    CHECK(repair->find("listen")->constant() == 2.0);
    CHECK(f.edits(clip).size() == 1); // the edits are kept

    const auto list = f.h.ok("audio.spectralRepairEdits", {{"clip", clip.value}});
    CHECK(list.data["options"]["fftSize"] == 4096);
    CHECK(list.data["options"]["listen"] == "removed");
    CHECK(list.data["options"]["enabled"] == false);
    REQUIRE(list.data["edits"].size() == 1);
    CHECK(list.data["edits"][0]["kind"] == "heal");
    CHECK(list.data["edits"][0]["description"].get<std::string>().find("Heal") == 0);
    // Listing a clip without a repair is empty, not an error.
    const auto none = f.h.ok("audio.spectralRepairEdits", {{"clip", f.h.clip("A2@0").value}});
    CHECK(none.data["edits"].empty());
}

TEST_CASE("The spectral edits survive saving and loading the project", "[audio][edit][spectral]") {
    Fixture f;
    const ClipId clip = f.dialogue();
    f.h.ok("audio.spectralAttenuate", rectParams(clip));
    f.h.ok("audio.spectralHeal", {{"clip", clip.value},
                                  {"shape", "lasso"},
                                  {"points", Json::array({Json{{"time", 1.0}, {"hz", 400}}, Json{{"time", 2.0}, {"hz", 400}}, Json{{"time", 1.5}, {"hz", 2500}}})}});
    f.h.ok("audio.spectralRemoveTone", {{"clip", clip.value}, {"start", 0}, {"end", 8}, {"hz", 60}, {"harmonics", 3}});
    f.h.ok("audio.spectralRepairOptions", {{"clip", clip.value}, {"fftSize", 1024}});
    const EffectInstance before = *f.repairOf(clip);
    const auto json = io::toJson(*f.h.state());
    REQUIRE(json.has_value());
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded.has_value());
    const Clip& again = loaded->findSequence(f.h.tl.seq)->findClip(clip)->clip();
    const EffectInstance* repair = nullptr;
    for (const EffectInstance& e : again.effects()) {
        repair = e.effect == kSpectralRepairEffect ? &e : repair;
    }
    REQUIRE(repair != nullptr);
    CHECK(*repair == before);
    CHECK(unpackSpectralEdits(*repair) == f.edits(clip));
    CHECK(repair->find("fftSize")->constant() == 0.0);
}
