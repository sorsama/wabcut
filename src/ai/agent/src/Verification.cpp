#include <wabcut/ai/agent/Verification.h>

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <regex>
#include <set>

namespace wabcut::ai::agent {

namespace {

struct TrackRef {
    const Track* track = nullptr;
    std::string label; // "V1", "A2"
};

std::vector<TrackRef> tracksOf(const Sequence& seq) {
    std::vector<TrackRef> out;
    for (std::size_t i = 0; i < seq.videoTracks.size(); ++i) {
        out.push_back({seq.videoTracks[i].get(), "V" + std::to_string(i + 1)});
    }
    for (std::size_t i = 0; i < seq.audioTracks.size(); ++i) {
        out.push_back({seq.audioTracks[i].get(), "A" + std::to_string(i + 1)});
    }
    return out;
}

const Track* findTrack(const Sequence& seq, TrackId id) {
    for (const auto& t : seq.videoTracks) {
        if (t->id == id) {
            return t.get();
        }
    }
    for (const auto& t : seq.audioTracks) {
        if (t->id == id) {
            return t.get();
        }
    }
    return nullptr;
}

std::string tc(const Sequence& seq, Ticks t) { return ticksToTimecode(t, seq.frameRate, seq.dropFrame); }

Ticks oneFrame(const Sequence& seq) { return framesToTicks(1, seq.frameRate); }

/// Clip unchanged except for its position (a ripple shift).
bool onlyMoved(const Clip& before, const Clip& after) {
    Clip shifted = after;
    shifted.timelineRange.start = before.timelineRange.start;
    return shifted == before;
}

struct ContentEdit {
    std::string track;
    TrackId trackId;
    TimeRange range;  // before range (removed/modified) or after range (added)
    std::string what; // "removed", "trimmed or changed", "added"
    std::string clip; // clip name
};

/// Content edits per track: clips removed, changed (beyond a shift) or added. Tracks present in both only.
std::vector<ContentEdit> contentEdits(const Sequence& before, const Sequence& after) {
    std::vector<ContentEdit> out;
    for (const TrackRef& ref : tracksOf(before)) {
        const Track* next = findTrack(after, ref.track->id);
        if (!next) {
            continue; // reported by the track check
        }
        std::map<std::uint64_t, const Clip*> afterById;
        for (const Clip& c : next->clips) {
            afterById[c.id.value] = &c;
        }
        std::set<std::uint64_t> beforeIds;
        for (const Clip& c : ref.track->clips) {
            beforeIds.insert(c.id.value);
            auto it = afterById.find(c.id.value);
            if (it == afterById.end()) {
                out.push_back({ref.label, ref.track->id, c.timelineRange, "removed", c.name()});
            } else if (!onlyMoved(c, *it->second)) {
                out.push_back({ref.label, ref.track->id, c.timelineRange, "changed", c.name()});
            }
        }
        for (const Clip& c : next->clips) {
            if (!beforeIds.contains(c.id.value)) {
                out.push_back({ref.label, ref.track->id, c.timelineRange, "added", c.name()});
            }
        }
    }
    return out;
}

/// Source interval [lo, hi) that a clip shows over the timeline range `part` (inside the clip).
std::pair<Ticks, Ticks> sourceInterval(const Clip& c, TimeRange part) {
    if (c.isFreeze()) {
        return {c.sourceIn, c.sourceIn};
    }
    const Ticks a = c.sourceIn + toSourceTicks(part.start - c.start(), c.speed);
    const Ticks b = c.sourceIn + toSourceTicks(part.end() - c.start(), c.speed);
    if (!c.reversed) {
        return {a, b};
    }
    // Reversed: the clip's head shows the top of its source range.
    const Ticks top = c.sourceOut();
    return {top - (b - c.sourceIn), top - (a - c.sourceIn)};
}

/// Whether [lo, hi) is covered by the union of `intervals`, allowing `slack` missing at either end.
bool covered(Ticks lo, Ticks hi, std::vector<std::pair<Ticks, Ticks>> intervals, Ticks slack) {
    if (hi <= lo) {
        return true;
    }
    std::sort(intervals.begin(), intervals.end());
    Ticks reach = lo + slack;
    for (const auto& [a, b] : intervals) {
        if (a > reach) {
            break;
        }
        reach = std::max(reach, b);
    }
    return reach + slack >= hi;
}

/// What a clip shows besides where: the same media, stream, speed, direction, enabled state and content (name,
/// parameters and their keyframes in media time, effects).
bool sameRendering(const Clip& a, const Clip& b) {
    return a.media == b.media && a.stream == b.stream && a.speed == b.speed && a.reversed == b.reversed &&
           a.enabled == b.enabled && a.maintainPitch == b.maintainPitch && a.content() == b.content();
}

std::int64_t magnitude(Ticks t) { return t.value < 0 ? -t.value : t.value; }

/// Content of `before` outside `scope` that `after` no longer shows the same way (review wave 3, P2-9). Per track,
/// every stretch of every clip outside the scope must still be shown with the same rendering and the same moment of
/// its source: before the scope at the same place, after it moved by one ripple offset for the whole track (what the
/// edit inside the scope removed or inserted). A partial trim inside the scope passes; a clip removed or changed as a
/// whole because it overlaps the scope does not, and neither does another instance of the same source.
std::vector<std::string> outsideScopeLosses(const Sequence& before, const Sequence& after, TimeRange scope) {
    std::vector<std::string> problems;
    const Ticks slack = oneFrame(before);
    for (const TrackRef& ref : tracksOf(before)) {
        const Track* next = findTrack(after, ref.track->id);
        if (!next) {
            continue; // reported by the track check
        }
        struct Piece {
            const Clip* clip;
            TimeRange range;
            bool afterScope;
        };
        std::vector<Piece> pieces;
        for (const Clip& clip : ref.track->clips) {
            if (clip.start() < scope.start) {
                pieces.push_back({&clip, TimeRange::fromStartEnd(clip.start(), std::min(clip.end(), scope.start)), false});
            }
            if (clip.end() > scope.end()) {
                pieces.push_back({&clip, TimeRange::fromStartEnd(std::max(clip.start(), scope.end()), clip.end()), true});
            }
        }
        // The track's ripple offset: where the first content after the scope went (never further left than the
        // scope's whole length).
        std::optional<Ticks> shift;
        for (const Piece& piece : pieces) {
            if (!piece.afterScope) {
                continue;
            }
            const Ticks moment = piece.clip->curveTimeAt(piece.range.start);
            std::optional<Ticks> best;
            for (const Clip& other : next->clips) {
                if (!sameRendering(*piece.clip, other)) {
                    continue;
                }
                const auto at = other.timelineTimeOfCurveTime(moment);
                if (!at) {
                    continue;
                }
                const Ticks delta = *at - piece.range.start;
                if (delta.value < -(scope.duration.value + slack.value)) {
                    continue;
                }
                if (!best || magnitude(delta) < magnitude(*best)) {
                    best = delta;
                }
            }
            if (best) {
                shift = best;
                break;
            }
        }
        for (const Piece& piece : pieces) {
            const Ticks d = piece.afterScope ? shift.value_or(Ticks{0}) : Ticks{0};
            const TimeRange mapped{piece.range.start + d, piece.range.duration};
            const std::int64_t tolerance = slack.value + magnitude(toSourceTicks(slack, piece.clip->speed));
            std::vector<std::pair<Ticks, Ticks>> shown;
            for (const Clip& other : next->clips) {
                const Ticks lo = std::max(mapped.start, other.start());
                const Ticks hi = std::min(mapped.end(), other.end());
                if (hi <= lo || !sameRendering(*piece.clip, other)) {
                    continue;
                }
                // The same moment of the source at the same (shifted) place.
                if (magnitude(other.curveTimeAt(lo) - piece.clip->curveTimeAt(lo - d)) > tolerance) {
                    continue;
                }
                shown.emplace_back(lo, hi);
            }
            if (!covered(mapped.start, mapped.end(), std::move(shown), slack)) {
                problems.push_back(ref.label + " clip '" + piece.clip->name() + "' removed or changed at " +
                                   tc(before, piece.range.start) + "–" + tc(before, piece.range.end()) +
                                   " (outside the scope)");
            }
        }
    }
    return problems;
}

/// Titles and motion graphics (C.13) come and go over the picture: the gaps next to them are not holes in the edit.
bool isOverlay(const ProjectState& state, const Clip& c) {
    if (c.graphic()) {
        return true;
    }
    const MediaItem* media = c.media.isValid() ? state.findMedia(c.media) : nullptr;
    return media && (media->isTitle() || media->isGraphic());
}

std::vector<TimeRange> internalGaps(const ProjectState& state, const Track& t) {
    std::vector<TimeRange> gaps;
    for (std::size_t i = 1; i < t.clips.size(); ++i) {
        const Ticks prevEnd = t.clips[i - 1].end();
        const Ticks start = t.clips[i].start();
        if (start > prevEnd && !isOverlay(state, t.clips[i - 1]) && !isOverlay(state, t.clips[i])) {
            gaps.push_back(TimeRange::fromStartEnd(prevEnd, start));
        }
    }
    return gaps;
}

std::string listed(const std::vector<std::string>& items, std::size_t limit = 3) {
    std::string out;
    for (std::size_t i = 0; i < items.size() && i < limit; ++i) {
        out += (i ? "; " : "") + items[i];
    }
    if (items.size() > limit) {
        out += "; and " + std::to_string(items.size() - limit) + " more";
    }
    return out;
}

// --- Constraint parsing ---------------------------------------------------------------------------------------

std::optional<Ticks> durationOf(const std::string& number, const std::string& unit) {
    const double value = std::stod(number);
    if (!(value > 0) || value > 24 * 3600) {
        return std::nullopt;
    }
    const bool minutes = !unit.empty() && (unit[0] == 'm' || unit[0] == 'M');
    const double seconds = minutes ? value * 60.0 : value;
    return Ticks{static_cast<std::int64_t>(std::llround(seconds * 1000.0)) * (kTicksPerSecond / 1000)};
}

/// "1:30" (m:ss), "1:02:03" (h:mm:ss), "00:01:02:03" / "00:01:02;03" (timecode in the sequence's rate).
std::optional<Ticks> timeOf(const std::string& text, const Sequence& seq) {
    const auto colons = std::count(text.begin(), text.end(), ':') + std::count(text.begin(), text.end(), ';');
    if (colons == 3) {
        auto t = parseTimecode(text, seq.frameRate, seq.dropFrame);
        return t ? std::optional<Ticks>(*t) : std::nullopt;
    }
    std::vector<int> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(':', start);
        parts.push_back(std::stoi(text.substr(start, end == std::string::npos ? std::string::npos : end - start)));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    std::int64_t seconds = 0;
    for (int p : parts) {
        seconds = seconds * 60 + p;
    }
    return Ticks::fromSeconds(seconds);
}

} // namespace

std::string formatDuration(Ticks t) {
    const bool negative = t.value < 0;
    const std::int64_t tenths = std::llabs(t.value) / (kTicksPerSecond / 10);
    const std::int64_t seconds = tenths / 10;
    char buf[32];
    if (tenths % 10 == 0) {
        std::snprintf(buf, sizeof buf, "%s%lld:%02lld", negative ? "-" : "", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    } else {
        std::snprintf(buf, sizeof buf, "%s%lld:%02lld.%lld", negative ? "-" : "", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60), static_cast<long long>(tenths % 10));
    }
    return buf;
}

Constraints parseConstraints(std::string_view promptView, const ProjectState& state, SequenceId sequence) {
    Constraints c;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return c;
    }
    const std::string prompt(promptView);
    const auto icase = std::regex::icase;
    const std::string unit = R"((seconds?|secs?|s|minutes?|mins?|m)\b)";
    const std::string number = R"((\d+(?:\.\d+)?))";
    std::smatch m;

