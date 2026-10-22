// A.9 (docs/ai.md "Audio channels and 5.1 (A.9)"): Premiere's Modify > Audio Channels, Fill Left / Fill Right, Swap,
// Channel Volume and split to mono on clips; channel maps of media items; track channel formats and 5.1 panners; the
// master's format; and the per-channel levels the assistant reads to find "the left-only audio".
//
//   audio.setChannels        clips: a preset or a map (EditSequence)
//   audio.setMediaChannels   media items: the same (EditProject: it changes the items)
//   audio.setChannelVolume   one clip channel's volume, a constant or a keyframe (EditSequence)
//   audio.fillChannel        Fill Left with Right / Fill Right with Left (EditSequence)
//   audio.splitToMono        a clip's channels as linked mono clips on separate tracks (EditSequence)
//   audio.channels           what each clip plays (Read)
//   audio.channelLevels      peak / RMS per source channel (MediaAnalysis: it decodes media)
//   track.setFormat          mono, stereo, 5.1, adaptive (EditSequence)
//   track.setSurroundPan     the track's 5.1 panner (EditSequence)
//   sequence.setMasterFormat stereo or 5.1 (EditSequence)
//
// Channel numbers in params and reports are 1-based, as Premiere shows them.

#include "AudioCommandHelpers.h"

#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/Resolve.h>

