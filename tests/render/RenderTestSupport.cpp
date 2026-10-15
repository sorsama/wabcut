#include "RenderTestSupport.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace wabcut::render::test {

namespace {

int g_argc = 1;
char g_argv0[] = "wabcut_render_tests";
char* g_argv[] = {g_argv0, nullptr};
QGuiApplication* g_app = nullptr;

class QtApplicationListener final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override {
        // A QGuiApplication for QShader resources and Qt's platform state; the tests never create a window.
        if (!g_app) {
            g_app = new QGuiApplication(g_argc, g_argv);
        }
    }
    void testRunEnded(const Catch::TestRunStats&) override {
        delete g_app;
        g_app = nullptr;
    }
};

/// Memory owner of a synthetic frame; counts live frames when asked to.
struct Storage {
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<std::atomic<int>> alive;
    ~Storage() {
        if (alive) {
            alive->fetch_sub(1);
        }
    }
};

std::shared_ptr<Storage> newStorage(std::size_t size, std::shared_ptr<std::atomic<int>> alive) {
    auto s = std::make_shared<Storage>();
    s->bytes.assign(size, 0);
    if (alive) {
        alive->fetch_add(1);
        s->alive = std::move(alive);
    }
    return s;
}

void put(std::uint8_t* row, int index, int value, int bytes) {
    if (bytes == 1) {
        row[index] = static_cast<std::uint8_t>(value);
    } else {
        const auto v = static_cast<std::uint16_t>(value);
        std::memcpy(row + 2 * index, &v, 2);
    }
}

} // namespace

std::array<int, 3> encodeYuv(const Rgb& rgb, ColorMatrix matrix, ColorRange range, int bits) {
    double kr = 0.2126;
    double kb = 0.0722;
    if (matrix == ColorMatrix::BT601) {
        kr = 0.299;
        kb = 0.114;
    } else if (matrix == ColorMatrix::BT2020NonConstant) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;
    const double y = kr * rgb[0] + kg * rgb[1] + kb * rgb[2];
    const double pb = (rgb[2] - y) / (2.0 * (1.0 - kb));
    const double pr = (rgb[0] - y) / (2.0 * (1.0 - kr));
    const double scale = std::ldexp(1.0, bits - 8);
    const double maxCode = std::ldexp(1.0, bits) - 1.0;
    if (range == ColorRange::Full) {
        return {static_cast<int>(std::lround(maxCode * y)),
                static_cast<int>(std::lround(maxCode * pb + std::ldexp(1.0, bits - 1))),
                static_cast<int>(std::lround(maxCode * pr + std::ldexp(1.0, bits - 1)))};
    }
    return {static_cast<int>(std::lround((219.0 * y + 16.0) * scale)),
            static_cast<int>(std::lround((224.0 * pb + 128.0) * scale)),
            static_cast<int>(std::lround((224.0 * pr + 128.0) * scale))};
}

media::VideoFrame makeFrame(media::PixelFormat format, int width, int height, const ColorInfo& color,
                            const RgbAt& rgbAt, std::shared_ptr<std::atomic<int>> alive) {
    using media::PixelFormat;
    const int bits = media::pixelFormatBitDepth(format);
    const int sampleBytes = bits > 8 ? 2 : 1;
    const bool p010 = format == PixelFormat::P010;
    const bool semi = format == PixelFormat::NV12 || p010;
    const bool withAlpha = format == PixelFormat::YUVA420P || format == PixelFormat::YUVA444P10;
    int sx = 0;
    int sy = 0;
    if (format == PixelFormat::NV12 || p010 || format == PixelFormat::YUV420P ||
        format == PixelFormat::YUV420P10 || format == PixelFormat::YUVA420P) {
        sx = sy = 1;
    } else if (format == PixelFormat::YUV422P || format == PixelFormat::YUV422P10) {
        sx = 1;
    }
    const int cw = (width + (1 << sx) - 1) >> sx;
    const int ch = (height + (1 << sy) - 1) >> sy;
    const int planes = media::pixelFormatPlaneCount(format);
    std::array<int, 4> pw{width, cw, cw, width};
    std::array<int, 4> ph{height, ch, ch, height};
    std::array<int, 4> stride{};
    std::array<std::size_t, 4> offset{};
    std::size_t total = 0;
    for (int p = 0; p < planes; ++p) {
        const int texel = media::pixelFormatBytesPerTexel(format, p);
        stride[static_cast<std::size_t>(p)] = pw[static_cast<std::size_t>(p)] * texel + 32; // padded rows
        offset[static_cast<std::size_t>(p)] = total;
        total += static_cast<std::size_t>(stride[static_cast<std::size_t>(p)]) *
                 static_cast<std::size_t>(ph[static_cast<std::size_t>(p)]);
    }
    auto storage = newStorage(total, std::move(alive));
    std::uint8_t* base = storage->bytes.data();
    const auto code = [&](int v) { return p010 ? v << 6 : v; };

    for (int y = 0; y < height; ++y) {
        std::uint8_t* row =
            base + offset[0] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[0]);
        for (int x = 0; x < width; ++x) {
            put(row, x, code(encodeYuv(rgbAt(x, y), color.matrix, color.range, bits)[0]), sampleBytes);
        }
    }
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const auto yuv = encodeYuv(rgbAt(std::min(x << sx, width - 1), std::min(y << sy, height - 1)),
                                       color.matrix, color.range, bits);
            if (semi) {
                std::uint8_t* row =
                    base + offset[1] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[1]);
                put(row, 2 * x, code(yuv[1]), sampleBytes);
                put(row, 2 * x + 1, code(yuv[2]), sampleBytes);
            } else {
                std::uint8_t* cb =
                    base + offset[1] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[1]);
                std::uint8_t* cr =
                    base + offset[2] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[2]);
                put(cb, x, yuv[1], sampleBytes);
                put(cr, x, yuv[2], sampleBytes);
            }
        }
    }

    if (withAlpha) { // opaque alpha plane
        const int maxCode = (1 << bits) - 1;
        for (int y = 0; y < height; ++y) {
            std::uint8_t* row =
                base + offset[3] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[3]);
            for (int x = 0; x < width; ++x) {
                put(row, x, maxCode, sampleBytes);
            }
        }
    }

    media::VideoFrame f;
    f.width = width;
    f.height = height;
    f.format = format;
    f.color = color;
    f.alpha = withAlpha ? AlphaMode::Straight : AlphaMode::None;
    f.duration = Ticks{kTicksPerSecond / 30};
    media::CpuPlanes cpu;
    for (int p = 0; p < planes; ++p) {
        const auto i = static_cast<std::size_t>(p);
        cpu.planes[i] = media::Plane{base + offset[i], stride[i], pw[i], ph[i]};
    }
    cpu.owner = storage;
    f.payload = std::move(cpu);
    return f;
}

