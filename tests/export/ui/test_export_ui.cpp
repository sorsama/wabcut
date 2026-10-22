// ExportController (the Export workspace's bridge) and the Export workspace on the real QML shell: preset and
// override mapping, plain-language errors, the job queue model (progress, cancel, reveal, offline media), the
// default output names, and an offscreen screenshot of the workspace with a running job (synthetic source).
// Uses tests/app's ShellFixture: one QGuiApplication per process, Qt message capture, a hidden Main.qml.

#include "CaptionsBridge.h"
#include "ExportController.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/Transcript.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/media/FrameHandle.h>
#include <wabcut/media/Probe.h>
#include <wabcut/playback/video/Evaluate.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJSValue>
#include <QQuickItem>
#include <QUrl>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;

const QColor kViolet(0x9B, 0x6C, 0xFF); // design language v3 violet, the default accent

std::filesystem::path freshDir(const char* name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "export_ui" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

QString q(const std::filesystem::path& p) {
    return QString::fromStdU16String(p.u16string());
}

Ticks seconds(std::int64_t s) {
    return Ticks::fromSeconds(s);
}

/// One 640x360 30 fps sequence of `length`, optionally with an audio clip of `audioFile` on A1.
ProjectState makeProject(Ticks length, const char* audioFile = nullptr, bool offline = false) {
    ProjectState state;
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Interview";
    seq.width = 640;
    seq.height = 360;
    seq.frameRate = kRate30;
    seq.duration = length;
    if (audioFile) {
        const std::string path = (std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / audioFile).generic_string();
        auto info = media::probe(path);
        REQUIRE(info.has_value());
        MediaItem item;
        item.id = state.ids.allocate<MediaId>();
        item.name = audioFile;
        item.locator.path = path;
        item.locator.offline = offline;
        item.info = *info;
        Clip clip;
        clip.id = state.ids.allocate<ClipId>();
        clip.media = item.id;
        clip.stream = {StreamKind::Audio, 0};
        clip.timelineRange = {Ticks{}, length};
        Track track;
        track.id = state.ids.allocate<TrackId>();
        track.kind = TrackKind::Audio;
        track.clips.push_back(std::move(clip));
        seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
        state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    }
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

const ExportJobModel::Row* row(const ExportController& c, const QString& jobId) {
    return c.jobs()->find(jobId);
}

bool waitFinished(const ExportController& c, const QString& jobId, int timeoutMs = 60'000) {
    return waitFor([&] { return row(c, jobId) && row(c, jobId)->finished; }, timeoutMs);
}

/// A synthetic source that holds frame `gate` until released, so a job stays "running" for a screenshot.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        changed.notify_all();
    }
};

class GatedSource final : public ex::IVideoFrameSource {
public:
    GatedSource(std::unique_ptr<ex::IVideoFrameSource> inner, std::int64_t gate, std::shared_ptr<Gate> state)
        : m_inner(std::move(inner)), m_gate(gate), m_state(std::move(state)) {}
    ~GatedSource() override { stop(); }
    std::string description() const override { return m_inner->description(); }
    int maxInFlight() const override { return m_inner->maxInFlight(); }
    std::expected<void, Error> request(std::int64_t frameIndex) override {
        return m_inner->request(frameIndex);
    }
    std::expected<media::FrameHandle, Error> next() override {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(4)); // a renderer's pace: progress keeps publishing
        if (m_taken++ >= m_gate) {
            std::unique_lock lock(m_state->mutex);
            m_state->changed.wait(lock, [&] { return m_state->open || m_stopped; });
        }
        return m_inner->next();
    }
    void stop() override {
        {
            std::lock_guard lock(m_state->mutex);
            m_stopped = true;
        }
        m_state->changed.notify_all();
        m_inner->stop();
    }

private:
    std::unique_ptr<ex::IVideoFrameSource> m_inner;
    std::int64_t m_gate;
    std::int64_t m_taken = 0;
    std::shared_ptr<Gate> m_state;
    bool m_stopped = false;
};

ex::VideoSourceFactory gatedFactory(std::int64_t gate, std::shared_ptr<Gate> state) {
    return [gate, state](const ProjectState&, const Sequence&, const ex::VideoSourceFormat& format)
               -> std::expected<std::unique_ptr<ex::IVideoFrameSource>, Error> {
        return std::make_unique<GatedSource>(ex::makeSyntheticVideoSource(format), gate, state);
    };
}

ex::ExportRequest requestFor(const ProjectState& state, const char* presetId,
                             const std::filesystem::path& out) {
    ex::ExportRequest r;
    r.snapshot = std::make_shared<const ProjectState>(state);
    r.sequence = state.sequences.front()->id;
    r.settings = ex::findPreset(presetId)->settings;
    r.output = out;
    r.videoSource = ex::syntheticVideoSourceFactory();
    return r;
}

} // namespace

