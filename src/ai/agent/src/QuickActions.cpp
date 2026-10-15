#include <wabcut/ai/agent/QuickActions.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wabcut::ai::agent {

namespace {

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string substitute(std::string text, std::string_view key, const std::string& value) {
    for (std::size_t pos = text.find(key); pos != std::string::npos; pos = text.find(key, pos + value.size())) {
        text.replace(pos, key.size(), value);
    }
    return text;
}

} // namespace

const std::vector<QuickAction>& quickActions() {
    static const std::vector<QuickAction> actions = {
        {"removeSilences", "Remove silences longer than {value} s",
         "Ripple-deletes pauses in the speech, keeping a little room tone.", "Seconds", 1.0, 0.2, 10.0, true, false, false},
        {"removeFillers", "Remove filler words", "Cuts um, uh and similar fillers out of the speech.", "", 0, 0, 0, true,
         false, false},
        {"highlight", "Make a {value}-second highlight of this sequence",
         "Keeps the strongest self-contained moments, in order, and cuts the rest.", "Seconds", 30.0, 5.0, 600.0, true,
         false, false},
        {"topicMarkers", "Add markers at topic changes", "Adds a named sequence marker where each topic starts.", "", 0, 0,
         0, true, false, false},
        {"beatCut", "Cut to the beat of track {track}", "Moves the cuts onto the beats of a music track.", "", 0, 0, 0,
         false, true, true},
        {"captions", "Generate captions", "Makes a caption track from the transcript.", "", 0, 0, 0, true, false, false},
    };
    return actions;
}

const QuickAction* findQuickAction(std::string_view id) {
    for (const QuickAction& a : quickActions()) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

std::vector<QuickAction> visibleQuickActions(const QuickActionContext& context) {
    std::vector<QuickAction> out;
    for (const QuickAction& a : quickActions()) {
        if (a.needsBeats && !context.beatDetection) {
            continue;
        }
        out.push_back(a);
    }
    return out;
}

std::optional<QuickActionRequest> quickActionRequest(std::string_view id, std::optional<double> value,
                                                     const ProjectState& state, SequenceId sequence, std::string_view track) {
    const QuickAction* action = findQuickAction(id);
    if (!action) {
        return std::nullopt;
    }
    double v = value.value_or(action->defaultValue);
    if (!action->valueLabel.empty()) {
        v = std::clamp(v, action->minValue, action->maxValue);
    }
    const std::string seconds = number(v);
    QuickActionRequest r;
    Constraints& c = r.constraints;
    const std::string_view which = action->id;
    if (which == "removeSilences") {
        r.prompt = substitute("Remove every pause in the speech longer than {value} seconds from this sequence, keeping a "
                        "little room tone. Use transcript_cleanup with silences {value} and fillers false (check "
                        "transcript_review first if you like). Do not remove any words.",
                        "{value}", seconds);
        c.expectChanges = false; // no pause that long is a fine answer
    } else if (which == "removeFillers") {
        r.prompt = "Remove the filler words (um, uh and the like) from this sequence. Use transcript_cleanup with "
                   "fillers true and no silences. Keep every other word.";
        c.expectChanges = false;
    } else if (which == "highlight") {
        r.prompt = substitute("Make a {value}-second highlight of this sequence: read the transcript, choose the strongest "
                        "self-contained moments (whole sentences), keep them in their original order with "
                        "transcript_keepRanges and cut everything else. The result must be {value} seconds long; check "
                        "the sequence length the edit reports and adjust.",
                        "{value}", seconds);
        c.targetDuration = Ticks{static_cast<std::int64_t>(std::llround(v * 1000.0)) * (kTicksPerSecond / 1000)};
    } else if (which == "topicMarkers") {
        r.prompt = "Add a sequence marker where each new topic starts: read the transcript, find the topic changes and "
                   "add one marker at the first word of each topic, named after the topic in a few words. Do not change "
                   "any clip.";
        c.allowClipEdits = false;
    } else if (which == "beatCut") {
        const std::string music(track.empty() ? "A1" : track);
        r.prompt = "Cut to the beat of the music on track " + music + ": find its beats with analysis_detectBeats on the "
                   "music clip (markers: downbeats, grid: true), then snap the edit points of the other clips to the "
                   "beat grid with edit_snapToBeats (edges: both). Do not change the clips on " + music + ".";
        // Everything but the music track may change.
        if (const Sequence* seq = state.findSequence(sequence)) {
            std::size_t index = 0;
            const bool video = !music.empty() && (music[0] == 'V' || music[0] == 'v');
            const bool parsed = music.size() > 1 && std::all_of(music.begin() + 1, music.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
            if (parsed) {
                index = static_cast<std::size_t>(std::stoul(music.substr(1)));
            }
            TrackId musicTrack;
            const auto& list = video ? seq->videoTracks : seq->audioTracks;
            if (index >= 1 && index <= list.size()) {
                musicTrack = list[index - 1]->id;
            }
            seq->forEachTrack([&](const Track& t) {
                if (t.id != musicTrack) {
                    c.onlyTracks.push_back(t.id);
                }
            });
        }
    } else if (which == "captions") {
        r.prompt = "Generate captions for this sequence from its transcript with captions_generate. Do not change any "
                   "clip.";
        c.allowClipEdits = false;
    }
    return r;
}

Constraints mergeConstraints(const Constraints& earlier, const Constraints& parsed) {
    Constraints c = parsed;
    const Constraints none;
    if (!parsed.targetDuration && !parsed.maxDuration) {
        c.targetDuration = earlier.targetDuration;
        c.maxDuration = earlier.maxDuration;
    }
    if (parsed.protectedRanges.empty()) {
        c.protectedRanges = earlier.protectedRanges;
    }
    if (!parsed.scope) {
        c.scope = earlier.scope;
    }
    if (parsed.onlyTracks.empty()) {
        c.onlyTracks = earlier.onlyTracks;
    }
    c.keepTrackCount = parsed.keepTrackCount && earlier.keepTrackCount;
    c.allowClipEdits = earlier.allowClipEdits;
    c.allowGaps = parsed.allowGaps || earlier.allowGaps;
    c.minClipFrames = earlier.minClipFrames;
    c.expectChanges = earlier.expectChanges;
    c.durationTolerance = parsed.durationTolerance != none.durationTolerance ? parsed.durationTolerance : earlier.durationTolerance;
    return c;
}

} // namespace wabcut::ai::agent
