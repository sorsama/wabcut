// M5n performance (the P.1 hotspots): the listeners that now refresh only for the edits that concern them, and the
// incremental structures, behave as before. Offscreen; no window is shown.
//   * Markers list again for marker, scoped-clip and timecode changes, and the playhead only at a marker's edges.
//   * The transcript follows edits from the controller's ChangeSet exactly as a full rebuild.
//   * The clip list's index stays right through moves across tracks, cuts, deletes, ripples, undo and redo.
//   * The selection loses removed clips only; a new selection is checked whole.
//   * Effect Controls, Color and Masks follow their clip, and the clip under the playhead.
//   * Timeline labels: the elision cache gives the labels a fresh view gives, and a new label font elides again.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "ClipAccessModel.h"
#include "ColorBridge.h"
#include "EffectControls.h"
#include "MarkersBridge.h"
#include "MaskBridge.h"
#include "ProjectController.h"
#include "TimelineView.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QFont>

#include <string>
#include <tuple>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

edit::EditOutcome moveBy(const ProjectState& s, SequenceId seq, ClipId clip, Ticks delta, edit::TrackShift tracks = {}) {
    const ClipId clips[] = {clip};
    return edit::moveClips(s, seq, clips, delta, tracks);
}

Marker markerAt(Ticks start, Ticks duration = {}) {
    Marker m;
    m.start = start;
    m.duration = duration;
    return m;
}

struct W {
    std::string text;
    double from = 0; ///< source frames
    double to = 0;
};

std::shared_ptr<const ai::Transcript> transcriptOf(std::vector<W> words) {
    auto t = std::make_shared<ai::Transcript>();
    t->language = "en";
    const double frame = static_cast<double>(framesToTicks(1, kRate).value);
    for (const W& w : words) {
        ai::TranscriptWord tw;
        tw.text = w.text;
        tw.t0 = Ticks{static_cast<std::int64_t>(w.from * frame)};
        tw.t1 = Ticks{static_cast<std::int64_t>(w.to * frame)};
        tw.confidence = 0.95f;
        t->words.push_back(std::move(tw));
    }
    return t;
}

using Words = std::vector<std::tuple<std::string, std::int64_t, std::int64_t>>;

Words wordsOf(const TranscriptBridge& bridge) {
    Words out;
    const auto& st = bridge.sequenceTranscript();
    for (std::size_t i = 0; i < st.size(); ++i)
        out.emplace_back(std::string(st.text(i)), st[i].timeline.start.value, st[i].timeline.end().value);
    return out;
}

/// Every row of the clip list maps back to itself, and every clip of the sequence has a row.
void requireIndexConsistent(const ProjectController& controller) {
    const ClipAccessModel& list = *controller.clipList();
    std::size_t clips = 0;
    controller.currentSequence()->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            ++clips;
            const int row = list.rowOf(c.id);
            INFO("clip " << c.id.value);
            REQUIRE(row >= 0);
            CHECK(list.clipAt(row) == c.id);
        }
    });
    REQUIRE(list.rowCount() == static_cast<int>(clips));
    for (int row = 0; row < list.rowCount(); ++row) {
        INFO("row " << row);
        CHECK(list.rowOf(list.clipAt(row)) == row);
    }
}

} // namespace

