// Source Monitor commands (Premiere's three- and four-point editing): a source's details, its In/Out marks (stored
// on the media item), insert / overwrite from it with source patching and the Fit Clip choice, track targeting, and
// match frame / reverse match frame. Thin adapters over the model's EditOps and FinishingOps.
//
// Source times (marks, positions) are ticks or a timecode in the media's own rate (sourceTimecodeRate). Source
// patching is the host's view state (SourceServices.h): what a host should show or keep comes back in `data`.

#include "Catalog.h"

#include <wabcut/command/SourceServices.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace wabcut::command {

Rate sourceTimecodeRate(const ProjectState& state, const MediaItem& media) {
    if (media.info.hasVideo()) {
        const Rate r = media.info.video.front().frameRate;
        if (r.num > 0 && r.den > 0) {
            return r;
        }
    }
    if (!state.sequences.empty() && state.sequences.front() && state.sequences.front()->frameRate.isValid()) {
        return state.sequences.front()->frameRate;
    }
    return kRate30;
}

std::string sourceStreamLabel(StreamRef stream) {
    return (stream.kind == StreamKind::Video ? "V" : "A") + std::to_string(stream.index + 1);
}

} // namespace wabcut::command

namespace wabcut::command::detail {

// --- Source times ------------------------------------------------------------------------------------------

Json sourceTimeParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string", "null"})},
                {"description", description + " Ticks, or a timecode in the media's frame rate (\"00:00:02:10\")."}};
}

/// A source time param: ticks or a timecode in `rate`; null is nullopt.
std::optional<Ticks> sourceTimeValue(const Json& value, const std::string& path, Rate rate) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (value.is_number_integer()) {
        return Ticks{value.get<std::int64_t>()};
    }
    if (value.is_string()) {
        const auto t = parseTimecode(value.get<std::string>(), rate, false);
        if (!t) {
            fail(ErrorCode::InvalidParams, "not a timecode: " + value.get<std::string>(), Json{{"path", path}});
        }
        return *t;
    }
    fail(ErrorCode::InvalidParams, "expected ticks or a timecode", Json{{"path", path}});
}

namespace {

Json sourceTimeJson(Ticks t, Rate rate) {
    return Json{{"ticks", t.value}, {"timecode", ticksToTimecode(t, rate, false)}};
}

Json marksJson(const edit::SourceMarks& marks, Rate rate) {
    return Json{{"in", marks.in ? sourceTimeJson(*marks.in, rate) : Json(nullptr)},
                {"out", marks.out ? sourceTimeJson(*marks.out, rate) : Json(nullptr)}};
}

const MediaItem& mediaItem(ExecutionContext& c) { return *c.state().findMedia(c.media("media")); }

bool given(ExecutionContext& c, const char* key) { return c.params().is_object() && c.params().contains(key); }

// --- Source patching ---------------------------------------------------------------------------------------

std::optional<StreamRef> parseStreamLabel(const std::string& label) {
    if (label.size() < 2 || (label[0] != 'V' && label[0] != 'A')) {
        return std::nullopt;
    }
    int number = 0;
    for (std::size_t i = 1; i < label.size(); ++i) {
        if (label[i] < '0' || label[i] > '9' || number > 999) {
            return std::nullopt;
        }
        number = number * 10 + (label[i] - '0');
    }
    if (number < 1) {
        return std::nullopt;
    }
    return StreamRef{label[0] == 'V' ? StreamKind::Video : StreamKind::Audio, number - 1};
}

/// Checks a patching against the sequence and the media and puts it in source order (video, then audio).
std::vector<SourcePatchSetting> normalized(const Sequence& seq, const MediaItem& media,
                                           std::vector<SourcePatchSetting> patches, const char* path) {
    std::sort(patches.begin(), patches.end(), [](const SourcePatchSetting& a, const SourcePatchSetting& b) {
        return std::pair(a.source.kind != StreamKind::Video, a.source.index) <
               std::pair(b.source.kind != StreamKind::Video, b.source.index);
    });
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const SourcePatchSetting& p = patches[i];
        const std::string label = sourceStreamLabel(p.source);
        if (!media.hasStream(p.source)) {
            fail(ErrorCode::InvalidParams, "'" + media.name + "' has no " + label + " source track", Json{{"path", path}});
        }
        if (i > 0 && patches[i - 1].source == p.source) {
            fail(ErrorCode::InvalidParams, label + " is patched twice", Json{{"path", path}});
        }
        if (!p.newTrack.empty() && p.source.kind != StreamKind::Audio) {
            fail(ErrorCode::InvalidParams, label + " cannot name or add a track: only audio source tracks can",
                 Json{{"path", path}});
        }
        if (!p.destination) {
            continue;
        }
        const Track* track = seq.findTrack(p.destination);
        if (!track) {
            fail(ErrorCode::NotFound, "track " + std::to_string(p.destination.value) + " does not exist",
                 Json{{"path", path}});
        }
        if (streamKindOf(track->kind) != p.source.kind) {
            fail(ErrorCode::InvalidParams, label + " cannot go to a " +
                                               (track->kind == TrackKind::Video ? "video" : "audio") + " track",
                 Json{{"path", path}});
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (p.enabled && patches[j].enabled && patches[j].destination == p.destination) {
                fail(ErrorCode::InvalidParams, "two source tracks are patched to one track", Json{{"path", path}});
            }
        }
    }
    return patches;
}