TEST_CASE("ExportController maps presets and overrides and explains errors in plain language",
          "[export][app]") {
    ExportController::setUserPresetFolder(freshDir("no_user_presets")); // nothing of the user's
    ExportController controller;
    const QVariantList presets = controller.presets();
    // The six general ones, the gaming presets, the four HDR ones (I.9, 0.2), then the sixteen formats of 0.2.
    REQUIRE(presets.size() == 38);
    QStringList ids;
    QStringList names;
    for (const QVariant& p : presets) {
        const QVariantMap m = p.toMap();
        ids << m.value(QStringLiteral("id")).toString();
        names << m.value(QStringLiteral("name")).toString();
        CHECK_FALSE(m.value(QStringLiteral("name")).toString().isEmpty());
        CHECK(m.value(QStringLiteral("name")).toString() != m.value(QStringLiteral("id")).toString());
        CHECK_FALSE(m.value(QStringLiteral("description")).toString().isEmpty());
        CHECK_FALSE(m.value(QStringLiteral("icon")).toString().isEmpty());
    }
    CHECK(ids.mid(0, 6) == QStringList{QStringLiteral("match-source"), QStringLiteral("youtube-1080p"),
                                       QStringLiteral("youtube-4k"), QStringLiteral("vertical-1080x1920"),
                                       QStringLiteral("prores-422-hq"), QStringLiteral("audio-wav")});
    CHECK(names.contains(QStringLiteral("Discord 25 MB")));
    CHECK(names.contains(QStringLiteral("GIF (memes)")));
    CHECK(ids.mid(18, 4) == QStringList{QStringLiteral("youtube-hdr-pq"), QStringLiteral("youtube-hdr-pq-av1"),
                                        QStringLiteral("hdr-hlg"), QStringLiteral("prores-hdr-pq")});
    CHECK(presets[22].toMap().value(QStringLiteral("category")) == QStringLiteral("formats"));

    // Overrides on top of a preset, with the dependent defaults following the codec / container.
    auto s = ExportController::settingsFor(QStringLiteral("match-source"),
                                           {{QStringLiteral("container"), QStringLiteral("mov")},
                                            {QStringLiteral("videoCodec"), QStringLiteral("prores")},
                                            {QStringLiteral("range"), QStringLiteral("inOut")},
                                            {QStringLiteral("width"), 1280},
                                            {QStringLiteral("height"), 720}});
    REQUIRE(s.has_value());
    CHECK(s->container == ex::Container::Mov);
    CHECK(s->video.codec == ex::VideoCodec::ProRes);
    CHECK(s->video.pixelFormat == ex::OutputPixelFormat::Yuv422p10);
    CHECK(s->range == ex::RangeMode::InOut);
    CHECK(s->video.width == 1280);

    s = ExportController::settingsFor(QStringLiteral("prores-422-hq"),
                                      {{QStringLiteral("container"), QStringLiteral("wav")}});
    REQUIRE(s.has_value());
    CHECK_FALSE(s->video.enabled);
    CHECK(s->audio.codec == ex::AudioCodec::Pcm24);

    s = ExportController::settingsFor(
        QStringLiteral("match-source"),
        {{QStringLiteral("encoder"), QStringLiteral("software")}, {QStringLiteral("quality"), 20}});
    REQUIRE(s.has_value());
    CHECK(s->video.encoder == "libx264");
    CHECK(s->video.quality == 20);

    s = ExportController::settingsFor(QStringLiteral("match-source"),
                                      {{QStringLiteral("container"), QStringLiteral("avi")}});
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("no-such-preset"), {}).has_value());

    // Plain language.
    const QString offline = ExportController::plainError(Error{
        ErrorCode::NotFound, "Original media offline or missing: a.mp4 (C:/media/a.mp4). Relink it to export "
                             "(exports never use proxies)."});
    CHECK(offline.startsWith(QStringLiteral("Some original media is offline or missing: a.mp4")));
    CHECK(offline.contains(QStringLiteral("Relink it, then export again.")));
    CHECK(ExportController::plainError(Error{ErrorCode::IoError, "The output folder does not exist: C:/x"}) ==
          QStringLiteral("The folder does not exist. Choose another location."));
    CHECK(ExportController::plainError(
              Error{ErrorCode::Unsupported, "No working hevc encoder for 7680x4320 yuv420p10 (x)"})
              .startsWith(QStringLiteral("No encoder on this computer can write this format.")));
    CHECK(ExportController::plainError(Error{ErrorCode::Cancelled, "Export cancelled"}) ==
          QStringLiteral("Export cancelled."));
    CHECK(ExportController::plainError(
              Error{ErrorCode::InvalidArgument, "MP4 holds AAC audio; use MOV, MKV or WAV for PCM"})
              .endsWith(QLatin1Char('.')));
}

TEST_CASE("ExportController: HDR masters of an HDR sequence through the dynamic range override (0.2)", "[export][app][hdr]") {
    ProjectController project;
    ProjectState state = makeProject(seconds(2));
    ExportController controller;
    controller.setProject(&project);

    // Direct Rec.709 (SDR): no HDR choice, SDR out.
    project.resetProject(state);
    QVariantMap d = controller.describe({}, QStringLiteral("youtube-1080p"));
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(d.value(QStringLiteral("hdrAvailable")).toBool());
    CHECK_FALSE(d.value(QStringLiteral("hdr")).toBool());
    // A PQ override is refused in plain language: the sequence is not HDR.
    d = controller.describe({}, QStringLiteral("youtube-1080p"), {{QStringLiteral("dynamicRange"), QStringLiteral("pq")}});
    CHECK_FALSE(d.value(QStringLiteral("ok")).toBool());

    // Direct PQ (HDR): HEVC offers HDR; Auto writes PQ once 10-bit, the override switches HEVC to 10-bit itself.
    state.colorManagement.mode = ColorManagementMode::HdrPq;
    project.resetProject(state);
    d = controller.describe({}, QStringLiteral("youtube-hdr-pq"));
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("hdrAvailable")).toBool());
    CHECK(d.value(QStringLiteral("hdr")).toBool());
    CHECK(d.value(QStringLiteral("dynamicRange")).toString().contains(QStringLiteral("PQ")));
    auto s = ExportController::settingsFor(QStringLiteral("match-source"), {{QStringLiteral("videoCodec"), QStringLiteral("hevc")},
                                                                         {QStringLiteral("dynamicRange"), QStringLiteral("hlg")}});
    REQUIRE(s.has_value());
    CHECK(s->video.dynamicRange == ex::DynamicRange::Hlg);
    CHECK(s->video.pixelFormat == ex::OutputPixelFormat::Yuv420p10);
    d = controller.describe({}, QStringLiteral("youtube-1080p"), {{QStringLiteral("dynamicRange"), QStringLiteral("sdr")}});
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(d.value(QStringLiteral("hdr")).toBool());
    CHECK(d.value(QStringLiteral("dynamicRange")).toString().contains(QStringLiteral("tone-mapped")));
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("match-source"), {{QStringLiteral("dynamicRange"), QStringLiteral("dolby")}})
                    .has_value());
}

