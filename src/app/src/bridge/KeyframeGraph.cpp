#include "KeyframeGraph.h"

#include "EffectControls.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/KeyframeEasing.h>

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace wabcut::app {

namespace {

#define LABEL(text) QT_TRANSLATE_NOOP("wabcut::app::ProjectController", text)

constexpr int kSamples = 240;       ///< points of a curve's path across the shown window
constexpr int kRangeSamples = 120;  ///< points that fit a curve's value range over the whole clip
constexpr double kPad = 0.12;       ///< the share of its range a curve keeps free above and below

using Steps = std::vector<std::function<edit::EditOutcome(const ProjectState&)>>;

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

double clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
}

} // namespace

KeyframeGraph::KeyframeGraph(EffectControlsModel* controls) : QObject(controls), m_ec(controls) {}

void KeyframeGraph::setActive(bool on) {
    if (on == m_active)
        return;
    m_active = on;
    emit activeChanged();
    if (on) {
        m_dirty = true;
        rebuild(true);
    }
}

void KeyframeGraph::setSpeedGraph(bool on) {
    if (on == m_speed)
        return;
    m_speed = on;
    emit viewChanged();
}

void KeyframeGraph::setSharedScale(bool on) {
    if (on == m_shared)
        return;
    m_shared = on;
    m_dirty = true;
    emit viewChanged();
    rebuild(true);
}

double KeyframeGraph::minSpan() const {
    if (!m_ec || !m_ec->m_controller || m_ec->m_lane.duration <= Ticks{})
        return 1.0;
    const double frame = static_cast<double>(m_ec->m_controller->frameTicks().value);
    return std::min(1.0, 3.0 * frame / static_cast<double>(m_ec->m_lane.duration.value));
}

const KeyframeGraph::Window* KeyframeGraph::windowOf(const QString& row) const {
    const auto frozen = m_frozen.find(row);
    if (frozen != m_frozen.end())
        return &frozen->second;
    const auto it = m_windows.find(row);
    return it == m_windows.end() ? nullptr : &it->second;
}

// ---- The view ----------------------------------------------------------------------------------------------------

void KeyframeGraph::setView(double start, double end, double vStart, double vEnd) {
    if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(vStart) || !std::isfinite(vEnd))
        return;
    const double span = std::clamp(end - start, minSpan(), 1.0);
    if (span >= 1.0 - 1e-9) {
        start = 0.0;
        end = 1.0;
    } else {
        start = std::clamp(start, 0.0, 1.0 - span);
        end = start + span;
    }
    // The value window may go past 0..1 (a curve can be dragged out of its range) but keeps a sane size.
    double vSpan = std::clamp(vEnd - vStart, 0.002, 40.0);
    vStart = std::clamp(vStart, -20.0, 20.0);
    vEnd = vStart + vSpan;
    if (start == m_viewStart && end == m_viewEnd && vStart == m_valueStart && vEnd == m_valueEnd)
        return;
    const bool timeChanged = start != m_viewStart || end != m_viewEnd;
    m_viewStart = start;
    m_viewEnd = end;
    m_valueStart = vStart;
    m_valueEnd = vEnd;
    if (timeChanged) {
        m_dirty = true; // the paths are sampled across the shown window
        rebuild(true);
    }
    emit viewChanged();
}

void KeyframeGraph::zoomTime(double factor, double at) {
    if (!std::isfinite(factor) || factor <= 0.0 || !std::isfinite(at))
        return;
    const double pointer = clamp01(at);
    const double span = m_viewEnd - m_viewStart;
    const double next = std::clamp(span / factor, minSpan(), 1.0);
    const double start = m_viewStart + pointer * (span - next);
    setView(start, start + next, m_valueStart, m_valueEnd);
}

void KeyframeGraph::panTime(double dx) {
    if (std::isfinite(dx))
        setView(m_viewStart + dx, m_viewEnd + dx, m_valueStart, m_valueEnd);
}

void KeyframeGraph::zoomValue(double factor, double at) {
    if (!std::isfinite(factor) || factor <= 0.0 || !std::isfinite(at))
        return;
    const double pointer = clamp01(at);
    const double span = m_valueEnd - m_valueStart;
    const double next = std::clamp(span / factor, 0.002, 40.0);
    const double start = m_valueStart + pointer * (span - next);
    setView(m_viewStart, m_viewEnd, start, start + next);
}

void KeyframeGraph::panValue(double dy) {
    if (std::isfinite(dy))
        setView(m_viewStart, m_viewEnd, m_valueStart + dy, m_valueEnd + dy);
}

void KeyframeGraph::fit(bool selectionOnly) {
    double x0 = std::numeric_limits<double>::infinity();
    double x1 = -x0;
    double y0 = x0;
    double y1 = -x0;
    const auto take = [&](const QVariantMap& key, bool onlySelected) {
        if (onlySelected && !key.value(QStringLiteral("selected")).toBool())
            return;
        const double x = key.value(QStringLiteral("x")).toDouble();
        const double y = key.value(QStringLiteral("y")).toDouble();
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    };
    bool anySelected = false;
    for (const QVariant& c : m_curves) {
        for (const QVariant& k : c.toMap().value(QStringLiteral("keys")).toList())
            anySelected = anySelected || k.toMap().value(QStringLiteral("selected")).toBool();
    }
    const bool only = selectionOnly && anySelected;
    for (const QVariant& c : m_curves) {
        for (const QVariant& k : c.toMap().value(QStringLiteral("keys")).toList())
            take(k.toMap(), only);
    }
    if (!(x1 >= x0)) {
        resetView();
        return;
    }
    const double span = std::max(minSpan(), (x1 - x0) * 1.2);
    const double mid = (x0 + x1) / 2;
    const double vSpan = std::max(0.05, (y1 - y0) * 1.4);
    const double vMid = (y0 + y1) / 2;
    setView(mid - span / 2, mid + span / 2, vMid - vSpan / 2, vMid + vSpan / 2);
}

void KeyframeGraph::resetView() {
    setView(0.0, 1.0, 0.0, 1.0);
}

// ---- The legend ----------------------------------------------------------------------------------------------------

void KeyframeGraph::setCurveVisible(const QString& row, bool visible) {
    if (visible)
        m_hidden.erase(row);
    else
        m_hidden.insert(row);
    m_dirty = true;
    rebuild(true);
}

void KeyframeGraph::setActiveRow(const QString& row) {
    if (row == m_activeRow)
        return;
    m_activeRow = row;
    m_dirty = true;
    rebuild(true);
}

