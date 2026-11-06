// Stream A, batch A8 (0.7): the Premiere keys that did nothing (card 024: Media Browser keys, Loop in Effect Controls and the
// Audio Track Mixer, the work area bar and Clear Work Area, the Export drawer's Work Area range) and Render and Replace for
// nested sequences (card 069: the bridge over the export queue, the dialog, the menus). C++ rigs for the bridge, the real shell
// (Main.qml, hidden) for the QML. Screenshots under test-output/ui (a8-*.png).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "CommandPalette.h"
#include "ExportController.h"
#include "KeymapDispatcher.h"
#include "LibraryBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "RenderReplaceBridge.h"
#include "SourceMonitorBridge.h"
#include "TimelineView.h"

#include <wabcut/editor/MediaImport.h>
#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/keymap/KeymapManager.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RenderReplace.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QQmlComponent>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

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

QObject* findObject(QObject* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QObject* child : root->children()) {
        if (QObject* found = findObject(child, name))
            return found;
    }
    if (const auto* item = qobject_cast<QQuickItem*>(root)) {
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = findObject(child, name))
                return found;
        }
    }
    return nullptr;
}

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name, int margin = 16) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                         .adjusted(-margin, -margin, margin, margin)
                         .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

/// The keyboard focus is `item` or inside it.
bool focusInside(Shell& shell, const QQuickItem* item) {
    for (const QQuickItem* focus = shell.window->activeFocusItem(); focus; focus = focus->parentItem()) {
        if (focus == item)
            return true;
    }
    return false;
}

bool runCommand(Shell& shell, const char* id) {
    return QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(id)));
}

std::filesystem::path testMedia(const char* name) { return std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name; }
bool haveMedia() { return std::filesystem::is_regular_file(testMedia("GX010042.MP4")); }

/// Every .qml file below the app's QML folder, as one text (the shell's routers name the ids they run).
QString allQml() {
    static const QString text = [] {
        QString all;
        const std::function<void(const QString&)> walk = [&](const QString& dir) {
            for (const QFileInfo& entry : QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (entry.isDir()) {
                    walk(entry.absoluteFilePath());
                } else if (entry.suffix() == QLatin1String("qml")) {
                    QFile file(entry.absoluteFilePath());
                    if (file.open(QIODevice::ReadOnly))
                        all += QString::fromUtf8(file.readAll());
                }
            }
        };
        walk(QStringLiteral(WABCUT_APP_QML_DIR));
        return all;
    }();
    return text;
}

} // namespace

// =====================================================================================================================
// 024: every Premiere default key runs something
// =====================================================================================================================

