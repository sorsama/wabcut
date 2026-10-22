// Synthetic media and adjustment layers (C.15; Bars and Tone and the counting leader P.2): descriptors, new items,
// checks, the bars and leader layouts, their sound, and the edits that make them.

#include <wabcut/model/Synthetic.h>

#include <wabcut/model/ChangeSet.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace wabcut {

namespace {

using U = ParamUnit;

constexpr std::array<EffectParamInfo, 3> kColorMatteParams{{
    {"color.r", "Red", U::Fraction, 1.0, 0.0, 1.0, false},
    {"color.g", "Green", U::Fraction, 1.0, 0.0, 1.0, false},
    {"color.b", "Blue", U::Fraction, 1.0, 0.0, 1.0, false},
}};

// P.2: Premiere's Bars and Tone (SD, SMPTE EG 1) and HD Bars and Tone (SMPTE RP 219) are one item with a pattern.
constexpr std::array<EffectParamInfo, 3> kBarsParams{{
    {"pattern", "Pattern (0 HD SMPTE RP 219, 1 SD SMPTE EG 1)", U::Amount, 0.0, 0.0, 1.0, false},
    {"tone.frequency", "Tone Frequency", U::Hertz, 1000.0, 20.0, 20000.0, false},
    {"tone.level", "Tone Level (dBFS)", U::Decibels, -20.0, -60.0, 0.0, false},
}};

// P.2: the Universal Counting Leader dialog's settings, in its order.
constexpr std::array<EffectParamInfo, 18> kLeaderParams{{
    {"wipe.r", "Wipe Color Red", U::Fraction, 0.6, 0.0, 1.0, false},
    {"wipe.g", "Wipe Color Green", U::Fraction, 0.6, 0.0, 1.0, false},
    {"wipe.b", "Wipe Color Blue", U::Fraction, 0.6, 0.0, 1.0, false},
    {"background.r", "Background Color Red", U::Fraction, 0.35, 0.0, 1.0, false},
    {"background.g", "Background Color Green", U::Fraction, 0.35, 0.0, 1.0, false},
    {"background.b", "Background Color Blue", U::Fraction, 0.35, 0.0, 1.0, false},
    {"line.r", "Line Color Red", U::Fraction, 0.0, 0.0, 1.0, false},
    {"line.g", "Line Color Green", U::Fraction, 0.0, 0.0, 1.0, false},
    {"line.b", "Line Color Blue", U::Fraction, 0.0, 0.0, 1.0, false},
    {"target.r", "Target Color Red", U::Fraction, 1.0, 0.0, 1.0, false},
    {"target.g", "Target Color Green", U::Fraction, 1.0, 0.0, 1.0, false},
    {"target.b", "Target Color Blue", U::Fraction, 1.0, 0.0, 1.0, false},
    {"numeral.r", "Numeral Color Red", U::Fraction, 0.0, 0.0, 1.0, false},
    {"numeral.g", "Numeral Color Green", U::Fraction, 0.0, 0.0, 1.0, false},
    {"numeral.b", "Numeral Color Blue", U::Fraction, 0.0, 0.0, 1.0, false},
    {"cueBlipOnOut", "Cue Blip on Out", U::Amount, 0.0, 0.0, 1.0, false},
    {"cueBlipOn2", "Cue Blip on 2", U::Amount, 1.0, 0.0, 1.0, false},
    {"cueBlipAllSeconds", "Cue Blip at All Second Starts", U::Amount, 0.0, 0.0, 1.0, false},
}};

// Premiere's File > New menu order.
constexpr std::array<SyntheticDescriptor, 5> kKinds{{
    {SyntheticKind::BarsAndTone, kBarsAndToneGenerator, "barsAndTone", "Bars and Tone", kBarsParams, true,
     "SMPTE colour bars (HD RP 219 or SD EG 1) with a 1 kHz reference tone at -20 dBFS: line-up for monitors and "
     "meters at the head of a master."},
    {SyntheticKind::BlackVideo, kBlackVideoGenerator, "blackVideo", "Black Video", {}, false,
     "Opaque black: a slug or a base layer."},
    {SyntheticKind::ColorMatte, kColorMatteGenerator, "colorMatte", "Color Matte", kColorMatteParams, false,
     "A solid colour frame (color.r, color.g, color.b: 0..1, the sequence's encoding): a background for text, a flash."},
    {SyntheticKind::CountingLeader, kCountingLeaderGenerator, "countingLeader", "Universal Counting Leader", kLeaderParams,
     true,
     "The universal counting leader: 8 seconds counting from 8 to 2 with a clock wipe, a one-frame 1 kHz blip on the 2 "
     "(the 2-pop) and black to picture start. Colours and cue blips as Premiere's dialog."},
    {SyntheticKind::TransparentVideo, kTransparentVideoGenerator, "transparentVideo", "Transparent Video", {}, false,
     "A fully transparent frame: a layer of its own for effects that draw (outlines, overlays)."},
}};

bool validFormat(const SyntheticFormat& f) {
    return f.width >= 1 && f.width <= 16384 && f.height >= 1 && f.height <= 16384 && f.frameRate.isValid() &&
           f.audioRate.isValid();
}

/// Checks `params` against `infos`: known, constant, finite, in range; sorted and unique (after sorting when asked).
std::optional<std::string> paramsProblem(std::vector<EffectParam>& params, std::span<const EffectParamInfo> infos,
                                         bool sortFirst) {
    if (sortFirst) {
        std::sort(params.begin(), params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const EffectParam& p = params[i];
        if (i > 0 && !(params[i - 1].key < p.key)) {
            return "parameter '" + p.key.substr(0, 64) + "' is listed twice (or the list is not sorted)";
        }
        const auto info = std::find_if(infos.begin(), infos.end(), [&](const EffectParamInfo& in) { return in.key == p.key; });
        if (info == infos.end()) {
            return "unknown parameter '" + p.key.substr(0, 64) + "'";
        }
        if (!p.value.isConstant()) {
            return "parameter '" + p.key + "' cannot be keyframed";
        }
        const double v = p.value.constant();
        if (!std::isfinite(v) || v < info->minValue || v > info->maxValue) {
            return "parameter '" + p.key + "' is outside " + std::to_string(info->minValue) + ".." +
                   std::to_string(info->maxValue);
        }
    }
    return std::nullopt;
}

MediaItem generatorItem(std::string_view kind, std::vector<EffectParam> params, const SyntheticFormat& f, AlphaMode alpha,
                        std::string name) {
    MediaItem item;
    item.name = std::move(name);
    item.generator = GeneratorSpec{std::string(kind), {}, {}, {}, std::move(params)};
    item.info.isStillImage = true; // unbounded: clips of it can be any length
    VideoStreamInfo v;
    v.index = 0;
    v.codec = std::string(kind);
    v.width = f.width;
    v.height = f.height;
    v.frameRate = f.frameRate;
    v.alpha = alpha;
    item.info.video.push_back(std::move(v));
    return item;
}

/// frac(frequency * n / rate) without forming a huge product (n split into whole seconds and a rest).
double cycles(double frequency, std::int64_t n, Rate rate) {
    const double perSecond = static_cast<double>(rate.num) / static_cast<double>(rate.den);
    const auto whole = static_cast<std::int64_t>(std::floor(static_cast<double>(n) / perSecond));
    const double rest = static_cast<double>(n) - static_cast<double>(whole) * perSecond;
    double phase = frequency * static_cast<double>(whole);
    phase -= std::floor(phase);
    phase += frequency * rest / perSecond;
    return phase - std::floor(phase);
}

Rate leaderRate(const MediaItem& item) {
    return item.info.video.empty() || !item.info.video.front().frameRate.isValid() ? kRate30 : item.info.video.front().frameRate;
}

} // namespace

std::span<const SyntheticDescriptor> syntheticKinds() { return kKinds; }

const SyntheticDescriptor* findSynthetic(std::string_view kindOrShortName) {
    for (const SyntheticDescriptor& d : kKinds) {
        if (d.generator == kindOrShortName || d.shortName == kindOrShortName) {
            return &d;
        }
    }
    return nullptr;
}

const SyntheticDescriptor* syntheticOf(const MediaItem& item) {
    return item.generator ? findSynthetic(item.generator->kind) : nullptr;
}

bool isAdjustmentLayer(const MediaItem& item) { return item.generator && item.generator->kind == kAdjustmentGenerator; }

std::expected<MediaItem, Error> syntheticMediaItem(SyntheticKind kind, std::vector<EffectParam> params,
                                                   const SyntheticFormat& format, std::string name) {
    const auto d = std::find_if(kKinds.begin(), kKinds.end(), [&](const SyntheticDescriptor& x) { return x.kind == kind; });
    if (d == kKinds.end()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "unknown synthetic kind"});
    }
    if (!validFormat(format)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a synthetic item needs a frame size of 1..16384 and positive rates"});
    }
    if (const auto problem = paramsProblem(params, d->params, true)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::string(d->displayName) + ": " + *problem});
    }
    const AlphaMode alpha = kind == SyntheticKind::TransparentVideo ? AlphaMode::Straight : AlphaMode::None;
    MediaItem item = generatorItem(d->generator, std::move(params), format, alpha,
                                   name.empty() ? std::string(d->displayName) : std::move(name));
    if (d->hasAudio) {
        AudioStreamInfo a;
        a.index = 1;
        a.codec = std::string(d->generator);
        a.sampleRate = format.audioRate;
        a.channels = 2;
        a.channelLayout = "stereo";
        item.info.audio.push_back(std::move(a));
    }
    if (kind == SyntheticKind::CountingLeader) {
        // A fixed length, like a file: 8 seconds of picture and sound.
        item.info.isStillImage = false;
        item.info.duration = kCountingLeaderLength;
        item.info.video.front().duration = kCountingLeaderLength;
        item.info.audio.front().duration = kCountingLeaderLength;
    }
    return item;
}

