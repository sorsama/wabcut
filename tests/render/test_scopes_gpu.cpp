// GPU scopes (shaders/scopes.comp) against the CPU fallback (downsampled readback + wabcut::color::computeScopes) and
// against the CPU binning of the rendered output: the same counts within float rounding at bin edges; colour bars on
// their vectorscope targets; the step grid at 1080p; the cost of a scope pass.

#include "RenderTestSupport.h"

#include <wabcut/color/Scopes.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/GpuScopes.h>
#include <wabcut/render/StillImage.h>

#include <QElapsedTimer>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <numeric>
#include <thread>
#include <rhi/qrhi.h>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

std::uint8_t byte(double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

/// A BT.709-tagged still of `at`: the monitor shows exactly these code values (decode and encode cancel).
CompositeLayer stillLayer(int w, int h, const std::function<std::array<std::uint8_t, 4>(int, int)>& at) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto p = at(x, y);
            std::copy(p.begin(), p.end(), rgba.begin() + (static_cast<std::ptrdiff_t>(y) * w + x) * 4);
        }
    }
    auto still = std::make_shared<const StillImage>(w, h, std::move(rgba));
    CompositeLayer layer;
    layer.view = still->view(still);
    return layer;
}

/// Scopes of what `renderer` just rendered (its compositor's canvas), on the GPU or through the CPU fallback.
color::ScopeData scopesOf(OffscreenRenderer& renderer, CanvasSize canvas, bool forceCpu, const color::ScopeSettings& settings = {}) {
    GpuScopes scopes(renderer.rhi());
    scopes.setForceCpu(forceCpu);
    QRhiCommandBuffer* cb = nullptr;
    REQUIRE(renderer.rhi()->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess);
    color::ScopeData out;
    bool done = false;
    const auto recorded = scopes.record(cb, renderer.compositor().canvasTexture(), QSize(canvas.width, canvas.height),
                                        OutputTransform::Rec709, settings, [&](color::ScopeData d) {
                                            out = std::move(d);
                                            done = true;
                                        });
    REQUIRE(recorded);
    REQUIRE(renderer.rhi()->endOffscreenFrame() == QRhi::FrameOpSuccess);
    if (!done) {
        renderer.rhi()->finish();
    }
    REQUIRE(done);
    return out;
}

/// L1 distance between two count vectors, as a fraction of the total.
double distance(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    double diff = 0.0, total = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        diff += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        total += a[i];
    }
    return total > 0 ? diff / total : 0.0;
}

std::array<std::uint8_t, 4> scene(int x, int y) {
    const double u = x / 639.0, v = y / 359.0;
    if (y < 120) { // 75 % bars
        const int bar = x * 8 / 640;
        const auto c = barColor(bar);
        return {byte(c[0]), byte(c[1]), byte(c[2]), 255};
    }
    if (y < 240) { // ramps
        return {byte(u), byte(u * u), byte(1 - u), 255};
    }
    return {byte(0.5 + 0.4 * std::sin(u * 12)), byte(0.5 + 0.4 * std::cos(v * 9)), byte(u * v), 255};
}

} // namespace

TEST_CASE("GPU scopes match the CPU fallback and the CPU binning of the output", "[render][gpu][scopes]") {
    auto renderer = makeRenderer(OutputRole::Display);
    const CanvasSize canvas{640, 360};
    auto composite = compositeOf(canvas, {stillLayer(640, 360, scene)});
    auto image = renderer->render(composite);
    REQUIRE(image);
    REQUIRE(renderer->rhi()->isFeatureSupported(QRhi::Compute));
    const color::ScopeData gpu = scopesOf(*renderer, canvas, false);
    const color::ScopeData cpu = scopesOf(*renderer, canvas, true);
    const color::ScopeData output = color::computeScopes8(image->rgba.data(), image->width, image->height, image->width * 4);
    CHECK(gpu.pixels == 640u * 360u);
    CHECK(cpu.pixels == gpu.pixels);
    CHECK(output.pixels == gpu.pixels);
    // The 8-bit paths quantise first, so values move to a neighbouring bin now and then; the shapes are the same.
    for (std::size_t c = 0; c < 4; ++c) {
        INFO("histogram " << c);
        CHECK(distance(gpu.histogram[c], cpu.histogram[c]) < 0.03);
        CHECK(distance(gpu.histogram[c], output.histogram[c]) < 0.03);
        CHECK(cpu.histogram[c] == output.histogram[c]); // the fallback IS the output's binning
    }
    CHECK(distance(gpu.waveLuma, output.waveLuma) < 0.05);
    for (std::size_t c = 0; c < 3; ++c) {
        CHECK(distance(gpu.waveRgb[c], output.waveRgb[c]) < 0.05);
    }
    CHECK(distance(gpu.vector, output.vector) < 0.1);
    // The statistics agree closely.
    const color::ScopeStats a = color::scopeStats(gpu);
    const color::ScopeStats b = color::scopeStats(output);
    CHECK(std::abs(a.lumaMean - b.lumaMean) < 0.3);
    CHECK(std::abs(a.p50 - b.p50) < 0.6);
    CHECK(a.exposure == b.exposure);
    // 75 % bars sit on their vectorscope targets (the still's code values are what the scopes see).
    for (int bar = 1; bar < 7; ++bar) {
        const Rgb bar75 = barColor(bar);
        const auto target = color::vectorscopeTarget({bar75[0] / 0.75, bar75[1] / 0.75, bar75[2] / 0.75}, 0.75);
        const int vx = std::clamp(static_cast<int>(std::floor(256 * (0.5 + target[0]) + 0.5)), 0, 255);
        const int vy = std::clamp(static_cast<int>(std::floor(256 * (0.5 - target[1]) + 0.5)), 0, 255);
        std::uint32_t around = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int x = std::clamp(vx + dx, 0, 255), y = std::clamp(vy + dy, 0, 255);
                around += gpu.vector[static_cast<std::size_t>(y) * 256 + static_cast<std::size_t>(x)];
            }
        }
        INFO("bar " << bar);
        CHECK(around >= 80u * 120u * 9u / 10u); // nearly all of the bar's pixels
    }
}

