#include <wabcut/ai/edit/TextEdits.h>

#include <wabcut/command/Schema.h>

#include <algorithm>
#include <map>
#include <set>

namespace wabcut::ai::edit {

namespace {

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; }

Ticks floorFrame(Ticks t, Rate rate) { return snapToFrame(t, rate); }

Ticks ceilFrame(Ticks t, Rate rate) {
    const Ticks down = snapToFrame(t, rate);
    return down == t ? t : framesToTicks(ticksToFrames(t, rate) + 1, rate);
}

/// Which tracks each link group has clips on, and which tracks are locked.
struct TrackInfo {
    std::map<std::uint64_t, std::set<std::uint64_t>> groupTracks;
    std::set<std::uint64_t> locked;

    explicit TrackInfo(const Sequence& seq) {
        seq.forEachTrack([&](const Track& t) {
            if (t.locked) {
                locked.insert(t.id.value);
            }
            for (const Clip& c : t.clips) {
                if (c.linkGroup.isValid()) {
                    groupTracks[c.linkGroup.value].insert(t.id.value);
                }
            }
        });
    }

    /// The unlocked tracks words first..last are heard on, with their linked partners' tracks. Empty when a word
    /// is heard on a locked track: cutting only its partners would put the pair out of sync.
    std::vector<TrackId> tracksFor(const SequenceTranscript& st, std::size_t first, std::size_t last) const {
        std::set<std::uint64_t> ids;
        for (std::size_t i = first; i <= last; ++i) {
            if (locked.contains(st[i].track.value)) {
                return {};
            }
            ids.insert(st[i].track.value);
            if (st[i].linkGroup.isValid()) {
                if (const auto it = groupTracks.find(st[i].linkGroup.value); it != groupTracks.end()) {
                    ids.insert(it->second.begin(), it->second.end());
                }
            }
        }
        std::vector<TrackId> out;
        for (const std::uint64_t id : ids) {
            if (!locked.contains(id)) {
                out.push_back(TrackId{id});
            }
        }
        return out;
    }
};

/// A cut before merging: its range, tracks, and its span in "slot" space (word w = 2w+1, the pause after w =
/// 2w+2), so cuts with no kept word between them can be told apart from cuts that have one.
struct Proto {
    TimeRange range;
    std::vector<TrackId> tracks;
    std::size_t lo = 0;
    std::size_t hi = 0;
};

/// [start, end) of words first..last widened by the padding, never reaching into the neighbouring words,
/// on frame boundaries; nullopt when no frame fits.
std::optional<TimeRange> paddedRange(const SequenceTranscript& st, std::size_t first, std::size_t last,
                                     const CutOptions& o, Rate rate) {
    const TimelineWord& a = st[first];
    const TimelineWord& b = st[last];
    const Ticks lower = first > 0 ? std::min(st[first - 1].timeline.end(), a.timeline.start) : std::min(Ticks{}, a.timeline.start);
    const Ticks upper = last + 1 < st.size() ? std::max(st[last + 1].timeline.start, b.timeline.end()) : Ticks::max();
    const Ticks start = std::max(a.timeline.start - o.padBefore, lower);
    const Ticks end = std::min(b.timeline.end() + o.padAfter, upper);
    Ticks s = floorFrame(start, rate);
    if (s < lower) {
        s = ceilFrame(start, rate);
    }
    Ticks e = ceilFrame(end, rate);
    if (e > upper) {
        e = floorFrame(end, rate);
    }
    if (e <= s) {
        return std::nullopt;
    }
    return TimeRange::fromStartEnd(s, e);
}

std::vector<TrackId> unionOf(const std::vector<TrackId>& a, const std::vector<TrackId>& b) {
    std::vector<TrackId> out = a;
    out.insert(out.end(), b.begin(), b.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// Sorts cuts and merges those that overlap or touch, and those with no kept word between them that are at
/// most a second apart.
std::vector<PlannedCut> mergeCuts(std::vector<Proto> protos) {
    std::sort(protos.begin(), protos.end(), [](const Proto& a, const Proto& b) { return a.range.start < b.range.start; });
    const Ticks maxPause = Ticks::fromSeconds(1);
    std::vector<Proto> merged;
    for (Proto& p : protos) {
        if (!merged.empty()) {
            Proto& cur = merged.back();
            const bool overlaps = p.range.start <= cur.range.end();
            const bool adjacent = p.lo <= cur.hi + 1 || (p.lo == cur.hi + 2 && cur.hi % 2 == 1);
            if (overlaps || (adjacent && p.range.start - cur.range.end() <= maxPause)) {
                cur.range = TimeRange::fromStartEnd(cur.range.start, std::max(cur.range.end(), p.range.end()));
                cur.tracks = unionOf(cur.tracks, p.tracks);
                cur.lo = std::min(cur.lo, p.lo);
                cur.hi = std::max(cur.hi, p.hi);
                continue;
            }
        }
        merged.push_back(std::move(p));
    }
    std::vector<PlannedCut> out;
    for (Proto& p : merged) {
        out.push_back(PlannedCut{p.range, std::move(p.tracks)});
    }
    return out;
}

std::expected<const Sequence*, Error> sequenceOf(const SequenceTranscript& st, const ProjectState& state) {
    const Sequence* seq = state.findSequence(st.sequence());
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "the transcript's sequence does not exist"});
    }
    return seq;
}

