// C.8 nested sequences and multicam in video evaluation (I.8; docs/internal/c8-nested-multicam-handoff.md): a nested or multicam
// clip becomes a group of LayerSpecs - the nested sequence's layers at the clip's mapped time - and the leaves the video
// evaluator decodes are exactly the model resolver's (resolveSequenceLeaves), frame by frame, through transitions inside
// and outside the nest, speed, reverse, remap holds, nests of nests, disabled nested tracks, the past-the-end rule and
// multicam angle cuts. Redactions follow the leaf's media unless a clip on its path excepts them; the nested
// sequence's master effects become a group of their own; a leaf shown twice decodes from two sessions; and the
// composite layers nest the same way.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using wabcut::playback::LayerSpec;

namespace {

std::string frameName(const ProjectState& state, MediaId media, Ticks t) {
    return state.findMedia(media)->name + std::to_string(t.value / f(1).value);
}

/// "A2 C3": the media letter and source frame of every leaf the video evaluator yields (V1 first, depth first).
std::string evaluated(const ProjectState& state, SequenceId seq, std::int64_t frame) {
    std::vector<LayerSpec> layers;
    playback::evaluateVideoLayers(state, *state.findSequence(seq), f(frame), layers, /*cullOccluded*/ false);
    std::string out;
    for (const LayerSpec& l : layers) {
        if (!l.nested && l.item) {
            out += (out.empty() ? "" : " ") + frameName(state, l.media, l.sourceTime);
        }
    }
    return out;
}

/// The same from the model's resolver.
std::string resolved(const ProjectState& state, SequenceId seq, std::int64_t frame) {
    std::string out;
    for (const ResolvedLeaf& l : resolveSequenceLeaves(state, seq, StreamKind::Video, f(frame))) {
        out += (out.empty() ? "" : " ") + frameName(state, l.clip->media, l.sourceTime);
    }
    return out;
}

/// Every frame of `seq` (and a few past its end): the evaluator's leaves are the resolver's.
void requireAgreement(const ProjectState& state, SequenceId seq, std::int64_t frames) {
    for (std::int64_t k = 0; k < frames + 3; ++k) {
        INFO("frame " << k);
        CHECK(evaluated(state, seq, k) == resolved(state, seq, k));
    }
}

/// The nest of the model tests: a dissolve at A|B on V1 and a blur on A, nested with C on V2.
struct Nested {
    Timeline tl{R"(
        V2     |..C3cc....|
        V1 tgt |AaaaB10b..|
        A1 tgt |AaaaB10b..|
    )"};
    ProjectState state;
    SequenceId nested;
    ClipId nestedClip;

    Nested() {
        tl.linkMatching();
        edit::TransitionSpec dissolve{tl.clip("V1@0"), tl.clip("V1@4"), f(1), f(1)};
        ProjectState before = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), dissolve)).state;
        const ClipId a = tl.clip("V1@0");
        before = ok(edit::addEffect(before, tl.seq, std::span<const ClipId>(&a, 1), "wabcut.blur.gaussian")).state;
        const std::vector<ClipId> selection{tl.clip("V1@0"), tl.clip("V1@4"), tl.clip("V2@2")};
        auto r = edit::nestClips(before, tl.seq, selection, "Nest");
        REQUIRE(r);
        state = r->edit.state;
        nested = r->nested;
        nestedClip = r->clips.front();
    }
};

