// C.15 through the engine: clip.setBlendMode, adjustment.add and media.newSynthetic preview in a dry run and commit in
// one undo step; the render commands show the render bar, render through the host's PreviewRenderer and delete preview
// files, and refuse cleanly in hosts without a preview cache.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/Synthetic.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

Json dryRun(std::string_view name, Json params) {
    return Json{{"command", name}, {"params", std::move(params)}, {"dryRun", true}};
}

/// A renderer that writes a small file per segment and counts what it rendered.
class FakeRenderer : public PreviewRenderer {
public:
    std::expected<std::int64_t, Error> render(const ProjectState&, SequenceId, TimeRange range, const fs::path& file,
                                              const std::function<bool(double)>& progress) override {
        if (!progress(0.5)) {
            return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
        }
        std::ofstream(file, std::ios::binary) << "preview " << range.start.value;
        ++renders;
        return 64;
    }
    int renders = 0;
};

fs::path freshDir(const std::string& tag) {
    const fs::path p = fs::path(WABCUT_TEST_OUTPUT_DIR) / "parity-commands" / tag;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

} // namespace

TEST_CASE("clip.setBlendMode previews and commits in one undo step (C.15)", "[command][parity]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    const ClipId a = h.clip("V1@0");
    const auto preview = h.run(dryRun("clip.setBlendMode", Json{{"clips", Json::array({a.value})}, {"mode", "multiply"}}));
    REQUIRE(preview.ok());
    CHECK(h.sequence(h.state()).findClip(a)->clip().blendMode() == BlendMode::Normal);
    CHECK(h.session.undoCount() == 0);

    const auto r = h.ok("clip.setBlendMode", Json{{"clips", Json::array({a.value})}, {"mode", "multiply"}});
    CHECK(r.data["mode"] == "multiply");
    CHECK(h.sequence(h.state()).findClip(a)->clip().blendMode() == BlendMode::Multiply);
    CHECK(h.session.undoCount() == 1);
    h.session.undo();
    CHECK(h.sequence(h.state()).findClip(a)->clip().blendMode() == BlendMode::Normal);
    CHECK(h.errorOf("clip.setBlendMode", Json{{"clips", Json::array({h.clip("A1@0").value})}, {"mode", "screen"}}) ==
          "EditRejected");
    CHECK(h.errorOf("clip.setBlendMode", Json{{"clips", Json::array({a.value})}, {"mode", "glow"}}) == "InvalidParams");
}

TEST_CASE("adjustment.add and media.newSynthetic make generator items in one undo step (C.15)", "[command][parity]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    const std::size_t mediaBefore = h.state()->media.size();
    REQUIRE(h.run(dryRun("adjustment.add", Json{{"at", tc(2)}, {"duration", tc(4)}, {"placement", "newTrack"}})).ok());
    CHECK(h.state()->media.size() == mediaBefore);

    const auto added = h.ok("adjustment.add", Json{{"at", tc(2)}, {"duration", tc(4)}, {"placement", "newTrack"}});
    CHECK(added.data["newItem"] == true);
    CHECK(added.data["track"] == "V2");
    const MediaId item{added.data["media"].get<std::uint64_t>()};
    CHECK(isAdjustmentLayer(*h.state()->findMedia(item)));
    CHECK(h.session.undoCount() == 1);
    // Reusing the item; then effects on it adjust what is below (clip.addEffect).
    const auto reused = h.ok("adjustment.add", Json{{"at", tc(7)}, {"duration", tc(2)}, {"track", "V2"}, {"media", item.value}});
    CHECK(reused.data["newItem"] == false);
    CHECK(reused.data["media"] == item.value);
    h.ok("clip.addEffect", Json{{"clips", Json::array({added.data["clip"]})}, {"effect", "wabcut.key.ultra"}});
    CHECK(h.errorOf("adjustment.add", Json{{"at", tc(0)}, {"track", "V1"}, {"media", h.tl.media('A').value}}) == "EditRejected");

    const auto matte = h.ok("media.newSynthetic", Json{{"kind", "colorMatte"}, {"params", Json{{"color.g", 0.25}}}});
    CHECK(matte.data["name"] == "Color Matte");
    const MediaItem& m = *h.state()->findMedia(MediaId{matte.data["media"].get<std::uint64_t>()});
    CHECK(syntheticParam(m, "color.g") == 0.25);
    CHECK(syntheticParam(m, "color.r") == 1.0);
    const auto black = h.ok("media.newSynthetic", Json{{"kind", "blackVideo"}, {"name", "Slug"}});
    CHECK(black.data["name"] == "Slug");
    CHECK(h.run(dryRun("media.newSynthetic", Json{{"kind", "transparentVideo"}})).ok());
    CHECK(h.errorOf("media.newSynthetic", Json{{"kind", "colorMatte"}, {"params", Json{{"glow", 1}}}}) == "InvalidParams");
    CHECK(h.errorOf("media.newSynthetic", Json{{"kind", "colorMatte"}, {"params", Json{{"color.r", "red"}}}}) == "InvalidParams");
    CHECK(h.errorOf("media.newSynthetic", Json{{"kind", "plasma"}}) == "InvalidParams");
    CHECK(h.errorOf("media.newSynthetic", Json{{"kind", "colorMatte"}, {"bin", 999}}) == "NotFound");
}

