// 0.6 (P1 round, stream 1C): export.
//   * #3  the size estimate prices every format (GIF, WebM VP9 / Opus, AV1, MP3, stills and image sequences, CineForm,
//         QuickTime Animation), not "1 KB";
//   * #21 vertical (9:16) sequences remember presets of their own; a 16:9 preset on one says "pillarboxed"; a vertical
//         export over 3:00 says it is no Short;
//   * #28 a size limit (targetMB) for any export, "Fits X MB", the low-bitrate warning;
//   * #27 offline media found before the export starts; Retry and Relink... on a failed job;
//   * #29 low disk space before an export (the estimate), while exports run, and before previews and proxies;
//   * #24 YouTube chapters to the clipboard, from In to Out, chapters only (MarkersBridge);
//   * #23 Export Frame sizes (YouTube thumbnail under 2 MB) and Export Frames at Markers (MonitorBridge);
//   * in the window: the Export panel's warnings, Size limit row and Retry, Quick Export's offline line, the Export
//     Frame dialog's sizes and markers mode (screenshots, dark and light).

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "DiskSpace.h"
#include "ExportController.h"
#include "GamingExport.h"
#include "MarkersBridge.h"
#include "MediaReadinessBridge.h"
#include "MonitorBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/command/Engine.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;
namespace fs = std::filesystem;

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "export-p1" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
}