void KeyframeGraph::showAll() {
    m_hidden.clear();
    m_dirty = true;
    rebuild(true);
}

void KeyframeGraph::showSelected() {
    if (!m_ec)
        return;
    std::set<QString> rows;
    for (const EffectControlsModel::KeyRef& k : m_ec->m_selectedKeys)
        rows.insert(k.row);
    if (rows.empty())
        return;
    m_hidden.clear();
    for (const QVariant& c : m_legend) {
        const QString row = c.toMap().value(QStringLiteral("row")).toString();
        if (!rows.contains(row))
            m_hidden.insert(row);
    }
    m_dirty = true;
    rebuild(true);
}

void KeyframeGraph::isolate(const QString& row) {
    m_hidden.clear();
    for (const QVariant& c : m_legend) {
        const QString other = c.toMap().value(QStringLiteral("row")).toString();
        if (other != row)
            m_hidden.insert(other);
    }
    m_activeRow = row;
    m_dirty = true;
    rebuild(true);
}

// ---- Building what the panel draws ---------------------------------------------------------------------------------

void KeyframeGraph::onRowsRefreshed() {
    if (m_active)
        rebuild(false);
}

void KeyframeGraph::rebuild(bool force) {
    if (!m_active || !m_ec || m_building)
        return;
    m_building = true;
    ProjectController* pc = m_ec->m_controller;
    const bool clipMode = m_ec->m_mode == QLatin1String("clip") && pc;

    // The rows the curves come from: every section open (the editor lists collapsed sections' curves too).
    const std::vector<EffectControlsModel::Row>& source = m_ec->m_uncollapsedRows.empty() ? m_ec->m_rows : m_ec->m_uncollapsedRows;

    // The cheap path: only the playhead moved. What the curves are made of: the shown clip, its animated rows and their key
    // counts, the keys selected, the revision.
    std::size_t selectionHash = m_ec->m_selectedKeys.size();
    for (const EffectControlsModel::KeyRef& k : m_ec->m_selectedKeys)
        selectionHash = selectionHash * 1315423911u + static_cast<std::size_t>(k.curveTime) + qHash(k.row);
    selectionHash = selectionHash * 31u + qHash(m_ec->m_mode) + static_cast<std::size_t>(m_ec->m_lane.start.value) +
                    static_cast<std::size_t>(m_ec->m_lane.duration.value) * 7u + (pc ? pc->primaryClip().value : 0u);
    for (const EffectControlsModel::Row& r : source) {
        if (r.kind == QLatin1String("param") && r.animated)
            selectionHash = selectionHash * 1099511628211ull + qHash(r.key) + static_cast<std::size_t>(r.keys.size());
    }
    const int revision = pc ? pc->revision() : -1;
    if (!force && !m_dirty && revision == m_builtRevision && selectionHash == m_builtSelection) {
        m_building = false;
        emit playheadChanged();
        return;
    }
    m_dirty = false;
    m_builtRevision = revision;
    m_builtSelection = selectionHash;

    struct Entry {
        const EffectControlsModel::Row* row;
        const Clip* clip;
        ParamValue value;
        int color;
    };
    std::vector<Entry> entries;
    std::map<QString, QString> sectionTitle;
    if (clipMode) {
        for (const EffectControlsModel::Row& r : source) {
            if (r.kind == QLatin1String("header"))
                sectionTitle[r.section] = r.title;
        }
        int color = 0;
        for (const EffectControlsModel::Row& r : source) {
            if (r.kind != QLatin1String("param") || !r.animated)
                continue;
            const Clip* clip = m_ec->clipOf(r);
            if (!clip)
                continue;
            ParamValue value = m_ec->rowValue(r);
            if (!value.curve() || value.curve()->keys.empty())
                continue;
            entries.push_back(Entry{&r, clip, std::move(value), color++});
        }
    }

    // Another clip: everything starts over.
    const ClipId clipId = entries.empty() ? ClipId{} : entries.front().clip->id;
    if (clipId != m_clip) {
        m_clip = clipId;
        m_hidden.clear();
        m_activeRow.clear();
        m_frozen.clear();
        m_viewStart = 0;
        m_viewEnd = 1;
        m_valueStart = 0;
        m_valueEnd = 1;
        emit viewChanged();
    }
    const double laneDuration = static_cast<double>(std::max<std::int64_t>(1, m_ec->m_lane.duration.value));

    // The legend.
    m_legend.clear();
    for (const Entry& e : entries) {
        const Curve& curve = *e.value.curve();
        const double nowValue = e.row->range.toUi(curve.valueAt(m_ec->rowCurveTimeAtPlayhead(*e.row, *e.clip)), e.row->frame);
        m_legend.append(QVariantMap{
            {QStringLiteral("row"), e.row->key},
            {QStringLiteral("title"), e.row->title},
            {QStringLiteral("section"), sectionTitle.contains(e.row->section) ? sectionTitle[e.row->section] : QString()},
            {QStringLiteral("color"), e.color},
            {QStringLiteral("visible"), !m_hidden.contains(e.row->key)},
            {QStringLiteral("active"), e.row->key == m_activeRow},
            {QStringLiteral("keys"), static_cast<int>(curve.keys.size())},
            {QStringLiteral("valueText"), QString::number(nowValue, 'f', static_cast<int>(e.row->decimals)) + (e.row->unit.isEmpty() ? QString() : QStringLiteral(" ") + e.row->unit)},
        });
    }
    const bool activeVisible = std::any_of(entries.begin(), entries.end(), [&](const Entry& e) {
        return e.row->key == m_activeRow && !m_hidden.contains(e.row->key);
    });
    if (!activeVisible) {
        m_activeRow.clear();
        for (const Entry& e : entries) {
            if (!m_hidden.contains(e.row->key)) {
                m_activeRow = e.row->key;
                break;
            }
        }
    }

    // The value windows: each curve fitted to its own range over the whole clip (or the union of them), frozen while a
    // gesture drags.
    m_windows.clear();
    struct Fit {
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        double speed = 0;
    };
    std::map<QString, Fit> fits;
    for (const Entry& e : entries) {
        if (m_hidden.contains(e.row->key))
            continue;
        const Curve& curve = *e.value.curve();
        Fit fit;
        const auto take = [&](double v) {
            fit.lo = std::min(fit.lo, v);
            fit.hi = std::max(fit.hi, v);
        };
        const auto ui = [&](double model) { return e.row->range.toUi(model, e.row->frame); };
        double previous = 0;
        for (int i = 0; i <= kRangeSamples; ++i) {
            const Ticks t{m_ec->m_lane.start.value + static_cast<std::int64_t>(std::llround(laneDuration * i / kRangeSamples))};
            const double v = ui(curve.valueAt(m_ec->rowCurveTime(*e.row, *e.clip, t)));
            take(v);
            if (i > 0) {
                const double seconds = laneDuration / kRangeSamples / static_cast<double>(kTicksPerSecond);
                fit.speed = std::max(fit.speed, std::abs(v - previous) / std::max(seconds, 1e-9));
            }
            previous = v;
        }
        for (std::size_t i = 0; i < curve.keys.size(); ++i) {
            const Keyframe& k = curve.keys[i];
            take(ui(k.value));
            if (k.interpolation != Interpolation::Bezier)
                continue;
            if (i + 1 < curve.keys.size()) {
                const double seconds = (curve.keys[i + 1].time - k.time).toSeconds();
                take(ui(k.value + k.out.slope * k.out.influence * seconds));
                fit.speed = std::max(fit.speed, std::abs(k.out.slope * e.row->range.factor));
            }
            if (i > 0) {
                const double seconds = (k.time - curve.keys[i - 1].time).toSeconds();
                take(ui(k.value - k.in.slope * k.in.influence * seconds));
                fit.speed = std::max(fit.speed, std::abs(k.in.slope * e.row->range.factor));
            }
        }
        fits[e.row->key] = fit;
    }
    double sharedLo = std::numeric_limits<double>::infinity();
    double sharedHi = -sharedLo;
    for (const auto& [row, fit] : fits) {
        sharedLo = std::min(sharedLo, fit.lo);
        sharedHi = std::max(sharedHi, fit.hi);
    }
    for (const Entry& e : entries) {
        const auto it = fits.find(e.row->key);
        if (it == fits.end())
            continue;
        double lo = m_shared ? sharedLo : it->second.lo;
        double hi = m_shared ? sharedHi : it->second.hi;
        const double minRange = std::max(std::abs(e.row->to - e.row->from) * 0.05, e.row->step);
        if (hi - lo < minRange) {
            const double mid = (lo + hi) / 2;
            lo = mid - minRange / 2;
            hi = mid + minRange / 2;
        }
        const double pad = (hi - lo) * kPad;
        Window w;
        w.lo = lo - pad;
        w.hi = hi + pad;
        w.speedMax = std::max(it->second.speed * 1.1, e.row->step);
        m_windows[e.row->key] = w;
    }

    // The curves.
    m_curves.clear();
    const double viewSpan = std::max(1e-9, m_viewEnd - m_viewStart);
    for (const Entry& e : entries) {
        if (m_hidden.contains(e.row->key))
            continue;
        const Curve& curve = *e.value.curve();
        const Window* w = windowOf(e.row->key);
        if (!w)
            continue;
        const EffectControlsModel::Row& r = *e.row;
        const Clip& clip = *e.clip;
        const auto ui = [&](double model) { return r.range.toUi(model, r.frame); };
        const auto yOf = [&](double v) { return (w->hi - v) / (w->hi - w->lo); };
        const auto speedY = [&](double speed) { return 0.5 - 0.5 * speed / w->speedMax; };
        const auto ticksOf = [](double seconds) {
            return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
        };
        const auto xOf = [&](Ticks curveTime) -> std::optional<double> {
            const auto t = m_ec->rowTimelineTime(r, clip, curveTime);
            if (!t)
                return std::nullopt;
            return static_cast<double>((*t - m_ec->m_lane.start).value) / laneDuration;
        };
        const auto valueAtX = [&](double x) {
            const Ticks t{m_ec->m_lane.start.value + static_cast<std::int64_t>(std::llround(clamp01(x) * laneDuration))};
            return ui(curve.valueAt(m_ec->rowCurveTime(r, clip, t)));
        };

        // One sample past each end of the window gives the central difference its neighbours.
        std::vector<double> xs(kSamples + 3);
        std::vector<double> values(kSamples + 3);
        for (int i = -1; i <= kSamples + 1; ++i) {
            const double x = m_viewStart + viewSpan * i / kSamples;
            xs[static_cast<std::size_t>(i + 1)] = x;
            values[static_cast<std::size_t>(i + 1)] = valueAtX(x);
        }
        QVariantList path;
        QVariantList speedPath;
        path.reserve((kSamples + 1) * 2);
        speedPath.reserve((kSamples + 1) * 2);
        for (int i = 0; i <= kSamples; ++i) {
            const auto at = static_cast<std::size_t>(i + 1);
            const double x = xs[at];
            path.append(x);
            path.append(yOf(values[at]));
            const double seconds = std::max(1e-9, (xs[at + 1] - xs[at - 1]) * laneDuration / static_cast<double>(kTicksPerSecond));
            speedPath.append(x);
            speedPath.append(speedY((values[at + 1] - values[at - 1]) / seconds));
        }

        QVariantList keys;
        for (const auto& [i, t] : m_ec->visibleKeys(r, curve, clip)) {
            const Keyframe& k = curve.keys[i];
            const double x = static_cast<double>((t - m_ec->m_lane.start).value) / laneDuration;
            const bool selected = m_ec->m_selectedKeys.contains(EffectControlsModel::KeyRef{r.key, k.time.value});
            QVariantMap key{
                {QStringLiteral("index"), static_cast<int>(i)},
                {QStringLiteral("tick"), static_cast<qlonglong>(k.time.value)},
                {QStringLiteral("x"), x},
                {QStringLiteral("y"), yOf(ui(k.value))},
                {QStringLiteral("selected"), selected},
                {QStringLiteral("shape"), fromView(toString(k.interpolation))},
                {QStringLiteral("timeText"), pc->formatTimecode(t)},
                {QStringLiteral("valueText"), QString::number(ui(k.value), 'f', static_cast<int>(r.decimals)) + (r.unit.isEmpty() ? QString() : QStringLiteral(" ") + r.unit)},
            };
            // The speed at the key: Bezier keys name theirs; the others run at the slope to their neighbour.
            double keySpeed = 0;
            if (k.interpolation == Interpolation::Bezier) {
                keySpeed = k.out.slope * r.range.factor;
            } else if (i + 1 < curve.keys.size()) {
                const double seconds = (curve.keys[i + 1].time - k.time).toSeconds();
                keySpeed = k.interpolation == Interpolation::Hold || seconds <= 0 ? 0.0
                                                                                   : (curve.keys[i + 1].value - k.value) * r.range.factor / seconds;
            } else if (i > 0) {
                const double seconds = (k.time - curve.keys[i - 1].time).toSeconds();
                keySpeed = seconds > 0 ? (k.value - curve.keys[i - 1].value) * r.range.factor / seconds : 0.0;
            }
            key.insert(QStringLiteral("speedY"), speedY(keySpeed));
            bool hasIn = false, hasOut = false;
            if (selected && k.interpolation == Interpolation::Bezier) {
                if (i + 1 < curve.keys.size()) {
                    const double seconds = (curve.keys[i + 1].time - k.time).toSeconds();
                    const double dt = k.out.influence * seconds;
                    if (const auto hx = xOf(k.time + ticksOf(dt))) {
                        hasOut = true;
                        key.insert(QStringLiteral("outX"), *hx);
                        key.insert(QStringLiteral("outY"), yOf(ui(k.value + k.out.slope * dt)));
                        key.insert(QStringLiteral("sOutX"), *hx);
                        key.insert(QStringLiteral("sOutY"), speedY(k.out.slope * r.range.factor));
                    }
                }
                if (i > 0) {
                    const double seconds = (k.time - curve.keys[i - 1].time).toSeconds();
                    const double dt = k.in.influence * seconds;
                    if (const auto hx = xOf(k.time - ticksOf(dt))) {
                        hasIn = true;
                        key.insert(QStringLiteral("inX"), *hx);
                        key.insert(QStringLiteral("inY"), yOf(ui(k.value - k.in.slope * dt)));
                        key.insert(QStringLiteral("sInX"), *hx);
                        key.insert(QStringLiteral("sInY"), speedY(k.in.slope * r.range.factor));
                    }
                }
            }
            key.insert(QStringLiteral("hasIn"), hasIn);
            key.insert(QStringLiteral("hasOut"), hasOut);
            key.insert(QStringLiteral("sHasIn"), hasIn);
            key.insert(QStringLiteral("sHasOut"), hasOut);
            key.insert(QStringLiteral("broken"), hasIn && hasOut && std::abs(k.in.slope - k.out.slope) > 1e-9 * std::max(1.0, std::abs(k.out.slope)));
            keys.append(key);
        }
        m_curves.append(QVariantMap{
            {QStringLiteral("row"), r.key},
            {QStringLiteral("title"), r.title},
            {QStringLiteral("section"), sectionTitle.contains(r.section) ? sectionTitle[r.section] : QString()},
            {QStringLiteral("color"), e.color},
            {QStringLiteral("active"), r.key == m_activeRow},
            {QStringLiteral("lo"), w->lo},
            {QStringLiteral("hi"), w->hi},
            {QStringLiteral("unit"), r.unit},
            {QStringLiteral("decimals"), r.decimals},
            {QStringLiteral("speedMax"), w->speedMax},
            {QStringLiteral("path"), path},
            {QStringLiteral("speedPath"), speedPath},
            {QStringLiteral("keys"), keys},
        });
    }

    // The info line and the numeric fields.
    QVariantMap info;
    info.insert(QStringLiteral("hasClip"), !entries.empty());
    info.insert(QStringLiteral("clipTitle"), m_ec->m_title);
    info.insert(QStringLiteral("subtitle"), m_ec->m_subtitle);
    info.insert(QStringLiteral("lengthSeconds"), laneDuration / static_cast<double>(kTicksPerSecond));
    info.insert(QStringLiteral("frameSeconds"),
                pc ? static_cast<double>(pc->frameTicks().value) / static_cast<double>(kTicksPerSecond) : 0.04);
    int selCount = 0;
    std::set<QString> selRows;
    const EffectControlsModel::KeyRef* primary = nullptr;
    for (const EffectControlsModel::KeyRef& k : m_ec->m_selectedKeys) {
        ++selCount;
        selRows.insert(k.row);
        if (!primary || (k.row == m_activeRow && primary->row != m_activeRow) ||
            (k.row == primary->row && k.curveTime < primary->curveTime && k.row == m_activeRow))
            primary = &k;
    }
    info.insert(QStringLiteral("selCount"), selCount);
    info.insert(QStringLiteral("selMixed"), selRows.size() > 1);
    info.insert(QStringLiteral("selShape"), m_ec->selectedKeyInterpolation());
    if (primary) {
        for (const Entry& e : entries) {
            if (e.row->key != primary->row)
                continue;
            const Curve& curve = *e.value.curve();
            const auto at = curve.keyAt(Ticks{primary->curveTime});
            if (!at)
                break;
            const auto t = m_ec->rowTimelineTime(*e.row, *e.clip, Ticks{primary->curveTime});
            info.insert(QStringLiteral("selRow"), e.row->key);
            info.insert(QStringLiteral("selTitle"), e.row->title);
            info.insert(QStringLiteral("selTimeText"), t && pc ? pc->formatTimecode(*t) : QString());
            info.insert(QStringLiteral("selValue"), e.row->range.toUi(curve.keys[*at].value, e.row->frame));
            info.insert(QStringLiteral("selUnit"), e.row->unit);
            info.insert(QStringLiteral("selDecimals"), e.row->decimals);
            info.insert(QStringLiteral("selFrom"), e.row->from);
            info.insert(QStringLiteral("selTo"), e.row->to);
            info.insert(QStringLiteral("selStep"), e.row->step);
            break;
        }
    }
    m_info = std::move(info);
    m_building = false;
    emit changed();
    emit playheadChanged();
}