/// The "patches" param: [{source: "V1", track: "V2" | null, enabled: true, newTrack: "Mic"}].
std::vector<SourcePatchSetting> patchesParam(ExecutionContext& c, const Sequence& seq, const MediaItem& media) {
    const Json& list = c.params()["patches"];
    std::vector<SourcePatchSetting> patches;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string path = "/patches/" + std::to_string(i);
        const auto source = parseStreamLabel(list[i].value("source", std::string()));
        if (!source) {
            fail(ErrorCode::InvalidParams, "source must be \"V1\", \"A1\", \"A2\", ...", Json{{"path", path + "/source"}});
        }
        SourcePatchSetting p{*source, {}, list[i].value("enabled", true)};
        p.newTrack = list[i].value("newTrack", std::string());
        if (list[i].contains("track") && !list[i]["track"].is_null()) {
            p.destination = c.trackValue(list[i]["track"], path + "/track");
        } else if (p.newTrack.empty()) {
            p.enabled = false; // an indicator without a track takes no part
        }
        patches.push_back(p);
    }
    return normalized(seq, media, std::move(patches), "/patches");
}

/// What a source edit uses: the "patches" param, else the host's patching (dropping what no longer fits), else
/// Premiere's default.
std::vector<SourcePatchSetting> effectivePatches(ExecutionContext& c, const Sequence& seq, const MediaItem& media) {
    if (given(c, "patches")) {
        return patchesParam(c, seq, media);
    }
    if (auto* host = findService<SourceMonitorServices>(c.services())) {
        std::vector<SourcePatchSetting> kept;
        for (const SourcePatchSetting& p : host->sourcePatching(seq.id, media.id)) {
            const Track* track = p.destination ? seq.findTrack(p.destination) : nullptr;
            if (media.hasStream(p.source) && (!p.destination || (track && streamKindOf(track->kind) == p.source.kind))) {
                kept.push_back(p);
            }
        }
        if (!kept.empty()) {
            return normalized(seq, media, std::move(kept), "/patches");
        }
    }
    // Premiere's default; a multi-track recording's audio routed by name (A.10), possibly to new tracks.
    std::vector<SourcePatchSetting> defaults;
    for (const edit::SourcePatch& p : edit::defaultSourcePatches(seq, media)) {
        defaults.push_back({p.source, p.destination, true, p.newTrack});
    }
    return defaults;
}

Json patchesJson(ExecutionContext& c, const std::vector<SourcePatchSetting>& patches) {
    Json out = Json::array();
    for (const SourcePatchSetting& p : patches) {
        Json patch{{"source", sourceStreamLabel(p.source)},
                   {"track", p.destination ? Json(c.trackLabel(p.destination)) : Json(nullptr)},
                   {"enabled", p.enabled}};
        if (!p.newTrack.empty()) {
            patch["newTrack"] = p.newTrack;
        }
        out.push_back(std::move(patch));
    }
    return out;
}

Json patchesSchema() {
    return schema::array(
        schema::object({{"source", schema::string("Source track: \"V1\" (the video), \"A1\", \"A2\", ... (audio streams).")},
                        {"track", Json{{"type", Json::array({"string", "integer", "null"})},
                                       {"description", "Destination track (\"V2\", \"A1\", ...), or null for none."}}},
                        {"enabled", schema::boolean("false switches the indicator off (Alt-click). Default true.", true)},
                        {"newTrack", Json{{"type", "string"},
                                          {"maxLength", 256},
                                          {"description",
                                           "Audio source tracks: the name of the track it goes to (multi-track "
                                           "recordings). Without \"track\" the edit adds an audio track with this "
                                           "name below the last one; an unnamed \"track\" is given the name. A "
                                           "recognised name sets the new track's role (Mic, Discord: dialogue; "
                                           "Music: music; Game, Alerts: effects)."}}}},
                       {"source"}),
        "Source patching: which source track goes to which sequence track.");
}

