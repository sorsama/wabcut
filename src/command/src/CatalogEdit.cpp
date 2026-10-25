// Built-in timeline edit commands (capability EditSequence). Each one is a thin adapter: it resolves its
// references, calls the model's EditOps and returns the new state. Model semantics stay in the model.

#include "Catalog.h"

#include <wabcut/model/Parameters.h>

#include <algorithm>
#include <cassert>

namespace wabcut::command::detail {

// --- Shared pieces ---------------------------------------------------------------------------------------

void addBuiltin(CommandRegistry& registry, CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
    assert(added && "built-in command rejected by the registry");
}

Json sequenceParam() {
    return schema::integer("Sequence id; only needed when the project has several.", 1);
}

Json timeParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Timecode in the sequence's rate (\"00:01:02:03\") or ticks."}};
}

Json deltaParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Signed timecode (\"-00:00:00:05\") or ticks."}};
}

Json trackParam(std::string description) {
    return Json{{"type", Json::array({"string", "integer"})},
                {"description", description + " \"V1\" (bottom video track), \"A2\", ... or a track id."},
                {"minimum", 1}};
}

Json tracksParam(std::string description) { return schema::array(trackParam("A track."), std::move(description)); }

Json clipParam(std::string description) {
    return Json{{"type", Json::array({"integer", "object"})},
                {"description", description + " A clip id, or {\"track\", \"time\"} for the clip there."},
                {"properties", Json{{"track", trackParam("Track of the clip.")},
                                    {"time", timeParam("A time inside the clip.")}}},
                {"required", Json::array({"track", "time"})},
                {"additionalProperties", false},
                {"minimum", 1}};
}

Json clipsParam(std::string description, std::int64_t minItems) {
    return schema::array(clipParam("A clip."), std::move(description), minItems);
}

Json mediaParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " A media id or exact name."},
                {"minimum", 1}};
}

Json linkedParam(bool defaultValue) {
    return schema::boolean(defaultValue ? "Include linked partners (Premiere's linked selection). Default true."
                                        : "Also cut linked partners. Default false.",
                           defaultValue);
}

Json edgeParam() {
    return schema::enumeration({"head", "tail"}, "Which edge: \"head\" (the clip's In point) or \"tail\" (its Out point).");
}

Json withSequence(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required) {
    Json s = schema::object(properties, std::move(required));
    s["properties"]["sequence"] = sequenceParam();
    return s;
}

Json timeJson(ExecutionContext& context, Ticks t) { return Json{{"ticks", t.value}, {"timecode", context.timecode(t)}}; }

CommandOutput edited(edit::EditResult result, Json data) {
    CommandOutput out;
    out.state = std::move(result.state);
    for (const ClipId id : result.created) {
        out.createdIds.push_back(id.value);
    }
    for (const TransitionId id : result.createdTransitions) {
        out.createdIds.push_back(id.value);
    }
    for (const EffectInstanceId id : result.createdEffects) {
        out.createdIds.push_back(id.value);
    }
    out.data = std::move(data);
    return out;
}

edit::EditResult withTargets(ExecutionContext& context, const std::vector<TrackId>& tracks,
                             const std::function<edit::EditOutcome(const ProjectState&)>& edit) {
    const Sequence& seq = context.sequence();
    std::vector<std::pair<TrackId, bool>> original;
    seq.forEachTrack([&](const Track& t) { original.emplace_back(t.id, t.targeted); });
    const auto retarget = [&](ProjectState state, const auto& wanted) {
        for (const auto& [id, was] : original) {
            const bool want = wanted(id, was);
            const Track* now = state.findSequence(seq.id)->findTrack(id);
            if (now && now->targeted != want) {
                edit::TrackStateChange change;
                change.targeted = want;
                state = context.take(edit::setTrackState(state, seq.id, id, change)).state;
            }
        }
        return state;
    };
    ProjectState input = context.state();
    if (!tracks.empty()) {
        input = retarget(std::move(input), [&](TrackId id, bool) {
            return std::find(tracks.begin(), tracks.end(), id) != tracks.end();
        });
    }
    edit::EditResult result = context.take(edit(input));
    if (!tracks.empty()) {
        result.state = retarget(std::move(result.state), [](TrackId, bool was) { return was; });
    }
    return result;
}