double KeyframeGraph::playhead() const {
    return m_ec ? m_ec->m_playheadPosition : -1.0;
}

// ---- Pointer -------------------------------------------------------------------------------------------------------

QString KeyframeGraph::press(const QString& pane, double x, double y, double tolX, double tolY, int modifiers) {
    m_press = {};
    if (!m_ec || !m_ec->m_controller || !(tolX > 0.0) || !(tolY > 0.0))
        return QStringLiteral("none");
    const bool speedPane = pane == QLatin1String("speed");
    struct Hit {
        double d2 = 2.0;
        QString row;
        int index = -1;
        qlonglong tick = 0;
        bool handle = false;
        bool out = true;
    };
    Hit bestHandle;
    Hit bestKey;
    for (const QVariant& cv : m_curves) {
        const QVariantMap curve = cv.toMap();
        const QString row = curve.value(QStringLiteral("row")).toString();
        for (const QVariant& kv : curve.value(QStringLiteral("keys")).toList()) {
            const QVariantMap key = kv.toMap();
            const auto consider = [&](Hit& best, double hx, double hy, bool handle, bool out) {
                const double dx = (hx - x) / tolX;
                const double dy = (hy - y) / tolY;
                const double d2 = dx * dx + dy * dy;
                if (d2 < best.d2 && d2 <= 1.0) {
                    best.d2 = d2;
                    best.row = row;
                    best.index = key.value(QStringLiteral("index")).toInt();
                    best.tick = key.value(QStringLiteral("tick")).toLongLong();
                    best.handle = handle;
                    best.out = out;
                }
            };
            consider(bestKey, key.value(QStringLiteral("x")).toDouble(),
                     key.value(speedPane ? QStringLiteral("speedY") : QStringLiteral("y")).toDouble(), false, true);
            for (const bool out : {true, false}) {
                const QString side = out ? QStringLiteral("Out") : QStringLiteral("In");
                if (!key.value(QStringLiteral("has") + side).toBool())
                    continue;
                const QString prefix = speedPane ? QStringLiteral("s") : QString();
                const QString xName = prefix.isEmpty() ? side.toLower() + QStringLiteral("X") : QStringLiteral("s") + side + QStringLiteral("X");
                const QString yName = prefix.isEmpty() ? side.toLower() + QStringLiteral("Y") : QStringLiteral("s") + side + QStringLiteral("Y");
                consider(bestHandle, key.value(xName).toDouble(), key.value(yName).toDouble(), true, out);
            }
        }
    }
    const bool add = Qt::KeyboardModifiers(modifiers).testFlag(Qt::ShiftModifier);
    const bool toggle = Qt::KeyboardModifiers(modifiers).testAnyFlags(Qt::ControlModifier | Qt::MetaModifier);
    if (bestHandle.index >= 0) {
        m_press = Press{true, QStringLiteral("handle"), pane, bestHandle.row, x, y, false, static_cast<std::size_t>(bestHandle.index), bestHandle.out};
        m_activeRow = bestHandle.row;
        return QStringLiteral("handle");
    }
    if (bestKey.index >= 0) {
        const EffectControlsModel::KeyRef hit{bestKey.row, bestKey.tick};
        if (toggle) {
            if (!m_ec->m_selectedKeys.erase(hit))
                m_ec->m_selectedKeys.insert(hit);
        } else if (add) {
            m_ec->m_selectedKeys.insert(hit);
        } else if (!m_ec->m_selectedKeys.contains(hit)) {
            m_ec->m_selectedKeys = {hit};
        }
        if (!m_ec->m_selectedKeys.empty())
            m_ec->setSectionSelection({}, {});
        m_activeRow = bestKey.row;
        m_dirty = true;
        m_ec->refresh();
        emit m_ec->keySelectionChanged();
        m_press = Press{m_ec->m_selectedKeys.contains(hit) || !toggle, QStringLiteral("key"), pane, bestKey.row, x, y, false, 0, true};
        return QStringLiteral("key");
    }
    return QStringLiteral("none");
}

