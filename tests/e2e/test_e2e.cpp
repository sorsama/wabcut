// The MVP exit gate (I.1): the editor loop end to end, through the same objects the app uses.
//
//   generated media -> import (ProjectController + its ProjectFile bridge) -> a 3-track sequence built with edit
//   commands -> 2 s of playback through the real Scheduler (an offscreen renderer stands in for the monitor) and
//   the Null audio engine -> save -> destroy everything -> reopen, compare -> move a media file -> reopen offline,
//   relink through the API -> export through the export API (ExportJob: the real audio mix; synthetic video until
//   the export module's renderer source lands).
//
// A second case loads the real shell (Main.qml, hidden; nothing appears on screen) and writes the offscreen
// screenshots: the library with real thumbnails and the monitor mid-playback.
//
// I.3: an H.264 4:2:2 10-bit 2160p60 clip imported into the shell gets an automatic proxy (Auto policy, the trial
// open finds no hardware decoder), and the offscreen render loop measures playback from the originals and from the
// proxy (test-output/e2e/proxy-playback.md); screenshots of the library mid-generation and the monitor's badge.

#include "BridgeIds.h"
#include "CollectFilesBridge.h"
#include "ColorBridge.h"
#include "MediaBridge.h"
#include "MaskBridge.h"
#include "MediaReadinessBridge.h"
#include "PlaybackBridge.h"
#include "ProgramMonitor.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "RhiPolicy.h"
#include "ScopesBridge.h"
#include "SourceMonitorBridge.h"
#include "SyntheticMotion.h"

#include <private/qquickwindow_p.h>

#include <wabcut/command/Engine.h>
#include <wabcut/editor/Export.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/render/CompositeMailbox.h>
#include <wabcut/render/GpuScopes.h>
#include <rhi/qrhi.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QtQml/QQmlExtensionPlugin>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

Q_IMPORT_QML_PLUGIN(WabcutPlugin)

using namespace wabcut;
namespace fs = std::filesystem;

namespace {

int g_argc = 1;
char g_argv0[] = "wabcut_e2e_tests";
char* g_argv[] = {g_argv0, nullptr};
QGuiApplication* g_app = nullptr;
std::unique_ptr<QTemporaryDir> g_settings;
std::unique_ptr<QTemporaryDir> g_cache;
QStringList g_testDataPaths;

class QtApplicationListener final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo&) override {
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0"); // deterministic pixels
        qputenv("WABCUT_AUDIO_BACKEND", "null");   // the engine runs only when the test pulls it
        QCoreApplication::setOrganizationName(QStringLiteral("WabcutTests"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("tests.wabcut.org"));
        // Caches and autosaves under the test locations, per process (the name carries the process id; removed at the
        // end), and Wabcut's own cache in a temporary directory: nothing carries over between runs (M5j).
        const QString testName = QStringLiteral("wabcut_e2e_tests-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
        QCoreApplication::setApplicationName(testName);
        QStandardPaths::setTestModeEnabled(true);
        g_cache = std::make_unique<QTemporaryDir>();
        qputenv("WABCUT_CACHE_DIR", QDir::toNativeSeparators(g_cache->path()).toUtf8());
        g_settings = std::make_unique<QTemporaryDir>();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, g_settings->path());
        g_app = new QGuiApplication(g_argc, g_argv);
        for (const auto location : {QStandardPaths::AppLocalDataLocation, QStandardPaths::AppDataLocation,
                                    QStandardPaths::AppConfigLocation, QStandardPaths::CacheLocation}) {
            const QString path = QDir::cleanPath(QStandardPaths::writableLocation(location));
            if (QDir::isAbsolutePath(path) && QDir(path).dirName() == testName)
                g_testDataPaths.append(path);
        }
        g_testDataPaths.removeDuplicates();
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        using namespace wabcut::app;
        applyRhiBackend(resolveRhiBackend({}, qgetenv("QSG_RHI_BACKEND"), currentRhiPlatform()));
    }
    void testRunEnded(const Catch::TestRunStats&) override {
        delete g_app;
        g_app = nullptr;
        g_settings.reset();
        g_cache.reset();
        for (const QString& path : g_testDataPaths)
            QDir(path).removeRecursively();
        g_testDataPaths.clear();
    }
};
CATCH_REGISTER_LISTENER(QtApplicationListener)

constexpr const char* kClips[] = {"e2e_a.mp4", "e2e_b.mp4", "e2e_c.mp4"};

void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        spin(10);
    }
    return true;
}

bool haveMedia() {
    for (const char* clip : kClips) {
        if (!fs::exists(fs::path(WABCUT_E2E_MEDIA_DIR) / clip)) {
            return false;
        }
    }
    return true;
}

/// A fresh folder under test-output/e2e with copies of the generated clips in <dir>/media.
fs::path freshWorkDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "media");
    for (const char* clip : kClips) {
        fs::copy_file(fs::path(WABCUT_E2E_MEDIA_DIR) / clip, dir / "media" / clip);
    }
    return dir;
}

QUrl urlOf(const fs::path& path) {
    return QUrl::fromLocalFile(QString::fromStdU16String(path.u16string()));
}

QList<QUrl> clipUrls(const fs::path& dir) {
    QList<QUrl> urls;
    for (const char* clip : kClips) {
        urls.append(urlOf(dir / "media" / clip));
    }
    return urls;
}

/// One "app session": a QML engine with the Wabcut singletons the shell uses.
struct Editor {
    QQmlEngine engine;
    app::ProjectController* project = nullptr;
    app::ProjectLifecycle* file = nullptr;
    app::PlaybackBridge* playback = nullptr;

    Editor() {
        QQmlComponent importer(&engine);
        importer.setData("import QtQml\nimport Wabcut\nQtObject {}", QUrl());
        std::unique_ptr<QObject> loaded(importer.create()); // registers the module's types with this engine
        project = engine.singletonInstance<app::ProjectController*>("Wabcut", "ProjectController");
        file = engine.singletonInstance<app::ProjectLifecycle*>("Wabcut", "ProjectFile");
        playback = engine.singletonInstance<app::PlaybackBridge*>("Wabcut", "Playback");
        REQUIRE(project != nullptr);
        REQUIRE(file != nullptr);
        REQUIRE(playback != nullptr);
        CHECK(engine.imageProvider(QStringLiteral("wabcut-thumbnails")) != nullptr); // library thumbnails
    }
};

/// The 3-track sequence, built with edit commands (one undo step): clip A on V1/A1 from 0, B overwritten on
/// V2/A2 from 1 s, C on V3/A3 from 2 s.
void buildSequence(EditSession& session) {
    command::CommandEngine commands(session);
    command::CommandBatch batch;
    batch.label = "Build the e2e sequence";
    batch.commands = {
        {.command = "clip.insert", .params = {{"media", "e2e_a.mp4"}, {"at", "00:00:00:00"}}},
        {.command = "clip.overwrite",
         .params = {{"media", "e2e_b.mp4"}, {"at", "00:00:01:00"}, {"video", "V2"}, {"audio", command::Json::array({"A2"})}}},
        {.command = "clip.overwrite",
         .params = {{"media", "e2e_c.mp4"}, {"at", "00:00:02:00"}, {"video", "V3"}, {"audio", command::Json::array({"A3"})}}},
    };
    const command::CommandResult result = commands.execute(batch);
    INFO((result.error ? result.error->message : std::string()));
    REQUIRE_FALSE(result.error);
}

std::size_t clipCount(const Sequence& seq) {
    std::size_t n = 0;
    seq.forEachTrack([&](const Track& t) { n += t.clips.size(); });
    return n;
}

// ---- Timing thresholds ---------------------------------------------------------------------------------------------
// The e2e cases always assert what must happen (pictures presented, sound heard, scopes delivered, proxies used).
// Timing thresholds (frame budgets, nothing dropped) need a quiet machine: they are asserted by the "e2e perf: ..."
// cases, tagged [perf] (CTest label "perf", tests/CMakeLists.txt: `ctest -L perf` on an idle machine, `-LE perf` while
// others build), which measure once more when the first attempt misses. The functional cases record their numbers in
// test-output/e2e/timing.md.

void recordTiming(const std::string& what, const std::string& numbers) {
    std::ofstream out(fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / "timing.md", std::ios::app);
    out << "- " << std::format("{:%F %T}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()))
        << " " << what << ": " << numbers << " (recorded, not asserted)\n";
}

/// Real-time pulls of the Null engine: `frames` stereo frames at steady time now.
double pullAudio(editor::PlaybackSession& session, std::vector<float>& block) {
    REQUIRE(session.engine().pull(block));
    double energy = 0.0;
    for (float s : block) {
        energy += std::abs(s);
    }
    return energy;
}

} // namespace

