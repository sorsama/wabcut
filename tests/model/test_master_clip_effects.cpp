// Master Clip Effects (0.6, card #71; MasterClipEffects.h): one live effect stack on a media item that every clip of it
// plays before its own effects. Edits through EffectTarget::ofMedia and through a clip naming a master effect, moving
// an effect to the master clip and back, the per-clip switch, subclips, undo, project files and flattening.

#include "TimelineFixture.h"

#include <wabcut/model/Auditions.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Resolve.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;
using edit::EffectTarget;
using edit::ParamRef;

namespace {

const Clip& clipIn(const ProjectState& s, SequenceId seq, ClipId id) {
    const auto loc = s.findSequence(seq)->findClip(id);
    REQUIRE(loc);
    return loc->clip();
}

/// The effect ids a clip plays, in order.
std::vector<std::string> playedIds(const ProjectState& s, SequenceId seq, ClipId id, EffectKind kind) {
    std::vector<std::string> out;
    for (const EffectInstance& e : playedEffects(s, clipIn(s, seq, id), kind)) {
        out.push_back(e.effect);
    }
    return out;
}

double paramOf(const EffectStack& stack, EffectInstanceId id, std::string_view key) {
    for (const EffectInstance& e : stack) {
        if (e.id == id) {
            const ParamValue* v = e.find(key);
            REQUIRE(v);
            return v->constant();
        }
    }
    FAIL("no such effect");
    return 0.0;
}

/// Two clips of A with a B between them, each with linked sound.
Timeline twoInstances() {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbA20aaa|
        A1 tgt |AaaaBbbbA20aaa|
    )");
    tl.linkMatching();
    return tl;
}

using Strings = std::vector<std::string>;

} // namespace