void KeyframeGraph::dragTo(const QString& pane, double x, double y, int modifiers) {
    if (!m_press.down || !m_ec || !m_ec->m_controller || !std::isfinite(x) || !std::isfinite(y))
        return;
    ProjectController* pc = m_ec->m_controller;
    const bool shift = Qt::KeyboardModifiers(modifiers).testFlag(Qt::ShiftModifier);
    const bool alt = Qt::KeyboardModifiers(modifiers).testFlag(Qt::AltModifier);
    if (!m_press.started) {
        m_press.started = true;
        m_frozen = m_windows; // a curve does not rescale under the pointer while it is dragged
    }
    const Window* w = windowOf(m_press.row);

    if (m_press.kind == QLatin1String("handle")) {
        const EffectControlsModel::Row* r = m_ec->findRow(m_press.row);
        const Clip* clip = r ? m_ec->clipOf(*r) : nullptr;
        if (!r || !clip || !w)
            return;
        auto& drag = m_ec->m_handleDrag;
        if (!drag.active) {
            drag.active = true;
            drag.row = m_press.row;
            drag.index = m_press.index;
            drag.out = m_press.out;
            drag.min = w->lo;
            drag.max = w->hi;
            drag.mergeKey = "keyframe-handle:" + std::to_string(++m_ec->m_dragCounter);
        }
        if (m_press.pane == QLatin1String("value")) {
            m_ec->dragHandle(x, y, alt, shift); // the same maths as the inline graph: x of the clip, y of the curve's range
            return;
        }
        // The speed graph: the handle's height IS the slope (UI units a second), its distance along the segment the influence.
        const ParamValue value = m_ec->rowValue(*r);
        const Curve* curve = value.curve();
        const std::size_t i = m_press.index;
        const bool out = m_press.out;
        if (!curve || i >= curve->keys.size() || (out ? i + 1 >= curve->keys.size() : i == 0))
            return;
        const Keyframe& k = curve->keys[i];
        const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_ec->m_lane.duration.value));
        const Ticks t{m_ec->m_lane.start.value + static_cast<std::int64_t>(std::llround(clamp01(x) * laneLength))};
        const Ticks curveTime = m_ec->rowCurveTime(*r, *clip, std::clamp(t, clip->start(), clip->end()));
        const double segment = out ? (curve->keys[i + 1].time - k.time).toSeconds() : (k.time - curve->keys[i - 1].time).toSeconds();
        if (segment <= 0 || r->range.factor == 0.0)
            return;
        const double along = std::clamp(out ? (curveTime - k.time).toSeconds() : (k.time - curveTime).toSeconds(), segment * 0.01, segment);
        const double speedUi = shift ? 0.0 : (0.5 - y) * 2.0 * w->speedMax;
        const BezierHandle handle{speedUi / r->range.factor, along / segment};
        edit::KeyframeChange change;
        (out ? change.out : change.in) = handle;
        if (!alt) {
            if (out && i > 0)
                change.in = BezierHandle{handle.slope, k.in.influence};
            if (!out && i + 1 < curve->keys.size())
                change.out = BezierHandle{handle.slope, k.out.influence};
        }
        const SequenceId seq = pc->currentSequenceId();
        const ClipId clipId = clip->id;
        const edit::ParamRef ref = r->ref;
        pc->applyEditFn(
            LABEL("Keyframe Handles"), [&](const ProjectState& state) { return edit::setKeyframe(state, seq, clipId, ref, i, change); },
            drag.mergeKey);
        return;
    }

    // Keys: the selected ones move together.
    if (!w || m_press.pane != pane)
        return;
    if (!m_ec->m_drag.active && !m_ec->beginKeyDrag(false))
        return;
    const SequencePtr seq = pc->currentSequence();
    if (!seq)
        return;
    const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_ec->m_lane.duration.value));
    const Ticks frame = pc->frameTicks();
    Ticks delta{static_cast<std::int64_t>(std::llround((x - m_press.x) * laneLength))};
    delta = Ticks{std::llround(static_cast<double>(delta.value) / static_cast<double>(frame.value)) * frame.value};
    const double dny = pane == QLatin1String("value") ? y - m_press.y : 0.0; // the speed graph moves keys in time only

    Ticks lo = Ticks::min(), hi = Ticks::max();
    for (const auto& g : m_ec->m_drag.groups) {
        const auto loc = seq->findClip(g.clip);
        const EffectControlsModel::Row* row = m_ec->findRow(g.row);
        if (!loc || !row)
            return;
        for (Ticks t : g.moving) {
            if (const auto tl = m_ec->rowTimelineTime(*row, loc->clip(), t)) {
                lo = std::max(lo, loc->clip().start() - *tl);
                hi = std::min(hi, loc->clip().end() - frame - *tl);
            }
        }
    }
    delta = std::clamp(delta, std::min(lo, Ticks{}), std::max(hi, Ticks{}));

    struct Result {
        const EffectControlsModel::DragGroup* group;
        Curve curve;
        std::vector<Ticks> times;
    };
    std::vector<Result> results;
    for (const auto& g : m_ec->m_drag.groups) {
        const Clip& clip = seq->findClip(g.clip)->clip();
        const EffectControlsModel::Row& row = *m_ec->findRow(g.row);
        const Window* gw = windowOf(g.row);
        Result res{&g, g.original, {}};
        std::vector<Keyframe> moved;
        for (Ticks t : g.moving) {
            if (const auto i = res.curve.keyAt(t))
                moved.push_back(res.curve.keys[*i]);
        }
        std::erase_if(res.curve.keys, [&](const Keyframe& k) {
            return std::find(g.moving.begin(), g.moving.end(), k.time) != g.moving.end();
        });
        for (Keyframe k : moved) {
            const auto tl = m_ec->rowTimelineTime(row, clip, k.time);
            if (!tl)
                continue;
            k.time = m_ec->rowCurveTime(row, clip, *tl + delta);
            if (gw && dny != 0.0) { // the same share of the curve's height on every curve
                const double ui = std::clamp(row.range.toUi(k.value, row.frame) - dny * (gw->hi - gw->lo), row.from, row.to);
                k.value = row.range.toModel(ui, row.frame);
            }
            if (res.curve.keyAt(k.time))
                return; // onto another key: the keys stay where they were last
            res.curve.keys.insert(res.curve.keys.begin() + static_cast<std::ptrdiff_t>(res.curve.insertionIndex(k.time)), k);
            res.times.push_back(k.time);
        }
        results.push_back(std::move(res));
    }
    const SequenceId seqId = seq->id;
    Steps steps;
    for (const Result& res : results) {
        const ClipId clip = res.group->clip;
        const edit::ParamRef ref = res.group->ref;
        const Curve curve = res.curve;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seqId, clip, ref, ParamValue(curve)); });
    }
    if (!m_ec->applyChainedEdit(LABEL("Move Keyframes"), steps, m_ec->m_drag.mergeKey))
        return;
    std::set<EffectControlsModel::KeyRef> selection;
    for (const Result& res : results) {
        for (Ticks t : res.times)
            selection.insert(EffectControlsModel::KeyRef{res.group->row, t.value});
    }
    if (selection != m_ec->m_selectedKeys) {
        m_ec->m_selectedKeys = std::move(selection);
        m_ec->refresh();
        emit m_ec->keySelectionChanged();
    }
}

