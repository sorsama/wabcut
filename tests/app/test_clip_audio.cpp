// M5l: the clip menu's audio tools on fake local models, offscreen; no window is shown.
//   * Enhance Speech… (A.4): audio.enhanceSpeech with progress, the take rendered once per media stream (cached), the
//     clip badges ("enhanced", "missing render"), Strength (one undo step per drag) and the bypass;
//   * Separate Stems… (A.4): the stems become media items separated from their source (the Library says so);
//   * Synchronize (A.5): the video and the external audio → audio.syncClips {apply, link}, with the report;
//   * the menu entries in the shell open the dialogs and run the commands.
// Screenshot: the Enhance Speech dialog.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "ClipAudioBridge.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioSync.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::audio::edit;

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "app-clip-audio" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// Writes a take file per render (the real one writes a WAV) and reports progress; counts the renders.
class FakeEnhancer final : public SpeechEnhancer {
public:
    explicit FakeEnhancer(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    std::string modelVersion() const override { return "fake-dfn@1"; }
    std::expected<Take, Error> render(const MediaItem& item, int stream, std::stop_token stop,
                                      const std::function<void(double)>& progress) override {
        ++renders;
        for (const double p : {0.25, 0.5, 1.0}) {
            if (progress)
                progress(p);
            if (stop.stop_requested())
                return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        Take t;
        const auto file = m_dir / (item.name + "-a" + std::to_string(stream) + ".wav");
        std::ofstream(file) << "take";
        t.path = pathToUtf8(file);
        t.model = modelVersion();
        t.seconds = 10.0;
        t.elapsed = 1.0;
        t.device = "FakeCPU";
        return t;
    }
    int renders = 0;

private:
    std::filesystem::path m_dir;
};

class FakeSeparator final : public StemSeparator {
public:
    explicit FakeSeparator(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    std::string modelVersion() const override { return "fake-demucs@1"; }
    std::expected<Result, Error> separate(const MediaItem& item, int, std::stop_token, const std::function<void(double)>& progress) override {
        ++calls;
        if (progress)
            progress(0.5);
        Result r;
        r.model = modelVersion();
        r.seconds = 10.0;
        r.elapsed = 4.0;
        r.device = "FakeCPU";
        for (const char* part : {"drums", "bass", "other", "vocals"}) {
            Stem s;
            s.part = part;
            s.path = pathToUtf8(m_dir / (item.name + "-" + part + ".wav"));
            s.info.path = s.path;
            s.info.duration = item.info.duration;
            AudioStreamInfo a;
            a.index = 0;
            a.codec = "pcm_f32le";
            a.sampleRate = Rate{44100, 1};
            a.channels = 2;
            a.duration = item.info.duration;
            s.info.audio.push_back(a);
            s.fingerprint.size = 1234;
            s.fingerprint.sampleHash = "00112233445566ff";
            r.stems.push_back(std::move(s));
        }
        return r;
    }
    int calls = 0;

private:
    std::filesystem::path m_dir;
};

/// A.5 stand-in: each media item starts at a fixed time on the reference's clock, with a fixed confidence.
class FixedSync final : public AudioSyncService {
public:
    std::map<std::uint64_t, double> startSeconds;
    std::map<std::uint64_t, double> confidence;
    int calls = 0;

    std::expected<Result, Error> sync(const ProjectState&, std::span<const Angle> angles, std::size_t reference,
                                      const SyncOptions& options, std::stop_token, const std::function<void(double)>& progress) override {
        ++calls;
        Result r;
        r.report.reference = reference;
        const double ref = startSeconds[angles[reference].media.front().value];
        for (const Angle& a : angles) {
            const std::uint64_t m = a.media.front().value;
            const double at = startSeconds[m] - ref;
            SyncMatch match;
            match.synced = true;
            match.offset = std::llround(at * options.refineRate);
            match.offsetAtStart = match.offset;
            match.confidence = confidence.contains(m) ? confidence[m] : 1.0;
            match.overlapSeconds = 3.0;
            match.windows = 4;
            r.report.matches.push_back(match);
            r.starts.push_back({seconds(at)});
            r.order.push_back(a.media);
        }
        if (progress)
            progress(1.0);
        return r;
    }
};

/// The camera (media A: V1 and its sound on A1, linked) and the recorder (media R on A2, out of sync at 0).
constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa..........|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa..........|
    A2     |Rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr....................|
)";

/// The timeline with the recorder's media sound-only (the fixture gives every media a picture): dual-system sound.
Timeline recorderTimeline() {
    Timeline tl(kPicture);
    tl.link("V1@0 A1@0");
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media('R')) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->info.video.clear();
            m = copy;
        }
    }
    return tl;
}

