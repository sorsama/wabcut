// Nested sequences and multicam (C.8): queries, validation helpers and the operations of Nesting.h.

#include <wabcut/model/Nesting.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace wabcut {

namespace {

constexpr std::array<std::pair<MulticamSync, std::string_view>, 6> kSyncNames{{
    {MulticamSync::Timecode, "timecode"},
    {MulticamSync::InPoints, "inPoints"},
    {MulticamSync::OutPoints, "outPoints"},
    {MulticamSync::Markers, "markers"},
    {MulticamSync::Audio, "audio"},
    {MulticamSync::Manual, "manual"},
}};

/// Bound on an angle's name (project files are untrusted).
constexpr std::size_t kMaxAngleNameBytes = 256;

constexpr TrackKind trackKindOf(StreamKind kind) { return kind == StreamKind::Video ? TrackKind::Video : TrackKind::Audio; }

std::string sequenceName(const ProjectState& state, SequenceId id) {
    const Sequence* s = state.findSequence(id);
    return s ? "'" + s->name + "'" : "sequence " + std::to_string(id.value);
}

} // namespace

std::string_view toString(MulticamSync sync) {
    for (const auto& [value, name] : kSyncNames) {
        if (value == sync) {
            return name;
        }
    }
    return "timecode";
}

std::optional<MulticamSync> multicamSyncFromString(std::string_view name) {
    for (const auto& [value, text] : kSyncNames) {
        if (text == name) {
            return value;
        }
    }
    return std::nullopt;
}

bool sequenceHasStream(const Sequence& sequence, StreamRef stream) {
    if (stream.index != 0) {
        return false;
    }
    if (sequence.multicam) {
        return std::any_of(sequence.multicam->angles.begin(), sequence.multicam->angles.end(), [&](const MulticamAngle& a) {
            return stream.kind == StreamKind::Video ? a.video.isValid() : !a.audio.empty();
        });
    }
    return !sequence.tracks(trackKindOf(stream.kind)).empty();
}

std::optional<Ticks> clipSourceExtent(const ProjectState& state, const Clip& clip) {
    if (clip.isNested()) {
        const Sequence* nested = state.findSequence(clip.sequence);
        return nested ? std::optional<Ticks>(nested->duration) : std::nullopt;
    }
    if (clip.hasUnknownSource()) {
        return std::nullopt;
    }
    const MediaItem* media = state.findMedia(clip.media);
    return media ? media->sourceExtent(clip.stream) : std::nullopt;
}

std::vector<SequenceId> nestedSequencesOf(const Sequence& sequence) {
    std::vector<SequenceId> out;
    sequence.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.isNested()) {
                out.push_back(c.sequence);
            }
        }
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool sequenceContains(const ProjectState& state, SequenceId outer, SequenceId inner) {
    std::vector<SequenceId> stack{outer};
    std::unordered_set<std::uint64_t> seen;
    while (!stack.empty()) {
        const SequenceId id = stack.back();
        stack.pop_back();
        if (id == inner) {
            return true;
        }
        if (!seen.insert(id.value).second) {
            continue;
        }
        if (const Sequence* s = state.findSequence(id)) {
            for (const SequenceId next : nestedSequencesOf(*s)) {
                stack.push_back(next);
            }
        }
    }
    return false;
}

std::optional<std::string> nestingCycle(const ProjectState& state) {
    std::unordered_map<std::uint64_t, std::vector<SequenceId>> edges;
    for (const SequencePtr& s : state.sequences) {
        if (s) {
            edges[s->id.value] = nestedSequencesOf(*s);
        }
    }
    enum class Mark : std::uint8_t { New, Open, Done };
    std::unordered_map<std::uint64_t, Mark> marks;
    for (const SequencePtr& s : state.sequences) {
        if (!s || marks[s->id.value] != Mark::New) {
            continue;
        }
        // Depth-first, iteratively (nesting depth is untrusted): the path holds each open sequence and its next edge.
        std::vector<std::pair<SequenceId, std::size_t>> path{{s->id, 0}};
        marks[s->id.value] = Mark::Open;
        while (!path.empty()) {
            const SequenceId id = path.back().first;
            const std::vector<SequenceId>& out = edges.at(id.value);
            if (path.back().second == out.size()) {
                marks[id.value] = Mark::Done;
                path.pop_back();
                continue;
            }
            const SequenceId to = out[path.back().second++];
            if (!edges.contains(to.value)) {
                continue; // a missing sequence: reported elsewhere
            }
            const Mark mark = marks[to.value];
            if (mark == Mark::Open) {
                std::string text;
                bool inCycle = false;
                for (const auto& [step, next] : path) {
                    inCycle = inCycle || step == to;
                    if (inCycle) {
                        text += sequenceName(state, step) + " -> ";
                    }
                }
                return text + sequenceName(state, to);
            }
            if (mark == Mark::New) {
                marks[to.value] = Mark::Open;
                path.emplace_back(to, 0);
            }
        }
    }
    return std::nullopt;
}

