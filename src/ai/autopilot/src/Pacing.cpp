// Creator autopilot: pacing match (Autopilot.h). The cut rhythm of a reference and of the edit, and cuts that bring the
// edit's long shots near the reference's shot length at the same point of the running time.

#include <wabcut/ai/autopilot/Autopilot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wabcut::ai::autopilot {

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::ranges::sort(v);
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

std::string oneDecimal(double v) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", v);
    return text;
}

/// The clip the picture shows just after `t` (from the top enabled video track down), or null in a gap.
const Clip* topClipAt(const Sequence& seq, Ticks t) {
    for (auto it = seq.videoTracks.rbegin(); it != seq.videoTracks.rend(); ++it) {
        const Track& track = **it;
        if (!track.enabled) {
            continue;
        }
        for (const Clip& c : track.clips) {
            if (c.enabled && c.start() <= t && t < c.end()) {
                return &c;
            }
        }
    }
    return nullptr;
}

} // namespace

Rhythm rhythmOf(Ticks start, Ticks end, std::span<const Ticks> cuts, double minShot) {
    Rhythm r;
    r.start = start;
    r.end = std::max(end, start);
    std::vector<Ticks> sorted(cuts.begin(), cuts.end());
    std::ranges::sort(sorted);
    const Ticks shortest = seconds(minShot);
    Ticks previous = r.start;
    for (const Ticks c : sorted) {
        if (c <= r.start || c >= r.end || c - previous < shortest || r.end - c < shortest) {
            continue;
        }
        r.cuts.push_back(c);
        previous = c;
    }
    Ticks from = r.start;
    for (std::size_t i = 0; i <= r.cuts.size(); ++i) {
        const Ticks to = i < r.cuts.size() ? r.cuts[i] : r.end;
        r.shots.push_back((to - from).toSeconds());
        from = to;
    }
    const double length = (r.end - r.start).toSeconds();
    if (length <= 0.0) {
        r.shots.clear();
        return r;
    }
    r.median = median(r.shots);
    double sum = 0.0;
    for (const double s : r.shots) {
        sum += s;
    }
    r.mean = sum / static_cast<double>(r.shots.size());
    r.cutsPerMinute = static_cast<double>(r.cuts.size()) / (length / 60.0);
    // The median shot length of each eighth of the running time (shots overlapping it).
    for (std::size_t p = 0; p < kPaceParts; ++p) {
        const double a = length * static_cast<double>(p) / static_cast<double>(kPaceParts);
        const double b = length * static_cast<double>(p + 1) / static_cast<double>(kPaceParts);
        std::vector<double> in;
        double at = 0.0;
        for (const double s : r.shots) {
            if (at < b && at + s > a) {
                in.push_back(s);
            }
            at += s;
        }
        r.curve[p] = in.empty() ? r.median : median(std::move(in));
    }
    return r;
}

Rhythm editRhythm(const Sequence& seq) {
    const Ticks end = seq.contentEnd();
    std::vector<Ticks> boundaries;
    for (const TrackPtr& t : seq.videoTracks) {
        if (!t->enabled) {
            continue;
        }
        for (const Clip& c : t->clips) {
            boundaries.push_back(c.start());
            boundaries.push_back(c.end());
        }
    }
    std::ranges::sort(boundaries);
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    const Ticks frame{seq.frameRate.ticksPerUnit()};
    std::vector<Ticks> cuts;
    for (const Ticks t : boundaries) {
        if (t <= Ticks{} || t >= end) {
            continue;
        }
        const Clip* before = topClipAt(seq, t - Ticks{1});
        const Clip* after = topClipAt(seq, t);
        if (before == after) {
            continue;
        }
        // A split that goes on showing the same media without a jump is no cut on screen.
        if (before && after && before->media == after->media && before->media.isValid() &&
            std::abs((before->sourceTimeAt(t) - after->sourceTimeAt(t)).value) <= frame.value) {
            continue;
        }
        cuts.push_back(t);
    }
    return rhythmOf(Ticks{}, end, cuts);
}

double targetAt(const Rhythm& rhythm, double fraction) {
    const double x = std::clamp(fraction, 0.0, 1.0) * static_cast<double>(kPaceParts) - 0.5;
    const double lo = std::floor(x);
    const auto i0 = static_cast<std::size_t>(std::clamp(lo, 0.0, static_cast<double>(kPaceParts - 1)));
    const auto i1 = static_cast<std::size_t>(std::clamp(lo + 1.0, 0.0, static_cast<double>(kPaceParts - 1)));
    const double w = std::clamp(x - lo, 0.0, 1.0);
    return rhythm.curve[i0] * (1.0 - w) + rhythm.curve[i1] * w;
}

std::vector<CutCandidate> cutCandidates(const Sequence& sequence, const edit::SequenceTranscript& transcript) {
    std::vector<CutCandidate> out;
    for (std::size_t i = 0; i + 1 < transcript.size(); ++i) {
        const Ticks from = transcript[i].timeline.end();
        const Ticks to = transcript[i + 1].timeline.start;
        const std::string_view text = transcript.text(i);
        const bool sentence = !text.empty() && (text.back() == '.' || text.back() == '?' || text.back() == '!');
        const double gap = (to - from).toSeconds();
        if (gap < 0.15 && !sentence) {
            continue;
        }
        const Ticks at = gap > 0.0 ? from + Ticks{(to - from).value / 2} : from;
        const double strength = std::min(1.0, std::max(0.0, gap) / 0.8) * 0.7 + (sentence ? 0.3 : 0.0);
        out.push_back(CutCandidate{at, strength, sentence ? "sentence" : "pause"});
    }
    if (sequence.beatGrid) {
        for (const Beat& b : sequence.beatGrid->beatsIn(TimeRange::fromStartEnd(Ticks{}, sequence.contentEnd()))) {
            out.push_back(CutCandidate{b.time, b.isDownbeat() ? 0.5 : 0.35, "beat"});
        }
    }
    std::ranges::sort(out, [](const CutCandidate& a, const CutCandidate& b) { return a.time < b.time; });
    return out;
}

