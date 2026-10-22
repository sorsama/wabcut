// Pictures of Bars and Tone and the Universal Counting Leader (P.2, SyntheticPicture.h).

#include <wabcut/render/SyntheticPicture.h>

#include <wabcut/model/Synthetic.h>

#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <numbers>
#include <string>

namespace wabcut::render {

namespace {

std::uint8_t byteOf(double encoded) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
}

/// The bars: every pixel takes the patch containing its centre (ramps: the colour at its centre).
std::vector<std::uint8_t> drawBars(int pattern, int w, int h) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4, 0);
    const std::vector<BarsPatch> patches = barsPattern(pattern);
    for (int y = 0; y < h; ++y) {
        const double fy = (y + 0.5) / h;
        for (int x = 0; x < w; ++x) {
            const double fx = (x + 0.5) / w;
            std::array<double, 3> c{0.0, 0.0, 0.0};
            for (const BarsPatch& p : patches) {
                if (fx >= p.x0 && fx < p.x1 && fy >= p.y0 && fy < p.y1) {
                    c = p.rgb;
                    if (p.ramp && p.x1 > p.x0) {
                        const double t = (fx - p.x0) / (p.x1 - p.x0);
                        for (std::size_t k = 0; k < 3; ++k) {
                            c[k] = p.rgb[k] + (p.rgbEnd[k] - p.rgb[k]) * t;
                        }
                    }
                    break;
                }
            }
            std::uint8_t* px = rgba.data() + 4 * (static_cast<std::size_t>(y) * w + x);
            px[0] = byteOf(c[0]);
            px[1] = byteOf(c[1]);
            px[2] = byteOf(c[2]);
            px[3] = 255;
        }
    }
    return rgba;
}

QColor colourOf(const MediaItem& item, const char* prefix) {
    const std::string p(prefix);
    return QColor(byteOf(syntheticParam(item, p + ".r")), byteOf(syntheticParam(item, p + ".g")),
                  byteOf(syntheticParam(item, p + ".b")));
}

/// The leader: the background, the clock wipe swept clockwise from 12 o'clock, the cross-hair, two target circles and
/// the number; black for 0.
std::vector<std::uint8_t> drawLeader(const MediaItem& item, const LeaderFrame& f, int w, int h) {
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::black);
    if (f.number > 0) {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(QRectF(0, 0, w, h), colourOf(item, "background"));
        const QPointF centre(w / 2.0, h / 2.0);
        const double radius = std::hypot(w, h);
        if (f.sweep > 0.0) {
            // Qt angles: counter-clockwise from 3 o'clock, in 1/16 degree. 12 o'clock is 90; clockwise is negative.
            QPainterPath wipe;
            wipe.moveTo(centre);
            wipe.arcTo(QRectF(centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius), 90.0, -360.0 * f.sweep);
            wipe.closeSubpath();
            p.fillPath(wipe, colourOf(item, "wipe"));
        }
        const double line = std::max(1.0, h / 220.0);
        p.setPen(QPen(colourOf(item, "line"), line));
        p.drawLine(QPointF(0, centre.y()), QPointF(w, centre.y()));
        p.drawLine(QPointF(centre.x(), 0), QPointF(centre.x(), h));
        p.setPen(QPen(colourOf(item, "target"), std::max(1.0, h / 120.0)));
        p.setBrush(Qt::NoBrush);
        for (const double r : {0.30 * h, 0.36 * h}) {
            p.drawEllipse(centre, r, r);
        }
        QFont font(QStringLiteral("Arial"));
        font.setBold(true);
        font.setPixelSize(std::max(8, static_cast<int>(std::lround(0.42 * h))));
        p.setFont(font);
        p.setPen(colourOf(item, "numeral"));
        p.drawText(QRectF(0, 0, w, h), Qt::AlignCenter, QString::number(f.number));
    }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        std::memcpy(rgba.data() + static_cast<std::size_t>(y) * w * 4, image.constScanLine(y), static_cast<std::size_t>(w) * 4);
    }
    return rgba;
}

} // namespace

std::shared_ptr<const TitleRaster> syntheticPicture(const MediaItem& item, Ticks time, CanvasSize size) {
    const SyntheticDescriptor* d = syntheticOf(item);
    if (!d || (d->kind != SyntheticKind::BarsAndTone && d->kind != SyntheticKind::CountingLeader) || !size.isValid() ||
        size.width > 16384 || size.height > 16384) {
        return nullptr;
    }
    // What decides the picture: the kind, its settings, the frame (the leader's number and sweep) and the size.
    std::string key = std::string(d->generator) + "#" + std::to_string(size.width) + "x" + std::to_string(size.height);
    for (const EffectParam& p : item.generator->params) {
        key += "#" + p.key + "=" + std::to_string(p.value.isConstant() ? p.value.constant() : 0.0);
    }
    LeaderFrame leader;
    if (d->kind == SyntheticKind::CountingLeader) {
        leader = countingLeaderAt(item, time);
        key += "#" + std::to_string(leader.number) + "@" + std::to_string(std::lround(leader.sweep * 4096.0));
    }
    static std::mutex mutex;
    static std::list<std::pair<std::string, std::shared_ptr<const TitleRaster>>> recent;
    {
        std::lock_guard lock(mutex);
        for (auto it = recent.begin(); it != recent.end(); ++it) {
            if (it->first == key) {
                recent.splice(recent.begin(), recent, it);
                return recent.front().second;
            }
        }
    }
    std::vector<std::uint8_t> rgba = d->kind == SyntheticKind::BarsAndTone
                                         ? drawBars(static_cast<int>(std::lround(syntheticParam(item, "pattern"))), size.width, size.height)
                                         : drawLeader(item, leader, size.width, size.height);
    auto raster = std::make_shared<const TitleRaster>(size.width, size.height, std::move(rgba), TitleLayoutInfo{});
    std::lock_guard lock(mutex);
    recent.emplace_front(key, raster);
    if (recent.size() > 6) {
        recent.pop_back();
    }
    return raster;
}

} // namespace wabcut::render
