// M5n performance (the P.1 hotspots), offscreen; no window is shown. Tagged [perf]: ctest labels them "perf", so the
// app|gpu|keymap runs skip them. Nothing is asserted about the numbers (only that the edits and frames happened); they
// are written to test-output/perf/*.json and printed.
//   * Commit fan-out per edit on a 10,000-clip sequence: a move, a trim and their undos, measured as the model edit
//     alone (EditSession), through a bare ProjectController (no QML: the controller's own work - selection pruning,
//     the clip list and track models), and in the real window (every bridge and QML binding listening), then with
//     the listeners P.1's profile names detached one at a time to attribute the fan-out.
//   * Timeline labels on 10,000 clips: TimelineView::updatePaintNode (its own stats().updateNs) after an edit on a
//     visible track, a zoom step and a jump scroll, in a persistent offscreen scene graph (RenderHarness).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "ClipAccessModel.h"
#include "ColorBridge.h"
#include "EffectControls.h"
#include "MarkersBridge.h"
#include "MaskBridge.h"
#include "ProjectController.h"
#include "TimelineView.h"
#include "TranscriptBridge.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr int kTracks = 4;           // V1-V4 and A1-A4
constexpr int kClipsPerTrack = 1250; // 4 x 1250 video + 4 x 1250 audio = 10,000 clips
constexpr int kClipFrames = 10;
constexpr int kMediaFiles = 200;
constexpr int kMarkers = 200;
constexpr int kWarmup = 3;
constexpr int kRuns = 30;

struct BigTimeline {
    ProjectState state;
    SequenceId seq;
    std::vector<std::vector<ClipId>> video; ///< per track, in time order
    std::vector<std::vector<ClipId>> audio;
};

/// `kTracks` video and audio tracks of adjacent 10-frame clips from 200 media files (clips keep their media's name),
/// Vn/An pairs linked like imported A/V clips, and sequence markers every 50 frames (tests/model/test_perf.cpp's
/// layout).
BigTimeline makeBigTimeline() {
    BigTimeline big;
    for (int m = 0; m < kMediaFiles; ++m) {
        char name[32];
        std::snprintf(name, sizeof name, "A%03d_C%03d_0101XY.mov", m / 10, m);
        MediaItem item;
        item.name = name;
        item.locator.path = std::string("D:/footage/") + name;
        item.info.duration = f(1'000'000);
        item.info.video.push_back({});
        item.info.video[0].duration = item.info.duration;
        item.info.audio.push_back({});
        item.info.audio[0].duration = item.info.duration;
        auto added = edit::addMedia(big.state, item);
        REQUIRE(added);
        big.state = std::move(added->state);
    }
    std::vector<const MediaItem*> media;
    for (const MediaPtr& m : big.state.media)
        media.push_back(m.get());
    edit::SequenceSettings settings;
    settings.frameRate = kRate;
    settings.videoTracks = kTracks;
    settings.audioTracks = kTracks;
    auto created = edit::addSequence(big.state, settings);
    REQUIRE(created);
    big.state = std::move(created->state);
    big.seq = created->id;

    auto seq = std::make_shared<Sequence>(*big.state.findSequence(big.seq));
    std::vector<std::shared_ptr<Track>> video;
    std::vector<std::shared_ptr<Track>> audio;
    for (int t = 0; t < kTracks; ++t) {
        video.push_back(std::make_shared<Track>(*seq->videoTracks[static_cast<std::size_t>(t)]));
        audio.push_back(std::make_shared<Track>(*seq->audioTracks[static_cast<std::size_t>(t)]));
    }
    big.video.resize(kTracks);
    big.audio.resize(kTracks);
    for (int t = 0; t < kTracks; ++t) {
        for (int i = 0; i < kClipsPerTrack; ++i) {
            const LinkGroupId group = big.state.ids.allocate<LinkGroupId>();
            const MediaItem& m = *media[static_cast<std::size_t>((i + t * 37) % kMediaFiles)];
            for (auto* list : {&video, &audio}) {
                Clip c;
                c.id = big.state.ids.allocate<ClipId>();
                c.media = m.id;
                c.stream = {list == &video ? StreamKind::Video : StreamKind::Audio, 0};
                c.timelineRange = {f(static_cast<std::int64_t>(i) * kClipFrames), f(kClipFrames)};
                c.sourceIn = f(static_cast<std::int64_t>(i % 50) * kClipFrames);
                c.setContent(m.defaultClipContent());
                c.linkGroup = group;
                (list == &video ? big.video : big.audio)[static_cast<std::size_t>(t)].push_back(c.id);
                (*list)[static_cast<std::size_t>(t)]->clips.push_back(std::move(c));
            }
        }
    }
    for (int t = 0; t < kTracks; ++t) {
        seq->videoTracks[static_cast<std::size_t>(t)] = video[static_cast<std::size_t>(t)];
        seq->audioTracks[static_cast<std::size_t>(t)] = audio[static_cast<std::size_t>(t)];
    }
    for (int i = 0; i < kMarkers; ++i) {
        Marker marker;
        marker.start = f(static_cast<std::int64_t>(i) * 50);
        marker.name = "Marker " + std::to_string(i + 1);
        seq->markers.push_back(std::move(marker));
    }
    seq->duration = seq->contentEnd();
    const IdGenerator ids = big.state.ids;
    big.state = big.state.withSequence(seq);
    big.state.ids = ids;
    return big;
}

