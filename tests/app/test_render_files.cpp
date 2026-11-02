// P1 round, stream 2A (#7, #58): Sequence > Render Selection, Delete Render Files and Delete Render Files In to Out
// (MediaReadiness through render.previewInOut / render.deletePreviewFiles), their greyed menu rows with the reason, the
// confirm with the size, the project's render files as a row of Settings > Media & cache, and the cache limits kept
// automatically (after start and after a proxy batch; never while a render, playback or an export runs).
// Screenshots: p1-render-menu.png / -light.png, p1-delete-render-files.png / -light.png,
// p1-render-files-cache.png / -light.png.

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "CommandPalette.h"
#include "MediaReadinessBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineFixture.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/segment/Matte.h>
#include <wabcut/editor/CacheInspector.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

/// Writes a 7-byte file per segment, remembers the ranges; runs `during` first (to look at the bridge mid-render).
class RecordingRenderer final : public PreviewRenderer {
public:
    std::expected<std::int64_t, Error> render(const ProjectState&, SequenceId, TimeRange range, const fs::path& file,
                                              const std::function<bool(double)>& progress) override {
        if (during)
            during();
        if (progress && !progress(0.5))
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        std::ofstream(file, std::ios::binary) << "preview";
        std::lock_guard lock(mutex);
        ranges.push_back(range);
        return std::int64_t{7};
    }
    std::function<void()> during;
    std::mutex mutex;
    std::vector<TimeRange> ranges;
};

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

/// Three yellow clips (a blur on each) with gaps: A [0, 4), B [6, 10), C [12, 16).
ProjectState blurredClips(Timeline& tl) {
    std::vector<ClipId> clips{clipAt(tl.state, tl.seq, "V1@0"), clipAt(tl.state, tl.seq, "V1@6"),
                              clipAt(tl.state, tl.seq, "V1@12")};
    return ok(edit::addEffect(tl.state, tl.seq, std::span(clips), "wabcut.blur.gaussian")).state;
}

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

QQuickItem* popupItem(QObject* popup) {
    auto* content = popup ? popup->property("contentItem").value<QQuickItem*>() : nullptr;
    return content ? content->parentItem() : nullptr;
}

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                         .adjusted(-16, -16, 16, 16)
                         .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

QVariantMap cacheRow(MediaReadinessBridge& readiness, const QString& id) {
    for (const QVariant& row : readiness.caches()) {
        if (row.toMap().value(QStringLiteral("id")).toString() == id)
            return row.toMap();
    }
    return {};
}

} // namespace

TEST_CASE("2AR Render Selection renders only the selected video clips", "[app][render][preview][2a]") {
    const fs::path dir = freshDir("render-selection");
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(blurredClips(tl), project, io::SaveOptions{0}));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.open(QUrl::fromLocalFile(QString::fromStdU16String(project.u16string()))));
    PlaybackBridge playback(&controller);
    MediaReadinessBridge readiness(&lifecycle, &playback);
    auto renderer = std::make_shared<RecordingRenderer>();
    readiness.setPreviewRendererForTests(renderer);
    readiness.setRenderSynchronous(true);
    const std::size_t undoBefore = controller.session().undoCount();

    // Nothing selected: greyed with the reason, and the call says the same.
    controller.clearSelection();
    QVariantMap state = readiness.commandAvailability(QStringLiteral("sequence.renderSelection"));
    CHECK(state.value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
    CHECK(state.value(QStringLiteral("reason")).toString() == QStringLiteral("Select a video clip first"));
    CHECK(readiness.renderSelection() == QStringLiteral("Select a video clip first"));
    CHECK(renderer->ranges.empty());

    // A and C selected: their ranges render (the gap and B between them do not), in one job.
    const ClipId a = clipAt(*controller.session().snapshot(), tl.seq, "V1@0");
    const ClipId c = clipAt(*controller.session().snapshot(), tl.seq, "V1@12");
    controller.setSelection({a, c}, a);
    CHECK(readiness.commandAvailability(QStringLiteral("sequence.renderSelection")).value(QStringLiteral("enabled")).toBool());
    int finished = 0;
    QString outcome;
    QObject::connect(&readiness, &MediaReadinessBridge::renderFinished, [&](const QString& o, const QString&) {
        ++finished;
        outcome = o;
    });
    // While it renders, the menu rows say why they wait.
    QVariantMap during;
    renderer->during = [&] {
        if (during.isEmpty())
            during = readiness.commandAvailability(QStringLiteral("sequence.renderSelection"));
    };
    INFO(readiness.renderMessage().toStdString());
    REQUIRE(readiness.renderSelection().isEmpty());
    CHECK(finished == 1);
    CHECK(outcome == QStringLiteral("done"));
    CHECK(readiness.renderScope() == QStringLiteral("selection"));
    CHECK_FALSE(during.value(QStringLiteral("enabled")).toBool());
    CHECK(during.value(QStringLiteral("reason")).toString() == QStringLiteral("A render is running"));
    REQUIRE(renderer->ranges.size() == 2);
    for (const TimeRange& r : renderer->ranges) {
        INFO(r.start.value << " - " << r.end().value);
        const bool inA = r.start >= f(0) && r.end() <= f(4);
        const bool inC = r.start >= f(12) && r.end() <= f(16);
        CHECK((inA || inC));
    }
    CHECK(controller.session().undoCount() == undoBefore); // rendering is not an edit
    // Render In to Out afterwards says it again ("inToOut").
    renderer->during = nullptr;
    REQUIRE(readiness.renderInToOut().isEmpty());
    CHECK(readiness.renderScope() == QStringLiteral("inToOut"));
    CHECK(renderer->ranges.size() == 3); // only B was left
}

