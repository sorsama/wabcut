#include "NestedPeaks.h"

#include "NestedMixPeaks.h"

#include <wabcut/model/Resolve.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace wabcut::app {

namespace {

std::size_t firstEndingAfter(const ClipList& clips, Ticks t) {
    std::size_t i = clips.upperBoundStart(t);
    if (i > 0 && clips[i - 1].end() > t)
        --i;
    return i;
}

/// a + (b - a) * f, to the nearest tick.
Ticks between(Ticks a, Ticks b, double f) {
    return Ticks{a.value + static_cast<std::int64_t>(std::llround(static_cast<double>(b.value - a.value) * f))};
}

std::uint64_t mix(std::uint64_t key, std::uint64_t value) {
    key ^= value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
    return key * 0xFF51AFD7ED558CCDull;
}

struct Walk {
    const ProjectState& state;
    const PeakSource& peaks;
    NestedPeaksResult result;
};

void readClip(Walk& walk, const Clip& clip, Ticks ta, Ticks tb, std::span<float> out, std::span<std::uint8_t> pending,
              int depth);

/// The loudest leaf peaks of `nested`'s audio tracks `tracks` over nested time [lo, hi) into `out` (equal columns of
/// [lo, hi), in time order), nested clips there followed while `depth` allows.
void readRange(Walk& walk, const Sequence& nested, std::span<const std::size_t> tracks, Ticks lo, Ticks hi,
               std::span<float> out, std::span<std::uint8_t> pending, int depth) {
    const std::size_t n = out.size();
    if (n == 0 || hi <= lo)
        return;
    const double span = static_cast<double>(hi.value - lo.value);
    const auto columnOf = [&](Ticks t) { return static_cast<double>(t.value - lo.value) / span * static_cast<double>(n); };
    for (const std::size_t index : tracks) {
        if (index >= nested.audioTracks.size() || !nested.audioTracks[index])
            continue;
        const ClipList& clips = nested.audioTracks[index]->clips;
        for (auto it = clips.iteratorAt(firstEndingAfter(clips, lo)); it != clips.end() && it->start() < hi; ++it) {
            const Clip& leaf = *it;
            if (!leaf.enabled || leaf.hasUnknownSource())
                continue;
            const Ticks a = std::max(lo, leaf.start());
            const Ticks b = std::min(hi, leaf.end());
            if (b <= a)
                continue;
            // The columns the leaf covers (the ones at its ends only partly: an approximation, as the whole is).
            const auto k0 = static_cast<std::size_t>(std::clamp(std::floor(columnOf(a)), 0.0, static_cast<double>(n - 1)));
            const auto k1 = static_cast<std::size_t>(
                std::clamp(std::ceil(columnOf(b)), static_cast<double>(k0 + 1), static_cast<double>(n)));
            const std::span<float> part = out.subspan(k0, k1 - k0);
            const std::span<std::uint8_t> partPending = pending.subspan(k0, k1 - k0);
            if (leaf.isNested()) {
                if (depth < kNestedPeakDepth)
                    readClip(walk, leaf, a, b, part, partPending, depth + 1);
                continue;
            }
            if (!leaf.media.isValid() || leaf.isFreeze())
                continue; // nothing to read, or silent (a frame hold)
            const TimeRange source = leaf.shownSourceRange(a, b);
            if (source.duration <= Ticks{})
                continue;
            std::vector<float> scratch(part.size());
            if (walk.peaks.peaks(leaf.media, leaf.stream, source.start, source.end(), scratch)) {
                const bool backwards = leaf.reversed && !leaf.isRemapped(); // a reversed leaf plays its source backwards
                for (std::size_t j = 0; j < part.size(); ++j)
                    part[j] = std::max(part[j], scratch[backwards ? part.size() - 1 - j : j]);
                walk.result.drawn = true;
            }
            if (const std::optional<Ticks> until = walk.peaks.buildingUntil(leaf.media, leaf.stream)) {
                // The columns whose source time the decoded part has not reached yet.
                const double width = static_cast<double>(b.value - a.value) / static_cast<double>(part.size());
                for (std::size_t j = 0; j < part.size(); ++j) {
                    const Ticks t = a + Ticks{static_cast<std::int64_t>(std::llround(width * (static_cast<double>(j) + 0.5)))};
                    if (leaf.sourceTimeAt(std::clamp(t, leaf.start(), leaf.end() - Ticks{1})) >= *until) {
                        partPending[j] = 1;
                        walk.result.building = true;
                    }
                }
            }
        }
    }
}

/// Where a nested clip is heard: nested sound plays at 100 % only (TrackRenderer), forwards or reversed; a remapped clip
/// in its 100 % spans.
std::vector<TimeRange> audibleRanges(const Clip& clip) {
    std::vector<TimeRange> audible;
    if (clip.isRemapped())
        audible = clip.unitySpeedRanges();
    else if (clip.speed.isUnity())
        audible.push_back(clip.timelineRange);
    return audible;
}

/// One column of [ta, tb): the nested time it shows and whether the clip is heard there.
struct Column {
    Ticks lo, hi;
    bool audible = false;
};

std::vector<Column> columnsOf(const Clip& clip, Ticks ta, Ticks tb, std::size_t n) {
    std::vector<Column> columns(n);
    const std::vector<TimeRange> audible = audibleRanges(clip);
    if (n == 0 || audible.empty())
        return columns;
    Ticks previous = clip.sourceTimeAt(std::clamp(ta, clip.start(), clip.end()));
    for (std::size_t k = 0; k < n; ++k) {
        const Ticks next = clip.sourceTimeAt(
            std::clamp(between(ta, tb, static_cast<double>(k + 1) / static_cast<double>(n)), clip.start(), clip.end()));
        const Ticks centre = between(ta, tb, (static_cast<double>(k) + 0.5) / static_cast<double>(n));
        Column& c = columns[k];
        c.lo = std::min(previous, next);
        c.hi = std::max(previous, next);
        c.audible = c.hi > c.lo && std::ranges::any_of(audible, [&](const TimeRange& r) { return r.contains(centre); });
        previous = next;
    }
    return columns;
}

/// Nested (or multicam, merged) clip `clip` over its timeline [ta, tb) into `out` (equal columns of [ta, tb)).
void readClip(Walk& walk, const Clip& clip, Ticks ta, Ticks tb, std::span<float> out, std::span<std::uint8_t> pending,
              int depth) {
    const std::size_t n = out.size();
    if (n == 0 || tb <= ta || !clip.isNested())
        return;
    const Sequence* nested = walk.state.findSequence(clip.sequence);
    if (!nested)
        return;
    const std::vector<std::size_t> tracks = nestedTracksRead(*nested, clip);
    if (tracks.empty())
        return;
    const std::vector<TimeRange> audible = audibleRanges(clip);
    if (audible.empty())
        return;
    // The nested time at each column edge.
    std::vector<Ticks> edge(n + 1);
    for (std::size_t k = 0; k <= n; ++k)
        edge[k] = clip.sourceTimeAt(std::clamp(between(ta, tb, static_cast<double>(k) / static_cast<double>(n)), clip.start(), clip.end()));
    const auto audibleAt = [&](std::size_t k) {
        const Ticks centre = between(ta, tb, (static_cast<double>(k) + 0.5) / static_cast<double>(n));
        return std::ranges::any_of(audible, [&](const TimeRange& r) { return r.contains(centre); });
    };
    // Runs of audible columns whose nested time runs one way (the whole clip, unless it is remapped).
    std::size_t k = 0;
    while (k < n) {
        if (!audibleAt(k) || edge[k + 1] == edge[k]) {
            ++k;
            continue;
        }
        const bool forward = edge[k + 1] > edge[k];
        std::size_t end = k + 1;
        while (end < n && audibleAt(end) && (forward ? edge[end + 1] > edge[end] : edge[end + 1] < edge[end]))
            ++end;
        const std::span<float> part = out.subspan(k, end - k);
        const std::span<std::uint8_t> partPending = pending.subspan(k, end - k);
        if (forward) {
            readRange(walk, *nested, tracks, edge[k], edge[end], part, partPending, depth);
        } else {
            // Reversed: nested time runs right to left; read it in time order and put it back mirrored.
            std::vector<float> mirrored(part.rbegin(), part.rend());
            std::vector<std::uint8_t> mirroredPending(partPending.rbegin(), partPending.rend());
            readRange(walk, *nested, tracks, edge[end], edge[k], mirrored, mirroredPending, depth);
            std::copy(mirrored.rbegin(), mirrored.rend(), part.begin());
            std::copy(mirroredPending.rbegin(), mirroredPending.rend(), partPending.begin());
        }
        k = end;
    }
}

void collectMedia(const ProjectState& state, SequenceId id, int depth, std::vector<std::uint64_t>& out,
                  std::unordered_set<std::uint64_t>& seen) {
    if (!seen.insert(id.value).second)
        return;
    const Sequence* sequence = state.findSequence(id);
    if (!sequence)
        return;
    for (const TrackPtr& track : sequence->audioTracks) {
        if (!track)
            continue;
        for (const Clip& c : track->clips) {
            if (c.isNested()) {
                if (depth < kNestedPeakDepth)
                    collectMedia(state, c.sequence, depth + 1, out, seen);
            } else if (c.media.isValid()) {
                out.push_back(c.media.value);
            }
        }
    }
}

void stampOf(const ProjectState& state, SequenceId id, int depth, std::uint64_t& key, std::unordered_set<std::uint64_t>& seen) {
    if (!seen.insert(id.value).second)
        return;
    const SequencePtr sequence = state.sequencePtr(id);
    key = mix(key, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(sequence.get())));
    if (!sequence || depth >= kNestedPeakDepth)
        return;
    for (const TrackPtr& track : sequence->audioTracks) {
        if (!track)
            continue;
        for (const Clip& c : track->clips) {
            if (c.isNested())
                stampOf(state, c.sequence, depth + 1, key, seen);
        }
    }
}

} // namespace

