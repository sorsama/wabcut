#include <wabcut/ai/dub/TimeFit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace wabcut::ai::dub {

namespace {

double seconds(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }
Ticks ticksOf(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::string secondsText(Ticks t) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f s", seconds(t));
    return text;
}

} // namespace

std::vector<FittedLine> fitLines(std::span<const Slot> targets, std::span<const Ticks> audio, const FitOptions& options, FitReport* report) {
    FitReport stats;
    std::vector<FittedLine> out;
    const std::size_t n = std::min(targets.size(), audio.size());
    out.reserve(n);
    const double maxSpeed = 1.0 + std::max(0.0, options.maxStretch);
    std::optional<Ticks> previousEnd;
    for (std::size_t i = 0; i < n; ++i) {
        const Slot& slot = targets[i];
        const Ticks length = std::max(audio[i], Ticks{1});
        FittedLine line;
        line.duration = length;
        // Speed up only as far as needed, and at most maxStretch.
        if (length > slot.duration && slot.duration > Ticks{}) {
            line.speed = std::min(maxSpeed, seconds(length) / seconds(slot.duration));
            if (line.speed > 1.0 + 1e-6) {
                line.duration = ticksOf(seconds(length) / line.speed);
                line.stretched = true;
            } else {
                line.speed = 1.0;
            }
        }
        line.start = slot.start;
        const Ticks earliest = previousEnd ? *previousEnd + options.minGap : Ticks{}; // never before the sequence's start
        Ticks overflow = line.duration - slot.duration;
        if (overflow > Ticks{}) {
            // Start a little earlier, into the free time before the slot.
            const Ticks lead = std::max(Ticks{}, std::min({overflow, options.maxLead, slot.start - earliest}));
            if (lead > Ticks{}) {
                line.start = slot.start - lead;
                line.shifted = true;
                line.note = "starts " + secondsText(lead) + " early";
            }
        }
        if (line.start < earliest) {
            line.note = "pushed " + secondsText(earliest - line.start) + " later by the line before";
            line.start = earliest;
            line.shifted = true;
        }
        line.start = std::max(line.start, Ticks{});
        // Past the next slot's start: flagged (the next line will be pushed).
        if (i + 1 < n) {
            const Ticks next = targets[i + 1].start - options.minGap;
            if (line.end() > next) {
                line.flagged = true;
                line.note = (line.note.empty() ? std::string() : line.note + "; ") + "runs " + secondsText(line.end() - next) +
                            " into the next line";
            }
        }
        stats.maxSpeed = std::max(stats.maxSpeed, line.speed);
        stats.stretched += line.stretched ? 1 : 0;
        stats.shifted += line.shifted ? 1 : 0;
        stats.flagged += line.flagged ? 1 : 0;
        stats.fitted += !line.stretched && !line.shifted && !line.flagged ? 1 : 0;
        previousEnd = line.end();
        out.push_back(std::move(line));
    }
    if (report) {
        *report = stats;
    }
    return out;
}

} // namespace wabcut::ai::dub
