// Generators and effects in FCPXML and OpenTimelineIO (0.6, cards #67 and #69): see InterchangeEffects.h.

#include "InterchangeEffects.h"

#include "TextUtil.h"

#include <wabcut/model/Synthetic.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace wabcut::interchange::detail {

namespace {

std::span<const EffectParamInfo> generatorParamInfos(std::string_view kind) {
    if (const SyntheticDescriptor* d = findSynthetic(kind); d && d->generator == kind) {
        return d->params;
    }
    return {};
}

// ---- the effects other editors know by name ----------------------------------------------------------------------
// Names are Final Cut Pro's and DaVinci Resolve's (and Premiere's, through FCP7 XML) for the same kind of effect.
// Settings are shared only where both sides mean the same thing; the rest stay private (wabcut:<key> parameters).

using namespace std::string_view_literals;

constexpr std::array kBlurAliases{"Gaussian Blur"sv, "Gaussian"sv, "GaussianBlur"sv, "FFGaussianBlur"sv, "Blur"sv};
constexpr ForeignParam kBlurParams[] = {{"radius", "Amount", 1.0}};
constexpr std::array kSharpenAliases{"Sharpen"sv, "FFSharpen"sv};
constexpr ForeignParam kSharpenParams[] = {{"amount", "Amount", 1.0}};
constexpr std::array kBalanceAliases{"Color Balance"sv,         "Balance Color"sv,   "Color Corrector 3-way"sv,
                                     "3-Way Color Corrector"sv, "Color Wheels"sv,    "Color Board"sv,
                                     "FFColorBalance"sv,        "Color Correction"sv, "ColorCorrection"sv};
constexpr std::array kLevelsAliases{"Levels"sv, "FFLevels"sv};
constexpr std::array kProcAmpAliases{"Proc Amp"sv, "ProcAmp"sv, "FFProcAmp"sv};
constexpr ForeignParam kProcAmpParams[] = {{"brightness", "Brightness", 1.0},
                                           {"contrast", "Contrast", 1.0},
                                           {"hue", "Hue", 1.0},
                                           {"saturation", "Saturation", 1.0}};
constexpr std::array kBrightnessAliases{"Brightness & Contrast"sv, "Brightness and Contrast"sv, "Brightness"sv};
constexpr ForeignParam kBrightnessParams[] = {{"brightness", "Brightness", 1.0}, {"contrast", "Contrast", 1.0}};
constexpr std::array kVignetteAliases{"Vignette"sv, "FFVignette"sv};
constexpr std::array kFlipAliases{"Flipped"sv, "Horizontal Flip"sv, "FFFlipped"sv};
constexpr std::array kEqAliases{"Channel EQ"sv,    "EQ"sv,          "Parametric EQ"sv, "AUNBandEQ"sv,
                                "Graphic EQ"sv,    "Fat EQ"sv,      "FFChannelEQ"sv,   "Equalizer"sv};
constexpr std::array kCompressorAliases{"Compressor"sv, "Logic Compressor"sv, "AUDynamicsProcessor"sv,
                                        "Dynamics Processor"sv, "FFCompressor"sv};
constexpr ForeignParam kCompressorParams[] = {{"threshold", "Threshold", 1.0},
                                              {"ratio", "Ratio", 1.0},
                                              {"attack", "Attack", 1.0},
                                              {"release", "Release", 1.0},
                                              {"makeup", "Makeup Gain", 1.0}};
constexpr std::array kLimiterAliases{"Limiter"sv, "Adaptive Limiter"sv, "AUPeakLimiter"sv, "Peak Limiter"sv,
                                     "FFLimiter"sv};
constexpr ForeignParam kLimiterParams[] = {{"ceiling", "Output Ceiling", 1.0}, {"release", "Release", 1.0}};

constexpr ForeignEffect kForeignEffects[] = {
    {"wabcut.blur.gaussian", "Gaussian Blur", "FFGaussianBlur", kBlurAliases, kBlurParams},
    {"wabcut.sharpen", "Sharpen", "FFSharpen", kSharpenAliases, kSharpenParams},
    {"wabcut.color.balance", "Color Balance", "FFColorBalance", kBalanceAliases, {}},
    {"wabcut.adjust.levels", "Levels", "FFLevels", kLevelsAliases, {}},
    {"wabcut.adjust.procAmp", "Proc Amp", "FFProcAmp", kProcAmpAliases, kProcAmpParams},
    {"wabcut.color.brightnessContrast", "Brightness & Contrast", "FFBrightnessContrast", kBrightnessAliases,
     kBrightnessParams},
    {"wabcut.vignette", "Vignette", "FFVignette", kVignetteAliases, {}},
    {"wabcut.transform.horizontalFlip", "Flipped", "FFFlipped", kFlipAliases, {}},
    {"wabcut.audio.eq3", "Channel EQ", "FFChannelEQ", kEqAliases, {}},
    {"wabcut.audio.compressor", "Compressor", "FFCompressor", kCompressorAliases, kCompressorParams},
    {"wabcut.audio.limiter", "Limiter", "FFLimiter", kLimiterAliases, kLimiterParams},
};

} // namespace

// ---- generators ----------------------------------------------------------------------------------------------------

std::string_view generatorDisplayName(std::string_view kind) {
    if (kind == kAdjustmentGenerator) {
        return "Adjustment Layer";
    }
    if (const SyntheticDescriptor* d = findSynthetic(kind); d && d->generator == kind) {
        return d->displayName;
    }
    return {};
}

