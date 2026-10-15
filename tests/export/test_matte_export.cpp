// AI-14 matte redactions in export (render/Matte.h): a redaction whose region is an AI object matte covers the matte's
// shape where the matte is known and fails closed where it is not (the bounding box, grown), in the export exactly as in
// the preview (one code path: buildCompositeLayers; the scopes read the same RenderedVideoSource). The matte source here
// is a test double: a disc for media times before 1.4 s, nothing after.

#include "ExportTestUtil.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/render/Matte.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <format>
#include <mutex>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;

namespace {

constexpr Rate kRate{30, 1};
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

/// A disc (centre (0.5, 0.5), radius 0.08 frame heights) before 1.4 s of media time; no matte after.
class DiscBefore final : public render::MatteSource {
public:
    std::shared_ptr<const render::MatteField> fieldAt(const MaskMatte&, Ticks mediaTime, double aspect, render::MatteUse) override {
        if (mediaTime >= ms(1400)) {
            return nullptr;
        }
        std::lock_guard lock(mutex);
        if (!field) {
            auto f = std::make_shared<render::MatteField>();
            f->width = 160;
            f->height = 90;
            f->identity = 41;
            f->distance.resize(160 * 90);
            for (int y = 0; y < 90; ++y) {
                for (int x = 0; x < 160; ++x) {
                    const double px = (x + 0.5) / 160 * aspect, py = (y + 0.5) / 90;
                    f->distance[static_cast<std::size_t>(y) * 160 + x] = static_cast<float>(0.08 - std::hypot(px - 0.5 * aspect, py - 0.5));
                }
            }
            field = f;
        }
        return field;
    }
    std::mutex mutex;
    std::shared_ptr<const render::MatteField> field;
};

struct Installed {
    explicit Installed(std::shared_ptr<render::MatteSource> s) { render::setMatteSource(std::move(s)); }
    ~Installed() { render::setMatteSource(nullptr); }
};

} // namespace

TEST_CASE("Rendered export: a matte redaction covers the matte, and fails closed to its grown box where the matte is missing (AI-14)",
          "[export][render][media][gpu][redaction][matte]") {
    if (!haveMotionTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    Installed installed(std::make_shared<DiscBefore>());
    TestProject p(kWidth, kHeight, kRate, Ticks::fromSeconds(1));
    const MediaId h264 = p.addMedia("h264_1080p60_aac.mp4");
    Clip a = p.clip(h264, StreamKind::Video, Ticks{}, ms(1000), ms(1000)); // media 1.0 .. 2.0 s
    ClipProperties fit;
    fit.set(ParamId::Scale, static_cast<double>(kWidth) / 1920.0);
    ClipContent content = a.content();
    content.properties = fit;
    a.setContent(std::move(content));
    p.addTrack(TrackKind::Video, {a});
    // Redacted in media time 1.0 .. 2.0 s with a magenta fill: its region is the matte, bounding box 0.2 high.
    Redaction r;
    r.media = h264;
    r.source = TimeRange{ms(1000), ms(1000)};
    r.style = RedactionStyle::Solid;
    r.colorR = 1.0;
    r.colorB = 1.0;
    r.region.shape = MaskShape::Matte;
    r.region.matte = MaskMatte{"disc", h264, 0, TimeRange{ms(1000), ms(400)}, "test"};
    r.region.set("center.x", 0.5);
    r.region.set("center.y", 0.5);
    r.region.set("size.x", 0.16 * 9.0 / 16.0);
    r.region.set("size.y", 0.16);
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);

    const auto dir = freshOutputDir("matte-redaction");
    ExportSettings settings = findPreset("prores-422-hq")->settings;
    const auto out = dir / "matte-redacted.mov";
    ExportRequest request = makeRequest(p, settings, out);
    request.videoSource = {}; // the rendered source
    const auto result = ExportJob(request).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->frames == 30);

    // The disc's centre is covered in every frame; the box's corner (outside the disc) only where the matte is missing:
    // frames 12.. (media 1.4 s on), where the box grown by a quarter stands in.
    const auto magentaAt = [](const YuvFrame& f, double u, double v) {
        const int x = static_cast<int>(u * f.width), y = static_cast<int>(v * f.height);
        const int shift = f.bits - 8;
        const int cx = x * f.chromaWidth / f.width;
        const int cy = y * f.chromaHeight / f.height;
        const int Y = f.y(x, y) >> shift;
        const int U = f.planes[1][static_cast<std::size_t>(cy) * f.chromaWidth + cx] >> shift;
        const int V = f.planes[2][static_cast<std::size_t>(cy) * f.chromaWidth + cx] >> shift;
        return std::abs(Y - 78) <= 8 && std::abs(U - 214) <= 10 && std::abs(V - 229) <= 10;
    };
    const double cornerU = 0.5 + 0.075 * 9.0 / 16.0, cornerV = 0.5 + 0.075; // inside the box, outside the disc
    int frame = 0, wrong = 0;
    std::string error;
    REQUIRE(decodeVideo(
        out,
        [&](const YuvFrame& f) {
            const bool missing = frame >= 12;
            if (!magentaAt(f, 0.5, 0.5) || magentaAt(f, cornerU, cornerV) != missing) {
                ++wrong;
                UNSCOPED_INFO("frame " << frame << (missing ? ": the box should cover" : ": only the disc should cover"));
            }
            ++frame;
        },
        &error));
    CHECK(frame == 30);
    CHECK(wrong == 0);

    // Export == preview.
    exporttest::PreviewRenderer preview(p);
    int compared = 0;
    double minPsnr = 1e9;
    REQUIRE(decodeVideo(
        out,
        [&](const YuvFrame& f) {
            const auto rgba = preview.render(framesToTicks(compared, kRate), framesToTicks(1, kRate));
            const double v = psnr(f, yuvFromRgba(rgba, f.width, f.height, f));
            minPsnr = std::min(minPsnr, std::isinf(v) ? 100.0 : v);
            ++compared;
        },
        &error));
    metric(std::format("matte redaction: preview==export PSNR min {:.2f} dB over {} frames", minPsnr, compared));
    CHECK(minPsnr >= 45.0);
}
