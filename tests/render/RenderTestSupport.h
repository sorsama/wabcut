#pragma once

// Shared helpers for the render tests: one QGuiApplication per process (no window is ever created), synthetic
// frames in every pixel format built with the FORWARD Y'CbCr equations (independent of the decode math under
// test), an offscreen renderer on the real GPU, PSNR, and PNG output under <build>/test-output/render/.

#include <wabcut/media/VideoFrame.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/FrameView.h>
#include <wabcut/render/OffscreenRenderer.h>
#include <wabcut/render/PreparedComposite.h>

#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::render::test {

/// Non-linear R'G'B' in [0, 1].
using Rgb = std::array<double, 3>;
using RgbAt = std::function<Rgb(int x, int y)>;

/// Y'CbCr code values for R'G'B' (Rec. ITU-R BT.601/709/2020 forward equations, H.273 quantization).
std::array<int, 3> encodeYuv(const Rgb& rgb, ColorMatrix matrix, ColorRange range, int bits);

/// A CPU frame of `format` whose pixels encode rgbAt(x, y). Chroma takes the colour at its co-sited
/// (left/top) luma sample. Planes get padded strides. `alive` (optional) counts live frames: +1 now, -1 when
/// the memory is freed.
media::VideoFrame makeFrame(media::PixelFormat format, int width, int height, const ColorInfo& color,
                            const RgbAt& rgbAt, std::shared_ptr<std::atomic<int>> alive = {});
/// The same pixels under a new owner (a new frame identity, as a decoder hands out), counted in `alive`.
media::VideoFrame withNewOwner(const media::VideoFrame& frame, std::shared_ptr<std::atomic<int>> alive);
/// RGBA8 frame with explicit 8-bit RGBA values.
media::VideoFrame makeRgbaFrame(int width, int height,
                                const std::function<std::array<std::uint8_t, 4>(int, int)>& at,
                                std::shared_ptr<std::atomic<int>> alive = {});
/// Planar YUVA 4:2:0 (8- or 10-bit, low bits) as a FrameView (media has no YUVA format yet).
FrameView makeYuvaView(int width, int height, int bits, const ColorInfo& color, const RgbAt& rgbAt,
                       const std::function<double(int, int)>& alphaAt);

/// SMPTE-style 75 % colour bars: white, yellow, cyan, green, magenta, red, blue, black; 8 bars of `barWidth`.
Rgb barColor(int index);

CompositeLayer layerOf(media::VideoFrame frame, LayerTransform transform = {}, double opacity = 1.0);
CompositeLayer layerOf(FrameView view, LayerTransform transform = {}, double opacity = 1.0);
std::shared_ptr<PreparedComposite> compositeOf(CanvasSize canvas, std::vector<CompositeLayer> layers,
                                               ColorPipeline color = {});

/// Offscreen renderer on the platform default backend (D3D11 on Windows). REQUIREs success.
std::unique_ptr<OffscreenRenderer> makeRenderer(OutputRole role = OutputRole::Export,
                                                OutputAlpha alpha = OutputAlpha::Opaque);

/// RGB of an output pixel in [0, 1].
Rgb pixelOf(const RenderedImage& image, int x, int y);
double alphaOf(const RenderedImage& image, int x, int y);
/// Max |a - b| per channel.
double maxDiff(const Rgb& a, const Rgb& b);
std::string describe(const Rgb& c);

/// PSNR (dB) over RGB of two RGBA8/RGB24 buffers of the same size.
double psnrRgb(const RenderedImage& image, const std::vector<std::uint8_t>& rgb24);

QString outputDir(); // <build>/test-output/render (created)
bool savePng(const RenderedImage& image, const QString& fileName);

std::string mediaPath(std::string_view name);
bool haveMedia(std::string_view name);
/// Runs ffmpeg (from the vcpkg tree) and returns its exit code.
int runFfmpeg(const QStringList& arguments);
bool haveFfmpeg();

} // namespace wabcut::render::test
