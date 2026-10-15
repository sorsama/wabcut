// M5o (gaming): "New gaming video" from the Home screen (GamingStart, GamingVideoCard), offscreen; no window is shown.
//   * describe(): size, the nominal frame rate (60 fps kept, 59.94 named), duration, codec, HDR, the flags that suggest
//     proxies (above 60 fps, 4K, 20 minutes) with the reason, one row per audio stream with Game / Mic / Discord /
//     "Audio n" suggested.
//   * inspect() probes off the GUI thread; a later call wins; a file that cannot be read says why.
//   * create() on real recordings made with the build's ffmpeg (skipped without one): the project is saved, its
//     sequence matches the recording's size and rate, each included stream is on its own named audio track in order,
//     an excluded stream is left out, tracks are added when the sequence has too few, proxies are asked for when
//     suggested; each step's failure is returned.
//   * The card in the Home screen: a dropped recording lands on it, the rows and badges, include / leave out and the
//     target tracks, Create with the keyboard (gamingRequested), the error line; accessibility; a screenshot.

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "BridgeIds.h"
#include "GamingStartBridge.h"
#include "HomeBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QJSValue>
#include <QMimeData>
#include <QProcess>
#include <QQmlComponent>
#include <QQuickItem>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// A probe result like a capture tool's: one video stream and the given audio streams ({channels, layout}).
MediaInfo recordingInfo(int width, int height, Rate rate, std::int64_t seconds,
                        const std::vector<std::pair<int, std::string>>& audio, std::string codec = "h264") {
    MediaInfo info;
    info.container = "matroska,webm";
    info.duration = Ticks::fromSeconds(seconds);
    info.fileSize = 6'650'000'000LL;
    VideoStreamInfo video;
    video.index = 0;
    video.codec = std::move(codec);
    video.width = width;
    video.height = height;
    video.frameRate = rate;
    video.duration = info.duration;
    info.video.push_back(video);
    int index = 1;
    for (const auto& [channels, layout] : audio) {
        AudioStreamInfo a;
        a.index = index++;
        a.codec = "aac";
        a.channels = channels;
        a.channelLayout = layout;
        a.duration = info.duration;
        info.audio.push_back(a);
    }
    return info;
}

QVariantList audioRows(const QVariantMap& recording) {
    return recording.value(QStringLiteral("audio")).toList();
}

QString audioField(const QVariantMap& recording, int row, const char* key) {
    return audioRows(recording).value(row).toMap().value(QString::fromLatin1(key)).toString();
}

