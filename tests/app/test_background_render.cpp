// Background rendering while idle in the app (MediaReadiness through editor/IdleRender.h): the red and yellow stretches of
// the open sequence are rendered by themselves after the idle time, the monitor and the render bar pick the files up, the
// setting and its options are kept and shown in Settings > Media & cache, Sequence > Background Rendering turns it on and
// off, the background tasks popover lists the render with a Stop, and Delete Render Files / Stop hold it until the next
// edit. The worker only runs on a real platform: these tests allow it on the offscreen one with a fake renderer and load.
// Screenshots: background-render-settings.png / -light.png, background-render-popover.png / -light.png.

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "BridgeIds.h"
#include "CommandPalette.h"
#include "LibraryBridge.h"
#include "MediaReadinessBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineFixture.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/editor/IdleRender.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/// A renderer that writes a 7-byte file per segment after `steps` progress calls of `stepMs` each; its first `slowCalls`
/// calls take `slowSteps` calls of `slowMs` each instead (to catch a render in hand).
class SlowRenderer final : public PreviewRenderer {
public:
    std::expected<std::int64_t, Error> render(const ProjectState&, SequenceId, TimeRange range, const fs::path& file,
                                              const std::function<bool(double)>& progress) override {
        {
            std::lock_guard lock(mutex);
            ranges.push_back(range);
        }
        const bool slow = calls++ < slowCalls.load();
        const int total = slow ? slowSteps : steps.load();
        const int ms = slow ? slowMs : stepMs.load();
        for (int i = 0; i < total; ++i) {
            if (progress && !progress(static_cast<double>(i + 1) / total)) {
                ++cancelled;
                return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
            }
            if (ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        std::ofstream(file, std::ios::binary) << "preview";
        return std::int64_t{7};
    }
    std::size_t started() {
        std::lock_guard lock(mutex);
        return ranges.size();
    }
    std::atomic<int> steps{3};
    std::atomic<int> stepMs{0};
    std::atomic<int> slowCalls{0};
    int slowSteps = 400;
    int slowMs = 5;
    std::atomic<int> calls{0};
    std::atomic<int> cancelled{0};
    std::mutex mutex;
    std::vector<TimeRange> ranges;
};

class CalmProbe final : public editor::SystemLoadProbe {
public:
    editor::SystemLoad sample(bool) override {
        return {.cpuOthers = 3.0, .gpu = 3.0, .power = editor::SystemLoad::Power::Mains, .batterySaver = false};
    }
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

/// Lets the bridge look at the editor (its timer does it every 250 ms) and the event loop run.
bool pollUntil(MediaReadinessBridge& readiness, const std::function<bool()>& done, int timeoutMs = 15000) {
    return waitFor(
        [&] {
            readiness.pollNow();
            return done();
        },
        timeoutMs);
}

/// A bridge on a saved project of three blurred clips, with the fake renderer and the worker allowed.
struct BackgroundBridge {
    fs::path dir;
    Timeline tl;
    ProjectController controller;
    ProjectLifecycle lifecycle;
    PlaybackBridge playback;
    MediaReadinessBridge readiness;
    std::shared_ptr<SlowRenderer> renderer = std::make_shared<SlowRenderer>();

    explicit BackgroundBridge(const char* name)
        : dir(freshDir(name)), tl(R"(
            V1 tgt |Aaaa..Bbbb..Cccc|
        )"),
          lifecycle(&controller), playback(&controller), readiness(&lifecycle, &playback) {
        readiness.setAutoEnforce(false);
        const fs::path project = dir / "film.wabcut";
        REQUIRE(io::saveProject(blurredClips(tl), project, io::SaveOptions{0}));
        REQUIRE(lifecycle.open(QUrl::fromLocalFile(QString::fromStdU16String(project.u16string()))));
        readiness.setPreviewRendererForTests(renderer);
        readiness.setBackgroundRenderAllowedForTests(true, std::make_shared<CalmProbe>());
        readiness.pollNow();
        REQUIRE(readiness.backgroundRenderer() != nullptr);
        readiness.backgroundRenderer()->setPollInterval(5ms);
        readiness.noteInput(); // the user just used it: nothing starts until a test lets it (noteInput(100))
    }
};

} // namespace

TEST_CASE("Background render: the worker starts only on a real platform and renders the stretches by itself",
          "[app][render][background]") {
    QSettings().clear();
    const fs::path dir = freshDir("background-render-start");
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
    auto renderer = std::make_shared<SlowRenderer>();
    readiness.setPreviewRendererForTests(renderer);

    // On the offscreen test platform nothing starts by itself (no user, no compositor): on, waiting, no worker.
    CHECK(readiness.backgroundRender());
    readiness.pollNow();
    CHECK(readiness.backgroundRenderer() == nullptr);
    CHECK(readiness.backgroundState() == QStringLiteral("waiting"));
    CHECK_FALSE(readiness.backgroundActive());

    // Allowed (as on a real platform): it renders all three stretches after the idle time and tells the render bar.
    readiness.setBackgroundRenderAllowedForTests(true, std::make_shared<CalmProbe>());
    readiness.pollNow();
    REQUIRE(readiness.backgroundRenderer() != nullptr);
    readiness.backgroundRenderer()->setPollInterval(5ms);
    editor::IdleRenderSettings quick = readiness.backgroundRenderer()->settings();
    quick.idleSeconds = 0.3;
    readiness.backgroundRenderer()->setSettings(quick);
    int filesChanged = 0;
    QObject::connect(&readiness, &MediaReadinessBridge::previewFilesChanged, [&] { ++filesChanged; });
    const std::size_t undoBefore = controller.session().undoCount();
    readiness.noteInput(); // input right now: it waits for the idle time first
    readiness.pollNow();
    CHECK(renderer->started() == 0);
    REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundState() == QStringLiteral("upToDate"); }));
    CHECK(renderer->started() == 3);
    CHECK(readiness.backgroundRendered() == 3);
    CHECK(readiness.backgroundRemaining() == 0);
    CHECK(filesChanged >= 1);
    const std::shared_ptr<PreviewCache> cache = readiness.previewCache();
    REQUIRE(cache->entries().size() == 3);
    for (const PreviewEntry& e : cache->entries())
        CHECK(fs::exists(cache->folder() / e.file));
    // The monitor was told (its scheduler plays the files), and rendering is not an edit.
    CHECK(playback.previewSettings().previews == cache);
    CHECK(controller.session().undoCount() == undoBefore);
    CHECK(readiness.backgroundText().contains(QStringLiteral("render file")));
    // render.status (the assistant's command) shows it too, from any thread.
    const auto services = std::dynamic_pointer_cast<command::PreviewServices>(readiness.previewServices());
    REQUIRE(services);
    REQUIRE(services->backgroundStatus);
    const command::Json status = services->backgroundStatus();
    CHECK(status.value("enabled", false));
    CHECK(status.value("state", std::string()) == "upToDate");
    CHECK(status.value("rendered", -1) == 3);
    CHECK(status.value("remaining", -1) == 0);
    CHECK_FALSE(status.value("text", std::string()).empty());
    // A manual render finds nothing left, and nothing more is rendered in the background.
    CHECK(readiness.renderInToOut().isEmpty());
    CHECK(renderer->started() == 3);
    QSettings().clear();
}