std::expected<MediaItem, Error> adjustmentMediaItem(const SyntheticFormat& format, std::string name) {
    if (!validFormat(format)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "an adjustment layer needs a frame size of 1..16384 and positive rates"});
    }
    return generatorItem(kAdjustmentGenerator, {}, format, AlphaMode::Straight,
                         name.empty() ? std::string("Adjustment Layer") : std::move(name));
}

double syntheticParam(const MediaItem& item, std::string_view key, double fallback) {
    if (item.generator) {
        for (const EffectParam& p : item.generator->params) {
            if (p.key == key && p.value.isConstant()) {
                return p.value.constant();
            }
        }
    }
    if (const SyntheticDescriptor* d = syntheticOf(item)) {
        for (const EffectParamInfo& info : d->params) {
            if (info.key == key) {
                return info.defaultValue;
            }
        }
    }
    return fallback;
}

std::optional<std::string> syntheticProblem(const MediaItem& item) {
    if (!item.generator) {
        return std::nullopt;
    }
    const bool adjustment = item.generator->kind == kAdjustmentGenerator;
    const SyntheticDescriptor* d = syntheticOf(item);
    if (!adjustment && !d) {
        // Titles, graphics and unknown kinds are checked elsewhere (or kept as they are); parameters stay bounded.
        if (item.generator->params.size() > 64) {
            return "has more than 64 generator parameters";
        }
        return std::nullopt;
    }
    const std::string what = adjustment ? std::string("an adjustment layer") : "a " + std::string(d->displayName) + " item";
    const bool sound = d && d->hasAudio;
    if (item.info.video.size() != 1 || item.info.audio.size() != (sound ? 1u : 0u)) {
        return "is " + what + " with the wrong streams";
    }
    if (d && d->kind == SyntheticKind::CountingLeader) {
        if (item.info.isStillImage || item.info.duration != kCountingLeaderLength) {
            return "is a counting leader that is not 8 seconds long";
        }
    } else if (!item.info.isStillImage) {
        return "is " + what + " but not an unbounded picture";
    }
    std::vector<EffectParam> params = item.generator->params;
    if (adjustment) {
        return params.empty() ? std::nullopt : std::optional<std::string>("is an adjustment layer with parameters");
    }
    if (const auto problem = paramsProblem(params, d->params, false)) {
        return "is " + what + " with a " + *problem;
    }
    return std::nullopt;
}

