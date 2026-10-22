// P.2 (E5): Premiere editing and sequence commands - Scale to Frame Size / Set to Frame Size / Set to Fill Frame and the
// default media scaling (FrameScaling.h).

#include "Catalog.h"

#include <wabcut/command/EditingServices.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FieldOptionsOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/RawSourceSettings.h>
#include <wabcut/model/SequencePresets.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/Timecode.h>

#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

/// 0.2 RAW Source Settings as the commands report them.
Json rawSettingsJson(const MediaItem& item) {
    const RawSourceSettings s = rawSourceSettingsOf(item.locator.path);
    Json out{{"media", item.id.value},
             {"raw", isRawMedia(item.info)},
             {"exposure", s.exposure},
             {"whiteBalance", s.white ? "custom" : "asShot"}};
    if (s.white) {
        out["temperature"] = s.white->temperature;
        out["tint"] = s.white->tint;
    }
    return out;
}

Json rateValue(Rate r) {
    return Json{{"num", r.num}, {"den", r.den}, {"fps", std::round(r.toDouble() * 1000.0) / 1000.0}};
}

/// A frame rate param: {"num", "den"} or frames per second (23.976, 29.97, 59.94 ... are the NTSC x/1001 rates).
Rate frameRateValue(const Json& value, const std::string& path) {
    if (value.is_object()) {
        const Rate r{value.value("num", 0), value.value("den", 0)};
        if (!r.isValid()) {
            fail(ErrorCode::InvalidParams, "a rate needs a positive \"num\" and \"den\"", Json{{"path", path}});
        }
        return r.normalized();
    }
    const double fps = value.get<double>();
    for (const Rate ntsc : {Rate{24000, 1001}, Rate{30000, 1001}, Rate{48000, 1001}, Rate{60000, 1001},
                            Rate{120000, 1001}, Rate{240000, 1001}}) {
        if (std::abs(fps - ntsc.toDouble()) < 0.006) {
            return ntsc;
        }
    }
    // Other rates to a thousandth of a frame (12.5 = 25/2).
    if (fps > 0.0 && fps <= 1000.0 && std::abs(fps * 1000.0 - std::round(fps * 1000.0)) < 1e-6) {
        return Rate{static_cast<std::int32_t>(std::lround(fps * 1000.0)), 1000}.normalized();
    }
    fail(ErrorCode::InvalidParams, "a frame rate is up to 1000 fps, to a thousandth, or an NTSC rate (23.976, 29.97, 59.94 ...)",
         Json{{"path", path}});
}

Json rateParam(std::string description) {
    return Json{{"type", Json::array({"number", "object"})},
                {"description", std::move(description) + " Frames per second (23.976, 25, 29.97, 59.94, 60 ...) or "
                                                         "{\"num\", \"den\"}."}};
}

template <class E>
std::vector<std::string> names(std::initializer_list<E> values) {
    std::vector<std::string> out;
    for (const E v : values) {
        out.emplace_back(toString(v));
    }
    return out;
}

/// What sequence.setSettings and sequence.settings report about a sequence.
Json settingsJson(const Sequence& s) {
    return Json{{"sequence", s.id.value},
                {"name", s.name},
                {"frameRate", rateValue(s.frameRate)},
                {"width", s.width},
                {"height", s.height},
                {"pixelAspect", Json{{"num", s.pixelAspect.num}, {"den", s.pixelAspect.den}, {"ratio", s.pixelAspect.toDouble()}}},
                {"fields", std::string(toString(s.fields))},
                {"audioRate", s.audioRate.num / std::max(1, s.audioRate.den)},
                {"dropFrame", s.dropFrame},
                {"previewFormat", s.previewFormat},
                {"videoDisplay", std::string(toString(s.videoDisplay))},
                {"audioDisplay", std::string(toString(s.audioDisplay))},
                {"startTimecode", ticksToTimecode(s.startTimecode, s.frameRate, s.dropFrame)},
                {"startTimecodeTicks", s.startTimecode.value},
                {"duration", ticksToTimecode(s.duration, s.frameRate, s.dropFrame)},
                {"label", s.colorIndex}};
}

CommandOutput created(ProjectState state, std::uint64_t id, Json data) {
    CommandOutput out;
    out.state = std::move(state);
    out.createdIds.push_back(id);
    out.data = std::move(data);
    return out;
}

CommandOutput projectChange(ProjectState state, Json data) {
    CommandOutput out;
    out.state = std::move(state);
    out.data = std::move(data);
    return out;
}

/// "clips" when given (may be empty), else none.
std::vector<ClipId> optionalClips(ExecutionContext& c) { return c.has("clips") ? c.clips("clips") : std::vector<ClipId>{}; }

/// The start of an item's usable media: its Source Monitor In, else a subclip's start, else 0.
Ticks itemIn(const MediaItem& item) {
    const edit::SourceMarks marks = edit::storedSourceMarks(item);
    return marks.in.value_or(item.subclip ? item.subclip->range.start : Ticks{});
}

void registerReplaceCommands(CommandRegistry& registry);
void registerSequenceToolCommands(CommandRegistry& registry);
void registerFootageCommands(CommandRegistry& registry);

// --- Replace Edit, Replace Footage, Make Offline (EditingOps.h) -----------------------------------------------------

void registerReplaceCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"clip.replace", 1, "Replace With Clip",
         "Premiere's Replace Edit (Sequence > Replace With Clip; Shift-drag from the Source Monitor, Alt-drag from the "
         "Project panel onto a clip): the clips (with linked partners) play \"media\" instead - same place, length, "
         "speed and direction, effects and attributes kept; the clip takes the item's name. From Source Monitor / From "
         "Bin: the first clip starts with the item's In mark (or \"sourceIn\"); \"matchFrame\" (From Source Monitor, "
         "Match Frame) lines the Source Monitor's frame up with the sequence playhead instead. Linked partners keep "
         "their sync. Refused when the item lacks the media the clip needs.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The clips to replace; the first one sets where the source starts.")},
                       {"media", mediaParam("The item to put in their place.")},
                       {"sourceIn", sourceTimeParam("The source time the first clip starts with. Default: the item's In "
                                                    "mark, else its start.")},
                       {"matchFrame", schema::object({{"sourceTime", sourceTimeParam("The frame the Source Monitor shows.")},
                                                      {"at", timeParam("The sequence time it lines up with (the playhead).")}},
                                                     {"sourceTime", "at"},
                                                     "From Source Monitor, Match Frame: the source frame shown at the "
                                                     "sequence playhead.")},
                       {"linked", linkedParam(true)}},
                      {"clips", "media"}),
         Json{{"clips", Json::array({42})}, {"media", 7}}, {},
         [](ExecutionContext& c) {
             const MediaId media = c.media("media");
             const MediaItem& item = *c.state().findMedia(media);
             const Rate rate = sourceTimecodeRate(c.state(), item);
             const std::vector<ClipId> clips = c.clips("clips");
             Ticks in = itemIn(item);
             if (c.has("matchFrame")) {
                 const Json& m = c.params()["matchFrame"];
                 const auto shown = sourceTimeValue(m["sourceTime"], "/matchFrame/sourceTime", rate);
                 if (!shown) {
                     fail(ErrorCode::InvalidParams, "give the Source Monitor's frame", Json{{"path", "/matchFrame/sourceTime"}});
                 }
                 const Ticks at = c.timeValue(m["at"], "/matchFrame/at");
                 const auto loc = c.sequence().findClip(clips.front());
                 if (!loc) {
                     fail(ErrorCode::NotFound, "clip " + std::to_string(clips.front().value) + " is not in the sequence");
                 }
                 const auto matched = edit::replaceMatchFrameIn(loc->clip(), *shown, at);
                 if (!matched) {
                     fail(ErrorCode::EditRejected, "a time-remapped clip cannot be replaced");
                 }
                 in = *matched;
             } else if (c.has("sourceIn")) {
                 if (const auto given = sourceTimeValue(c.params()["sourceIn"], "/sourceIn", rate)) {
                     in = *given;
                 }
             }
             edit::EditResult r = c.take(
                 edit::replaceClips(c.state(), c.sequenceId(), clips, edit::ReplaceSource{media, in}, c.linkMode()));
             std::size_t replaced = 0;
             for (const SequenceChange& s : r.changes.sequences) {
                 replaced += s.modifiedClips.size();
             }
             Json data{{"media", media.value}, {"name", item.name}, {"clips", replaced}};
             return edited(std::move(r), std::move(data));
         }});
    addBuiltin(
        registry,
        touchesFiles(
            {"media.replaceFootage", 1, "Replace Footage",
             "Premiere's Replace Footage (Project panel): the item plays another file from now on - every clip of it "
             "keeps its place, source times, effects and markers, and its marks, markers and settings stay; the old "
             "file's proxy is dropped. \"rename\" (default true) gives the item the new file's name. Refused when a clip "
             "would play past the new file's end or the file lacks a stream a clip plays. Needs a host that reads media "
             "files (the app, wabcut-cli).",
             Capability::EditProject,
             withSequence({{"media", mediaParam("The item.")},
                           {"path", schema::string("The new file (an absolute path).")},
                           {"rename", schema::boolean("Rename the item to the new file's name.", true)}},
                          {"media", "path"}),
             Json{{"media", 3}, {"path", "D:/footage/A001_v2.mov"}}, {},
             [](ExecutionContext& c) {
                 const MediaId media = c.media("media");
                 const auto* probe = findService<MediaProbeServices>(c.services());
                 if (!probe) {
                     fail(ErrorCode::InvalidRequest, "replacing footage needs a host that reads media files (the app, "
                                                     "wabcut-cli)");
                 }
                 auto described = probe->describe(pathFromUtf8(c.string("path")));
                 if (!described) {
                     fail(described.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::InvalidParams,
                          "cannot read the new file: " + described.error().message, Json{{"path", "/path"}});
                 }
                 ProjectState next = c.take(edit::replaceFootage(c.state(), media, *described, c.boolean("rename", true)));
                 const MediaItem& item = *next.findMedia(media);
                 Json data{{"media", media.value}, {"name", item.name}, {"path", item.locator.path}};
                 return projectChange(std::move(next), std::move(data));
             }},
            {"path"}));
    addBuiltin(
        registry,
        {"media.makeOffline", 1, "Make Offline",
         "Premiere's Make Offline (Project panel; the media files remain on disk): the items go offline - their clips "
         "show Media Offline - until they are linked again (File > Relink). Titles and other generated items have no "
         "file and are refused.",
         Capability::EditProject,
         withSequence({{"media", schema::array(mediaParam("A media item."), "The items.", 1)}}, {"media"}),
         Json{{"media", Json::array({3})}}, {},
         [](ExecutionContext& c) {
             const std::vector<MediaId> media = c.mediaList("media");
             return projectChange(c.take(edit::makeOffline(c.state(), media)), Json{{"media", media.size()}});
         }});
}

// --- Make Subsequence, through edits, gaps, Frame Hold Options (EditingOps.h) -------------------------------------

void registerSequenceToolCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"sequence.makeSubsequence", 1, "Make Subsequence",
         "Premiere's Sequence > Make Subsequence (Shift+U): a new sequence with copies of the clips (and their linked "
         "partners) on the same tracks, the earliest at 0, with the transitions between them and the markers and "
         "caption cues over their span; with no clips, everything on the targeted tracks between In and Out (Make "
         "Subsequence from In to Out). The sequence is unchanged. Named \"<sequence> Sub_01\" unless \"name\" is given. "
         "createdIds: the subsequence.",
         Capability::EditProject,
         withSequence({{"clips", clipsParam("The clips. Empty or absent: In to Out on the targeted tracks.", 0)},
                       {"name", schema::string("The subsequence's name.")}}),
         Json{{"clips", Json::array({42, 43})}}, {},
         [](ExecutionContext& c) {
             auto made = c.take(edit::makeSubsequence(c.state(), c.sequenceId(), optionalClips(c), c.string("name")));
             const Sequence& sub = *made.state.findSequence(made.id);
             std::size_t clips = 0;
             sub.forEachTrack([&](const Track& t) { clips += t.clips.size(); });
             const std::string name = sub.name;
             const Ticks duration = sub.duration;
             const Rate rate = sub.frameRate;
             const bool drop = sub.dropFrame;
             return created(std::move(made.state), made.id.value,
                            Json{{"sequence", made.id.value}, {"name", name}, {"clips", clips},
                                 {"duration", ticksToTimecode(duration, rate, drop)}});
         }});
    addBuiltin(
        registry,
        {"sequence.joinThroughEdits", 1, "Join Through Edits",
         "Premiere's Join Through Edit (right-click a through edit) and Sequence > Join Through Edits: through edits - "
         "cuts between two pieces of the same clip that continue each other (same source, continuous source time, the "
         "same speed, effects and attributes, no transition at the cut) - at the edges of the clips (and their linked "
         "partners) go, each pair becoming one clip; with no clips, every through edit on the unlocked tracks. Refused "
         "when there is none there.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("Clips whose through edits to join. Empty or absent: all.", 0)},
                       {"linked", linkedParam(true)}}),
         Json{{"clips", Json::array({42})}}, {},
         [](ExecutionContext& c) {
             edit::EditResult r = c.take(edit::joinThroughEdits(c.state(), c.sequenceId(), optionalClips(c), c.linkMode()));
             Json data{{"joined", r.removed.size()}};
             return edited(std::move(r), std::move(data));
         }});
    addBuiltin(
        registry,
        {"sequence.goToGap", 1, "Go to Gap",
         "Premiere's Sequence > Go to Gap: where the next (previous) gap after (before) \"time\" starts - in the "
         "sequence (a time no track has a clip in, before the last clip) or in a track (\"scope\": \"track\": a gap "
         "on any of \"tracks\", default the targeted tracks). data.time is null when there is none; hosts move the "
         "playhead there.",
         Capability::Read,
         withSequence({{"time", timeParam("Where to look from (the playhead).")},
                       {"direction", schema::enumeration({"next", "previous"}, "Next (default) or previous.")},
                       {"scope", schema::enumeration({"sequence", "track"}, "In the sequence (default) or in tracks.")},
                       {"tracks", tracksParam("Track scope: the tracks. Default: the targeted tracks.")}},
                      {"time"}),
         Json{{"time", "00:00:10:00"}, {"direction", "next"}, {"scope", "sequence"}}, {},
         [](ExecutionContext& c) {
             const Sequence& seq = c.sequence();
             const Ticks t = c.time("time");
             std::vector<TrackId> tracks;
             if (c.string("scope", "sequence") == "track") {
                 tracks = c.has("tracks") ? c.tracks("tracks") : edit::targetedTracks(seq);
                 if (tracks.empty()) {
                     fail(ErrorCode::EditRejected, "target a track to go to its gaps");
                 }
             }
             const auto gap = c.string("direction", "next") == "previous" ? edit::previousGap(seq, t, tracks)
                                                                            : edit::nextGap(seq, t, tracks);
             CommandOutput out;
             out.state = c.state();
             out.data = Json{{"time", gap ? timeJson(c, *gap) : Json(nullptr)}};
             return out;
         }});
    addBuiltin(
        registry,
        {"sequence.closeGap", 1, "Close Gap",
         "Premiere's Sequence > Close Gap: removes the empty time between the clips (with their linked partners) on "
         "their tracks - with no clips, every gap of the unlocked tracks from the sequence's start - only where none of "
         "those tracks has material, so linked clips stay in sync; other unlocked sync-locked tracks lose the same time, "
         "and a gap where one of them has material stays. Caption cues follow. One undo step.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The clips between which to close gaps. Empty or absent: all gaps.", 0)}}),
         Json::object(), {},
         [](ExecutionContext& c) {
             return edited(c.take(edit::closeGaps(c.state(), c.sequenceId(), optionalClips(c))));
         }});
    addBuiltin(
        registry,
        {"clip.frameHoldOptions", 1, "Frame Hold Options",
         "Premiere's Clip > Video Options > Frame Hold Options: the whole video clip holds one frame - at a source "
         "timecode, a sequence timecode, its In point, its Out point or the playhead (\"time\": a media time for "
         "sourceTimecode, a sequence time for sequenceTimecode and playhead) - keeping its place and length. "
         "\"holdFilters\" holds its keyframed effects at that frame too (off: they keep animating). \"enabled\": false "
         "plays the clip normally again from the frame it held. Add Frame Hold / Insert Frame Hold Segment are "
         "clip.frameHold.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The video clip.")},
                       {"enabled", schema::boolean("Hold (default) or play normally again.", true)},
                       {"holdOn", schema::enumeration({"sourceTimecode", "sequenceTimecode", "inPoint", "outPoint", "playhead"},
                                                      "Which frame to hold (default inPoint).")},
                       {"time", Json{{"type", Json::array({"integer", "string"})},
                                     {"description", "The time for sourceTimecode (a media timecode) or sequenceTimecode / "
                                                     "playhead (a sequence timecode); ticks or a timecode."}}},
                       {"holdFilters", schema::boolean("Hold the clip's effects at the held frame too.", false)}},
                      {"clip"}),
         Json{{"clip", 42}, {"holdOn", "playhead"}, {"time", "00:00:04:12"}, {"holdFilters", true}}, {},
         [](ExecutionContext& c) {
             const ClipId clip = c.clip("clip");
             edit::FrameHoldOptions options;
             options.enabled = c.boolean("enabled", true);
             options.holdOn = edit::holdOnFromString(c.string("holdOn", "inPoint")).value_or(edit::HoldOn::InPoint);
             options.holdFilters = c.boolean("holdFilters", false);
             const bool needsTime = options.holdOn == edit::HoldOn::SourceTimecode ||
                                    options.holdOn == edit::HoldOn::SequenceTimecode || options.holdOn == edit::HoldOn::Playhead;
             if (options.enabled && needsTime) {
                 if (!c.has("time")) {
                     fail(ErrorCode::InvalidParams, "give the \"time\" to hold", Json{{"path", "/time"}});
                 }
                 if (options.holdOn == edit::HoldOn::SourceTimecode) {
                     const auto loc = c.sequence().findClip(clip);
                     const MediaItem* item = loc ? c.state().findMedia(loc->clip().media) : nullptr;
                     if (!item) {
                         fail(ErrorCode::EditRejected, "the clip plays no media to hold");
                     }
                     const auto t = sourceTimeValue(c.params()["time"], "/time", sourceTimecodeRate(c.state(), *item));
                     options.time = t.value_or(Ticks{});
                 } else {
                     options.time = c.time("time");
                 }
             }
             edit::EditResult r = c.take(edit::setFrameHoldOptions(c.state(), c.sequenceId(), clip, options));
             const auto loc = r.state.findSequence(c.sequenceId())->findClip(clip);
             Json data{{"held", loc && loc->clip().isFreeze()}};
             if (loc && loc->clip().isFreeze()) {
                 data["holdFilters"] = loc->clip().content().holdFilters;
             }
             return edited(std::move(r), std::move(data));
         }});
}