struct Tools {
    std::filesystem::path dir;
    std::shared_ptr<FakeEnhancer> enhancer;
    std::shared_ptr<FakeSeparator> separator;
    std::shared_ptr<FixedSync> sync;
    std::shared_ptr<AudioServices> services;

    explicit Tools(const std::string& name)
        : dir(scratch(name)), enhancer(std::make_shared<FakeEnhancer>(dir)), separator(std::make_shared<FakeSeparator>(dir)),
          sync(std::make_shared<FixedSync>()), services(std::make_shared<AudioServices>(nullptr, nullptr)) {
        services->enhancer = enhancer;
        services->separator = separator;
        services->sync = sync;
    }
    void attach(ClipAudioBridge& bridge) const {
        bridge.setServices(services);
        bridge.setSynchronous(true);
        bridge.setModelsReadyForTests(true);
    }
};

struct Fixture {
    Timeline tl = recorderTimeline();
    Tools tools;
    ProjectController controller;
    ClipAudioBridge audio;

    explicit Fixture(const std::string& name) : tools(name) {
        controller.resetProject(tl.state, tl.seq);
        audio.setController(&controller);
        tools.attach(audio);
    }
    QString key(std::string_view ref) const { return clipKey(tl.clip(ref)); }
    std::size_t undo() { return controller.session().undoCount(); }
    QVariantMap info(std::string_view ref) const { return audio.clipInfo(key(ref)); }
};

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

QObject* openDialog(Shell& shell, const char* loaderName) {
    QObject* loader = shell.object(loaderName);
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    return dialog;
}

QQuickItem* inDialog(QObject* dialog, const QString& name) {
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    return findNamed(content->parentItem(), name);
}

void openMenu(QObject* menu, QQuickItem* target, const QString& clip) {
    REQUIRE(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(target)),
                                      Q_ARG(QVariant, clip), Q_ARG(QVariant, QPointF(200, 20))));
}

QObject* menuItem(QObject* menu, const char* name) {
    QObject* item = menu->findChild<QObject*>(QString::fromLatin1(name));
    REQUIRE(item);
    return item;
}

} // namespace