TEST_CASE("Background render: the options are kept, clamped, and turn it off", "[app][render][background]") {
    QSettings().clear();
    {
        BackgroundBridge b("background-render-options");
        MediaReadinessBridge& readiness = b.readiness;
        CHECK(readiness.backgroundRender());
        CHECK(readiness.backgroundIdleSeconds() == 10);
        CHECK(readiness.backgroundPauseOnBattery());
        CHECK(readiness.backgroundPauseWhenBusy());

        readiness.setBackgroundIdleSeconds(1); // under the minimum
        CHECK(readiness.backgroundIdleSeconds() == 3);
        readiness.setBackgroundIdleSeconds(5000);
        CHECK(readiness.backgroundIdleSeconds() == 600);
        readiness.setBackgroundIdleSeconds(30);
        readiness.setBackgroundPauseOnBattery(false);
        readiness.setBackgroundPauseWhenBusy(false);
        CHECK(readiness.backgroundRenderer()->settings().idleSeconds == 30.0);
        CHECK_FALSE(readiness.backgroundRenderer()->settings().pauseOnBattery);
        CHECK_FALSE(readiness.backgroundRenderer()->settings().pauseWhenBusy);
        // A new bridge (the next start) reads them back.
        {
            MediaReadinessBridge again(&b.lifecycle, &b.playback);
            CHECK(again.backgroundIdleSeconds() == 30);
            CHECK_FALSE(again.backgroundPauseOnBattery());
            CHECK_FALSE(again.backgroundPauseWhenBusy());
            CHECK(again.backgroundRender());
        }

        // Off: no render, the state says so; on again: it works.
        readiness.setBackgroundIdleSeconds(3);
        readiness.backgroundRenderer()->setSettings(
            {.enabled = false, .idleSeconds = 0.2, .pauseOnBattery = true, .pauseWhenBusy = true});
        readiness.setBackgroundRender(false);
        CHECK_FALSE(readiness.backgroundRender());
        CHECK_FALSE(QSettings().value(QStringLiteral("render/background"), true).toBool());
        readiness.pollNow();
        REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundState() == QStringLiteral("off"); }));
        CHECK(readiness.backgroundText() == QStringLiteral("Background rendering is off."));
        spin(300);
        CHECK(b.renderer->started() == 0);
        readiness.toggleBackgroundRender();
        CHECK(readiness.backgroundRender());
        editor::IdleRenderSettings quick = readiness.backgroundRenderer()->settings();
        quick.idleSeconds = 0.2;
        readiness.backgroundRenderer()->setSettings(quick);
        readiness.noteInput(100.0); // the user has been away
        REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundState() == QStringLiteral("upToDate"); }));
        CHECK(b.renderer->started() == 3);
    }
    QSettings().clear();
}

