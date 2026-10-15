// P.1: the shell's ProjectController frees what its session lets go (SnapshotReleaser, on its own thread) while the
// shell's views, models and bridges keep working. An edit storm in one process: past the undo limit (evictions),
// property gestures (merged intermediate states), undo and redo branches, project resets, the views refreshing and an
// offscreen timeline rendering between actions. Under ASan (a use of a freed snapshot) it fails loudly; anywhere, it
// checks that nothing is left queued and the controller still answers.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "DemoProject.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineView.h"

#include <wabcut/model/EditSession.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QString>

#include <chrono>
#include <thread>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

void pump() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace

TEST_CASE("Retired snapshots are freed while the shell's views keep working through an edit and reset storm",
          "[app][timeline][release]") {
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(controller != nullptr);
    REQUIRE(file != nullptr);
    RenderHarness harness(QSize(1200, 300));
    REQUIRE(harness.initialize());
    TimelineView* view = addView(harness, *controller, QSize(1200, 300));
    view->setPixelsPerSecond(48.0);

    const auto load = [&] {
        DemoProject stress = buildStressProject({2, 2, 400, 10});
        controller->resetProject(std::move(stress.state), stress.mainSequence);
        pump();
        harness.frame();
    };
    load();
    int resets = 0;
    for (int action = 0; action < 720; ++action) { // resets at 174, 349, 524, 699; edits after the last
        const SequencePtr seq = controller->currentSequence();
        REQUIRE(seq != nullptr);
        REQUIRE_FALSE(seq->videoTracks.empty());
        const auto& v1 = seq->videoTracks.front()->clips;
        REQUIRE(v1.size() > 8);
        const ClipId target = v1[1 + static_cast<std::size_t>(action * 7) % (v1.size() - 4)].id;
        controller->selectClipById(target, ProjectController::Replace);
        switch (action % 10) {
        case 0: controller->nudgeSelection(1); break;
        case 1: controller->trimSelectedEdge(-1); break;
        case 2:
            controller->setPlayhead(v1[1].timelineRange.start + Ticks{v1[1].timelineRange.duration.value / 2});
            controller->rippleTrimNextToPlayhead();
            break;
        case 3:
            for (int k = 0; k < 5; ++k) { // a slider drag: every intermediate value but the last is let go
                controller->setClipProperty(QStringLiteral("opacity"), 50.0 + k);
            }
            controller->endPropertyGesture();
            break;
        case 4: controller->undo(); break;
        case 5: controller->undo(); break;
        case 6: controller->nudgeSelection(-1); break; // drops the redo branch
        case 7: controller->redo(); break;
        case 8: controller->razorAtPlayhead(); break;
        default: controller->nudgeSelection(2); break;
        }
        pump();
        harness.frame(); // the view draws from what it holds
        if (action % 175 == 174) {
            file->newProject(); // the whole project and its history go
            pump();
            load();
            ++resets;
        }
    }
    CHECK(resets == 4);
    // Nothing waits in the session's queue for long: the releaser frees it on its own.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (controller->session().pendingReleases() != 0 && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(controller->session().pendingReleases() == 0);
    CHECK(controller->clipCount() > 0);
    CHECK(controller->canUndo());
}