    // Durations of the whole result only: "a 30-second highlight", "make it 2 minutes", "keep it under 60 seconds".
    // A bare "less than 2 s" usually describes pauses or clips, so it is not read as a length.
    const std::string whole = R"((?:highlight|cut|version|edit|video|teaser|trailer|recap|summary|reel|short|sizzle|sequence))";
    const std::string verb = R"((?:make|keep|get|trim|cut|shorten|bring)\s+(?:it|this|the\s+\w+)\s+(?:down\s+)?)";
    static const std::regex target1(number + R"(\s*-?\s*)" + unit + R"([\s-]*(?:long\s+)?)" + whole, icase);
    static const std::regex target2(verb + R"((?:to\s+)?(?:exactly\s+|about\s+|around\s+|roughly\s+)?)" + number + R"(\s*)" + unit,
                                    icase);
    static const std::regex maximum(R"((?:)" + verb + R"((?:to\s+)?|)" + whole + R"(\s+(?:of\s+|that\s+is\s+|that's\s+)?))" +
                                        R"((?:under|at most|no (?:more|longer) than|less than|shorter than|max(?:imum)?(?: of)?)\s+)" +
                                        number + R"(\s*)" + unit,
                                    icase);
    if (std::regex_search(prompt, m, target1) || std::regex_search(prompt, m, target2)) {
        c.targetDuration = durationOf(m[1].str(), m[2].str());
    } else if (std::regex_search(prompt, m, maximum)) {
        c.maxDuration = durationOf(m[1].str(), m[2].str());
    }

