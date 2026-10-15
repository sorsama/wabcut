// C.15, the parity pack on the model side: blend modes, synthetic media, adjustment layers, the composite plan
// (adjustment ordering inside nests, track mattes) and the preview render cache (render bar, content hash, index and
// exact invalidation).

#include "TimelineFixture.h"

#include <wabcut/model/Blend.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/model/Synthetic.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

ErrorCode errorOf(const edit::EditOutcome& outcome) {
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error().code;
}

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId id) { return state.findSequence(seq)->findClip(id)->clip(); }

std::filesystem::path freshFolder(const char* name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "parity" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& file, std::string_view text) {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

} // namespace

// ---- Blend modes --------------------------------------------------------------------------------------------------

TEST_CASE("Blend modes: Premiere's 27 in menu order with stable names (C.15)", "[model][parity][blend]") {
    const auto modes = blendModesInMenuOrder();
    REQUIRE(modes.size() == kBlendModeCount);
    CHECK(modes.front() == BlendMode::Normal);
    CHECK(modes.back() == BlendMode::Luminosity);
    std::set<std::string_view> names;
    for (const BlendMode m : modes) {
        names.insert(toString(m));
        CHECK(blendModeFromString(toString(m)) == m);
        CHECK_FALSE(displayName(m).empty());
    }
    CHECK(names.size() == kBlendModeCount);
    // The C.13 graphic names keep their meaning.
    CHECK(blendModeFromString("add") == BlendMode::Add);
    CHECK(displayName(BlendMode::Add) == "Linear Dodge (Add)");
    CHECK(blendModeFromString("difference") == BlendMode::Difference);
    CHECK_FALSE(blendModeFromString("Normal").has_value()); // names are case-sensitive
    CHECK(blendModeGroup(BlendMode::ColorBurn) == 1);
    CHECK(blendModeGroup(BlendMode::Luminosity) == 5);
}

TEST_CASE("A clip's blend mode is set on video clips only and saved only when not normal (C.15)", "[model][parity][blend]") {
    Timeline tl(R"(
        V1 |Aaaa|
        A1 |Aaaa|
    )");
    const ClipId v = tl.clip("V1@0");
    const ClipId a = tl.clip("A1@0");
    const std::string plain = io::toJson(tl.state).value();
    CHECK(plain.find("blendMode") == std::string::npos);

    edit::ClipAttributeChange screen;
    screen.blendMode = BlendMode::Screen;
    const edit::EditResult r = ok(edit::setClipAttributes(tl.state, tl.seq, std::vector{v}, screen));
    CHECK(clipOf(r.state, tl.seq, v).blendMode() == BlendMode::Screen);
    REQUIRE(r.changes.sequences.size() == 1);
    CHECK(r.changes.sequences[0].roles.has(PropertyRole::Opacity));
    CHECK(errorOf(edit::setClipAttributes(tl.state, tl.seq, std::vector{a}, screen)) == ErrorCode::InvalidArgument);

    // Saved by name, read back; an unknown name is refused on load.
    const std::string saved = io::toJson(r.state).value();
    CHECK(saved.find("\"blendMode\": \"screen\"") != std::string::npos);
    const auto back = io::fromJson(saved);
    REQUIRE(back.has_value());
    CHECK(clipOf(*back, tl.seq, v).blendMode() == BlendMode::Screen);
    std::string broken = saved;
    broken.replace(broken.find("\"screen\""), 8, "\"glow!!\"");
    CHECK_FALSE(io::fromJson(broken).has_value());
    // Back to normal: the file is as before.
    edit::ClipAttributeChange normal;
    normal.blendMode = BlendMode::Normal;
    CHECK(io::toJson(ok(edit::setClipAttributes(r.state, tl.seq, std::vector{v}, normal)).state).value() == plain);
}

