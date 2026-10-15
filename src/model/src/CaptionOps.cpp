// Caption track operations (FinishingOps.h: whole tracks; CaptionOps.h: cues, styles, presets).

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/FinishingOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace wabcut::edit {

namespace {

bool oneLineName(std::string_view s, std::size_t maxBytes, bool allowEmpty) {
    return (allowEmpty || !s.empty()) && s.size() <= maxBytes && isValidCaptionText(s) &&
           s.find('\n') == std::string_view::npos && s.find('\t') == std::string_view::npos;
}

void sortCues(CaptionTrack& track) {
    std::stable_sort(track.cues.begin(), track.cues.end(),
                     [](const CaptionCue& a, const CaptionCue& b) { return a.start < b.start; });
}

std::expected<void, Error> checkSpeakers(const ProjectState& state, const CaptionTrack& track) {
    for (const CaptionCue& cue : track.cues) {
        if (cue.speaker.isValid() && !state.findSpeaker(cue.speaker)) {
            return std::unexpected(detail::notFound("speaker " + std::to_string(cue.speaker.value) + " does not exist"));
        }
    }
    return {};
}

std::expected<void, Error> checkIndex(const CaptionTrack& track, std::size_t index) {
    if (index >= track.cues.size()) {
        return std::unexpected(detail::notFound("caption track " + std::to_string(track.id.value) + " has no cue " +
                                                std::to_string(index) + " (it has " +
                                                std::to_string(track.cues.size()) + ")"));
    }
    return {};
}

using TrackEdit = std::function<std::expected<void, Error>(CaptionTrack& track)>;

/// Applies `edit` to a copy of the caption track, checks the result and stores it.
EditOutcome editCaptionTrack(const ProjectState& state, SequenceId sequenceId, CaptionTrackId trackId,
                             const TrackEdit& edit, Ticks appliedDelta = {}) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto& tracks = (*seq)->captionTracks;
    const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const CaptionTrack& t) { return t.id == trackId; });
    if (it == tracks.end()) {
        return std::unexpected(detail::notFound("caption track " + std::to_string(trackId.value) + " does not exist"));
    }
    CaptionTrack next = *it;
    if (auto ok = edit(next); !ok) {
        return std::unexpected(ok.error());
    }
    if (const auto problem = captionTrackProblem(next)) {
        return std::unexpected(detail::invalidArgument("the caption track " + *problem));
    }
    if (auto ok = checkSpeakers(state, next); !ok) {
        return std::unexpected(ok.error());
    }
    if (next == *it) {
        return detail::unchanged(state);
    }
    detail::SequenceEditor ed(state, **seq);
    for (CaptionTrack& t : ed.header().captionTracks) {
        if (t.id == trackId) {
            t = std::move(next);
        }
    }
    return ed.finish(std::nullopt, appliedDelta);
}

/// The longest line of `text` in display cells (at least `minimum`), and its line count.
std::pair<int, int> layoutOf(std::string_view text, int minimum) {
    int longest = minimum;
    int lines = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        const std::string_view line = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        longest = std::max(longest, static_cast<int>(captionDisplayLength(line)));
        ++lines;
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    return {longest, std::max(1, lines)};
}

std::string trimmed(std::string_view s) {
    const std::size_t a = s.find_first_not_of(" \t\n");
    if (a == std::string_view::npos) {
        return {};
    }
    const std::size_t b = s.find_last_not_of(" \t\n");
    return std::string(s.substr(a, b - a + 1));
}

} // namespace

std::optional<std::string> captionTrackProblem(const CaptionTrack& track) {
    if (!oneLineName(track.name, kMaxCaptionNameBytes, true) || !oneLineName(track.language, kMaxCaptionNameBytes, true)) {
        return "has a name or language that is not one line of UTF-8 up to " + std::to_string(kMaxCaptionNameBytes) +
               " bytes";
    }
    if (track.cues.size() > kMaxCaptionCues) {
        return "has more than " + std::to_string(kMaxCaptionCues) + " cues";
    }
    if (const auto problem = captionStyleProblem(track.style)) {
        return "style " + *problem;
    }
    for (std::size_t i = 0; i < track.cues.size(); ++i) {
        const CaptionCue& cue = track.cues[i];
        if (cue.start.value < 0 || cue.duration.value <= 0 ||
            cue.duration.value > std::numeric_limits<std::int64_t>::max() - cue.start.value) {
            return "has a cue with a negative start or no duration";
        }
        if (cue.text.size() > kMaxCaptionTextBytes || !isValidCaptionText(cue.text)) {
            return "has a cue whose text is not UTF-8 up to " + std::to_string(kMaxCaptionTextBytes) + " bytes";
        }
        if (i > 0 && (track.cues[i - 1].start > cue.start ||
                      (!track.style.allowOverlap && track.cues[i - 1].end() > cue.start))) {
            return "has cues that overlap or are out of order (cue " + std::to_string(i) + ")";
        }
    }
    return std::nullopt;
}