const LayerSpec* headOf(const std::vector<LayerSpec>& layers, ClipId clip) {
    for (const LayerSpec& l : layers) {
        if (l.nested && l.clip == clip) {
            return &l;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Nested video: the evaluator decodes exactly the resolver's leaves, frame by frame", "[playback][nested]") {
    Nested n;
    requireAgreement(n.state, n.tl.seq, 10);
    CHECK(evaluated(n.state, n.tl.seq, 4) == "A4 B10 C5"); // inside the nested dissolve
    CHECK(evaluated(n.state, n.tl.seq, 9).empty());        // past the nested clip

    SECTION("speed, reverse and a remap hold on the nested clip") {
        edit::SpeedChange twice;
        twice.speed = Speed{2, 1};
        const ProjectState fast = ok(edit::setSpeed(n.state, n.tl.seq, std::vector<ClipId>{n.nestedClip}, twice)).state;
        requireAgreement(fast, n.tl.seq, 5);
        CHECK(evaluated(fast, n.tl.seq, 3) == "B12");

        edit::SpeedChange back;
        back.reversed = true;
        const ProjectState reversed = ok(edit::setSpeed(n.state, n.tl.seq, std::vector<ClipId>{n.nestedClip}, back)).state;
        requireAgreement(reversed, n.tl.seq, 8);
        CHECK(evaluated(reversed, n.tl.seq, 0) == "B13");

        ProjectState remapped = ok(edit::setTimeRemapEnabled(n.state, n.tl.seq, n.nestedClip, true)).state;
        remapped = ok(edit::addTimeRemapHold(remapped, n.tl.seq, n.nestedClip, f(2), f(3))).state;
        requireAgreement(remapped, n.tl.seq, 12);
        CHECK(evaluated(remapped, n.tl.seq, 4) == "A2 C3"); // the held nested frame
    }
    SECTION("a nest of the nest, and a disabled nested track") {
        const auto twice = edit::nestClips(n.state, n.tl.seq, std::vector<ClipId>{n.nestedClip}, "Outer");
        REQUIRE(twice);
        requireAgreement(twice->edit.state, n.tl.seq, 10);
        std::vector<LayerSpec> layers;
        playback::evaluateVideoLayers(twice->edit.state, *twice->edit.state.findSequence(n.tl.seq), f(2), layers, false);
        // Outer group > inner group > A, C: the paths name both nested clips, outermost first.
        REQUIRE(layers.size() == 4);
        CHECK(layers[0].nested);
        CHECK(layers[0].nestedCount == 3);
        CHECK(layers[1].nested);
        CHECK(layers[1].nestedCount == 2);
        REQUIRE(layers[2].nestPath);
        CHECK(layers[2].nestPath->size() == 2);
        CHECK(layers[2].nestPath->front() == twice->clips.front());
        CHECK(layers[2].nestPath->back() == n.nestedClip);

        edit::TrackStateChange off;
        off.enabled = false;
        const ProjectState hidden =
            ok(edit::setTrackState(n.state, n.nested, n.state.findSequence(n.nested)->videoTracks[1]->id, off)).state;
        requireAgreement(hidden, n.tl.seq, 10);
        CHECK(evaluated(hidden, n.tl.seq, 2) == "A2");
    }
}

TEST_CASE("Nested video: the group's shape - placement, transitions inside, master effects, past the end",
          "[playback][nested]") {
    Nested n;
    std::vector<LayerSpec> layers;
    playback::evaluateVideoLayers(n.state, *n.state.findSequence(n.tl.seq), f(4), layers, false);
    // Group (the nested clip) + A, B (the nested dissolve, A carrying the group) + C.
    REQUIRE(layers.size() == 4);
    CHECK(layers[0].nested);
    CHECK(layers[0].clip == n.nestedClip);
    CHECK(layers[0].nestedCount == 3);
    CHECK(layers[0].nestedCanvas == render::CanvasSize{1920, 1080});
    CHECK(playback::nextSibling(layers, 0) == 4);
    CHECK_FALSE(playback::needsDecode(layers[0]));
    REQUIRE(layers[1].transition);
    CHECK(layers[1].transition->layerCount == 2);
    CHECK(layers[1].transition->progress == 0.5);
    CHECK(layers[1].effects.size() == 1); // A's blur, inside the nest
    CHECK(layers[0].effects.empty());

    // The nested clip's own Motion, Opacity and effects apply to the group.
    ProjectState moved =
        ok(edit::setParameter(n.state, n.tl.seq, n.nestedClip, edit::ParamRef::of(ParamId::Scale), ParamValue(0.5))).state;
    moved = ok(edit::setParameter(moved, n.tl.seq, n.nestedClip, edit::ParamRef::of(ParamId::Opacity), ParamValue(0.75))).state;
    moved = ok(edit::addEffect(moved, n.tl.seq, std::span<const ClipId>(&n.nestedClip, 1), "wabcut.crop")).state;
    playback::evaluateVideoLayers(moved, *moved.findSequence(n.tl.seq), f(4), layers, false);
    REQUIRE_FALSE(layers.empty());
    CHECK(layers[0].transform.scale == 0.5);
    CHECK(layers[0].opacity == 0.75);
    REQUIRE(layers[0].effects.size() == 1);
    CHECK(layers[0].effects[0].type == render::EffectType::Crop);

    // The nested sequence's master effects: a group of their own around the leaves (identity placement).
    auto withMaster = std::make_shared<Sequence>(*n.state.findSequence(n.nested));
    EffectInstance blur;
    blur.id = n.state.ids.allocate<EffectInstanceId>();
    blur.effect = "wabcut.blur.gaussian";
    blur.set("radius", ParamValue(8.0));
    withMaster->effects.push_back(blur);
    const ProjectState mastered = n.state.withSequence(withMaster);
    playback::evaluateVideoLayers(mastered, *mastered.findSequence(n.tl.seq), f(2), layers, false);
    REQUIRE(layers.size() == 4); // group, master group, A, C
    CHECK(layers[0].nestedCount == 3);
    CHECK(layers[1].nested);
    CHECK(layers[1].nestedCount == 2);
    CHECK(layers[1].transform == render::LayerTransform{});
    REQUIRE(layers[1].effects.size() == 1);
    CHECK(layers[1].effects[0].type == render::EffectType::GaussianBlur);

    // Past the nested sequence's end (shortened after the clip was cut) the clip shows nothing.
    auto shorter = std::make_shared<Sequence>(*n.state.findSequence(n.nested));
    shorter->duration = f(5);
    const ProjectState shortened = n.state.withSequence(shorter);
    CHECK(evaluated(shortened, n.tl.seq, 4) == "A4 B10 C5");
    CHECK(evaluated(shortened, n.tl.seq, 5).empty());
    CHECK(evaluated(shortened, n.tl.seq, 7).empty());
}

TEST_CASE("Nested video: redactions inside follow the media, and an exception anywhere on the path excepts them",
          "[playback][nested][redaction]") {
    Nested n;
    Redaction r;
    r.media = n.tl.media('A');
    r.name = "Plate";
    r.source = {f(1), f(2)};
    r.region.shape = MaskShape::Rectangle;
    r.region.set("center.x", 0.3);
    r.region.set("size.x", 0.2);
    const auto added = edit::addRedaction(n.state, r);
    REQUIRE(added);
    const auto redactionsOfA = [&](const ProjectState& s, std::int64_t frame) {
        std::vector<LayerSpec> layers;
        playback::evaluateVideoLayers(s, *s.findSequence(n.tl.seq), f(frame), layers, false);
        for (const LayerSpec& l : layers) {
            if (!l.nested && l.media == n.tl.media('A')) {
                return static_cast<int>(l.redactions.size());
            }
        }
        return -1;
    };
    CHECK(redactionsOfA(added->state, 2) == 1);
    CHECK(redactionsOfA(added->state, 0) == 0);
    // Excepting the nested clip excepts everything inside it; excepting the leaf itself does too.
    const auto byNest = edit::setRedactionOverride(added->state, added->id, n.nestedClip, false);
    REQUIRE(byNest);
    CHECK(redactionsOfA(*byNest, 2) == 0);
    const auto byLeaf = edit::setRedactionOverride(added->state, added->id, n.tl.clip("V1@0"), false);
    REQUIRE(byLeaf);
    CHECK(redactionsOfA(*byLeaf, 2) == 0);
    // The rule is the resolver's.
    const Clip& nestedClip = added->state.findSequence(n.tl.seq)->findClip(n.nestedClip)->clip();
    CHECK(resolveLeaves(*byNest, n.tl.seq, nestedClip, f(2)).front().redactions.empty());

    // The composite layers re-evaluate redactions at the displayed frame with the same path rule.
    std::vector<LayerSpec> specs;
    playback::evaluateVideoLayers(*byNest, *byNest->findSequence(n.tl.seq), f(2), specs, false);
    std::vector<media::VideoFrame> frames(specs.size());
    std::vector<render::CompositeLayer> composite;
    playback::buildCompositeLayers(specs, frames, composite);
    CHECK(composite.empty()); // nothing decoded: the group shows nothing (and nothing fails open)
}

TEST_CASE("Multicam video: the active angle at its sync, angle cuts frame-accurate", "[playback][multicam]") {
    Timeline tl(R"(
        V1 tgt |....................|
        A1 tgt |....................|
    )");
    edit::MulticamSpec spec;
    spec.name = "M";
    spec.angles = {tl.media('A'), tl.media('B'), tl.media('C'), tl.media('D')};
    spec.sync = MulticamSync::Manual;
    spec.offsets = {Ticks{}, f(2), f(5), f(1)}; // the angles started recording at different times
    const auto made = edit::createMulticam(tl.state, spec);
    REQUIRE(made);
    edit::SourceClip clip;
    clip.sequence = made->id;
    clip.sourceRange = {f(5), f(20)};
    clip.patches = edit::defaultSourcePatches(tl.sequence(), *made->state.findSequence(made->id));
    ProjectState s = ok(edit::overwriteEdit(made->state, tl.seq, clip, Ticks{})).state;
    // Cut to angles 2, 4 and 3 at frames 5, 9 and 14.
    s = ok(edit::switchMulticamAngle(s, tl.seq, clipAt(s, tl.seq, "V1@0"), f(5), 1, edit::AngleTarget::Video)).state;
    s = ok(edit::switchMulticamAngle(s, tl.seq, clipAt(s, tl.seq, "V1@5"), f(9), 3, edit::AngleTarget::Video)).state;
    s = ok(edit::switchMulticamAngle(s, tl.seq, clipAt(s, tl.seq, "V1@9"), f(14), 2, edit::AngleTarget::Video)).state;
    requireAgreement(s, tl.seq, 20);
    // Each angle's own frame at the cut: nested time 5 + k, minus the angle's offset.
    CHECK(evaluated(s, tl.seq, 4) == "A9");
    CHECK(evaluated(s, tl.seq, 5) == "B8");
    CHECK(evaluated(s, tl.seq, 8) == "B11");
    CHECK(evaluated(s, tl.seq, 9) == "D13");
    CHECK(evaluated(s, tl.seq, 13) == "D17");
    CHECK(evaluated(s, tl.seq, 14) == "C14");
    // A multicam clip shows its angle's track alone: no master stage, exactly one leaf.
    std::vector<LayerSpec> layers;
    playback::evaluateVideoLayers(s, *s.findSequence(tl.seq), f(6), layers, false);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].nested);
    CHECK(layers[0].nestedCount == 1);
}