namespace {

edit::Edge edgeOf(ExecutionContext& c) { return c.string("edge") == "head" ? edit::Edge::Head : edit::Edge::Tail; }

// --- Source edits ----------------------------------------------------------------------------------------

/// A sequence edited in as a source (C.8): a nested clip, or a multicam clip of a multicam source.
CommandOutput sequenceSourceEdit(ExecutionContext& c, bool insert) {
    const Sequence& seq = c.sequence();
    const Json& ref = c.params()["sourceSequence"];
    const Sequence* source = ref.is_number_integer() && ref.get<std::int64_t>() > 0
                                 ? c.state().findSequence(SequenceId{ref.get<std::uint64_t>()})
                                 : nullptr;
    if (!source) {
        fail(ErrorCode::NotFound, "no sequence with id " + ref.dump(), Json{{"path", "/sourceSequence"}});
    }
    std::vector<edit::SourcePatch> patches;
    const bool videoGiven = c.params().contains("video");
    const bool audioGiven = c.params().contains("audio");
    for (const edit::SourcePatch& p : edit::defaultSourcePatches(seq, *source)) {
        if ((p.source.kind == StreamKind::Video && !videoGiven) || (p.source.kind == StreamKind::Audio && !audioGiven)) {
            patches.push_back(p);
        }
    }
    if (videoGiven && !c.params()["video"].is_null()) {
        if (!sequenceHasStream(*source, {StreamKind::Video, 0})) {
            fail(ErrorCode::InvalidParams, "the sequence has no picture", Json{{"path", "/video"}});
        }
        patches.push_back({{StreamKind::Video, 0}, c.track("video")});
    }
    if (audioGiven) {
        const std::vector<TrackId> audio = c.tracks("audio");
        if (audio.size() > 1 || (!audio.empty() && !sequenceHasStream(*source, {StreamKind::Audio, 0}))) {
            fail(ErrorCode::InvalidParams, "a sequence has one mix: give at most one audio track", Json{{"path", "/audio"}});
        }
        for (const TrackId t : audio) {
            patches.push_back({{StreamKind::Audio, 0}, t});
        }
    }
    if (patches.empty()) {
        fail(ErrorCode::InvalidParams, "nothing to edit in: no video or audio track is patched");
    }
    const std::optional<Ticks> at = c.has("at") ? c.optionalTime("at") : seq.markIn;
    if (!at) {
        fail(ErrorCode::InvalidParams, "pass \"at\" (the sequence has no In mark)", Json{{"path", "/at"}});
    }
    const Ticks in = c.optionalTime("sourceIn").value_or(source->markIn.value_or(Ticks{}));
    const Ticks out = c.optionalTime("sourceOut").value_or(source->markOut.value_or(source->duration));
    if (out <= in) {
        fail(ErrorCode::InvalidParams, "the source range is empty", Json{{"path", "/sourceOut"}});
    }
    edit::SourceClip clip;
    clip.sequence = source->id;
    clip.sourceRange = TimeRange::fromStartEnd(in, out);
    clip.patches = patches;
    if (c.has("angle")) {
        const std::size_t angle = c.indexValue(c.params()["angle"], "/angle");
        clip.videoAngle = AngleSet::of(angle == 0 ? kMaxMulticamAngles : angle - 1);
    }
    if (c.has("audioAngles")) {
        const Json& list = c.params()["audioAngles"];
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::size_t angle = c.indexValue(list[i], "/audioAngles/" + std::to_string(i));
            clip.audioAngles.bits |= AngleSet::of(angle == 0 ? kMaxMulticamAngles : angle - 1).bits;
        }
    }
    edit::EditResult r = c.take(insert ? edit::insertEdit(c.state(), seq.id, clip, *at)
                                       : edit::overwriteEdit(c.state(), seq.id, clip, *at));
    Json placed = Json::array();
    for (const ClipId id : r.placed) {
        placed.push_back(id.value);
    }
    Json data{{"placed", std::move(placed)}, {"at", timeJson(c, *at)}};
    if (r.playhead) {
        data["playhead"] = timeJson(c, *r.playhead);
    }
    return edited(std::move(r), std::move(data));
}

CommandOutput sourceEdit(ExecutionContext& c, bool insert) {
    if (c.has("sourceSequence")) {
        if (c.has("media")) {
            fail(ErrorCode::InvalidParams, "pass \"media\" or \"sourceSequence\", not both", Json{{"path", "/media"}});
        }
        return sequenceSourceEdit(c, insert);
    }
    if (!c.has("media")) {
        fail(ErrorCode::InvalidParams, "pass \"media\" (or \"sourceSequence\")", Json{{"path", "/media"}});
    }
    const Sequence& seq = c.sequence();
    const MediaId mediaId = c.media("media");
    const MediaItem& media = *c.state().findMedia(mediaId);

    std::vector<edit::SourcePatch> patches;
    const bool videoGiven = c.params().contains("video");
    const bool audioGiven = c.params().contains("audio");
    // The default patching; a multi-track recording's audio is routed by name (A.10) and may add tracks.
    for (const edit::SourcePatch& p : edit::defaultSourcePatches(seq, media)) {
        if ((p.source.kind == StreamKind::Video && !videoGiven) || (p.source.kind == StreamKind::Audio && !audioGiven)) {
            patches.push_back(p);
        }
    }
    if (videoGiven && !c.params()["video"].is_null()) {
        if (!media.info.hasVideo()) {
            fail(ErrorCode::InvalidParams, "the media has no video stream", Json{{"path", "/video"}});
        }
        patches.push_back({{StreamKind::Video, 0}, c.track("video")});
    }
    if (audioGiven) {
        const std::vector<TrackId> audio = c.tracks("audio");
        if (audio.size() > media.info.audio.size()) {
            fail(ErrorCode::InvalidParams, "the media has only " + std::to_string(media.info.audio.size()) +
                                               " audio streams",
                 Json{{"path", "/audio"}});
        }
        for (std::size_t i = 0; i < audio.size(); ++i) {
            patches.push_back({{StreamKind::Audio, static_cast<std::int32_t>(i)}, audio[i]});
        }
    }
    if (patches.empty()) {
        fail(ErrorCode::InvalidParams, "nothing to edit in: no video or audio track is patched");
    }

    const std::optional<Ticks> at = c.optionalTime("at");
    if (!at && !seq.markIn && !seq.markOut) {
        fail(ErrorCode::InvalidParams, "pass \"at\" (the sequence has no In or Out mark)", Json{{"path", "/at"}});
    }
    const edit::SourceMarks marks{c.optionalTime("sourceIn"), c.optionalTime("sourceOut")};
    const edit::ThreePointEdit placement =
        c.take(edit::resolveThreePointEdit(c.state(), seq.id, mediaId, patches, marks, at.value_or(Ticks{})));
    const edit::SourceClip source{mediaId, placement.sourceRange, patches};
    edit::EditResult r = c.take(insert ? edit::insertEdit(c.state(), seq.id, source, placement.at)
                                       : edit::overwriteEdit(c.state(), seq.id, source, placement.at));
    Json placed = Json::array();
    for (const ClipId id : r.placed) {
        placed.push_back(id.value);
    }
    Json data{{"placed", std::move(placed)}, {"at", timeJson(c, placement.at)}};
    if (r.playhead) {
        data["playhead"] = timeJson(c, *r.playhead);
    }
    if (!r.createdTracks.empty()) {
        // A.10: the tracks the edit added, labelled as they are after it ("A4").
        Json created = Json::array();
        const Sequence& after = *r.state.findSequence(seq.id);
        for (const TrackId id : r.createdTracks) {
            const auto pos = after.trackPosition(id);
            created.push_back(pos ? "A" + std::to_string(pos->index + 1) : std::to_string(id.value));
        }
        data["createdTracks"] = std::move(created);
    }
    return edited(std::move(r), std::move(data));
}

