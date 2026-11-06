// The pacing heat strip's analysis (PacingMap.h).

#include <wabcut/model/PacingMap.h>

#include <wabcut/model/Resolve.h>

#include <algorithm>
#include <cmath>

namespace wabcut {

namespace {

constexpr std::size_t kMaxCells = 20'000;

// How much each signal weighs in a moment's score (their noisy-or is the score).
constexpr double kWeightLongShot = 0.80;
constexpr double kWeightStill = 0.55;
constexpr double kWeightSilence = 0.75;
constexpr double kWeightSlowSpeech = 0.45;
constexpr double kReasonGrade = 0.20; ///< a weighted grade this high is named as a reason

Ticks secondsToTicks(double seconds) {
    return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
}

double ramp(double x, double from, double to) {
    if (to <= from) {
        return x >= to ? 1.0 : 0.0;
    }
    return std::clamp((x - from) / (to - from), 0.0, 1.0);
}

/// What a curve says over [a, b): its highest and mean value; unknown when it is empty or does not reach there.
struct CurveRead {
    bool known = false;
    double max = 0.0;
    double mean = 0.0;
};

CurveRead readCurve(const ActivityCurve& curve, Ticks a, Ticks b) {
    CurveRead out;
    if (curve.values.empty() || curve.step.value <= 0 || b <= a) {
        return out;
    }
    const std::int64_t last = static_cast<std::int64_t>(curve.values.size()) - 1;
    std::int64_t i0 = (a - curve.start).value / curve.step.value;
    std::int64_t i1 = ((b - curve.start).value + curve.step.value - 1) / curve.step.value - 1;
    i0 = std::max<std::int64_t>(i0, 0);
    i1 = std::min(i1, last);
    if (i1 < i0) {
        return out;
    }
    double sum = 0.0;
    for (std::int64_t i = i0; i <= i1; ++i) {
        const double v = curve.values[static_cast<std::size_t>(i)];
        sum += v;
        out.max = std::max(out.max, v);
    }
    out.known = true;
    out.mean = sum / static_cast<double>(i1 - i0 + 1);
    return out;
}

bool inAny(const std::vector<TimeRange>& ranges, Ticks t) {
    return std::any_of(ranges.begin(), ranges.end(), [t](const TimeRange& r) { return r.contains(t); });
}

/// Whether `t` is inside one of the sorted, disjoint ranges.
bool inSorted(const std::vector<TimeRange>& ranges, Ticks t) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), t, [](Ticks value, const TimeRange& r) { return value < r.start; });
    return it != ranges.begin() && std::prev(it)->contains(t);
}

/// `next` carries on from `previous` on the same track: the same stretch of the same media played on, so the join is no cut.
bool continuesOn(const Clip& previous, const Clip& next) {
    if (previous.isNested() || next.isNested() || previous.hasUnknownSource() || next.hasUnknownSource()) {
        return false;
    }
    if (previous.end() != next.start() || previous.media != next.media || previous.stream != next.stream ||
        previous.speed != next.speed || previous.reversed != next.reversed || previous.isRemapped() || next.isRemapped() ||
        previous.isFreeze() || next.isFreeze()) {
        return false;
    }
    return previous.reversed ? next.sourceOut() == previous.sourceIn : next.sourceIn == previous.sourceOut();
}

} // namespace

std::optional<std::string> pacingOptionsProblem(const PacingOptions& o) {
    const auto bad = [](double v, double lo, double hi) { return !std::isfinite(v) || v < lo || v > hi; };
    if (bad(o.stepSeconds, 0.1, 600) || bad(o.longShotSeconds, 1, 600) || bad(o.veryLongShotSeconds, 1, 3600) ||
        o.veryLongShotSeconds < o.longShotSeconds) {
        return "the shot lengths must be 1 to 600 s, the second not shorter than the first";
    }
    if (bad(o.stillAfterSeconds, 0, 600) || bad(o.stillFullSeconds, 0, 3600) || o.stillFullSeconds < o.stillAfterSeconds ||
        bad(o.stillLevel, 0.001, 1)) {
        return "the stillness settings are out of range";
    }
    if (bad(o.silenceLevel, 0.0, 1) || bad(o.silenceSeconds, 0, 600) || bad(o.verySilentSeconds, 0, 3600) ||
        o.verySilentSeconds < o.silenceSeconds) {
        return "the silence settings are out of range";
    }
    if (bad(o.slowWordsPerMinute, 1, 600) || bad(o.wordWindowSeconds, 2, 600) || bad(o.minStretchSeconds, 0, 600)) {
        return "the speech settings are out of range";
    }
    return std::nullopt;
}

