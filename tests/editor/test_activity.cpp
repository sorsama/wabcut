// C.16 media activity for dead-time removal (Activity.h): motion from luma-grid frame differences and loudness from
// waveform peaks, five samples a second from the media's origin; audio-only and picture-only media; generated, still
// and offline media refused; the motion term on clips made here with ffmpeg (a still picture: ~0, a moving
// checkerboard: clearly more); and the command layer's measure (activityMeasure): an in-memory cache per media file, the
// analysis lookup first, and cancelling.

#include <wabcut/editor/Activity.h>
#include <wabcut/editor/MediaImport.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/GamingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Synthetic.h>

#include <QImage>
#include <QProcess>
#include <QStringList>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path testMedia(const char* name) { return fs::path(WABCUT_TEST_MEDIA_DIR) / name; }

bool haveMedia() {
    return fs::exists(testMedia("h264_1080p60_aac.mp4")) && fs::exists(testMedia("sine_48k.wav")) &&
           fs::exists(testMedia("prores_422.mov"));
}

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

MediaId importOne(EditSession& session, const fs::path& file) {
    const std::vector<fs::path> files{file};
    const MediaImportResult r = importMediaFiles(session, files);
    REQUIRE(r.failed.empty());
    REQUIRE(r.added.size() == 1);
    return r.added.front();
}

const MediaItem& itemOf(const EditSession& session, MediaId media) {
    const MediaItem* item = session.snapshot()->findMedia(media);
    REQUIRE(item);
    return *item;
}

bool inUnitRange(const std::vector<float>& values) {
    return std::ranges::all_of(values, [](float v) { return v >= 0.0F && v <= 1.0F; });
}

double mean(const std::vector<float>& values) {
    return values.empty() ? 0.0 : std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

constexpr Ticks kStep{kTicksPerSecond / 5};

/// ffmpeg encodes `file` (H.264, no sound) from 60 raw 320x180 luma frames, 2 s at 30 fps: a checkerboard of 32 px
/// cells (50 and 200) moving `pixelsPerFrame` to the left per frame (0: a still picture). The build's ffmpeg has no
/// lavfi input, so the frames are written here.
bool makeClip(const fs::path& file, int pixelsPerFrame) {
    const QString ffmpeg = QStringLiteral(WABCUT_FFMPEG_EXECUTABLE);
    if (ffmpeg.isEmpty() || !fs::exists(fs::path(ffmpeg.toStdWString()))) {
        return false;
    }
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    fs::path raw = file;
    raw.replace_extension(".gray");
    {
        std::ofstream out(raw, std::ios::binary | std::ios::trunc);
        std::vector<char> frame(static_cast<std::size_t>(kWidth) * kHeight);
        for (int n = 0; n < 60; ++n) {
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    const bool light = ((x + n * pixelsPerFrame) / 32 + y / 32) % 2 == 0;
                    frame[static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)] =
                        static_cast<char>(light ? 200 : 50);
                }
            }
            out.write(frame.data(), static_cast<std::streamsize>(frame.size()));
        }
    }
    const QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                           QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("rawvideo"),
                           QStringLiteral("-pix_fmt"), QStringLiteral("gray"), QStringLiteral("-s"),
                           QStringLiteral("320x180"), QStringLiteral("-framerate"), QStringLiteral("30"),
                           QStringLiteral("-i"), QString::fromStdWString(raw.wstring()), QStringLiteral("-pix_fmt"),
                           QStringLiteral("yuv420p"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
                           QStringLiteral("-preset"), QStringLiteral("veryfast"), QStringLiteral("-crf"),
                           QStringLiteral("18"), QString::fromStdWString(file.wstring())};
    return QProcess::execute(ffmpeg, args) == 0 && fs::exists(file);
}

} // namespace