Json sourceEditSchema() {
    return withSequence({{"media", mediaParam("The media to edit in.")},
                         {"sourceSequence",
                          schema::integer("Or a sequence to edit in as a nested clip (C.8; a multicam clip when it is a "
                                          "multicam source). Its In/Out marks are the default source range.",
                                          1)},
                         {"angle", schema::integer("A multicam source's angle for the picture (1 = the first; default "
                                                   "the first with a picture).",
                                                   1, static_cast<std::int64_t>(kMaxMulticamAngles))},
                         {"audioAngles", schema::array(schema::integer("An angle (1 = the first).", 1,
                                                                       static_cast<std::int64_t>(kMaxMulticamAngles)),
                                                       "A multicam source's angles for the sound (default the first "
                                                       "with sound).",
                                                       1)},
                         {"at", timeParam("Where the edit starts (the playhead). Ignored when the sequence has In/Out "
                                          "marks: Premiere's three-point editing uses them.")},
                         {"sourceIn", timeParam("Source In: the first source frame to use. Default: the media start.")},
                         {"sourceOut", timeParam("Source Out (exclusive). Default: the media end.")},
                         {"video", Json{{"type", Json::array({"string", "integer", "null"})},
                                        {"description", "Track for the media's video (default V1); null for none."}}},
                         {"audio", tracksParam("Tracks for the media's audio streams in order (default A1, A2, ...); "
                                               "[] for none.")}});
}

// --- Ranges, cuts and deletes ----------------------------------------------------------------------------

CommandOutput markedRemoval(ExecutionContext& c, bool ripple) {
    const Sequence& seq = c.sequence();
    const std::optional<Ticks> in = c.optionalTime("in");
    const std::optional<Ticks> out = c.optionalTime("out");
    const std::vector<TrackId> tracks = c.tracks("tracks");
    edit::EditResult r = withTargets(c, tracks, [&](const ProjectState& s) -> edit::EditOutcome {
        ProjectState state = s;
        if (in) {
            state = c.take(edit::markIn(state, seq.id, *in)).state;
        }
        if (out) {
            state = c.take(edit::markOut(state, seq.id, *out - Ticks{1})).state;
        }
        return ripple ? edit::extract(state, seq.id) : edit::lift(state, seq.id);
    });
    Json data = r.playhead ? Json{{"playhead", timeJson(c, *r.playhead)}} : Json(nullptr);
    return edited(std::move(r), std::move(data));
}

Json markedRemovalSchema() {
    return withSequence({{"in", timeParam("Start of the range. Default: the sequence In mark.")},
                         {"out", timeParam("End of the range (exclusive). Default: the sequence Out mark.")},
                         {"tracks", tracksParam("Tracks to act on. Default: the targeted tracks.")}});
}

CommandOutput rippleDelete(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const bool hasClips = c.has("clips");
    const bool hasGap = c.has("gap");
    if (hasClips == hasGap) {
        fail(ErrorCode::InvalidParams, "pass either \"clips\" or \"gap\"");
    }
    if (hasGap) {
        const Json& gap = c.params()["gap"];
        const TrackId track = c.trackValue(gap["track"], "/gap/track");
        const Ticks at = c.timeValue(gap["time"], "/gap/time");
        return edited(c.take(edit::rippleDeleteGap(c.state(), seq.id, track, at)));
    }
    return edited(c.take(edit::rippleDeleteClips(c.state(), seq.id, c.clips("clips"), c.linkMode())));
}

