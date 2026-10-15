// MediaBridge: real waveform peaks (media::WaveformPeaksService) for media files, placeholders for the rest, and
// library thumbnails (media::ThumbnailService) through the image://wabcut-thumbnails provider. The test media is
// written by the tests themselves (a PCM WAV and a BMP still), so the suite does not depend on the media fixture.

#include "ShellFixture.h"

#include "MediaBridge.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

constexpr int kSampleRate = 48000;

// 16-bit stereo PCM: one second of a 0.5-amplitude 440 Hz sine, then one second of silence.
bool writeTestWav(const QString& path) {
    const int frames = 2 * kSampleRate;
    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < kSampleRate; ++i) {
        const double v = 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * i / kSampleRate);
        const auto s = static_cast<std::int16_t>(std::lround(v * 32767.0));
        samples[static_cast<std::size_t>(i) * 2] = s;
        samples[static_cast<std::size_t>(i) * 2 + 1] = s;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const auto u32 = [&](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    file.write("RIFF", 4);
    u32(36 + dataBytes);
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(2);
    u32(kSampleRate);
    u32(kSampleRate * 4);
    u16(4);
    u16(16);
    file.write("data", 4);
    u32(dataBytes);
    file.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    return true;
}

MediaItem wavMedia(const QString& path) {
    MediaItem item;
    item.name = "Tone.wav";
    item.locator.path = QDir::toNativeSeparators(path).toStdString();
    item.info.path = item.locator.path;
    item.info.duration = Ticks::fromSeconds(2);
    AudioStreamInfo a;
    a.index = 0;
    a.codec = "pcm_s16le";
    a.sampleRate = {kSampleRate, 1};
    a.channels = 2;
    a.duration = item.info.duration;
    item.info.audio.push_back(a);
    return item;
}

} // namespace

TEST_CASE("MediaPeakSource reads real peaks in the background and keeps placeholders for demo media",
          "[app][media]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString wav = dir.filePath(QStringLiteral("tone.wav"));
    REQUIRE(writeTestWav(wav));

    MediaPeakSource source(dir.filePath(QStringLiteral("cache")).toStdU16String());
    int ready = 0;
    QObject::connect(&source, &MediaPeakSource::peaksReady, [&] { ++ready; });
    source.setMediaFiles({{7, {QDir::toNativeSeparators(wav).toStdString(), {0}}}});

    std::vector<float> out(20, -1.0f);
    const StreamRef audio{StreamKind::Audio, 0};
    // Not decoded yet: no waveform, the job is queued.
    CHECK_FALSE(source.peaks(MediaId{7}, audio, Ticks{}, Ticks::fromSeconds(2), out));
    REQUIRE(waitFor([&] { return ready > 0; }, 20000));
    CHECK(source.idle());

    REQUIRE(source.peaks(MediaId{7}, audio, Ticks{}, Ticks::fromSeconds(2), out));
    for (std::size_t i = 0; i < 10; ++i) {
        INFO("column " << i << " = " << out[i]);
        CHECK(std::abs(out[i] - 0.5f) < 0.03f); // the sine's amplitude
    }
    for (std::size_t i = 11; i < 20; ++i) {
        INFO("column " << i << " = " << out[i]);
        CHECK(out[i] < 0.01f); // silence
    }
    CHECK(QDir(dir.filePath(QStringLiteral("cache/peaks"))).entryList(QDir::Files).size() >= 1); // disk cache

    // Media without a file (the demo project) keep the deterministic placeholder envelope, at once.
    std::vector<float> placeholder(20, -1.0f);
    CHECK(source.peaks(MediaId{8}, audio, Ticks{}, Ticks::fromSeconds(2), placeholder));
    CHECK(std::ranges::all_of(placeholder, [](float v) { return v >= 0.0f && v <= 1.0f; }));
}