TEST_CASE("e2e: import, edit, play, save, reopen, relink, export", "[e2e]") {
    if (!haveMedia()) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    const fs::path dir = freshWorkDir("loop");
    const fs::path projectFile = dir / "project" / "e2e.wabcut";
    fs::create_directories(projectFile.parent_path());

    ProjectState saved;
    {
        // The renderer first: it shares its device with the decoders (zero-copy) before any decoder opens.
        render::OffscreenOptions ro;
        ro.role = render::OutputRole::Display;
        ro.compositor = {render::TexturePool::Preview, 1, 48};
        ro.shareDeviceWithDecoders = true;
        auto renderer = render::OffscreenRenderer::create(ro);
        REQUIRE(renderer);

        Editor editor;
        CHECK(editor.file->demo());

        // ---- Import 3 clips (the demo gives way to a new project; its sequence follows the first clip) ----
        const QVariantMap imported = editor.file->importFiles(clipUrls(dir));
        CHECK(imported.value(QStringLiteral("added")).toInt() == 3);
        CHECK(imported.value(QStringLiteral("failed")).toList().isEmpty());
        CHECK_FALSE(editor.file->demo());
        CHECK(editor.file->untitled());
        CHECK(editor.file->dirty());
        const QVariantList items = editor.project->mediaItems();
        REQUIRE(items.size() == 3);
        for (const QVariant& v : items) {
            const QVariantMap item = v.toMap();
            CHECK_FALSE(item.value(QStringLiteral("offline")).toBool());
            CHECK(item.value(QStringLiteral("thumbnail")).toString().startsWith(QStringLiteral("image://wabcut-thumbnails/")));
            CHECK(item.value(QStringLiteral("kind")).toString() == QStringLiteral("video"));
            CHECK(item.value(QStringLiteral("durationText")).toString() == QStringLiteral("00:04:00"));
        }
        // The library's thumbnail URLs decode (the provider the cards load them from).
        {
            app::ThumbnailImageProvider provider((dir / "thumbnail-cache").u16string());
            const QString url = items.front().toMap().value(QStringLiteral("thumbnail")).toString();
            const QString id = url.mid(QStringLiteral("image://wabcut-thumbnails/").size());
            std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(id, QSize(0, 90)));
            REQUIRE(response);
            bool finished = false;
            QObject::connect(response.get(), &QQuickImageResponse::finished, [&] { finished = true; });
            REQUIRE(waitFor([&] { return finished; }, 20000));
            INFO(url.toStdString() << ": " << response->errorString().toStdString());
            CHECK(response->errorString().isEmpty());
        }
        const SequencePtr seq0 = editor.project->currentSequence();
        REQUIRE(seq0);
        CHECK(seq0->width == 1280);
        CHECK(seq0->height == 720);
        CHECK(seq0->frameRate == Rate{30, 1});

        // ---- A 3-track sequence through edit commands ----
        buildSequence(editor.project->session());
        const SequencePtr seq = editor.project->currentSequence();
        REQUIRE(seq);
        CHECK(clipCount(*seq) == 6); // 3 video + 3 audio, linked in pairs
        for (int v = 0; v < 3; ++v) {
            CHECK(seq->videoTracks[static_cast<std::size_t>(v)]->clips.size() == 1);
            CHECK(seq->audioTracks[static_cast<std::size_t>(v)]->clips.size() == 1);
        }
        CHECK(seq->contentEnd() == Ticks::fromSeconds(6));
        CHECK(editor.project->frameRateText() == QStringLiteral("30 fps"));

        // ---- Play 2 s: real Scheduler + offscreen renderer, audio from the Null engine ----
        editor.project->setPlayhead({});
        editor.playback->startAudio();
        editor::PlaybackSession& session = *editor.playback->session();
        REQUIRE(session.hasSound());
        playback::PlaybackController& video = *editor.playback->activeController();
        render::CompositeMailbox& mailbox = *video.mailbox();
        constexpr std::int64_t kRefreshNs = 16'666'667;
        std::vector<float> block(800 * 2); // one 60 Hz refresh of 48 kHz stereo
        // Warm-up: the first frame at 0 decoded and the audio rings filled during the preroll.
        REQUIRE(waitFor([&] { return video.stats().published > 0; }));
        for (int i = 0; i < 6; ++i) {
            pullAudio(session, block);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        (void)mailbox.acquire(playback::steadyNowNs());
        const render::MailboxStats before = mailbox.stats();

        editor.playback->play();
        CHECK(editor.playback->playing());
        double energy = 0.0;
        int presented = 0;
        render::RenderedImage frame;
        const auto start = std::chrono::steady_clock::now();
        for (std::int64_t k = 0; k < 120; ++k) {
            std::this_thread::sleep_until(start + std::chrono::nanoseconds(k * kRefreshNs));
            energy += pullAudio(session, block);
            QCoreApplication::processEvents(); // the bridge moves the playhead
            const auto selection = mailbox.acquire(playback::steadyNowNs() + kRefreshNs);
            if (selection.changed && selection.composite) {
                REQUIRE((*renderer)->render(selection.composite, frame, QSize(640, 360)));
                ++presented;
            }
        }
        editor.playback->pause();
        spin(50);
        const render::MailboxStats after = mailbox.stats();
        const editor::PlaybackSessionStats stats = session.stats();
        const render::FrameImporterStats imports = (*renderer)->compositor().importer().stats();
        INFO("presented " << presented << ", dropped " << (after.dropped - before.dropped) << ", decode "
                          << stats.video.decodeBackends << ", transport " << stats.video.transport << ", renderer "
                          << (*renderer)->backendName() << ", zero-copy imports " << imports.nativeImports
                          << ", uploads " << imports.uploads << ", rejected " << imports.nativeRejected);
        if ((*renderer)->backendName() == "D3D11" && !media::sharedD3D11Device().isCpu()) {
            CHECK(imports.nativeImports > 0); // decoder surfaces sampled in place
            CHECK(imports.nativeRejected == 0);
        }
        // The pictures came (the real-time rate, >= 45 of 60, is the [perf] case's: "e2e perf: the 3-track edit ...").
        CHECK(presented > 0);
        CHECK(after.presented - before.presented > 0);
        recordTiming("editor loop, 2 s at 30 fps", std::format("{} of 60 pictures presented", presented));
        CHECK(stats.video.decodeErrors == 0);
        CHECK(stats.video.transport == "timeline audio (Null)");
        CHECK(energy > 1000.0); // the three tones were heard
        CHECK(stats.audio.framesFed > 0);
        CHECK(stats.audio.feedErrors == 0);
        CHECK(stats.audio.activeTracks >= 3);
        CHECK(editor.project->playhead() > Ticks::fromSeconds(1));
        CHECK(editor.project->playhead() < Ticks::fromSeconds(3));
        // The last frame rendered is a real picture (testsrc2 / bars), not black.
        int bright = 0;
        for (int y = 0; y < frame.height; y += 8) {
            for (int x = 0; x < frame.width; x += 8) {
                const std::uint8_t* p = frame.pixel(x, y);
                bright += (p[0] + p[1] + p[2]) > 150 ? 1 : 0;
            }
        }
        CHECK(bright > 100);
        QImage(frame.rgba.data(), frame.width, frame.height, frame.width * 4, QImage::Format_RGBA8888)
            .save(QString::fromStdU16String((dir / "offscreen_monitor_2s.png").u16string()));

        // ---- Save ----
        REQUIRE(editor.file->saveAs(urlOf(projectFile)));
        CHECK_FALSE(editor.file->dirty());
        CHECK(editor.file->windowTitle() == QStringLiteral("e2e — Wabcut"));
        CHECK(fs::exists(projectFile));
        saved = editor.project->session().stateForSave();
    } // everything is destroyed: QML engine, singletons, playback threads, renderer
    (void)media::setSharedD3D11Device(nullptr, nullptr); // the renderer that shared its device is gone

    // ---- Reopen: the same project ----
    {
        Editor editor;
        REQUIRE(editor.file->open(urlOf(projectFile)));
        CHECK_FALSE(editor.file->dirty());
        CHECK(editor.file->offlineCount() == 0);
        const ProjectState reopened = editor.project->session().stateForSave();
        REQUIRE(reopened.sequences.size() == saved.sequences.size());
        for (std::size_t i = 0; i < saved.sequences.size(); ++i) {
            CHECK(*reopened.sequences[i] == *saved.sequences[i]);
        }
        REQUIRE(reopened.media.size() == saved.media.size());
        for (std::size_t i = 0; i < saved.media.size(); ++i) {
            const MediaItem& a = *saved.media[i];
            const MediaItem& b = *reopened.media[i];
            CHECK(a.id == b.id);
            CHECK(a.name == b.name);
            CHECK(a.info == b.info);
            CHECK(a.locator.path == b.locator.path);
            CHECK(a.locator.fingerprint == b.locator.fingerprint);
            CHECK(b.locator.relativePath == "../media/" + a.name); // written relative to the project file
        }
        CHECK(reopened.bins == saved.bins);
        CHECK(editor.project->sequenceName() == QString::fromStdString(saved.sequences[0]->name));
    }

    // ---- Move a media file: it opens offline; relink through the API ----
    fs::create_directories(dir / "moved" / "deeper");
    fs::rename(dir / "media" / "e2e_b.mp4", dir / "moved" / "deeper" / "e2e_b.mp4");
    {
        Editor editor;
        REQUIRE(editor.file->open(urlOf(projectFile)));
        REQUIRE(editor.file->offlineCount() == 1);
        const QVariantMap offline = editor.file->offlineMedia().front().toMap();
        CHECK(offline.value(QStringLiteral("name")).toString() == QStringLiteral("e2e_b.mp4"));
        int offlineCards = 0;
        for (const QVariant& v : editor.project->mediaItems()) {
            offlineCards += v.toMap().value(QStringLiteral("offline")).toBool() ? 1 : 0;
        }
        CHECK(offlineCards == 1);

        const QVariantList candidates =
            editor.file->relinkCandidates(offline.value(QStringLiteral("id")).toString(), {urlOf(dir / "moved")});
        REQUIRE(candidates.size() == 1);
        CHECK(candidates.front().toMap().value(QStringLiteral("matches")).toBool());
        CHECK(editor.file->relinkAll({urlOf(dir / "moved")}) == 1);
        CHECK(editor.file->offlineCount() == 0);
        CHECK(editor.file->dirty());
        for (const MediaPtr& m : editor.project->session().snapshot()->media) {
            CHECK_FALSE(m->locator.offline);
            if (m->name == "e2e_b.mp4") {
                CHECK(pathFromUtf8(m->locator.path) == fs::absolute(dir / "moved" / "deeper" / "e2e_b.mp4").lexically_normal());
            }
        }
        // The relinked clip decodes again: scrub into it (V2 covers 1..5 s).
        editor.project->setPlayhead(Ticks::fromSeconds(2));
        playback::PlaybackController& video = *editor.playback->activeController();
        const std::uint64_t published = video.stats().published;
        REQUIRE(waitFor([&] { return video.stats().published > published; }));
        CHECK(video.stats().decodeErrors == 0);
        REQUIRE(editor.file->save());
        CHECK_FALSE(editor.file->dirty());
    }
    {
        Editor editor;
        REQUIRE(editor.file->open(urlOf(projectFile)));
        CHECK(editor.file->offlineCount() == 0); // the relinked path was saved

        // ---- Export through the export API (ExportJob behind the editor's seam) ----
        // M6 phase 1: the timeline's audio is mixed for real; the video source is the export module's synthetic
        // test pattern until its renderer source (phase 2) lands.
        const SequencePtr seq = editor.project->currentSequence();
        REQUIRE(seq);
        const fs::path output = dir / "export" / "e2e.mp4";
        fs::create_directories(output.parent_path());
        editor::setExporter(editor::makeExportJobExporter(exporter::syntheticVideoSourceFactory()));
        double lastFraction = 0.0;
        const auto exported = editor::exportSequence(editor.project->session(), {.sequence = seq->id, .output = output},
                                                     [&](double fraction) {
                                                         lastFraction = fraction;
                                                         return true;
                                                     });
        editor::setExporter(nullptr);
        INFO((exported ? std::string() : exported.error().message));
        REQUIRE(exported);
        CHECK(fs::exists(output));
        CHECK(exported->videoFrames == 180); // 6 s at 30 fps
        CHECK(exported->audioSamples == 6 * 48000);
        CHECK(lastFraction > 0.9);
        const auto probed = media::probe(pathToUtf8(output));
        REQUIRE(probed);
        CHECK(probed->hasVideo());
        CHECK(probed->hasAudio());
        CHECK(std::llabs(probed->duration.value - seq->contentEnd().value) <= Ticks::fromSeconds(1).value / 10);
        // The exported audio is the timeline mix: the clips' tones, not silence.
        auto decoder = media::AudioDecoder::open(pathToUtf8(output), -1, Rate{48000, 1}, 2);
        REQUIRE(decoder);
        std::vector<float> pcm(48000 * 2);
        REQUIRE((*decoder)->read(Ticks::fromSeconds(2), pcm));
        double exportedEnergy = 0.0;
        for (float v : pcm) {
            exportedEnergy += std::abs(v);
        }
        CHECK(exportedEnergy > 1000.0);

        // ---- A timeline from another editor goes through the same import, with its report ----
        QVariantMap report;
        QObject::connect(editor.file, &app::ProjectLifecycle::importReport,
                         [&](const QVariantMap& r) { report = r; });
        const std::size_t sequencesBefore = editor.project->session().snapshot()->sequences.size();
        const QVariantMap imported =
            editor.file->importFiles({urlOf(fs::path(WABCUT_INTERCHANGE_FIXTURES) / "avid_25fps.edl")});
        CHECK(imported.value(QStringLiteral("timelines")).toInt() == 1);
        REQUIRE_FALSE(report.isEmpty());
        CHECK(report.value(QStringLiteral("format")).toString() == QStringLiteral("CMX3600 EDL"));
        CHECK(report.value(QStringLiteral("sequences")).toInt() >= 1);
        CHECK(report.value(QStringLiteral("carried")).toInt() > 0);
        CHECK(editor.project->session().snapshot()->sequences.size() > sequencesBefore);
        CHECK(editor.project->session().undoLabel() == "Import avid_25fps.edl");
    }
}

namespace {

void collectItems(QQuickItem* item, std::vector<QQuickItem*>& out) {
    if (!item) {
        return;
    }
    out.push_back(item);
    for (QQuickItem* child : item->childItems()) {
        collectItems(child, out);
    }
}

std::string g_thumbnailStatus;
QStringList g_messages;

void captureMessages(QtMsgType, const QMessageLogContext&, const QString& message) {
    g_messages.append(message);
}

/// Library thumbnails (Image items showing image://wabcut-thumbnails/...) and how many have loaded.
std::pair<int, int> thumbnailState(QQuickWindow* window) {
    g_thumbnailStatus.clear();
    std::vector<QQuickItem*> items;
    // The source monitor can also display a thumbnail after another case selected media. This assertion is
    // specifically about the library's three imported cards, not every image in the window.
    QQuickItem* library = window->findChild<QQuickItem*>(QStringLiteral("libraryPanel"));
    REQUIRE(library != nullptr);
    collectItems(library, items);
    int total = 0;
    int ready = 0;
    for (QQuickItem* item : items) {
        const QUrl source = item->property("source").toUrl();
        if (source.scheme() == QStringLiteral("image") && source.host() == QStringLiteral("wabcut-thumbnails")) {
            ++total;
            ready += item->property("status").toInt() == 1 ? 1 : 0; // Image.Ready
            g_thumbnailStatus += " " + std::to_string(item->property("status").toInt());
        }
    }
    return {total, ready};
}

QRect sceneRect(const QQuickItem* item) {
    const QPointF topLeft = item->mapToScene(QPointF(0, 0));
    return QRect(qRound(topLeft.x()), qRound(topLeft.y()), qRound(item->width()), qRound(item->height()));
}

} // namespace