namespace {

/// Nesting deeper than this is a cycle or a crafted file: counted as past any limit (the depth also bounds the
/// recursion below, since project files are untrusted).
constexpr std::size_t kMaxFanOutDepth = 128;

/// The nested clip instances `id` reaches (saturating at limit + 1).
std::uint64_t fanOut(const ProjectState& state, SequenceId id, std::uint64_t limit, std::size_t depth,
                     std::unordered_map<std::uint64_t, std::uint64_t>& memo) {
    if (const auto it = memo.find(id.value); it != memo.end()) {
        return it->second;
    }
    if (depth > kMaxFanOutDepth) {
        return limit + 1;
    }
    std::uint64_t total = 0;
    if (const Sequence* s = state.findSequence(id)) {
        s->forEachTrack([&](const Track& track) {
            for (const Clip& clip : track.clips) {
                if (total > limit || !clip.isNested()) {
                    continue;
                }
                total = std::min(limit + 1, total + 1 + fanOut(state, clip.sequence, limit, depth + 1, memo));
            }
        });
    }
    memo[id.value] = total;
    return total;
}

} // namespace

std::optional<std::string> nestingFanOutProblem(const ProjectState& state, std::uint64_t limit) {
    std::unordered_map<std::uint64_t, std::uint64_t> memo;
    for (const SequencePtr& s : state.sequences) {
        if (s && fanOut(state, s->id, limit, 0, memo) > limit) {
            return "sequence '" + sequenceName(state, s->id) + "' reaches more than " + std::to_string(limit) +
                   " nested clips through its nested sequences";
        }
    }
    return std::nullopt;
}

