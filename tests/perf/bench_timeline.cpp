// Timeline and memory benchmarks (P.1, docs/performance.md) in the real UI stack, offscreen: the hidden Main.qml shell
// (tests/app/ShellFixture.h) with the 1-hour, 10,000-clip project (PerfProjects.h) in its ProjectController, so every
// commit fans out to everything the app's QML and bridges hang on that controller. A hidden shell never renders, so
// the timeline that is drawn and measured is a TimelineView on the same controller in an offscreen QQuickRenderControl
// window (tests/app/TimelineHarness.h), which keeps its scene graph between frames as the app's window does.
//   * timeline: drag (a V1 clip a few frames; its linked A1 clip follows), trim (its tail) and ripple trim (Ctrl on
//     its tail: the ~10,000 later clips shift on the sync-locked tracks), 30 times each with real mouse events: the
//     preview of every mouse move, the commit on release, the scene-graph update after it, and the undo.
//   * memory: working set, private bytes and GPU memory with the shell idle, after File > Open of the project, after
//     "30 minutes" of scripted editing and after File > New (what closing the project leaves behind).

#include "PerfHarness.h"
#include "PerfProjects.h"
#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineView.h"

#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointF>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kViewWidth = 1400;
constexpr int kViewHeight = 400;
constexpr double kPixelsPerSecond = 48.0; // the shell's default zoom: a 36-frame clip is 69 px wide
constexpr std::size_t kGrabbed = 12;      // V1's 13th clip (17.28 s to 18.72 s), in view at scroll 0
constexpr int kWarmUpReps = 2;
constexpr int kReps = 30;
constexpr int kMoves = 10;                // preview moves per gesture after the one that starts it
constexpr int kEditCycles = 180;          // ten actions each: 1,800 actions, "30 minutes" at one action a second
constexpr int kAutosaveEvery = 60;        // actions: the app's 60 s autosave interval
constexpr double kMaxEditingSeconds = 90.0;

double toMs(qint64 ns) { return static_cast<double>(ns) / 1e6; }

QUrl urlOf(const fs::path& path) { return QUrl::fromLocalFile(QString::fromStdU16String(path.u16string())); }

/// What the app's event loop does between two user actions: queued work, timers that are due, deferred deletes
/// (processEvents() alone leaves those to an event loop).
void pumpEvents() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

/// The hidden shell plus the measured timeline: a TimelineView on the shell's ProjectController in an offscreen window,
/// with the shell's design tokens. Motion is off there (Theme's reduce-motion durations): each frame measures the edit,
/// not the 340 ms drop spring, and no animation is left running into the next repetition. The shell's own TimelineView
/// keeps its motion; it never renders, but it takes every change like the app's.
struct ShellRig {
    Shell shell;
    RenderHarness harness{QSize(kViewWidth, kViewHeight)};
    ProjectController* controller = nullptr;
    ProjectLifecycle* file = nullptr;
    TimelineView* view = nullptr;

    ShellRig() {
        INFO("QML errors:\n" << join(shell.errors));
        REQUIRE(shell.window != nullptr);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        REQUIRE(controller != nullptr);
        REQUIRE(file != nullptr);
        REQUIRE(harness.initialize());
        view = addView(harness, *controller, QSize(kViewWidth, kViewHeight));
        QVariantMap style;
        if (auto* lanes = qobject_cast<TimelineView*>(shell.object("timelineView"))) {
            style = lanes->style();
        }
        for (const char* key : {"liftDuration", "settleDuration", "rippleDuration", "flashDuration"}) {
            style.insert(QString::fromLatin1(key), 0);
        }
        view->setStyle(style);
        spin(300); // layout and theme settle
        showStart();
    }

    /// The default zoom at the sequence start, rendered.
    void showStart() {
        view->setPixelsPerSecond(kPixelsPerSecond);
        view->setScrollX(0);
        view->setScrollY(0);
        harness.frame();
    }
};

