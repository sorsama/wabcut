#include "ScopesBridge.h"

#include <QImage>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::app {

// ---- ScopeHub -------------------------------------------------------------------------------------------------

ScopeHub& ScopeHub::shared() {
    static ScopeHub hub;
    return hub;
}

void ScopeHub::setWanted(bool wanted) {
    const bool was = m_wanted.exchange(wanted, std::memory_order_acq_rel);
    if (wanted && !was) {
        requestFrames(); // a paused picture gets its scopes too
    }
}

color::ScopeSettings ScopeHub::settings() const {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

void ScopeHub::setSettings(const color::ScopeSettings& settings) {
    {
        std::lock_guard lock(m_mutex);
        m_settings = settings;
    }
    if (wanted()) {
        requestFrames();
    }
}

void ScopeHub::deliver(color::ScopeData data) {
    QPointer<QObject> receiver;
    std::function<void()> callback;
    {
        std::lock_guard lock(m_mutex);
        m_latest = std::make_shared<const color::ScopeData>(std::move(data));
        const auto now = std::chrono::steady_clock::now();
        m_times.push_back(now);
        std::erase_if(m_times, [&](const auto& t) { return now - t > std::chrono::seconds(1); });
        receiver = m_receiver;
        callback = m_onDelivered;
    }
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    if (receiver && callback) {
        QMetaObject::invokeMethod(receiver.data(), callback, Qt::QueuedConnection);
    }
}

std::shared_ptr<const color::ScopeData> ScopeHub::latest() const {
    std::lock_guard lock(m_mutex);
    return m_latest;
}

double ScopeHub::rate() const {
    std::lock_guard lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    return static_cast<double>(std::ranges::count_if(m_times, [&](const auto& t) { return now - t <= std::chrono::seconds(1); }));
}

void ScopeHub::setListener(QObject* receiver, std::function<void()> onDelivered) {
    std::lock_guard lock(m_mutex);
    m_receiver = receiver;
    m_onDelivered = std::move(onDelivered);
}

void ScopeHub::addMonitor(QQuickItem* monitor) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_monitors, [](const QPointer<QQuickItem>& m) { return m.isNull(); });
    m_monitors.emplace_back(monitor);
}

void ScopeHub::requestFrames() {
    std::vector<QPointer<QQuickItem>> monitors;
    {
        std::lock_guard lock(m_mutex);
        monitors = m_monitors;
    }
    for (const QPointer<QQuickItem>& m : monitors) {
        if (m) {
            QMetaObject::invokeMethod(m.data(), [item = m] {
                if (item) {
                    item->update();
                }
            }, Qt::QueuedConnection);
        }
    }
}

// ---- ScopesBridge ---------------------------------------------------------------------------------------------

ScopesBridge::ScopesBridge(QObject* parent) : QObject(parent) {
    ScopeHub::shared().setListener(this, [this] { onDelivered(); });
}

ScopesBridge::~ScopesBridge() {
    ScopeHub::shared().setListener(nullptr, nullptr);
    ScopeHub::shared().setWanted(false);
}

ScopesBridge* ScopesBridge::create(QQmlEngine*, QJSEngine*) { return new ScopesBridge; }

void ScopesBridge::setActive(bool active) {
    if (active == m_active) {
        return;
    }
    m_active = active;
    ScopeHub::shared().setWanted(active);
    emit settingsChanged();
}

void ScopesBridge::setWaveformMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    if (mode != m_waveformMode) {
        m_waveformMode = mode;
        emit settingsChanged();
    }
}

void ScopesBridge::setVectorZoom(int zoom) {
    zoom = zoom >= 5 ? 5 : zoom >= 2 ? 2 : 1;
    if (zoom == m_vectorZoom) {
        return;
    }
    m_vectorZoom = zoom;
    color::ScopeSettings s = ScopeHub::shared().settings();
    s.vectorZoom = zoom;
    ScopeHub::shared().setSettings(s);
    emit settingsChanged();
}