struct Samples {
    std::vector<double> ms;
    void add(double value) { ms.push_back(value); }
    double percentile(double p) const {
        if (ms.empty())
            return 0.0;
        std::vector<double> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        const auto at = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
        return sorted[std::min(at, sorted.size() - 1)];
    }
    double p50() const { return percentile(0.5); }
    double p95() const { return percentile(0.95); }
    QJsonObject json() const {
        return {{QStringLiteral("p50Ms"), p50()}, {QStringLiteral("p95Ms"), p95()}, {QStringLiteral("runs"), static_cast<int>(ms.size())}};
    }
    std::string text() const {
        char out[64];
        std::snprintf(out, sizeof out, "p50 %.2f ms, p95 %.2f ms", p50(), p95());
        return out;
    }
};

double msOf(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1e6;
}

/// What the GUI thread still has queued after a commit (posted events, queued connections, Qt.callLater): run it.
void settle() {
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
}

QString perfDir() {
    const QString dir = QStringLiteral(WABCUT_TEST_OUTPUT_DIR "/perf");
    QDir().mkpath(dir);
    return dir;
}

void writePerf(const QString& name, const QJsonObject& report) {
    QFile file(perfDir() + QLatin1Char('/') + name);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        file.write(QJsonDocument(report).toJson());
}

/// The edits a drag and a trim commit (TimelineView::finishGesture: ProjectController::commitGesture with the gesture's
/// pure edit), each followed by its undo. `elsewhere` = clips the selection does not hold.
struct Edits {
    SequenceId seq;
    ClipId moved;   ///< V2, mid-sequence
    ClipId trimmed; ///< V3, mid-sequence
    ClipId selected;

    edit::EditOutcome move(const ProjectState& s, ClipId clip) const {
        const ClipId clips[] = {clip};
        return edit::moveClips(s, seq, clips, f(1));
    }
    edit::EditOutcome trim(const ProjectState& s, ClipId clip) const {
        return edit::trimEdge(s, seq, clip, edit::Edge::Tail, -f(1));
    }
};

Edits editsFor(const BigTimeline& big) {
    return Edits{big.seq, big.video[1][kClipsPerTrack / 2], big.video[2][kClipsPerTrack / 2 + 100], big.video[0][10]};
}

struct FanOut {
    Samples move, trim, undo, moveSelected;
    QJsonObject json() const {
        return {{QStringLiteral("move"), move.json()},
                {QStringLiteral("trim"), trim.json()},
                {QStringLiteral("undo"), undo.json()},
                {QStringLiteral("moveSelected"), moveSelected.json()}};
    }
};

