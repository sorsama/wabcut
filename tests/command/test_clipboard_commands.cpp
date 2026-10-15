// C.11 through the engine: clipboard.copy / cut / paste / get (insert and overwrite, exported documents pasted into
// another project), clip.pasteAttributes / removeAttributes with dry-run summaries, and the library commands.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaBbbbCccc........|
    A1 tgt |AaaaBbbbCccc........|
)";

Json ids(const std::vector<ClipId>& clips) {
    Json out = Json::array();
    for (const ClipId c : clips) {
        out.push_back(c.value);
    }
    return out;
}

} // namespace

TEST_CASE("clipboard: copy, cut and paste through the engine", "[command][clipboard]") {
    CommandHarness h(kPicture);
    h.tl.linkMatching();
    h.session.reset(h.tl.state);
    CHECK(h.ok("clipboard.get").data["empty"] == true);
    CHECK(h.errorOf("clipboard.paste", Json{{"at", tc(12)}}) == "EditRejected"); // nothing copied yet

    // Copy B (its audio partner follows); the project does not change.
    const auto copied = h.ok("clipboard.copy", Json{{"clips", ids({h.clip("V1@4")})}});
    CHECK(copied.data["clips"] == 2);
    CHECK(copied.data["duration"] == f(4).value);
    CHECK(h.session.undoCount() == 0);
    CHECK(h.ok("clipboard.get").data["clips"] == 2);

    // Ctrl+Shift+V at 0, then Ctrl+V at 16: one undo step each.
    const auto inserted = h.ok("clipboard.paste", Json{{"at", tc(0)}, {"mode", "insert"}});
    CHECK(inserted.data["pasted"].size() == 2);
    CHECK(inserted.data["playhead"]["ticks"] == f(4).value);
    CHECK_SESSION(h, R"(
        V1 |BbbbAaaaBbbbCccc....|
        A1 |BbbbAaaaBbbbCccc....|
    )");
    h.ok("clipboard.paste", Json{{"at", tc(16)}});
    CHECK_SESSION(h, R"(
        V1 |BbbbAaaaBbbbCcccBbbb|
        A1 |BbbbAaaaBbbbCcccBbbb|
    )");
    CHECK(h.session.undoCount() == 2);
    h.session.undo();
    h.session.undo();
    CHECK_SESSION(h, kPicture);

    // Cut leaves a gap and fills the clipboard.
    h.ok("clipboard.cut", Json{{"clips", ids({h.clip("V1@8")})}});
    CHECK_SESSION(h, R"(
        V1 |AaaaBbbb............|
        A1 |AaaaBbbb............|
    )");
    h.ok("clipboard.paste", Json{{"at", tc(0)}});
    CHECK_SESSION(h, R"(
        V1 |CcccBbbb............|
        A1 |CcccBbbb............|
    )");
}

TEST_CASE("clipboard: an exported document pastes into another project", "[command][clipboard]") {
    CommandHarness from(kPicture);
    const auto exported =
        from.ok("clipboard.copy", Json{{"clips", ids({from.clip("V1@0"), from.clip("V1@4")})}, {"export", true}});
    const std::string document = exported.data["document"].get<std::string>();
    REQUIRE_FALSE(document.empty());

    // Another project where B is another file: A is the same file (by id), B is added.
    Timeline other(R"(
        V1 tgt |Zzzz........|
        A1 tgt |Zzzz........|
    )");
    ProjectState target = other.state;
    for (MediaPtr& m : target.media) {
        if (m->name == "B") {
            auto elsewhere = std::make_shared<MediaItem>(*m);
            elsewhere->locator.path = "D:/other/B.mov";
            m = std::move(elsewhere);
        }
    }
    other.state = target;
    CommandHarness to(std::move(other));
    const auto pasted = to.ok("clipboard.paste", Json{{"at", tc(4)}, {"document", document}});
    CHECK(pasted.data["mediaAdded"].size() == 1);
    REQUIRE(pasted.data["notes"].size() == 1);
    CHECK(pasted.data["notes"][0].get<std::string>().find("'B': added to the project") != std::string::npos);
    CHECK(to.state()->media.size() == target.media.size() + 1);
    // A damaged document is refused, not trusted.
    CHECK(to.errorOf("clipboard.paste", Json{{"at", tc(0)}, {"document", "{\"format\": \"wabcut-clipboard\"}"}}) ==
          "EditRejected");
}

