// The mask, tracking and redaction commands through the CommandEngine (editingCommands(), so the registration is
// tested too), with fake pictures: synthetic textures moving at a known speed per media item. mask.add/update/remove/
// list with undo; track.mask forward, backward, to a frame count, lost on a cut, progress and cancellation; track.
// transform follow and stabilize; redaction.add (by media + source and through a clip), update, override (linked
// partners), list and remove, with undo; errors without pictures and for the detector seam.

#include "CommandHarness.h"
#include "SyntheticMotion.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/masks/MaskCommands.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>

using namespace wabcut;
using namespace wabcut::masks;
using namespace wabcut::test;
using Catch::Approx;
using command::Json;

namespace {

/// Pictures of the fixture's media (1920x1080, 25 fps) at most 320 wide: media A's content moves (vx, vy) working
/// pixels per frame; from `cutAt` on it shows another picture.
class FakeTracking final : public TrackingServices {
public:
    double vx = 0.8;
    double vy = -0.3;
    std::int64_t cutAt = -1;
    int frames = 0;
    std::vector<TrackProgress> reports;

    std::expected<TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time, int maxWidth) override {
        ++frames;
        const VideoStreamInfo& v = state.findMedia(media)->info.video.at(static_cast<std::size_t>(stream));
        if (time.value < 0 || time >= v.duration) {
            return std::unexpected(Error{ErrorCode::NotFound, "past the end"});
        }
        const std::int64_t k = ticksToFrames(time, v.frameRate);
        const int w = std::min(maxWidth, 320);
        const int h = w * v.height / v.width;
        const bool cut = cutAt >= 0 && k >= cutAt;
        const masks::test::Texture& texture = cut ? m_other : m_texture;
        TrackingFrame f;
        f.luma = masks::test::render(texture, w, h, Motion{1, 0, -vx * static_cast<double>(k), 0, 1, -vy * static_cast<double>(k)});
        f.start = framesToTicks(k, v.frameRate);
        f.duration = framesToTicks(1, v.frameRate);
        return f;
    }
    void progress(const TrackProgress& p) override { reports.push_back(p); }

private:
    masks::test::Texture m_texture{17};
    masks::test::Texture m_other{18};
};

struct MaskHarness {
    std::shared_ptr<FakeTracking> pictures = std::make_shared<FakeTracking>();
    CommandHarness h;

    explicit MaskHarness(std::string_view picture, bool services = true) : MaskHarness(Timeline(picture), services) {}
    explicit MaskHarness(Timeline timeline, bool services = true)
        : h(std::move(timeline), policy(pictures, services), ai::edit::editingCommands()) {}

    static command::EnginePolicy policy(const std::shared_ptr<FakeTracking>& pictures, bool services) {
        command::EnginePolicy p;
        if (services) {
            p.services = pictures;
        }
        return p;
    }
    const Clip& clip(std::string_view ref) { return h.state()->findSequence(h.tl.seq)->findClip(h.clip(ref))->clip(); }
    const EffectInstance* effectWith(std::string_view ref, std::uint32_t mask) {
        for (const EffectInstance& e : clip(ref).effects()) {
            for (const Mask& m : e.masks) {
                if (m.id == mask) {
                    return &e;
                }
            }
        }
        return nullptr;
    }
    const Mask& mask(std::string_view ref, std::uint32_t id) {
        const EffectInstance* e = effectWith(ref, id);
        REQUIRE(e);
        return *std::find_if(e->masks.begin(), e->masks.end(), [&](const Mask& m) { return m.id == id; });
    }
};

Ticks frame(std::int64_t k) { return framesToTicks(k, kRate25); }

/// Applies `fn` to every video clip of `seq` in `state`.
void editVideoClips(ProjectState& state, SequenceId seq, const std::function<void(Clip&)>& fn) {
    for (SequencePtr& sp : state.sequences) {
        if (!sp || sp->id != seq) {
            continue;
        }
        auto copy = std::make_shared<Sequence>(*sp);
        for (TrackPtr& t : copy->videoTracks) {
            std::vector<Clip> clips(t->clips.begin(), t->clips.end());
            for (Clip& c : clips) {
                fn(c);
            }
            auto track = std::make_shared<Track>(*t);
            track->clips = ClipList(std::move(clips));
            t = std::move(track);
        }
        sp = std::move(copy);
    }
}

} // namespace