std::optional<std::string> multicamProblem(const Sequence& sequence) {
    if (!sequence.multicam) {
        return std::nullopt;
    }
    const MulticamInfo& m = *sequence.multicam;
    if (m.angles.empty()) {
        return "has no angles";
    }
    if (m.angles.size() > kMaxMulticamAngles) {
        return "has more than " + std::to_string(kMaxMulticamAngles) + " angles";
    }
    std::unordered_set<std::uint64_t> used;
    for (std::size_t i = 0; i < m.angles.size(); ++i) {
        const MulticamAngle& a = m.angles[i];
        const std::string what = "angle " + std::to_string(i + 1);
        if (a.name.size() > kMaxAngleNameBytes) {
            return what + " has a name longer than " + std::to_string(kMaxAngleNameBytes) + " bytes";
        }
        if (!a.video.isValid() && a.audio.empty()) {
            return what + " has no tracks";
        }
        if (a.video.isValid()) {
            const Track* t = sequence.findTrack(a.video);
            if (!t || t->kind != TrackKind::Video) {
                return what + " names a video track the sequence does not have";
            }
            if (!used.insert(a.video.value).second) {
                return what + " shares a track with another angle";
            }
        }
        for (const TrackId id : a.audio) {
            const Track* t = sequence.findTrack(id);
            if (!t || t->kind != TrackKind::Audio) {
                return what + " names an audio track the sequence does not have";
            }
            if (!used.insert(id.value).second) {
                return what + " shares a track with another angle";
            }
        }
        if (a.offset.value < 0 || a.offset > kMaxMulticamOffset) {
            return what + " has a sync offset outside 0 to 24 hours";
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> multicamAngleOfTrack(const MulticamInfo& multicam, TrackId track) {
    for (std::size_t i = 0; i < multicam.angles.size(); ++i) {
        const MulticamAngle& a = multicam.angles[i];
        if (a.video == track || std::find(a.audio.begin(), a.audio.end(), track) != a.audio.end()) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::string> clipAnglesProblem(const ProjectState& state, const Clip& clip) {
    const Sequence* source = clip.isNested() ? state.findSequence(clip.sequence) : nullptr;
    if (!source || !source->multicam) {
        if (!clip.angles.empty()) {
            return std::string("has multicam angles but does not play a multicam source");
        }
        return std::nullopt;
    }
    const std::vector<MulticamAngle>& angles = source->multicam->angles;
    if (clip.angles.empty()) {
        return std::string("is a multicam clip without an active angle");
    }
    if (clip.stream.kind == StreamKind::Video && clip.angles.count() != 1) {
        return std::string("is a multicam video clip with more than one active angle");
    }
    for (std::size_t i = 0; i < kMaxMulticamAngles; ++i) {
        if (!clip.angles.contains(i)) {
            continue;
        }
        if (i >= angles.size()) {
            return "plays angle " + std::to_string(i + 1) + ", which its multicam source does not have";
        }
        if (clip.stream.kind == StreamKind::Video ? !angles[i].video.isValid() : angles[i].audio.empty()) {
            return "plays angle " + std::to_string(i + 1) + ", which has no " +
                   (clip.stream.kind == StreamKind::Video ? "picture" : "sound");
        }
    }
    return std::nullopt;
}

std::expected<std::vector<Ticks>, Error> OffsetTableAudioSync::offsets(const ProjectState&,
                                                                       std::span<const MediaId> angles) const {
    if (m_table.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "no audio match between the angles"});
    }
    const auto offsetOf = [&](MediaId media) {
        const auto it = std::find_if(m_table.begin(), m_table.end(), [&](const auto& e) { return e.first == media; });
        return it == m_table.end() ? Ticks{} : it->second;
    };
    std::vector<Ticks> out;
    const Ticks first = angles.empty() ? Ticks{} : offsetOf(angles.front());
    for (const MediaId media : angles) {
        out.push_back(offsetOf(media) - first);
    }
    return out;
}

namespace edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;
using detail::trackLabel;

namespace {

/// "Nested Sequence 01", "02", ... (Premiere's names): the first one no sequence has.
std::string newNestedName(const ProjectState& state, const std::string& base) {
    for (int n = 1;; ++n) {
        char text[64];
        std::snprintf(text, sizeof text, "%s %02d", base.c_str(), n);
        if (std::none_of(state.sequences.begin(), state.sequences.end(),
                         [&](const SequencePtr& s) { return s->name == text; })) {
            return text;
        }
    }
}

/// Adds a sequence with a new (the highest) id: the list stays sorted.
void appendSequence(ProjectState& state, std::shared_ptr<Sequence> sequence) {
    state.sequences.push_back(std::move(sequence));
}

std::shared_ptr<Track> newTrack(ProjectState& state, TrackKind kind, std::string name, bool targeted) {
    auto t = std::make_shared<Track>();
    t->id = state.ids.allocate<TrackId>();
    t->kind = kind;
    t->name = std::move(name);
    t->targeted = targeted;
    return t;
}

/// Why `track` would not play its clips exactly as plain clips on another track would (its own processing), or
/// nullopt.
std::optional<std::string> processingProblem(const Track& track, const std::string& label) {
    if (!track.effects.empty()) {
        return label + " has track effects";
    }
    if (!(track.volume == ParamValue()) || !(track.pan == ParamValue())) {
        return label + " has a fader or pan setting";
    }
    if (!track.enabled || track.muted || track.solo) {
        return label + " is turned off, muted or soloed";
    }
    return std::nullopt;
}

/// Why `clip` cannot be replaced by the clips it plays exactly, or nullopt.
std::optional<std::string> clipProcessingProblem(const Clip& clip) {
    if (!clip.speed.isUnity() || clip.reversed || clip.isRemapped()) {
        return std::string("it is retimed, reversed or time-remapped");
    }
    if (!clip.effects().empty()) {
        return std::string("it has effects, which apply to what it plays as a whole");
    }
    if (!(clip.properties() == ClipProperties{})) {
        return std::string("its Motion, Opacity or Volume is not the default");
    }
    return std::nullopt;
}

/// One track of a nested sequence (or angle) whose clips go back inline onto `dest`.
struct Layer {
    const Track* source = nullptr;
    TrackId dest;
};

/// Copies the clips of `layers` that lie inside `window` (nested time) onto their destinations, from timeline time
/// `at`: trimmed to the window, with new ids and new link groups (`groups` maps a nested group to its new group
/// across the whole edit), and the transitions between copied clips that lie inside the window. Refuses a
/// transition across the window's edge (the copy would not look the same).
std::expected<void, Error> copyLayers(SequenceEditor& ed, std::span<const Layer> layers, TimeRange window, Ticks at,
                                      std::unordered_map<std::uint64_t, LinkGroupId>& groups) {
    for (const Layer& layer : layers) {
        std::unordered_map<std::uint64_t, ClipId> copied;
        for (const Clip& c : layer.source->clips) {
            if (c.end() <= window.start || c.start() >= window.end()) {
                continue;
            }
            Clip copy = c;
            if (copy.start() < window.start) {
                copy.moveHead(window.start - copy.start());
            }
            if (copy.end() > window.end()) {
                copy.moveTail(window.end() - copy.end());
            }
            copy.timelineRange.start = at + (copy.start() - window.start);
            copy.id = ed.newClipId();
            copied.emplace(c.id.value, copy.id);
            if (c.linkGroup.isValid()) {
                LinkGroupId& group = groups[c.linkGroup.value];
                if (!group.isValid()) {
                    group = ed.newLinkGroup();
                }
                copy.linkGroup = group;
                ed.touchGroup(group);
            }
            ed.placeNewClip(layer.dest, std::move(copy), false);
        }
        for (const Transition& t : layer.source->transitions) {
            const auto cut = transitionEditPoint(*layer.source, t);
            if (!cut) {
                continue;
            }
            const TimeRange region = TimeRange::fromStartEnd(*cut - t.before, *cut + t.after);
            if (region.end() <= window.start || region.start >= window.end()) {
                continue; // outside what the clip shows
            }
            if (region.start < window.start || region.end() > window.end()) {
                return std::unexpected(invalidArgument("a transition crosses the edge of what the clip shows"));
            }
            const auto copyOf = [&](ClipId id) -> std::optional<ClipId> {
                if (!id.isValid()) {
                    return ClipId{};
                }
                const auto it = copied.find(id.value);
                return it == copied.end() ? std::nullopt : std::optional<ClipId>(it->second);
            };
            const auto outgoing = copyOf(t.outgoing);
            const auto incoming = copyOf(t.incoming);
            if (!outgoing || !incoming) {
                return std::unexpected(invalidArgument("a transition crosses the edge of what the clip shows"));
            }
            Transition copy = t;
            copy.id = ed.newTransitionId();
            copy.outgoing = *outgoing;
            copy.incoming = *incoming;
            ed.mutableTrack(layer.dest).transitions.push_back(std::move(copy));
        }
    }
    return {};
}

/// The destination tracks for `count` source layers from `clipTrack` (the clip's own track first): video upwards,
/// audio downwards; each unlocked, processing like `clipTrack` does (none) and free over `range` except for `self`.
std::expected<std::vector<TrackId>, Error> destinations(const Sequence& seq, TrackId clipTrack, std::size_t count,
                                                        TimeRange range, ClipId self) {
    const auto pos = seq.trackPosition(clipTrack);
    const std::vector<TrackPtr>& tracks = seq.tracks(pos->kind);
    std::vector<TrackId> out;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t index = pos->index + k;
        if (index >= tracks.size()) {
            return std::unexpected(invalidArgument(
                "it needs " + std::string(pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1) +
                ", which this sequence does not have; add tracks first"));
        }
        const Track& t = *tracks[index];
        const std::string label = trackLabel(seq, t.id);
        if (t.locked) {
            return std::unexpected(invalidArgument("track " + label + " is locked"));
        }
        if (const auto problem = processingProblem(t, "track " + label)) {
            return std::unexpected(invalidArgument(*problem));
        }
        for (std::size_t i = t.firstClipAtOrAfter(range.start) > 0 ? t.firstClipAtOrAfter(range.start) - 1 : 0;
             i < t.clips.size() && t.clips[i].start() < range.end(); ++i) {
            const Clip& c = t.clips[i];
            if (c.id != self && c.end() > range.start) {
                return std::unexpected(invalidArgument("clip '" + c.name() + "' on " + label + " is in the way"));
            }
        }
        out.push_back(t.id);
    }
    return out;
}

/// The clip and its linked partners that play the same sequence over the same time (unnest, flatten: the picture and
/// sound of one nested or multicam clip), clip first.
std::vector<ClipRef> sameSourcePartners(const Sequence& seq, const Clip& clip) {
    std::vector<ClipRef> out;
    seq.forEachTrack([&](const Track& t) {
        std::size_t index = 0;
        for (const Clip& c : t.clips) {
            if (c.id == clip.id || (clip.linkGroup.isValid() && c.linkGroup == clip.linkGroup &&
                                    c.sequence == clip.sequence && c.timelineRange == clip.timelineRange)) {
                out.push_back({t.id, c.id, index});
            }
            ++index;
        }
    });
    std::stable_partition(out.begin(), out.end(), [&](const ClipRef& r) { return r.clip == clip.id; });
    return out;
}

struct InlinePlan {
    ClipRef clip;
    TimeRange window;
    Ticks at;
    std::vector<Layer> layers;
};

/// Replaces each planned clip by its layers' clips (unnest, flatten).
EditOutcome inlineClips(const ProjectState& state, const Sequence& seq, std::vector<InlinePlan> plans) {
    SequenceEditor ed(state, seq);
    std::vector<ClipRef> refs;
    for (const InlinePlan& p : plans) {
        refs.push_back(p.clip);
    }
    // removeClips wants track order, then ascending indices.
    std::sort(refs.begin(), refs.end(), [&](const ClipRef& a, const ClipRef& b) {
        const auto pa = seq.trackPosition(a.track);
        const auto pb = seq.trackPosition(b.track);
        return std::tuple(pa->kind, pa->index, a.index) < std::tuple(pb->kind, pb->index, b.index);
    });
    ed.removeClips(refs);
    std::unordered_map<std::uint64_t, LinkGroupId> groups;
    for (const InlinePlan& p : plans) {
        if (const auto copied = copyLayers(ed, p.layers, p.window, p.at, groups); !copied) {
            return std::unexpected(copied.error());
        }
    }
    return ed.finish();
}

std::expected<const Sequence*, Error> multicamOf(const ProjectState& state, const Clip& clip) {
    const Sequence* source = clip.isNested() ? state.findSequence(clip.sequence) : nullptr;
    if (!source || !source->multicam) {
        return std::unexpected(invalidArgument("clip '" + clip.name() + "' is not a multicam clip"));
    }
    return source;
}

} // namespace

// --- Nest ------------------------------------------------------------------------------------------------

std::expected<NestResult, Error> nestClips(const ProjectState& state, SequenceId sequenceId, std::span<const ClipId> clips,
                                           std::string name, LinkMode links) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (clips.empty()) {
        return std::unexpected(invalidArgument("select the clips to nest"));
    }
    const auto refs = detail::resolveClips(**seq, clips, links);
    if (!refs) {
        return std::unexpected(refs.error());
    }
    const Sequence& parent = **seq;
    Ticks from = Ticks::max();
    Ticks to = Ticks::min();
    std::unordered_set<std::uint64_t> selected;
    std::unordered_map<std::uint64_t, int> selectedInGroup;
    std::optional<std::size_t> lowest[2];
    std::size_t highest[2] = {0, 0};
    for (const ClipRef& r : *refs) {
        const Clip& c = parent.findTrack(r.track)->clips[r.index];
        from = std::min(from, c.start());
        to = std::max(to, c.end());
        selected.insert(c.id.value);
        if (c.linkGroup.isValid()) {
            ++selectedInGroup[c.linkGroup.value];
        }
        const auto pos = parent.trackPosition(r.track);
        const auto k = static_cast<std::size_t>(pos->kind);
        lowest[k] = lowest[k] ? std::min(*lowest[k], pos->index) : pos->index;
        highest[k] = std::max(highest[k], pos->index);
    }
    // The nested clips go to the lowest selected track of each kind: free over the span but for the selection.
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto k = static_cast<std::size_t>(kind);
        if (!lowest[k]) {
            continue;
        }
        const Track& dest = *parent.tracks(kind)[*lowest[k]];
        for (const Clip& c : dest.clips) {
            if (c.start() < to && c.end() > from && !selected.contains(c.id.value)) {
                return std::unexpected(invalidArgument("clip '" + c.name() + "' on " + trackLabel(parent, dest.id) +
                                                       " lies inside the nest's span and is not selected"));
            }
        }
    }

    ProjectState next = state;
    auto nested = std::make_shared<Sequence>();
    nested->id = next.ids.allocate<SequenceId>();
    nested->bin = parent.bin;
    nested->name = name.empty() ? newNestedName(state, "Nested Sequence") : std::move(name);
    nested->frameRate = parent.frameRate;
    nested->width = parent.width;
    nested->height = parent.height;
    nested->audioRate = parent.audioRate;
    nested->dropFrame = parent.dropFrame;
    std::vector<std::shared_ptr<Track>> tracks[2];
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto k = static_cast<std::size_t>(kind);
        for (std::size_t i = 0; lowest[k] && i <= highest[k]; ++i) {
            const Track& like = *parent.tracks(kind)[i];
            auto t = newTrack(next, kind, like.name, i == 0);
            t->role = like.role;
            tracks[k].push_back(std::move(t));
        }
    }
    // Links: a group wholly inside keeps its id (it leaves the parent); part of a group gets a new one; a lone member none.
    std::unordered_map<std::uint64_t, int> groupSize;
    parent.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.linkGroup.isValid() && selectedInGroup.contains(c.linkGroup.value)) {
                ++groupSize[c.linkGroup.value];
            }
        }
    });
    std::unordered_map<std::uint64_t, LinkGroupId> groupMap;
    for (const auto& [group, count] : selectedInGroup) {
        if (count == groupSize[group]) {
            groupMap[group] = LinkGroupId{group};
        } else if (count >= 2) {
            groupMap[group] = next.ids.allocate<LinkGroupId>();
        } else {
            groupMap[group] = LinkGroupId{};
        }
    }
    for (const ClipRef& r : *refs) {
        const Track& source = *parent.findTrack(r.track);
        Clip c = source.clips[r.index];
        c.timelineRange.start = c.start() - from;
        if (c.linkGroup.isValid()) {
            c.linkGroup = groupMap.at(c.linkGroup.value);
        }
        const auto pos = parent.trackPosition(r.track);
        tracks[static_cast<std::size_t>(pos->kind)][pos->index]->clips.push_back(std::move(c));
    }
    // Transitions between nested clips (or at a nested clip's head or tail) move with them.
    parent.forEachTrack([&](const Track& t) {
        const auto pos = parent.trackPosition(t.id);
        const auto k = static_cast<std::size_t>(pos->kind);
        for (const Transition& tr : t.transitions) {
            const bool inside = (!tr.outgoing.isValid() || selected.contains(tr.outgoing.value)) &&
                                (!tr.incoming.isValid() || selected.contains(tr.incoming.value));
            if (inside && pos->index < tracks[k].size()) {
                tracks[k][pos->index]->transitions.push_back(tr);
            }
        }
    });
    for (auto& t : tracks[0]) {
        nested->videoTracks.push_back(std::move(t));
    }
    for (auto& t : tracks[1]) {
        nested->audioTracks.push_back(std::move(t));
    }
    nested->duration = to - from;
    const SequenceId nestedId = nested->id;
    const std::string nestedName = nested->name;
    appendSequence(next, std::move(nested));

    // The parent: the selection out, the nested clips in (linked when there are both).
    const Sequence& parentNow = *next.findSequence(sequenceId);
    SequenceEditor ed(next, parentNow);
    ed.removeClips(*refs);
    const LinkGroupId group = lowest[0] && lowest[1] ? ed.newLinkGroup() : LinkGroupId{};
    ClipContent content;
    content.name = nestedName;
    const auto shared = std::make_shared<const ClipContent>(std::move(content));
    NestResult out;
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto k = static_cast<std::size_t>(kind);
        if (!lowest[k]) {
            continue;
        }
        Clip c;
        c.id = ed.newClipId();
        c.sequence = nestedId;
        c.stream = {streamKindOf(kind), 0};
        c.timelineRange = TimeRange::fromStartEnd(from, to);
        c.linkGroup = group;
        c.setContent(shared);
        out.clips.push_back(c.id);
        ed.placeNewClip(parentNow.tracks(kind)[*lowest[k]]->id, std::move(c), false);
    }
    out.edit = ed.finish();
    out.edit.changes.addedSequences.push_back(nestedId);
    out.nested = nestedId;
    return out;
}

