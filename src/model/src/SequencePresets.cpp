// Sequence presets and the variable-frame-rate policy (C.15, gaming): SequencePresets.h, Vfr.h.

#include <wabcut/model/SequencePresets.h>
#include <wabcut/model/Vfr.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut {

namespace {

constexpr Rate k59_94{60000, 1001};
constexpr Rate k60{60, 1};
constexpr Rate k120{120, 1};
constexpr Rate k144{144, 1};
constexpr Rate k240{240, 1};
constexpr Rate k30{30, 1};

constexpr std::array<SequencePreset, 18> kPresets{{
    {"1080p30", "1080p 30 fps", "Landscape 16:9", 1920, 1080, k30},
    {"1080p60", "1080p 60 fps (YouTube, Twitch)", "Landscape 16:9", 1920, 1080, k60},
    {"1080p59.94", "1080p 59.94 fps", "Landscape 16:9", 1920, 1080, k59_94},
    {"1080p120", "1080p 120 fps", "Landscape 16:9", 1920, 1080, k120},
    {"1080p144", "1080p 144 fps", "Landscape 16:9", 1920, 1080, k144},
    {"1080p240", "1080p 240 fps", "Landscape 16:9", 1920, 1080, k240},
    {"1440p30", "1440p 30 fps", "Landscape 16:9", 2560, 1440, k30},
    {"1440p60", "1440p 60 fps (YouTube)", "Landscape 16:9", 2560, 1440, k60},
    {"1440p120", "1440p 120 fps", "Landscape 16:9", 2560, 1440, k120},
    {"1440p144", "1440p 144 fps", "Landscape 16:9", 2560, 1440, k144},
    {"1440p240", "1440p 240 fps", "Landscape 16:9", 2560, 1440, k240},
    {"2160p30", "4K 30 fps", "Landscape 16:9", 3840, 2160, k30},
    {"2160p60", "4K 60 fps (YouTube)", "Landscape 16:9", 3840, 2160, k60},
    {"2160p120", "4K 120 fps", "Landscape 16:9", 3840, 2160, k120},
    {"2160p144", "4K 144 fps", "Landscape 16:9", 3840, 2160, k144},
    {"2160p240", "4K 240 fps", "Landscape 16:9", 3840, 2160, k240},
    {"shorts", "Shorts / TikTok / Reels 1080x1920 60 fps", "Vertical 9:16", 1080, 1920, k60},
    {"shorts30", "Shorts / TikTok / Reels 1080x1920 30 fps", "Vertical 9:16", 1080, 1920, k30},
}};

static_assert(std::all_of(kPresets.begin(), kPresets.end(), [](const SequencePreset& p) { return p.frameRate.isExact(); }),
              "every sequence preset's frame is a whole number of ticks");

constexpr std::array<Rate, 16> kNominal{{{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48, 1}, {50, 1},
                                          {60000, 1001}, {60, 1}, {90, 1}, {100, 1}, {120000, 1001}, {120, 1}, {144, 1},
                                          {165, 1}, {240, 1}}};

double fps(Rate r) { return static_cast<double>(r.num) / static_cast<double>(r.den); }

} // namespace

std::span<const SequencePreset> sequencePresets() { return kPresets; }

const SequencePreset* findSequencePreset(std::string_view id) {
    for (const SequencePreset& p : kPresets) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

edit::SequenceSettings sequenceSettingsFor(const SequencePreset& preset, std::string name) {
    edit::SequenceSettings s;
    s.name = name.empty() ? std::string(preset.name) : std::move(name);
    s.width = preset.width;
    s.height = preset.height;
    s.frameRate = preset.frameRate;
    s.audioRate = kRate48kHz;
    s.dropFrame = false;
    return s;
}

Rate editingRate(const VideoStreamInfo& stream, Rate sequenceRate) {
    if (!stream.variableFrameRate && stream.frameRate.isValid() && stream.frameRate.isExact()) {
        return stream.frameRate;
    }
    return sequenceRate;
}

Ticks snapToEditingGrid(Ticks t, const VideoStreamInfo& stream, Rate sequenceRate) {
    const Rate rate = editingRate(stream, sequenceRate);
    if (!rate.isValid() || !rate.isExact()) {
        return t;
    }
    const std::int64_t unit = rate.ticksPerUnit();
    const std::int64_t q = t.value >= 0 ? t.value / unit : -((-t.value + unit - 1) / unit);
    return Ticks{q * unit};
}

std::vector<Ticks> conformedSourceTimes(const Clip& clip, Rate sequenceRate, std::int64_t firstFrame, std::int64_t count) {
    std::vector<Ticks> out;
    if (!sequenceRate.isValid() || count <= 0) {
        return out;
    }
    count = std::min<std::int64_t>(count, 1'000'000);
    for (std::int64_t n = firstFrame; n < firstFrame + count; ++n) {
        const auto t = framesToTicks(n, sequenceRate, Rounding::Floor);
        if (!t) {
            break;
        }
        if (*t < clip.start() || *t >= clip.end()) {
            continue;
        }
        out.push_back(clip.sourceTimeAt(*t));
    }
    return out;
}

Rate nominalRate(Rate average) {
    if (!average.isValid()) {
        return average;
    }
    const double a = fps(average);
    const Rate* best = nullptr;
    double bestError = 0.0;
    for (const Rate& r : kNominal) {
        const double error = std::abs(fps(r) - a) / fps(r);
        if (error <= 0.02 && (!best || error < bestError)) {
            best = &r;
            bestError = error;
        }
    }
    return best ? *best : average.normalized();
}

} // namespace wabcut
