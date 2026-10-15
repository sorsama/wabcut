#include <wabcut/ai/analysis/AnalysisCommands.h>

#include <wabcut/command/Schema.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wabcut::ai::analysis {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

/// Marker colours (Premiere's order): shots cyan, downbeats red, other beats blue.
constexpr std::int32_t kShotColor = 7;
constexpr std::int32_t kDownbeatColor = 1;
constexpr std::int32_t kBeatColor = 6;

AnalysisProvider& providerOf(ExecutionContext& c) {
    auto* services = command::findService<AnalysisServices>(c.services());
    if (!services || !services->provider) {
        fail(ErrorCode::InvalidRequest,
             "media analysis is not available here: the host provides none (wabcut-cli analyses media files)");
    }
    return *services->provider;
}

Json withSequence(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required = {}) {
    Json s = schema::object(properties, std::move(required));
    s["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    return s;
}

Json timeParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Timecode in the sequence's rate (\"00:01:02:03\") or ticks."}};
}

Json trackParam(std::string description) {
    return Json{{"type", Json::array({"string", "integer"})},
                {"description", description + " \"V1\", \"A2\", ... or a track id."},
                {"minimum", 1}};
}

Json clipParam(std::string description) {
    return Json{{"type", Json::array({"integer", "object"})},
                {"description", description + " A clip id, or {\"track\", \"time\"} for the clip there."},
                {"properties", Json{{"track", trackParam("Track of the clip.")}, {"time", timeParam("A time inside the clip.")}}},
                {"required", Json::array({"track", "time"})},
                {"additionalProperties", false},
                {"minimum", 1}};
}

Json mediaParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})}, {"description", description + " A media id or exact name."}, {"minimum", 1}};
}

struct Target {
    const Clip* clip = nullptr;
    TrackId track;
    const MediaItem* media = nullptr;
};

Target targetOf(ExecutionContext& c) {
    Target t;
    if (c.has("clip")) {
        const auto loc = c.sequence().findClip(c.clip("clip"));
        t.clip = &loc->clip();
        t.track = loc->track->id;
        t.media = c.state().findMedia(t.clip->media);
        if (t.clip->isNested()) {
            fail(ErrorCode::InvalidParams,
                 "clip " + std::to_string(t.clip->id.value) +
                     " plays a nested or multicam sequence, not a media item: clip.splitAtShots reads through it, or "
                     "name the angle's \"media\"",
                 Json{{"path", "/clip"}});
        }
    } else if (c.has("media")) {
        t.media = c.state().findMedia(c.media("media"));
    } else {
        fail(ErrorCode::InvalidParams, "name a \"clip\" or a \"media\" item");
    }
    if (!t.media) {
        fail(ErrorCode::NotFound, "the clip's media item does not exist");
    }
    return t;
}

AnalysisProgress progressFor(ExecutionContext& c, std::string what) {
    return [&c, what = std::move(what)](double fraction) {
        char text[64];
        std::snprintf(text, sizeof text, " (%d%%)", static_cast<int>(std::lround(fraction * 100.0)));
        c.checkpoint(what + text);
        return true;
    };
}

ShotOptions shotOptions(ExecutionContext& c) {
    ShotOptions o;
    if (c.has("detector")) {
        o.detector = *shotDetectorFromString(c.string("detector"));
    }
    if (c.has("threshold")) {
        o.contentThreshold = c.number("threshold");
    }
    return o;
}

int streamFor(const Target& t, StreamKind kind) {
    const int index = t.clip && t.clip->stream.kind == kind ? t.clip->stream.index : 0;
    if (!t.media->hasStream(StreamRef{kind, index})) {
        fail(ErrorCode::EditRejected, "'" + t.media->name + "' has no " + (kind == StreamKind::Video ? "video" : "audio"));
    }
    return index;
}

ShotAnalysis shotsOf(ExecutionContext& c, const MediaItem& media, int stream, const ShotOptions& options) {
    auto shots = providerOf(c).shots(media, stream, options, progressFor(c, "detecting shots in '" + media.name + "'"));
    if (!shots) {
        fail(shots.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
             "shot detection of '" + media.name + "' failed: " + shots.error().message);
    }
    return std::move(*shots);
}