TEST_CASE("Paste attributes carries the blend mode with Opacity and Remove Attributes resets it (C.15)", "[model][parity][blend]") {
    Timeline tl(R"(
        V1 |Aaaa..Bbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@6");
    edit::ClipAttributeChange multiply;
    multiply.blendMode = BlendMode::Multiply;
    ProjectState s = ok(edit::setClipAttributes(tl.state, tl.seq, std::vector{a}, multiply)).state;
    AttributeSet opacity;
    opacity.opacity = true;
    const auto pasted = pasteAttributes(s, tl.seq, clipOf(s, tl.seq, a), std::vector{b}, opacity);
    REQUIRE(pasted.has_value());
    CHECK(clipOf(pasted->edit.state, tl.seq, b).blendMode() == BlendMode::Multiply);
    const auto removed = removeAttributes(pasted->edit.state, tl.seq, std::vector{b}, opacity);
    REQUIRE(removed.has_value());
    CHECK(clipOf(removed->edit.state, tl.seq, b).blendMode() == BlendMode::Normal);
}

// ---- Synthetic media -------------------------------------------------------------------------------------------------

TEST_CASE("Synthetic items are made for the sequence with checked parameters (C.15)", "[model][parity][synthetic]") {
    Timeline tl("    V1 |Aaaa|\n");
    for (const SyntheticDescriptor& d : syntheticKinds()) {
        INFO(d.shortName);
        const auto added = edit::addSyntheticMedia(tl.state, d.kind, {}, tl.seq);
        REQUIRE(added.has_value());
        const MediaItem& item = *added->state.findMedia(added->id);
        CHECK(item.isGenerator());
        CHECK(syntheticOf(item) == &d);
        CHECK(item.name == d.displayName);
        REQUIRE(item.info.video.size() == 1);
        CHECK(item.info.video[0].width == tl.sequence().width);
        CHECK(item.info.video[0].height == tl.sequence().height);
        CHECK(item.info.video[0].frameRate == tl.sequence().frameRate);
        CHECK(item.info.audio.empty());
        CHECK(item.info.isStillImage);
        CHECK(item.info.video[0].alpha == (d.kind == SyntheticKind::TransparentVideo ? AlphaMode::Straight : AlphaMode::None));
        CHECK_FALSE(syntheticProblem(item).has_value());
        requireValid(added->state);
    }
    const auto matte = edit::addSyntheticMedia(tl.state, SyntheticKind::ColorMatte,
                                               {EffectParam{"color.r", 0.2}, EffectParam{"color.b", 0.9}}, tl.seq, {}, "Blue");
    REQUIRE(matte.has_value());
    const MediaItem& blue = *matte->state.findMedia(matte->id);
    CHECK(blue.name == "Blue");
    CHECK(syntheticParam(blue, "color.r") == Approx(0.2));
    CHECK(syntheticParam(blue, "color.g") == Approx(1.0)); // the default
    CHECK(blue.generator->params.size() == 2);             // only what was given
    // Bad parameters: unknown, out of range, keyframed.
    CHECK_FALSE(edit::addSyntheticMedia(tl.state, SyntheticKind::ColorMatte, {EffectParam{"glow", 1.0}}, tl.seq).has_value());
    CHECK_FALSE(edit::addSyntheticMedia(tl.state, SyntheticKind::ColorMatte, {EffectParam{"color.g", 1.5}}, tl.seq).has_value());
    CHECK_FALSE(edit::addSyntheticMedia(tl.state, SyntheticKind::BlackVideo, {EffectParam{"color.r", 0.5}}, tl.seq).has_value());
    Curve keyed{{Keyframe{Ticks{}, 0.0}, Keyframe{f(5), 1.0}}};
    CHECK_FALSE(edit::addSyntheticMedia(tl.state, SyntheticKind::ColorMatte, {EffectParam{"color.r", keyed}}, tl.seq).has_value());
    CHECK(edit::addSyntheticMedia(tl.state, SyntheticKind::ColorMatte, {}, SequenceId{999}).error().code == ErrorCode::NotFound);

    // Saved with its parameters, read back equal and valid; an edited file with a bad parameter is refused.
    const std::string saved = io::toJson(matte->state).value();
    const auto back = io::fromJson(saved);
    REQUIRE(back.has_value());
    CHECK(*back->findMedia(matte->id) == blue);
    std::string broken = saved;
    const std::size_t at = broken.find("0.9", broken.find("\"color.b\"")); // "color.b": {"value": 0.9}
    REQUIRE(at != std::string::npos);
    broken.replace(at, 3, "9.0");
    const auto loaded = io::fromJson(broken);
    CHECK((!loaded.has_value() || !validate(*loaded, Validation::Structure).has_value()));
}

// ---- Adjustment layers --------------------------------------------------------------------------------------------

TEST_CASE("Adjustment layers go on video tracks as new or reused items (C.15)", "[model][parity][adjustment]") {
    Timeline tl(R"(
        V2      |..........|
        V1      |Aaaaaaaaaa|
        V3 lock |..........|
        A1      |Aaaaaaaaaa|
    )");
    const edit::EditResult r = ok(edit::addAdjustmentLayer(tl.state, tl.seq, tl.track("V2"), f(2), f(4)));
    REQUIRE(r.placed.size() == 1);
    REQUIRE(r.createdMedia.isValid());
    const MediaItem& item = *r.state.findMedia(r.createdMedia);
    CHECK(isAdjustmentLayer(item));
    CHECK(item.name == "Adjustment Layer");
    CHECK(item.info.isStillImage);
    const Clip& clip = clipOf(r.state, tl.seq, r.placed.front());
    CHECK(clip.start() == f(2));
    CHECK(clip.duration() == f(4));
    // Reuse the item; refuse the wrong track kind, a locked track, an empty range and media that is not an adjustment layer.
    const edit::EditResult again = ok(edit::addAdjustmentLayer(r.state, tl.seq, tl.track("V2"), f(7), f(2), r.createdMedia));
    CHECK_FALSE(again.createdMedia.isValid());
    CHECK(clipOf(again.state, tl.seq, again.placed.front()).media == r.createdMedia);
    CHECK(errorOf(edit::addAdjustmentLayer(tl.state, tl.seq, tl.track("A1"), f(0), f(2))) == ErrorCode::InvalidArgument);
    CHECK(errorOf(edit::addAdjustmentLayer(tl.state, tl.seq, tl.track("V3"), f(0), f(2))) == ErrorCode::InvalidArgument);
    CHECK(errorOf(edit::addAdjustmentLayer(tl.state, tl.seq, tl.track("V2"), f(0), Ticks{})) == ErrorCode::InvalidArgument);
    CHECK(errorOf(edit::addAdjustmentLayer(tl.state, tl.seq, tl.track("V2"), f(0), f(2), tl.media('A'))) ==
          ErrorCode::InvalidArgument);
    // Saved and read back; an adjustment item with an audio stream is invalid.
    const auto back = io::fromJson(io::toJson(again.state).value());
    REQUIRE(back.has_value());
    CHECK(isAdjustmentLayer(*back->findMedia(r.createdMedia)));
    MediaItem broken = item;
    broken.info.audio.push_back(AudioStreamInfo{});
    CHECK(syntheticProblem(broken).has_value());
}

// ---- The composite plan -------------------------------------------------------------------------------------------

TEST_CASE("The composite plan orders layers and adjustments bottom to top (C.15)", "[model][parity][plan]") {
    Timeline tl(R"(
        V3 |....Bbbb..|
        V2 |..........|
        V1 |Aaaaaaaaaa|
    )");
    ProjectState s = ok(edit::addAdjustmentLayer(tl.state, tl.seq, tl.track("V2"), f(2), f(6))).state;
    const ClipId adjust = clipAt(s, tl.seq, "V2@2");
    s = ok(edit::addEffect(s, tl.seq, std::vector{adjust}, "wabcut.blur.gaussian")).state;
    edit::ClipAttributeChange screen;
    screen.blendMode = BlendMode::Screen;
    s = ok(edit::setClipAttributes(s, tl.seq, std::vector{tl.clip("V3@4")}, screen)).state;

    const CompositePlan at5 = resolveComposite(s, tl.seq, f(5));
    REQUIRE(at5.steps.size() == 3);
    CHECK(at5.steps[0].kind == CompositeStepKind::Layer);
    CHECK(at5.steps[0].clip->id == tl.clip("V1@0"));
    CHECK(at5.steps[1].isAdjustment());
    CHECK(at5.steps[1].clip->id == adjust);
    CHECK(at5.steps[1].clip->effects().size() == 1);
    CHECK(at5.steps[1].opacity == 1.0);
    CHECK(at5.steps[2].clip->id == tl.clip("V3@4"));
    CHECK(at5.steps[2].blend == BlendMode::Screen);
    CHECK(at5.masterEffects == nullptr);
    CHECK_FALSE(at5.truncated);
    // Outside the adjustment layer: no adjust step.
    const CompositePlan at1 = resolveComposite(s, tl.seq, f(1));
    REQUIRE(at1.steps.size() == 1);
    CHECK_FALSE(at1.steps[0].isAdjustment());
    // A switched-off track is not composited.
    edit::TrackStateChange off;
    off.enabled = false;
    const ProjectState hidden = ok(edit::setTrackState(s, tl.seq, tl.track("V2"), off)).state;
    CHECK(resolveComposite(hidden, tl.seq, f(5)).steps.size() == 2);
    CHECK(resolveComposite(s, SequenceId{999}, f(5)).steps.empty());
}

TEST_CASE("Inside a nest an adjustment layer adjusts only the nest's lower tracks (C.15)", "[model][parity][plan]") {
    Timeline tl(R"(
        V2 |Bbbbbbbbbb|
        V1 |Aaaaaaaaaa|
    )");
    // Nest B (on V2); inside the nest B stays on V2. Add V3 inside with an adjustment layer and a master effect.
    const auto nest = edit::nestClips(tl.state, tl.seq, std::vector{tl.clip("V2@0")}, "Inner");
    REQUIRE(nest.has_value());
    ProjectState s = nest->edit.state;
    const SequenceId inner = nest->nested;
    const auto v3 = edit::addTrack(s, inner, TrackKind::Video);
    REQUIRE(v3.has_value());
    s = v3->state;
    s = ok(edit::addAdjustmentLayer(s, inner, v3->id, f(0), f(10))).state;
    s = ok(edit::addEffect(s, inner, edit::EffectTarget::masterVideo(), "wabcut.color.basic")).state;

    const CompositePlan plan = resolveComposite(s, tl.seq, f(5));
    REQUIRE(plan.steps.size() == 2);
    CHECK(plan.steps[0].clip->id == tl.clip("V1@0")); // A is below the nest: never adjusted by it
    CHECK_FALSE(plan.steps[0].isAdjustment());
    const CompositeStep& nested = plan.steps[1];
    CHECK(nested.clip->isNested());
    REQUIRE(nested.nested);
    CHECK(nested.nested->sequence == inner);
    CHECK(nested.nested->time == nested.sourceTime);
    REQUIRE(nested.nested->steps.size() == 2);
    CHECK(nested.nested->steps[0].clip->media == tl.media('B'));
    CHECK(nested.nested->steps[1].isAdjustment());
    REQUIRE(nested.nested->masterEffects != nullptr);
    CHECK(nested.nested->masterEffects->size() == 1);
    CHECK(plan.stepCount() == 4);
    // Every step of the top level is the top level's.
    for (const CompositeStep& step : plan.steps) {
        CHECK(step.sequence == tl.seq);
    }
}

TEST_CASE("A track matte reads its track whatever the track's output switch (C.15)", "[model][parity][plan]") {
    Timeline tl(R"(
        V3 |Cccccccccc|
        V2 |Bbbbbbbbbb|
        V1 |Aaaaaaaaaa|
    )");
    ProjectState s = ok(edit::addEffect(tl.state, tl.seq, std::vector{tl.clip("V2@0")}, std::string(kTrackMatteEffect),
                                        {EffectParam{"matteTrack", 3.0}, EffectParam{"compositeUsing", 1.0}}))
                         .state;
    edit::TrackStateChange off;
    off.enabled = false;
    s = ok(edit::setTrackState(s, tl.seq, tl.track("V3"), off)).state; // hide the matte itself

    const CompositePlan plan = resolveComposite(s, tl.seq, f(4));
    REQUIRE(plan.steps.size() == 2); // V3 is off: not composited
    const CompositeStep& keyed = plan.steps[1];
    CHECK(keyed.clip->id == tl.clip("V2@0"));
    REQUIRE(keyed.matte.has_value());
    CHECK(keyed.matte->trackNumber == 3);
    CHECK(keyed.matte->track == tl.track("V3"));
    CHECK(keyed.matte->luma);
    CHECK_FALSE(keyed.matte->reverse);
    REQUIRE(keyed.matte->plan);
    REQUIRE(keyed.matte->plan->steps.size() == 1);
    CHECK(keyed.matte->plan->steps[0].clip->id == tl.clip("V3@0"));
    // A matte naming its own track or a missing one keys nothing (no plan).
    for (const double track : {2.0, 9.0, 0.0}) {
        const ProjectState other = ok(edit::addEffect(tl.state, tl.seq, std::vector{tl.clip("V2@0")}, std::string(kTrackMatteEffect),
                                                      {EffectParam{"matteTrack", track}}))
                                       .state;
        const CompositePlan p = resolveComposite(other, tl.seq, f(4));
        REQUIRE(p.steps[1].matte.has_value());
        CHECK_FALSE(p.steps[1].matte->track.isValid());
        CHECK(p.steps[1].matte->plan == nullptr);
    }
}

// ---- The preview render cache ---------------------------------------------------------------------------------------

TEST_CASE("The render bar: none for plain clips - yellow for effects - red for adjustments and nests (C.15)", "[model][parity][render]") {
    Timeline tl(R"(
        V2 |..........Cccc|
        V1 |AaaaBbbb..Dddd|
    )");
    ProjectState s = ok(edit::addEffect(tl.state, tl.seq, std::vector{tl.clip("V1@4")}, "wabcut.blur.gaussian")).state;
    s = ok(edit::addAdjustmentLayer(s, tl.seq, tl.track("V2"), f(0), f(2))).state;
    const auto nest = edit::nestClips(s, tl.seq, std::vector{tl.clip("V1@10"), tl.clip("V2@10")}, "Nest");
    REQUIRE(nest.has_value());
    s = nest->edit.state;
    const std::vector<RenderSegment> bar = renderSegments(s, tl.seq, TimeRange{Ticks{}, f(14)});
    // Cuts at 0, 2, 4, 8, 10, 14.
    REQUIRE(bar.size() == 5);
    CHECK(bar[0].range == TimeRange{Ticks{}, f(2)});
    CHECK(bar[0].status == RenderStatus::Red); // under the adjustment layer
    CHECK(bar[0].reason == "adjustment layer");
    CHECK(bar[1].status == RenderStatus::None); // A alone
    CHECK(bar[2].status == RenderStatus::Yellow); // B with a blur
    CHECK(bar[2].reason.find("Gaussian Blur") != std::string::npos);
    CHECK(bar[3].status == RenderStatus::None); // the gap
    CHECK(bar[4].status == RenderStatus::Red); // the nest
    CHECK(bar[4].reason == "nested sequence");
    // Many heavy effects: red although nothing is nested.
    ProjectState heavy = tl.state;
    for (int i = 0; i < 3; ++i) {
        heavy = ok(edit::addEffect(heavy, tl.seq, std::vector{tl.clip("V1@0")}, std::string(kUltraKeyEffect))).state;
    }
    const std::vector<RenderSegment> heavyBar = renderSegments(heavy, tl.seq, TimeRange{Ticks{}, f(4)});
    REQUIRE(heavyBar.size() == 1);
    CHECK(heavyBar[0].status == RenderStatus::Red);
    CHECK(heavyBar[0].cost > kRealTimeBudget);
    CHECK(effectRenderCost("plugin.unknown") == 1.0);
}

TEST_CASE("Segment content hashes change with what shows there and nothing else (C.15)", "[model][parity][render]") {
    Timeline tl(R"(
        V1 |AaaaBbbb|
        A1 |AaaaBbbb|
    )");
    const TimeRange first{Ticks{}, f(4)};
    const TimeRange second{f(4), f(4)};
    const std::string h1 = renderContentHash(tl.state, tl.seq, first);
    const std::string h2 = renderContentHash(tl.state, tl.seq, second);
    CHECK(h1.size() == 16);
    CHECK(h1 != h2);
    CHECK(renderContentHash(tl.state, tl.seq, first) == h1); // stable
    // A rename, a label colour and audio do not change the pixels.
    edit::ClipAttributeChange rename;
    rename.name = "Interview";
    rename.colorIndex = 3;
    ProjectState s = ok(edit::setClipAttributes(tl.state, tl.seq, std::vector{tl.clip("V1@0")}, rename)).state;
    edit::ClipAttributeChange quiet;
    ClipProperties p = tl.sequence().audioTracks[0]->clips[0].properties();
    p.set(ParamId::VolumeDb, -6.0);
    quiet.properties = p;
    s = ok(edit::setClipAttributes(s, tl.seq, std::vector{tl.clip("A1@0")}, quiet)).state;
    CHECK(renderContentHash(s, tl.seq, first) == h1);
    // An opacity change shows.
    edit::ClipAttributeChange fade;
    ClipProperties q = tl.sequence().videoTracks[0]->clips[0].properties();
    q.set(ParamId::Opacity, 0.5);
    fade.properties = q;
    const ProjectState faded = ok(edit::setClipAttributes(tl.state, tl.seq, std::vector{tl.clip("V1@0")}, fade)).state;
    CHECK(renderContentHash(faded, tl.seq, first) != h1);
    CHECK(renderContentHash(faded, tl.seq, second) == h2);
    CHECK(renderContentHash(tl.state, SequenceId{999}, first).empty());
}

namespace {

/// Writes a fake preview file and records it.
void recordFake(PreviewCache& cache, const ProjectState& state, SequenceId seq, TimeRange range, std::int64_t bytes = 100) {
    const std::string hash = renderContentHash(state, seq, range);
    const std::string file = PreviewCache::fileNameFor(seq, range, hash);
    writeFile(cache.folder() / file, std::string(static_cast<std::size_t>(bytes), 'x'));
    cache.record(PreviewEntry{seq, range, hash, file, bytes, 0});
}

} // namespace

TEST_CASE("Preview files stay valid through edits outside them and go with edits inside (C.15)", "[model][parity][render]") {
    Timeline tl(R"(
        V1 |AaaaBbbb..Cccc|
        A1 |AaaaBbbb..Cccc|
    )");
    const SequenceId seq = tl.seq;
    PreviewCache cache(freshFolder("invalidate"));
    const TimeRange first{Ticks{}, f(4)};
    const TimeRange second{f(4), f(4)};
    recordFake(cache, tl.state, seq, first);
    recordFake(cache, tl.state, seq, second);
    REQUIRE(cache.entries().size() == 2);
    const auto bar = cache.renderBar(tl.state, seq, TimeRange{Ticks{}, f(8)});
    REQUIRE(bar.size() == 2);
    CHECK(bar[0].status == RenderStatus::Rendered);
    CHECK(bar[1].status == RenderStatus::Rendered);
    CHECK(cache.lookup(seq, f(5))->range == second);
    CHECK_FALSE(cache.lookup(seq, f(9)).has_value());

    // An edit outside both segments (C, later) keeps them without re-rendering.
    edit::ClipAttributeChange fade;
    ClipProperties p = tl.sequence().videoTracks[0]->clips[2].properties();
    p.set(ParamId::Opacity, 0.5);
    fade.properties = p;
    edit::EditResult outside = ok(edit::setClipAttributes(tl.state, seq, std::vector{tl.clip("V1@10")}, fade));
    CHECK(cache.invalidate(outside.state, outside.changes).empty());
    CHECK(cache.entries().size() == 2);
    // A rename inside keeps it too (the hash decides, not the ChangeSet).
    edit::ClipAttributeChange rename;
    rename.name = "Renamed";
    edit::EditResult renamed = ok(edit::setClipAttributes(outside.state, seq, std::vector{tl.clip("V1@4")}, rename));
    CHECK(cache.invalidate(renamed.state, renamed.changes).empty());
    // An edit inside the second segment drops it and deletes its file; the first stays.
    const std::string secondFile = cache.entries()[1].file;
    REQUIRE(std::filesystem::exists(cache.folder() / secondFile));
    edit::EditResult inside = ok(edit::setClipAttributes(renamed.state, seq, std::vector{tl.clip("V1@4")}, fade));
    const std::vector<PreviewEntry> dropped = cache.invalidate(inside.state, inside.changes);
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].range == second);
    CHECK_FALSE(std::filesystem::exists(cache.folder() / secondFile));
    REQUIRE(cache.entries().size() == 1);
    CHECK(cache.entries()[0].range == first);
    const auto after = cache.renderBar(inside.state, seq, TimeRange{Ticks{}, f(8)});
    CHECK(after[0].status == RenderStatus::Rendered);
    CHECK(after[1].status != RenderStatus::Rendered);

    // An edit inside a nested sequence a segment plays invalidates that segment.
    const auto nest = edit::nestClips(inside.state, seq, std::vector{tl.clip("V1@0")}, "Inner");
    REQUIRE(nest.has_value());
    PreviewCache nested(freshFolder("nested"));
    const ProjectState n = nest->edit.state;
    recordFake(nested, n, seq, first);
    const Clip& inner = n.findSequence(nest->nested)->videoTracks[0]->clips[0];
    edit::EditResult deep = ok(edit::setClipAttributes(n, nest->nested, std::vector{inner.id}, fade));
    CHECK(nested.invalidate(deep.state, deep.changes).size() == 1);
}

