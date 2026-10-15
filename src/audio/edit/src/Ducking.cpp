#include <wabcut/audio/edit/Ducking.h>

#include <algorithm>
#include <cmath>

namespace wabcut::audio::edit {

namespace {

double rampFraction(Ticks t, Ticks from, Ticks length) {
    return length.value > 0 ? static_cast<double>((t - from).value) / static_cast<double>(length.value) : 1.0;
}

} // namespace

DuckingOptions gameUnderVoiceDucking() {
    DuckingOptions o;
    // -10 dB, not a music bed's -12 or more: the game is what the video is about (shots, footsteps, the callout the
    // streamer reacts to), so it stays present under the voice. 10 dB is enough for a voice mixed at -16 LUFS to sit
    // clearly over a game bed at its own loudness.
    o.amountDb = -10.0;
    // A short lead-in: the down-ramp ends where the speech starts, and a voice reacting to the game follows it closely, so
    // the game stays up until just before the first word. A tenth of a second is still a ramp, not a click.
    o.attack = Ticks{kTicksPerSecond * 12 / 100};
    // Down through the gaps between words and a breath, so the game does not jump up in the middle of a sentence.
    o.hold = Ticks{kTicksPerSecond * 35 / 100};
    // Back fast enough to hear the game between sentences, slowly enough not to pump; with the attack and the hold a pause
    // under 1.27 s stays down (duckRegions bridges it).
    o.release = Ticks{kTicksPerSecond * 8 / 10};
    // An open mic picks up clicks, a cough, a short laugh: speech under 0.15 s does not take the game down.
    o.minSpeech = Ticks{kTicksPerSecond * 15 / 100};
    return o;
}

std::vector<DuckRegion> duckRegions(std::span<const TimeRange> speech, const DuckingOptions& options) {
    std::vector<TimeRange> runs;
    for (const TimeRange& r : speech) {
        if (r.duration >= options.minSpeech && r.duration.value > 0) {
            runs.push_back(r);
        }
    }
    std::sort(runs.begin(), runs.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<DuckRegion> out;
    for (const TimeRange& r : runs) {
        if (!out.empty()) {
            DuckRegion& last = out.back();
            // Down again before the music would be back up: stay down across the gap.
            if (r.start - options.attack < last.upEnd) {
                const Ticks end = std::max(last.speech.end(), r.end());
                last.speech = TimeRange::fromStartEnd(last.speech.start, end);
                last.upStart = end + options.hold;
                last.upEnd = last.upStart + options.release;
                continue;
            }
        }
        DuckRegion region;
        region.speech = r;
        region.downStart = r.start - options.attack;
        region.upStart = r.end() + options.hold;
        region.upEnd = region.upStart + options.release;
        out.push_back(region);
    }
    return out;
}

double duckOffsetDb(std::span<const DuckRegion> regions, const DuckingOptions& options, Ticks t) {
    double offset = 0.0;
    for (const DuckRegion& r : regions) {
        double here = 0.0;
        if (t >= r.downStart && t < r.speech.start) {
            here = options.amountDb * rampFraction(t, r.downStart, options.attack);
        } else if (t >= r.speech.start && t < r.upStart) {
            here = options.amountDb;
        } else if (t >= r.upStart && t < r.upEnd) {
            here = options.amountDb * (1.0 - rampFraction(t, r.upStart, options.release));
        }
        offset = std::min(offset, here);
    }
    return offset;
}

std::optional<ParamValue> duckedVolume(const Clip& clip, std::span<const DuckRegion> regions,
                                       const DuckingOptions& options, std::optional<TimeRange> range) {
    Ticks from = clip.start();
    Ticks to = clip.end();
    if (range) {
        from = std::max(from, range->start);
        to = std::min(to, range->end());
    }
    if (to <= from) {
        return std::nullopt;
    }
    const ParamValue& volume = clip.properties()[ParamId::VolumeDb];
    const ParameterInfo& info = parameterInfo(ParamId::VolumeDb);
    const double base = volume.valueAt(clip.curveTimeAt(from));

    // Envelope corners inside the span, plus its two ends.
    std::vector<Ticks> times{from, to};
    for (const DuckRegion& r : regions) {
        for (const Ticks t : {r.downStart, r.speech.start, r.upStart, r.upEnd}) {
            if (t > from && t < to) {
                times.push_back(t);
            }
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());

    std::vector<Keyframe> keys;
    for (const Ticks t : times) {
        Keyframe k;
        k.time = clip.curveTimeAt(t);
        k.value = std::clamp(base + duckOffsetDb(regions, options, t), info.minValue, info.maxValue);
        k.interpolation = Interpolation::Linear;
        keys.push_back(k);
    }
    // Keys of the old curve outside the span stay (curve time runs backwards on reversed clips).
    const Ticks c0 = std::min(keys.front().time, keys.back().time);
    const Ticks c1 = std::max(keys.front().time, keys.back().time);
    if (const Curve* old = volume.curve()) {
        for (const Keyframe& k : old->keys) {
            if (k.time < c0 || k.time > c1) {
                keys.push_back(k);
            }
        }
    }
    std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    keys.erase(std::unique(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time == b.time; }),
               keys.end());
    const bool flat = std::all_of(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.value == keys.front().value; });
    if (flat) {
        return ParamValue(keys.front().value);
    }
    return ParamValue(Curve{std::move(keys)});
}

} // namespace wabcut::audio::edit