TEST_CASE("2AR Delete Render Files and Delete Render Files In to Out delete what they say", "[app][render][preview][2a]") {
    const fs::path dir = freshDir("delete-render-files");
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(blurredClips(tl), project, io::SaveOptions{0}));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.open(QUrl::fromLocalFile(QString::fromStdU16String(project.u16string()))));
    PlaybackBridge playback(&controller);
    MediaReadinessBridge readiness(&lifecycle, &playback);
    auto renderer = std::make_shared<RecordingRenderer>();
    readiness.setPreviewRendererForTests(renderer);
    readiness.setRenderSynchronous(true);
    const std::shared_ptr<PreviewCache> cache = readiness.previewCache();

    // No render files yet: both rows greyed with their reason; In to Out also needs a mark.
    CHECK(readiness.commandAvailability(QStringLiteral("sequence.deleteRenderFiles")).value(QStringLiteral("reason")).toString() ==
          QStringLiteral("This sequence has no render files"));
    CHECK(readiness.commandAvailability(QStringLiteral("sequence.deleteRenderFilesInToOut")).value(QStringLiteral("reason")).toString() ==
          QStringLiteral("Set an In or an Out in the sequence first"));
    CHECK(readiness.deleteRenderFiles(false) == QStringLiteral("This sequence has no render files"));

    // Render everything, then the dry-run numbers.
    REQUIRE(readiness.renderInToOut().isEmpty());
    REQUIRE(cache->entries().size() == 3);
    QVariantMap all = readiness.previewFilesInfo(false);
    CHECK(all.value(QStringLiteral("enabled")).toBool());
    CHECK(all.value(QStringLiteral("files")).toInt() == 3);
    CHECK(all.value(QStringLiteral("bytes")).toDouble() == 21.0);
    CHECK_FALSE(all.value(QStringLiteral("sizeText")).toString().isEmpty());
    CHECK_FALSE(all.value(QStringLiteral("sequenceName")).toString().isEmpty());

    // In to Out around C: only C's file.
    REQUIRE(controller.session().apply("marks", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        ProjectState marked = ok(edit::markIn(s, tl.seq, f(11))).state;
        return ok(edit::markOut(marked, tl.seq, f(17))).state;
    }));
    const QVariantMap inOut = readiness.previewFilesInfo(true);
    CHECK(inOut.value(QStringLiteral("enabled")).toBool());
    CHECK(inOut.value(QStringLiteral("files")).toInt() == 1);
    CHECK(readiness.commandAvailability(QStringLiteral("sequence.deleteRenderFilesInToOut")).value(QStringLiteral("enabled")).toBool());

    std::vector<std::pair<bool, QString>> deleted;
    QObject::connect(&readiness, &MediaReadinessBridge::renderFilesDeleted,
                     [&](bool done, const QString& message) { deleted.emplace_back(done, message); });
    int filesChanged = 0;
    QObject::connect(&readiness, &MediaReadinessBridge::previewFilesChanged, [&] { ++filesChanged; });
    const std::size_t undoBefore = controller.session().undoCount();
    const std::vector<PreviewEntry> before = cache->entries();
    REQUIRE(readiness.deleteRenderFiles(true).isEmpty());
    REQUIRE(deleted.size() == 1);
    CHECK(deleted.back().first);
    CHECK(deleted.back().second.startsWith(QStringLiteral("Deleted ")));
    CHECK(deleted.back().second.contains(QStringLiteral("1 file")));
    CHECK(filesChanged >= 1);
    REQUIRE(cache->entries().size() == 2);
    for (const PreviewEntry& e : cache->entries())
        CHECK_FALSE(e.range.overlaps(TimeRange::fromStartEnd(f(11), f(17))));
    for (const PreviewEntry& e : before) {
        const bool kept = !e.range.overlaps(TimeRange::fromStartEnd(f(11), f(17)));
        CHECK(fs::exists(cache->folder() / e.file) == kept);
    }
    CHECK(controller.session().undoCount() == undoBefore); // deleting render files is not an edit
    CHECK(playback.previewSettings().previews == cache);
    // Nothing left between In and Out now.
    CHECK(readiness.previewFilesInfo(true).value(QStringLiteral("reason")).toString() ==
          QStringLiteral("There are no render files between In and Out"));

    // While a render runs the delete waits.
    QString whileRendering;
    renderer->during = [&] {
        if (whileRendering.isEmpty())
            whileRendering = readiness.deleteRenderFiles(false);
    };
    REQUIRE(readiness.renderInToOut().isEmpty()); // renders C's segment again
    CHECK(whileRendering == QStringLiteral("Wait for the render to finish"));
    renderer->during = nullptr;

    // The whole sequence.
    REQUIRE(readiness.deleteRenderFiles(false).isEmpty());
    CHECK(cache->entries().empty());
    CHECK(deleted.back().first);
    CHECK_FALSE(readiness.commandAvailability(QStringLiteral("sequence.deleteRenderFiles")).value(QStringLiteral("enabled")).toBool());
    // The index on disk says the same (the next open finds no files).
    PreviewCache reopened(cache->folder());
    REQUIRE(reopened.load());
    CHECK(reopened.entries().empty());
}