// --- Unnest ------------------------------------------------------------------------------------------------

EditOutcome unnestClip(const ProjectState& state, SequenceId sequenceId, ClipId clipId) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto loc = (*seq)->findClip(clipId);
    if (!loc) {
        return std::unexpected(notFound("clip " + std::to_string(clipId.value) + " is not in this sequence"));
    }
    const Clip& clip = loc->clip();
    if (!clip.isNested()) {
        return std::unexpected(invalidArgument("clip '" + clip.name() + "' is not a nested sequence"));
    }
    const Sequence* nested = state.findSequence(clip.sequence);
    if (!nested) {
        return std::unexpected(notFound("the clip's sequence does not exist"));
    }
    if (nested->multicam) {
        return std::unexpected(invalidArgument("clip '" + clip.name() + "' is a multicam clip: flatten it instead"));
    }
    const auto refuse = [&](const std::string& why) {
        return std::unexpected(invalidArgument("cannot put '" + clip.name() + "' back inline exactly: " + why));
    };
    std::vector<InlinePlan> plans;
    for (const ClipRef& r : sameSourcePartners(**seq, clip)) {
        const Track& track = *(*seq)->findTrack(r.track);
        const Clip& c = track.clips[r.index];
        if (track.locked) {
            return refuse("track " + trackLabel(**seq, track.id) + " is locked");
        }
        if (const auto problem = clipProcessingProblem(c)) {
            return refuse(*problem);
        }
        const TrackKind kind = track.kind;
        if (kind == TrackKind::Video ? !nested->effects.empty()
                                     : !nested->audioEffects.empty() || !(nested->masterVolume == ParamValue())) {
            return refuse("the nested sequence has master effects or a master fader setting");
        }
        const TimeRange window{c.sourceIn, c.duration()};
        const std::vector<TrackPtr>& sources = nested->tracks(kind);
        // Nested track i goes i tracks from the clip's own (video up, audio down); only tracks with clips in the window.
        std::size_t count = 0;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const Track& s = *sources[i];
            const bool used = std::any_of(s.clips.begin(), s.clips.end(), [&](const Clip& x) {
                return x.start() < window.end() && x.end() > window.start;
            });
            if (used) {
                if (const auto problem = processingProblem(s, "nested track " + trackLabel(*nested, s.id))) {
                    return refuse(*problem);
                }
                count = i + 1;
            }
        }
        if (const auto problem = processingProblem(track, "track " + trackLabel(**seq, track.id))) {
            return refuse(*problem);
        }
        const auto dest = destinations(**seq, track.id, count, c.timelineRange, c.id);
        if (!dest) {
            return refuse(dest.error().message);
        }
        InlinePlan plan{r, window, c.start(), {}};
        for (std::size_t i = 0; i < count; ++i) {
            plan.layers.push_back({sources[i].get(), (*dest)[i]});
        }
        plans.push_back(std::move(plan));
    }
    auto result = inlineClips(state, **seq, std::move(plans));
    if (!result) {
        return refuse(result.error().message);
    }
    return result;
}

