// AI video effects (0.7) out of process: the process_picture protocol (round trips and bounds), HelperBackend against the
// scripted wabcut-embed (fake_embed: each model marks the values, an upscaler enlarges by nearest neighbour), a GPU crash
// while loading (the CPU takes over), a helper that keeps crashing, a model that is not downloaded (no helper started), a
// hung helper cancelled, and release; and the opt-in test of the real models (WABCUT_TEST_VIDEO_MODELS_DIR).

#include "VideoTestUtil.h"

#include <wabcut/ai/search/EmbedProtocol.h>
#include <wabcut/ai/video/HelperBackend.h>
#include <wabcut/ai/video/Runner.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;
namespace embed = wabcut::ai::search::embed;

namespace {

HelperBackendOptions fakeOptions(const std::string& mode, const std::string& dirName, std::vector<std::string> extra = {}) {
    HelperBackendOptions o;
    o.executable = WABCUT_FAKE_EMBED;
    o.arguments = {"--mode", mode};
    o.arguments.insert(o.arguments.end(), extra.begin(), extra.end());
    o.workDir = freshDir(dirName);
    o.resolveModel = [](Model) { return std::optional<std::filesystem::path>{"fake-model.onnx"}; };
    o.startTimeout = std::chrono::seconds(20);
    o.loadTimeout = std::chrono::seconds(20);
    o.requestBase = std::chrono::seconds(20);
    return o;
}

Step stepOf(Model model, int tile = 64) {
    Step s;
    s.model = model;
    s.tile = tile;
    return s;
}

} // namespace

TEST_CASE("The process_picture protocol: round trips and bounds", "[ai][video][protocol]") {
    embed::ProcessPicture r;
    r.id = "9";
    r.model = "drunet-color";
    r.input = embed::PictureFile{"C:/tmp/in.rgb16", 1920, 1080};
    r.output = "C:/tmp/out.rgb16";
    r.scale = 1;
    r.tile = 256;
    r.pad = 16;
    r.sigma = 0.0627;
    auto back = embed::decodeRequest(embed::encode(embed::Request{r}));
    REQUIRE(back);
    const auto* p = std::get_if<embed::ProcessPicture>(&*back);
    REQUIRE(p);
    CHECK(p->model == "drunet-color");
    CHECK(p->input.width == 1920);
    CHECK(p->tile == 256);
    CHECK(p->pad == 16);
    CHECK(p->sigma == 0.0627);

    const auto refused = [](embed::ProcessPicture bad) { return !embed::decodeRequest(embed::encode(embed::Request{bad})).has_value(); };
    auto bad = r;
    bad.scale = 3;
    CHECK(refused(bad));
    bad = r;
    bad.tile = 100; // not a multiple of 8
    CHECK(refused(bad));
    bad = r;
    bad.tile = 32; // below the minimum
    CHECK(refused(bad));
    bad = r;
    bad.pad = 100;
    CHECK(refused(bad));
    bad = r;
    bad.input.width = embed::kMaxPictureSide + 1;
    CHECK(refused(bad));
    bad = r;
    bad.scale = 4;
    bad.input.width = 8000;
    bad.input.height = 4500; // 4x is 576 megapixels
    CHECK(refused(bad));
    bad = r;
    bad.model.clear();
    CHECK(refused(bad));
    bad = r;
    bad.output.clear();
    CHECK(refused(bad));

    // Loading picture models, and the answer.
    embed::Load load;
    load.id = "10";
    load.pictures = {{"drunet-color", "C:/m/drunet_color.onnx"}, {"nafnet-deblur", "C:/m/nafnet.onnx"}};
    auto loadBack = embed::decodeRequest(embed::encode(embed::Request{load}));
    REQUIRE(loadBack);
    const auto* l = std::get_if<embed::Load>(&*loadBack);
    REQUIRE(l);
    REQUIRE(l->pictures.size() == 2);
    CHECK(l->pictures[1].key == "nafnet-deblur");
    embed::Load many = load;
    many.pictures.resize(embed::kMaxPictureModels + 1, many.pictures.front());
    CHECK_FALSE(embed::decodeRequest(embed::encode(embed::Request{many})));

    embed::PictureDone done{"9", 3840, 2160, 135, 812.5, "DirectML"};
    auto answer = embed::decodeResponse(embed::encode(embed::Response{done}));
    REQUIRE(answer);
    const auto* d = std::get_if<embed::PictureDone>(&*answer);
    REQUIRE(d);
    CHECK(d->width == 3840);
    CHECK(d->tiles == 135);
    CHECK(d->device == "DirectML");
    CHECK(embed::responseId(*answer) == "9");
    CHECK_FALSE(embed::decodeResponse(R"({"type":"picture","id":"9","width":0,"height":10})"));
}

