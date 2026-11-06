#include "SpectrogramView.h"

#include "SpectralRepairBridge.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QQmlEngine>
#include <QThreadPool>
#include <QQuickWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace wabcut::app {

namespace dsp = wabcut::audio::dsp;

namespace {

struct Stop {
    double at;
    int r, g, b;
};

/// A colour map of 256 steps from quiet to loud, for a dark or a light surface: the quiet end is (nearly) the surface's own
/// tone, so silence disappears into the panel and the loud parts stand out in both themes.
std::array<QRgb, 256> colourMap(bool dark) {
    static const std::array<Stop, 7> darkStops{{{0.00, 10, 9, 16},
                                                {0.18, 38, 18, 77},
                                                {0.36, 92, 25, 112},
                                                {0.54, 150, 38, 110},
                                                {0.70, 209, 70, 76},
                                                {0.84, 245, 134, 38},
                                                {1.00, 252, 236, 150}}};
    static const std::array<Stop, 7> lightStops{{{0.00, 247, 246, 251},
                                                 {0.18, 226, 216, 248},
                                                 {0.36, 190, 165, 240},
                                                 {0.54, 146, 110, 226},
                                                 {0.72, 105, 62, 200},
                                                 {0.88, 62, 28, 140},
                                                 {1.00, 28, 10, 70}}};
    const auto& stops = dark ? darkStops : lightStops;
    std::array<QRgb, 256> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double t = static_cast<double>(i) / 255.0;
        std::size_t k = 0;
        while (k + 2 < stops.size() && t > stops[k + 1].at) {
            ++k;
        }
        const double u = std::clamp((t - stops[k].at) / (stops[k + 1].at - stops[k].at), 0.0, 1.0);
        const auto mixed = [&](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * u)); };
        out[i] = qRgb(mixed(stops[k].r, stops[k + 1].r), mixed(stops[k].g, stops[k + 1].g), mixed(stops[k].b, stops[k + 1].b));
    }
    return out;
}

struct Row {
    int b0 = 0;
    int b1 = 0;
    float frac = 0.0f;
    bool interpolate = false;
};

} // namespace

struct SpectrogramView::Request {
    std::shared_ptr<dsp::SpectrogramCache> cache;
    int width = 0;
    int height = 0;
    double dpr = 1.0;
    double viewStart = 0.0;
    double viewSpan = 1.0;
    double minHz = 20.0;
    double maxHz = 24000.0;
    bool log = true;
    bool dark = true;
    double floorDb = -100.0;
    double ceilingDb = -12.0;
    QRgb background = 0;
};

namespace {

double hzFor(double u, double minHz, double maxHz, bool log) {
    return log ? minHz * std::pow(maxHz / minHz, u) : minHz + u * (maxHz - minHz);
}

} // namespace