    // Ranges: protected ranges first, then a scope that is not one of them.
    const std::string time = R"((\d{1,2}:\d{2}(?::\d{2})?(?:[:;]\d{2})?))";
    const std::string sep = "\\s*(?:-|\u2013|to|and|until|through)\\s*"; // "-", an en dash (UTF-8) or a word
    static const std::regex keep(R"((?:keep|protect|preserve|don'?t touch|do not touch|leave alone)\s+(?:the\s+)?(?:part\s+|section\s+|range\s+)?(?:from\s+|between\s+)?)" +
                                     time + sep + time,
                                 icase);
    static const std::regex between(R"((?:between|from|within)\s+)" + time + sep + time, icase);
    std::vector<std::pair<std::ptrdiff_t, std::ptrdiff_t>> used;
    for (auto it = std::sregex_iterator(prompt.begin(), prompt.end(), keep); it != std::sregex_iterator(); ++it) {
        const auto a = timeOf((*it)[1].str(), *seq);
        const auto b = timeOf((*it)[2].str(), *seq);
        if (a && b && *b > *a) {
            c.protectedRanges.push_back(TimeRange::fromStartEnd(*a, *b));
            used.emplace_back(it->position(), it->position() + it->length());
        }
    }
    for (auto it = std::sregex_iterator(prompt.begin(), prompt.end(), between); it != std::sregex_iterator(); ++it) {
        const auto pos = it->position();
        const bool inside = std::any_of(used.begin(), used.end(), [&](const auto& u) { return pos >= u.first && pos < u.second; });
        if (inside || c.scope) {
            continue;
        }
        const auto a = timeOf((*it)[1].str(), *seq);
        const auto b = timeOf((*it)[2].str(), *seq);
        if (a && b && *b > *a) {
            c.scope = TimeRange::fromStartEnd(*a, *b);
        }
    }