/// The build's ffmpeg: $WABCUT_FFMPEG, else <build>/vcpkg_installed/<triplet>/tools/ffmpeg, else PATH.
QString findFfmpeg() {
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("ffmpeg.exe");
#else
    const QString exe = QStringLiteral("ffmpeg");
#endif
    if (const QString env = qEnvironmentVariable("WABCUT_FFMPEG"); !env.isEmpty() && QFileInfo::exists(env))
        return env;
    QDir dir(QCoreApplication::applicationDirPath());
    for (int up = 0; up < 6; ++up) {
        const QDir installed(dir.filePath(QStringLiteral("vcpkg_installed")));
        if (installed.exists()) {
            for (const QString& triplet : installed.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString candidate = installed.filePath(triplet + QStringLiteral("/tools/ffmpeg/") + exe);
                if (QFileInfo::exists(candidate))
                    return candidate;
            }
        }
        if (!dir.cdUp())
            break;
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

/// A one-second recording: H.264 `width` x `height` at `fps`, one AAC stream per entry of `channels` (OBS's multitrack
/// recording: game, mic, voice chat ...). Filter graph sources, as tools/gen_test_media.py (the vcpkg ffmpeg has no
/// lavfi input device).
bool makeRecording(const QString& ffmpeg, const QString& out, int width, int height, int fps, const std::vector<int>& channels) {
    QString graph = QStringLiteral("testsrc2=size=%1x%2:rate=%3:duration=1[v]").arg(width).arg(height).arg(fps);
    for (std::size_t i = 0; i < channels.size(); ++i) {
        graph += QStringLiteral(";sine=frequency=%1:sample_rate=48000:duration=1[a%2]")
                     .arg(330 + 110 * static_cast<int>(i))
                     .arg(i);
    }
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                     QStringLiteral("-y"),           QStringLiteral("-filter_complex"), graph,
                     QStringLiteral("-map"),         QStringLiteral("[v]")};
    for (std::size_t i = 0; i < channels.size(); ++i)
        args << QStringLiteral("-map") << QStringLiteral("[a%1]").arg(i);
    args << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-preset") << QStringLiteral("ultrafast")
         << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-c:a") << QStringLiteral("aac");
    for (std::size_t i = 0; i < channels.size(); ++i)
        args << QStringLiteral("-ac:a:%1").arg(i) << QString::number(channels[i]);
    args << out;
    QProcess process;
    process.start(ffmpeg, args);
    if (!process.waitForFinished(60000)) {
        process.kill();
        return false;
    }
    UNSCOPED_INFO(process.readAllStandardError().toStdString());
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && QFileInfo::exists(out);
}

/// Home and GamingStart over a real project file service (no window).
struct Rig {
    ProjectController controller;
    ProjectLifecycle lifecycle{&controller};
    HomeBridge home;
    GamingStartBridge start;
    QTemporaryDir dir;
    QStringList proxyRequests;
    int proxyAnswer = 1;

    Rig() {
        resetSettings();
        home.setController(&controller);
        home.setLifecycle(&lifecycle);
        start.setHome(&home);
        start.setLifecycle(&lifecycle);
        start.setController(&controller);
        start.setProxyRequester([this](const QStringList& keys) {
            proxyRequests << keys;
            return proxyAnswer;
        });
    }
    QUrl folder(const QString& sub = {}) const {
        return QUrl::fromLocalFile(sub.isEmpty() ? dir.path() : dir.filePath(sub));
    }
    bool inspect(const QString& file) {
        start.inspect(QUrl::fromLocalFile(file));
        return waitFor([&] { return !start.inspecting(); }, 20000);
    }
};

/// The audio clips of `track` (their source streams).
std::vector<int> streamsOn(const Track& track) {
    std::vector<int> streams;
    for (const Clip& clip : track.clips)
        streams.push_back(clip.stream.kind == StreamKind::Audio ? clip.stream.index : -1);
    return streams;
}

QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* visibleItem(Shell& shell, const QString& objectName) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) {
        return i->objectName() == objectName && i->isVisible();
    });
}

QVariantMap toMap(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toVariant().toMap();
    return value.toMap();
}

} // namespace

// ---- describe ------------------------------------------------------------------------------------------------------