TEST_CASE("Picture files round trip and refuse a short or oversized file", "[ai][video][protocol]") {
    const auto dir = freshDir("picture-files");
    const Picture p = pattern(13, 7, 2);
    auto written = embed::writePictureFile(dir / "p.rgb16", p.width, p.height, p.rgb);
    REQUIRE(written);
    auto read = embed::readPictureFile(*written);
    REQUIRE(read);
    CHECK(*read == p.rgb);
    CHECK_FALSE(embed::writePictureFile(dir / "bad.rgb16", 13, 7, std::span<const std::uint16_t>(p.rgb.data(), 10)));
    CHECK_FALSE(embed::writePictureFile(dir / "zero.rgb16", 0, 7, {}));
    CHECK_FALSE(embed::readPictureFile(embed::PictureFile{dir / "p.rgb16", 14, 7})); // longer than the file
    CHECK_FALSE(embed::readPictureFile(embed::PictureFile{dir / "missing.rgb16", 13, 7}));
}

TEST_CASE("HelperBackend runs a model through the helper and gets the picture back", "[ai][video][helper]") {
    auto options = fakeOptions("normal", "helper-normal");
    const std::filesystem::path work = options.workDir;
    HelperBackend backend(std::move(options));
    const Picture in = pattern(10, 6, 1);
    auto denoised = backend.run(in, stepOf(Model::Denoise), {});
    REQUIRE(denoised);
    CHECK(denoised->device == "FakeCPU");
    CHECK(denoised->picture.width == 10);
    CHECK(denoised->picture.rgb[0] == in.rgb[0] + 1000); // the stand-in's mark of DRUNet
    CHECK(denoised->tiles == 1);
    auto deblurred = backend.run(in, stepOf(Model::Deblur), {});
    REQUIRE(deblurred);
    CHECK(deblurred->picture.rgb[0] == in.rgb[0] + 2000);
    auto upscaled = backend.run(in, stepOf(Model::Upscale2x), {});
    REQUIRE(upscaled);
    CHECK(upscaled->picture.width == 20);
    CHECK(upscaled->picture.height == 12);
    CHECK(upscaled->picture.rgb[(3 * 20 + 5) * 3] == in.rgb[((3 / 2) * 10 + 5 / 2) * 3]);
    auto big = backend.run(pattern(200, 100, 0), stepOf(Model::Denoise, 64), {});
    REQUIRE(big);
    CHECK(big->tiles == 4 * 2); // 200 x 100 in cores of 64
    CHECK(backend.starts() == 1); // one helper for all of it
    CHECK(backend.device() == "FakeCPU");
    // Files are cleaned up.
    int leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(work)) {
        (void)entry;
        ++leftovers;
    }
    CHECK(leftovers == 0);
}

TEST_CASE("HelperBackend: a model that is not downloaded starts no helper", "[ai][video][helper]") {
    auto o = fakeOptions("normal", "helper-missing");
    o.resolveModel = [](Model) { return std::optional<std::filesystem::path>{}; };
    HelperBackend backend(std::move(o));
    auto r = backend.run(pattern(8, 8), stepOf(Model::Deblur), {});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::NotFound);
    CHECK(r.error().message.find("not downloaded") != std::string::npos);
    CHECK(backend.starts() == 0);
}

TEST_CASE("HelperBackend: a GPU crash while loading hands over to the CPU", "[ai][video][helper]") {
    HelperBackend backend(fakeOptions("crash-gpu-picture", "helper-gpucrash"));
    auto r = backend.run(pattern(8, 8), stepOf(Model::Denoise), {});
    REQUIRE(r);
    CHECK(r->picture.rgb[0] == pattern(8, 8).rgb[0] + 1000);
    CHECK(backend.starts() == 2); // the crashed one and the CPU one
}