/// Labels ("A4") of the tracks an edit created, in `state` (they do not exist in the context's state yet).
Json createdTracksJson(const ProjectState& state, SequenceId seqId, const std::vector<TrackId>& tracks) {
    Json out = Json::array();
    const Sequence* seq = state.findSequence(seqId);
    for (const TrackId id : tracks) {
        std::optional<Sequence::TrackPosition> pos;
        if (seq) {
            pos = seq->trackPosition(id);
        }
        out.push_back(pos ? (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1)
                          : std::to_string(id.value));
    }
    return out;
}

// --- Targeting ---------------------------------------------------------------------------------------------

/// `state` with exactly `targets` targeted in the sequence.
ProjectState retargeted(ExecutionContext& c, ProjectState state, SequenceId seqId, const std::vector<TrackId>& targets) {
    std::vector<std::pair<TrackId, bool>> changes;
    state.findSequence(seqId)->forEachTrack([&](const Track& t) {
        const bool want = std::find(targets.begin(), targets.end(), t.id) != targets.end();
        if (t.targeted != want) {
            changes.emplace_back(t.id, want);
        }
    });
    for (const auto& [id, want] : changes) {
        edit::TrackStateChange change;
        change.targeted = want;
        state = c.take(edit::setTrackState(state, seqId, id, change)).state;
    }
    return state;
}

Json targetsJson(ExecutionContext& c, const ProjectState& state, SequenceId seqId) {
    Json out = Json::array();
    for (const TrackId id : edit::targetedTracks(*state.findSequence(seqId))) {
        out.push_back(c.trackLabel(id));
    }
    return out;
}

// --- source.load / setMarks / clearMarks -------------------------------------------------------------------

