// Dead time (C.16, GamingOps.h): low-activity stretches of gameplay from media activity curves, and the ripple delete
// that removes them.

#include <wabcut/model/GamingOps.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Resolve.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace wabcut {

namespace {

using edit::detail::invalidArgument;
using edit::detail::notFound;

bool inUnit(float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; }

/// The value of the 90th percentile of the positive values (0 when none).
double percentile90(const std::vector<double>& values) {
    std::vector<double> positive;
    positive.reserve(values.size());
    for (const double v : values) {
        if (v > 0.0) {
            positive.push_back(v);
        }
    }
    if (positive.empty()) {
        return 0.0;
    }
    const std::size_t k = std::min(positive.size() - 1, (positive.size() * 9) / 10);
    std::nth_element(positive.begin(), positive.begin() + static_cast<std::ptrdiff_t>(k), positive.end());
    return positive[k];
}

void normalise(std::vector<double>& values) {
    const double p90 = percentile90(values);
    for (double& v : values) {
        v = p90 > 1e-9 ? std::clamp(v / p90, 0.0, 1.0) : 0.0;
    }
}

} // namespace

std::optional<std::string> mediaActivityProblem(const MediaActivity& a) {
    if (a.step.value <= 0) {
        return "the activity step must be positive";
    }
    if (a.audio.size() > MediaActivity::kMaxSamples || a.motion.size() > MediaActivity::kMaxSamples) {
        return "too many activity samples";
    }
    if (!std::all_of(a.audio.begin(), a.audio.end(), inUnit) || !std::all_of(a.motion.begin(), a.motion.end(), inUnit)) {
        return "activity values must be 0..1";
    }
    return std::nullopt;
}

std::optional<std::string> deadTimeOptionsProblem(const DeadTimeOptions& o) {
    if (!std::isfinite(o.minSeconds) || o.minSeconds < 0.5 || o.minSeconds > 3600.0) {
        return "minSeconds must be 0.5..3600";
    }
    if (!std::isfinite(o.sensitivity) || o.sensitivity < 0.0 || o.sensitivity > 1.0) {
        return "sensitivity must be 0..1";
    }
    if (!std::isfinite(o.handleSeconds) || o.handleSeconds < 0.0 || o.handleSeconds > 5.0) {
        return "handleSeconds must be 0..5";
    }
    if (o.range && (o.range->start.value < 0 || o.range->duration.value <= 0)) {
        return "the range must be a positive span";
    }
    return std::nullopt;
}

std::vector<TrackId> defaultActivityTracks(const Sequence& seq) {
    std::vector<TrackId> out;
    for (const auto& t : seq.videoTracks) {
        if (std::any_of(t->clips.begin(), t->clips.end(), [](const Clip& c) { return c.media.isValid() || c.isNested(); })) {
            out.push_back(t->id);
            break;
        }
    }
    for (const auto& t : seq.audioTracks) {
        if (!t->muted && t->enabled) {
            out.push_back(t->id);
        }
    }
    return out;
}