// --- Multicam ------------------------------------------------------------------------------------------------

std::expected<Created<SequenceId>, Error> createMulticam(const ProjectState& state, const MulticamSpec& spec) {
    if (spec.angles.empty()) {
        return std::unexpected(invalidArgument("a multicam source needs at least one angle"));
    }
    if (spec.angles.size() > kMaxMulticamAngles) {
        return std::unexpected(invalidArgument("a multicam source has at most " + std::to_string(kMaxMulticamAngles) +
                                               " angles"));
    }
    std::vector<const MediaItem*> media;
    for (const MediaId id : spec.angles) {
        const MediaItem* m = state.findMedia(id);
        if (!m) {
            return std::unexpected(notFound("media " + std::to_string(id.value) + " does not exist"));
        }
        if (!m->info.hasVideo() && !m->info.hasAudio()) {
            return std::unexpected(invalidArgument("'" + m->name + "' has neither picture nor sound"));
        }
        media.push_back(m);
    }
    // Where each angle's media starts: aligned so the chosen sync points meet, the earliest angle at 0.
    std::vector<Ticks> starts(media.size());
    const auto alignTo = [&](const std::vector<Ticks>& points) {
        const Ticks latest = *std::max_element(points.begin(), points.end());
        for (std::size_t i = 0; i < points.size(); ++i) {
            starts[i] = latest - points[i];
        }
    };
    const auto lengthOf = [](const MediaItem& m) -> std::optional<Ticks> { return sourceMarkExtent(m); };
    std::vector<Ticks> points;
    switch (spec.sync) {
    case MulticamSync::Timecode:
        for (const MediaItem* m : media) {
            if (!m->info.startTimecode) {
                return std::unexpected(invalidArgument("'" + m->name + "' has no timecode to sync by"));
            }
            points.push_back(Ticks{} - *m->info.startTimecode); // later timecode = later start
        }
        alignTo(points);
        break;
    case MulticamSync::InPoints:
        for (const MediaItem* m : media) {
            points.push_back(m->markIn.value_or(Ticks{}));
        }
        alignTo(points);
        break;
    case MulticamSync::OutPoints:
        for (const MediaItem* m : media) {
            const std::optional<Ticks> out = m->markOut ? m->markOut : lengthOf(*m);
            if (!out) {
                return std::unexpected(invalidArgument("'" + m->name + "' has no Out mark and no known length"));
            }
            points.push_back(*out);
        }
        alignTo(points);
        break;
    case MulticamSync::Markers:
        for (const MediaItem* m : media) {
            if (m->markers.empty()) {
                return std::unexpected(invalidArgument("'" + m->name + "' has no clip marker to sync by"));
            }
            points.push_back(m->markers.front().start);
        }
        alignTo(points);
        break;
    case MulticamSync::Audio: {
        if (!spec.audioSync) {
            return std::unexpected(invalidArgument("syncing by audio needs the audio analysis, which is not available here"));
        }
        const auto found = spec.audioSync->offsets(state, spec.angles, spec.audioSyncControl);
        if (!found) {
            return std::unexpected(found.error());
        }
        if (found->size() != media.size()) {
            return std::unexpected(invalidArgument("the audio analysis did not sync every angle"));
        }
        for (const Ticks t : *found) {
            points.push_back(Ticks{} - t); // a later start is a later offset
        }
        alignTo(points);
        break;
    }
    case MulticamSync::Manual:
        if (!spec.offsets.empty() && spec.offsets.size() != media.size()) {
            return std::unexpected(invalidArgument("give one offset per angle"));
        }
        for (std::size_t i = 0; i < media.size(); ++i) {
            starts[i] = spec.offsets.empty() ? Ticks{} : spec.offsets[i];
        }
        break;
    }
    for (std::size_t i = 0; i < starts.size(); ++i) {
        if (starts[i].value < 0 || starts[i] > kMaxMulticamOffset) {
            return std::unexpected(invalidArgument("angle " + std::to_string(i + 1) +
                                                   "'s sync offset is outside 0 to 24 hours"));
        }
    }

    Created<SequenceId> out{state, {}};
    ProjectState& next = out.state;
    auto seq = std::make_shared<Sequence>();
    seq->id = next.ids.allocate<SequenceId>();
    seq->bin = spec.bin.isValid() ? spec.bin : media.front()->bin;
    if (spec.bin.isValid() && !next.findBin(spec.bin)) {
        return std::unexpected(notFound("bin does not exist"));
    }
    seq->name = spec.name.empty() ? media.front()->name + " Multicam" : spec.name;
    // Settings from the first angle with a picture.
    for (const MediaItem* m : media) {
        if (m->info.hasVideo()) {
            const VideoStreamInfo& v = m->info.video.front();
            if (v.frameRate.isExact() && v.frameRate.num > 0) {
                seq->frameRate = v.frameRate;
            }
            if (v.width > 0 && v.height > 0) {
                seq->width = v.width;
                seq->height = v.height;
            }
            break;
        }
    }
    MulticamInfo multicam;
    multicam.sync = spec.sync;
    for (std::size_t i = 0; i < media.size(); ++i) {
        const MediaItem& m = *media[i];
        MulticamAngle angle;
        angle.name = i < spec.angleNames.size() && !spec.angleNames[i].empty() ? spec.angleNames[i] : m.name;
        angle.offset = starts[i];
        const auto content = m.defaultClipContent();
        std::vector<std::pair<std::shared_ptr<Track>, Clip>> placed;
        const auto place = [&](TrackKind kind, StreamRef stream) -> std::expected<void, Error> {
            const auto length = m.sourceExtent(stream);
            if (!length || length->value <= 0) {
                return std::unexpected(invalidArgument("'" + m.name + "' has no known length"));
            }
            auto track = newTrack(next, kind, angle.name, false);
            Clip c;
            c.id = next.ids.allocate<ClipId>();
            c.media = m.id;
            c.stream = stream;
            c.timelineRange = {starts[i], *length};
            c.setContent(content);
            placed.emplace_back(std::move(track), std::move(c));
            return {};
        };
        if (m.info.hasVideo()) {
            if (const auto ok = place(TrackKind::Video, {StreamKind::Video, 0}); !ok) {
                return std::unexpected(ok.error());
            }
        }
        for (std::size_t s = 0; s < m.info.audio.size(); ++s) {
            if (const auto ok = place(TrackKind::Audio, {StreamKind::Audio, static_cast<std::int32_t>(s)}); !ok) {
                return std::unexpected(ok.error());
            }
        }
        const LinkGroupId group = placed.size() > 1 ? next.ids.allocate<LinkGroupId>() : LinkGroupId{};
        for (auto& [track, clip] : placed) {
            clip.linkGroup = group;
            seq->duration = std::max(seq->duration, clip.end());
            if (track->kind == TrackKind::Video) {
                angle.video = track->id;
            } else {
                angle.audio.push_back(track->id);
            }
            track->clips.push_back(std::move(clip));
            std::vector<TrackPtr>& list = track->kind == TrackKind::Video ? seq->videoTracks : seq->audioTracks;
            track->targeted = list.empty(); // V1 and A1, as Premiere targets them
            list.push_back(std::move(track));
        }
        multicam.angles.push_back(std::move(angle));
    }
    seq->multicam = std::move(multicam);
    out.id = seq->id;
    appendSequence(next, std::move(seq));
    return out;
}