std::optional<TimeRange> clipRange(const ProjectController& controller, ClipId clip) {
    const SequencePtr seq = controller.currentSequence();
    const auto loc = seq ? seq->findClip(clip) : std::nullopt;
    if (!loc) {
        return std::nullopt;
    }
    return loc->clip().timelineRange;
}

/// Where to press on `clip` (window coordinates, on its own row): the middle of its body, or 2 px inside its tail (the
/// trim zone).
std::optional<QPointF> clipPoint(const ShellRig& rig, ClipId clip, bool tail) {
    const SequencePtr seq = rig.controller->currentSequence();
    const auto loc = seq ? seq->findClip(clip) : std::nullopt;
    if (!loc) {
        return std::nullopt;
    }
    const Clip& c = loc->clip();
    const double pps = rig.view->pixelsPerSecond();
    const double x = tail ? c.end().toSeconds() * pps - 2.0 : (c.start().toSeconds() + c.end().toSeconds()) / 2.0 * pps;
    for (const TimelineView::Row& row : rig.view->rows()) {
        if (row.track->id == loc->track->id) {
            return QPointF(x - rig.view->scrollX(), row.y + row.height / 2 - rig.view->scrollY());
        }
    }
    return std::nullopt;
}

void sendMouse(QQuickWindow& window, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods) {
    QMouseEvent event(type, pos, pos, button, buttons, mods);
    QCoreApplication::sendEvent(&window, &event);
}

struct GestureTiming {
    bool started = false;   ///< the drag passed the threshold and became a gesture
    bool committed = false; ///< the release changed the snapshot (one undo step)
    double startMs = 0.0;   ///< the move past the drag threshold: snap targets, ghosts and the first preview
    std::vector<double> previewMs;      ///< each later mouse move (previewEdit on the snapshot + showing it)
    std::vector<double> previewEditMs;  ///< of which the edit (TimelineStats::previewNs)
    std::vector<double> previewPaintMs; ///< updatePaintNode of the frame after each move
    double commitMs = 0.0;  ///< the release: commitGesture -> EditSession -> syncAfterChange -> changed() fan-out
    double paintMs = -1.0;  ///< updatePaintNode of the frame after the release (-1: not called)
    double frameMs = 0.0;   ///< that whole frame: polish, sync, render, end of frame
};