std::vector<PacingCell> pacingCells(const PacingInputs& in, const PacingOptions& o) {
    std::vector<PacingCell> cells;
    if (in.end <= in.start) {
        return cells;
    }
    Ticks step = secondsToTicks(std::max(o.stepSeconds, 0.05));
    const std::int64_t span = (in.end - in.start).value;
    std::int64_t count = (span + step.value - 1) / step.value;
    if (count > static_cast<std::int64_t>(kMaxCells)) {
        step = Ticks{(span + static_cast<std::int64_t>(kMaxCells) - 1) / static_cast<std::int64_t>(kMaxCells)};
        count = (span + step.value - 1) / step.value;
    }
    const double stepSeconds = step.toSeconds();
    cells.resize(static_cast<std::size_t>(count));

    // ---- What was measured in each cell ----
    std::vector<char> silent(cells.size(), 0);
    std::vector<char> onPicture(cells.size(), 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        PacingCell& cell = cells[i];
        cell.start = in.start + Ticks{step.value * static_cast<std::int64_t>(i)};
        const Ticks to = std::min(in.end, cell.start + step);
        const Ticks mid = cell.start + Ticks{(to - cell.start).value / 2};
        if (inSorted(in.picture, mid)) {
            const auto next = std::upper_bound(in.cuts.begin(), in.cuts.end(), mid);
            const Ticks shotStart = next == in.cuts.begin() ? in.start : *std::prev(next);
            const Ticks shotEnd = next == in.cuts.end() ? in.end : *next;
            cell.shotSeconds = static_cast<float>((shotEnd - shotStart).toSeconds());
            onPicture[i] = 1;
        }
        if (!inAny(in.unmeasured, mid)) {
            const CurveRead sound = readCurve(in.audio, cell.start, to);
            silent[i] = sound.known && sound.max < o.silenceLevel ? 1 : 0;
            const CurveRead picture = readCurve(in.motion, cell.start, to);
            if (picture.known) {
                cell.motion = static_cast<float>(picture.mean);
            }
        }
    }

    // ---- Silent runs: every cell of a run knows how long the run is ----
    for (std::size_t i = 0; i < cells.size();) {
        if (!silent[i]) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < cells.size() && silent[j]) {
            ++j;
        }
        const float seconds = static_cast<float>(static_cast<double>(j - i) * stepSeconds);
        for (std::size_t k = i; k < j; ++k) {
            cells[k].silentSeconds = seconds;
        }
        i = j;
    }

    // ---- Words a minute around each cell (the words are sorted: two moving ends) ----
    if (!in.words.empty()) {
        const Ticks half = secondsToTicks(o.wordWindowSeconds / 2.0);
        std::size_t lo = 0;
        std::size_t hi = 0;
        for (PacingCell& cell : cells) {
            const Ticks mid = cell.start + Ticks{step.value / 2};
            while (lo < in.words.size() && in.words[lo] < mid - half) {
                ++lo;
            }
            hi = std::max(hi, lo);
            while (hi < in.words.size() && in.words[hi] < mid + half) {
                ++hi;
            }
            cell.wordsPerMinute = static_cast<float>(static_cast<double>(hi - lo) * 60.0 / o.wordWindowSeconds);
        }
    }

    // ---- The grades and the score ----
    for (std::size_t i = 0; i < cells.size(); ++i) {
        PacingCell& cell = cells[i];
        const double longShot = onPicture[i] ? ramp(cell.shotSeconds, o.longShotSeconds, o.veryLongShotSeconds) : 0.0;
        double still = 0.0;
        if (onPicture[i] && cell.motion >= 0.0F) {
            const double calm = std::clamp(1.0 - static_cast<double>(cell.motion) / (2.0 * o.stillLevel), 0.0, 1.0);
            still = calm * ramp(cell.shotSeconds, o.stillAfterSeconds, o.stillFullSeconds);
        }
        const double silence = ramp(cell.silentSeconds, o.silenceSeconds, o.verySilentSeconds);
        double slow = 0.0;
        if (cell.wordsPerMinute > 0.0F && cell.wordsPerMinute < o.slowWordsPerMinute) {
            slow = (o.slowWordsPerMinute - cell.wordsPerMinute) / o.slowWordsPerMinute;
        }
        const double a = kWeightLongShot * longShot;
        const double b = kWeightStill * still;
        const double c = kWeightSilence * silence;
        const double d = kWeightSlowSpeech * slow;
        cell.score = static_cast<float>(1.0 - (1.0 - a) * (1.0 - b) * (1.0 - c) * (1.0 - d));
        cell.reasons = static_cast<std::uint8_t>((a >= kReasonGrade ? kPacingLongShot : 0) | (b >= kReasonGrade ? kPacingStill : 0) |
                                                 (c >= kReasonGrade ? kPacingSilence : 0) |
                                                 (d >= kReasonGrade ? kPacingSlowSpeech : 0));
    }
    return cells;
}