TEST_CASE("Background render: playback, an export and a manual render hold it back and Delete Render Files holds it until the next edit",
          "[app][render][background]") {
    QSettings().clear();
    BackgroundBridge b("background-render-holds");
    MediaReadinessBridge& readiness = b.readiness;
    editor::IdleRenderSettings quick = readiness.backgroundRenderer()->settings();
    quick.idleSeconds = 0.2;
    readiness.backgroundRenderer()->setSettings(quick);
    std::atomic<bool> exporting{true};
    readiness.setExportBusyProbe([&] { return exporting.load(); });
    readiness.pollNow(); // the worker sees the export before it sees the user away
    readiness.noteInput(100.0); // away for a while

    // An export running: it waits, says why, and goes on when the export ends.
    REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundText() == QStringLiteral("Waiting: an export is running."); }));
    spin(150);
    CHECK(b.renderer->started() == 0);
    exporting = false;
    REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundState() == QStringLiteral("upToDate"); }));
    CHECK(b.renderer->started() == 3);

    // Delete Render Files: the files go and stay gone until the project changes (the next edit), however idle it is.
    readiness.setRenderSynchronous(true);
    int deleted = 0;
    QObject::connect(&readiness, &MediaReadinessBridge::renderFilesDeleted, [&](bool ok, const QString&) { deleted += ok; });
    REQUIRE(readiness.deleteRenderFiles(false).isEmpty());
    CHECK(deleted == 1);
    CHECK(readiness.previewCache()->entries().empty());
    REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundText() == QStringLiteral("Waiting: stopped until your next edit."); }));
    spin(400);
    readiness.pollNow();
    CHECK(b.renderer->started() == 3); // nothing was rendered again
    CHECK(readiness.previewCache()->entries().empty());

    // An edit (a change of one clip) ends the hold: the stretches are rendered again.
    REQUIRE(b.controller.session().apply("Change", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        std::vector<ClipId> clip{clipAt(s, b.tl.seq, "V1@0")};
        return ok(edit::addEffect(s, b.tl.seq, std::span(clip), "wabcut.sharpen")).state;
    }));
    REQUIRE(pollUntil(readiness, [&] { return readiness.previewCache()->entries().size() == 3; }));
    CHECK(b.renderer->started() == 6);
    REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundState() == QStringLiteral("upToDate"); }));
    QSettings().clear();
}