media::VideoFrame withNewOwner(const media::VideoFrame& frame, std::shared_ptr<std::atomic<int>> alive) {
    struct Wrapper {
        std::shared_ptr<void> inner;
        std::shared_ptr<std::atomic<int>> alive;
        ~Wrapper() {
            if (alive) {
                alive->fetch_sub(1);
            }
        }
    };
    auto wrapper = std::make_shared<Wrapper>();
    wrapper->inner = std::const_pointer_cast<void>(frame.cpu()->owner);
    if (alive) {
        alive->fetch_add(1);
        wrapper->alive = std::move(alive);
    }
    media::VideoFrame copy = frame;
    auto cpu = *frame.cpu();
    cpu.owner = wrapper;
    copy.payload = cpu;
    return copy;
}

media::VideoFrame makeRgbaFrame(int width, int height,
                                const std::function<std::array<std::uint8_t, 4>(int, int)>& at,
                                std::shared_ptr<std::atomic<int>> alive) {
    const int stride = width * 4 + 16;
    auto storage =
        newStorage(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), std::move(alive));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto px = at(x, y);
            std::memcpy(storage->bytes.data() +
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
                            static_cast<std::size_t>(x) * 4,
                        px.data(), 4);
        }
    }
    media::VideoFrame f;
    f.width = width;
    f.height = height;
    f.format = media::PixelFormat::RGBA8;
    f.alpha = AlphaMode::Straight;
    f.duration = Ticks{kTicksPerSecond / 30};
    media::CpuPlanes cpu;
    cpu.planes[0] = media::Plane{storage->bytes.data(), stride, width, height};
    cpu.owner = storage;
    f.payload = std::move(cpu);
    return f;
}

FrameView makeYuvaView(int width, int height, int bits, const ColorInfo& color, const RgbAt& rgbAt,
                       const std::function<double(int, int)>& alphaAt) {
    const int sampleBytes = bits > 8 ? 2 : 1;
    const int cw = (width + 1) / 2;
    const int ch = (height + 1) / 2;
    const std::array<int, 4> pw{width, cw, cw, width};
    const std::array<int, 4> ph{height, ch, ch, height};
    std::array<std::size_t, 4> offset{};
    std::array<int, 4> stride{};
    std::size_t total = 0;
    for (std::size_t p = 0; p < 4; ++p) {
        stride[p] = pw[p] * sampleBytes + 8;
        offset[p] = total;
        total += static_cast<std::size_t>(stride[p]) * static_cast<std::size_t>(ph[p]);
    }
    auto storage = newStorage(total, {});
    std::uint8_t* base = storage->bytes.data();
    const double maxCode = std::ldexp(1.0, bits) - 1.0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            put(base + offset[0] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[0]), x,
                encodeYuv(rgbAt(x, y), color.matrix, color.range, bits)[0], sampleBytes);
            put(base + offset[3] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[3]), x,
                static_cast<int>(std::lround(alphaAt(x, y) * maxCode)), sampleBytes);
        }
    }
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const auto yuv = encodeYuv(rgbAt(std::min(2 * x, width - 1), std::min(2 * y, height - 1)),
                                       color.matrix, color.range, bits);
            put(base + offset[1] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[1]), x,
                yuv[1], sampleBytes);
            put(base + offset[2] + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride[2]), x,
                yuv[2], sampleBytes);
        }
    }
    FrameView v;
    v.width = width;
    v.height = height;
    v.layout = PlaneLayout::Planar4;
    v.encoding = bits > 8 ? SampleEncoding::Bits10Low : SampleEncoding::Bits8;
    v.chromaShiftX = 1;
    v.chromaShiftY = 1;
    v.planeCount = 4;
    for (std::size_t p = 0; p < 4; ++p) {
        v.planes[p] = PlaneView{base + offset[p], stride[p], pw[p], ph[p]};
    }
    v.color = color;
    v.alpha = AlphaMode::Straight;
    v.identity = storage.get();
    v.owner = storage;
    v.keepAlive = storage;
    return v;
}

