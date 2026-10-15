// The sequence beat grid: beat times, bars, the nearest beat; edit::setBeatGrid.

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>

#include "SequenceEditor.h"

#include <algorithm>

namespace wabcut {

namespace {

/// Ticks in a beat at 1 BPM, times the thousandths bpmMilli counts in: 60 s x 1000.
constexpr std::int64_t kBeatNumerator = 60 * kTicksPerSecond * 1000;
/// More beats than any timeline needs (a day at 1000 BPM); keeps a huge range from allocating without end.
constexpr std::size_t kMaxBeats = 2'000'000;

std::int32_t inBarOf(std::int64_t index, std::int32_t beatsPerBar) {
    const std::int64_t bpb = std::max<std::int32_t>(beatsPerBar, 1);
    return static_cast<std::int32_t>(((index % bpb) + bpb) % bpb) + 1;
}

/// Time of regular beat k (k = 0 is `phase`).
std::optional<Ticks> regularBeat(const BeatGrid& g, std::int64_t k) {
    const auto offset = mulDiv(k, kBeatNumerator, g.bpmMilli, Rounding::Nearest);
    if (!offset) {
        return std::nullopt;
    }
    return g.phase + Ticks{*offset};
}

/// The regular beat index at or before `t` (floor).
std::int64_t regularIndexBefore(const BeatGrid& g, Ticks t) {
    std::int64_t k = mulDiv((t - g.phase).value, g.bpmMilli, kBeatNumerator, Rounding::Floor).value_or(0);
    // Nearest rounding of the beat times can put beat k + 1 at or before t (or beat k after it): settle exactly.
    while (true) {
        const auto next = regularBeat(g, k + 1);
        if (!next || *next > t) {
            break;
        }
        ++k;
    }
    while (true) {
        const auto here = regularBeat(g, k);
        if (!here || *here <= t) {
            break;
        }
        --k;
    }
    return k;
}

/// Index of `phase` in explicit beats (the nearest beat when it is not one of them).
std::size_t phaseIndex(const BeatGrid& g) {
    const auto it = std::lower_bound(g.beats.begin(), g.beats.end(), g.phase);
    if (it == g.beats.end()) {
        return g.beats.empty() ? 0 : g.beats.size() - 1;
    }
    if (it != g.beats.begin() && g.phase - *(it - 1) < *it - g.phase) {
        return static_cast<std::size_t>(it - g.beats.begin()) - 1;
    }
    return static_cast<std::size_t>(it - g.beats.begin());
}

Beat explicitBeat(const BeatGrid& g, std::size_t i, std::size_t zero) {
    const auto index = static_cast<std::int64_t>(i) - static_cast<std::int64_t>(zero);
    return Beat{g.beats[i], static_cast<std::int32_t>(index), inBarOf(index, g.beatsPerBar)};
}

} // namespace

Ticks BeatGrid::period() const {
    return Ticks{mulDiv(1, kBeatNumerator, std::max<std::int64_t>(bpmMilli, 1), Rounding::Nearest).value_or(0)};
}

std::vector<Beat> BeatGrid::beatsIn(TimeRange range) const {
    std::vector<Beat> out;
    const Ticks from = std::max(range.start, Ticks{});
    const Ticks to = range.end();
    if (to <= from || bpmMilli <= 0) {
        return out;
    }
    if (!beats.empty()) {
        const std::size_t zero = phaseIndex(*this);
        for (auto it = std::lower_bound(beats.begin(), beats.end(), from); it != beats.end() && *it < to; ++it) {
            out.push_back(explicitBeat(*this, static_cast<std::size_t>(it - beats.begin()), zero));
        }
        return out;
    }
    std::int64_t k = regularIndexBefore(*this, from);
    for (; out.size() < kMaxBeats; ++k) {
        const auto t = regularBeat(*this, k);
        if (!t || *t >= to) {
            break;
        }
        if (*t >= from) {
            out.push_back(Beat{*t, static_cast<std::int32_t>(k), inBarOf(k, beatsPerBar)});
        }
    }
    return out;
}

std::optional<Beat> BeatGrid::beatAtOrAfter(Ticks t) const {
    t = std::max(t, Ticks{});
    if (bpmMilli <= 0) {
        return std::nullopt;
    }
    if (!beats.empty()) {
        const auto it = std::lower_bound(beats.begin(), beats.end(), t);
        if (it == beats.end()) {
            return std::nullopt;
        }
        return explicitBeat(*this, static_cast<std::size_t>(it - beats.begin()), phaseIndex(*this));
    }
    std::int64_t k = regularIndexBefore(*this, t);
    auto time = regularBeat(*this, k);
    if (time && *time < t) {
        ++k;
        time = regularBeat(*this, k);
    }
    if (!time) {
        return std::nullopt;
    }
    return Beat{*time, static_cast<std::int32_t>(k), inBarOf(k, beatsPerBar)};
}

std::optional<Beat> BeatGrid::nearestBeat(Ticks t) const {
    const auto after = beatAtOrAfter(t);
    std::optional<Beat> before;
    if (!beats.empty()) {
        const auto it = std::lower_bound(beats.begin(), beats.end(), std::max(t, Ticks{}));
        if (it != beats.begin()) {
            before = explicitBeat(*this, static_cast<std::size_t>(it - beats.begin()) - 1, phaseIndex(*this));
        }
    } else if (bpmMilli > 0 && t > Ticks{}) {
        const std::int64_t k = regularIndexBefore(*this, t);
        if (const auto time = regularBeat(*this, k); time && *time >= Ticks{} && *time <= t) {
            before = Beat{*time, static_cast<std::int32_t>(k), inBarOf(k, beatsPerBar)};
        }
    }
    if (before && after) {
        return t - before->time <= after->time - t ? before : after;
    }
    return before ? before : after;
}

namespace edit {

std::optional<std::string> beatGridProblem(const BeatGrid& g) {
    if (g.bpmMilli < 1000 || g.bpmMilli > 1'000'000) {
        return "the tempo must be 1..1000 BPM";
    }
    if (g.beatsPerBar < 1 || g.beatsPerBar > 32) {
        return "a bar has 1..32 beats";
    }
    if (g.phase.value < 0) {
        return "the grid's downbeat lies before 0";
    }
    for (std::size_t i = 0; i < g.beats.size(); ++i) {
        if (g.beats[i].value < 0 || (i > 0 && g.beats[i - 1] >= g.beats[i])) {
            return "beats must be at or after 0, in order, without repeats";
        }
    }
    if (!g.beats.empty() && !std::binary_search(g.beats.begin(), g.beats.end(), g.phase)) {
        return "the grid's downbeat is not one of its beats";
    }
    return std::nullopt;
}

EditOutcome setBeatGrid(const ProjectState& state, SequenceId sequenceId, std::optional<BeatGrid> grid) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (grid) {
        if (const auto problem = beatGridProblem(*grid)) {
            return std::unexpected(detail::invalidArgument(*problem));
        }
    }
    if ((*seq)->beatGrid == grid) {
        return detail::unchanged(state);
    }
    detail::SequenceEditor ed(state, **seq);
    ed.header().beatGrid = std::move(grid);
    return ed.finish();
}

} // namespace edit

} // namespace wabcut
