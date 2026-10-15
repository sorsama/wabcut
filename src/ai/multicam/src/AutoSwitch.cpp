#include <wabcut/ai/multicam/AutoSwitch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace wabcut::ai::multicam {

namespace {

double seconds(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }
Ticks secondsToTicks(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::string fixed(double v, int digits) {
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", digits, v);
    return text;
}

struct Desire {
    Ticks start{};
    std::size_t angle = 0;
    CutReason reason = CutReason::Speaker;
    std::optional<std::size_t> speaker;
    double confidence = 0.0;
    std::optional<std::size_t> other; ///< overlap: the second speaker
};

} // namespace

std::string_view toString(CutReason reason) {
    switch (reason) {
    case CutReason::Opening:
        return "opening";
    case CutReason::Speaker:
        return "speaker";
    case CutReason::Overlap:
        return "overlap";
    case CutReason::Laughter:
        return "laughter";
    case CutReason::Silence:
        return "silence";
    case CutReason::Variety:
        return "variety";
    case CutReason::Reaction:
        return "reaction";
    case CutReason::Back:
        return "back";
    }
    return "speaker";
}

std::vector<std::string> presetNames() { return {"calm", "interview", "energetic"}; }

std::optional<SwitchOptions> presetOptions(std::string_view name) {
    SwitchOptions o;
    if (name == "calm" || name == "podcast" || name == "podcastCalm") {
        o.preset = "calm";
        o.minHold = secondsToTicks(3.0);
        o.lead = secondsToTicks(0.3);
        o.overlapMin = secondsToTicks(1.0);
        o.silenceWide = secondsToTicks(4.0);
        o.varietyAfter = secondsToTicks(25.0);
        o.varietyLength = secondsToTicks(4.0);
        o.reactions = false;
        o.openLength = secondsToTicks(4.0);
        return o;
    }
    if (name == "interview") {
        return o; // the defaults
    }
    if (name == "energetic") {
        o.preset = "energetic";
        o.minHold = secondsToTicks(1.0);
        o.lead = secondsToTicks(0.15);
        o.overlapMin = secondsToTicks(0.4);
        o.silenceWide = secondsToTicks(2.0);
        o.varietyAfter = secondsToTicks(8.0);
        o.varietyLength = secondsToTicks(2.0);
        o.reactions = true;
        o.reactionStrength = 0.5;
        o.reactionHold = secondsToTicks(1.0);
        o.openLength = secondsToTicks(2.0);
        return o;
    }
    return std::nullopt;
}

SwitchPlan planSwitches(const SpeakerTimeline& timeline, std::span<const TimeRange> words, std::span<const std::string> angleNames,
                        TimeRange span, const SwitchOptions& options) {
    SwitchPlan plan;
    if (span.isEmpty()) {
        plan.warnings.push_back("nothing to switch: the clip is empty");
        return plan;
    }
    const auto name = [&](std::size_t angle) {
        return angle < angleNames.size() && !angleNames[angle].empty() ? angleNames[angle] : "Angle " + std::to_string(angle + 1);
    };
    const auto who = [&](std::optional<std::size_t> s) {
        return s && *s < timeline.speakers.size() ? timeline.speakers[*s].name : std::string("someone");
    };
    const std::optional<std::size_t> wide = timeline.wide();
    if (!wide) {
        plan.warnings.push_back("no wide angle: crosstalk, laughter and variety stay on the close-ups (tell which angle is wide)");
    }
    bool anyCloseUp = false;
    for (std::size_t s = 0; s < timeline.speakers.size(); ++s) {
        anyCloseUp = anyCloseUp || timeline.closeUpOf(s).has_value();
    }
    if (!anyCloseUp) {
        plan.warnings.push_back("no angle is anyone's close-up: tell which angle shows whom");
    }

    // ---- 1. What each moment wants to show ----
    std::vector<Desire> desires;
    const auto want = [&](Desire d) {
        if (!desires.empty() && desires.back().angle == d.angle) {
            return; // already showing it
        }
        desires.push_back(std::move(d));
    };
    for (const SpeakerSegment& seg : timeline.segments) {
        const Ticks a = std::max(seg.range.start, span.start);
        const Ticks b = std::min(seg.range.end(), span.end());
        if (b <= a) {
            continue;
        }
        switch (seg.kind) {
        case SegmentKind::Single: {
            const std::size_t s = seg.speakers.front();
            if (const auto close = timeline.closeUpOf(s)) {
                want(Desire{a, *close, CutReason::Speaker, s, seg.confidence, {}});
            } else if (wide) {
                want(Desire{a, *wide, CutReason::Speaker, s, seg.confidence, {}});
            }
            break;
        }
        case SegmentKind::Overlap:
            if (options.wideOnOverlap && wide && b - a >= options.overlapMin) {
                want(Desire{a, *wide, CutReason::Overlap, seg.speakers.front(), seg.confidence,
                            seg.speakers.size() > 1 ? std::optional(seg.speakers[1]) : std::nullopt});
            }
            break;
        case SegmentKind::NonSpeech:
            if (options.wideOnLaughter && wide && b - a >= Ticks{kTicksPerSecond / 2}) {
                want(Desire{a, *wide, CutReason::Laughter, {}, seg.confidence, {}});
            }
            break;
        case SegmentKind::Silence:
            if (wide && options.silenceWide.value > 0 && b - a >= options.silenceWide) {
                want(Desire{a + options.silenceWide, *wide, CutReason::Silence, {}, seg.confidence, {}});
            }
            break;
        }
    }
    // The opening: wide, or whoever the first wish is.
    std::vector<Desire> shaped;
    if (options.openWide && wide) {
        shaped.push_back(Desire{span.start, *wide, CutReason::Opening, {}, 1.0, {}});
        for (const Desire& d : desires) {
            if (d.start >= span.start + options.openLength) {
                shaped.push_back(d);
            }
        }
        // What the opening hands over to: the wish in force when it ends.
        std::optional<Desire> inForce;
        for (const Desire& d : desires) {
            if (d.start < span.start + options.openLength) {
                inForce = d;
            }
        }
        if (inForce && inForce->angle != *wide) {
            inForce->start = span.start + options.openLength;
            shaped.insert(std::find_if(shaped.begin() + 1, shaped.end(), [&](const Desire& d) { return d.start > inForce->start; }), *inForce);
        }
    } else {
        Desire first = desires.empty() ? Desire{span.start, wide.value_or(0), CutReason::Opening, {}, 0.5, {}} : desires.front();
        first.start = span.start;
        if (first.reason == CutReason::Speaker) {
            first.reason = CutReason::Opening;
        }
        shaped.push_back(first);
        for (std::size_t i = desires.empty() ? 0 : 1; i < desires.size(); ++i) {
            if (desires[i].start > span.start) {
                shaped.push_back(desires[i]);
            }
        }
    }

    // ---- 2. Lead: cut to a speaker a little before they start ----
    for (std::size_t i = 1; i < shaped.size(); ++i) {
        if (shaped[i].reason == CutReason::Speaker) {
            shaped[i].start = std::max(shaped[i - 1].start + Ticks{1}, shaped[i].start - options.lead);
        }
    }

    // ---- 3. Reactions: the listener's close-up on a strong reaction, then back ----
    if (options.reactions) {
        std::vector<Desire> withReactions;
        const auto inForceAt = [&](Ticks t) -> const Desire* {
            const Desire* d = nullptr;
            for (const Desire& x : shaped) {
                if (x.start <= t) {
                    d = &x;
                }
            }
            return d;
        };
        std::vector<Desire> extra;
        for (const Reaction& r : timeline.reactions) {
            if (r.strength < options.reactionStrength || r.range.start < span.start || r.range.end() > span.end()) {
                continue;
            }
            const auto close = timeline.closeUpOf(r.speaker);
            const Desire* base = inForceAt(r.range.start);
            // Only while someone else holds the shot on their close-up (not over a wide shot).
            const bool onSpeaker = base && (base->reason == CutReason::Speaker || base->reason == CutReason::Back ||
                                             (base->reason == CutReason::Opening && base->speaker && (!wide || base->angle != *wide)));
            if (!close || !onSpeaker || base->angle == *close) {
                continue;
            }
            const Ticks from = std::max(base->start + options.minHold, r.range.start);
            const Ticks to = std::max(from + options.reactionHold, r.range.end());
            // The next wish must come after the reaction and its return.
            const auto after = std::find_if(shaped.begin(), shaped.end(), [&](const Desire& d) { return d.start > base->start; });
            if (after != shaped.end() && after->start < to + options.minHold) {
                continue;
            }
            extra.push_back(Desire{from, *close, CutReason::Reaction, r.speaker, r.strength, {}});
            Desire back = *base;
            back.start = to;
            back.reason = CutReason::Back;
            extra.push_back(back);
        }
        withReactions = shaped;
        withReactions.insert(withReactions.end(), extra.begin(), extra.end());
        std::stable_sort(withReactions.begin(), withReactions.end(), [](const Desire& a, const Desire& b) { return a.start < b.start; });
        shaped = std::move(withReactions);
    }

    // ---- 4. Minimum hold: a wish too soon after the last cut waits, or goes ----
    std::vector<Desire> cuts;
    for (std::size_t i = 0; i < shaped.size(); ++i) {
        Desire d = shaped[i];
        if (cuts.empty()) {
            cuts.push_back(d);
            continue;
        }
        if (d.angle == cuts.back().angle) {
            continue;
        }
        const Ticks earliest = cuts.back().start + options.minHold;
        if (d.start < earliest) {
            // Still wanted when the hold ends (no later wish before then)?
            const bool superseded = i + 1 < shaped.size() && shaped[i + 1].start <= earliest;
            if (superseded) {
                continue;
            }
            d.start = earliest;
        }
        if (d.start >= span.end()) {
            continue;
        }
        cuts.push_back(d);
    }

    // ---- 5. Variety: a long close-up goes wide for a moment, then back ----
    if (wide && options.varietyAfter.value > 0) {
        std::vector<Desire> varied;
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            varied.push_back(cuts[i]);
            const Ticks end = i + 1 < cuts.size() ? cuts[i + 1].start : span.end();
            if (cuts[i].angle == *wide) {
                continue;
            }
            Ticks t = cuts[i].start + options.varietyAfter;
            while (t + options.varietyLength + options.minHold <= end) {
                varied.push_back(Desire{t, *wide, CutReason::Variety, cuts[i].speaker, 0.8, {}});
                Desire back = cuts[i];
                back.start = t + std::max(options.varietyLength, options.minHold);
                back.reason = CutReason::Back;
                varied.push_back(back);
                t = back.start + options.varietyAfter;
            }
        }
        cuts = std::move(varied);
    }

