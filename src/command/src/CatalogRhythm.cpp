// Built-in rhythm commands (C.5): the sequence beat grid (set by hand, by taps, or from beat detection), listing
// beats, snapping clip edges to beats and assembling clips one per beat group (beat-synced music-video editing).
// Beat detection itself (analysis.detectBeats) needs the media and lives with the analysis commands.

#include "Catalog.h"

#include <wabcut/model/FinishingOps.h>

#include <algorithm>
#include <cmath>

namespace wabcut::command::detail {

namespace {

const BeatGrid& gridOf(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    if (!seq.beatGrid) {
        fail(ErrorCode::EditRejected,
             "the sequence has no beat grid: run analysis.detectBeats on the music or sequence.setBeatGrid first");
    }
    return *seq.beatGrid;
}

Json beatJson(ExecutionContext& c, const Beat& b) { return Json::array({c.timecode(b.time), b.inBar, b.index}); }

/// Tempo and downbeat from tapped times: a least-squares line through tap i at time t_i.
BeatGrid gridFromTaps(ExecutionContext& c, std::vector<Ticks> taps) {
    std::sort(taps.begin(), taps.end());
    if (taps.size() < 2) {
        fail(ErrorCode::InvalidParams, "tap at least two beats", Json{{"path", "/taps"}});
    }
    const auto n = static_cast<double>(taps.size());
    double meanI = 0.0;
    double meanT = 0.0;
    for (std::size_t i = 0; i < taps.size(); ++i) {
        meanI += static_cast<double>(i);
        meanT += static_cast<double>(taps[i].value);
    }
    meanI /= n;
    meanT /= n;
    double sxy = 0.0;
    double sxx = 0.0;
    for (std::size_t i = 0; i < taps.size(); ++i) {
        const double di = static_cast<double>(i) - meanI;
        sxy += di * (static_cast<double>(taps[i].value) - meanT);
        sxx += di * di;
    }
    const double periodTicks = sxy / sxx;
    if (!(periodTicks > 0.0)) {
        fail(ErrorCode::InvalidParams, "the taps must be at different times", Json{{"path", "/taps"}});
    }
    BeatGrid grid;
    grid.bpmMilli = std::llround(60.0 * static_cast<double>(kTicksPerSecond) * 1000.0 / periodTicks);
    grid.phase = Ticks{std::max<std::int64_t>(std::llround(meanT - meanI * periodTicks), 0)};
    grid.beatsPerBar = static_cast<std::int32_t>(c.integer("beatsPerBar", 4));
    return grid;
}

CommandOutput setBeatGridCommand(ExecutionContext& c) {
    if (c.boolean("clear", false)) {
        return edited(c.take(edit::setBeatGrid(c.state(), c.sequenceId(), std::nullopt)), Json{{"beatGrid", nullptr}});
    }
    BeatGrid grid;
    if (c.has("taps")) {
        std::vector<Ticks> taps;
        const Json& list = c.params()["taps"];
        for (std::size_t i = 0; i < list.size(); ++i) {
            taps.push_back(c.timeValue(list[i], "/taps/" + std::to_string(i)));
        }
        grid = gridFromTaps(c, std::move(taps));
    } else {
        if (!c.has("bpm")) {
            fail(ErrorCode::InvalidParams, "give \"bpm\" (and \"phase\"), \"taps\", or \"clear\"");
        }
        grid.bpmMilli = std::llround(c.number("bpm") * 1000.0);
        grid.phase = c.optionalTime("phase").value_or(Ticks{});
        grid.beatsPerBar = static_cast<std::int32_t>(c.integer("beatsPerBar", 4));
    }
    if (c.has("beats")) {
        const Json& list = c.params()["beats"];
        for (std::size_t i = 0; i < list.size(); ++i) {
            grid.beats.push_back(c.timeValue(list[i], "/beats/" + std::to_string(i)));
        }
        std::sort(grid.beats.begin(), grid.beats.end());
        grid.beats.erase(std::unique(grid.beats.begin(), grid.beats.end()), grid.beats.end());
        if (!c.has("phase") && !grid.beats.empty()) {
            grid.phase = grid.beats.front();
        }
    }
    const double bpm = grid.bpm();
    const Ticks phase = grid.phase;
    const std::int32_t beatsPerBar = grid.beatsPerBar;
    return edited(c.take(edit::setBeatGrid(c.state(), c.sequenceId(), std::move(grid))),
                  Json{{"bpm", bpm}, {"phase", c.timecode(phase)}, {"beatsPerBar", beatsPerBar}});
}

CommandOutput listBeats(ExecutionContext& c) {
    const BeatGrid& grid = gridOf(c);
    const Sequence& seq = c.sequence();
    const Ticks from = c.optionalTime("from").value_or(Ticks{});
    const Ticks to = c.optionalTime("to").value_or(std::max(seq.duration, seq.contentEnd()));
    const auto limit = static_cast<std::size_t>(c.integer("limit", 500));
    Json beats = Json::array();
    bool truncated = false;
    for (const Beat& b : grid.beatsIn(TimeRange::fromStartEnd(from, std::max(from, to)))) {
        if (beats.size() == limit) {
            truncated = true;
            break;
        }
        beats.push_back(beatJson(c, b));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"bpm", grid.bpm()},
                    {"phase", c.timecode(grid.phase)},
                    {"beatsPerBar", grid.beatsPerBar},
                    {"detected", !grid.beats.empty()},
                    {"beatFields", Json::array({"time", "inBar", "index"})},
                    {"beats", std::move(beats)}};
    if (truncated) {
        out.data["truncated"] = true;
    }
    return out;
}

CommandOutput snapToBeats(ExecutionContext& c) {
    const BeatGrid& grid = gridOf(c);
    const Ticks tolerance = c.optionalTime("tolerance").value_or(Ticks{grid.period().value / 4});
    const std::string edges = c.string("edges", "both");
    const bool heads = edges != "tail";
    const bool tails = edges != "head";
    ProjectState state = c.state();
    const SequenceId seqId = c.sequenceId();
    Json snapped = Json::array();
    std::vector<std::string> warnings;
    for (const ClipId id : c.clips("clips")) {
        for (const edit::Edge edge : {edit::Edge::Head, edit::Edge::Tail}) {
            if ((edge == edit::Edge::Head && !heads) || (edge == edit::Edge::Tail && !tails)) {
                continue;
            }
            const auto loc = state.findSequence(seqId)->findClip(id);
            if (!loc) {
                break; // a linked partner's trim removed it? (cannot happen with trims; be safe)
            }
            const Clip& clip = loc->clip();
            const Ticks at = edge == edit::Edge::Head ? clip.start() : clip.end();
            const auto beat = grid.nearestBeat(at);
            if (!beat || beat->time == at) {
                continue;
            }
            const Ticks distance = beat->time > at ? beat->time - at : at - beat->time;
            if (distance > tolerance) {
                continue;
            }
            if ((edge == edit::Edge::Head && beat->time >= clip.end()) ||
                (edge == edit::Edge::Tail && beat->time <= clip.start())) {
                continue; // the clip would vanish
            }
            auto trimmed = edit::trimEdge(state, seqId, id, edge, beat->time - at, c.linkMode());
            const char* name = edge == edit::Edge::Head ? "head" : "tail";
            if (!trimmed || trimmed->appliedDelta != beat->time - at) {
                warnings.push_back("clip " + std::to_string(id.value) + " " + name + " could not reach the beat at " +
                                   c.timecode(beat->time) +
                                   (trimmed ? std::string(": no room or media") : ": " + trimmed.error().message));
                continue;
            }
            state = std::move(trimmed->state);
            snapped.push_back(Json::array({id.value, name, c.timecode(at), c.timecode(beat->time), beat->inBar}));
        }
    }
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"snapFields", Json::array({"clip", "edge", "from", "to", "inBar"})}, {"snapped", std::move(snapped)}};
    out.warnings = std::move(warnings);
    return out;
}

