#pragma once

// Fitting generated lines to their time slots (AI-12, docs/ai.md "Going global"). A line that is longer than its slot
// is sped up, pitch preserved, by at most `maxStretch` (15 %); what still does not fit starts earlier into the free
// time before it (at most half a second) and runs into the gap after it; a line that would still overlap the next one
// pushes the next lines later. Lines shorter than their slot play at their own speed from the slot's start. Every
// stretched or shifted line is reported, and a line that overlaps or ends past the next slot is flagged.

#include <wabcut/model/Time.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace wabcut::ai::dub {

struct FitOptions {
    double maxStretch = 0.15;            ///< speed-up at most 1 + maxStretch (slow-down: lines that are short stay short)
    Ticks minGap{kTicksPerSecond / 10};  ///< between two lines
    Ticks maxLead{kTicksPerSecond / 2};  ///< how much earlier than its slot a line may start
};

struct Slot {
    Ticks start{};
    Ticks duration{};
};

struct FittedLine {
    Ticks start{};        ///< where it plays on the timeline
    Ticks duration{};     ///< after the stretch
    double speed = 1.0;   ///< source seconds per output second (> 1: sped up)
    bool stretched = false;
    bool shifted = false; ///< does not start at its slot's start
    bool flagged = false; ///< still overlaps or runs past where the next line's slot starts
    std::string note;     ///< why it was shifted or flagged

    Ticks end() const { return start + duration; }
};

struct FitReport {
    std::size_t fitted = 0;    ///< as generated, in its slot
    std::size_t stretched = 0;
    std::size_t shifted = 0;
    std::size_t flagged = 0;
    double maxSpeed = 1.0;
};

/// `audio[i]` is line i's generated length; slots in time order.
std::vector<FittedLine> fitLines(std::span<const Slot> targets, std::span<const Ticks> audio, const FitOptions& options = {},
                                 FitReport* report = nullptr);

} // namespace wabcut::ai::dub