TEST_CASE("The preview index is bounded and keeps a size budget (C.15)", "[model][parity][render]") {
    Timeline tl("    V1 |AaaaBbbbCccc|\n");
    const std::filesystem::path folder = freshFolder("index");
    {
        PreviewCache cache(folder, 250);
        recordFake(cache, tl.state, tl.seq, TimeRange{Ticks{}, f(4)});
        recordFake(cache, tl.state, tl.seq, TimeRange{f(4), f(4)});
        CHECK(cache.lookup(tl.seq, f(1)).has_value()); // A was used last: B goes first
        recordFake(cache, tl.state, tl.seq, TimeRange{f(8), f(4)});
        REQUIRE(cache.entries().size() == 2);
        CHECK(cache.totalBytes() == 200);
        CHECK_FALSE(cache.lookup(tl.seq, f(5)).has_value());
        REQUIRE(cache.save().has_value());
    }
    PreviewCache reread(folder, 250);
    REQUIRE(reread.load().has_value());
    REQUIRE(reread.entries().size() == 2);
    CHECK(reread.remove(tl.seq).size() == 2);
    CHECK(reread.entries().empty());

    // Hostile indexes are errors, never crashes or files outside the folder.
    for (const char* text : {"", "[]", "{\"format\":\"wabcut-preview-cache\",\"version\":2,\"entries\":[]}",
                             R"({"format":"wabcut-preview-cache","version":1,"entries":[{"sequence":1,"start":0,"duration":10,"hash":"0123456789abcdef","file":"../evil","bytes":1,"lastUse":1}]})",
                             R"({"format":"wabcut-preview-cache","version":1,"entries":[{"sequence":1,"start":-5,"duration":10,"hash":"0123456789abcdef","file":"a.wpv","bytes":1,"lastUse":1}]})",
                             R"({"format":"wabcut-preview-cache","version":1,"entries":[{"sequence":1,"start":0,"duration":10,"hash":"XYZ","file":"a.wpv","bytes":1,"lastUse":1}]})",
                             R"({"format":"wabcut-preview-cache","version":1,"entries":[{"sequence":1,"start":0,"duration":10,"hash":"0123456789abcdef","file":"a.wpv","bytes":1,"lastUse":1},{"sequence":1,"start":5,"duration":10,"hash":"0123456789abcdef","file":"b.wpv","bytes":1,"lastUse":2}]})"}) {
        INFO(text);
        CHECK_FALSE(PreviewCache::parseIndex(text).has_value());
    }
    const std::vector<PreviewEntry> one{PreviewEntry{SequenceId{3}, TimeRange{f(1), f(2)}, "0123456789abcdef", "3-1-x.wpv", 7, 9}};
    const auto parsed = PreviewCache::parseIndex(PreviewCache::indexJson(one));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == one);
    CHECK(PreviewCache::isPlainFileName("12-0-abc.wpv"));
    CHECK_FALSE(PreviewCache::isPlainFileName(".hidden"));
    CHECK_FALSE(PreviewCache::isPlainFileName("a/b"));
    CHECK_FALSE(PreviewCache::isPlainFileName("a\\b"));
}