std::expected<DeadTimeAnalysis, Error> findDeadTime(const ProjectState& state, SequenceId sequenceId, const ActivityLookup& activity,
                                                    const DeadTimeOptions& options) {
    const auto found = edit::detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (const auto problem = deadTimeOptionsProblem(options)) {
        return std::unexpected(invalidArgument("dead time: " + *problem));
    }
    const Sequence& seq = **found;
    DeadTimeAnalysis out;
    out.tracks = options.tracks.empty() ? defaultActivityTracks(seq) : options.tracks;
    for (const TrackId id : out.tracks) {
        if (!seq.findTrack(id)) {
            return std::unexpected(notFound("track " + std::to_string(id.value) + " does not exist"));
        }
    }
    const Ticks step{kTicksPerSecond / kActivityRate};
    const TimeRange span = options.range.value_or(TimeRange{Ticks{}, seq.contentEnd()});
    const std::int64_t count = (span.duration.value + step.value - 1) / step.value;
    if (count > static_cast<std::int64_t>(MediaActivity::kMaxSamples)) {
        return std::unexpected(invalidArgument("the sequence is too long to look for dead time in one go (use a range)"));
    }
    const std::size_t n = static_cast<std::size_t>(std::max<std::int64_t>(count, 0));
    for (ActivityCurve* curve : {&out.audio, &out.motion, &out.combined}) {
        curve->start = span.start;
        curve->step = step;
        curve->values.assign(n, 0.0);
    }
    std::vector<bool> unmeasuredAt(n, false); // a sample under media without activity: never dead
    bool anyAudio = false;
    bool anyMotion = false;
    std::set<std::uint64_t> unmeasured;
    for (const TrackId id : out.tracks) {
        const Track& track = *seq.findTrack(id);
        const bool audio = track.kind == TrackKind::Audio;
        std::vector<double>& target = audio ? out.audio.values : out.motion.values;
        std::size_t clipIndex = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Ticks t = span.start + Ticks{step.value * static_cast<std::int64_t>(i) + step.value / 2};
            while (clipIndex < track.clips.size() && track.clips[clipIndex].end() <= t) {
                ++clipIndex;
            }
            if (clipIndex >= track.clips.size()) {
                break;
            }
            const Clip& clip = track.clips[clipIndex];
            if (!clip.timelineRange.contains(t) || !clip.enabled || (!clip.media.isValid() && !clip.isNested())) {
                continue;
            }
            // One media clip's activity at the media time it shows or plays.
            const auto sample = [&](const Clip& leaf, Ticks source) {
                const MediaItem* item = state.findMedia(leaf.media);
                if (!item || item->isGenerator()) {
                    return; // a title or graphic over nothing: quiet
                }
                const MediaActivity* a = activity ? activity(leaf.media) : nullptr;
                const std::vector<float>* samples = a ? (audio ? &a->audio : &a->motion) : nullptr;
                if (!a || samples->empty() || mediaActivityProblem(*a)) {
                    unmeasured.insert(leaf.media.value);
                    unmeasuredAt[i] = true;
                    return;
                }
                const std::int64_t index = source.value / a->step.value;
                if (index < 0 || static_cast<std::size_t>(index) >= samples->size()) {
                    return;
                }
                const double v = (*samples)[static_cast<std::size_t>(index)];
                target[i] = std::max(target[i], v);
                (audio ? anyAudio : anyMotion) = true;
            };
            if (clip.isNested()) {
                // 0.2: a nested or multicam clip (a gaming nest, a podcast's cameras) through what it shows or plays at
                // t: the active angle's media, the nested sequence's clips, each at its media time through every
                // level's time map.
                for (const ResolvedLeaf& leaf : resolveLeaves(state, seq.id, clip, t)) {
                    sample(*leaf.clip, leaf.sourceTime);
                }
                continue;
            }
            sample(clip, clip.sourceTimeAt(t));
        }
    }
    for (const std::uint64_t id : unmeasured) {
        out.unmeasured.push_back(MediaId{id});
    }
    normalise(out.audio.values);
    normalise(out.motion.values);
    // Both kinds measured: their mean (music over a loading screen is not action); one: that one.
    std::vector<double> raw(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = out.audio.values[i];
        const double m = out.motion.values[i];
        raw[i] = anyAudio && anyMotion ? 0.5 * (a + m) : anyAudio ? a : m;
    }
    // Smoothed over half a second (centred), so a single quiet or loud sample does not split a stretch.
    const std::size_t radius = static_cast<std::size_t>(kActivityRate / 4);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t lo = i >= radius ? i - radius : 0;
        const std::size_t hi = std::min(n - 1, i + radius);
        double sum = 0.0;
        for (std::size_t j = lo; j <= hi; ++j) {
            sum += raw[j];
        }
        out.combined.values[i] = unmeasuredAt[i] ? 1.0 : sum / static_cast<double>(hi - lo + 1);
    }
    out.threshold = 0.04 + 0.32 * options.sensitivity;

    // Quiet runs at least minSeconds long, less their handles, in whole frames.
    const Rate rate = seq.frameRate;
    const Ticks frame = framesToTicks(1, rate);
    const Ticks handle{static_cast<std::int64_t>(std::llround(options.handleSeconds * static_cast<double>(kTicksPerSecond)))};
    const Ticks minimum{static_cast<std::int64_t>(std::llround(options.minSeconds * static_cast<double>(kTicksPerSecond)))};
    std::size_t i = 0;
    while (i < n) {
        if (out.combined.values[i] >= out.threshold) {
            ++i;
            continue;
        }
        std::size_t j = i;
        double sum = 0.0;
        while (j < n && out.combined.values[j] < out.threshold) {
            sum += out.combined.values[j];
            ++j;
        }
        const Ticks start = span.start + Ticks{step.value * static_cast<std::int64_t>(i)};
        const Ticks end = std::min(span.end(), span.start + Ticks{step.value * static_cast<std::int64_t>(j)});
        if (end - start >= minimum) {
            Ticks first = snapToFrame(start + handle, rate);
            if (first < start + handle) {
                first += frame;
            }
            const Ticks last = snapToFrame(end - handle, rate);
            if (last > first) {
                DeadTimeRange r;
                r.stretch = TimeRange::fromStartEnd(start, end);
                r.remove = TimeRange::fromStartEnd(first, last);
                r.activity = sum / static_cast<double>(j - i);
                out.ranges.push_back(r);
            }
        }
        i = j;
    }
    return out;
}

namespace edit {

EditOutcome removeTimeRanges(const ProjectState& state, SequenceId sequenceId, std::span<const TimeRange> ranges, bool rippleMarkers) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& seq = **found;
    std::vector<TimeRange> sorted(ranges.begin(), ranges.end());
    for (const TimeRange& r : sorted) {
        if (r.start.value < 0 || r.duration.value <= 0) {
            return std::unexpected(invalidArgument("a range to remove must be a positive span"));
        }
    }
    std::sort(sorted.begin(), sorted.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& r : sorted) {
        if (!merged.empty() && r.start <= merged.back().end()) {
            merged.back() = TimeRange::fromStartEnd(merged.back().start, std::max(merged.back().end(), r.end()));
        } else {
            merged.push_back(r);
        }
    }
    if (merged.empty()) {
        return detail::unchanged(state);
    }
    detail::SequenceEditor ed(state, seq);
    std::vector<TrackId> unlocked;
    for (const TrackId id : ed.trackIds()) {
        if (!ed.track(id).locked) {
            unlocked.push_back(id);
        }
    }
    if (unlocked.empty()) {
        return std::unexpected(invalidArgument("every track is locked"));
    }
    // The markers follow with the captions (SequenceEditor, 0.4) when asked to, whatever the sequence's own Ripple
    // Sequence Markers says: dead-time removal keeps its "move markers" option.
    ed.setMarkersFollow(rippleMarkers);
    for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
        for (const TrackId id : unlocked) {
            ed.rippleRemove(id, *it);
        }
        ed.captionsRemove(*it);
    }
    return ed.finish(merged.front().start);
}

} // namespace edit

} // namespace wabcut