TEST_CASE("Enhance Speech: one render per media stream (cached), the badges, Strength and the bypass",
          "[app][audio][enhance]") {
    Fixture f("enhance");
    ClipAudioBridge& a = f.audio;
    CHECK(a.models().value(QStringLiteral("speechReady")).toBool());
    QVariantMap v1 = f.info("V1@0");
    CHECK(v1.value(QStringLiteral("audio")).toBool()); // the video clip stands for its sound
    CHECK(v1.value(QStringLiteral("video")).toBool());
    CHECK_FALSE(v1.value(QStringLiteral("enhanced")).toBool());
    CHECK(v1.value(QStringLiteral("badge")).toString().isEmpty());

    // Progress is reported while it renders (synchronous here: the last values stay).
    std::vector<double> progress;
    QObject::connect(&a, &ClipAudioBridge::jobChanged, [&] {
        if (a.busy() == QStringLiteral("enhance"))
            progress.push_back(a.progress());
    });
    const auto before = f.undo();
    REQUIRE(a.enhanceSpeech({f.key("V1@0")}, 80).isEmpty());
    CHECK(f.undo() == before + 1);
    CHECK(f.tools.enhancer->renders == 1);
    CHECK(a.busy().isEmpty());
    CHECK_FALSE(progress.empty());
    CHECK(a.enhanceReport().value(QStringLiteral("ok")).toBool());
    QVariantMap a1 = f.info("A1@0");
    CHECK(a1.value(QStringLiteral("enhanced")).toBool());
    CHECK(a1.value(QStringLiteral("strength")).toDouble() == 80.0);
    CHECK(a1.value(QStringLiteral("take")).toString() == QStringLiteral("ready"));
    CHECK(a1.value(QStringLiteral("badge")).toString() == QStringLiteral("enhanced"));
    {
        const SequencePtr seq = f.controller.currentSequence();
        CHECK(enhanceBadge(*f.controller.snapshot(), seq->findClip(f.tl.clip("A1@0"))->clip()) == QStringLiteral("enhanced"));
        CHECK(enhanceBadge(*f.controller.snapshot(), seq->findClip(f.tl.clip("A2@0"))->clip()).isEmpty());
    }

    // Again on the same media: the cached take, nothing rendered.
    REQUIRE(a.enhanceSpeech({f.key("A1@0")}, 80).isEmpty());
    CHECK(f.tools.enhancer->renders == 1);

    // Strength: a drag is one undo step.
    const auto beforeDrag = f.undo();
    for (const double s : {70.0, 60.0, 50.0})
        CHECK(a.setStrength(f.key("A1@0"), s).isEmpty());
    a.endStrength();
    CHECK(f.undo() == beforeDrag + 1);
    CHECK(f.info("A1@0").value(QStringLiteral("strength")).toDouble() == 50.0);
    // The bypass: the effect off (no badge), then on again.
    CHECK(a.setBypassed(f.key("A1@0"), true).isEmpty());
    CHECK(f.info("A1@0").value(QStringLiteral("bypassed")).toBool());
    CHECK(f.info("A1@0").value(QStringLiteral("badge")).toString().isEmpty());
    CHECK(a.setBypassed(f.key("A1@0"), false).isEmpty());
    CHECK(f.info("A1@0").value(QStringLiteral("badge")).toString() == QStringLiteral("enhanced"));

    // The effect without a render waits for one: "missing render"; Render Enhancement renders it.
    REQUIRE(a.enhanceSpeech({f.key("A2@0")}, 100, false).isEmpty());
    CHECK(f.tools.enhancer->renders == 1);
    CHECK(f.info("A2@0").value(QStringLiteral("badge")).toString() == QStringLiteral("missingRender"));
    CHECK(f.info("A2@0").value(QStringLiteral("take")).toString() == QStringLiteral("missing"));
    REQUIRE(a.enhanceSpeech({f.key("A2@0")}, 100, true).isEmpty());
    CHECK(f.tools.enhancer->renders == 2);
    CHECK(f.info("A2@0").value(QStringLiteral("badge")).toString() == QStringLiteral("enhanced"));

    // A take file that went away: missing render again.
    for (const auto& entry : std::filesystem::directory_iterator(f.tools.dir)) {
        if (entry.path().filename().string().rfind("R-", 0) == 0)
            std::filesystem::remove(entry.path());
    }
    CHECK(f.info("A2@0").value(QStringLiteral("badge")).toString() == QStringLiteral("missingRender"));
    CHECK_FALSE(a.enhanceSpeech({}, 100).isEmpty());
}

TEST_CASE("Enhance badges in the timeline: ENHANCED and NOT RENDERED micro-tags", "[app][audio][enhance][timeline]") {
    Rig rig(kPicture, 8.0, false);
    Tools tools("badges");
    ClipAudioBridge audio;
    audio.setController(&rig.controller);
    tools.attach(audio);
    REQUIRE(audio.enhanceSpeech({clipKey(rig.clip("A1@0"))}, 100, true).isEmpty());
    REQUIRE(audio.enhanceSpeech({clipKey(rig.clip("A2@0"))}, 100, false).isEmpty());
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        rig.harness.frame();
    }
    const TimelineStats& stats = rig.view->stats();
    CHECK(stats.badgeTexts.value(rig.clip("A1@0").value) == QStringLiteral("ENHANCED"));
    CHECK(stats.badgeTexts.value(rig.clip("A2@0").value) == QStringLiteral("NOT RENDERED"));
    CHECK_FALSE(stats.badgeTexts.contains(rig.clip("V1@0").value));
}