std::vector<BarsPatch> barsPattern(int pattern) {
    std::vector<BarsPatch> out;
    const auto patch = [&](double x0, double y0, double x1, double y1, std::array<double, 3> rgb) {
        out.push_back(BarsPatch{x0, y0, x1, y1, rgb, false, {}});
    };
    using C = std::array<double, 3>;
    constexpr C kWhite75{0.75, 0.75, 0.75}, kYellow{0.75, 0.75, 0.0}, kCyan{0.0, 0.75, 0.75}, kGreen{0.0, 0.75, 0.0},
        kMagenta{0.75, 0.0, 0.75}, kRed{0.75, 0.0, 0.0}, kBlue{0.0, 0.0, 0.75}, kBlack{0.0, 0.0, 0.0};
    if (pattern == 1) {
        // SMPTE EG 1 (SD): seven 75 % bars over 2/3, reverse-blue castellations, then -I, white, +Q, black and the pluge.
        constexpr std::array<C, 7> top{kWhite75, kYellow, kCyan, kGreen, kMagenta, kRed, kBlue};
        constexpr std::array<C, 7> middle{kBlue, kBlack, kMagenta, kBlack, kCyan, kBlack, kWhite75};
        for (std::size_t i = 0; i < 7; ++i) {
            const double x0 = static_cast<double>(i) / 7.0;
            const double x1 = static_cast<double>(i + 1) / 7.0;
            patch(x0, 0.0, x1, 2.0 / 3.0, top[i]);
            patch(x0, 2.0 / 3.0, x1, 0.75, middle[i]);
        }
        const double w = 5.0 / 28.0;
        patch(0.0, 0.75, w, 1.0, C{0.0, 0.247, 0.415});       // -I
        patch(w, 0.75, 2 * w, 1.0, C{1.0, 1.0, 1.0});         // 100 % white
        patch(2 * w, 0.75, 3 * w, 1.0, C{0.255, 0.0, 0.475}); // +Q
        patch(3 * w, 0.75, 4 * w, 1.0, kBlack);
        const double p = 1.0 / 21.0;
        patch(4 * w, 0.75, 4 * w + p, 1.0, C{-0.04, -0.04, -0.04}); // pluge: below black
        patch(4 * w + p, 0.75, 4 * w + 2 * p, 1.0, kBlack);
        patch(4 * w + 2 * p, 0.75, 4 * w + 3 * p, 1.0, C{0.04, 0.04, 0.04});
        patch(4 * w + 3 * p, 0.75, 1.0, 1.0, kBlack);
        return out;
    }
    // SMPTE RP 219 (HD): side panels d = 1/8, bars c = 3/28 of the width; rows 7/12, 1/12, 1/12, 3/12 of the height.
    const double d = 1.0 / 8.0;
    const double c = 3.0 / 28.0;
    constexpr C kGray40{0.4, 0.4, 0.4}, kGray15{0.15, 0.15, 0.15};
    const double r1 = 7.0 / 12.0, r2 = 8.0 / 12.0, r3 = 9.0 / 12.0;
    patch(0.0, 0.0, d, r1, kGray40);
    constexpr std::array<C, 7> bars{kWhite75, kYellow, kCyan, kGreen, kMagenta, kRed, kBlue};
    for (std::size_t i = 0; i < 7; ++i) {
        patch(d + c * static_cast<double>(i), 0.0, d + c * static_cast<double>(i + 1), r1, bars[i]);
    }
    patch(1.0 - d, 0.0, 1.0, r1, kGray40);
    patch(0.0, r1, d, r2, C{0.0, 1.0, 1.0});
    patch(d, r1, 1.0 - d, r2, kWhite75);
    patch(1.0 - d, r1, 1.0, r2, C{0.0, 0.0, 1.0});
    patch(0.0, r2, d, r3, C{1.0, 1.0, 0.0});
    patch(d, r2, d + c, r3, kBlack);
    out.push_back(BarsPatch{d + c, r2, 1.0 - d - c, r3, kBlack, true, C{1.0, 1.0, 1.0}}); // Y ramp 0 -> 100 %
    patch(1.0 - d - c, r2, 1.0 - d, r3, C{1.0, 1.0, 1.0});
    patch(1.0 - d, r2, 1.0, r3, C{1.0, 0.0, 0.0});
    // Row 4: 15 % gray, black 1.5c, white 2c, black 5/6 c, pluge -2/0/+2/0/+4 % at c/3 each, black c, 15 % gray.
    patch(0.0, r3, d, 1.0, kGray15);
    double x = d;
    const auto run = [&](double width, C rgb) {
        patch(x, r3, x + width, 1.0, rgb);
        x += width;
    };
    run(1.5 * c, kBlack);
    run(2.0 * c, C{1.0, 1.0, 1.0});
    run(5.0 / 6.0 * c, kBlack);
    run(c / 3.0, C{-0.02, -0.02, -0.02});
    run(c / 3.0, kBlack);
    run(c / 3.0, C{0.02, 0.02, 0.02});
    run(c / 3.0, kBlack);
    run(c / 3.0, C{0.04, 0.04, 0.04});
    patch(x, r3, 1.0 - d, 1.0, kBlack);
    patch(1.0 - d, r3, 1.0, 1.0, kGray15);
    return out;
}