EditOutcome switchMulticamAngle(const ProjectState& state, SequenceId sequenceId, ClipId clipId, std::optional<Ticks> at,
                                std::size_t angle, AngleTarget target) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto loc = (*seq)->findClip(clipId);
    if (!loc) {
        return std::unexpected(notFound("clip " + std::to_string(clipId.value) + " is not in this sequence"));
    }
    const Clip& named = loc->clip();
    const auto source = multicamOf(state, named);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::vector<MulticamAngle>& angles = (*source)->multicam->angles;
    if (angle >= angles.size()) {
        return std::unexpected(invalidArgument("the multicam source has " + std::to_string(angles.size()) +
                                               " angles; there is no angle " + std::to_string(angle + 1)));
    }
    const Ticks when = at.value_or(named.start());
    if (at && (*at < named.start() || *at >= named.end())) {
        return std::unexpected(invalidArgument("the switch time lies outside the clip"));
    }
    // The clips that switch: the named one and its linked multicam partners of the same source under `when`, of the
    // kinds asked for.
    std::vector<ClipRef> parts;
    (*seq)->forEachTrack([&](const Track& t) {
        std::size_t index = 0;
        for (const Clip& c : t.clips) {
            const bool partner = c.id == named.id ||
                                 (named.linkGroup.isValid() && c.linkGroup == named.linkGroup && c.sequence == named.sequence);
            const bool kind = target == AngleTarget::Both ||
                              (target == AngleTarget::Video) == (c.stream.kind == StreamKind::Video);
            if (partner && kind && c.start() <= when && when < c.end()) {
                parts.push_back({t.id, c.id, index});
            }
            ++index;
        }
    });
    if (parts.empty()) {
        return std::unexpected(invalidArgument(std::string("no multicam ") +
                                               (target == AngleTarget::Audio ? "audio" : "video") + " clip to switch there"));
    }
    SequenceEditor ed(state, **seq);
    for (const ClipRef& r : parts) {
        const Track& t = ed.track(r.track);
        if (t.locked) {
            return std::unexpected(invalidArgument("track " + trackLabel(**seq, r.track) + " is locked"));
        }
        const Clip& c = t.clips[r.index];
        const bool video = c.stream.kind == StreamKind::Video;
        if (video ? !angles[angle].video.isValid() : angles[angle].audio.empty()) {
            return std::unexpected(invalidArgument("angle " + std::to_string(angle + 1) + " has no " +
                                                   (video ? "picture" : "sound")));
        }
        std::size_t index = r.index;
        if (at && *at > c.start()) {
            if (target == AngleTarget::Both) {
                // Picture and sound cut together: the right pieces link to each other (a razor cut).
                const auto right = ed.splitAt(r.track, *at);
                if (!right) {
                    return std::unexpected(invalidArgument("cannot cut the clip there"));
                }
                index = *right;
            } else {
                // One side only: the right piece stays linked with the uncut other side (and the left piece).
                const ClipId left = c.id;
                Clip piece = ed.mutableClip(r.track, r.index).splitOff(*at);
                piece.id = ed.newClipId();
                const ClipId right = piece.id;
                ed.placeNewClip(r.track, std::move(piece), false);
                // A transition at the clip's Out point now belongs to the right piece, as in splitAt (E5-P2-1).
                for (Transition& tr : ed.mutableTrack(r.track).transitions) {
                    if (tr.outgoing == left) {
                        tr.outgoing = right;
                    }
                }
                index = r.index + 1;
            }
        }
        ed.mutableClip(r.track, index).angles = AngleSet::of(angle);
    }
    return ed.finish();
}

