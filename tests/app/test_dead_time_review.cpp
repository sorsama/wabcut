// 0.5 (p0 round, stream C): Remove dead time's review (MomentsBridge, singleton `Moments`; DeadTimeReview.qml in the
// Highlights panel), offscreen.
//   * the find takes the remembered options (Sensitivity, Shortest stretch, Keep at each end, Only In to Out) and lists
//     each stretch with a remove switch; Remove sends only the switched-on ones and is one undo step (the real
//     sequence.removeDeadTime ripples them out; one undo puts them back);
//   * the Removal bands are owner-tagged highlights: the transcript's selection and cleanup preview stay as they are;
//     they go when the review closes, on another sequence, and when the sequence's clips changed (Remove then asks for a
//     new find);
//   * the keyboard review (Up / Down, K keep, Delete remove), jumps, progress and Cancel;
//   * in the shell: the Highlights panel's sheet, its list, switches and keys, Remove; screenshots dark and light.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "MomentsBridge.h"
#include "ShortsBridge.h" // setPreviewFramesForTests
#include "ProjectController.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Command.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QImage>
#include <QPainter>
#include <QQmlComponent>
#include <QQuickItem>
#include <QSettings>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

using command::Json;

/// `s` seconds as ticks.
Ticks secs(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// What the last sequence.removeDeadTime request carried; `g_during` runs inside a preview (progress, Cancel).
Json g_params;
std::function<void(command::ExecutionContext&)> g_during;

/// sequence.removeDeadTime with the real schema and the real apply (the ripple delete of the ranges given), and a
/// preview that measures nothing: three stretches, 1 s to 3 s, 5 s to 7 s and 9 s to 10 s.
const command::CommandRegistry& deadTimeCatalog() {
    static const command::CommandRegistry registry = [] {
        command::CommandRegistry r;
        const command::CommandSpec* real = ai::edit::editingCommands().find("sequence.removeDeadTime");
        REQUIRE(real);
        command::CommandSpec spec = *real;
        spec.execute = [apply = real->execute](command::ExecutionContext& c) -> command::CommandOutput {
            g_params = c.params();
            if (c.boolean("apply", false))
                return apply(c);
            if (g_during)
                g_during(c);
            const auto range = [](double from, double to) {
                return Json{{"startTicks", secs(from).value}, {"endTicks", secs(to).value}, {"seconds", to - from}};
            };
            command::CommandOutput out;
            out.state = c.state();
            out.data = Json{{"ranges", Json::array({range(5, 7), range(1, 3), range(9, 10)})}, {"removeSeconds", 5.0}};
            return out;
        };
        const auto added = r.add(std::move(spec));
        REQUIRE(added.has_value());
        return r;
    }();
    return registry;
}

/// 12 s of A on V1 and A1 (linked).
std::string picture() {
    const std::string body = "A" + std::string(299, 'a');
    return "V1 tgt |" + body + "|\nA1 tgt |" + body + "|";
}

struct DeadRig {
    Timeline tl{picture()};
    ProjectController controller;
    MomentsBridge moments;

    DeadRig() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        moments.setController(&controller);
        moments.setRegistry(&deadTimeCatalog());
        moments.setSynchronous(true);
        g_params = Json();
        g_during = {};
    }
    ~DeadRig() { moments.setRegistry(nullptr); }
    std::size_t undo() { return controller.session().undoCount(); }
    QVariantMap row(int i) const { return moments.deadTimeRanges().value(i).toMap(); }
    Ticks contentEnd() const { return controller.currentSequence()->contentEnd(); }
    /// The Removal bands on the timeline.
    std::vector<TimeRange> bands() const {
        std::vector<TimeRange> out;
        for (const TimelineHighlight& h : controller.timelineHighlights()) {
            if (h.kind == TimelineHighlight::Kind::Removal && h.tracks.empty())
                out.push_back(h.range);
        }
        return out;
    }
};

TimeRange seconds(double from, double to) { return TimeRange::fromStartEnd(secs(from), secs(to)); }

void forgetOptions() {
    QSettings().remove(QStringLiteral("moments"));
}

/// How many times `signal` of `sender` was emitted since this was made.
template <class Signal>
std::shared_ptr<int> countOf(MomentsBridge* sender, Signal signal) {
    auto count = std::make_shared<int>(0);
    QObject::connect(sender, signal, sender, [count] { ++*count; });
    return count;
}

} // namespace