void SpectrogramView::render(Request rq, std::stop_token stop, quint64 generation, QPointer<SpectrogramView> self) {
    dsp::SpectrogramCache& cache = *rq.cache;
    const std::array<QRgb, 256> lut = colourMap(rq.dark);
    std::array<std::uint8_t, 256> windowed{};
    for (int q = 0; q < 256; ++q) {
        const double db = dsp::spectrogramDb(static_cast<std::uint8_t>(q));
        const double u = std::clamp((db - rq.floorDb) / std::max(rq.ceilingDb - rq.floorDb, 1.0), 0.0, 1.0);
        windowed[static_cast<std::size_t>(q)] = static_cast<std::uint8_t>(std::lround(u * 255.0));
    }
    const double columnSeconds = cache.columnSeconds();
    const int level = std::min(dsp::SpectrogramCache::levelFor(rq.viewSpan / rq.width / columnSeconds), cache.maxLevel());
    const double levelSeconds = columnSeconds * static_cast<double>(std::int64_t{1} << level);
    const std::int64_t totalColumns = cache.columns(level);
    const double binHz = static_cast<double>(cache.sampleRate()) / cache.fftSize();
    const int bins = cache.bins();

    std::vector<Row> rows(static_cast<std::size_t>(rq.height));
    for (int y = 0; y < rq.height; ++y) {
        const double hzLow = hzFor(1.0 - static_cast<double>(y + 1) / rq.height, rq.minHz, rq.maxHz, rq.log);
        const double hzHigh = hzFor(1.0 - static_cast<double>(y) / rq.height, rq.minHz, rq.maxHz, rq.log);
        const double lo = hzLow / binHz;
        const double hi = hzHigh / binHz;
        Row& row = rows[static_cast<std::size_t>(y)];
        if (hi - lo < 1.0) {
            const double centre = 0.5 * (lo + hi);
            row.b0 = std::clamp(static_cast<int>(std::floor(centre)), 0, bins - 1);
            row.b1 = std::min(row.b0 + 1, bins - 1);
            row.frac = static_cast<float>(centre - std::floor(centre));
            row.interpolate = true;
        } else {
            row.b0 = std::clamp(static_cast<int>(std::floor(lo)), 0, bins - 1);
            row.b1 = std::clamp(static_cast<int>(std::floor(hi)), row.b0, bins - 1);
        }
    }

    // The columns the view shows, the pixel each belongs to, and the tiles they are in.
    std::vector<std::int64_t> columnOf(static_cast<std::size_t>(rq.width), -1);
    std::int64_t firstTile = std::numeric_limits<std::int64_t>::max();
    std::int64_t lastTile = -1;
    for (int x = 0; x < rq.width; ++x) {
        const double t = rq.viewStart + (x + 0.5) / rq.width * rq.viewSpan;
        const auto c = static_cast<std::int64_t>(std::floor((t + 0.5 * columnSeconds) / levelSeconds));
        if (c >= 0 && c < totalColumns) {
            columnOf[static_cast<std::size_t>(x)] = c;
            firstTile = std::min(firstTile, c / dsp::kSpectrogramTileColumns);
            lastTile = std::max(lastTile, c / dsp::kSpectrogramTileColumns);
        }
    }
    const std::int64_t tileCount = lastTile >= firstTile ? lastTile - firstTile + 1 : 0;
    std::vector<std::shared_ptr<const dsp::SpectrogramTile>> tiles(static_cast<std::size_t>(tileCount));
    for (std::int64_t i = 0; i < tileCount; ++i) {
        tiles[static_cast<std::size_t>(i)] = cache.peek(level, firstTile + i);
    }

    const auto compose = [&]() {
        QImage image(rq.width, rq.height, QImage::Format_RGB32);
        std::vector<QRgb> column(static_cast<std::size_t>(rq.height));
        std::int64_t previous = -2;
        for (int x = 0; x < rq.width; ++x) {
            const std::int64_t c = columnOf[static_cast<std::size_t>(x)];
            if (c != previous) {
                previous = c;
                const dsp::SpectrogramTile* tile =
                    c >= 0 ? tiles[static_cast<std::size_t>(c / dsp::kSpectrogramTileColumns - firstTile)].get() : nullptr;
                if (tile == nullptr) {
                    std::fill(column.begin(), column.end(), rq.background);
                } else {
                    const std::uint8_t* levels = tile->column(static_cast<int>(c % dsp::kSpectrogramTileColumns));
                    for (int y = 0; y < rq.height; ++y) {
                        const Row& row = rows[static_cast<std::size_t>(y)];
                        int q = 0;
                        if (row.interpolate) {
                            q = static_cast<int>(std::lround(levels[row.b0] * (1.0f - row.frac) + levels[row.b1] * row.frac));
                        } else {
                            for (int b = row.b0; b <= row.b1; ++b) {
                                q = std::max<int>(q, levels[b]);
                            }
                        }
                        column[static_cast<std::size_t>(y)] = lut[windowed[static_cast<std::size_t>(std::clamp(q, 0, 255))]];
                    }
                }
            }
            for (int y = 0; y < rq.height; ++y) {
                reinterpret_cast<QRgb*>(image.scanLine(y))[x] = column[static_cast<std::size_t>(y)];
            }
        }
        image.setDevicePixelRatio(rq.dpr);
        return image;
    };

    const auto publish = [&](int done) {
        QImage image = compose();
        const int total = static_cast<int>(tileCount);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, image = std::move(image), generation, start = rq.viewStart, span = rq.viewSpan, done, total]() {
                if (self) {
                    self->applyImage(image, generation, start, span, done, total);
                }
            },
            Qt::QueuedConnection);
    };

    int done = 0;
    for (const auto& t : tiles) {
        done += t ? 1 : 0;
    }
    if (stop.stop_requested()) {
        return;
    }
    publish(done);
    if (done == static_cast<int>(tileCount)) {
        return;
    }
    // Compute the tiles that are missing, nearest the middle of the view first; the picture grows as they arrive.
    std::vector<std::int64_t> missing;
    for (std::int64_t i = 0; i < tileCount; ++i) {
        if (!tiles[static_cast<std::size_t>(i)]) {
            missing.push_back(i);
        }
    }
    const double middle = (rq.viewStart + 0.5 * rq.viewSpan + 0.5 * columnSeconds) / levelSeconds / dsp::kSpectrogramTileColumns - static_cast<double>(firstTile);
    std::ranges::sort(missing, {}, [&](std::int64_t i) { return std::fabs(static_cast<double>(i) + 0.5 - middle); });
    for (const std::int64_t i : missing) {
        if (stop.stop_requested()) {
            return;
        }
        auto tile = cache.tile(level, firstTile + i, stop);
        if (!tile) {
            return; // cancelled
        }
        tiles[static_cast<std::size_t>(i)] = std::move(tile);
        ++done;
        publish(done);
    }
}

