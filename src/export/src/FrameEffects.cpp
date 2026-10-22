#include "FrameEffects.h"

#include <wabcut/color/Looks.h>
#include <wabcut/color/Lut.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Timecode.h>

#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

namespace wabcut::exporter::detail {

/// An overlay drawn into its own premultiplied ARGB32 image, placed in the output frame.
struct FrameEffects::Layer {
    QImage image;
    int x = 0;
    int y = 0;
    double opacity = 1.0;
};

namespace {

/// Runs fn(rowBegin, rowEnd) over [0, rows) on a few threads (the CPU limit preference caps them).
template <class Fn> void parallelRows(int rows, Fn&& fn) {
    const int hardware = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int limit = exportThreadLimit();
    const int threads = std::clamp(std::min(limit > 0 ? limit : hardware, 8), 1, std::max(1, rows / 16));
    if (threads <= 1) {
        fn(0, rows);
        return;
    }
    std::vector<std::jthread> pool;
    const int chunk = (rows + threads - 1) / threads;
    for (int t = 1; t < threads; ++t) {
        const int begin = t * chunk;
        const int end = std::min(rows, begin + chunk);
        if (begin < end) {
            pool.emplace_back([&fn, begin, end] { fn(begin, end); });
        }
    }
    fn(0, std::min(rows, chunk));
}

bool deep(const AVFrame* f) {
    return f->format == AV_PIX_FMT_RGBA64LE;
}

/// Pixel (x, y) channel c of an RGBA / RGBA64LE frame as 0..1, and back.
inline float load(const AVFrame* f, int x, int y, int c) {
    const std::uint8_t* row = f->data[0] + static_cast<std::ptrdiff_t>(y) * f->linesize[0];
    if (deep(f)) {
        return static_cast<float>(reinterpret_cast<const std::uint16_t*>(row)[x * 4 + c]) / 65535.0f;
    }
    return static_cast<float>(row[x * 4 + c]) / 255.0f;
}

inline void store(AVFrame* f, int x, int y, int c, float v) {
    std::uint8_t* row = f->data[0] + static_cast<std::ptrdiff_t>(y) * f->linesize[0];
    v = std::clamp(v, 0.0f, 1.0f);
    if (deep(f)) {
        reinterpret_cast<std::uint16_t*>(row)[x * 4 + c] = static_cast<std::uint16_t>(std::lround(v * 65535.0f));
    } else {
        row[x * 4 + c] = static_cast<std::uint8_t>(std::lround(v * 255.0f));
    }
}

std::filesystem::path resolvePath(const std::string& utf8, const std::filesystem::path& projectFile) {
    std::filesystem::path p = pathFromUtf8(utf8);
    if (p.is_relative() && !projectFile.empty()) {
        p = projectFile.parent_path() / p;
    }
    return p;
}

/// The top-left corner of a `w` x `h` overlay at `placement` in a `W` x `H` frame.
std::pair<int, int> place(const OverlayPlacement& placement, int w, int h, int W, int H) {
    const int col = static_cast<int>(placement.position) % 3; // left, centre, right
    const int row = static_cast<int>(placement.position) / 3; // top, centre, bottom
    const int x = col == 0 ? 0 : col == 1 ? (W - w) / 2 : W - w;
    const int y = row == 0 ? 0 : row == 1 ? (H - h) / 2 : H - h;
    return {x + placement.offsetX, y + placement.offsetY};
}

/// White text on a translucent black box (60 %), `pixelSize` high.
QImage textImage(const QString& text, int pixelSize, bool monospace) {
    QFont font = monospace ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QGuiApplication::font();
    font.setPixelSize(std::max(4, pixelSize));
    font.setWeight(QFont::DemiBold);
    const QFontMetrics metrics(font);
    const int pad = std::max(2, pixelSize / 3);
    const QSize size(std::max(1, metrics.horizontalAdvance(text) + 2 * pad), std::max(1, metrics.height() + pad));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(image.rect(), QColor(0, 0, 0, 153));
    painter.setPen(Qt::white);
    painter.setFont(font);
    painter.drawText(image.rect(), Qt::AlignCenter, text);
    painter.end();
    return image;
}

int textPixelSize(int frameHeight, double sizePercent) {
    return std::max(6, static_cast<int>(std::lround(frameHeight * 0.045 * sizePercent / 100.0)));
}

} // namespace

FrameEffects::~FrameEffects() = default;

std::expected<std::shared_ptr<FrameEffects>, Error> FrameEffects::create(const ExportEffects& fx, const Context& ctx) {
    auto effects = std::shared_ptr<FrameEffects>(new FrameEffects());
    effects->m_fx = fx;
    effects->m_ctx = ctx;
    // PQ / HLG: overlay white at reference white (203 nits: PQ 0.58, HLG 0.75), not at the format's peak.
    effects->m_overlayGain = ctx.dynamicRange == DynamicRange::Pq ? 0.58 : ctx.dynamicRange == DynamicRange::Hlg ? 0.75 : 1.0;
    if ((fx.name.applied || fx.timecode.applied) && qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Name and Timecode Overlays need fonts (a GUI application)"});
    }
    if (fx.look.applied) {
        std::shared_ptr<const color::Lut> lut;
        if (fx.look.lut.starts_with(color::kBuiltinPrefix)) {
            lut = color::builtinLut(fx.look.lut);
            if (!lut) {
                return std::unexpected(Error{ErrorCode::NotFound, "Lumetri Look/LUT: no built-in look " + fx.look.lut});
            }
        } else {
            auto loaded = color::loadCube(resolvePath(fx.look.lut, ctx.projectFile));
            if (!loaded) {
                return std::unexpected(Error{loaded.error().code == ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::InvalidArgument,
                                             "Lumetri Look/LUT: " + loaded.error().message});
            }
            lut = std::make_shared<const color::Lut>(std::move(*loaded));
        }
        effects->m_lut.resize(static_cast<std::size_t>(kLutSize * kLutSize * kLutSize * 3));
        for (int b = 0; b < kLutSize; ++b) {
            for (int g = 0; g < kLutSize; ++g) {
                for (int r = 0; r < kLutSize; ++r) {
                    const color::Rgb out = color::applyLut(
                        *lut, {r / double(kLutSize - 1), g / double(kLutSize - 1), b / double(kLutSize - 1)});
                    const std::size_t i = static_cast<std::size_t>((r + kLutSize * (g + kLutSize * b)) * 3);
                    for (std::size_t c = 0; c < 3; ++c) {
                        effects->m_lut[i + c] = static_cast<float>(out[c]);
                    }
                }
            }
        }
    }
    if (fx.image.applied) {
        const std::filesystem::path file = resolvePath(fx.image.image, ctx.projectFile);
        QImage image(QString::fromStdU16String(file.u16string()));
        if (image.isNull()) {
            return std::unexpected(Error{ErrorCode::NotFound, "Image Overlay: cannot read " + pathToUtf8(file)});
        }
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const double scale = fx.image.placement.size / 100.0;
        if (std::abs(scale - 1.0) > 1e-6) {
            image = image.scaled(std::max(1, static_cast<int>(std::lround(image.width() * scale))),
                                 std::max(1, static_cast<int>(std::lround(image.height() * scale))), Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
        }
        auto layer = std::make_unique<Layer>();
        const auto [x, y] = place(fx.image.placement, image.width(), image.height(), ctx.width, ctx.height);
        layer->image = std::move(image);
        layer->x = x;
        layer->y = y;
        layer->opacity = fx.image.placement.opacity / 100.0;
        effects->m_image = std::move(layer);
    }
    if (fx.name.applied) {
        std::string name;
        switch (fx.name.format) {
        case NameOverlayFormat::SourceName:
            name = ctx.sequenceName;
            break;
        case NameOverlayFormat::OutputName:
            name = pathToUtf8(ctx.output.stem());
            break;
        case NameOverlayFormat::OutputNameWithExtension:
            name = pathToUtf8(ctx.output.filename());
            break;
        }
        const QString text = QString::fromStdString(fx.name.prefix + name + fx.name.suffix);
        QImage image = textImage(text, textPixelSize(ctx.height, fx.name.placement.size), false);
        auto layer = std::make_unique<Layer>();
        const auto [x, y] = place(fx.name.placement, image.width(), image.height(), ctx.width, ctx.height);
        layer->image = std::move(image);
        layer->x = x;
        layer->y = y;
        layer->opacity = fx.name.placement.opacity / 100.0;
        effects->m_name = std::move(layer);
    }
    return effects;
}

std::string FrameEffects::timecodeText(std::int64_t frameIndex) const {
    if (!m_fx.timecode.applied) {
        return {};
    }
    const Rate rate = m_ctx.frameRate;
    const Ticks first = m_fx.timecode.source == TimecodeSource::Generate ? m_fx.timecode.startTimecode
                                                                         : m_ctx.sequenceStartTimecode + m_ctx.rangeStart;
    const std::int64_t firstFrame = ticksToFrames(first, rate, Rounding::Nearest).value_or(0);
    const std::int64_t frame = firstFrame + frameIndex;
    if (m_fx.timecode.display == TimeDisplay::Frames) {
        return std::to_string(frame);
    }
    return framesToTimecode(frame, rate, m_ctx.dropFrame);
}

void FrameEffects::apply(AVFrame* frame, std::int64_t frameIndex) const {
    if (!m_lut.empty()) {
        applyLook(frame);
    }
    if (m_fx.limiter.applied) {
        applyLimiter(frame);
    }
    if (m_image) {
        composite(frame, *m_image);
    }
    if (m_name) {
        composite(frame, *m_name);
    }
    if (m_fx.timecode.applied) {
        Layer layer;
        layer.image = textImage(QString::fromStdString(timecodeText(frameIndex)),
                                textPixelSize(m_ctx.height, m_fx.timecode.placement.size), true);
        const auto [x, y] = place(m_fx.timecode.placement, layer.image.width(), layer.image.height(), m_ctx.width, m_ctx.height);
        layer.x = x;
        layer.y = y;
        layer.opacity = m_fx.timecode.placement.opacity / 100.0;
        composite(frame, layer);
    }
}

void FrameEffects::applyLook(AVFrame* frame) const {
    const int w = frame->width;
    constexpr int n = kLutSize - 1;
    const auto at = [&](int r, int g, int b) {
        return &m_lut[static_cast<std::size_t>((r + kLutSize * (g + kLutSize * b)) * 3)];
    };
    parallelRows(frame->height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                float in[3];
                int i0[3];
                float t[3];
                for (int c = 0; c < 3; ++c) {
                    in[c] = load(frame, x, y, c) * n;
                    i0[c] = std::min(static_cast<int>(in[c]), n - 1);
                    t[c] = in[c] - static_cast<float>(i0[c]);
                }
                // Trilinear between the 8 lattice points around the colour.
                for (int c = 0; c < 3; ++c) {
                    const float c000 = at(i0[0], i0[1], i0[2])[c], c100 = at(i0[0] + 1, i0[1], i0[2])[c];
                    const float c010 = at(i0[0], i0[1] + 1, i0[2])[c], c110 = at(i0[0] + 1, i0[1] + 1, i0[2])[c];
                    const float c001 = at(i0[0], i0[1], i0[2] + 1)[c], c101 = at(i0[0] + 1, i0[1], i0[2] + 1)[c];
                    const float c011 = at(i0[0], i0[1] + 1, i0[2] + 1)[c], c111 = at(i0[0] + 1, i0[1] + 1, i0[2] + 1)[c];
                    const float c00 = c000 + (c100 - c000) * t[0], c10 = c010 + (c110 - c010) * t[0];
                    const float c01 = c001 + (c101 - c001) * t[0], c11 = c011 + (c111 - c011) * t[0];
                    const float c0 = c00 + (c10 - c00) * t[1], c1 = c01 + (c11 - c01) * t[1];
                    store(frame, x, y, c, c0 + (c1 - c0) * t[2]);
                }
            }
        }
    });
}