TEST_CASE("Mask commands: add to the opacity mask or an effect, update, list, remove, with undo", "[masks][commands]") {
    MaskHarness m("V1 |Aaaaaaaaaa|");
    const ClipId clip = m.h.clip("V1@0");
    // Without an effect: the clip's opacity mask (wabcut.mask) is created.
    const auto added = m.h.ok("mask.add", {{"clip", clip.value}, {"name", "Face"},
                                           {"params", {{"center.x", 0.4}, {"size.x", 0.3}, {"feather", 0.05}}}});
    REQUIRE(added.createdIds == std::vector<std::uint64_t>{1});
    const EffectInstance* holder = m.effectWith("V1@0", 1);
    REQUIRE(holder);
    CHECK(holder->effect == "wabcut.mask");
    CHECK(m.mask("V1@0", 1).name == "Face");
    CHECK(maskValue(m.mask("V1@0", 1), "feather", Ticks{}) == Approx(0.05));
    // On an effect: a local correction.
    const auto applied = m.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", "wabcut.color.basic"}});
    REQUIRE(applied.createdIds.size() == 1);
    const EffectInstanceId basic{applied.createdIds[0]};
    const auto path = m.h.ok("mask.add", {{"clip", clip.value}, {"effect", basic.value}, {"shape", "path"},
                                          {"path", Json::array({Json::array({0.2, 0.2}), Json::array({0.8, 0.25}),
                                                                Json{{"x", 0.5}, {"y", 0.8}, {"inX", 0.1}, {"outX", -0.1}}})},
                                          {"mode", "subtract"}});
    REQUIRE(path.createdIds == std::vector<std::uint64_t>{1}); // ids are per effect
    CHECK(m.effectWith("V1@0", 1) != nullptr);
    // Keyframed update: key times are sequence times on the clip.
    m.h.ok("mask.update", {{"clip", clip.value}, {"effect", basic.value}, {"mask", 1}, {"inverted", true},
                           {"params", {{"expansion", Json{{"keys", Json::array({Json{{"time", tc(2)}, {"value", 0.0}},
                                                                                 Json{{"time", tc(6)}, {"value", 0.1}}})}}}}}});
    const EffectInstance* b = nullptr;
    for (const EffectInstance& e : m.clip("V1@0").effects()) {
        b = e.id == basic ? &e : b;
    }
    REQUIRE(b);
    REQUIRE(b->masks.size() == 1);
    CHECK(b->masks[0].inverted);
    CHECK(b->masks[0].mode == MaskMode::Subtract);
    CHECK(maskValue(b->masks[0], "expansion", frame(4)) == Approx(0.05));
    const auto listed = m.h.ok("mask.list", {{"clip", clip.value}});
    CHECK(listed.data["effects"].size() == 2);
    // Errors: unknown parameter, unknown mask, a path without vertices.
    CHECK(m.h.errorOf("mask.update", {{"clip", clip.value}, {"mask", 1}, {"params", {{"bogus", 1}}}}) == "InvalidParams");
    CHECK(m.h.errorOf("mask.update", {{"clip", clip.value}, {"mask", 9}}) == "NotFound");
    CHECK(m.h.errorOf("mask.add", {{"clip", clip.value}, {"shape", "path"}}) == "InvalidParams");
    // Remove, then undo everything back to no masks.
    m.h.ok("mask.remove", {{"clip", clip.value}, {"mask", 1}, {"effect", basic.value}});
    CHECK(m.effectWith("V1@0", 1)->effect == "wabcut.mask");
    while (m.h.session.canUndo()) {
        m.h.session.undo();
    }
    CHECK(m.clip("V1@0").effects().empty());
}