TEST_CASE("GPU scopes at 1080p: the step grid and the cost of a pass", "[render][gpu][scopes][perf]") {
    auto renderer = makeRenderer(OutputRole::Display);
    const CanvasSize canvas{1920, 1080};
    const auto picture = [](int x, int y) { return std::array<std::uint8_t, 4>{byte(x / 1919.0), byte(y / 1079.0), 128, 255}; };
    auto composite = compositeOf(canvas, {stillLayer(1920, 1080, picture)});
    REQUIRE(renderer->render(composite));
    const color::ScopeData d = scopesOf(*renderer, canvas, false);
    CHECK(d.step == 2);
    CHECK(d.pixels == 960u * 540u);
    // Wall-clock cost of recording + running + reading back one scope pass (offscreen frames wait for the GPU), for a
    // gradient (every bin a little) and a flat picture (every pixel in the same bins: the worst case for the atomics).
    const auto timePasses = [&](int pacingMs = 0) {
        std::vector<double> ms;
        for (int i = 0; i < 20; ++i) {
            if (pacingMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(pacingMs));
            }
            QElapsedTimer t;
            t.start();
            (void)scopesOf(*renderer, canvas, false);
            ms.push_back(t.nsecsElapsed() / 1e6);
        }
        std::sort(ms.begin(), ms.end());
        return ms;
    };
    const std::vector<double> gradient = timePasses();
    const auto flat = [](int, int) { return std::array<std::uint8_t, 4>{180, 120, 90, 255}; };
    REQUIRE(renderer->render(compositeOf(canvas, {stillLayer(1920, 1080, flat)})));
    const color::ScopeData flatData = scopesOf(*renderer, canvas, false);
    CHECK(flatData.pixels == 960u * 540u);
    CHECK(*std::ranges::max_element(flatData.histogram[3]) == 960u * 540u); // one luma bin holds every pixel
    const std::vector<double> solid = timePasses();
    const std::vector<double> paced = timePasses(33); // the monitor's ~30 Hz: the GPU idles between passes
    const auto row = [](const char* what, const std::vector<double>& ms) {
        return std::format("| {} | {:.2f} ms | {:.2f} ms |", what, ms[ms.size() / 2], ms.back());
    };
    WARN(row("gradient", gradient) << "\n" << row("flat", solid) << "\n" << row("flat, 33 ms apart", paced));
    std::ofstream report((outputDir() + QStringLiteral("/scopes-perf.md")).toStdString(), std::ios::trunc);
    report << "# GPU scopes, 1920x1080 (" << renderer->backendName() << ", " << renderer->adapterName() << ")\n\n"
           << "One scope pass (clear, accumulate on the 2-px grid, read back 1.8 MB), offscreen frame wall clock, 20 runs.\n\n"
           << "| Picture | Median | Max |\n|---|---|---|\n"
           << row("gradient (every bin a little)", gradient) << "\n"
           << row("flat colour (every pixel in the same bins)", solid) << "\n"
           << row("flat colour, one pass every 33 ms (the monitor's rate)", paced) << "\n";
    CHECK(gradient[gradient.size() / 2] < 8.0);
    CHECK(solid[solid.size() / 2] < 8.0);
}