void FrameEffects::applyLimiter(AVFrame* frame) const {
    // R'G'B' at or under the clip level (100 IRE = code value 1.0), a soft knee from `compression` % under it; scaling
    // the three channels together keeps the hue, and keeps luma and chroma under the level too.
    const float clip = static_cast<float>(std::min(m_fx.limiter.clipLevel, 109.0) / 100.0);
    const float knee = clip * static_cast<float>(1.0 - m_fx.limiter.compression / 100.0);
    const float span = std::max(clip - knee, 1e-6f);
    const int w = frame->width;
    parallelRows(frame->height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const float r = load(frame, x, y, 0), g = load(frame, x, y, 1), b = load(frame, x, y, 2);
                const float m = std::max({r, g, b});
                if (m <= knee) {
                    continue;
                }
                const float limited = knee + span * std::tanh((m - knee) / span);
                const float s = limited / m;
                store(frame, x, y, 0, r * s);
                store(frame, x, y, 1, g * s);
                store(frame, x, y, 2, b * s);
            }
        }
    });
}

void FrameEffects::composite(AVFrame* frame, const Layer& layer) const {
    const int x0 = std::max(0, layer.x);
    const int y0 = std::max(0, layer.y);
    const int x1 = std::min(frame->width, layer.x + layer.image.width());
    const int y1 = std::min(frame->height, layer.y + layer.image.height());
    const float opacity = static_cast<float>(std::clamp(layer.opacity, 0.0, 1.0));
    const float gain = static_cast<float>(m_overlayGain);
    for (int y = y0; y < y1; ++y) {
        const auto* src = reinterpret_cast<const QRgb*>(layer.image.constScanLine(y - layer.y));
        for (int x = x0; x < x1; ++x) {
            const QRgb p = src[x - layer.x];
            const float a = static_cast<float>(qAlpha(p)) / 255.0f * opacity;
            if (a <= 0.0f) {
                continue;
            }
            const float premul[3] = {static_cast<float>(qRed(p)) / 255.0f * opacity * gain,
                                     static_cast<float>(qGreen(p)) / 255.0f * opacity * gain,
                                     static_cast<float>(qBlue(p)) / 255.0f * opacity * gain};
            const float da = load(frame, x, y, 3);
            const float outA = a + da * (1.0f - a);
            for (int c = 0; c < 3; ++c) {
                const float dc = load(frame, x, y, c);
                store(frame, x, y, c, outA > 0.0f ? (premul[c] + dc * da * (1.0f - a)) / outA : 0.0f);
            }
            store(frame, x, y, 3, outA);
        }
    }
}

} // namespace wabcut::exporter::detail