TEST_CASE("Nested video: one leaf shown twice decodes from two sessions; top-level keys are clip ids",
          "[playback][nested]") {
    Nested n;
    // The same nested sequence a second time, on V2 over the first, from the same nested time: the same leaf twice.
    const Sequence& seq = *n.state.findSequence(n.tl.seq);
    ProjectState s = n.state;
    {
        auto copy = std::make_shared<Sequence>(seq);
        Track track = *seq.videoTracks[1];
        Clip second = n.state.findSequence(n.tl.seq)->findClip(n.nestedClip)->clip();
        second.id = s.ids.allocate<ClipId>();
        second.linkGroup = {};
        second.timelineRange = {Ticks{}, f(4)};
        second.sourceIn = Ticks{};
        track.clips = ClipList({second});
        copy->videoTracks[1] = std::make_shared<const Track>(std::move(track));
        s = s.withSequence(copy);
    }
    std::vector<LayerSpec> layers;
    playback::evaluateVideoLayers(s, *s.findSequence(n.tl.seq), f(1), layers, false);
    std::set<std::uint64_t> keys;
    int leaves = 0;
    for (const LayerSpec& l : layers) {
        if (playback::needsDecode(l)) {
            ++leaves;
            keys.insert(playback::sessionKey(l));
            CHECK(playback::sessionKey(l) != l.clip.value); // nested leaves never use the plain clip id
        }
    }
    CHECK(leaves == 2); // A1 through each nested clip
    CHECK(keys.size() == 2);
    CHECK(evaluated(s, n.tl.seq, 1) == "A1 A1");

    // A top-level media layer keeps its clip id as the key.
    Timeline flat(R"(
        V1 |Aaaa|
    )");
    playback::evaluateVideoLayers(flat.state, flat.sequence(), f(1), layers, false);
    REQUIRE(layers.size() == 1);
    CHECK(playback::sessionKey(layers[0]) == flat.clip("V1@0").value);
}