void KeyframeGraph::release() {
    const Press press = m_press;
    m_press = {};
    if (!press.down || !m_ec)
        return;
    if (press.started) {
        if (press.kind == QLatin1String("handle"))
            m_ec->endHandleDrag();
        else
            m_ec->endKeyDrag();
    }
    m_frozen.clear();
    m_dirty = true;
    m_ec->refresh();
    rebuild(true);
}

void KeyframeGraph::boxSelect(const QString& pane, double x0, double y0, double x1, double y1, int modifiers) {
    if (!m_ec)
        return;
    const bool speedPane = pane == QLatin1String("speed");
    const bool add = Qt::KeyboardModifiers(modifiers).testAnyFlags(Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier);
    const double xa = std::min(x0, x1), xb = std::max(x0, x1);
    const double ya = std::min(y0, y1), yb = std::max(y0, y1);
    std::set<EffectControlsModel::KeyRef> selection = add ? m_ec->m_selectedKeys : std::set<EffectControlsModel::KeyRef>{};
    for (const QVariant& cv : m_curves) {
        const QVariantMap curve = cv.toMap();
        const QString row = curve.value(QStringLiteral("row")).toString();
        for (const QVariant& kv : curve.value(QStringLiteral("keys")).toList()) {
            const QVariantMap key = kv.toMap();
            const double kx = key.value(QStringLiteral("x")).toDouble();
            const double ky = key.value(speedPane ? QStringLiteral("speedY") : QStringLiteral("y")).toDouble();
            if (kx >= xa && kx <= xb && ky >= ya && ky <= yb)
                selection.insert(EffectControlsModel::KeyRef{row, key.value(QStringLiteral("tick")).toLongLong()});
        }
    }
    if (selection == m_ec->m_selectedKeys)
        return;
    m_ec->m_selectedKeys = std::move(selection);
    if (!m_ec->m_selectedKeys.empty())
        m_ec->setSectionSelection({}, {});
    m_dirty = true;
    m_ec->refresh();
    emit m_ec->keySelectionChanged();
}