TEST_CASE("2AR Cache limits are kept automatically and wait while work runs", "[app][cache][2a]") {
    // Fix round 1 (F1A, ARGON-04): only the keeper deletes, only with its switch on, and only from caches no project
    // refers to (thumbnails, waveforms). Proxies past a limit stay: project files name them and other projects may use
    // them; Clean deletes them.
    const fs::path dir = freshDir("auto-enforce");
    resetSettings();
    QSettings().setValue(QStringLiteral("proxies/policy"), QStringLiteral("off"));
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    // Old proxies of other media next to the project, over a 3.5 kB proxy limit set before (the original counts too):
    // no limit applies to proxies any more (F1A). The project's own original sits in the same folder: never deleted.
    const fs::path proxies = dir / "film Proxies";
    fs::create_directories(proxies);
    const auto now = fs::file_time_type::clock::now();
    const fs::path files[] = {proxies / "old.mov", proxies / "middle.mov", proxies / "new.mov"};
    for (int i = 0; i < 3; ++i) {
        std::ofstream(files[i], std::ios::binary) << std::string(1000, 'x');
        fs::last_write_time(files[i], now - std::chrono::hours(30 - 10 * i));
    }
    const fs::path original = proxies / "original.mov";
    std::ofstream(original, std::ios::binary) << std::string(1000, 'o');
    fs::last_write_time(original, now - std::chrono::hours(40)); // the oldest of all
    ProjectState state = blurredClips(tl);
    for (MediaPtr& m : state.media) {
        if (m && m->id == tl.media('A')) {
            MediaItem copy = *m;
            copy.locator.path = original.generic_string();
            copy.locator.relativePath.clear();
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(state, project, io::SaveOptions{0}));
    QSettings().setValue(QStringLiteral("cache/limit/proxies"), 3500);

    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.open(QUrl::fromLocalFile(QString::fromStdU16String(project.u16string()))));
    // Old thumbnails in the media cache, over a 2.5 kB limit: those the keeper may delete.
    const fs::path thumbnails = dir / "media-cache" / "thumbnails";
    fs::create_directories(thumbnails);
    const fs::path thumbs[] = {thumbnails / "old.wthumb", thumbnails / "middle.wthumb", thumbnails / "new.wthumb"};
    for (int i = 0; i < 3; ++i) {
        std::ofstream(thumbs[i], std::ios::binary) << std::string(1000, 't');
        fs::last_write_time(thumbs[i], now - std::chrono::hours(30 - 10 * i));
    }
    controller.setMediaCacheDirectory(QString::fromStdU16String((dir / "media-cache").u16string()));
    QSettings().setValue(QStringLiteral("cache/limit/thumbnails"), 2500);
    PlaybackBridge playback(&controller);
    MediaReadinessBridge readiness(&lifecycle, &playback);
    readiness.setPreviewRendererForTests(std::make_shared<RecordingRenderer>());
    readiness.setRenderSynchronous(true);

    // On by default: it waits on its timer after start (the dialog was never opened).
    CHECK(readiness.autoEnforce());
    CHECK(readiness.autoEnforcePending());

    // F1A: with the switch off, measuring (Settings opening) and a lower limit delete nothing, and nothing waits.
    readiness.setAutoEnforce(false);
    readiness.scanCaches();
    REQUIRE(waitFor([&] { return !readiness.cacheBusy() && !readiness.caches().isEmpty(); }));
    readiness.setCacheLimit(QStringLiteral("thumbnails"), 1500.0);
    REQUIRE(waitFor([&] { return !readiness.cacheBusy(); }));
    CHECK_FALSE(readiness.autoEnforcePending());
    for (const fs::path& p : thumbs)
        CHECK(fs::exists(p));
    for (const fs::path& p : files)
        CHECK(fs::exists(p));
    // Only the caches no project refers to have a limit: the Proxies row has none (and says why in Settings).
    CHECK(cacheRow(readiness, QStringLiteral("thumbnails")).value(QStringLiteral("limited")).toBool());
    CHECK(cacheRow(readiness, QStringLiteral("thumbnails")).value(QStringLiteral("limitBytes")).toDouble() == 1500.0);
    CHECK_FALSE(cacheRow(readiness, QStringLiteral("proxies")).value(QStringLiteral("limited")).toBool());
    CHECK(cacheRow(readiness, QStringLiteral("proxies")).value(QStringLiteral("limitBytes")).toDouble() == 0.0);
    CHECK_FALSE(cacheRow(readiness, QStringLiteral("analysis")).value(QStringLiteral("limited")).toBool());
    readiness.setCacheLimit(QStringLiteral("thumbnails"), 2500.0);
    REQUIRE(waitFor([&] { return !readiness.cacheBusy(); }));
    readiness.setAutoEnforce(true);
    CHECK(readiness.autoEnforcePending());

    // An export runs: skipped, nothing deleted, tried again later.
    bool exporting = true;
    readiness.setExportBusyProbe([&] { return exporting; });
    CHECK_FALSE(readiness.autoEnforceCaches());
    CHECK(readiness.autoEnforcePending());
    spin(100);
    for (const fs::path& p : files)
        CHECK(fs::exists(p));
    for (const fs::path& p : thumbs)
        CHECK(fs::exists(p));

    // Nothing runs: the oldest thumbnail beyond the limit goes, off the GUI thread. The proxies stay, though past their
    // old limit (F1A), and so does the original in use.
    exporting = false;
    REQUIRE(readiness.autoEnforceCaches());
    CHECK_FALSE(readiness.autoEnforcePending());
    REQUIRE(waitFor([&] { return !readiness.cacheBusy() && !readiness.caches().isEmpty(); }));
    CHECK_FALSE(fs::exists(thumbs[0]));
    CHECK(fs::exists(thumbs[1]));
    CHECK(fs::exists(thumbs[2]));
    for (const fs::path& p : files)
        CHECK(fs::exists(p));
    CHECK(fs::exists(original));

    // The render files are listed with the caches (their size; no limit).
    REQUIRE(readiness.renderInToOut().isEmpty());
    const QVariantMap render = cacheRow(readiness, QStringLiteral("renderFiles"));
    REQUIRE_FALSE(render.isEmpty());
    CHECK(render.value(QStringLiteral("files")).toInt() == 3);
    CHECK(render.value(QStringLiteral("bytes")).toDouble() == 21.0);
    CHECK(render.value(QStringLiteral("name")).toString() == QStringLiteral("Render files (this project)"));
    CHECK(render.value(QStringLiteral("limitBytes")).toDouble() == 0.0);
    // Its Clean deletes every render file of the project.
    readiness.cleanCache(QStringLiteral("renderFiles"));
    CHECK(readiness.previewCache()->entries().empty());
    CHECK(cacheRow(readiness, QStringLiteral("renderFiles")).value(QStringLiteral("files")).toInt() == 0);

    // Off (Settings, tests): no timer, and the job does nothing.
    readiness.setAutoEnforce(false);
    CHECK_FALSE(readiness.autoEnforce());
    CHECK_FALSE(readiness.autoEnforcePending());
    CHECK_FALSE(readiness.autoEnforceCaches());
    CHECK_FALSE(QSettings().value(QStringLiteral("cache/autoEnforce"), true).toBool());
    {
        MediaReadinessBridge again(&lifecycle, &playback);
        CHECK_FALSE(again.autoEnforcePending());
    }
    readiness.setAutoEnforce(true);
    CHECK(readiness.autoEnforcePending());
    resetSettings();
}