namespace wabcut::audio::edit {

namespace {

using namespace detail;

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

const Clip& clipIn(ExecutionContext& c, ClipId id) {
    const auto at = c.sequence().findClip(id);
    if (!at) {
        fail(ErrorCode::NotFound, "no clip " + std::to_string(id.value) + " in the sequence");
    }
    return at->clip();
}

/// The stream's channel count (2 when it was not probed).
int sourceChannelsOf(const ProjectState& state, const Clip& clip) {
    const MediaItem* media = state.findMedia(clip.media);
    if (media != nullptr && clip.stream.index >= 0 && static_cast<std::size_t>(clip.stream.index) < media->info.audio.size()) {
        const int n = media->info.audio[static_cast<std::size_t>(clip.stream.index)].channels;
        if (n > 0) {
            return n;
        }
    }
    return 2;
}

ChannelFormat formatParam(ExecutionContext& c, const char* key) {
    const auto format = channelFormatFromString(c.string(key));
    if (!format) {
        fail(ErrorCode::InvalidParams, "unknown channel format '" + c.string(key) + "' (mono, stereo, 5.1, adaptive)",
             Json{{"path", std::string("/") + key}});
    }
    return *format;
}

/// A map from "map" ({format, channels: [{sources: [1-based], muted, gainDb}]}) or "preset".
std::optional<AudioChannelMap> mapParam(ExecutionContext& c, int sourceChannels) {
    if (c.has("map") == c.has("preset")) {
        fail(ErrorCode::InvalidParams, "give \"preset\" or \"map\"");
    }
    if (c.has("preset")) {
        const std::string preset = c.string("preset");
        const int channel = static_cast<int>(c.integer("channel", 1)) - 1;
        if (preset == "default") {
            return std::nullopt;
        }
        if (preset == "stereo" || preset == "identity") {
            return AudioChannelMap::identity(preset == "stereo" ? 2 : sourceChannels);
        }
        if (preset == "mono") {
            if (channel < 0 || channel >= kMaxSourceChannels) {
                fail(ErrorCode::InvalidParams, "\"channel\" is a source channel, 1 or more", Json{{"path", "/channel"}});
            }
            return AudioChannelMap::mono(channel);
        }
        if (preset == "sumToMono") {
            return AudioChannelMap::sumToMono(sourceChannels);
        }
        if (preset == "fillLeft") {
            return AudioChannelMap::fill(1);
        }
        if (preset == "fillRight") {
            return AudioChannelMap::fill(0);
        }
        if (preset == "swap") {
            return AudioChannelMap::swap();
        }
        fail(ErrorCode::InvalidParams, "unknown preset '" + preset + "'", Json{{"path", "/preset"}});
    }
    const Json& m = c.params()["map"];
    AudioChannelMap map;
    const auto format = m.contains("format") && m["format"].is_string() ? channelFormatFromString(m["format"].get<std::string>()) : std::nullopt;
    if (!format) {
        fail(ErrorCode::InvalidParams, "the map needs a \"format\": mono, stereo, 5.1 or adaptive", Json{{"path", "/map/format"}});
    }
    map.format = *format;
    if (!m.contains("channels") || !m["channels"].is_array()) {
        fail(ErrorCode::InvalidParams, "the map needs \"channels\"", Json{{"path", "/map/channels"}});
    }
    for (const Json& ch : m["channels"]) {
        ChannelRoute route;
        if (ch.contains("sources") && ch["sources"].is_array()) {
            for (const Json& s : ch["sources"]) {
                if (!s.is_number_integer() || s.get<std::int64_t>() < 1 || s.get<std::int64_t>() > kMaxSourceChannels) {
                    fail(ErrorCode::InvalidParams, "source channels are 1.." + std::to_string(kMaxSourceChannels),
                         Json{{"path", "/map/channels"}});
                }
                route.sources.push_back(static_cast<std::int32_t>(s.get<std::int64_t>() - 1));
            }
        }
        route.muted = ch.contains("muted") && ch["muted"].is_boolean() && ch["muted"].get<bool>();
        if (ch.contains("gainDb") && ch["gainDb"].is_number()) {
            route.gainDb = ParamValue(ch["gainDb"].get<double>());
        }
        map.channels.push_back(std::move(route));
    }
    if (const auto problem = channelMapProblem(map)) {
        fail(ErrorCode::InvalidParams, *problem, Json{{"path", "/map"}});
    }
    return map;
}

Json gainJson(const ParamValue& v) {
    if (v.isConstant()) {
        return round3(v.constant());
    }
    Json keys = Json::array();
    for (const Keyframe& k : v.curve()->keys) {
        keys.push_back(Json{{"ticks", k.time.value}, {"db", round3(k.value)}});
    }
    return Json{{"keys", std::move(keys)}};
}

Json mapJson(const AudioChannelMap& map) {
    Json channels = Json::array();
    for (const ChannelRoute& r : map.channels) {
        Json sources = Json::array();
        for (const std::int32_t s : r.sources) {
            sources.push_back(s + 1);
        }
        Json ch{{"sources", std::move(sources)}};
        if (r.muted) {
            ch["muted"] = true;
        }
        if (!(r.gainDb == ParamValue())) {
            ch["gainDb"] = gainJson(r.gainDb);
        }
        channels.push_back(std::move(ch));
    }
    return Json{{"format", std::string(toString(map.format))}, {"channels", std::move(channels)}};
}

/// What a clip plays: its map (or its media's, or none: the stream as it is).
Json clipChannelsJson(ExecutionContext& c, const ProjectState& state, const Clip& clip) {
    const MediaItem* media = state.findMedia(clip.media);
    const AudioChannelMap* map = wabcut::edit::effectiveChannelMap(clip, media);
    Json out{{"clip", clip.id.value}, {"sourceChannels", sourceChannelsOf(state, clip)}};
    out["mapFrom"] = clip.channelMap() != nullptr ? "clip" : map != nullptr ? "media" : "none";
    out["map"] = map != nullptr ? mapJson(*map) : mapJson(AudioChannelMap::identity(sourceChannelsOf(state, clip)));
    (void)c;
    return out;
}

CommandOutput setChannels(ExecutionContext& c) {
    const std::vector<ClipId> clips = audioClipsOf(c, c.clips("clips"));
    if (clips.empty()) {
        fail(ErrorCode::InvalidParams, "\"clips\" names no clip", Json{{"path", "/clips"}});
    }
    const std::optional<AudioChannelMap> map = mapParam(c, sourceChannelsOf(c.state(), clipIn(c, clips.front())));
    wabcut::edit::EditResult result = c.take(wabcut::edit::setClipChannels(c.state(), c.sequenceId(), clips, map));
    Json report = Json::array();
    for (const ClipId id : clips) {
        const Clip& clip = result.state.findSequence(c.sequenceId())->findClip(id)->clip();
        report.push_back(clipChannelsJson(c, result.state, clip));
    }
    return edited(std::move(result), Json{{"clips", std::move(report)}});
}

CommandOutput setMediaChannels(ExecutionContext& c) {
    const std::vector<MediaId> media = c.mediaList("media");
    if (media.empty()) {
        fail(ErrorCode::InvalidParams, "\"media\" names no item", Json{{"path", "/media"}});
    }
    ProjectState state = c.state();
    Json report = Json::array();
    for (const MediaId id : media) {
        const MediaItem* item = state.findMedia(id);
        if (item == nullptr) {
            fail(ErrorCode::NotFound, "no media " + std::to_string(id.value));
        }
        const int sourceChannels = !item->info.audio.empty() && item->info.audio.front().channels > 0 ? item->info.audio.front().channels : 2;
        const std::optional<AudioChannelMap> map = mapParam(c, sourceChannels);
        state = c.take(wabcut::edit::setMediaChannels(state, id, map));
        report.push_back(Json{{"media", id.value}, {"map", map ? mapJson(*map) : Json(nullptr)}});
    }
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"media", std::move(report)}};
    return out;
}