PacingPlan planPacing(const Rhythm& reference, const Rhythm& edit, std::span<const CutCandidate> candidates, Rate frameRate,
                      const PacingOptions& options, const edit::SequenceTranscript* words) {
    PacingPlan plan;
    plan.reference = reference;
    plan.edit = edit;
    if (reference.shots.empty() || edit.shots.empty()) {
        plan.warnings.emplace_back(reference.shots.empty() ? "the reference has no length" : "the sequence is empty");
        plan.after = edit;
        return plan;
    }
    if (reference.cuts.size() < 3) {
        plan.warnings.push_back("the reference barely cuts (" + std::to_string(reference.cuts.size()) +
                                " cuts found): its rhythm says little; try another video or a more sensitive shot detection");
    }
    const double strength = std::clamp(options.strength, 0.0, 1.0);
    const double length = (edit.end - edit.start).toSeconds();
    const Ticks shortest = seconds(options.minShot);
    std::vector<Ticks> all = edit.cuts;
    Ticks a = edit.start;
    for (std::size_t s = 0; s < edit.shots.size(); ++s) {
        const Ticks b = s < edit.cuts.size() ? edit.cuts[s] : edit.end;
        const double shot = (b - a).toSeconds();
        const double fraction = length > 0.0 ? ((a - edit.start).toSeconds() + shot / 2.0) / length : 0.0;
        const double reach = std::max(0.1, targetAt(reference, fraction));
        const double target = std::exp(strength * std::log(reach) + (1.0 - strength) * std::log(std::max(0.1, edit.median)));
        if (shot < target / 1.6) {
            ++plan.fasterShots;
        }
        if (shot > options.tolerance * target) {
            const auto pieces = static_cast<std::size_t>(std::max(2.0, std::round(shot / target)));
            Ticks previous = a;
            for (std::size_t j = 1; j < pieces; ++j) {
                const Ticks ideal = a + seconds(shot * static_cast<double>(j) / static_cast<double>(pieces));
                const Ticks window = seconds(0.35 * target);
                const Ticks lowest = previous + shortest;
                const Ticks highest = b - shortest;
                if (lowest >= highest) {
                    break;
                }
                const CutCandidate* best = nullptr;
                double bestScore = -1.0;
                for (const CutCandidate& c : candidates) {
                    if (c.time < ideal - window || c.time < lowest) {
                        continue;
                    }
                    if (c.time > ideal + window || c.time > highest) {
                        break;
                    }
                    const double off = std::abs((c.time - ideal).toSeconds()) / std::max(0.05, window.toSeconds());
                    const double score = c.strength - 0.5 * off;
                    if (score > bestScore) {
                        bestScore = score;
                        best = &c;
                    }
                }
                Ticks at = ideal;
                std::string kind = "even";
                if (best) {
                    at = best->time;
                    kind = best->kind;
                } else if (words && !words->empty()) {
                    // Never inside a word: the pause next to the nearer edge of the word playing there (its middle, so
                    // the frame it snaps to stays out of both words).
                    if (const auto w = words->wordAt(ideal)) {
                        const TimeRange r = (*words)[*w].timeline;
                        if (r.start <= ideal && ideal < r.end()) {
                            if (ideal - r.start < r.end() - ideal) {
                                const Ticks before = *w > 0 ? std::min((*words)[*w - 1].timeline.end(), r.start) : r.start;
                                at = before + Ticks{(r.start - before).value / 2};
                            } else {
                                const Ticks after = *w + 1 < words->size() ? std::max((*words)[*w + 1].timeline.start, r.end()) : r.end();
                                at = r.end() + Ticks{(after - r.end()).value / 2};
                            }
                            kind = "between words";
                        }
                    }
                }
                at = snapToFrame(at, frameRate);
                if (at <= lowest - Ticks{1} || at >= highest + Ticks{1}) {
                    at = snapToFrame(std::clamp(ideal, lowest, highest), frameRate);
                    kind = "even";
                }
                CutSuggestion cut;
                cut.time = at;
                cut.kind = kind;
                cut.targetSeconds = target;
                cut.shotSeconds = shot;
                const std::string where = kind == "sentence"        ? "at a sentence end"
                                          : kind == "pause"         ? "in a pause"
                                          : kind == "beat"          ? "on a beat"
                                          : kind == "between words" ? "between words"
                                                                    : "evenly spaced";
                cut.reason = "a " + oneDecimal(shot) + " s shot where the reference cuts about every " + oneDecimal(reach) + " s; " + where;
                plan.cuts.push_back(std::move(cut));
                all.push_back(at);
                previous = at;
            }
        }
        a = b;
    }
    plan.after = rhythmOf(edit.start, edit.end, all, 0.05);
    if (plan.cuts.empty()) {
        plan.warnings.emplace_back(edit.median <= reference.median * options.tolerance
                                       ? "the sequence already cuts about as often as the reference (or faster): no cuts to suggest"
                                       : "no shot is long enough for a cut");
    }
    return plan;
}

} // namespace wabcut::ai::autopilot