TEST_CASE("Enhance badges follow an edit that moves their clips and its undo (p02 integration)",
          "[app][audio][enhance][timeline]") {
    // The timeline's cached lane facts follow each clip an edit modifies (one walk over the clips per edit, not a
    // lookup per clip): the badges stay on the moved clips.
    Rig rig(kPicture, 8.0, false);
    Tools tools("badges-follow");
    ClipAudioBridge audio;
    audio.setController(&rig.controller);
    tools.attach(audio);
    const ClipId a1 = rig.clip("A1@0");
    const ClipId a2 = rig.clip("A2@0");
    REQUIRE(audio.enhanceSpeech({clipKey(a1)}, 100, true).isEmpty());
    REQUIRE(audio.enhanceSpeech({clipKey(a2)}, 100, false).isEmpty());
    const auto settle = [&] {
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
            rig.harness.frame();
        }
    };
    settle();
    const std::vector<ClipId> both{a1, a2};
    REQUIRE(rig.controller.commitGesture("Move", [&](const ProjectState& s) {
        return edit::moveClips(s, rig.tl.seq, both, f(2), {}, edit::MoveMode::Overwrite, edit::LinkMode::IgnoreLinks);
    }));
    settle();
    CHECK(rig.clip("A1@2") == a1);
    CHECK(rig.clip("A2@2") == a2);
    CHECK(rig.view->stats().badgeTexts.value(a1.value) == QStringLiteral("ENHANCED"));
    CHECK(rig.view->stats().badgeTexts.value(a2.value) == QStringLiteral("NOT RENDERED"));
    REQUIRE(rig.controller.undo());
    settle();
    CHECK(rig.view->stats().badgeTexts.value(a1.value) == QStringLiteral("ENHANCED"));
    CHECK(rig.view->stats().badgeTexts.value(a2.value) == QStringLiteral("NOT RENDERED"));
}

TEST_CASE("Separate Stems: four stems imported as media separated from their source, one undo step",
          "[app][audio][stems]") {
    Fixture f("stems");
    LibraryBridge library;
    library.setController(&f.controller);
    const std::size_t mediaBefore = f.controller.snapshot()->media.size();
    const auto before = f.undo();
    REQUIRE(f.audio.separateStems(f.key("A2@0")).isEmpty());
    INFO(f.audio.message().toStdString());
    CHECK(f.undo() == before + 1);
    CHECK(f.tools.separator->calls == 1);
    const ProjectSnapshot snap = f.controller.snapshot();
    REQUIRE(snap->media.size() == mediaBefore + 4);
    int stems = 0;
    for (const MediaPtr& m : snap->media) {
        if (m->derived) {
            ++stems;
            CHECK(m->derived->source == f.tl.media('R'));
        }
    }
    CHECK(stems == 4);
    CHECK(f.audio.message().contains(QStringLiteral("4")));
    CHECK_FALSE(f.audio.separateStems(QStringLiteral("junk")).isEmpty());
}

// Review wave 5, A5-P2-20. The bridge made the app's audio services once, for the project file open at its first job, and
// kept them: Enhance Speech, stems and remix takes of project B then went into A.wabcut-media/. Fails today: the job on B
// reuses the services made for A (the factory is not called again).
TEST_CASE("Clip audio services are made again when another project file is open", "[app][audio][enhance]") {
    Fixture f("services-per-project");
    std::vector<std::string> builtFor;
    f.audio.setServices(nullptr);
    f.audio.setServiceFactory([&f, &builtFor]() -> std::shared_ptr<command::CommandServices> {
        builtFor.push_back(pathToUtf8(f.controller.projectFile()));
        return f.tools.services;
    });
    const std::filesystem::path a = f.tools.dir / "A.wabcut";
    const std::filesystem::path b = f.tools.dir / "B.wabcut";
    f.controller.setProjectFile(a);
    REQUIRE(f.audio.enhanceSpeech({f.key("V1@0")}, 80).isEmpty());
    REQUIRE(f.audio.enhanceSpeech({f.key("A2@0")}, 80).isEmpty());
    CHECK(builtFor == std::vector<std::string>{pathToUtf8(a)}); // the same project: made once
    f.controller.setProjectFile(b);
    REQUIRE(f.audio.enhanceSpeech({f.key("A1@0")}, 80).isEmpty());
    CHECK(builtFor == std::vector<std::string>{pathToUtf8(a), pathToUtf8(b)});
}