Rgb barColor(int index) {
    static constexpr std::array<Rgb, 8> kBars{{{0.75, 0.75, 0.75},
                                               {0.75, 0.75, 0.0},
                                               {0.0, 0.75, 0.75},
                                               {0.0, 0.75, 0.0},
                                               {0.75, 0.0, 0.75},
                                               {0.75, 0.0, 0.0},
                                               {0.0, 0.0, 0.75},
                                               {0.0, 0.0, 0.0}}};
    return kBars[static_cast<std::size_t>(std::clamp(index, 0, 7))];
}

CompositeLayer layerOf(media::VideoFrame frame, LayerTransform transform, double opacity) {
    CompositeLayer layer;
    layer.frame = std::move(frame);
    layer.transform = transform;
    layer.opacity = opacity;
    return layer;
}

CompositeLayer layerOf(FrameView view, LayerTransform transform, double opacity) {
    CompositeLayer layer;
    layer.view = std::move(view);
    layer.transform = transform;
    layer.opacity = opacity;
    return layer;
}

std::shared_ptr<PreparedComposite> compositeOf(CanvasSize canvas, std::vector<CompositeLayer> layers,
                                               ColorPipeline color) {
    auto c = std::make_shared<PreparedComposite>();
    c->canvas = canvas;
    c->color = std::move(color);
    c->layers = std::move(layers);
    c->frameDuration = Ticks{kTicksPerSecond / 60};
    return c;
}

std::unique_ptr<OffscreenRenderer> makeRenderer(OutputRole role, OutputAlpha alpha) {
    OffscreenOptions options;
    options.role = role;
    options.alpha = alpha;
    auto renderer = OffscreenRenderer::create(options);
    if (!renderer) {
        FAIL("OffscreenRenderer::create failed: " << renderer.error().message);
    }
    return std::move(*renderer);
}

Rgb pixelOf(const RenderedImage& image, int x, int y) {
    const std::uint8_t* p = image.pixel(x, y);
    return {p[0] / 255.0, p[1] / 255.0, p[2] / 255.0};
}

double alphaOf(const RenderedImage& image, int x, int y) {
    return image.pixel(x, y)[3] / 255.0;
}

double maxDiff(const Rgb& a, const Rgb& b) {
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

std::string describe(const Rgb& c) {
    std::ostringstream s;
    s << "(" << c[0] * 255.0 << ", " << c[1] * 255.0 << ", " << c[2] * 255.0 << ")";
    return s.str();
}

double psnrRgb(const RenderedImage& image, const std::vector<std::uint8_t>& rgb24) {
    const std::size_t pixels = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    if (rgb24.size() < pixels * 3) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < pixels; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const double d =
                static_cast<double>(image.rgba[i * 4 + c]) - static_cast<double>(rgb24[i * 3 + c]);
            sum += d * d;
        }
    }
    const double mse = sum / static_cast<double>(pixels * 3);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

QString outputDir() {
    const QString dir = QStringLiteral(WABCUT_TEST_OUTPUT_DIR "/render");
    QDir().mkpath(dir);
    return dir;
}

bool savePng(const RenderedImage& image, const QString& fileName) {
    const QImage view(image.rgba.data(), image.width, image.height, image.width * 4, QImage::Format_RGBA8888);
    const QString path = outputDir() + QLatin1Char('/') + fileName;
    QDir().mkpath(QFileInfo(path).absolutePath()); // "grade/x.png", "graphics/y.png"
    return view.save(path);
}

std::string mediaPath(std::string_view name) {
    const std::u8string p = (std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name).generic_u8string();
    return std::string(reinterpret_cast<const char*>(p.data()), p.size());
}

bool haveMedia(std::string_view name) {
    return std::filesystem::is_regular_file(std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name);
}

bool haveFfmpeg() {
    return std::filesystem::is_regular_file(std::filesystem::path(WABCUT_FFMPEG_EXECUTABLE));
}

int runFfmpeg(const QStringList& arguments) {
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process.start(QString::fromUtf8(WABCUT_FFMPEG_EXECUTABLE),
                  QStringList{"-hide_banner", "-loglevel", "error", "-nostdin", "-y"} + arguments);
    if (!process.waitForFinished(120000)) {
        process.kill();
        return -1;
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

} // namespace wabcut::render::test

CATCH_REGISTER_LISTENER(wabcut::render::test::QtApplicationListener)