TEST_CASE("Nested video: composite layers nest the same way", "[playback][nested]") {
    // Hand-made specs: a group of two title-less leaves with frames, and a top-level leaf.
    std::vector<LayerSpec> specs(4);
    specs[0].nested = true;
    specs[0].nestedCount = 2;
    specs[0].nestedCanvas = {960, 540};
    specs[0].transform.scale = 0.5;
    specs[0].opacity = 0.8;
    specs[0].clip = ClipId{7};
    specs[1].clip = ClipId{8};
    specs[2].clip = ClipId{9};
    specs[3].clip = ClipId{10};
    std::vector<media::VideoFrame> frames(specs.size());
    const auto frame = [](int w, int h) {
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
        media::VideoFrame v;
        v.width = w;
        v.height = h;
        v.format = media::PixelFormat::RGBA8;
        v.duration = f(1);
        media::CpuPlanes cpu;
        cpu.planes[0] = {pixels->data(), w * 4, w, h};
        cpu.owner = pixels;
        v.payload = cpu;
        return v;
    };
    frames[1] = frame(64, 36);
    frames[2] = frame(32, 18);
    frames[3] = frame(16, 16);
    std::vector<render::CompositeLayer> out;
    playback::buildCompositeLayers(specs, frames, out);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].nested);
    CHECK(out[0].nested->canvas == render::CanvasSize{960, 540});
    CHECK(out[0].nested->layers.size() == 2);
    CHECK(out[0].transform.scale == 0.5);
    CHECK(out[0].opacity == 0.8);
    CHECK(out[0].clipId == 7);
    CHECK(out[1].clipId == 10);
    CHECK_FALSE(out[1].nested);

    // A group whose leaves all failed shows nothing, and a transition with it becomes one-sided.
    frames.assign(specs.size(), media::VideoFrame{});
    frames[3] = frame(16, 16);
    specs[0].transition = render::LayerTransition{};
    specs[0].transition->layerCount = 2;
    out.clear();
    playback::buildCompositeLayers(specs, frames, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].clipId == 10);
    REQUIRE(out[0].transition);
    CHECK(out[0].transition->layerCount == 1);
    CHECK(out[0].transition->incomingOnly);
}