TEST_CASE("clip.pasteAttributes and clip.removeAttributes, with dry runs", "[command][clipboard][attributes]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
        A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId a = tl.clip("V1@0");
    s = ok(edit::setParameter(s, tl.seq, a, edit::ParamRef::of(ParamId::Opacity), ParamValue(0.25))).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.blur.gaussian")).state;
    s = ok(edit::setParameter(s, tl.seq, tl.clip("A1@0"), edit::ParamRef::of(ParamId::VolumeDb), ParamValue(-6.0))).state;
    tl.state = s;
    CommandHarness h(std::move(tl));

    // Copy A (video and audio), then paste opacity, the blur and the volume onto B and C (video and audio).
    h.ok("clipboard.copy", Json{{"clips", ids({h.clip("V1@0")})}});
    const Json params{{"clips", ids({h.clip("V1@10"), h.clip("V1@20"), h.clip("A1@10")})},
                      {"attributes", Json{{"opacity", true}, {"volume", true}, {"effects", Json::array({0})}}}};
    // A dry run says what would happen and changes nothing.
    const auto preview = h.run(Json{{"command", "clip.pasteAttributes"}, {"params", params}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.data["changes"].size() == 3);
    CHECK(h.session.undoCount() == 0);
    const auto pasted = h.ok("clip.pasteAttributes", params);
    CHECK(h.session.undoCount() == 1); // one undo step for video and audio targets alike
    const Sequence& seq = h.sequence(h.state());
    const Clip& b = seq.findClip(h.clip("V1@10"))->clip();
    CHECK(b.properties()[ParamId::Opacity] == ParamValue(0.25));
    REQUIRE(b.effects().size() == 1);
    CHECK(b.effects()[0].effect == "wabcut.blur.gaussian");
    CHECK(seq.findClip(h.clip("A1@10"))->clip().properties()[ParamId::VolumeDb] == ParamValue(-6.0));
    bool sawAudio = false;
    for (const Json& change : pasted.data["changes"]) {
        if (change["clip"] == h.clip("A1@10").value) {
            sawAudio = true;
            CHECK(change["applied"] == Json::array({"Volume"}));
        }
    }
    CHECK(sawAudio);

    // Remove Attributes: opacity back to 1, every effect gone.
    h.ok("clip.removeAttributes", Json{{"clips", ids({h.clip("V1@10")})},
                                      {"attributes", Json{{"opacity", true}, {"allEffects", true}}}});
    const Clip& cleared = h.sequence(h.state()).findClip(h.clip("V1@10"))->clip();
    CHECK(cleared.properties()[ParamId::Opacity] == ClipProperties()[ParamId::Opacity]);
    CHECK(cleared.effects().empty());
    // From a clip of the sequence instead of the clipboard.
    h.ok("clip.pasteAttributes", Json{{"clips", ids({h.clip("V1@10")})}, {"source", h.clip("V1@0").value},
                                     {"attributes", Json{{"opacity", true}}}});
    CHECK(h.sequence(h.state()).findClip(h.clip("V1@10"))->clip().properties()[ParamId::Opacity] == ParamValue(0.25));
    CHECK(h.errorOf("clip.pasteAttributes", Json{{"clips", ids({h.clip("V1@10")})}, {"attributes", Json::object()}}) ==
          "InvalidParams");
}

TEST_CASE("library commands: bins, media attributes and subclips", "[command][library]") {
    CommandHarness h(kPicture);
    const auto bin = h.ok("bin.create", Json{{"name", "Interviews"}});
    const std::uint64_t interviews = bin.data["bin"];
    const std::uint64_t selects = h.ok("bin.create", Json{{"name", "Selects"}, {"parent", interviews}}).data["bin"];
    h.ok("bin.rename", Json{{"bin", selects}, {"name", "Best takes"}});
    CHECK(h.state()->findBin(BinId{selects})->name == "Best takes");
    const MediaId a = h.tl.media('A');
    h.ok("bin.move", Json{{"target", selects}, {"media", Json::array({a.value})}});
    CHECK(h.state()->findMedia(a)->bin == BinId{selects});
    CHECK(h.errorOf("bin.move", Json{{"target", selects}, {"bins", Json::array({interviews})}}) == "EditRejected");

    h.ok("media.setAttributes", Json{{"media", Json::array({a.value})}, {"colorIndex", 4}, {"comment", "Best take"},
                                     {"posterFrame", "00:00:00:12"}});
    const MediaItem& item = *h.state()->findMedia(a);
    CHECK(item.colorIndex == 4);
    CHECK(item.comment == "Best take");
    REQUIRE(item.posterFrame);
    CHECK(*item.posterFrame == f(12));

    const auto sub = h.ok("media.makeSubclip", Json{{"media", a.value}, {"in", "00:00:02:00"}, {"out", "00:00:05:00"},
                                                    {"name", "Laugh"}, {"bin", interviews}});
    const MediaId laugh{sub.data["media"].get<std::uint64_t>()};
    CHECK(h.state()->findMedia(laugh)->name == "Laugh");
    h.ok("media.setSubclip", Json{{"media", laugh.value}, {"in", "00:00:01:00"}, {"out", "00:00:06:00"}});
    CHECK(h.errorOf("media.makeSubclip", Json{{"media", a.value}, {"in", "00:00:05:00"}, {"out", "00:00:02:00"},
                                              {"name", "Backwards"}}) == "InvalidParams");

    // Deleting a bin deletes what is in it; refused while its media is used by a sequence that stays.
    CHECK(h.errorOf("bin.remove", Json{{"bins", Json::array({selects})}}) == "EditRejected"); // A is on the timeline
    h.ok("bin.move", Json{{"media", Json::array({a.value})}});
    h.ok("bin.remove", Json{{"bins", Json::array({interviews})}});
    CHECK_FALSE(h.state()->findBin(BinId{interviews}));
    CHECK_FALSE(h.state()->findMedia(laugh)); // the subclip was in it
    CHECK(h.session.undoCount() == 9);
}

// Review wave 5, S5-P2-12: clips copied from a no-cloud project stayed on the app's one clipboard after another project
// was opened, where the cloud assistant could read them (clipboard.get with export) or paste and then read them.
TEST_CASE("clipboard: clips from a no-cloud project stay out of a cloud caller's reach", "[command][clipboard][privacy]") {
    auto board = std::make_shared<command::ClipboardServices>();
    const command::EnginePolicy user{command::CapabilitySet::all(), 1024, board};
    const command::EnginePolicy assistant{command::CapabilitySet::all(), 1024, board->forCloudCaller()};
    Timeline locked(kPicture);
    locked.state.aiNoCloud = true;
    CommandHarness source(std::move(locked), user);
    source.ok("clipboard.copy", Json{{"clips", ids({source.clip("V1@4")})}});
    const auto exported = source.ok("clipboard.get", Json{{"export", true}});

    // The assistant of a cloud-enabled project opened afterwards: refused.
    CommandHarness cloud(kPicture, assistant);
    CHECK(cloud.errorOf("clipboard.get", Json{{"export", true}}) == "PermissionDenied");
    CHECK(cloud.errorOf("clipboard.paste", Json{{"at", tc(12)}}) == "PermissionDenied");
    // In a no-cloud project nothing it reads can reach a cloud model: allowed.
    Timeline alsoLocked(kPicture);
    alsoLocked.state.aiNoCloud = true;
    CommandHarness local(std::move(alsoLocked), assistant);
    CHECK(local.ok("clipboard.get").data["clips"].get<int>() >= 1);

    // The user pastes into a cloud-enabled project: allowed, and that project is now no-cloud.
    CommandHarness destination(kPicture, user);
    REQUIRE_FALSE(destination.state()->aiNoCloud);
    destination.ok("clipboard.paste", Json{{"at", tc(12)}});
    CHECK(destination.state()->aiNoCloud);
    // An exported document keeps the mark.
    CommandHarness other(kPicture);
    other.ok("clipboard.paste", Json{{"at", tc(12)}, {"document", exported.data["document"]}});
    CHECK(other.state()->aiNoCloud);
}

TEST_CASE("clipboard: a dry run or a failed batch leaves the clipboard as it was (E5-P2-7)", "[command][clipboard]") {
    CommandHarness h(kPicture);
    h.tl.linkMatching();
    h.session.reset(h.tl.state);
    h.ok("clipboard.copy", Json{{"clips", ids({h.clip("V1@0"), h.clip("V1@4")})}});
    const auto holds = [&] { return h.ok("clipboard.get").data["duration"].get<std::int64_t>(); };
    REQUIRE(holds() == f(8).value);

    // A previewed cut (an assistant plan's dry run): nothing is cut, and the clipboard is not replaced.
    const auto cut = h.run(Json{{"command", "clipboard.cut"}, {"params", {{"clips", ids({h.clip("V1@8")})}}}, {"dryRun", true}});
    REQUIRE(cut.ok());
    CHECK(holds() == f(8).value);
    const auto copy = h.run(Json{{"command", "clipboard.copy"}, {"params", {{"clips", ids({h.clip("V1@8")})}}}, {"dryRun", true}});
    REQUIRE(copy.ok());
    CHECK(holds() == f(8).value);

    // A batch whose later step fails: nothing of it happened, the clipboard included.
    const auto failed = h.run(Json{{"commands", Json::array({
                                       Json{{"command", "clipboard.cut"}, {"params", {{"clips", ids({h.clip("V1@8")})}}}},
                                       Json{{"command", "clip.trim"},
                                            {"params", {{"clip", 999'999}, {"edge", "tail"}, {"delta", "-00:00:00:01"}}}},
                                   })}});
    REQUIRE_FALSE(failed.ok());
    CHECK(holds() == f(8).value);
    CHECK_SESSION(h, kPicture);

    // A real cut still fills it.
    h.ok("clipboard.cut", Json{{"clips", ids({h.clip("V1@8")})}});
    CHECK(holds() == f(4).value);
    h.session.undo();

    // Copy then paste in one batch: the paste sees the copy before the batch commits.
    const auto duplicated = h.run(Json{{"commands", Json::array({
                                           Json{{"command", "clipboard.copy"}, {"params", {{"clips", ids({h.clip("V1@8")})}}}},
                                           Json{{"command", "clipboard.paste"}, {"params", {{"at", tc(14)}}}},
                                       })}});
    REQUIRE(duplicated.ok());
    CHECK_SESSION(h, R"(
        V1 |AaaaBbbbCccc..Cccc..|
        A1 |AaaaBbbbCccc..Cccc..|
    )");
    CHECK(holds() == f(4).value);
}