bool samePath(const QString& a, const QString& b) {
    return !a.isEmpty() && QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(
                               QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}

/// A project of one empty `width` x `height` sequence of `seconds` at 30 fps (exports render silence and the synthetic
/// picture), with In/Out marks when given.
ProjectState emptyProject(double seconds, int width = 640, int height = 360,
                          std::optional<std::pair<double, double>> marks = {}) {
    ProjectState state;
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Clip";
    seq.width = width;
    seq.height = height;
    seq.frameRate = kRate30;
    seq.duration = Ticks::fromSeconds(seconds);
    if (marks) {
        seq.markIn = Ticks::fromSeconds(marks->first);
        seq.markOut = Ticks::fromSeconds(marks->second);
    }
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

/// The fixture's timeline (media at made-up paths: offline for an export) reshaped to `width` x `height`, `seconds`.
struct Rig {
    Timeline tl;
    ProjectController controller;
    GamingExport gaming;

    explicit Rig(const char* picture = "V1 |....|", Rate rate = Rate{60, 1}, int width = 1920, int height = 1080,
                 double seconds = 30.0, std::optional<std::pair<double, double>> marks = {})
        : tl(picture) {
        Sequence s = tl.sequence();
        s.frameRate = rate;
        s.width = width;
        s.height = height;
        s.duration = Ticks::fromSeconds(seconds);
        if (marks) {
            s.markIn = Ticks::fromSeconds(marks->first);
            s.markOut = Ticks::fromSeconds(marks->second);
        }
        controller.resetProject(tl.state.withSequence(std::make_shared<const Sequence>(std::move(s))), tl.seq);
        gaming.setController(&controller);
    }
    QVariantMap estimate(const char* preset, const QVariantMap& overrides = {}) {
        const QVariantMap e = gaming.estimate(QString(), QString::fromLatin1(preset), overrides);
        INFO(preset << ": " << e.value(QStringLiteral("error")).toString().toStdString());
        REQUIRE(e.value(QStringLiteral("ok")).toBool());
        return e;
    }
};

bool finished(const ExportController& c, const QString& jobId, int timeoutMs = 60'000) {
    return waitFor([&] { return c.job(jobId).value(QStringLiteral("finished")).toBool(); }, timeoutMs);
}

QQuickItem* findWith(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findWith(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* named(QQuickItem* root, const QString& name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

void saveCrop(const QImage& image, const QRectF& rect, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRect pixels(static_cast<int>(rect.x() * dpr), static_cast<int>(rect.y() * dpr),
                       static_cast<int>(rect.width() * dpr), static_cast<int>(rect.height() * dpr));
    REQUIRE(image.copy(pixels).save(QDir(outputDir()).filePath(name)));
}

/// Restores the real free-space lookup at the end of a test.
struct FakeDisk {
    explicit FakeDisk(qint64 bytes) {
        diskspace::setFreeBytesProviderForTests([bytes](const QString&) { return bytes; });
    }
    ~FakeDisk() { diskspace::setFreeBytesProviderForTests({}); }
};

} // namespace

// ---- #3: every format priced -----------------------------------------------------------------------------------------

TEST_CASE("Export estimates price GIF WebM AV1 MP3 stills CineForm and Animation", "[app][export][p1]") {
    Rig r; // 1920x1080, 60 fps, 30 s
    const auto bytes = [](const QVariantMap& e) { return e.value(QStringLiteral("bytes")).toDouble(); };
    const auto notes = [](const QVariantMap& e) { return e.value(QStringLiteral("notes")).toStringList().join(QLatin1Char(' ')); };

    // GIF: a palette per frame, about 0.8 bits a pixel; marked approximate.
    const QVariantMap gif = r.estimate("gif-480p15");
    CHECK(bytes(gif) > 1e6);
    CHECK(gif.value(QStringLiteral("approximate")).toBool());
    CHECK(notes(gif).contains(QStringLiteral("GIF")));
    CHECK(gif.value(QStringLiteral("sizeText")).toString() != QStringLiteral("1 KB"));
    // WebM: VP9 at constant quality (HEVC's factor) plus Opus.
    const QVariantMap webm = r.estimate("webm-vp9-720p");
    CHECK(bytes(webm) > 5e6);
    CHECK(webm.value(QStringLiteral("videoMbps")).toDouble() > 1.0);
    CHECK(webm.value(QStringLiteral("audioKbps")).toDouble() == Catch::Approx(128.0));
    // AV1: its bitrate (not only the sound): tens of MB for 30 s of 4K60.
    const QVariantMap av1 = r.estimate("youtube-4k60-av1");
    CHECK(av1.value(QStringLiteral("videoMbps")).toDouble() > 10.0);
    CHECK(bytes(av1) > 50e6);
    // AV1 at constant quality is smaller than H.264 at the same quality.
    const QVariantMap h264q = r.estimate("match-source", {{QStringLiteral("quality"), 20}});
    const QVariantMap av1q = r.estimate("match-source", {{QStringLiteral("videoCodec"), QStringLiteral("av1")},
                                                         {QStringLiteral("quality"), 20}});
    CHECK(bytes(av1q) < bytes(h264q) * 0.6);
    CHECK(bytes(av1q) > bytes(h264q) * 0.5);
    // MP3: its bitrate.
    const QVariantMap mp3 = r.estimate("audio-mp3");
    CHECK(bytes(mp3) == Catch::Approx(mp3.value(QStringLiteral("audioKbps")).toDouble() * 1e3 * 30 / 8 * 1.01).epsilon(0.01));
    CHECK(bytes(mp3) > 100e3);
    // A still is one frame (PNG about half its 8-bit RGB bytes); a sequence is one per frame.
    const QVariantMap still = r.estimate("png-still");
    CHECK(bytes(still) == Catch::Approx(1920.0 * 1080 * 3 * 0.5));
    const QVariantMap pngs = r.estimate("png-sequence");
    CHECK(bytes(pngs) == Catch::Approx(1920.0 * 1080 * 4 * 0.5 * 1800).epsilon(0.001)); // 8-bit RGBA, 30 s at 60 fps
    CHECK(notes(pngs).contains(QStringLiteral("Stills")));
    const QVariantMap exr = r.estimate("exr-sequence");
    CHECK(bytes(exr) > bytes(pngs));
    const QVariantMap jpegs = r.estimate("jpeg-sequence");
    CHECK(bytes(jpegs) == Catch::Approx(1920.0 * 1080 * 0.25 * 1800).epsilon(0.001));
    CHECK(bytes(r.estimate("dpx-sequence")) > bytes(jpegs));
    CHECK(bytes(r.estimate("tiff-sequence")) > 0);
    CHECK(bytes(r.estimate("targa-sequence")) > 0);
    // CineForm like ProRes HQ (12-bit RGBA half as much again); QuickTime Animation about half its RGBA bytes.
    const QVariantMap cineform = r.estimate("cineform-alpha");
    CHECK(cineform.value(QStringLiteral("videoMbps")).toDouble() == Catch::Approx(200.0 * 1.5 * 60 / 29.97).epsilon(0.01));
    const QVariantMap animation = r.estimate("animation-alpha");
    CHECK(animation.value(QStringLiteral("videoMbps")).toDouble() ==
          Catch::Approx(1920.0 * 1080 * 4 * 0.5 * 8 * 60 / 1e6).epsilon(0.01));
    // Formats without a hardware encoder name their own.
    CHECK(gif.value(QStringLiteral("encoder")).toString() == QStringLiteral("gif"));
    CHECK(pngs.value(QStringLiteral("encoder")).toString() == QStringLiteral("png"));
    // The encode times of the new codecs: SVT-AV1 and libvpx are slower than x264.
    CHECK(GamingExport::encodeSeconds("av1", "libsvtav1", 1920, 1080, 3600) >
          GamingExport::encodeSeconds("h264", "libx264", 1920, 1080, 3600));
    CHECK(GamingExport::encodeSeconds("vp9", "libvpx-vp9", 1920, 1080, 3600) >
          GamingExport::encodeSeconds("av1", "libsvtav1", 1920, 1080, 3600));
}

// ---- #21: vertical sequences ------------------------------------------------------------------------------------------

TEST_CASE("Gaming export: a 9:16 sequence is pillarboxed by 16:9 presets and vertical exports over 3 minutes say so",
          "[app][export][p1]") {
    Rig portrait("V1 |....|", Rate{60, 1}, 1080, 1920, 60.0);
    const QVariantMap youtube = portrait.gaming.plan(QStringLiteral("youtube-1080p60"), QString(), QStringLiteral("whole"));
    REQUIRE(youtube.value(QStringLiteral("ok")).toBool());
    const QString youtubeNotes = youtube.value(QStringLiteral("notes")).toStringList().join(QLatin1Char(' '));
    CHECK(youtubeNotes.contains(QStringLiteral("pillarboxed into 16:9")));
    CHECK_FALSE(youtubeNotes.contains(QStringLiteral("Upscaled")));
    // A landscape sequence smaller than the preset still says Upscaled.
    Rig small("V1 |....|", Rate{60, 1}, 1280, 720, 60.0);
    CHECK(small.gaming.plan(QStringLiteral("youtube-1080p60"), QString(), QStringLiteral("whole"))
              .value(QStringLiteral("notes")).toStringList().join(QLatin1Char(' ')).contains(QStringLiteral("Upscaled")));

    // Over 3:00 a vertical file is no Short (said, never blocked); 3:00 or less says nothing.
    const auto shortNote = [](const QVariantMap& e) {
        return e.value(QStringLiteral("notes")).toStringList().join(QLatin1Char(' ')).contains(QStringLiteral("Over 3:00"));
    };
    Rig long_("V1 |....|", Rate{60, 1}, 1080, 1920, 200.0);
    const QVariantMap longShort = long_.gaming.estimateFor(QStringLiteral("vertical-1080x1920-60"), QString(), QStringLiteral("whole"));
    REQUIRE(longShort.value(QStringLiteral("ok")).toBool());
    CHECK(shortNote(longShort));
    CHECK(shortNote(long_.estimate("vertical-1080x1920"))); // the built-in vertical preset too
    CHECK_FALSE(shortNote(portrait.gaming.estimateFor(QStringLiteral("vertical-1080x1920-60"), QString(), QStringLiteral("whole"))));
    CHECK_FALSE(shortNote(Rig("V1 |....|", Rate{60, 1}, 1920, 1080, 200.0).estimate("youtube-1080p"))); // landscape
}

TEST_CASE("Export memory: a vertical sequence remembers presets of its own", "[app][export][p1]") {
    resetSettings();
    ProjectController project;
    project.resetProject(emptyProject(2)); // 640 x 360
    ExportController controller;
    controller.setProject(&project);
    int changes = 0;
    QObject::connect(&controller, &ExportController::exportMemoryChanged, [&] { ++changes; });

    // Landscape choices.
    controller.setLastPreset(QStringLiteral("youtube-1080p"));
    controller.setLastGamingPreset(QStringLiteral("youtube-1080p60"));
    controller.setLastQuickPreset(QStringLiteral("audio-wav"));
    QVariantMap remembered = controller.rememberedFor({});
    CHECK_FALSE(remembered.value(QStringLiteral("portrait")).toBool());
    CHECK(remembered.value(QStringLiteral("preset")).toString() == QStringLiteral("youtube-1080p"));
    CHECK(remembered.value(QStringLiteral("gamingPreset")).toString() == QStringLiteral("youtube-1080p60"));
    CHECK(remembered.value(QStringLiteral("quickPreset")).toString() == QStringLiteral("audio-wav"));

    // A 9:16 sequence never starts from them: Match Source, no gaming preset.
    const int before = changes;
    project.resetProject(emptyProject(2, 1080, 1920));
    CHECK(changes > before); // what lastPreset reads changed with the shape
    remembered = controller.rememberedFor({});
    CHECK(remembered.value(QStringLiteral("portrait")).toBool());
    CHECK(remembered.value(QStringLiteral("preset")).toString() == QStringLiteral("match-source"));
    CHECK(remembered.value(QStringLiteral("gamingPreset")).toString().isEmpty());
    CHECK(remembered.value(QStringLiteral("quickPreset")).toString() == QStringLiteral("match-source"));
    CHECK(controller.lastPreset() == QStringLiteral("match-source"));
    // Its own choices are kept apart.
    controller.setLastPreset(QStringLiteral("vertical-1080x1920"));
    controller.setLastGamingPreset(QStringLiteral("vertical-1080x1920-60"));
    controller.setLastQuickPreset(QStringLiteral("vertical-1080x1920"));
    CHECK(controller.lastGamingPreset() == QStringLiteral("vertical-1080x1920-60"));
    project.resetProject(emptyProject(2));
    CHECK(controller.lastPreset() == QStringLiteral("youtube-1080p"));
    CHECK(controller.lastQuickPreset() == QStringLiteral("audio-wav"));
    CHECK(controller.rememberedFor({}).value(QStringLiteral("gamingPreset")).toString() == QStringLiteral("youtube-1080p60"));
    project.resetProject(emptyProject(2, 1080, 1920));
    CHECK(controller.lastPreset() == QStringLiteral("vertical-1080x1920"));
    CHECK(controller.lastQuickPreset() == QStringLiteral("vertical-1080x1920"));
    // Reset to defaults forgets both shapes.
    controller.forgetExportChoices();
    CHECK(controller.lastPreset() == QStringLiteral("match-source"));
    project.resetProject(emptyProject(2));
    CHECK(controller.lastPreset() == QStringLiteral("match-source"));
    CHECK(controller.lastGamingPreset().isEmpty());
    resetSettings();
}

// ---- #28: a size limit for any export -------------------------------------------------------------------------------

TEST_CASE("Size limit: targetMB fits any export and the summary says what fits", "[app][export][p1]") {
    resetSettings();
    // The override itself: TargetSize at decimal megabytes; 0 is off; out of range or not a number is refused.
    const auto sized = ExportController::settingsFor(QStringLiteral("match-source"), {{QStringLiteral("targetMB"), 25}});
    REQUIRE(sized);
    CHECK(sized->video.rateControl == ex::RateControl::TargetSize);
    CHECK(sized->video.targetBytes == 25'000'000);
    const auto off = ExportController::settingsFor(QStringLiteral("match-source"), {{QStringLiteral("targetMB"), 0}});
    REQUIRE(off);
    CHECK(off->video.rateControl == ex::RateControl::Quality);
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("match-source"), {{QStringLiteral("targetMB"), 0.5}}));
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("match-source"), {{QStringLiteral("targetMB"), 5000}}));
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("match-source"), {{QStringLiteral("targetMB"), QStringLiteral("big")}}));
    // AV1 and VP9 take it; GIF does not.
    CHECK(ExportController::settingsFor(QStringLiteral("webm-vp9-720p"), {{QStringLiteral("targetMB"), 8}}));
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("gif-480p15"), {{QStringLiteral("targetMB"), 8}}));
    // It wins over a quality chosen before it.
    const auto both = ExportController::settingsFor(QStringLiteral("match-source"),
                                                    {{QStringLiteral("quality"), 20}, {QStringLiteral("targetMB"), 100}});
    REQUIRE(both);
    CHECK(both->video.rateControl == ex::RateControl::TargetSize);

    // In the summary: "Fits 25 MB (≈ X Mb/s)", and the estimate stays under it.
    ProjectController project;
    project.resetProject(emptyProject(30, 1920, 1080));
    ExportController controller;
    controller.setProject(&project);
    GamingExport gaming;
    gaming.setController(&project);
    const QVariantMap overrides{{QStringLiteral("targetMB"), 25}};
    const QVariantMap d = controller.describe({}, QStringLiteral("match-source"), overrides);
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("quality")).toString().startsWith(QStringLiteral("Fits 25 MB (≈ ")));
    CHECK(d.value(QStringLiteral("targetMB")).toDouble() == Catch::Approx(25.0));
    CHECK(d.value(QStringLiteral("targetMbps")).toDouble() > 5.0);
    CHECK_FALSE(d.value(QStringLiteral("lowBitrate")).toBool());
    const QVariantMap e = gaming.estimate({}, QStringLiteral("match-source"), overrides);
    CHECK(e.value(QStringLiteral("bytes")).toDouble() < 25e6);
    CHECK(e.value(QStringLiteral("bytes")).toDouble() > 22e6);
    // 8 MB for two minutes of 1080p30 is too few bits: said, never blocked.
    project.resetProject(emptyProject(120, 1920, 1080));
    const QVariantMap thin = controller.describe({}, QStringLiteral("match-source"),
                                                 {{QStringLiteral("targetMB"), 8}, {QStringLiteral("audioBitrateKbps"), 64}});
    INFO(thin.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(thin.value(QStringLiteral("ok")).toBool());
    CHECK(thin.value(QStringLiteral("lowBitrate")).toBool());
    CHECK(thin.value(QStringLiteral("lowBitrateNote")).toString().contains(QStringLiteral("block up")));
    // An hour does not fit 1 MB at all: the summary says why.
    project.resetProject(emptyProject(3600, 1920, 1080));
    CHECK_FALSE(controller.describe({}, QStringLiteral("match-source"), {{QStringLiteral("targetMB"), 1}})
                    .value(QStringLiteral("ok")).toBool());
    // The custom value is remembered for the field (and forgotten by Reset to defaults).
    controller.setLastTargetMB(64);
    CHECK(controller.lastTargetMB() == Catch::Approx(64.0));
    controller.setLastTargetMB(0.2);
    CHECK(controller.lastTargetMB() == 0.0);
    controller.setLastTargetMB(16);
    controller.forgetExportChoices();
    CHECK(controller.lastTargetMB() == 0.0);
    resetSettings();
}

