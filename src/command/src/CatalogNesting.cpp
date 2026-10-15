// Built-in nesting and multicam commands (C.8, model: Nesting.h): Nest, Unnest (Resolve's Decompose), opening a nested
// clip's sequence, multicam sources (create, angles, sync), angle switches and Flatten.

#include "Catalog.h"

#include <wabcut/model/Nesting.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <stop_token>

namespace wabcut::command::detail {

namespace {

/// A time of sequence `s` in its own rate.
Json timeIn(const Sequence& s, Ticks t) {
    return Json{{"ticks", t.value}, {"timecode", ticksToTimecode(t, s.frameRate, s.dropFrame)}};
}

/// "V2", "A1": a track of `s` (not the context's sequence).
std::string labelOf(const Sequence& s, TrackId track) {
    const auto pos = s.trackPosition(track);
    return pos ? (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1) : std::string("?");
}

Json angleParam(std::string description) {
    return schema::integer(std::move(description) + " An angle number: 1 is the first (Premiere's 1-9 keys).", 1,
                           static_cast<std::int64_t>(kMaxMulticamAngles));
}

/// A 1-based angle number from the params, as an index.
std::size_t angleIndex(ExecutionContext& c, const Json& value, const std::string& path) {
    const std::size_t n = c.indexValue(value, path);
    if (n < 1 || n > kMaxMulticamAngles) {
        fail(ErrorCode::InvalidParams, "angles are numbered from 1 to " + std::to_string(kMaxMulticamAngles),
             Json{{"path", path}});
    }
    return n - 1;
}

const Sequence& sequenceParamValue(ExecutionContext& c, const char* key) {
    const Json& v = c.params()[key];
    const Sequence* s = v.is_number_integer() && v.get<std::int64_t>() > 0
                            ? c.state().findSequence(SequenceId{v.get<std::uint64_t>()})
                            : nullptr;
    if (!s) {
        fail(ErrorCode::NotFound, "no sequence with id " + v.dump(), Json{{"path", std::string("/") + key}});
    }
    return *s;
}

/// The multicam source named by "multicam", else the context's sequence (which must be one).
const Sequence& multicamSource(ExecutionContext& c) {
    const Sequence& s = c.has("multicam") ? sequenceParamValue(c, "multicam") : c.sequence();
    if (!s.multicam) {
        fail(ErrorCode::EditRejected, "sequence '" + s.name + "' is not a multicam source");
    }
    return s;
}

Json anglesJson(const Sequence& s) {
    Json angles = Json::array();
    for (std::size_t i = 0; i < s.multicam->angles.size(); ++i) {
        const MulticamAngle& a = s.multicam->angles[i];
        Json audio = Json::array();
        for (const TrackId t : a.audio) {
            audio.push_back(labelOf(s, t));
        }
        angles.push_back(Json{{"angle", i + 1},
                              {"name", a.name},
                              {"video", a.video.isValid() ? Json(labelOf(s, a.video)) : Json(nullptr)},
                              {"audio", std::move(audio)},
                              {"offset", timeIn(s, a.offset)}});
    }
    return angles;
}

Json anglesOf(const Clip& clip) {
    Json out = Json::array();
    for (std::size_t i = 0; i < kMaxMulticamAngles; ++i) {
        if (clip.angles.contains(i)) {
            out.push_back(i + 1);
        }
    }
    return out;
}

std::vector<std::string> syncNames() {
    return {"timecode", "inPoints", "outPoints", "markers", "audio", "manual"};
}

Json idsJson(const std::vector<ClipId>& ids) {
    Json out = Json::array();
    for (const ClipId id : ids) {
        out.push_back(id.value);
    }
    return out;
}

CommandOutput readOutput(ExecutionContext& c, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    return out;
}

} // namespace

void registerNestingCommands(CommandRegistry& registry) {
    // --- Nesting -------------------------------------------------------------------------------------------
    Json nameSchema = schema::string("Name of the new sequence. Default: \"Nested Sequence 01\", 02, ...");
    nameSchema["maxLength"] = 256;
    addBuiltin(
        registry,
        {"sequence.nest", 1, "Nest",
         "Premiere's Nest: the clips (and, with \"linked\", their partners) move into a new sequence in the same bin, "
         "keeping their tracks, effects, links and the transitions between them, and one nested clip per kind (picture "
         "on the lowest selected video track, the mix on the lowest audio track, linked) takes their place. Returns "
         "the new sequence's id and the nested clips.",
         Capability::EditProject,
         withSequence({{"clips", clipsParam("The clips to nest.")}, {"name", nameSchema}, {"linked", linkedParam(true)}},
                      {"clips"}),
         Json{{"clips", Json::array({42, 43})}, {"name", "Interview"}}, {},
         [](ExecutionContext& c) {
             const std::vector<ClipId> clips = c.clips("clips");
             edit::NestResult r = c.take(edit::nestClips(c.state(), c.sequenceId(), clips, c.string("name"), c.linkMode()));
             const Sequence& nested = *r.edit.state.findSequence(r.nested);
             Json data{{"nested", r.nested.value}, {"name", nested.name}, {"clips", idsJson(r.clips)}};
             return edited(std::move(r.edit), std::move(data));
         }});
    addBuiltin(
        registry,
        {"sequence.unnest", 1, "Unnest",
         "Resolve's Decompose: a nested clip (with its linked nested partners) is replaced by copies of the clips it "
         "shows - trimmed to what it shows, on its track and the ones above (audio: below), with their effects, links "
         "and transitions. Only when that looks and sounds exactly the same; otherwise it is refused with the reason "
         "(retimed, effects on the nested clip, track effects, a missing track, a clip in the way, ...).",
         Capability::EditSequence, withSequence({{"clip", clipParam("The nested clip.")}}, {"clip"}),
         Json{{"clip", 57}}, {},
         [](ExecutionContext& c) {
             edit::EditResult r = c.take(edit::unnestClip(c.state(), c.sequenceId(), c.clip("clip")));
             Json data{{"clips", idsJson(r.created)}};
             return edited(std::move(r), std::move(data));
         }});
    addBuiltin(
        registry,
        {"clip.openNested", 1, "Open Nested Sequence",
         "The sequence a nested or multicam clip plays (open it to edit inside), the range of it the clip shows and, "
         "for a multicam clip, its active angles.",
         Capability::Read, withSequence({{"clip", clipParam("The nested or multicam clip.")}}, {"clip"}),
         Json{{"clip", 57}}, {},
         [](ExecutionContext& c) {
             const Clip& clip = c.sequence().findClip(c.clip("clip"))->clip();
             if (!clip.isNested()) {
                 fail(ErrorCode::EditRejected, "clip '" + clip.name() + "' is not a nested or multicam clip");
             }
             const Sequence& nested = *c.state().findSequence(clip.sequence);
             const TimeRange shown = clip.shownSourceRange(clip.start(), clip.end());
             Json data{{"sequence", nested.id.value},
                       {"name", nested.name},
                       {"multicam", nested.multicam.has_value()},
                       {"from", timeIn(nested, shown.start)},
                       {"to", timeIn(nested, shown.end())}};
             if (nested.multicam) {
                 data["angles"] = anglesOf(clip);
             }
             return readOutput(c, std::move(data));
         }});

    // --- Multicam ------------------------------------------------------------------------------------------
    addBuiltin(
        registry,
        {"multicam.create", 1, "Create Multi-Camera Source Sequence",
         "Premiere's Create Multi-Camera Source Sequence: a new sequence with one angle per media item (its video on "
         "a video track, each audio stream on an audio track) synced by \"sync\": their start timecodes, Source "
         "Monitor In or Out marks, first clip markers, audio waveforms (needs the host's audio analysis) or "
         "\"offsets\" given by hand. Cut it into a sequence with sequence.insert / overwrite \"sourceSequence\".",
         Capability::EditProject,
         withSequence({{"media", schema::array(mediaParam("An angle's media."), "The angles, in order.", 1)},
                       {"sync", schema::enumeration(syncNames(), "How the angles line up (default timecode).")},
                       {"name", schema::string("Name of the new sequence. Default: \"<first angle> Multicam\".")},
                       {"bin", schema::integer("Bin for the new sequence. Default: the first angle's bin.", 1)},
                       {"offsets", schema::array(timeParam("Where the angle starts."), "manual: one per angle.")},
                       {"angleNames", schema::array(schema::string("A name."), "Angle names. Default: the media names.")}},
                      {"media"}),
         Json{{"media", Json::array({3, 4, 5})}, {"sync", "audio"}, {"name", "Podcast"}}, {},
         [](ExecutionContext& c) {
             if (c.has("media") && c.params()["media"].size() > kMaxMulticamAngles) {
                 fail(ErrorCode::InvalidParams, "at most " + std::to_string(kMaxMulticamAngles) + " angles",
                      Json{{"path", "/media"}});
             }
             edit::MulticamSpec spec;
             spec.angles = c.mediaList("media");
             spec.sync = multicamSyncFromString(c.string("sync", "timecode")).value_or(MulticamSync::Timecode);
             spec.name = c.string("name");
             spec.bin = BinId{static_cast<std::uint64_t>(c.integer("bin", 0))};
             if (c.has("offsets")) {
                 const Json& list = c.params()["offsets"];
                 for (std::size_t i = 0; i < list.size(); ++i) {
                     spec.offsets.push_back(c.timeValue(list[i], "/offsets/" + std::to_string(i)));
                 }
             }
             if (c.has("angleNames")) {
                 for (const Json& n : c.params()["angleNames"]) {
                     spec.angleNames.push_back(n.get<std::string>());
                 }
             }
             if (const auto* services = findService<MulticamServices>(c.services())) {
                 spec.audioSync = services->audioSync();
             }
             if (spec.sync == MulticamSync::Audio && !spec.audioSync) {
                 fail(ErrorCode::InvalidRequest, "syncing by audio needs the host's audio analysis, which is not "
                                                 "available here: sync by timecode, marks, markers or offsets");
             }
             // The analysis reports through the request's checkpoints, and a stop asked for there stops it.
             std::stop_source stop;
             std::exception_ptr stopped;
             int lastPercent = -1;
             spec.audioSyncControl.stop = stop.get_token();
             spec.audioSyncControl.progress = [&](double fraction) {
                 const int percent = static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
                 if (percent == lastPercent || stopped) {
                     return;
                 }
                 lastPercent = percent;
                 try {
                     c.checkpoint("syncing the angles by audio (" + std::to_string(percent) + "%)");
                 } catch (...) {
                     stopped = std::current_exception();
                     stop.request_stop();
                 }
             };
             auto made = edit::createMulticam(c.state(), spec);
             if (stopped) {
                 std::rethrow_exception(stopped);
             }
             edit::Created<SequenceId> created = c.take(std::move(made));
             const Sequence& source = *created.state.findSequence(created.id);
             CommandOutput out;
             out.data = Json{{"multicam", created.id.value},
                             {"name", source.name},
                             {"sync", std::string(toString(source.multicam->sync))},
                             {"angles", anglesJson(source)}};
             out.createdIds = {created.id.value};
             out.state = std::move(created.state);
             return out;
         }});
    addBuiltin(registry,
               {"multicam.angles", 1, "Multicam Angles",
                "A multicam source's angles: their names, tracks and sync offsets, and how they were synced.",
                Capability::Read,
                withSequence({{"multicam", schema::integer("The multicam source. Default: the \"sequence\".", 1)}}),
                Json{{"multicam", 12}}, {},
                [](ExecutionContext& c) {
                    const Sequence& source = multicamSource(c);
                    return readOutput(c, Json{{"multicam", source.id.value},
                                              {"name", source.name},
                                              {"sync", std::string(toString(source.multicam->sync))},
                                              {"duration", timeIn(source, source.duration)},
                                              {"angles", anglesJson(source)}});
                }});
    addBuiltin(
        registry,
        {"multicam.switchAngle", 1, "Switch Multicam Angle",
         "Premiere's angle switch (the 1-9 keys while playing): the multicam clip is cut at \"at\" and plays "
         "\"angle\" from there to its next cut; without \"at\" the whole clip switches. \"switches\" records a list "
         "of [time, angle] switches (a live multicam edit) as one undo step. \"target\": the picture, the sound or "
         "both (default).",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("A multicam clip of the track to switch.")},
                       {"angle", angleParam("The angle to switch to.")},
                       {"at", timeParam("Where the switch happens (default: the whole clip).")},
                       {"target", schema::enumeration({"video", "audio", "both"}, "What switches (default both).")},
                       {"switches", schema::array(Json{{"type", "object"},
                                                       {"properties", Json{{"at", timeParam("Switch time.")},
                                                                           {"angle", angleParam("The angle.")}}},
                                                       {"required", Json::array({"at", "angle"})},
                                                       {"additionalProperties", false}},
                                                  "Several switches in time order instead of one.", 1)}},
                      {"clip"}),
         Json{{"clip", 57}, {"switches", Json::array({Json{{"at", "00:00:04:00"}, {"angle", 2}},
                                                      Json{{"at", "00:00:09:12"}, {"angle", 1}}})}},
         {},
         [](ExecutionContext& c) {
             const std::string targetName = c.string("target", "both");
             const edit::AngleTarget target = targetName == "video"   ? edit::AngleTarget::Video
                                              : targetName == "audio" ? edit::AngleTarget::Audio
                                                                      : edit::AngleTarget::Both;
             std::vector<std::pair<std::optional<Ticks>, std::size_t>> switches;
             if (c.has("switches")) {
                 const Json& list = c.params()["switches"];
                 for (std::size_t i = 0; i < list.size(); ++i) {
                     const std::string path = "/switches/" + std::to_string(i);
                     switches.emplace_back(c.timeValue(list[i]["at"], path + "/at"),
                                           angleIndex(c, list[i]["angle"], path + "/angle"));
                 }
                 std::stable_sort(switches.begin(), switches.end(),
                                  [](const auto& a, const auto& b) { return *a.first < *b.first; });
             } else {
                 if (!c.has("angle")) {
                     fail(ErrorCode::InvalidParams, "pass \"angle\" or \"switches\"", Json{{"path", "/angle"}});
                 }
                 switches.emplace_back(c.optionalTime("at"), angleIndex(c, c.params()["angle"], "/angle"));
             }
             const SequenceId seqId = c.sequenceId();
             const ClipId named = c.clip("clip");
             const auto loc = c.sequence().findClip(named);
             const TrackId track = loc->track->id;
             const SequenceId source = loc->clip().sequence;
             ProjectState state = c.state();
             std::vector<ClipId> created;
             for (const auto& [at, angle] : switches) {
                 // Each switch acts on the clip of the same source under its time on the named clip's track.
                 ClipId switching = named;
                 if (at) {
                     switching = ClipId{};
                     for (const Clip& clip : state.findSequence(seqId)->findTrack(track)->clips) {
                         if (clip.sequence == source && clip.timelineRange.contains(*at)) {
                             switching = clip.id;
                         }
                     }
                     if (!switching.isValid()) {
                         fail(ErrorCode::EditRejected, "no multicam clip of that source at " + c.timecode(*at));
                     }
                 }
                 edit::EditResult r = c.take(edit::switchMulticamAngle(state, seqId, switching, at, angle, target));
                 created.insert(created.end(), r.created.begin(), r.created.end());
                 state = std::move(r.state);
             }
             CommandOutput out;
             out.state = std::move(state);
             for (const ClipId id : created) {
                 out.createdIds.push_back(id.value);
             }
             out.data = Json{{"switches", switches.size()}, {"clips", idsJson(created)}};
             return out;
         }});
    addBuiltin(
        registry,
        {"multicam.flatten", 1, "Flatten",
         "Premiere's Flatten: a multicam clip (with its linked multicam partners) is replaced by the clips of the "
         "angles it plays, trimmed to what it shows (audio angles' tracks on its audio track and the ones below). "
         "Refused when that would not look and sound the same (effects on it, retimed, a clip in the way).",
         Capability::EditSequence, withSequence({{"clip", clipParam("The multicam clip.")}}, {"clip"}),
         Json{{"clip", 57}}, {},
         [](ExecutionContext& c) {
             edit::EditResult r = c.take(edit::flattenMulticam(c.state(), c.sequenceId(), c.clip("clip")));
             Json data{{"clips", idsJson(r.created)}};
             return edited(std::move(r), std::move(data));
         }});
    addBuiltin(
        registry,
        {"multicam.setSync", 1, "Set Angle Sync",
         "Moves one angle of a multicam source in time (its sync offset: where its material starts); every multicam "
         "clip then shows that angle shifted. The source's sync method becomes manual.",
         Capability::EditSequence,
         withSequence({{"multicam", schema::integer("The multicam source. Default: the \"sequence\".", 1)},
                       {"angle", angleParam("The angle.")},
                       {"offset", timeParam("Its new offset (0 to 24 hours).")}},
                      {"angle", "offset"}),
         Json{{"multicam", 12}, {"angle", 2}, {"offset", "00:00:01:05"}}, {},
         [](ExecutionContext& c) {
             const Sequence& source = multicamSource(c);
             const std::size_t angle = angleIndex(c, c.params()["angle"], "/angle");
             edit::EditResult r = c.take(edit::setMulticamSync(c.state(), source.id, angle, c.time("offset")));
             return edited(std::move(r), Json{{"multicam", source.id.value}});
         }});
}

} // namespace wabcut::command::detail