// ---- Marker highlights (G.2) -----------------------------------------------------------------------------------------

TEST_CASE("Marker highlights are bounded - saved only when set - and read back (C.15)", "[model][parity][highlight]") {
    Timeline tl("    V1 |Aaaaaaaaaa|\n");
    Marker m{f(2), f(4), "Clutch"};
    const std::string plain = io::toJson(ok(edit::addMarker(tl.state, tl.seq, m)).state).value();
    CHECK(plain.find("highlight") == std::string::npos);
    m.highlight = MarkerHighlight{0.87, HighlightConfidence::High, {"kill streak", "mic peak"}, f(3), "highlights"};
    CHECK_FALSE(markerProblem(m).has_value());
    const ProjectState s = ok(edit::addMarker(tl.state, tl.seq, m)).state;
    const std::string saved = io::toJson(s).value();
    const auto back = io::fromJson(saved);
    REQUIRE(back.has_value());
    REQUIRE(back->findSequence(tl.seq)->markers.size() == 1);
    CHECK(back->findSequence(tl.seq)->markers[0] == m);
    // Bounds.
    const auto broken = [&](auto change) {
        Marker bad = m;
        change(*bad.highlight);
        return markerProblem(bad).has_value();
    };
    CHECK(broken([](MarkerHighlight& h) { h.score = 1.5; }));
    CHECK(broken([](MarkerHighlight& h) { h.score = -0.1; }));
    CHECK(broken([&](MarkerHighlight& h) { h.peak = f(7); })); // the marker ends at 6
    CHECK(broken([&](MarkerHighlight& h) { h.peak = f(1); }));
    CHECK(broken([](MarkerHighlight& h) { h.reasons.assign(9, "x"); }));
    CHECK(broken([](MarkerHighlight& h) { h.reasons = {std::string(201, 'x')}; }));
    CHECK(broken([](MarkerHighlight& h) { h.reasons = {""}; }));
    CHECK(broken([](MarkerHighlight& h) { h.source.clear(); }));
    CHECK(broken([](MarkerHighlight& h) { h.source = std::string(65, 's'); }));
    // A hostile file is refused.
    std::string hostile = saved;
    hostile.replace(hostile.find("0.87"), 4, "2.50");
    CHECK_FALSE(io::fromJson(hostile).has_value());
    CHECK(highlightConfidenceFromString("high") == HighlightConfidence::High);
    CHECK_FALSE(highlightConfidenceFromString("certain").has_value());
}