std::string trackLabel(const Sequence& seq, TrackId id) {
    const auto pos = seq.trackPosition(id);
    return pos ? (pos->kind == TrackKind::Video ? "V" : "A") + std::to_string(pos->index + 1) : std::to_string(id.value);
}

command::Json trackLabels(const Sequence& seq, const std::vector<TrackId>& tracks) {
    command::Json out = command::Json::array();
    for (const TrackId id : tracks) {
        out.push_back(trackLabel(seq, id));
    }
    return out;
}

command::CommandRequest request(std::string name, command::Json params) {
    command::CommandRequest r;
    r.command = std::move(name);
    r.params = std::move(params);
    return r;
}

} // namespace

Ticks EditPlan::removed() const {
    Ticks total{};
    for (const PlannedCut& c : cuts) {
        total += c.range.duration;
    }
    return total;
}

std::expected<EditPlan, Error> planDelete(const SequenceTranscript& st, const ProjectState& state,
                                          const DeleteRequest& request) {
    const auto seq = sequenceOf(st, state);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Rate rate = (*seq)->frameRate;
    const TrackInfo info(**seq);
    std::vector<Proto> protos;
    std::vector<bool> removed(st.size(), false);
    for (const WordRange& r : request.words) {
        if (r.first > r.last || r.last >= st.size()) {
            return std::unexpected(invalid("word range " + std::to_string(r.first) + ".." + std::to_string(r.last) +
                                           " is not within the " + std::to_string(st.size()) + " words"));
        }
        const auto range = paddedRange(st, r.first, r.last, request.options, rate);
        if (!range) {
            continue;
        }
        auto tracks = info.tracksFor(st, r.first, r.last);
        if (tracks.empty()) {
            return std::unexpected(invalid("words " + std::to_string(r.first) + ".." + std::to_string(r.last) +
                                           " are heard on a locked track"));
        }
        for (std::size_t i = r.first; i <= r.last; ++i) {
            removed[i] = true;
        }
        protos.push_back(Proto{*range, std::move(tracks), 2 * r.first + 1, 2 * r.last + 1});
    }
    for (const std::size_t i : request.gapsAfter) {
        if (i >= st.size() || st.size() - i < 2) { // no `i + 1`: it wraps for i = SIZE_MAX
            return std::unexpected(invalid("there is no pause after word " + std::to_string(i)));
        }
        // Inward: keep room tone next to both words.
        const Ticks a = ceilFrame(st[i].timeline.end() + request.options.roomTone, rate);
        const Ticks b = floorFrame(st[i + 1].timeline.start - request.options.roomTone, rate);
        if (b <= a) {
            continue;
        }
        auto tracks = info.tracksFor(st, i, i + 1);
        if (tracks.empty()) {
            continue;
        }
        protos.push_back(Proto{TimeRange::fromStartEnd(a, b), std::move(tracks), 2 * i + 2, 2 * i + 2});
    }
    EditPlan plan;
    plan.sequence = st.sequence();
    plan.cuts = mergeCuts(std::move(protos));
    plan.words = static_cast<std::size_t>(std::count(removed.begin(), removed.end(), true));
    return plan;
}