TEST_CASE("Background render: a render the user starts takes over from the one in the background",
          "[app][render][background]") {
    QSettings().clear();
    BackgroundBridge b("background-render-manual");
    MediaReadinessBridge& readiness = b.readiness;
    editor::IdleRenderSettings quick = readiness.backgroundRenderer()->settings();
    quick.idleSeconds = 0.2;
    readiness.backgroundRenderer()->setSettings(quick);
    b.renderer->slowCalls = 1; // the background render's first segment is slow enough to catch mid-way (2 s)
    readiness.noteInput(100.0); // away for a while
    REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundActive(); }));
    CHECK(readiness.backgroundState() == QStringLiteral("rendering"));
    CHECK(readiness.backgroundRemaining() == 3);
    CHECK(readiness.backgroundText().contains(QStringLiteral("3 segments left")));
    const std::size_t startedInBackground = b.renderer->started();
    REQUIRE(startedInBackground >= 1);

    // Render In to Out (on the calling thread, fast): the background one gives its segment up first.
    readiness.setRenderSynchronous(true);
    REQUIRE(readiness.renderInToOut().isEmpty());
    REQUIRE(pollUntil(readiness, [&] { return b.renderer->cancelled >= 1; }));
    CHECK(b.renderer->cancelled >= 1);
    CHECK(readiness.previewCache()->entries().size() == 3);
    REQUIRE(pollUntil(readiness, [&] { return readiness.backgroundState() == QStringLiteral("upToDate"); }));
    QSettings().clear();
}