TEST_CASE("Nesting fan-out is bounded: doubling nests are refused, and evaluation stops at its budget (review wave 5 "
          "E5-P2-9)",
          "[playback][nested]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    ProjectState s = tl.state;
    // 40 levels, each playing the next twice (two tracks): 2^40 leaves if expanded, yet acyclic and only 40 deep.
    SequenceId inner = tl.seq;
    for (int k = 0; k < 40; ++k) {
        auto seq = std::make_shared<Sequence>(*s.findSequence(tl.seq));
        seq->id = s.ids.allocate<SequenceId>();
        seq->name = "Level " + std::to_string(k);
        seq->videoTracks.clear();
        seq->audioTracks.clear();
        for (int t = 0; t < 2; ++t) {
            Track track;
            track.id = s.ids.allocate<TrackId>();
            track.kind = TrackKind::Video;
            Clip c;
            c.id = s.ids.allocate<ClipId>();
            c.sequence = inner;
            c.stream = {StreamKind::Video, 0};
            c.timelineRange = {Ticks{}, f(4)};
            track.clips = ClipList({c});
            seq->videoTracks.push_back(std::make_shared<const Track>(std::move(track)));
        }
        seq->duration = f(4);
        inner = seq->id;
        s.sequences.push_back(std::move(seq));
    }
    REQUIRE(nestingFanOutProblem(s).has_value()); // the file is refused on load
    CHECK_FALSE(validate(s, Validation::Structure).has_value());

    // Not validated (a state built in memory): evaluation and resolution stop at their budgets, fail closed.
    const auto start = std::chrono::steady_clock::now();
    std::vector<LayerSpec> layers;
    playback::evaluateVideoLayers(s, *s.findSequence(inner), f(1), layers, false);
    CHECK(layers.size() <= 4096);
    CHECK(resolveSequenceLeaves(s, inner, StreamKind::Video, f(1)).size() <= 4096);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    // A real project's nesting is far from the bound.
    CHECK_FALSE(nestingFanOutProblem(tl.state).has_value());
}