/// Press at `from`, drag horizontally (`direction` +1 right, -1 left) and release, rendering a frame after every event
/// as the app does. The first move passes the drag threshold; the next `moves` stay 6..14 px from the press (3..7
/// frames at 48 px/s), far from every snap target (the clip edges around are 69 px apart).
GestureTiming performGesture(ShellRig& rig, QPointF from, double direction, Qt::KeyboardModifiers mods, int moves) {
    GestureTiming g;
    QQuickWindow& window = rig.harness.window();
    const TimelineView& view = *rig.view;
    const int revision = rig.controller->revision();
    sendMouse(window, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
    rig.harness.frame();

    const double threshold = QGuiApplication::styleHints()->startDragDistance() + 2.0;
    QPointF at = from + QPointF(direction * threshold, 0.0);
    auto t0 = Clock::now();
    sendMouse(window, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton, mods);
    g.startMs = perf::msSince(t0);
    g.started = view.gestureActive();
    rig.harness.frame();
    for (int i = 0; i < moves && g.started; ++i) {
        at = from + QPointF(direction * (6.0 + 2.0 * (i % 5)), 0.0);
        t0 = Clock::now();
        sendMouse(window, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton, mods);
        g.previewMs.push_back(perf::msSince(t0));
        g.previewEditMs.push_back(toMs(view.stats().previewNs));
        const int framesBefore = view.stats().frames;
        rig.harness.frame();
        if (view.stats().frames > framesBefore) {
            g.previewPaintMs.push_back(toMs(view.stats().updateNs));
        }
    }

    t0 = Clock::now();
    sendMouse(window, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, mods);
    g.commitMs = perf::msSince(t0);
    g.committed = rig.controller->revision() != revision;
    const int paintedBefore = view.stats().frames;
    t0 = Clock::now();
    rig.harness.frame();
    g.frameMs = perf::msSince(t0);
    if (view.stats().frames > paintedBefore) {
        g.paintMs = toMs(view.stats().updateNs);
    }
    return g;
}

// ---- Timeline -----------------------------------------------------------------------------------------------------

enum class BenchGesture { Drag, Trim, Ripple };

struct BenchOp {
    const char* name;
    BenchGesture kind;
    bool tail;        ///< pressed on the tail edge (else the body)
    double direction; ///< +1 drags right, -1 left
    Qt::KeyboardModifiers mods;
};

struct OpSamples {
    perf::Samples start, preview, previewEdit, previewPaint;
    perf::Samples commit, commitPaint, commitFrame, releaseToFrame, deferred;
    perf::Samples undo, undoPaint;
};

void addOp(perf::PerfResult& r, const std::string& op, const OpSamples& s) {
    const auto gated = [&](const char* metric, const perf::Samples& samples, const char* stat) {
        CHECK_FALSE(samples.empty());
        if (!samples.empty()) {
            r.addSamples(op + "_" + metric, "ms", samples, stat);
        }
    };
    const auto info = [&](const char* metric, const perf::Samples& samples, bool p95) {
        if (!samples.empty()) {
            r.note(op + "_" + metric, "ms", p95 ? samples.p95() : samples.p50());
        }
    };
    gated("start_ms", s.start, "p50");
    gated("preview_ms", s.preview, "p50");
    gated("preview_p95_ms", s.preview, "p95");
    gated("preview_paint_p95_ms", s.previewPaint, "p95");
    gated("commit_ms", s.commit, "p50");
    gated("commit_p95_ms", s.commit, "p95");
    gated("commit_paint_ms", s.commitPaint, "p50");
    gated("commit_paint_p95_ms", s.commitPaint, "p95");
    gated("undo_ms", s.undo, "p50");
    gated("undo_p95_ms", s.undo, "p95");
    info("preview_edit_ms", s.previewEdit, false);
    info("commit_frame_ms", s.commitFrame, false);
    info("commit_deferred_ms", s.deferred, false);
    info("release_to_frame_p95_ms", s.releaseToFrame, true);
    info("undo_paint_ms", s.undoPaint, false);
}

// ---- Memory -------------------------------------------------------------------------------------------------------

struct MemoryPoint {
    double workingSet = 0.0;
    double privateBytes = 0.0;
    double gpu = -1.0;
    std::size_t pendingReleases = 0; ///< EditSession snapshots queued for releasePending()
    std::size_t undoSteps = 0;
    double undoMb = 0.0;             ///< the undo history's own estimate of what it keeps alive
    int clips = 0;
};

/// Lets the shell settle (`settleMs` of event loop, deferred deletes), renders a frame, then reads the process.
MemoryPoint measurePoint(ShellRig& rig, int settleMs) {
    spin(settleMs);
    pumpEvents();
    rig.harness.frame();
    MemoryPoint m;
    m.workingSet = perf::workingSetMb();
    m.privateBytes = perf::privateMb();
    m.gpu = perf::gpuMemoryMb();
    const EditSession& session = rig.controller->session();
    m.pendingReleases = session.pendingReleases();
    m.undoSteps = session.undoCount();
    m.undoMb = static_cast<double>(session.undoMemoryBytes()) / (1024.0 * 1024.0);
    m.clips = rig.controller->clipCount();
    return m;
}

perf::Json pointJson(const MemoryPoint& m) {
    return perf::Json{{"workingSetMb", m.workingSet}, {"privateMb", m.privateBytes},      {"gpuMb", m.gpu},
                      {"pendingReleases", m.pendingReleases}, {"undoSteps", m.undoSteps}, {"undoMb", m.undoMb},
                      {"clips", m.clips}};
}

struct EditingRun {
    int actions = 0;
    int changed = 0;   ///< actions that changed the snapshot
    int drags = 0;     ///< mouse drags that committed
    int autosaves = 0; ///< autosaves that wrote a file
    bool stoppedEarly = false;
};

/// Scripted editing: `cycles` cycles of ten actions on the clip each cycle picks (spread over the sequence, scrolled
/// into view), through the controller's commands and the timeline's mouse gestures; autosaves every `autosaveEvery`
/// actions (0: never). Stops after `maxSeconds`.
EditingRun scriptedEditing(ShellRig& rig, int cycles, int autosaveEvery, double maxSeconds) {
    ProjectController& pc = *rig.controller;
    EditingRun run;
    const auto started = Clock::now();
    const auto act = [&](const std::function<void()>& action) {
        const int revision = pc.revision();
        action();
        ++run.actions;
        if (pc.revision() != revision) {
            ++run.changed;
        }
        if (autosaveEvery > 0 && run.actions % autosaveEvery == 0 && rig.file->autosaveNow()) {
            ++run.autosaves;
        }
    };
    const auto playheadInto = [&](ClipId clip, double fraction) {
        if (const auto range = clipRange(pc, clip)) {
            const auto offset = static_cast<std::int64_t>(static_cast<double>(range->duration.value) * fraction);
            pc.setPlayhead(Ticks{range->start.value + offset});
        }
    };
    for (int cycle = 0; cycle < cycles; ++cycle) {
        if (perf::msSince(started) > maxSeconds * 1000.0) {
            run.stoppedEarly = true;
            break;
        }
        ClipId target;
        ClipId later;
        {
            const SequencePtr seq = pc.currentSequence();
            if (!seq || seq->videoTracks.empty() || seq->videoTracks.front()->clips.size() < 8) {
                break;
            }
            const auto& v1 = seq->videoTracks.front()->clips;
            const std::size_t k = 1 + (static_cast<std::size_t>(cycle) * 97) % (v1.size() - 4);
            target = v1[k].id;
            later = v1[k + 2].id;
        }
        pc.selectClipById(target, ProjectController::Replace);
        rig.view->revealClip(clipKey(target));
        rig.harness.frame();
        act([&] { pc.nudgeSelection(1); });                                    // move (Alt+Right)
        act([&] { pc.trimSelectedEdge(-1); });                                 // trim the tail
        act([&] { playheadInto(target, 0.5); pc.rippleTrimNextToPlayhead(); }); // W: ripple
        act([&] { playheadInto(target, 1.0 / 3.0); pc.razorAtPlayhead(); });  // Ctrl+K: split
        act([&] { pc.undo(); });
        act([&] { pc.redo(); });
        act([&] {
            pc.setClipProperty(QStringLiteral("opacity"), 40.0 + cycle % 50); // a parameter edit
            pc.endPropertyGesture();
        });
        act([&] { // a mouse drag of the clip two later (overwrites 3..7 frames of its neighbour)
            rig.view->revealClip(clipKey(later));
            rig.harness.frame();
            const auto from = clipPoint(rig, later, false);
            if (from && performGesture(rig, *from, 1.0, {}, 3).committed) {
                ++run.drags;
            } else {
                pc.nudgeSelection(-1);
            }
        });
        act([&] { pc.undo(); });
        act([&] { pc.undo(); });
        pumpEvents();
        rig.harness.frame();
    }
    return run;
}

} // namespace