TEST_CASE("Background render: Settings, the Sequence menu and the background tasks list show and control it",
          "[app][qml][render][background][screenshot]") {
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
    auto renderer = std::make_shared<SlowRenderer>();
    readiness->setPreviewRendererForTests(renderer);
    readiness->setBackgroundRenderAllowedForTests(true, std::make_shared<CalmProbe>());
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    controller->resetProject(blurredClips(tl), tl.seq);
    REQUIRE(file->saveAs(QUrl::fromLocalFile(QDir(dir.path()).filePath(QStringLiteral("Film.wabcut")))));
    spin(50);
    readiness->pollNow();
    REQUIRE(readiness->backgroundRenderer() != nullptr);
    readiness->backgroundRenderer()->setPollInterval(5ms);
    readiness->noteInput(); // just used: it waits (the status line says for how long)
    readiness->pollNow();
    CHECK(readiness->backgroundText() == QStringLiteral("Waiting for 10 seconds without input before it renders."));

    // The palette command and the menu row toggle the setting; the row is a checkable row of the Sequence menu.
    CHECK(palette->commandState(QStringLiteral("sequence.backgroundRender")).value(QStringLiteral("enabled")).toBool());
    QObject* bar = shell.object("appMenuBar");
    REQUIRE(bar);
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 3), Q_ARG(bool, false)));
    QObject* menu = shell.object("sequenceMenuBar");
    REQUIRE(menu);
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }));
    spin(100);
    QObject* row = menu->findChild<QObject*>(QStringLiteral("sequenceMenuBackgroundRender"));
    REQUIRE(row);
    CHECK(row->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.backgroundRender"))));
    CHECK_FALSE(readiness->backgroundRender());
    CHECK(readiness->backgroundState() == QStringLiteral("off"));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.backgroundRender"))));
    CHECK(readiness->backgroundRender());

    // Settings > Media & cache: the section with its switch, start time tabs, the two waits and the status line.
    REQUIRE(QMetaObject::invokeMethod(readiness, "openSettings"));
    QObject* loader = shell.object("mediaCacheSettingsLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }));
    spin(150);
    QQuickItem* box = popupItem(dialog);
    REQUIRE(box);
    QQuickItem* toggle = findNamed(box, QStringLiteral("backgroundRenderToggle"));
    QQuickItem* tabs = findNamed(box, QStringLiteral("backgroundIdleTabs"));
    QQuickItem* battery = findNamed(box, QStringLiteral("backgroundBatteryToggle"));
    QQuickItem* busy = findNamed(box, QStringLiteral("backgroundBusyToggle"));
    QQuickItem* status = findNamed(box, QStringLiteral("backgroundRenderStatus"));
    REQUIRE(toggle);
    REQUIRE(tabs);
    REQUIRE(battery);
    REQUIRE(busy);
    REQUIRE(status);
    CHECK(toggle->property("checked").toBool());
    CHECK(tabs->property("currentIndex").toInt() == 1); // 10 s
    CHECK(battery->property("checked").toBool());
    CHECK(busy->property("checked").toBool());
    CHECK(status->property("text").toString() == QStringLiteral("Waiting for 10 seconds without input before it renders."));
    // The tabs change the setting; the switch greys the options out.
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 2)));
    CHECK(readiness->backgroundIdleSeconds() == 30);
    battery->setProperty("checked", false);
    REQUIRE(QMetaObject::invokeMethod(battery, "toggled"));
    CHECK_FALSE(readiness->backgroundPauseOnBattery());
    readiness->setBackgroundIdleSeconds(10);
    readiness->setBackgroundPauseOnBattery(true);
    auto* scroller = qobject_cast<QQuickItem*>(findNamed(box, QStringLiteral("mediaCacheScroller")));
    REQUIRE(scroller);
    const auto frame = [&] {
        auto* content = scroller->property("contentItem").value<QQuickItem*>();
        REQUIRE(content);
        for (int pass = 0; pass < 10; ++pass) {
            const double rowY = toggle->mapToItem(content, QPointF(0, 0)).y();
            const double room = std::max(0.0, scroller->property("contentHeight").toDouble() - scroller->height());
            const double wanted = std::clamp(rowY - 40.0, 0.0, room);
            scroller->setProperty("contentY", wanted);
            spin(150);
            if (std::abs(scroller->property("contentY").toDouble() - wanted) < 1.0)
                break;
        }
        const QRectF top = toggle->mapRectToScene(QRectF(0, 0, toggle->width(), toggle->height()));
        const QRectF bottom = status->mapRectToScene(QRectF(0, 0, status->width(), status->height()));
        const QRectF dialogRect = box->mapRectToScene(QRectF(0, 0, box->width(), box->height()));
        // The explanation under the status line is part of the picture.
        return QRectF(dialogRect.left(), top.top() - 40, dialogRect.width(), bottom.bottom() - top.top() + 110);
    };
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
                    .save(QDir(outputDir()).filePath(dark ? QStringLiteral("background-render-settings.png")
                                                          : QStringLiteral("background-render-settings-light.png"))));
    }
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    spin(100);

    // The background tasks list: a segment in hand, paused because the user touched the mouse; Stop gives it up.
    renderer->slowCalls = 1000; // every render in the background takes 3 s: it can be paused, shown and stopped
    renderer->slowSteps = 600;
    {
        editor::IdleRenderSettings quick = readiness->backgroundRenderer()->settings();
        quick.idleSeconds = 3.0; // the minimum a user can choose
        readiness->backgroundRenderer()->setSettings(quick);
    }
    readiness->noteInput(100.0);
    REQUIRE(pollUntil(*readiness, [&] { return readiness->backgroundState() == QStringLiteral("rendering"); }));
    readiness->noteInput(); // the mouse moves: paused at once, for the idle time
    REQUIRE(pollUntil(*readiness, [&] { return readiness->backgroundState() == QStringLiteral("paused"); }));
    CHECK(readiness->backgroundActive());
    CHECK(readiness->backgroundText() == QStringLiteral("Paused: you used the keyboard or mouse."));
    CHECK(readiness->backgroundDetail() == QStringLiteral("paused while you work"));
    // The title bar's activity pill shows it too (in the warning hue while it waits).
    QQuickItem* pill = shell.item("activityPill");
    REQUIRE(pill != nullptr);
    CHECK(pill->isVisible());
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("window.backgroundTasks"))));
    QObject* popover = shell.object("activityPopover");
    REQUIRE(popover);
    REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }));
    spin(150);
    QQuickItem* popBox = popupItem(popover);
    REQUIRE(popBox);
    QQuickItem* jobRow = findNamed(popBox, QStringLiteral("activityBackgroundRender"));
    REQUIRE(jobRow);
    CHECK(jobRow->isVisible());
    QQuickItem* stop = findNamed(jobRow, QStringLiteral("activityBackgroundRenderAction"));
    REQUIRE(stop);
    (void)shell.grab(); // the popover has opened and settled before the first picture
    spin(300);
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(400);
        readiness->noteInput(); // keep it paused while the picture is taken
        const QRectF r = popBox->mapRectToScene(QRectF(0, 0, popBox->width(), popBox->height())).adjusted(-16, -16, 16, 16);
        const QImage image = shell.grab();
        const double dpr = image.devicePixelRatio();
        const QRectF cut = r.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE_FALSE(cut.isEmpty());
        REQUIRE(image.copy(QRectF(cut.x() * dpr, cut.y() * dpr, cut.width() * dpr, cut.height() * dpr).toRect())
                    .save(QDir(outputDir()).filePath(dark ? QStringLiteral("background-render-popover.png")
                                                          : QStringLiteral("background-render-popover-light.png"))));
    }
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(QMetaObject::invokeMethod(stop, "clicked"));
    REQUIRE(pollUntil(*readiness, [&] { return readiness->backgroundText() == QStringLiteral("Waiting: stopped until your next edit."); }));
    CHECK_FALSE(readiness->backgroundActive());
    REQUIRE(waitFor([&] { return !jobRow->isVisible(); }));
    REQUIRE(QMetaObject::invokeMethod(popover, "close"));
    spin(100);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