TEST_CASE("The render commands show the bar and need a preview cache to render or delete (C.15)", "[command][parity][render]") {
    CommandHarness h(R"(
        V1 tgt |AaaaBbbb|
    )");
    h.ok("clip.addEffect", Json{{"clips", Json::array({h.clip("V1@4").value})}, {"effect", "wabcut.blur.gaussian"}});
    const auto bar = h.ok("render.status");
    REQUIRE(bar.data["segments"].size() == 2);
    CHECK(bar.data["segments"][0]["status"] == "none");
    CHECK(bar.data["segments"][1]["status"] == "yellow");
    CHECK(bar.data["cache"] == false);
    // Without a preview renderer: a dry run lists the work, a real run is refused; deleting needs a cache.
    const auto planned = h.run(dryRun("render.previewInOut", Json::object()));
    REQUIRE(planned.ok());
    CHECK(planned.data["segments"].size() == 1);
    CHECK(h.errorOf("render.previewInOut") == "InvalidRequest");
    CHECK(h.errorOf("render.deletePreviewFiles", Json{{"all", true}}) == "InvalidRequest");
}

TEST_CASE("render.previewInOut renders the yellow and red segments once and deletePreviewFiles removes them (C.15)",
          "[command][parity][render]") {
    const fs::path folder = freshDir("render");
    auto cache = std::make_shared<PreviewCache>(folder);
    auto renderer = std::make_shared<FakeRenderer>();
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::PreviewServices>(cache, renderer);
    CommandHarness h(Timeline(R"(
        V2 tgt |..........|
        V1 tgt |AaaaBbbbCc|
    )"),
                     policy);
    h.ok("clip.addEffect", Json{{"clips", Json::array({h.clip("V1@4").value})}, {"effect", "wabcut.blur.gaussian"}});
    h.ok("adjustment.add", Json{{"at", tc(8)}, {"duration", tc(2)}, {"track", "V2"}});
    const std::size_t undo = h.session.undoCount();

    const auto rendered = h.ok("render.previewInOut");
    CHECK(rendered.data["rendered"] == 2); // B (yellow) and C under the adjustment layer (red)
    CHECK(renderer->renders == 2);
    CHECK(h.session.undoCount() == undo); // rendering is not an edit
    REQUIRE(cache->entries().size() == 2);
    CHECK(fs::exists(folder / std::string(PreviewCache::kIndexFile)));
    const auto status = h.ok("render.status");
    CHECK(status.data["counts"]["rendered"] == 2);
    CHECK(status.data["cache"] == true);
    // Nothing left to render.
    CHECK(h.ok("render.previewInOut").data["rendered"] == 0);
    CHECK(renderer->renders == 2);

    // A dry run of the delete lists; the real one deletes the files.
    const auto listed = h.run(dryRun("render.deletePreviewFiles", Json::object()));
    REQUIRE(listed.ok());
    CHECK(listed.data["files"] == 2);
    CHECK(cache->entries().size() == 2);
    const auto deleted = h.ok("render.deletePreviewFiles", Json{{"start", tc(4)}, {"end", tc(8)}});
    CHECK(deleted.data["files"] == 1);
    CHECK(cache->entries().size() == 1);
    CHECK(h.ok("render.deletePreviewFiles", Json{{"all", true}}).data["files"] == 1);
    CHECK(cache->entries().empty());
}

TEST_CASE("sequence.create makes a Shorts sequence from a preset in one undo step (C.15)", "[command][parity][gaming]") {
    CommandHarness h("    V1 tgt |Aaaa|\n");
    const auto presets = h.ok("sequence.presets");
    bool shorts = false;
    for (const Json& p : presets.data["presets"]) {
        shorts = shorts || (p["id"] == "shorts" && p["width"] == 1080 && p["height"] == 1920 && p["frameRate"]["num"] == 60);
    }
    CHECK(shorts);
    CHECK(presets.data["presets"].size() >= 18);
    const std::size_t before = h.state()->sequences.size();
    REQUIRE(h.run(dryRun("sequence.create", Json{{"preset", "1440p144"}})).ok());
    CHECK(h.state()->sequences.size() == before);
    const auto made = h.ok("sequence.create", Json{{"preset", "shorts"}, {"name", "Clutch (Short)"}});
    const Sequence& s = *h.state()->findSequence(SequenceId{made.data["sequence"].get<std::uint64_t>()});
    CHECK(s.name == "Clutch (Short)");
    CHECK(s.width == 1080);
    CHECK(s.height == 1920);
    CHECK(s.frameRate == Rate{60, 1});
    CHECK(h.session.undoCount() == 1);
    CHECK(h.errorOf("sequence.create", Json{{"preset", "8k1000"}}) == "InvalidParams");
}