/// Commits through `commit` and undoes through `undo`, timing each until the GUI thread is idle again.
FanOut measure(const Edits& e, const std::function<bool(const char*, const std::function<edit::EditOutcome(const ProjectState&)>&)>& commit,
               const std::function<bool()>& undo, bool processEvents) {
    FanOut out;
    const auto timed = [&](Samples* samples, const std::function<bool()>& step) {
        QElapsedTimer timer;
        timer.start();
        const bool ok = step();
        if (processEvents)
            settle();
        REQUIRE(ok);
        if (samples)
            samples->add(msOf(timer));
    };
    for (int i = 0; i < kWarmup + kRuns; ++i) {
        const bool record = i >= kWarmup;
        timed(record ? &out.move : nullptr, [&] { return commit("Move", [&](const ProjectState& s) { return e.move(s, e.moved); }); });
        timed(record ? &out.undo : nullptr, undo);
        timed(record ? &out.trim : nullptr, [&] { return commit("Trim", [&](const ProjectState& s) { return e.trim(s, e.trimmed); }); });
        timed(record ? &out.undo : nullptr, undo);
        timed(record ? &out.moveSelected : nullptr,
              [&] { return commit("Move", [&](const ProjectState& s) { return e.move(s, e.selected); }); });
        timed(record ? &out.undo : nullptr, undo);
    }
    return out;
}

} // namespace