CommandOutput setChannelVolume(ExecutionContext& c) {
    const std::vector<ClipId> clips = audioClipsOf(c, c.clips("clips"));
    const int channel = static_cast<int>(c.integer("channel")) - 1;
    const double db = c.number("db");
    const std::optional<Ticks> time = c.optionalTime("time");
    ProjectState state = c.state();
    for (const ClipId id : clips) {
        const Clip& clip = state.findSequence(c.sequenceId())->findClip(id)->clip();
        const MediaItem* media = state.findMedia(clip.media);
        const AudioChannelMap* current = wabcut::edit::effectiveChannelMap(clip, media);
        AudioChannelMap map = current != nullptr ? *current : AudioChannelMap::identity(sourceChannelsOf(state, clip));
        if (channel < 0 || channel >= map.channelCount()) {
            fail(ErrorCode::InvalidParams,
                 "clip " + std::to_string(id.value) + " has " + std::to_string(map.channelCount()) + " channels, not " +
                     std::to_string(channel + 1),
                 Json{{"path", "/channel"}});
        }
        ParamValue& gain = map.channels[static_cast<std::size_t>(channel)].gainDb;
        if (time) {
            // A key at the clip's curve time of that timeline time (Premiere's Channel Volume keyframes).
            const Ticks curveTime = clip.curveTimeAt(*time);
            Curve curve = gain.isCurve() ? *gain.curve() : Curve{};
            if (curve.keys.empty() && gain.isConstant() && !(gain == ParamValue())) {
                curve.keys.push_back(Keyframe{clip.curveTimeAt(clip.start()), gain.constant()});
            }
            std::erase_if(curve.keys, [&](const Keyframe& k) { return k.time == curveTime; });
            curve.keys.push_back(Keyframe{curveTime, db});
            std::sort(curve.keys.begin(), curve.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
            gain = ParamValue(std::move(curve));
        } else {
            gain = ParamValue(db);
        }
        const ClipId only[] = {id};
        state = c.take(wabcut::edit::setClipChannels(state, c.sequenceId(), only, map)).state;
    }
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"clips", clips.size()}, {"channel", channel + 1}, {"db", db}};
    return out;
}

CommandOutput fillChannel(ExecutionContext& c) {
    const std::vector<ClipId> clips = audioClipsOf(c, c.clips("clips"));
    const std::string from = c.string("from");
    const std::string to = c.string("to", from == "left" ? "right" : "left");
    if ((from != "left" && from != "right") || (to != "left" && to != "right") || from == to) {
        fail(ErrorCode::InvalidParams, "\"from\" and \"to\" are left and right, one each", Json{{"path", "/from"}});
    }
    for (const ClipId id : clips) {
        if (sourceChannelsOf(c.state(), clipIn(c, id)) < 2) {
            fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " has a mono source: there is no other side to fill from",
                 Json{{"path", "/clips"}});
        }
    }
    // Fill Left with Right: both sides play the right channel (source 2).
    const AudioChannelMap map = AudioChannelMap::fill(from == "right" ? 1 : 0);
    wabcut::edit::EditResult result = c.take(wabcut::edit::setClipChannels(c.state(), c.sequenceId(), clips, map));
    return edited(std::move(result), Json{{"clips", clips.size()}, {"from", from}, {"to", to}, {"map", mapJson(map)}});
}

