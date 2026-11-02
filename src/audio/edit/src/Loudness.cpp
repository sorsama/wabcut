#include <wabcut/audio/edit/Loudness.h>

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/EssentialSound.h>

#include <algorithm>
#include <array>
#include <optional>

namespace wabcut::audio::edit {

namespace {

constexpr std::array<LoudnessPreset, 6> kPresets{{
    {"youtube", "YouTube", -14.0, -1.0},
    {"spotify", "Spotify (music)", -14.0, -1.0},
    {"podcast", "Spotify / podcasts", -16.0, -1.0},
    {"applePodcasts", "Apple Podcasts", -16.0, -1.0},
    {"broadcast", "Broadcast (EBU R128)", -23.0, -1.0},
    {"atsc", "Broadcast (ATSC A/85)", -24.0, -2.0},
}};

} // namespace

std::span<const LoudnessPreset> loudnessPresets() { return kPresets; }

const LoudnessPreset* findLoudnessPreset(std::string_view id) {
    for (const LoudnessPreset& p : kPresets) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

std::size_t normalizationGainIndex(const EffectStack& stack) {
    return !stack.empty() && stack.front().effect == kEssentialSoundEffect ? 1 : 0;
}

const EffectInstance* normalizationGain(const EffectStack& stack) {
    const std::size_t at = normalizationGainIndex(stack);
    if (stack.size() <= at) {
        return nullptr;
    }
    const EffectInstance& first = stack[at];
    if (first.effect != kGainEffect || !first.enabled) {
        return nullptr;
    }
    const ParamValue* gain = first.find("gain");
    return gain == nullptr || gain->isConstant() ? &first : nullptr;
}

double nextNormalizationGainDb(double currentDb, double deltaDb) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(kGainEffect);
    const EffectParamInfo* info = d != nullptr ? d->param("gain") : nullptr;
    const double lo = info != nullptr ? info->minValue : -96.0;
    const double hi = info != nullptr ? info->maxValue : 24.0;
    return std::clamp(currentDb + deltaDb, lo, hi);
}

// ---- 0.6 (P1 round, #54): true-peak overs ------------------------------------------------------------------------

std::vector<TruePeakOver> truePeakOvers(const LoudnessStats& stats, double ceilingDbtp, std::size_t maxOvers, std::size_t* total) {
    const double ceiling = std::max(ceilingDbtp, stats.peakFloorDbtp);
    std::vector<TruePeakOver> out;
    std::size_t count = 0;
    std::optional<TruePeakOver> open;
    Ticks lastLength{};
    const auto close = [&] {
        if (open) {
            ++count;
            if (out.size() < maxOvers) {
                out.push_back(*open);
            }
            open.reset();
        }
    };
    for (const LoudnessStats::PeakBlock& b : stats.peakBlocks) {
        if (b.truePeakDbtp <= ceiling) {
            continue;
        }
        const TimeRange block{stats.start + b.at, b.duration};
        // A gap of a block or less (a beat between two hot hits) does not split an over.
        if (open && block.start <= open->range.end() + std::max(lastLength, b.duration)) {
            open->range = TimeRange::fromStartEnd(open->range.start, std::max(open->range.end(), block.end()));
            open->truePeakDbtp = std::max(open->truePeakDbtp, b.truePeakDbtp);
        } else {
            close();
            open = TruePeakOver{block, b.truePeakDbtp};
        }
        lastLength = b.duration;
    }
    close();
    if (total) {
        *total = count;
    }
    return out;
}

} // namespace wabcut::audio::edit