TEST_CASE("Perf: commit fan-out per edit on a 10,000-clip sequence; report", "[app][perf]") {
    resetSettings();
    clearMessages();
    const BigTimeline big = makeBigTimeline();
    const Edits e = editsFor(big);
    REQUIRE(big.video[0].size() + big.audio[0].size() == 2 * kClipsPerTrack);
    QJsonObject report{{QStringLiteral("clips"), kTracks * kClipsPerTrack * 2}, {QStringLiteral("markers"), kMarkers}};
    QStringList lines;

    // 1. The model edit alone.
    {
        EditSession session(big.state);
        const FanOut model = measure(
            e,
            [&](const char* label, const auto& fn) { return session.apply(label, fn).has_value(); },
            [&] { return session.undo().has_value(); }, false);
        report.insert(QStringLiteral("model"), model.json());
        lines << QStringLiteral("model only: move %1, trim %2, undo %3")
                     .arg(QString::fromStdString(model.move.text()), QString::fromStdString(model.trim.text()),
                          QString::fromStdString(model.undo.text()));
    }
    // 2. A bare controller: + selection pruning, the clip list (ClipAccessModel), the track model.
    {
        app::ProjectController controller;
        controller.resetProject(big.state, big.seq, {e.selected});
        const std::size_t steps = controller.session().undoCount();
        const FanOut bare = measure(
            e, [&](const char* label, const auto& fn) { return controller.commitGesture(label, fn); },
            [&] { return controller.undo(); }, true);
        CHECK(controller.session().undoCount() == steps);
        report.insert(QStringLiteral("controller"), bare.json());
        lines << QStringLiteral("bare controller: move %1, trim %2, undo %3")
                     .arg(QString::fromStdString(bare.move.text()), QString::fromStdString(bare.trim.text()),
                          QString::fromStdString(bare.undo.text()));

        // Its pieces P.1 names: the clip list's update of one track (with rebuildIndex) and a pruneSelection-like
        // set of every clip id.
        const SequencePtr seq = controller.currentSequence();
        REQUIRE(seq);
        app::ClipAccessModel list(&controller);
        list.reset(seq.get());
        const TrackId v2 = seq->videoTracks[1]->id;
        const TrackId tracks[] = {v2};
        Samples update, prune;
        for (int i = 0; i < kWarmup + kRuns; ++i) {
            QElapsedTimer timer;
            timer.start();
            list.updateTracks(*seq, tracks);
            if (i >= kWarmup)
                update.add(msOf(timer));
            timer.restart();
            std::set<ClipId> present;
            seq->forEachTrack([&](const Track& t) {
                for (const Clip& c : t.clips)
                    present.insert(c.id);
            });
            if (i >= kWarmup)
                prune.add(msOf(timer));
            CHECK(present.size() == static_cast<std::size_t>(kTracks * kClipsPerTrack * 2));
        }
        report.insert(QStringLiteral("clipListUpdateTracks"), update.json());
        report.insert(QStringLiteral("pruneSelectionSet"), prune.json());
        lines << QStringLiteral("ClipAccessModel::updateTracks(1 track) %1; pruneSelection's set %2")
                     .arg(QString::fromStdString(update.text()), QString::fromStdString(prune.text()));
    }
    // 3. The real window: every bridge and QML binding listening.
    Shell shell;
    REQUIRE(shell.window);
    auto* controller = shell.engine.singletonInstance<app::ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    controller->resetProject(big.state, big.seq, {e.selected});
    // The panels that follow the selected clip attach to it (Effect Controls, Color, Masks).
    auto* color = shell.engine.singletonInstance<app::ColorBridge*>("Wabcut", "Color");
    auto* masks = shell.engine.singletonInstance<app::MaskBridge*>("Wabcut", "Masks");
    auto* markers = shell.engine.singletonInstance<app::MarkersBridge*>("Wabcut", "Markers");
    auto* transcript = shell.engine.singletonInstance<app::TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(color);
    REQUIRE(masks);
    REQUIRE(markers);
    REQUIRE(transcript);
    settle();
    const auto commitIn = [&](app::ProjectController* c) {
        return [c](const char* label, const auto& fn) { return c->commitGesture(label, fn); };
    };
    const auto undoIn = [&](app::ProjectController* c) { return [c] { return c->undo(); }; };
    const FanOut full = measure(e, commitIn(controller), undoIn(controller), true);
    report.insert(QStringLiteral("window"), full.json());
    lines << QStringLiteral("window: move %1, trim %2, undo %3, move of the selected clip %4")
                 .arg(QString::fromStdString(full.move.text()), QString::fromStdString(full.trim.text()),
                      QString::fromStdString(full.undo.text()), QString::fromStdString(full.moveSelected.text()));

    // 4. Attribution: the listeners P.1's profile names, detached one at a time (cumulative); each row is the window
    // without it and the ones above. The bridges stop following the project after this: the shell is not reused.
    struct Listener {
        const char* name;
        QObject* receiver;
    };
    const Listener listeners[] = {
        {"MarkersBridge", markers},
        {"TranscriptBridge", transcript},
        {"EffectControlsModel", controller->effectControls()},
        {"ColorBridge", color},
        {"MaskBridge", masks},
    };
    QJsonArray detached;
    double previous = (full.move.p50() + full.trim.p50() + full.undo.p50()) / 3.0;
    for (const Listener& l : listeners) {
        QObject::disconnect(controller, nullptr, l.receiver, nullptr);
        const FanOut without = measure(e, commitIn(controller), undoIn(controller), true);
        const double mean = (without.move.p50() + without.trim.p50() + without.undo.p50()) / 3.0;
        QJsonObject row = without.json();
        row.insert(QStringLiteral("detached"), QString::fromLatin1(l.name));
        row.insert(QStringLiteral("savedMsPerEdit"), previous - mean);
        detached.append(row);
        lines << QStringLiteral("  without %1 (and the above): move %2, trim %3, undo %4 (%5 ms less per edit)")
                     .arg(QString::fromLatin1(l.name), QString::fromStdString(without.move.text()),
                          QString::fromStdString(without.trim.text()), QString::fromStdString(without.undo.text()))
                     .arg(previous - mean, 0, 'f', 2);
        previous = mean;
    }
    report.insert(QStringLiteral("detached"), detached);
    writePerf(QStringLiteral("ui-commit-fanout.json"), report);
    WARN("commit fan-out, 10,000 clips:\n" << lines.join(QLatin1Char('\n')).toStdString());
    takeMessages(); // detached bridges may warn; not the point here
}