SpectrogramView::SpectrogramView(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(30);
    connect(&m_debounce, &QTimer::timeout, this, &SpectrogramView::startRender);
}

SpectrogramView::~SpectrogramView() { m_stop.request_stop(); }

void SpectrogramView::setDark(bool dark) {
    if (dark == m_dark) {
        return;
    }
    m_dark = dark;
    emit styleChanged();
    request();
}

void SpectrogramView::setSelectionColor(const QColor& c) {
    if (c == m_selectionColor) {
        return;
    }
    m_selectionColor = c;
    emit styleChanged();
    update();
}

void SpectrogramView::setEditColor(const QColor& c) {
    if (c == m_editColor) {
        return;
    }
    m_editColor = c;
    emit styleChanged();
    update();
}

void SpectrogramView::setPlayheadColor(const QColor& c) {
    if (c == m_playheadColor) {
        return;
    }
    m_playheadColor = c;
    emit styleChanged();
    update();
}

void SpectrogramView::setBackgroundColor(const QColor& c) {
    if (c == m_background) {
        return;
    }
    m_background = c;
    emit styleChanged();
    update();
}

void SpectrogramView::setSelectionShape(const QString& shape) {
    if (shape == m_shape) {
        return;
    }
    m_shape = shape;
    emit selectionChanged();
    update();
}

void SpectrogramView::setSelectionPoints(const QVariantList& points) {
    m_points = points;
    emit selectionChanged();
    update();
}

void SpectrogramView::setBrushRadiusSeconds(double seconds) {
    if (seconds == m_radiusSeconds) {
        return;
    }
    m_radiusSeconds = seconds;
    emit selectionChanged();
    update();
}

void SpectrogramView::setBrushRadiusOctaves(double octaves) {
    if (octaves == m_radiusOctaves) {
        return;
    }
    m_radiusOctaves = octaves;
    emit selectionChanged();
    update();
}

double SpectrogramView::minHz() const { return logFrequency() ? 20.0 : 0.0; }

double SpectrogramView::maxHz() const {
    const double nyquist = m_bridge ? m_bridge->sampleRate() / 2.0 : 24000.0;
    return std::max(nyquist, 1000.0);
}

bool SpectrogramView::logFrequency() const { return m_bridge ? m_bridge->logFrequency() : true; }

double SpectrogramView::timeAt(double x) const {
    const double span = m_bridge ? m_bridge->viewSpan() : 1.0;
    const double start = m_bridge ? m_bridge->viewStart() : 0.0;
    return start + x / std::max(width(), 1.0) * span;
}