TEST_CASE("2AR Sequence menu render rows grey out ask with the size and list render files in Media and cache",
          "[app][qml][render][2a][screenshot]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* readiness = shell.engine.singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness");
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(readiness);
    REQUIRE(file);
    REQUIRE(controller);
    REQUIRE(palette);
    readiness->setAutoEnforce(false);
    readiness->setPreviewRendererForTests(std::make_shared<RecordingRenderer>());
    readiness->setRenderSynchronous(true);
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    controller->resetProject(blurredClips(tl), tl.seq);
    REQUIRE(file->saveAs(QUrl::fromLocalFile(QDir(dir.path()).filePath(QStringLiteral("Film.wabcut")))));
    controller->clearSelection();
    spin(50);
    const auto stateOf = [&](const char* id) { return palette->commandState(QString::fromLatin1(id)); };

    // Greyed, with the reason (menu bar and palette).
    CHECK(stateOf("sequence.renderSelection").value(QStringLiteral("reason")).toString() == QStringLiteral("Select a video clip first"));
    CHECK(stateOf("sequence.deleteRenderFiles").value(QStringLiteral("reason")).toString() == QStringLiteral("This sequence has no render files"));
    CHECK(stateOf("sequence.deleteRenderFilesInToOut").value(QStringLiteral("reason")).toString() ==
          QStringLiteral("Set an In or an Out in the sequence first"));

    // Render Selection from the menu's command path: B renders.
    controller->setSelection({clipAt(*controller->session().snapshot(), tl.seq, "V1@6")});
    CHECK(stateOf("sequence.renderSelection").value(QStringLiteral("enabled")).toBool());
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.renderSelection"))));
    REQUIRE(readiness->previewCache()->entries().size() == 1);
    CHECK(stateOf("sequence.deleteRenderFiles").value(QStringLiteral("enabled")).toBool());
    CHECK(shell.object("toast")->property("text").toString() == QStringLiteral("Previews rendered: the selected clips play in real time"));

    // The Sequence menu: the three rows under Render Effects In to Out.
    QObject* bar = shell.object("appMenuBar");
    REQUIRE(bar);
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 3), Q_ARG(bool, false)));
    QObject* menu = shell.object("sequenceMenuBar");
    REQUIRE(menu);
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }));
    spin(100);
    QObject* selectionRow = menu->findChild<QObject*>(QStringLiteral("menuCommand_sequence.renderSelection"));
    QObject* deleteRow = menu->findChild<QObject*>(QStringLiteral("menuCommand_sequence.deleteRenderFiles"));
    QObject* inOutRow = menu->findChild<QObject*>(QStringLiteral("menuCommand_sequence.deleteRenderFilesInToOut"));
    REQUIRE(selectionRow);
    REQUIRE(deleteRow);
    REQUIRE(inOutRow);
    CHECK(selectionRow->property("enabled").toBool());
    CHECK(deleteRow->property("enabled").toBool());
    CHECK_FALSE(inOutRow->property("enabled").toBool());
    CHECK(inOutRow->property("disabledReason").toString() == QStringLiteral("Set an In or an Out in the sequence first"));
    QQuickItem* menuBox = popupItem(menu);
    REQUIRE(menuBox);
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(150);
        saveCrop(shell.grab(), menuBox, dark ? QStringLiteral("p1-render-menu.png") : QStringLiteral("p1-render-menu-light.png"));
    }
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }));

    // Delete Render Files asks first, with the size; Cancel keeps them, Delete deletes them.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.deleteRenderFiles"))));
    QObject* confirm = shell.object("menuConfirmDialog");
    REQUIRE(confirm);
    REQUIRE(waitFor([&] { return confirm->property("opened").toBool(); }));
    CHECK(confirm->property("title").toString().startsWith(QStringLiteral("Delete ")));
    CHECK(confirm->property("title").toString().endsWith(QStringLiteral(" of render files?")));
    CHECK(confirm->property("text").toString().contains(QStringLiteral("1 file")));
    spin(100);
    QQuickItem* confirmBox = popupItem(confirm);
    REQUIRE(confirmBox);
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(150);
        saveCrop(shell.grab(), confirmBox,
                 dark ? QStringLiteral("p1-delete-render-files.png") : QStringLiteral("p1-delete-render-files-light.png"));
    }
    shell.setAppearance(true, "ultraviolet");
    QQuickItem* cancel = findNamed(confirmBox, QStringLiteral("menuConfirmCancel"));
    REQUIRE(cancel);
    REQUIRE(QMetaObject::invokeMethod(cancel, "clicked"));
    REQUIRE(waitFor([&] { return !confirm->property("visible").toBool(); }));
    CHECK(readiness->previewCache()->entries().size() == 1);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.deleteRenderFiles"))));
    REQUIRE(waitFor([&] { return confirm->property("opened").toBool(); }));
    QQuickItem* confirmButton = findNamed(confirmBox, QStringLiteral("menuConfirmButton"));
    REQUIRE(confirmButton);
    REQUIRE(QMetaObject::invokeMethod(confirmButton, "clicked"));
    REQUIRE(waitFor([&] { return readiness->previewCache()->entries().empty(); }));
    REQUIRE(waitFor([&] { return shell.object("toast")->property("text").toString().startsWith(QStringLiteral("Deleted ")); }));

    // Media & cache: the render files row (no Limit button) and the automatic limits switch.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.renderSelection"))));
    REQUIRE(QMetaObject::invokeMethod(readiness, "openSettings"));
    QObject* loader = shell.object("mediaCacheSettingsLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }));
    REQUIRE(waitFor([&] { return !readiness->cacheBusy() && !readiness->caches().isEmpty(); }));
    spin(100);
    QQuickItem* box = popupItem(dialog);
    REQUIRE(box);
    QQuickItem* row = findNamed(box, QStringLiteral("cacheRow_renderFiles"));
    REQUIRE(row);
    QQuickItem* clean = findNamed(row, QStringLiteral("cacheClean_renderFiles"));
    REQUIRE(clean);
    CHECK(clean->property("enabled").toBool());
    QQuickItem* toggle = findNamed(box, QStringLiteral("autoEnforceToggle"));
    REQUIRE(toggle);
    CHECK_FALSE(toggle->property("checked").toBool());
    // Fix F1A (ARGON-04): a Limit only on the caches no project refers to (thumbnails, waveforms); proxies and analysis
    // results say they are never deleted automatically.
    QQuickItem* thumbRow = findNamed(box, QStringLiteral("cacheRow_thumbnails"));
    REQUIRE(thumbRow);
    QQuickItem* thumbLimit = findNamed(thumbRow, QStringLiteral("cacheLimit_thumbnails"));
    REQUIRE(thumbLimit);
    CHECK(thumbLimit->isVisible());
    QQuickItem* thumbNote = findNamed(thumbRow, QStringLiteral("cacheNoLimit_thumbnails"));
    REQUIRE(thumbNote);
    CHECK_FALSE(thumbNote->isVisible());
    for (const QString& cacheId : {QStringLiteral("proxies"), QStringLiteral("analysis")}) {
        INFO(cacheId.toStdString());
        QQuickItem* cacheItem = findNamed(box, QStringLiteral("cacheRow_") + cacheId);
        REQUIRE(cacheItem);
        QQuickItem* limitItem = findNamed(cacheItem, QStringLiteral("cacheLimit_") + cacheId);
        REQUIRE(limitItem);
        CHECK_FALSE(limitItem->isVisible());
        QQuickItem* noteItem = findNamed(cacheItem, QStringLiteral("cacheNoLimit_") + cacheId);
        REQUIRE(noteItem);
        CHECK(noteItem->isVisible());
    }
    QQuickItem* renderLimit = findNamed(row, QStringLiteral("cacheLimit_renderFiles"));
    REQUIRE(renderLimit);
    CHECK_FALSE(renderLimit->isVisible());
    // The screenshots frame the Disk caches end: the thumbnails row (F1A: with the rows that have a Limit and the ones
    // that say why they have none) down to the switch, scrolled there when the dialog scrolls; the crop is that part
    // of the dialog, so it shows whatever the window size.
    auto* scroller = qobject_cast<QQuickItem*>(findNamed(box, QStringLiteral("mediaCacheScroller")));
    REQUIRE(scroller);
    const auto frame = [&] {
        auto* content = scroller->property("contentItem").value<QQuickItem*>();
        REQUIRE(content);
        // Again until it stays: the cache rows may still be rebuilt (a scan or a render finishing) and scroll back.
        for (int pass = 0; pass < 10; ++pass) {
            const double rowY = thumbRow->mapToItem(content, QPointF(0, 0)).y();
            const double room = std::max(0.0, scroller->property("contentHeight").toDouble() - scroller->height());
            const double wanted = std::clamp(rowY - 40.0, 0.0, room);
            scroller->setProperty("contentY", wanted);
            spin(150);
            if (pass > 0 && std::abs(scroller->property("contentY").toDouble() - wanted) < 1.0 && wanted > 0.0)
                break;
        }
        const QRectF top = thumbRow->mapRectToScene(QRectF(0, 0, thumbRow->width(), thumbRow->height()));
        const QRectF bottom = toggle->mapRectToScene(QRectF(0, 0, toggle->width(), toggle->height()));
        const QRectF dialogRect = box->mapRectToScene(QRectF(0, 0, box->width(), box->height()));
        return QRectF(dialogRect.left(), top.top() - 40, dialogRect.width(), bottom.bottom() - top.top() + 80);
    };
    // A first frame and grab settle the freshly opened dialog (its first grab showed it unscrolled).
    (void)frame();
    (void)shell.grab();
    spin(150);
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(150);
        const QRectF area = frame();
        const QImage image = shell.grab();
        const double dpr = image.devicePixelRatio();
        const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE_FALSE(r.isEmpty());
        REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                    .save(QDir(outputDir()).filePath(dark ? QStringLiteral("p1-render-files-cache.png")
                                                          : QStringLiteral("p1-render-files-cache-light.png"))));
    }
    shell.setAppearance(true, "ultraviolet");
    toggle->setProperty("checked", true);
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
    CHECK(readiness->autoEnforce());
    readiness->setAutoEnforce(false);
    REQUIRE(QMetaObject::invokeMethod(clean, "clicked"));
    REQUIRE(waitFor([&] { return readiness->previewCache()->entries().empty(); }));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    spin(100);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