TEST_CASE("Gaming start: a recording is described with its rate flags and suggested track names", "[app][gaming]") {
    SECTION("1080p60 with game and mic and voice chat: 60 fps kept and half an hour asks for proxies") {
        const QVariantMap r = GamingStartBridge::describe(
            recordingInfo(1920, 1080, Rate{60, 1}, 30 * 60 - 1, {{2, "stereo"}, {1, "mono"}, {2, "stereo"}}),
            QStringLiteral("C:/Videos/Ranked match.mkv"));
        REQUIRE(r.value("ok").toBool());
        CHECK(r.value("name").toString() == QStringLiteral("Ranked match"));
        CHECK(r.value("width").toInt() == 1920);
        CHECK(r.value("height").toInt() == 1080);
        CHECK(r.value("fps").toMap().value("num").toInt() == 60);
        CHECK(r.value("fps").toMap().value("den").toInt() == 1);
        CHECK(r.value("fps").toMap().value("text").toString() == QStringLiteral("60"));
        CHECK(r.value("sizeText").toString() == QStringLiteral("1920 × 1080"));
        CHECK(r.value("durationText").toString() == QStringLiteral("29:59"));
        CHECK(r.value("videoCodec").toString() == QStringLiteral("H.264"));
        CHECK_FALSE(r.value("highFps").toBool());
        CHECK_FALSE(r.value("is4k").toBool());
        CHECK(r.value("isLong").toBool()); // 29:59 is past 20 minutes
        CHECK(r.value("proxies").toBool());
        CHECK(r.value("proxyReason").toString().contains(QStringLiteral("29:59")));
        CHECK_FALSE(r.value("vfr").toBool());
        CHECK(r.value("hdr").toString().isEmpty());
        REQUIRE(audioRows(r).size() == 3);
        CHECK(audioField(r, 0, "suggested") == QStringLiteral("Game"));
        CHECK(audioField(r, 1, "suggested") == QStringLiteral("Mic"));
        CHECK(audioField(r, 2, "suggested") == QStringLiteral("Discord"));
        CHECK(audioField(r, 0, "layout") == QStringLiteral("Stereo"));
        CHECK(audioField(r, 1, "layout") == QStringLiteral("Mono"));
        CHECK(audioRows(r)[1].toMap().value("index").toInt() == 1);
        CHECK(audioRows(r)[1].toMap().value("stream").toInt() == 2);
    }
    SECTION("a short 1080p60 clip asks for nothing") {
        const QVariantMap r = GamingStartBridge::describe(recordingInfo(1920, 1080, Rate{60, 1}, 95, {{2, "stereo"}}),
                                                          QStringLiteral("C:/Videos/Clip.mp4"));
        CHECK_FALSE(r.value("proxies").toBool());
        CHECK(r.value("proxyReason").toString().isEmpty());
        CHECK(r.value("durationText").toString() == QStringLiteral("1:35"));
    }
    SECTION("4K at 120 fps for 45 minutes in HDR with a variable rate and five streams") {
        MediaInfo info = recordingInfo(3840, 2160, Rate{120, 1}, 45 * 60,
                                       {{2, "stereo"}, {1, "mono"}, {2, "stereo"}, {2, "stereo"}, {6, "5.1(side)"}}, "hevc");
        info.video.front().variableFrameRate = true;
        info.video.front().color.transfer = TransferFunction::PQ;
        const QVariantMap r = GamingStartBridge::describe(info, QStringLiteral("D:/Captures/Boss fight.mp4"));
        REQUIRE(r.value("ok").toBool());
        CHECK(r.value("is4k").toBool());
        CHECK(r.value("highFps").toBool());
        CHECK(r.value("isLong").toBool());
        CHECK(r.value("proxies").toBool());
        const QString reason = r.value("proxyReason").toString();
        CHECK(reason.contains(QStringLiteral("4K")));
        CHECK(reason.contains(QStringLiteral("120 fps")));
        CHECK(reason.contains(QStringLiteral("45:00")));
        CHECK(r.value("vfr").toBool());
        CHECK(r.value("hdr").toString() == QStringLiteral("PQ"));
        CHECK(r.value("videoCodec").toString() == QStringLiteral("HEVC"));
        CHECK(r.value("durationText").toString() == QStringLiteral("45:00"));
        CHECK(audioField(r, 3, "suggested") == QStringLiteral("Audio 4"));
        CHECK(audioField(r, 4, "suggested") == QStringLiteral("Audio 5"));
        CHECK(audioField(r, 4, "layout") == QStringLiteral("5.1"));
    }
    SECTION("59.94 fps and HLG; a turned phone capture; over an hour") {
        MediaInfo info = recordingInfo(1920, 1080, Rate{60000, 1001}, 3723, {{2, "stereo"}});
        info.video.front().color.transfer = TransferFunction::HLG;
        info.video.front().rotationDegrees = 90;
        const QVariantMap r = GamingStartBridge::describe(info, QStringLiteral("C:/Videos/Phone.mov"));
        CHECK(r.value("fps").toMap().value("text").toString() == QStringLiteral("59.94"));
        CHECK(r.value("fps").toMap().value("num").toInt() == 60000);
        CHECK(r.value("fps").toMap().value("den").toInt() == 1001);
        CHECK_FALSE(r.value("highFps").toBool());
        CHECK(r.value("hdr").toString() == QStringLiteral("HLG"));
        CHECK(r.value("width").toInt() == 1080);
        CHECK(r.value("height").toInt() == 1920);
        CHECK(r.value("durationText").toString() == QStringLiteral("1:02:03"));
    }
    SECTION("a variable rate capture's measured rate snaps to the nominal one (C.15 nominalRate)") {
        MediaInfo info = recordingInfo(2560, 1440, Rate{5991, 100}, 600, {{2, "stereo"}});
        info.video.front().variableFrameRate = true;
        const QVariantMap r = GamingStartBridge::describe(info, QStringLiteral("C:/Videos/OBS.mkv"));
        CHECK(r.value("vfr").toBool());
        CHECK(r.value("fps").toMap().value("num").toInt() == 60000); // 59.91 measured: 59.94
        CHECK(r.value("fps").toMap().value("den").toInt() == 1001);
        info.video.front().frameRate = Rate{14370, 100};             // 143.7 measured: 144
        const QVariantMap high = GamingStartBridge::describe(info, QStringLiteral("C:/Videos/OBS.mkv"));
        CHECK(high.value("fps").toMap().value("num").toInt() == 144);
        CHECK(high.value("fps").toMap().value("den").toInt() == 1);
    }
    SECTION("no video: not a recording") {
        MediaInfo info = recordingInfo(1920, 1080, Rate{60, 1}, 10, {{2, "stereo"}});
        info.video.clear();
        const QVariantMap r = GamingStartBridge::describe(info, QStringLiteral("C:/Videos/Voice.mkv"));
        CHECK_FALSE(r.value("ok").toBool());
        CHECK(r.value("error").toString().contains(QStringLiteral("Voice.mkv")));
    }
    CHECK(GamingStartBridge::suggestedTrackName(0) == QStringLiteral("Game"));
    CHECK(GamingStartBridge::suggestedTrackName(6) == QStringLiteral("Audio 7"));
}