TEST_CASE("Dead time review lists each stretch and Remove takes only the switched-on ones in one undo step",
          "[app][moments][deadtime]") {
    resetSettings();
    forgetOptions();
    DeadRig r;
    const std::size_t before = r.undo();
    r.moments.findDeadTime({{QStringLiteral("sensitivity"), 0.7},
                            {QStringLiteral("minSeconds"), 2.0},
                            {QStringLiteral("handleSeconds"), 0.5},
                            {QStringLiteral("inOut"), false}});
    // The options went to the command (its own schema checked them) and nothing changed.
    INFO(g_params.dump());
    CHECK(g_params.value("sensitivity", 0.0) == Catch::Approx(0.7));
    CHECK(g_params.value("minSeconds", 0.0) == Catch::Approx(2.0));
    CHECK(g_params.value("handleSeconds", 0.0) == Catch::Approx(0.5));
    CHECK(g_params.value("inOut", true) == false);
    CHECK_FALSE(g_params.contains("apply"));
    CHECK(r.undo() == before);

    // In time order, each with its timecode, length and switch (on).
    const QVariantList rows = r.moments.deadTimeRanges();
    REQUIRE(rows.size() == 3);
    CHECK(r.row(0).value(QStringLiteral("startText")).toString() == r.controller.formatTimecode(Ticks::fromSeconds(1)));
    CHECK(r.row(0).value(QStringLiteral("lengthText")).toString() == QStringLiteral("2.0 s"));
    CHECK(r.row(1).value(QStringLiteral("start")).toDouble() == Catch::Approx(5.0));
    CHECK(r.row(2).value(QStringLiteral("seconds")).toDouble() == Catch::Approx(1.0));
    for (const QVariant& v : rows)
        CHECK(v.toMap().value(QStringLiteral("remove")).toBool());
    QVariantMap dead = r.moments.deadTime();
    CHECK(dead.value(QStringLiteral("found")).toBool());
    CHECK(dead.value(QStringLiteral("count")).toInt() == 3);
    CHECK(dead.value(QStringLiteral("seconds")).toDouble() == Catch::Approx(5.0));
    CHECK(dead.value(QStringLiteral("text")).toString().contains(QStringLiteral("0:05")));

    // Keep the second: Remove takes out the other two.
    r.moments.setDeadTimeRemove(1, false);
    CHECK_FALSE(r.row(1).value(QStringLiteral("remove")).toBool());
    dead = r.moments.deadTime();
    CHECK(dead.value(QStringLiteral("removeCount")).toInt() == 2);
    CHECK(dead.value(QStringLiteral("removeSeconds")).toDouble() == Catch::Approx(3.0));
    CHECK(dead.value(QStringLiteral("removeText")).toString() == QStringLiteral("2 of 3 to remove · 0:03"));
    REQUIRE(r.moments.removeDeadTime().isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Remove Dead Time"));
    CHECK(g_params.value("apply", false));
    REQUIRE(g_params["ranges"].size() == 2);
    CHECK(g_params["ranges"][0].value("start", std::int64_t{0}) == Ticks::fromSeconds(1).value);
    CHECK(g_params["ranges"][0].value("end", std::int64_t{0}) == Ticks::fromSeconds(3).value);
    CHECK(g_params["ranges"][1].value("start", std::int64_t{0}) == Ticks::fromSeconds(9).value);
    // The real ripple delete: 3 s shorter on both tracks.
    CHECK(r.contentEnd() == Ticks::fromSeconds(9));
    CHECK(r.controller.currentSequence()->findTrack(r.tl.track("A1"))->clips.back().end() == Ticks::fromSeconds(9));
    // Used up: the list is empty, the status says what went.
    CHECK(r.moments.deadTimeRanges().isEmpty());
    dead = r.moments.deadTime();
    CHECK_FALSE(dead.value(QStringLiteral("found")).toBool());
    CHECK(dead.value(QStringLiteral("text")).toString() == QStringLiteral("Removed 2 stretches, 0:03 in all"));
    CHECK_FALSE(r.moments.removeDeadTime().isEmpty());

    REQUIRE(r.controller.undo());
    CHECK(r.contentEnd() == Ticks::fromSeconds(12));

    // Every stretch kept: nothing to remove, no undo step.
    r.moments.findDeadTime();
    r.moments.setAllDeadTimeRemove(false);
    CHECK(r.moments.deadTime().value(QStringLiteral("removeCount")).toInt() == 0);
    const std::size_t kept = r.undo();
    CHECK(r.moments.removeDeadTime() == QStringLiteral("Every stretch is kept: switch one on to remove it"));
    CHECK(r.undo() == kept);
    r.moments.setAllDeadTimeRemove(true);
    CHECK(r.moments.deadTime().value(QStringLiteral("removeCount")).toInt() == 3);
    forgetOptions();
}

TEST_CASE("Dead time options are remembered and Only In to Out needs both marks", "[app][moments][deadtime]") {
    resetSettings();
    forgetOptions();
    {
        MomentsBridge a;
        QVariantMap o = a.deadTimeOptions();
        CHECK(o.value(QStringLiteral("sensitivity")).toDouble() == Catch::Approx(0.5));
        CHECK(o.value(QStringLiteral("minSeconds")).toDouble() == Catch::Approx(3.0));
        CHECK(o.value(QStringLiteral("handleSeconds")).toDouble() == Catch::Approx(0.3));
        CHECK_FALSE(o.value(QStringLiteral("inOut")).toBool());
        const auto changed = countOf(&a, &MomentsBridge::deadTimeOptionsChanged);
        a.setDeadTimeOption(QStringLiteral("sensitivity"), 1.5); // clamped
        a.setDeadTimeOption(QStringLiteral("minSeconds"), 0.1);
        a.setDeadTimeOption(QStringLiteral("handleSeconds"), 9.0);
        a.setDeadTimeOption(QStringLiteral("inOut"), true);
        a.setDeadTimeOption(QStringLiteral("inOut"), true); // no change
        a.setDeadTimeOption(QStringLiteral("nonsense"), 1);
        CHECK(*changed == 4);
        o = a.deadTimeOptions();
        CHECK(o.value(QStringLiteral("sensitivity")).toDouble() == Catch::Approx(1.0));
        CHECK(o.value(QStringLiteral("minSeconds")).toDouble() == Catch::Approx(0.5));
        CHECK(o.value(QStringLiteral("handleSeconds")).toDouble() == Catch::Approx(5.0));
    }
    MomentsBridge b;
    CHECK(b.deadTimeOptions().value(QStringLiteral("sensitivity")).toDouble() == Catch::Approx(1.0));
    CHECK(b.deadTimeOptions().value(QStringLiteral("minSeconds")).toDouble() == Catch::Approx(0.5));
    CHECK(b.deadTimeOptions().value(QStringLiteral("handleSeconds")).toDouble() == Catch::Approx(5.0));
    CHECK(b.deadTimeOptions().value(QStringLiteral("inOut")).toBool());

    // Only In to Out without marks: refused before anything is measured.
    DeadRig r;
    r.moments.findDeadTime();
    CHECK(g_params.is_null());
    CHECK(r.moments.deadTime().value(QStringLiteral("error")).toString() ==
          QStringLiteral("Set an In and an Out mark first, or turn off Only In to Out"));
    r.controller.setPlayhead(Ticks::fromSeconds(2));
    r.controller.markInAtPlayhead();
    r.controller.setPlayhead(Ticks::fromSeconds(8));
    r.controller.markOutAtPlayhead();
    r.moments.findDeadTime();
    CHECK(g_params.value("inOut", false));
    CHECK(g_params.value("minSeconds", 0.0) == Catch::Approx(0.5));
    CHECK(r.moments.deadTime().value(QStringLiteral("found")).toBool());
    forgetOptions();
}

TEST_CASE("Dead time bands are owner-tagged and leave the transcript highlights alone", "[app][moments][deadtime]") {
    resetSettings();
    forgetOptions();
    DeadRig r;
    // The transcript's selection (its owner, the overload without one).
    TimelineHighlight words;
    words.kind = TimelineHighlight::Kind::Selection;
    words.range = seconds(4, 4.5);
    words.tracks = {r.tl.track("A1")};
    r.controller.setTimelineHighlights({words});

    r.moments.findDeadTime();
    CHECK(r.bands().empty()); // the review is not open
    const auto reviewing = countOf(&r.moments, &MomentsBridge::deadTimeReviewingChanged);
    r.moments.setDeadTimeReviewing(true);
    CHECK(*reviewing == 1);
    CHECK(r.bands() == (std::vector<TimeRange>{seconds(1, 3), seconds(5, 7), seconds(9, 10)}));
    REQUIRE(r.controller.timelineHighlights().size() == 4);
    CHECK(r.controller.timelineHighlights().front().kind == TimelineHighlight::Kind::Selection); // the transcript's first
    // A kept stretch is no band.
    r.moments.setDeadTimeRemove(1, false);
    CHECK(r.bands() == (std::vector<TimeRange>{seconds(1, 3), seconds(9, 10)}));
    // The transcript replaces only its own: its cleanup preview (a removal ghost on A1), then nothing.
    TimelineHighlight ghost;
    ghost.kind = TimelineHighlight::Kind::Removal;
    ghost.range = seconds(4, 4.5);
    ghost.tracks = {r.tl.track("A1")};
    r.controller.setTimelineHighlights({ghost});
    CHECK(r.controller.timelineHighlights().size() == 3);
    CHECK(r.bands().size() == 2);
    r.controller.setTimelineHighlights({});
    CHECK(r.controller.timelineHighlights().size() == 2);
    // Dead time's own changes leave the transcript's alone too.
    r.controller.setTimelineHighlights({words});
    r.moments.setDeadTimeRemove(1, true);
    CHECK(r.controller.timelineHighlights().size() == 4);

    // The sequence's clips change (a razor): the stretches no longer match - no bands, and Remove asks for a new find.
    const TrackId v1[] = {r.tl.track("V1")};
    REQUIRE(r.controller.applyEditFn("Add Edit", [&](const ProjectState& s) {
        return edit::addEdit(s, r.tl.seq, Ticks::fromSeconds(6), v1, edit::LinkMode::FollowLinks);
    }));
    CHECK(r.moments.deadTime().value(QStringLiteral("stale")).toBool());
    CHECK(r.bands().empty());
    const std::size_t steps = r.undo();
    CHECK(r.moments.removeDeadTime() == QStringLiteral("The sequence changed since the dead time was found: find it again"));
    CHECK(r.undo() == steps);
    REQUIRE(r.controller.undo()); // as it was measured again
    CHECK_FALSE(r.moments.deadTime().value(QStringLiteral("stale")).toBool());
    CHECK(r.bands().size() == 3);

    // Closing the review takes the bands away (the transcript's stay); a new find while it is closed draws none.
    r.moments.setDeadTimeReviewing(false);
    CHECK(r.bands().empty());
    CHECK(r.controller.timelineHighlights().size() == 1);
    r.moments.setDeadTimeReviewing(true);
    CHECK(r.bands().size() == 3);
    // Cancel drops the list and its bands.
    r.moments.cancelDeadTime();
    CHECK(r.bands().empty());
    CHECK(r.moments.deadTimeRanges().isEmpty());
    CHECK(r.controller.timelineHighlights().size() == 1);
    forgetOptions();
}

TEST_CASE("Dead time: an option changed after a find says so and Remove waits for a new find", "[app][moments][deadtime]") {
    resetSettings();
    forgetOptions();
    DeadRig r;
    r.moments.findDeadTime();
    CHECK_FALSE(r.moments.deadTime().value(QStringLiteral("optionsChanged")).toBool());
    // Sensitivity moves: the list stays (nothing is measured again) and says it is from the earlier settings.
    g_params = Json();
    r.moments.setDeadTimeOption(QStringLiteral("sensitivity"), 0.8);
    CHECK(g_params.is_null());
    CHECK(r.moments.deadTime().value(QStringLiteral("optionsChanged")).toBool());
    CHECK(r.moments.deadTimeRanges().size() == 3);
    const std::size_t before = r.undo();
    CHECK(r.moments.removeDeadTime() ==
          QStringLiteral("The options changed since the dead time was found: find it again, or set them back"));
    CHECK(r.undo() == before);
    // Set back: the list matches its settings again.
    r.moments.setDeadTimeOption(QStringLiteral("sensitivity"), 0.5);
    CHECK_FALSE(r.moments.deadTime().value(QStringLiteral("optionsChanged")).toBool());
    // Each option counts; Find again clears it.
    r.moments.setDeadTimeOption(QStringLiteral("inOut"), true);
    CHECK(r.moments.deadTime().value(QStringLiteral("optionsChanged")).toBool());
    r.moments.setDeadTimeOption(QStringLiteral("inOut"), false);
    r.moments.setDeadTimeOption(QStringLiteral("handleSeconds"), 1.0);
    CHECK(r.moments.deadTime().value(QStringLiteral("optionsChanged")).toBool());
    r.moments.findDeadTime();
    CHECK(g_params.value("handleSeconds", 0.0) == Catch::Approx(1.0));
    CHECK_FALSE(r.moments.deadTime().value(QStringLiteral("optionsChanged")).toBool());
    CHECK(r.moments.removeDeadTime().isEmpty());
    CHECK(r.undo() == before + 1);
    // Nothing listed: no such state.
    r.moments.setDeadTimeOption(QStringLiteral("minSeconds"), 5.0);
    CHECK_FALSE(r.moments.deadTime().value(QStringLiteral("optionsChanged")).toBool());
    forgetOptions();
}

TEST_CASE("Dead time rows carry the frame at each stretch's middle", "[app][moments][deadtime]") {
    resetSettings();
    forgetOptions();
    DeadRig r;
    r.moments.findDeadTime();
    // A plays from its frame 0 at 0 s: the middles 2 s, 6 s and 9.5 s (on a frame) are those source times.
    const QString media = mediaKey(r.tl.media('A'));
    CHECK(r.row(0).value(QStringLiteral("thumbnail")).toString() ==
          QStringLiteral("image://wabcut-frames/%1/%2").arg(media).arg(Ticks::fromSeconds(2).value));
    CHECK(r.row(1).value(QStringLiteral("thumbnail")).toString() ==
          QStringLiteral("image://wabcut-frames/%1/%2").arg(media).arg(Ticks::fromSeconds(6).value));
    CHECK(r.row(2).value(QStringLiteral("thumbnail")).toString() ==
          QStringLiteral("image://wabcut-frames/%1/%2").arg(media).arg(snapToFrame(secs(9.5), kRate).value));
    forgetOptions();
}

TEST_CASE("Dead time keyboard review moves jumps keeps and removes", "[app][moments][deadtime]") {
    resetSettings();
    forgetOptions();
    DeadRig r;
    CHECK_FALSE(r.moments.reviewDeadTime(QStringLiteral("next"))); // nothing listed yet
    r.moments.findDeadTime();
    const auto reveal = countOf(&r.moments, &MomentsBridge::revealRequested);
    CHECK(r.moments.deadTimeCurrent() == -1);
    // Down: the first stretch, the playhead on it.
    CHECK(r.moments.reviewDeadTime(QStringLiteral("next")));
    CHECK(r.moments.deadTimeCurrent() == 0);
    CHECK(r.controller.playhead() == Ticks::fromSeconds(1));
    CHECK(*reveal == 1);
    // K keeps it and goes on to the next.
    CHECK(r.moments.reviewDeadTime(QStringLiteral("keep")));
    CHECK_FALSE(r.row(0).value(QStringLiteral("remove")).toBool());
    CHECK(r.moments.deadTimeCurrent() == 1);
    CHECK(r.controller.playhead() == Ticks::fromSeconds(5));
    // Delete removes it (it was on) and goes on.
    CHECK(r.moments.reviewDeadTime(QStringLiteral("remove")));
    CHECK(r.row(1).value(QStringLiteral("remove")).toBool());
    CHECK(r.moments.deadTimeCurrent() == 2);
    // On the last row it stays there.
    CHECK(r.moments.reviewDeadTime(QStringLiteral("keep")));
    CHECK_FALSE(r.row(2).value(QStringLiteral("remove")).toBool());
    CHECK(r.moments.deadTimeCurrent() == 2);
    CHECK(r.moments.reviewDeadTime(QStringLiteral("toggle")));
    CHECK(r.row(2).value(QStringLiteral("remove")).toBool());
    // Up, Home, End.
    CHECK(r.moments.reviewDeadTime(QStringLiteral("previous")));
    CHECK(r.moments.deadTimeCurrent() == 1);
    CHECK(r.moments.reviewDeadTime(QStringLiteral("first")));
    CHECK(r.moments.deadTimeCurrent() == 0);
    CHECK(r.moments.reviewDeadTime(QStringLiteral("previous")));
    CHECK(r.moments.deadTimeCurrent() == 0);
    CHECK(r.moments.reviewDeadTime(QStringLiteral("last")));
    CHECK(r.controller.playhead() == Ticks::fromSeconds(9));
    CHECK_FALSE(r.moments.reviewDeadTime(QStringLiteral("dance")));
    // A row's click.
    r.moments.jumpToDeadTime(1);
    CHECK(r.moments.deadTimeCurrent() == 1);
    CHECK(r.controller.playhead() == Ticks::fromSeconds(5));
    r.moments.jumpToDeadTime(7); // out of range: nothing
    CHECK(r.moments.deadTimeCurrent() == 1);
    // The review edits nothing.
    CHECK(r.undo() == 0);
    forgetOptions();
}

TEST_CASE("Dead time find reports its progress and Cancel drops its result", "[app][moments][deadtime]") {
    resetSettings();
    forgetOptions();
    DeadRig r;
    QVariantMap during;
    g_during = [&](command::ExecutionContext& c) {
        c.checkpoint("measuring activity (40%)");
        during = r.moments.deadTime();
    };
    r.moments.findDeadTime();
    CHECK(during.value(QStringLiteral("busy")).toBool());
    CHECK(during.value(QStringLiteral("progress")).toDouble() == Catch::Approx(0.4));
    CHECK_FALSE(r.moments.deadTime().value(QStringLiteral("busy")).toBool());
    CHECK(r.moments.deadTime().value(QStringLiteral("found")).toBool());

    // Cancel while it measures: it stops at the next checkpoint and nothing is listed.
    g_during = [&](command::ExecutionContext& c) {
        r.moments.cancelDeadTime();
        c.checkpoint("measuring activity (60%)");
    };
    r.moments.findDeadTime();
    CHECK(r.moments.deadTimeRanges().isEmpty());
    const QVariantMap dead = r.moments.deadTime();
    CHECK_FALSE(dead.value(QStringLiteral("busy")).toBool());
    CHECK_FALSE(dead.value(QStringLiteral("found")).toBool());
    CHECK_FALSE(dead.contains(QStringLiteral("error")));
    CHECK(r.undo() == 0);

    // Find again right after Cancel, while the cancelled one still winds down: it starts once that one has stopped.
    int calls = 0;
    bool waiting = false;
    g_during = [&](command::ExecutionContext& c) {
        if (++calls > 1)
            return;
        r.moments.cancelDeadTime();
        r.moments.findDeadTime();
        waiting = r.moments.deadTime().value(QStringLiteral("busy")).toBool();
        c.checkpoint("measuring activity (60%)");
    };
    r.moments.findDeadTime();
    CHECK(waiting);
    CHECK(calls == 2);
    CHECK(r.moments.deadTime().value(QStringLiteral("found")).toBool());
    CHECK(r.moments.deadTimeRanges().size() == 3);
    forgetOptions();
}

// ---- In the shell ---------------------------------------------------------------------------------------------------

namespace {

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml, QRectF at) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/p05-deadtime/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setPosition(at.topLeft());
    item->setSize(at.size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

/// Lays out `root` and everything under it now (a window that is never shown only polishes when a frame is rendered).
void polishAll(QQuickItem* root) {
    if (!root)
        return;
    root->ensurePolished();
    for (QQuickItem* child : root->childItems())
        polishAll(child);
}

void saveArea(Shell& shell, const QRectF& area, const QString& file) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
}

} // namespace