// --- Merge Clips, Interpret Footage, Modify > Timecode (EditingOps.h) -----------------------------------------------

void registerFootageCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"media.mergeClips", 1, "Merge Clips",
         "Premiere's Clip > Merge Clips: a merged clip of a video clip and separately recorded audio, synced by their "
         "In points, Out points, timecode, first clip markers or audio waveforms (\"audio\" needs the host's audio "
         "analysis). It is a synced source (a merged multicam source: the camera as angle 1, each recorder as another); "
         "edited into a sequence it shows the camera and plays every recording (and the camera's sound unless "
         "\"removeCameraAudio\"), and trims, ripples and undoes like any clip; multicam.flatten turns a clip of it into "
         "the items' own linked clips. createdIds: the merged clip.",
         Capability::EditProject,
         withSequence({{"video", mediaParam("The camera clip (an item with video).")},
                       {"audio", schema::array(mediaParam("A sound recording."), "The separately recorded audio.", 1)},
                       {"sync", schema::enumeration({"inPoints", "outPoints", "timecode", "markers", "audio"},
                                                    "Synchronize point (default inPoints).")},
                       {"removeCameraAudio", schema::boolean("Leave the camera's own sound out.", false)},
                       {"name", schema::string("The merged clip's name. Default: \"<video> - Merged\".")},
                       {"bin", schema::integer("Bin for it (bin id). Default: the video's.", 1)}},
                      {"audio"}),
         Json{{"video", 3}, {"audio", Json::array({4})}, {"sync", "timecode"}, {"removeCameraAudio", true}}, {},
         [](ExecutionContext& c) {
             edit::MergeSpec spec;
             spec.video = c.has("video") ? c.media("video") : MediaId{};
             spec.audio = c.mediaList("audio");
             const std::string sync = c.string("sync", "inPoints");
             spec.sync = sync == "outPoints"  ? MulticamSync::OutPoints
                         : sync == "timecode" ? MulticamSync::Timecode
                         : sync == "markers"  ? MulticamSync::Markers
                         : sync == "audio"    ? MulticamSync::Audio
                                              : MulticamSync::InPoints;
             spec.removeCameraAudio = c.boolean("removeCameraAudio", false);
             spec.name = c.string("name");
             spec.bin = BinId{static_cast<std::uint64_t>(c.integer("bin", 0))};
             if (const auto* services = findService<MulticamServices>(c.services())) {
                 spec.audioSync = services->audioSync();
             }
             if (spec.sync == MulticamSync::Audio && !spec.audioSync) {
                 fail(ErrorCode::InvalidRequest, "syncing by audio needs the host's audio analysis, which is not "
                                                 "available here: sync by In or Out points, timecode or markers");
             }
             auto made = c.take(edit::mergeClips(c.state(), spec));
             const Sequence& merged = *made.state.findSequence(made.id);
             Json data{{"merged", made.id.value}, {"name", merged.name}, {"angles", merged.multicam ? merged.multicam->angles.size() : 0}};
             return created(std::move(made.state), made.id.value, std::move(data));
         }});
    addBuiltin(
        registry,
        {"media.interpretFootage", 1, "Interpret Footage",
         "Premiere's Clip > Modify > Interpret Footage on items: \"frameRate\" assumes that rate for the file's frames "
         "(a 120, 144 or 240 fps game capture as 30 or 60 fps plays every frame in slow motion: its clips, new and "
         "placed, run at that speed through the speed engine and its frame interpolation; placed clips keep their source "
         "In and grow into the gap after them), \"pixelAspect\" conforms the pixel aspect ratio, \"fieldOrder\" the "
         "fields, and \"ignoreAlpha\" / \"invertAlpha\" / \"premultiplication\" the alpha channel. A field that is "
         "absent keeps its interpretation; null goes back to what the file says.",
         Capability::EditProject,
         withSequence({{"media", schema::array(mediaParam("A media item."), "The items.", 1)},
                       {"frameRate", Json{{"type", Json::array({"number", "object", "null"})},
                                          {"description", "Assume this frame rate (fps, or {\"num\", \"den\"}); null: the "
                                                          "file's."}}},
                       {"pixelAspect", Json{{"type", Json::array({"number", "object", "null"})},
                                            {"description", "Conform to this pixel aspect ratio; null: the file's."}}},
                       {"fieldOrder", Json{{"type", Json::array({"string", "null"})},
                                           {"enum", Json::array({"progressive", "upperFirst", "lowerFirst", nullptr})},
                                           {"description", "Conform the field order; null: the file's."}}},
                       {"ignoreAlpha", schema::boolean("Ignore the alpha channel (opaque).")},
                       {"invertAlpha", schema::boolean("Invert the alpha channel.")},
                       {"premultiplication", Json{{"type", Json::array({"string", "null"})},
                                                  {"enum", Json::array({"straight", "premultiplied", nullptr})},
                                                  {"description", "Conform the alpha premultiplication; null: the file's."}}}},
                      {"media"}),
         Json{{"media", Json::array({3})}, {"frameRate", 30}}, {},
         [](ExecutionContext& c) {
             const std::vector<MediaId> media = c.mediaList("media");
             const MediaItem& first = *c.state().findMedia(media.front());
             MediaInterpretation in = first.interpretation.value_or(MediaInterpretation{});
             const Json& p = c.params();
             const auto ratio = [&](const char* key) -> std::optional<Rate> {
                 const Json& v = p[key];
                 if (v.is_null()) {
                     return std::nullopt;
                 }
                 if (v.is_object() || std::string_view(key) == "frameRate") {
                     return frameRateValue(v, std::string("/") + key);
                 }
                 const double x = v.get<double>();
                 if (!(x >= 0.1 && x <= 10.0)) {
                     fail(ErrorCode::InvalidParams, "the pixel aspect ratio must be 0.1 to 10", Json{{"path", "/pixelAspect"}});
                 }
                 return Rate{static_cast<std::int32_t>(std::lround(x * 10000.0)), 10000}.normalized();
             };
             if (p.contains("frameRate")) {
                 in.frameRate = ratio("frameRate");
             }
             if (p.contains("pixelAspect")) {
                 in.pixelAspect = ratio("pixelAspect");
             }
             if (p.contains("fieldOrder")) {
                 in.fieldOrder = p["fieldOrder"].is_null() ? std::nullopt : fieldOrderFromString(c.string("fieldOrder"));
             }
             in.ignoreAlpha = c.boolean("ignoreAlpha", in.ignoreAlpha);
             in.invertAlpha = c.boolean("invertAlpha", in.invertAlpha);
             if (p.contains("premultiplication")) {
                 const std::string v = p["premultiplication"].is_null() ? std::string() : c.string("premultiplication");
                 in.premultiplication = v == "straight"        ? std::optional(AlphaMode::Straight)
                                        : v == "premultiplied" ? std::optional(AlphaMode::Premultiplied)
                                                               : std::nullopt;
             }
             edit::EditResult r = c.take(edit::interpretFootage(c.state(), media, in));
             const MediaItem& item = *r.state.findMedia(media.front());
             const Speed conform = edit::conformSpeed(item);
             std::size_t retimed = 0;
             for (const SequenceChange& s : r.changes.sequences) {
                 retimed += s.modifiedClips.size();
             }
             Json data{{"media", media.size()},
                       {"conformSpeed", std::round(static_cast<double>(conform.num) / conform.den * 100000.0) / 1000.0},
                       {"retimedClips", retimed}};
             return edited(std::move(r), std::move(data));
         }});
    // --- RAW Source Settings (0.2, RawSourceSettings.h) ------------------------------------------------------------
    addBuiltin(
        registry,
        {"media.rawSourceSettings", 1, "RAW Source Settings",
         "The Source Settings a camera RAW item (ProRes RAW, CinemaDNG) is developed with: \"raw\" whether the item is "
         "camera RAW at all, \"exposure\" in stops, \"whiteBalance\" \"asShot\" (the camera's) or \"custom\" with the "
         "\"temperature\" (K) and \"tint\" of the light it is balanced for.",
         Capability::Read, withSequence({{"media", mediaParam("A media item.")}}, {"media"}), Json{{"media", 3}}, {},
         [](ExecutionContext& c) {
             const MediaItem* item = c.state().findMedia(c.media("media"));
             CommandOutput out;
             out.state = c.state();
             out.data = rawSettingsJson(*item);
             return out;
         }});
    addBuiltin(
        registry,
        {"media.setRawSourceSettings", 1, "Source Settings",
         "Premiere's Clip > Modify > Source Settings for camera RAW items (ProRes RAW, CinemaDNG): develops the items "
         "with \"exposure\" (stops, -5 to +5) and a white balance - \"asShot\" (the camera's) or \"custom\": the "
         "\"temperature\" (2000 to 15000 K; lower turns the picture bluer) and \"tint\" (-100 to +100, toward magenta) "
         "of the light to balance for, from the as-shot light (ProRes RAW's temperature; 6500 K for CinemaDNG). Absent "
         "fields keep the first item's. Playback, thumbnails and export all develop the items so; their proxies are "
         "dropped (made from the old development). Items that are not camera RAW are refused. One undo step.",
         Capability::EditProject,
         withSequence({{"media", schema::array(mediaParam("A camera RAW item."), "The items.", 1)},
                       {"exposure", schema::number("Exposure, stops.", -kMaxRawExposureStops, kMaxRawExposureStops)},
                       {"whiteBalance", schema::enumeration({"asShot", "custom"}, "White balance.")},
                       {"temperature", schema::integer("Colour temperature of the light to balance for, K (custom).",
                                                       kMinRawTemperature, kMaxRawTemperature)},
                       {"tint", schema::integer("Tint, + toward magenta (custom).", -kMaxRawTint, kMaxRawTint)}},
                      {"media"}),
         Json{{"media", Json::array({3})}, {"exposure", 0.5}, {"whiteBalance", "custom"}, {"temperature", 3200}}, {},
         [](ExecutionContext& c) {
             const std::vector<MediaId> media = c.mediaList("media");
             RawSourceSettings settings = rawSourceSettingsOf(c.state().findMedia(media.front())->locator.path);
             if (c.has("exposure")) {
                 settings.exposure = c.number("exposure");
             }
             const std::string white = c.string("whiteBalance", settings.white ? "custom" : "asShot");
             if (white == "asShot") {
                 if (c.has("temperature") || c.has("tint")) {
                     fail(ErrorCode::InvalidParams, "a temperature or tint needs \"whiteBalance\": \"custom\"",
                          Json{{"path", "/whiteBalance"}});
                 }
                 settings.white.reset();
             } else {
                 RawWhiteBalance balance = settings.white.value_or(RawWhiteBalance{});
                 balance.temperature = static_cast<int>(c.integer("temperature", balance.temperature));
                 balance.tint = static_cast<int>(c.integer("tint", balance.tint));
                 settings.white = balance;
             }
             edit::EditResult r = c.take(edit::setRawSourceSettings(c.state(), media, settings));
             Json data = rawSettingsJson(*r.state.findMedia(media.front()));
             data["items"] = media.size();
             return edited(std::move(r), std::move(data));
         }});
    addBuiltin(
        registry,
        {"media.setTimecode", 1, "Modify Timecode",
         "Premiere's Clip > Modify > Timecode on items: the timecode of the first frame (\"timecode\" in the item's frame "
         "rate, or ticks; null: the file's own) and the reel (tape) name. Multicam and Merge Clips sync by it, and the "
         "Source Monitor shows it.",
         Capability::EditProject,
         withSequence({{"media", schema::array(mediaParam("A media item."), "The items.", 1)},
                       {"timecode", Json{{"type", Json::array({"integer", "string", "null"})},
                                         {"description", "The start timecode (\"01:00:00:00\"), ticks, or null for the "
                                                         "file's."}}},
                       {"reelName", schema::string("The reel name (at most 128 bytes).")}},
                      {"media"}),
         Json{{"media", Json::array({3})}, {"timecode", "01:00:00:00"}, {"reelName", "A001"}}, {},
         [](ExecutionContext& c) {
             const std::vector<MediaId> media = c.mediaList("media");
             const MediaItem& first = *c.state().findMedia(media.front());
             std::optional<Ticks> start = first.timecode ? first.timecode->start : std::nullopt;
             if (c.has("timecode")) {
                 const Json& v = c.params()["timecode"];
                 if (v.is_null()) {
                     start.reset();
                 } else if (v.is_string()) {
                     const Rate rate = sourceTimecodeRate(c.state(), first);
                     const auto t = parseTimecode(v.get<std::string>(), rate, false);
                     if (!t) {
                         fail(ErrorCode::InvalidParams, "not a timecode", Json{{"path", "/timecode"}});
                     }
                     start = *t;
                 } else {
                     start = Ticks{v.get<std::int64_t>()};
                 }
             }
             const std::string reel = c.has("reelName") ? c.string("reelName")
                                                        : (first.timecode ? first.timecode->reelName : std::string());
             ProjectState next = c.take(edit::setMediaTimecode(c.state(), media, start, reel));
             return projectChange(std::move(next), Json{{"media", media.size()}, {"reelName", reel}});
         }});
}