TEST_CASE("HelperBackend: a helper that crashes on every picture fails cleanly after the CPU try", "[ai][video][helper]") {
    HelperBackend backend(fakeOptions("crash-on-embed", "helper-crashes"));
    auto r = backend.run(pattern(8, 8), stepOf(Model::Denoise), {});
    REQUIRE_FALSE(r);
    CHECK(backend.starts() == 2); // once on the GPU, once on the CPU, then it gives up
    // The next picture starts a new helper and fails the same way: the editor is never taken down.
    auto again = backend.run(pattern(8, 8), stepOf(Model::Denoise), {});
    CHECK_FALSE(again);
}

TEST_CASE("HelperBackend: a crash once is survived (the picture is made again)", "[ai][video][helper]") {
    const auto marker = freshDir("helper-once-marker") / "crashed";
    HelperBackend backend(fakeOptions("crash-once", "helper-once", {"--marker", marker.string()}));
    auto r = backend.run(pattern(8, 8), stepOf(Model::Denoise), {});
    REQUIRE(r);
    CHECK(r->picture.rgb[0] == pattern(8, 8).rgb[0] + 1000);
    CHECK(backend.starts() == 2);
}

TEST_CASE("HelperBackend: cancel kills a helper that is busy, and release frees it", "[ai][video][helper]") {
    HelperBackend backend(fakeOptions("normal", "helper-cancel", {"--picture-sleep", "5000"}));
    std::stop_source stop;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        stop.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    auto r = backend.run(pattern(8, 8), stepOf(Model::Denoise), stop.get_token());
    canceller.join();
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Cancelled);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(4));

    HelperBackend other(fakeOptions("normal", "helper-release"));
    REQUIRE(other.run(pattern(8, 8), stepOf(Model::Denoise), {}));
    other.release();
    REQUIRE(other.run(pattern(8, 8), stepOf(Model::Denoise), {}));
    CHECK(other.starts() == 2);
}