TEST_CASE("Edit gating: Markers list again only for marker, scoped clip or timecode changes; the playhead only at marker edges",
          "[app][markers]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa..........Bbbbbbbbbb|
        A1 tgt |Aaaaaaaaaa..........Bbbbbbbbbb|
    )");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    MarkersBridge markers;
    markers.setController(&controller);
    int listed = 0;
    QObject::connect(&markers, &MarkersBridge::markersChanged, [&] { ++listed; });
    // A point marker at 10 and a range over 20-25.
    REQUIRE(controller.applyEditFn("Add Marker", [&](const ProjectState& s) { return edit::addMarker(s, tl.seq, markerAt(f(10))); }));
    REQUIRE(controller.applyEditFn("Add Marker", [&](const ProjectState& s) { return edit::addMarker(s, tl.seq, markerAt(f(20), f(5))); }));
    CHECK(listed >= 2);
    REQUIRE(markers.items().size() == 2);

    // An edit that changes no marker (and its undo): not listed again.
    listed = 0;
    REQUIRE(controller.commitGesture("Move", [&](const ProjectState& s) { return moveBy(s, tl.seq, tl.clip("V1@20"), -f(2)); }));
    REQUIRE(controller.undo());
    CHECK(listed == 0);
    // A marker edit and its undo: listed again.
    REQUIRE(controller.applyEditFn("Add Marker", [&](const ProjectState& s) { return edit::addMarker(s, tl.seq, markerAt(f(28))); }));
    CHECK(listed >= 1);
    CHECK(markers.items().size() == 3);
    listed = 0;
    REQUIRE(controller.undo());
    CHECK(listed >= 1);
    CHECK(markers.items().size() == 2);

    // The playhead: only entering or leaving a marker lists again (its atPlayhead item).
    const auto atPlayhead = [&] {
        std::vector<int> out;
        for (const QVariant& item : markers.items()) {
            if (item.toMap().value(QStringLiteral("atPlayhead")).toBool())
                out.push_back(item.toMap().value(QStringLiteral("index")).toInt());
        }
        return out;
    };
    controller.setPlayhead(f(0));
    listed = 0;
    controller.setPlayhead(f(3));
    CHECK(listed == 0);
    controller.setPlayhead(f(10)); // on the point marker
    CHECK(listed == 1);
    CHECK(atPlayhead() == std::vector<int>{0});
    controller.setPlayhead(f(11)); // past it
    CHECK(listed == 2);
    CHECK(atPlayhead().empty());
    controller.setPlayhead(f(21)); // inside the range
    CHECK(listed == 3);
    CHECK(atPlayhead() == std::vector<int>{1});
    controller.setPlayhead(f(23)); // still inside
    CHECK(listed == 3);
    controller.setPlayhead(f(25)); // the range's end is outside it
    CHECK(listed == 4);
    CHECK(atPlayhead().empty());

    // Clip scope: the selected clip's media markers; edits of other clips and the playhead do not list again.
    markers.setScope(QStringLiteral("clip"));
    controller.setSelection({tl.clip("V1@0")});
    listed = 0;
    REQUIRE(controller.commitGesture("Move", [&](const ProjectState& s) { return moveBy(s, tl.seq, tl.clip("V1@20"), -f(2)); }));
    controller.setPlayhead(f(10));
    CHECK(listed == 0);
    REQUIRE(controller.applyEditFn("Clip Marker", [&](const ProjectState& s) {
        return edit::setMediaMarkers(s, tl.media('A'), {markerAt(f(4))});
    }));
    CHECK(listed >= 1);
    CHECK(markers.items().size() == 1);
    // Renaming the clip changes the scope's clip name.
    listed = 0;
    controller.renameClip(clipKey(tl.clip("V1@0")), QStringLiteral("Interview"));
    CHECK(listed >= 1);
    CHECK(markers.clipName() == QStringLiteral("Interview"));
}