// ---- #27: offline media before the export, Retry and Relink after it ----------------------------------------------

TEST_CASE("Offline media: describe lists what the export would stop on before it starts", "[app][export][p1]") {
    // The fixture's media live at made-up paths: offline for an export. The clip covers the first 4 frames.
    Rig r("V1 |Aaaa|\nA1 |Aaaa|", Rate{25, 1}, 1920, 1080, 2.0, std::pair{1.0, 2.0}); // In to Out: after the clip
    ExportController controller;
    controller.setProject(&r.controller);
    const QVariantMap whole = controller.describe({}, QStringLiteral("match-source"));
    REQUIRE(whole.value(QStringLiteral("ok")).toBool());
    CHECK(whole.value(QStringLiteral("offlineCount")).toInt() == 1);
    CHECK(whole.value(QStringLiteral("offline")).toStringList() == QStringList{QStringLiteral("A")});
    // In to Out over the gap reads no media.
    const QVariantMap marked = controller.describe({}, QStringLiteral("match-source"), {{QStringLiteral("range"), QStringLiteral("inOut")}});
    INFO(marked.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(marked.value(QStringLiteral("ok")).toBool());
    CHECK(marked.value(QStringLiteral("offlineCount")).toInt() == 0);
    // Audio only reads the clip's sound: still offline.
    CHECK(controller.describe({}, QStringLiteral("audio-wav")).value(QStringLiteral("offlineCount")).toInt() == 1);
    // The export module's own check (the same one the export stops on).
    const SequencePtr seq = r.controller.currentSequence();
    const auto settings = ExportController::settingsFor(QStringLiteral("match-source"), {});
    REQUIRE(settings);
    const auto missing = ex::missingOriginals(*r.controller.snapshot(), *seq, *settings);
    REQUIRE(missing.size() == 1);
    CHECK(missing.front().name == "A");
    CHECK(missing.front().path == "D:/footage/A.mov");
    // An empty sequence has nothing offline.
    r.controller.resetProject(emptyProject(2));
    CHECK(controller.describe({}, QStringLiteral("match-source")).value(QStringLiteral("offlineCount")).toInt() == 0);
}

TEST_CASE("Retry: a failed or cancelled export queues again with its request", "[app][export][p1]") {
    resetSettings();
    const fs::path dir = freshDir("retry");
    Rig r("V1 |Aaaa|", Rate{25, 1}, 640, 360, 4.0 / 25.0);
    ExportController controller;
    controller.setProject(&r.controller);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    controller.setProjectFile(dir / "Film.wabcut");

    // Offline media: the export fails; the row says it can be retried and why it failed.
    const QString failed = controller.queueExport({}, QStringLiteral("match-source"), {{QStringLiteral("output"), q(dir / "cut.mp4")}});
    REQUIRE_FALSE(failed.isEmpty());
    REQUIRE(finished(controller, failed));
    REQUIRE(controller.job(failed).value(QStringLiteral("state")).toString() == QStringLiteral("failed"));
    CHECK(controller.job(failed).value(QStringLiteral("errorKind")).toString() == QStringLiteral("offline"));
    ExportJobModel* jobs = controller.jobs();
    CHECK(jobs->roleNames().value(ExportJobModel::RetryableRole) == QByteArray("retryable"));
    const auto rowOf = [&](const QString& id) {
        for (int i = 0; i < jobs->rowCount(); ++i) {
            if (jobs->data(jobs->index(i), ExportJobModel::JobIdRole).toString() == id)
                return i;
        }
        return -1;
    };
    CHECK(jobs->data(jobs->index(rowOf(failed)), ExportJobModel::RetryableRole).toBool());
    // Retry: the same request into the same file (none was written).
    const QString again = controller.retry(failed);
    REQUIRE_FALSE(again.isEmpty());
    CHECK(again != failed);
    CHECK(samePath(controller.job(again).value(QStringLiteral("outputPath")).toString(), q(dir / "cut.mp4")));
    CHECK(controller.job(again).value(QStringLiteral("origin")).toString() == QStringLiteral("export"));
    REQUIRE(finished(controller, again));
    // The name is taken now (another file there): a numbered one.
    { std::ofstream(dir / "cut.mp4", std::ios::binary) << "x"; }
    const QString numbered = controller.retry(again);
    REQUIRE_FALSE(numbered.isEmpty());
    CHECK(samePath(controller.job(numbered).value(QStringLiteral("outputPath")).toString(), q(dir / "cut (2).mp4")));
    REQUIRE(finished(controller, numbered));
    // A file there from before the export was queued is the one it was to replace (the user chose that file): Retry
    // exports into it again, not into a numbered one (fix round 1, ARGON-11). It stays as it was until an export
    // succeeds.
    { std::ofstream(dir / "old.mp4", std::ios::binary) << "old"; }
    const QString replacing = controller.queueExport({}, QStringLiteral("match-source"), {{QStringLiteral("output"), q(dir / "old.mp4")}});
    REQUIRE_FALSE(replacing.isEmpty());
    REQUIRE(finished(controller, replacing));
    REQUIRE(controller.job(replacing).value(QStringLiteral("state")).toString() == QStringLiteral("failed"));
    const QString replacingAgain = controller.retry(replacing);
    REQUIRE_FALSE(replacingAgain.isEmpty());
    CHECK(samePath(controller.job(replacingAgain).value(QStringLiteral("outputPath")).toString(), q(dir / "old.mp4")));
    REQUIRE(finished(controller, replacingAgain));
    CHECK(fs::file_size(dir / "old.mp4") == 3);
    CHECK_FALSE(fs::exists(dir / "old (2).mp4"));

    // A cancelled export queued from a preset runs again (here it succeeds); a done one is not retried.
    r.controller.resetProject(emptyProject(1));
    const QStringList held = controller.sendToQueue({}, {QStringLiteral("audio-wav")}, {{QStringLiteral("output"), q(dir / "mix.wav")}}, true);
    REQUIRE(held.size() == 1);
    controller.cancel(held.front());
    REQUIRE(finished(controller, held.front()));
    CHECK(jobs->data(jobs->index(rowOf(held.front())), ExportJobModel::RetryableRole).toBool());
    const QString rerun = controller.retry(held.front());
    REQUIRE_FALSE(rerun.isEmpty());
    REQUIRE(finished(controller, rerun));
    CHECK(controller.job(rerun).value(QStringLiteral("state")).toString() == QStringLiteral("done"));
    CHECK(controller.job(rerun).value(QStringLiteral("origin")).toString() == QStringLiteral("queue"));
    CHECK_FALSE(jobs->data(jobs->index(rowOf(rerun)), ExportJobModel::RetryableRole).toBool());
    CHECK(controller.retry(rerun).isEmpty());
    CHECK_FALSE(controller.lastError().isEmpty());
    CHECK(controller.retry(QStringLiteral("j9999")).isEmpty());
    // A request built in code (not from a preset) is not retryable.
    ex::ExportRequest request;
    request.snapshot = r.controller.snapshot();
    request.sequence = r.controller.currentSequenceId();
    request.settings = *ExportController::settingsFor(QStringLiteral("audio-wav"), {});
    request.output = dir / "built.wav";
    const QString built = controller.queueRequest(std::move(request), QStringLiteral("Built"));
    controller.cancel(built);
    REQUIRE(finished(controller, built));
    CHECK_FALSE(jobs->data(jobs->index(rowOf(built)), ExportJobModel::RetryableRole).toBool());
    resetSettings();
}

// ---- #29: low disk space ----------------------------------------------------------------------------------------------

TEST_CASE("Low disk space: the estimate warns before the export and running exports say so once", "[app][export][p1]") {
    resetSettings();
    const fs::path dir = freshDir("space");
    ProjectController project;
    project.resetProject(emptyProject(20, 1920, 1080));
    ExportController controller;
    controller.setProject(&project);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    controller.setProjectFile(dir / "Film.wabcut");
    GamingExport gaming;
    gaming.setController(&project);
    gaming.setExport(&controller);
    {
        // 1 MB free: a 20 s 1080p export needs more.
        FakeDisk disk(1'000'000);
        const QVariantMap e = gaming.estimate({}, QStringLiteral("youtube-1080p"), {});
        REQUIRE(e.value(QStringLiteral("ok")).toBool());
        CHECK(e.value(QStringLiteral("lowSpace")).toBool());
        CHECK(e.value(QStringLiteral("freeBytes")).toDouble() == 1e6);
        CHECK(e.value(QStringLiteral("lowSpaceNote")).toString().startsWith(QStringLiteral("Only 1 MB is free on ")));
        CHECK(e.value(QStringLiteral("lowSpaceNote")).toString().contains(QStringLiteral("is needed for the export")));
        CHECK(e.value(QStringLiteral("notes")).toStringList().contains(e.value(QStringLiteral("lowSpaceNote")).toString()));
        CHECK(samePath(e.value(QStringLiteral("folder")).toString(), q(dir)));
        // A chosen output is where it looks.
        const QVariantMap chosen = gaming.estimate({}, QStringLiteral("youtube-1080p"), {{QStringLiteral("output"), q(dir / "sub" / "a.mp4")}});
        CHECK(samePath(chosen.value(QStringLiteral("folder")).toString(), q(dir / "sub")));
    }
    {
        // Plenty of room: no warning.
        FakeDisk disk(4'000'000'000'000);
        const QVariantMap e = gaming.estimate({}, QStringLiteral("youtube-1080p"), {});
        CHECK_FALSE(e.value(QStringLiteral("lowSpace")).toBool());
        CHECK(e.value(QStringLiteral("lowSpaceNote")).toString().isEmpty());
    }
    CHECK(diskspace::freeBytesAt(q(dir)) > 0); // the real lookup
    CHECK(diskspace::freeBytesAt(QString()) == -1);
    CHECK(diskspace::freeBytesAt(q(dir / "not" / "yet" / "there.mp4")) > 0); // its nearest existing folder

    // While an export runs: under 2 GB on its drive is said once.
    QStringList said;
    QObject::connect(&controller, &ExportController::diskSpaceLow, [&](const QString& m) { said << m; });
    FakeDisk disk(1'500'000'000);
    const QString job = controller.queueExport({}, QStringLiteral("audio-wav"), {{QStringLiteral("output"), q(dir / "mix.wav")}});
    REQUIRE_FALSE(job.isEmpty());
    REQUIRE(QMetaObject::invokeMethod(&controller, "checkDiskSpace")); // the 30 s timer's check, now
    REQUIRE(said.size() == 1);
    CHECK(said.front().startsWith(QStringLiteral("Only 1.5 GB left on ")));
    CHECK(said.front().contains(QStringLiteral("mix.wav")));
    REQUIRE(QMetaObject::invokeMethod(&controller, "checkDiskSpace"));
    CHECK(said.size() == 1); // once per drive
    REQUIRE(finished(controller, job));
    resetSettings();
}

namespace {

class FakePreviews final : public PreviewRenderer {
public:
    std::expected<std::int64_t, Error> render(const ProjectState&, SequenceId, TimeRange, const fs::path& file,
                                              const std::function<bool(double)>&) override {
        std::ofstream(file, std::ios::binary) << "preview";
        ++renders;
        return std::int64_t{7};
    }
    int renders = 0;
};

} // namespace

TEST_CASE("Low disk space: Render In to Out and Create proxy do not start on a full drive", "[app][export][p1][render]") {
    const fs::path dir = freshDir("space-previews");
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    const ClipId clip = clipAt(tl.state, tl.seq, "V1@0");
    const ProjectState blurred = ok(edit::addEffect(tl.state, tl.seq, std::span(&clip, 1), "wabcut.blur.gaussian")).state;
    const fs::path file = dir / "film.wabcut";
    REQUIRE(io::saveProject(blurred, file, io::SaveOptions{0}));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.open(QUrl::fromLocalFile(q(file))));
    PlaybackBridge playback(&controller);
    MediaReadinessBridge readiness(&lifecycle, &playback);
    auto renderer = std::make_shared<FakePreviews>();
    readiness.setPreviewRendererForTests(renderer);
    readiness.setRenderSynchronous(true);
    QStringList outcomes;
    QObject::connect(&readiness, &MediaReadinessBridge::renderFinished,
                     [&](const QString& outcome, const QString& message) { outcomes << outcome + QLatin1Char('|') + message; });
    QStringList proxyWarnings;
    QObject::connect(&readiness, &MediaReadinessBridge::diskSpaceLow, [&](const QString& m) { proxyWarnings << m; });
    {
        FakeDisk disk(1000);
        const QString why = readiness.renderInToOut();
        CHECK(why.startsWith(QStringLiteral("Only 1 KB is free on ")));
        CHECK(why.contains(QStringLiteral("is needed for the previews")));
        CHECK(renderer->renders == 0);
        REQUIRE(outcomes.size() == 1);
        CHECK(outcomes.front().startsWith(QStringLiteral("failed|Only 1 KB is free")));
        CHECK_FALSE(readiness.rendering());
        // Create proxy: none queued, and why.
        CHECK(readiness.generateProxies({mediaKey(tl.media('A'))}) == 0);
        REQUIRE(proxyWarnings.size() == 1);
        CHECK(proxyWarnings.front().contains(QStringLiteral("is needed for the proxies")));
    }
    // With room the render runs.
    FakeDisk disk(4'000'000'000'000);
    CHECK(readiness.renderInToOut().isEmpty());
    CHECK(renderer->renders > 0);
}

// ---- #24: YouTube chapters to the clipboard ---------------------------------------------------------------------------

TEST_CASE("YouTube chapters: Copy to the clipboard from In to Out and chapter markers only", "[app][markers][p1]") {
    Rig r("V1 |....|", Rate{25, 1}, 1920, 1080, 120.0, std::pair{10.0, 70.0});
    MarkersBridge markers;
    markers.setController(&r.controller);
    {
        command::CommandEngine engine(r.controller.session());
        for (const auto& [seconds, name, chapter] : {std::tuple{10, "Intro", true}, std::tuple{15, "todo: mix", false},
                                                     std::tuple{40, "Part 2", true}, std::tuple{50, "Ending", true},
                                                     std::tuple{100, "Outro", true}}) {
            command::CommandRequest add;
            add.command = "marker.add";
            add.params = command::Json{{"time", Ticks::fromSeconds(seconds).value}, {"name", name},
                                       {"type", chapter ? "chapter" : "comment"}, {"sequence", r.tl.seq.value}};
            REQUIRE(engine.execute(add).ok());
        }
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);
    clipboard->clear();
    REQUIRE(markers.copyToClipboard(QStringLiteral("youtube"), QStringLiteral("inOut"), true));
    CHECK(clipboard->text() == QStringLiteral("0:00 Intro\n0:30 Part 2\n0:40 Ending\n"));
    CHECK(markers.message() == QStringLiteral("Copied 3 chapter(s)"));
    // The whole sequence: from its start, with YouTube's rule about 0:00 as a warning.
    REQUIRE(markers.copyToClipboard(QStringLiteral("youtube"), QString(), true));
    CHECK(clipboard->text().startsWith(QStringLiteral("0:10 Intro\n")));
    CHECK(markers.message().contains(QStringLiteral("with warnings: the first chapter must start at 0:00")));
    // The file export takes the same options.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString written = markers.exportToFile(QStringLiteral("youtube"), QUrl::fromLocalFile(dir.filePath(QStringLiteral("ch.txt"))),
                                                 QStringLiteral("inOut"), true);
    REQUIRE_FALSE(written.isEmpty());
    QFile text(written);
    REQUIRE(text.open(QIODevice::ReadOnly));
    CHECK(QString::fromUtf8(text.readAll()) == QStringLiteral("0:00 Intro\n0:30 Part 2\n0:40 Ending\n"));
    // Chapters only with no chapter marker: nothing copied, and why.
    Rig plain("V1 |....|", Rate{25, 1}, 1920, 1080, 60.0);
    MarkersBridge other;
    other.setController(&plain.controller);
    {
        command::CommandEngine engine(plain.controller.session());
        command::CommandRequest add;
        add.command = "marker.add";
        add.params = command::Json{{"time", 0}, {"name", "todo"}, {"sequence", plain.tl.seq.value}};
        REQUIRE(engine.execute(add).ok());
    }
    clipboard->setText(QStringLiteral("before"));
    CHECK_FALSE(other.copyToClipboard(QStringLiteral("youtube"), QString(), true));
    CHECK(clipboard->text() == QStringLiteral("before"));
    CHECK(other.message().contains(QStringLiteral("no chapter markers")));
}

