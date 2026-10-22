// M5k: "Explain this frame" (FrameInspector). On a synthetic timeline with a time-remapped clip, a cross dissolve, a
// colour grade with an effect mask, a source redaction, a caption and an audio clip with a gain, an effect and a
// bleep, the explanation matches what the preview evaluator computes; the preview-only overrides (solo, bypass)
// never touch the project: no undo step, no revision, the same snapshot. Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "FrameInspector.h"
#include "ProjectController.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>

#include <algorithm>
#include <cmath>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// V1: A (frames 0-19, time-remapped at 200 %) dissolving into B (20-39, graded, an effect mask, redacted); A1: C (a
/// gain of -6 dB, a Gain effect, a bleep); a caption over 18-28.
struct Scene {
    Timeline tl{R"(
        V1 tgt |A100aaaaaaaaaaaaaaaaB200bbbbbbbbbbbbbbbb|
        A1 tgt |C50ccccccccccccccccccccccccccccccccccccc|
    )"};
    ProjectState state;
    ClipId a, b, c;
    EffectInstanceId grade, gain;

    Scene() {
        state = tl.state;
        a = tl.clip("V1@0");
        b = tl.clip("V1@20");
        c = tl.clip("A1@0");
        state = ok(edit::setTimeRemapEnabled(state, tl.seq, a, true)).state;
        state = ok(edit::setTimeRemapSpeed(state, tl.seq, a, 0, Speed{2, 1})).state;

        edit::TransitionSpec dissolve;
        dissolve.outgoing = a;
        dissolve.incoming = b;
        dissolve.before = f(4);
        dissolve.after = f(4);
        state = ok(edit::addTransition(state, tl.seq, tl.track("V1"), dissolve)).state;

        auto graded = ok(edit::addEffect(state, tl.seq, std::vector<ClipId>{b}, "wabcut.color.grade",
                                         {EffectParam{"basic.exposure", ParamValue(1.0)}}));
        grade = graded.createdEffects.front();
        state = std::move(graded.state);
        Mask face;
        face.name = "Face";
        auto masked = edit::addMask(state, tl.seq, b, grade, face);
        REQUIRE(masked);
        state = std::move(masked->state);

        Redaction plate;
        plate.media = tl.media('B');
        plate.name = "Plate";
        plate.source = {f(180), f(60)};
        auto redacted = edit::addRedaction(state, plate);
        REQUIRE(redacted);
        state = std::move(redacted->state);
        Redaction voice;
        voice.media = tl.media('C');
        voice.name = "Name";
        voice.source = {f(60), f(20)};
        voice.audio = RedactionAudio::Bleep;
        auto bleeped = edit::addRedaction(state, voice);
        REQUIRE(bleeped);
        state = std::move(bleeped->state);

        edit::ClipAttributeChange quieter;
        ClipProperties props = state.findSequence(tl.seq)->findClip(c)->clip().properties();
        props.set(ParamId::VolumeDb, -6.0);
        quieter.properties = props;
        state = ok(edit::setClipAttributes(state, tl.seq, std::vector<ClipId>{c}, quieter)).state;
        auto gained = ok(edit::addEffect(state, tl.seq, std::vector<ClipId>{c}, "wabcut.audio.gain"));
        gain = gained.createdEffects.front();
        state = std::move(gained.state);

        CaptionTrack english;
        english.name = "English";
        english.cues.push_back({f(18), f(10), "Hello there"});
        state = ok(edit::setCaptionTrack(state, tl.seq, english)).state;
        requireValid(state);
    }
    const Sequence& sequence() const { return *state.findSequence(tl.seq); }
};

