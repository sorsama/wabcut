#include "MeterBar.h"

#include <QLocale>
#include <QSGNode>
#include <QSGSimpleRectNode>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wabcut::app {

namespace {

constexpr double kMidDb = -18.0;
constexpr double kHighDb = -6.0;

} // namespace

MeterBar::MeterBar(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void MeterBar::setSource(AudioMixerBridge* source) {
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source)
        connect(m_source, &AudioMixerBridge::metersUpdated, this, &MeterBar::readLevels);
    readLevels();
    emit sourceChanged();
}

void MeterBar::setChannel(const QString& channel) {
    if (m_channel == channel)
        return;
    m_channel = channel;
    readLevels();
    emit channelChanged();
}

void MeterBar::setFloorDb(double db) {
    if (m_floorDb == db)
        return;
    m_floorDb = std::min(db, -1.0);
    update();
    emit appearanceChanged();
}

#define WABCUT_METER_COLOR_SETTER(Name, member)                                                                         \
    void MeterBar::set##Name(const QColor& c) {                                                                        \
        if (member == c)                                                                                               \
            return;                                                                                                    \
        member = c;                                                                                                    \
        update();                                                                                                      \
        emit appearanceChanged();                                                                                      \
    }
WABCUT_METER_COLOR_SETTER(TrackColor, m_trackColor)
WABCUT_METER_COLOR_SETTER(LowColor, m_lowColor)
WABCUT_METER_COLOR_SETTER(MidColor, m_midColor)
WABCUT_METER_COLOR_SETTER(HighColor, m_highColor)
WABCUT_METER_COLOR_SETTER(PeakColor, m_peakColor)
#undef WABCUT_METER_COLOR_SETTER

double MeterBar::fraction(double db) const {
    if (!std::isfinite(db))
        return 0.0;
    return std::clamp((db - m_floorDb) / -m_floorDb, 0.0, 1.0);
}

void MeterBar::readLevels() {
    const AudioMixerBridge::MeterLevels next = m_source ? m_source->meter(m_channel) : AudioMixerBridge::MeterLevels{};
    const bool same = next.channels == m_levels.channels && next.clipped == m_levels.clipped
                      && next.peakDb == m_levels.peakDb && next.rmsDb == m_levels.rmsDb && next.holdDb == m_levels.holdDb;
    if (same)
        return;
    const bool clippedChanged = next.clipped != m_levels.clipped || next.channels != m_levels.channels;
    m_levels = next;
    double hold = -std::numeric_limits<double>::infinity();
    for (int ch = 0; ch < m_levels.channels; ++ch)
        hold = std::max(hold, m_levels.holdDb[static_cast<std::size_t>(ch)]);
    const QString text = m_levels.channels == 0 || !std::isfinite(hold) || hold <= m_floorDb
                             ? QStringLiteral("−∞")
                             : (hold < 0 ? QStringLiteral("−") : QString()) + QLocale().toString(std::abs(hold), 'f', 1);
    const bool textChanged = text != m_peakText;
    m_peakText = text;
    update();
    if (clippedChanged || textChanged)
        emit levelsChanged();
}

QSGNode* MeterBar::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    QSGNode* root = oldNode ? oldNode : new QSGNode;
    while (QSGNode* child = root->firstChild()) {
        root->removeChildNode(child);
        delete child;
    }
    const double w = width();
    const double h = height();
    int drawn = 0;
    const auto rect = [&](const QRectF& r, const QColor& color) {
        if (r.width() <= 0 || r.height() <= 0)
            return;
        root->appendChildNode(new QSGSimpleRectNode(r, color));
        ++drawn;
    };
    const int channels = std::max(1, m_levels.channels);
    const double gap = channels > 1 ? 1.0 : 0.0;
    const double barWidth = (w - gap * (channels - 1)) / channels;
    const double yMid = h * (1.0 - fraction(kMidDb));
    const double yHigh = h * (1.0 - fraction(kHighDb));
    for (int ch = 0; ch < channels; ++ch) {
        const double x = ch * (barWidth + gap);
        rect(QRectF(x, 0, barWidth, h), m_trackColor);
        --drawn; // the track is not a level
        if (m_levels.channels == 0)
            continue;
        const auto c = static_cast<std::size_t>(ch);
        // RMS fill in its zones, bottom up.
        const double yRms = h * (1.0 - fraction(m_levels.rmsDb[c]));
        rect(QRectF(x, std::max(yRms, yMid), barWidth, h - std::max(yRms, yMid)), m_lowColor);
        if (yRms < yMid)
            rect(QRectF(x, std::max(yRms, yHigh), barWidth, yMid - std::max(yRms, yHigh)), m_midColor);
        if (yRms < yHigh)
            rect(QRectF(x, yRms, barWidth, yHigh - yRms), m_highColor);
        // Peak line and hold.
        if (fraction(m_levels.peakDb[c]) > 0)
            rect(QRectF(x, std::clamp(h * (1.0 - fraction(m_levels.peakDb[c])) - 1, 0.0, h - 1), barWidth, 1), m_peakColor);
        if (fraction(m_levels.holdDb[c]) > 0) {
            const double hold = m_levels.holdDb[c];
            rect(QRectF(x, std::clamp(h * (1.0 - fraction(hold)) - 1, 0.0, h - 2), barWidth, 2),
                 hold >= kHighDb ? m_highColor : hold >= kMidDb ? m_midColor : m_lowColor);
        }
    }
    m_drawnRects = drawn;
    return root;
}

} // namespace wabcut::app