/// Free audio tracks below `home` over `range` (none of their clips overlaps), adding tracks as needed.
std::vector<TrackId> freeTracksBelow(ExecutionContext& c, ProjectState& state, TrackId home, TimeRange range, std::size_t count) {
    std::vector<TrackId> out;
    const auto isFree = [&](const Track& t) {
        return !t.locked && std::none_of(t.clips.begin(), t.clips.end(),
                                         [&](const Clip& o) { return o.start() < range.end() && range.start < o.end(); });
    };
    bool below = false;
    for (const TrackPtr& t : state.findSequence(c.sequenceId())->audioTracks) {
        if (t->id == home) {
            below = true;
            continue;
        }
        if (below && out.size() < count && isFree(*t)) {
            out.push_back(t->id);
        }
    }
    while (out.size() < count) {
        auto made = c.take(wabcut::edit::addTrack(state, c.sequenceId(), TrackKind::Audio));
        state = std::move(made.state);
        out.push_back(made.id);
    }
    return out;
}

CommandOutput splitToMono(ExecutionContext& c) {
    const std::vector<ClipId> requested = audioClipsOf(c, {c.clip("clip")});
    const ClipId id = requested.front();
    const auto at = c.sequence().findClip(id);
    const Clip clip = at->clip();
    const TrackId home = at->track->id;
    const int sources = sourceChannelsOf(c.state(), clip);
    if (sources < 2) {
        fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " is mono already", Json{{"path", "/clip"}});
    }
    ProjectState state = c.state();
    std::vector<TrackId> targets;
    if (c.has("tracks")) {
        targets = c.tracks("tracks");
        if (targets.size() != static_cast<std::size_t>(sources - 1)) {
            fail(ErrorCode::InvalidParams,
                 "a " + std::to_string(sources) + "-channel clip needs " + std::to_string(sources - 1) + " more tracks",
                 Json{{"path", "/tracks"}});
        }
    } else {
        targets = freeTracksBelow(c, state, home, clip.timelineRange, static_cast<std::size_t>(sources - 1));
    }
    std::vector<int> channels;
    for (int s = 0; s < sources; ++s) {
        channels.push_back(s);
    }
    wabcut::edit::EditResult result = c.take(wabcut::edit::splitClipToMono(state, c.sequenceId(), id, channels, targets));
    Json report = Json::array();
    const Sequence& seq = *result.state.findSequence(c.sequenceId());
    const auto labelOf = [&](TrackId t) { // in the result: the split may have added tracks
        for (std::size_t i = 0; i < seq.audioTracks.size(); ++i) {
            if (seq.audioTracks[i]->id == t) {
                return "A" + std::to_string(i + 1);
            }
        }
        return std::string("A?");
    };
    report.push_back(Json{{"clip", id.value}, {"track", labelOf(home)}, {"channel", 1}});
    for (std::size_t k = 0; k < targets.size(); ++k) {
        for (const Clip& other : seq.findTrack(targets[k])->clips) {
            if (other.start() == clip.start() && other.media == clip.media) {
                report.push_back(Json{{"clip", other.id.value}, {"track", labelOf(targets[k])}, {"channel", static_cast<int>(k) + 2}});
            }
        }
    }
    return edited(std::move(result), Json{{"clips", std::move(report)}});
}

CommandOutput channels(ExecutionContext& c) {
    std::vector<ClipId> clips = c.has("clips") ? audioClipsOf(c, c.clips("clips")) : std::vector<ClipId>{};
    if (clips.empty()) {
        for (const TrackPtr& t : c.sequence().audioTracks) {
            for (const Clip& clip : t->clips) {
                if (!clip.isNested() && !clip.hasUnknownSource()) {
                    clips.push_back(clip.id);
                }
            }
        }
    }
    Json report = Json::array();
    for (const ClipId id : clips) {
        report.push_back(clipChannelsJson(c, c.state(), clipIn(c, id)));
    }
    Json tracks = Json::array();
    for (const TrackPtr& t : c.sequence().audioTracks) {
        Json track{{"track", c.trackLabel(t->id)}, {"format", std::string(toString(t->format))}};
        if (!t->surround.isDefault()) {
            track["surround"] = Json{{"x", round3(t->surround.x.constant())}, {"y", round3(t->surround.y.constant())},
                                     {"lfeDb", round3(t->surround.lfeDb.constant())}};
        }
        tracks.push_back(std::move(track));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"master", std::string(toString(c.sequence().masterFormat))}, {"tracks", std::move(tracks)}, {"clips", std::move(report)}};
    return out;
}