EditOutcome flattenMulticam(const ProjectState& state, SequenceId sequenceId, ClipId clipId) {
    const auto seq = detail::findSequence(state, sequenceId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto loc = (*seq)->findClip(clipId);
    if (!loc) {
        return std::unexpected(notFound("clip " + std::to_string(clipId.value) + " is not in this sequence"));
    }
    const Clip& clip = loc->clip();
    const auto source = multicamOf(state, clip);
    if (!source) {
        return std::unexpected(source.error());
    }
    const auto refuse = [&](const std::string& why) {
        return std::unexpected(invalidArgument("cannot flatten '" + clip.name() + "' exactly: " + why));
    };
    const Sequence& multicam = **source;
    std::vector<InlinePlan> plans;
    for (const ClipRef& r : sameSourcePartners(**seq, clip)) {
        const Track& track = *(*seq)->findTrack(r.track);
        const Clip& c = track.clips[r.index];
        if (track.locked) {
            return refuse("track " + trackLabel(**seq, track.id) + " is locked");
        }
        if (const auto problem = clipProcessingProblem(c)) {
            return refuse(*problem);
        }
        if (const auto problem = processingProblem(track, "track " + trackLabel(**seq, track.id))) {
            return refuse(*problem);
        }
        // The angle tracks it plays, in angle order.
        std::vector<const Track*> sources;
        for (std::size_t i = 0; i < multicam.multicam->angles.size(); ++i) {
            if (!c.angles.contains(i)) {
                continue;
            }
            const MulticamAngle& a = multicam.multicam->angles[i];
            if (c.stream.kind == StreamKind::Video) {
                sources.push_back(multicam.findTrack(a.video));
            } else {
                for (const TrackId id : a.audio) {
                    sources.push_back(multicam.findTrack(id));
                }
            }
        }
        for (const Track* s : sources) {
            if (const auto problem = processingProblem(*s, "angle track " + trackLabel(multicam, s->id))) {
                return refuse(*problem);
            }
        }
        const auto dest = destinations(**seq, track.id, sources.size(), c.timelineRange, c.id);
        if (!dest) {
            return refuse(dest.error().message);
        }
        InlinePlan plan{r, TimeRange{c.sourceIn, c.duration()}, c.start(), {}};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            plan.layers.push_back({sources[i], (*dest)[i]});
        }
        plans.push_back(std::move(plan));
    }
    auto result = inlineClips(state, **seq, std::move(plans));
    if (!result) {
        return refuse(result.error().message);
    }
    return result;
}

