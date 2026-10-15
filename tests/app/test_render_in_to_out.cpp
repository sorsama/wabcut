// 0.1.1: Sequence > Render In to Out (Enter) in the app. MediaReadiness renders the sequence's In-Out range through
// render.previewInOut into the project's preview cache ("<project> Preview Files"), hands that cache to playback (the
// scheduler then decodes the rendered file wherever PreviewCache::validAt says it still shows the sequence:
// tests/playback/video/test_preview_playback.cpp), and an edit inside the range makes it invalid again.

#include "MediaReadinessBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QUrl>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

/// Writes a small file per segment and remembers the ranges it was asked for; can stop at its first progress call.
class FakePreviewRenderer final : public PreviewRenderer {
public:
    std::expected<std::int64_t, Error> render(const ProjectState&, SequenceId, TimeRange range, const fs::path& file,
                                              const std::function<bool(double)>& progress) override {
        if (progress && !progress(0.5)) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        std::ofstream(file, std::ios::binary) << "preview";
        std::lock_guard lock(mutex);
        ranges.push_back(range);
        return std::int64_t{7};
    }
    std::mutex mutex;
    std::vector<TimeRange> ranges;
};

} // namespace

TEST_CASE("Render In to Out renders the marked range and playback uses it until an edit", "[app][render][preview]") {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "render-in-to-out";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    // A blur makes the clip yellow (a plain clip plays in real time and needs no render, as in Premiere).
    const ClipId clip = clipAt(tl.state, tl.seq, "V1@0");
    ProjectState marked = ok(edit::addEffect(tl.state, tl.seq, std::span(&clip, 1), "wabcut.blur.gaussian")).state;
    marked = ok(edit::markIn(marked, tl.seq, f(5))).state;
    marked = ok(edit::markOut(marked, tl.seq, f(19))).state;
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(marked, project, io::SaveOptions{0}));

    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.open(QUrl::fromLocalFile(QString::fromStdU16String(project.u16string()))));
    REQUIRE(controller.currentSequenceId() == tl.seq);
    PlaybackBridge playback(&controller);
    MediaReadinessBridge readiness(&lifecycle, &playback);
    auto renderer = std::make_shared<FakePreviewRenderer>();
    readiness.setPreviewRendererForTests(renderer);
    readiness.setRenderSynchronous(true);
    const std::shared_ptr<PreviewCache> cache = readiness.previewCache();
    CHECK(cache->folder() == dir / "film Preview Files");
    const std::size_t undoBefore = controller.session().undoCount();

    // The menu / Enter: renders what lies between In and Out, then hands the cache to playback.
    INFO(readiness.renderMessage().toStdString());
    REQUIRE(readiness.renderInToOut().isEmpty());
    CHECK_FALSE(readiness.rendering());
    CHECK(readiness.renderMessage().isEmpty());
    REQUIRE_FALSE(renderer->ranges.empty());
    CHECK(renderer->ranges.front().start == f(5));
    CHECK(renderer->ranges.back().end() <= f(20));
    CHECK(renderer->ranges.back().end() >= f(19));
    CHECK(cache->entries().size() == renderer->ranges.size());
    for (const PreviewEntry& e : cache->entries()) {
        CHECK(fs::exists(cache->folder() / e.file));
    }
    CHECK(controller.session().undoCount() == undoBefore); // rendering is not an edit
    CHECK(playback.previewSettings().previews == cache);

    // Playback switches to the rendered file inside the range, and composites live outside it.
    const ProjectSnapshot rendered = controller.session().snapshot();
    CHECK(cache->validAt(*rendered, tl.seq, f(10)).has_value());
    CHECK_FALSE(cache->validAt(*rendered, tl.seq, f(25)).has_value());
    // Rendering again finds nothing left to do.
    REQUIRE(readiness.renderInToOut().isEmpty());
    CHECK(renderer->ranges.size() == cache->entries().size());

    // An edit inside the range: the file no longer shows the sequence, so playback composites live there again.
    REQUIRE(controller.session().apply("lift", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto lifted = edit::lift(s, tl.seq);
        if (!lifted) {
            return std::unexpected(lifted.error());
        }
        return std::move(lifted->state);
    }));
    const ProjectSnapshot edited = controller.session().snapshot();
    CHECK_FALSE(cache->validAt(*edited, tl.seq, f(10)).has_value());
}

TEST_CASE("Render In to Out without marks renders the whole sequence", "[app][render][preview]") {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "render-in-to-out-cancel";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    const ClipId clip = clipAt(tl.state, tl.seq, "V1@0");
    const ProjectState blurred = ok(edit::addEffect(tl.state, tl.seq, std::span(&clip, 1), "wabcut.blur.gaussian")).state;
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(blurred, project, io::SaveOptions{0}));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.open(QUrl::fromLocalFile(QString::fromStdU16String(project.u16string()))));
    PlaybackBridge playback(&controller);
    MediaReadinessBridge readiness(&lifecycle, &playback);
    auto renderer = std::make_shared<FakePreviewRenderer>();
    readiness.setPreviewRendererForTests(renderer);
    readiness.setRenderSynchronous(true);
    readiness.cancelRender(); // no job: nothing happens
    // Without marks the whole sequence renders.
    REQUIRE(readiness.renderInToOut().isEmpty());
    CHECK(readiness.renderMessage().isEmpty());
    REQUIRE_FALSE(renderer->ranges.empty());
    CHECK(renderer->ranges.front().start == Ticks{});
    CHECK(renderer->ranges.back().end() == f(10));
}