void KeyframeGraph::clearSelection() {
    if (m_ec)
        m_ec->clearKeySelection();
}

bool KeyframeGraph::addKeyAt(double x, double y, double tolY) {
    if (!m_ec || !m_ec->m_controller || !std::isfinite(x) || !std::isfinite(y))
        return false;
    ProjectController* pc = m_ec->m_controller;
    const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_ec->m_lane.duration.value));
    const Ticks frame = pc->frameTicks();
    double best = tolY;
    const EffectControlsModel::Row* row = nullptr;
    for (const QVariant& cv : m_curves) {
        const QVariantMap curve = cv.toMap();
        const QVariantList path = curve.value(QStringLiteral("path")).toList();
        // The curve's height at x: the polyline between the two samples around it.
        for (int i = 0; i + 3 < path.size(); i += 2) {
            const double xa = path[i].toDouble(), xb = path[i + 2].toDouble();
            if (x < xa || x > xb || xb <= xa)
                continue;
            const double f = (x - xa) / (xb - xa);
            const double cy = path[i + 1].toDouble() + (path[i + 3].toDouble() - path[i + 1].toDouble()) * f;
            if (std::abs(cy - y) <= best) {
                best = std::abs(cy - y);
                row = m_ec->findRow(curve.value(QStringLiteral("row")).toString());
            }
            break;
        }
    }
    const Clip* clip = row ? m_ec->clipOf(*row) : nullptr;
    if (!clip)
        return false;
    Ticks timeline{m_ec->m_lane.start.value + static_cast<std::int64_t>(std::llround(clamp01(x) * laneLength))};
    timeline = Ticks{std::llround(static_cast<double>(timeline.value) / static_cast<double>(frame.value)) * frame.value};
    timeline = std::clamp(timeline, clip->start(), std::max(clip->start(), clip->end() - frame));
    const Ticks curveTime = m_ec->rowCurveTime(*row, *clip, timeline);
    const ParamValue value = m_ec->rowValue(*row);
    const Curve* curve = value.curve();
    if (!curve)
        return false;
    if (curve->keyAt(curveTime)) { // already a key there: select it
        m_ec->m_selectedKeys = {EffectControlsModel::KeyRef{row->key, curveTime.value}};
        m_activeRow = row->key;
        m_dirty = true;
        m_ec->refresh();
        emit m_ec->keySelectionChanged();
        return true;
    }
    Keyframe key;
    key.time = curveTime;
    key.value = curve->valueAt(curveTime);
    const std::size_t next = curve->insertionIndex(curveTime);
    key.interpolation = next > 0 ? curve->keys[next - 1].interpolation : Interpolation::Linear;
    if (key.interpolation == Interpolation::Bezier) {
        // Smooth through the point the curve passes: its slope there, a third of the way along each side.
        const double h = 0.001;
        const double slope = (curve->valueAt(curveTime + Ticks{static_cast<std::int64_t>(h * kTicksPerSecond)}) -
                              curve->valueAt(curveTime - Ticks{static_cast<std::int64_t>(h * kTicksPerSecond)})) / (2 * h);
        key.in = BezierHandle{slope, 1.0 / 3.0};
        key.out = BezierHandle{slope, 1.0 / 3.0};
    }
    const SequenceId seq = pc->currentSequenceId();
    const ClipId clipId = clip->id;
    const edit::ParamRef ref = row->ref;
    const QString rowKey = row->key;
    if (!pc->applyEditFn(LABEL("Add Keyframe"), [&](const ProjectState& state) { return edit::addKeyframe(state, seq, clipId, ref, key); }))
        return false;
    m_ec->m_selectedKeys = {EffectControlsModel::KeyRef{rowKey, curveTime.value}};
    m_activeRow = rowKey;
    m_dirty = true;
    m_ec->refresh();
    emit m_ec->keySelectionChanged();
    return true;
}