TEST_CASE("Benchmark timeline interaction on 10000 clips", "[perf][gpu][app]") {
    const perf::BenchmarkOutcome outcome = perf::runBenchmark("timeline", [] {
        perf::PerfResult r;
        resetSettings();
        clearMessages();
        ShellRig rig;

        perf::OneHourProject project = perf::oneHourProject();
        REQUIRE(project.v1.size() > kGrabbed + 1);
        REQUIRE(project.a1.size() > kGrabbed);
        const std::size_t clips = project.clips;
        const ClipId grabbed = project.v1[kGrabbed];
        const ClipId partner = project.a1[kGrabbed];
        const ClipId next = project.v1[kGrabbed + 1];
        const auto loadStart = Clock::now();
        rig.controller->resetProject(std::move(project.state), project.sequence);
        r.note("load_ms", "ms", perf::msSince(loadStart));
        REQUIRE(rig.controller->clipCount() == static_cast<int>(clips));
        rig.showStart();
        const TimelineStats first = rig.view->stats();
        r.note("load_paint_ms", "ms", toMs(first.updateNs));
        pumpEvents();

        // What a ripple at the grabbed clip's tail shifts: everything after it, on every (sync-locked) track.
        std::size_t shifted = 0;
        {
            const auto range = clipRange(*rig.controller, grabbed);
            REQUIRE(range);
            const SequencePtr seq = rig.controller->currentSequence();
            seq->forEachTrack([&](const Track& track) {
                for (const Clip& c : track.clips) {
                    if (c.start() >= range->end()) {
                        ++shifted;
                    }
                }
            });
        }

        const BenchOp ops[] = {
            {"drag", BenchGesture::Drag, false, 1.0, Qt::NoModifier},
            {"trim", BenchGesture::Trim, true, -1.0, Qt::NoModifier},
            {"ripple", BenchGesture::Ripple, true, -1.0, Qt::ControlModifier},
        };
        perf::Json missed = perf::Json::object();
        for (const BenchOp& op : ops) {
            OpSamples s;
            int misses = 0;
            for (int rep = 0; rep < kWarmUpReps + kReps; ++rep) {
                const auto from = clipPoint(rig, grabbed, op.tail);
                REQUIRE(from);
                const auto grabbedBefore = clipRange(*rig.controller, grabbed);
                const auto partnerBefore = clipRange(*rig.controller, partner);
                const auto nextBefore = clipRange(*rig.controller, next);
                const GestureTiming g = performGesture(rig, *from, op.direction, op.mods, kMoves);
                auto t0 = Clock::now();
                pumpEvents();
                const double deferredMs = perf::msSince(t0);
                if (!g.started || !g.committed) {
                    ++misses;
                    continue;
                }
                if (rep == 0) {
                    // Each gesture is what it says: the move takes the linked partner along, the trim leaves the next
                    // clip where it was, the ripple trim pulls it in by what the clip lost.
                    const auto grabbedAfter = clipRange(*rig.controller, grabbed);
                    const auto partnerAfter = clipRange(*rig.controller, partner);
                    const auto nextAfter = clipRange(*rig.controller, next);
                    REQUIRE((grabbedBefore && partnerBefore && nextBefore && grabbedAfter && partnerAfter && nextAfter));
                    switch (op.kind) {
                    case BenchGesture::Drag:
                        CHECK(grabbedAfter->start != grabbedBefore->start);
                        CHECK(grabbedAfter->start - grabbedBefore->start == partnerAfter->start - partnerBefore->start);
                        break;
                    case BenchGesture::Trim:
                        CHECK(grabbedAfter->duration < grabbedBefore->duration);
                        CHECK(nextAfter->start == nextBefore->start);
                        break;
                    case BenchGesture::Ripple:
                        CHECK(grabbedAfter->duration < grabbedBefore->duration);
                        CHECK(nextAfter->start - nextBefore->start == grabbedAfter->duration - grabbedBefore->duration);
                        break;
                    }
                }
                t0 = Clock::now();
                const bool undone = rig.controller->undo();
                const double undoMs = perf::msSince(t0);
                REQUIRE(undone);
                const int paintedBefore = rig.view->stats().frames;
                rig.harness.frame();
                const bool undoPainted = rig.view->stats().frames > paintedBefore;
                const double undoPaintMs = toMs(rig.view->stats().updateNs);
                pumpEvents();
                if (rep < kWarmUpReps) {
                    continue; // first gestures: glyph cache, first allocations
                }
                s.start.add(g.startMs);
                for (const double v : g.previewMs) {
                    s.preview.add(v);
                }
                for (const double v : g.previewEditMs) {
                    s.previewEdit.add(v);
                }
                for (const double v : g.previewPaintMs) {
                    s.previewPaint.add(v);
                }
                s.commit.add(g.commitMs);
                if (g.paintMs >= 0.0) {
                    s.commitPaint.add(g.paintMs);
                }
                s.commitFrame.add(g.frameMs);
                s.releaseToFrame.add(g.commitMs + g.frameMs);
                s.deferred.add(deferredMs);
                s.undo.add(undoMs);
                if (undoPainted) {
                    s.undoPaint.add(undoPaintMs);
                }
            }
            INFO(op.name << ": " << misses << " gestures did not start or commit");
            CHECK(misses == 0);
            missed[op.name] = misses;
            addOp(r, op.name, s);
        }

        const QStringList messages = takeMessages();
        perf::Json warnings = perf::Json::array();
        for (const QString& m : messages.mid(0, 20)) {
            warnings.push_back(m.toStdString());
        }
        r.details["project"] = perf::Json{
            {"clips", clips},
            {"shape", "V1/A1 and V2/A2: 2,500 linked pairs of 36 frames each at 25 fps (1 hour), 200 offline media, "
                      "100 markers; loaded with ProjectController::resetProject into the shell's controller"},
            {"clipsAfterRippledEdge", shifted}};
        r.details["view"] = perf::Json{
            {"width", kViewWidth},
            {"height", kViewHeight},
            {"pixelsPerSecond", kPixelsPerSecond},
            {"motion", "off (Theme reduce-motion durations) in the measured view; the shell's hidden view keeps it"},
            {"firstBuild",
             {{"nodes", first.nodes}, {"builtClips", first.builtClips}, {"visibleClips", first.visibleClips},
              {"labels", first.labels}}}};
        r.details["gestures"] = perf::Json{
            {"clip", "V1's 13th clip (17.28 s to 18.72 s, 69 px wide) and its linked A1 clip"},
            {"drag", "body, right by 3..7 frames (overwrite)"},
            {"trim", "tail, left by 3..7 frames"},
            {"ripple", "Ctrl on the tail, left by 3..7 frames"},
            {"repetitions", kReps},
            {"warmUpRepetitions", kWarmUpReps},
            {"previewMovesPerGesture", kMoves},
            {"dragThresholdPx", QGuiApplication::styleHints()->startDragDistance()},
            {"missed", missed}};
        r.details["method"] =
            "Real mouse events into the offscreen window, one frame (polish, sync, render) after each. <op>_start_ms: "
            "the move past the drag threshold (snap targets of every clip, ghosts, the first preview). "
            "<op>_preview_ms: each later move (previewEdit on the snapshot and showing it; <op>_preview_edit_ms is "
            "the edit alone, TimelineStats::previewNs). <op>_preview_paint_p95_ms: updatePaintNode of the frames "
            "during the drag. <op>_commit_ms: the release (commitGesture, EditSession apply, syncAfterChange and the "
            "changed() fan-out into the shell's QML and bridges, all synchronous). <op>_commit_paint_ms: "
            "updatePaintNode (TimelineStats::updateNs) of the next frame; <op>_commit_frame_ms that whole frame; "
            "<op>_commit_deferred_ms the queued work the event loop then runs. <op>_undo_ms: ProjectController::undo "
            "with its fan-out; every repetition is undone, so each starts from the same project.";
        r.details["qtWarnings"] = perf::Json{{"count", messages.size()}, {"first", warnings}};
        return r;
    });
    INFO(outcome.describe());
    CHECK(outcome.ok());
}

