// Built-in project commands (capability EditProject) and read/query commands (capability Read).
// Query output is built for agents: compact arrays with a field list instead of one object per clip, and
// times as timecodes in the sequence's rate.

#include "Catalog.h"

#include <wabcut/model/Parameters.h>
#include <wabcut/model/RedactedSpeech.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wabcut::command::detail {

namespace {

std::string rateText(Rate r) { return std::to_string(r.num) + "/" + std::to_string(r.den); }

Json linkValue(const Clip& clip) { return clip.linkGroup ? Json(clip.linkGroup.value) : Json(nullptr); }

std::vector<std::string> trackFlags(const Track& t) {
    std::vector<std::string> flags;
    if (t.locked) {
        flags.emplace_back("locked");
    }
    if (t.targeted) {
        flags.emplace_back("targeted");
    }
    if (t.syncLocked) {
        flags.emplace_back("syncLocked");
    }
    if (t.muted) {
        flags.emplace_back("muted");
    }
    if (t.solo) {
        flags.emplace_back("solo");
    }
    if (!t.enabled) {
        flags.emplace_back("outputOff");
    }
    return flags;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

CommandOutput readOnly(ExecutionContext& c, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    return out;
}

const Json kClipFields = Json::array({"id", "name", "start", "end", "media", "sourceIn", "link"});

CommandOutput projectSummary(ExecutionContext& c) {
    const ProjectState& s = c.state();
    Json sequences = Json::array();
    for (const SequencePtr& seq : s.sequences) {
        std::size_t clips = 0;
        seq->forEachTrack([&](const Track& t) { clips += t.clips.size(); });
        sequences.push_back(Json{{"id", seq->id.value},
                                 {"name", seq->name},
                                 {"frameRate", rateText(seq->frameRate)},
                                 {"dropFrame", seq->dropFrame},
                                 {"size", Json::array({seq->width, seq->height})},
                                 {"duration", ticksToTimecode(seq->duration, seq->frameRate, seq->dropFrame)},
                                 {"videoTracks", seq->videoTracks.size()},
                                 {"audioTracks", seq->audioTracks.size()},
                                 {"clips", clips}});
    }
    std::size_t offline = 0;
    for (const MediaPtr& m : s.media) {
        offline += m->locator.offline ? 1 : 0;
    }
    return readOnly(c, Json{{"sequences", std::move(sequences)},
                            {"media", s.media.size()},
                            {"offlineMedia", offline},
                            {"bins", s.bins.size()},
                            {"ticksPerSecond", kTicksPerSecond}});
}

CommandOutput sequenceSummary(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const auto limit = static_cast<std::size_t>(c.integer("maxClips", 500));
    const std::optional<Ticks> from = c.optionalTime("from");
    const std::optional<Ticks> to = c.optionalTime("to");
    const std::vector<TrackId> only = c.tracks("tracks");
    std::size_t listed = 0;
    bool truncated = false;
    bool anyTransitions = false;
    Json tracks = Json::array();
    seq.forEachTrack([&](const Track& t) {
        if (!only.empty() && std::find(only.begin(), only.end(), t.id) == only.end()) {
            return;
        }
        Json clips = Json::array();
        Json disabled = Json::array();
        for (const Clip& clip : t.clips) {
            if ((from && clip.end() <= *from) || (to && clip.start() >= *to)) {
                continue;
            }
            if (listed == limit) {
                truncated = true;
                break;
            }
            ++listed;
            clips.push_back(Json::array({clip.id.value, clip.name(), c.timecode(clip.start()), c.timecode(clip.end()),
                                         clip.media.value, c.timecode(clip.sourceIn), linkValue(clip)}));
            if (!clip.enabled) {
                disabled.push_back(clip.id.value);
            }
        }
        Json track{{"track", c.trackLabel(t.id)}, {"id", t.id.value}, {"flags", trackFlags(t)}, {"clips", std::move(clips)}};
        if (!t.name.empty()) {
            track["name"] = t.name;
        }
        if (!disabled.empty()) {
            track["disabled"] = std::move(disabled);
        }
        if (!t.transitions.empty()) {
            Json list = Json::array();
            for (const Transition& tr : t.transitions) {
                const Ticks cut = transitionEditPoint(t, tr).value_or(Ticks{});
                list.push_back(Json::array({tr.id.value, tr.effect, c.timecode(cut), c.timecode(tr.before), c.timecode(tr.after)}));
            }
            track["transitions"] = std::move(list);
            anyTransitions = true;
        }
        tracks.push_back(std::move(track));
    });
    Json markers = Json::array();
    Json highlights = Json::array();
    for (std::size_t i = 0; i < seq.markers.size(); ++i) {
        const Marker& m = seq.markers[i];
        markers.push_back(Json::array({i, c.timecode(m.start), c.timecode(m.duration), m.name}));
        if (m.highlight) { // C.15
            Json h = highlightJson(c, *m.highlight);
            h["index"] = i;
            highlights.push_back(std::move(h));
        }
    }
    Json data{{"sequence", Json{{"id", seq.id.value},
                                {"name", seq.name},
                                {"frameRate", rateText(seq.frameRate)},
                                {"dropFrame", seq.dropFrame},
                                {"duration", c.timecode(seq.duration)},
                                {"in", seq.markIn ? Json(c.timecode(*seq.markIn)) : Json(nullptr)},
                                {"out", seq.markOut ? Json(c.timecode(*seq.markOut)) : Json(nullptr)}}},
              {"clipFields", kClipFields},
              {"tracks", std::move(tracks)},
              {"markerFields", Json::array({"index", "start", "duration", "name"})},
              {"markers", std::move(markers)}};
    if (!highlights.empty()) {
        data["highlights"] = std::move(highlights);
    }
    if (truncated) {
        data["truncated"] = true;
    }
    if (anyTransitions) {
        data["transitionFields"] = Json::array({"id", "effect", "cut", "before", "after"});
    }
    if (seq.beatGrid) {
        data["beatGrid"] = Json{{"bpm", seq.beatGrid->bpm()},
                                {"phase", c.timecode(seq.beatGrid->phase)},
                                {"beatsPerBar", seq.beatGrid->beatsPerBar},
                                {"detectedBeats", seq.beatGrid->beats.size()}};
    }
    if (!seq.captionTracks.empty()) {
        Json captions = Json::array();
        for (const CaptionTrack& ct : seq.captionTracks) {
            captions.push_back(Json::array({ct.id.value, ct.name, ct.language, ct.enabled, ct.cues.size()}));
        }
        data["captionTrackFields"] = Json::array({"id", "name", "language", "enabled", "cues"});
        data["captionTracks"] = std::move(captions);
    }
    return readOnly(c, std::move(data));
}

CommandOutput captionsGet(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    if (seq.captionTracks.empty()) {
        fail(ErrorCode::NotFound, "the sequence has no caption tracks (captions.generate makes one)");
    }
    const CaptionTrack* track = &seq.captionTracks.front();
    if (c.has("captionTrack")) {
        const auto id = static_cast<std::uint64_t>(c.integer("captionTrack"));
        const auto it = std::find_if(seq.captionTracks.begin(), seq.captionTracks.end(),
                                     [&](const CaptionTrack& ct) { return ct.id.value == id; });
        if (it == seq.captionTracks.end()) {
            fail(ErrorCode::NotFound, "no caption track " + std::to_string(id) + " in this sequence");
        }
        track = &*it;
    }
    const std::optional<Ticks> from = c.optionalTime("from");
    const std::optional<Ticks> to = c.optionalTime("to");
    const auto limit = static_cast<std::size_t>(c.integer("limit", 500));
    // Caption text read through the catalog goes to agents (the assistant's cloud model, MCP clients): a cue over speech
    // that a redaction bleeps or mutes is "[bleep]" as a whole, whatever made it - typed, imported or made before the
    // redaction (review wave 5, S5-P1-2). The stored text is unchanged; redactedCues lists the masked ones.
    const std::vector<TimeRange> redacted = redactedSpeechRanges(c.state(), seq);
    Json cues = Json::array();
    Json masked = Json::array();
    bool truncated = false;
    for (std::size_t i = 0; i < track->cues.size(); ++i) {
        const CaptionCue& cue = track->cues[i];
        if ((from && cue.end() <= *from) || (to && cue.start >= *to)) {
            continue;
        }
        if (cues.size() == limit) {
            truncated = true;
            break;
        }
        std::string text = outwardCueText(cue, redacted);
        if (text != cue.text) {
            masked.push_back(i);
        }
        cues.push_back(Json::array({c.timecode(cue.start), c.timecode(cue.end()), std::move(text)}));
    }
    // Speakers of the cues that have one, as [cue index, speaker id] (the index counts every cue of the track).
    Json speakers = Json::array();
    for (std::size_t i = 0; i < track->cues.size(); ++i) {
        if (track->cues[i].speaker.isValid()) {
            speakers.push_back(Json::array({i, track->cues[i].speaker.value}));
        }
    }
    Json data{{"captionTrack", Json{{"id", track->id.value},
                                    {"name", track->name},
                                    {"language", track->language},
                                    {"enabled", track->enabled},
                                    {"cues", track->cues.size()},
                                    {"style", captionStyleJson(track->style)},
                                    {"trackStyle", track->trackStyle}}},
              {"cueFields", Json::array({"start", "end", "text"})},
              {"cues", std::move(cues)}};
    if (!speakers.empty()) {
        data["cueSpeakers"] = std::move(speakers);
    }
    if (truncated) {
        data["truncated"] = true;
    }
    if (!masked.empty()) {
        data["redactedCues"] = std::move(masked);
    }
    return readOnly(c, std::move(data));
}

/// A constant as a number; a curve as {"keys": [[index, timeline timecode or null, media ticks, value,
/// interpolation]]} (keyFields order). A key outside the clip (trimmed off) has a null timeline time.
Json valueJson(ExecutionContext& c, const Clip& clip, const ParamValue& v) {
    const Curve* curve = v.curve();
    if (!curve) {
        return v.constant();
    }
    Json keys = Json::array();
    for (std::size_t i = 0; i < curve->keys.size(); ++i) {
        const Keyframe& k = curve->keys[i];
        const auto at = clip.timelineTimeOfCurveTime(k.time);
        keys.push_back(Json::array({i, at ? Json(c.timecode(*at)) : Json(nullptr), k.time.value, k.value,
                                    toString(k.interpolation)}));
    }
    return Json{{"keys", std::move(keys)}};
}

CommandOutput clipDetails(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const ClipId id = c.clip("clip");
    const auto loc = seq.findClip(id);
    const Clip& clip = loc->clip();
    const MediaItem* media = c.state().findMedia(clip.media);
    Json linked = Json::array();
    if (clip.linkGroup) {
        seq.forEachTrack([&](const Track& t) {
            for (const Clip& other : t.clips) {
                if (other.linkGroup == clip.linkGroup && other.id != clip.id) {
                    linked.push_back(other.id.value);
                }
            }
        });
    }
    Json params = Json::object();
    for (const ParameterInfo& p : allParameters()) {
        params[std::string(p.key)] = valueJson(c, clip, clip.properties()[p.id]);
    }
    Json effects = Json::array();
    for (const EffectInstance& e : clip.effects()) {
        Json values = Json::object();
        for (const EffectParam& p : e.params) {
            values[p.key] = valueJson(c, clip, p.value);
        }
        const EffectKind kind = loc->track->kind == TrackKind::Video ? EffectKind::Video : EffectKind::Audio;
        Json effect{{"instance", e.id.value},
                    {"effect", e.effect},
                    {"enabled", e.enabled},
                    {"known", isEvaluable(e, kind)},
                    {"params", std::move(values)}};
        // 0.2: text data (Simple Text's text, Image Matte Key's image, a grade's LUT), as clip.setEffect takes it.
        Json data = Json::object();
        for (const EffectData& d : e.data) {
            if (const std::string* text = d.text()) {
                data[d.key] = *text;
            }
        }
        if (!data.empty()) {
            effect["data"] = std::move(data);
        }
        effects.push_back(std::move(effect));
    }
    Json transitions = Json::array();
    for (const Transition& t : loc->track->transitions) {
        if (t.outgoing == clip.id || t.incoming == clip.id) {
            transitions.push_back(Json{{"id", t.id.value},
                                       {"effect", t.effect},
                                       {"edge", t.outgoing == clip.id ? "tail" : "head"},
                                       {"before", c.timecode(t.before)},
                                       {"after", c.timecode(t.after)},
                                       {"alignment", std::string(toString(t.alignment))}});
        }
    }
    Json details{{"id", clip.id.value},
                {"name", clip.name()},
                {"track", c.trackLabel(loc->track->id)},
                {"start", timeJson(c, clip.start())},
                {"end", timeJson(c, clip.end())},
                {"duration", timeJson(c, clip.duration())},
                {"media", Json{{"id", clip.media.value}, {"name", media ? media->name : std::string()}}},
                {"stream", Json{{"kind", clip.stream.kind == StreamKind::Video ? "video" : "audio"},
                                {"index", clip.stream.index}}},
                {"sourceIn", timeJson(c, clip.sourceIn)},
                {"sourceOut", timeJson(c, clip.sourceOut())},
                {"speed", std::to_string(clip.speed.num) + "/" + std::to_string(clip.speed.den)},
                {"enabled", clip.enabled},
                {"colorIndex", clip.colorIndex},
                {"link", linkValue(clip)},
                {"linkedClips", std::move(linked)},
                {"params", std::move(params)},
                {"keyFields", Json::array({"index", "time", "mediaTime", "value", "interpolation"})},
                {"effects", std::move(effects)},
                {"transitions", std::move(transitions)}};
    if (clip.reversed) {
        details["reversed"] = true;
    }
    if (const Sequence* nested = clip.isNested() ? c.state().findSequence(clip.sequence) : nullptr) {
        // C.8: the clip plays a sequence (its sourceIn/sourceOut are that sequence's times).
        details["media"] = nullptr;
        details["nested"] = Json{{"id", nested->id.value}, {"name", nested->name}, {"multicam", nested->multicam.has_value()}};
        if (nested->multicam) {
            Json angles = Json::array();
            for (std::size_t i = 0; i < kMaxMulticamAngles; ++i) {
                if (clip.angles.contains(i)) {
                    angles.push_back(i + 1);
                }
            }
            details["nested"]["angles"] = std::move(angles);
        }
    } else if (clip.hasUnknownSource()) {
        details["media"] = nullptr;
        details["unknownSource"] = true; // from a newer build: kept, plays nothing here
    }
    if (clip.maintainPitch) {
        details["maintainPitch"] = true;
    }
    if (clip.isFreeze()) {
        details["frameHold"] = true;
    }
    if (clip.content().scaleToFrame) {
        details["scaleToFrame"] = true; // P.2: Scale to Frame Size
    }
    if (const ClipFieldOptions& fields = clip.content().fieldOptions; !fields.isDefault()) { // 0.2: Field Options
        details["fieldOptions"] = Json{{"reverseFieldDominance", fields.reverseFieldDominance},
                                       {"processing", std::string(toString(fields.processing))}};
    }
    if (const Curve* remap = clip.timeRemap()) {
        // Keys at timeline times (null when outside the clip) with the source position they hold; the segments the
        // clip shows with their speed (null for a ramp).
        const auto timelineOf = [&](Ticks curveTime) { return clip.start() + (curveTime - clip.sourceIn); };
        Json keys = Json::array();
        for (std::size_t i = 0; i < remap->keys.size(); ++i) {
            const Keyframe& k = remap->keys[i];
            const Ticks t = timelineOf(k.time);
            keys.push_back(Json::array({i, t >= clip.start() && t <= clip.end() ? timeJson(c, t) : Json(nullptr),
                                        timeJson(c, Ticks{static_cast<std::int64_t>(k.value)}),
                                        std::string(toString(k.interpolation))}));
        }
        Json segments = Json::array();
        for (const RemapSegment& s : remapSegments(*remap)) {
            const Ticks a = std::max(clip.sourceIn, s.from);
            const Ticks b = std::min(clip.sourceOut(), s.to);
            if (a >= b) {
                continue;
            }
            const char* motion = s.motion == RemapMotion::Hold      ? "hold"
                                 : s.motion == RemapMotion::Forward ? "forward"
                                                                    : "reverse";
            Json speed = nullptr;
            if (s.motion == RemapMotion::Hold) {
                speed = 0.0;
            } else if (s.straight) {
                const double ratio = std::abs(static_cast<double>((s.sourceTo - s.sourceFrom).value)) /
                                     static_cast<double>((s.to - s.from).value);
                speed = std::round(ratio * 100000.0) / 1000.0;
            }
            segments.push_back(Json::array({timeJson(c, timelineOf(a)), timeJson(c, timelineOf(b)), motion, speed}));
        }
        // sourceIn/sourceOut are curve times on a remapped clip; the media it shows is "shownMedia".
        const TimeRange shownMedia = clip.shownSourceRange(clip.start(), clip.end());
        details["timeRemap"] = Json{{"keyFields", Json::array({"index", "time", "source", "interpolation"})},
                                    {"keys", std::move(keys)},
                                    {"segmentFields", Json::array({"start", "end", "motion", "speedPercent"})},
                                    {"segments", std::move(segments)},
                                    {"shownMedia", Json{{"start", timeJson(c, shownMedia.start)},
                                                        {"end", timeJson(c, shownMedia.end())}}}};
    }
    if (media && media->generator) {
        details["media"]["generator"] = Json{{"kind", media->generator->kind},
                                             {"text", media->generator->text},
                                             {"fontFamily", media->generator->fontFamily},
                                             {"fontStyle", media->generator->fontStyle}};
    }
    return readOnly(c, std::move(details));
}

CommandOutput mediaList(ExecutionContext& c) {
    const std::string filter = lower(c.string("name"));
    Json items = Json::array();
    for (const MediaPtr& m : c.state().media) {
        if (!filter.empty() && lower(m->name).find(filter) == std::string::npos) {
            continue;
        }
        const double seconds =
            static_cast<double>(m->info.duration.value) / static_cast<double>(kTicksPerSecond);
        items.push_back(Json::array({m->id.value, m->name, static_cast<std::int64_t>(seconds * 1000.0 + 0.5),
                                     m->info.hasVideo(), m->info.audio.size(), m->info.isStillImage,
                                     m->locator.offline}));
    }
    return readOnly(c, Json{{"mediaFields", Json::array({"id", "name", "durationMs", "hasVideo", "audioStreams",
                                                         "still", "offline"})},
                            {"media", std::move(items)}});
}

CommandOutput clipFind(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const std::string needle = lower(c.string("name"));
    const std::optional<Ticks> at = c.optionalTime("time");
    const std::vector<TrackId> tracks = c.tracks("tracks");
    const std::optional<MediaId> media = c.has("media") ? std::optional(c.media("media")) : std::nullopt;
    const auto limit = static_cast<std::size_t>(c.integer("limit", 50));
    Json found = Json::array();
    bool truncated = false;
    seq.forEachTrack([&](const Track& t) {
        if (truncated || (!tracks.empty() && std::find(tracks.begin(), tracks.end(), t.id) == tracks.end())) {
            return;
        }
        for (const Clip& clip : t.clips) {
            if ((at && !clip.timelineRange.contains(*at)) || (media && clip.media != *media) ||
                (!needle.empty() && lower(clip.name()).find(needle) == std::string::npos)) {
                continue;
            }
            if (found.size() == limit) {
                truncated = true;
                return;
            }
            found.push_back(Json::array({clip.id.value, c.trackLabel(t.id), clip.name(), c.timecode(clip.start()),
                                         c.timecode(clip.end()), linkValue(clip)}));
        }
    });
    Json data{{"clipFields", Json::array({"id", "track", "name", "start", "end", "link"})}, {"clips", std::move(found)}};
    if (truncated) {
        data["truncated"] = true;
    }
    return readOnly(c, std::move(data));
}

} // namespace

void registerProjectCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"sequence.addTrack", 1, "Add Track",
                "Adds a video track above the top video track or an audio track below the last audio track.",
                Capability::EditProject,
                withSequence({{"kind", schema::enumeration({"video", "audio"}, "Track kind.")},
                              {"name", schema::string("Custom name. Default: \"Video n\" / \"Audio n\".")}},
                             {"kind"}),
                Json{{"kind", "audio"}}, {},
                [](ExecutionContext& c) {
                    const TrackKind kind = c.string("kind") == "video" ? TrackKind::Video : TrackKind::Audio;
                    auto created = c.take(edit::addTrack(c.state(), c.sequenceId(), kind, c.string("name")));
                    CommandOutput out;
                    const Sequence& seq = *created.state.findSequence(c.sequenceId());
                    const std::size_t number = seq.tracks(kind).size();
                    out.data = Json{{"track", (kind == TrackKind::Video ? "V" : "A") + std::to_string(number)},
                                    {"id", created.id.value}};
                    out.createdIds.push_back(created.id.value);
                    out.state = std::move(created.state);
                    return out;
                }});
    addBuiltin(registry,
               {"sequence.removeTrack", 1, "Delete Track",
                "Deletes a track and its clips (their linked partners on other tracks stay, unlinked). The track must "
                "be unlocked. A submix track (\"S1\"): the tracks assigned to it output to the Mix and the sends to it "
                "are removed.",
                Capability::EditProject, withSequence({{"track", trackParam("The track to delete.")}}, {"track"}),
                Json{{"track", "V3"}}, {},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::removeTrack(c.state(), c.sequenceId(), c.track("track"))));
                }});
}

void registerQueryCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"project.summary", 1, "Project Summary",
                "Lists the project's sequences (id, rate, length, track and clip counts) and media counts. Start here.",
                Capability::Read, schema::object({}), Json::object(), {}, projectSummary});
    addBuiltin(
        registry,
        {"sequence.summary", 1, "Sequence Summary",
         "Compact view of a sequence: settings, In/Out marks, every track (label, id, flags) with its clips as arrays "
         "in clipFields order (times are timecodes; link = link group, clips with the same value are linked), and "
         "markers. Use from/to to look at part of a long sequence.",
         Capability::Read,
         withSequence({{"from", timeParam("Only clips that end after this time.")},
                       {"to", timeParam("Only clips that start before this time.")},
                       {"tracks", tracksParam("Only these tracks. Default: all.")},
                       {"maxClips", schema::integer("Most clips to list (default 500); \"truncated\" is set when more exist.",
                                                    1, 100000)}}),
         Json{{"from", "00:00:00:00"}, {"to", "00:01:00:00"}}, {}, sequenceSummary});
    addBuiltin(registry,
               {"clip.details", 1, "Clip Details",
                "Everything about one clip: track, timeline and source range (ticks and timecode), media, stream, speed, "
                "enabled, label colour, linked clips and all parameter values (and effects' text data, e.g. Simple "
                "Text's text).",
                Capability::Read, withSequence({{"clip", clipParam("The clip.")}}, {"clip"}), Json{{"clip", 42}}, {},
                clipDetails});
    addBuiltin(registry,
               {"media.list", 1, "List Media",
                "Lists the project's media as arrays in mediaFields order: id, name, duration in ms, whether it has "
                "video, its number of audio streams, whether it is a still image and whether it is offline.",
                Capability::Read,
                schema::object({{"name", schema::string("Only media whose name contains this (case-insensitive).")}}),
                Json{{"name", "interview"}}, {}, mediaList});
    addBuiltin(registry,
               {"clip.find", 1, "Find Clips",
                "Finds clips by name (case-insensitive substring), by a time they cover, by track and by media. Returns "
                "arrays in clipFields order.",
                Capability::Read,
                withSequence({{"name", schema::string("Part of the clip name.")},
                              {"time", timeParam("Only clips under this time.")},
                              {"tracks", tracksParam("Only these tracks.")},
                              {"media", mediaParam("Only clips of this media.")},
                              {"limit", schema::integer("Most clips to return (default 50).", 1, 10000)}}),
                Json{{"name", "interview"}, {"tracks", Json::array({"V1"})}}, {}, clipFind});
    addBuiltin(registry,
               {"captions.get", 1, "Caption Cues",
                "The cues of a caption track (default: the sequence's first) as arrays in cueFields order: start, end, "
                "text (its lines joined by newlines). A cue over redacted speech (a bleep or mute) reads \"[bleep]\" as "
                "a whole; redactedCues lists their indices. sequence.summary lists the caption tracks.",
                Capability::Read,
                withSequence({{"captionTrack", schema::integer("Caption track id. Default: the first.", 1)},
                              {"from", timeParam("Only cues that end after this time.")},
                              {"to", timeParam("Only cues that start before this time.")},
                              {"limit", schema::integer("Most cues to return (default 500).", 1, 100000)}}),
                Json{{"from", "00:00:00:00"}, {"to", "00:01:00:00"}}, {}, captionsGet});
}

} // namespace wabcut::command::detail