CommandOutput channelLevels(ExecutionContext& c) {
    auto* services = command::findService<AudioServices>(c.services());
    if (services == nullptr || !services->channels) {
        fail(ErrorCode::InvalidRequest, "no channel analysis here: the host provides none (wabcut-cli and the app measure the media)");
    }
    const std::vector<ClipId> clips = audioClipsOf(c, c.clips("clips"));
    Json report = Json::array();
    // One recording a clip plays: its media's stream over `source`.
    struct Played {
        const MediaItem* media = nullptr;
        int stream = 0;
        TimeRange source;
    };
    for (const ClipId id : clips) {
        const Clip& clip = clipIn(c, id);
        std::vector<Played> played;
        if (clip.isNested()) {
            // 0.2: a nested or multicam clip plays a mix of recordings (a multicam clip: its active audio angles'
            // tracks): each of them is measured over what the clip plays of it, found at its start or a little later
            // (an angle that starts late), one entry each.
            for (int quarter = 0; quarter < 4 && played.empty(); ++quarter) {
                const Ticks t = clip.start() + Ticks{clip.duration().value * quarter / 4};
                for (const ResolvedLeaf& leaf : resolveLeaves(c.state(), c.sequenceId(), clip, t)) {
                    const MediaItem* media = c.state().findMedia(leaf.clip->media);
                    const int stream = leaf.clip->stream.kind == StreamKind::Audio ? leaf.clip->stream.index : 0;
                    if (media == nullptr || std::ranges::any_of(played, [&](const Played& p) { return p.media == media && p.stream == stream; })) {
                        continue;
                    }
                    played.push_back({media, stream, TimeRange{leaf.sourceTime, std::min(clip.end() - t, leaf.clip->end() - leaf.time)}});
                }
            }
        } else if (const MediaItem* media = c.state().findMedia(clip.media)) {
            played.push_back({media, clip.stream.index, TimeRange{clip.sourceIn, clip.sourceDuration()}});
        }
        if (played.empty()) {
            fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " plays no media item", Json{{"path", "/clips"}});
        }
        for (const Played& p : played) {
            const TimeRange source = p.source;
            auto levels = services->channels->measure(c.state(), *p.media, p.stream, source, c.control());
            if (!levels) {
                failWith(levels.error(), "cannot measure the channels of clip " + std::to_string(id.value));
            }
            Json list = Json::array();
            std::size_t loudest = 0;
            for (std::size_t k = 0; k < levels->size(); ++k) {
                list.push_back(Json{{"channel", k + 1}, {"peakDb", levelJson((*levels)[k].peakDb)}, {"rmsDb", levelJson((*levels)[k].rmsDb)}});
                if ((*levels)[k].rmsDb > (*levels)[loudest].rmsDb) {
                    loudest = k;
                }
            }
            // A channel 30 dB or more under the loudest is "silent" for the report ("left-only" audio).
            Json silent = Json::array();
            for (std::size_t k = 0; k < levels->size(); ++k) {
                if ((*levels)[k].rmsDb < (*levels)[loudest].rmsDb - 30.0) {
                    silent.push_back(k + 1);
                }
            }
            Json entry{{"clip", id.value},
                       {"channels", std::move(list)},
                       {"silentChannels", std::move(silent)},
                       {"measuredSeconds", round3(std::min(source.duration.toSeconds(), ChannelAnalyzer::kMaxSeconds))}};
            if (clip.isNested()) {
                entry["media"] = p.media->id.value; // the recording inside the nested / multicam clip
                entry["name"] = p.media->name;
            }
            report.push_back(std::move(entry));
        }
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"clips", std::move(report)}};
    return out;
}

CommandOutput setTrackFormat(ExecutionContext& c) {
    wabcut::edit::TrackStateChange change;
    change.format = formatParam(c, "format");
    const TrackId track = c.track("track");
    return edited(c.take(wabcut::edit::setTrackState(c.state(), c.sequenceId(), track, change)),
                  Json{{"track", c.trackLabel(track)}, {"format", c.string("format")}});
}

CommandOutput setSurroundPan(ExecutionContext& c) {
    const TrackId track = c.track("track");
    const Track* t = c.sequence().findTrack(track);
    if (t == nullptr) {
        fail(ErrorCode::NotFound, "no such track");
    }
    SurroundPan pan = t->surround;
    if (c.has("x")) {
        pan.x = ParamValue(c.number("x"));
    }
    if (c.has("y")) {
        pan.y = ParamValue(c.number("y"));
    }
    if (c.has("lfeDb")) {
        pan.lfeDb = ParamValue(c.number("lfeDb"));
    }
    wabcut::edit::TrackStateChange change;
    change.surround = pan;
    return edited(c.take(wabcut::edit::setTrackState(c.state(), c.sequenceId(), track, change)),
                  Json{{"track", c.trackLabel(track)},
                       {"x", round3(pan.x.constant())},
                       {"y", round3(pan.y.constant())},
                       {"lfeDb", round3(pan.lfeDb.constant())}});
}