TEST_CASE("Projects with media files feed the timeline and the library", "[app][media]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString wav = dir.filePath(QStringLiteral("tone.wav"));
    REQUIRE(writeTestWav(wav));

    // A sequence with the WAV on A1.
    ProjectState state;
    auto media = edit::addMedia(state, wavMedia(wav));
    REQUIRE(media);
    state = std::move(media->state);
    edit::SequenceSettings settings;
    settings.name = "Real Media";
    settings.frameRate = kRate24;
    settings.videoTracks = 1;
    settings.audioTracks = 1;
    auto seq = edit::addSequence(state, settings);
    REQUIRE(seq);
    state = std::move(seq->state);
    const TrackId a1 = state.findSequence(seq->id)->audioTracks.front()->id;
    const edit::SourceClip source{media->id, {Ticks{}, Ticks::fromSeconds(2)}, {{{StreamKind::Audio, 0}, a1}}};
    auto placed = edit::overwriteEdit(state, seq->id, source, Ticks{});
    REQUIRE(placed);

    ProjectController controller;
    controller.setMediaCacheDirectory(dir.filePath(QStringLiteral("cache")));
    CHECK_FALSE(controller.hasMediaFiles()); // the demo project has no files
    controller.resetProject(std::move(placed->state), seq->id);
    CHECK(controller.hasMediaFiles());
    REQUIRE(controller.mediaItems().size() == 1);
    const QVariantMap item = controller.mediaItems().front().toMap();
    CHECK(item.value(QStringLiteral("kind")).toString() == QStringLiteral("audio"));
    CHECK(item.value(QStringLiteral("name")).toString() == QStringLiteral("Tone.wav"));
    CHECK(item.value(QStringLiteral("id")).toString().startsWith(QLatin1Char('m')));
    CHECK(item.value(QStringLiteral("thumbnail")).toString().isEmpty()); // audio has no picture

    // The view asks for the peaks, gets nothing yet, and redraws once they arrive.
    int peaksChanged = 0;
    QObject::connect(&controller, &ProjectController::peaksChanged, [&] { ++peaksChanged; });
    TimelineView view;
    view.setSize(QSizeF(800, 200));
    view.setController(&controller);
    std::vector<float> out(8);
    CHECK_FALSE(controller.peakSource()->peaks(media->id, {StreamKind::Audio, 0}, Ticks{},
                                               Ticks::fromSeconds(1), out));
    REQUIRE(waitFor([&] { return peaksChanged > 0; }, 20000));
    CHECK(controller.peakSource()->peaks(media->id, {StreamKind::Audio, 0}, Ticks{}, Ticks::fromSeconds(1),
                                         out));
    CHECK(std::abs(out[3] - 0.5f) < 0.03f);
}

TEST_CASE("Library thumbnails come from ThumbnailService", "[app][media]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // A still image: left half red, right half blue.
    QImage picture(320, 180, QImage::Format_RGB32);
    picture.fill(Qt::red);
    for (int y = 0; y < picture.height(); ++y) {
        for (int x = picture.width() / 2; x < picture.width(); ++x)
            picture.setPixel(x, y, qRgb(0, 0, 255));
    }
    const QString still = dir.filePath(QStringLiteral("frame.bmp"));
    REQUIRE(picture.save(still));

    const QString url = ThumbnailImageProvider::urlFor(QDir::toNativeSeparators(still).toStdString());
    CHECK(url.startsWith(QStringLiteral("image://wabcut-thumbnails/")));

    ThumbnailImageProvider provider(dir.filePath(QStringLiteral("cache")).toStdU16String());
    const QString id = url.mid(QStringLiteral("image://wabcut-thumbnails/").size());
    std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(id, QSize(0, 90)));
    REQUIRE(response != nullptr);
    bool finished = false;
    QObject::connect(response.get(), &QQuickImageResponse::finished, [&] { finished = true; });
    REQUIRE(waitFor([&] { return finished; }, 20000));
    INFO(response->errorString().toStdString());
    REQUIRE(response->errorString().isEmpty());
    std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
    REQUIRE(factory != nullptr);
    const QImage image = factory->image();
    REQUIRE(image.height() == 90);
    CHECK(image.width() == 160);
    const QColor left = image.pixelColor(20, 45);
    const QColor right = image.pixelColor(140, 45);
    INFO("left " << describe(left) << ", right " << describe(right));
    CHECK(left.red() > 200);
    CHECK(left.blue() < 60);
    CHECK(right.blue() > 200);
    CHECK(right.red() < 60);
}