double SpectrogramView::xAt(double seconds) const {
    const double span = m_bridge ? m_bridge->viewSpan() : 1.0;
    const double start = m_bridge ? m_bridge->viewStart() : 0.0;
    return (seconds - start) / std::max(span, 1.0e-6) * width();
}

double SpectrogramView::hzAt(double y) const {
    const double u = 1.0 - std::clamp(y / std::max(height(), 1.0), 0.0, 1.0);
    return hzFor(u, minHz(), maxHz(), logFrequency());
}

double SpectrogramView::yAt(double hz) const {
    const double lo = minHz();
    const double hi = maxHz();
    const double clamped = std::clamp(hz, std::max(lo, 1.0e-3), hi);
    const double u = logFrequency() ? std::log(clamped / lo) / std::log(hi / lo) : (clamped - lo) / (hi - lo);
    return (1.0 - u) * height();
}

double SpectrogramView::levelAt(double x, double y) const {
    if (!m_bridge || !m_cache) {
        return -1000.0;
    }
    const double seconds = timeAt(x);
    const double hz = hzAt(y);
    const double columnSeconds = m_cache->columnSeconds();
    const int level = std::min(dsp::SpectrogramCache::levelFor(m_bridge->viewSpan() / std::max(width(), 1.0) / columnSeconds), m_cache->maxLevel());
    const double levelSeconds = columnSeconds * static_cast<double>(std::int64_t{1} << level);
    const auto c = static_cast<std::int64_t>(std::floor((seconds + 0.5 * columnSeconds) / levelSeconds));
    if (c < 0 || c >= m_cache->columns(level)) {
        return -1000.0;
    }
    const auto tile = m_cache->peek(level, c / dsp::kSpectrogramTileColumns);
    if (!tile) {
        return -1000.0;
    }
    const double binHz = static_cast<double>(m_cache->sampleRate()) / m_cache->fftSize();
    const int bin = std::clamp(static_cast<int>(std::lround(hz / binHz)), 0, m_cache->bins() - 1);
    return dsp::spectrogramDb(tile->at(static_cast<int>(c % dsp::kSpectrogramTileColumns), bin));
}

void SpectrogramView::componentComplete() {
    QQuickPaintedItem::componentComplete();
    QQmlEngine* engine = qmlEngine(this);
    if (engine) {
        m_bridge = engine->singletonInstance<SpectralRepairBridge*>("Wabcut", "SpectralRepair");
    }
    if (m_bridge) {
        connect(m_bridge, &SpectralRepairBridge::viewChanged, this, [this] {
            emit axisChanged();
            request();
            update();
        });
        connect(m_bridge, &SpectralRepairBridge::analysisChanged, this, &SpectrogramView::request);
        connect(m_bridge, &SpectralRepairBridge::clipChanged, this, [this] {
            emit axisChanged();
            request();
        });
        connect(m_bridge, &SpectralRepairBridge::editsChanged, this, [this] { update(); });
        connect(m_bridge, &SpectralRepairBridge::playheadChanged, this, [this] { update(); });
    }
    request();
}

void SpectrogramView::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        request();
    }
}

void SpectrogramView::request() {
    if (!isComponentComplete()) {
        return;
    }
    m_ready = false;
    m_debounce.start();
}

void SpectrogramView::refresh() {
    m_debounce.stop();
    startRender();
}

