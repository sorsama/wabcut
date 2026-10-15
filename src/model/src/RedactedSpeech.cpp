#include <wabcut/model/RedactedSpeech.h>

#include <wabcut/model/Masks.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/model/TimeRemap.h>

#include <algorithm>

namespace wabcut {

namespace {

/// The part of `clip`'s timeline range where it shows media time in [a, b), rounded outwards when the speed does not
/// divide evenly.
void mapPlainClip(const Clip& clip, Ticks a, Ticks b, std::vector<TimeRange>& out) {
    const TimeRange span = clip.timelineRange;
    if (clip.isFreeze()) {
        if (clip.sourceIn >= a && clip.sourceIn < b) {
            out.push_back(span);
        }
        return;
    }
    const TimeMap map = clip.timeMap();
    const auto ta0 = map.inverse(a, Rounding::Floor), ta1 = map.inverse(a, Rounding::Ceil);
    const auto tb0 = map.inverse(b, Rounding::Floor), tb1 = map.inverse(b, Rounding::Ceil);
    if (!ta0 || !ta1 || !tb0 || !tb1) {
        out.push_back(span); // unrepresentable: cover the clip rather than miss a word
        return;
    }
    // Forward, media a..b plays over [t(a), t(b)); reversed, over (t(b), t(a)], so a tick past t(a).
    const bool reverse = map.isReverse();
    const Ticks lo = std::max(span.start, reverse ? *tb0 : *ta0);
    const Ticks hi = std::min(span.end(), reverse ? *ta1 + Ticks{1} : *tb1);
    if (lo < hi) {
        out.push_back(TimeRange::fromStartEnd(lo, hi));
    }
}

/// The same for a remapped clip (C.6): exact in its 100 % segments (the only ones with sound), whole elsewhere.
void mapRemappedClip(const Clip& clip, const Curve& remap, Ticks a, Ticks b, std::vector<TimeRange>& out) {
    const Ticks from = clip.sourceIn;
    const Ticks to = clip.sourceOut();
    for (const RemapSegment& s : remapSegments(remap)) {
        const Ticks c0 = std::max(from, s.from);
        const Ticks c1 = std::min(to, s.to);
        if (c0 >= c1) {
            continue;
        }
        const Ticks t0 = clip.start() + (c0 - from); // curve time runs with the timeline from sourceIn
        if (s.isUnitySpeed()) {
            const Ticks shown0 = remapSourceAt(remap, c0); // then source = shown0 + (t - t0)
            const Ticks lo = std::max(t0, t0 + (a - shown0));
            const Ticks hi = std::min(t0 + (c1 - c0), t0 + (b - shown0));
            if (lo < hi) {
                out.push_back(TimeRange::fromStartEnd(lo, hi));
            }
            continue;
        }
        const auto [lo, hi] = remapShownRange(remap, c0, c1);
        if (lo < b && hi >= a) {
            out.push_back(TimeRange::fromStartEnd(t0, t0 + (c1 - c0)));
        }
    }
}

/// Where `clip` shows its source (media or nested sequence) time [a, b) on its timeline.
void mapClip(const Clip& clip, Ticks a, Ticks b, std::vector<TimeRange>& out) {
    if (const Curve* remap = clip.timeRemap()) {
        mapRemappedClip(clip, *remap, a, b, out);
    } else {
        mapPlainClip(clip, a, b, out);
    }
}

/// Nested clips looked into per redaction before precision gives way (a crafted project of doubling nests): past it, a
/// nested clip is covered whole (fail closed).
constexpr int kMaxNestedVisits = 4096;

/// One redaction's speech, looked for through nested and multicam clips (review wave 5 E5-P1-2).
struct SpeechWalk {
    const ProjectState& project;
    const Redaction& redaction;
    MediaId root;                     ///< its redaction root (subclips and stems share it: E5-P1-1)
    std::span<const TimeRange> media; ///< its media ranges
    int visits = 0;
};

/// The times of `tracks` (of one sequence, in its time) where the redaction's media plays a covered range. Clips the
/// redaction excepts are left out with everything inside them; nested clips are followed into the tracks they read
/// (a multicam clip: its angle's) and what they hold is mapped out through their own timing.
void speechIn(SpeechWalk& walk, std::span<const Track* const> tracks, std::size_t depth, std::optional<TimeRange> within,
              std::vector<TimeRange>& out) {
    for (const Track* track : tracks) {
        for (const Clip& clip : track->clips) {
            if ((within && !clip.timelineRange.overlaps(*within)) || !walk.redaction.appliesTo(clip.id)) {
                continue;
            }
            if (!clip.isNested()) {
                if (clip.media == walk.redaction.media || redactionRoot(walk.project, clip.media) == walk.root) {
                    for (const TimeRange& m : walk.media) {
                        if (!m.isEmpty()) {
                            mapClip(clip, m.start, m.end(), out);
                        }
                    }
                }
                continue;
            }
            const Sequence* nested = walk.project.findSequence(clip.sequence);
            if (!nested) {
                continue;
            }
            if (depth + 1 >= kMaxNestingDepth || ++walk.visits > kMaxNestedVisits) {
                out.push_back(clip.timelineRange); // cannot look further: cover what it shows
                continue;
            }
            const TrackKind kind = clip.stream.kind == StreamKind::Video ? TrackKind::Video : TrackKind::Audio;
            const std::vector<TrackPtr>& all = nested->tracks(kind);
            std::vector<const Track*> read;
            for (const std::size_t i : nestedTracksRead(*nested, clip)) {
                if (i < all.size() && all[i]) {
                    read.push_back(all[i].get());
                }
            }
            std::vector<TimeRange> inside;
            speechIn(walk, read, depth + 1, std::nullopt, inside);
            for (const TimeRange& r : inside) {
                mapClip(clip, r.start, r.end(), out);
            }
        }
    }
}

} // namespace

bool hasSpeechRedactions(const ProjectState& project) {
    return std::any_of(project.redactions.begin(), project.redactions.end(),
                       [](const Redaction& r) { return r.enabled && r.audio != RedactionAudio::None; });
}

std::vector<TimeRange> redactedSpeechRanges(const ProjectState& project, const Sequence& sequence, std::optional<TimeRange> within) {
    std::vector<TimeRange> out;
    std::vector<const Track*> tracks;
    sequence.forEachTrack([&](const Track& track) { tracks.push_back(&track); });
    for (const Redaction& r : project.redactions) {
        if (!r.enabled || r.audio == RedactionAudio::None) {
            continue;
        }
        std::vector<TimeRange> media = r.audioRanges;
        if (media.empty()) {
            media.push_back(r.source);
        }
        SpeechWalk walk{project, r, redactionRoot(project, r.media), media};
        speechIn(walk, tracks, 0, within, out);
    }
    std::sort(out.begin(), out.end(), [](const TimeRange& x, const TimeRange& y) { return x.start < y.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& r : out) {
        if (!merged.empty() && r.start <= merged.back().end()) {
            merged.back() = TimeRange::fromStartEnd(merged.back().start, std::max(merged.back().end(), r.end()));
        } else {
            merged.push_back(r);
        }
    }
    return merged;
}

bool overlapsRedactedSpeech(std::span<const TimeRange> ranges, Ticks start, Ticks end) {
    // Sorted and merged: the first range ending after `start` is the only candidate.
    const auto it = std::upper_bound(ranges.begin(), ranges.end(), start, [](Ticks t, const TimeRange& r) { return t < r.end(); });
    return it != ranges.end() && it->start < end;
}

std::string outwardCueText(const CaptionCue& cue, std::span<const TimeRange> ranges, std::string_view token) {
    if (ranges.empty() || !overlapsRedactedSpeech(ranges, cue.start, std::max(cue.end(), cue.start + Ticks{1}))) {
        return cue.text;
    }
    return std::string(token);
}

} // namespace wabcut