TEST_CASE("e2e: offscreen screenshots of the library and the monitor mid-playback", "[e2e][screenshot]") {
    if (!haveMedia()) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    // Keep disposable inputs separate from the shared screenshot output (including the I.6 captures).
    const fs::path dir = freshWorkDir("screenshots-work");
    // The grab renders with a temporary QRhi of its own: keep decoded frames on the CPU for it.
    qputenv("WABCUT_ZERO_COPY", "0");
    struct RestoreZeroCopy {
        ~RestoreZeroCopy() { qunsetenv("WABCUT_ZERO_COPY"); }
    } restoreZeroCopy;

    QQmlEngine engine;
    QQmlComponent component(&engine, "Wabcut", "Main");
    INFO(component.errorString().toStdString());
    REQUIRE_FALSE(component.isError());
    std::unique_ptr<QQuickWindow> window(
        qobject_cast<QQuickWindow*>(component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
    REQUIRE(window);
    window->create();
    window->resize(1600, 960);
    window->contentItem()->setSize(QSizeF(1600, 960));
    auto* project = engine.singletonInstance<app::ProjectController*>("Wabcut", "ProjectController");
    auto* file = engine.singletonInstance<app::ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* playback = engine.singletonInstance<app::PlaybackBridge*>("Wabcut", "Playback");
    REQUIRE(project);
    REQUIRE(file);
    REQUIRE(playback);
    CHECK(window->title() == QStringLiteral("Demo project — Wabcut"));
    CHECK(engine.imageProvider(QStringLiteral("wabcut-thumbnails")) != nullptr);

    // Import through the bridge and drop the clips on the timeline the way the timeline's DropArea does.
    const QVariantMap imported = file->importFiles(clipUrls(dir));
    REQUIRE(imported.value(QStringLiteral("added")).toInt() == 3);
    const SequencePtr seq = project->currentSequence();
    REQUIRE(seq);
    QStringList ids;
    for (const QVariant& v : project->mediaItems()) {
        ids.append(v.toMap().value(QStringLiteral("id")).toString());
    }
    REQUIRE(ids.size() == 3);
    CHECK(file->dropMedia(ids[0], QStringLiteral("00:00:00:00"), QString(), false));
    CHECK(file->dropMedia(ids[1], QStringLiteral("00:00:04:00"), app::trackKey(seq->videoTracks[0]->id), false));
    CHECK(file->dropMedia(ids[2], QStringLiteral("00:00:01:00"), app::trackKey(seq->videoTracks[1]->id), true));
    CHECK(clipCount(*project->currentSequence()) == 6);
    CHECK(window->title() == QStringLiteral("● Untitled — Wabcut"));

    // Real thumbnails in the library. A window that is never shown is never polished by a render loop, so lay
    // it out here (the GridView creates its cards in updatePolish); the one grab below renders it.
    std::pair<int, int> thumbnails;
    g_messages.clear();
    const QtMessageHandler previousHandler = qInstallMessageHandler(captureMessages);
    const bool loaded = waitFor([&] {
        QQuickWindowPrivate::get(window.get())->polishItems();
        thumbnails = thumbnailState(window.get());
        return thumbnails.first == 3 && thumbnails.second == 3;
    }, 20000);
    INFO("thumbnail images " << thumbnails.first << ", ready " << thumbnails.second << ", library items "
                             << project->mediaItems().size() << ", status" << g_thumbnailStatus << "; "
                             << g_messages.join(QStringLiteral(" | ")).toStdString());
    qInstallMessageHandler(previousHandler);
    REQUIRE(loaded);

    // Mid-playback: play ~1.6 s on the Null engine's clock. A window that is never shown has no render loop, so
    // the test runs one: every 60 Hz refresh takes the due composite from the monitor's mailbox and renders it
    // (what the monitor's render thread does), so presentation statistics are real.
    auto* monitor = window->findChild<app::ProgramMonitor*>(QStringLiteral("programMonitor"));
    REQUIRE(monitor != nullptr);
    render::OffscreenOptions presenterOptions;
    presenterOptions.role = render::OutputRole::Display;
    presenterOptions.compositor = {render::TexturePool::Preview, 1, 48};
    auto presenter = render::OffscreenRenderer::create(presenterOptions);
    REQUIRE(presenter);
    project->setPlayhead({});
    playback->startAudio();
    REQUIRE(playback->session()->hasSound());
    playback->play();
    render::CompositeMailbox& mailbox = *playback->activeController()->mailbox();
    std::vector<float> block(800 * 2);
    const auto start = std::chrono::steady_clock::now();
    int presentedHere = 0;
    for (int k = 0; k < 96; ++k) {
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(k * 16'666'667LL));
        pullAudio(*playback->session(), block);
        QCoreApplication::processEvents();
        const auto selection = mailbox.acquire(playback::steadyNowNs() + 16'666'667LL);
        if (selection.changed && selection.composite) {
            REQUIRE((*presenter)->renderTimed(selection.composite, QSize(960, 540)));
            ++presentedHere;
        }
    }
    CHECK(playback->playing());
    CHECK(project->playhead() > Ticks::fromSeconds(1));
    // The transport readout: the measured rate of a 30 fps sequence, nothing dropped.
    const QString fps = playback->fpsText();
    INFO("readout: " << fps.toStdString() << " · " << playback->droppedFrames() << " dropped; presented " << presentedHere);
    // The readout works and pictures came; its 30 fps and nothing dropped are the [perf] case's.
    CHECK(presentedHere > 0);
    CHECK(fps.section(QLatin1Char(' '), 0, 0).toDouble() > 0.0);
    recordTiming("monitor mid-playback (screenshots)",
                 std::format("{} pictures in 96 refreshes, readout {}, {} dropped", presentedHere, fps.toStdString(),
                             playback->droppedFrames()));

    const QImage image = window->grabWindow();
    REQUIRE_FALSE(image.isNull());
    playback->pause();
    const QString out = QString::fromStdU16String((fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / "screenshots").u16string());
    QDir().mkpath(out);
    CHECK(image.save(out + QStringLiteral("/editor_mid_playback.png")));
    REQUIRE(waitFor([&] { return monitor->hasFrame(); }, 3000));

    auto* library = window->findChild<QQuickItem*>(QStringLiteral("libraryPanel"));
    auto* monitorPanel = window->findChild<QQuickItem*>(QStringLiteral("monitorPanel"));
    REQUIRE(library);
    REQUIRE(monitorPanel);
    CHECK(image.copy(sceneRect(library)).save(out + QStringLiteral("/library_thumbnails.png")));
    CHECK(image.copy(sceneRect(monitorPanel)).save(out + QStringLiteral("/monitor_mid_playback.png")));

    // The monitor shows a picture: many distinct colours (testsrc2), not the mock gradient or black.
    const QImage monitorArea = image.copy(sceneRect(monitor));
    QSet<QRgb> colours;
    for (int y = 0; y < monitorArea.height(); y += 4) {
        for (int x = 0; x < monitorArea.width(); x += 4) {
            colours.insert(monitorArea.pixel(x, y) & 0xF0F0F0u);
        }
    }
    INFO("distinct colours in the monitor: " << colours.size());
    CHECK(colours.size() > 40);
}

TEST_CASE("e2e: crash recovery offers unsaved work at the next start", "[e2e][recovery]") {
    if (!haveMedia()) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    const fs::path dir = freshWorkDir("recovery");
    const auto toPath = [](const QUrl& url) { return fs::path(url.toLocalFile().toStdU16String()); };

    // 1. An untitled project with unsaved work is autosaved, then the app "crashes": its engine stays alive, so
    //    the clean exit that removes autosaves never runs.
    auto crashed = std::make_unique<Editor>();
    REQUIRE(crashed->file->importFiles(clipUrls(dir)).value(QStringLiteral("added")).toInt() == 3);
    buildSequence(crashed->project->session());
    REQUIRE(crashed->file->autosaveNow());
    const ProjectState lost = crashed->project->session().stateForSave();
    {
        // The shell offers it on start.
        QQmlEngine engine;
        QQmlComponent component(&engine, "Wabcut", "Main");
        REQUIRE_FALSE(component.isError());
        std::unique_ptr<QQuickWindow> window(
            qobject_cast<QQuickWindow*>(component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
        REQUIRE(window);
        QObject* dialog = window->findChild<QObject*>(QStringLiteral("recoveryDialog"));
        REQUIRE(dialog != nullptr);
        CHECK(dialog->property("visible").toBool());
    }
    {
        Editor next; // the next start
        const QVariantList found = next.file->recoveryFiles();
        REQUIRE_FALSE(found.isEmpty());
        const QVariantMap entry = found.front().toMap();
        CHECK(entry.value(QStringLiteral("project")).toUrl().isEmpty()); // untitled
        const QUrl url = entry.value(QStringLiteral("url")).toUrl();
        REQUIRE(next.file->recover(url));
        CHECK(next.file->dirty());
        CHECK(next.file->untitled());
        CHECK_FALSE(next.file->demo());
        const ProjectState recovered = next.project->session().stateForSave();
        REQUIRE(recovered.sequences.size() == lost.sequences.size());
        CHECK(*recovered.sequences[0] == *lost.sequences[0]);
        CHECK(recovered.media.size() == lost.media.size());
        CHECK_FALSE(fs::exists(toPath(url))); // replaced by the new session's own autosave
        CHECK(fs::exists(next.file->document().autosaveFile()));
    }
    crashed.reset();

    // 2. A saved project, edited after its last save and autosaved, then a crash: the autosave is newer than the
    //    project file, so the next start offers it as that project.
    const fs::path projectFile = dir / "project" / "recover.wabcut";
    fs::create_directories(projectFile.parent_path());
    ProjectState edited;
    {
        Editor e;
        REQUIRE(e.file->importFiles(clipUrls(dir)).value(QStringLiteral("added")).toInt() == 3);
        buildSequence(e.project->session());
        REQUIRE(e.file->saveAs(urlOf(projectFile)));
        const QString first = e.project->mediaItems().front().toMap().value(QStringLiteral("id")).toString();
        REQUIRE(e.file->dropMedia(first, QStringLiteral("00:00:06:00"), QString(), false));
        REQUIRE(e.file->dirty());
        REQUIRE(e.file->autosaveNow());
        fs::copy_file(io::autosavePath(projectFile), dir / "kept.autosave"); // what the crash leaves behind
        edited = e.project->session().stateForSave();
    } // a clean exit: the autosave goes
    CHECK_FALSE(fs::exists(io::autosavePath(projectFile)));
    fs::copy_file(dir / "kept.autosave", io::autosavePath(projectFile));
    fs::last_write_time(io::autosavePath(projectFile), fs::last_write_time(projectFile) + std::chrono::seconds(5));
    {
        Editor next;
        QUrl url;
        for (const QVariant& v : next.file->recoveryFiles()) {
            if (toPath(v.toMap().value(QStringLiteral("project")).toUrl()) == fs::absolute(projectFile).lexically_normal()) {
                url = v.toMap().value(QStringLiteral("url")).toUrl();
            }
        }
        REQUIRE_FALSE(url.isEmpty());
        REQUIRE(next.file->recover(url));
        CHECK(next.file->dirty());
        CHECK(next.file->document().file() == fs::absolute(projectFile).lexically_normal());
        const ProjectState recovered = next.project->session().stateForSave();
        REQUIRE(recovered.sequences.size() == edited.sequences.size());
        CHECK(*recovered.sequences[0] == *edited.sequences[0]);
        CHECK(next.file->recoveryFiles().isEmpty());
        // Saving makes it the project; a clean exit then leaves nothing to recover.
        REQUIRE(next.file->save());
    }
    {
        Editor after;
        CHECK(after.file->recoveryFiles().isEmpty());
        // 3. Discarding forgets (and deletes) an autosave.
        fs::copy_file(dir / "kept.autosave", io::autosavePath(projectFile), fs::copy_options::overwrite_existing);
        fs::last_write_time(io::autosavePath(projectFile), fs::last_write_time(projectFile) + std::chrono::seconds(5));
        after.file->scanRecovery();
        REQUIRE(after.file->recoveryFiles().size() == 1);
        after.file->discardAllRecovery();
        CHECK(after.file->recoveryFiles().isEmpty());
        CHECK_FALSE(fs::exists(io::autosavePath(projectFile)));
    }
}

// ---- I.3: progressive media readiness ----------------------------------------------------------------------------

namespace {

constexpr const char* kHeavyClip = "e2e_422_10bit.mp4"; // 3840x2160 60 fps H.264 High 4:2:2 10-bit, 2 s

/// Sleeps until `due` within a fraction of a millisecond. A display's vsync paces the real monitor; this harness
/// paces itself, and on Windows 11 a process that owns a window (even a hidden one) sleeps in 15.6 ms steps
/// whatever timeBeginPeriod asks for, so two 60 Hz refreshes would fall into one slot and the harness itself would
/// supersede pictures. A high-resolution waitable timer is not tied to that resolution; the rest is spun.
void sleepUntilPrecisely(std::chrono::steady_clock::time_point due) {
#if defined(_WIN32)
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto remaining = due - std::chrono::steady_clock::now() - std::chrono::microseconds(500);
    if (timer && remaining > std::chrono::microseconds(0)) {
        LARGE_INTEGER relative;
        relative.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100;
        if (SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        }
    }
#else
    std::this_thread::sleep_until(due - std::chrono::microseconds(500));
#endif
    while (std::chrono::steady_clock::now() < due) {
        std::this_thread::yield();
    }
}

struct PlaybackRun {
    int refreshes = 0;
    int presentedHere = 0;
    std::uint64_t presented = 0;
    std::uint64_t dropped = 0;
    std::uint64_t late = 0;
    std::uint64_t skippedLate = 0;
    int droppedReadout = 0;
    QString fps;
    QString badge;
    double worstRenderMs = 0.0;
};

/// Plays the sequence from the start for `refreshes` 60 Hz refreshes of the offscreen render loop (what the
/// monitor's render thread does: take the due composite, render it) on the Null engine's clock.
PlaybackRun playFromStart(app::ProjectController* project, app::PlaybackBridge* playback,
                          app::MediaReadinessBridge* readiness, render::OffscreenRenderer& presenter, int refreshes,
                          const std::function<void()>& beforePause = {}) {
    project->setPlayhead({});
    // The paused seek decodes its frame, and decodes left over from a previous run (4K originals take a while)
    // finish before measuring.
    spin(1500);
    playback::PlaybackController* controller = playback->activeController();
    REQUIRE(controller);
    const playback::PlaybackStats before = controller->stats();
    playback->play();
    render::CompositeMailbox& mailbox = *controller->mailbox();
    std::vector<float> block(800 * 2);
    PlaybackRun run;
    run.refreshes = refreshes;
#if defined(_WIN32)
    // The loop stands in for the display's vsync: on a machine busy with other builds it must not lose its turn
    // to them (the pipeline's own threads keep their priorities).
    const int priority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    struct RestorePriority {
        int value;
        ~RestorePriority() { SetThreadPriority(GetCurrentThread(), value); }
    } restorePriority{priority};
#endif
    const auto start = std::chrono::steady_clock::now();
    for (int k = 0; k < refreshes; ++k) {
        sleepUntilPrecisely(start + std::chrono::nanoseconds(k * 16'666'667LL));
        pullAudio(*playback->session(), block);
        QCoreApplication::processEvents();
        const auto selection = mailbox.acquire(playback::steadyNowNs() + 16'666'667LL);
        if (selection.changed && selection.composite) {
            const auto timed = presenter.renderTimed(selection.composite, QSize(960, 540));
            REQUIRE(timed);
            run.worstRenderMs = std::max(run.worstRenderMs, timed->cpuMs);
            ++run.presentedHere;
        }
    }
    readiness->pollNow();
    const playback::PlaybackStats after = controller->stats();
    run.presented = after.presented - before.presented;
    run.dropped = after.dropped - before.dropped;
    run.late = after.late - before.late;
    run.skippedLate = after.skippedLate - before.skippedLate;
    run.fps = playback->fpsText();
    run.droppedReadout = playback->droppedFrames();
    run.badge = readiness->qualityBadge();
    if (beforePause) {
        beforePause(); // outside the measured loop (a window grab takes a while)
    }
    playback->pause();
    spin(100);
    return run;
}

std::string row(const char* what, const PlaybackRun& r) {
    return std::format("| {} | {} | {} / {} | {} | {} | {} | {} | {:.1f} ms |", what, r.badge.toStdString(),
                       r.presentedHere, r.refreshes, r.fps.toStdString(), r.droppedReadout, r.skippedLate, r.dropped,
                       r.worstRenderMs);
}

} // namespace

namespace {

/// Auto proxies the heavy clip, then plays it without and with the proxy. `perf`: the proxy playback must drop
/// nothing (measured once more when the first attempt misses); otherwise what must happen is asserted, the numbers are
/// recorded and the screenshots saved.
void proxyPlayback(bool perf) {
    const fs::path heavy = fs::path(WABCUT_E2E_MEDIA_DIR) / kHeavyClip;
    if (!fs::exists(heavy)) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / (perf ? "proxies-perf" : "proxies");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "media");
    fs::copy_file(heavy, dir / "media" / kHeavyClip);
    qputenv("WABCUT_ZERO_COPY", "0"); // the grabs render with a QRhi of their own: CPU frames
    struct RestoreZeroCopy {
        ~RestoreZeroCopy() { qunsetenv("WABCUT_ZERO_COPY"); }
    } restoreZeroCopy;

    QQmlEngine engine;
    QQmlComponent component(&engine, "Wabcut", "Main");
    INFO(component.errorString().toStdString());
    REQUIRE_FALSE(component.isError());
    std::unique_ptr<QQuickWindow> window(
        qobject_cast<QQuickWindow*>(component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
    REQUIRE(window);
    window->create();
    window->resize(1600, 960);
    window->contentItem()->setSize(QSizeF(1600, 960));
    auto* project = engine.singletonInstance<app::ProjectController*>("Wabcut", "ProjectController");
    auto* file = engine.singletonInstance<app::ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* playback = engine.singletonInstance<app::PlaybackBridge*>("Wabcut", "Playback");
    auto* readiness = engine.singletonInstance<app::MediaReadinessBridge*>("Wabcut", "MediaReadiness");
    REQUIRE(project);
    REQUIRE(file);
    REQUIRE(playback);
    REQUIRE(readiness);
    // Proxies of earlier runs would be found again through the untitled proxy folder's index: start empty.
    fs::remove_all(readiness->manager()->proxyFolder(), ec);
    readiness->setPolicy(QStringLiteral("auto"));
    readiness->setProxyFormat(QStringLiteral("prores"));
    readiness->setProxyDivisor(2);
    readiness->setPreviewQuality(QStringLiteral("full")); // isolate proxies from reduced-resolution decoding
    readiness->setUseProxies(true);

    // Import: a new project whose sequence matches the clip (2160p60), the clip on V1.
    const QVariantMap imported = file->importFiles({urlOf(dir / "media" / kHeavyClip)});
    REQUIRE(imported.value(QStringLiteral("added")).toInt() == 1);
    REQUIRE(project->mediaItems().size() == 1);
    const QString id = project->mediaItems().front().toMap().value(QStringLiteral("id")).toString();
    REQUIRE(file->dropMedia(id, QStringLiteral("00:00:00:00"), QString(), false));
    REQUIRE(project->currentSequence());
    CHECK(project->currentSequence()->width == 3840);
    CHECK(project->currentSequence()->frameRate == Rate{60, 1});

    // Auto: the trial open finds no hardware decoder for 4:2:2 10-bit H.264 here, so a proxy is queued and made.
    const bool gpuDecodes = [&] {
        auto d = media::VideoDecoder::open(pathToUtf8(dir / "media" / kHeavyClip), -1, media::DecoderOptions{});
        return d && (*d)->isHardwareAccelerated();
    }();
    if (gpuDecodes) {
        SKIP("this GPU decodes H.264 4:2:2 10-bit: Auto makes no proxy");
    }
    const auto stateOf = [&] { return readiness->mediaStates().value(id).toMap(); };
    bool sawProgress = false;
    QImage progressShot;
    QString reason;
    const bool started = waitFor([&] {
        readiness->pollNow();
        const QVariantMap s = stateOf();
        if (s.value(QStringLiteral("state")).toString() != QLatin1String("ready") && reason.isEmpty()) {
            reason = s.value(QStringLiteral("detail")).toString(); // why Auto queued it
        }
        if (s.value(QStringLiteral("state")).toString() == QLatin1String("generating") &&
            s.value(QStringLiteral("progress")).toDouble() >= 0.2 && !sawProgress) {
            sawProgress = true;
            QQuickWindowPrivate::get(window.get())->polishItems();
            progressShot = window->grabWindow(); // the library card mid-generation, "Proxy NN%"
        }
        return s.value(QStringLiteral("state")).toString() == QLatin1String("ready");
    }, 180000);
    INFO("proxy state: " << stateOf().value(QStringLiteral("state")).toString().toStdString() << " "
                         << stateOf().value(QStringLiteral("detail")).toString().toStdString());
    REQUIRE(started);
    CHECK(sawProgress);
    CHECK(readiness->statusText() == QStringLiteral("Proxies 1/1"));
    CHECK(reason == QStringLiteral("H.264 4:2:2 10-bit: no hardware decoder here"));
    const fs::path proxyFile(QDir::fromNativeSeparators(stateOf().value(QStringLiteral("file")).toString()).toStdU16String());
    CHECK(fs::exists(proxyFile));
    CHECK(proxyFile.parent_path().filename() == fs::path("Untitled Proxies"));

    // Playback, 1.6 s at 60 Hz: the originals (software decoding of 4K 4:2:2 10-bit), then the proxies.
    render::OffscreenOptions presenterOptions;
    presenterOptions.role = render::OutputRole::Display;
    presenterOptions.compositor = {render::TexturePool::Preview, 1, 48};
    auto presenter = render::OffscreenRenderer::create(presenterOptions);
    REQUIRE(presenter);
    playback->startAudio();
    REQUIRE(playback->session()->hasSound());

    readiness->setUseProxies(false);
    const PlaybackRun originals = playFromStart(project, playback, readiness, **presenter, 96);
    CHECK(originals.badge == QStringLiteral("Full"));
    // The scheduler blames the clip it waited for: Auto's "dropped frames in playback" evidence.
    std::uint64_t blamed = 0;
    for (const auto& [media, late] : playback->session()->video().scheduler().stats().lateFramesByMedia) {
        blamed += late;
    }
    CHECK(blamed >= editor::kLateFramesForProxy);

    // With proxies: smooth. Always: the proxy is what plays, and pictures come. [perf]: the scheduler never misses a
    // target and nothing is dropped; a busy machine (other builds) can disturb the harness's own refresh pacing, so
    // a second run is made when the first misses.
    readiness->setUseProxies(true);
    QImage playingShot;
    std::vector<PlaybackRun> proxyRuns;
    for (int attempt = 0; attempt < (perf ? 2 : 1); ++attempt) {
        proxyRuns.push_back(playFromStart(project, playback, readiness, **presenter, 96, [&] {
            readiness->pollNow();
            QQuickWindowPrivate::get(window.get())->polishItems();
            playingShot = window->grabWindow(); // the monitor mid-playback with its quality badge
        }));
        const PlaybackRun& r = proxyRuns.back();
        CHECK(r.badge == QStringLiteral("Proxy 1/2"));
        CHECK(r.presentedHere > 0);
        if (r.droppedReadout == 0 && r.dropped == 0 && r.skippedLate == 0 && r.presentedHere >= 85) {
            break;
        }
    }
    const PlaybackRun& proxies = proxyRuns.back();
    if (perf) {
        CHECK(proxies.droppedReadout == 0);
        CHECK(proxies.dropped == 0);
        CHECK(proxies.skippedLate == 0);
        CHECK(proxies.presentedHere >= 85);
    } else {
        recordTiming("proxy playback 2160p60 4:2:2 10-bit", row("proxies", proxies));
    }

    // Report and screenshots.
    const fs::path outDir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e";
    {
        std::ofstream report(outDir / (perf ? "proxy-playback.md" : "proxy-playback-functional.md"), std::ios::trunc);
        report << "# Playback of " << kHeavyClip << " (3840x2160 60p H.264 High 4:2:2 10-bit), with and without proxies\n\n";
        report << "Offscreen 60 Hz render loop (e2e harness), Full preview quality, 96 refreshes from the start of the "
                  "sequence; Auto proxy reason: \"" << reason.toStdString() << "\"; proxy " << proxyFile.filename().string()
               << ".\n\n";
        report << "| Decoding | Badge | Pictures / refreshes | Readout fps | Dropped (readout) | Late targets | "
                  "Superseded | Slowest render |\n|---|---|---|---|---|---|---|---|\n";
        report << row("originals", originals) << "\n";
        for (std::size_t i = 0; i < proxyRuns.size(); ++i) {
            report << row(std::format("proxies (ProRes Proxy 1/2), run {}", i + 1).c_str(), proxyRuns[i]) << "\n";
        }
        report << "\nPictures: new pictures the 60 Hz loop rendered. Dropped (readout): what the monitor's readout "
                  "shows. Late targets: sequence frames the scheduler abandoned because their decode was not ready "
                  "in time. Superseded: composites replaced before the loop took them.\n";
    }
    std::string summary = row("originals", originals);
    for (const PlaybackRun& r : proxyRuns) {
        summary += "\n" + row("proxies", r);
    }
    WARN(summary);
    if (perf) {
        return; // the screenshots belong to the functional run
    }

    const QString shots = QString::fromStdU16String((outDir / "screenshots").u16string());
    QDir().mkpath(shots);
    auto* library = window->findChild<QQuickItem*>(QStringLiteral("libraryPanel"));
    auto* monitorPanel = window->findChild<QQuickItem*>(QStringLiteral("monitorPanel"));
    REQUIRE(library);
    REQUIRE(monitorPanel);
    if (!progressShot.isNull()) {
        CHECK(progressShot.copy(sceneRect(library)).save(shots + QStringLiteral("/library_proxy_progress.png")));
    }
    REQUIRE_FALSE(playingShot.isNull());
    CHECK(playingShot.save(shots + QStringLiteral("/editor_proxy_playback.png")));
    CHECK(playingShot.copy(sceneRect(monitorPanel)).save(shots + QStringLiteral("/monitor_proxy_badge.png")));
    readiness->pollNow();
    QQuickWindowPrivate::get(window.get())->polishItems();
    const QImage ready = window->grabWindow();
    CHECK(ready.copy(sceneRect(library)).save(shots + QStringLiteral("/library_proxy_ready.png")));
}

} // namespace

TEST_CASE("e2e: Auto makes a proxy for an H.264 4:2:2 10-bit clip; playback with and without it",
          "[e2e][screenshot][proxy]") {
    proxyPlayback(false);
}

TEST_CASE("e2e perf: the proxy of the 4:2:2 10-bit clip plays without a dropped frame", "[e2e][proxy][perf]") {
    proxyPlayback(true);
}

TEST_CASE("e2e perf: the 3-track edit plays at 30 fps in real time", "[e2e][perf]") {
    if (!haveMedia()) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    const fs::path dir = freshWorkDir("loop-perf");
    render::OffscreenOptions ro;
    ro.role = render::OutputRole::Display;
    ro.compositor = {render::TexturePool::Preview, 1, 48};
    ro.shareDeviceWithDecoders = true;
    auto renderer = render::OffscreenRenderer::create(ro);
    REQUIRE(renderer);
    Editor editor;
    REQUIRE(editor.file->importFiles(clipUrls(dir)).value(QStringLiteral("added")).toInt() == 3);
    buildSequence(editor.project->session());
    editor.playback->startAudio();
    editor::PlaybackSession& session = *editor.playback->session();
    REQUIRE(session.hasSound());
    playback::PlaybackController& video = *editor.playback->activeController();
    render::CompositeMailbox& mailbox = *video.mailbox();
    std::vector<float> block(800 * 2);
    render::RenderedImage frame;
    // 2 s at 60 Hz of a 30 fps sequence: at least 45 of its 60 pictures, the readout near 30 fps, at most 2 dropped.
    std::vector<std::string> attempts;
    int presented = 0;
    double fps = 0.0;
    int dropped = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        editor.project->setPlayhead({});
        spin(800);
        REQUIRE(waitFor([&] { return video.stats().published > 0; }));
        (void)mailbox.acquire(playback::steadyNowNs());
        editor.playback->play();
        presented = 0;
        const auto start = std::chrono::steady_clock::now();
        for (std::int64_t k = 0; k < 120; ++k) {
            sleepUntilPrecisely(start + std::chrono::nanoseconds(k * 16'666'667LL));
            pullAudio(session, block);
            QCoreApplication::processEvents();
            const auto selection = mailbox.acquire(playback::steadyNowNs() + 16'666'667LL);
            if (selection.changed && selection.composite) {
                REQUIRE((*renderer)->render(selection.composite, frame, QSize(640, 360)));
                ++presented;
            }
        }
        fps = editor.playback->fpsText().section(QLatin1Char(' '), 0, 0).toDouble();
        dropped = editor.playback->droppedFrames();
        editor.playback->pause();
        spin(100);
        attempts.push_back(std::format("attempt {}: {} of 60 pictures, readout {:.1f} fps, {} dropped", attempt + 1,
                                       presented, fps, dropped));
        if (presented >= 45 && fps >= 25.0 && fps <= 35.0 && dropped <= 2) {
            break;
        }
    }
    std::string all;
    for (const std::string& a : attempts) {
        all += a + "; ";
    }
    INFO(all);
    CHECK(presented >= 45);
    CHECK(fps >= 25.0);
    CHECK(fps <= 35.0);
    CHECK(dropped <= 2);
}

// ---- I.4: burned-in captions, the recovery dashboard, Collect Files ---------------------------------------------

TEST_CASE("e2e: captions drawn in the monitor, the recovery dashboard, a collected and verified capsule",
          "[e2e][screenshot][finishing]") {
    if (!haveMedia()) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    const fs::path dir = freshWorkDir("finishing");
    qputenv("WABCUT_ZERO_COPY", "0"); // the grabs render with a QRhi of their own: CPU frames
    struct RestoreZeroCopy {
        ~RestoreZeroCopy() { qunsetenv("WABCUT_ZERO_COPY"); }
    } restoreZeroCopy;

    QQmlEngine engine;
    QQmlComponent component(&engine, "Wabcut", "Main");
    INFO(component.errorString().toStdString());
    REQUIRE_FALSE(component.isError());
    std::unique_ptr<QQuickWindow> window(
        qobject_cast<QQuickWindow*>(component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
    REQUIRE(window);
    window->create();
    window->resize(1600, 960);
    window->contentItem()->setSize(QSizeF(1600, 960));
    auto* project = engine.singletonInstance<app::ProjectController*>("Wabcut", "ProjectController");
    auto* file = engine.singletonInstance<app::ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* playback = engine.singletonInstance<app::PlaybackBridge*>("Wabcut", "Playback");
    auto* collect = engine.singletonInstance<app::CollectFilesBridge*>("Wabcut", "Collect");
    REQUIRE(project);
    REQUIRE(file);
    REQUIRE(playback);
    REQUIRE(collect);
    // A window that is never shown does not run animations: no motion, so tab highlights sit where they belong.
    if (QObject* theme = engine.singletonInstance<QObject*>("Wabcut", "Theme")) {
        theme->setProperty("reduceMotion", true);
    }

    // A real project: two clips on the timeline, saved.
    REQUIRE(file->importFiles(clipUrls(dir)).value(QStringLiteral("added")).toInt() == 3);
    QStringList ids;
    for (const QVariant& v : project->mediaItems()) {
        ids.append(v.toMap().value(QStringLiteral("id")).toString());
    }
    REQUIRE(file->dropMedia(ids[0], QStringLiteral("00:00:00:00"), QString(), false));
    REQUIRE(file->dropMedia(ids[1], QStringLiteral("00:00:04:00"), QString(), false));
    const fs::path projectFile = dir / "finishing.wabcut";
    REQUIRE(file->saveAs(urlOf(projectFile)));

    // Captions: an English and a Thai cue, drawn by the compositor with the Captions panel's style, karaoke on.
    const SequenceId seq = project->currentSequence()->id;
    REQUIRE(project->session().apply("Add captions", [&](const ProjectState& s) {
        CaptionTrack t; // no id: a new track
        t.name = "English";
        t.language = "en";
        t.enabled = true;
        t.cues = {{Ticks::fromSeconds(1), Ticks::fromSeconds(2), "Captions are burned in, frame for frame"},
                  {Ticks::fromSeconds(3), Ticks::fromSeconds(2), "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb8\x9a"}};
        return edit::setCaptionTrack(s, seq, t);
    }));
    playback->setCaptionKaraoke(true);
    REQUIRE(waitFor([&] { return playback->captionsShown(); }, 5000));
    REQUIRE(playback->captionSettings()->karaoke);

    auto* monitor = window->findChild<app::ProgramMonitor*>(QStringLiteral("programMonitor"));
    auto* monitorPanel = window->findChild<QQuickItem*>(QStringLiteral("monitorPanel"));
    REQUIRE(monitor);
    REQUIRE(monitorPanel);
    project->setPlayhead(Ticks::fromSeconds(2));
    spin(600); // the paused frame is decoded and published
    // A window that is never shown renders only when grabbed: the first grab takes the composite, the second shows it.
    QQuickWindowPrivate::get(window.get())->polishItems();
    (void)window->grabWindow();
    spin(100);
    QQuickWindowPrivate::get(window.get())->polishItems();
    const QImage captioned = window->grabWindow();
    CHECK(monitor->hasFrame());
    REQUIRE_FALSE(captioned.isNull());
    const QString shots = QString::fromStdU16String((fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / "screenshots").u16string());
    QDir().mkpath(shots);
    CHECK(captioned.copy(sceneRect(monitorPanel)).save(shots + QStringLiteral("/monitor_captions.png")));
    // The caption is in the picture: the same frame with the captions hidden (Captions.overlay off) differs only where
    // the caption is drawn - a box in the lower part of the picture, with the karaoke highlight colour (the words already
    // spoken) on some of its letters and white on the rest. (The test pattern itself has orange-ish pixels along its
    // diagonal, so a colour count over the whole monitor does not tell whether the caption is there.)
    QObject* captionsBridge = engine.singletonInstance<QObject*>("Wabcut", "Captions");
    REQUIRE(captionsBridge);
    REQUIRE(captionsBridge->setProperty("overlay", false));
    REQUIRE(waitFor([&] { return !playback->captionsShown(); }, 5000));
    spin(600); // the paused frame is composited again without them
    QQuickWindowPrivate::get(window.get())->polishItems();
    (void)window->grabWindow();
    spin(100);
    QQuickWindowPrivate::get(window.get())->polishItems();
    const QImage plain = window->grabWindow();
    REQUIRE_FALSE(plain.isNull());
    CHECK(plain.copy(sceneRect(monitorPanel)).save(shots + QStringLiteral("/monitor_captions_off.png")));
    {
        const QImage withCaption = captioned.copy(sceneRect(monitor));
        const QImage without = plain.copy(sceneRect(monitor));
        REQUIRE(withCaption.size() == without.size());
        int changed = 0;
        int highlighted = 0; // the karaoke highlight (255, 209, 64), anti-aliased against the caption's box
        int white = 0;       // the words still to come
        QRect box;
        for (int y = 0; y < withCaption.height(); ++y) {
            for (int x = 0; x < withCaption.width(); ++x) {
                const QColor c = withCaption.pixelColor(x, y);
                const QColor o = without.pixelColor(x, y);
                if (std::abs(c.red() - o.red()) + std::abs(c.green() - o.green()) + std::abs(c.blue() - o.blue()) < 48) {
                    continue;
                }
                ++changed;
                box = box.united(QRect(x, y, 1, 1));
                highlighted += c.red() > 170 && c.green() > 130 && c.blue() < 140 && c.red() - c.blue() > 80 ? 1 : 0;
                white += c.red() > 190 && c.green() > 190 && c.blue() > 190 ? 1 : 0;
            }
        }
        INFO("changed " << changed << " in " << box.x() << "," << box.y() << " " << box.width() << "x" << box.height()
                        << " of " << withCaption.width() << "x" << withCaption.height() << "; highlighted " << highlighted
                        << " white " << white);
        CHECK(changed > 300);                                   // a caption box, not a few stray pixels
        CHECK(box.center().y() > withCaption.height() / 2);     // in the lower part of the picture
        CHECK(box.width() > box.height() * 3);                  // a line of text
        CHECK(highlighted > 10);
        CHECK(white > 5);
    }
    REQUIRE(captionsBridge->setProperty("overlay", true));
    REQUIRE(waitFor([&] { return playback->captionsShown(); }, 5000));
    project->setPlayhead(Ticks::fromSeconds(4));
    spin(800);
    QQuickWindowPrivate::get(window.get())->polishItems();
    (void)window->grabWindow();
    spin(100);
    QQuickWindowPrivate::get(window.get())->polishItems();
    CHECK(window->grabWindow().copy(sceneRect(monitorPanel)).save(shots + QStringLiteral("/monitor_captions_thai.png")));

    // Recovery dashboard: two more saves (backups), an autosave, then File > Recover…
    REQUIRE(file->dropMedia(ids[2], QStringLiteral("00:00:06:00"), QString(), false));
    REQUIRE(file->save());
    REQUIRE(file->dropMedia(ids[0], QStringLiteral("00:00:08:00"), QString(), false));
    REQUIRE(file->save());
    REQUIRE(file->dropMedia(ids[1], QStringLiteral("00:00:10:00"), QString(), false));
    REQUIRE(file->autosaveNow());
    auto* recovery = window->findChild<QObject*>(QStringLiteral("recoveryDialog"));
    REQUIRE(recovery);
    REQUIRE(QMetaObject::invokeMethod(recovery, "openDashboard"));
    REQUIRE(waitFor([&] {
        if (file->recoveryScanning()) {
            return false;
        }
        int posters = 0;
        for (const QVariant& v : file->recoveryVersions()) {
            posters += v.toMap().value(QStringLiteral("poster")).toString().isEmpty() ? 0 : 1;
        }
        return posters >= 3;
    }, 60000));
    const QVariantList versions = file->recoveryVersions();
    REQUIRE(versions.size() >= 4);
    CHECK(versions[0].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("current"));
    CHECK(versions[1].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("autosave"));
    CHECK_FALSE(versions[1].toMap().value(QStringLiteral("comparison")).toString().isEmpty());
    // A window that is never shown creates list delegates only when polished: lay it out until the posters show.
    const bool postersShown = waitFor([&] {
        QQuickWindowPrivate::get(window.get())->polishItems();
        std::vector<QQuickItem*> items; // delegates are items of the tree, not QObject children
        collectItems(window->contentItem(), items);
        int ready = 0;
        for (QQuickItem* image : items) {
            if (image->objectName() == QLatin1String("recoveryPoster")) {
                ready += image->property("status").toInt() == 1 ? 1 : 0; // Image.Ready
            }
        }
        return ready >= 3;
    }, 10000);
    {
        QStringList states;
        std::vector<QQuickItem*> items;
        collectItems(window->contentItem(), items);
        for (QQuickItem* image : items) {
            if (image->objectName() == QLatin1String("recoveryPoster")) {
                states << QStringLiteral("%1:%2").arg(image->property("status").toInt()).arg(image->property("source").toString().right(40));
            }
        }
        INFO("poster images: " << states.join(QStringLiteral(" | ")).toStdString());
        CHECK(postersShown);
    }
    QQuickWindowPrivate::get(window.get())->polishItems();
    CHECK(window->grabWindow().save(shots + QStringLiteral("/recovery_dashboard.png")));
    QMetaObject::invokeMethod(recovery, "close");
    spin(200);

    // Collect Files: a trimmed capsule, verified by reopening it elsewhere.
    REQUIRE(file->save());
    auto* loader = window->findChild<QObject*>(QStringLiteral("collectFilesDialogLoader"));
    REQUIRE(loader);
    loader->setProperty("active", true);
    QObject* dialogItem = qvariant_cast<QObject*>(loader->property("item"));
    REQUIRE(dialogItem);
    const QUrl capsules = urlOf(dir / "capsules");
    QDir().mkpath(capsules.toLocalFile());
    dialogItem->setProperty("folder", capsules);
    REQUIRE(QMetaObject::invokeMethod(dialogItem, "open"));
    spin(100);
    // As the user would: used ranges + handles, then the dialog's own options.
    QQuickWindowPrivate::get(window.get())->polishItems();
    QObject* trimTabs = nullptr;
    {
        std::vector<QQuickItem*> items;
        collectItems(window->contentItem(), items);
        for (QQuickItem* item : items) {
            if (item->objectName() == QLatin1String("collectTrimTabs")) {
                trimTabs = item;
            }
        }
    }
    REQUIRE(trimTabs);
    trimTabs->setProperty("currentIndex", 1);
    QVariant options;
    REQUIRE(QMetaObject::invokeMethod(dialogItem, "options", Q_RETURN_ARG(QVariant, options)));
    REQUIRE(options.toMap().value(QStringLiteral("trim")).toBool());
    REQUIRE(collect->start(capsules, options.toMap()));
    REQUIRE(collect->waitFinished(180000));
    const QVariantMap result = collect->result();
    INFO("collect: " << result.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("verified")).toBool());
    CHECK(result.value(QStringLiteral("offline")).toInt() == 0);
    CHECK(result.value(QStringLiteral("trimmed")).toInt() == 3);
    CHECK(fs::is_regular_file(fs::path(result.value(QStringLiteral("folderPath")).toString().toStdU16String()) / "manifest.json"));
    spin(200);
    QQuickWindowPrivate::get(window.get())->polishItems();
    CHECK(window->grabWindow().save(shots + QStringLiteral("/collect_files.png")));
    {
        std::ofstream report(fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / "capsule-roundtrip.md", std::ios::trunc);
        report << "# Collect Files round trip (e2e)\n\n"
               << "Capsule: " << result.value(QStringLiteral("folderPath")).toString().toStdString() << "\n\n"
               << "- verified (reopened in a temporary folder, absolute paths cleared): "
               << (result.value(QStringLiteral("verified")).toBool() ? "yes" : "no") << "\n"
               << "- offline clips after reopening: " << result.value(QStringLiteral("offline")).toInt() << "\n"
               << "- fingerprint mismatches: " << result.value(QStringLiteral("mismatches")).toInt() << "\n"
               << "- files: " << result.value(QStringLiteral("files")).toInt() << ", media "
               << result.value(QStringLiteral("media")).toInt() << " (trimmed "
               << result.value(QStringLiteral("trimmed")).toInt() << "), "
               << result.value(QStringLiteral("size")).toString().toStdString() << "\n";
    }
}

// ---- I.5: the Color workspace ----------------------------------------------------------------------------------

namespace {

constexpr const char* kGradeClip = "e2e_1080p60.mp4"; // 1920x1080 60 fps H.264, 4 s

/// A full grade: every stage at work (input and look LUTs built in, curves, wheels, HSL with refine, vignette).
EffectInstance fullGrade(IdGenerator& ids) {
    EffectInstance e;
    e.id = ids.allocate<EffectInstanceId>();
    e.effect = "wabcut.color.grade";
    for (const auto& [key, value] : std::initializer_list<std::pair<const char*, double>>{
             {"basic.temperature", 18.0}, {"basic.exposure", 0.25}, {"basic.contrast", 15.0}, {"basic.highlights", -25.0},
             {"basic.shadows", 15.0}, {"basic.saturation", 112.0}, {"basic.vibrance", 20.0}, {"creative.lookIntensity", 70.0},
             {"creative.fadedFilm", 10.0}, {"creative.shadowTint.amount", 20.0}, {"wheels.lift.blue", 0.02},
             {"wheels.gain.red", 1.04}, {"wheels.saturation", 1.05}, {"hsl.hue.center", 30.0}, {"hsl.hue.width", 50.0},
             {"hsl.hue.softness", 20.0}, {"hsl.denoise", 10.0}, {"hsl.blur", 4.0}, {"hsl.saturation", 125.0},
             {"vignette.amount", -0.8}}) {
        e.set(key, ParamValue(value));
    }
    e.setData(EffectData{"basic.inputLut", std::string("builtin:vlog-vgamut-to-rec709")});
    e.setData(EffectData{"creative.look", std::string("builtin:teal-orange")});
    e.setData(EffectData{"curves.master", std::vector<EffectPoint>{{0, 0.02}, {0.5, 0.53}, {1, 0.98}}});
    e.setData(EffectData{"curves.hueSat", std::vector<EffectPoint>{{0.0, 0.5}, {0.33, 0.7}, {0.66, 0.45}}});
    return e;
}

struct FrameTimes {
    std::vector<double> ms; ///< per new picture: render (+ scopes when on), wall clock
    std::vector<double> scopeMs; ///< per scope pass: record + run + read back, wall clock
    int pictures = 0;
    int scopePasses = 0;
    std::uint64_t dropped = 0;
    std::uint64_t skippedLate = 0;
    static double quantile(std::vector<double> s, double q) {
        if (s.empty()) {
            return 0.0;
        }
        std::sort(s.begin(), s.end());
        return s[std::min(s.size() - 1, static_cast<std::size_t>(q * static_cast<double>(s.size())))];
    }
    double p(double q) const { return quantile(ms, q); }
    double scopeP(double q) const { return quantile(scopeMs, q); }
};

/// Waits until the GPU has finished everything submitted so far: QRhi::finish() submits but (on D3D11) does not wait,
/// while reading a small buffer back does (the copy queues behind all earlier work).
class GpuFence {
public:
    explicit GpuFence(QRhi* rhi) : m_rhi(rhi) {
        m_buffer.reset(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, 16));
        REQUIRE(m_buffer->create());
    }
    void wait() {
        QRhiCommandBuffer* cb = nullptr;
        REQUIRE(m_rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess);
        QRhiResourceUpdateBatch* batch = m_rhi->nextResourceUpdateBatch();
        QRhiReadbackResult result;
        batch->readBackBuffer(m_buffer.get(), 0, 16, &result);
        cb->resourceUpdate(batch);
        REQUIRE(m_rhi->endOffscreenFrame() == QRhi::FrameOpSuccess);
    }

private:
    QRhi* m_rhi;
    std::unique_ptr<QRhiBuffer> m_buffer;
};

/// Plays the sequence for `refreshes` 60 Hz refreshes of the offscreen loop (the monitor's render thread: take the due
/// composite and render it), recording GPU scopes after every other picture when `scopes` (the monitor's ~30 Hz).
/// Every picture is waited for on the GPU, so a frame time is CPU + GPU in series: an upper bound of the monitor's,
/// where the two overlap.
FrameTimes playMeasured(app::ProjectController* project, app::PlaybackBridge* playback, render::OffscreenRenderer& presenter,
                        int refreshes, bool scopes) {
    project->setPlayhead({});
    spin(1200);
    playback::PlaybackController* controller = playback->activeController();
    REQUIRE(controller);
    const playback::PlaybackStats before = controller->stats();
    playback->play();
    render::CompositeMailbox& mailbox = *controller->mailbox();
    std::vector<float> block(800 * 2);
    render::GpuScopes gpuScopes(presenter.rhi());
    GpuFence fence(presenter.rhi());
    FrameTimes out;
#if defined(_WIN32)
    const int priority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    struct RestorePriority {
        int value;
        ~RestorePriority() { SetThreadPriority(GetCurrentThread(), value); }
    } restorePriority{priority};
#endif
    const auto start = std::chrono::steady_clock::now();
    for (int k = 0; k < refreshes; ++k) {
        sleepUntilPrecisely(start + std::chrono::nanoseconds(k * 16'666'667LL));
        pullAudio(*playback->session(), block);
        QCoreApplication::processEvents();
        const auto selection = mailbox.acquire(playback::steadyNowNs() + 16'666'667LL);
        if (!selection.changed || !selection.composite) {
            continue;
        }
        QElapsedTimer t;
        t.start();
        const auto timed = presenter.renderTimed(selection.composite, QSize(1920, 1080));
        REQUIRE(timed);
        fence.wait();
        if (scopes && out.pictures % 2 == 0) {
            QElapsedTimer scopeTimer;
            scopeTimer.start();
            QRhiCommandBuffer* cb = nullptr;
            REQUIRE(presenter.rhi()->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess);
            bool delivered = false;
            const auto recorded = gpuScopes.record(cb, presenter.compositor().canvasTexture(), timed->composite.canvasPixels,
                                                   render::OutputTransform::Rec709, color::ScopeSettings{},
                                                   [&](color::ScopeData) { delivered = true; });
            REQUIRE(presenter.rhi()->endOffscreenFrame() == QRhi::FrameOpSuccess);
            out.scopePasses += recorded && delivered ? 1 : 0;
            out.scopeMs.push_back(static_cast<double>(scopeTimer.nsecsElapsed()) / 1e6);
        }
        out.ms.push_back(static_cast<double>(t.nsecsElapsed()) / 1e6);
        ++out.pictures;
    }
    const playback::PlaybackStats after = controller->stats();
    out.dropped = after.dropped - before.dropped;
    out.skippedLate = after.skippedLate - before.skippedLate;
    playback->pause();
    spin(100);
    return out;
}

std::string timesRow(const char* what, const FrameTimes& f) {
    const std::string scope = f.scopeMs.empty() ? std::string("-") : std::format("{:.2f} / {:.2f} ms", f.scopeP(0.5), f.scopeP(0.99));
    return std::format("| {} | {} | {} | {:.2f} ms | {:.2f} ms | {:.2f} ms | {} | {} | {} |", what, f.pictures, f.scopePasses,
                       f.p(0.5), f.p(0.99), f.p(1.0), scope, f.dropped, f.skippedLate);
}

} // namespace

TEST_CASE("e2e: the Color workspace - grade, scopes, before/after, reference, Auto; scopes stop outside it",
          "[e2e][screenshot][color]") {
    if (!haveMedia()) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    const fs::path dir = freshWorkDir("color");
    qputenv("WABCUT_ZERO_COPY", "0"); // the grabs render with a QRhi of their own: CPU frames
    struct RestoreZeroCopy {
        ~RestoreZeroCopy() { qunsetenv("WABCUT_ZERO_COPY"); }
    } restoreZeroCopy;

    QQmlEngine engine;
    QQmlComponent component(&engine, "Wabcut", "Main");
    INFO(component.errorString().toStdString());
    REQUIRE_FALSE(component.isError());
    std::unique_ptr<QQuickWindow> window(
        qobject_cast<QQuickWindow*>(component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
    REQUIRE(window);
    window->create();
    window->resize(1600, 960);
    window->contentItem()->setSize(QSizeF(1600, 960));
    auto* project = engine.singletonInstance<app::ProjectController*>("Wabcut", "ProjectController");
    auto* file = engine.singletonInstance<app::ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* playback = engine.singletonInstance<app::PlaybackBridge*>("Wabcut", "Playback");
    auto* color = engine.singletonInstance<app::ColorBridge*>("Wabcut", "Color");
    auto* scopes = engine.singletonInstance<app::ScopesBridge*>("Wabcut", "Scopes");
    REQUIRE(project);
    REQUIRE(file);
    REQUIRE(playback);
    REQUIRE(color);
    REQUIRE(scopes);
    QQuickWindowPrivate::get(window.get())->polishItems();

    REQUIRE(file->importFiles(clipUrls(dir)).value(QStringLiteral("added")).toInt() == 3);
    QStringList ids;
    for (const QVariant& v : project->mediaItems()) {
        ids.append(v.toMap().value(QStringLiteral("id")).toString());
    }
    REQUIRE(file->dropMedia(ids[0], QStringLiteral("00:00:00:00"), QString(), false));
    REQUIRE(file->dropMedia(ids[1], QStringLiteral("00:00:04:00"), QString(), false));
    const fs::path projectFile = dir / "color.wabcut";
    REQUIRE(file->saveAs(urlOf(projectFile)));
    const auto grab = [&] {
        QQuickWindowPrivate::get(window.get())->polishItems();
        return window->grabWindow();
    };

    // The Color workspace: the scopes panel shows, so the program monitor computes scopes.
    QMetaObject::invokeMethod(window.get(), "openPreset", Q_ARG(QString, QStringLiteral("color")));
    spin(600); // the cross-fade (reduced motion is off here)
    auto* scopesPanel = window->findChild<QQuickItem*>(QStringLiteral("scopesPanel"));
    auto* colorPanel = window->findChild<QQuickItem*>(QStringLiteral("colorPanel"));
    auto* monitorPanel = window->findChild<QQuickItem*>(QStringLiteral("monitorPanel"));
    auto* monitor = window->findChild<app::ProgramMonitor*>(QStringLiteral("programMonitor"));
    REQUIRE(scopesPanel);
    REQUIRE(colorPanel);
    REQUIRE(monitorPanel);
    REQUIRE(monitor);
    CHECK(scopesPanel->isVisible());
    CHECK(scopes->active());
    CHECK(app::ScopeHub::shared().wanted());

    // Grade the clip under the playhead through the Color panel's bridge: one undo step per gesture.
    // The panel follows the selection (else the clip under the playhead): select clip A, under the playhead at 1 s.
    project->setPlayhead(Ticks::fromSeconds(1));
    const QString clipA = app::clipKey(project->currentSequence()->videoTracks.front()->clips.front().id);
    project->selectClip(clipA, 0, true);
    REQUIRE(waitFor([&] { return color->clip() == clipA; }, 3000));
    const std::size_t undoBefore = project->session().undoCount();
    color->beginGesture();
    for (const double v : {0.2, 0.4, 0.6}) {
        REQUIRE(color->setParam(QStringLiteral("basic.exposure"), v)); // a drag: three moves, one step
    }
    color->endGesture();
    REQUIRE(color->hasGrade());
    CHECK(color->params().value(QStringLiteral("basic.exposure")).toDouble() == 0.6);
    REQUIRE(color->setParam(QStringLiteral("basic.temperature"), 35));
    REQUIRE(color->setCurve(QStringLiteral("master"), {QVariantList{0.0, 0.0}, QVariantList{0.5, 0.58}, QVariantList{1.0, 1.0}}));
    REQUIRE(color->setLut(QStringLiteral("look"), QStringLiteral("builtin:teal-orange")));
    REQUIRE(color->setParams({{QStringLiteral("wheels.gain.red"), 1.08}, {QStringLiteral("wheels.gain.green"), 1.0},
                              {QStringLiteral("wheels.gain.blue"), 0.94}}));
    CHECK(color->look() == QStringLiteral("builtin:teal-orange"));
    CHECK(color->curves().value(QStringLiteral("master")).toList().size() == 3);
    // Add grade + exposure drag (merged: one step) + temperature + curve + look + wheel.
    CHECK(project->session().undoCount() - undoBefore <= 6);
    CHECK(project->session().undoCount() - undoBefore >= 5);

    // The monitor with its scopes: render, then a frame for the readback, then the panel repaints.
    REQUIRE(waitFor([&] {
        grab();
        return monitor->hasFrame();
    }, 8000));
    const int generationBefore = scopes->generation();
    REQUIRE(waitFor([&] {
        grab();
        return scopes->generation() > generationBefore;
    }, 8000));
    spin(100);
    const QImage workspace = grab();
    CHECK(scopes->stats().value(QStringLiteral("pixels")).toULongLong() > 0u);
    const QString shots = QString::fromStdU16String((fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / "screenshots").u16string());
    QDir().mkpath(shots);
    {
        // The views draw the traces, not just their grids: bright pixels (grid lines are ~62/255 over the surround,
        // traces at least ~90) inside each plot, away from the labels; the vectorscope's trace is coloured.
        const auto count = [&](const char* name, QMargins inset, const std::function<bool(QColor)>& lit) {
            auto* view = window->findChild<QQuickItem*>(QString::fromLatin1(name));
            REQUIRE(view);
            const QImage crop = workspace.copy(sceneRect(view).marginsRemoved(inset));
            int n = 0;
            for (int y = 0; y < crop.height(); ++y) {
                for (int x = 0; x < crop.width(); ++x) {
                    n += lit(crop.pixelColor(x, y)) ? 1 : 0;
                }
            }
            return n;
        };
        const auto bright = [](QColor c) { return std::max({c.red(), c.green(), c.blue()}) >= 85; };
        const auto coloured = [](QColor c) {
            const int hi = std::max({c.red(), c.green(), c.blue()}), lo = std::min({c.red(), c.green(), c.blue()});
            return hi >= 80 && hi - lo >= 40;
        };
        const int wave = count("waveformView", QMargins(38, 20, 8, 8), bright);
        const int vector = count("vectorscopeView", QMargins(24, 24, 24, 24), coloured);
        const int histogram = count("histogramView", QMargins(6, 6, 6, 18), bright);
        INFO("trace pixels: waveform " << wave << ", vectorscope " << vector << ", histogram " << histogram);
        CHECK(wave >= 300);
        CHECK(vector >= 600);
        CHECK(histogram >= 200);
    }
    CHECK(workspace.save(shots + QStringLiteral("/color_workspace.png")));
    CHECK(workspace.copy(sceneRect(scopesPanel)).save(shots + QStringLiteral("/color_scopes.png")));
    CHECK(workspace.copy(sceneRect(colorPanel)).save(shots + QStringLiteral("/color_panel.png")));

    // Graded vs ungraded: the same frame with the grade off, side by side.
    const QImage graded = workspace.copy(sceneRect(monitor));
    REQUIRE(color->setGradeEnabled(false));
    spin(300);
    grab();
    spin(200);
    const QImage ungraded = grab().copy(sceneRect(monitor));
    REQUIRE(color->setGradeEnabled(true));
    {
        QImage pair(graded.width() * 2 + 8, graded.height(), QImage::Format_RGB32);
        pair.fill(Qt::black);
        QPainter p(&pair);
        p.drawImage(0, 0, ungraded);
        p.drawImage(graded.width() + 8, 0, graded);
        p.end();
        CHECK(pair.save(shots + QStringLiteral("/color_graded_vs_ungraded.png")));
    }
    const auto meanLuma = [](const QImage& im) {
        double sum = 0;
        int n = 0;
        for (int y = im.height() / 4; y < im.height() * 3 / 4; y += 4) {
            for (int x = im.width() / 4; x < im.width() * 3 / 4; x += 4) {
                const QColor c = im.pixelColor(x, y);
                sum += 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF();
                ++n;
            }
        }
        return n ? sum / n : 0.0;
    };
    CHECK(meanLuma(graded) > meanLuma(ungraded) + 0.03); // +0.6 stops and a lifted curve

    // Before / after split at the middle: the left half is the ungraded picture.
    color->setCompareMode(1);
    color->setSplitPosition(0.5);
    spin(300);
    grab();
    spin(200);
    const QImage split = grab();
    CHECK(split.copy(sceneRect(monitorPanel)).save(shots + QStringLiteral("/color_before_after.png")));
    {
        const QImage m = split.copy(sceneRect(monitor));
        const QImage left = m.copy(0, 0, m.width() / 2 - 4, m.height());
        const QImage right = m.copy(m.width() / 2 + 4, 0, m.width() / 2 - 4, m.height());
        const QImage ungradedLeft = ungraded.copy(0, 0, ungraded.width() / 2 - 4, ungraded.height());
        const QImage gradedRight = graded.copy(graded.width() / 2 + 4, 0, graded.width() / 2 - 4, graded.height());
        const QImage ungradedRight = ungraded.copy(ungraded.width() / 2 + 4, 0, ungraded.width() / 2 - 4, ungraded.height());
        CHECK(std::abs(meanLuma(left) - meanLuma(ungradedLeft)) < 0.02);  // before
        CHECK(std::abs(meanLuma(right) - meanLuma(gradedRight)) < 0.02);  // after
        CHECK(meanLuma(gradedRight) > meanLuma(ungradedRight) + 0.02);
    }

    // A reference frame side by side (clip B at 5 s), then Auto on clip A.
    project->setPlayhead(Ticks::fromSeconds(5));
    spin(400);
    REQUIRE(color->setReference());
    CHECK(color->hasReference());
    project->setPlayhead(Ticks::fromSeconds(1));
    color->setCompareMode(2);
    spin(300);
    grab();
    spin(200);
    CHECK(grab().copy(sceneRect(monitorPanel)).save(shots + QStringLiteral("/color_reference.png")));
    color->setCompareMode(0);
    CHECK(color->autoColor());
    CHECK(color->status().startsWith(QStringLiteral("Auto")));
    CHECK(color->matchToReference(0.8, true));

    // Outside the Color workspace the scopes stop entirely: no compute, no readback, while the monitor keeps drawing.
    QMetaObject::invokeMethod(window.get(), "openPreset", Q_ARG(QString, QStringLiteral("edit")));
    spin(600);
    CHECK_FALSE(scopes->active());
    CHECK_FALSE(app::ScopeHub::shared().wanted());
    const int idle = scopes->generation();
    for (int i = 0; i < 6; ++i) {
        project->setPlayhead(Ticks::fromSeconds(1) + Ticks{i * kTicksPerSecond / 30});
        spin(80);
        grab(); // the monitor renders every time
    }
    CHECK(scopes->generation() == idle);
    QMetaObject::invokeMethod(window.get(), "openPreset", Q_ARG(QString, QStringLiteral("color")));
    spin(600);
    REQUIRE(waitFor([&] {
        grab();
        return scopes->generation() > idle;
    }, 8000));
    QMetaObject::invokeMethod(window.get(), "openPreset", Q_ARG(QString, QStringLiteral("edit")));
    spin(600);
    CHECK_FALSE(scopes->active());

    // A LUT the grade uses goes offline: the relink dialog lists it with the media and relinks it.
    const fs::path looks = dir / "looks";
    fs::create_directories(looks);
    {
        std::ofstream cube(looks / "my_look.cube", std::ios::binary);
        cube << color::writeCube(color::identityLut3d(5));
    }
    const QString imported = color->importLut(urlOf(looks / "my_look.cube"), QStringLiteral("look"));
    INFO(imported.toStdString());
    CHECK(imported.isEmpty());
    const QString reference = color->look();
    REQUIRE(reference.startsWith(QStringLiteral("asset:")));
    REQUIRE(file->save());
    fs::rename(looks, dir / "moved");
    // File > Revert reads the saved project again (opening the project that is already open leaves it as it is).
    REQUIRE(file->revert());
    spin(300);
    CHECK(file->offlineCount() == 1);
    const QVariantList offline = file->offlineMedia();
    REQUIRE(offline.size() == 1);
    CHECK(offline.front().toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("lut"));
    CHECK(offline.front().toMap().value(QStringLiteral("id")).toString() == reference);
    CHECK(file->relink(reference, urlOf(dir / "moved" / "my_look.cube"), false));
    CHECK(file->offlineCount() == 0);
    CHECK(file->offlineMedia().isEmpty());
}

namespace {

/// Three stacked graded 1080p60 layers played through the offscreen 60 Hz loop, without and with scopes. `perf`: the
/// strict frame budget (a [perf] case); otherwise only what must happen is asserted and the numbers are recorded.
void gradedStackPlayback(bool perf) {
    const fs::path clip = fs::path(WABCUT_E2E_MEDIA_DIR) / kGradeClip;
    if (!fs::exists(clip)) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / (perf ? "color-playback-perf" : "color-playback");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "media");
    QList<QUrl> urls;
    for (const char* name : {"layer1.mp4", "layer2.mp4", "layer3.mp4"}) {
        fs::copy_file(clip, dir / "media" / name);
        urls.append(urlOf(dir / "media" / name));
    }
    Editor editor;
    REQUIRE(editor.file->importFiles(urls).value(QStringLiteral("added")).toInt() == 3);
    QStringList ids;
    for (const QVariant& v : editor.project->mediaItems()) {
        ids.append(v.toMap().value(QStringLiteral("id")).toString());
    }
    // V1, V2, V3: the same time, the upper two at 60 % opacity (nothing occludes, all three decode).
    REQUIRE(editor.file->dropMedia(ids[0], QStringLiteral("00:00:00:00"), QString(), false));
    EditSession& session = editor.project->session();
    const SequenceId seq = editor.project->currentSequence()->id;
    REQUIRE(editor.project->currentSequence()->frameRate == Rate{60, 1});
    {
        auto r = session.apply("Stack", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
            ProjectState next = s;
            for (int i = 0; i < 2; ++i) {
                auto track = edit::addTrack(next, seq, TrackKind::Video);
                if (!track) {
                    return std::unexpected(track.error());
                }
                next = std::move(track->state);
            }
            return next;
        });
        REQUIRE(r);
    }
    const auto& videoTracks = editor.project->currentSequence()->videoTracks;
    REQUIRE(videoTracks.size() >= 3);
    for (int i = 1; i <= 2; ++i) {
        REQUIRE(editor.file->dropMedia(ids[i], QStringLiteral("00:00:00:00"), app::trackKey(videoTracks[static_cast<std::size_t>(i)]->id), true));
    }
    const auto gradeAll = [&](bool grade) {
        auto r = session.apply("Grades", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
            ProjectState next = s;
            auto sequence = std::make_shared<Sequence>(*next.findSequence(seq));
            for (std::size_t t = 0; t < sequence->videoTracks.size(); ++t) {
                auto track = std::make_shared<Track>(*sequence->videoTracks[t]);
                for (std::size_t c = 0; c < track->clips.size(); ++c) {
                    Clip& clipRef = track->clips.mutableAt(c);
                    clipRef.setEffects(grade ? EffectStack{fullGrade(next.ids)} : EffectStack{});
                    if (t > 0) {
                        ClipProperties p = clipRef.properties();
                        p[ParamId::Opacity] = ParamValue(0.6);
                        clipRef.setProperties(p);
                    }
                }
                sequence->videoTracks[t] = std::move(track);
            }
            return next.withSequence(std::move(sequence));
        });
        REQUIRE(r);
    };
    render::OffscreenOptions presenterOptions;
    presenterOptions.role = render::OutputRole::Display;
    presenterOptions.compositor = {render::TexturePool::Preview, 1, 48};
    auto presenter = render::OffscreenRenderer::create(presenterOptions);
    REQUIRE(presenter);
    editor.playback->startAudio();

    const int refreshes = perf ? 180 : 60;
    gradeAll(false);
    const FrameTimes plain = playMeasured(editor.project, editor.playback, **presenter, refreshes, false);
    gradeAll(true);
    // The first graded pictures build the built-in LUTs (65^3 input, 33^3 look) and upload them once: measured apart.
    const FrameTimes firstGraded = playMeasured(editor.project, editor.playback, **presenter, 60, false);
    std::vector<std::pair<FrameTimes, FrameTimes>> attempts; // graded, graded + scopes
    const auto meets = [](const FrameTimes& graded, const FrameTimes& withScopes) {
        return graded.pictures >= 150 && withScopes.pictures >= 150 && withScopes.scopePasses >= 70 &&
               graded.p(0.99) < 16.7 && withScopes.p(0.99) < 16.7 && withScopes.skippedLate <= graded.skippedLate + 2;
    };
    for (int attempt = 0; attempt < (perf ? 2 : 1); ++attempt) {
        // A [perf] case measures once more when the first try misses: another process was busy (builds, tests).
        const FrameTimes graded = playMeasured(editor.project, editor.playback, **presenter, refreshes, false);
        const FrameTimes withScopes = playMeasured(editor.project, editor.playback, **presenter, refreshes, true);
        attempts.emplace_back(graded, withScopes);
        if (meets(graded, withScopes)) {
            break;
        }
    }
    const auto& [graded, withScopes] = attempts.back();
    {
        std::ofstream report(fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / (perf ? "color-playback.md" : "color-playback-functional.md"),
                             std::ios::trunc);
        report << "# Playback: three stacked 1920x1080 60p H.264 layers (upper two at 60 % opacity)\n\n"
               << "Offscreen 60 Hz loop standing in for the program monitor's render thread (" << (*presenter)->backendName() << ", "
               << (*presenter)->adapterName() << "), " << refreshes << " refreshes from the start. Frame time = the composite's render "
               << "(upload, layers, effects, output) until the GPU has finished it, plus, with scopes, the scope pass "
               << "(compute + 1.8 MB readback) after every other picture (~30 Hz). CPU and GPU run in series here, so this "
               << "is an upper bound of the program monitor's frame time, where they overlap."
               << (perf ? " A [perf] run measures once more when the first attempt misses the budget (a busy machine)."
                        : " Functional run: the numbers are recorded, not asserted (the [perf] case asserts them).")
               << "\n\n"
               << "| Run | Pictures | Scope passes | p50 | p99 | max | Scope pass p50 / p99 | Dropped | Late targets |\n"
               << "|---|---|---|---|---|---|---|---|---|\n"
               << timesRow("no grade", plain) << "\n"
               << timesRow("first second after the grade is added (60 refreshes; LUTs built + uploaded)", firstGraded) << "\n";
        for (std::size_t i = 0; i < attempts.size(); ++i) {
            const std::string n = attempts.size() > 1 ? std::format(", attempt {}", i + 1) : std::string();
            report << timesRow(("full grade on all 3 layers" + n).c_str(), attempts[i].first) << "\n"
                   << timesRow(("full grade + scopes (~30 Hz)" + n).c_str(), attempts[i].second) << "\n";
        }
    }
    WARN(timesRow("no grade", plain) << "\n" << timesRow("first graded", firstGraded) << "\n" << timesRow("graded", graded)
                                     << "\n" << timesRow("graded + scopes", withScopes));
    // Always: the pictures were rendered, graded, and the scopes delivered.
    CHECK(plain.pictures > 0);
    CHECK(graded.pictures > 0);
    CHECK(withScopes.pictures > 0);
    CHECK(withScopes.scopePasses > 0);
    if (perf) {
        CHECK(graded.pictures >= 150);
        CHECK(withScopes.pictures >= 150);
        CHECK(withScopes.scopePasses >= 70);
        CHECK(graded.p(0.99) < 16.7);     // the grade fits a 60 Hz refresh
        CHECK(withScopes.p(0.99) < 16.7); // and so do the scopes
        CHECK(withScopes.skippedLate <= graded.skippedLate + 2);
    } else {
        recordTiming("graded 1080p60 x3", std::format("graded p99 {:.2f} ms, with scopes p99 {:.2f} ms, {} / {} pictures",
                                                      graded.p(0.99), withScopes.p(0.99), graded.pictures, withScopes.pictures));
    }
}

} // namespace

TEST_CASE("e2e: playback of three graded 1080p60 layers, with and without scopes", "[e2e][color][gpu]") { // GPU compositing
    gradedStackPlayback(false);
}

TEST_CASE("e2e perf: three graded 1080p60 layers fit a 60 Hz refresh, with scopes too", "[e2e][color][perf]") {
    gradedStackPlayback(true);
}

// ---- I.6: masks, tracking and source-anchored redaction in the shell ----------------------------------------------

namespace {

/// 4 s at 30 fps, 640x360: a textured picture (tests/masks/SyntheticMotion.h) moving 2 px left and 1 px up per frame
/// (a crop window sliding over a still), for the tracker. False without ffmpeg.
bool makePanClip(const fs::path& file) {
    const QString ffmpeg = QStringLiteral(WABCUT_FFMPEG_EXECUTABLE);
    if (ffmpeg.isEmpty() || !fs::exists(ffmpeg.toStdString())) {
        return false;
    }
    const masks::test::Texture texture(31);
    const fs::path still = file.parent_path() / "pan-still.gray";
    {
        std::ofstream out(still, std::ios::binary | std::ios::trunc);
        for (int y = 0; y < 720; ++y) {
            for (int x = 0; x < 1280; ++x) {
                out.put(static_cast<char>(static_cast<unsigned char>(std::clamp(texture.at(x, y), 0.0, 1.0) * 255.0)));
            }
        }
    }
    const QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
                           QStringLiteral("-f"), QStringLiteral("rawvideo"), QStringLiteral("-pix_fmt"), QStringLiteral("gray"),
                           QStringLiteral("-s"), QStringLiteral("1280x720"), QStringLiteral("-framerate"), QStringLiteral("30"),
                           QStringLiteral("-stream_loop"), QStringLiteral("-1"), QStringLiteral("-t"), QStringLiteral("4"),
                           QStringLiteral("-i"), QString::fromStdU16String(still.u16string()), QStringLiteral("-vf"),
                           QStringLiteral("crop=640:360:x='100+n*2':y='80+n*1',format=yuv420p"), QStringLiteral("-c:v"),
                           QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("veryfast"), QStringLiteral("-crf"),
                           QStringLiteral("12"), QString::fromStdU16String(file.u16string())};
    const bool ok = QProcess::execute(ffmpeg, args) == 0 && fs::exists(file);
    std::error_code ec;
    fs::remove(still, ec);
    return ok;
}

/// Mean absolute RGB difference of two same-size crops, 0..255.
double meanDifference(const QImage& a, const QImage& b) {
    const QImage x = a.convertToFormat(QImage::Format_RGB32);
    const QImage y = b.convertToFormat(QImage::Format_RGB32);
    double sum = 0.0;
    const int w = std::min(x.width(), y.width());
    const int h = std::min(x.height(), y.height());
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const QRgb p = x.pixel(i, j);
            const QRgb q = y.pixel(i, j);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
        }
    }
    return w > 0 && h > 0 ? sum / (3.0 * w * h) : 0.0;
}

} // namespace