TEST_CASE("Synchronize: the video clip and the external audio by their sound, moved and linked; the report",
          "[app][audio][sync]") {
    Fixture f("sync");
    f.tools.sync->startSeconds = {{f.tl.media('A').value, 0.0}, {f.tl.media('R').value, 0.4}};
    f.tools.sync->confidence = {{f.tl.media('R').value, 0.91}};
    CHECK_FALSE(f.audio.synchronize({f.key("V1@0")}).isEmpty()); // two clips at least
    const auto before = f.undo();
    REQUIRE(f.audio.synchronize({f.key("V1@0"), f.key("A2@0")}).isEmpty());
    const QVariantMap report = f.audio.syncReport();
    INFO(report.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    CHECK(f.undo() == before + 1);
    CHECK(f.tools.sync->calls == 1);
    const QVariantList rows = report.value(QStringLiteral("rows")).toList();
    REQUIRE(rows.size() == 2);
    CHECK(report.value(QStringLiteral("reference")).toString() == f.key("V1@0"));
    const QVariantMap recorder = rows[1].toMap();
    CHECK(recorder.value(QStringLiteral("synced")).toBool());
    CHECK(recorder.value(QStringLiteral("confidence")).toString() == QStringLiteral("91 %"));
    CHECK(recorder.value(QStringLiteral("offset")).toString().startsWith(QStringLiteral("+")));
    // The recorder's clip moved to 0.4 s and is linked to the camera's.
    const SequencePtr seq = f.controller.currentSequence();
    const auto& a2 = seq->audioTracks[1]->clips;
    REQUIRE(a2.size() == 1);
    CHECK(a2[0].start() == seconds(0.4));
    const Clip& camera = seq->videoTracks[0]->clips[0];
    CHECK(camera.linkGroup.isValid());
    CHECK(a2[0].linkGroup == camera.linkGroup);
}

TEST_CASE("Clip menu audio tools in the shell: Enhance Speech, Separate Stems, Synchronize; screenshot",
          "[app][audio][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* audio = shell.engine.singletonInstance<ClipAudioBridge*>("Wabcut", "ClipAudio");
    REQUIRE(controller);
    REQUIRE(audio);
    Tools tools("shell");
    tools.attach(*audio);
    Timeline tl = recorderTimeline();
    tools.sync->startSeconds = {{tl.media('A').value, 0.0}, {tl.media('R').value, 0.4}};
    tools.sync->confidence = {{tl.media('R').value, 0.96}};
    controller->resetProject(tl.state, tl.seq);
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());
    QObject* menu = w->findChild<QObject*>(QStringLiteral("clipContextMenu"));
    REQUIRE(menu);
    QQuickItem* lanes = findNamed(w->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);

    // One audio clip selected: Enhance Speech… and Separate Stems… are offered; Synchronize needs two clips.
    controller->setSelection({tl.clip("A1@0")});
    openMenu(menu, lanes, clipKey(tl.clip("A1@0")));
    CHECK(menuItem(menu, "enhanceSpeechMenuItem")->property("visible").toBool());
    CHECK(menuItem(menu, "separateStemsMenuItem")->property("visible").toBool());
    CHECK_FALSE(menuItem(menu, "renderEnhancementMenuItem")->property("visible").toBool());
    CHECK_FALSE(menuItem(menu, "synchronizeMenuItem")->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(menuItem(menu, "enhanceSpeechMenuItem"), "triggered"));
    QMetaObject::invokeMethod(menu, "close");

    // The dialog: Enhance renders the take, the badge says it is ready; Strength and the bypass are live.
    QObject* enhance = openDialog(shell, "enhanceSpeechDialogLoader");
    CHECK(enhance->property("clips").toStringList() == QStringList{clipKey(tl.clip("A1@0"))});
    QQuickItem* button = inDialog(enhance, QStringLiteral("enhanceButton"));
    REQUIRE(button);
    const auto before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
    REQUIRE(waitFor([&] { return enhance->property("enhanced").toBool(); }, 5000));
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(tools.enhancer->renders == 1);
    QQuickItem* badge = inDialog(enhance, QStringLiteral("enhanceTakeBadge"));
    REQUIRE(badge);
    CHECK(badge->isVisible());
    QQuickItem* bypass = inDialog(enhance, QStringLiteral("enhanceBypass"));
    REQUIRE(bypass);
    CHECK(bypass->isVisible());
    spin(150);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("enhance-speech-dialog.png"))));
    }
    QMetaObject::invokeMethod(enhance, "close");
    spin(100);

    // The enhanced clip's menu: no Render Enhancement (its take is ready).
    openMenu(menu, lanes, clipKey(tl.clip("A1@0")));
    CHECK_FALSE(menuItem(menu, "renderEnhancementMenuItem")->property("visible").toBool());
    // Separate Stems… opens its dialog; Separate runs the job.
    REQUIRE(QMetaObject::invokeMethod(menuItem(menu, "separateStemsMenuItem"), "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    QObject* stems = openDialog(shell, "stemsDialogLoader");
    QQuickItem* separate = inDialog(stems, QStringLiteral("stemsButton"));
    REQUIRE(separate);
    const std::size_t mediaBefore = controller->snapshot()->media.size();
    REQUIRE(QMetaObject::invokeMethod(separate, "clicked"));
    REQUIRE(waitFor([&] { return controller->snapshot()->media.size() == mediaBefore + 4; }, 5000));
    CHECK(tools.separator->calls == 1);
    QMetaObject::invokeMethod(stems, "close");
    spin(100);

    // Synchronize: the camera and the recorder selected; the report dialog opens.
    controller->setSelection({tl.clip("V1@0"), tl.clip("A2@0")});
    openMenu(menu, lanes, clipKey(tl.clip("A2@0")));
    QObject* syncItem = menuItem(menu, "synchronizeMenuItem");
    CHECK(syncItem->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(syncItem, "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    QObject* report = openDialog(shell, "syncReportDialogLoader");
    CHECK(audio->syncReport().value(QStringLiteral("ok")).toBool());
    CHECK(controller->currentSequence()->audioTracks[1]->clips[0].start() == seconds(0.4));
    QQuickItem* rows = inDialog(report, QStringLiteral("syncReportRows"));
    REQUIRE(rows);
    CHECK(rows->isVisible());
    QMetaObject::invokeMethod(report, "close");
    spin(100);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- 0.4: Increase / Decrease Clip Volume (] [, Shift: Many) ------------------------------------------------------

namespace {

constexpr std::string_view kVolumePicture = R"(
    V2     |....Cccc............|
    V1 tgt |Aaaaaaaaaa..........|
    A1 tgt |Aaaaaaaaaa..........|
    A2     |..Mmmmmm............|
)";

struct VolumeRig {
    Timeline tl{kVolumePicture};
    ProjectController controller;
    ClipAudioBridge audio;

    VolumeRig() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        audio.setController(&controller);
    }
    /// The clip's Volume Level (Effect Controls > Volume > Level).
    ParamValue level(std::string_view ref) const {
        const ClipId id = clipAt(*controller.snapshot(), tl.seq, ref);
        const auto loc = controller.currentSequence()->findClip(id);
        REQUIRE(loc);
        return ProjectController::parameterValue(loc->clip(), edit::ParamRef::of(ParamId::VolumeDb));
    }
    double constant(std::string_view ref) const { return level(ref).constant(); }
    void select(std::string_view ref) { controller.setSelection({clipAt(*controller.snapshot(), tl.seq, ref)}); }
    std::size_t undo() { return controller.session().undoCount(); }
    QString run(const char* id) {
        const QVariantMap r = audio.runVolumeCommand(QString::fromLatin1(id));
        return r.value(QStringLiteral("text")).toString();
    }
};

} // namespace

TEST_CASE("Clip volume keys move the Volume Level and merge into one undo step", "[app][audio][volumekeys]") {
    VolumeRig rig;
    CHECK(ClipAudioBridge::handlesCommand(QStringLiteral("clip.increaseVolume")));
    CHECK(ClipAudioBridge::handlesCommand(QStringLiteral("clip.decreaseVolumeMany")));
    CHECK_FALSE(ClipAudioBridge::handlesCommand(QStringLiteral("clip.audioGain")));
    rig.select("V1@0"); // the video stands for its linked audio
    const std::size_t undo = rig.undo();
    CHECK(rig.run("clip.increaseVolume") == QStringLiteral("Volume +1 dB (now 1.0 dB)"));
    rig.run("clip.increaseVolume");
    rig.run("clip.increaseVolume");
    CHECK(rig.constant("A1@0") == 3.0);
    CHECK(rig.run("clip.decreaseVolumeMany") == QStringLiteral("Volume -6 dB (now -3.0 dB)"));
    CHECK(rig.constant("A1@0") == -3.0);
    CHECK(rig.constant("A2@2") == 0.0); // not selected
    CHECK(rig.undo() == undo + 1);      // presses within a second are one step
    CHECK(rig.controller.undoText().contains(QStringLiteral("Clip Volume")));
    REQUIRE(rig.controller.undo());
    CHECK(rig.constant("A1@0") == 0.0);
    REQUIRE(rig.controller.redo());
    CHECK(rig.constant("A1@0") == -3.0);
    // After a second the next press is a step of its own.
    spin(1200);
    rig.run("clip.increaseVolume");
    CHECK(rig.undo() == undo + 2);
    CHECK(rig.constant("A1@0") == -2.0);
    // Clamped to the parameter's range (+24 dB).
    for (int i = 0; i < 6; ++i)
        rig.run("clip.increaseVolumeMany");
    CHECK(rig.constant("A1@0") == 24.0);
}

TEST_CASE("Clip volume keys leave a slider drag that starts within the second one undo step", "[app][audio][volumekeys]") {
    // A drag merges by its own key until it ends; the volume keys' one-second window must not close it halfway.
    VolumeRig rig;
    rig.select("A1@0");
    const std::size_t undo = rig.undo();
    rig.run("clip.increaseVolume");
    CHECK(rig.undo() == undo + 1);
    // A fader drag on another clip (the Audio Clip Mixer's merge key), its first move within the second.
    const ClipId a2 = rig.tl.clip("A2@2");
    rig.controller.setPlayhead(f(3));
    const edit::ParamRef volume = edit::ParamRef::of(ParamId::VolumeDb);
    REQUIRE(rig.controller.setParameterAtPlayhead(a2, volume, -2.0, "Clip Volume", "clipMixer:test:volume"));
    CHECK(rig.undo() == undo + 2);
    spin(1200); // the volume keys' window ends while the drag goes on
    REQUIRE(rig.controller.setParameterAtPlayhead(a2, volume, -4.0, "Clip Volume", "clipMixer:test:volume"));
    CHECK(rig.undo() == undo + 2); // still the drag's one step
    CHECK(rig.constant("A2@2") == -4.0);
    rig.controller.endPropertyGesture();
    // The next volume key press is a step of its own; undo takes back only it, then the whole drag.
    rig.run("clip.increaseVolume");
    CHECK(rig.undo() == undo + 3);
    CHECK(rig.constant("A1@0") == 2.0);
    REQUIRE(rig.controller.undo());
    CHECK(rig.constant("A1@0") == 1.0);
    REQUIRE(rig.controller.undo());
    CHECK(rig.constant("A2@2") == 0.0);
    CHECK(rig.constant("A1@0") == 1.0);
}

TEST_CASE("Clip volume keys move every volume keyframe", "[app][audio][volumekeys]") {
    VolumeRig rig;
    const ClipId a1 = rig.tl.clip("A1@0");
    const SequenceId seq = rig.tl.seq;
    REQUIRE(rig.controller.applyEditFn("Volume", [&](const ProjectState& s) {
        return edit::setParameter(s, seq, a1, edit::ParamRef::of(ParamId::VolumeDb),
                                  ParamValue(Curve{{Keyframe{f(0), -12.0}, Keyframe{f(5), 0.0}}}));
    }));
    rig.select("A1@0");
    CHECK(rig.run("clip.increaseVolumeMany") == QStringLiteral("Volume +6 dB on 1 clip"));
    const ParamValue level = rig.level("A1@0");
    REQUIRE(level.isCurve());
    REQUIRE(level.curve()->keys.size() == 2);
    CHECK(level.curve()->keys[0].value == -6.0); // the shape stays
    CHECK(level.curve()->keys[1].value == 6.0);
    CHECK(level.curve()->keys[1].time == f(5));
}

TEST_CASE("Clip volume keys without a selection act on the clip under the playhead on targeted tracks",
          "[app][audio][volumekeys]") {
    VolumeRig rig;
    rig.controller.clearSelection();
    rig.controller.setPlayhead(f(3));
    // V1 and A1 are targeted: the A clip's audio (once, though both its video and audio are under the playhead).
    CHECK(rig.audio.volumeTargets().size() == 2);
    CHECK(rig.run("clip.decreaseVolume") == QStringLiteral("Volume -1 dB (now -1.0 dB)"));
    CHECK(rig.constant("A1@0") == -1.0);
    CHECK(rig.constant("A2@2") == 0.0); // A2 is not targeted
    // No track targeted: every track.
    for (const char* track : {"V1", "A1"})
        rig.controller.setTrackFlag(trackKey(rig.tl.track(track)), QStringLiteral("targeted"), false);
    CHECK(rig.run("clip.increaseVolume") == QStringLiteral("Volume +1 dB on 2 clips"));
    CHECK(rig.constant("A1@0") == 0.0);
    CHECK(rig.constant("A2@2") == 1.0);
}

TEST_CASE("Clip volume keys skip locked tracks and explain an empty target", "[app][audio][volumekeys]") {
    VolumeRig rig;
    // A video clip with no sound.
    rig.select("V2@4");
    QVariantMap r = rig.audio.runVolumeCommand(QStringLiteral("clip.increaseVolume"));
    CHECK_FALSE(r.value(QStringLiteral("ok")).toBool());
    CHECK(r.value(QStringLiteral("text")).toString() == QStringLiteral("Select clips with sound"));
    QVariantMap state = rig.audio.commandAvailability(QStringLiteral("clip.increaseVolume"));
    CHECK(state.value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
    CHECK(state.value(QStringLiteral("reason")).toString() == QStringLiteral("Select clips with sound"));
    // Its audio on a locked track.
    rig.controller.setTrackFlag(trackKey(rig.tl.track("A1")), QStringLiteral("locked"), true);
    const std::size_t undo = rig.undo();
    rig.select("V1@0");
    r = rig.audio.runVolumeCommand(QStringLiteral("clip.decreaseVolume"));
    CHECK_FALSE(r.value(QStringLiteral("ok")).toBool());
    CHECK(r.value(QStringLiteral("text")).toString() == QStringLiteral("The audio tracks are locked"));
    CHECK(rig.constant("A1@0") == 0.0);
    CHECK(rig.undo() == undo);
    // With an unlocked clip beside it, only that one changes.
    rig.controller.setSelection({rig.tl.clip("V1@0"), rig.tl.clip("A2@2")});
    CHECK(rig.run("clip.decreaseVolume") == QStringLiteral("Volume -1 dB (now -1.0 dB)"));
    CHECK(rig.constant("A2@2") == -1.0);
    CHECK(rig.constant("A1@0") == 0.0);
    state = rig.audio.commandAvailability(QStringLiteral("clip.decreaseVolume"));
    CHECK(state.value(QStringLiteral("enabled")).toBool());
    CHECK_FALSE(rig.audio.commandAvailability(QStringLiteral("clip.audioGain")).value(QStringLiteral("handled")).toBool());
}