TEST_CASE("Dead time review sheet in the Highlights panel: list, switches, keys, bands; screenshots dark and light",
          "[app][moments][deadtime][qml][screenshot]") {
    resetSettings();
    clearMessages();
    forgetOptions();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* moments = shell.engine.singletonInstance<MomentsBridge*>("Wabcut", "Moments");
    REQUIRE(controller);
    REQUIRE(moments);
    moments->setRegistry(&deadTimeCatalog());
    moments->setSynchronous(true);
    g_params = Json();
    g_during = {};
    // The thumbnails' frames: a colour per second with a bright ball (decoded on the provider's thread, so counted
    // atomically).
    static std::atomic<int> decoded{0};
    decoded = 0;
    setPreviewFramesForTests([](const MediaItem&, Ticks time) {
        ++decoded;
        QImage image(320, 180, QImage::Format_RGB888);
        const int hue = static_cast<int>(time.toSeconds() * 37.0) % 360;
        image.fill(QColor::fromHsv(hue, 140, 80));
        QPainter p(&image);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor::fromHsv((hue + 180) % 360, 200, 235));
        p.drawEllipse(QPointF(160 + 60 * std::sin(time.toSeconds()), 90), 50, 50);
        return image;
    });
    Timeline tl{picture()};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    spin(200);
    QQuickWindow* w = shell.window.get();
    const auto undo = [&] { return controller->session().undoCount(); };
    const auto bands = [&] {
        std::size_t n = 0;
        for (const TimelineHighlight& h : controller->timelineHighlights())
            n += h.kind == TimelineHighlight::Kind::Removal ? 1 : 0;
        return n;
    };

    // The Highlights panel in the browser slot, over the Library; its scissors button opens the sheet.
    QQuickItem* library = findNamed(w->contentItem(), QStringLiteral("libraryPanel"));
    REQUIRE(library);
    const auto panel = makeItem(shell, "import Wabcut\nHighlightsPanel {}\n", sceneRect(library));
    QQuickItem* open = findNamed(panel.get(), QStringLiteral("highlightsDeadTimeButton"));
    QQuickItem* sheet = findNamed(panel.get(), QStringLiteral("deadTimeReview"));
    REQUIRE(open);
    REQUIRE(sheet);
    CHECK(open->isVisible());
    CHECK_FALSE(sheet->isVisible());
    const auto requested = countOf(moments, &MomentsBridge::deadTimeReviewRequested);
    REQUIRE(QMetaObject::invokeMethod(open, "clicked"));
    CHECK(*requested == 1);
    CHECK(moments->deadTimeReviewing());
    CHECK(sheet->isVisible());
    polishAll(panel.get());
    CHECK(findNamed(panel.get(), QStringLiteral("deadTimeEmpty"))->isVisible());

    // The options show the remembered values; Find lists the stretches.
    QQuickItem* sensitivity = findNamed(sheet, QStringLiteral("deadTimeSensitivity"));
    REQUIRE(sensitivity);
    CHECK(sensitivity->property("value").toDouble() == Catch::Approx(50));
    CHECK(findNamed(sheet, QStringLiteral("deadTimeMinSecondsValue"))->property("text").toString() == QStringLiteral("3.0 s"));
    REQUIRE(QMetaObject::invokeMethod(findNamed(sheet, QStringLiteral("deadTimeInOut")), "toggle"));
    REQUIRE(QMetaObject::invokeMethod(findNamed(sheet, QStringLiteral("deadTimeInOut")), "toggled"));
    CHECK(moments->deadTimeOptions().value(QStringLiteral("inOut")).toBool());
    REQUIRE(QMetaObject::invokeMethod(findNamed(sheet, QStringLiteral("deadTimeInOut")), "toggle"));
    REQUIRE(QMetaObject::invokeMethod(findNamed(sheet, QStringLiteral("deadTimeInOut")), "toggled"));
    CHECK_FALSE(moments->deadTimeOptions().value(QStringLiteral("inOut")).toBool());
    QQuickItem* find = findNamed(sheet, QStringLiteral("deadTimeFindButton"));
    REQUIRE(find);
    CHECK(find->property("text").toString() == QStringLiteral("Find dead time"));
    REQUIRE(QMetaObject::invokeMethod(find, "clicked"));
    CHECK(g_params.value("sensitivity", 0.0) == Catch::Approx(0.5));
    polishAll(panel.get());
    polishAll(panel.get()); // the rows the list made
    REQUIRE(findNamed(sheet, QStringLiteral("deadTimeRow_2")));
    CHECK(find->property("text").toString() == QStringLiteral("Find again"));
    // Each row's thumbnail loads off the GUI thread from the frame provider, once.
    const auto thumbs = [&] {
        QList<QQuickItem*> out;
        for (int i = 0; i < 3; ++i) {
            if (QQuickItem* row = findNamed(sheet, QStringLiteral("deadTimeRow_%1").arg(i)))
                out << findNamed(row, QStringLiteral("deadTimeThumb"));
        }
        return out;
    };
    REQUIRE(waitFor([&] {
        const QList<QQuickItem*> images = thumbs();
        return images.size() == 3 && std::all_of(images.begin(), images.end(), [](QQuickItem* i) {
                   return i && i->property("status").toInt() == 1; // Image.Ready
               });
    }, 5000));
    CHECK(thumbs()[1]->property("source").toUrl().toString().startsWith(QStringLiteral("image://wabcut-frames/")));
    const int firstDecodes = decoded.load();
    CHECK(firstDecodes == 3);
    CHECK(findNamed(sheet, QStringLiteral("deadTimeSummary"))->property("text").toString() == QStringLiteral("3 of 3 to remove · 0:05"));
    CHECK(bands() == 3);
    QQuickItem* remove = findNamed(sheet, QStringLiteral("deadTimeRemoveButton"));
    REQUIRE(remove);
    CHECK(remove->property("text").toString() == QStringLiteral("Remove 3 stretches"));
    // The options fold away once stretches are listed; the sliders button shows them again.
    QQuickItem* options = findNamed(sheet, QStringLiteral("deadTimeOptions"));
    QQuickItem* optionsButton = findNamed(sheet, QStringLiteral("deadTimeOptionsButton"));
    REQUIRE(options);
    REQUIRE(optionsButton);
    CHECK_FALSE(options->isVisible());
    CHECK(optionsButton->isVisible());
    REQUIRE(QMetaObject::invokeMethod(optionsButton, "clicked"));
    CHECK(options->isVisible());
    REQUIRE(QMetaObject::invokeMethod(optionsButton, "clicked"));
    CHECK_FALSE(options->isVisible());

    // A row's switch: kept.
    QQuickItem* second = findNamed(sheet, QStringLiteral("deadTimeSwitch_1"));
    REQUIRE(second);
    REQUIRE(QMetaObject::invokeMethod(second, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(second, "toggled"));
    CHECK_FALSE(moments->deadTimeRanges().value(1).toMap().value(QStringLiteral("remove")).toBool());
    CHECK(bands() == 2);
    // The rows were made again with the switch: their thumbnails come from the pixmap cache, nothing is decoded again
    // (nor on a repaint).
    polishAll(panel.get());
    polishAll(panel.get());
    REQUIRE_FALSE(shell.grab().isNull());
    spin(100);
    CHECK(decoded.load() == firstDecodes);

    // The keyboard: Tab reaches the list; Down jumps to the first, K keeps it, Delete removes the next.
    QQuickItem* list = findNamed(sheet, QStringLiteral("deadTimeList"));
    REQUIRE(list);
    CHECK(tabReaches(sheet, QStringLiteral("deadTimeList")));
    CHECK(pressWithKey(w, list, Qt::Key_Down));
    CHECK(moments->deadTimeCurrent() == 0);
    CHECK(controller->playhead() == Ticks::fromSeconds(1));
    CHECK(pressWithKey(w, list, Qt::Key_K, QStringLiteral("k")));
    CHECK_FALSE(moments->deadTimeRanges().value(0).toMap().value(QStringLiteral("remove")).toBool());
    CHECK(moments->deadTimeCurrent() == 1);
    const std::size_t clips = controller->currentSequence()->videoTracks[0]->clips.size();
    CHECK(pressWithKey(w, list, Qt::Key_Delete));
    CHECK(moments->deadTimeRanges().value(1).toMap().value(QStringLiteral("remove")).toBool());
    CHECK(moments->deadTimeCurrent() == 2);
    CHECK(controller->currentSequence()->videoTracks[0]->clips.size() == clips); // Delete did not clear clips
    CHECK(list->hasActiveFocus());
    CHECK(bands() == 2);
    polishAll(panel.get());
    polishAll(panel.get());
    {
        const QStringList issues = auditAccessibility(sheet);
        INFO(join(issues));
        CHECK(issues.isEmpty());
    }

    // Screenshots: the sheet with the timeline's bands (dark), the sheet (light).
    QQuickItem* timeline = findNamed(w->contentItem(), QStringLiteral("timelinePanel"));
    REQUIRE(timeline);
    if (auto* view = timeline->property("view").value<QObject*>()) {
        view->setProperty("pixelsPerSecond", 90.0);
        view->setProperty("scrollX", 0.0);
    }
    spin(200);
    shell.grab(); // lets the timeline draw the bands before the screenshot frame
    spin(100);
    saveArea(shell, sceneRect(panel.get()).united(sceneRect(timeline)), QStringLiteral("dead-time-review.png"));
    CHECK(decoded.load() == firstDecodes);

    // An option changed after the find (light theme, the options open): the button says so, the list dims, Remove
    // waits; nothing is measured again. Fix round 1 (QA Flash #1 / #5): its note is in Theme.warningText (readable on
    // the light panel) and the screenshot has the timeline's bands too, like the dark one.
    shell.setAppearance(false, "ultraviolet");
    sheet->setProperty("optionsOpen", true);
    g_params = Json();
    moments->setDeadTimeOption(QStringLiteral("sensitivity"), 0.75);
    polishAll(panel.get());
    CHECK(g_params.is_null());
    CHECK(find->property("text").toString() == QStringLiteral("Options changed: Find again"));
    CHECK(findNamed(sheet, QStringLiteral("deadTimeOptionsChangedNote"))->isVisible());
    CHECK(list->property("opacity").toDouble() < 1.0);
    CHECK_FALSE(remove->isEnabled());
    CHECK(sameColor(findNamed(sheet, QStringLiteral("deadTimeOptionsChangedNote"))->property("color").value<QColor>(),
                    shell.themeColor("warningText")));
    CHECK_FALSE(sameColor(shell.themeColor("warningText"), shell.themeColor("warning")));
    CHECK(bands() == 2); // still drawn: the list itself is not stale
    spin(300);
    shell.grab(); // lets the timeline draw in the light theme before the screenshot frame
    spin(100);
    saveArea(shell, sceneRect(panel.get()).united(sceneRect(timeline)), QStringLiteral("dead-time-review-light.png"));
    // Set back: the list is current again.
    moments->setDeadTimeOption(QStringLiteral("sensitivity"), 0.5);
    CHECK(find->property("text").toString() == QStringLiteral("Find again"));
    CHECK(remove->isEnabled());
    sheet->setProperty("optionsOpen", false);
    shell.setAppearance(true, "ultraviolet");
    spin(100);

    // Remove: the switched-on stretches (5 s to 7 s and 9 s to 10 s), one undo step; the sheet says what went.
    const std::size_t before = undo();
    REQUIRE(QMetaObject::invokeMethod(remove, "clicked"));
    CHECK(undo() == before + 1);
    CHECK(controller->currentSequence()->contentEnd() == Ticks::fromSeconds(9));
    polishAll(panel.get());
    CHECK(findNamed(sheet, QStringLiteral("deadTimeStatus"))->property("text").toString() ==
          QStringLiteral("Removed 2 stretches, 0:03 in all"));
    CHECK(bands() == 0);

    // The × closes the sheet: the moments are back.
    // With stretches listed, closing drops the rows (and the loads of their thumbnails): the list is empty while the
    // sheet is closed and fills again when it opens.
    moments->findDeadTime();
    polishAll(panel.get());
    CHECK(list->property("count").toInt() == 3);
    REQUIRE(QMetaObject::invokeMethod(findNamed(sheet, QStringLiteral("deadTimeClose")), "clicked"));
    CHECK_FALSE(moments->deadTimeReviewing());
    CHECK_FALSE(sheet->isVisible());
    CHECK(list->property("count").toInt() == 0);
    moments->setDeadTimeReviewing(true);
    CHECK(list->property("count").toInt() == 3);
    moments->setDeadTimeReviewing(false);
    setPreviewFramesForTests(nullptr);
    moments->setRegistry(nullptr);
    moments->setSynchronous(false);
    forgetOptions();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