void SpectrogramView::startRender() {
    m_stop.request_stop();
    m_stop = std::stop_source{};
    ++m_generation;
    const bool hadImage = !m_image.isNull();
    if (!m_bridge || width() < 2.0 || height() < 2.0) {
        m_image = QImage();
        m_ready = false;
        m_analysing = false;
        if (hadImage) {
            emit progressChanged();
        }
        update();
        return;
    }
    m_cache = m_bridge->displayCache();
    if (!m_cache) {
        m_image = QImage();
        m_ready = false;
        m_analysing = false;
        if (hadImage) {
            emit progressChanged();
        }
        update();
        return;
    }
    const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    Request rq;
    rq.cache = m_cache;
    rq.width = std::max(2, static_cast<int>(std::lround(width() * dpr)));
    rq.height = std::max(2, static_cast<int>(std::lround(height() * dpr)));
    rq.dpr = dpr;
    rq.viewStart = m_bridge->viewStart();
    rq.viewSpan = m_bridge->viewSpan();
    rq.minHz = minHz() > 0.0 ? minHz() : 0.0;
    rq.maxHz = maxHz();
    rq.log = logFrequency();
    rq.dark = m_dark;
    rq.floorDb = m_bridge->floorDb();
    rq.ceilingDb = m_bridge->ceilingDb();
    rq.background = m_background.rgb();
    m_analysing = true;
    m_ready = false;
    emit progressChanged();
    QPointer<SpectrogramView> self(this);
    const std::stop_token token = m_stop.get_token();
    const quint64 generation = m_generation;
    QThreadPool::globalInstance()->start([rq = std::move(rq), token, generation, self]() mutable { render(std::move(rq), token, generation, self); });
}

void SpectrogramView::applyImage(const QImage& image, quint64 generation, double start, double span, int done, int total) {
    if (generation != m_generation) {
        return;
    }
    m_image = image;
    m_imageStart = start;
    m_imageSpan = span;
    m_progress = total > 0 ? static_cast<double>(done) / total : 1.0;
    m_ready = done >= total;
    m_analysing = !m_ready;
    update();
    emit progressChanged();
}

SpectralEdit SpectrogramView::draftEdit() const {
    SpectralEdit e;
    e.radiusSeconds = std::max(m_radiusSeconds, 1.0e-3);
    e.radiusOctaves = std::max(m_radiusOctaves, 1.0e-2);
    for (const QVariant& v : m_points) {
        const QVariantMap p = v.toMap();
        e.path.push_back({p.value(QStringLiteral("time")).toDouble(), std::max(p.value(QStringLiteral("hz")).toDouble(), 1.0)});
    }
    if (m_shape == QLatin1String("lasso")) {
        e.shape = SpectralShape::Lasso;
    } else if (m_shape == QLatin1String("brush")) {
        e.shape = SpectralShape::Brush;
    } else if (m_shape == QLatin1String("tone")) {
        e.kind = SpectralKind::Tone;
        e.shape = SpectralShape::Rectangle;
        e.harmonics = 1;
        e.widthHz = 12.0;
    } else {
        e.shape = SpectralShape::Rectangle;
        if (e.path.size() >= 2) {
            e.start = std::min(e.path[0].time, e.path[1].time);
            e.end = std::max(e.path[0].time, e.path[1].time);
            e.low = std::min(e.path[0].hz, e.path[1].hz);
            e.high = std::max(e.path[0].hz, e.path[1].hz);
        }
    }
    return e;
}