std::vector<std::string> scalingNames() {
    return {std::string(toString(MediaScaling::None)), std::string(toString(MediaScaling::ScaleToFrame)),
            std::string(toString(MediaScaling::SetToFrame)), std::string(toString(MediaScaling::SetToFill))};
}

/// The video clips among "clips" with their linked partners when "linked" (the audio partners are skipped by the ops).
std::vector<ClipId> clipsWithLinks(ExecutionContext& c) {
    const std::vector<ClipId> clips = c.clips("clips");
    return c.linkMode() == edit::LinkMode::FollowLinks ? edit::withLinkedClips(c.sequence(), clips) : clips;
}

/// {"clips": n, "scale": percent} of the first video clip after a Set to Frame Size / Fill Frame.
Json scaledJson(const edit::EditResult& r, SequenceId sequence, const std::vector<ClipId>& clips) {
    Json data{{"clips", clips.size()}};
    const Sequence* seq = r.state.findSequence(sequence);
    for (const ClipId id : clips) {
        const auto loc = seq ? seq->findClip(id) : std::nullopt;
        if (loc && loc->clip().stream.kind == StreamKind::Video) {
            data["scale"] = loc->clip().properties()[ParamId::Scale].constant() * 100.0;
            break;
        }
    }
    return data;
}

} // namespace