// Card "Dolby Vision 8.4 and HDR10+ dynamic metadata": Get Properties of a clip lists the Dolby Vision configuration its
// container declares and the HDR10+ its first frames carry; a clip with neither has no such row.
TEST_CASE("Get Properties lists a stream's Dolby Vision configuration and HDR10+", "[app][library][hdr]") {
    QSettings().clear();
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    ProjectState state = tl.state;
    for (MediaPtr& m : state.media) {
        if (m->id != tl.media('A'))
            continue;
        auto item = std::make_shared<MediaItem>(*m);
        item->info.video.front().dolbyVision = DolbyVisionInfo{8, 6, 4, true, false, true};
        item->info.video.front().hdr10Plus = true;
        m = item;
    }
    ProjectController controller;
    LibraryBridge library;
    controller.resetProject(state, tl.seq);
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setAnalysisProbe({});
    library.setController(&controller);
    const auto lineOf = [&](char letter, const QString& label) -> QString {
        const QVariantMap info = library.mediaProperties(mediaKey(tl.media(letter)));
        for (const QVariant& v : library.propertyLines(info)) {
            const QVariantMap line = v.toMap();
            if (line.value(QStringLiteral("label")).toString() == label)
                return line.value(QStringLiteral("value")).toString();
        }
        return {};
    };
    const QString dynamic = lineOf('A', QStringLiteral("Dynamic HDR"));
    INFO(dynamic.toStdString());
    CHECK(dynamic.contains(QStringLiteral("Dolby Vision profile 8.4, level 6, HLG base layer, RPU in the stream")));
    CHECK(dynamic.contains(QStringLiteral("HDR10+ (SMPTE ST 2094-40) in the first frames")));
    // Another clip: no row at all.
    CHECK(lineOf('B', QStringLiteral("Dynamic HDR")).isEmpty());
    // The other base layers: 8.1 is an HDR10 one, 8.2 an SDR one; a stream without an RPU says so.
    for (const auto& [compat, text] : {std::pair{1, "HDR10 base layer"}, std::pair{2, "SDR base layer"}}) {
        ProjectState other = state;
        for (MediaPtr& m : other.media) {
            if (m->id == tl.media('A')) {
                auto item = std::make_shared<MediaItem>(*m);
                item->info.video.front().dolbyVision = DolbyVisionInfo{8, 5, compat, false, false, true};
                item->info.video.front().hdr10Plus = false;
                m = item;
            }
        }
        controller.resetProject(other, tl.seq);
        const QString line = lineOf('A', QStringLiteral("Dynamic HDR"));
        INFO(line.toStdString());
        CHECK(line.contains(QStringLiteral("Dolby Vision profile 8.%1").arg(compat)));
        CHECK(line.contains(QString::fromLatin1(text)));
        CHECK(line.contains(QStringLiteral("no RPU")));
        CHECK_FALSE(line.contains(QStringLiteral("HDR10+")));
    }
    QSettings().clear();
}