struct AssemblySource {
    MediaId media;
    Ticks sourceIn;
    std::string label;
};

CommandOutput cutToBeats(ExecutionContext& c) {
    const BeatGrid& grid = gridOf(c);
    const Sequence& seq = c.sequence();
    const SequenceId seqId = seq.id;
    std::vector<AssemblySource> sources;
    for (const ClipId id : c.clips("clips")) {
        const Clip& clip = seq.findClip(id)->clip();
        // From the media time the clip starts on (a remapped clip's sourceIn is a curve time).
        sources.push_back({clip.media, clip.isRemapped() ? clip.sourceTimeAt(clip.start()) : clip.sourceIn,
                           "clip " + std::to_string(id.value)});
    }
    for (const MediaId id : c.mediaList("media")) {
        sources.push_back({id, Ticks{}, c.state().findMedia(id)->name});
    }
    if (sources.empty()) {
        fail(ErrorCode::InvalidParams, "give \"clips\" and/or \"media\" to assemble");
    }
    const auto beatsPerCut = static_cast<std::size_t>(c.integer("beatsPerCut", 4));
    if (!c.has("videoTrack") && seq.videoTracks.empty()) {
        fail(ErrorCode::InvalidParams, "cut to beats needs a video track; this sequence has none");
    }
    const TrackId videoTrack = c.has("videoTrack") ? c.track("videoTrack") : seq.videoTracks.front()->id;
    if (seq.findTrack(videoTrack)->kind != TrackKind::Video) {
        fail(ErrorCode::InvalidParams, "videoTrack must name a video track");
    }
    std::optional<TrackId> audioTrack;
    if (c.has("audioTrack")) {
        audioTrack = c.track("audioTrack");
    }
    // The beats the cuts land on: from the first beat at or after `at`, one group of beatsPerCut per source.
    std::optional<Beat> beat = grid.beatAtOrAfter(c.time("at"));
    ProjectState state = c.state();
    Json placed = Json::array();
    std::vector<std::string> warnings;
    std::vector<std::uint64_t> created;
    for (const AssemblySource& src : sources) {
        if (!beat) {
            warnings.push_back("the beat grid ends before " + src.label + "; it was not placed");
            continue;
        }
        const Beat first = *beat;
        for (std::size_t k = 0; k < beatsPerCut && beat; ++k) {
            beat = grid.beatAtOrAfter(beat->time + Ticks{1});
        }
        if (!beat) {
            warnings.push_back("the beat grid ends inside the group for " + src.label + "; it was not placed");
            continue;
        }
        const MediaItem& media = *state.findMedia(src.media);
        Ticks duration = beat->time - first.time;
        edit::SourceClip source;
        source.media = src.media;
        if (media.hasStream(StreamRef{StreamKind::Video, 0})) {
            source.patches.push_back({StreamRef{StreamKind::Video, 0}, videoTrack});
        }
        if (audioTrack && media.hasStream(StreamRef{StreamKind::Audio, 0})) {
            source.patches.push_back({StreamRef{StreamKind::Audio, 0}, *audioTrack});
        }
        if (source.patches.empty()) {
            warnings.push_back(src.label + " has nothing for the chosen tracks; it was not placed");
            continue;
        }
        for (const edit::SourcePatch& p : source.patches) {
            if (const auto extent = media.sourceExtent(p.source); extent && src.sourceIn + duration > *extent) {
                duration = std::max(*extent - src.sourceIn, Ticks{});
            }
        }
        if (duration.value <= 0) {
            warnings.push_back(src.label + " has no media after its In; it was not placed");
            continue;
        }
        if (duration < beat->time - first.time) {
            warnings.push_back(src.label + " is shorter than its beat group; it ends early");
        }
        source.sourceRange = TimeRange{src.sourceIn, duration};
        edit::EditResult r = c.take(edit::overwriteEdit(state, seqId, source, first.time));
        for (const ClipId id : r.placed) {
            created.push_back(id.value);
        }
        placed.push_back(Json::array({r.placed.empty() ? 0 : r.placed.front().value, src.label, c.timecode(first.time),
                                      c.timecode(first.time + duration), first.inBar}));
        state = std::move(r.state);
    }
    CommandOutput out;
    out.state = std::move(state);
    out.createdIds = std::move(created);
    out.data = Json{{"placedFields", Json::array({"clip", "source", "start", "end", "inBar"})}, {"placed", std::move(placed)}};
    out.warnings = std::move(warnings);
    return out;
}

} // namespace

void registerRhythmCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"sequence.setBeatGrid", 1, "Set Beat Grid",
                "Sets the sequence's beat grid, which timeline snapping, edit.snapToBeats and edit.cutToBeats use: a "
                "tempo and the time of a downbeat, a tempo from tapped beats (at least two times), or detected beats; "
                "\"clear\" removes it. analysis.detectBeats sets it from the music.",
                Capability::EditSequence,
                withSequence({{"bpm", schema::number("Tempo in beats per minute.", 1.0, 1000.0)},
                              {"phase", timeParam("A downbeat (beat 1 of a bar). Default 0.")},
                              {"beatsPerBar", schema::integer("Default 4.", 1, 32)},
                              {"taps", schema::array(timeParam("A tapped beat."), "Tapped beats: tempo and phase from them.", 2)},
                              {"beats", schema::array(timeParam("A beat."), "Detected beats (music that drifts).")},
                              {"clear", schema::boolean("Remove the grid.")}}),
                Json{{"bpm", 120}, {"phase", "00:00:00:12"}}, {}, setBeatGridCommand});
    addBuiltin(registry,
               {"sequence.beats", 1, "List Beats",
                "The beats of the sequence's beat grid as arrays in beatFields order: time, place in the bar (1 = "
                "downbeat) and index from the grid's downbeat.",
                Capability::Read,
                withSequence({{"from", timeParam("Only beats at or after this time.")},
                              {"to", timeParam("Only beats before this time. Default: the end of the sequence.")},
                              {"limit", schema::integer("Most beats to return (default 500).", 1, 100000)}}),
                Json{{"from", "00:00:00:00"}, {"to", "00:00:10:00"}}, {}, listBeats});
    addBuiltin(registry,
               {"edit.snapToBeats", 1, "Snap to Beats",
                "Trims each clip's head and/or tail to the nearest beat of the beat grid when it is within the tolerance "
                "(linked partners follow). Edges that cannot reach their beat (a neighbouring clip, too little media) "
                "are left and reported.",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to snap.")},
                              {"tolerance", timeParam("Largest move. Default: a quarter beat.")},
                              {"edges", schema::enumeration({"both", "head", "tail"}, "Which edges. Default both.")},
                              {"linked", linkedParam(true)}},
                             {"clips"}),
                Json{{"clips", Json::array({42, 43})}, {"tolerance", "00:00:00:06"}}, {}, snapToBeats});
    addBuiltin(registry,
               {"edit.cutToBeats", 1, "Cut to Beats",
                "Music-video assembly: places the given clips' media (from each clip's source In) and media items "
                "(from their start) one after another, each lasting beatsPerCut beats of the beat grid, starting at "
                "the first beat at or after \"at\" (the playhead). Overwrites what is there; video goes to "
                "videoTrack, audio only when audioTrack is given (the music usually stays alone).",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips whose media to use, in order.", 0)},
                              {"media", schema::array(mediaParam("A media item."), "Media items to use, after the clips.")},
                              {"at", timeParam("Start at the first beat at or after this time (the playhead).")},
                              {"beatsPerCut", schema::integer("Beats per clip. Default 4 (a bar).", 1, 64)},
                              {"videoTrack", trackParam("Video destination. Default V1.")},
                              {"audioTrack", trackParam("Audio destination; none by default.")}},
                             {"at"}),
                Json{{"media", Json::array({"Shot 1", "Shot 2", "Shot 3"})}, {"at", "00:00:00:00"}, {"beatsPerCut", 2}},
                {}, cutToBeats});
}

} // namespace wabcut::command::detail