EditOutcome setCaptionTrack(const ProjectState& state, SequenceId sequenceId, CaptionTrack track) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (const auto problem = captionTrackProblem(track)) {
        return std::unexpected(detail::invalidArgument("the caption track " + *problem));
    }
    if (auto ok = checkSpeakers(state, track); !ok) {
        return std::unexpected(ok.error());
    }
    detail::SequenceEditor ed(state, **seq);
    auto& tracks = ed.header().captionTracks;
    CaptionTrackId created;
    if (!track.id.isValid()) {
        if (tracks.size() >= kMaxCaptionTracks) {
            return std::unexpected(detail::invalidArgument("a sequence holds at most " +
                                                           std::to_string(kMaxCaptionTracks) + " caption tracks"));
        }
        track.id = ed.newCaptionTrackId();
        created = track.id;
        tracks.push_back(std::move(track));
    } else {
        const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const CaptionTrack& t) { return t.id == track.id; });
        if (it == tracks.end()) {
            return std::unexpected(detail::notFound("caption track " + std::to_string(track.id.value) + " does not exist"));
        }
        if (*it == track) {
            return detail::unchanged(state);
        }
        *it = std::move(track);
    }
    EditResult result = ed.finish();
    result.createdCaptionTrack = created;
    return result;
}

EditOutcome removeCaptionTrack(const ProjectState& state, SequenceId sequenceId, CaptionTrackId id) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto& existing = (*seq)->captionTracks;
    if (std::none_of(existing.begin(), existing.end(), [&](const CaptionTrack& t) { return t.id == id; })) {
        return std::unexpected(detail::notFound("caption track " + std::to_string(id.value) + " does not exist"));
    }
    detail::SequenceEditor ed(state, **seq);
    auto& tracks = ed.header().captionTracks;
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const CaptionTrack& t) { return t.id == id; }),
                 tracks.end());
    return ed.finish();
}

// --- Tracks and cues ----------------------------------------------------------------------------------------

EditOutcome addCaptionTrack(const ProjectState& state, SequenceId sequence, std::string name, std::string language,
                            CaptionStyle style) {
    CaptionTrack track;
    track.name = name.empty() ? std::string("Captions") : std::move(name);
    track.language = std::move(language);
    track.style = std::move(style);
    return setCaptionTrack(state, sequence, std::move(track));
}

EditOutcome addCaptionCue(const ProjectState& state, SequenceId sequence, CaptionTrackId track, CaptionCue cue) {
    return editCaptionTrack(state, sequence, track, [&](CaptionTrack& t) -> std::expected<void, Error> {
        const auto at = std::upper_bound(t.cues.begin(), t.cues.end(), cue.start,
                                         [](Ticks v, const CaptionCue& c) { return v < c.start; });
        t.cues.insert(at, std::move(cue));
        return {};
    });
}

EditOutcome updateCaptionCue(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::size_t index,
                             const CaptionCueChange& change) {
    return editCaptionTrack(state, sequence, track, [&](CaptionTrack& t) -> std::expected<void, Error> {
        if (auto ok = checkIndex(t, index); !ok) {
            return ok;
        }
        CaptionCue& cue = t.cues[index];
        const Ticks start = change.start.value_or(cue.start);
        const Ticks end = change.end.value_or(cue.end());
        if (end <= start) {
            return std::unexpected(detail::invalidArgument("a caption cue must end after it starts"));
        }
        cue.start = start;
        cue.duration = end - start;
        if (change.text) {
            cue.text = *change.text;
        }
        if (change.speaker) {
            cue.speaker = *change.speaker;
        }
        sortCues(t);
        return {};
    });
}