CommandOutput setMasterFormat(ExecutionContext& c) {
    const ChannelFormat format = formatParam(c, "format");
    return edited(c.take(wabcut::edit::setMasterFormat(c.state(), c.sequenceId(), format)), Json{{"master", c.string("format")}});
}

Json mapSchema() {
    Json route = schema::object({{"sources", schema::array(schema::integer("A source channel, 1-based.", 1, kMaxSourceChannels),
                                                           "Source channels averaged into this clip channel (empty: silent).")},
                                 {"muted", schema::boolean("Mute this clip channel.")},
                                 {"gainDb", schema::number("This channel's volume, dB.", -144.0, 24.0)}});
    return schema::object({{"format", schema::enumeration({"mono", "stereo", "5.1", "adaptive"}, "The clip's channel format.")},
                           {"channels", schema::array(route, "One entry per clip channel (L, R, C, LFE, Ls, Rs for 5.1).", 1)}},
                          {"format", "channels"});
}

Json presetSchema() {
    return schema::enumeration({"default", "stereo", "identity", "mono", "sumToMono", "fillLeft", "fillRight", "swap"},
                               "default: no map (the stream as it is); stereo: source 1 and 2 as L and R; identity: every source "
                               "channel as it is; mono: one source channel (\"channel\"); sumToMono: every channel averaged; "
                               "fillLeft: both sides from the right channel (Fill Left with Right); fillRight: both from the left; "
                               "swap: left and right exchanged.");
}

} // namespace