TEST_CASE("layout.apply lays out gameplay and facecam as editable clips in one undo step (C.15)", "[command][parity][gaming]") {
    CommandHarness h(R"(
        V2 tgt |Ffffffffff|
        V1 tgt |Gggggggggg|
        A1 tgt |Gggggggggg|
    )");
    const ClipId game = h.clip("V1@0");
    const ClipId cam = h.clip("V2@0");
    const Json request{{"preset", "pip"}, {"gameplay", game.value}, {"facecam", cam.value}, {"corner", "topRight"}, {"circle", true}};
    REQUIRE(h.run(dryRun("layout.apply", request)).ok());
    CHECK(h.sequence(h.state()).findClip(cam)->clip().effects().empty());
    const auto laid = h.ok("layout.apply", request);
    CHECK(laid.data["preset"] == "pip");
    CHECK(laid.data["facecam"] == cam.value);
    CHECK(h.session.undoCount() == 1);
    const Clip& framed = h.sequence(h.state()).findClip(cam)->clip();
    CHECK(framed.effects().size() == 4); // crop, circle mask, border, shadow
    CHECK(framed.properties().value(ParamId::Scale) < 1.0);
    // A vertical blur fill in a Shorts sequence would need a 9:16 sequence; here it still lays out (any frame).
    const auto fill = h.ok("layout.apply", Json{{"preset", "verticalBlurFill"}, {"gameplay", game.value}, {"zoom", 1.2}});
    CHECK(fill.data.contains("foreground"));
    CHECK(h.errorOf("layout.apply", Json{{"preset", "pip"}, {"gameplay", cam.value}, {"facecam", game.value}}) == "EditRejected");
    CHECK(h.errorOf("layout.apply", Json{{"preset", "pip"}, {"gameplay", game.value}, {"facecam", cam.value}, {"size", 3}}) ==
          "InvalidParams");
    const auto list = h.ok("layout.list");
    CHECK(list.data["layouts"].size() == 3);
    CHECK(list.data["defaults"]["size"] == 0.28);
}

TEST_CASE("marker.add and marker.update carry a highlight and marker.list reads it (C.15)", "[command][parity][highlight]") {
    CommandHarness h("    V1 tgt |Aaaaaaaaaa|\n");
    const Json highlight{{"score", 0.9}, {"confidence", "high"}, {"reasons", Json::array({"kill streak", "mic peak"})},
                         {"peak", tc(3)}};
    h.ok("marker.add", Json{{"time", tc(2)}, {"duration", tc(4)}, {"name", "Clutch"}, {"highlight", highlight}});
    const auto listed = h.ok("marker.list");
    REQUIRE(listed.data["markers"].size() == 1);
    const Json& got = listed.data["markers"][0]["highlight"];
    CHECK(got["score"] == 0.9);
    CHECK(got["confidence"] == "high");
    CHECK(got["reasons"] == Json::array({"kill streak", "mic peak"}));
    CHECK(got["peak"] == tc(3));
    CHECK(got["source"] == "highlights");
    const auto summary = h.ok("sequence.summary");
    REQUIRE(summary.data["highlights"].size() == 1);
    CHECK(summary.data["highlights"][0]["index"] == 0);
    // Moving the marker moves its peak; null removes the highlight.
    h.ok("marker.update", Json{{"index", 0}, {"time", tc(5)}});
    CHECK(h.ok("marker.list").data["markers"][0]["highlight"]["peak"] == tc(6));
    h.ok("marker.update", Json{{"index", 0}, {"highlight", nullptr}});
    CHECK(h.ok("marker.list").data["markers"][0]["highlight"].is_null());
    // Bounds.
    CHECK(h.errorOf("marker.add", Json{{"time", tc(1)}, {"highlight", Json{{"score", 1.5}}}}) == "InvalidParams");
    CHECK(h.errorOf("marker.add", Json{{"time", tc(1)}, {"highlight", Json{{"score", 0.5}, {"peak", tc(4)}}}}) == "InvalidParams");
    CHECK(h.errorOf("marker.add", Json{{"time", tc(1)}, {"highlight", Json{{"score", 0.5}, {"confidence", "sure"}}}}) ==
          "InvalidParams");
}