TEST_CASE("Master Clip Effects: every clip of a media plays them before its own (0.6)", "[model][effects][master]") {
    Timeline tl = twoInstances();
    const MediaId a = tl.media('A');
    const ClipId first = tl.clip("V1@0");
    const ClipId second = tl.clip("V1@8");
    const ClipId other = tl.clip("V1@4");

    // A Gaussian Blur on A's master clip: one new instance, reported like a clip's.
    const auto blur = ok(edit::addEffect(tl.state, tl.seq, EffectTarget::ofMedia(a), "wabcut.blur.gaussian",
                                         {EffectParam{"radius", ParamValue(12.0)}}));
    REQUIRE(blur.createdEffects.size() == 1);
    const EffectInstanceId blurId = blur.createdEffects.front();
    CHECK(blur.changes.modifiedMedia == std::vector<MediaId>{a});
    CHECK(blur.changes.sequences.empty()); // the clips are as they were: the media item changed
    ProjectState s = blur.state;
    REQUIRE(s.findMedia(a)->masterEffects.size() == 1);
    CHECK(edit::effectStackOf(s, tl.seq, EffectTarget::ofMedia(a)) == &s.findMedia(a)->masterEffects);

    // The second instance has an effect of its own: it plays after the master's.
    s = ok(edit::addEffect(s, tl.seq, std::span(&second, 1), "wabcut.color.tint")).state;
    CHECK(playedIds(s, tl.seq, first, EffectKind::Video) == Strings{"wabcut.blur.gaussian"});
    CHECK(playedIds(s, tl.seq, second, EffectKind::Video) == Strings{"wabcut.blur.gaussian", "wabcut.color.tint"});
    CHECK(playedIds(s, tl.seq, other, EffectKind::Video).empty());
    CHECK(clipIn(s, tl.seq, first).effects().empty()); // the clip's own stack stays its own
    CHECK(playsMasterEffects(s, clipIn(s, tl.seq, first)));
    CHECK_FALSE(playsMasterEffects(s, clipIn(s, tl.seq, other)));

    // The sound plays the audio effects only, and the picture the video ones.
    CHECK(playedIds(s, tl.seq, tl.clip("A1@0"), EffectKind::Audio).empty());
    s = ok(edit::addEffect(s, tl.seq, EffectTarget::ofMedia(a), "wabcut.audio.gain", {EffectParam{"gain", ParamValue(-6.0)}}))
            .state;
    CHECK(playedIds(s, tl.seq, tl.clip("A1@0"), EffectKind::Audio) == Strings{"wabcut.audio.gain"});
    CHECK(playedIds(s, tl.seq, tl.clip("A1@8"), EffectKind::Audio) == Strings{"wabcut.audio.gain"});
    CHECK(playedIds(s, tl.seq, first, EffectKind::Video) == Strings{"wabcut.blur.gaussian"});
    CHECK(masterEffectsOf(s, a).size() == 2);
    CHECK(masterEffectsOf(s, a, EffectKind::Video).size() == 1);

    SECTION("a change made through one instance shows on every instance") {
        const auto changed = ok(edit::setParameter(s, tl.seq, first, ParamRef::ofEffect(blurId, "radius"), ParamValue(30.0)));
        CHECK(changed.changes.modifiedMedia == std::vector<MediaId>{a});
        CHECK(paramOf(playedEffects(changed.state, clipIn(changed.state, tl.seq, second), EffectKind::Video), blurId, "radius") ==
              Approx(30.0));
        CHECK(paramOf(changed.state.findMedia(a)->masterEffects, blurId, "radius") == Approx(30.0));
        // Keys are media time, so both instances hold the key at the same source frame.
        const auto keyed = ok(edit::addKeyframe(changed.state, tl.seq, second, ParamRef::ofEffect(blurId, "radius"),
                                                Keyframe{f(21), 50.0}));
        const ParamValue* radius = keyed.state.findMedia(a)->masterEffects.front().find("radius");
        REQUIRE(radius);
        REQUIRE(radius->isCurve());
        CHECK(radius->curve()->keys.front().time == f(21));
        // Turned off and removed through a clip, too.
        edit::EffectChange off;
        off.enabled = false;
        const auto disabled = ok(edit::setEffect(keyed.state, tl.seq, second, blurId, off));
        CHECK_FALSE(disabled.state.findMedia(a)->masterEffects.front().enabled);
        const auto removed = ok(edit::removeEffect(disabled.state, tl.seq, first, blurId));
        CHECK(masterEffectsOf(removed.state, a, EffectKind::Video).empty());
        CHECK(playedIds(removed.state, tl.seq, second, EffectKind::Video) == Strings{"wabcut.color.tint"});
    }
    SECTION("a new instance has them at once") {
        edit::SourceClip source;
        source.media = a;
        source.sourceRange = {f(40), f(4)};
        source.patches = edit::defaultSourcePatches(*s.findSequence(tl.seq), *s.findMedia(a));
        const auto placed = ok(edit::overwriteEdit(s, tl.seq, source, f(14)));
        REQUIRE(placed.placed.size() >= 2); // the picture and the sound
        int pictures = 0;
        int sounds = 0;
        for (const ClipId id : placed.placed) {
            const Clip& clip = clipIn(placed.state, tl.seq, id);
            CHECK(clip.effects().empty());
            if (clip.stream.kind == StreamKind::Video) {
                ++pictures;
                CHECK(playedIds(placed.state, tl.seq, id, EffectKind::Video) == Strings{"wabcut.blur.gaussian"});
            } else {
                ++sounds;
                CHECK(playedIds(placed.state, tl.seq, id, EffectKind::Audio) == Strings{"wabcut.audio.gain"});
            }
        }
        CHECK(pictures == 1);
        CHECK(sounds >= 1);
    }
    SECTION("one clip turns them off and the others keep them") {
        const auto off = ok(edit::setMasterEffectsOff(s, tl.seq, std::span(&second, 1), true));
        CHECK(clipIn(off.state, tl.seq, second).content().masterEffectsOff);
        CHECK(playedIds(off.state, tl.seq, second, EffectKind::Video) == Strings{"wabcut.color.tint"});
        CHECK(playedIds(off.state, tl.seq, first, EffectKind::Video) == Strings{"wabcut.blur.gaussian"});
        CHECK_FALSE(playsMasterEffects(off.state, clipIn(off.state, tl.seq, second)));
        const auto on = ok(edit::setMasterEffectsOff(off.state, tl.seq, std::span(&second, 1), false));
        CHECK(playedIds(on.state, tl.seq, second, EffectKind::Video) == Strings{"wabcut.blur.gaussian", "wabcut.color.tint"});
    }
    SECTION("a master clip has effects only, of the kinds its media has") {
        CHECK_FALSE(edit::setParameter(s, tl.seq, EffectTarget::ofMedia(a), ParamRef::of(ParamId::Opacity), ParamValue(0.5)));
        CHECK_FALSE(edit::addEffect(s, tl.seq, EffectTarget::ofMedia(MediaId{999'999}), "wabcut.blur.gaussian"));
        ProjectState silent = s;
        MediaItem item = *silent.findMedia(tl.media('B'));
        item.info.audio.clear();
        for (MediaPtr& m : silent.media) {
            if (m->id == item.id) {
                m = std::make_shared<const MediaItem>(item);
            }
        }
        CHECK_FALSE(edit::addEffect(silent, tl.seq, EffectTarget::ofMedia(item.id), "wabcut.audio.gain"));
        CHECK(edit::addEffect(silent, tl.seq, EffectTarget::ofMedia(item.id), "wabcut.blur.gaussian"));
    }
}

TEST_CASE("Master Clip Effects: an effect moves to the master clip and back in one undo step each (0.6)",
          "[model][effects][master]") {
    Timeline tl = twoInstances();
    const MediaId a = tl.media('A');
    const ClipId first = tl.clip("V1@0");
    const ClipId second = tl.clip("V1@8");
    EditSession session(tl.state);
    const auto tint = session.apply("Add Effect", [&](const ProjectState& s) {
        return edit::addEffect(s, tl.seq, std::span(&first, 1), "wabcut.color.tint", {EffectParam{"amount", ParamValue(0.4)}});
    });
    REQUIRE(tint);
    const EffectInstanceId tintId = tint->createdEffects.front();
    const std::size_t steps = session.undoCount();

    REQUIRE(session.apply("Move to Master Clip", [&](const ProjectState& s) { return edit::moveEffectToMaster(s, tl.seq, first, tintId); }));
    CHECK(session.undoCount() == steps + 1);
    {
        const ProjectSnapshot s = session.snapshot();
        CHECK(clipIn(*s, tl.seq, first).effects().empty());
        REQUIRE(s->findMedia(a)->masterEffects.size() == 1);
        CHECK(s->findMedia(a)->masterEffects.front().id == tintId); // the same instance, its values kept
        CHECK(paramOf(s->findMedia(a)->masterEffects, tintId, "amount") == Approx(0.4));
        CHECK(playedIds(*s, tl.seq, second, EffectKind::Video) == Strings{"wabcut.color.tint"});
        requireValid(*s);
    }
    REQUIRE(session.undo());
    {
        const ProjectSnapshot s = session.snapshot();
        CHECK(s->findMedia(a)->masterEffects.empty());
        CHECK(clipIn(*s, tl.seq, first).effects().size() == 1);
        CHECK(playedIds(*s, tl.seq, second, EffectKind::Video).empty());
    }
    REQUIRE(session.redo());
    // A video effect does not move onto the sound.
    CHECK_FALSE(edit::moveEffectToClip(*session.snapshot(), tl.seq, tl.clip("A1@8"), tintId));

    // Back onto the second clip: first in its stack (it played first), gone from the master and the first clip.
    REQUIRE(session.apply("Add Effect", [&](const ProjectState& s) {
        return edit::addEffect(s, tl.seq, std::span(&second, 1), "wabcut.blur.gaussian");
    }));
    REQUIRE(session.apply("Move to Clip", [&](const ProjectState& s) { return edit::moveEffectToClip(s, tl.seq, second, tintId); }));
    {
        const ProjectSnapshot s = session.snapshot();
        CHECK(s->findMedia(a)->masterEffects.empty());
        CHECK(playedIds(*s, tl.seq, second, EffectKind::Video) == Strings{"wabcut.color.tint", "wabcut.blur.gaussian"});
        CHECK(playedIds(*s, tl.seq, first, EffectKind::Video).empty());
        requireValid(*s);
    }
    REQUIRE(session.undo());
    CHECK(session.snapshot()->findMedia(a)->masterEffects.size() == 1);
    CHECK_FALSE(edit::moveEffectToMaster(*session.snapshot(), tl.seq, first, EffectInstanceId{999'999}));
}

TEST_CASE("Master Clip Effects: a subclip plays its master item's effects then its own (0.6)", "[model][effects][master]") {
    Timeline tl = twoInstances();
    const MediaId a = tl.media('A');
    ProjectState s = tl.state;
    MediaItem sub = *s.findMedia(a);
    sub.id = s.ids.allocate<MediaId>();
    sub.name = "A sub";
    sub.subclip = SubclipSpec{a, TimeRange{f(30), f(20)}, true};
    s.media.push_back(std::make_shared<const MediaItem>(sub));
    requireValid(s);

    const auto blur = ok(edit::addEffect(s, tl.seq, EffectTarget::ofMedia(a), "wabcut.blur.gaussian"));
    const auto tint = ok(edit::addEffect(blur.state, tl.seq, EffectTarget::ofMedia(sub.id), "wabcut.color.tint"));
    s = tint.state;
    edit::SourceClip source;
    source.media = sub.id;
    source.sourceRange = {f(30), f(4)};
    source.patches = {edit::SourcePatch{StreamRef{StreamKind::Video, 0}, tl.track("V1")}};
    const auto placed = ok(edit::overwriteEdit(s, tl.seq, source, f(14)));
    REQUIRE(placed.placed.size() == 1);
    CHECK(playedIds(placed.state, tl.seq, placed.placed[0], EffectKind::Video) == Strings{"wabcut.blur.gaussian", "wabcut.color.tint"});
    CHECK(playedIds(placed.state, tl.seq, tl.clip("V1@0"), EffectKind::Video) == Strings{"wabcut.blur.gaussian"});
    // An edit of the master item's effect through the subclip's clip lands on the master item.
    CHECK(masterEffectHolder(placed.state, sub.id, blur.createdEffects.front())->id == a);
    CHECK(masterEffectHolder(placed.state, sub.id, tint.createdEffects.front())->id == sub.id);
    const auto changed = ok(edit::setParameter(placed.state, tl.seq, placed.placed[0],
                                               ParamRef::ofEffect(blur.createdEffects.front(), "radius"), ParamValue(8.0)));
    CHECK(changed.changes.modifiedMedia == std::vector<MediaId>{a});
}

TEST_CASE("Project files: Master Clip Effects and the clip switch round-trip and are written only when set (0.6)",
          "[model][effects][master][io]") {
    Timeline tl = twoInstances();
    const MediaId a = tl.media('A');
    const ClipId second = tl.clip("V1@8");
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    CHECK(plain->find("\"masterEffects\"") == std::string::npos);
    CHECK(plain->find("\"masterEffectsOff\"") == std::string::npos);

    Curve radius;
    radius.keys = {Keyframe{f(0), 0.0}, Keyframe{f(10), 40.0}};
    ProjectState s = ok(edit::addEffect(tl.state, tl.seq, EffectTarget::ofMedia(a), "wabcut.blur.gaussian",
                                        {EffectParam{"radius", ParamValue(radius)}}))
                         .state;
    s = ok(edit::addEffect(s, tl.seq, EffectTarget::ofMedia(a), "wabcut.audio.gain")).state;
    s = ok(edit::setMasterEffectsOff(s, tl.seq, std::span(&second, 1), true)).state;
    const auto text = io::toJson(s);
    REQUIRE(text);
    CHECK(text->find("\"masterEffects\"") != std::string::npos);
    CHECK(text->find("\"masterEffectsOff\": true") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(*loaded == s);
    CHECK(loaded->findMedia(a)->masterEffects.size() == 2);
    CHECK(playedIds(*loaded, tl.seq, tl.clip("V1@0"), EffectKind::Video) == Strings{"wabcut.blur.gaussian"});
    CHECK(playedIds(*loaded, tl.seq, second, EffectKind::Video).empty());

    SECTION("a file with too many master clip effects is refused") {
        ProjectState big = s;
        MediaItem item = *big.findMedia(a);
        for (int i = 0; i < 64; ++i) {
            EffectInstance e;
            e.id = big.ids.allocate<EffectInstanceId>();
            e.effect = "wabcut.color.tint";
            item.masterEffects.push_back(e);
        }
        for (MediaPtr& m : big.media) {
            if (m->id == a) {
                m = std::make_shared<const MediaItem>(item);
            }
        }
        CHECK_FALSE(validate(big, Validation::Structure));
    }
}

TEST_CASE("Master Clip Effects: flattening folds them onto every clip for formats without them (0.6)",
          "[model][effects][master][interchange]") {
    Timeline tl = twoInstances();
    const MediaId a = tl.media('A');
    const ClipId first = tl.clip("V1@0");
    const ClipId second = tl.clip("V1@8");
    ProjectState s = ok(edit::addEffect(tl.state, tl.seq, EffectTarget::ofMedia(a), "wabcut.blur.gaussian")).state;
    s = ok(edit::addEffect(s, tl.seq, EffectTarget::ofMedia(a), "wabcut.audio.gain")).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&second, 1), "wabcut.color.tint")).state;

    std::size_t folded = 0;
    const ProjectState flat = flattenMasterEffects(s, &folded);
    CHECK(folded == 4); // two pictures and two sounds of A
    CHECK(flat.findMedia(a)->masterEffects.empty());
    requireValid(flat);
    // Every clip plays what it played before, now from its own stack.
    for (const char* ref : {"V1@0", "V1@4", "V1@8"}) {
        INFO(ref);
        CHECK(playedIds(flat, tl.seq, tl.clip(ref), EffectKind::Video) == playedIds(s, tl.seq, tl.clip(ref), EffectKind::Video));
    }
    CHECK(clipIn(flat, tl.seq, second).effects().size() == 2);
    CHECK(clipIn(flat, tl.seq, first).effects().front().effect == "wabcut.blur.gaussian");
    CHECK(clipIn(flat, tl.seq, tl.clip("A1@0")).effects().front().effect == "wabcut.audio.gain");

    // A clip with them off keeps its own stack; a project without any is returned as it is.
    const auto off = ok(edit::setMasterEffectsOff(s, tl.seq, std::span(&first, 1), true));
    std::size_t fewer = 0;
    const ProjectState partly = flattenMasterEffects(off.state, &fewer);
    CHECK(fewer == 3);
    CHECK(clipIn(partly, tl.seq, first).effects().empty());
    std::size_t none = 99;
    CHECK(flattenMasterEffects(tl.state, &none) == tl.state);
    CHECK(none == 0);
}

TEST_CASE("Master Clip Effects: an edit of them makes render previews stale and shows on the render bar (fix F1A)",
          "[model][effects][master][render]") {
    // ARGON-02: the render key left the master clip effects out, so a preview rendered before a master effect edit
    // stayed "Rendered" and played the old picture; and a clip whose only effects were master ones showed no bar.
    Timeline tl = twoInstances();
    const MediaId a = tl.media('A');
    const ClipId first = tl.clip("V1@0");
    const ClipId second = tl.clip("V1@8");
    const TimeRange range = clipIn(tl.state, tl.seq, first).timelineRange;
    const TimeRange later = clipIn(tl.state, tl.seq, second).timelineRange;
    const std::string plain = renderContentHash(tl.state, tl.seq, range);
    REQUIRE_FALSE(plain.empty());

    // A preview of the first instance, rendered before the master effect is added.
    const std::filesystem::path folder = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "master" / "previews";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    PreviewCache cache(folder);
    PreviewEntry entry{tl.seq, range, plain, PreviewCache::fileNameFor(tl.seq, range, plain), 4, 0};
    std::ofstream(folder / entry.file, std::ios::binary) << "wpv!";
    cache.record(entry);
    REQUIRE(cache.renderBar(tl.state, tl.seq, range).front().status == RenderStatus::Rendered);
    CHECK(renderSegments(tl.state, tl.seq, range).front().status == RenderStatus::None);

    // A Gaussian Blur on A's master clip: every instance's content changes, the preview is dropped (its file too), and
    // the bar says why.
    const auto blur = ok(edit::addEffect(tl.state, tl.seq, EffectTarget::ofMedia(a), "wabcut.blur.gaussian",
                                         {EffectParam{"radius", ParamValue(12.0)}}));
    const std::string blurred = renderContentHash(blur.state, tl.seq, range);
    CHECK(blurred != plain);
    CHECK(renderContentHash(blur.state, tl.seq, later) != renderContentHash(tl.state, tl.seq, later));
    CHECK(cache.invalidate(blur.state, blur.changes).size() == 1);
    CHECK(cache.entries().empty());
    CHECK_FALSE(std::filesystem::exists(folder / entry.file));
    const auto bar = renderSegments(blur.state, tl.seq, range);
    REQUIRE_FALSE(bar.empty());
    CHECK(bar.front().status == RenderStatus::Yellow);
    CHECK(bar.front().reason.find("Gaussian Blur") != std::string::npos);
    CHECK(bar.front().cost > renderSegments(tl.state, tl.seq, range).front().cost);

    // A parameter of it edited (through an instance): stale again.
    const EffectInstanceId blurId = blur.createdEffects.front();
    const auto wider = ok(edit::setParameter(blur.state, tl.seq, first, ParamRef::ofEffect(blurId, "radius"), ParamValue(30.0)));
    CHECK(renderContentHash(wider.state, tl.seq, range) != blurred);

    // Off for the first instance: its content is no longer the blurred one, and its bar is clear again.
    const ClipId off[] = {first};
    const auto unblurred = ok(edit::setMasterEffectsOff(wider.state, tl.seq, off, true));
    CHECK(renderContentHash(unblurred.state, tl.seq, range) != renderContentHash(wider.state, tl.seq, range));
    CHECK(renderSegments(unblurred.state, tl.seq, range).front().status == RenderStatus::None);
    std::filesystem::remove_all(folder);
}

TEST_CASE("Master Clip Effects: a Track Matte Key on the master clip mattes every instance (fix F1A)",
          "[model][effects][master][resolve]") {
    // ARGON-06 sweep: the compositor's plan looked for a Track Matte Key in the clip's own stack only.
    Timeline tl = twoInstances();
    const MediaId a = tl.media('A');
    const ClipId first = tl.clip("V1@0");
    const auto keyed = ok(edit::addEffect(tl.state, tl.seq, EffectTarget::ofMedia(a), std::string(kTrackMatteEffect)));
    // The Track Matte Key the first clip's step uses at `at` (invalid: none), or nullopt when it has no step there.
    const auto matteOf = [&](const ProjectState& s, Ticks at) -> std::optional<EffectInstanceId> {
        const CompositePlan plan = resolveComposite(s, tl.seq, at);
        for (const CompositeStep& step : plan.steps) {
            if (step.clip && step.clip->id == first) {
                return step.matte && step.matte->effect ? step.matte->effect->id : EffectInstanceId{};
            }
        }
        return std::nullopt;
    };
    const Ticks at = clipIn(tl.state, tl.seq, first).start();
    CHECK(matteOf(keyed.state, at) == std::optional<EffectInstanceId>(keyed.createdEffects.front()));
    // Not before (nothing to matte), nor with the master clip effects off on the clip.
    CHECK(matteOf(tl.state, at) == std::optional<EffectInstanceId>(EffectInstanceId{}));
    const ClipId off[] = {first};
    const auto unkeyed = ok(edit::setMasterEffectsOff(keyed.state, tl.seq, off, true));
    CHECK(matteOf(unkeyed.state, at) == std::optional<EffectInstanceId>(EffectInstanceId{}));
}

TEST_CASE("Master Clip Effects: flattening folds them onto an audition's other takes too (fix F1A)",
          "[model][effects][master][interchange]") {
    // ARGON-06: FCPXML and OTIO write an audition's other takes; flattening folded the master clip effects onto the
    // active take only, so a take made active in the other application lost its media's look.
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    const ClipId takes[] = {tl.clip("V1@0"), tl.clip("V1@4")};
    ProjectState s = ok(edit::createAudition(tl.state, tl.seq, takes)).state;
    s = ok(edit::addEffect(s, tl.seq, EffectTarget::ofMedia(tl.media('B')), "wabcut.blur.gaussian")).state;
    const Clip& audition = clipIn(s, tl.seq, tl.clip("V1@0"));
    REQUIRE(audition.audition());
    REQUIRE(audition.audition()->others.size() == 1);
    REQUIRE(audition.audition()->others.front().media == tl.media('B'));
    std::size_t folded = 0;
    const ProjectState flat = flattenMasterEffects(s, &folded);
    CHECK(folded == 1);
    const Clip& flatAudition = clipIn(flat, tl.seq, tl.clip("V1@0"));
    REQUIRE(flatAudition.audition());
    const EffectStack& takeEffects = flatAudition.audition()->others.front().contentOrDefault().effects;
    REQUIRE(takeEffects.size() == 1);
    CHECK(takeEffects.front().effect == "wabcut.blur.gaussian");
    CHECK(flatAudition.effects().empty()); // the active take (A) has no master clip effects
    CHECK(flat.findMedia(tl.media('B'))->masterEffects.empty());
}
