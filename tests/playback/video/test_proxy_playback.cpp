// Preview quality and proxies through the Scheduler: a proxy plays frame for frame in place of its original
// (same frame number at every sequence frame, placed exactly where the original is), the preview switches between
// proxies, originals and reduced-resolution decoding at run time, proxy pictures match the box-downscaled original
// (PSNR, rendered by the compositor), and an offline original plays from its proxy.

#include "PlaybackTestSupport.h"

#include <wabcut/export/Proxy.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <thread>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::playback::test;
using namespace std::chrono_literals;

namespace {

constexpr const char* kClip = "h264_422_10bit.mp4"; // 1280x720 30 fps, 60 frames; barcode = frame number
constexpr Rate kRate30{30, 1};

/// The clip's proxy at 1/`divisor` (made once per build tree).
std::filesystem::path proxyOf(int divisor) {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "playback" / "proxies";
    const std::filesystem::path file = dir / std::format("h264_422_10bit.half{}.mov", divisor);
    if (!std::filesystem::exists(file)) {
        auto made = exporter::transcodeProxy(mediaPath(kClip), file, {exporter::ProxyCodec::ProResProxy, divisor, true});
        REQUIRE(made);
    }
    return file;
}

int luma8(const media::VideoFrame& f, int x, int y) {
    const media::CpuPlanes* cpu = f.cpu();
    if (!cpu || x < 0 || y < 0 || x >= f.width || y >= f.height) {
        return -1;
    }
    const media::Plane& p = cpu->planes[0];
    const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
    if (media::pixelFormatBitDepth(f.format) > 8) {
        std::uint16_t v = 0;
        std::memcpy(&v, row + 2 * x, 2);
        return f.format == media::PixelFormat::P010 ? v >> 8 : v >> 2;
    }
    return row[x];
}

/// The frame number of a frame of any size (cells of 32 source pixels).
std::int64_t barcodeAt(const media::VideoFrame& f, int sourceWidth) {
    const double s = static_cast<double>(f.width) / sourceWidth;
    std::int64_t index = 0;
    for (int bit = 0; bit < 16 && bit * 32 + 16 < sourceWidth; ++bit) {
        if (luma8(f, static_cast<int>((bit * 32 + 16) * s), static_cast<int>(16 * s)) > 128) {
            index |= std::int64_t{1} << bit;
        }
    }
    return index;
}

struct Fixture {
    TestProject project;
    EditSession session;
    PlaybackController controller;
    MediaId media;

    explicit Fixture(bool offline = false)
        : project(makeProject({{kClip, 0, 0, 60, 0}}, 1280, 720, kRate30)),
          session(withOffline(project.state, offline)) {
        media = project.state.media.front()->id;
        controller.setSession(&session);
        controller.setSequence(project.sequence);
    }

    static ProjectState withOffline(ProjectState state, bool offline) {
        if (offline) {
            auto item = std::make_shared<MediaItem>(*state.media.front());
            item->locator.offline = true;
            state.media.front() = std::move(item);
        }
        return state;
    }

    std::shared_ptr<const ProxyTable> table(int divisor = 2) const {
        auto t = std::make_shared<ProxyTable>();
        t->entries[media.value] = {pathToUtf8(proxyOf(divisor)), divisor};
        return t;
    }

    /// Seeks to sequence frame `frame` and returns the composite presented for it.
    std::shared_ptr<const render::PreparedComposite> show(std::int64_t frame) {
        const Ticks target = framesToTicks(frame, kRate30);
        controller.seek(target);
        const std::uint64_t epoch = controller.mailbox()->epoch();
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(16ms);
            const auto sel = controller.mailbox()->acquire(steadyNowNs() + 16'666'667);
            if (sel.changed && sel.composite && sel.composite->seekEpoch == epoch && sel.composite->targetTime == target) {
                return sel.composite;
            }
        }
        FAIL("no picture for frame " << frame);
        return nullptr;
    }

    /// Waits until the scheduler's stats describe the latest picture (they are sampled per publish).
    PlaybackStats statsAfter(std::int64_t frame) {
        show(frame);
        return controller.stats();
    }
};

double psnr(const render::RenderedImage& a, const render::RenderedImage& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double se = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const double d = static_cast<double>(a.rgba[i + c]) - b.rgba[i + c];
            se += d * d;
            ++n;
        }
    }
    const double mse = se / static_cast<double>(n);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