EditOutcome removeCaptionCue(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::size_t index) {
    return editCaptionTrack(state, sequence, track, [&](CaptionTrack& t) -> std::expected<void, Error> {
        if (auto ok = checkIndex(t, index); !ok) {
            return ok;
        }
        t.cues.erase(t.cues.begin() + static_cast<std::ptrdiff_t>(index));
        return {};
    });
}

EditOutcome splitCaptionCue(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::size_t index,
                            Ticks at, std::optional<std::size_t> textOffset, const LineBreaker& breaker) {
    return editCaptionTrack(state, sequence, track, [&](CaptionTrack& t) -> std::expected<void, Error> {
        if (auto ok = checkIndex(t, index); !ok) {
            return ok;
        }
        const CaptionCue cue = t.cues[index];
        if (at <= cue.start || at >= cue.end()) {
            return std::unexpected(detail::invalidArgument("the split point must lie inside the cue"));
        }
        std::string first;
        std::string second;
        if (textOffset) {
            const std::size_t o = *textOffset;
            if (o == 0 || o >= cue.text.size() || (static_cast<unsigned char>(cue.text[o]) & 0xC0) == 0x80) {
                return std::unexpected(detail::invalidArgument("textOffset must be a character boundary inside the text"));
            }
            first = cue.text.substr(0, o);
            second = cue.text.substr(o);
        } else {
            // The break opportunity whose share of the text is closest to the share of time before `at`.
            const std::string flat = flattenCaptionText(cue.text, t.language);
            const double share = static_cast<double>((at - cue.start).value) / static_cast<double>(cue.duration.value);
            const double total = static_cast<double>(std::max<std::size_t>(1, captionDisplayLength(flat)));
            std::optional<std::size_t> best;
            double bestDistance = 2.0;
            for (const std::size_t o : breaker.breakOpportunities(flat, t.language)) {
                if (o == 0 || o >= flat.size()) {
                    continue;
                }
                const double d = std::abs(static_cast<double>(captionDisplayLength(flat.substr(0, o))) / total - share);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = o;
                }
            }
            if (!best) {
                return std::unexpected(detail::invalidArgument(
                    "the cue's text has nowhere to break (one word, or unspaced text without a line breaker): give "
                    "textOffset"));
            }
            first = flat.substr(0, *best);
            second = flat.substr(*best);
        }
        first = trimmed(first);
        second = trimmed(second);
        if (first.empty() || second.empty()) {
            return std::unexpected(detail::invalidArgument("both halves of a split cue need text"));
        }
        // Each half wrapped like the cue was: its longest line and its number of lines at most.
        const auto [width, lines] = layoutOf(cue.text, 8);
        first = wrapCaptionText(first, t.language, width, lines, breaker);
        second = wrapCaptionText(second, t.language, width, lines, breaker);
        t.cues[index] = CaptionCue{cue.start, at - cue.start, std::move(first), cue.speaker};
        t.cues.insert(t.cues.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                      CaptionCue{at, cue.end() - at, std::move(second), cue.speaker});
        return {};
    });
}

EditOutcome mergeCaptionCues(const ProjectState& state, SequenceId sequence, CaptionTrackId track, std::size_t index,
                             std::size_t count, const LineBreaker& breaker, int maxCharsPerLine, int maxLines) {
    return editCaptionTrack(state, sequence, track, [&](CaptionTrack& t) -> std::expected<void, Error> {
        if (count < 2) {
            return std::unexpected(detail::invalidArgument("merging needs at least two cues"));
        }
        if (index >= t.cues.size() || count > t.cues.size() - index) {
            return std::unexpected(detail::notFound("caption track " + std::to_string(t.id.value) + " has no cues " +
                                                    std::to_string(index) + ".." + std::to_string(index + count - 1)));
        }
        const bool unspaced = isUnspacedLanguage(t.language);
        std::string text;
        Ticks end = t.cues[index].end();
        for (std::size_t k = index; k < index + count; ++k) {
            const std::string part = flattenCaptionText(t.cues[k].text, t.language);
            if (!part.empty()) {
                text += (text.empty() || unspaced ? "" : " ") + part;
            }
            end = std::max(end, t.cues[k].end());
        }
        CaptionCue merged{t.cues[index].start, end - t.cues[index].start,
                          wrapCaptionText(text, t.language, maxCharsPerLine, maxLines, breaker), t.cues[index].speaker};
        t.cues.erase(t.cues.begin() + static_cast<std::ptrdiff_t>(index),
                     t.cues.begin() + static_cast<std::ptrdiff_t>(index + count));
        t.cues.insert(t.cues.begin() + static_cast<std::ptrdiff_t>(index), std::move(merged));
        return {};
    });
}