    // Tracks.
    static const std::regex only(R"(only\s+(?:on\s+|in\s+)?(?:the\s+)?(?:tracks?\s+)?([VA]\d+(?:\s*(?:,|and|&)\s*[VA]\d+)*))",
                                 icase);
    if (std::regex_search(prompt, m, only)) {
        static const std::regex label(R"(([VA])(\d+))", icase);
        const std::string list = m[1].str();
        for (auto it = std::sregex_iterator(list.begin(), list.end(), label); it != std::sregex_iterator(); ++it) {
            const bool video = (*it)[1].str()[0] == 'V' || (*it)[1].str()[0] == 'v';
            const std::size_t index = static_cast<std::size_t>(std::stoul((*it)[2].str()));
            const auto& tracks = video ? seq->videoTracks : seq->audioTracks;
            if (index >= 1 && index <= tracks.size()) {
                c.onlyTracks.push_back(tracks[index - 1]->id);
            }
        }
    }

    static const std::regex gaps(R"(\blift\b|leave (?:a |the )?gaps?|keep (?:the )?gaps?|with gaps)", icase);
    c.allowGaps = std::regex_search(prompt, gaps);
    static const std::regex trackChange(R"((?:add|create|insert|delete|remove)\s+(?:an?\s+|the\s+)?(?:new\s+)?(?:video\s+|audio\s+)?tracks?\b)",
                                        icase);
    c.keepTrackCount = !std::regex_search(prompt, trackChange);
    return c;
}