TEST_CASE("Activity: a video with sound gives motion and loudness five times a second", "[editor][activity]") {
    if (!haveMedia()) {
        SKIP("test media not generated");
    }
    EditSession session;
    const MediaId media = importOne(session, testMedia("h264_1080p60_aac.mp4")); // 10 s, 1080p60, AAC stereo
    std::vector<double> fractions;
    const auto start = std::chrono::steady_clock::now();
    const auto measured = measureMediaActivity(itemOf(session, media), {}, [&](double f) {
        fractions.push_back(f);
        return true;
    });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO((measured ? std::string() : measured.error().message));
    REQUIRE(measured);
    const MediaActivity& a = *measured;
    WARN("10 s of 1080p60: " << a.motion.size() << " motion and " << a.audio.size() << " loudness samples in " << seconds
                             << " s; mean motion " << mean(a.motion) << ", mean loudness " << mean(a.audio));
    CHECK(a.step == kStep);
    CHECK(a.source == "original");
    CHECK(a.motion.size() >= 49);
    CHECK(a.motion.size() <= 51);
    CHECK(a.audio.size() >= 49);
    CHECK(a.audio.size() <= 51);
    CHECK(inUnitRange(a.motion));
    CHECK(inUnitRange(a.audio));
    CHECK_FALSE(mediaActivityProblem(a).has_value());
    CHECK(mean(a.motion) > 0.0); // a moving test picture
    CHECK(mean(a.audio) > 0.3);  // a tone, not silence
    REQUIRE_FALSE(fractions.empty());
    CHECK(std::ranges::is_sorted(fractions));
    CHECK(fractions.front() >= 0.0);
    CHECK(fractions.back() == 1.0);

    // Ten samples a second on a coarser grid: twice as many samples.
    const auto denser = measureMediaActivity(itemOf(session, media), {}, {}, {.samplesPerSecond = 10, .gridWidth = 16});
    REQUIRE(denser);
    CHECK(denser->step == Ticks{kTicksPerSecond / 10});
    CHECK(denser->motion.size() >= 98);
    CHECK(denser->motion.size() <= 101);
    CHECK(denser->audio.size() >= 98);
    CHECK(denser->audio.size() <= 101);
    // Options out of range are refused.
    for (const ActivityOptions bad : {ActivityOptions{.samplesPerSecond = 0}, ActivityOptions{.samplesPerSecond = 31},
                                      ActivityOptions{.gridWidth = 4}, ActivityOptions{.gridWidth = 512}}) {
        const auto refused = measureMediaActivity(itemOf(session, media), {}, {}, bad);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("Activity: a steady sine is loud and level and has no motion", "[editor][activity]") {
    if (!haveMedia()) {
        SKIP("test media not generated");
    }
    EditSession session;
    const MediaId media = importOne(session, testMedia("sine_48k.wav")); // 5 s, L = 0.5 sin (-6 dBFS), R = 0.25 sin
    const auto measured = measureMediaActivity(itemOf(session, media), {});
    INFO((measured ? std::string() : measured.error().message));
    REQUIRE(measured);
    CHECK(measured->step == kStep);
    CHECK(measured->motion.empty());
    REQUIRE(measured->audio.size() == 25);
    // The louder channel peaks at -6 dBFS: (60 - 6) / 60 = 0.9 on the -60..0 dBFS scale, in every bin.
    const auto [lo, hi] = std::ranges::minmax(measured->audio);
    INFO("loudness " << lo << " .. " << hi);
    CHECK(lo > 0.88F);
    CHECK(hi < 0.92F);
    CHECK(hi - lo < 0.01F);
}

TEST_CASE("Activity: generated, still and offline media are refused", "[editor][activity]") {
    if (!haveMedia()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("activity_refused");
    // A colour matte has no file to measure.
    const auto matte = syntheticMediaItem(SyntheticKind::ColorMatte, {}, SyntheticFormat{});
    REQUIRE(matte);
    const auto generated = measureMediaActivity(*matte, {});
    REQUIRE_FALSE(generated);
    CHECK(generated.error().code == ErrorCode::InvalidArgument);

    // A still picture has neither motion nor sound.
    QImage picture(320, 180, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    REQUIRE(picture.save(QString::fromStdWString((dir / "still.png").wstring())));
    EditSession session;
    const MediaId still = importOne(session, dir / "still.png");
    REQUIRE(itemOf(session, still).info.isStillImage);
    const auto pictureOnly = measureMediaActivity(itemOf(session, still), {});
    REQUIRE_FALSE(pictureOnly);
    CHECK(pictureOnly.error().code == ErrorCode::Unsupported);

    // A file that is gone (no proxy either) is offline.
    fs::copy_file(testMedia("sine_48k.wav"), dir / "gone.wav");
    const MediaId gone = importOne(session, dir / "gone.wav");
    fs::remove(dir / "gone.wav");
    const auto offline = measureMediaActivity(itemOf(session, gone), {});
    REQUIRE_FALSE(offline);
    CHECK(offline.error().code == ErrorCode::NotFound);
}

TEST_CASE("Activity: motion is near zero on a still picture and clearly above it on a moving one", "[editor][activity]") {
    const fs::path dir = workDir("activity_motion");
    if (!makeClip(dir / "still.mp4", 0) || !makeClip(dir / "moving.mp4", 2)) {
        SKIP("ffmpeg is not available to make the clips");
    }
    EditSession session;
    const MediaId stillMedia = importOne(session, dir / "still.mp4");
    const MediaId movingMedia = importOne(session, dir / "moving.mp4");
    const auto still = measureMediaActivity(itemOf(session, stillMedia), {});
    const auto moving = measureMediaActivity(itemOf(session, movingMedia), {});
    REQUIRE(still);
    REQUIRE(moving);
    CHECK(still->audio.empty());
    CHECK(moving->audio.empty());
    CHECK(still->motion.size() == 10);
    CHECK(moving->motion.size() == 10);
    const float stillMax = *std::ranges::max_element(still->motion);
    const float movingMin = *std::ranges::min_element(moving->motion);
    WARN("motion: still picture max " << stillMax << "; moving checkerboard min " << movingMin << ", mean "
                                     << mean(moving->motion));
    CHECK(stillMax < 0.01F);
    CHECK(movingMin > 0.05F);
    CHECK(mean(moving->motion) > 0.1);
}

TEST_CASE("Activity: the command measure keeps results per media file, asks the analysis first and can be stopped",
          "[editor][activity]") {
    if (!haveMedia()) {
        SKIP("test media not generated");
    }
    EditSession session;
    const MediaId prores = importOne(session, testMedia("prores_422.mov")); // 2 s of 720p ProRes + PCM
    const MediaId sine = importOne(session, testMedia("sine_48k.wav"));
    const ProjectSnapshot state = session.snapshot();

    SECTION("a second measure of the same media is the cached result, nothing decoded") {
        const command::ActivityServices::Measure measure = activityMeasure({});
        int calls = 0;
        const auto counting = [&](double) {
            ++calls;
            return true;
        };
        auto t0 = std::chrono::steady_clock::now();
        const auto first = measure(*state, prores, counting);
        const double firstSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(first);
        const int firstCalls = calls;
        CHECK(firstCalls > 0);
        CHECK_FALSE(first->motion.empty());
        CHECK_FALSE(first->audio.empty());
        t0 = std::chrono::steady_clock::now();
        const auto second = measure(*state, prores, counting);
        const double secondSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(second);
        WARN("first measure " << firstSeconds << " s (" << firstCalls << " progress calls), cached " << secondSeconds
                              << " s");
        CHECK(calls == firstCalls); // no progress: nothing decoded
        CHECK(*second == *first);
        // Another media item is measured on its own.
        const auto other = measure(*state, sine, counting);
        REQUIRE(other);
        CHECK(calls > firstCalls);
        CHECK(other->motion.empty());
        // An unknown media id.
        const auto unknown = measure(*state, MediaId{987654}, counting);
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error().code == ErrorCode::NotFound);
    }
    SECTION("the analysis cache answers first; media it does not know is measured") {
        MediaActivity curves;
        curves.step = Ticks{kTicksPerSecond / 10};
        curves.audio = {0.1F, 0.9F, 0.2F};
        curves.motion = {0.5F, 0.5F, 0.0F};
        curves.source = "analysis";
        int asked = 0;
        const command::ActivityServices::Measure measure =
            activityMeasure({}, [&](const MediaItem& item) -> std::optional<MediaActivity> {
                ++asked;
                return item.id == prores ? std::optional(curves) : std::nullopt;
            });
        int calls = 0;
        const auto counting = [&](double) {
            ++calls;
            return true;
        };
        const auto analysed = measure(*state, prores, counting);
        REQUIRE(analysed);
        CHECK(*analysed == curves);
        CHECK(calls == 0);
        CHECK(asked == 1);
        const auto measured = measure(*state, sine, counting);
        REQUIRE(measured);
        CHECK(asked == 2);
        CHECK(measured->source == "original");
        CHECK(calls > 0);
    }
    SECTION("progress returning false stops it as Cancelled and nothing is kept") {
        const command::ActivityServices::Measure measure = activityMeasure({});
        const auto stop = [](double) { return false; };
        const auto stopped = measure(*state, prores, stop);
        REQUIRE_FALSE(stopped);
        CHECK(stopped.error().code == ErrorCode::Cancelled);
        int calls = 0;
        const auto again = measure(*state, prores, [&](double) {
            ++calls;
            return true;
        });
        REQUIRE(again);
        CHECK(calls > 0); // measured now: the stopped run left nothing in the cache
        // Stopped in the audio pass of a sound-only file.
        const auto audioOnly = measureMediaActivity(itemOf(session, sine), {}, stop);
        REQUIRE_FALSE(audioOnly);
        CHECK(audioOnly.error().code == ErrorCode::Cancelled);
        // Stopped half way through the picture pass.
        const auto halfWay = measureMediaActivity(itemOf(session, prores), {}, [](double f) { return f < 0.4; });
        REQUIRE_FALSE(halfWay);
        CHECK(halfWay.error().code == ErrorCode::Cancelled);
    }
}
