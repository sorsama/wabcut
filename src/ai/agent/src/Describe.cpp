#include <wabcut/ai/agent/Describe.h>

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/model/Timecode.h>

#include <cstdio>
#include <optional>
#include <set>

namespace wabcut::ai::agent {

using command::Json;

namespace {

std::string trackLabel(const Sequence& seq, TrackId id) {
    const auto pos = seq.trackPosition(id);
    if (!pos) {
        return "track " + std::to_string(id.value);
    }
    return (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1);
}

/// A time param (ticks or a timecode string) as a timecode of the sequence.
std::string timeText(const Json& value, const Sequence* seq) {
    if (value.is_number_integer() && seq) {
        return ticksToTimecode(Ticks{value.get<std::int64_t>()}, seq->frameRate, seq->dropFrame);
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return value.dump();
}

std::string seconds(const Json& value) {
    if (!value.is_number()) {
        return value.dump();
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g s", value.get<double>());
    return buf;
}

std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string range(const Json& params, const Sequence* seq, const char* from = "from", const char* to = "to") {
    const bool hasFrom = params.contains(from);
    const bool hasTo = params.contains(to);
    if (!hasFrom && !hasTo) {
        return {};
    }
    return " (" + (hasFrom ? timeText(params[from], seq) : std::string("start")) + "–" +
           (hasTo ? timeText(params[to], seq) : std::string("end")) + ")";
}

std::string wordRanges(const Json& ranges) {
    if (!ranges.is_array()) {
        return {};
    }
    std::string out;
    std::size_t shown = 0;
    for (const Json& r : ranges) {
        if (shown == 3) {
            out += ", …";
            break;
        }
        if (r.is_array() && r.size() == 2) {
            out += (shown ? ", " : "") + r[0].dump() + (r[0] == r[1] ? "" : "–" + r[1].dump());
            ++shown;
        }
    }
    return out;
}

/// "name: value" pairs of the params other than the sequence, shortened.
std::string genericParams(const Json& params, const Sequence* seq) {
    std::string out;
    for (const auto& [key, value] : params.items()) {
        if (key == "sequence") {
            continue;
        }
        std::string v = (key == "time" || key == "start" || key == "end" || key == "from" || key == "to")
                            ? timeText(value, seq)
                            : (value.is_string() ? value.get<std::string>() : value.dump());
        if (v.size() > 40) {
            v = v.substr(0, 37) + "…";
        }
        out += (out.empty() ? "" : ", ") + key + " " + v;
        if (out.size() > 90) {
            out += ", …";
            break;
        }
    }
    return out;
}

} // namespace

namespace {

std::string describeUnguarded(const command::CommandSpec& spec, const Json& params, const ProjectState& state,
                              SequenceId sequence) {
    const Sequence* seq = state.findSequence(sequence);
    const std::string& name = spec.name;
    const Json p = params.is_object() ? params : Json::object();

    if (name == "transcript.text") {
        return "Read the transcript" + range(p, seq);
    }
    if (name == "transcript.words") {
        if (p.contains("first") || p.contains("last")) {
            return "Read words " + (p.contains("first") ? p["first"].dump() : "0") + "–" +
                   (p.contains("last") ? p["last"].dump() : "end");
        }
        return "Read the transcript's words" + range(p, seq);
    }
    if (name == "transcript.review") {
        return "Find filler words and pauses";
    }
    if (name == "transcript.cleanup") {
        const bool fillers = jsonBoolOr(p, "fillers", false);
        const bool silences = p.contains("silences") && p["silences"].is_number();
        if (fillers && silences) {
            return "Remove filler words and pauses longer than " + seconds(p["silences"]);
        }
        if (silences) {
            return "Remove pauses longer than " + seconds(p["silences"]);
        }
        return fillers ? "Remove filler words" : "Clean up the transcript";
    }
    if (name == "transcript.deleteRanges") {
        std::string what;
        if (p.contains("words") && p["words"].is_array() && !p["words"].empty()) {
            what = "words " + wordRanges(p["words"]);
        }
        if (p.contains("gaps") && p["gaps"].is_array() && !p["gaps"].empty()) {
            what += (what.empty() ? "" : " and ") + plural(p["gaps"].size(), "pause", "pauses");
        }
        return what.empty() ? "Delete words" : "Delete " + what;
    }
    if (name == "transcript.keepRanges") {
        return "Keep only words " + wordRanges(p.value("words", Json::array())) + ", cut the rest";
    }
    if (name == "transcript.moveRange") {
        return "Move words " + wordRanges(Json::array({p.value("words", Json::array())})) + " before word " +
               p.value("before", Json(0)).dump();
    }
    if (name == "timeline.removeSilences") {
        const std::size_t n = p.contains("ranges") && p["ranges"].is_array() ? p["ranges"].size() : 0;
        std::string out = "Remove " + plural(n, "silent range", "silent ranges");
        if (n > 0 && p["ranges"][0].is_object()) {
            out += " (" + timeText(p["ranges"][0].value("start", Json()), seq) + "–" +
                   timeText(p["ranges"][0].value("end", Json()), seq) + (n > 1 ? ", …)" : ")");
        }
        return out;
    }
    if (name == "marker.add") {
        std::string out = "Add marker";
        if (p.contains("name") && p["name"].is_string() && !p["name"].get<std::string>().empty()) {
            out += " “" + p["name"].get<std::string>() + "”";
        }
        return out + " at " + timeText(p.value("time", Json()), seq);
    }
    if (name == "analysis.detectBeats") {
        return "Find the beats of the music" + (p.contains("clip") && p["clip"].is_object() && p["clip"].contains("track")
                                                     ? " on " + timeText(p["clip"]["track"], nullptr)
                                                     : std::string());
    }
    if (name == "edit.snapToBeats") {
        const std::size_t n = p.contains("clips") && p["clips"].is_array() ? p["clips"].size() : 0;
        return "Snap the edges of " + plural(n, "clip", "clips") + " to the beat";
    }
    if (name == "edit.cutToBeats") {
        return "Cut clips to the beat";
    }
    if (name == "sequence.beats") {
        return "Read the beat grid" + range(p, seq);
    }
    if (name == "analysis.detectShots") {
        return "Find the shots";
    }
    if (name == "clip.splitAtShots") {
        const std::size_t n = p.contains("clips") && p["clips"].is_array() ? p["clips"].size() : 0;
        return "Split " + plural(n, "clip", "clips") + " at their shots";
    }
    if (name == "captions.generate") {
        return "Generate captions from the transcript";
    }
    if (name == "sequence.summary") {
        return "Look at the sequence" + range(p, seq);
    }
    if (name == "project.summary") {
        return "Look at the project";
    }
    if (name == "clip.details") {
        return "Look at clip " + (p.contains("clip") ? p["clip"].dump() : std::string());
    }
    if (name == "clip.find") {
        return "Find clips" + (p.contains("name") ? " named “" + jsonStringOr(p, "name") + "”" : std::string());
    }
    if (name == "media.list") {
        return "Search media" + (p.contains("name") ? " for “" + jsonStringOr(p, "name") + "”" : std::string());
    }
    const std::string args = genericParams(p, seq);
    return args.empty() ? spec.title : spec.title + " (" + args + ")";
}

} // namespace

std::string describeCall(const command::CommandSpec& spec, const Json& params, const ProjectState& state,
                         SequenceId sequence) {
    // A description is a label: whatever the params hold (the agent validates them first), it must never throw.
    try {
        return describeUnguarded(spec, params, state, sequence);
    } catch (const std::exception&) {
        return spec.title;
    }
}

std::string describeChanges(const ChangeSet& changes, const ProjectState& before, const ProjectState& after) {
    std::string out;
    auto add = [&](const std::string& part) { out += (out.empty() ? "" : "; ") + part; };
    for (const SequenceChange& sc : changes.sequences) {
        const Sequence* b = before.findSequence(sc.sequence);
        const Sequence* a = after.findSequence(sc.sequence);
        // The first count names the noun: "2 clips removed, 1 added, 3 changed".
        std::string part;
        auto count = [&](std::size_t n, const char* verb) {
            if (n == 0) {
                return;
            }
            part += part.empty() ? plural(n, "clip ", "clips ") + verb : ", " + std::to_string(n) + " " + verb;
        };
        // A clip that only slid along its track (a ripple) is "moved", not "changed".
        std::size_t moved = 0;
        for (ClipId id : sc.modifiedClips) {
            const auto was = b ? b->findClip(id) : std::nullopt;
            const auto now = a ? a->findClip(id) : std::nullopt;
            if (was && now && was->track->id == now->track->id) {
                Clip shifted = now->clip();
                shifted.timelineRange.start = was->clip().timelineRange.start;
                moved += shifted == was->clip() ? 1 : 0;
            }
        }
        count(sc.removedClips.size(), "removed");
        count(sc.addedClips.size(), "added");
        count(sc.modifiedClips.size() - moved, "changed");
        count(moved, "moved");
        if (!part.empty()) {
            std::set<std::string> labels;
            std::string tracks;
            for (const TrackChange& tc : sc.tracks) {
                if (tc.ranges.empty()) {
                    continue;
                }
                const Sequence* s = a && a->findTrack(tc.track) ? a : b;
                const std::string label = s ? trackLabel(*s, tc.track) : "?";
                if (labels.insert(label).second) {
                    tracks += (tracks.empty() ? "" : ", ") + label;
                }
            }
            add(tracks.empty() ? part : part + " on " + tracks);
        }
        if (sc.trackListChanged) {
            add("tracks added or removed");
        }
        if (sc.markersChanged) {
            const std::size_t nb = b ? b->markers.size() : 0;
            const std::size_t na = a ? a->markers.size() : 0;
            add(na > nb ? plural(na - nb, "marker added", "markers added") : "markers changed");
        }
        if (sc.captionsChanged) {
            add("captions changed");
        }
        if (sc.marksChanged) {
            add("In/Out marks changed");
        }
        if (sc.settingsChanged) {
            add("sequence settings changed");
        }
        if (a && b && a->contentEnd() != b->contentEnd()) {
            add("length " + ticksToTimecode(b->contentEnd(), b->frameRate, b->dropFrame) + " → " +
                ticksToTimecode(a->contentEnd(), a->frameRate, a->dropFrame));
        }
    }
    if (!changes.addedSequences.empty() || !changes.removedSequences.empty()) {
        add("sequences added or removed");
    }
    if (!changes.addedMedia.empty() || !changes.removedMedia.empty() || !changes.modifiedMedia.empty()) {
        add("media changed");
    }
    if (changes.binsChanged) {
        add("bins changed");
    }
    return out;
}

} // namespace wabcut::ai::agent