NestedPeaksResult nestedPeaks(const Clip& clip, const ProjectState& state, const PeakSource& peaks, Ticks ta, Ticks tb,
                              std::span<float> out, std::vector<std::uint8_t>* pending, const NestedMixSource* mixes,
                              const ProjectSnapshot& project) {
    std::fill(out.begin(), out.end(), 0.0f);
    std::vector<std::uint8_t> local;
    std::vector<std::uint8_t>& flags = pending ? *pending : local;
    flags.assign(out.size(), 0);
    Walk walk{state, peaks, {}};
    // Phase 2: the mix the clip plays, rendered in the background (a miss queues it).
    NestedMixSource::Lookup rendered;
    if (mixes && project && clip.isNested() && clip.stream.kind == StreamKind::Audio && tb > ta && !out.empty())
        rendered = mixes->lookup(clip, project);
    if (rendered.state == NestedMixSource::State::Ready && rendered.peaks) {
        const std::vector<Column> columns = columnsOf(clip, ta, tb, out.size());
        for (std::size_t k = 0; k < out.size(); ++k) {
            if (columns[k].audible)
                out[k] = rendered.peaks->peak(columns[k].lo, columns[k].hi);
        }
        walk.result.drawn = true;
        walk.result.exact = true;
        return walk.result;
    }
    // The approximation from the leaf clips (phase 1): while the mix renders, and when it failed.
    readClip(walk, clip, ta, tb, out, flags, 1);
    if (rendered.state == NestedMixSource::State::Pending) {
        // What is rendered so far is exact; the rest stays approximate, shaded as building.
        const std::vector<Column> columns = columnsOf(clip, ta, tb, out.size());
        const Ticks reach = rendered.peaks ? rendered.peaks->reach() : Ticks{};
        for (std::size_t k = 0; k < out.size(); ++k) {
            if (!columns[k].audible)
                continue;
            if (rendered.peaks && columns[k].hi <= reach) {
                out[k] = rendered.peaks->peak(columns[k].lo, columns[k].hi);
                flags[k] = 0;
                walk.result.drawn = true;
            } else {
                flags[k] = 1;
                walk.result.building = true;
            }
        }
    }
    return walk.result;
}

std::vector<std::uint64_t> nestedPeakMedia(const ProjectState& state, SequenceId sequence) {
    std::vector<std::uint64_t> out;
    std::unordered_set<std::uint64_t> seen;
    collectMedia(state, sequence, 1, out, seen);
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::uint64_t nestedPeakStamp(const ProjectState& state, SequenceId sequence) {
    if (!state.findSequence(sequence))
        return 0;
    std::uint64_t key = mix(0xA0D1'0000ull, sequence.value);
    std::unordered_set<std::uint64_t> seen;
    stampOf(state, sequence, 1, key, seen);
    return key == 0 ? 1 : key;
}

} // namespace wabcut::app