void KeyframeGraph::seek(double x) {
    if (m_ec)
        m_ec->seekLane(x);
}

QString KeyframeGraph::timeText(double x) const {
    if (!m_ec || !m_ec->m_controller || !std::isfinite(x))
        return {};
    const double laneLength = static_cast<double>(m_ec->m_lane.duration.value);
    return m_ec->m_controller->formatTimecode(
        Ticks{m_ec->m_lane.start.value + static_cast<std::int64_t>(std::llround(std::clamp(x, 0.0, 1.0) * laneLength))});
}

QString KeyframeGraph::describeAt(const QString& pane, double x, double y, double tolX, double tolY) const {
    const bool speedPane = pane == QLatin1String("speed");
    double bestKey = 2.0;
    QString text;
    for (const QVariant& cv : m_curves) {
        const QVariantMap curve = cv.toMap();
        for (const QVariant& kv : curve.value(QStringLiteral("keys")).toList()) {
            const QVariantMap key = kv.toMap();
            const double dx = (key.value(QStringLiteral("x")).toDouble() - x) / tolX;
            const double dy = (key.value(speedPane ? QStringLiteral("speedY") : QStringLiteral("y")).toDouble() - y) / tolY;
            const double d2 = dx * dx + dy * dy;
            if (d2 <= 1.0 && d2 < bestKey) {
                bestKey = d2;
                text = tr("%1, key at %2: %3").arg(curve.value(QStringLiteral("title")).toString(),
                                                     key.value(QStringLiteral("timeText")).toString(),
                                                     key.value(QStringLiteral("valueText")).toString());
            }
        }
    }
    return text;
}

// ---- Numeric entry and keys ----------------------------------------------------------------------------------------

bool KeyframeGraph::setSelectionValue(double value) {
    if (!m_ec || !m_ec->m_controller || !std::isfinite(value) || m_ec->m_selectedKeys.empty())
        return false;
    ProjectController* pc = m_ec->m_controller;
    // The active curve's selected keys, else the curve of the first selected key.
    QString target = m_activeRow;
    const bool activeHasSelection = std::any_of(m_ec->m_selectedKeys.begin(), m_ec->m_selectedKeys.end(),
                                                [&](const EffectControlsModel::KeyRef& k) { return k.row == target; });
    if (!activeHasSelection)
        target = m_ec->m_selectedKeys.begin()->row;
    const EffectControlsModel::Row* row = m_ec->findRow(target);
    const Clip* clip = row ? m_ec->clipOf(*row) : nullptr;
    if (!clip)
        return false;
    ParamValue current = m_ec->rowValue(*row);
    Curve curve;
    if (const Curve* c = current.curve())
        curve = *c;
    else
        return false;
    const double model = row->range.toModel(std::clamp(value, row->from, row->to), row->frame);
    bool changed = false;
    for (const EffectControlsModel::KeyRef& k : m_ec->m_selectedKeys) {
        if (k.row != target)
            continue;
        if (const auto i = curve.keyAt(Ticks{k.curveTime}); i && curve.keys[*i].value != model) {
            curve.keys[*i].value = model;
            changed = true;
        }
    }
    if (!changed)
        return true;
    const SequenceId seq = pc->currentSequenceId();
    const ClipId clipId = clip->id;
    const edit::ParamRef ref = row->ref;
    return pc->applyEditFn(LABEL("Set Keyframe Value"),
                           [&](const ProjectState& state) { return edit::setParameter(state, seq, clipId, ref, ParamValue(curve)); });
}