void registerEditingCommands(CommandRegistry& registry) {
    // --- Scale to Frame Size / Set to Frame Size / Set to Fill Frame (FrameScaling.h) -------------------------------
    addBuiltin(
        registry,
        {"clip.scaleToFrameSize", 1, "Scale to Frame Size",
         "Premiere's Clip > Video Options > Scale to Frame Size (also the clip's right-click menu): fits each video "
         "clip's picture into the sequence frame (whole picture visible, letterboxed when the aspect differs) before its "
         "Motion, so Motion > Scale stays 100 % and a later Scale applies to the fitted picture. \"enabled\": false turns "
         "it off (the picture shows at its pixel size again). Audio clips in the list (or linked) are skipped.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The clips (their video clips are scaled).")},
                       {"enabled", schema::boolean("On (default) or off.", true)},
                       {"linked", linkedParam(true)}},
                      {"clips"}),
         Json{{"clips", Json::array({42})}}, {},
         [](ExecutionContext& c) {
             const std::vector<ClipId> clips = clipsWithLinks(c);
             const bool enabled = c.boolean("enabled", true);
             return edited(c.take(edit::setScaleToFrameSize(c.state(), c.sequenceId(), clips, enabled)),
                           Json{{"enabled", enabled}, {"clips", clips.size()}});
         }});
    // --- Field Options (0.2, FieldOptions.h) ---------------------------------------------------------------------
    addBuiltin(
        registry,
        {"clip.setFieldOptions", 1, "Field Options",
         "Premiere's Clip > Video Options > Field Options for interlaced footage. \"reverseFieldDominance\" swaps the "
         "clip's field order; \"processing\" is \"none\" (default), \"interlaceConsecutiveFrames\" (kept for "
         "interchange: Wabcut renders progressive frames), \"alwaysDeinterlace\" (show one field, the dominant one, "
         "even in an interlaced sequence or on footage without fields) or \"flickerRemoval\" (blend the fields "
         "slightly against flicker on thin horizontal lines). Interlaced footage in a progressive sequence is "
         "deinterlaced anyway, both fields shown when the sequence runs at the field rate. Audio clips in the list (or "
         "linked) are skipped. One undo step.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The clips (their video clips change).")},
                       {"reverseFieldDominance", schema::boolean("Swap the clip's field order.", false)},
                       {"processing", schema::enumeration({"none", "interlaceConsecutiveFrames", "alwaysDeinterlace",
                                                           "flickerRemoval"},
                                                          "Processing Options.")},
                       {"linked", linkedParam(true)}},
                      {"clips"}),
         Json{{"clips", Json::array({42})}, {"processing", "alwaysDeinterlace"}}, {},
         [](ExecutionContext& c) {
             const std::vector<ClipId> clips = clipsWithLinks(c);
             ClipFieldOptions options;
             options.reverseFieldDominance = c.boolean("reverseFieldDominance", false);
             if (c.has("processing")) { // the schema admits the four names only
                 options.processing = fieldProcessingFromString(c.string("processing")).value_or(FieldProcessing::None);
             }
             return edited(c.take(edit::setFieldOptions(c.state(), c.sequenceId(), clips, options)),
                           Json{{"reverseFieldDominance", options.reverseFieldDominance},
                                {"processing", std::string(toString(options.processing))},
                                {"clips", clips.size()}});
         }});
    addBuiltin(
        registry,
        {"clip.setToFrameSize", 1, "Set to Frame Size",
         "Premiere's Set to Frame Size (Fit to Frame Size in Premiere 25 and later; Clip > Video Options and the "
         "right-click menu): sets each video clip's Motion > Scale so its whole picture fits the sequence frame, so the "
         "Effect Controls show how much it was scaled. Keys on Scale are replaced by the constant; Scale to Frame Size "
         "goes off. data.scale: the first clip's new Scale in percent.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The clips (their video clips are scaled).")}, {"linked", linkedParam(true)}},
                      {"clips"}),
         Json{{"clips", Json::array({42})}}, {},
         [](ExecutionContext& c) {
             const std::vector<ClipId> clips = clipsWithLinks(c);
             edit::EditResult r = c.take(edit::setToFrameSize(c.state(), c.sequenceId(), clips, edit::FrameFit::Frame));
             Json data = scaledJson(r, c.sequenceId(), clips);
             return edited(std::move(r), std::move(data));
         }});
    addBuiltin(
        registry,
        {"clip.setToFillFrame", 1, "Set to Fill Frame",
         "Premiere's Fill Frame (the clip's right-click menu): sets each video clip's Motion > Scale so its picture covers "
         "the whole sequence frame, cropping what overhangs (a 16:9 clip in a 9:16 Short fills the height). Keys on "
         "Scale are replaced by the constant; Scale to Frame Size goes off. data.scale: the first clip's new Scale in "
         "percent.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The clips (their video clips are scaled).")}, {"linked", linkedParam(true)}},
                      {"clips"}),
         Json{{"clips", Json::array({42})}}, {},
         [](ExecutionContext& c) {
             const std::vector<ClipId> clips = clipsWithLinks(c);
             edit::EditResult r = c.take(edit::setToFrameSize(c.state(), c.sequenceId(), clips, edit::FrameFit::Fill));
             Json data = scaledJson(r, c.sequenceId(), clips);
             return edited(std::move(r), std::move(data));
         }});
    addBuiltin(
        registry,
        {"media.setScaling", 1, "Default Media Scaling",
         "Sets the default media scaling of media items (Premiere's Preferences > Media > Default Media Scaling, which "
         "Premiere applies to items as they are imported): how later insert / overwrite edits and drops size their "
         "video clips in a sequence of another frame size - \"none\" (pixel size), \"scaleToFrameSize\", "
         "\"setToFrameSize\" or \"setToFillFrame\". Clips already in sequences keep their size.",
         Capability::EditProject,
         withSequence({{"media", schema::array(mediaParam("A media item."), "The media items.", 1)},
                       {"scaling", schema::enumeration(scalingNames(), "The default media scaling.")}},
                      {"media", "scaling"}),
         Json{{"media", Json::array({3})}, {"scaling", "setToFrameSize"}}, {},
         [](ExecutionContext& c) {
             const auto scaling = mediaScalingFromString(c.string("scaling"));
             if (!scaling) {
                 fail(ErrorCode::InvalidParams, "unknown media scaling");
             }
             const std::vector<MediaId> media = c.mediaList("media");
             CommandOutput out;
             out.state = c.take(edit::setMediaScaling(c.state(), media, *scaling));
             out.data = Json{{"media", media.size()}, {"scaling", std::string(toString(*scaling))}};
             return out;
         }});

    // --- Duplicate, New Sequence From Clip, Sequence Settings (SequenceSettings.h) ----------------------------------
    addBuiltin(
        registry,
        {"sequence.duplicate", 1, "Duplicate Sequence",
         "Premiere's Edit > Duplicate on a sequence in the Project panel: a copy of the whole sequence (tracks, clips, "
         "effects, markers, captions; new ids) in the same bin, named \"<name> Copy\" (\"Copy 2\", ... when taken) unless "
         "\"name\" is given. createdIds: the copy.",
         Capability::EditProject, withSequence({{"name", schema::string("The copy's name.")}}), Json::object(), {},
         [](ExecutionContext& c) {
             auto copy = c.take(edit::duplicateSequence(c.state(), c.sequenceId(), c.string("name")));
             const std::string name = copy.state.findSequence(copy.id)->name;
             return created(std::move(copy.state), copy.id.value, Json{{"sequence", copy.id.value}, {"name", name}});
         }});
    addBuiltin(
        registry,
        {"media.duplicate", 1, "Duplicate",
         "Premiere's Edit > Duplicate on a project item: another item for the same file (or title, synthetic item) with "
         "its settings, Source Monitor marks, clip markers and metadata, in the same bin, named \"<name> Copy\" unless "
         "\"name\" is given. Clips in sequences keep playing the original. createdIds: the new item.",
         Capability::EditProject,
         withSequence({{"media", mediaParam("The item to duplicate.")}, {"name", schema::string("The copy's name.")}},
                      {"media"}),
         Json{{"media", 3}}, {},
         [](ExecutionContext& c) {
             auto copy = c.take(edit::duplicateMedia(c.state(), c.media("media"), c.string("name")));
             const std::string name = copy.state.findMedia(copy.id)->name;
             return created(std::move(copy.state), copy.id.value, Json{{"media", copy.id.value}, {"name", name}});
         }});
    addBuiltin(
        registry,
        {"sequence.newFromClip", 1, "New Sequence From Clip",
         "Premiere's File > New > Sequence From Clip (the Project panel's right-click): a sequence whose settings match "
         "the item - frame size as it displays (a phone video turned 90 degrees makes a vertical sequence), frame rate "
         "(its interpretation when set), pixel aspect and audio sample rate - named after it, in its bin, with the item "
         "(between its Source Monitor marks) edited in at the start, picture on V1 and each audio stream on its own "
         "track. createdIds: the sequence.",
         Capability::EditProject,
         withSequence({{"media", mediaParam("The item.")}, {"name", schema::string("The sequence's name. Default: the item's.")}},
                      {"media"}),
         Json{{"media", 3}}, {},
         [](ExecutionContext& c) {
             auto made = c.take(edit::newSequenceFromClip(c.state(), c.media("media"), c.string("name")));
             const Sequence& seq = *made.state.findSequence(made.id);
             Json data = settingsJson(seq);
             std::size_t clips = 0;
             seq.forEachTrack([&](const Track& t) { clips += t.clips.size(); });
             data["clips"] = clips;
             return created(std::move(made.state), made.id.value, std::move(data));
         }});
    addBuiltin(
        registry,
        {"sequence.settings", 1, "Sequence Settings Info",
         "A sequence's settings as Premiere's Sequence Settings shows them: timebase, frame size, pixel aspect, fields, "
         "audio sample rate, drop-frame display, preview file format, video and audio display formats and the start "
         "timecode.",
         Capability::Read, withSequence({}), Json::object(), {},
         [](ExecutionContext& c) {
             CommandOutput out;
             out.state = c.state();
             out.data = settingsJson(c.sequence());
             return out;
         }});
    addBuiltin(
        registry,
        {"sequence.setSettings", 1, "Sequence Settings",
         "Premiere's Sequence > Sequence Settings on an existing sequence. \"preset\" (sequence.presets) sets the frame "
         "size, timebase and drop-frame display at once; the other fields override it. Changing the timebase of a "
         "sequence with clips keeps the edit in real time and puts every edit point on the new frame grid: clip starts "
         "and ends, transitions, markers, In/Out, the work area and caption cues move to the nearest frame (a clip "
         "without media to reach it gets the frame inside instead), clips keep the picture under them, adjacent clips "
         "stay adjacent and locked tracks are left alone. Frame size changes keep each clip's Motion. Drop-frame display "
         "applies at 29.97 and 59.94 only. \"label\" is the sequence's label colour (Premiere's Label: the Project "
         "panel and its timeline tab). One undo step.",
         Capability::EditProject,
         withSequence(
             {{"preset", schema::string("A preset id from sequence.presets (\"1080p23.976\", \"2160p29.97df\", ...).")},
              {"label", schema::integer("The label colour, 0 to 15 (the Project panel's 16 label colours).", 0, 15)},
              {"name", schema::string("The sequence's name (not empty).")},
              {"frameRate", rateParam("Timebase.")},
              {"width", schema::integer("Frame width, pixels.", 16, 16384)},
              {"height", schema::integer("Frame height, pixels.", 16, 16384)},
              {"pixelAspect", rateParam("Pixel aspect ratio (1 square, 0.9091 D1/DV NTSC, 1.4587 PAL widescreen, 2 "
                                        "anamorphic ...).")},
              {"fields", schema::enumeration(names({FieldOrder::Progressive, FieldOrder::UpperFirst, FieldOrder::LowerFirst}),
                                             "Fields: progressive (no fields), upper or lower field first.")},
              {"audioRate", schema::integer("Audio sample rate in Hz (32000, 44100, 48000, 96000 ...).", 8000, 384000)},
              {"dropFrame", schema::boolean("Drop-frame timecode display (29.97 and 59.94).")},
              {"previewFormat", schema::string("Preview file format id (\"\" = default; \"prores\", \"dnxhr\", \"h264\").")},
              {"videoDisplay",
               schema::enumeration(names({VideoTimeDisplay::Timecode, VideoTimeDisplay::Frames, VideoTimeDisplay::Feet16mm,
                                          VideoTimeDisplay::Feet35mm}),
                                   "Video display format.")},
              {"audioDisplay",
               schema::enumeration(names({AudioTimeDisplay::AudioSamples, AudioTimeDisplay::Milliseconds}),
                                   "Audio display format.")},
              {"startTimecode", Json{{"type", Json::array({"integer", "string"})},
                                     {"description", "Start time: the timecode of the first frame (\"01:00:00:00\") or "
                                                     "ticks."}}}}),
         Json{{"preset", "1080p23.976"}}, {},
         [](ExecutionContext& c) {
             const Sequence& seq = c.sequence();
             edit::SequenceSettingsChange change;
             if (c.has("preset")) {
                 const SequencePreset* preset = findSequencePreset(c.string("preset"));
                 if (!preset) {
                     fail(ErrorCode::InvalidParams, "unknown sequence preset", Json{{"path", "/preset"}});
                 }
                 change.frameRate = preset->frameRate;
                 change.width = preset->width;
                 change.height = preset->height;
                 change.dropFrame = preset->dropFrame;
             }
             if (c.has("name")) {
                 change.name = c.string("name");
             }
             if (c.has("label")) {
                 change.colorIndex = static_cast<std::int32_t>(c.integer("label"));
             }
             if (c.has("frameRate")) {
                 change.frameRate = frameRateValue(c.params()["frameRate"], "/frameRate");
             }
             if (c.has("width")) {
                 change.width = static_cast<std::int32_t>(c.integer("width"));
             }
             if (c.has("height")) {
                 change.height = static_cast<std::int32_t>(c.integer("height"));
             }
             if (c.has("pixelAspect")) {
                 const Json& par = c.params()["pixelAspect"];
                 if (par.is_object()) {
                     change.pixelAspect = frameRateValue(par, "/pixelAspect");
                 } else {
                     // A ratio such as 0.9091 or 1.4587: thousandths are exact enough for every PAR Premiere lists.
                     const double v = par.get<double>();
                     if (!(v >= 0.1 && v <= 10.0)) {
                         fail(ErrorCode::InvalidParams, "the pixel aspect ratio must be 0.1 to 10", Json{{"path", "/pixelAspect"}});
                     }
                     change.pixelAspect = Rate{static_cast<std::int32_t>(std::lround(v * 10000.0)), 10000}.normalized();
                 }
             }
             if (c.has("fields")) {
                 change.fields = fieldOrderFromString(c.string("fields"));
             }
             if (c.has("audioRate")) {
                 change.audioRate = Rate{static_cast<std::int32_t>(c.integer("audioRate")), 1};
             }
             if (c.has("dropFrame")) {
                 change.dropFrame = c.boolean("dropFrame", false);
             }
             if (c.has("previewFormat")) {
                 change.previewFormat = c.string("previewFormat");
             }
             if (c.has("videoDisplay")) {
                 change.videoDisplay = videoTimeDisplayFromString(c.string("videoDisplay"));
             }
             if (c.has("audioDisplay")) {
                 change.audioDisplay = audioTimeDisplayFromString(c.string("audioDisplay"));
             }
             if (c.has("startTimecode")) {
                 const Json& v = c.params()["startTimecode"];
                 if (v.is_string()) {
                     const Rate rate = change.frameRate.value_or(seq.frameRate);
                     const bool drop = change.dropFrame.value_or(seq.dropFrame);
                     const auto t = parseTimecode(v.get<std::string>(), rate, drop);
                     if (!t) {
                         fail(ErrorCode::InvalidParams, "not a timecode", Json{{"path", "/startTimecode"}});
                     }
                     change.startTimecode = *t;
                 } else {
                     change.startTimecode = Ticks{v.get<std::int64_t>()};
                 }
             }
             edit::EditResult r = c.take(edit::setSequenceSettings(c.state(), seq.id, change));
             const Sequence& after = *r.state.findSequence(seq.id);
             Json data = settingsJson(after);
             data["removed"] = r.removed.size();
             return edited(std::move(r), std::move(data));
         }});

    registerReplaceCommands(registry);
    registerSequenceToolCommands(registry);
    registerFootageCommands(registry);
}

} // namespace wabcut::command::detail