void SpectrogramView::drawEdit(QPainter& p, const SpectralEdit& e, bool draft) const {
    QColor fill = draft ? m_selectionColor : m_editColor;
    fill.setAlphaF(draft ? 0.22f : 0.16f);
    QColor line = draft ? m_selectionColor : m_editColor;
    line.setAlphaF(draft ? 1.0f : 0.85f);
    QPen pen(line, draft ? 1.6 : 1.2, draft ? Qt::DashLine : Qt::SolidLine);
    p.setPen(pen);
    p.setBrush(fill);
    if (e.kind == SpectralKind::Tone) {
        if (e.path.empty()) {
            return;
        }
        const double bandPx = std::max(3.0, std::fabs(yAt(e.path.front().hz + e.widthHz * 0.5) - yAt(e.path.front().hz - e.widthHz * 0.5)));
        for (int h = 1; h <= std::max(e.harmonics, 1); ++h) {
            QPainterPath path;
            if (e.path.size() == 1) {
                const double y = yAt(e.path.front().hz * h);
                const double x0 = draft ? xAt(e.path.front().time) : xAt(e.start);
                const double x1 = draft ? width() : xAt(e.end);
                path.moveTo(std::min(x0, x1), y);
                path.lineTo(std::max(x0, x1), y);
            } else {
                for (std::size_t i = 0; i < e.path.size(); ++i) {
                    const QPointF pt(xAt(e.path[i].time), yAt(e.path[i].hz * h));
                    if (i == 0) {
                        path.moveTo(pt);
                    } else {
                        path.lineTo(pt);
                    }
                }
            }
            QPen band(fill, bandPx, Qt::SolidLine, Qt::FlatCap);
            band.setColor(QColor(fill.red(), fill.green(), fill.blue(), draft ? 90 : 60));
            p.setPen(band);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            p.setPen(pen);
            p.drawPath(path);
        }
        return;
    }
    switch (e.shape) {
    case SpectralShape::Rectangle: {
        if (e.end <= e.start && e.high <= e.low) {
            return;
        }
        const QRectF r(QPointF(xAt(e.start), yAt(e.high)), QPointF(xAt(e.end), yAt(e.low)));
        p.drawRect(r.normalized());
        break;
    }
    case SpectralShape::Lasso: {
        if (e.path.size() < 2) {
            return;
        }
        QPolygonF poly;
        for (const SpectralPoint& pt : e.path) {
            poly << QPointF(xAt(pt.time), yAt(pt.hz));
        }
        if (e.path.size() > 2) {
            p.drawPolygon(poly);
        } else {
            p.drawPolyline(poly);
        }
        break;
    }
    case SpectralShape::Brush: {
        if (e.path.empty()) {
            return;
        }
        QPainterPath path;
        path.setFillRule(Qt::WindingFill);
        const auto stamp = [&](double t, double hz) {
            const double rx = std::max(2.0, std::fabs(xAt(t + e.radiusSeconds) - xAt(t)));
            const double ry = std::max(2.0, std::fabs(yAt(hz * std::exp2(e.radiusOctaves)) - yAt(hz / std::exp2(e.radiusOctaves))) * 0.5);
            path.addEllipse(QPointF(xAt(t), yAt(hz)), rx, ry);
        };
        for (std::size_t i = 0; i < e.path.size(); ++i) {
            stamp(e.path[i].time, e.path[i].hz);
            if (i + 1 < e.path.size()) {
                const SpectralPoint& a = e.path[i];
                const SpectralPoint& b = e.path[i + 1];
                const double steps = std::clamp(std::hypot(xAt(b.time) - xAt(a.time), yAt(b.hz) - yAt(a.hz)) / 4.0, 1.0, 200.0);
                for (int s = 1; s < static_cast<int>(steps); ++s) {
                    const double u = s / steps;
                    stamp(a.time + (b.time - a.time) * u, a.hz * std::pow(b.hz / a.hz, u));
                }
            }
        }
        p.drawPath(path.simplified());
        break;
    }
    }
}

void SpectrogramView::paint(QPainter* painter) {
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillRect(QRectF(0, 0, width(), height()), m_background);
    if (!m_image.isNull()) {
        // Where the image belongs in the view as it is now: it slides and stretches while the next one is made.
        const double x0 = xAt(m_imageStart);
        const double x1 = xAt(m_imageStart + m_imageSpan);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->drawImage(QRectF(x0, 0, x1 - x0, height()), m_image);
    }
    painter->save();
    painter->setClipRect(QRectF(0, 0, width(), height()));
    if (m_bridge) {
        for (const SpectralEdit& e : m_bridge->editList()) {
            drawEdit(*painter, e, false);
        }
    }
    if (!m_shape.isEmpty() && !m_points.isEmpty()) {
        drawEdit(*painter, draftEdit(), true);
    }
    if (m_bridge && m_bridge->playhead() >= 0.0) {
        const double x = xAt(m_bridge->playhead());
        if (x >= 0.0 && x <= width()) {
            painter->setPen(QPen(m_playheadColor, 1.5));
            painter->drawLine(QPointF(x, 0), QPointF(x, height()));
        }
    }
    painter->restore();
}

} // namespace wabcut::app