TEST_CASE("Video redactions: a subclip's clips are covered by its master's, nested ones too (review wave 5 E5-P1-1)",
          "[playback][redaction][subclip]") {
    Timeline tl(R"(
        V1 tgt |..........|
    )");
    const MediaId master = tl.media('A');
    const auto sub = edit::makeSubclip(tl.state, master, TimeRange{f(2), f(6)}, "A, the answer");
    REQUIRE(sub);
    Redaction r;
    r.media = master;
    r.name = "Face";
    r.source = {f(3), f(2)}; // media frames 3..5
    r.region.shape = MaskShape::Rectangle;
    r.region.set("size.x", 0.3);
    r.region.set("size.y", 0.3);
    const auto face = edit::addRedaction(sub->state, r);
    REQUIRE(face);
    edit::SourceClip clip;
    clip.media = sub->id;
    clip.sourceRange = {f(2), f(6)}; // media 2..8 at timeline 0..6
    clip.patches = {{{StreamKind::Video, 0}, tl.track("V1")}};
    const ProjectState s = ok(edit::overwriteEdit(face->state, tl.seq, clip, Ticks{})).state;

    std::vector<LayerSpec> layers;
    playback::evaluateVideoLayers(s, *s.findSequence(tl.seq), f(1), layers, false); // media frame 3: covered
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].media == sub->id);
    CHECK(layers[0].redactions.size() == 1); // it showed the face before
    CHECK(layers[0].sourceRedactions.size() == 1);
    playback::evaluateVideoLayers(s, *s.findSequence(tl.seq), f(3), layers, false); // media frame 5: not
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].redactions.empty());

    // The displayed frame's own time decides at composite time, through the same rule.
    playback::evaluateVideoLayers(s, *s.findSequence(tl.seq), f(1), layers, false);
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(64 * 36 * 4, 128);
    media::VideoFrame frame;
    frame.width = 64;
    frame.height = 36;
    frame.format = media::PixelFormat::RGBA8;
    frame.pts = f(3);
    frame.duration = f(1);
    media::CpuPlanes cpu;
    cpu.planes[0] = {pixels->data(), 64 * 4, 64, 36};
    cpu.owner = pixels;
    frame.payload = cpu;
    std::vector<media::VideoFrame> frames{frame};
    std::vector<render::CompositeLayer> composite;
    playback::buildCompositeLayers(layers, frames, composite);
    REQUIRE(composite.size() == 1);
    CHECK(composite[0].redactions.size() == 1);

    // Nested: the subclip's clip inside a nest is covered the same way.
    const ClipId leaf = clipAt(s, tl.seq, "V1@0");
    auto nested = edit::nestClips(s, tl.seq, std::vector<ClipId>{leaf}, "Nest");
    REQUIRE(nested);
    playback::evaluateVideoLayers(nested->edit.state, *nested->edit.state.findSequence(tl.seq), f(1), layers, false);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].nested);
    CHECK(layers[1].redactions.size() == 1);
}