CommandOutput loadSource(ExecutionContext& c) {
    const MediaItem& media = mediaItem(c);
    const Rate rate = sourceTimecodeRate(c.state(), media);
    bool clamped = false;
    const edit::SourceMarks marks = edit::storedSourceMarks(media, &clamped);
    const std::optional<Ticks> extent = edit::sourceMarkExtent(media);
    std::optional<Ticks> position;
    if (given(c, "time")) {
        position = sourceTimeValue(c.params()["time"], "/time", rate);
    }
    if (position && (position->value < 0 || (extent && *position >= *extent))) {
        fail(ErrorCode::InvalidParams, "the time is outside the media", Json{{"path", "/time"}});
    }
    const char* kind = !media.info.hasVideo() ? "audio" : media.info.isStillImage || !extent ? "still" : "video";
    Json data{{"media", media.id.value},
              {"name", media.name},
              {"kind", kind},
              {"frameRate", Json{{"num", rate.num}, {"den", rate.den}}},
              {"duration", extent ? sourceTimeJson(*extent, rate) : Json(nullptr)},
              {"streams", Json{{"video", media.info.video.size()}, {"audio", media.info.audio.size()}}},
              {"marks", marksJson(marks, rate)},
              {"position", sourceTimeJson(position.value_or(marks.in.value_or(Ticks{})), rate)}};
    if (given(c, "sequence") || c.state().sequences.size() == 1) {
        data["patches"] = patchesJson(c, effectivePatches(c, c.sequence(), media));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    if (clamped) {
        out.warnings.push_back("the stored marks of '" + media.name + "' lie past the end of the media; they were clamped");
    }
    return out;
}

CommandOutput setMarks(ExecutionContext& c, bool clear) {
    const MediaItem& media = mediaItem(c);
    const Rate rate = sourceTimecodeRate(c.state(), media);
    edit::SourceMarks marks{media.markIn, media.markOut};
    if (clear) {
        if (c.boolean("in", true)) {
            marks.in.reset();
        }
        if (c.boolean("out", true)) {
            marks.out.reset();
        }
    } else {
        const bool inGiven = given(c, "in");
        const bool outGiven = given(c, "out");
        if (inGiven) {
            marks.in = sourceTimeValue(c.params()["in"], "/in", rate);
        }
        if (outGiven) {
            marks.out = sourceTimeValue(c.params()["out"], "/out", rate);
        }
        // Premiere: an In set at or after the Out (or an Out at or before the In) drops the other mark.
        if (marks.in && marks.out && *marks.out <= *marks.in) {
            if (inGiven && !outGiven) {
                marks.out.reset();
            } else if (outGiven && !inGiven) {
                marks.in.reset();
            }
        }
    }
    edit::EditResult r = c.take(edit::setSourceMarks(c.state(), media.id, marks));
    Json data{{"media", media.id.value}, {"marks", marksJson(marks, rate)}};
    return edited(std::move(r), std::move(data));
}

// --- Insert / overwrite from the source ---------------------------------------------------------------------

/// Premiere's Fit Clip dialog choices for a four-point edit whose source and sequence ranges differ.
enum class Fit { None, Speed, IgnoreSourceIn, IgnoreSourceOut, IgnoreSequenceIn, IgnoreSequenceOut };

Fit fitOf(const std::string& name) {
    if (name == "speed") {
        return Fit::Speed;
    }
    if (name == "ignoreSourceIn") {
        return Fit::IgnoreSourceIn;
    }
    if (name == "ignoreSourceOut") {
        return Fit::IgnoreSourceOut;
    }
    if (name == "ignoreSequenceIn") {
        return Fit::IgnoreSequenceIn;
    }
    if (name == "ignoreSequenceOut") {
        return Fit::IgnoreSequenceOut;
    }
    return Fit::None;
}

const char* fitName(Fit fit) {
    switch (fit) {
    case Fit::Speed:
        return "speed";
    case Fit::IgnoreSourceIn:
        return "ignoreSourceIn";
    case Fit::IgnoreSourceOut:
        return "ignoreSourceOut";
    case Fit::IgnoreSequenceIn:
        return "ignoreSequenceIn";
    case Fit::IgnoreSequenceOut:
        return "ignoreSequenceOut";
    case Fit::None:
        break;
    }
    return "";
}

void append(std::vector<ClipId>& to, const std::vector<ClipId>& from) { to.insert(to.end(), from.begin(), from.end()); }

CommandOutput editFromSource(ExecutionContext& c, bool insert) {
    const Sequence& seq = c.sequence();
    const MediaItem& media = mediaItem(c);
    const Rate rate = sourceTimecodeRate(c.state(), media);
    std::vector<std::string> warnings;

    // Marks: the media's stored ones (clamped to a shorter relinked file), unless the params give them.
    bool clamped = false;
    edit::SourceMarks marks = edit::storedSourceMarks(media, &clamped);
    const bool inGiven = given(c, "sourceIn");
    const bool outGiven = given(c, "sourceOut");
    if (clamped && !(inGiven && outGiven)) {
        warnings.push_back("the stored marks of '" + media.name + "' lie past the end of the media; they were clamped");
    }
    if (inGiven) {
        marks.in = sourceTimeValue(c.params()["sourceIn"], "/sourceIn", rate);
    }
    if (outGiven) {
        marks.out = sourceTimeValue(c.params()["sourceOut"], "/sourceOut", rate);
    }

    const std::vector<SourcePatchSetting> settings = effectivePatches(c, seq, media);
    std::vector<edit::SourcePatch> patches;
    for (const SourcePatchSetting& p : settings) {
        // A.10: a patch with a track name and no track goes to a new track of that name.
        if (p.enabled && (p.destination || !p.newTrack.empty())) {
            patches.push_back({p.source, p.destination, p.newTrack});
        }
    }
    if (patches.empty()) {
        fail(ErrorCode::InvalidParams, "nothing to edit in: no source track is patched and switched on",
             Json{{"path", "/patches"}});
    }
    const std::optional<Ticks> at = c.optionalTime("at");
    // A drag onto the timeline goes where it is dropped, whatever the sequence marks say.
    const bool sequenceMarks = c.boolean("sequenceMarks", true);
    if (!at && (!sequenceMarks || (!seq.markIn && !seq.markOut))) {
        fail(ErrorCode::InvalidParams, "pass \"at\" (no sequence In or Out mark places the edit)", Json{{"path", "/at"}});
    }

    // A four-point edit whose two ranges differ by a sequence frame or more asks how to fit (Premiere's Fit Clip).
    const bool fourPoint = sequenceMarks && marks.in && marks.out && seq.markIn && seq.markOut;
    const Ticks sourceLength = fourPoint ? *marks.out - *marks.in : Ticks{};
    const Ticks sequenceLength = fourPoint ? *seq.markOut - *seq.markIn : Ticks{};
    const Ticks frame = framesToTicks(1, seq.frameRate);
    const bool mismatch = fourPoint && std::llabs(sourceLength.value - sequenceLength.value) >= frame.value;
    Fit fit = mismatch ? fitOf(c.string("fit")) : Fit::None;
    if (mismatch && fit == Fit::None) {
        fit = Fit::IgnoreSourceOut;
        warnings.push_back("four-point edit: the source and sequence ranges differ, so the source Out was ignored "
                           "(pass \"fit\" to choose)");
    }

    ProjectState placementState = sequenceMarks ? c.state() : c.take(edit::clearMarks(c.state(), seq.id, true, true)).state;
    edit::SourceMarks used = marks;
    switch (fit) {
    case Fit::IgnoreSourceIn:
        used.in.reset();
        break;
    case Fit::IgnoreSourceOut:
        used.out.reset();
        break;
    case Fit::IgnoreSequenceIn:
        placementState = c.take(edit::clearMarks(placementState, seq.id, true, false)).state;
        break;
    case Fit::IgnoreSequenceOut:
        placementState = c.take(edit::clearMarks(placementState, seq.id, false, true)).state;
        break;
    case Fit::Speed:
        // The whole source range at the sequence In; the speed change below makes it fill the range.
        placementState = c.take(edit::clearMarks(placementState, seq.id, true, true)).state;
        break;
    case Fit::None:
        break;
    }
    const edit::ThreePointEdit placement = c.take(edit::resolveThreePointEdit(
        placementState, seq.id, media.id, patches, used, fit == Fit::Speed ? *seq.markIn : at.value_or(Ticks{})));
    const edit::SourceClip source{media.id, placement.sourceRange, patches};

    edit::EditResult r;
    std::optional<Speed> speed;
    if (fit == Fit::Speed) {
        speed = edit::speedForDuration(sourceLength, sequenceLength);
        if (!speed) {
            fail(ErrorCode::EditRejected, "no speed fits the source range into the sequence range");
        }
        edit::SpeedChange change;
        change.speed = *speed;
        if (insert) {
            // Insert at the In, then retime with ripple: later material follows the new length.
            r = c.take(edit::insertEdit(c.state(), seq.id, source, placement.at));
            change.ripple = true;
            edit::EditResult sped = c.take(edit::setSpeed(r.state, seq.id, r.placed, change, edit::LinkMode::IgnoreLinks));
            r.state = std::move(sped.state);
            append(r.created, sped.created);
            append(r.removed, sped.removed);
        } else {
            // Overwrite past the end of everything, retime it there (nothing to collide with), then move it onto
            // the marked range: it replaces exactly [In, Out).
            const Ticks parking = snapToFrame(std::max(seq.contentEnd(), *seq.markOut), seq.frameRate) + frame;
            r = c.take(edit::overwriteEdit(c.state(), seq.id, source, parking));
            change.ripple = false;
            edit::EditResult sped = c.take(edit::setSpeed(r.state, seq.id, r.placed, change, edit::LinkMode::IgnoreLinks));
            edit::EditResult moved = c.take(edit::moveClips(sped.state, seq.id, r.placed, placement.at - parking, {},
                                                            edit::MoveMode::Overwrite, edit::LinkMode::IgnoreLinks));
            // Parking grew the sequence's length; it is back to what the content needs.
            const Ticks length = std::max(seq.duration, moved.state.findSequence(seq.id)->contentEnd());
            r.state = c.take(edit::setSequenceDuration(moved.state, seq.id, length)).state;
            append(r.created, sped.created);
            append(r.created, moved.created);
            append(r.removed, moved.removed);
        }
        // Premiere parks the playhead after the new clips.
        Ticks end = placement.at;
        for (const ClipId id : r.placed) {
            if (const auto placed = r.state.findSequence(seq.id)->findClip(id)) {
                end = std::max(end, placed->clip().end());
            }
        }
        r.playhead = end;
    } else {
        r = c.take(insert ? edit::insertEdit(c.state(), seq.id, source, placement.at)
                          : edit::overwriteEdit(c.state(), seq.id, source, placement.at));
    }
    if (given(c, "targets")) {
        r.state = retargeted(c, std::move(r.state), seq.id, c.tracks("targets"));
    }

    Json placed = Json::array();
    for (const ClipId id : r.placed) {
        placed.push_back(id.value);
    }
    Json data{{"placed", std::move(placed)},
              {"at", timeJson(c, placement.at)},
              {"sourceIn", sourceTimeJson(placement.sourceRange.start, rate)},
              {"sourceOut", sourceTimeJson(placement.sourceRange.end(), rate)},
              {"fourPoint", mismatch},
              {"fit", fit == Fit::None ? Json(nullptr) : Json(fitName(fit))},
              {"patches", patchesJson(c, settings)}};
    if (speed) {
        data["speed"] = Json{{"num", speed->num},
                             {"den", speed->den},
                             {"percent", 100.0 * static_cast<double>(speed->num) / static_cast<double>(speed->den)}};
    }
    if (r.playhead) {
        data["playhead"] = timeJson(c, *r.playhead);
    }
    if (!r.createdTracks.empty()) {
        data["createdTracks"] = createdTracksJson(r.state, seq.id, r.createdTracks);
    }
    CommandOutput out = edited(std::move(r), std::move(data));
    out.warnings = std::move(warnings);
    return out;
}

Json editFromSourceSchema() {
    return withSequence(
        {{"media", mediaParam("The source media.")},
         {"sourceIn", sourceTimeParam("Source In: the first source frame. Default: the media's stored In mark, else its "
                                      "start; null for none.")},
         {"sourceOut", sourceTimeParam("Source Out (exclusive). Default: the media's stored Out mark, else its end; null "
                                       "for none.")},
         {"at", timeParam("Where the edit starts (the playhead). Ignored when the sequence has In/Out marks: Premiere's "
                          "three-point editing uses them.")},
         {"patches", patchesSchema()},
         {"targets", tracksParam("Tracks to target after the edit (Premiere's targeting column). Default: unchanged.")},
         {"sequenceMarks", schema::boolean("false places the edit at \"at\" even when the sequence has In/Out marks (a "
                                           "drag onto the timeline). Default true.",
                                           true)},
         {"fit", schema::enumeration({"speed", "ignoreSourceIn", "ignoreSourceOut", "ignoreSequenceIn", "ignoreSequenceOut"},
                                     "Four-point edit whose source and sequence ranges differ (Premiere's Fit Clip "
                                     "dialog): \"speed\" changes the clip speed to fill the range (fit to fill); the "
                                     "others ignore one mark. Default: ignoreSourceOut, with a warning.")}},
        {"media"});
}

// --- Match frame --------------------------------------------------------------------------------------------

struct Found {
    const Track* track = nullptr;
    const Clip* clip = nullptr;
};

/// The clip under `t` on `tracks`: video tracks from the top down, then audio tracks from A1 (Premiere's Match
/// Frame takes the topmost targeted clip).
Found clipUnder(const Sequence& seq, Ticks t, const std::vector<TrackId>& tracks) {
    const auto look = [&](const TrackPtr& track) -> Found {
        if (std::find(tracks.begin(), tracks.end(), track->id) == tracks.end()) {
            return {};
        }
        if (const auto index = track->clipIndexAt(t)) {
            return {track.get(), &track->clips[*index]};
        }
        return {};
    };
    for (auto it = seq.videoTracks.rbegin(); it != seq.videoTracks.rend(); ++it) {
        if (const Found f = look(*it); f.clip) {
            return f;
        }
    }
    for (const TrackPtr& track : seq.audioTracks) {
        if (const Found f = look(track); f.clip) {
            return f;
        }
    }
    return {};
}

CommandOutput matchFrame(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const Ticks t = c.time("time");
    Found found;
    if (given(c, "tracks")) {
        found = clipUnder(seq, t, c.tracks("tracks"));
    } else {
        // The targeted tracks, else every track.
        found = clipUnder(seq, t, edit::targetedTracks(seq));
        if (!found.clip) {
            found = clipUnder(seq, t, edit::allTracks(seq));
        }
    }
    if (!found.clip) {
        fail(ErrorCode::NotFound, "no clip under " + c.timecode(t));
    }
    const Clip& clip = *found.clip;
    const MediaItem* media = c.state().findMedia(clip.media);
    if (!media) {
        fail(ErrorCode::NotFound, "the clip's media does not exist");
    }
    const Rate rate = sourceTimecodeRate(c.state(), *media);
    // The clip's used range becomes the source marks (a frame hold has only its frame: an In; a remapped clip: the
    // media it shows, as its sourceIn/sourceOut are curve times).
    edit::SourceMarks marks{clip.sourceIn, clip.isFreeze() ? std::nullopt : std::optional<Ticks>(clip.sourceOut())};
    if (clip.isRemapped()) {
        const TimeRange shown = clip.shownSourceRange(clip.start(), clip.end());
        marks = {shown.start, shown.end()};
    }
    CommandOutput out;
    out.state = c.state();
    if (c.boolean("setMarks", true)) {
        out.state = c.take(edit::setSourceMarks(c.state(), media->id, marks)).state;
    }
    out.data = Json{{"clip", clip.id.value},
                    {"track", c.trackLabel(found.track->id)},
                    {"media", media->id.value},
                    {"name", media->name},
                    {"sourceTime", sourceTimeJson(clip.sourceTimeAt(t), rate)},
                    {"marks", marksJson(marks, rate)}};
    return out;
}

CommandOutput reverseMatchFrame(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const MediaItem& media = mediaItem(c);
    const Rate rate = sourceTimecodeRate(c.state(), media);
    const std::optional<Ticks> sourceTime = sourceTimeValue(c.params()["time"], "/time", rate);
    if (!sourceTime) {
        fail(ErrorCode::InvalidParams, "pass the source time", Json{{"path", "/time"}});
    }
    const Ticks from = c.optionalTime("from").value_or(Ticks{});
    const std::vector<TrackId> tracks = given(c, "tracks") ? c.tracks("tracks") : edit::allTracks(seq);
    struct Use {
        Ticks time;
        const Track* track;
        const Clip* clip;
    };
    std::vector<Use> uses;
    seq.forEachTrack([&](const Track& track) {
        if (std::find(tracks.begin(), tracks.end(), track.id) == tracks.end()) {
            return;
        }
        for (const Clip& clip : track.clips) {
            if (clip.media != media.id) {
                continue;
            }
            if (const auto at = clip.timelineTimeOfSourceTime(*sourceTime)) {
                uses.push_back({*at, &track, &clip});
            }
        }
    });
    if (uses.empty()) {
        fail(ErrorCode::NotFound, "that source frame of '" + media.name + "' is not used in the sequence");
    }
    // The first use at or after `from` (the playhead), else the first one; video before audio at the same time.
    const auto earlier = [](const Use& a, const Use& b) {
        return std::pair(a.time, a.track->kind != TrackKind::Video) < std::pair(b.time, b.track->kind != TrackKind::Video);
    };
    std::sort(uses.begin(), uses.end(), earlier);
    const auto after = std::find_if(uses.begin(), uses.end(), [&](const Use& u) { return u.time >= from; });
    const Use& use = after != uses.end() ? *after : uses.front();
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"clip", use.clip->id.value},
                    {"track", c.trackLabel(use.track->id)},
                    {"time", timeJson(c, snapToFrame(use.time, seq.frameRate))},
                    {"uses", uses.size()}};
    return out;
}

} // namespace

void registerSourceCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"source.load", 1, "Load Source",
                "Opens a media item as the source (Premiere's Source Monitor): its kind, duration, frame rate, stored In/Out "
                "marks and the source patching a source edit would use. The app also shows it in its Source Monitor.",
                Capability::Read,
                withSequence({{"media", mediaParam("The media to load.")},
                              {"time", sourceTimeParam("Source position to show. Default: the In mark, else the start.")}},
                             {"media"}),
                Json{{"media", 3}, {"time", "00:00:04:00"}}, {}, loadSource});
    addBuiltin(registry,
               {"source.setMarks", 1, "Set Source Marks",
                "Sets a media item's source In/Out marks (Premiere's Source Monitor I and O). They are saved with the "
                "project and used by edit.insertFromSource / overwriteFromSource. A key left out keeps its mark; null "
                "clears it; an In at or after the Out (or an Out at or before the In) clears the other mark.",
                Capability::EditProject,
                withSequence({{"media", mediaParam("The media.")},
                              {"in", sourceTimeParam("Source In: the first frame to use.")},
                              {"out", sourceTimeParam("Source Out (exclusive): just past the last frame to use.")}},
                             {"media"}),
                Json{{"media", 3}, {"in", "00:00:02:00"}, {"out", "00:00:06:12"}}, {},
                [](ExecutionContext& c) { return setMarks(c, false); }});
    addBuiltin(registry,
               {"source.clearMarks", 1, "Clear Source Marks",
                "Clears a media item's source In and/or Out mark (Premiere's Source Monitor Clear In / Clear Out / Clear "
                "In and Out).",
                Capability::EditProject,
                withSequence({{"media", mediaParam("The media.")},
                              {"in", schema::boolean("Clear the In. Default true.", true)},
                              {"out", schema::boolean("Clear the Out. Default true.", true)}},
                             {"media"}),
                Json{{"media", 3}, {"out", false}}, {}, [](ExecutionContext& c) { return setMarks(c, true); }});
    addBuiltin(registry,
               {"edit.insertFromSource", 1, "Insert from Source",
                "Insert edit from the source (Premiere ','), with three-point editing: the source marks (stored on the "
                "media unless given), the sequence In/Out marks and the playhead decide where it goes and what it uses. "
                "Later material on the patched and sync-locked tracks moves later. A four-point edit whose ranges differ "
                "takes a \"fit\" choice (Premiere's Fit Clip dialog).",
                Capability::EditSequence, editFromSourceSchema(),
                Json{{"media", 3}, {"at", "00:00:10:00"}, {"patches", Json::array({Json{{"source", "V1"}, {"track", "V2"}},
                                                                                    Json{{"source", "A1"}, {"track", "A1"}}})}},
                {}, [](ExecutionContext& c) { return editFromSource(c, true); }});
    addBuiltin(registry,
               {"edit.overwriteFromSource", 1, "Overwrite from Source",
                "Overwrite edit from the source (Premiere '.'), with three-point editing like edit.insertFromSource: "
                "replaces what is on the patched tracks; nothing moves. \"fit\": \"speed\" retimes the clip to fill the "
                "sequence In/Out exactly.",
                Capability::EditSequence, editFromSourceSchema(),
                Json{{"media", 3}, {"sourceIn", "00:00:01:00"}, {"sourceOut", "00:00:05:00"}, {"fit", "speed"}}, {},
                [](ExecutionContext& c) { return editFromSource(c, false); }});
    addBuiltin(registry,
               {"sequence.setTargets", 1, "Set Track Targets",
                "Targets exactly the given tracks (Premiere's track targeting: Lift, Extract, paste, Up/Down navigation "
                "and Match Frame use them).",
                Capability::EditSequence,
                withSequence({{"tracks", schema::array(trackParam("A track."), "Tracks to target; [] untargets all.")}},
                             {"tracks"}),
                Json{{"tracks", Json::array({"V1", "A1", "A2"})}}, {},
                [](ExecutionContext& c) {
                    const SequenceId seq = c.sequenceId();
                    CommandOutput out;
                    out.state = retargeted(c, c.state(), seq, c.tracks("tracks"));
                    out.data = Json{{"targeted", targetsJson(c, out.state, seq)}};
                    return out;
                }});
    addBuiltin(registry,
               {"sequence.setPatches", 1, "Set Source Patching",
                "Checks a source patching (Premiere's source track indicators: which source track goes to which sequence "
                "track, and which are switched off) and returns it in order. Patching is view state of the app's Source "
                "Monitor, which keeps it; elsewhere nothing is stored: pass \"patches\" to the source edits.",
                Capability::Read,
                withSequence({{"media", mediaParam("The source media.")}, {"patches", patchesSchema()}}, {"media", "patches"}),
                Json{{"media", 3}, {"patches", Json::array({Json{{"source", "V1"}, {"track", "V2"}},
                                                            Json{{"source", "A1"}, {"track", "A3"}},
                                                            Json{{"source", "A2"}, {"track", nullptr}}})}},
                {},
                [](ExecutionContext& c) {
                    const MediaItem& media = mediaItem(c);
                    CommandOutput out;
                    out.state = c.state();
                    out.data = Json{{"media", media.id.value},
                                    {"patches", patchesJson(c, patchesParam(c, c.sequence(), media))}};
                    if (!findService<SourceMonitorServices>(c.services())) {
                        out.warnings.push_back("source patching is view state of the app's Source Monitor: nothing was "
                                               "stored; pass \"patches\" to edit.insertFromSource");
                    }
                    return out;
                }});
    addBuiltin(registry,
               {"sequence.matchFrame", 1, "Match Frame",
                "Match Frame (Premiere F): the clip under a time on the targeted tracks (the topmost; every track when none "
                "is targeted there), its media and the source frame shown. Like Premiere, the media's source marks become "
                "the clip's used range (setMarks false leaves them).",
                Capability::EditProject,
                withSequence({{"time", timeParam("The playhead.")},
                              {"tracks", tracksParam("Tracks to look on. Default: the targeted tracks, else all.")},
                              {"setMarks", schema::boolean("Set the source marks to the clip's range. Default true.", true)}},
                             {"time"}),
                Json{{"time", "00:00:12:05"}}, {}, matchFrame});
    addBuiltin(registry,
               {"sequence.reverseMatchFrame", 1, "Reverse Match Frame",
                "Reverse Match Frame (Premiere Shift+R): where a source frame is used in the sequence: the first use at "
                "or after \"from\", else the first one.",
                Capability::Read,
                withSequence({{"media", mediaParam("The source media.")},
                              {"time", sourceTimeParam("The source frame.")},
                              {"from", timeParam("Search from here (the playhead). Default: the start.")},
                              {"tracks", tracksParam("Tracks to look on. Default: all.")}},
                             {"media", "time"}),
                Json{{"media", 3}, {"time", "00:00:07:00"}}, {}, reverseMatchFrame});
}

} // namespace wabcut::command::detail