std::expected<EditPlan, Error> planKeep(const SequenceTranscript& st, const ProjectState& state,
                                        std::span<const WordRange> keep, const CutOptions& options) {
    const auto seq = sequenceOf(st, state);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (keep.empty()) {
        return std::unexpected(invalid("nothing to keep"));
    }
    const auto scope = st.extent();
    if (!scope || st.empty()) {
        return std::unexpected(invalid("the sequence has no transcript"));
    }
    const Rate rate = (*seq)->frameRate;
    std::vector<TimeRange> keeps;
    std::vector<bool> kept(st.size(), false);
    for (const WordRange& r : keep) {
        if (r.first > r.last || r.last >= st.size()) {
            return std::unexpected(invalid("word range " + std::to_string(r.first) + ".." + std::to_string(r.last) +
                                           " is not within the " + std::to_string(st.size()) + " words"));
        }
        if (const auto range = paddedRange(st, r.first, r.last, options, rate)) {
            keeps.push_back(*range);
        }
        for (std::size_t i = r.first; i <= r.last; ++i) {
            kept[i] = true;
        }
    }
    std::sort(keeps.begin(), keeps.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    const TrackInfo info(**seq);
    const auto tracks = info.tracksFor(st, 0, st.size() - 1);
    if (tracks.empty()) {
        return std::unexpected(invalid("the transcript is heard on a locked track"));
    }
    EditPlan plan;
    plan.sequence = st.sequence();
    Ticks cursor = floorFrame(scope->start, rate);
    const auto cutTo = [&](Ticks end) {
        if (end > cursor) {
            plan.cuts.push_back(PlannedCut{TimeRange::fromStartEnd(cursor, end), tracks});
        }
    };
    for (const TimeRange& k : keeps) {
        cutTo(k.start);
        cursor = std::max(cursor, k.end());
    }
    cutTo(ceilFrame(scope->end(), rate));
    plan.words = static_cast<std::size_t>(std::count(kept.begin(), kept.end(), false));
    return plan;
}

std::expected<MovePlan, Error> planMove(const SequenceTranscript& st, const ProjectState& state, WordRange words,
                                        std::size_t before, const CutOptions& options) {
    const auto seq = sequenceOf(st, state);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if (words.first > words.last || words.last >= st.size() || before > st.size()) {
        return std::unexpected(invalid("the word range or the destination is not within the " +
                                       std::to_string(st.size()) + " words"));
    }
    if (before >= words.first && before <= words.last + 1) {
        return std::unexpected(invalid("the destination is inside or right after the moved words"));
    }
    const Rate rate = (*seq)->frameRate;
    const auto source = paddedRange(st, words.first, words.last, options, rate);
    if (!source) {
        return std::unexpected(invalid("the words are too short to move"));
    }
    Ticks destination;
    if (before == st.size()) {
        destination = ceilFrame(st[before - 1].timeline.end() + options.padAfter, rate);
    } else {
        const Ticks lower = before > 0 ? std::min(st[before - 1].timeline.end(), st[before].timeline.start) : Ticks{};
        const Ticks start = std::max(st[before].timeline.start - options.padBefore, lower);
        destination = floorFrame(start, rate);
        if (destination < lower) {
            destination = ceilFrame(start, rate);
        }
    }
    if (destination >= source->start && destination <= source->end()) {
        return std::unexpected(invalid("the destination lies inside the moved range"));
    }
    MovePlan plan;
    plan.sequence = st.sequence();
    plan.source = *source;
    plan.destination = destination;
    plan.tracks = TrackInfo(**seq).tracksFor(st, words.first, words.last);
    if (plan.tracks.empty()) {
        return std::unexpected(invalid("the words are heard on a locked track"));
    }
    return plan;
}

command::CommandBatch toCommandBatch(const EditPlan& plan, std::string label) {
    command::CommandBatch batch;
    batch.label = std::move(label);
    // Last cut first, so earlier cuts keep their positions.
    for (auto it = plan.cuts.rbegin(); it != plan.cuts.rend(); ++it) {
        command::Json tracks = command::Json::array();
        for (const TrackId id : it->tracks) {
            tracks.push_back(id.value);
        }
        batch.commands.push_back(request("sequence.extract", command::Json{{"sequence", plan.sequence.value},
                                                                          {"in", it->range.start.value},
                                                                          {"out", it->range.end().value},
                                                                          {"tracks", std::move(tracks)}}));
    }
    return batch;
}

command::CommandBatch toCommandBatch(const MovePlan& plan, const ProjectState& state, std::string label) {
    command::CommandBatch batch;
    batch.label = std::move(label);
    const Sequence* seq = state.findSequence(plan.sequence);
    if (!seq) {
        return batch;
    }
    const command::Json tracks = trackLabels(*seq, plan.tracks);
    const command::Json sequence = plan.sequence.value;
    for (const Ticks at : {plan.source.start, plan.source.end()}) {
        batch.commands.push_back(request("sequence.addEdit", command::Json{{"sequence", sequence},
                                                                          {"time", at.value},
                                                                          {"tracks", tracks}}));
    }
    // The pieces inside the range after those cuts, named by where they start.
    command::Json pieces = command::Json::array();
    for (const TrackId id : plan.tracks) {
        const Track* t = seq->findTrack(id);
        for (const Clip& c : t->clips) {
            if (c.start() < plan.source.end() && c.end() > plan.source.start) {
                pieces.push_back(command::Json{{"track", trackLabel(*seq, id)},
                                               {"time", std::max(c.start(), plan.source.start).value}});
            }
        }
    }
    const Ticks delta = plan.destination - plan.source.start;
    batch.commands.push_back(request("clip.move", command::Json{{"sequence", sequence},
                                                               {"clips", std::move(pieces)},
                                                               {"delta", delta.value},
                                                               {"mode", "insert"},
                                                               {"linked", false}}));
    // Close the place the words left (it moved later when they were inserted before it).
    const TimeRange vacated = plan.destination < plan.source.start
                                  ? TimeRange{plan.source.start + plan.source.duration, plan.source.duration}
                                  : plan.source;
    batch.commands.push_back(request("sequence.extract", command::Json{{"sequence", sequence},
                                                                      {"in", vacated.start.value},
                                                                      {"out", vacated.end().value},
                                                                      {"tracks", tracks}}));
    return batch;
}

ProjectState runInline(const command::CommandRegistry& registry, const ProjectState& state,
                       const command::CommandBatch& batch, command::ExecutionControl* control,
                       command::CommandServices* services) {
    ProjectState working = state;
    for (std::size_t i = 0; i < batch.commands.size(); ++i) {
        const command::CommandRequest& r = batch.commands[i];
        const command::CommandSpec* spec = registry.find(r.command, r.version);
        if (!spec) {
            command::fail(command::ErrorCode::Internal, "the plan uses an unknown command " + r.command);
        }
        if (const auto issues = command::validateAgainstSchema(spec->params, r.params); !issues.empty()) {
            command::fail(command::ErrorCode::Internal,
                          "the plan's " + r.command + " is invalid: " + issues.front().path + " " + issues.front().message);
        }
        if (control) {
            if (auto stop = control->checkpoint("step " + std::to_string(i + 1) + " of " +
                                                std::to_string(batch.commands.size()) + ": " + spec->name)) {
                throw command::CommandFailure{std::move(*stop)};
            }
        }
        command::ExecutionContext context(working, r.params, control, services);
        command::CommandOutput out = spec->execute(context);
        if (spec->mutates()) {
            working = std::move(out.state);
        }
    }
    return working;
}

} // namespace wabcut::ai::edit