    // ---- 6. Never mid-word: to the nearest word edge that no other word covers ----
    std::vector<TimeRange> sortedWords;
    Ticks longest{};
    if (options.avoidMidWord) {
        for (const TimeRange& w : words) {
            if (!w.isEmpty()) {
                sortedWords.push_back(w);
                longest = std::max(longest, w.duration);
            }
        }
        std::sort(sortedWords.begin(), sortedWords.end(), [](const TimeRange& x, const TimeRange& y) { return x.start < y.start; });
    }
    std::vector<Ticks> moved(cuts.size());
    for (std::size_t i = 1; i < cuts.size() && !sortedWords.empty(); ++i) {
        const Ticks t = cuts[i].start;
        // The words near t (any word that could cover t or have an edge within maxSnap of it).
        const Ticks lo = t - options.maxSnap - longest;
        auto first = std::lower_bound(sortedWords.begin(), sortedWords.end(), lo, [](const TimeRange& w, Ticks v) { return w.start < v; });
        std::vector<TimeRange> near;
        for (auto it = first; it != sortedWords.end() && it->start <= t + options.maxSnap; ++it) {
            near.push_back(*it);
        }
        const auto covered = [&](Ticks x) { return std::any_of(near.begin(), near.end(), [&](const TimeRange& o) { return x > o.start && x < o.end(); }); };
        if (!covered(t)) {
            continue; // already between words
        }
        Ticks best = Ticks{std::numeric_limits<std::int64_t>::max()};
        Ticks to = t;
        for (const TimeRange& w : near) {
            for (const Ticks edge : {w.start, w.end()}) {
                const Ticks d = edge > t ? edge - t : t - edge;
                if (d <= options.maxSnap && !covered(edge) && (d < best || (d == best && edge < to))) {
                    best = d;
                    to = edge;
                }
            }
        }
        if (best.value == std::numeric_limits<std::int64_t>::max()) {
            ++plan.stats.midWord;
            continue;
        }
        moved[i] = to - t;
        cuts[i].start = to;
    }
    // Keep the order (a cut snapped onto or before the previous one goes).
    {
        std::vector<Desire> ordered;
        std::vector<Ticks> orderedMoved;
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            if (!ordered.empty() && (cuts[i].start <= ordered.back().start || cuts[i].angle == ordered.back().angle)) {
                continue;
            }
            ordered.push_back(cuts[i]);
            orderedMoved.push_back(moved[i]);
        }
        cuts = std::move(ordered);
        moved = std::move(orderedMoved);
    }

    // ---- 7. The cuts and their explanations ----
    std::vector<double> shots;
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        const Desire& d = cuts[i];
        PlannedCut c;
        c.at = d.start;
        c.angle = d.angle;
        c.reason = d.reason;
        c.speaker = d.speaker;
        c.confidence = std::clamp(d.confidence, 0.0, 1.0);
        c.moved = moved[i];
        switch (d.reason) {
        case CutReason::Opening:
            c.explanation = wide && d.angle == *wide ? "Open on " + name(d.angle) + ": the wide shot sets the scene"
                                                     : "Open on " + name(d.angle) + ": " + who(d.speaker) + " speaks first";
            break;
        case CutReason::Speaker:
            c.explanation = "Cut to " + name(d.angle) + ": " + who(d.speaker) + " starts talking";
            if (options.lead.value > 0) {
                c.explanation += " (" + fixed(seconds(options.lead), 2) + " s early)";
            }
            break;
        case CutReason::Overlap:
            c.explanation = "Wide on " + name(d.angle) + ": " + who(d.speaker) + (d.other ? " and " + who(d.other) : std::string()) +
                            " talk over each other";
            break;
        case CutReason::Laughter:
            c.explanation = "Wide on " + name(d.angle) + ": laughter";
            break;
        case CutReason::Silence:
            c.explanation = "Wide on " + name(d.angle) + ": a long pause";
            break;
        case CutReason::Variety: {
            const double held = i > 0 ? seconds(d.start - cuts[i - 1].start) : 0.0;
            c.explanation = "Wide on " + name(d.angle) + ": variety after " + fixed(held, 0) + " s on " + who(d.speaker);
            break;
        }
        case CutReason::Reaction: {
            std::string kind = "reacts";
            for (const Reaction& r : timeline.reactions) {
                if (r.speaker == d.speaker && r.range.start <= d.start + Ticks{kTicksPerSecond} && r.range.end() >= d.start - Ticks{kTicksPerSecond}) {
                    kind = r.kind;
                }
            }
            c.explanation = "Cut to " + name(d.angle) + ": " + who(d.speaker) + " " + kind;
            break;
        }
        case CutReason::Back:
            c.explanation = "Back to " + name(d.angle) + ": " + who(d.speaker) + " continues";
            break;
        }
        if (c.moved.value != 0) {
            c.explanation += " (moved " + fixed(std::abs(seconds(c.moved)), 2) + " s to a pause between words)";
            ++plan.stats.snapped;
        }
        switch (d.reason) {
        case CutReason::Speaker:
        case CutReason::Back:
            ++plan.stats.speakerCuts;
            break;
        case CutReason::Overlap:
        case CutReason::Laughter:
        case CutReason::Silence:
            ++plan.stats.wideCuts;
            break;
        case CutReason::Variety:
            ++plan.stats.varietyCuts;
            break;
        case CutReason::Reaction:
            ++plan.stats.reactionCuts;
            break;
        case CutReason::Opening:
            break;
        }
        const Ticks end = i + 1 < cuts.size() ? cuts[i + 1].start : span.end();
        shots.push_back(seconds(end - d.start));
        plan.cuts.push_back(std::move(c));
    }
    plan.stats.cuts = plan.cuts.empty() ? 0 : plan.cuts.size() - 1;
    if (!shots.empty()) {
        plan.stats.averageShot = std::accumulate(shots.begin(), shots.end(), 0.0) / static_cast<double>(shots.size());
        plan.stats.shortestShot = *std::min_element(shots.begin(), shots.end());
    }
    if (plan.stats.midWord > 0) {
        plan.warnings.push_back(std::to_string(plan.stats.midWord) + " cut(s) fall inside a word with no pause within " +
                                fixed(seconds(options.maxSnap), 1) + " s");
    }
    return plan;
}

} // namespace wabcut::ai::multicam
