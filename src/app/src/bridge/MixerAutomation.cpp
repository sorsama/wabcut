#include "MixerAutomation.h"

#include <algorithm>
#include <cmath>

namespace wabcut::app {

QString automationModeName(AutomationMode mode) {
    switch (mode) {
    case AutomationMode::Touch:
        return QStringLiteral("touch");
    case AutomationMode::Latch:
        return QStringLiteral("latch");
    case AutomationMode::Write:
        return QStringLiteral("write");
    case AutomationMode::Read:
        break;
    }
    return QStringLiteral("read");
}

std::optional<AutomationMode> automationModeFromName(const QString& name) {
    if (name == QStringLiteral("read"))
        return AutomationMode::Read;
    if (name == QStringLiteral("touch"))
        return AutomationMode::Touch;
    if (name == QStringLiteral("latch"))
        return AutomationMode::Latch;
    if (name == QStringLiteral("write"))
        return AutomationMode::Write;
    return std::nullopt;
}

std::vector<std::pair<Ticks, double>> thinAutomation(std::span<const std::pair<Ticks, double>> samples,
                                                      double tolerance) {
    std::vector<std::pair<Ticks, double>> out;
    if (samples.empty())
        return out;
    out.push_back(samples.front());
    for (std::size_t i = 1; i + 1 < samples.size(); ++i) {
        // Keep sample i when the line from the last kept sample to the next one misses it.
        const auto& [t0, v0] = out.back();
        const auto& [t1, v1] = samples[i];
        const auto& [t2, v2] = samples[i + 1];
        const double span = static_cast<double>((t2 - t0).value);
        const double predicted = span > 0 ? v0 + (v2 - v0) * static_cast<double>((t1 - t0).value) / span : v0;
        if (std::abs(predicted - v1) > tolerance)
            out.push_back(samples[i]);
    }
    if (samples.size() > 1)
        out.push_back(samples.back());
    return out;
}

ParamValue applyAutomationPass(const ParamValue& existing, std::span<const AutomationSegment> segments,
                               double tolerance) {
    std::vector<Keyframe> keys;
    if (const Curve* curve = existing.curve())
        keys = curve->keys;
    bool wrote = false;
    for (const AutomationSegment& segment : segments) {
        if (segment.samples.empty() || segment.end < segment.start)
            continue;
        wrote = true;
        const Ticks from = segment.start - kAutomationAnchorGap;
        const Ticks to = segment.end + kAutomationAnchorGap;
        // The old automation at the anchors, read before this segment replaces anything.
        const ParamValue before = keys.empty() ? existing : ParamValue(Curve{keys});
        const double oldFrom = before.valueAt(from);
        const double oldTo = before.valueAt(to);
        std::erase_if(keys, [&](const Keyframe& k) { return k.time >= from && k.time <= to; });
        const bool leadingAnchor = from.value >= 0;
        if (leadingAnchor)
            keys.push_back(Keyframe{from, oldFrom});
        for (const auto& [time, value] : thinAutomation(segment.samples, tolerance))
            keys.push_back(Keyframe{std::clamp(time, segment.start, segment.end), value});
        keys.push_back(Keyframe{to, oldTo});
        std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
        // Strictly increasing times: a later key at the same time wins (the written value over an anchor).
        std::vector<Keyframe> unique;
        unique.reserve(keys.size());
        for (const Keyframe& k : keys) {
            if (!unique.empty() && unique.back().time == k.time)
                unique.back() = k;
            else
                unique.push_back(k);
        }
        keys = std::move(unique);
    }
    if (!wrote)
        return existing;
    return ParamValue(Curve{std::move(keys)});
}

} // namespace wabcut::app