TEST_CASE("ExportController keeps Premiere's Content Credentials settings: producer, thumbnail, AI training, certificate (0.2)",
          "[export][app][c2pa]") {
    {
        ExportController controller;
        CHECK(controller.credentialsProducer().isEmpty());
        CHECK(controller.credentialsThumbnail());     // on by default
        CHECK_FALSE(controller.credentialsDoNotTrain());
        CHECK(controller.signingIdentity().value(QStringLiteral("local")).toBool());
        controller.setCredentialsProducer(QStringLiteral("  Phosphor Films  "));
        controller.setCredentialsThumbnail(false);
        controller.setCredentialsDoNotTrain(true);
        controller.setContentCredentials(QStringLiteral("always"));
        const auto dir = freshDir("c2pa-settings");
        controller.setCredentialsCertificate(QUrl::fromLocalFile(q(dir / "chain.pem")).toString());
        controller.setCredentialsKey(q(dir / "key.pem"));
        // Files that do not exist: the dialog says why it cannot sign with them.
        const QVariantMap identity = controller.signingIdentity();
        CHECK_FALSE(identity.value(QStringLiteral("local")).toBool());
        CHECK_FALSE(identity.value(QStringLiteral("ready")).toBool());
        CHECK_FALSE(identity.value(QStringLiteral("error")).toString().isEmpty());
    }
    ExportController again; // persisted (QSettings)
    CHECK(again.credentialsProducer() == QStringLiteral("Phosphor Films"));
    CHECK_FALSE(again.credentialsThumbnail());
    CHECK(again.credentialsDoNotTrain());
    CHECK(again.contentCredentials() == QStringLiteral("always"));
    CHECK(again.credentialsCertificate().endsWith(QStringLiteral("chain.pem")));
    CHECK_FALSE(again.credentialsCertificate().startsWith(QStringLiteral("file:")));
    again.setCredentialsCertificate({});
    again.setCredentialsKey({});
    again.setContentCredentials(QStringLiteral("ai"));
    again.setCredentialsThumbnail(true);
    again.setCredentialsDoNotTrain(false);
    again.setCredentialsProducer({});
    CHECK(again.signingIdentity().value(QStringLiteral("local")).toBool());
}