TEST_CASE("track.mask keys the mask's transform frame by frame; backward; to a count; lost on a cut", "[masks][commands][tracking]") {
    MaskHarness m("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    const ClipId clip = m.h.clip("V1@0");
    m.h.ok("mask.add", {{"clip", clip.value}, {"params", {{"size.x", 0.35}, {"size.y", 0.5}}}});
    const double dx = 0.8 / 320.0;  // normalised per frame
    const double dy = -0.3 / 180.0;

    SECTION("forward, ten frames") {
        const auto r = m.h.ok("track.mask", {{"clip", clip.value}, {"mask", 1}, {"frames", 10}, {"model", "translation"}});
        CHECK(r.data["frames"] == 10);
        CHECK(r.data["stopped"] == "limit");
        CHECK(r.data["confidence"].size() == 11);
        const Mask& tracked = m.mask("V1@0", 1);
        const ParamValue* x = tracked.find("transform.x");
        REQUIRE(x);
        REQUIRE(x->curve());
        CHECK(x->curve()->keys.size() == 11);
        for (const int k : {0, 3, 10}) {
            INFO("frame " << k);
            CHECK(maskValue(tracked, "transform.x", frame(k)) == Approx(dx * k).margin(0.0006));
            CHECK(maskValue(tracked, "transform.y", frame(k)) == Approx(dy * k).margin(0.0006));
        }
        CHECK(m.pictures->reports.size() == 10);
        CHECK(m.pictures->reports.back().total == 10);
        // One undo step.
        m.h.session.undo();
        CHECK(m.mask("V1@0", 1).find("transform.x") == nullptr);
    }
    SECTION("backward from a frame") {
        const auto r = m.h.ok("track.mask", {{"clip", clip.value}, {"mask", 1}, {"time", tc(15)}, {"direction", "backward"}, {"frames", 5}});
        CHECK(r.data["frames"] == 5);
        const Mask& tracked = m.mask("V1@0", 1);
        CHECK(maskValue(tracked, "transform.x", frame(15)) == Approx(0.0).margin(1e-9));
        CHECK(maskValue(tracked, "transform.x", frame(10)) == Approx(-5 * dx).margin(0.0006));
        CHECK(maskValue(tracked, "transform.scaleX", frame(10)) == Approx(1.0).margin(0.01)); // similarity, no zoom
    }
    SECTION("to the clip's end") {
        const auto r = m.h.ok("track.mask", {{"clip", clip.value}, {"mask", 1}});
        CHECK(r.data["stopped"] == "end");
        CHECK(r.data["frames"] == 23); // frames 1..23 of a 24-frame clip
    }
    SECTION("lost where the picture cuts away") {
        m.pictures->cutAt = 8;
        const auto r = m.h.ok("track.mask", {{"clip", clip.value}, {"mask", 1}});
        CHECK(r.data["stopped"] == "lost");
        CHECK(r.data["lostAtMediaTime"] == frame(8).value);
        CHECK(r.data["frames"] == 7);
        CHECK_FALSE(r.warnings.empty());
        CHECK(m.mask("V1@0", 1).find("transform.x")->curve()->keys.back().time == frame(7));
        CHECK(m.pictures->reports.back().lost);
    }
}

TEST_CASE("track.transform: a clip follows the region, or the clip is stabilised on it", "[masks][commands][tracking]") {
    MaskHarness m("V1 |Aaaaaaaaaaaaaaa|\n"
                  "V2 |Bbbbbbbbbbbbbbb|");
    const ClipId a = m.h.clip("V1@0");
    const ClipId b = m.h.clip("V2@0");
    const Json region{{"x", 0.35}, {"y", 0.3}, {"width", 0.3}, {"height", 0.4}};
    // 0.8 working px per frame at 320 wide = 4.8 source px = 4.8 canvas px at scale 1.
    SECTION("follow") {
        const auto r = m.h.ok("track.transform", {{"clip", a.value}, {"region", region}, {"target", b.value}, {"frames", 10}});
        CHECK(r.data["mode"] == "follow");
        const ClipProperties& p = m.clip("V2@0").properties();
        CHECK(p.value(ParamId::PositionX, frame(10)) == Approx(48.0).margin(1.0));
        CHECK(p.value(ParamId::PositionY, frame(10)) == Approx(-18.0).margin(1.0));
        CHECK(p.value(ParamId::Scale, frame(10)) == Approx(1.0).margin(0.01));
        CHECK(p.value(ParamId::Rotation, frame(10)) == Approx(0.0).margin(0.2));
        CHECK(m.clip("V1@0").properties()[ParamId::PositionX].isConstant()); // the tracked clip is untouched
    }
    SECTION("stabilize") {
        m.h.ok("track.transform", {{"clip", a.value}, {"region", region}, {"frames", 10}, {"model", "translation"}});
        const ClipProperties& p = m.clip("V1@0").properties();
        CHECK(p.value(ParamId::PositionX, frame(10)) == Approx(-48.0).margin(1.0));
        CHECK(p.value(ParamId::PositionY, frame(10)) == Approx(18.0).margin(1.0));
    }
    SECTION("from a mask; the detector seam; bad requests") {
        m.h.ok("mask.add", {{"clip", a.value}, {"shape", "rectangle"}, {"params", {{"size.x", 0.3}, {"size.y", 0.4}}}});
        m.h.ok("track.transform", {{"clip", a.value}, {"region", {{"mask", 1}}}, {"target", b.value}, {"frames", 3}});
        CHECK(m.h.errorOf("track.transform", {{"clip", a.value}, {"region", {{"detect", "face"}}}}) == "EditRejected");
        CHECK(m.h.errorOf("track.transform", {{"clip", a.value}, {"region", {{"x", 0.1}}}}) == "InvalidParams");
        CHECK(m.h.errorOf("track.transform", {{"clip", a.value}, {"region", region}, {"target", b.value}, {"mode", "stabilize"}}) ==
              "InvalidParams");
    }
}

TEST_CASE("Remapped tracking fails before decoding and redaction includes interior source extrema",
          "[masks][commands][tracking][redaction][remap]") {
    MaskHarness m("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    const ClipId id = m.h.clip("V1@0");
    m.h.ok("mask.add", {{"clip", id.value}});
    m.h.ok("timeRemap.enable", {{"clip", id.value}});
    SECTION("tracking requires an unremapped source") {
        const auto before = m.h.state();
        CHECK(m.h.errorOf("track.mask", {{"clip", id.value}, {"mask", 1}, {"frames", 2}}) == "EditRejected");
        CHECK(m.h.errorOf("track.transform", {{"clip", id.value}, {"frames", 2},
                         {"region", {{"x", 0.2}, {"y", 0.2}, {"width", 0.4}, {"height", 0.4}}}}) == "EditRejected");
        CHECK(m.pictures->frames == 0);
        CHECK(m.h.state() == before);
    }
    SECTION("a reverse excursion between endpoints is all redacted") {
        m.h.ok("timeRemap.addKey", {{"clip", id.value}, {"time", tc(10)}});
        m.h.ok("timeRemap.setKey", {{"clip", id.value}, {"index", 1}, {"source", frame(20).value}});
        m.h.ok("timeRemap.setKey", {{"clip", id.value}, {"index", 2}, {"source", 0}});
        m.h.ok("redaction.add", {{"clip", id.value}, {"in", tc(0)}, {"out", tc(24)}, {"style", "solid"}});
        const Redaction& r = m.h.state()->redactions.front();
        for (int k = 0; k < 24; ++k) {
            INFO("timeline frame " << k);
            CHECK(r.source.contains(m.clip("V1@0").sourceTimeAt(frame(k))));
        }
        m.h.session.undo();
        CHECK(m.h.state()->redactions.empty());
    }
}

TEST_CASE("Tracking needs pictures from the host", "[masks][commands][tracking]") {
    MaskHarness m("V1 |Aaaaaaaa|", false);
    const ClipId clip = m.h.clip("V1@0");
    m.h.ok("mask.add", {{"clip", clip.value}});
    CHECK(m.h.errorOf("track.mask", {{"clip", clip.value}, {"mask", 1}}) == "InvalidRequest");
    CHECK(m.h.errorOf("track.transform", {{"clip", clip.value}, {"region", {{"x", 0.1}, {"y", 0.1}, {"width", 0.2}, {"height", 0.2}}}}) ==
          "InvalidRequest");
}

TEST_CASE("Redaction commands: add by media or through a clip, update, override linked partners, list, remove, undo",
          "[masks][commands][redaction]") {
    Timeline tl("V1 |Aaaaaaaaaa..A20aaaa|\n"
                "A1 |Aaaaaaaaaa..A20aaaa|");
    tl.linkMatching();
    MaskHarness m(std::move(tl));
    const MediaId media = m.h.tl.media('A');
    const auto byMedia = m.h.ok("redaction.add", {{"media", media.value}, {"source", {{"start", tc(2)}, {"end", tc(6)}}},
                                                  {"style", "mosaic"}, {"audio", "bleep"}, {"name", "Plate"},
                                                  {"region", {{"x", 0.6}, {"y", 0.5}, {"width", 0.2}, {"height", 0.1}}}});
    REQUIRE(byMedia.createdIds.size() == 1);
    CHECK(byMedia.changes.redactionsChanged);
    const ProjectSnapshot s1 = m.h.state();
    REQUIRE(s1->redactions.size() == 1);
    CHECK(s1->redactions[0].name == "Plate");
    CHECK(s1->redactions[0].source == TimeRange::fromStartEnd(frame(2), frame(6)));
    CHECK(maskValue(s1->redactions[0].region, "center.x", Ticks{}) == Approx(0.7));
    CHECK(s1->redactions[0].region.shape == MaskShape::Rectangle);
    // Through a clip: the second clip shows media frames 20..24 at sequence frames 12..16.
    const ClipId second = m.h.clip("V1@12");
    const auto viaClip = m.h.ok("redaction.add", {{"clip", second.value}, {"in", tc(13)}, {"out", tc(15)}, {"style", "solid"},
                                                  {"color", Json::array({0, 0, 0})}});
    const ProjectSnapshot s2 = m.h.state();
    REQUIRE(s2->redactions.size() == 2);
    CHECK(s2->redactions[1].media == media);
    CHECK(s2->redactions[1].source == TimeRange::fromStartEnd(frame(21), frame(23)));
    // Update and override (the linked audio clip is excepted too).
    m.h.ok("redaction.update", {{"redaction", byMedia.createdIds[0]}, {"strength", 0.1}, {"words", Json::array({Json{{"start", tc(3)}, {"end", tc(4)}}})}});
    CHECK(m.h.state()->redactions[0].strength == Approx(0.1));
    CHECK(m.h.state()->redactions[0].audioRanges.size() == 1);
    const auto over = m.h.ok("redaction.override", {{"redaction", byMedia.createdIds[0]}, {"clip", m.h.clip("V1@0").value}});
    CHECK(over.data["clips"].size() == 2);
    CHECK_FALSE(m.h.state()->redactions[0].appliesTo(m.h.clip("V1@0")));
    CHECK_FALSE(m.h.state()->redactions[0].appliesTo(m.h.clip("A1@0")));
    CHECK(m.h.state()->redactions[0].appliesTo(second));
    const auto listed = m.h.ok("redaction.list", {{"media", media.value}});
    CHECK(listed.data["redactions"].size() == 2);
    m.h.ok("redaction.remove", {{"redaction", viaClip.createdIds[0]}});
    CHECK(m.h.state()->redactions.size() == 1);
    // Errors.
    CHECK(m.h.errorOf("redaction.add", {{"style", "blur"}}) == "InvalidParams");
    CHECK(m.h.errorOf("redaction.update", {{"redaction", 999}}) == "NotFound");
    CHECK(m.h.errorOf("redaction.add", {{"media", media.value}, {"source", {{"start", tc(6)}, {"end", tc(2)}}}}) == "InvalidParams");
    // Undo all the way: no redactions.
    while (m.h.session.canUndo()) {
        m.h.session.undo();
    }
    CHECK(m.h.state()->redactions.empty());
}

// ---- Codex wave-4 review (docs/internal/reviews-wave4.md), I.6 P2s. Each case failed before its fix. ----------------------------

TEST_CASE("Redaction regions: incomplete, unknown, empty or inverted regions are refused (wave-4 P2-27, P2-29)",
          "[masks][commands][redaction]") {
    MaskHarness m("V1 |Aaaaaaaaaa|");
    const MediaId media = m.h.tl.media('A');
    const auto add = [&](Json region) {
        return m.h.errorOf("redaction.add", {{"media", media.value}, {"source", {{"start", 0}, {"end", frame(4).value}}},
                                             {"region", std::move(region)}});
    };
    CHECK(add({{"x", 0.1}, {"width", 0.2}}) == "InvalidParams");                       // no y / height
    CHECK(add({{"x", 0.1}, {"y", 0.1}, {"w", 0.2}, {"h", 0.2}}) == "InvalidParams");   // unknown keys
    CHECK(add({{"x", 0.1}, {"y", 0.1}, {"width", 0.0}, {"height", 0.2}}) == "InvalidParams");
    CHECK(add({{"shape", "ellipse"}, {"params", {{"size.x", 0.0}}}}) == "InvalidParams"); // no area
    CHECK(add({{"shape", "ellipse"}, {"inverted", true}}) == "InvalidParams");            // would expose the rest
    CHECK(add({{"centre", 0.5}}) == "InvalidParams");
    CHECK(m.h.state()->redactions.empty());
    // The complete forms still work.
    m.h.ok("redaction.add", {{"media", media.value}, {"source", {{"start", 0}, {"end", frame(4).value}}},
                             {"region", {{"x", 0.1}, {"y", 0.2}, {"width", 0.3}, {"height", 0.4}, {"shape", "ellipse"}}}});
    m.h.ok("redaction.add", {{"media", media.value}, {"source", {{"start", 0}, {"end", frame(4).value}}},
                             {"region", {{"shape", "rectangle"}, {"params", {{"size.x", 0.2}, {"size.y", 0.1}}}}}});
    CHECK(m.h.state()->redactions.size() == 2);
}

TEST_CASE("Redaction timecodes are in the media's rate, not the sequence's (wave-4 P2-28)", "[masks][commands][redaction]") {
    Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|"); // a 25 fps sequence
    for (MediaPtr& item : tl.state.media) {
        auto copy = std::make_shared<MediaItem>(*item);
        copy->info.video.front().frameRate = Rate{24, 1}; // a 24p source
        item = std::move(copy);
    }
    MaskHarness m(std::move(tl));
    const MediaId media = m.h.tl.media('A');
    m.h.ok("redaction.add", {{"media", media.value}, {"source", {{"start", "00:00:00:12"}, {"end", "00:00:00:18"}}},
                             {"audio", "bleep"}, {"words", Json::array({Json{{"start", "00:00:00:13"}, {"end", "00:00:00:14"}}})}});
    const Redaction& r = m.h.state()->redactions.front();
    CHECK(r.source == TimeRange::fromStartEnd(framesToTicks(12, Rate{24, 1}), framesToTicks(18, Rate{24, 1})));
    REQUIRE(r.audioRanges.size() == 1);
    CHECK(r.audioRanges.front() == TimeRange::fromStartEnd(framesToTicks(13, Rate{24, 1}), framesToTicks(14, Rate{24, 1})));
}

TEST_CASE("Mask ids are per effect: mask.add names its own effect; an ambiguous id needs \"effect\" (wave-4 P2-30)",
          "[masks][commands]") {
    MaskHarness m("V1 |Aaaaaaaaaa|");
    const ClipId clip = m.h.clip("V1@0");
    // The effect's mask first, then the opacity mask: both get id 1.
    const auto applied = m.h.ok("clip.addEffect", {{"clips", Json::array({clip.value})}, {"effect", "wabcut.color.basic"}});
    const EffectInstanceId basic{applied.createdIds.at(0)};
    m.h.ok("mask.add", {{"clip", clip.value}, {"effect", basic.value}, {"name", "Local"}});
    const auto opacity = m.h.ok("mask.add", {{"clip", clip.value}, {"name", "Cut-out"}});
    std::uint64_t opacityEffect = 0;
    for (const EffectInstance& e : m.clip("V1@0").effects()) {
        opacityEffect = e.effect == "wabcut.mask" ? e.id.value : opacityEffect;
    }
    REQUIRE(opacityEffect != 0);
    CHECK(opacity.data["effect"] == opacityEffect);
    CHECK(opacity.data["mask"]["name"] == "Cut-out");
    // Without "effect", id 1 is ambiguous: refused, nothing edited.
    CHECK(m.h.errorOf("mask.update", {{"clip", clip.value}, {"mask", 1}, {"name", "Renamed"}}) == "InvalidParams");
    CHECK(m.h.errorOf("mask.remove", {{"clip", clip.value}, {"mask", 1}}) == "InvalidParams");
    for (const EffectInstance& e : m.clip("V1@0").effects()) {
        CHECK(e.masks.front().name != "Renamed");
    }
    m.h.ok("mask.update", {{"clip", clip.value}, {"effect", opacityEffect}, {"mask", 1}, {"name", "Renamed"}});
}

TEST_CASE("Tracking a reversed clip follows the timeline direction; defaults track (wave-4 P2-34)", "[masks][commands][tracking]") {
    Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    editVideoClips(tl.state, tl.seq, [](Clip& c) { c.reversed = true; }); // shows media frames 23 .. 0
    MaskHarness m(std::move(tl));
    const ClipId clip = m.h.clip("V1@0");
    m.h.ok("mask.add", {{"clip", clip.value}, {"params", {{"size.x", 0.35}, {"size.y", 0.5}}}});
    const double dx = 0.8 / 320.0; // per media frame
    SECTION("forward from the clip's first frame (media frame 23, going back through the media)") {
        const auto r = m.h.ok("track.mask", {{"clip", clip.value}, {"mask", 1}, {"model", "translation"}});
        CHECK(r.data["frames"] == 23);
        CHECK(r.data["direction"] == "forward");
        const Mask& tracked = m.mask("V1@0", 1);
        for (const int f : {23, 20, 10, 0}) {
            INFO("media frame " << f);
            CHECK(maskValue(tracked, "transform.x", frame(f)) == Approx(dx * (f - 23)).margin(0.0006));
        }
    }
    SECTION("backward from the clip's last frame (media frame 0)") {
        const auto r = m.h.ok("track.mask", {{"clip", clip.value}, {"mask", 1}, {"direction", "backward"}, {"frames", 6},
                                             {"model", "translation"}});
        CHECK(r.data["frames"] == 6);
        CHECK(maskValue(m.mask("V1@0", 1), "transform.x", frame(6)) == Approx(dx * 6).margin(0.0006));
    }
}

TEST_CASE("Tracking a flipped mask keeps it flipped between keys (wave-4 P2-36)", "[masks][commands][tracking]") {
    MaskHarness m("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    const ClipId clip = m.h.clip("V1@0");
    m.h.ok("mask.add", {{"clip", clip.value}, {"params", {{"size.x", 0.35}, {"size.y", 0.5}, {"transform.scaleX", -1.0}}}});
    m.h.ok("track.mask", {{"clip", clip.value}, {"mask", 1}, {"frames", 5}});
    const Mask& tracked = m.mask("V1@0", 1);
    for (const Ticks t : {frame(0), frame(0) + Ticks{frame(1).value / 2}, frame(2) + Ticks{frame(1).value / 2}, frame(5)}) {
        INFO("at " << t.value);
        CHECK(maskValue(tracked, "transform.scaleX", t) == Approx(-1.0).margin(0.02));
        CHECK(maskValue(tracked, "transform.scaleY", t) == Approx(1.0).margin(0.02));
        CHECK(std::abs(maskValue(tracked, "transform.rotation", t)) < 2.0);
    }
}

TEST_CASE("Free-form params of the wrong type are InvalidParams, not internal errors (wave-4 P2-39)",
          "[masks][commands][redaction]") {
    MaskHarness m("V1 |Aaaaaaaaaa|");
    const MediaId media = m.h.tl.media('A');
    const ClipId clip = m.h.clip("V1@0");
    const Json source{{"start", 0}, {"end", frame(4).value}};
    CHECK(m.h.errorOf("redaction.add", {{"media", media.value}, {"source", source}, {"name", 5}}) == "InvalidParams");
    CHECK(m.h.errorOf("redaction.add", {{"media", media.value}, {"source", source},
                                        {"region", {{"x", "left"}, {"y", 0}, {"width", 1}, {"height", 1}}}}) == "InvalidParams");
    CHECK(m.h.errorOf("redaction.add", {{"media", media.value}, {"source", source}, {"color", Json::array({"r", 0, 0})}}) ==
          "InvalidParams");
    CHECK(m.h.errorOf("track.transform", {{"clip", clip.value}, {"region", {{"mask", "one"}}}}) == "InvalidParams");
}