const ExplainedVideoLayer* layerOf(const FrameExplanation& e, ClipId clip) {
    const auto it = std::find_if(e.video.begin(), e.video.end(), [&](const ExplainedVideoLayer& l) { return l.clip == clip; });
    return it == e.video.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("Explain this frame matches the evaluator: remap, transition, grade, mask, redaction, caption, audio",
          "[app][inspector]") {
    Scene s;
    const Ticks t = f(21); // inside the dissolve [16, 24)
    const playback::PreviewSettings preview;
    playback::CaptionSettings captions;
    const FrameExplanation e = explainFrame(s.state, s.sequence(), t, preview, &captions);

    std::vector<playback::LayerSpec> specs;
    playback::evaluateVideoLayers(s.state, s.sequence(), t, specs, false, &preview, &captions);
    const auto pictures = std::count_if(specs.begin(), specs.end(), [](const auto& spec) { return !spec.caption; });
    REQUIRE(static_cast<std::ptrdiff_t>(e.video.size()) == pictures);
    REQUIRE(e.video.size() == 2);
    CHECK(e.video.front().clip == s.b); // the top of the stack first: B is drawn over A in the group
    for (const playback::LayerSpec& spec : specs) {
        if (spec.caption)
            continue;
        const ExplainedVideoLayer* layer = layerOf(e, spec.clip);
        REQUIRE(layer);
        INFO("clip " << spec.clip.value);
        CHECK(layer->sourceTime == spec.sourceTime);
        CHECK(layer->evaluatedEffects == static_cast<int>(spec.effects.size()));
        CHECK(layer->bypassedEffects == spec.bypassedEffects);
        CHECK(layer->evaluatedRedactions == static_cast<int>(spec.redactions.size()));
        CHECK(layer->redactions.size() == static_cast<qsizetype>(spec.redactions.size()));
        CHECK(layer->transform == spec.transform);
        CHECK(layer->opacity == spec.opacity);
    }
    // A: remapped at 200 %, the outgoing side of the dissolve.
    const ExplainedVideoLayer* a = layerOf(e, s.a);
    REQUIRE(a);
    CHECK(a->remapped);
    CHECK(a->mapping.contains(QStringLiteral("Time remap")));
    CHECK(a->mapping.contains(QStringLiteral("200 %")));
    REQUIRE(a->transition);
    CHECK(*a->transition == QStringLiteral("Cross Dissolve"));
    CHECK(a->transitionSide == QStringLiteral("outgoing"));
    CHECK(std::abs(a->transitionProgress - 5.0 / 8.0) < 1e-9);
    CHECK(a->sourceFrame == ticksToFrames(a->sourceTime, kRate, Rounding::Floor).value_or(-1));
    // B: graded (basic correction active), its grade masked, redacted, the incoming side.
    const ExplainedVideoLayer* b = layerOf(e, s.b);
    REQUIRE(b);
    CHECK(b->transitionSide == QStringLiteral("incoming"));
    REQUIRE(b->effects.size() == 1);
    CHECK(b->effects.front().state == ExplainedEffect::State::Applied);
    CHECK(b->effects.front().masks == QStringList{QStringLiteral("Face (add)")});
    REQUIRE_FALSE(b->grade.empty());
    const auto basic = std::find_if(b->grade.begin(), b->grade.end(),
                                    [](const GradeStage& g) { return g.name == QStringLiteral("Basic correction"); });
    REQUIRE(basic != b->grade.end());
    CHECK(basic->active);
    CHECK(b->redactions == QStringList{QStringLiteral("Plate: blur")});
    CHECK(b->source == ExplainedVideoLayer::Source::Original);

    // Audio: the source sample, the gain chain, the effect, the bleep.
    REQUIRE(e.audio.size() == 1);
    const ExplainedAudioLayer& voice = e.audio.front();
    const Clip& clip = s.sequence().findClip(s.c)->clip();
    CHECK(voice.sourceTime == clip.sourceTimeAt(t));
    CHECK(voice.sampleRate == 48000);
    CHECK(voice.sourceSample == ticksToFrames(clip.sourceTimeAt(t), kRate48kHz, Rounding::Floor).value_or(-1));
    REQUIRE(voice.gain.size() == 5);
    CHECK(voice.gain[0].value == QStringLiteral("-6.0 dB"));
    CHECK(voice.gain[1].value == QStringLiteral("Centre"));
    CHECK(voice.gain[2].value == QStringLiteral("0.0 dB")); // the A1 fader (A.3) at unity
    CHECK(voice.gain[4].value == QStringLiteral("0.0 dB")); // the master fader
    REQUIRE(voice.clipEffects.size() == 1);
    CHECK(voice.clipEffects.front().state == ExplainedEffect::State::Applied);
    CHECK(voice.redactions.size() == 1);
    CHECK(voice.redactions.front().contains(QStringLiteral("bleep")));
    CHECK(voice.silentReason.isEmpty());

    // The caption and where its timing comes from.
    REQUIRE(e.caption);
    CHECK(e.caption->text == QStringLiteral("Hello there"));
    CHECK(e.caption->shown);
    CHECK(e.caption->timing.contains(QStringLiteral("Cue In/Out")));

    // Outside the dissolve, A alone: no transition; a frame where B covers nothing else.
    const FrameExplanation later = explainFrame(s.state, s.sequence(), f(30), preview, nullptr);
    REQUIRE(later.video.size() == 1);
    CHECK_FALSE(later.video.front().transition);
    CHECK(later.video.front().clip == s.b);
    CHECK_FALSE(later.caption.has_value()); // the cue ended at 28
}

TEST_CASE("Explain this frame: solo and bypass are preview-only and never create undo steps", "[app][inspector]") {
    Scene s;
    ProjectController controller;
    controller.resetProject(s.state, s.tl.seq);
    controller.setPlayhead(f(21));
    FrameInspector inspector;
    inspector.setController(&controller);
    inspector.setActive(true);
    inspector.refresh();
    REQUIRE(inspector.layers().size() == 2);

    const ProjectSnapshot before = controller.snapshot();
    const auto undo = controller.session().undoCount();
    const int revision = controller.revision();

    inspector.toggleSolo(clipKey(s.b));
    inspector.toggleBypass(clipKey(s.b), effectKey(s.grade));
    inspector.toggleBypass(clipKey(s.c), effectKey(s.gain));
    CHECK(inspector.previewing());
    CHECK(inspector.overrideCount() == 3);
    // The project: untouched.
    CHECK(controller.snapshot() == before);
    CHECK(controller.session().undoCount() == undo);
    CHECK_FALSE(controller.canRedo());
    CHECK(controller.revision() == revision);
    // The preview: A hidden, B's grade and C's gain bypassed.
    const ProjectSnapshot preview = inspector.previewSnapshot();
    REQUIRE(preview);
    const Sequence& seq = *preview->findSequence(s.tl.seq);
    CHECK_FALSE(seq.findClip(s.a)->clip().enabled);
    CHECK(seq.findClip(s.b)->clip().enabled);
    CHECK_FALSE(seq.findClip(s.b)->clip().effects().front().enabled);
    CHECK_FALSE(seq.findClip(s.c)->clip().effects().front().enabled);
    // The explanation marks them (and still explains the project as it is).
    const ExplainedVideoLayer* b = layerOf(inspector.explanation(), s.b);
    REQUIRE(b);
    CHECK(b->soloed);
    CHECK(b->effects.front().state == ExplainedEffect::State::PreviewBypassed);
    CHECK(inspector.explanation().video.size() == 2);

    // A real edit while previewing: one undo step (the user's), and the preview follows it.
    REQUIRE(controller.applyEditFn("Rename", [&](const ProjectState& st) {
        edit::ClipAttributeChange rename;
        rename.name = "Renamed";
        return edit::setClipAttributes(st, s.tl.seq, std::vector<ClipId>{s.b}, rename);
    }));
    CHECK(controller.session().undoCount() == undo + 1);
    const ProjectSnapshot followed = inspector.previewSnapshot();
    REQUIRE(followed);
    CHECK(followed->findSequence(s.tl.seq)->findClip(s.b)->clip().name() == "Renamed");
    CHECK_FALSE(followed->findSequence(s.tl.seq)->findClip(s.a)->clip().enabled);

    // Toggling off and clearing: still no step of their own.
    inspector.toggleSolo(clipKey(s.b));
    inspector.clearOverrides();
    CHECK_FALSE(inspector.previewing());
    CHECK_FALSE(inspector.previewSnapshot());
    CHECK(controller.session().undoCount() == undo + 1);
    controller.undo();
    CHECK(controller.snapshot()->findSequence(s.tl.seq)->findClip(s.b)->clip().name() == "B");
    CHECK_FALSE(controller.canUndo());
}

TEST_CASE("Explain this frame: the panel, with an override, as a screenshot", "[app][inspector][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* inspector = shell.engine.singletonInstance<FrameInspector*>("Wabcut", "FrameInspector");
    REQUIRE(controller);
    REQUIRE(inspector);
    QQuickItem* panel = shell.item("frameInspector");
    REQUIRE(panel);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showFrameInspector"));
    controller->goToNextEdit();
    controller->stepFrames(12);
    spin(50);
    REQUIRE(inspector->active());
    REQUIRE_FALSE(inspector->layers().isEmpty());
    const QString first = inspector->layers().front().toMap().value(QStringLiteral("key")).toString();
    inspector->toggleSolo(first);
    spin(50);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    const double dpr = image.devicePixelRatio();
    REQUIRE(image.copy(QRect(static_cast<int>(rect.x() * dpr), static_cast<int>(rect.y() * dpr),
                             static_cast<int>(rect.width() * dpr), static_cast<int>(rect.height() * dpr)))
                .save(QDir(outputDir()).filePath(QStringLiteral("frame-inspector.png"))));
    inspector->clearOverrides();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Explain this frame describes Border, Ultra Key and Track Matte layers (0.2)", "[app][inspector]") {
    // V1: A with a Border, an Ultra Key, a Track Matte Key reading V2 by its luma, and a grade after them; V2 (output
    // off, as editors hide a matte track): B, the matte for the first five frames.
    Timeline tl(R"(
        V2 off |Bbbbb.....|
        V1 tgt |Aaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V2@0");
    ProjectState state = tl.state;
    state = ok(edit::addEffect(state, tl.seq, std::vector<ClipId>{a}, "wabcut.border", {EffectParam{"width", ParamValue(8.0)}})).state;
    state = ok(edit::addEffect(state, tl.seq, std::vector<ClipId>{a}, "wabcut.key.ultra")).state;
    state = ok(edit::addEffect(state, tl.seq, std::vector<ClipId>{a}, "wabcut.key.trackMatte",
                               {EffectParam{"matteTrack", ParamValue(2.0)}, EffectParam{"compositeUsing", ParamValue(1.0)}}))
                .state;
    state = ok(edit::addEffect(state, tl.seq, std::vector<ClipId>{a}, "wabcut.color.grade",
                               {EffectParam{"basic.exposure", ParamValue(1.0)}}))
                .state;
    requireValid(state);
    const Sequence& seq = *state.findSequence(tl.seq);

    const FrameExplanation e = explainFrame(state, seq, f(2), {}, nullptr);
    const ExplainedVideoLayer* fill = layerOf(e, a);
    REQUIRE(fill);
    REQUIRE(fill->effects.size() == 4);
    CHECK(fill->effects[0].name == QStringLiteral("Border"));
    CHECK(fill->effects[0].detail == QStringLiteral("8 px, #FFFFFF, 100 % opacity"));
    CHECK(fill->effects[1].detail ==
          QStringLiteral("key #00FF00, output Composite; matte: transparency 45, tolerance 50, pedestal 10; cleanup: choke 0, "
                         "soften 0; spill 50"));
    CHECK(fill->effects[2].detail == QStringLiteral("Matte V2 (luma)"));
    CHECK(fill->effects[3].detail.isEmpty());
    // The grade after them pairs with its evaluated stage (before 0.2 the Border broke the pairing: no stages).
    REQUIRE_FALSE(fill->grade.empty());
    CHECK(fill->matteFor.isEmpty());
    // B is A's matte, not drawn itself (its track's output is off; the matte is read anyway).
    const ExplainedVideoLayer* matte = layerOf(e, b);
    REQUIRE(matte);
    CHECK(matte->matteFor == QStringLiteral("Track Matte of V1"));

    // Where V2 is empty, the matte is: the layer shows nothing (or, reversed, in full).
    const FrameExplanation empty = explainFrame(state, seq, f(7), {}, nullptr);
    REQUIRE(layerOf(empty, a));
    CHECK(layerOf(empty, a)->effects[2].detail == QStringLiteral("Matte V2 is empty here: the layer shows nothing"));
    CHECK_FALSE(layerOf(empty, b));

    // A matte track that does not exist.
    const EffectInstanceId key = seq.findClip(a)->clip().effects()[2].id;
    const ProjectState missing = ok(edit::setParameter(state, tl.seq, a, edit::ParamRef::ofEffect(key, "matteTrack"), ParamValue(5.0))).state;
    const FrameExplanation none = explainFrame(missing, *missing.findSequence(tl.seq), f(2), {}, nullptr);
    REQUIRE(layerOf(none, a));
    CHECK(layerOf(none, a)->effects[2].detail == QStringLiteral("No matte track V5: the layer shows nothing"));

    // The bridge publishes them for the panel.
    ProjectController controller;
    controller.resetProject(state, tl.seq);
    FrameInspector inspector;
    inspector.setController(&controller);
    controller.setPlayhead(f(2));
    inspector.setActive(true);
    inspector.refresh();
    bool sawMatte = false;
    bool sawDetail = false;
    for (const QVariant& row : inspector.layers()) {
        const QVariantMap layer = row.toMap();
        if (layer.value(QStringLiteral("matteText")).toString() == QStringLiteral("Track Matte of V1"))
            sawMatte = true;
        for (const QVariant& effect : layer.value(QStringLiteral("effects")).toList()) {
            if (effect.toMap().value(QStringLiteral("detail")).toString() == QStringLiteral("Matte V2 (luma)"))
                sawDetail = true;
        }
    }
    CHECK(sawMatte);
    CHECK(sawDetail);
}