void ScopesBridge::onDelivered() {
    const auto data = ScopeHub::shared().latest();
    if (data) {
        const color::ScopeStats s = color::scopeStats(*data);
        const auto r = [](double v) { return std::round(v * 10.0) / 10.0; };
        m_stats = QVariantMap{{QStringLiteral("exposure"), QString::fromStdString(s.exposure)},
                              {QStringLiteral("cast"), QString::fromStdString(s.cast)},
                              {QStringLiteral("median"), r(s.p50)},
                              {QStringLiteral("p1"), r(s.p01)},
                              {QStringLiteral("p99"), r(s.p99)},
                              {QStringLiteral("mean"), r(s.lumaMean)},
                              {QStringLiteral("clipped"), std::round(s.clippedHigh * 1000.0) / 10.0},
                              {QStringLiteral("crushed"), std::round(s.clippedLow * 1000.0) / 10.0},
                              {QStringLiteral("saturation"), r(s.saturationMean)},
                              {QStringLiteral("pixels"), static_cast<qulonglong>(s.pixels)}};
    }
    emit updated();
}

// ---- ScopeView ------------------------------------------------------------------------------------------------

ScopeView::ScopeView(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setOpaquePainting(true);
    setFillColor(m_background);
}

void ScopeView::setBackground(const QColor& c) {
    if (c != m_background) {
        m_background = c;
        setFillColor(c);
        emit colorsChanged();
        update();
    }
}

void ScopeView::setGridColor(const QColor& c) {
    if (c != m_grid) {
        m_grid = c;
        emit colorsChanged();
        update();
    }
}

void ScopeView::setLabelColor(const QColor& c) {
    if (c != m_label) {
        m_label = c;
        emit colorsChanged();
        update();
    }
}

void ScopeView::setSkinColor(const QColor& c) {
    if (c != m_skin) {
        m_skin = c;
        emit colorsChanged();
        update();
    }
}

void ScopeView::setKind(const QString& kind) {
    if (kind != m_kind) {
        m_kind = kind;
        emit kindChanged();
        update();
    }
}

void ScopeView::setGeneration(int generation) {
    if (generation != m_generation) {
        m_generation = generation;
        emit generationChanged();
        update();
    }
}

namespace {

QImage imageOf(const std::vector<std::uint8_t>& rgba, int w, int h) {
    QImage image(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        std::memcpy(image.scanLine(y), rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4,
                    static_cast<std::size_t>(w) * 4);
    }
    return image;
}

/// The image of a scope renderer: `render(width, height)` returns the RGBA8 rows and sets the size, so it runs before
/// the size is read (as call arguments, the order would be unspecified).
template <typename Render>
QImage scopeImage(Render&& render) {
    int w = 0, h = 0;
    const std::vector<std::uint8_t> rgba = render(w, h);
    return imageOf(rgba, w, h);
}

/// The largest power-of-two pooling (at most 8) that still leaves at least one bin per device pixel: a view smaller
/// than the scope sums neighbouring bins instead of letting the downscale filter thin traces away.
int poolFactor(int bins, double pixels) {
    int factor = 1;
    while (factor < 8 && bins % (factor * 2) == 0 && bins / (factor * 2) >= pixels) {
        factor *= 2;
    }
    return factor;
}

} // namespace