TEST_CASE("Edit gating: the transcript follows edits from the controller's ChangeSet exactly as a full rebuild",
          "[app][transcript]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa....Bbbbbbbbbbbbbbbbbbbb|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa....Bbbbbbbbbbbbbbbbbbbb|
    )");
    tl.linkMatching();
    const auto wordsA = transcriptOf({{"Hello", 0, 4}, {"world", 5, 9}, {"again", 12, 18}});
    const auto wordsB = transcriptOf({{"Second", 1, 5}, {"clip", 6, 10}, {"talks", 12, 16}});
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    TranscriptBridge bridge;
    bridge.setUseAnalysisCache(false);
    bridge.setController(&controller);
    bridge.addTranscript(tl.media('A'), 0, wordsA);
    bridge.addTranscript(tl.media('B'), 0, wordsB);
    // A bridge that maps the sequence from scratch now.
    const auto fresh = [&] {
        TranscriptBridge rebuilt;
        rebuilt.setUseAnalysisCache(false);
        rebuilt.setController(&controller);
        rebuilt.addTranscript(tl.media('A'), 0, wordsA);
        rebuilt.addTranscript(tl.media('B'), 0, wordsB);
        return wordsOf(rebuilt);
    };
    REQUIRE(wordsOf(bridge).size() == 6);
    CHECK(wordsOf(bridge) == fresh());

    const ClipId b = tl.clip("V1@24");
    const ClipId a = tl.clip("V1@0");
    // Moves, trims (words drop out), a ripple delete, undo and redo.
    REQUIRE(controller.commitGesture("Move", [&](const ProjectState& s) { return moveBy(s, tl.seq, b, -f(3)); }));
    CHECK(wordsOf(bridge) == fresh());
    REQUIRE(controller.commitGesture("Trim", [&](const ProjectState& s) {
        return edit::trimEdge(s, tl.seq, a, edit::Edge::Tail, -f(5));
    }));
    CHECK(wordsOf(bridge) == fresh());
    REQUIRE(controller.commitGesture("Trim", [&](const ProjectState& s) {
        return edit::trimEdge(s, tl.seq, b, edit::Edge::Head, f(6));
    }));
    CHECK(wordsOf(bridge) == fresh());
    REQUIRE(controller.applyEditFn("Ripple Delete", [&](const ProjectState& s) {
        const ClipId clips[] = {a};
        return edit::rippleDeleteClips(s, tl.seq, clips);
    }));
    CHECK(wordsOf(bridge) == fresh());
    REQUIRE(controller.undo());
    CHECK(wordsOf(bridge) == fresh());
    REQUIRE(controller.undo());
    CHECK(wordsOf(bridge) == fresh());
    REQUIRE(controller.redo());
    CHECK(wordsOf(bridge) == fresh());
    REQUIRE(controller.undo());
    REQUIRE(controller.undo());
    REQUIRE(controller.undo());
    CHECK(wordsOf(bridge) == fresh());
    CHECK(wordsOf(bridge).size() == 6);
}

TEST_CASE("Edit gating: the clip list's index stays right through moves across tracks, cuts, deletes, ripples, undo and redo",
          "[app][timeline]") {
    Timeline tl(R"(
        V2     |..........Cccccccccc..........|
        V1 tgt |AaaaaaaaaaBbbbbbbbbbDddddddddd|
        A1 tgt |AaaaaaaaaaBbbbbbbbbbDddddddddd|
        A2     |....................Eeeeeeeeee|
    )");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    requireIndexConsistent(controller);
    const SequenceId seq = tl.seq;
    const auto step = [&](const char* label, const ProjectController::EditFn& fn) {
        INFO(label);
        REQUIRE(controller.commitGesture(label, fn));
        requireIndexConsistent(controller);
    };
    // B up to V2 over C (its audio stays on A1), D's audio down to A2 over E.
    step("Move", [&](const ProjectState& s) { return moveBy(s, seq, tl.clip("V1@10"), {}, edit::TrackShift{1, 0}); });
    step("Move", [&](const ProjectState& s) {
        const ClipId clips[] = {tl.clip("A1@20")};
        return edit::moveClips(s, seq, clips, {}, edit::TrackShift{0, 1}, edit::MoveMode::Overwrite, edit::LinkMode::IgnoreLinks);
    });
    step("Add Edit", [&](const ProjectState& s) { return edit::addEditToAllTracks(s, seq, f(5)); });
    step("Delete", [&](const ProjectState& s) {
        const ClipId clips[] = {clipAt(s, seq, "V1@0")};
        return edit::deleteClips(s, seq, clips);
    });
    step("Ripple Delete", [&](const ProjectState& s) {
        const ClipId clips[] = {clipAt(s, seq, "V1@20")};
        return edit::rippleDeleteClips(s, seq, clips);
    });
    for (int i = 0; i < 5; ++i) {
        REQUIRE(controller.undo());
        requireIndexConsistent(controller);
    }
    for (int i = 0; i < 3; ++i) {
        REQUIRE(controller.redo());
        requireIndexConsistent(controller);
    }
}

