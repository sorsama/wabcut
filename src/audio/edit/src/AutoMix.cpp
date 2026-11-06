#include <wabcut/audio/edit/AutoMix.h>

#include <algorithm>

namespace wabcut::audio::edit {

namespace {

/// The ranges of `in` at least `minSpeech` long, merged: what counts as a voice speaking.
std::vector<TimeRange> voiced(const std::vector<TimeRange>& in, Ticks minSpeech) {
    std::vector<TimeRange> kept;
    for (const TimeRange& r : in) {
        if (r.duration.value > 0 && r.duration >= minSpeech) {
            kept.push_back(r);
        }
    }
    return mergedRanges(std::move(kept));
}

/// Each range extended by `hold` after its end, merged.
std::vector<TimeRange> held(const std::vector<TimeRange>& in, Ticks hold) {
    std::vector<TimeRange> out;
    out.reserve(in.size());
    for (const TimeRange& r : in) {
        out.push_back(TimeRange::fromStartEnd(r.start, r.end() + hold));
    }
    return mergedRanges(std::move(out));
}

/// `a` without `b` (both sorted and disjoint).
std::vector<TimeRange> subtracted(const std::vector<TimeRange>& a, const std::vector<TimeRange>& b) {
    std::vector<TimeRange> out;
    std::size_t first = 0;
    for (const TimeRange& r : a) {
        Ticks from = r.start;
        const Ticks to = r.end();
        while (first < b.size() && b[first].end() <= from) {
            ++first;
        }
        for (std::size_t k = first; from < to; ++k) {
            if (k >= b.size() || b[k].start >= to) {
                out.push_back(TimeRange::fromStartEnd(from, to));
                break;
            }
            if (b[k].start > from) {
                out.push_back(TimeRange::fromStartEnd(from, b[k].start));
            }
            from = std::max(from, b[k].end());
        }
    }
    return out;
}

} // namespace

std::vector<TimeRange> mergedRanges(std::vector<TimeRange> ranges) {
    std::erase_if(ranges, [](const TimeRange& r) { return r.duration.value <= 0; });
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> out;
    for (const TimeRange& r : ranges) {
        if (!out.empty() && r.start <= out.back().end()) {
            out.back() = TimeRange::fromStartEnd(out.back().start, std::max(out.back().end(), r.end()));
        } else {
            out.push_back(r);
        }
    }
    return out;
}

DuckingOptions duckingOptionsOf(const AutoMixOptions& options) {
    DuckingOptions o;
    o.amountDb = options.amountDb;
    o.attack = options.attack;
    o.release = options.release;
    o.hold = options.hold;
    o.minSpeech = options.minSpeech;
    return o;
}

std::vector<DuckRegion> autoMixRegions(std::span<const std::vector<TimeRange>> speech, std::size_t voice,
                                       const AutoMixOptions& options, std::optional<TimeRange> span) {
    std::vector<DuckRegion> regions;
    if (voice >= speech.size()) {
        return regions;
    }
    const std::vector<TimeRange> own = held(voiced(speech[voice], options.minSpeech), options.hold);

    // What lowers this microphone: the other voices (each held after its last word), or, with the gate, everything.
    std::vector<TimeRange> others;
    for (std::size_t k = 0; k < speech.size(); ++k) {
        if (k != voice) {
            const std::vector<TimeRange> theirs = held(voiced(speech[k], options.minSpeech), options.hold);
            others.insert(others.end(), theirs.begin(), theirs.end());
        }
    }
    others = mergedRanges(std::move(others));
    std::vector<TimeRange> target = others;
    if (options.gate) {
        if (span) {
            target = {*span};
        } else {
            // No span: from the first to the last word of anyone.
            std::vector<TimeRange> all = others;
            all.insert(all.end(), own.begin(), own.end());
            all = mergedRanges(std::move(all));
            target.clear();
            if (!all.empty()) {
                target.push_back(TimeRange::fromStartEnd(all.front().start, all.back().end()));
            }
        }
    }
    // Never lowered under its own words (nor the hold after them).
    std::vector<TimeRange> down = subtracted(target, own);

    // A gap shorter than a ramp down and up is not worth coming back for, unless the voice itself speaks there.
    const Ticks bridge = options.attack + options.release;
    std::vector<TimeRange> stretches;
    for (const TimeRange& r : down) {
        if (!stretches.empty()) {
            const Ticks gapStart = stretches.back().end();
            const bool voiceInGap = std::any_of(own.begin(), own.end(), [&](const TimeRange& o) {
                return o.start < r.start && gapStart < o.end();
            });
            if (r.start - gapStart < bridge && !voiceInGap) {
                stretches.back() = TimeRange::fromStartEnd(stretches.back().start, r.end());
                continue;
            }
        }
        stretches.push_back(r);
    }

    for (const TimeRange& stretch : stretches) {
        const Ticks begin = stretch.start;
        const Ticks end = stretch.end();
        // The voice's own held speech before the stretch, and its next first word after it.
        Ticks previousOwnEnd = Ticks::min();
        Ticks nextOwnStart = Ticks::max();
        for (const TimeRange& o : own) {
            if (o.end() <= begin) {
                previousOwnEnd = std::max(previousOwnEnd, o.end());
            }
            if (o.start >= end) {
                nextOwnStart = std::min(nextOwnStart, o.start);
            }
        }
        DuckRegion region;
        region.downStart = std::max(begin - options.attack, previousOwnEnd); // after the voice's tail, not under it
        const Ticks plateauStart = region.downStart + options.attack;
        region.upEnd = std::min(end + options.release, nextOwnStart - options.lead); // up before its first word
        region.upStart = region.upEnd - options.release;
        if (region.upStart <= plateauStart) {
            continue; // too short to go down and come back
        }
        region.speech = TimeRange::fromStartEnd(plateauStart, region.upStart);
        regions.push_back(region);
    }
    return regions;
}

} // namespace wabcut::audio::edit
