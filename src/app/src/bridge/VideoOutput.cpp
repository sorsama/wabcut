// Video output to another display (VideoOutput.h, 0.7 card 110).

#include "VideoOutput.h"

#include <algorithm>
#include <cmath>

namespace wabcut::app::videoout {

namespace {

/// How far (relative) a refresh-per-frame ratio may be from a whole number and still count as that number: 1001 / 1000 is the
/// distance between 23.976 and 24, 29.97 and 30, 59.94 and 60 (0.1 %).
constexpr double kNearWhole = 0.0015;

} // namespace

std::string toString(Scaling scaling) {
    switch (scaling) {
    case Scaling::Fill:
        return "fill";
    case Scaling::Native:
        return "native";
    case Scaling::Fit:
        break;
    }
    return "fit";
}

Scaling scalingFromString(const std::string& text) {
    if (text == "fill") {
        return Scaling::Fill;
    }
    if (text == "native") {
        return Scaling::Native;
    }
    return Scaling::Fit;
}

int clampSyncOffset(int ms) { return std::clamp(ms, -kMaxSyncOffsetMs, kMaxSyncOffsetMs); }

Rect placePicture(const Timing& timing, int displayWidth, int displayHeight, Scaling scaling) {
    const double dw = std::max(0, displayWidth);
    const double dh = std::max(0, displayHeight);
    // The picture as it is shown is `num` wide for `den` high (the frame's width times its pixels' shape, by its height); the
    // products are taken before the divisions so a frame of the display's own shape comes out whole.
    const double num = static_cast<double>(timing.width) * timing.pixelAspect;
    const double den = timing.height;
    if (dw <= 0 || dh <= 0 || num <= 0 || den <= 0) {
        return {};
    }
    double w = 0;
    double h = 0;
    switch (scaling) {
    case Scaling::Fill:
        // Cover the display: the width that fits the height, or the whole width, whichever is bigger.
        w = std::max(dw, dh * num / den);
        h = w * den / num;
        break;
    case Scaling::Native:
        w = num;
        h = den;
        break;
    case Scaling::Fit:
        w = std::min(dw, dh * num / den);
        h = w * den / num;
        break;
    }
    return Rect{(dw - w) / 2, (dh - h) / 2, w, h};
}

CadenceInfo analyzeCadence(double fps, double refreshHz) {
    CadenceInfo info;
    if (!(fps > 0) || !(refreshHz > 0)) {
        return info;
    }
    info.ratio = refreshHz / fps;
    const double whole = std::round(info.ratio);
    if (whole >= 1 && std::abs(info.ratio - whole) < 1e-9 * whole) {
        info.kind = Cadence::Even;
        info.multiple = static_cast<int>(whole);
    } else if (whole >= 1 && std::abs(info.ratio - whole) <= kNearWhole * whole) {
        // 23.976 on 24 Hz, 59.94 on 60 Hz, 29.97 on 30: a frame is repeated or dropped now and then.
        info.kind = Cadence::NearEven;
        info.multiple = static_cast<int>(whole);
    } else if (std::abs(info.ratio * 2 - std::round(info.ratio * 2)) <= 0.002 * info.ratio * 2 &&
               static_cast<long long>(std::round(info.ratio * 2)) % 2 == 1) {
        info.kind = Cadence::Pulldown; // n + 1/2 refreshes per frame: frames alternate n and n + 1
    } else {
        info.kind = Cadence::Uneven;
    }
    return info;
}

std::optional<DisplayMode> bestModeFor(double fps, const DisplayMode& current, std::span<const DisplayMode> modes) {
    if (!(fps > 0)) {
        return std::nullopt;
    }
    const auto suits = [&](double hz) {
        const double ratio = hz / fps;
        const double whole = std::round(ratio);
        return whole >= 1 && std::abs(ratio - whole) <= kNearWhole * whole;
    };
    if (current.refreshHz > 0 && suits(current.refreshHz)) {
        return std::nullopt; // it already runs at a multiple
    }
    std::optional<DisplayMode> best;
    for (const DisplayMode& m : modes) {
        if (m.width != current.width || m.height != current.height || !suits(m.refreshHz)) {
            continue;
        }
        if (!best || m.refreshHz < best->refreshHz) {
            best = m;
        }
    }
    return best;
}

int findDisplay(std::span<const DisplayInfo> displays, const std::string& key) {
    if (key.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < displays.size(); ++i) {
        if (displays[i].key == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Plan planOutput(std::span<const DisplayInfo> displays, const Settings& settings, const Timing& timing,
                std::span<const DisplayMode> modes) {
    Plan plan;
    plan.syncOffsetMs = clampSyncOffset(settings.syncOffsetMs);
    plan.display.key = settings.displayKey;
    if (!settings.enabled) {
        plan.state = State::Off;
    } else if (settings.displayKey.empty()) {
        plan.state = State::NoDisplay;
    }
    const int index = findDisplay(displays, settings.displayKey);
    if (index >= 0) {
        plan.displayIndex = index;
        plan.display = displays[static_cast<std::size_t>(index)];
    }
    if (settings.enabled && !settings.displayKey.empty()) {
        plan.state = index >= 0 ? State::Active : State::DisplayMissing;
    }
    // What is known of the display and the sequence is planned even while the output is off, so the settings can say how the
    // rate would sit before it is switched on.
    if (index >= 0) {
        plan.picture = placePicture(timing, plan.display.width, plan.display.height, settings.scaling);
        plan.cadence = analyzeCadence(timing.fps(), plan.display.refreshHz);
        if (settings.matchRefreshRate && plan.cadence.kind != Cadence::Even) {
            plan.switchTo = bestModeFor(timing.fps(), DisplayMode{plan.display.width, plan.display.height, plan.display.refreshHz}, modes);
        }
    }
    return plan;
}

} // namespace wabcut::app::videoout