bool VerificationReport::passed() const {
    return std::all_of(items.begin(), items.end(), [](const VerificationItem& i) { return i.passed || !i.blocking; });
}

std::string VerificationReport::failuresForModel() const {
    std::string out;
    for (const auto& i : items) {
        if (!i.passed && i.blocking) {
            out += "- " + i.label + ": " + i.detail + "\n";
        }
    }
    return out;
}

const VerificationItem* VerificationReport::item(std::string_view id) const {
    for (const auto& i : items) {
        if (i.id == id) {
            return &i;
        }
    }
    return nullptr;
}

VerificationReport verify(const ProjectState& before, const ProjectState& after, SequenceId sequence,
                          const Constraints& c, std::size_t commandsStaged) {
    VerificationReport report;
    auto add = [&](std::string id, std::string label, std::vector<std::string> problems, bool blocking = true) {
        VerificationItem item;
        item.id = std::move(id);
        item.label = std::move(label);
        item.passed = problems.empty();
        item.detail = listed(problems);
        item.blocking = blocking;
        report.items.push_back(std::move(item));
    };

    // Invariants.
    {
        std::vector<std::string> problems;
        if (auto valid = validate(after, Validation::Structure); !valid) {
            problems.push_back(valid.error().message);
        }
        add("invariants", "The edited project is consistent", std::move(problems));
    }

    const Sequence* b = before.findSequence(sequence);
    const Sequence* a = after.findSequence(sequence);
    if (!b || !a) {
        add("sequence", "The sequence still exists", {"the sequence is missing"});
        return report;
    }
    const ChangeSet changes = diff(before, after);
    const SequenceChange* seqChange = changes.sequence(sequence);
    const bool anyChange = !changes.empty();

    // Additive project edits (AI-9): new sequences may appear; nothing else outside this sequence may change.
    {
        std::vector<std::string> problems;
        for (const SequenceChange& change : changes.sequences) {
            if (change.sequence != sequence) {
                const Sequence* other = before.findSequence(change.sequence);
                // C.11: a sequence moved to another bin (bin.move) is library organisation, not a content change.
                if (const Sequence* now = after.findSequence(change.sequence); other && now) {
                    Sequence moved = *other;
                    moved.bin = now->bin;
                    if (moved == *now) {
                        continue;
                    }
                }
                problems.push_back("sequence '" + (other ? other->name : std::to_string(change.sequence.value)) + "' changed");
            }
        }
        for (const SequenceId removed : changes.removedSequences) {
            const Sequence* gone = before.findSequence(removed);
            problems.push_back("sequence '" + (gone ? gone->name : std::to_string(removed.value)) + "' was removed");
        }
        if (!changes.removedMedia.empty()) {
            problems.push_back(std::to_string(changes.removedMedia.size()) + " media item(s) removed");
        }
        add("others", "Other sequences and media are unchanged", std::move(problems));
        if (!changes.addedSequences.empty()) {
            std::vector<std::string> tooMany;
            if (changes.addedSequences.size() > c.maxNewSequences) {
                tooMany.push_back(std::to_string(changes.addedSequences.size()) + " new sequences");
            }
            add("newSequences", "At most " + std::to_string(c.maxNewSequences) + " new sequences", std::move(tooMany));
        }
    }

    // Something was staged.
    if (c.expectChanges) {
        add("changes", "Edits were staged", anyChange && commandsStaged > 0 ? std::vector<std::string>{}
                                                                             : std::vector<std::string>{"nothing changed"});
    } else {
        add("changes", anyChange ? "Edits were staged" : "No edits were needed", {}, false);
    }

    // Duration.
    const Ticks length = a->contentEnd();
    if (c.targetDuration) {
        Ticks tolerance = c.durationTolerance;
        if (tolerance.value <= 0) {
            tolerance = Ticks{std::max(kTicksPerSecond, c.targetDuration->value / 20)};
        }
        std::vector<std::string> problems;
        const Ticks delta = length - *c.targetDuration;
        if (std::llabs(delta.value) > tolerance.value) {
            problems.push_back(formatDuration(length) + " is " + formatDuration(Ticks{std::llabs(delta.value)}) +
                               (delta.value > 0 ? " too long" : " too short"));
        }
        add("duration", "Length is " + formatDuration(*c.targetDuration) + " ± " + formatDuration(tolerance),
            std::move(problems));
    }
    if (c.maxDuration) {
        std::vector<std::string> problems;
        if (length > *c.maxDuration) {
            problems.push_back(formatDuration(length) + " is longer than " + formatDuration(*c.maxDuration));
        }
        add("duration", "Length is at most " + formatDuration(*c.maxDuration), std::move(problems));
    }

    // Tracks added or removed.
    if (c.keepTrackCount) {
        std::vector<std::string> problems;
        if (a->videoTracks.size() != b->videoTracks.size()) {
            problems.push_back("video tracks " + std::to_string(b->videoTracks.size()) + " -> " +
                               std::to_string(a->videoTracks.size()));
        }
        if (a->audioTracks.size() != b->audioTracks.size()) {
            problems.push_back("audio tracks " + std::to_string(b->audioTracks.size()) + " -> " +
                               std::to_string(a->audioTracks.size()));
        }
        add("tracks", "No tracks added or removed", std::move(problems));
    }

    const std::vector<ContentEdit> edits = contentEdits(*b, *a);

    // Only markers / captions.
    if (!c.allowClipEdits) {
        std::vector<std::string> problems;
        for (const auto& e : edits) {
            problems.push_back(e.track + " clip '" + e.clip + "' " + e.what + " at " + tc(*b, e.range.start));
        }
        if (seqChange && seqChange->trackListChanged) {
            problems.push_back("the track list changed");
        }
        add("onlyMarkers", "Only markers and captions changed", std::move(problems));
    }

    // Scope and tracks.
    if (c.scope || !c.onlyTracks.empty()) {
        std::vector<std::string> problems;
        for (const auto& e : edits) {
            // Removed and changed content is judged by what is still shown outside the scope (below); an added clip
            // by where it went.
            const bool addedOutside = c.scope && e.what == "added" && !e.range.overlaps(*c.scope);
            const bool outsideTracks = !c.onlyTracks.empty() &&
                                       std::find(c.onlyTracks.begin(), c.onlyTracks.end(), e.trackId) == c.onlyTracks.end();
            if (addedOutside || outsideTracks) {
                problems.push_back(e.track + " clip '" + e.clip + "' " + e.what + " at " + tc(*b, e.range.start));
            }
        }
        if (c.scope) {
            for (std::string& problem : outsideScopeLosses(*b, *a, *c.scope)) {
                problems.push_back(std::move(problem));
            }
        }
        std::string label = "No edits outside";
        if (c.scope) {
            label += " " + tc(*b, c.scope->start) + "–" + tc(*b, c.scope->end());
        }
        if (!c.onlyTracks.empty()) {
            label += c.scope ? " and the requested tracks" : " the requested tracks";
        }
        add("scope", label, std::move(problems));
    }

    // Protected ranges: their source content is still on the same track (it may have moved).
    if (!c.protectedRanges.empty()) {
        std::vector<std::string> problems;
        const Ticks slack = oneFrame(*b);
        for (const TimeRange& range : c.protectedRanges) {
            for (const TrackRef& ref : tracksOf(*b)) {
                const Track* next = findTrack(*a, ref.track->id);
                for (const Clip& clip : ref.track->clips) {
                    const Ticks lo = std::max(range.start, clip.start());
                    const Ticks hi = std::min(range.end(), clip.end());
                    if (hi <= lo) {
                        continue;
                    }
                    const auto [s0, s1] = sourceInterval(clip, TimeRange::fromStartEnd(lo, hi));
                    std::vector<std::pair<Ticks, Ticks>> available;
                    if (next) {
                        for (const Clip& other : next->clips) {
                            if (other.media == clip.media && other.stream == clip.stream && other.speed == clip.speed &&
                                other.reversed == clip.reversed) {
                                available.emplace_back(other.sourceIn, other.isFreeze() ? other.sourceIn : other.sourceOut());
                            }
                        }
                    }
                    if (!next || !covered(s0, s1, std::move(available), slack)) {
                        problems.push_back(ref.label + " '" + clip.name() + "' in " + tc(*b, lo) + "–" + tc(*b, hi));
                    }
                }
            }
        }
        std::string label = "Protected ranges kept:";
        for (const TimeRange& r : c.protectedRanges) {
            label += " " + tc(*b, r.start) + "–" + tc(*b, r.end());
        }
        add("protected", label, std::move(problems));
    }

    // Flash frames.
    {
        std::vector<std::string> problems;
        const Ticks minimum{oneFrame(*a).value * std::max(1, c.minClipFrames)};
        std::map<std::uint64_t, Ticks> beforeDurations;
        b->forEachTrack([&](const Track& t) {
            for (const Clip& clip : t.clips) {
                beforeDurations[clip.id.value] = clip.duration();
            }
        });
        for (const TrackRef& ref : tracksOf(*a)) {
            for (const Clip& clip : ref.track->clips) {
                if (clip.duration() >= minimum) {
                    continue;
                }
                auto it = beforeDurations.find(clip.id.value);
                if (it != beforeDurations.end() && it->second == clip.duration()) {
                    continue; // was already this short
                }
                problems.push_back(ref.label + " '" + clip.name() + "' is " +
                                   std::to_string(clip.duration().value / std::max<std::int64_t>(1, oneFrame(*a).value)) +
                                   " frame(s) at " + tc(*a, clip.start()));
            }
        }
        add("flashFrames", "No clips shorter than " + std::to_string(c.minClipFrames) + " frames", std::move(problems));
    }

    // New gaps.
    if (!c.allowGaps) {
        std::vector<std::string> problems;
        for (const TrackRef& ref : tracksOf(*a)) {
            const Track* old = findTrack(*b, ref.track->id);
            const std::size_t beforeGaps = old ? internalGaps(before, *old).size() : 0;
            const auto gapsNow = internalGaps(after, *ref.track);
            if (gapsNow.size() > beforeGaps) {
                problems.push_back(ref.label + ": " + std::to_string(gapsNow.size() - beforeGaps) + " new gap(s), first at " +
                                   tc(*a, gapsNow.front().start));
            }
        }
        add("gaps", "No new gaps", std::move(problems));
    }

    // Locked tracks.
    {
        std::vector<std::string> problems;
        for (const TrackRef& ref : tracksOf(*b)) {
            if (!ref.track->locked) {
                continue;
            }
            const Track* next = findTrack(*a, ref.track->id);
            bool same = next && next->clips.size() == ref.track->clips.size();
            for (std::size_t i = 0; same && i < next->clips.size(); ++i) {
                same = next->clips[i] == ref.track->clips[i];
            }
            if (!same) {
                problems.push_back(ref.label + " is locked but changed");
            }
        }
        add("locked", "Locked tracks untouched", std::move(problems));
    }
    return report;
}

} // namespace wabcut::ai::agent