namespace {

EffectInstance blurEffect(ProjectState& state, double radius) {
    EffectInstance blur;
    blur.id = state.ids.allocate<EffectInstanceId>();
    blur.effect = "wabcut.blur.gaussian";
    blur.set("radius", ParamValue(radius));
    return blur;
}

/// `state` with `effects` on track `label` of `seq` (a video track).
ProjectState withTrackEffects(const ProjectState& state, SequenceId seq, TrackId track, EffectStack effects) {
    auto copy = std::make_shared<Sequence>(*state.findSequence(seq));
    for (TrackPtr& t : copy->videoTracks) {
        if (t->id == track) {
            Track changed = *t;
            changed.effects = std::move(effects);
            t = std::make_shared<const Track>(std::move(changed));
        }
    }
    return state.withSequence(copy);
}

ProjectState withMasterEffects(const ProjectState& state, SequenceId seq, EffectStack effects) {
    auto copy = std::make_shared<Sequence>(*state.findSequence(seq));
    copy->effects = std::move(effects);
    return state.withSequence(copy);
}

ProjectState withCaption(const ProjectState& state, SequenceId seq, Ticks start, Ticks duration, std::string text) {
    auto copy = std::make_shared<Sequence>(*state.findSequence(seq));
    ProjectState out = state;
    CaptionTrack track;
    track.id = out.ids.allocate<CaptionTrackId>();
    track.enabled = true;
    track.cues = {CaptionCue{start, duration, std::move(text)}};
    copy->captionTracks = {track};
    return out.withSequence(copy);
}

} // namespace