TEST_CASE("ExportController queues exports of the current sequence and tracks them", "[export][app][media]") {
    if (!QFile::exists(QString::fromUtf8(WABCUT_TEST_MEDIA_DIR "/sine_48k.wav"))) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    clearMessages();
    const auto dir = freshDir("queue");
    ProjectController project;
    project.resetProject(makeProject(seconds(2), "sine_48k.wav"));
    ExportController controller;
    controller.setProject(&project);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    QStringList revealed;
    controller.setRevealHandler([&](const QString& path) {
        revealed << path;
        return true;
    });

    // describe(): what the export would write, and plain errors for impossible ranges.
    QVariantMap d = controller.describe({}, QStringLiteral("match-source"));
    CHECK(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("resolution")).toString() == QStringLiteral("640 × 360"));
    CHECK(d.value(QStringLiteral("frameRate")).toString() == QStringLiteral("30 fps"));
    CHECK(d.value(QStringLiteral("frames")).toString() == QStringLiteral("60"));
    CHECK(d.value(QStringLiteral("duration")).toString() == QStringLiteral("0:02"));
    CHECK(d.value(QStringLiteral("rangeAvailable")).toMap().value(QStringLiteral("inOut")).toBool() == false);
    d = controller.describe({}, QStringLiteral("match-source"),
                            {{QStringLiteral("range"), QStringLiteral("inOut")}});
    CHECK_FALSE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("error")).toString().contains(QStringLiteral("In and Out")));

    // Default output names: next to the project file, numbered when taken.
    controller.setProjectFile(dir / "project.wabcut");
    const QString first = controller.defaultOutputPath({}, QStringLiteral("match-source"));
    CHECK(first == QDir::toNativeSeparators(q(dir / "Interview.mp4")));
    {
        std::ofstream(dir / "Interview.mp4") << "taken";
    }
    CHECK(controller.defaultOutputPath({}, QStringLiteral("match-source")) ==
          QDir::toNativeSeparators(q(dir / "Interview (2).mp4")));

    // An audio-only and a video export run to completion; the model reports them.
    const QString wav = controller.queueExport({}, QStringLiteral("audio-wav"),
                                               {{QStringLiteral("output"), q(dir / "mix.wav")}});
    REQUIRE(wav == QStringLiteral("j1"));
    const QString mp4 = controller.queueExport(
        {}, QStringLiteral("match-source"), {{QStringLiteral("output"), q(dir / "cut")}}); // extension added
    REQUIRE_FALSE(mp4.isEmpty());
    CHECK(controller.busy());
    REQUIRE(waitFinished(controller, wav));
    REQUIRE(waitFinished(controller, mp4));
    CHECK_FALSE(controller.busy());
    CHECK(row(controller, wav)->state == QStringLiteral("done"));
    CHECK(row(controller, wav)->progress == 1.0);
    CHECK(row(controller, wav)->encoder == QStringLiteral("pcm_s24le"));
    CHECK(row(controller, wav)->detail.contains(QStringLiteral("realtime")));
    CHECK(row(controller, mp4)->state == QStringLiteral("done"));
    CHECK(row(controller, mp4)->title == QStringLiteral("cut.mp4"));
    CHECK_FALSE(row(controller, mp4)->encoder.isEmpty());
    CHECK(std::filesystem::is_regular_file(dir / "mix.wav"));
    CHECK(std::filesystem::is_regular_file(dir / "cut.mp4"));
    CHECK(controller.reveal(mp4));
    CHECK(revealed == QStringList{QDir::toNativeSeparators(q(dir / "cut.mp4"))});

    // Cancelling a queued job finishes it at once; the running one may still finish first.
    const QString longJob = controller.queueExport({}, QStringLiteral("prores-422-hq"),
                                                   {{QStringLiteral("output"), q(dir / "long.mov")}});
    const QString queued = controller.queueExport({}, QStringLiteral("audio-wav"),
                                                  {{QStringLiteral("output"), q(dir / "queued.wav")}});
    controller.cancel(queued);
    REQUIRE(waitFinished(controller, queued));
    CHECK(row(controller, queued)->state == QStringLiteral("cancelled"));
    CHECK(row(controller, queued)->detail == QStringLiteral("Export cancelled."));
    controller.cancel(longJob);
    REQUIRE(waitFinished(controller, longJob));
    CHECK((row(controller, longJob)->state == QStringLiteral("cancelled") ||
           row(controller, longJob)->state == QStringLiteral("done")));
    CHECK_FALSE(std::filesystem::exists(dir / "queued.wav"));

    // Errors in plain language: a folder that does not exist; offline media.
    const QString badFolder = controller.queueExport(
        {}, QStringLiteral("audio-wav"), {{QStringLiteral("output"), q(dir / "missing" / "a.wav")}});
    REQUIRE(waitFinished(controller, badFolder));
    CHECK(row(controller, badFolder)->state == QStringLiteral("failed"));
    CHECK(row(controller, badFolder)->detail ==
          QStringLiteral("The folder does not exist. Choose another location."));

    project.resetProject(makeProject(seconds(2), "sine_48k.wav", true));
    const QString offline = controller.queueExport({}, QStringLiteral("audio-wav"),
                                                   {{QStringLiteral("output"), q(dir / "offline.wav")}});
    REQUIRE(waitFinished(controller, offline));
    CHECK(row(controller, offline)->state == QStringLiteral("failed"));
    CHECK(row(controller, offline)
              ->detail.startsWith(QStringLiteral("Some original media is offline or missing")));

    // Bad settings never reach the queue.
    const int before = controller.jobs()->rowCount();
    CHECK(controller
              .queueExport({}, QStringLiteral("match-source"),
                           {{QStringLiteral("range"), QStringLiteral("inOut")}})
              .isEmpty());
    CHECK(controller.lastError().contains(QStringLiteral("In and Out")));
    CHECK(controller.jobs()->rowCount() == before);

    controller.clearFinished();
    CHECK(controller.jobs()->rowCount() == 0);
    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Export workspace screenshots: settings drawer, and the queue with a running job",
          "[export][app][screenshot]") {
    // Read by every QQmlEngine created from now on: the Qt installation's QML modules.
    qputenv("QML_IMPORT_PATH", QByteArrayLiteral(WABCUT_QT_QML_DIR));
    resetSettings();
    clearMessages();
    const auto dir = freshDir("screenshot");
    auto save = [](const QImage& image, const char* file) {
        REQUIRE_FALSE(image.isNull());
        CHECK(image.size() == QSize(kShotWidth, kShotHeight));
        const QString path = outputDir() + QLatin1Char('/') + QLatin1String(file);
        REQUIRE(image.save(path));
        UNSCOPED_INFO("wrote " << path.toStdString());
    };
    {
        Shell warmUp; // initializes the renderer once, like the app's screenshot suite
        INFO(join(warmUp.errors));
        REQUIRE(warmUp.window != nullptr);
        REQUIRE_FALSE(warmUp.grab().isNull());
    }

    // 1. The settings drawer open on the default preset (a fresh window per grab, see Shell::grab()).
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.window->setProperty("workspace", 2);
        QObject* drawer = shell.object("exportSettingsDrawer");
        REQUIRE(drawer != nullptr);
        drawer->setProperty("open", true);
        REQUIRE(shell.settle(true, kViolet));
        save(shell.grab(), "workspace-export-settings.png");
    }

    // 2. The queue: a finished job, a failed one (offline original) and one held at 50 % by a gated source.
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.window->setProperty("workspace", 2);
    auto* controller = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(controller != nullptr);

    const ProjectState silent = makeProject(seconds(10));
    const QString done = controller->queueRequest(requestFor(silent, "audio-wav", dir / "Interview mix.wav"),
                                                  QStringLiteral("Audio only"));
    ProjectState offlineProject = makeProject(seconds(2));
    if (QFile::exists(QString::fromUtf8(WABCUT_TEST_MEDIA_DIR "/sine_48k.wav"))) {
        offlineProject = makeProject(seconds(2), "sine_48k.wav", true);
    }
    const QString failed = controller->queueRequest(
        requestFor(offlineProject, "audio-wav", dir / "Promo cut.wav"), QStringLiteral("Audio only"));
    auto gate = std::make_shared<Gate>();
    ex::ExportRequest running = requestFor(silent, "match-source", dir / "Interview.mp4");
    running.videoSource = gatedFactory(150, gate); // held at frame 150 of 300
    const QString runningId = controller->queueRequest(std::move(running), QStringLiteral("Match Source"));

    REQUIRE(waitFinished(*controller, done));
    REQUIRE(waitFinished(*controller, failed));
    REQUIRE(waitFor(
        [&] {
            const auto* r = row(*controller, runningId);
            return r && r->state == QStringLiteral("running") && r->progress >= 0.3 && !r->fpsText.isEmpty();
        },
        30'000));
    CHECK(row(*controller, failed)->detail ==
          QStringLiteral(
              "Some original media is offline or missing: sine_48k.wav. Relink it, then export again."));
    REQUIRE(shell.settle(true, kViolet));
    save(shell.grab(), "workspace-export-job.png");

    gate->release();
    controller->cancel(runningId);
    REQUIRE(waitFinished(*controller, runningId));
    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

// Codex wave-3 P2-8 (docs/internal/reviews-codex-wave3.md): karaoke burn-in through the real bridges. The preview highlights the
// words the transcript says were spoken; the export must highlight the same ones, with an export range that does not
// start at 0. Failed before the fix: the export request carried no word timings, so the export shared the cue's time
// among its words by length and highlighted "world" 0.5 s before it is said.
TEST_CASE("Karaoke burn-in (wave-3 P2-8): the export highlights the words the preview highlights",
          "[export][app][captions][gpu][wave3]") {
    if (!QFile::exists(QString::fromUtf8(WABCUT_TEST_MEDIA_DIR "/sine_48k.wav"))) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    auto* project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* transcripts = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(project);
    REQUIRE(transcripts);
    REQUIRE(captions);
    REQUIRE(playback);
    REQUIRE(exporter);

    // A 2 s sequence: the audio clip, one cue "Hello world" across it, In/Out 0.5 s .. 1.5 s.
    ProjectState state = makeProject(seconds(2), "sine_48k.wav");
    const MediaId media = state.media.front()->id;
    {
        auto seq = std::make_shared<Sequence>(*state.sequences.front());
        CaptionTrack track;
        track.id = state.ids.allocate<CaptionTrackId>();
        track.name = "English";
        track.enabled = true;
        track.cues = {{Ticks{}, seconds(2), "Hello world"}};
        seq->captionTracks = {track};
        seq->markIn = Ticks{kTicksPerSecond / 2};
        seq->markOut = Ticks{kTicksPerSecond * 3 / 2};
        state = state.withSequence(std::move(seq));
    }
    project->resetProject(state);
    // The transcript: "Hello" at once, "world" only at 1.7 s (uneven: a pause between them).
    auto words = std::make_shared<ai::Transcript>();
    words->language = "en";
    words->words.push_back({"Hello", Ticks{}, Ticks{kTicksPerSecond / 5}, 0.9f});
    words->words.push_back({"world", Ticks{kTicksPerSecond * 17 / 10}, Ticks{kTicksPerSecond * 19 / 10}, 0.9f});
    transcripts->setUseAnalysisCache(false);
    transcripts->addTranscript(media, 0, words);
    captions->setOverlay(true);
    playback->setCaptionKaraoke(true);
    QCoreApplication::processEvents();

    // The preview at 1.2 s: only "Hello" has been said.
    const Ticks at{kTicksPerSecond * 6 / 5};
    REQUIRE(playback->captionSettings());
    const auto preview = playback::evaluateCaption(*project->currentSequence(), at, *playback->captionSettings());
    REQUIRE(preview);
    CHECK(preview->highlightEnd == 5);

    // The export, queued through the controller like the Export workspace does; its video source factory sees the
    // request's format (a synthetic source keeps the job itself quick).
    struct Captured {
        std::mutex mutex;
        std::optional<ProjectState> project;
        std::optional<Sequence> sequence;
        std::optional<ex::VideoSourceFormat> format;
    };
    auto captured = std::make_shared<Captured>();
    exporter->setVideoSourceFactory([captured](const ProjectState& p, const Sequence& s, const ex::VideoSourceFormat& f)
                                        -> std::expected<std::unique_ptr<ex::IVideoFrameSource>, Error> {
        std::lock_guard lock(captured->mutex);
        captured->project = p;
        captured->sequence = s;
        captured->format = f;
        return ex::makeSyntheticVideoSource(f);
    });
    const auto dir = freshDir("karaoke");
    const QString job = exporter->queueExport({}, QStringLiteral("match-source"),
                                              {{QStringLiteral("output"), q(dir / "karaoke.mp4")},
                                               {QStringLiteral("captions"), QStringLiteral("burnIn")},
                                               {QStringLiteral("range"), QStringLiteral("inOut")}});
    INFO(exporter->lastError().toStdString());
    REQUIRE_FALSE(job.isEmpty());
    REQUIRE(waitFinished(*exporter, job));
    exporter->setVideoSourceFactory({});
    std::lock_guard lock(captured->mutex);
    REQUIRE(captured->format);
    REQUIRE(captured->format->burnInCaptions);
    CHECK(captured->format->start == Ticks{kTicksPerSecond / 2}); // a range that does not start at 0

    // That request rendered by the export renderer at 1.2 s: "Hello" in the highlight colour, "world" not.
    ex::VideoSourceFormat one = *captured->format;
    one.start = at;
    one.frameCount = 1;
    ex::RenderedSourceOptions options;
    options.renderThreads = 1;
    options.maxInFlight = 1;
    auto source = ex::renderedVideoSourceFactory(options)(*captured->project, *captured->sequence, one);
    REQUIRE(source);
    REQUIRE((*source)->request(0));
    auto picture = (*source)->next();
    (*source)->stop();
    REQUIRE(picture);
    const media::CpuPlanes* cpu = picture->cpu();
    REQUIRE(cpu);
    const media::Plane& plane = cpu->planes[0];
    int leftHighlight = 0;
    int rightHighlight = 0;
    int rightText = 0;
    for (int y = picture->height / 2; y < picture->height; ++y) {
        const std::uint8_t* row = plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride;
        for (int x = 0; x < picture->width; ++x) {
            const int r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            const bool highlight = r > 200 && g > 140 && g < 235 && b < 140; // the karaoke colour (warm yellow)
            const bool text = r > 200 && g > 200 && b > 200;                  // the caption's own colour (white)
            if (x < picture->width / 2 - 2) {
                leftHighlight += highlight ? 1 : 0;
            } else if (x > picture->width / 2 + 2) {
                rightHighlight += highlight ? 1 : 0;
                rightText += text ? 1 : 0;
            }
        }
    }
    INFO("highlight left " << leftHighlight << ", right " << rightHighlight << "; plain text right " << rightText);
    CHECK(leftHighlight > 50);
    CHECK(rightHighlight < 5);
    CHECK(rightText > 50);
}

// Wave-3 audit: the default export name comes from the sequence name, which comes from the project file. A name that
// is a Windows device ("NUL", "COM1.x") must not send the export into that device.
TEST_CASE("Default export names (wave-3 audit): a sequence name is never a Windows device", "[export][app][wave3]") {
    const auto dir = freshDir("device-names");
    for (const auto& [name, expected] : {std::pair{"NUL", "_NUL.mp4"}, std::pair{"com1.backup.", "_com1.backup.mp4"},
                                         std::pair{"Interview", "Interview.mp4"}}) {
        ProjectState state = makeProject(seconds(2));
        auto seq = std::make_shared<Sequence>(*state.sequences.front());
        seq->name = name;
        state = state.withSequence(std::move(seq));
        ProjectController project;
        project.resetProject(state);
        ExportController controller;
        controller.setProject(&project);
        controller.setProjectFile(dir / "project.wabcut");
        INFO(name);
        CHECK(controller.defaultOutputPath({}, QStringLiteral("match-source"))
                  .endsWith(QDir::separator() + QString::fromLatin1(expected)));
    }
}

// 0.2 (Premiere parity): Quick Export, Send to Media Encoder (held jobs, Start Queue), Save / Delete Preset, the custom
// range, what describe() tells the drawer about a format, watch folders, the CPU limit, and the new Export workspace
// pieces on the real QML shell without a Qt warning.
TEST_CASE("Export workspace 0.2: Quick Export the queue presets watch folders and the CPU limit", "[export][app][parity]") {
    const auto dir = freshDir("parity");
    ExportController::setUserPresetFolder(dir / "presets");
    ProjectController project;
    project.resetProject(makeProject(seconds(2)));
    ExportController controller;
    controller.setProject(&project);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    controller.setProjectFile(dir / "Interview.wabcut");

    // What the drawer shows for Premiere's formats.
    QVariantMap d = controller.describe({}, QStringLiteral("png-sequence"));
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("isImage")).toBool());
    CHECK(d.value(QStringLiteral("extension")) == QStringLiteral(".png"));
    CHECK(d.value(QStringLiteral("alphaAvailable")).toBool());
    CHECK(d.value(QStringLiteral("output")).toString().endsWith(QStringLiteral("Interview.png")));
    d = controller.describe({}, QStringLiteral("audio-mp3"));
    CHECK(d.value(QStringLiteral("audioOnly")).toBool());
    d = controller.describe({}, QStringLiteral("match-source"));
    CHECK(d.value(QStringLiteral("range")) == QStringLiteral("Entire Source"));
    CHECK(d.value(QStringLiteral("rangeIn")) == QStringLiteral("00:00:00:00"));
    CHECK(d.value(QStringLiteral("rangeOut")) == QStringLiteral("00:00:02:00"));
    CHECK(d.value(QStringLiteral("subtitlesAvailable")).toBool());
    CHECK_FALSE(d.value(QStringLiteral("alphaAvailable")).toBool());
    d = controller.describe({}, QStringLiteral("match-source"),
                            {{QStringLiteral("range"), QStringLiteral("custom")},
                             {QStringLiteral("rangeIn"), QStringLiteral("00:00:00:15")},
                             {QStringLiteral("rangeOut"), QStringLiteral("00:00:01:15")}});
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("range")) == QStringLiteral("Custom"));
    CHECK(d.value(QStringLiteral("frames")) == QStringLiteral("30"));
    d = controller.describe({}, QStringLiteral("match-source"),
                            {{QStringLiteral("rangeIn"), QStringLiteral("00:00:01:00")},
                             {QStringLiteral("rangeOut"), QStringLiteral("00:00:00:10")}});
    CHECK_FALSE(d.value(QStringLiteral("ok")).toBool());
    d = controller.describe({}, QStringLiteral("match-source"),
                            {{QStringLiteral("metadata"), QVariantMap{{QStringLiteral("startTimecode"), QStringLiteral("01:00:00:00")},
                                                                     {QStringLiteral("xmp"), QStringLiteral("sidecar")}}}});
    CHECK(d.value(QStringLiteral("ok")).toBool());

    // Save Preset, then it is a preset like the others; Delete.
    const QVariantMap saved = controller.savePreset(QStringLiteral("Podcast 44k"), QStringLiteral("for the feed"),
                                                    QStringLiteral("audio-wav"), {{QStringLiteral("sampleRate"), 44100}});
    REQUIRE(saved.value(QStringLiteral("ok")).toBool());
    const QString userId = saved.value(QStringLiteral("id")).toString();
    CHECK(userId == QStringLiteral("user-podcast-44k"));
    const auto user = [&] {
        for (const QVariant& p : controller.presets()) {
            if (p.toMap().value(QStringLiteral("id")) == userId) {
                return p.toMap();
            }
        }
        return QVariantMap{};
    }();
    CHECK(user.value(QStringLiteral("user")).toBool());
    CHECK(user.value(QStringLiteral("name")) == QStringLiteral("Podcast 44k"));
    CHECK_FALSE(controller.savePreset(QStringLiteral("Podcast 44k"), {}, QStringLiteral("audio-wav")).value(QStringLiteral("ok")).toBool());
    REQUIRE(ExportController::settingsFor(userId, {}).has_value());
    CHECK(ExportController::settingsFor(userId, {})->audio.sampleRate == 44100);

    // Quick Export with it.
    const QString quick = controller.quickExport(userId, q(dir / "quick.wav"));
    REQUIRE_FALSE(quick.isEmpty());
    REQUIRE(waitFinished(controller, quick));
    CHECK(row(controller, quick)->state == QStringLiteral("done"));
    CHECK(std::filesystem::is_regular_file(dir / "quick.wav"));

    // Send to Media Encoder: two destinations wait for Start Queue.
    const QStringList held = controller.sendToQueue({}, {QStringLiteral("audio-wav"), QStringLiteral("audio-aiff")}, {}, true);
    REQUIRE(held.size() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    QCoreApplication::processEvents();
    CHECK(row(controller, held[0])->state == QStringLiteral("held"));
    CHECK(row(controller, held[1])->state == QStringLiteral("held"));
    controller.startQueue();
    REQUIRE(waitFinished(controller, held[0]));
    REQUIRE(waitFinished(controller, held[1]));
    CHECK(row(controller, held[1])->state == QStringLiteral("done"));
    CHECK(controller.sendToQueue({}, {QStringLiteral("no-such")}).isEmpty());
    CHECK_FALSE(controller.lastError().isEmpty());

    REQUIRE(controller.deletePreset(userId));
    CHECK_FALSE(ExportController::settingsFor(userId, {}).has_value());

    // Watch folders.
    const QVariantMap watch = controller.addWatchFolder(q(dir / "Drop"), QStringLiteral("audio-wav"));
    REQUIRE(watch.value(QStringLiteral("ok")).toBool());
    REQUIRE(controller.watchFolders().size() == 1);
    CHECK(controller.watchFolders()[0].toMap().value(QStringLiteral("presetName")) == QStringLiteral("Audio only"));
    controller.removeWatchFolder(watch.value(QStringLiteral("id")).toString());
    CHECK(controller.watchFolders().isEmpty());

    // The CPU limit preference.
    controller.setCpuLimit(50);
    CHECK(ex::exportThreadLimit() == ex::threadLimitForPercent(50));
    controller.setCpuLimit(100);
    CHECK(ex::exportThreadLimit() == 0);

    // The export commands over this controller's queue.
    const auto services = controller.commandServices();
    REQUIRE(services != nullptr);
    CHECK(services->presets().size() == ex::builtInPresets().size());
    // export.queue.remove drops that finished job only.
    REQUIRE(row(controller, quick) != nullptr);
    REQUIRE(row(controller, held[0]) != nullptr);
    REQUIRE(services->control(quick.toStdString(), "remove").has_value());
    REQUIRE(waitFor([&] { return row(controller, quick) == nullptr; }, 5'000));
    CHECK(row(controller, held[0]) != nullptr);
}

TEST_CASE("Export workspace 0.2: the new sections load without a Qt warning", "[export][app][parity]") {
    qputenv("QML_IMPORT_PATH", QByteArrayLiteral(WABCUT_QT_QML_DIR));
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.window->setProperty("workspace", 2);
    REQUIRE(waitFor([&] { return shell.object("exportEffectsSection") != nullptr; }, 10'000));
    CHECK(shell.object("exportFormatPresets") != nullptr);
    CHECK(shell.object("exportSendToQueue") != nullptr);
    CHECK(shell.object("quickExportButton") != nullptr);
    QObject* effects = shell.object("exportEffectsSection");
    effects->setProperty("open", true);
    // Every effect applied, so each one's options show (through the panel's overrides, like a click).
    for (const char* section : {"look", "imageOverlay", "nameOverlay", "timecodeOverlay", "limiter", "loudness"}) {
        QMetaObject::invokeMethod(effects, "setEffect", Q_ARG(QVariant, QVariant(QString::fromLatin1(section))),
                                  Q_ARG(QVariant, QVariant(QStringLiteral("applied"))), Q_ARG(QVariant, QVariant(true)));
    }
    REQUIRE(shell.settle(true, kViolet));
    const QVariantMap fx = effects->property("fx").value<QJSValue>().toVariant().toMap();
    CHECK(fx.value(QStringLiteral("look")).toMap().value(QStringLiteral("applied")).toBool());
    CHECK(fx.value(QStringLiteral("loudness")).toMap().value(QStringLiteral("applied")).toBool());
    QObject* queue = shell.object("exportQueueDialog");
    REQUIRE(queue != nullptr);
    QMetaObject::invokeMethod(queue, "openOn", Q_ARG(QVariant, QVariant(QStringLiteral("watch"))));
    REQUIRE(shell.settle(true, kViolet));
    QMetaObject::invokeMethod(queue, "close");
    QObject* manager = shell.object("exportPresetManager");
    REQUIRE(manager != nullptr);
    QMetaObject::invokeMethod(manager, "openWith", Q_ARG(QVariant, QVariant(QStringLiteral("match-source"))),
                              Q_ARG(QVariant, QVariant(QVariantMap{})));
    REQUIRE(shell.settle(true, kViolet));
    QMetaObject::invokeMethod(manager, "close");
    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

// Premiere's File > Export > Send to Media Encoder (Alt+Shift+M): the keymap command, from the Edit workspace, puts the
// current export into the queue held (Media Encoder's "Ready") and shows the queue.
TEST_CASE("Export workspace 0.2: Send to Media Encoder queues the export held and shows the queue", "[export][app][parity]") {
    qputenv("QML_IMPORT_PATH", QByteArrayLiteral(WABCUT_QT_QML_DIR));
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    auto* controller = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(controller != nullptr);
    const int before = controller->jobs()->rowCount();
    QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QStringLiteral("file.sendToMediaEncoder"))));
    REQUIRE(waitFor([&] { return controller->jobs()->rowCount() == before + 1; }, 10'000));
    CHECK(shell.window->property("workspacePreset").toInt() == 2); // a workspace made from the Export preset (0.1.3)
    const ExportJobModel::Row held = controller->jobs()->rows().back(); // a copy: the model changes below
    CHECK(held.state == QStringLiteral("held"));
    CHECK(held.held);
    QObject* queue = shell.object("exportQueueDialog");
    REQUIRE(queue != nullptr);
    REQUIRE(waitFor([&] { return queue->property("visible").toBool(); }, 5'000));
    REQUIRE(shell.settle(true, kViolet));
    controller->cancel(held.jobId);
    REQUIRE(waitFinished(*controller, held.jobId));
    CHECK(row(*controller, held.jobId)->state == QStringLiteral("cancelled"));
    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Export workspace 0.2 screenshots: effects and metadata, the queue, the presets, Quick Export", "[export][app][screenshot][parity]") {
    qputenv("QML_IMPORT_PATH", QByteArrayLiteral(WABCUT_QT_QML_DIR));
    resetSettings();
    clearMessages();
    const auto save = [](const QImage& image, const char* file) {
        REQUIRE_FALSE(image.isNull());
        const QString path = outputDir() + QLatin1Char('/') + QLatin1String(file);
        REQUIRE(image.save(path));
        UNSCOPED_INFO("wrote " << path.toStdString());
    };
    {
        Shell warmUp;
        REQUIRE(warmUp.window != nullptr);
        REQUIRE_FALSE(warmUp.grab().isNull());
    }
    // 1. The Effects & Metadata section open, scrolled into view.
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.window->setProperty("workspace", 2);
        REQUIRE(waitFor([&] { return shell.item("exportEffectsSection") != nullptr; }, 10'000));
        QQuickItem* effects = shell.item("exportEffectsSection");
        effects->setProperty("open", true);
        for (const char* section : {"look", "nameOverlay", "loudness"}) {
            QMetaObject::invokeMethod(effects, "setEffect", Q_ARG(QVariant, QVariant(QString::fromLatin1(section))),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("applied"))), Q_ARG(QVariant, QVariant(true)));
        }
        REQUIRE(shell.settle(true, kViolet));
        QQuickItem* flick = nullptr;
        for (QQuickItem* p = effects->parentItem(); p; p = p->parentItem()) {
            if (p->inherits("QQuickFlickable")) {
                flick = p;
                break;
            }
        }
        REQUIRE(flick != nullptr);
        auto* content = flick->property("contentItem").value<QQuickItem*>();
        flick->setProperty("contentY", effects->mapToItem(content, QPointF(0, 0)).y() - 8);
        REQUIRE(shell.settle(true, kViolet));
        save(shell.grab(), "workspace-export-effects.png");
        // The rest of the section: the panel scrolled to its end.
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toReal() - flick->height()));
        REQUIRE(shell.settle(true, kViolet));
        save(shell.grab(), "workspace-export-effects-end.png");
    }
    // 2. The export queue (Media Encoder) on its Watch Folders page, and 3. the preset manager.
    for (const char* which : {"queue", "presets", "quick"}) {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.window->setProperty("workspace", 2);
        REQUIRE(waitFor([&] { return shell.object("exportQueueDialog") != nullptr; }, 10'000));
        if (std::string_view(which) == "queue") {
            QMetaObject::invokeMethod(shell.object("exportQueueDialog"), "openOn", Q_ARG(QVariant, QVariant(QStringLiteral("queue"))));
        } else if (std::string_view(which) == "presets") {
            QMetaObject::invokeMethod(shell.object("exportPresetManager"), "openWith",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("match-source"))), Q_ARG(QVariant, QVariant(QVariantMap{})));
        } else {
            QObject* quick = shell.object("quickExportButton");
            REQUIRE(quick != nullptr);
            QMetaObject::invokeMethod(quick, "clicked");
        }
        REQUIRE(shell.settle(true, kViolet));
        save(shell.grab(), std::string_view(which) == "queue"     ? "export-queue-dialog.png"
                           : std::string_view(which) == "presets" ? "export-preset-manager.png"
                                                                  : "quick-export.png");
    }
    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}