// ---- #23: Export Frame sizes and Export Frames at Markers -----------------------------------------------------------

TEST_CASE("Export Frame writes a YouTube thumbnail under 2 MB and a still at each marker into a Thumbnails bin",
          "[app][monitor][exportframe][gpu][p1]") {
    resetSettings();
    Timeline tl(R"(
        V1 tgt |............|
        A1 tgt |............|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    {
        command::CommandEngine engine(controller.session());
        command::CommandRequest title;
        title.command = "title.add";
        title.params = command::Json{{"text", "Thumbnail"}, {"at", 0}, {"duration", f(12).value}, {"track", "V1"},
                                     {"sequence", tl.seq.value}};
        REQUIRE(engine.execute(title).ok());
        for (const auto& [frame, name] : {std::pair{2, "Clutch: 1v3"}, std::pair{8, "Ace"}}) {
            command::CommandRequest add;
            add.command = "marker.add";
            add.params = command::Json{{"time", f(frame).value}, {"name", name}, {"sequence", tl.seq.value}};
            REQUIRE(engine.execute(add).ok());
        }
    }
    controller.setPlayhead(f(4));
    MonitorBridge monitors;
    monitors.setController(&controller);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl folder = QUrl::fromLocalFile(dir.path());

    // The sizes the dialog offers.
    const QVariantMap defaults = monitors.exportFrameDefaults(false);
    CHECK(defaults.value(QStringLiteral("sizes")).toList().size() == 4);
    CHECK(defaults.value(QStringLiteral("size")).toString() == QStringLiteral("sequence"));
    CHECK(defaults.value(QStringLiteral("keepUnder")).toBool());

    // A YouTube thumbnail: 1280 x 720 JPEG under 2 MB.
    const QVariantMap thumbnail{{QStringLiteral("size"), QStringLiteral("thumbnail")}, {QStringLiteral("fill"), true},
                                {QStringLiteral("quality"), 90}, {QStringLiteral("keepUnder"), true}};
    const QString thumb = monitors.exportFrame(false, QStringLiteral("Thumb"), QStringLiteral("jpeg"), folder, false, thumbnail);
    INFO(monitors.message().toStdString());
    REQUIRE_FALSE(thumb.isEmpty());
    const QImage written(thumb);
    REQUIRE_FALSE(written.isNull());
    CHECK(written.size() == QSize(1280, 720));
    CHECK(QFileInfo(thumb).size() <= 2'000'000);
    // Remembered for the next time; a custom size too.
    const QVariantMap again = monitors.exportFrameDefaults(false);
    CHECK(again.value(QStringLiteral("size")).toString() == QStringLiteral("thumbnail"));
    CHECK(again.value(QStringLiteral("quality")).toInt() == 90);
    const QString custom = monitors.exportFrame(false, QStringLiteral("Custom"), QStringLiteral("png"), folder, false,
                                                {{QStringLiteral("size"), QStringLiteral("custom")},
                                                 {QStringLiteral("width"), 400}, {QStringLiteral("height"), 300},
                                                 {QStringLiteral("fill"), false}});
    REQUIRE_FALSE(custom.isEmpty());
    CHECK(QImage(custom).size() == QSize(400, 300));
    CHECK(monitors.exportFrameDefaults(false).value(QStringLiteral("width")).toInt() == 400);
    CHECK(monitors.exportFrame(false, QStringLiteral("Bad"), QStringLiteral("png"), folder, false,
                               {{QStringLiteral("size"), QStringLiteral("custom")}, {QStringLiteral("width"), 4},
                                {QStringLiteral("height"), 4}})
              .isEmpty());

    // A still at each marker, named after it, imported into a Thumbnails bin: one undo step.
    const QVariantMap counts = monitors.markerFrameCounts();
    CHECK(counts.value(QStringLiteral("all")).toInt() == 2);
    CHECK(counts.value(QStringLiteral("highlights")).toInt() == 0);
    monitors.setFramesSynchronous(true);
    std::vector<std::pair<bool, QString>> finishedSpy;
    QObject::connect(&monitors, &MonitorBridge::framesFinished,
                     [&](bool ok, const QString& message) { finishedSpy.emplace_back(ok, message); });
    const std::size_t media = controller.snapshot()->media.size();
    REQUIRE(monitors.exportFramesAtMarkers({{QStringLiteral("folder"), folder}, {QStringLiteral("format"), QStringLiteral("jpeg")},
                                            {QStringLiteral("size"), QStringLiteral("thumbnail")},
                                            {QStringLiteral("importIntoProject"), true}}));
    REQUIRE(finishedSpy.size() == 1);
    INFO(finishedSpy.front().second.toStdString());
    CHECK(finishedSpy.front().first);
    CHECK_FALSE(monitors.framesBusy());
    CHECK(QFileInfo::exists(dir.filePath(QStringLiteral("Clutch_ 1v3.jpg"))));
    CHECK(QImage(dir.filePath(QStringLiteral("Ace.jpg"))).size() == QSize(1280, 720));
    CHECK(controller.snapshot()->media.size() == media + 2);
    CHECK(controller.session().undoLabel() == "Export Frames at Markers");
    REQUIRE(controller.session().undo());
    CHECK(controller.snapshot()->media.size() == media);
    // No highlight markers: refused with why.
    REQUIRE(monitors.exportFramesAtMarkers({{QStringLiteral("folder"), folder}, {QStringLiteral("markers"), QStringLiteral("highlights")}}));
    REQUIRE(finishedSpy.size() == 2);
    CHECK_FALSE(finishedSpy.back().first);
    CHECK(finishedSpy.back().second.contains(QStringLiteral("no highlight markers")));
    CHECK_FALSE(monitors.exportFramesAtMarkers({{QStringLiteral("folder"), QUrl::fromLocalFile(dir.filePath(QStringLiteral("gone")))}}));
    resetSettings();
}

// ---- In the window ---------------------------------------------------------------------------------------------------

TEST_CASE("Export panel P1: offline media wait with Relink, the size limit, low space and Retry; screenshots",
          "[app][export][qml][screenshot][p1]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            const fs::path dir = freshDir(dark ? "panel-dark" : "panel-light");
            A11yScope a11y;
            Shell shell;
            REQUIRE(shell.window);
            INFO(join(shell.errors));
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.theme->setProperty("dark", dark);
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
            REQUIRE(controller);
            REQUIRE(exporter);
            exporter->setVideoSourceFactory(ex::syntheticVideoSourceFactory());
            exporter->setLastFolder(q(dir));
            FakeDisk disk(10'000); // 10 KB free: the export needs more (set before the project: the estimate reads it then)
            Timeline tl("V1 |Aaaa|\nA1 |Aaaa|");
            controller->resetProject(tl.state, tl.seq);
            shell.window->setProperty("workspace", 2);
            spin(200);
            QQuickItem* content = shell.window->contentItem();
            QQuickItem* panel = nullptr;
            REQUIRE(waitFor([&] {
                panel = findWith(content, [](QQuickItem* i) { return i->property("sessionPicks").isValid(); });
                return panel != nullptr && panel->isVisible();
            }));

            // Offline media: the warning with Relink…, and Export waits.
            QQuickItem* offline = named(content, QStringLiteral("exportOfflineWarning"));
            QQuickItem* button = named(content, QStringLiteral("exportButton"));
            QQuickItem* relink = named(content, QStringLiteral("exportRelink"));
            REQUIRE(offline);
            REQUIRE(button);
            REQUIRE(relink);
            REQUIRE(waitFor([&] { return offline->isVisible(); }));
            CHECK(offline->property("text").toString().startsWith(QStringLiteral("1 offline file in this export: A.")));
            CHECK_FALSE(button->isEnabled());
            CHECK_FALSE(named(content, QStringLiteral("exportSendToQueue"))->isEnabled());
            int relinkSpy = 0;
            QObject::connect(exporter, &ExportController::relinkRequested, [&] { ++relinkSpy; });
            REQUIRE(QMetaObject::invokeMethod(relink, "clicked"));
            CHECK(relinkSpy == 1);
            QObject* relinkDialog = shell.object("relinkDialog");
            if (relinkDialog)
                QMetaObject::invokeMethod(relinkDialog, "close");
            // The drive is short: the warning by the button (the notes do not repeat it).
            QQuickItem* lowSpace = named(content, QStringLiteral("exportLowSpaceWarning"));
            REQUIRE(lowSpace);
            CHECK(lowSpace->isVisible());
            CHECK(lowSpace->property("text").toString().startsWith(QStringLiteral("Only 10 KB is free on ")));
            CHECK_FALSE(panel->property("estimateNotes").toStringList().join(QLatin1Char(' ')).contains(QStringLiteral("Only 10 KB")));

            // The size limit: 25 MB; the quality waits; the summary says what fits.
            QObject* drawer = shell.object("exportSettingsDrawer");
            if (!drawer)
                drawer = named(content, QStringLiteral("exportSettingsDrawer"));
            REQUIRE(drawer);
            drawer->setProperty("open", true);
            QQuickItem* sizeTabs = named(content, QStringLiteral("exportSizeLimitTabs"));
            REQUIRE(sizeTabs);
            REQUIRE(sizeTabs->isVisible());
            REQUIRE(QMetaObject::invokeMethod(sizeTabs, "activated", Q_ARG(int, 2)));
            CHECK(panel->property("overrides").toMap().value(QStringLiteral("targetMB")).toInt() == 25);
            CHECK(panel->property("summary").toMap().value(QStringLiteral("quality")).toString().startsWith(QStringLiteral("Fits 25 MB")));
            CHECK_FALSE(named(content, QStringLiteral("exportQualityTabs"))->isEnabled());
            CHECK(sizeTabs->property("currentIndex").toInt() == 2);
            {
                // Screenshot: the drawer scrolled into view with its Size limit row.
                auto* drawerItem = qobject_cast<QQuickItem*>(drawer);
                QQuickItem* scroll = named(content, QStringLiteral("exportPresetScroll"));
                REQUIRE(drawerItem);
                REQUIRE(scroll);
                spin(400); // the drawer opens (its height animates)
                auto* flickContent = scroll->property("contentItem").value<QQuickItem*>();
                if (flickContent && flickContent->isAncestorOf(drawerItem)) { // a narrow slot: the drawer under the tiles
                    // The Size limit row in the middle of the view.
                    const QPointF at = sizeTabs->mapToItem(flickContent, QPointF(0, 0));
                    scroll->setProperty("contentY", std::max(0.0, at.y() - scroll->height() / 2.0));
                }
                spin(100);
                const QImage shot = shell.grab();
                REQUIRE_FALSE(shot.isNull());
                saveCrop(shot, panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height())),
                         dark ? QStringLiteral("export-p1-size-limit.png") : QStringLiteral("export-p1-size-limit-light.png"));
                scroll->setProperty("contentY", 0.0);
            }
            // A custom size is remembered for the field.
            QQuickItem* customField = named(content, QStringLiteral("exportSizeLimitCustom"));
            REQUIRE(customField);
            bool applied = false;
            REQUIRE(QMetaObject::invokeMethod(drawer, "applyCustomSizeLimit", Q_RETURN_ARG(bool, applied),
                                              Q_ARG(QString, QStringLiteral("64"))));
            CHECK(applied);
            CHECK(panel->property("overrides").toMap().value(QStringLiteral("targetMB")).toDouble() == Catch::Approx(64.0));
            CHECK(exporter->lastTargetMB() == Catch::Approx(64.0));
            CHECK(sizeTabs->property("currentIndex").toInt() == -1);
            CHECK(customField->property("text").toString() == QStringLiteral("64"));
            REQUIRE(QMetaObject::invokeMethod(sizeTabs, "activated", Q_ARG(int, 0)));
            CHECK_FALSE(panel->property("overrides").toMap().contains(QStringLiteral("targetMB")));

            // A failed export in the queue: Relink… and Retry on its row.
            const QString job = exporter->queueExport({}, QStringLiteral("match-source"), {{QStringLiteral("output"), q(dir / "cut.mp4")}});
            REQUIRE_FALSE(job.isEmpty());
            REQUIRE(finished(*exporter, job));
            QQuickItem* pill = nullptr;
            REQUIRE(waitFor([&] {
                if (pill == nullptr)
                    shell.grab();
                pill = named(content, QStringLiteral("exportJob_") + job);
                return pill != nullptr && pill->isVisible();
            }));
            QQuickItem* retry = named(pill, QStringLiteral("exportJobRetry"));
            QQuickItem* pillRelink = named(pill, QStringLiteral("exportJobRelink"));
            REQUIRE(retry);
            REQUIRE(pillRelink);
            CHECK(retry->isVisible());
            CHECK(pillRelink->isVisible());
            REQUIRE(QMetaObject::invokeMethod(pillRelink, "clicked"));
            CHECK(relinkSpy == 2);
            if (relinkDialog)
                QMetaObject::invokeMethod(relinkDialog, "close");
            spin(200);
            {
                const QStringList issues = auditAccessibility(panel);
                INFO(issues.join(QStringLiteral("; ")).toStdString());
                CHECK(issues.isEmpty());
            }
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            saveCrop(image, panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height())),
                     dark ? QStringLiteral("export-p1-warnings.png") : QStringLiteral("export-p1-warnings-light.png"));
            const int jobsBefore = exporter->jobs()->rowCount();
            REQUIRE(QMetaObject::invokeMethod(retry, "clicked"));
            CHECK(exporter->jobs()->rowCount() == jobsBefore + 1);
            exporter->cancelAll();
            REQUIRE(waitFor([&] { return exporter->jobs()->activeCount() == 0; }, 60'000));

            // Relinked (here: an empty sequence instead): the warning goes and Export is back.
            controller->resetProject(emptyProject(2));
            REQUIRE(waitFor([&] { return !offline->isVisible(); }));
            CHECK(button->isEnabled());
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
    resetSettings();
}

TEST_CASE("Export panel and Quick Export: a 9:16 sequence starts from its own preset; Quick Export waits for offline media",
          "[app][export][qml][p1]") {
    resetSettings();
    clearMessages();
    {
        ExportController earlier; // landscape choices from an earlier session
        earlier.setLastPreset(QStringLiteral("youtube-1080p"));
        earlier.setLastQuickPreset(QStringLiteral("audio-wav"));
    }
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(controller);
    REQUIRE(exporter);
    controller->resetProject(emptyProject(4));
    shell.window->setProperty("workspace", 2);
    QQuickItem* content = shell.window->contentItem();
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = findWith(content, [](QQuickItem* i) { return i->property("sessionPicks").isValid(); });
        return panel != nullptr && panel->isVisible();
    }));
    CHECK(panel->property("presetId").toString() == QStringLiteral("youtube-1080p"));
    // A 9:16 sequence: Match Source (its own shape), not the landscape YouTube preset.
    controller->resetProject(emptyProject(4, 1080, 1920));
    REQUIRE(waitFor([&] { return panel->property("presetId").toString() == QStringLiteral("match-source"); }));
    REQUIRE(QMetaObject::invokeMethod(panel, "choosePreset", Q_ARG(QVariant, QVariant(QStringLiteral("vertical-1080x1920")))));
    CHECK(exporter->rememberedFor({}).value(QStringLiteral("preset")).toString() == QStringLiteral("vertical-1080x1920"));
    // Back to landscape: its own choice again.
    controller->resetProject(emptyProject(4));
    REQUIRE(waitFor([&] { return panel->property("presetId").toString() == QStringLiteral("youtube-1080p"); }));

    // Quick Export: offline media, so Export waits with Relink….
    Timeline tl("V1 |Aaaa|\nA1 |Aaaa|");
    controller->resetProject(tl.state, tl.seq);
    QObject* quick = shell.object("quickExportButton");
    REQUIRE(quick);
    CHECK(quick->property("presetId").toString() == QStringLiteral("audio-wav"));
    REQUIRE(QMetaObject::invokeMethod(quick, "clicked"));
    REQUIRE(waitFor([&] { return quick->property("checked").toBool(); }));
    QQuickItem* offline = nullptr;
    QQuickItem* go = nullptr;
    REQUIRE(waitFor([&] {
        offline = named(content, QStringLiteral("quickExportOffline"));
        go = named(content, QStringLiteral("quickExportGo"));
        return offline != nullptr && go != nullptr && offline->isVisible();
    }));
    CHECK_FALSE(go->isEnabled());
    spin(200);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("quick-export-offline.png"))));
    int relinkSpy = 0;
            QObject::connect(exporter, &ExportController::relinkRequested, [&] { ++relinkSpy; });
    REQUIRE(QMetaObject::invokeMethod(named(content, QStringLiteral("quickExportRelink")), "clicked"));
    CHECK(relinkSpy == 1);
    CHECK(waitFor([&] { return !quick->property("checked").toBool(); }));
    QObject* relinkDialog = shell.object("relinkDialog");
    if (relinkDialog)
        QMetaObject::invokeMethod(relinkDialog, "close");
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