void ScopeView::paint(QPainter* p) {
    const QRectF area = boundingRect();
    const auto data = ScopeHub::shared().latest();
    QFont font = p->font();
    font.setPixelSize(10);
    p->setFont(font);
    if (!data || data->pixels == 0) {
        p->setPen(m_label);
        p->drawText(area, Qt::AlignCenter, tr("No picture"));
        return;
    }
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    if (m_kind == QLatin1String("vectorscope")) {
        const double side = std::min(area.width(), area.height()) - 8.0;
        const QRectF r(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
        const color::ScopeData shown = color::pooledScopes(*data, 1, 1, poolFactor(data->settings.vectorSize, side * dpr));
        p->drawImage(r, scopeImage([&](int& w, int& h) { return color::renderVectorscope(shown, w, h); }));
        const double zoom = data->settings.vectorZoom;
        const QPointF centre = r.center();
        const auto at = [&](double cb, double cr) { return centre + QPointF(cb * zoom * side, -cr * zoom * side); };
        p->setPen(m_grid);
        p->drawEllipse(centre, side / 2, side / 2);
        p->drawLine(at(-0.5 / zoom, 0), at(0.5 / zoom, 0));
        p->drawLine(at(0, -0.5 / zoom), at(0, 0.5 / zoom));
        const double a = color::kSkinToneAngle * std::numbers::pi / 180.0;
        p->setPen(m_skin);
        p->drawLine(centre, at(0.5 / zoom * std::cos(a), 0.5 / zoom * std::sin(a)));
        const color::Rgb bars[6] = {{1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 0}};
        const char* names[6] = {"R", "Mg", "B", "Cy", "G", "Yl"};
        for (int i = 0; i < 6; ++i) {
            for (const double amount : {0.75, 1.0}) {
                const auto t = color::vectorscopeTarget(bars[i], amount);
                const QPointF c = at(t[0], t[1]);
                if (!r.adjusted(-6, -6, 6, 6).contains(c)) {
                    continue; // zoomed out of view
                }
                p->setPen(amount < 1.0 ? m_label : m_grid);
                p->drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));
                if (amount < 1.0) {
                    p->drawText(c + QPointF(7, -5), QString::fromLatin1(names[i]));
                }
            }
        }
        p->drawText(r.adjusted(4, 4, 0, 0), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("%1x").arg(zoom));
        return;
    }
    if (m_kind == QLatin1String("histogram")) {
        const QRectF bars = area.adjusted(4, 4, -4, -14);
        const color::ScopeData shown = color::pooledScopes(*data, 1, poolFactor(data->settings.levels, bars.width() * dpr), 1);
        p->drawImage(bars, scopeImage([&](int& w, int& h) { return color::renderHistogram(shown, w, h); }));
        p->setPen(m_grid);
        for (const int pct : {0, 25, 50, 75, 100}) {
            const double x = area.left() + 4 + (area.width() - 8) * pct / 100.0;
            p->drawLine(QPointF(x, area.top() + 4), QPointF(x, area.bottom() - 14));
            p->drawText(QPointF(x - 8, area.bottom() - 2), QString::number(pct));
        }
        return;
    }
    const int mode = m_kind == QLatin1String("parade") ? 1 : m_kind == QLatin1String("ycbcr") ? 2 : 0;
    const QRectF r = area.adjusted(36, 6, -6, -6);
    const int panels = mode == 0 ? 1 : 3;
    const color::ScopeData shown = color::pooledScopes(*data, poolFactor(data->settings.columns, r.width() * dpr / panels),
                                                       poolFactor(data->settings.levels, r.height() * dpr), 1);
    p->drawImage(r, scopeImage([&](int& w, int& h) { return color::renderWaveform(shown, mode, w, h); }));
    // IRE lines with BT.1886 nits (100 nit SDR display).
    for (const int ire : {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100}) {
        const double y = r.bottom() - (r.height() - 1) * ire / 100.0;
        QColor line = m_grid;
        line.setAlphaF(line.alphaF() * (ire % 50 == 0 ? 1.0 : 0.45));
        p->setPen(line);
        p->drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        if (ire % 20 == 0 || ire == 100) {
            p->setPen(m_label);
            p->drawText(QRectF(area.left(), y - 7, 32, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(ire));
        }
    }
    p->setPen(m_label);
    const double nits100 = 100.0;
    p->drawText(QRectF(r.right() - 120, r.top() + 2, 116, 12), Qt::AlignRight, QStringLiteral("IRE · %1 nits at 100").arg(nits100));
    if (mode > 0) {
        p->setPen(m_grid);
        for (int i = 1; i < 3; ++i) {
            const double x = r.left() + r.width() * i / 3.0;
            p->drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
    }
}

} // namespace wabcut::app