// ---- inspect -------------------------------------------------------------------------------------------------------

TEST_CASE("Gaming start: inspect reads the recording off the GUI thread and the last call wins", "[app][gaming]") {
    QTemporaryDir dir;
    GamingStartBridge start;
    std::atomic<QThread*> probeThread{nullptr};
    std::atomic<int> probes{0};
    start.setProbe([&](const std::string& path) -> std::expected<MediaInfo, Error> {
        probeThread = QThread::currentThread();
        ++probes;
        if (path.find("slow") != std::string::npos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return recordingInfo(1280, 720, Rate{30, 1}, 10, {{2, "stereo"}});
        }
        if (path.find("broken") != std::string::npos)
            return std::unexpected(Error{ErrorCode::ParseError, "Invalid data found when processing input"});
        return recordingInfo(1920, 1080, Rate{60, 1}, 60, {{2, "stereo"}, {1, "mono"}});
    });
    CHECK(start.recording().isEmpty());
    CHECK_FALSE(start.inspecting());

    start.inspect(QUrl::fromLocalFile(dir.filePath(QStringLiteral("slow.mkv"))));
    CHECK(start.inspecting());
    start.inspect(QUrl::fromLocalFile(dir.filePath(QStringLiteral("fast.mkv"))));
    REQUIRE(waitFor([&] { return !start.inspecting(); }));
    REQUIRE(waitFor([&] { return probes.load() == 2; }));
    spin(300); // the slow probe's answer arrives after the fast one's: dropped
    CHECK(probeThread.load() != QCoreApplication::instance()->thread());
    CHECK(start.recording().value("ok").toBool());
    CHECK(start.recording().value("name").toString() == QStringLiteral("fast"));
    CHECK(start.recording().value("width").toInt() == 1920);

    start.inspect(QUrl::fromLocalFile(dir.filePath(QStringLiteral("broken.mkv"))));
    REQUIRE(waitFor([&] { return !start.inspecting(); }));
    CHECK_FALSE(start.recording().value("ok").toBool());
    CHECK(start.recording().value("error").toString().contains(QStringLiteral("broken.mkv")));
    CHECK(start.recording().value("error").toString().contains(QStringLiteral("Invalid data")));
    // Nothing to create from.
    CHECK(start.create(QStringLiteral("Broken"), QUrl::fromLocalFile(dir.path()), {}) ==
          QStringLiteral("Choose a game recording first."));

    start.clear();
    CHECK(start.recording().isEmpty());

    CHECK(start.isRecording(QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.mkv")))));
    CHECK(start.isRecording(QUrl::fromLocalFile(dir.filePath(QStringLiteral("b.MP4")))));
    CHECK(start.isRecording(QUrl::fromLocalFile(dir.filePath(QStringLiteral("c.flv")))));
    CHECK_FALSE(start.isRecording(QUrl::fromLocalFile(dir.filePath(QStringLiteral("d.wav")))));
    CHECK_FALSE(start.isRecording(QUrl::fromLocalFile(dir.filePath(QStringLiteral("e.xml")))));
    CHECK_FALSE(start.nameFilters().isEmpty());
}

// ---- create --------------------------------------------------------------------------------------------------------

TEST_CASE("Gaming start: a multi-stream recording becomes a saved project with one named track per included stream",
          "[app][gaming]") {
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty())
        SKIP("no ffmpeg (the vcpkg tools/ffmpeg of this build, $WABCUT_FFMPEG or PATH)");
    Rig rig;
    REQUIRE(rig.dir.isValid());

    // OBS-style: 60 fps, game (stereo), mic (mono), voice chat (stereo).
    const QString ranked = rig.dir.filePath(QStringLiteral("Ranked match.mkv"));
    REQUIRE(makeRecording(ffmpeg, ranked, 320, 180, 60, {2, 1, 2}));
    REQUIRE(rig.inspect(ranked));
    QVariantMap r = rig.start.recording();
    INFO(r.value("error").toString().toStdString());
    REQUIRE(r.value("ok").toBool());
    CHECK(r.value("width").toInt() == 320);
    CHECK(r.value("height").toInt() == 180);
    CHECK(r.value("fps").toMap().value("num").toInt() == 60);
    CHECK(r.value("fps").toMap().value("den").toInt() == 1);
    CHECK(r.value("fps").toMap().value("text").toString() == QStringLiteral("60"));
    CHECK(r.value("videoCodec").toString() == QStringLiteral("H.264"));
    CHECK_FALSE(r.value("proxies").toBool());
    REQUIRE(audioRows(r).size() == 3);
    CHECK(audioField(r, 0, "suggested") == QStringLiteral("Game"));
    CHECK(audioField(r, 1, "suggested") == QStringLiteral("Mic"));
    CHECK(audioField(r, 2, "suggested") == QStringLiteral("Discord"));
    CHECK(audioField(r, 0, "layout") == QStringLiteral("Stereo"));
    CHECK(audioField(r, 1, "layout") == QStringLiteral("Mono"));
    CHECK(audioField(r, 2, "layout") == QStringLiteral("Stereo"));

    // Create: the mic left out, the voice chat renamed.
    const QVariantList plan{
        QVariantMap{{"index", 0}, {"name", QStringLiteral("Game")}, {"include", true}},
        QVariantMap{{"index", 1}, {"name", QStringLiteral("Mic")}, {"include", false}},
        QVariantMap{{"index", 2}, {"name", QStringLiteral("Squad")}, {"include", true}},
    };
    QString error = rig.start.create(QStringLiteral("Ranked match"), rig.folder(QStringLiteral("Projects")), plan);
    INFO(error.toStdString());
    REQUIRE(error.isEmpty());
    const QString projectFile = rig.dir.filePath(QStringLiteral("Projects/Ranked match.wabcut"));
    CHECK(QFile::exists(projectFile));
    CHECK(QDir::fromNativeSeparators(rig.lifecycle.filePath()) == projectFile);
    CHECK_FALSE(rig.lifecycle.dirty());
    CHECK_FALSE(rig.lifecycle.demo());

    SequencePtr seq = rig.controller.currentSequence();
    REQUIRE(seq);
    CHECK(seq->width == 320);
    CHECK(seq->height == 180);
    CHECK(seq->frameRate.num == 60);
    CHECK(seq->frameRate.den == 1);
    REQUIRE(rig.controller.snapshot()->media.size() == 1);
    const MediaId media = rig.controller.snapshot()->media.front()->id;
    REQUIRE_FALSE(seq->videoTracks.empty());
    REQUIRE(seq->videoTracks.front()->clips.size() == 1);
    CHECK(seq->videoTracks.front()->clips[0].media == media);
    CHECK(seq->videoTracks.front()->clips[0].start() == Ticks{});
    REQUIRE(seq->audioTracks.size() == 3); // the default three: two used, one left empty
    CHECK(seq->audioTracks[0]->name == "Game");
    CHECK(seq->audioTracks[1]->name == "Squad");
    CHECK(streamsOn(*seq->audioTracks[0]) == std::vector<int>{0});
    CHECK(streamsOn(*seq->audioTracks[1]) == std::vector<int>{2});
    CHECK(seq->audioTracks[2]->clips.size() == 0); // the mic (stream 1) is nowhere
    CHECK(rig.proxyRequests.isEmpty());

    // The same name in the same folder: Home refuses it and nothing changes.
    REQUIRE(rig.inspect(ranked));
    error = rig.start.create(QStringLiteral("Ranked match"), rig.folder(QStringLiteral("Projects")), plan);
    CHECK_FALSE(error.isEmpty());
    CHECK(QDir::fromNativeSeparators(rig.lifecycle.filePath()) == projectFile);

    // 120 fps with four streams: proxies, and a fourth audio track added (named after its stream).
    const QString clutch = rig.dir.filePath(QStringLiteral("Clutch 120.mp4"));
    REQUIRE(makeRecording(ffmpeg, clutch, 160, 90, 120, {2, 1, 2, 2}));
    REQUIRE(rig.inspect(clutch));
    r = rig.start.recording();
    REQUIRE(r.value("ok").toBool());
    CHECK(r.value("highFps").toBool());
    CHECK(r.value("proxies").toBool());
    CHECK(r.value("proxyReason").toString().contains(QStringLiteral("120 fps")));
    REQUIRE(audioRows(r).size() == 4);
    error = rig.start.create(QStringLiteral("Clutch"), rig.folder(), {}); // every stream, the suggested names
    INFO(error.toStdString());
    REQUIRE(error.isEmpty());
    seq = rig.controller.currentSequence();
    CHECK(seq->frameRate.num == 120);
    CHECK(seq->frameRate.den == 1);
    CHECK(seq->width == 160);
    REQUIRE(seq->audioTracks.size() == 4);
    const std::vector<std::string> names{"Game", "Mic", "Discord", "Audio 4"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        INFO("A" << i + 1);
        CHECK(seq->audioTracks[i]->name == names[i]);
        CHECK(streamsOn(*seq->audioTracks[i]) == std::vector<int>{static_cast<int>(i)});
    }
    REQUIRE(rig.controller.snapshot()->media.size() == 1);
    CHECK(rig.proxyRequests == QStringList{mediaKey(rig.controller.snapshot()->media.front()->id)});
    CHECK_FALSE(rig.lifecycle.dirty());

    // Proxies refused: the project is still made and saved, and the message says what to do.
    rig.proxyAnswer = 0;
    REQUIRE(rig.inspect(clutch));
    error = rig.start.create(QStringLiteral("Clutch again"), rig.folder(), {});
    CHECK(error.contains(QStringLiteral("proxies")));
    CHECK(QFile::exists(rig.dir.filePath(QStringLiteral("Clutch again.wabcut"))));
    CHECK_FALSE(rig.lifecycle.dirty());
    CHECK(rig.controller.currentSequence()->audioTracks.size() == 4);
}

// ---- The card in the Home screen ----------------------------------------------------------------------------------

TEST_CASE("Home screen: the gaming card reads a dropped recording - rows and badges and Create - accessibility and a screenshot",
          "[app][gaming][screenshots][qml]") {
    A11yScope a11y;
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* start = shell.engine.singletonInstance<GamingStartBridge*>("Wabcut", "GamingStart");
    REQUIRE(start != nullptr);
    // A 1440p60 capture, 42 minutes, HEVC in HDR with a variable rate: three streams (game, mic, voice chat).
    start->setProbe([](const std::string&) -> std::expected<MediaInfo, Error> {
        MediaInfo info = recordingInfo(2560, 1440, Rate{60, 1}, 42 * 60 + 17, {{2, "stereo"}, {1, "mono"}, {2, "stereo"}},
                                       "hevc");
        info.video.front().variableFrameRate = true;
        info.video.front().color.transfer = TransferFunction::PQ;
        return info;
    });
    QStringList proxyRequests;
    start->setProxyRequester([&](const QStringList& keys) {
        proxyRequests << keys;
        return static_cast<int>(keys.size());
    });
    QTemporaryDir dir;
    const QString recording = dir.filePath(QStringLiteral("Ranked match 2026-10-12.mkv"));

    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(400);
    QQuickItem* screen = shell.item("homeScreen");
    REQUIRE(screen != nullptr);
    CHECK(screen->property("opened").toBool());
    QQuickItem* card = visibleItem(shell, QStringLiteral("homeGamingCard"));
    REQUIRE(card != nullptr);
    // First in the right column, above New project.
    QQuickItem* newProject = visibleItem(shell, QStringLiteral("homeNewProjectCard"));
    REQUIRE(newProject != nullptr);
    const QRectF cardRect = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    const QRectF newRect = newProject->mapRectToScene(QRectF(0, 0, newProject->width(), newProject->height()));
    CHECK(cardRect.bottom() <= newRect.top());
    CHECK(qAbs(cardRect.left() - newRect.left()) < 1);
    REQUIRE(visibleItem(shell, QStringLiteral("gamingDropZone")) != nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("gamingChooseButton")) != nullptr);
    CHECK(visibleItem(shell, QStringLiteral("gamingCreateButton")) == nullptr); // nothing to create from yet

    // Drag the recording over Home: the gaming card lights up, not the import.
    QQuickItem* zone = visibleItem(shell, QStringLiteral("gamingDropZone"));
    const QPoint at = zone->mapToScene(QPointF(zone->width() / 2, zone->height() / 2)).toPoint();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(recording)});
    QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &enter);
    QDragMoveEvent move(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &move);
    spin(150);
    CHECK(screen->property("dragRecording").toBool());
    CHECK(card->property("dragActive").toBool());
    QObject* importCard = shell.object("homeImportCard");
    REQUIRE(importCard != nullptr);
    CHECK_FALSE(importCard->property("dragActive").toBool());

    // Drop: the card reads it.
    QDropEvent dropped(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &dropped);
    REQUIRE(waitFor([&] { return start->recording().value("ok").toBool(); }));
    spin(200);
    CHECK(screen->property("opened").toBool()); // still Home: nothing imported yet
    CHECK_FALSE(card->property("dragActive").toBool());
    CHECK(visibleItem(shell, QStringLiteral("gamingDropZone")) == nullptr);
    QQuickItem* name = visibleItem(shell, QStringLiteral("gamingProjectName"));
    REQUIRE(name != nullptr);
    CHECK(name->property("text").toString() == QStringLiteral("Ranked match 2026-10-12"));
    QQuickItem* summary = visibleItem(shell, QStringLiteral("gamingSummary"));
    REQUIRE(summary != nullptr);
    CHECK(summary->property("text").toString().startsWith(QStringLiteral("2560 × 1440 · 60 fps · 42:17 · HEVC")));
    QQuickItem* sequenceSummary = visibleItem(shell, QStringLiteral("gamingSequenceSummary"));
    REQUIRE(sequenceSummary != nullptr);
    CHECK(sequenceSummary->property("text").toString() == QStringLiteral("Sequence 2560 × 1440 · 60 fps"));
    for (const char* badge : {"gamingBadge_long", "gamingBadge_vfr", "gamingBadge_hdr", "gamingNote_proxies",
                              "gamingNote_vfr", "gamingNote_hdr"}) {
        INFO(badge);
        CHECK(visibleItem(shell, QString::fromLatin1(badge)) != nullptr);
    }
    CHECK(visibleItem(shell, QStringLiteral("gamingBadge_4k")) == nullptr);
    CHECK(visibleItem(shell, QStringLiteral("gamingBadge_highFps")) == nullptr);
    const QStringList suggested{QStringLiteral("Game"), QStringLiteral("Mic"), QStringLiteral("Discord")};
    for (int i = 0; i < 3; ++i) {
        INFO("stream " << i);
        QQuickItem* field = visibleItem(shell, QStringLiteral("gamingTrackName_%1").arg(i));
        REQUIRE(field != nullptr);
        CHECK(field->property("text").toString() == suggested[i]);
        QQuickItem* target = visibleItem(shell, QStringLiteral("gamingTarget_%1").arg(i));
        REQUIRE(target != nullptr);
        CHECK(target->property("text").toString() == QStringLiteral("→ A%1").arg(i + 1));
    }

    // Leave the mic out with the keyboard: the voice chat moves up to A2.
    QQuickItem* includeMic = visibleItem(shell, QStringLiteral("gamingInclude_1"));
    REQUIRE(includeMic != nullptr);
    REQUIRE(pressWithKey(shell.window.get(), includeMic, Qt::Key_Space, QStringLiteral(" ")));
    spin(100);
    CHECK_FALSE(includeMic->property("checked").toBool());
    CHECK(visibleItem(shell, QStringLiteral("gamingTarget_1"))->property("text").toString() == QStringLiteral("Left out"));
    CHECK(visibleItem(shell, QStringLiteral("gamingTarget_2"))->property("text").toString() == QStringLiteral("→ A2"));
    CHECK_FALSE(visibleItem(shell, QStringLiteral("gamingTrackName_1"))->isEnabled());
    // Rename the voice chat track by typing.
    QQuickItem* chat = visibleItem(shell, QStringLiteral("gamingTrackName_2"));
    REQUIRE(chat != nullptr);
    chat->forceActiveFocus(Qt::TabFocusReason);
    QMetaObject::invokeMethod(chat, "selectAll");
    {
        QKeyEvent type(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier, QStringLiteral("Squad"));
        QCoreApplication::sendEvent(shell.window.get(), &type);
    }
    spin(50);
    CHECK(chat->property("text").toString() == QStringLiteral("Squad"));

    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/gaming-new-video.png")));
    }

    // Accessibility: every control named, the keyboard reaches each one.
    const QStringList issues = auditAccessibility(card);
    INFO(issues.join(QLatin1Char('\n')).toStdString());
    CHECK(issues.isEmpty());
    for (const char* control : {"gamingChangeButton", "gamingInclude_0", "gamingTrackName_0", "gamingInclude_2",
                                "gamingProjectName", "gamingBrowseButton", "gamingCreateButton"}) {
        INFO(control << ": " << describeTabChain(card).toStdString());
        CHECK(tabReaches(card, QString::fromLatin1(control)));
    }

    // Create (Space on the button): Home asks Main with the name, the folder and the plan.
    card->setProperty("folder", QUrl::fromLocalFile(dir.path()));
    QQmlComponent spyComponent(&shell.engine);
    spyComponent.setData(QByteArrayLiteral("import QtQuick\n"
                                           "Connections {\n"
                                           "    property var last: null\n"
                                           "    property int count: 0\n"
                                           "    function onGamingRequested(name, folder, tracks) {\n"
                                           "        last = { name: name, folder: folder, tracks: tracks }\n"
                                           "        ++count\n"
                                           "    }\n"
                                           "}\n"),
                         QUrl(QStringLiteral("qrc:/wabcut-test/GamingSpy.qml")));
    INFO(spyComponent.errorString().toStdString());
    std::unique_ptr<QObject> spy(spyComponent.createWithInitialProperties({{QStringLiteral("target"), QVariant::fromValue<QObject*>(screen)}}));
    REQUIRE(spy != nullptr);
    QQuickItem* createButton = visibleItem(shell, QStringLiteral("gamingCreateButton"));
    REQUIRE(createButton != nullptr);
    REQUIRE(pressWithKey(shell.window.get(), createButton, Qt::Key_Space, QStringLiteral(" ")));
    spin(100);
    REQUIRE(spy->property("count").toInt() == 1);
    const QVariantMap request = toMap(spy->property("last"));
    CHECK(request.value("name").toString() == QStringLiteral("Ranked match 2026-10-12"));
    CHECK(QDir::fromNativeSeparators(request.value("folder").toUrl().toLocalFile()) == dir.path());
    const QVariantList tracks = request.value("tracks").toList();
    REQUIRE(tracks.size() == 3);
    CHECK(tracks[0].toMap().value("include").toBool());
    CHECK_FALSE(tracks[1].toMap().value("include").toBool());
    CHECK(tracks[2].toMap().value("name").toString() == QStringLiteral("Squad"));
    CHECK(tracks[2].toMap().value("index").toInt() == 2);

    // Main runs it (GamingStart.create): the file is not really there, so the import step fails and says so; Home
    // shows it on the card and stays open.
    QQuickItem* errorLine = nullptr;
    REQUIRE(waitFor([&] {
        errorLine = visibleItem(shell, QStringLiteral("gamingError"));
        return errorLine != nullptr
               && errorLine->property("text").toString().contains(QStringLiteral("Ranked match 2026-10-12.mkv"));
    }, 5000));
    CHECK(proxyRequests.isEmpty());
    CHECK(screen->property("opened").toBool());

    // Home opening again starts from an empty card.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(200);
    CHECK(start->recording().isEmpty());
    CHECK(visibleItem(shell, QStringLiteral("gamingDropZone")) != nullptr);

    spy.reset();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
