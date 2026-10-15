// I.6 in the editor: the tracker's pictures from the media decoders (MediaTrackingService: luma with square pixels at
// a working width, the decoded frame's own time span), and track.mask end to end on a real encoded clip whose
// picture pans by a known amount per frame (made here with ffmpeg: a crop window sliding over testsrc2).

#include "SyntheticMotion.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/editor/Tracking.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>

#include <QImage>
#include <QProcess>
#include <QStringList>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::editor;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

/// 2 s at 30 fps, 640x360: a textured picture (tests/masks/SyntheticMotion.h) that moves 2 px left and 1 px up per
/// frame (a crop window sliding over a still).
bool makePan(const fs::path& dir) {
    const QString ffmpeg = QStringLiteral(WABCUT_FFMPEG_EXECUTABLE);
    if (ffmpeg.isEmpty() || !fs::exists(ffmpeg.toStdString())) {
        return false;
    }
    const masks::test::Texture texture(31);
    {
        std::ofstream still(dir / "still.gray", std::ios::binary | std::ios::trunc); // raw 8-bit luma (no PNG decoder needed)
        for (int y = 0; y < 720; ++y) {
            for (int x = 0; x < 1280; ++x) {
                still.put(static_cast<char>(static_cast<unsigned char>(std::clamp(texture.at(x, y), 0.0, 1.0) * 255.0)));
            }
        }
    }
    const QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
                           QStringLiteral("-f"), QStringLiteral("rawvideo"), QStringLiteral("-pix_fmt"), QStringLiteral("gray"),
                           QStringLiteral("-s"), QStringLiteral("1280x720"), QStringLiteral("-framerate"), QStringLiteral("30"),
                           QStringLiteral("-stream_loop"), QStringLiteral("-1"), QStringLiteral("-t"), QStringLiteral("2"),
                           QStringLiteral("-i"), QString::fromStdString(pathToUtf8(dir / "still.gray")),
                           QStringLiteral("-vf"), QStringLiteral("crop=640:360:x='100+n*2':y='80+n*1',format=yuv420p"),
                           QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("veryfast"),
                           QStringLiteral("-crf"), QStringLiteral("10"), QString::fromStdString(pathToUtf8(dir / "pan.mp4"))};
    return QProcess::execute(ffmpeg, args) == 0 && fs::exists(dir / "pan.mp4");
}
} // namespace

TEST_CASE("Tracking in the editor: decoded luma pictures, and track.mask on a real panning clip", "[editor][masks][tracking]") {
    const fs::path dir = workDir("tracking");
    if (!makePan(dir)) {
        SKIP("ffmpeg is not available to make the panning clip");
    }
    EditSession session;
    ProjectDocument doc(session);
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    const SequenceId sequence = created->id;
    session.reset(std::move(created->state));
    REQUIRE(doc.saveAs(dir / "project.wabcut"));
    const std::vector<fs::path> files{dir / "pan.mp4"};
    const MediaImportResult imported = importMediaFiles(session, files, {.projectFile = dir / "project.wabcut"});
    REQUIRE(imported.added.size() == 1);
    const MediaId media = imported.added.front();
    auto placed = dropMedia(session, {.media = media, .sequence = sequence, .at = Ticks{}});
    REQUIRE(placed);
    ClipId clip;
    for (const ClipId id : placed->placed) {
        const auto loc = session.snapshot()->findSequence(sequence)->findClip(id);
        if (loc && loc->clip().stream.kind == StreamKind::Video) {
            clip = id;
        }
    }
    REQUIRE(clip.isValid());

    auto service = std::make_shared<MediaTrackingService>(dir / "project.wabcut");
    const Ticks frame = Ticks{kTicksPerSecond / 30};
    {
        auto picture = service->frame(*session.snapshot(), media, 0, Ticks{frame.value * 10 + 5}, 320);
        REQUIRE(picture);
        CHECK(picture->luma.width == 320);
        CHECK(picture->luma.height == 180);
        CHECK(picture->start == Ticks{frame.value * 10});
        CHECK(picture->duration == frame);
        float lo = 1.0f, hi = 0.0f;
        for (const float v : picture->luma.px) {
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
        CHECK(hi - lo > 0.3f); // a real picture (the texture spans about 0.2 .. 0.8)
        CHECK(service->frame(*session.snapshot(), media, 0, Ticks::fromSeconds(5), 320).error().code == ErrorCode::NotFound);
    }

    int reports = 0;
    double lowest = 1.0;
    service->setProgressCallback([&](const masks::TrackProgress& p) {
        ++reports;
        lowest = std::min(lowest, p.confidence);
    });
    command::EnginePolicy policy;
    policy.services = service;
    command::CommandEngine engine(session, ai::edit::editingCommands(), policy);
    const auto run = [&](command::Json document) {
        const auto result = engine.executeJson(document);
        INFO(command::toJson(result).dump());
        REQUIRE(result.ok());
        return result;
    };
    run({{"command", "mask.add"},
         {"params", {{"clip", clip.value}, {"shape", "rectangle"}, {"params", {{"size.x", 0.5}, {"size.y", 0.5}}}}}});
    const auto tracked = run({{"command", "track.mask"},
                              {"params", {{"clip", clip.value}, {"mask", 1}, {"frames", 20}, {"workingWidth", 640}}}});
    CHECK(tracked.data["frames"] == 20);
    CHECK(reports == 20);
    WARN("lowest confidence over 20 frames " << lowest);
    CHECK(lowest > 0.5);
    const Clip& after = session.snapshot()->findSequence(sequence)->findClip(clip)->clip();
    const EffectInstance* holder = nullptr;
    for (const EffectInstance& e : after.effects()) {
        holder = e.masks.empty() ? holder : &e;
    }
    REQUIRE(holder);
    const Mask& mask = holder->masks.front();
    const Ticks t20{frame.value * 20};
    WARN("transform at frame 20: x " << maskValue(mask, "transform.x", t20) << " (expected " << -40.0 / 640 << "), y "
                                     << maskValue(mask, "transform.y", t20) << " (expected " << -20.0 / 360 << ")");
    CHECK(maskValue(mask, "transform.x", t20) == Approx(-40.0 / 640).margin(0.003));
    CHECK(maskValue(mask, "transform.y", t20) == Approx(-20.0 / 360).margin(0.003));
    CHECK(maskValue(mask, "transform.scaleX", t20) == Approx(1.0).margin(0.01));
}