TEST_CASE("Export Frame dialog: the sizes and the markers mode; Markers export menu rows; screenshots",
          "[app][monitor][qml][screenshot][p1]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            A11yScope a11y;
            Shell shell;
            REQUIRE(shell.window);
            INFO(join(shell.errors));
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.theme->setProperty("dark", dark);
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* monitors = shell.engine.singletonInstance<MonitorBridge*>("Wabcut", "Monitors");
            REQUIRE(controller);
            REQUIRE(monitors);
            Timeline tl("V1 |....|");
            controller->resetProject(tl.state, tl.seq);
            {
                command::CommandEngine engine(controller->session());
                for (const auto& [frame, name, highlight] : {std::tuple{0, "Intro", false}, std::tuple{2, "Ace", true}}) {
                    command::CommandRequest add;
                    add.command = "marker.add";
                    add.params = command::Json{{"time", f(frame).value}, {"name", name}, {"type", "chapter"},
                                               {"sequence", tl.seq.value}};
                    if (highlight)
                        add.params["highlight"] = command::Json{{"score", 0.8}};
                    REQUIRE(engine.execute(add).ok());
                }
            }
            QObject* dialog = shell.object("exportFrameDialog");
            REQUIRE(dialog);

            // Export Frame: the YouTube thumbnail size makes it a JPEG under 2 MB.
            monitors->requestExportFrame(false);
            REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
            REQUIRE(QMetaObject::invokeMethod(dialog, "chooseSize", Q_ARG(QVariant, QVariant(QStringLiteral("thumbnail")))));
            CHECK(dialog->property("format").toString() == QStringLiteral("jpeg"));
            CHECK(dialog->property("keepUnder").toBool());
            spin(300);
            QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            CHECK(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-frame-sizes.png")
                                                             : QStringLiteral("export-frame-sizes-light.png"))));
            QMetaObject::invokeMethod(dialog, "close");
            REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));

            // Export Frames at Markers from the Highlights side: the highlight markers first, a count on the button.
            monitors->requestExportFramesAtMarkers(true);
            REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
            CHECK(dialog->property("mode").toString() == QStringLiteral("markers"));
            CHECK(dialog->property("markers").toString() == QStringLiteral("highlights"));
            CHECK(dialog->property("markerCount").toInt() == 1);
            QQuickItem* ok = named(shell.window->contentItem(), QStringLiteral("exportFrameOk"));
            if (!ok) {
                const auto items = dialog->findChildren<QQuickItem*>(QStringLiteral("exportFrameOk"));
                ok = items.isEmpty() ? nullptr : items.front();
            }
            REQUIRE(ok);
            CHECK(ok->property("text").toString() == QStringLiteral("Export 1 still(s)"));
            spin(300);
            image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            CHECK(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-frames-at-markers.png")
                                                             : QStringLiteral("export-frames-at-markers-light.png"))));
            QMetaObject::invokeMethod(dialog, "close");

            // The Markers menu's row and the Markers panel's export menu rows exist.
            CHECK(MonitorBridge::handlesCommand(QStringLiteral("marker.exportFrames")));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
    resetSettings();
}