TEST_CASE("e2e: masks - a local correction with handles, tracking, a redaction across two sequences",
          "[e2e][screenshot][masks]") {
    if (!haveMedia()) {
        SKIP("e2e media missing (CTest generates it: e2e.generate_media)");
    }
    const fs::path dir = freshWorkDir("masks");
    qputenv("WABCUT_ZERO_COPY", "0"); // the grabs render with a QRhi of their own: CPU frames
    struct RestoreZeroCopy {
        ~RestoreZeroCopy() { qunsetenv("WABCUT_ZERO_COPY"); }
    } restoreZeroCopy;

    QQmlEngine engine;
    QQmlComponent component(&engine, "Wabcut", "Main");
    INFO(component.errorString().toStdString());
    REQUIRE_FALSE(component.isError());
    std::unique_ptr<QQuickWindow> window(
        qobject_cast<QQuickWindow*>(component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
    REQUIRE(window);
    window->create();
    window->resize(1600, 960);
    window->contentItem()->setSize(QSizeF(1600, 960));
    auto* project = engine.singletonInstance<app::ProjectController*>("Wabcut", "ProjectController");
    auto* file = engine.singletonInstance<app::ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* masks = engine.singletonInstance<app::MaskBridge*>("Wabcut", "Masks");
    auto* source = engine.singletonInstance<app::SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    REQUIRE(project);
    REQUIRE(file);
    REQUIRE(masks);
    REQUIRE(source);
    QQuickWindowPrivate::get(window.get())->polishItems();

    const bool pan = makePanClip(dir / "media" / "pan.mp4");
    QList<QUrl> urls = clipUrls(dir);
    if (pan) {
        urls.append(urlOf(dir / "media" / "pan.mp4"));
    }
    REQUIRE(file->importFiles(urls).value(QStringLiteral("added")).toInt() == urls.size());
    QStringList ids;
    for (const QVariant& v : project->mediaItems()) {
        ids.append(v.toMap().value(QStringLiteral("id")).toString());
    }
    REQUIRE(file->dropMedia(ids[0], QStringLiteral("00:00:00:00"), QString(), false));
    REQUIRE(file->saveAs(urlOf(dir / "masks.wabcut")));
    const auto grab = [&] {
        QQuickWindowPrivate::get(window.get())->polishItems();
        return window->grabWindow();
    };
    auto* monitor = window->findChild<app::ProgramMonitor*>(QStringLiteral("programMonitor"));
    auto* monitorPanel = window->findChild<QQuickItem*>(QStringLiteral("monitorPanel"));
    auto* frameArea = window->findChild<QQuickItem*>(QStringLiteral("programFrameArea"));
    auto* effectControls = window->findChild<QQuickItem*>(QStringLiteral("effectControls"));
    auto* sourcePanel = window->findChild<QQuickItem*>(QStringLiteral("sourceMonitorPanel"));
    REQUIRE(monitor);
    REQUIRE(monitorPanel);
    REQUIRE(frameArea);
    REQUIRE(effectControls);
    REQUIRE(sourcePanel);
    const QString shots = QString::fromStdU16String((fs::path(WABCUT_TEST_OUTPUT_DIR) / "e2e" / "screenshots").u16string());
    QDir().mkpath(shots);
    const auto shotOf = [&](const QImage& image, const QQuickItem* item) { return image.copy(sceneRect(item)); };
    // A picture of the paused program monitor once the scheduler has re-rendered (frames settle within a few grabs).
    const auto settledShot = [&] {
        REQUIRE(waitFor([&] {
            grab();
            return monitor->hasFrame();
        }, 8000));
        spin(400);
        grab();
        spin(200);
        return grab();
    };
    const auto frameCrop = [&](const QImage& image, double x0, double y0, double x1, double y1) {
        const QRect r = sceneRect(frameArea);
        return image.copy(QRect(r.x() + qRound(x0 * r.width()), r.y() + qRound(y0 * r.height()), qRound((x1 - x0) * r.width()),
                                qRound((y1 - y0) * r.height())));
    };

    // ---- 1. A local colour correction: Basic Correction (brighter, no colour) inside a feathered ellipse ----
    project->setPlayhead(Ticks::fromSeconds(1));
    const ClipId clipA = project->currentSequence()->videoTracks.front()->clips.front().id;
    project->selectClip(app::clipKey(clipA), 0, true);
    REQUIRE(waitFor([&] { return masks->clip() == app::clipKey(clipA); }, 3000));
    const QImage before = settledShot();
    REQUIRE(project->applyEditFn("Add Effect", [&](const ProjectState& s) {
        const std::vector<ClipId> clips{clipA};
        return edit::addEffect(s, project->currentSequenceId(), clips, std::string("wabcut.color.basic"),
                               {{"exposure", ParamValue(0.7)}, {"saturation", ParamValue(0.0)}});
    }));
    REQUIRE(waitFor([&] { return masks->targets().size() == 2; }, 3000));
    masks->setTarget(masks->targets()[1].toMap().value(QStringLiteral("effect")).toString());
    REQUIRE(masks->drawShape(QStringLiteral("ellipse"), 0.3, 0.2, 0.7, 0.8));
    REQUIRE(masks->setValue(QStringLiteral("feather"), 0.08));
    masks->endEdit();
    const QImage local = settledShot();
    shotOf(local, monitorPanel).save(shots + QStringLiteral("/masks_local_correction.png"));
    shotOf(local, effectControls).save(shots + QStringLiteral("/masks_effect_controls.png"));
    {
        const double inside = meanDifference(frameCrop(before, 0.45, 0.45, 0.55, 0.55), frameCrop(local, 0.45, 0.45, 0.55, 0.55));
        const double outside = meanDifference(frameCrop(before, 0.02, 0.4, 0.12, 0.6), frameCrop(local, 0.02, 0.4, 0.12, 0.6));
        WARN(std::format("masks: local correction changes the inside by {:.1f}, the outside by {:.1f} (0..255)", inside, outside));
        CHECK(inside > 10.0);
        CHECK(outside < 2.0);
    }

    // ---- 2. A redaction marked once on the source covers the clip in two sequences ----
    REQUIRE(source->load(ids[0]));
    REQUIRE(waitFor([&] { return source->mediaId().isValid(); }, 3000));
    masks->setRedactStyle(QStringLiteral("mosaic"));
    masks->setRedactAudio(QStringLiteral("bleep"));
    masks->setRedactTool(true);
    REQUIRE(masks->addRedaction(0.55, 0.5, 0.95, 0.92)); // over the textured corner (the bars are flat)
    REQUIRE(masks->redactions().size() == 1);
    const QImage sequence1 = settledShot();
    shotOf(sequence1, sourcePanel).save(shots + QStringLiteral("/redaction_source_monitor.png"));
    const QImage seq1Monitor = shotOf(sequence1, monitorPanel);
    {
        const double region = meanDifference(frameCrop(local, 0.72, 0.6, 0.92, 0.88), frameCrop(sequence1, 0.72, 0.6, 0.92, 0.88));
        const double elsewhere = meanDifference(frameCrop(local, 0.02, 0.05, 0.2, 0.3), frameCrop(sequence1, 0.02, 0.05, 0.2, 0.3));
        WARN(std::format("masks: the redaction changes its region by {:.1f}, elsewhere {:.1f} (0..255)", region, elsewhere));
        CHECK(region > 5.0);
        CHECK(elsewhere < 2.0);
    }
    // A second sequence uses the same source at 2 s: the redaction is there too, without marking it again.
    project->newSequence();
    REQUIRE(waitFor([&] { return project->currentSequenceIndex() == 1; }, 3000));
    REQUIRE(file->dropMedia(ids[0], QStringLiteral("00:00:02:00"), QString(), false));
    project->setPlayhead(Ticks::fromSeconds(3));
    const QImage sequence2 = settledShot();
    const QImage seq2Monitor = shotOf(sequence2, monitorPanel);
    seq1Monitor.save(shots + QStringLiteral("/redaction_sequence1.png"));
    seq2Monitor.save(shots + QStringLiteral("/redaction_sequence2.png"));
    {
        QImage both(seq1Monitor.width() + seq2Monitor.width() + 8, std::max(seq1Monitor.height(), seq2Monitor.height()), QImage::Format_RGB32);
        both.fill(Qt::black);
        QPainter painter(&both);
        painter.drawImage(0, 0, seq1Monitor);
        painter.drawImage(seq1Monitor.width() + 8, 0, seq2Monitor);
        painter.end();
        both.save(shots + QStringLiteral("/redaction_two_sequences.png"));
    }
    {
        const Sequence& two = *project->currentSequence();
        REQUIRE_FALSE(two.videoTracks.empty());
        REQUIRE_FALSE(two.videoTracks.front()->clips.empty());
        const Clip& reused = two.videoTracks.front()->clips.front();
        CHECK(reused.start() == Ticks::fromSeconds(2));
        CHECK(project->snapshot()->redactions.front().appliesTo(reused.id));
    }

    // ---- 3. Tracking: an ellipse on the panning clip follows it; the progress and confidence strip mid-way ----
    if (pan) {
        project->newSequence();
        REQUIRE(waitFor([&] { return project->currentSequenceIndex() == 2; }, 3000));
        REQUIRE(file->dropMedia(ids[3], QStringLiteral("00:00:00:00"), QString(), false));
        const ClipId panClip = project->currentSequence()->videoTracks.front()->clips.front().id;
        project->setPlayhead(Ticks{});
        project->selectClip(app::clipKey(panClip), 0, true);
        REQUIRE(waitFor([&] { return masks->clip() == app::clipKey(panClip); }, 3000));
        masks->setTarget(QString());
        REQUIRE(masks->drawShape(QStringLiteral("ellipse"), 0.35, 0.3, 0.65, 0.7));
        REQUIRE(masks->track(QStringLiteral("forward"), 0));
        REQUIRE(waitFor([&] { return masks->confidence().size() >= 12 || !masks->tracking(); }, 30000));
        // The hidden window lays out only when polished: lay out, then let a few more frames scroll the section.
        QQuickWindowPrivate::get(window.get())->polishItems();
        const qsizetype seen = masks->confidence().size();
        waitFor([&] { return masks->confidence().size() >= seen + 3 || !masks->tracking(); }, 30000);
        const QImage tracking = grab();
        if (auto* flick = window->findChild<QQuickItem*>(QStringLiteral("maskSectionFlick"))) {
            WARN("mask section: contentY " << flick->property("contentY").toDouble() << " of " << flick->property("contentHeight").toDouble()
                                        << ", height " << flick->height() << ", tracking " << masks->tracking() << ", frames " << masks->confidence().size());
        }
        shotOf(tracking, effectControls).save(shots + QStringLiteral("/masks_tracking_progress.png"));
        REQUIRE(waitFor([&] { return !masks->tracking(); }, 120000));
        INFO(masks->trackStatus().toStdString());
        CHECK(masks->trackStatus().startsWith(QStringLiteral("Tracked")));
        const QVariantList confidence = masks->confidence();
        CHECK(confidence.size() >= 100);
        double lowest = 1.0;
        for (const QVariant& v : confidence) {
            lowest = std::min(lowest, v.toDouble());
        }
        WARN(std::format("masks: tracked {} frames of a real H.264 pan, lowest confidence {:.3f}", confidence.size(), lowest));
        CHECK(lowest > 0.5);
        const Clip& tracked = project->currentSequence()->findClip(panClip)->clip();
        const Mask& m = tracked.effects().front().masks.front();
        const Ticks frame100{kTicksPerSecond / 30 * 100};
        CHECK(std::abs(maskValue(m, "transform.x", frame100) - (-200.0 / 640.0)) < 0.004);
        CHECK(std::abs(maskValue(m, "transform.y", frame100) - (-100.0 / 360.0)) < 0.004);
        project->setPlayhead(Ticks::fromSeconds(3));
        const QImage done = settledShot();
        shotOf(done, monitorPanel).save(shots + QStringLiteral("/masks_tracked.png"));
        shotOf(done, effectControls).save(shots + QStringLiteral("/masks_tracked_controls.png"));
    } else {
        WARN("ffmpeg is not available: the tracking screenshots are skipped");
    }
    REQUIRE(file->save());
}