TEST_CASE("Edit gating: the selection loses removed clips only, and a new selection is checked whole", "[app][timeline]") {
    Timeline tl(R"(
        V1 tgt |AaaaaBbbbbCccccDdddd|
        A1 tgt |AaaaaBbbbbCccccDdddd|
    )");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@5");
    const ClipId d = tl.clip("V1@15");
    controller.setSelection({a, b}, b);
    // An edit elsewhere keeps the selection and its primary.
    REQUIRE(controller.commitGesture("Trim", [&](const ProjectState& s) {
        return edit::trimEdge(s, tl.seq, d, edit::Edge::Tail, -f(1));
    }));
    CHECK(controller.selection() == std::vector<ClipId>{a, b});
    CHECK(controller.primaryClip() == b);
    // Deleting B (with its audio): B goes, the primary moves to A.
    REQUIRE(controller.applyEditFn("Delete", [&](const ProjectState& s) {
        const ClipId clips[] = {b};
        return edit::deleteClips(s, tl.seq, clips);
    }));
    CHECK(controller.selection() == std::vector<ClipId>{a});
    CHECK(controller.primaryClip() == a);
    // Undo brings B back, not into the selection.
    REQUIRE(controller.undo());
    CHECK(controller.selection() == std::vector<ClipId>{a});
    // A selection holding a clip that does not exist is checked whole at the next change.
    controller.setSelection({a, ClipId{987654}}, a);
    REQUIRE(controller.commitGesture("Trim", [&](const ProjectState& s) {
        return edit::trimEdge(s, tl.seq, d, edit::Edge::Tail, -f(1));
    }));
    CHECK(controller.selection() == std::vector<ClipId>{a});
    // Select all, delete C (and its audio): the rest stays selected.
    controller.selectAll();
    REQUIRE(controller.selection().size() == 8);
    REQUIRE(controller.applyEditFn("Delete", [&](const ProjectState& s) {
        const ClipId clips[] = {clipAt(s, tl.seq, "V1@10")};
        return edit::deleteClips(s, tl.seq, clips);
    }));
    CHECK(controller.selection().size() == 6);

    // A large selection (over 32 clips is checked against a set of the sequence's clips): the same.
    Timeline big(R"(
        V1 tgt |AaBbCcDdEeFfGgHhIiJjKkLlMmNnOoPpQq|
        A1 tgt |AaBbCcDdEeFfGgHhIiJjKkLlMmNnOoPpQq|
    )");
    big.linkMatching();
    ProjectController many;
    many.resetProject(big.state, big.seq);
    many.selectAll();
    REQUIRE(many.selection().size() == 34);
    REQUIRE(many.applyEditFn("Delete", [&](const ProjectState& s) {
        const ClipId clips[] = {clipAt(s, big.seq, "V1@4")};
        return edit::deleteClips(s, big.seq, clips);
    }));
    CHECK(many.selection().size() == 32);
    REQUIRE(many.commitGesture("Trim", [&](const ProjectState& s) {
        return edit::trimEdge(s, big.seq, clipAt(s, big.seq, "V1@32"), edit::Edge::Tail, -f(1));
    }));
    CHECK(many.selection().size() == 32);
}