bool generatorHasAudio(std::string_view kind) {
    const SyntheticDescriptor* d = findSynthetic(kind);
    return d && d->generator == kind && d->hasAudio;
}

std::vector<EffectParam> cleanGeneratorParams(std::string_view kind, std::vector<EffectParam> params,
                                              std::vector<std::string>& dropped) {
    const auto infos = generatorParamInfos(kind);
    std::vector<EffectParam> kept;
    for (EffectParam& p : params) {
        const auto info = std::find_if(infos.begin(), infos.end(), [&](const EffectParamInfo& i) { return i.key == p.key; });
        const bool ok = info != infos.end() && p.value.isConstant() && std::isfinite(p.value.constant()) &&
                        p.value.constant() >= info->minValue && p.value.constant() <= info->maxValue;
        const bool duplicate =
            std::any_of(kept.begin(), kept.end(), [&](const EffectParam& k) { return k.key == p.key; });
        if (!ok || duplicate) {
            dropped.push_back(p.key.substr(0, 64));
            continue;
        }
        kept.push_back(std::move(p));
    }
    std::sort(kept.begin(), kept.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    return kept;
}

double generatorParam(const IrGenerator& generator, std::string_view key) {
    for (const EffectParam& p : generator.params) {
        if (p.key == key && p.value.isConstant()) {
            return p.value.constant();
        }
    }
    for (const EffectParamInfo& info : generatorParamInfos(generator.kind)) {
        if (info.key == key) {
            return info.defaultValue;
        }
    }
    return 0.0;
}

std::string generatorKey(const IrGenerator& generator) {
    std::string key = "generator:" + generator.kind;
    for (const EffectParam& p : generator.params) {
        key += ";" + p.key + "=" + numberText(p.value.isConstant() ? p.value.constant() : 0.0);
    }
    return key;
}

IrMedia generatorMedia(IrGenerator generator, std::string name) {
    IrMedia m;
    m.key = generatorKey(generator);
    m.name = name.empty() ? std::string(generatorDisplayName(generator.kind)) : std::move(name);
    m.reference.name = m.name;
    m.hasVideo = true;
    const bool leader = generator.kind == kCountingLeaderGenerator;
    m.stillImage = !leader;
    if (leader) {
        m.duration = kCountingLeaderLength;
    }
    if (generatorHasAudio(generator.kind)) {
        m.audioChannels.push_back(2);
    }
    m.generator = std::move(generator);
    return m;
}

// ---- effects -------------------------------------------------------------------------------------------------------

const ForeignEffect* foreignEffectFor(std::string_view wabcutId) {
    for (const ForeignEffect& e : kForeignEffects) {
        if (e.wabcut == wabcutId) {
            return &e;
        }
    }
    return nullptr;
}

std::string wabcutEffectNamed(std::string_view name, std::string_view uid, EffectKind kind) {
    const EffectRegistry& registry = EffectRegistry::builtin();
    const auto kindOf = [&](std::string_view id) {
        const EffectDescriptor* d = registry.find(id);
        return d && !d->intrinsic && d->kind == kind;
    };
    const std::string_view trimmedName = trim(name);
    const std::string_view trimmedUid = trim(uid);
    for (const ForeignEffect& e : kForeignEffects) {
        if (!kindOf(e.wabcut)) {
            continue;
        }
        const bool uidMatch = !trimmedUid.empty() && iequals(trimmedUid, e.fcpxUid);
        const bool nameMatch = !trimmedName.empty() && (iequals(trimmedName, e.name) ||
                                                        std::any_of(e.aliases.begin(), e.aliases.end(), [&](std::string_view a) {
                                                            return iequals(trimmedName, a);
                                                        }));
        if (uidMatch || nameMatch) {
            return std::string(e.wabcut);
        }
    }
    if (trimmedName.empty()) {
        return {};
    }
    for (const EffectDescriptor& d : registry.all()) {
        if (!d.intrinsic && d.kind == kind && (iequals(d.displayName, trimmedName) || iequals(d.id, trimmedName))) {
            return std::string(d.id);
        }
    }
    return {};
}

bool isForeignPlaceholder(const EffectInstance& e) {
    return e.effect.starts_with(kFcpxPlaceholder) || e.effect.starts_with(kOtioPlaceholder);
}

std::string effectLabel(const EffectInstance& e) {
    if (const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect)) {
        return std::string(d->displayName);
    }
    if (const std::string name = dataText(e, kSourceName); !name.empty()) {
        return name;
    }
    return e.effect;
}

std::string dataText(const EffectInstance& e, std::string_view key) {
    if (const EffectData* d = e.findData(key)) {
        if (const std::string* text = d->text()) {
            return *text;
        }
    }
    return {};
}

bool addDataText(EffectInstance& e, std::string key, std::string text) {
    if (key.empty() || text.empty() || text.size() > kMaxEffectText) {
        return false;
    }
    if (!e.findData(key) && e.data.size() >= kMaxEffectDataEntries) {
        return false;
    }
    e.setData(EffectData{std::move(key), std::move(text)});
    return true;
}

std::string numberText(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(v));
    }
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.10g", v);
    if (std::strtod(buffer, nullptr) != v) {
        std::snprintf(buffer, sizeof buffer, "%.17g", v); // exact: values are read back bit for bit
    }
    return buffer;
}

} // namespace wabcut::interchange::detail