// Fix round 2 (F2C): Settings > Media & cache cleans only files Wabcut made, asks first with how much and what stays,
// and "Clean regenerable data" keeps the AI masks and chat logs (their own row and Clean, which says they cannot be
// rebuilt). Screenshots: f2c-clean-proxies-confirm.png / -light.png, f2c-clean-kept-confirm.png / -light.png.
TEST_CASE("F2C Media and cache Clean deletes only files Wabcut made asks first and keeps AI masks",
          "[app][qml][cache][screenshot]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* readiness = shell.engine.singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness");
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(readiness);
    REQUIRE(file);
    REQUIRE(controller);
    readiness->setAutoEnforce(false);
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(file->saveAs(QUrl::fromLocalFile(QDir(dir.path()).filePath(QStringLiteral("Film.wabcut")))));
    spin(50);

    // The project's proxy folder holds an unused proxy Wabcut made (another project's, recorded in the folder's index
    // by Wabcut's generator), the user's own video and a note.
    const fs::path proxies(readiness->proxyFolder().toStdU16String());
    fs::create_directories(proxies);
    const fs::path otherProxy = proxies / "Other film.0123abcd.g1.mov";
    const fs::path userVideo = proxies / "My video.mov";
    const fs::path userNote = proxies / "notes.txt";
    std::ofstream(otherProxy, std::ios::binary) << std::string(1000, 'p');
    std::ofstream(userVideo, std::ios::binary) << std::string(3000, 'v');
    std::ofstream(userNote, std::ios::binary) << "notes";
    std::ofstream(proxies / "proxies.json", std::ios::binary)
        << R"({"version": 1, "generator": "wabcut-proxy 1", "proxies": [{"file": "Other film.0123abcd.g1.mov", )"
           R"("generator": "wabcut-proxy 1", "sourcePath": "D:/Other/clip.mp4"}]})";
    // The analysis cache (this test process's own): a transcript, which can be made again, and an AI object mask's
    // matte, which cannot (the analyser id the object mask tracker writes under).
    CHECK(std::ranges::find(editor::kUnrebuildableAnalyzers, ai::segment::kMatteAnalyzer) != editor::kUnrebuildableAnalyzers.end());
    ai::AnalysisCache analysis(ai::AnalysisCache::defaultRoot());
    const ai::AnalysisKey transcriptKey{0xF2C0F2C0F2C0F2C0ull, 0, "transcript", "whisper", "p1"};
    const ai::AnalysisKey matteKey{0xF2C0F2C0F2C0F2C0ull, 0, std::string(ai::segment::kMatteAnalyzer), "sam2", "p2"};
    REQUIRE(analysis.put(transcriptKey, R"({"words": []})").has_value());
    REQUIRE(analysis.put(matteKey, R"({"frames": []})").has_value());
    const fs::path transcriptFile = analysis.pathFor(transcriptKey);
    const fs::path matteFile = analysis.pathFor(matteKey);
    REQUIRE(fs::exists(transcriptFile));
    REQUIRE(fs::exists(matteFile));

    REQUIRE(QMetaObject::invokeMethod(readiness, "openSettings"));
    QObject* loader = shell.object("mediaCacheSettingsLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }));
    REQUIRE(waitFor([&] { return !readiness->cacheBusy() && !readiness->caches().isEmpty(); }));
    spin(100);

    // The rows count only what Wabcut made, and say what Clean deletes.
    const QVariantMap proxyRow = cacheRow(*readiness, QStringLiteral("proxies"));
    CHECK(proxyRow.value(QStringLiteral("files")).toInt() == 1);
    CHECK(proxyRow.value(QStringLiteral("bytes")).toDouble() == 1000.0);
    CHECK(proxyRow.value(QStringLiteral("cleanableFiles")).toInt() == 1);
    CHECK(proxyRow.value(QStringLiteral("proxies")).toBool());
    const QVariantMap analysisRow = cacheRow(*readiness, QStringLiteral("analysis"));
    CHECK(analysisRow.value(QStringLiteral("cleanableFiles")).toInt() == 1);
    CHECK(analysisRow.value(QStringLiteral("rebuildable")).toBool());
    const QVariantMap keptRow = cacheRow(*readiness, QStringLiteral("analysisKept"));
    REQUIRE_FALSE(keptRow.isEmpty());
    CHECK(keptRow.value(QStringLiteral("files")).toInt() == 1);
    CHECK_FALSE(keptRow.value(QStringLiteral("rebuildable")).toBool());

    QQuickItem* box = popupItem(dialog);
    REQUIRE(box);
    QObject* confirm = dialog->findChild<QObject*>(QStringLiteral("cacheCleanConfirm"));
    if (!confirm)
        confirm = shell.object("cacheCleanConfirm");
    REQUIRE(confirm);
    const auto confirmText = [&] {
        QQuickItem* text = findNamed(popupItem(confirm), QStringLiteral("menuConfirmText"));
        return text ? text->property("text").toString() : QString();
    };
    const auto answer = [&](const char* button) {
        QQuickItem* item = findNamed(popupItem(confirm), QString::fromLatin1(button));
        REQUIRE(item);
        REQUIRE(QMetaObject::invokeMethod(item, "clicked"));
        REQUIRE(waitFor([&] { return !confirm->property("visible").toBool(); }));
        REQUIRE(waitFor([&] { return !readiness->cacheBusy(); }));
        spin(50);
    };
    const auto press = [&](const char* row, const char* button) {
        QQuickItem* rowItem = findNamed(box, QString::fromLatin1(row));
        REQUIRE(rowItem);
        QQuickItem* item = findNamed(rowItem, QString::fromLatin1(button));
        REQUIRE(item);
        REQUIRE(item->property("enabled").toBool());
        REQUIRE(QMetaObject::invokeMethod(item, "clicked"));
        REQUIRE(waitFor([&] { return confirm->property("opened").toBool(); }));
        spin(100);
    };

    // The proxies row's Clean asks first: how many, how much, that other projects may still use them. Cancel keeps all.
    press("cacheRow_proxies", "cacheClean_proxies");
    CHECK(confirm->property("title").toString() == QStringLiteral("Delete proxies?"));
    CHECK(confirmText().contains(QStringLiteral("1 proxy that Wabcut made (1.0 KB)")));
    CHECK(confirmText().contains(QStringLiteral("Other projects may still use these proxies")));
    CHECK(confirmText().contains(QStringLiteral("any file Wabcut did not make stay")));
    {
        const QStringList issues = auditAccessibility(popupItem(confirm));
        INFO(join(issues));
        CHECK(issues.isEmpty());
    }
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(150);
        saveCrop(shell.grab(), popupItem(confirm),
                 dark ? QStringLiteral("f2c-clean-proxies-confirm.png") : QStringLiteral("f2c-clean-proxies-confirm-light.png"));
    }
    shell.setAppearance(true, "ultraviolet");
    answer("menuConfirmCancel");
    CHECK(fs::exists(otherProxy));
    // Confirmed: the unused Wabcut proxy goes; the user's video and note stay.
    press("cacheRow_proxies", "cacheClean_proxies");
    answer("menuConfirmButton");
    REQUIRE(waitFor([&] { return !fs::exists(otherProxy) && !readiness->cacheBusy(); }));
    CHECK(fs::exists(userVideo));
    CHECK(fs::exists(userNote));
    CHECK(cacheRow(*readiness, QStringLiteral("proxies")).value(QStringLiteral("files")).toInt() == 0);

    // Clean regenerable data asks too, says what it keeps, and leaves the AI mask's matte.
    std::ofstream(otherProxy, std::ios::binary) << std::string(1000, 'p'); // B made it again
    readiness->scanCaches();
    REQUIRE(waitFor([&] { return !readiness->cacheBusy(); }));
    spin(50);
    QQuickItem* cleanAll = findNamed(box, QStringLiteral("cleanAllButton"));
    REQUIRE(cleanAll);
    REQUIRE(cleanAll->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(cleanAll, "clicked"));
    REQUIRE(waitFor([&] { return confirm->property("opened").toBool(); }));
    CHECK(confirm->property("title").toString() == QStringLiteral("Clean regenerable data?"));
    CHECK(confirmText().contains(QStringLiteral("1 proxy (1.0 KB): other projects may still use them")));
    CHECK(confirmText().contains(QStringLiteral("AI masks and chat logs (they cannot be rebuilt)")));
    CHECK(confirmText().contains(QStringLiteral("any file Wabcut did not make")));
    answer("menuConfirmButton");
    REQUIRE(waitFor([&] { return !fs::exists(transcriptFile) && !readiness->cacheBusy(); }));
    CHECK_FALSE(fs::exists(otherProxy));
    CHECK(fs::exists(matteFile));
    CHECK(fs::exists(userVideo));
    CHECK(fs::exists(userNote));
    CHECK(cacheRow(*readiness, QStringLiteral("analysisKept")).value(QStringLiteral("files")).toInt() == 1);

    // The AI masks' own Clean says they cannot be rebuilt; Cancel keeps them, confirming deletes them.
    QQuickItem* keptNote = findNamed(findNamed(box, QStringLiteral("cacheRow_analysisKept")), QStringLiteral("cacheNoLimit_analysisKept"));
    REQUIRE(keptNote);
    CHECK(keptNote->property("text").toString().startsWith(QStringLiteral("Cannot be rebuilt from the media")));
    press("cacheRow_analysisKept", "cacheClean_analysisKept");
    CHECK(confirm->property("title").toString() == QStringLiteral("Delete AI masks and chat logs?"));
    CHECK(confirmText().contains(QStringLiteral("cannot be rebuilt from the original media")));
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(150);
        saveCrop(shell.grab(), popupItem(confirm),
                 dark ? QStringLiteral("f2c-clean-kept-confirm.png") : QStringLiteral("f2c-clean-kept-confirm-light.png"));
    }
    shell.setAppearance(true, "ultraviolet");
    answer("menuConfirmCancel");
    CHECK(fs::exists(matteFile));
    press("cacheRow_analysisKept", "cacheClean_analysisKept");
    answer("menuConfirmButton");
    REQUIRE(waitFor([&] { return !fs::exists(matteFile) && !readiness->cacheBusy(); }));
    CHECK(fs::exists(userVideo));
    CHECK(cacheRow(*readiness, QStringLiteral("analysisKept")).isEmpty()); // nothing kept: no row

    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    spin(100);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}