void registerChannelCommands(command::CommandRegistry& registry) {
    const Json clipsParam = schema::array(Json{{"type", Json::array({"integer", "object"})}},
                                          "The audio clips (video clips stand for their linked audio).", 1);
    add(registry,
        {"audio.setChannels", 1, "Modify Audio Channels",
         "Premiere's Modify > Audio Channels on clips: which source channels feed each of the clip's channels, in a clip format "
         "(mono, stereo, 5.1, adaptive), with mutes and per-channel volume. A \"preset\" (fillLeft, fillRight, swap, sumToMono, "
         "mono with \"channel\", stereo, identity, default) or a \"map\". Use it when the two microphones of an interview are "
         "on the left and right channels of one file, or a recording has sound on one side only. One undo step.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam},
                       {"preset", presetSchema()},
                       {"channel", schema::integer("For the mono preset: the source channel, 1-based.", 1, kMaxSourceChannels)},
                       {"map", mapSchema()}},
                      {"clips"}),
         Json{{"clips", Json::array({12})}, {"preset", "fillLeft"}}, {}, setChannels});
    add(registry,
        {"audio.setMediaChannels", 1, "Modify Media Audio Channels",
         "Modify > Audio Channels on media items: the channel map every audio clip of the item plays unless it has its own "
         "(audio.setChannels). The same presets and maps. It changes media items, so it is a project edit. One undo step.",
         Capability::EditProject,
         schema::object({{"media", schema::array(Json{{"type", Json::array({"integer", "string"})}}, "The media items.", 1)},
                         {"preset", presetSchema()},
                         {"channel", schema::integer("For the mono preset: the source channel, 1-based.", 1, kMaxSourceChannels)},
                         {"map", mapSchema()}},
                        {"media"}),
         Json{{"media", Json::array({4})}, {"preset", "stereo"}}, {}, setMediaChannels});
    add(registry,
        {"audio.setChannelVolume", 1, "Set Channel Volume",
         "Premiere's Channel Volume: the volume of one channel of audio clips (1-based clip channel), dB, a constant, or with "
         "\"time\" a keyframe there (the channel's volume becomes animated). One undo step.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam},
                       {"channel", schema::integer("The clip channel, 1-based (1 = left, 2 = right).", 1, kMaxClipChannels)},
                       {"db", schema::number("The level in dB (-144 = silence .. +24).", -144.0, 24.0)},
                       {"time", timeParam("Put a keyframe here instead of setting a constant.")}},
                      {"clips", "channel", "db"}),
         Json{{"clips", Json::array({12})}, {"channel", 2}, {"db", -6}}, {}, setChannelVolume});
    add(registry,
        {"audio.fillChannel", 1, "Fill Channel",
         "Premiere's Fill Left with Right and Fill Right with Left: both sides of the clips play one channel - for audio heard "
         "on one side only (\"fix the left-only audio\": from left). \"from\" is the side that has the sound. One undo step.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam},
                       {"from", schema::enumeration({"left", "right"}, "The channel with the sound.")},
                       {"to", schema::enumeration({"left", "right"}, "The side to fill (default: the other one).")}},
                      {"clips", "from"}),
         Json{{"clips", Json::array({12})}, {"from", "left"}}, {}, fillChannel});
    add(registry,
        {"audio.splitToMono", 1, "Split to Mono",
         "Splits a clip's channels into linked mono clips, one per source channel, on separate audio tracks (the clip keeps "
         "channel 1; the others go on the free audio tracks below it, new ones when needed, or on \"tracks\"): \"split the "
         "two mics into separate tracks\". Each keeps the clip's level and effects. One undo step.",
         Capability::EditSequence,
         withSequence({{"clip", schema::integer("The audio clip (or a video clip with linked audio).", 1)},
                       {"tracks", schema::array(trackParam("A track for channel 2, 3, ..."), "The tracks for the other channels.")}},
                      {"clip"}),
         Json{{"clip", 12}}, {}, splitToMono});
    add(registry,
        {"audio.channels", 1, "Audio Channels",
         "What each audio clip plays (its source channel count and channel map: the clip's, its media item's or none), each "
         "audio track's channel format and 5.1 panner, and the master's format. A read: it changes nothing.",
         Capability::Read, withSequence({{"clips", clipsParam}}), Json::object(), {}, channels});
    add(registry,
        {"audio.channelLevels", 1, "Measure Channel Levels",
         "Peak and RMS of each source channel of audio clips (the clip's source range, at most 120 s), and the channels 30 dB "
         "or more under the loudest (\"silentChannels\"): which microphone is on which channel, and whether a recording has "
         "sound on one side only. Decodes media; changes nothing.",
         Capability::MediaAnalysis, withSequence({{"clips", clipsParam}}, {"clips"}), Json{{"clips", Json::array({12})}}, {},
         channelLevels});
    add(registry,
        {"track.setFormat", 1, "Set Track Channel Format",
         "An audio track's channel format (Premiere's mono, stereo, 5.1 and adaptive tracks): clips are mixed to it (mono "
         "averages, 5.1 to stereo is ITU-R BS.775), and it to the master's format; adaptive passes channels through one to "
         "one. One undo step.",
         Capability::EditSequence,
         withSequence({{"track", trackParam("The audio track.")},
                       {"format", schema::enumeration({"mono", "stereo", "5.1", "adaptive"}, "The format.")}},
                      {"track", "format"}),
         Json{{"track", "A2"}, {"format", "mono"}}, {}, setTrackFormat});
    add(registry,
        {"track.setSurroundPan", 1, "Set Track 5.1 Panner",
         "The 5.1 panner of a mono or stereo track on a 5.1 master (Premiere's track 5.1 panner): the puck x (-1 left .. +1 "
         "right) and y (-1 back .. +1 front; front centre plays stereo on L/R and mono on C) and the LFE send in dB (-144 = "
         "off). One undo step.",
         Capability::EditSequence,
         withSequence({{"track", trackParam("The audio track.")},
                       {"x", schema::number("Left .. right.", -1.0, 1.0)},
                       {"y", schema::number("Back .. front.", -1.0, 1.0)},
                       {"lfeDb", schema::number("The LFE send, dB.", -144.0, 0.0)}},
                      {"track"}),
         Json{{"track", "A1"}, {"x", -0.5}, {"y", 0.5}}, {}, setSurroundPan});
    add(registry,
        {"sequence.setMasterFormat", 1, "Set Master Channel Format",
         "The sequence's master: stereo, or 5.1 (mono and stereo tracks through their 5.1 panners; previewed on a stereo device "
         "and exported to stereo through an ITU-R BS.775 downmix; exports with 0 channels follow it). One undo step.",
         Capability::EditSequence, withSequence({{"format", schema::enumeration({"stereo", "5.1"}, "The master's format.")}}, {"format"}),
         Json{{"format", "5.1"}}, {}, setMasterFormat});
}

} // namespace wabcut::audio::edit