LeaderFrame countingLeaderAt(const MediaItem& item, Ticks t) {
    LeaderFrame f;
    if (t.value < 0 || t >= kCountingLeaderLength) {
        return f;
    }
    const std::int64_t second = t.value / kTicksPerSecond;
    const double within = static_cast<double>(t.value % kTicksPerSecond) / static_cast<double>(kTicksPerSecond);
    if (second < 6) {
        f.number = 8 - static_cast<int>(second);
        f.sweep = within;
        return f;
    }
    // The 2 shows for one frame at 6 s (the 2-pop), then black to picture start.
    const auto frame = framesToTicks(1, leaderRate(item), Rounding::Ceil).value_or(Ticks{kTicksPerSecond / 30});
    if (second == 6 && t < Ticks{6 * kTicksPerSecond} + frame) {
        f.number = 2;
    }
    return f;
}

std::size_t renderSyntheticAudio(const MediaItem& item, Ticks sourceTime, std::span<float> interleaved, int channels,
                                 Rate sampleRate) {
    std::fill(interleaved.begin(), interleaved.end(), 0.0f);
    if (channels < 1 || sampleRate.num <= 0 || sampleRate.den <= 0) {
        return 0;
    }
    const std::size_t frames = interleaved.size() / static_cast<std::size_t>(channels);
    const SyntheticDescriptor* d = syntheticOf(item);
    if (!d || !d->hasAudio) {
        return frames;
    }
    // The first sample's index at this rate (nearest), so consecutive reads continue the phase exactly.
    const double perSecond = static_cast<double>(sampleRate.num) / static_cast<double>(sampleRate.den);
    const auto first = static_cast<std::int64_t>(
        std::llround(static_cast<double>(sourceTime.value) / static_cast<double>(kTicksPerSecond) * perSecond));
    const auto write = [&](std::size_t i, float v) {
        for (int ch = 0; ch < channels; ++ch) {
            interleaved[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)] = v;
        }
    };
    if (d->kind == SyntheticKind::BarsAndTone) {
        const double frequency = syntheticParam(item, "tone.frequency", 1000.0);
        const auto amplitude = static_cast<float>(std::pow(10.0, syntheticParam(item, "tone.level", -20.0) / 20.0));
        for (std::size_t i = 0; i < frames; ++i) {
            const double phase = cycles(frequency, first + static_cast<std::int64_t>(i), sampleRate);
            write(i, amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * phase)));
        }
        return frames;
    }
    // Counting leader: one-frame blips.
    const Rate rate = leaderRate(item);
    const double frameSeconds = static_cast<double>(rate.den) / static_cast<double>(rate.num);
    const bool on2 = syntheticParam(item, "cueBlipOn2", 1.0) >= 0.5;
    const bool everySecond = syntheticParam(item, "cueBlipAllSeconds", 0.0) >= 0.5;
    const bool onOut = syntheticParam(item, "cueBlipOnOut", 0.0) >= 0.5;
    const double length = static_cast<double>(kCountingLeaderLength.value) / static_cast<double>(kTicksPerSecond);
    const auto amplitude = static_cast<float>(std::pow(10.0, -20.0 / 20.0));
    for (std::size_t i = 0; i < frames; ++i) {
        const std::int64_t n = first + static_cast<std::int64_t>(i);
        const double s = static_cast<double>(n) / perSecond;
        if (s < 0.0 || s >= length) {
            continue;
        }
        const double second = std::floor(s);
        const bool inFirstFrame = s - second < frameSeconds;
        const bool blip = (inFirstFrame && on2 && second == 6.0) || (inFirstFrame && everySecond && second < 6.0) ||
                          (onOut && s >= length - frameSeconds);
        if (blip) {
            write(i, amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * cycles(1000.0, n, sampleRate))));
        }
    }
    return frames;
}