std::vector<PacingStretch> pacingStretches(const std::vector<PacingCell>& cells, Ticks end, const PacingOptions& o) {
    std::vector<PacingStretch> out;
    if (cells.empty()) {
        return out;
    }
    const Ticks step = cells.size() > 1 ? cells[1].start - cells[0].start : secondsToTicks(o.stepSeconds);
    for (std::size_t i = 0; i < cells.size();) {
        const int level = pacingLevelOf(cells[i].score);
        if (level == 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        PacingStretch s;
        s.level = level;
        double motionSum = 0.0;
        std::size_t motionCount = 0;
        while (j < cells.size() && pacingLevelOf(cells[j].score) == level) {
            const PacingCell& c = cells[j];
            s.peak = std::max(s.peak, static_cast<double>(c.score));
            s.reasons = static_cast<std::uint8_t>(s.reasons | c.reasons);
            s.longestShot = std::max(s.longestShot, static_cast<double>(c.shotSeconds));
            s.longestSilence = std::max(s.longestSilence, static_cast<double>(c.silentSeconds));
            if (c.wordsPerMinute > 0.0F && (s.slowestWpm < 0.0 || c.wordsPerMinute < s.slowestWpm)) {
                s.slowestWpm = c.wordsPerMinute;
            }
            if (c.motion >= 0.0F) {
                motionSum += c.motion;
                ++motionCount;
            }
            ++j;
        }
        if (motionCount > 0) {
            s.meanMotion = motionSum / static_cast<double>(motionCount);
        }
        s.range = TimeRange::fromStartEnd(cells[i].start, std::min(end, cells[j - 1].start + step));
        if (!s.range.isEmpty() && (level >= 3 || s.range.duration.toSeconds() >= o.minStretchSeconds)) {
            out.push_back(s);
        }
        i = j;
    }
    return out;
}

PacingInputs pacingInputsOf(const Sequence& sequence) {
    PacingInputs in;
    in.end = sequence.contentEnd();
    std::vector<const Track*> shown; // bottom to top
    for (const TrackPtr& track : sequence.videoTracks) {
        if (track && videoTrackShown(sequence, *track)) {
            shown.push_back(track.get());
        }
    }
    std::vector<Ticks> edges;
    for (const Track* track : shown) {
        for (const Clip& clip : track->clips) {
            if (clip.enabled && clip.duration().value > 0) {
                edges.push_back(clip.start());
                edges.push_back(clip.end());
            }
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    // The top clip over each interval between two edges; neighbours showing the same clip are one segment.
    struct Segment {
        Ticks from;
        Ticks to;
        const Clip* clip = nullptr;
        const Track* track = nullptr;
    };
    std::vector<Segment> segments;
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        Segment seg{edges[i], edges[i + 1], nullptr, nullptr};
        for (auto it = shown.rbegin(); it != shown.rend() && !seg.clip; ++it) {
            if (const auto at = (*it)->clipIndexAt(seg.from); at && (*it)->clips[*at].enabled) {
                seg.clip = &(*it)->clips[*at];
                seg.track = *it;
            }
        }
        if (!segments.empty() && segments.back().clip == seg.clip && segments.back().to == seg.from) {
            segments.back().to = seg.to;
        } else {
            segments.push_back(seg);
        }
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const Segment& seg = segments[i];
        if (seg.clip != nullptr) {
            // The picture's ranges: runs of clips joined end to start (cuts inside them stay cuts).
            if (!in.picture.empty() && in.picture.back().end() == seg.from) {
                in.picture.back().duration = seg.to - in.picture.back().start;
            } else {
                in.picture.push_back(TimeRange::fromStartEnd(seg.from, seg.to));
            }
        }
        if (i == 0) {
            if (seg.clip != nullptr && seg.from > Ticks{}) {
                in.cuts.push_back(seg.from); // the picture begins after a gap
            }
            continue;
        }
        const Segment& before = segments[i - 1];
        const bool throughEdit = seg.clip != nullptr && before.clip != nullptr && seg.track == before.track &&
                                 continuesOn(*before.clip, *seg.clip);
        if (!throughEdit && (seg.clip != nullptr || before.clip != nullptr)) {
            in.cuts.push_back(seg.from);
        }
    }
    if (!segments.empty() && segments.back().clip != nullptr) {
        in.cuts.push_back(segments.back().to); // the picture ends
    }
    std::sort(in.cuts.begin(), in.cuts.end());
    in.cuts.erase(std::unique(in.cuts.begin(), in.cuts.end()), in.cuts.end());
    return in;
}

std::vector<MediaId> addPacingActivity(PacingInputs& in, const ProjectState& state, SequenceId sequenceId,
                                       const ActivityLookup& activity) {
    const Sequence* sequence = state.findSequence(sequenceId);
    if (!sequence) {
        return {};
    }
    const auto analysis = findDeadTime(state, sequenceId, activity);
    if (!analysis) {
        return {};
    }
    in.audio = analysis->audio;
    in.motion = analysis->motion;
    in.unmeasured.clear();
    if (analysis->unmeasured.empty()) {
        return {};
    }
    // Media nobody measured count as quiet in findDeadTime; here they are unknown, not silent.
    for (const TrackId id : analysis->tracks) {
        const Track* track = sequence->findTrack(id);
        if (!track) {
            continue;
        }
        for (const Clip& clip : track->clips) {
            const std::vector<MediaId> leaves = mediaPlayedBy(state, clip);
            const bool unknown = std::any_of(leaves.begin(), leaves.end(), [&](MediaId m) {
                return std::find(analysis->unmeasured.begin(), analysis->unmeasured.end(), m) != analysis->unmeasured.end();
            });
            if (unknown) {
                in.unmeasured.push_back(clip.timelineRange);
            }
        }
    }
    return analysis->unmeasured;
}

} // namespace wabcut