TEST_CASE("Track and master effects: groups over a track's layers and over every track (I.9)", "[playback][effects]") {
    Timeline tl(R"(
        V2     |..Cccc....|
        V1 tgt |Aaaaaaaaaa|
    )");
    ProjectState s = tl.state;
    const EffectInstance blur = blurEffect(s, 12.0);
    std::vector<LayerSpec> layers;
    const auto evaluate = [&](const ProjectState& state, std::int64_t frame, bool cull = false,
                              const playback::CaptionSettings* captions = nullptr) {
        playback::evaluateVideoLayers(state, *state.findSequence(tl.seq), f(frame), layers, cull, nullptr, captions);
    };

    // A blur on V1: a group of V1's clip carries it (it was ignored before), identity placement on the canvas.
    const ProjectState tracked = withTrackEffects(s, tl.seq, tl.track("V1"), {blur});
    evaluate(tracked, 1);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].nested);
    CHECK_FALSE(layers[0].clip.isValid());
    CHECK(layers[0].nestedCount == 1);
    CHECK(layers[0].nestedCanvas == render::CanvasSize{1920, 1080});
    CHECK(layers[0].transform == render::LayerTransform{});
    REQUIRE(layers[0].effects.size() == 1);
    CHECK(layers[0].effects[0].type == render::EffectType::GaussianBlur);
    CHECK(layers[1].media == tl.media('A'));
    CHECK(layers[1].effects.empty());
    // At frame 3 V2's clip is over it: V1's group, then V2's clip.
    evaluate(tracked, 3);
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].nested);
    CHECK(layers[2].media == tl.media('C'));

    // Occlusion: V2's full-frame clip hides V1 - unless V2 has effects (a blur may not cover).
    evaluate(s, 3, true);
    CHECK(layers.size() == 1);
    evaluate(withTrackEffects(s, tl.seq, tl.track("V2"), {blur}), 3, true);
    REQUIRE(layers.size() == 3); // V1's clip, then V2's group and clip
    CHECK(layers[1].nested);
    CHECK(layers[1].nestedCount == 1);

    // The master stack: one group over every track; the burned-in caption stays above it.
    const ProjectState mastered = withCaption(withMasterEffects(tracked, tl.seq, {blur}), tl.seq, f(0), f(10), "hello");
    const playback::CaptionSettings captions;
    evaluate(mastered, 3, false, &captions);
    REQUIRE(layers.size() == 5); // master group, V1 group, A, C, caption
    CHECK(layers[0].nested);
    CHECK(layers[0].nestedCount == 3);
    CHECK(layers[0].effects.size() == 1);
    CHECK(layers[1].nested);
    CHECK(layers[4].caption);
    CHECK(playback::nextSibling(layers, 0) == 4);

    // The composite: the master group holding V1's group (holding A) and C.
    evaluate(withMasterEffects(tracked, tl.seq, {blur}), 3);
    REQUIRE(layers.size() == 4);
    const auto frame = [](int w, int h) {
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
        media::VideoFrame v;
        v.width = w;
        v.height = h;
        v.format = media::PixelFormat::RGBA8;
        v.duration = f(1);
        media::CpuPlanes cpu;
        cpu.planes[0] = {pixels->data(), w * 4, w, h};
        cpu.owner = pixels;
        v.payload = cpu;
        return v;
    };
    std::vector<media::VideoFrame> frames(layers.size());
    frames[2] = frame(64, 36);
    frames[3] = frame(64, 36);
    std::vector<render::CompositeLayer> composite;
    playback::buildCompositeLayers(layers, frames, composite);
    REQUIRE(composite.size() == 1);
    REQUIRE(composite[0].nested);
    CHECK(composite[0].effects.size() == 1);
    REQUIRE(composite[0].nested->layers.size() == 2);
    CHECK(composite[0].nested->layers[0].nested);
    CHECK(composite[0].nested->layers[0].effects.size() == 1);
}

TEST_CASE("Nested sequences draw their own captions, in their track's style, at nested time (I.9)",
          "[playback][nested][captions]") {
    Nested n;
    // The nested sequence's caption at nested 2..5; the nested clip starts at 0 and plays the nested sequence from 0.
    ProjectState s = withCaption(n.state, n.nested, f(2), f(3), "inside the nest");
    std::vector<LayerSpec> layers;
    const playback::CaptionSettings captions;
    playback::evaluateVideoLayers(s, *s.findSequence(n.tl.seq), f(3), layers, false, nullptr, &captions);
    const LayerSpec* head = headOf(layers, n.nestedClip);
    REQUIRE(head);
    const std::size_t h = static_cast<std::size_t>(head - layers.data());
    const std::size_t end = playback::nextSibling(layers, h);
    const LayerSpec& last = layers[end - 1];
    REQUIRE(last.caption);
    CHECK(last.caption->text == "inside the nest");
    CHECK(last.caption->frameWidth == 1920);
    // Only while the top level shows captions.
    playback::evaluateVideoLayers(s, *s.findSequence(n.tl.seq), f(3), layers, false, nullptr, nullptr);
    CHECK(std::none_of(layers.begin(), layers.end(), [](const LayerSpec& l) { return l.caption.has_value(); }));
    // Outside the cue: none.
    playback::evaluateVideoLayers(s, *s.findSequence(n.tl.seq), f(1), layers, false, nullptr, &captions);
    CHECK(std::none_of(layers.begin(), layers.end(), [](const LayerSpec& l) { return l.caption.has_value(); }));
}