// --- Remove silences -------------------------------------------------------------------------------------

CommandOutput removeSilences(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const Ticks frame = framesToTicks(1, seq.frameRate);
    // Only whole frames inside each range go: round the start up and the end down to frame boundaries.
    std::vector<TimeRange> ranges;
    const Json& list = c.params()["ranges"];
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string path = "/ranges/" + std::to_string(i);
        const Ticks start = c.timeValue(list[i]["start"], path + "/start");
        const Ticks end = c.timeValue(list[i]["end"], path + "/end");
        if (end <= start) {
            fail(ErrorCode::InvalidParams, "a range must end after it starts", Json{{"path", path}});
        }
        Ticks first = snapToFrame(start, seq.frameRate);
        if (first < start) {
            first += frame;
        }
        const Ticks last = snapToFrame(end, seq.frameRate);
        if (last > first) {
            ranges.push_back(TimeRange::fromStartEnd(first, last));
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& r : ranges) {
        if (!merged.empty() && r.start <= merged.back().end()) {
            merged.back() = TimeRange::fromStartEnd(merged.back().start, std::max(merged.back().end(), r.end()));
        } else {
            merged.push_back(r);
        }
    }
    std::vector<TrackId> tracks = c.tracks("tracks");
    if (merged.empty()) {
        CommandOutput out;
        out.state = c.state();
        out.data = Json{{"rangesRemoved", 0}, {"removed", timeJson(c, Ticks{})}};
        out.warnings.push_back("no whole frame lies inside the given ranges; nothing was removed");
        return out;
    }
    if (tracks.empty()) {
        tracks = edit::allTracks(seq); // keep every track in sync by default
    }
    const std::optional<Ticks> markIn = seq.markIn;
    const std::optional<Ticks> markOut = seq.markOut;
    std::vector<std::uint64_t> created;
    Ticks removedTotal{};
    edit::EditResult r = withTargets(c, tracks, [&](const ProjectState& s) -> edit::EditOutcome {
        edit::EditResult step;
        step.state = s;
        // Last range first, so earlier ranges keep their positions.
        std::size_t done = 0;
        for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
            c.checkpoint("range " + std::to_string(++done) + " of " + std::to_string(merged.size()));
            ProjectState state = c.take(edit::markIn(step.state, seq.id, it->start)).state;
            state = c.take(edit::markOut(state, seq.id, it->end() - Ticks{1})).state;
            step = c.take(edit::extract(state, seq.id));
            for (const ClipId id : step.created) {
                created.push_back(id.value);
            }
            removedTotal += it->duration;
        }
        // Leave the user's In/Out marks as they were.
        edit::EditResult restored = c.take(edit::clearMarks(step.state, seq.id));
        if (markIn) {
            restored = c.take(edit::markIn(restored.state, seq.id, *markIn));
        }
        if (markOut) {
            restored = c.take(edit::markOut(restored.state, seq.id, *markOut - Ticks{1}));
        }
        return restored;
    });
    CommandOutput out = edited(std::move(r), Json{{"rangesRemoved", merged.size()}, {"removed", timeJson(c, removedTotal)}});
    out.createdIds = std::move(created);
    return out;
}

// --- Clip parameters ---------------------------------------------------------------------------------------

std::vector<std::string> parameterKeys() {
    std::vector<std::string> keys;
    for (const ParameterInfo& p : allParameters()) {
        keys.emplace_back(p.key);
    }
    return keys;
}

std::string parameterHelp() {
    std::string help = "The parameter: ";
    bool first = true;
    for (const ParameterInfo& p : allParameters()) {
        help += (first ? "" : ", ") + std::string(p.key) + " [" + Json(p.minValue).dump() + ".." +
                Json(p.maxValue).dump() + ", default " + Json(p.defaultValue).dump() + "]";
        first = false;
    }
    return help + ". Position/anchor in pixels from the centre, scale as a factor (1 = 100%), rotation in degrees, "
                  "opacity 0..1, volume in dB, pan -1 (left)..1 (right).";
}

CommandOutput setParam(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const auto id = parameterByKey(c.string("param"));
    const ParameterInfo& info = parameterInfo(*id);
    const double value = c.number("value");
    if (value < info.minValue || value > info.maxValue) {
        fail(ErrorCode::InvalidParams, std::string(info.key) + " must be between " + Json(info.minValue).dump() +
                                           " and " + Json(info.maxValue).dump(),
             Json{{"path", "/value"}});
    }
    ProjectState state = c.state();
    for (const ClipId clip : c.clips("clips")) {
        ClipProperties props = state.findSequence(seq.id)->findClip(clip)->clip().properties();
        props.set(*id, value);
        edit::ClipAttributeChange change;
        change.properties = props;
        state = c.take(edit::setClipAttributes(state, seq.id, std::vector<ClipId>{clip}, change)).state;
    }
    CommandOutput out;
    out.state = std::move(state);
    return out;
}

CommandOutput setAttributes(ExecutionContext& c, const edit::ClipAttributeChange& change) {
    return edited(c.take(edit::setClipAttributes(c.state(), c.sequenceId(), c.clips("clips"), change)));
}

} // namespace

void registerEditCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"clip.insert", 1, "Insert",
                "Insert edit (Premiere ','): puts a media source range onto the patched tracks at a time and pushes "
                "later material on those tracks and every sync-locked track later, cutting through clips there. "
                "Video and audio clips are linked.",
                Capability::EditSequence, sourceEditSchema(),
                Json{{"media", 3}, {"at", "00:00:10:00"}, {"sourceIn", "00:00:02:00"}, {"sourceOut", "00:00:05:00"}},
                {"sequence.insert"},
                [](ExecutionContext& c) { return sourceEdit(c, true); }});
    addBuiltin(registry,
               {"clip.overwrite", 1, "Overwrite",
                "Overwrite edit (Premiere '.'): puts a media source range onto the patched tracks at a time, replacing "
                "whatever is there. Nothing moves.",
                Capability::EditSequence, sourceEditSchema(),
                Json{{"media", 3}, {"at", "00:00:10:00"}, {"sourceOut", "00:00:03:00"}, {"audio", Json::array()}},
                {"sequence.overwrite"},
                [](ExecutionContext& c) { return sourceEdit(c, false); }});
    addBuiltin(registry,
               {"sequence.lift", 1, "Lift",
                "Lift (Premiere ';'): removes a time range from the targeted tracks and leaves a gap.",
                Capability::EditSequence, markedRemovalSchema(), Json{{"in", "00:00:04:00"}, {"out", "00:00:06:00"}},
                {},
                [](ExecutionContext& c) { return markedRemoval(c, false); }});
    addBuiltin(registry,
               {"sequence.extract", 1, "Extract",
                "Extract (Premiere \"'\"): removes a time range from the targeted tracks and closes the gap; sync-locked "
                "tracks lose the same range so they stay in sync.",
                Capability::EditSequence, markedRemovalSchema(),
                Json{{"in", "00:00:04:00"}, {"out", "00:00:06:00"}, {"tracks", Json::array({"V1", "A1"})}}, {},
                [](ExecutionContext& c) { return markedRemoval(c, true); }});
    addBuiltin(registry,
               {"sequence.addEdit", 1, "Add Edit",
                "Add edit / razor (Premiere Ctrl+K): cuts the clips under a time on the given tracks (default: the "
                "targeted tracks).",
                Capability::EditSequence,
                withSequence({{"time", timeParam("Where to cut.")},
                              {"tracks", tracksParam("Tracks to cut. Default: the targeted tracks.")},
                              {"linked", linkedParam(false)}},
                             {"time"}),
                Json{{"time", "00:00:12:00"}, {"tracks", Json::array({"V1"})}, {"linked", true}}, {"timeline.razor"},
                [](ExecutionContext& c) {
                    const Sequence& seq = c.sequence();
                    std::vector<TrackId> tracks = c.tracks("tracks");
                    if (tracks.empty()) {
                        tracks = edit::targetedTracks(seq);
                    }
                    if (tracks.empty()) {
                        fail(ErrorCode::EditRejected, "no track is targeted: pass \"tracks\"");
                    }
                    return edited(c.take(edit::addEdit(c.state(), seq.id, c.time("time"), tracks,
                                                       c.boolean("linked", false) ? edit::LinkMode::FollowLinks
                                                                                  : edit::LinkMode::IgnoreLinks)));
                }});
    addBuiltin(registry,
               {"sequence.addEditToAllTracks", 1, "Add Edit to All Tracks",
                "Add edit to all tracks (Premiere Ctrl+Shift+K): cuts every unlocked track at a time.",
                Capability::EditSequence, withSequence({{"time", timeParam("Where to cut.")}}, {"time"}),
                Json{{"time", "00:00:12:00"}}, {"timeline.razorAll"},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::addEditToAllTracks(c.state(), c.sequenceId(), c.time("time"))));
                }});
    addBuiltin(registry,
               {"edit.clear", 1, "Delete",
                "Delete (Premiere Delete key): removes clips and leaves gaps.", Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to delete.")}, {"linked", linkedParam(true)}}, {"clips"}),
                Json{{"clips", Json::array({42, Json{{"track", "V2"}, {"time", "00:00:03:00"}}})}}, {"clip.delete"},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::deleteClips(c.state(), c.sequenceId(), c.clips("clips"), c.linkMode())));
                }});
    addBuiltin(
        registry,
        {"timeline.rippleDelete", 1, "Ripple Delete",
         "Ripple delete: removes clips and closes the time they used, or closes a gap. Sync-locked tracks lose the same "
         "time. A gap only closes when no sync-locked track has material in it.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("Clips to remove (use this or gap).")},
                       {"gap", schema::object({{"track", trackParam("Track of the gap.")},
                                               {"time", timeParam("A time inside the gap.")}},
                                              {"track", "time"}, "A gap to close (use this or clips).")},
                       {"linked", linkedParam(true)}}),
         Json{{"clips", Json::array({42})}}, {"edit.rippleDelete"}, rippleDelete});
    addBuiltin(registry,
               {"sequence.rippleTrimPreviousToPlayhead", 1, "Ripple Trim Previous Edit to Playhead",
                "Q: removes the time from the previous edit point to the playhead on the targeted tracks and closes it.",
                Capability::EditSequence, withSequence({{"time", timeParam("The playhead.")}}, {"time"}),
                Json{{"time", "00:00:07:12"}}, {},
                [](ExecutionContext& c) {
                    edit::EditResult r = c.take(edit::rippleTrimPreviousEditToPlayhead(c.state(), c.sequenceId(), c.time("time")));
                    Json data = r.playhead ? Json{{"playhead", timeJson(c, *r.playhead)}} : Json(nullptr);
                    return edited(std::move(r), std::move(data));
                }});
    addBuiltin(registry,
               {"sequence.rippleTrimNextToPlayhead", 1, "Ripple Trim Next Edit to Playhead",
                "W: removes the time from the playhead to the next edit point on the targeted tracks and closes it.",
                Capability::EditSequence, withSequence({{"time", timeParam("The playhead.")}}, {"time"}),
                Json{{"time", "00:00:07:12"}}, {},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::rippleTrimNextEditToPlayhead(c.state(), c.sequenceId(), c.time("time"))));
                }});

    const auto trimSchema = [] {
        return withSequence({{"clip", clipParam("The clip whose edge moves.")},
                             {"edge", edgeParam()},
                             {"delta", deltaParam("How far the edge moves; positive = later.")},
                             {"linked", linkedParam(true)}},
                            {"clip", "edge", "delta"});
    };
    const auto deltaData = [](ExecutionContext& c, edit::EditResult r) {
        Json data{{"appliedDelta", timeJson(c, r.appliedDelta)}};
        return edited(std::move(r), std::move(data));
    };
    addBuiltin(registry,
               {"clip.trim", 1, "Trim",
                "Trim an edge (selection tool): moves a clip's In or Out point without moving anything else. Clamped to "
                "neighbours, source media and one frame; the delta actually applied is returned.",
                Capability::EditSequence, trimSchema(), Json{{"clip", 42}, {"edge", "tail"}, {"delta", "-00:00:00:10"}}, {},
                [deltaData](ExecutionContext& c) {
                    return deltaData(c, c.take(edit::trimEdge(c.state(), c.sequenceId(), c.clip("clip"), edgeOf(c),
                                                              c.time("delta"), c.linkMode())));
                }});
    addBuiltin(registry,
               {"clip.rippleTrim", 1, "Ripple Edit",
                "Ripple edit (B): moves a clip's In or Out point and ripples later material (sync-locked tracks too).",
                Capability::EditSequence, trimSchema(), Json{{"clip", 42}, {"edge", "head"}, {"delta", "00:00:00:12"}},
                {},
                [deltaData](ExecutionContext& c) {
                    return deltaData(c, c.take(edit::rippleTrim(c.state(), c.sequenceId(), c.clip("clip"), edgeOf(c),
                                                                c.time("delta"), c.linkMode())));
                }});
    addBuiltin(registry,
               {"clip.roll", 1, "Rolling Edit",
                "Rolling edit (N): moves the edit point at a clip's edge, trimming the neighbour the other way.",
                Capability::EditSequence, trimSchema(), Json{{"clip", 42}, {"edge", "tail"}, {"delta", "00:00:00:06"}}, {},
                [deltaData](ExecutionContext& c) {
                    return deltaData(c, c.take(edit::rollEdit(c.state(), c.sequenceId(), c.clip("clip"), edgeOf(c),
                                                              c.time("delta"), c.linkMode())));
                }});
    addBuiltin(registry,
               {"clip.slip", 1, "Slip",
                "Slip (Y): shifts which source frames clips show, keeping their place and length.",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to slip.")},
                              {"delta", deltaParam("Source shift; positive = later source frames.")},
                              {"linked", linkedParam(true)}},
                             {"clips", "delta"}),
                Json{{"clips", Json::array({42})}, {"delta", "00:00:01:00"}}, {},
                [deltaData](ExecutionContext& c) {
                    return deltaData(c, c.take(edit::slip(c.state(), c.sequenceId(), c.clips("clips"), c.time("delta"),
                                                          c.linkMode())));
                }});
    addBuiltin(registry,
               {"clip.slide", 1, "Slide",
                "Slide (U): moves a clip between its neighbours, trimming them to compensate.", Capability::EditSequence,
                withSequence({{"clip", clipParam("The clip to slide.")},
                              {"delta", deltaParam("How far it moves; positive = later.")},
                              {"linked", linkedParam(true)}},
                             {"clip", "delta"}),
                Json{{"clip", 42}, {"delta", "-00:00:00:08"}}, {},
                [deltaData](ExecutionContext& c) {
                    return deltaData(c, c.take(edit::slide(c.state(), c.sequenceId(), c.clip("clip"), c.time("delta"),
                                                           c.linkMode())));
                }});
    addBuiltin(
        registry,
        {"clip.move", 1, "Move",
         "Moves clips in time and across tracks. \"overwrite\" (plain drag) replaces what is at the destination; "
         "\"insert\" (Ctrl-drag) makes room there; \"rearrange\" (Ctrl+Alt-drag) closes the clips' old place and makes "
         "room at the drop, on the dragged clips' and the destination tracks only (a clip dropped at the cut after its "
         "neighbour swaps with it). With copy (Alt-drag) the clips are copied there instead and the originals stay "
         "(overwrite or insert; not rearrange). Clips cannot start before 0.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("Clips to move.")},
                       {"delta", deltaParam("Time shift; positive = later. Default 0.")},
                       {"videoTracks", schema::integer("Video track shift; +1 = one track up (V1 -> V2). Default 0.")},
                       {"audioTracks", schema::integer("Audio track shift; +1 = one track down (A1 -> A2). Default 0.")},
                       {"mode", schema::enumeration({"overwrite", "insert", "rearrange"}, "Default overwrite.")},
                       {"copy", schema::boolean("Alt-drag: copy the clips instead of moving them; createdIds lists the "
                                                "copies. Default false.",
                                                false)},
                       {"linked", linkedParam(true)}},
                      {"clips"}),
         Json{{"clips", Json::array({42})}, {"delta", "00:00:02:00"}, {"videoTracks", 1}}, {},
         [deltaData](ExecutionContext& c) {
             const Ticks delta = c.optionalTime("delta").value_or(Ticks{});
             const edit::TrackShift shift{static_cast<int>(c.integer("videoTracks")), static_cast<int>(c.integer("audioTracks"))};
             const std::string modeName = c.string("mode", "overwrite");
             const edit::MoveMode mode = modeName == "insert"      ? edit::MoveMode::Insert
                                         : modeName == "rearrange" ? edit::MoveMode::Rearrange
                                                                   : edit::MoveMode::Overwrite;
             if (c.boolean("copy", false)) {
                 // duplicateClips refuses a rearrange copy (InvalidArgument).
                 return deltaData(c, c.take(edit::duplicateClips(c.state(), c.sequenceId(), c.clips("clips"), delta,
                                                                 shift, mode, c.linkMode())));
             }
             return deltaData(c, c.take(edit::moveClips(c.state(), c.sequenceId(), c.clips("clips"), delta, shift, mode,
                                                        c.linkMode())));
         }});
    addBuiltin(registry,
               {"clip.setParam", 1, "Set Clip Parameter",
                "Sets one effect parameter (motion, opacity, volume, pan) on clips.", Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to change.")},
                              {"param", schema::enumeration(parameterKeys(), parameterHelp())},
                              {"value", schema::number("The new value, in the parameter's unit.")}},
                             {"clips", "param", "value"}),
                Json{{"clips", Json::array({42})}, {"param", "volume.level"}, {"value", -6.0}}, {}, setParam});
    addBuiltin(registry,
               {"clip.enable", 1, "Enable",
                "Enables or disables clips (Premiere Shift+E). Disabled clips stay in place but are not shown or heard.",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to change.")},
                              {"enabled", schema::boolean("true to enable, false to disable.", true)}},
                             {"clips", "enabled"}),
                Json{{"clips", Json::array({42})}, {"enabled", false}}, {},
                [](ExecutionContext& c) {
                    edit::ClipAttributeChange change;
                    change.enabled = c.boolean("enabled", true);
                    return setAttributes(c, change);
                }});
    addBuiltin(registry,
               {"clip.rename", 1, "Rename Clip", "Renames clips (the name shown on the timeline; the media keeps its name).",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to rename.")}, {"name", schema::string("The new name.")}},
                             {"clips", "name"}),
                Json{{"clips", Json::array({42})}, {"name", "Interview, take 2"}}, {},
                [](ExecutionContext& c) {
                    edit::ClipAttributeChange change;
                    change.name = c.string("name");
                    return setAttributes(c, change);
                }});
    addBuiltin(registry,
               {"clip.setColor", 1, "Set Label Color", "Sets the label colour of clips (0-15).", Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to change.")},
                              {"colorIndex", schema::integer("Label colour index.", 0, 15)}},
                             {"clips", "colorIndex"}),
                Json{{"clips", Json::array({42})}, {"colorIndex", 3}}, {"clip.label"},
                [](ExecutionContext& c) {
                    edit::ClipAttributeChange change;
                    change.colorIndex = static_cast<std::int32_t>(c.integer("colorIndex"));
                    return setAttributes(c, change);
                }});
    addBuiltin(registry,
               {"clip.link", 1, "Link", "Links clips so they select and edit together.", Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to link (at least two).", 2)}}, {"clips"}),
                Json{{"clips", Json::array({42, 43})}}, {},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::linkClips(c.state(), c.sequenceId(), c.clips("clips"))));
                }});
    addBuiltin(registry,
               {"clip.unlink", 1, "Unlink", "Takes clips out of their link groups.", Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to unlink.")}}, {"clips"}), Json{{"clips", Json::array({42})}},
                {},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::unlinkClips(c.state(), c.sequenceId(), c.clips("clips"))));
                }});
    addBuiltin(registry,
               {"marker.add", 1, "Add Marker",
                "Adds a sequence marker. Premiere's marker colours are 0-7: green, red, purple, orange, yellow, white, "
                "blue, cyan.",
                Capability::EditSequence,
                withSequence({{"time", timeParam("Where the marker starts.")},
                              {"duration", timeParam("Length for a range marker. Default 0 (a point marker).")},
                              {"name", schema::string("Marker name.")},
                              {"comment", schema::string("Marker comment.")},
                              {"colorIndex", schema::integer("Marker colour index.", 0, 15)},
                              {"type", markerTypeParam()},
                              {"url", markerUrlParam()},
                              {"target", markerTargetParam()},
                              {"highlight", markerHighlightParam()}},
                             {"time"}),
                Json{{"time", "00:01:00:00"}, {"name", "Chapter 2"}, {"type", "chapter"}}, {"markers.add"},
                [](ExecutionContext& c) {
                    Marker marker{c.time("time"), c.optionalTime("duration").value_or(Ticks{}), c.string("name"),
                                  c.string("comment"), static_cast<std::int32_t>(c.integer("colorIndex"))};
                    applyMarkerLinkParams(c, marker);
                    requireValidMarker(marker);
                    return edited(c.take(edit::addMarker(c.state(), c.sequenceId(), marker)));
                }});
    addBuiltin(registry,
               {"marker.remove", 1, "Remove Marker",
                "Removes the sequence marker at an index (as listed by sequence.summary).", Capability::EditSequence,
                withSequence({{"index", schema::integer("Marker index.", 0)}}, {"index"}), Json{{"index", 0}},
                {"markers.remove"},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::removeMarker(c.state(), c.sequenceId(),
                                                            static_cast<std::size_t>(c.integer("index")))));
                }});
    addBuiltin(registry,
               {"marker.markIn", 1, "Mark In", "Sets the sequence In mark at the start of the frame at a time.",
                Capability::EditSequence, withSequence({{"time", timeParam("The time to mark.")}}, {"time"}),
                Json{{"time", "00:00:04:00"}}, {},
                [](ExecutionContext& c) { return edited(c.take(edit::markIn(c.state(), c.sequenceId(), c.time("time")))); }});
    addBuiltin(registry,
               {"marker.markOut", 1, "Mark Out",
                "Sets the sequence Out mark at the end of the frame at a time (the frame is included).",
                Capability::EditSequence, withSequence({{"time", timeParam("The last frame to include.")}}, {"time"}),
                Json{{"time", "00:00:09:29"}}, {},
                [](ExecutionContext& c) { return edited(c.take(edit::markOut(c.state(), c.sequenceId(), c.time("time")))); }});
    addBuiltin(registry,
               {"marker.clearInAndOut", 1, "Clear In and Out", "Clears the sequence In and Out marks.",
                Capability::EditSequence, withSequence({}), Json::object(), {},
                [](ExecutionContext& c) { return edited(c.take(edit::clearMarks(c.state(), c.sequenceId()))); }});
    addBuiltin(
        registry,
        {"timeline.removeSilences", 1, "Remove Silences",
         "Ripple-deletes a list of time ranges (e.g. silences found in a transcript) in one step. Only whole frames "
         "inside each range are removed. By default every track loses the same time so everything stays in sync; "
         "In/Out marks and track targeting are left as they were.",
         Capability::EditSequence,
         withSequence({{"ranges", schema::array(schema::object({{"start", timeParam("Range start.")},
                                                                {"end", timeParam("Range end (exclusive).")}},
                                                               {"start", "end"}),
                                                "Ranges to remove, in sequence time, any order.", 1)},
                       {"tracks", tracksParam("Tracks to remove the ranges from. Default: all tracks.")}},
                      {"ranges"}),
         Json{{"ranges", Json::array({Json{{"start", "00:00:01:10"}, {"end", "00:00:02:20"}},
                                      Json{{"start", "00:00:07:00"}, {"end", "00:00:07:15"}}})}},
         {}, removeSilences});
    addBuiltin(
        registry,
        {"track.setState", 1, "Set Track State",
         "Changes track header toggles: lock, target, sync lock, mute, solo, output, name. Unlocking is how a locked "
         "track becomes editable again.",
         Capability::EditSequence,
         withSequence({{"track", trackParam("The track.")},
                       {"name", schema::string("New name (\"\" for the default).")},
                       {"locked", schema::boolean("Track lock.")},
                       {"targeted", schema::boolean("Track targeting.")},
                       {"syncLocked", schema::boolean("Sync lock.")},
                       {"muted", schema::boolean("Mute (audio).")},
                       {"solo", schema::boolean("Solo (audio).")},
                       {"enabled", schema::boolean("Track output on.")}},
                      {"track"}),
         Json{{"track", "A2"}, {"locked", true}}, {},
         [](ExecutionContext& c) {
             edit::TrackStateChange change;
             if (c.has("name")) {
                 change.name = c.string("name");
             }
             const auto flag = [&](const char* key, std::optional<bool>& field) {
                 if (c.has(key)) {
                     field = c.boolean(key, false);
                 }
             };
             flag("locked", change.locked);
             flag("targeted", change.targeted);
             flag("syncLocked", change.syncLocked);
             flag("muted", change.muted);
             flag("solo", change.solo);
             flag("enabled", change.enabled);
             return edited(c.take(edit::setTrackState(c.state(), c.sequenceId(), c.track("track"), change)));
         }});
}

} // namespace wabcut::command::detail