TEST_CASE("HelperBackend refuses an invalid picture", "[ai][video][helper]") {
    HelperBackend backend(fakeOptions("normal", "helper-invalid"));
    auto r = backend.run(Picture{}, stepOf(Model::Denoise), {});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("The process_picture protocol carries a second picture and a timestep for frame interpolation", "[ai][video][protocol]") {
    embed::ProcessPicture r;
    r.id = "11";
    r.model = "rife-v425-lite";
    r.input = embed::PictureFile{"C:/tmp/a.rgb16", 1920, 1080};
    r.input2 = embed::PictureFile{"C:/tmp/b.rgb16", 1920, 1080};
    r.timestep = 0.25;
    r.output = "C:/tmp/out.rgb16";
    r.scale = 1;
    r.tile = 64;
    r.pad = 16;
    auto back = embed::decodeRequest(embed::encode(embed::Request{r}));
    REQUIRE(back);
    const auto* p = std::get_if<embed::ProcessPicture>(&*back);
    REQUIRE(p);
    CHECK(p->input2.width == 1920);
    CHECK(p->input2.height == 1080);
    CHECK(p->timestep == 0.25);
    // A request without a second picture is a plain one (an older parent's message).
    embed::ProcessPicture plain = r;
    plain.input2 = {};
    auto plainBack = embed::decodeRequest(embed::encode(embed::Request{plain}));
    REQUIRE(plainBack);
    CHECK(std::get<embed::ProcessPicture>(*plainBack).input2.path.empty());
    // The two pictures must be one size and the timestep a fraction.
    const auto refused = [](embed::ProcessPicture bad) { return !embed::decodeRequest(embed::encode(embed::Request{bad})).has_value(); };
    auto bad = r;
    bad.input2.width = 1280;
    CHECK(refused(bad));
    bad = r;
    bad.timestep = 1.5;
    CHECK(refused(bad));
    bad = r;
    bad.timestep = -0.1;
    CHECK(refused(bad));
}

TEST_CASE("HelperBackend interpolates two pictures and makes a depth map through the scripted helper", "[ai][video][helper][side]") {
    HelperBackend backend(fakeOptions("normal", "helper-side"));
    const Picture a = pattern(16, 12, 0);
    const Picture b = pattern(16, 12, 5);
    auto between = backend.interpolate(a, b, 0.25, {});
    REQUIRE(between);
    CHECK(between->picture.width == 16);
    CHECK(between->picture.height == 12);
    for (std::size_t i = 0; i < a.rgb.size(); ++i) {
        const double expected = a.rgb[i] + (static_cast<double>(b.rgb[i]) - a.rgb[i]) * 0.25;
        REQUIRE(std::abs(static_cast<double>(between->picture.rgb[i]) - expected) <= 1.0);
    }
    CHECK(between->device == "FakeCPU");
    // Depth: one picture in, a grey map of its size out.
    auto depth = backend.run(pattern(32, 24), stepOf(Model::Depth), {});
    REQUIRE(depth);
    CHECK(depth->picture.width == 32);
    CHECK(depth->picture.height == 24);
    CHECK(depth->picture.rgb[0] == 0);
    CHECK(depth->picture.rgb[3 * 31] == 65535);
    CHECK(depth->picture.rgb[3 * 31] == depth->picture.rgb[3 * 31 + 2]); // grey
    // Two pictures of different sizes are refused before anything is sent; so is an invalid one.
    const auto mismatched = backend.interpolate(a, pattern(8, 8), 0.5, {});
    REQUIRE_FALSE(mismatched);
    CHECK(mismatched.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(backend.interpolate(Picture{}, b, 0.5, {}));
    // The other backends do not interpolate unless they say so.
    FakeBackend plain;
    CHECK(plain.interpolations.empty());
}

#ifdef WABCUT_EMBED_HELPER
namespace {

std::optional<std::filesystem::path> realModel(const char* dir, Model model) {
    const char* file = model == Model::Denoise         ? "drunet_color.onnx"
                       : model == Model::Deblur        ? "deblurring_nafnet_2025may.onnx"
                       : model == Model::UpscaleFast4x ? "realesr-general-x4v3.onnx"
                       : model == Model::Upscale2x     ? "real_esrgan_x2.onnx"
                       : model == Model::Interpolate   ? "rife_v425lite.onnx"
                       : model == Model::Depth         ? "depth_anything_v2_small.onnx"
                                                       : "real_esrgan_x4.onnx";
    const std::filesystem::path path = std::filesystem::path(dir) / file;
    return std::filesystem::is_regular_file(path) ? std::optional<std::filesystem::path>{path} : std::nullopt;
}

/// A picture with edges, ramps and fine texture, smooth enough that a denoiser has something to restore.
Picture scene(int w, int h) {
    Picture p;
    p.width = w;
    p.height = h;
    p.rgb.resize(p.pixels() * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double u = static_cast<double>(x) / w;
            const double v = static_cast<double>(y) / h;
            double r = 0.25 + 0.5 * u;
            double g = 0.3 + 0.4 * v;
            double b = 0.5 + 0.2 * std::sin(u * 9.0) * std::cos(v * 7.0);
            if (((x / 24) + (y / 24)) % 2 == 0 && u > 0.5) {
                r *= 0.6;
                g *= 1.2;
            }
            const auto put = [&](int c, double value) {
                p.rgb[(static_cast<std::size_t>(y) * w + x) * 3 + c] = static_cast<std::uint16_t>(std::clamp(value, 0.0, 1.0) * 65535.0 + 0.5);
            };
            put(0, r);
            put(1, g);
            put(2, b);
        }
    }
    return p;
}

Picture noisy(const Picture& in, double sigma) {
    Picture out = in;
    std::uint32_t state = 12345;
    const auto uniform = [&] {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / 16777216.0;
    };
    for (std::uint16_t& v : out.rgb) {
        // Box-Muller
        const double a = std::sqrt(-2.0 * std::log(std::max(uniform(), 1e-9)));
        const double n = a * std::cos(6.283185307179586 * uniform()) * sigma;
        v = static_cast<std::uint16_t>(std::clamp(v + n * 65535.0, 0.0, 65535.0));
    }
    return out;
}

} // namespace

TEST_CASE("The real models: denoise gains dB, upscalers and the deblurrer give sane pictures (WABCUT_TEST_VIDEO_MODELS_DIR)",
          "[ai][video][model]") {
    const char* dir = std::getenv("WABCUT_TEST_VIDEO_MODELS_DIR");
    if (dir == nullptr || *dir == '\0') {
        SKIP("set WABCUT_TEST_VIDEO_MODELS_DIR to a directory with drunet_color.onnx, deblurring_nafnet_2025may.onnx, "
             "realesr-general-x4v3.onnx, real_esrgan_x2.onnx and real_esrgan_x4.onnx");
    }
    HelperBackendOptions o;
    o.executable = WABCUT_EMBED_HELPER;
    o.workDir = freshDir("helper-real");
    o.resolveModel = [dir](Model m) { return realModel(dir, m); };
    o.loadTimeout = std::chrono::seconds(300);
    o.requestBase = std::chrono::seconds(300);
    HelperBackend backend(std::move(o));

    const Picture clean = scene(192, 128);
    const Picture grainy = noisy(clean, 15.0 / 255.0);
    const double before = psnr(clean, grainy);

    // Denoise: DRUNet at sigma 15, tiled in 64 pixel cores (tiles and seams exercised on a picture of 3 x 2 tiles).
    Step denoise = stepOf(Model::Denoise, 64);
    denoise.sigma = 15.0 / 255.0;
    auto cleaned = backend.run(grainy, denoise, {});
    REQUIRE(cleaned);
    INFO("device " << cleaned->device << ", PSNR " << before << " -> " << psnr(clean, cleaned->picture) << " dB, " << cleaned->milliseconds << " ms");
    CHECK(cleaned->picture.width == 192);
    CHECK(cleaned->tiles == 6);
    CHECK(psnr(clean, cleaned->picture) > before + 3.0);

    // The same picture in one tile (no seams at all) differs from the tiled one by very little.
    Step single = denoise;
    single.tile = 192 + 8; // 200 is a multiple of 8 and covers the picture
    auto whole = backend.run(grainy, single, {});
    REQUIRE(whole);
    CHECK(psnr(whole->picture, cleaned->picture) > 40.0);

    // The GPU and the CPU make the same picture (a graph DirectML runs differently from the CPU would be caught here).
    HelperBackendOptions cpuOptions;
    cpuOptions.executable = WABCUT_EMBED_HELPER;
    cpuOptions.workDir = freshDir("helper-real-cpu");
    cpuOptions.resolveModel = [dir](Model m) { return realModel(dir, m); };
    cpuOptions.device = "cpu";
    cpuOptions.loadTimeout = std::chrono::seconds(300);
    cpuOptions.requestBase = std::chrono::seconds(600);
    HelperBackend cpu(std::move(cpuOptions));

    const Picture small = halved(clean);
    const auto mean = [](const Picture& p) {
        double sum = 0.0;
        for (const std::uint16_t v : p.rgb) {
            sum += v;
        }
        return sum / (65535.0 * static_cast<double>(p.rgb.size()));
    };
    for (const Model m : {Model::UpscaleFast4x, Model::Upscale2x, Model::Upscale4x, Model::Deblur}) {
        const bool upscaler = describe(m).scale > 1;
        const Picture& input = upscaler ? small : clean;
        auto gpu = backend.run(input, stepOf(m, 64), {});
        auto host = cpu.run(input, stepOf(m, 64), {});
        INFO(describe(m).key << " on the GPU: " << (gpu ? std::string("ok") : gpu.error().message) << "; on the CPU: "
                             << (host ? std::string("ok") : host.error().message));
        REQUIRE(gpu);
        REQUIRE(host);
        const int scale = describe(m).scale;
        REQUIRE(gpu->picture.width == input.width * scale);
        REQUIRE(host->picture.width == input.width * scale);
        INFO(describe(m).key << ": " << gpu->device << " " << gpu->milliseconds << " ms, " << host->device << " " << host->milliseconds
                             << " ms; mean in " << mean(input) << " out " << mean(gpu->picture) << " (cpu " << mean(host->picture)
                             << "); gpu against cpu " << psnr(host->picture, gpu->picture) << " dB");
        CHECK(psnr(host->picture, gpu->picture) > 35.0);
        if (upscaler) {
            // Sane against the original: a model that sharpens a synthetic picture is allowed to differ from a bilinear
            // enlargement, a wrecked one (wrong range, wrong channels) is not.
            const Picture back = resized(host->picture, clean.width, clean.height);
            INFO("against the original " << psnr(clean, back) << " dB, bilinear " << psnr(clean, resized(small, clean.width, clean.height)) << " dB");
            CHECK(psnr(clean, back) > 20.0);
            CHECK(std::abs(mean(back) - mean(clean)) < 0.03);
        } else {
            INFO("against the original " << psnr(clean, host->picture) << " dB");
            CHECK(std::abs(mean(host->picture) - mean(clean)) < 0.1);
            CHECK(psnr(clean, host->picture) > 15.0);
        }
    }
}
TEST_CASE("The real models: RIFE makes the frame between two better than a cross-fade, Depth Anything gives a grey map (WABCUT_TEST_VIDEO_MODELS_DIR)",
          "[ai][video][model][side]") {
    const char* dir = std::getenv("WABCUT_TEST_VIDEO_MODELS_DIR");
    if (dir == nullptr || *dir == '\0') {
        SKIP("set WABCUT_TEST_VIDEO_MODELS_DIR to a directory with rife_v425lite.onnx and depth_anything_v2_small.onnx");
    }
    if (!realModel(dir, Model::Interpolate) || !realModel(dir, Model::Depth)) {
        FAIL("rife_v425lite.onnx or depth_anything_v2_small.onnx is missing from WABCUT_TEST_VIDEO_MODELS_DIR");
    }
    const auto makeBackend = [&](const char* device, const char* name) {
        HelperBackendOptions o;
        o.executable = WABCUT_EMBED_HELPER;
        o.workDir = freshDir(name);
        o.resolveModel = [dir](Model m) { return realModel(dir, m); };
        o.device = device;
        o.loadTimeout = std::chrono::seconds(300);
        o.requestBase = std::chrono::seconds(600);
        return HelperBackend(std::move(o));
    };
    HelperBackend gpu = makeBackend("auto", "helper-real-side");
    HelperBackend cpu = makeBackend("cpu", "helper-real-side-cpu");

    // A textured picture moved 16 pixels between the two frames: the frame between them is the picture moved 8.
    const Picture wide = scene(320, 192);
    const auto moved = [&](int dx) {
        Picture p;
        p.width = 256;
        p.height = 192;
        p.rgb.resize(p.pixels() * 3);
        for (int y = 0; y < p.height; ++y) {
            for (int x = 0; x < p.width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    p.rgb[(static_cast<std::size_t>(y) * p.width + x) * 3 + c] = wide.rgb[(static_cast<std::size_t>(y) * wide.width + (x + 32 - dx)) * 3 + c];
                }
            }
        }
        return p;
    };
    const Picture a = moved(0);
    const Picture b = moved(16);
    const Picture truth = moved(8);
    auto onGpu = gpu.interpolate(a, b, 0.5, {});
    auto onCpu = cpu.interpolate(a, b, 0.5, {});
    REQUIRE(onGpu);
    REQUIRE(onCpu);
    REQUIRE(onGpu->picture.width == 256);
    REQUIRE(onGpu->picture.height == 192);
    const Picture fade = mixed(a, b, 0.5);
    INFO("RIFE on " << onGpu->device << ": " << onGpu->milliseconds << " ms, PSNR against the truth " << psnr(truth, onGpu->picture)
                    << " dB, a cross-fade " << psnr(truth, fade) << " dB, GPU against CPU " << psnr(onCpu->picture, onGpu->picture) << " dB");
    CHECK(psnr(truth, onCpu->picture) > psnr(truth, fade) + 1.0); // the model follows the motion, a cross-fade doubles the edges
    CHECK(psnr(onCpu->picture, onGpu->picture) > 30.0);

    // Depth: a grey map of the picture's size, not flat, the same on the GPU and the CPU.
    Step step = stepOf(Model::Depth);
    const Picture input = scene(266, 154); // multiples of 14 are the helper's business
    auto depthGpu = gpu.run(input, step, {});
    auto depthCpu = cpu.run(input, step, {});
    REQUIRE(depthGpu);
    REQUIRE(depthCpu);
    CHECK(depthGpu->picture.width == 266);
    CHECK(depthGpu->picture.height == 154);
    std::uint16_t lo = 65535;
    std::uint16_t hi = 0;
    for (std::size_t i = 0; i < depthCpu->picture.pixels(); ++i) {
        const std::uint16_t v = depthCpu->picture.rgb[3 * i];
        CHECK(v == depthCpu->picture.rgb[3 * i + 1]);
        CHECK(v == depthCpu->picture.rgb[3 * i + 2]);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    CHECK(hi - lo > 20000); // the percentile normalisation spreads the map over the range
    INFO("Depth Anything on " << depthGpu->device << ": " << depthGpu->milliseconds << " ms, GPU against CPU "
                              << psnr(depthCpu->picture, depthGpu->picture) << " dB");
    CHECK(psnr(depthCpu->picture, depthGpu->picture) > 25.0);
}
#endif