TEST_CASE("Perf: timeline label build in updatePaintNode on 10,000 clips; report", "[app][perf]") {
    resetSettings();
    clearMessages();
    const BigTimeline big = makeBigTimeline();
    const Edits e = editsFor(big);
    const QSize size(1600, 900);
    app::ProjectController controller;
    controller.resetProject(big.state, big.seq, {e.selected});
    RenderHarness harness(size);
    REQUIRE(harness.initialize());
    app::TimelineView* view = addView(harness, controller, size);
    // 6 px a frame: a 10-frame clip is 60 px, so its 22-character name is elided; a few hundred clips are visible.
    const double ppsA = 150.0;
    const double ppsB = 158.0;
    view->setPixelsPerSecond(ppsA);
    const double clipPx = ppsA * kClipFrames / 25.0;
    const double middle = clipPx * (kClipsPerTrack / 2 - 10);
    view->setScrollX(middle);
    view->setScrollY(0);
    // Lays out every label (the per-frame text layout budget spreads them over frames).
    const auto drain = [&] {
        harness.frame();
        for (int i = 0; i < 400 && view->stats().labelsPending; ++i) {
            QCoreApplication::processEvents(); // deliver the label budget's queued update
            harness.frame();
        }
    };
    drain();
    REQUIRE(view->stats().visibleClips > 100);
    REQUIRE(view->stats().labels > 100);
    const int labels = view->stats().labels;
    const int visible = view->stats().visibleClips;

    struct Scenario {
        Samples update; ///< updatePaintNode (stats().updateNs) of the frame after the change
        Samples frame;  ///< the whole frame: polish, sync (updatePaintNode), render
        QJsonObject json() const {
            return {{QStringLiteral("updatePaintNode"), update.json()}, {QStringLiteral("frame"), frame.json()}};
        }
    };
    const auto timedFrame = [&](Scenario& s, bool record) {
        QElapsedTimer timer;
        timer.start();
        harness.frame();
        if (record) {
            s.frame.add(msOf(timer));
            s.update.add(static_cast<double>(view->stats().updateNs) / 1e6);
        }
    };
    Scenario edit, zoom, scroll;
    int rebuiltTracks = 0;
    bool zoomFull = true;
    bool scrollFull = true;
    const ClipId visibleClip = big.video[1][kClipsPerTrack / 2]; // V2, in view
    for (int i = 0; i < kWarmup + kRuns; ++i) {
        const bool record = i >= kWarmup;
        // Every round starts from the same view (not measured).
        view->setPixelsPerSecond(ppsA);
        view->setScrollX(middle);
        drain();
        // An edit on a visible track (a drag committed), then its undo: the track's clips and labels are rebuilt.
        REQUIRE(controller.commitGesture("Move", [&](const ProjectState& s) { return e.move(s, visibleClip); }));
        timedFrame(edit, record);
        rebuiltTracks = std::max(rebuiltTracks, view->stats().rebuiltTracks);
        drain();
        REQUIRE(controller.undo());
        timedFrame(edit, record);
        drain();
        // A zoom step: every visible track is rebuilt at the new scale.
        view->setPixelsPerSecond(ppsB);
        timedFrame(zoom, record);
        zoomFull = zoomFull && view->stats().fullRebuild;
        drain();
        // A jump scroll past the built range (2.5 screens).
        view->setScrollX(middle + 2.5 * size.width());
        timedFrame(scroll, record);
        scrollFull = scrollFull && view->stats().fullRebuild;
        drain();
    }
    CHECK(rebuiltTracks >= 1);

    const QJsonObject report{{QStringLiteral("clips"), kTracks * kClipsPerTrack * 2},
                             {QStringLiteral("visibleClips"), visible},
                             {QStringLiteral("labels"), labels},
                             {QStringLiteral("edit"), edit.json()},
                             {QStringLiteral("zoom"), zoom.json()},
                             {QStringLiteral("zoomFullRebuild"), zoomFull},
                             {QStringLiteral("scroll"), scroll.json()},
                             {QStringLiteral("scrollFullRebuild"), scrollFull}};
    writePerf(QStringLiteral("ui-timeline-labels.json"), report);
    WARN("timeline labels, 10,000 clips (" << visible << " visible, " << labels << " labels): updatePaintNode after an edit "
                                           << edit.update.text() << " (frame " << edit.frame.text() << "); after a zoom step "
                                           << zoom.update.text() << " (frame " << zoom.frame.text()
                                           << "); after a jump scroll " << scroll.update.text() << " (frame "
                                           << scroll.frame.text() << ")");
    takeMessages();
}