TEST_CASE("Benchmark memory footprint of a one-hour project", "[perf][gpu][app]") {
    const perf::BenchmarkOutcome outcome = perf::runBenchmark("memory", [] {
        perf::PerfResult r;
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const fs::path root(dir.path().toStdU16String());
        const fs::path hourFile = root / "One hour.wabcut";
        const fs::path warmUpFile = root / "Warm-up.wabcut";
        std::size_t hourClips = 0;
        {
            const perf::OneHourProject hour = perf::oneHourProject();
            hourClips = hour.clips;
            const auto saved = io::saveProject(hour.state, hourFile, io::SaveOptions{0});
            INFO((saved ? std::string() : saved.error().message));
            REQUIRE(saved);
            const perf::OneHourProject warmUpProject = perf::oneHourProject(50); // 200 clips
            REQUIRE(io::saveProject(warmUpProject.state, warmUpFile, io::SaveOptions{0}));
        }
#if defined(_WIN32)
        _heapmin(); // the generated projects' freed heap goes back before the shell's baseline
#endif
        std::error_code ec;
        r.note("project_file_mb", "MB", static_cast<double>(fs::file_size(hourFile, ec)) / (1024.0 * 1024.0));

        resetSettings();
        clearMessages();
        ShellRig rig;
        rig.file->setAutosaveIntervalSeconds(3600); // the script autosaves itself, every 60 actions

        // Warm-up: a 200-clip project opened, edited and closed the same way, so what the shell creates on first use
        // (dialogs, delegates, glyph caches, thread pools) is in the baseline instead of in "retained".
        {
            const bool warmUpOpened = rig.file->open(urlOf(warmUpFile));
            INFO(rig.file->lastError().toStdString());
            REQUIRE(warmUpOpened);
        }
        rig.showStart();
        scriptedEditing(rig, 3, 0, 30.0);
        rig.file->newProject();
        rig.showStart();
        const MemoryPoint shell = measurePoint(rig, 1000);

        auto t0 = Clock::now();
        const bool opened = rig.file->open(urlOf(hourFile));
        r.note("open_ms", "ms", perf::msSince(t0));
        {
            INFO(rig.file->lastError().toStdString());
            REQUIRE(opened);
        }
        REQUIRE(rig.controller->clipCount() == static_cast<int>(hourClips));
        rig.showStart();
        const MemoryPoint openedPoint = measurePoint(rig, 1000);

        t0 = Clock::now();
        const EditingRun run = scriptedEditing(rig, kEditCycles, kAutosaveEvery, kMaxEditingSeconds);
        const double editingSeconds = perf::msSince(t0) / 1000.0;
        if (run.stoppedEarly) {
            WARN("scripted editing stopped after " << kMaxEditingSeconds << " s at " << run.actions << " actions");
        }
        CHECK(run.changed > run.actions / 2);
        const MemoryPoint edited = measurePoint(rig, 1000);

        rig.file->newProject(); // File > New: the one-hour project is closed
        rig.showStart();
        const MemoryPoint closed = measurePoint(rig, 1000);
        CHECK(closed.clips == 0);

        const auto point = [&r](const std::string& name, const MemoryPoint& m, bool gated) {
            if (gated) {
                r.add(name + "_private_mb", "MB", m.privateBytes);
            } else {
                r.note(name + "_private_mb", "MB", m.privateBytes);
            }
            r.note(name + "_ws_mb", "MB", m.workingSet);
            if (m.gpu >= 0.0) {
                if (gated && name == "opened") {
                    r.add(name + "_gpu_mb", "MB", m.gpu);
                } else {
                    r.note(name + "_gpu_mb", "MB", m.gpu);
                }
            }
            r.note(name + "_pending_releases_count", "snapshots", static_cast<double>(m.pendingReleases));
        };
        point("shell", shell, true);
        point("opened", openedPoint, true);
        point("edited", edited, true);
        point("closed", closed, false);
        r.add("open_cost_mb", "MB", openedPoint.privateBytes - shell.privateBytes);
        r.add("editing_growth_mb", "MB", edited.privateBytes - openedPoint.privateBytes);
        r.note("editing_growth_ws_mb", "MB", edited.workingSet - openedPoint.workingSet);
        r.add("retained_after_close_mb", "MB", closed.privateBytes - shell.privateBytes);
        r.note("peak_ws_mb", "MB", perf::peakWorkingSetMb());
        r.note("editing_s", "s", editingSeconds);
        r.note("edit_actions_count", "actions", run.actions);

        // Diagnostic only, after every gated value: how much of what stayed is EditSession's release queue.
        const std::size_t released = rig.controller->session().releasePending();
        const MemoryPoint drained = measurePoint(rig, 500);
        r.note("drained_private_mb", "MB", drained.privateBytes);
        r.note("retained_after_drain_mb", "MB", drained.privateBytes - shell.privateBytes);

        const QStringList messages = takeMessages();
        perf::Json warnings = perf::Json::array();
        for (const QString& m : messages.mid(0, 20)) {
            warnings.push_back(m.toStdString());
        }
        r.details["points"] = perf::Json{{"shell", pointJson(shell)},   {"opened", pointJson(openedPoint)},
                                         {"edited", pointJson(edited)}, {"closed", pointJson(closed)},
                                         {"drained", pointJson(drained)}};
        r.details["editing"] = perf::Json{
            {"scaling",
             "30 minutes of brisk editing at one action a second = 1,800 actions: 180 cycles of ten on a clip each "
             "cycle picks (every 97th V1 clip, scrolled into view): nudge +1 frame, trim its tail -1 frame, W "
             "(ripple-trim to the playhead in its middle: the ~10,000 later clips shift on the sync-locked tracks), "
             "Ctrl+K at a third of it (split), undo, redo, opacity (a parameter edit), a mouse drag of the clip two "
             "later (3..7 frames, overwrite), undo, undo. Net four edits a cycle stay. After each cycle the event loop "
             "runs (queued work, due timers, deferred deletes) and the timeline renders; ProjectFile.autosaveNow() "
             "every 60 actions stands in for the 60 s timer (30 autosaves)."},
            {"cycles", kEditCycles},
            {"actions", run.actions},
            {"snapshotChanges", run.changed},
            {"mouseDrags", run.drags},
            {"autosaves", run.autosaves},
            {"seconds", editingSeconds},
            {"stoppedEarly", run.stoppedEarly}};
        r.details["releaseQueue"] = perf::Json{
            {"note",
             "EditSession (deferRelease) queues the snapshots it lets go (undo steps past the 500-step limit, dropped "
             "redo branches, gesture intermediates, the previous project on open / new) for releasePending(), which "
             "nothing in the app calls: they live until the ProjectController dies. Measured as found; "
             "retained_after_drain_mb drains the queue after every gated value, as a diagnostic."},
            {"pendingAfterOpen", openedPoint.pendingReleases},
            {"pendingAfterEditing", edited.pendingReleases},
            {"pendingAfterClose", closed.pendingReleases},
            {"releasedByDiagnosticDrain", released}};
        r.details["method"] =
            "Private bytes (commit charge), working set and this process's GPU memory (DXGI local + non-local) after "
            "1 s of event loop, deferred deletes and one rendered frame. shell: the hidden shell after a 200-clip "
            "warm-up project was opened, edited and closed, on an empty untitled project. opened: after "
            "ProjectFile.open() of the saved one-hour project (200 offline media). edited: after the scripted editing. "
            "closed: after ProjectFile.newProject(). GPU: the hidden shell never renders; the number is the offscreen "
            "timeline (its RHI device and scene graph) plus whatever the playback and monitor bridges allocate. "
            "Freed memory the CRT heap keeps is counted, as the app's would be.";
        r.details["qtWarnings"] = perf::Json{{"count", messages.size()}, {"first", warnings}};
        return r;
    });
    INFO(outcome.describe());
    CHECK(outcome.ok());
}