bool KeyframeGraph::setSelectionTime(const QString& text) {
    if (!m_ec || !m_ec->m_controller || m_ec->m_selectedKeys.empty())
        return false;
    ProjectController* pc = m_ec->m_controller;
    const auto wanted = pc->parseTimecodeText(text);
    if (!wanted)
        return false;
    // The first of the selected keys in time.
    std::optional<Ticks> first;
    for (const EffectControlsModel::KeyRef& k : m_ec->m_selectedKeys) {
        const EffectControlsModel::Row* row = m_ec->findRow(k.row);
        const Clip* clip = row ? m_ec->clipOf(*row) : nullptr;
        const auto t = clip ? m_ec->rowTimelineTime(*row, *clip, Ticks{k.curveTime}) : std::nullopt;
        if (t && (!first || *t < *first))
            first = *t;
    }
    if (!first)
        return false;
    const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_ec->m_lane.duration.value));
    if (!m_ec->beginKeyDrag(false))
        return false;
    m_ec->dragKeys(static_cast<double>((*wanted - *first).value) / laneLength);
    m_ec->endKeyDrag();
    return true;
}

void KeyframeGraph::nudge(int frames, int valueSteps) {
    if (!m_ec || !m_ec->m_controller || m_ec->m_selectedKeys.empty())
        return;
    ProjectController* pc = m_ec->m_controller;
    if (frames != 0) {
        const double laneLength = static_cast<double>(std::max<std::int64_t>(1, m_ec->m_lane.duration.value));
        if (m_ec->beginKeyDrag(false)) {
            m_ec->dragKeys(static_cast<double>(frames) * static_cast<double>(pc->frameTicks().value) / laneLength);
            m_ec->endKeyDrag();
        }
    }
    if (valueSteps == 0)
        return;
    std::map<QString, Curve> curves;
    for (const EffectControlsModel::KeyRef& k : m_ec->m_selectedKeys) {
        const EffectControlsModel::Row* row = m_ec->findRow(k.row);
        const Clip* clip = row ? m_ec->clipOf(*row) : nullptr;
        if (!clip)
            continue;
        auto it = curves.find(k.row);
        if (it == curves.end()) {
            const ParamValue v = m_ec->rowValue(*row);
            if (!v.curve())
                continue;
            it = curves.emplace(k.row, *v.curve()).first;
        }
        if (const auto i = it->second.keyAt(Ticks{k.curveTime})) {
            Keyframe& key = it->second.keys[*i];
            const double ui = std::clamp(row->range.toUi(key.value, row->frame) + valueSteps * row->step, row->from, row->to);
            key.value = row->range.toModel(ui, row->frame);
        }
    }
    const SequenceId seq = pc->currentSequenceId();
    Steps steps;
    for (const auto& [rowKey, curve] : curves) {
        const EffectControlsModel::Row* row = m_ec->findRow(rowKey);
        const Clip* clip = row ? m_ec->clipOf(*row) : nullptr;
        if (!clip)
            continue;
        const ClipId clipId = clip->id;
        const edit::ParamRef ref = row->ref;
        const Curve copy = curve;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seq, clipId, ref, ParamValue(copy)); });
    }
    if (!steps.empty())
        m_ec->applyChainedEdit(LABEL("Set Keyframe Value"), steps);
}

int KeyframeGraph::ease(const QString& id) {
    const edit::EasingPreset* preset = edit::findEasingPreset(id.toStdString());
    if (!preset || !m_ec || !m_ec->m_controller || m_ec->m_selectedKeys.empty())
        return 0;
    ProjectController* pc = m_ec->m_controller;
    std::map<QString, Curve> curves;
    std::map<QString, std::vector<std::size_t>> indices;
    for (const EffectControlsModel::KeyRef& k : m_ec->m_selectedKeys) {
        const EffectControlsModel::Row* row = m_ec->findRow(k.row);
        const Clip* clip = row ? m_ec->clipOf(*row) : nullptr;
        if (!clip)
            continue;
        auto it = curves.find(k.row);
        if (it == curves.end()) {
            const ParamValue v = m_ec->rowValue(*row);
            if (!v.curve())
                continue;
            it = curves.emplace(k.row, *v.curve()).first;
        }
        if (const auto i = it->second.keyAt(Ticks{k.curveTime}))
            indices[k.row].push_back(*i);
    }
    int segments = 0;
    const SequenceId seq = pc->currentSequenceId();
    Steps steps;
    for (auto& [rowKey, curve] : curves) {
        std::vector<std::size_t>& list = indices[rowKey];
        std::sort(list.begin(), list.end());
        const std::size_t done = edit::applyEasing(curve, list, *preset);
        if (done == 0)
            continue;
        segments += static_cast<int>(done);
        const EffectControlsModel::Row* row = m_ec->findRow(rowKey);
        const Clip* clip = row ? m_ec->clipOf(*row) : nullptr;
        if (!clip)
            continue;
        const ClipId clipId = clip->id;
        const edit::ParamRef ref = row->ref;
        const Curve copy = curve;
        steps.push_back([=](const ProjectState& state) { return edit::setParameter(state, seq, clipId, ref, ParamValue(copy)); });
    }
    if (steps.empty() || !m_ec->applyChainedEdit(LABEL("Ease Keyframes"), steps))
        return 0;
    return segments;
}

QVariantList KeyframeGraph::easingPresets() const {
    QVariantList out;
    for (const edit::EasingPreset& p : edit::easingPresets()) {
        QVariantList thumb;
        constexpr int kThumb = 24;
        for (int i = 0; i <= kThumb; ++i) {
            const double u = static_cast<double>(i) / kThumb;
            thumb.append(u);
            thumb.append(edit::easingValue(p, u));
        }
        out.append(QVariantMap{
            {QStringLiteral("id"), QString::fromUtf8(p.id.data(), static_cast<qsizetype>(p.id.size()))},
            {QStringLiteral("name"), QCoreApplication::translate("wabcut::app::KeyframeGraph", p.name.data())},
            {QStringLiteral("group"), QString::fromUtf8(p.group.data(), static_cast<qsizetype>(p.group.size()))},
            {QStringLiteral("x1"), p.x1}, {QStringLiteral("y1"), p.y1}, {QStringLiteral("x2"), p.x2}, {QStringLiteral("y2"), p.y2},
            {QStringLiteral("thumb"), thumb},
        });
    }
    return out;
}

} // namespace wabcut::app