EditOutcome shiftCaptionCues(const ProjectState& state, SequenceId sequence, CaptionTrackId track, TimeRange range,
                             Ticks delta) {
    return editCaptionTrack(
        state, sequence, track,
        [&](CaptionTrack& t) -> std::expected<void, Error> {
            bool any = false;
            for (CaptionCue& cue : t.cues) {
                if (range.contains(cue.start)) {
                    if (cue.start.value + delta.value < 0) {
                        return std::unexpected(detail::invalidArgument("a cue would start before 0"));
                    }
                    cue.start += delta;
                    any = true;
                }
            }
            if (!any) {
                return std::unexpected(detail::invalidArgument("no cue starts in that range"));
            }
            sortCues(t);
            return {};
        },
        delta);
}

EditOutcome setCaptionStyle(const ProjectState& state, SequenceId sequence, CaptionTrackId track, CaptionStyle style) {
    return editCaptionTrack(state, sequence, track, [&](CaptionTrack& t) -> std::expected<void, Error> {
        t.style = std::move(style);
        return {};
    });
}

EditOutcome setCaptionTrackInfo(const ProjectState& state, SequenceId sequence, CaptionTrackId track,
                                const CaptionTrackChange& change) {
    return editCaptionTrack(state, sequence, track, [&](CaptionTrack& t) -> std::expected<void, Error> {
        if (change.name) {
            if (change.name->empty()) {
                return std::unexpected(detail::invalidArgument("a caption track needs a name"));
            }
            t.name = *change.name;
        }
        t.language = change.language.value_or(t.language);
        t.enabled = change.enabled.value_or(t.enabled);
        return {};
    });
}

// --- Presets ------------------------------------------------------------------------------------------------

std::optional<CaptionStyle> captionPresetStyle(const ProjectState& state, std::string_view name) {
    if (const CaptionPreset* p = state.findCaptionPreset(name)) {
        return p->style;
    }
    if (const CaptionPreset* p = findBuiltinCaptionPreset(name)) {
        return p->style;
    }
    return std::nullopt;
}

std::expected<ProjectState, Error> saveCaptionPreset(const ProjectState& state, CaptionPreset preset) {
    if (!oneLineName(preset.name, kMaxCaptionNameBytes, false)) {
        return std::unexpected(detail::invalidArgument("a caption preset needs a one-line name of 1.." +
                                                       std::to_string(kMaxCaptionNameBytes) + " bytes"));
    }
    if (findBuiltinCaptionPreset(preset.name)) {
        return std::unexpected(detail::invalidArgument("\"" + preset.name + "\" is a built-in preset"));
    }
    if (const auto problem = captionStyleProblem(preset.style)) {
        return std::unexpected(detail::invalidArgument("the style " + *problem));
    }
    ProjectState out = state;
    if (const CaptionPreset* existing = state.findCaptionPreset(preset.name)) {
        out.captionPresets.edit()[static_cast<std::size_t>(existing - state.captionPresets.data())] = std::move(preset);
        return out;
    }
    if (out.captionPresets.size() >= kMaxCaptionPresets) {
        return std::unexpected(detail::invalidArgument("a project holds at most " + std::to_string(kMaxCaptionPresets) +
                                                       " caption presets"));
    }
    out.captionPresets.push_back(std::move(preset));
    return out;
}

std::expected<ProjectState, Error> removeCaptionPreset(const ProjectState& state, std::string_view name) {
    if (findBuiltinCaptionPreset(name)) {
        return std::unexpected(detail::invalidArgument("built-in presets cannot be removed"));
    }
    const CaptionPreset* p = state.findCaptionPreset(name);
    if (!p) {
        return std::unexpected(detail::notFound("there is no caption preset \"" + std::string(name) + "\""));
    }
    ProjectState out = state;
    out.captionPresets.erase(out.captionPresets.begin() + (p - state.captionPresets.data()));
    return out;
}

} // namespace wabcut::edit
