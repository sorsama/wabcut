#include <wabcut/model/MotionBlur.h>

#include <algorithm>
#include <cmath>

namespace wabcut {

ShutterSettings shutterSettingsOf(const EffectInstance& instance, Ticks curveTime) {
    ShutterSettings s;
    s.angle = std::clamp(effectParameterAt(instance, "shutterAngle", curveTime, s.angle), 0.0, 720.0);
    s.phase = std::clamp(effectParameterAt(instance, "shutterPhase", curveTime, s.phase), -360.0, 360.0);
    const double samples = effectParameterAt(instance, "samples", curveTime, static_cast<double>(s.samples));
    s.samples = std::clamp(static_cast<int>(std::lround(samples)), 2, kMaxBlurSamples);
    const double preview = effectParameterAt(instance, "previewSamples", curveTime, static_cast<double>(s.previewSamples));
    s.previewSamples = std::clamp(static_cast<int>(std::lround(preview)), 2, kMaxBlurSamples);
    return s;
}

std::optional<ShutterSettings> motionBlurOf(std::span<const EffectInstance> stack, Ticks curveTime) {
    for (const EffectInstance& instance : stack) {
        if (instance.effect != kMotionBlurEffect || !instance.enabled || !isEvaluable(instance, EffectKind::Video)) {
            continue;
        }
        const ShutterSettings s = shutterSettingsOf(instance, curveTime);
        if (s.angle > 0.0) {
            return s;
        }
        return std::nullopt; // the first one decides: an angle of 0 turns the blur off
    }
    return std::nullopt;
}

std::vector<double> shutterOffsets(const ShutterSettings& settings, int count) {
    std::vector<double> out;
    if (count < 1) {
        return out;
    }
    if (!(settings.angle > 0.0)) {
        out.push_back(0.0);
        return out;
    }
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out.push_back((settings.phase + settings.angle * (static_cast<double>(i) + 0.5) / count) / 360.0);
    }
    return out;
}

int blurSampleCount(const ShutterSettings& settings, double travelPixels, bool preview) {
    if (!(travelPixels >= 0.25) || !(settings.angle > 0.0)) {
        return 1;
    }
    const int wanted = static_cast<int>(std::ceil(travelPixels / 1.5)) + 1;
    return std::clamp(wanted, 2, std::clamp(settings.cap(preview), 2, kMaxBlurSamples));
}

} // namespace wabcut