// The two shipped tables (Windows, macOS): each command bound in them is run by the app - the palette's rules know it
// (ProjectController's actions, the shell's router list, a bridge's own ids) or a QML router names it - or it is on the list
// below, which is empty: a key with no Wabcut feature behind it is unbound instead (tests/keymap: Premiere default keys of
// features Wabcut does not have are unbound), so no panel swallows a key that works elsewhere.
TEST_CASE("Every command bound in the shipped Premiere tables is run by the app (card 024)", "[app][keymap][coverage][a8]") {
    const std::set<std::string> knownUnrouted{};
    const QString qml = allQml();
    REQUIRE(qml.size() > 100'000);
    for (const keymap::Platform platform : {keymap::Platform::Windows, keymap::Platform::MacOS}) {
        const auto table = keymap::KeymapManager::withDefaults(platform);
        REQUIRE(table);
        std::set<std::string> ids;
        for (const keymap::Binding& b : table->bindings())
            ids.insert(b.command);
        REQUIRE(ids.size() > 200);
        for (const std::string& id : ids) {
            const QString name = QString::fromStdString(id);
            INFO(id);
            const bool known = ProjectController::handlesCommand(name) || KeymapCommandProvider::viewHandles(name) ||
                               qml.contains(QLatin1Char('"') + name + QLatin1Char('"'));
            if (knownUnrouted.contains(id)) {
                CHECK_FALSE(known); // a stale entry: the key works now, take it off the list
            } else {
                CHECK(known);
            }
        }
    }
}

TEST_CASE("The keys of card 024 are in the palette's router list and ready", "[app][keymap][palette][a8]") {
    for (const char* id : {"mediaBrowser.openInSourceMonitor", "mediaBrowser.selectDirectoryList", "mediaBrowser.selectMediaList",
                           "effectControls.loopAudioPlayback", "audioMixer.loop", "clip.renderAndReplace",
                           "clip.restoreUnrendered", "audioMixer.showHideTracks", "audioMixer.meterInputsOnly"}) {
        INFO(id);
        CHECK(KeymapCommandProvider::viewHandles(QString::fromLatin1(id)));
    }
    CHECK(ProjectController::handlesCommand(QStringLiteral("timeline.clearWorkArea")));
    // The unbound ones are not run by anything, which is why they have no key.
    for (const char* id : {"metadata.loop", "metadata.play"}) {
        INFO(id);
        CHECK_FALSE(KeymapCommandProvider::viewHandles(QString::fromLatin1(id)));
    }
}

// =====================================================================================================================
// 024: the Media Browser's keys and Loop, through the shell
// =====================================================================================================================

TEST_CASE("Media Browser keys and Loop in Effect Controls and the Audio Track Mixer act through the shell (card 024)",
          "[app][keymap][qml][a8]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(playback);
    REQUIRE(keys);

    // Loop: both panels' Ctrl+L toggle the Program Monitor's Loop, and say so.
    const bool loop = playback->loop();
    REQUIRE(runCommand(shell, "audioMixer.loop"));
    CHECK(playback->loop() == !loop);
    REQUIRE(runCommand(shell, "effectControls.loopAudioPlayback"));
    CHECK(playback->loop() == loop);

    // The Media Browser: its panel has the keymap scope, the list takes the focus, the places menu opens.
    REQUIRE(runCommand(shell, "window.mediaBrowser"));
    QQuickItem* browser = nullptr;
    REQUIRE(waitFor([&] {
        browser = findNamed(shell.window->contentItem(), QStringLiteral("mediaBrowserPanel"));
        return browser != nullptr && browser->isVisible();
    }, 8000));
    spin(150);
    QQuickItem* list = findNamed(browser, QStringLiteral("mediaBrowserList"));
    REQUIRE(list);
    CHECK(keys->scopeAt(list) == QStringLiteral("mediaBrowser"));
    CHECK(keys->scopeAt(browser) == QStringLiteral("mediaBrowser"));
    REQUIRE(runCommand(shell, "mediaBrowser.selectMediaList"));
    CHECK(waitFor([&] { return focusInside(shell, list); }));

    QObject* places = findObject(browser, QStringLiteral("mediaBrowserPlacesMenu"));
    REQUIRE(places);
    REQUIRE(runCommand(shell, "mediaBrowser.selectDirectoryList"));
    CHECK(waitFor([&] { return places->property("visible").toBool(); }));

    // Open in Source Monitor needs a clip as the current row: in a folder with nothing in it there is none.
    QMetaObject::invokeMethod(places, "close");
    spin(100);
    QTemporaryDir empty;
    REQUIRE(empty.isValid());
    bool opened = false;
    REQUIRE(QMetaObject::invokeMethod(browser, "openFolder", Q_RETURN_ARG(bool, opened), Q_ARG(QVariant, empty.path())));
    clearMessages();
    bool shown = true;
    REQUIRE(QMetaObject::invokeMethod(browser, "openInSourceMonitor", Q_RETURN_ARG(bool, shown)));
    CHECK_FALSE(browser->property("currentIsClip").toBool());
    CHECK_FALSE(shown);
    CHECK(takeMessages().isEmpty());
}

TEST_CASE("Open in Source Monitor loads the Media Browser's current clip without importing it (card 024)",
          "[app][keymap][qml][a8]") {
    if (!haveMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    resetSettings();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString clip = QDir(folder.path()).filePath(QStringLiteral("interview.mp4"));
    REQUIRE(QFile::copy(QString::fromStdWString(testMedia("GX010042.MP4").wstring()), clip));

    REQUIRE(runCommand(shell, "window.mediaBrowser"));
    QQuickItem* browser = nullptr;
    REQUIRE(waitFor([&] {
        browser = findNamed(shell.window->contentItem(), QStringLiteral("mediaBrowserPanel"));
        return browser != nullptr && browser->isVisible();
    }, 8000));
    bool opened = false;
    REQUIRE(QMetaObject::invokeMethod(browser, "openFolder", Q_RETURN_ARG(bool, opened), Q_ARG(QVariant, folder.path())));
    REQUIRE(opened);
    REQUIRE(browser->property("currentIsClip").toBool());
    const std::size_t media = controller->snapshot()->media.size();
    bool shown = false;
    REQUIRE(QMetaObject::invokeMethod(browser, "openInSourceMonitor", Q_RETURN_ARG(bool, shown)));
    CHECK(shown);
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    REQUIRE(source);
    CHECK(waitFor([&] { return source->loaded(); }));
    CHECK(controller->snapshot()->media.size() == media); // loaded, not imported
}

// =====================================================================================================================
// 024: the work area
// =====================================================================================================================

TEST_CASE("Clear Work Area is one undo step and greys out without a work area (card 024)", "[app][workarea][a8]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    const auto state = [&] { return controller.commandAvailability(QStringLiteral("timeline.clearWorkArea")); };
    CHECK(state().value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(state().value(QStringLiteral("enabled")).toBool());
    CHECK(state().value(QStringLiteral("reason")).toString() == QStringLiteral("The sequence has no work area"));
    const std::size_t steps = controller.session().undoCount();
    REQUIRE(controller.runCommand(QStringLiteral("timeline.clearWorkArea"))); // nothing to clear: no empty step
    CHECK(controller.session().undoCount() == steps);

    controller.setPlayhead(f(12));
    REQUIRE(controller.runCommand(QStringLiteral("timeline.setWorkAreaOut")));
    controller.setPlayhead(f(4));
    REQUIRE(controller.runCommand(QStringLiteral("timeline.setWorkAreaIn")));
    REQUIRE(controller.workArea().has_value());
    CHECK(controller.workArea()->start == f(4));
    CHECK(controller.workArea()->end() == f(12));
    CHECK(state().value(QStringLiteral("enabled")).toBool());

    const std::size_t before = controller.session().undoCount();
    REQUIRE(controller.runCommand(QStringLiteral("timeline.clearWorkArea")));
    CHECK_FALSE(controller.workArea().has_value());
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(controller.undoText().contains(QStringLiteral("Clear Work Area")));
    REQUIRE(controller.undo());
    CHECK(controller.workArea().has_value());
    CHECK(controller.workArea()->start == f(4));
}

TEST_CASE("The ruler draws the work area as a thin bar and clears with the command in dark and light (card 024)",
          "[app][workarea][qml][screenshot][a8]") {
    for (const bool dark : {true, false}) {
        INFO((dark ? "dark" : "light"));
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        Timeline tl(R"(
            V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
            A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        )");
        controller->resetProject(tl.state, tl.seq);
        spin(300);
        QQuickItem* bar = findNamed(shell.window->contentItem(), QStringLiteral("rulerWorkArea"));
        REQUIRE(bar);
        CHECK_FALSE(bar->isVisible()); // no work area, no bar
        auto* view = qobject_cast<TimelineView*>(findNamed(shell.window->contentItem(), QStringLiteral("timelineView")));
        REQUIRE(view);
        CHECK(std::isnan(view->workAreaInX()));

        // Alt+[ and Alt+] through the keymap commands the shell routes.
        controller->setPlayhead(f(30));
        REQUIRE(controller->runCommand(QStringLiteral("timeline.setWorkAreaOut")));
        controller->setPlayhead(f(10));
        REQUIRE(controller->runCommand(QStringLiteral("timeline.setWorkAreaIn")));
        spin(200);
        REQUIRE(controller->workArea().has_value());
        CHECK(bar->isVisible());
        CHECK(bar->x() == std::round(view->workAreaInX()));
        CHECK(bar->width() == std::round(view->workAreaOutX()) - std::round(view->workAreaInX()));
        CHECK(bar->width() > 10);
        CHECK(bar->height() < 6); // thin
        CHECK(bar->property("enabled").toBool());

        const QImage shot = shell.grab();
        REQUIRE_FALSE(shot.isNull());
        QQuickItem* ruler = findNamed(shell.window->contentItem(), QStringLiteral("timelineRuler"));
        saveCrop(shot, ruler ? ruler : bar, dark ? QStringLiteral("a8-work-area-bar.png") : QStringLiteral("a8-work-area-bar-light.png"), 24);

        // Sequence > Clear Work Area, as the menu runs it.
        REQUIRE(controller->runCommand(QStringLiteral("timeline.clearWorkArea")));
        spin(100);
        CHECK_FALSE(controller->workArea().has_value());
        CHECK_FALSE(bar->isVisible());
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("The Export drawer's Work Area range is there once the sequence has a work area (card 024)", "[app][workarea][export][qml][a8]") {
    resetSettings();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    QQmlComponent component(&shell.engine, "Wabcut", "ExportSettingsDrawer");
    INFO(component.errorString().toStdString());
    for (const bool available : {false, true}) {
        INFO(available);
        QVariantMap summary{{QStringLiteral("rangeAvailable"),
                             QVariantMap{{QStringLiteral("whole"), true}, {QStringLiteral("inOut"), false},
                                         {QStringLiteral("workArea"), available}, {QStringLiteral("custom"), true}}}};
        QObject* made = component.createWithInitialProperties(
            {{QStringLiteral("open"), true}, {QStringLiteral("summary"), summary}, {QStringLiteral("width"), 600}});
        auto* drawer = qobject_cast<QQuickItem*>(made);
        REQUIRE(drawer);
        drawer->setParentItem(shell.window->contentItem());
        spin(100);
        QQuickItem* tabs = findNamed(drawer, QStringLiteral("exportRangeTabs"));
        REQUIRE(tabs);
        bool enabled = !available;
        REQUIRE(QMetaObject::invokeMethod(tabs, "segmentEnabled", Q_RETURN_ARG(bool, enabled), Q_ARG(int, 2)));
        CHECK(enabled == available); // Entire Source, Source In/Out, Work Area, Custom
        bool whole = false;
        REQUIRE(QMetaObject::invokeMethod(tabs, "segmentEnabled", Q_RETURN_ARG(bool, whole), Q_ARG(int, 0)));
        CHECK(whole);
        delete drawer;
    }
}

// =====================================================================================================================
// 069: Render and Replace
// =====================================================================================================================

namespace {

/// A sequence of one real clip (picture and sound) nested into a nest, its head trimmed by `trimFrames` (30 fps).
struct RealNest {
    ProjectState state;
    SequenceId seq;
    SequenceId nested;
    ClipId video;
    ClipId audio;
};

RealNest realNest(std::int64_t trimFrames) {
    auto item = editor::describeMediaFile(testMedia("GX010042.MP4"));
    REQUIRE(item);
    auto added = edit::addMedia(ProjectState{}, *item);
    REQUIRE(added);
    edit::SequenceSettings settings;
    settings.name = "Main";
    settings.frameRate = Rate{30, 1};
    settings.width = 640;
    settings.height = 360;
    settings.videoTracks = 1;
    settings.audioTracks = 1;
    auto seq = edit::addSequence(added->state, settings);
    REQUIRE(seq);
    const Sequence& sequence = *seq->state.findSequence(seq->id);
    const MediaItem& media = *seq->state.findMedia(added->id);
    edit::SourceClip source;
    source.media = added->id;
    source.sourceRange = TimeRange{Ticks{}, media.sourceExtent(StreamRef{StreamKind::Video, 0}).value_or(item->info.duration)};
    source.patches = edit::defaultSourcePatches(sequence, media);
    auto placed = edit::overwriteEdit(seq->state, seq->id, source, Ticks{});
    REQUIRE(placed);
    const std::vector<ClipId> ids(placed->placed.begin(), placed->placed.end());
    auto nest = edit::nestClips(placed->state, seq->id, ids, "Heavy nest");
    REQUIRE(nest);
    RealNest out{nest->edit.state, seq->id, nest->nested, nest->clips.at(0), nest->clips.size() > 1 ? nest->clips[1] : ClipId{}};
    if (trimFrames > 0) {
        out.state = ok(edit::trimEdge(out.state, seq->id, out.video, edit::Edge::Head, framesToTicks(trimFrames, Rate{30, 1}))).state;
    }
    return out;
}

/// The bridge, a controller and an export queue that draws synthetic pictures (the sound is the real clip's).
struct RenderRig {
    ProjectController controller;
    ExportController queue;
    RenderReplaceBridge bridge;
    LibraryBridge library;
    QTemporaryDir folder;
    QStringList notices;
    RealNest nest;

    explicit RenderRig(std::int64_t trimFrames) : nest(realNest(trimFrames)) {
        REQUIRE(folder.isValid());
        controller.resetProject(nest.state, nest.seq);
        queue.setProject(&controller);
        queue.setVideoSourceFactory([](const ProjectState&, const Sequence&, const exporter::VideoSourceFormat& format)
                                        -> std::expected<std::unique_ptr<exporter::IVideoFrameSource>, Error> {
            return exporter::makeSyntheticVideoSource(format);
        });
        bridge.setController(&controller);
        bridge.setExport(&queue);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setController(&controller);
        QObject::connect(&bridge, &RenderReplaceBridge::notice, [this](const QString& text, bool warning) {
            notices << (warning ? QStringLiteral("! ") : QStringLiteral("")) + text;
        });
        select();
    }
    void select() { controller.setSelection({nest.video, nest.audio}, nest.video); }
    const Clip& clip(ClipId id) const { return controller.currentSequence()->findClip(id)->clip(); }
    std::size_t steps() { return controller.session().undoCount(); }
    QString path() const { return QDir::toNativeSeparators(folder.path()); }
};

} // namespace

// ---- the model's ids in the palette / menus ----

TEST_CASE("Render and Replace says why it cannot run with nothing selected or nothing nested (card 069)",
          "[app][render-replace][a8]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    RenderReplaceBridge bridge;
    bridge.setController(&controller);
    const auto state = [&](const char* id) { return bridge.commandAvailability(QString::fromLatin1(id)); };
    controller.clearSelection();
    CHECK_FALSE(state("clip.renderAndReplace").value(QStringLiteral("enabled")).toBool());
    CHECK(state("clip.renderAndReplace").value(QStringLiteral("reason")).toString() == QStringLiteral("Select a clip first"));
    controller.setSelection({tl.clip("V1@0")}, tl.clip("V1@0"));
    CHECK_FALSE(state("clip.renderAndReplace").value(QStringLiteral("enabled")).toBool());
    CHECK(state("clip.renderAndReplace").value(QStringLiteral("reason")).toString().contains(QStringLiteral("nested sequence")));
    CHECK_FALSE(state("clip.restoreUnrendered").value(QStringLiteral("enabled")).toBool());
    CHECK(state("clip.restoreUnrendered").value(QStringLiteral("reason")).toString().contains(QStringLiteral("rendered and replaced")));
    CHECK_FALSE(bridge.inspect(1.0).value(QStringLiteral("ok")).toBool());

    // A nest: ready, and described.
    const edit::NestResult nest = [&] {
        std::vector<ClipId> all{tl.clip("V1@0"), tl.clip("V1@4"), tl.clip("A1@0"), tl.clip("A1@4")};
        auto r = edit::nestClips(tl.state, tl.seq, all, "Nest");
        REQUIRE(r);
        return *r;
    }();
    controller.resetProject(nest.edit.state, tl.seq);
    controller.setSelection({nest.clips[0]}, nest.clips[0]);
    CHECK(state("clip.renderAndReplace").value(QStringLiteral("enabled")).toBool());
    const QVariantMap info = bridge.inspect(0.0);
    REQUIRE(info.value(QStringLiteral("ok")).toBool());
    const QVariantList units = info.value(QStringLiteral("units")).toList();
    REQUIRE(units.size() == 1);
    CHECK(units.front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Nest"));
    CHECK(units.front().toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("Picture and sound"));
    CHECK(units.front().toMap().value(QStringLiteral("clips")).toInt() == 2);
    // Neither command reaches the palette as "Not available yet".
    CHECK(KeymapCommandProvider::viewHandles(QStringLiteral("clip.renderAndReplace")));
    CHECK(bridge.presets().size() == 4);
}

TEST_CASE("Render and Replace renders the nest through the export queue and the clips play the file in one undo step (card 069)",
          "[app][render-replace][export][a8]") {
    if (!haveMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    resetSettings();
    RenderRig rig(12); // the clip plays the nest from 0.4 s; with 0.2 s of handles the file starts at 0.2 s
    const Clip before = rig.clip(rig.nest.video);
    const Clip audioBefore = rig.clip(rig.nest.audio);
    REQUIRE(before.isNested());
    const std::size_t steps = rig.steps();

    REQUIRE(rig.bridge.start(QStringLiteral("prores-422-hq"), 0.2, rig.path()));
    CHECK(rig.bridge.running());
    CHECK(rig.bridge.pendingCount() == 1);
    CHECK(rig.bridge.status().contains(QStringLiteral("Rendering Heavy nest")));
    // Starting again for the same clips is refused while it renders.
    CHECK_FALSE(rig.bridge.start(QStringLiteral("prores-422-hq"), 0.2, rig.path()));
    REQUIRE(rig.notices.size() == 1);
    CHECK(rig.notices.front().startsWith(QStringLiteral("!"))); // it said why
    rig.notices.clear();
    CHECK(rig.steps() == steps); // nothing is replaced yet
    CHECK(rig.clip(rig.nest.video).isNested());

    REQUIRE(waitFor([&] { return rig.bridge.pendingCount() == 0; }, 120000));
    INFO(rig.bridge.status().toStdString());
    const std::filesystem::path made = std::filesystem::path(rig.folder.path().toStdWString()) / "Heavy nest (rendered).mov";
    CHECK(std::filesystem::is_regular_file(made));
    CHECK(rig.steps() == steps + 1);
    CHECK(rig.controller.undoText().contains(QStringLiteral("Render and Replace")));
    for (const ClipId id : {rig.nest.video, rig.nest.audio}) {
        const Clip& c = rig.clip(id);
        INFO(id.value);
        CHECK_FALSE(c.isNested());
        REQUIRE(c.renderedNest() != nullptr);
        CHECK(c.renderedNest()->sequence == rig.nest.nested);
        CHECK(c.renderedNest()->origin == framesToTicks(6, Rate{30, 1})); // 0.4 s - 0.2 s of handles
        const Clip& was = id == rig.nest.video ? before : audioBefore;
        CHECK(c.timelineRange == was.timelineRange);
        CHECK(c.linkGroup == was.linkGroup);
        CHECK(c.sourceIn == was.sourceIn - c.renderedNest()->origin);
        const MediaItem* media = rig.controller.snapshot()->findMedia(c.media);
        REQUIRE(media);
        CHECK(media->name == "Heavy nest (rendered)");
        CHECK(media->info.hasVideo());
        CHECK(media->info.hasAudio());
        CHECK(media->info.video.front().codec == "prores");
    }
    CHECK(rig.notices.size() == 1);
    CHECK_FALSE(rig.notices.front().startsWith(QStringLiteral("!")));
    CHECK_FALSE(rig.bridge.running());
    // The Library still counts the nest as used while a rendered clip can be restored to it (Remove Unused keeps it).
    const auto nestUse = rig.library.sequenceUsage().find(rig.nest.nested.value);
    REQUIRE(nestUse != rig.library.sequenceUsage().end());
    CHECK(nestUse->second.uses > 0);

    // Undo is one step and brings the nest back as it was; redo replaces again.
    REQUIRE(rig.controller.undo());
    CHECK(rig.clip(rig.nest.video) == before);
    CHECK(rig.clip(rig.nest.audio) == audioBefore);
    REQUIRE(rig.controller.redo());
    CHECK_FALSE(rig.clip(rig.nest.video).isNested());

    // Restore Unrendered: the nest plays again, exactly as it did, in one step; the file stays in the project.
    rig.select();
    CHECK(rig.bridge.commandAvailability(QStringLiteral("clip.restoreUnrendered")).value(QStringLiteral("enabled")).toBool());
    const std::size_t media = rig.controller.snapshot()->media.size();
    const std::size_t beforeRestore = rig.steps();
    REQUIRE(rig.bridge.restore());
    CHECK(rig.steps() == beforeRestore + 1);
    CHECK(rig.clip(rig.nest.video) == before);
    CHECK(rig.clip(rig.nest.audio) == audioBefore);
    CHECK(rig.controller.snapshot()->media.size() == media);
    CHECK(rig.controller.undoText().contains(QStringLiteral("Restore Unrendered")));
    // Nothing is left to restore.
    CHECK_FALSE(rig.bridge.commandAvailability(QStringLiteral("clip.restoreUnrendered")).value(QStringLiteral("enabled")).toBool());
}

TEST_CASE("Render and Replace stops with Cancel and leaves the clips on the nest (card 069)", "[app][render-replace][export][a8]") {
    if (!haveMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    resetSettings();
    RenderRig rig(0);
    const Clip before = rig.clip(rig.nest.video);
    const std::size_t steps = rig.steps();
    REQUIRE(rig.bridge.start(QStringLiteral("prores-422-hq"), 0.0, rig.path()));
    rig.bridge.cancel();
    REQUIRE(waitFor([&] { return rig.bridge.pendingCount() == 0; }, 120000));
    CHECK(rig.steps() == steps);
    CHECK(rig.clip(rig.nest.video) == before);
    REQUIRE_FALSE(rig.notices.isEmpty());
    CHECK(rig.notices.back().startsWith(QStringLiteral("!")));
    CHECK(rig.bridge.status().contains(QStringLiteral("its clips still play the nest")));
    CHECK_FALSE(rig.bridge.running());
}

TEST_CASE("Render and Replace leaves the clips alone when the nest was edited while it rendered (card 069)",
          "[app][render-replace][export][a8]") {
    if (!haveMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    resetSettings();
    RenderRig rig(0);
    REQUIRE(rig.bridge.start(QStringLiteral("prores-422-hq"), 0.0, rig.path()));
    // An edit inside the nest: the file would show the old one.
    const SequenceId nested = rig.nest.nested;
    Marker marker;
    marker.start = framesToTicks(3, Rate{30, 1});
    marker.name = "late";
    REQUIRE(rig.controller.applyEditFn("Add Marker", [&](const ProjectState& s) {
        return edit::setSequenceMarkers(s, nested, std::vector<Marker>{marker});
    }));
    const std::size_t steps = rig.steps();
    REQUIRE(waitFor([&] { return rig.bridge.pendingCount() == 0; }, 120000));
    CHECK(rig.steps() == steps);
    CHECK(rig.clip(rig.nest.video).isNested());
    REQUIRE_FALSE(rig.notices.isEmpty());
    CHECK(rig.notices.back().startsWith(QStringLiteral("!")));
    CHECK(rig.notices.back().contains(QStringLiteral("changed while it was rendering")));
}

TEST_CASE("Render and Replace dialog lists the nests and formats and handles and folder in dark and light (card 069)",
          "[app][render-replace][qml][screenshot][a8]") {
    if (!haveMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    for (const bool dark : {true, false}) {
        INFO((dark ? "dark" : "light"));
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        const RealNest nest = realNest(12);
        controller->resetProject(nest.state, nest.seq);
        controller->setSelection({nest.video, nest.audio}, nest.video);

        // The Clip menu's row is ready and the palette lists it as ready.
        REQUIRE(runCommand(shell, "clip.renderAndReplace"));
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = shell.window->findChild<QObject*>(QStringLiteral("renderReplaceDialog"));
            return dialog && dialog->property("opened").toBool();
        }, 8000));
        spin(200);
        auto* unit = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("renderReplaceUnit_0")));
        REQUIRE(unit);
        CHECK(unit->isVisible());
        auto* defaultFormat = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("renderReplacePreset_prores-422-hq")));
        REQUIRE(defaultFormat);
        CHECK(defaultFormat->property("selected").toBool()); // the default format
        auto* start = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("renderReplaceStart")));
        REQUIRE(start);
        CHECK(start->isEnabled());
        auto* handles = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("renderReplaceHandles")));
        REQUIRE(handles);
        CHECK(handles->property("value").toDouble() == 1.0);
        auto* warning = qobject_cast<QQuickItem*>(findObject(dialog, QStringLiteral("renderReplaceAlphaWarning")));
        REQUIRE(warning);
        CHECK_FALSE(warning->isVisible()); // a clip covers the whole nest: no transparency
        // Narrow it: nothing runs off the card.
        auto* card = dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(card);
        const QRectF cardRect = card->parentItem()->mapRectToScene(QRectF(0, 0, card->parentItem()->width(), card->parentItem()->height()));
        CHECK(cardRect.left() >= 0);
        CHECK(cardRect.right() <= shell.window->width());
        CHECK(cardRect.top() >= 0);
        CHECK(cardRect.bottom() <= shell.window->height());

        const QImage shot = shell.grab();
        REQUIRE_FALSE(shot.isNull());
        saveCrop(shot, card->parentItem(), dark ? QStringLiteral("a8-render-replace-dialog.png") : QStringLiteral("a8-render-replace-dialog-light.png"));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}