/// The cuts strictly inside `clip`, in timeline time and order.
std::vector<std::pair<Ticks, const ShotCut*>> cutsInClip(const ShotAnalysis& shots, const Clip& clip, Rate rate) {
    std::vector<std::pair<Ticks, const ShotCut*>> out;
    for (const ShotCut& cut : shots.cuts) {
        if (const auto t = timelineTimeOfSource(clip, cut.time, rate); t && *t > clip.start() && *t < clip.end()) {
            out.emplace_back(*t, &cut);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

double rounded(double v, double unit) { return std::round(v / unit) * unit; }

/// 0.1.2: the media clips a nested or multicam video clip shows - the active video angle's track of a multicam
/// source, or a nested sequence's only video track with clips. Empty when there is no single such track (layers).
std::vector<const Clip*> pictureClipsOf(const ProjectState& state, const Clip& clip) {
    std::vector<const Clip*> out;
    const Sequence* inner = clip.isNested() && clip.stream.kind == StreamKind::Video ? state.findSequence(clip.sequence) : nullptr;
    if (!inner) {
        return out;
    }
    const Track* track = nullptr;
    if (inner->multicam) {
        const std::size_t angle = clip.angles.first();
        if (angle < inner->multicam->angles.size()) {
            track = inner->findTrack(inner->multicam->angles[angle].video);
        }
    } else {
        for (const TrackPtr& candidate : inner->videoTracks) {
            if (candidate->clips.empty()) {
                continue;
            }
            if (track) {
                return out;
            }
            track = candidate.get();
        }
    }
    if (!track) {
        return out;
    }
    for (const Clip& leaf : track->clips) {
        if (!leaf.isNested() && leaf.enabled) {
            out.push_back(&leaf);
        }
    }
    return out;
}

CommandOutput detectShotsCommand(ExecutionContext& c) {
    const Target t = targetOf(c);
    const ShotAnalysis shots = shotsOf(c, *t.media, streamFor(t, StreamKind::Video), shotOptions(c));
    CommandOutput out;
    ProjectState state = c.state();
    Json cuts = Json::array();
    std::size_t markers = 0;
    if (t.clip) {
        const bool addMarkers = c.boolean("markers", true);
        int shot = 1;
        for (const auto& [time, cut] : cutsInClip(shots, *t.clip, c.sequence().frameRate)) {
            cuts.push_back(Json::array({c.timecode(time), cut->frame, toString(cut->kind), rounded(cut->confidence, 0.01)}));
            if (addMarkers) {
                Marker m;
                m.start = time;
                m.name = "Shot " + std::to_string(++shot);
                m.comment = std::string(toString(cut->kind)) + ", confidence " + std::to_string(std::lround(cut->confidence * 100)) + "%";
                m.colorIndex = kShotColor;
                state = c.take(edit::addMarker(state, c.sequenceId(), std::move(m))).state;
                ++markers;
            }
        }
    } else {
        for (const ShotCut& cut : shots.cuts) {
            cuts.push_back(Json::array({c.timecode(cut.time), cut.frame, toString(cut.kind), rounded(cut.confidence, 0.01)}));
        }
    }
    out.state = std::move(state);
    out.data = Json{{"media", t.media->id.value},
                    {"frames", shots.frames},
                    {"timeBase", t.clip ? "timeline" : "media"},
                    {"cutFields", Json::array({"time", "frame", "kind", "confidence"})},
                    {"cuts", std::move(cuts)},
                    {"markersAdded", markers}};
    return out;
}

CommandOutput splitAtShotsCommand(ExecutionContext& c) {
    const ShotOptions options = shotOptions(c);
    const Sequence& seq = c.sequence();
    const SequenceId seqId = seq.id;
    std::vector<std::pair<TrackId, Ticks>> razors;
    Json cuts = Json::array();
    std::vector<std::string> warnings;
    for (const ClipId id : c.clips("clips")) {
        const auto loc = seq.findClip(id);
        const Clip& clip = loc->clip();
        if (clip.isNested()) {
            // 0.1.2: a multicam (or nested) clip is cut where the media its active angle shows changes shot: each cut
            // goes from that media's time to the source sequence's (the angle clip's place) and through the clip's own
            // time map to the timeline. The clip is razored in place and stays a multicam clip.
            bool analysed = false;
            for (const Clip* leaf : pictureClipsOf(c.state(), clip)) {
                const MediaItem* media = c.state().findMedia(leaf->media);
                const int leafStream = leaf->stream.kind == StreamKind::Video ? leaf->stream.index : 0;
                if (!media || !media->hasStream(StreamRef{StreamKind::Video, leafStream})) {
                    continue;
                }
                if (leaf->speed.num != leaf->speed.den || leaf->reversed || leaf->isRemapped()) {
                    warnings.push_back("clip " + std::to_string(id.value) + ": '" + media->name +
                                       "' is retimed inside the source sequence, so its shots are not used");
                    continue;
                }
                analysed = true;
                const ShotAnalysis shots = shotsOf(c, *media, leafStream, options);
                for (const ShotCut& cut : shots.cuts) {
                    const Ticks inner = leaf->start() + (cut.time - leaf->sourceIn);
                    if (inner <= leaf->start() || inner >= leaf->end()) {
                        continue;
                    }
                    if (const auto time = timelineTimeOfSource(clip, inner, seq.frameRate); time && *time > clip.start() && *time < clip.end()) {
                        razors.emplace_back(loc->track->id, *time);
                        cuts.push_back(Json::array({id.value, c.timecode(*time), toString(cut.kind), rounded(cut.confidence, 0.01)}));
                    }
                }
            }
            if (!analysed) {
                warnings.push_back("clip " + std::to_string(id.value) +
                                   " shows no single media clip to find shots in (a nested sequence with layers: split inside it)");
            }
            continue;
        }
        Target t{&clip, loc->track->id, c.state().findMedia(clip.media)};
        const int stream = t.clip->stream.kind == StreamKind::Video ? t.clip->stream.index : 0;
        if (!t.media || !t.media->hasStream(StreamRef{StreamKind::Video, stream})) {
            warnings.push_back("clip " + std::to_string(id.value) + " has no video to find shots in");
            continue;
        }
        const ShotAnalysis shots = shotsOf(c, *t.media, stream, options);
        for (const auto& [time, cut] : cutsInClip(shots, *t.clip, seq.frameRate)) {
            razors.emplace_back(t.track, time);
            cuts.push_back(Json::array({id.value, c.timecode(time), toString(cut->kind), rounded(cut->confidence, 0.01)}));
        }
    }
    ProjectState state = c.state();
    std::vector<std::uint64_t> created;
    for (const auto& [track, time] : razors) {
        const TrackId tracks[] = {track};
        edit::EditResult r = c.take(edit::addEdit(state, seqId, time, tracks, c.linkMode()));
        for (const ClipId id : r.created) {
            created.push_back(id.value);
        }
        state = std::move(r.state);
    }
    CommandOutput out;
    out.state = std::move(state);
    out.createdIds = std::move(created);
    out.data = Json{{"cutFields", Json::array({"clip", "time", "kind", "confidence"})}, {"cuts", std::move(cuts)}};
    out.warnings = std::move(warnings);
    return out;
}

CommandOutput detectBeatsCommand(ExecutionContext& c) {
    const Target t = targetOf(c);
    BeatOptions options;
    options.beatsPerBar = static_cast<int>(c.integer("beatsPerBar", 4));
    if (c.has("minBpm")) {
        options.minBpm = c.number("minBpm");
    }
    if (c.has("maxBpm")) {
        options.maxBpm = c.number("maxBpm");
    }
    const int stream = streamFor(t, StreamKind::Audio);
    auto analysed = providerOf(c).beats(*t.media, stream, options, progressFor(c, "detecting beats in '" + t.media->name + "'"));
    if (!analysed) {
        fail(analysed.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
             "beat detection of '" + t.media->name + "' failed: " + analysed.error().message);
    }
    const BeatAnalysis& beats = *analysed;
    // Each beat's place in the bar, counted from the detected downbeats.
    const auto bpb = static_cast<std::int64_t>(std::max(beats.beatsPerBar, 1));
    std::int64_t firstDownbeat = 0;
    if (!beats.downbeats.empty()) {
        const auto it = std::lower_bound(beats.beats.begin(), beats.beats.end(), beats.downbeats.front());
        firstDownbeat = it - beats.beats.begin();
    }
    const auto inBar = [&](std::size_t i) {
        return static_cast<std::int32_t>(((static_cast<std::int64_t>(i) - firstDownbeat) % bpb + bpb) % bpb) + 1;
    };
    CommandOutput out;
    ProjectState state = c.state();
    Json first = Json::array();
    std::size_t markers = 0;
    bool gridSet = false;
    double timelineBpm = beats.bpm;
    std::size_t inClip = 0;
    if (t.clip) {
        std::vector<std::pair<Ticks, std::int32_t>> mapped;
        for (std::size_t i = 0; i < beats.beats.size(); ++i) {
            if (const auto time = timelineTimeOfSource(*t.clip, beats.beats[i]);
                time && *time >= t.clip->start() && *time < t.clip->end()) {
                mapped.emplace_back(*time, inBar(i));
            }
        }
        std::sort(mapped.begin(), mapped.end());
        mapped.erase(std::unique(mapped.begin(), mapped.end(),
                                 [](const auto& a, const auto& b) { return a.first == b.first; }),
                     mapped.end());
        inClip = mapped.size();
        timelineBpm = t.clip->speed.den > 0
                          ? beats.bpm * static_cast<double>(t.clip->speed.num) / static_cast<double>(t.clip->speed.den)
                          : beats.bpm;
        const std::string mode = c.string("markers", "all");
        for (const auto& [time, place] : mapped) {
            if (first.size() < 16) {
                first.push_back(Json::array({c.timecode(time), place}));
            }
            if (mode == "none" || (mode == "downbeats" && place != 1)) {
                continue;
            }
            Marker m;
            m.start = time;
            m.name = std::to_string(place);
            m.colorIndex = place == 1 ? kDownbeatColor : kBeatColor;
            state = c.take(edit::addMarker(state, c.sequenceId(), std::move(m))).state;
            ++markers;
        }
        if (c.boolean("grid", true) && mapped.size() >= 2 && timelineBpm >= 1.0 && timelineBpm <= 1000.0) {
            BeatGrid grid;
            grid.bpmMilli = std::llround(timelineBpm * 1000.0);
            grid.beatsPerBar = static_cast<std::int32_t>(bpb);
            for (const auto& [time, place] : mapped) {
                grid.beats.push_back(time);
            }
            const auto downbeat = std::find_if(mapped.begin(), mapped.end(), [](const auto& b) { return b.second == 1; });
            grid.phase = downbeat != mapped.end() ? downbeat->first : mapped.front().first;
            state = c.take(edit::setBeatGrid(state, c.sequenceId(), std::move(grid))).state;
            gridSet = true;
        }
    } else {
        for (std::size_t i = 0; i < beats.beats.size() && first.size() < 16; ++i) {
            first.push_back(Json::array({c.timecode(beats.beats[i]), inBar(i)}));
        }
    }
    out.state = std::move(state);
    out.data = Json{{"media", t.media->id.value},
                    {"bpm", rounded(timelineBpm, 0.01)},
                    {"mediaBpm", rounded(beats.bpm, 0.01)},
                    {"confidence", rounded(beats.confidence, 0.01)},
                    {"beats", beats.beats.size()},
                    {"downbeats", beats.downbeats.size()},
                    {"timeBase", t.clip ? "timeline" : "media"},
                    {"beatsInClip", inClip},
                    {"markersAdded", markers},
                    {"grid", gridSet},
                    {"beatFields", Json::array({"time", "inBar"})},
                    {"firstBeats", std::move(first)}};
    if (beats.beats.empty()) {
        out.warnings.push_back("no beat found in '" + t.media->name + "'");
    }
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

std::expected<ShotAnalysis, Error> MapAnalysisProvider::shots(const MediaItem& media, int, const ShotOptions&,
                                                              const AnalysisProgress&) {
    const auto it = m_shots.find(media.id.value);
    if (it == m_shots.end()) {
        return std::unexpected(Error{wabcut::ErrorCode::NotFound, "no shots for '" + media.name + "'"});
    }
    return it->second;
}

std::expected<BeatAnalysis, Error> MapAnalysisProvider::beats(const MediaItem& media, int, const BeatOptions&,
                                                              const AnalysisProgress&) {
    const auto it = m_beats.find(media.id.value);
    if (it == m_beats.end()) {
        return std::unexpected(Error{wabcut::ErrorCode::NotFound, "no beats for '" + media.name + "'"});
    }
    return it->second;
}

std::optional<Ticks> timelineTimeOfSource(const Clip& clip, Ticks source, std::optional<Rate> snapTo) {
    Ticks t;
    if (clip.isRemapped()) {
        // sourceIn/sourceOut are curve times there: where the curve first shows `source`.
        const auto at = clip.timelineTimeOfSourceTime(source);
        if (!at) {
            return std::nullopt;
        }
        t = *at;
    } else {
        if (clip.isFreeze() || clip.speed.num <= 0 || source < clip.sourceIn || source > clip.sourceOut()) {
            return std::nullopt;
        }
        const Ticks offset = clip.reversed ? clip.sourceOut() - source : source - clip.sourceIn;
        const auto scaled = mulDiv(offset.value, clip.speed.den, clip.speed.num, Rounding::Nearest);
        if (!scaled) {
            return std::nullopt;
        }
        t = clip.start() + Ticks{*scaled};
    }
    if (!snapTo) {
        return t;
    }
    // The nearest frame boundary.
    const Ticks frame = framesToTicks(1, *snapTo);
    return frame.value > 0 ? snapToFrame(t + Ticks{frame.value / 2}, *snapTo) : t;
}

void registerAnalysisCommands(command::CommandRegistry& registry) {
    using command::Capability;
    const Json detector = schema::enumeration({"combined", "content", "adaptive", "threshold"},
                                              "Detector: combined (hard cuts, fades, dissolves; flashes ignored; default), "
                                              "content, adaptive or threshold (fades through black).");
    const Json threshold = schema::number("Content threshold (default 27; lower finds more cuts).", 1.0, 255.0);
    add(registry,
        {"analysis.detectShots", 1, "Detect Shots",
         "Finds the cuts (hard cuts, fades through black, dissolves; flashes are not cuts) in a clip's video and, for a "
         "clip, marks them with sequence markers (\"markers\": false to only list them). For a media item the cuts are "
         "listed in media time. Results are cached per source file.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip.")},
                       {"media", mediaParam("Or a media item.")},
                       {"detector", detector},
                       {"threshold", threshold},
                       {"markers", schema::boolean("Add a marker at each cut in the clip. Default true.", true)}}),
         Json{{"clip", Json{{"track", "V1"}, {"time", "00:00:10:00"}}}}, {}, detectShotsCommand});
    add(registry,
        {"clip.splitAtShots", 1, "Split at Shots",
         "Razors each clip at the cuts in its video (scene-edit detection), with its linked partners. One undo step; "
         "dryRun previews the split.",
         Capability::EditSequence,
         withSequence({{"clips", schema::array(clipParam("A clip."), "Clips to split.", 1)},
                       {"detector", detector},
                       {"threshold", threshold},
                       {"linked", schema::boolean("Split linked partners too. Default true.", true)}},
                      {"clips"}),
         Json{{"clips", Json::array({42})}}, {}, splitAtShotsCommand});
    add(registry,
        {"analysis.detectBeats", 1, "Detect Beats",
         "Finds the tempo and beats of a clip's music: beat markers labelled 1-2-3-4 (downbeats red) and the "
         "sequence's beat grid (snapping, edit.snapToBeats and edit.cutToBeats use it). For a media item the beats are "
         "listed in media time. Results are cached per source file.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The music clip.")},
                       {"media", mediaParam("Or a media item.")},
                       {"markers", schema::enumeration({"all", "downbeats", "none"}, "Which beats get markers. Default all.")},
                       {"grid", schema::boolean("Set the sequence's beat grid from the beats. Default true.", true)},
                       {"beatsPerBar", schema::integer("Default 4.", 1, 32)},
                       {"minBpm", schema::number("Slowest tempo to consider. Default 60.", 20.0, 400.0)},
                       {"maxBpm", schema::number("Fastest tempo to consider. Default 200.", 30.0, 600.0)}}),
         Json{{"clip", Json{{"track", "A1"}, {"time", "00:00:00:00"}}}, {"markers", "downbeats"}}, {}, detectBeatsCommand});
}

} // namespace wabcut::ai::analysis
