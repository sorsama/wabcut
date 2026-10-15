// End to end on real media: decode a test-media frame (M3 decoder, hardware when available), composite it on
// the GPU and compare with FFmpeg's own swscale conversion of the same frame.

#include "RenderTestSupport.h"

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/render/OffscreenRenderer.h>
#include <wabcut/render/ZeroCopy.h>

#include <QFile>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

/// Frame number from gen_test_media.py's barcode: 16 cells of 32x32 luma in the top-left, bit b at x = 32b.
std::int64_t barcodeOf(const RenderedImage& image) {
    std::int64_t index = 0;
    for (int bit = 0; bit < 16; ++bit) {
        if (image.pixel(bit * 32 + 16, 16)[1] > 128) {
            index |= std::int64_t{1} << bit;
        }
    }
    return index;
}

} // namespace

TEST_CASE("Decoded H.264 frame composites like FFmpeg's swscale (PSNR >= 45 dB)", "[render][gpu][media]") {
    const char* clip = "h264_1080p60_aac.mp4";
    if (!haveMedia(clip) || !haveFfmpeg()) {
        SKIP("test media or ffmpeg missing");
    }
    constexpr int kFrame = 37;
    auto decoder = media::VideoDecoder::open(mediaPath(clip));
    REQUIRE(decoder.has_value());
    const Ticks t = (*decoder)->firstFrameTime() + Ticks{kFrame * (kTicksPerSecond / 60)};
    auto frame = (*decoder)->frameAt(t);
    REQUIRE(frame.has_value());
    INFO("decoder backend " << (*decoder)->backendName() << ", format "
                            << media::pixelFormatName(frame->format));

    auto renderer = makeRenderer();
    auto composite = compositeOf({frame->width, frame->height}, {layerOf(*frame)});
    auto image = renderer->render(composite);
    REQUIRE(image.has_value());
    CHECK(barcodeOf(*image) == kFrame);
    CHECK(savePng(*image, QStringLiteral("decoded_h264_1080p60_frame37.png")));

    // Reference: the same frame through swscale, BT.709 limited -> full-range RGB, chroma interpolated
    // bilinearly at the stream's siting. Untagged H.264 4:2:0 is MPEG-2 "left" (co-sited horizontally,
    // centred vertically), which is what ColorPipeline resolves too; swscale must be told (its own default is
    // centre siting), else chroma edges differ by half a pixel (~36 dB).
    const QString ref = outputDir() + QStringLiteral("/decoded_h264_1080p60_frame37.ref.rgb");
    QFile::remove(ref);
    REQUIRE(
        runFfmpeg({"-i", QString::fromStdString(mediaPath(clip)), "-vf",
                   QStringLiteral("select=eq(n\\,%1),scale=in_color_matrix=bt709:in_range=tv:out_range=pc:"
                                  "in_h_chr_pos=0:in_v_chr_pos=128:flags=bilinear+accurate_rnd+full_chroma_"
                                  "int+full_chroma_inp")
                       .arg(kFrame),
                   "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", ref}) == 0);
    QFile file(ref);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    REQUIRE(bytes.size() == static_cast<qsizetype>(image->width) * image->height * 3);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(bytes.size()));
    std::memcpy(rgb.data(), bytes.constData(), rgb.size());

    {
        // Our output as raw RGB24 too, for side-by-side analysis with ffmpeg tools.
        QFile ours(outputDir() + QStringLiteral("/decoded_h264_1080p60_frame37.rgb"));
        if (ours.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            for (std::size_t i = 0; i < image->rgba.size(); i += 4) {
                ours.write(reinterpret_cast<const char*>(image->rgba.data() + i), 3);
            }
        }
    }
    const double psnr = psnrRgb(*image, rgb);
    INFO("PSNR " << psnr << " dB");
    WARN("decoded-frame PSNR vs swscale: " << psnr << " dB (" << (*decoder)->backendName() << ", "
                                           << renderer->backendName() << " on " << renderer->adapterName()
                                           << ")");
    CHECK(psnr >= 45.0);
}

TEST_CASE("Zero-copy: a D3D11 decoder surface composites like the CPU path", "[render][gpu][media][zerocopy]") {
    const char* clip = "h264_1080p60_aac.mp4";
    if (!haveMedia(clip)) {
        SKIP("test media missing");
    }
    constexpr int kFrame = 37;
    // CPU path reference: software decode, frames uploaded.
    auto cpuRenderer = makeRenderer();
    media::DecoderOptions software;
    software.allowHardware = false;
    auto cpuDecoder = media::VideoDecoder::open(mediaPath(clip), -1, software);
    REQUIRE(cpuDecoder.has_value());
    auto cpuFrame = (*cpuDecoder)->frameAt((*cpuDecoder)->firstFrameTime() + Ticks{kFrame * (kTicksPerSecond / 60)});
    REQUIRE(cpuFrame.has_value());
    auto reference = cpuRenderer->render(compositeOf({cpuFrame->width, cpuFrame->height}, {layerOf(*cpuFrame)}));
    REQUIRE(reference.has_value());

    // Zero-copy path: the renderer shares its D3D11 device, the decoder outputs native surfaces on it.
    OffscreenOptions options;
    options.shareDeviceWithDecoders = true;
    auto created = OffscreenRenderer::create(options);
    REQUIRE(created.has_value());
    auto& renderer = **created;
    if (renderer.backendName() != "D3D11" || !decodersShareDevice(renderer.rhi())) {
        SKIP("zero-copy needs a D3D11 device with video support");
    }
    media::DecoderOptions native;
    native.outputMode = media::OutputMode::Native;
    auto decoder = media::VideoDecoder::open(mediaPath(clip), -1, native);
    REQUIRE(decoder.has_value());
    auto frame = (*decoder)->frameAt((*decoder)->firstFrameTime() + Ticks{kFrame * (kTicksPerSecond / 60)});
    REQUIRE(frame.has_value());
    REQUIRE(frame->payloadKind() == media::PayloadKind::D3D11Texture);
    auto image = renderer.render(compositeOf({frame->width, frame->height}, {layerOf(*frame)}));
    REQUIRE(image.has_value());
    const FrameImporterStats stats = renderer.compositor().importer().stats();
    CHECK(stats.nativeImports == 1);
    CHECK(stats.uploads == 0); // nothing went through the CPU
    CHECK(savePng(*image, QStringLiteral("zero_copy_frame37.png")));

    std::vector<std::uint8_t> rgb(reference->rgba.size() / 4 * 3);
    for (std::size_t i = 0, j = 0; i < reference->rgba.size(); i += 4, j += 3) {
        std::memcpy(rgb.data() + j, reference->rgba.data() + i, 3);
    }
    const double psnr = psnrRgb(*image, rgb);
    INFO("zero-copy vs CPU path PSNR " << psnr << " dB");
    CHECK(psnr >= 45.0);
    (void)media::setSharedD3D11Device(nullptr, nullptr);
}
