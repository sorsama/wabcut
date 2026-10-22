// Sequence presets and the variable-frame-rate policy (C.15, gaming): SequencePresets.h, Vfr.h.

#include <wabcut/model/SequencePresets.h>
#include <wabcut/model/Vfr.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut {

namespace {

constexpr Rate k23_976{24000, 1001};
constexpr Rate k24{24, 1};
constexpr Rate k25{25, 1};
constexpr Rate k29_97{30000, 1001};
constexpr Rate k30{30, 1};
constexpr Rate k50{50, 1};
constexpr Rate k59_94{60000, 1001};
constexpr Rate k60{60, 1};
constexpr Rate k120{120, 1};
constexpr Rate k144{144, 1};
constexpr Rate k240{240, 1};

constexpr std::string_view kLandscape = "Landscape 16:9";
constexpr std::string_view kVertical = "Vertical 9:16";
constexpr std::string_view kSquare = "Square 1:1";

// C.15's gaming presets keep their ids and names; P.2 adds Premiere's broadcast and film rates, 720p, the drop-frame
// variants and the square frame.
constexpr std::array<SequencePreset, 61> kPresets{{
    // 720p
    {"720p23.976", "720p 23.976 fps", kLandscape, 1280, 720, k23_976},
    {"720p24", "720p 24 fps", kLandscape, 1280, 720, k24},
    {"720p25", "720p 25 fps", kLandscape, 1280, 720, k25},
    {"720p29.97", "720p 29.97 fps", kLandscape, 1280, 720, k29_97},
    {"720p30", "720p 30 fps", kLandscape, 1280, 720, k30},
    {"720p50", "720p 50 fps", kLandscape, 1280, 720, k50},
    {"720p59.94", "720p 59.94 fps", kLandscape, 1280, 720, k59_94},
    {"720p60", "720p 60 fps", kLandscape, 1280, 720, k60},
    // 1080p
    {"1080p23.976", "1080p 23.976 fps", kLandscape, 1920, 1080, k23_976},
    {"1080p24", "1080p 24 fps", kLandscape, 1920, 1080, k24},
    {"1080p25", "1080p 25 fps", kLandscape, 1920, 1080, k25},
    {"1080p29.97", "1080p 29.97 fps", kLandscape, 1920, 1080, k29_97},
    {"1080p29.97df", "1080p 29.97 fps drop-frame", kLandscape, 1920, 1080, k29_97, true},
    {"1080p30", "1080p 30 fps", kLandscape, 1920, 1080, k30},
    {"1080p50", "1080p 50 fps", kLandscape, 1920, 1080, k50},
    {"1080p59.94", "1080p 59.94 fps", kLandscape, 1920, 1080, k59_94},
    {"1080p59.94df", "1080p 59.94 fps drop-frame", kLandscape, 1920, 1080, k59_94, true},
    {"1080p60", "1080p 60 fps (YouTube, Twitch)", kLandscape, 1920, 1080, k60},
    {"1080p120", "1080p 120 fps", kLandscape, 1920, 1080, k120},
    {"1080p144", "1080p 144 fps", kLandscape, 1920, 1080, k144},
    {"1080p240", "1080p 240 fps", kLandscape, 1920, 1080, k240},
    // 1440p
    {"1440p23.976", "1440p 23.976 fps", kLandscape, 2560, 1440, k23_976},
    {"1440p24", "1440p 24 fps", kLandscape, 2560, 1440, k24},
    {"1440p25", "1440p 25 fps", kLandscape, 2560, 1440, k25},
    {"1440p29.97", "1440p 29.97 fps", kLandscape, 2560, 1440, k29_97},
    {"1440p30", "1440p 30 fps", kLandscape, 2560, 1440, k30},
    {"1440p50", "1440p 50 fps", kLandscape, 2560, 1440, k50},
    {"1440p59.94", "1440p 59.94 fps", kLandscape, 2560, 1440, k59_94},
    {"1440p60", "1440p 60 fps (YouTube)", kLandscape, 2560, 1440, k60},
    {"1440p120", "1440p 120 fps", kLandscape, 2560, 1440, k120},
    {"1440p144", "1440p 144 fps", kLandscape, 2560, 1440, k144},
    {"1440p240", "1440p 240 fps", kLandscape, 2560, 1440, k240},
    // UHD 2160p
    {"2160p23.976", "4K 23.976 fps", kLandscape, 3840, 2160, k23_976},
    {"2160p24", "4K 24 fps", kLandscape, 3840, 2160, k24},
    {"2160p25", "4K 25 fps", kLandscape, 3840, 2160, k25},
    {"2160p29.97", "4K 29.97 fps", kLandscape, 3840, 2160, k29_97},
    {"2160p29.97df", "4K 29.97 fps drop-frame", kLandscape, 3840, 2160, k29_97, true},
    {"2160p30", "4K 30 fps", kLandscape, 3840, 2160, k30},
    {"2160p50", "4K 50 fps", kLandscape, 3840, 2160, k50},
    {"2160p59.94", "4K 59.94 fps", kLandscape, 3840, 2160, k59_94},
    {"2160p59.94df", "4K 59.94 fps drop-frame", kLandscape, 3840, 2160, k59_94, true},
    {"2160p60", "4K 60 fps (YouTube)", kLandscape, 3840, 2160, k60},
    {"2160p120", "4K 120 fps", kLandscape, 3840, 2160, k120},
    {"2160p144", "4K 144 fps", kLandscape, 3840, 2160, k144},
    {"2160p240", "4K 240 fps", kLandscape, 3840, 2160, k240},
    // Vertical 1080x1920
    {"vertical23.976", "Vertical 1080x1920 23.976 fps", kVertical, 1080, 1920, k23_976},
    {"vertical24", "Vertical 1080x1920 24 fps", kVertical, 1080, 1920, k24},
    {"vertical25", "Vertical 1080x1920 25 fps", kVertical, 1080, 1920, k25},
    {"vertical29.97", "Vertical 1080x1920 29.97 fps", kVertical, 1080, 1920, k29_97},
    {"shorts30", "Shorts / TikTok / Reels 1080x1920 30 fps", kVertical, 1080, 1920, k30},
    {"vertical50", "Vertical 1080x1920 50 fps", kVertical, 1080, 1920, k50},
    {"vertical59.94", "Vertical 1080x1920 59.94 fps", kVertical, 1080, 1920, k59_94},
    {"shorts", "Shorts / TikTok / Reels 1080x1920 60 fps", kVertical, 1080, 1920, k60},
    {"vertical120", "Vertical 1080x1920 120 fps", kVertical, 1080, 1920, k120},
    // Square 1080x1080
    {"square1080p24", "Square 1080x1080 24 fps", kSquare, 1080, 1080, k24},
    {"square1080p25", "Square 1080x1080 25 fps", kSquare, 1080, 1080, k25},
    {"square1080p29.97", "Square 1080x1080 29.97 fps", kSquare, 1080, 1080, k29_97},
    {"square1080p30", "Square 1080x1080 30 fps", kSquare, 1080, 1080, k30},
    {"square1080p50", "Square 1080x1080 50 fps", kSquare, 1080, 1080, k50},
    {"square1080p59.94", "Square 1080x1080 59.94 fps", kSquare, 1080, 1080, k59_94},
    {"square1080p60", "Square 1080x1080 60 fps", kSquare, 1080, 1080, k60},
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
    s.dropFrame = preset.dropFrame;
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