TEST_CASE("Preview: proxies play frame for frame at half resolution, placed like the original",
          "[playback][proxy]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    Fixture f;
    f.controller.scheduler().setPreview({.useProxies = true, .decodeDivisor = 1, .proxies = f.table()});
    for (const std::int64_t frame : {0, 13, 29, 30, 59}) {
        INFO("sequence frame " << frame);
        const auto c = f.show(frame);
        REQUIRE(c->layers.size() == 1);
        const render::CompositeLayer& layer = c->layers.front();
        CHECK(layer.frame.width == 640);
        CHECK(layer.frame.height == 360);
        CHECK(barcodeAt(layer.frame, 1280) == frame); // 30 fps sequence over a 30 fps clip
        CHECK(layer.transform.scale == Catch::Approx(2.0)); // Motion is in original pixels
    }
    PlaybackStats s = f.statsAfter(5);
    CHECK(s.proxyLayers == 1);
    CHECK(s.originalLayers == 0);
    CHECK(s.decodeDivisor == 2);

    SECTION("back to the originals at run time") {
        f.controller.scheduler().setPreview({.useProxies = false, .decodeDivisor = 1, .proxies = f.table()});
        const auto c = f.show(17);
        REQUIRE(c->layers.size() == 1);
        CHECK(c->layers.front().frame.width == 1280);
        CHECK(barcodeAt(c->layers.front().frame, 1280) == 17);
        CHECK(c->layers.front().transform.scale == Catch::Approx(1.0));
        s = f.statsAfter(18);
        CHECK(s.originalLayers == 1);
        CHECK(s.proxyLayers == 0);
        CHECK(s.decodeDivisor == 1);
    }
    SECTION("reduced-resolution decoding of the originals (1/2) and of proxies (1/4 from a 1/2 proxy)") {
        f.controller.scheduler().setPreview({.useProxies = false, .decodeDivisor = 2, .proxies = f.table()});
        auto c = f.show(21);
        REQUIRE(c->layers.size() == 1);
        CHECK(c->layers.front().frame.width == 640);
        CHECK(barcodeAt(c->layers.front().frame, 1280) == 21);
        CHECK(c->layers.front().transform.scale == Catch::Approx(2.0));
        s = f.statsAfter(22);
        CHECK(s.originalLayers == 1);
        CHECK(s.decodeDivisor == 2);

        f.controller.scheduler().setPreview({.useProxies = true, .decodeDivisor = 4, .proxies = f.table()});
        c = f.show(40);
        REQUIRE(c->layers.size() == 1);
        CHECK(c->layers.front().frame.width == 320);
        CHECK(c->layers.front().frame.height == 180);
        CHECK(barcodeAt(c->layers.front().frame, 1280) == 40);
        CHECK(c->layers.front().transform.scale == Catch::Approx(4.0));
        s = f.statsAfter(41);
        CHECK(s.proxyLayers == 1);
        CHECK(s.decodeDivisor == 4);
    }
}

TEST_CASE("Preview: proxy pictures match the downscaled original (rendered, PSNR)", "[playback][proxy][gpu]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    auto renderer = render::OffscreenRenderer::create({.role = render::OutputRole::Display});
    REQUIRE(renderer);
    Fixture f;
    const QSize half(640, 360);
    double worst = 99.0;
    double worstFull = 99.0;
    for (const std::int64_t frame : {0, 11, 33, 58}) {
        f.controller.scheduler().setPreview({.useProxies = true, .decodeDivisor = 1, .proxies = f.table()});
        const auto proxy = f.show(frame);
        f.controller.scheduler().setPreview({.useProxies = false, .decodeDivisor = 2, .proxies = f.table()});
        const auto downscaled = f.show(frame); // the original, box-filtered to 1/2 after decoding
        f.controller.scheduler().setPreview({.useProxies = false, .decodeDivisor = 1, .proxies = f.table()});
        const auto full = f.show(frame);       // the original at full resolution, rendered at 1/2
        auto a = (*renderer)->render(proxy, half);
        auto b = (*renderer)->render(downscaled, half);
        auto c = (*renderer)->render(full, half);
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(c);
        const double p = psnr(*a, *b);
        const double pf = psnr(*a, *c);
        UNSCOPED_INFO(std::format("frame {}: proxy vs downscaled original {:.2f} dB, vs full original {:.2f} dB", frame, p,
                                  pf));
        worst = std::min(worst, p);
        worstFull = std::min(worstFull, pf);
    }
    CHECK(worst >= 35.0);
    CHECK(worstFull >= 28.0);
    WARN(std::format("proxy vs box-downscaled original: worst {:.2f} dB; vs full-resolution original rendered at 1/2: "
                     "worst {:.2f} dB",
                     worst, worstFull));
}

TEST_CASE("Preview: an offline original plays from its proxy; without one it is left out", "[playback][proxy]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    Fixture f(true);
    SECTION("with a proxy (even with proxies off)") {
        f.controller.scheduler().setPreview({.useProxies = false, .decodeDivisor = 1, .proxies = f.table()});
        const auto c = f.show(9);
        REQUIRE(c->layers.size() == 1);
        CHECK(barcodeAt(c->layers.front().frame, 1280) == 9);
        const PlaybackStats s = f.statsAfter(10);
        CHECK(s.proxyLayers == 1);
        CHECK(s.offlineProxyLayers == 1);
    }
    SECTION("without") {
        f.controller.scheduler().setPreview({.useProxies = true, .decodeDivisor = 1, .proxies = nullptr});
        const auto c = f.show(9);
        CHECK(c->layers.empty());
    }
}