TEST_CASE("Edit gating: Effect Controls, Color and Masks follow their clip, and the clip under the playhead", "[app][finishing]") {
    resetSettings();
    clearMessages();
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa..........Bbbbbbbbbb|
        A1 tgt |Aaaaaaaaaa..........Bbbbbbbbbb|
    )");
    tl.linkMatching();
    Shell shell;
    REQUIRE(shell.window);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* color = shell.engine.singletonInstance<ColorBridge*>("Wabcut", "Color");
    auto* masks = shell.engine.singletonInstance<MaskBridge*>("Wabcut", "Masks");
    REQUIRE(controller);
    REQUIRE(color);
    REQUIRE(masks);
    controller->resetProject(tl.state, tl.seq);
    spin(50); // the bridges attach on the next turn of the event loop
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@20");

    // Nothing selected, the playhead in the gap: no target.
    controller->setPlayhead(f(12));
    CHECK_FALSE(color->targetClip().isValid());
    CHECK_FALSE(masks->clipId().isValid());
    // An edit puts B under the playhead: both follow it at once; its undo takes it away.
    REQUIRE(controller->commitGesture("Move", [&](const ProjectState& s) { return moveBy(s, tl.seq, b, -f(10)); }));
    CHECK(color->targetClip() == b);
    CHECK(masks->clipId() == b);
    REQUIRE(controller->undo());
    CHECK_FALSE(color->targetClip().isValid());
    CHECK_FALSE(masks->clipId().isValid());

    // A selected clip: an edit elsewhere keeps it, an edit of it shows at once.
    controller->setSelection({a, tl.clip("A1@0")}, a);
    CHECK(color->targetClip() == a);
    CHECK(masks->clipId() == a);
    EffectControlsModel* controls = controller->effectControls();
    REQUIRE(controls);
    const QString subtitle = controls->property("subtitle").toString();
    REQUIRE_FALSE(subtitle.isEmpty());
    REQUIRE(controller->commitGesture("Move", [&](const ProjectState& s) { return moveBy(s, tl.seq, b, f(1)); }));
    CHECK(color->targetClip() == a);
    CHECK(masks->clipId() == a);
    CHECK(controls->property("subtitle").toString() == subtitle);
    controller->renameClip(clipKey(a), QStringLiteral("Interview"));
    CHECK(color->clipName() == QStringLiteral("Interview"));
    CHECK(masks->clipName() == QStringLiteral("Interview"));
    CHECK(controls->property("title").toString() == QStringLiteral("Interview"));
    REQUIRE(controller->commitGesture("Trim", [&](const ProjectState& s) {
        return edit::trimEdge(s, tl.seq, a, edit::Edge::Tail, -f(2));
    }));
    CHECK(controls->property("subtitle").toString() != subtitle);
    REQUIRE(controller->undo());
    CHECK(controls->property("subtitle").toString() == subtitle);
    // Deleting it leaves nothing to show.
    REQUIRE(controller->applyEditFn("Delete", [&](const ProjectState& s) {
        const ClipId clips[] = {a};
        return edit::deleteClips(s, tl.seq, clips);
    }));
    CHECK_FALSE(color->targetClip().isValid());
    CHECK(controls->rowCount() == 0);
    takeMessages();
}

TEST_CASE("Timeline labels: the elision cache gives the labels a fresh view gives; a new label font elides again",
          "[app][timeline]") {
    Rig rig(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbbCcccccccccDddddddddd|
        A1 tgt |AaaaaaaaaaBbbbbbbbbbCcccccccccDddddddddd|
    )");
    const char* names[] = {"Interview with the director, take three", "Wide establishing shot of the harbour",
                           "Close-up of the hands at the wheel", "Archive footage, nineteen seventy-four"};
    const std::string_view refs[] = {"V1@0", "V1@10", "V1@20", "V1@30"};
    for (std::size_t i = 0; i < 4; ++i)
        rig.controller.renameClip(clipKey(rig.clip(refs[i])), QString::fromLatin1(names[i]));
    const auto drain = [&] {
        for (int i = 0; i < 60; ++i) {
            QCoreApplication::processEvents();
            rig.harness.frame();
            if (!rig.view->stats().labelsPending && i > 2)
                break;
        }
    };
    drain();
    const QHash<quint64, QString> first = rig.view->stats().labelTexts;
    REQUIRE(first.size() >= 4);
    bool elided = false;
    for (std::size_t i = 0; i < 4; ++i) {
        const QString label = first.value(rig.clip(refs[i]).value);
        elided = elided || (label != QString::fromLatin1(names[i]) && !label.isEmpty());
    }
    REQUIRE(elided); // 100 px clips: the names are elided

    // A zoom away and back: the same labels (the cached elisions).
    rig.view->setPixelsPerSecond(8.0 * 25.0);
    drain();
    rig.view->setPixelsPerSecond(10.0 * 25.0);
    drain();
    CHECK(rig.view->stats().labelTexts == first);
    // And what a view built now shows.
    TimelineView* fresh = addView(rig.harness, rig.controller, QSize(900, 260));
    fresh->setPixelsPerSecond(10.0 * 25.0);
    fresh->setScrollX(0);
    fresh->setScrollY(0);
    drain();
    CHECK(fresh->stats().labelTexts == first);

    // A bigger label font: every elision is made again at the new size (not served from the cache).
    QFont font;
    font.setPixelSize(20);
    font.setWeight(QFont::DemiBold);
    const QVariantMap style{{QStringLiteral("labelFont"), font}};
    rig.view->setStyle(style);
    fresh->setStyle(style);
    drain();
    const QHash<quint64, QString> bigger = rig.view->stats().labelTexts;
    CHECK(bigger != first);
    CHECK(fresh->stats().labelTexts == bigger);
}