namespace edit {

std::expected<Created<MediaId>, Error> addSyntheticMedia(const ProjectState& state, SyntheticKind kind,
                                                         std::vector<EffectParam> params, SequenceId sequence, BinId bin,
                                                         std::string name) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (bin.isValid() && !state.findBin(bin)) {
        return std::unexpected(detail::notFound("bin " + std::to_string(bin.value) + " does not exist"));
    }
    auto item = syntheticMediaItem(kind, std::move(params),
                                   SyntheticFormat{(*seq)->width, (*seq)->height, (*seq)->frameRate, (*seq)->audioRate},
                                   std::move(name));
    if (!item) {
        return std::unexpected(item.error());
    }
    item->bin = bin;
    return addMedia(state, std::move(*item));
}

EditOutcome addAdjustmentLayer(const ProjectState& state, SequenceId sequenceId, TrackId trackId, Ticks at, Ticks duration,
                               MediaId media, BinId bin) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Track* track = (*seq)->findTrack(trackId);
    if (!track) {
        return std::unexpected(detail::notFound("track " + std::to_string(trackId.value) + " does not exist"));
    }
    if (track->kind != TrackKind::Video) {
        return std::unexpected(detail::invalidArgument("an adjustment layer goes on a video track"));
    }
    if (track->locked) {
        return std::unexpected(detail::invalidArgument("track " + detail::trackLabel(**seq, trackId) + " is locked"));
    }
    if (duration.value <= 0 || at.value < 0) {
        return std::unexpected(detail::invalidArgument("an adjustment layer needs a start >= 0 and a positive duration"));
    }
    ProjectState next = state;
    MediaId created;
    if (media.isValid()) {
        const MediaItem* item = state.findMedia(media);
        if (!item) {
            return std::unexpected(detail::notFound("media " + std::to_string(media.value) + " does not exist"));
        }
        if (!isAdjustmentLayer(*item)) {
            return std::unexpected(detail::invalidArgument("media '" + item->name + "' is not an adjustment layer"));
        }
    } else {
        if (bin.isValid() && !state.findBin(bin)) {
            return std::unexpected(detail::notFound("bin " + std::to_string(bin.value) + " does not exist"));
        }
        auto item = adjustmentMediaItem(SyntheticFormat{(*seq)->width, (*seq)->height, (*seq)->frameRate, (*seq)->audioRate});
        if (!item) {
            return std::unexpected(item.error());
        }
        item->bin = bin;
        auto added = addMedia(state, std::move(*item));
        if (!added) {
            return std::unexpected(added.error());
        }
        next = std::move(added->state);
        media = created = added->id;
    }
    const SourceClip source{media, TimeRange{Ticks{}, duration}, {SourcePatch{StreamRef{StreamKind::Video, 0}, trackId}}};
    auto placed = overwriteEdit(next, sequenceId, source, at);
    if (!placed) {
        return std::unexpected(placed.error());
    }
    if (placed->placed.size() != 1) {
        return std::unexpected(detail::invalidArgument("the adjustment layer could not be placed"));
    }
    EditResult result = std::move(*placed);
    result.changes = diff(state, result.state);
    result.createdMedia = created;
    return result;
}

} // namespace edit

} // namespace wabcut