EditOutcome setMulticamSync(const ProjectState& state, SequenceId multicamId, std::size_t angle, Ticks offset) {
    const auto seq = detail::findSequence(state, multicamId);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (!(*seq)->multicam) {
        return std::unexpected(invalidArgument("sequence '" + (*seq)->name + "' is not a multicam source"));
    }
    const std::vector<MulticamAngle>& angles = (*seq)->multicam->angles;
    if (angle >= angles.size()) {
        return std::unexpected(invalidArgument("the multicam source has " + std::to_string(angles.size()) +
                                               " angles; there is no angle " + std::to_string(angle + 1)));
    }
    if (offset.value < 0 || offset > kMaxMulticamOffset) {
        return std::unexpected(invalidArgument("a sync offset lies between 0 and 24 hours"));
    }
    const MulticamAngle& a = angles[angle];
    const Ticks delta = offset - a.offset;
    std::vector<TrackId> tracks = a.audio;
    if (a.video.isValid()) {
        tracks.insert(tracks.begin(), a.video);
    }
    SequenceEditor ed(state, **seq);
    for (const TrackId id : tracks) {
        const Track& t = ed.track(id);
        if (t.locked) {
            return std::unexpected(invalidArgument("track " + trackLabel(**seq, id) + " is locked"));
        }
        if (!t.clips.empty() && (t.clips.front().start() + delta).value < 0) {
            return std::unexpected(invalidArgument("the angle's clips would start before 0"));
        }
    }
    if (delta.value == 0 && (*seq)->multicam->sync == MulticamSync::Manual) {
        return detail::unchanged(state);
    }
    for (const TrackId id : tracks) {
        if (delta.value != 0 && !ed.track(id).clips.empty()) {
            ed.shiftFrom(id, ed.track(id).clips.front().start(), delta);
        }
    }
    MulticamInfo& info = *ed.header().multicam;
    info.angles[angle].offset = offset;
    info.sync = MulticamSync::Manual;
    return ed.finish();
}

} // namespace edit

} // namespace wabcut
