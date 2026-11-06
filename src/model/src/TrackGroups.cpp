// Track groups (TrackGroups.h, 0.7 card 124).

#include <wabcut/model/TrackGroups.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/SubmixOps.h>
#include <wabcut/model/TitleOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace wabcut::edit {

using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

namespace {

std::vector<TrackPtr>& stackOf(Sequence& sequence, TrackKind kind) {
    return kind == TrackKind::Video ? sequence.videoTracks : sequence.audioTracks;
}

std::optional<std::size_t> indexInStack(const std::vector<TrackPtr>& stack, TrackId track) {
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (stack[i]->id == track) {
            return i;
        }
    }
    return std::nullopt;
}

bool contains(const std::vector<TrackId>& list, TrackId id) { return std::find(list.begin(), list.end(), id) != list.end(); }

TrackGroup* groupOf(Sequence& sequence, TrackId track) {
    for (TrackGroup& g : sequence.trackGroups) {
        if (contains(g.members, track)) {
            return &g;
        }
    }
    return nullptr;
}

/// Every group's members in stack order; groups left without tracks go.
void tidyGroups(Sequence& sequence) {
    for (TrackGroup& g : sequence.trackGroups) {
        std::vector<TrackId> ordered;
        for (const TrackPtr& t : sequence.tracks(g.kind)) {
            if (contains(g.members, t->id)) {
                ordered.push_back(t->id);
            }
        }
        g.members = std::move(ordered);
    }
    std::erase_if(sequence.trackGroups, [](const TrackGroup& g) { return g.members.empty(); });
}

std::string trimmed(std::string text) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && space(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    std::size_t first = 0;
    while (first < text.size() && space(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    return text.substr(first);
}

std::optional<std::string> nameProblem(const std::string& name) {
    if (name.empty()) {
        return "a track group needs a name";
    }
    if (name.size() > kMaxTrackGroupNameBytes) {
        return "a track group's name is at most " + std::to_string(kMaxTrackGroupNameBytes) + " bytes";
    }
    if (!isValidUtf8(name)) {
        return "a track group's name is not valid UTF-8";
    }
    return std::nullopt;
}

std::expected<const TrackGroup*, Error> findGroup(const Sequence& sequence, TrackGroupId group) {
    if (const TrackGroup* g = sequence.findTrackGroup(group)) {
        return g;
    }
    return std::unexpected(notFound("track group " + std::to_string(group.value) + " does not exist"));
}

/// A chain of operations done one after the other on the state: one EditResult for all of them.
EditOutcome chained(const ProjectState& before, ProjectState after) {
    EditResult result;
    result.changes = diff(before, after);
    if (result.changes.empty()) {
        return detail::unchanged(before);
    }
    result.state = std::move(after);
    return result;
}

} // namespace

std::string defaultTrackGroupName(const Sequence& sequence) {
    for (int n = 1;; ++n) {
        const std::string candidate = "Group " + std::to_string(n);
        if (std::none_of(sequence.trackGroups.begin(), sequence.trackGroups.end(),
                         [&](const TrackGroup& g) { return g.name == candidate; })) {
            return candidate;
        }
    }
}

EditOutcome createTrackGroup(const ProjectState& state, SequenceId sequenceId, std::span<const TrackId> tracks,
                             std::string name, std::int32_t colorIndex) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& seq = **found;
    if (tracks.empty()) {
        return std::unexpected(invalidArgument("name at least one track to put in the group"));
    }
    if (seq.trackGroups.size() >= kMaxTrackGroups) {
        return std::unexpected(invalidArgument("a sequence has at most " + std::to_string(kMaxTrackGroups) + " track groups"));
    }
    if (colorIndex < -1 || colorIndex > 15) {
        return std::unexpected(invalidArgument("a track group's label colour is 0..15, or -1 for none"));
    }
    name = trimmed(std::move(name));
    if (name.empty()) {
        name = defaultTrackGroupName(seq);
    }
    if (const auto problem = nameProblem(name)) {
        return std::unexpected(invalidArgument(*problem));
    }
    std::optional<TrackKind> kind;
    std::unordered_set<TrackId> wanted;
    for (const TrackId id : tracks) {
        const Track* track = seq.findTrack(id);
        if (!track) {
            if (seq.findSubmix(id)) {
                return std::unexpected(invalidArgument(detail::trackLabel(seq, id) + " is a submix track: only video and audio tracks group"));
            }
            return std::unexpected(notFound("track " + std::to_string(id.value) + " does not exist"));
        }
        if (kind && *kind != track->kind) {
            return std::unexpected(invalidArgument("a group holds video tracks or audio tracks, not both"));
        }
        kind = track->kind;
        if (const TrackGroup* other = seq.trackGroupOf(id)) {
            return std::unexpected(invalidArgument(detail::trackLabel(seq, id) + " is already in the group '" + other->name +
                                                   "': take it out of that group first"));
        }
        if (!wanted.insert(id).second) {
            return std::unexpected(invalidArgument("track " + detail::trackLabel(seq, id) + " is named twice"));
        }
    }
    SequenceEditor ed(state, seq);
    Sequence& s = ed.header();
    std::vector<TrackPtr>& stack = stackOf(s, *kind);
    // The group's tracks gather where the first of them is, in stack order.
    std::vector<TrackPtr> gathered;
    for (const TrackPtr& t : stack) {
        if (wanted.contains(t->id)) {
            gathered.push_back(t);
        }
    }
    std::vector<TrackPtr> next;
    bool placed = false;
    for (const TrackPtr& t : stack) {
        if (wanted.contains(t->id)) {
            if (!placed) {
                next.insert(next.end(), gathered.begin(), gathered.end());
                placed = true;
            }
            continue;
        }
        next.push_back(t);
    }
    stack = std::move(next);
    TrackGroup group;
    group.id = ed.newTrackGroup();
    group.kind = *kind;
    group.name = std::move(name);
    group.colorIndex = colorIndex;
    for (const TrackPtr& t : gathered) {
        group.members.push_back(t->id);
    }
    const TrackGroupId id = group.id;
    s.trackGroups.push_back(std::move(group));
    EditResult result = ed.finish();
    result.createdTrackGroup = id;
    return result;
}

EditOutcome setTrackGroup(const ProjectState& state, SequenceId sequenceId, TrackGroupId groupId,
                          const TrackGroupChange& change) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const auto group = findGroup(**found, groupId);
    if (!group) {
        return std::unexpected(group.error());
    }
    std::optional<std::string> name;
    if (change.name) {
        name = trimmed(*change.name);
        if (const auto problem = nameProblem(*name)) {
            return std::unexpected(invalidArgument(*problem));
        }
    }
    if (change.colorIndex && (*change.colorIndex < -1 || *change.colorIndex > 15)) {
        return std::unexpected(invalidArgument("a track group's label colour is 0..15, or -1 for none"));
    }
    const TrackGroup& g = **group;
    if ((!name || *name == g.name) && (!change.colorIndex || *change.colorIndex == g.colorIndex) &&
        (!change.collapsed || *change.collapsed == g.collapsed)) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **found);
    for (TrackGroup& target : ed.header().trackGroups) {
        if (target.id != groupId) {
            continue;
        }
        if (name) {
            target.name = *name;
        }
        if (change.colorIndex) {
            target.colorIndex = *change.colorIndex;
        }
        if (change.collapsed) {
            target.collapsed = *change.collapsed;
        }
    }
    return ed.finish();
}

EditOutcome ungroupTracks(const ProjectState& state, SequenceId sequenceId, TrackGroupId groupId) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (const auto group = findGroup(**found, groupId); !group) {
        return std::unexpected(group.error());
    }
    SequenceEditor ed(state, **found);
    std::erase_if(ed.header().trackGroups, [&](const TrackGroup& g) { return g.id == groupId; });
    return ed.finish();
}

EditOutcome addTracksToTrackGroup(const ProjectState& state, SequenceId sequenceId, TrackGroupId groupId,
                                  std::span<const TrackId> tracks) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& seq = **found;
    const auto group = findGroup(seq, groupId);
    if (!group) {
        return std::unexpected(group.error());
    }
    std::unordered_set<TrackId> joining;
    for (const TrackId id : tracks) {
        const Track* track = seq.findTrack(id);
        if (!track) {
            return std::unexpected(seq.findSubmix(id) ? invalidArgument(detail::trackLabel(seq, id) + " is a submix track: only video and audio tracks group")
                                                      : notFound("track " + std::to_string(id.value) + " does not exist"));
        }
        if (track->kind != (*group)->kind) {
            return std::unexpected(invalidArgument(detail::trackLabel(seq, id) + " cannot join the group '" + (*group)->name +
                                                   "': it holds " + (track->kind == TrackKind::Video ? "audio" : "video") + " tracks"));
        }
        if (!contains((*group)->members, id)) {
            joining.insert(id);
        }
    }
    if (joining.empty()) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, seq);
    Sequence& s = ed.header();
    std::vector<TrackPtr>& stack = stackOf(s, (*group)->kind);
    std::vector<TrackPtr> moving;
    std::vector<TrackPtr> rest;
    for (const TrackPtr& t : stack) {
        (joining.contains(t->id) ? moving : rest).push_back(t);
    }
    // After the last track the group keeps.
    std::size_t at = 0;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (contains((*group)->members, rest[i]->id)) {
            at = i + 1;
        }
    }
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(at), moving.begin(), moving.end());
    stack = std::move(rest);
    for (TrackGroup& g : s.trackGroups) {
        if (g.id == groupId) {
            for (const TrackPtr& t : moving) {
                g.members.push_back(t->id);
            }
        } else {
            std::erase_if(g.members, [&](TrackId id) { return joining.contains(id); });
        }
    }
    tidyGroups(s);
    return ed.finish();
}

EditOutcome removeTracksFromTrackGroups(const ProjectState& state, SequenceId sequenceId, std::span<const TrackId> tracks) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const Sequence& seq = **found;
    std::unordered_set<TrackId> leaving;
    for (const TrackId id : tracks) {
        if (!seq.findTrack(id)) {
            return std::unexpected(seq.findSubmix(id) ? invalidArgument(detail::trackLabel(seq, id) + " is a submix track: it is in no group")
                                                      : notFound("track " + std::to_string(id.value) + " does not exist"));
        }
        if (seq.trackGroupOf(id)) {
            leaving.insert(id);
        }
    }
    if (leaving.empty()) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, seq);
    Sequence& s = ed.header();
    for (TrackGroup& g : s.trackGroups) {
        // A track from the middle of the group moves to its end, so the tracks left stay together.
        std::vector<TrackId> staying;
        std::vector<TrackId> going;
        for (const TrackId id : g.members) {
            (leaving.contains(id) ? going : staying).push_back(id);
        }
        if (going.empty()) {
            continue;
        }
        std::vector<TrackPtr>& stack = stackOf(s, g.kind);
        std::vector<TrackId> inside;
        if (!staying.empty()) {
            std::size_t firstStaying = stack.size();
            std::size_t lastStaying = 0;
            for (std::size_t i = 0; i < stack.size(); ++i) {
                if (contains(staying, stack[i]->id)) {
                    firstStaying = std::min(firstStaying, i);
                    lastStaying = std::max(lastStaying, i);
                }
            }
            for (const TrackId id : going) {
                const std::size_t i = *indexInStack(stack, id);
                if (i > firstStaying && i < lastStaying) {
                    inside.push_back(id);
                }
            }
        }
        if (!inside.empty()) {
            std::vector<TrackPtr> moving;
            std::vector<TrackPtr> rest;
            for (const TrackPtr& t : stack) {
                (contains(inside, t->id) ? moving : rest).push_back(t);
            }
            std::size_t at = 0;
            for (std::size_t i = 0; i < rest.size(); ++i) {
                if (contains(staying, rest[i]->id)) {
                    at = i + 1;
                }
            }
            rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(at), moving.begin(), moving.end());
            stack = std::move(rest);
        }
        g.members = std::move(staying);
    }
    tidyGroups(s);
    return ed.finish();
}

EditOutcome setTrackGroupTracksState(const ProjectState& state, SequenceId sequenceId, TrackGroupId groupId,
                                     const TrackStateChange& change) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const auto group = findGroup(**found, groupId);
    if (!group) {
        return std::unexpected(group.error());
    }
    ProjectState next = state;
    for (const TrackId id : (*group)->members) {
        auto step = setTrackState(next, sequenceId, id, change);
        if (!step) {
            return std::unexpected(step.error());
        }
        next = std::move(step->state);
    }
    return chained(state, std::move(next));
}

EditOutcome routeTrackGroupToSubmix(const ProjectState& state, SequenceId sequenceId, TrackGroupId groupId, TrackId submix) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const auto group = findGroup(**found, groupId);
    if (!group) {
        return std::unexpected(group.error());
    }
    if ((*group)->kind != TrackKind::Audio) {
        return std::unexpected(invalidArgument("only an audio group has an output: the group '" + (*group)->name + "' holds video tracks"));
    }
    ProjectState next = state;
    for (const TrackId id : (*group)->members) {
        auto step = setTrackOutput(next, sequenceId, id, submix);
        if (!step) {
            return std::unexpected(step.error());
        }
        next = std::move(step->state);
    }
    return chained(state, std::move(next));
}

EditOutcome routeTrackGroupToNewSubmix(const ProjectState& state, SequenceId sequenceId, TrackGroupId groupId) {
    const auto found = detail::findSequence(state, sequenceId);
    if (!found) {
        return std::unexpected(found.error());
    }
    const auto group = findGroup(**found, groupId);
    if (!group) {
        return std::unexpected(group.error());
    }
    if ((*group)->kind != TrackKind::Audio) {
        return std::unexpected(invalidArgument("only an audio group has an output: the group '" + (*group)->name + "' holds video tracks"));
    }
    auto made = addSubmixTrack(state, sequenceId, ChannelFormat::Stereo, (*group)->name);
    if (!made) {
        return std::unexpected(made.error());
    }
    if (made->createdTracks.empty()) {
        return std::unexpected(invalidArgument("the submix track was not made"));
    }
    const TrackId submix = made->createdTracks.front();
    ProjectState next = std::move(made->state);
    for (const TrackId id : (*group)->members) {
        auto step = setTrackOutput(next, sequenceId, id, submix);
        if (!step) {
            return std::unexpected(step.error());
        }
        next = std::move(step->state);
    }
    EditOutcome result = chained(state, std::move(next));
    if (result) {
        result->createdTracks = {submix};
    }
    return result;
}

bool regroupAfterMove(Sequence& sequence, TrackId moved) {
    const auto position = sequence.trackPosition(moved);
    if (!position || sequence.trackGroups.empty()) {
        return false;
    }
    const std::vector<TrackPtr>& stack = sequence.tracks(position->kind);
    const auto groupAt = [&](std::ptrdiff_t i) -> TrackGroupId {
        if (i < 0 || i >= static_cast<std::ptrdiff_t>(stack.size())) {
            return {};
        }
        const TrackGroup* g = sequence.trackGroupOf(stack[static_cast<std::size_t>(i)]->id);
        return g ? g->id : TrackGroupId{};
    };
    const auto i = static_cast<std::ptrdiff_t>(position->index);
    const TrackGroupId before = groupAt(i - 1);
    const TrackGroupId after = groupAt(i + 1);
    TrackGroup* own = groupOf(sequence, moved);
    bool changed = false;
    if (own) {
        if (own->id != before && own->id != after) { // it no longer touches its group
            std::erase(own->members, moved);
            changed = true;
        }
    } else if (before.isValid() && before == after) { // dropped between two members of a group
        for (TrackGroup& g : sequence.trackGroups) {
            if (g.id == before) {
                g.members.push_back(moved);
                changed = true;
            }
        }
    }
    tidyGroups(sequence);
    return changed;
}

bool pruneTrackGroups(Sequence& sequence) {
    if (sequence.trackGroups.empty()) {
        return false;
    }
    const std::vector<TrackGroup> before = sequence.trackGroups;
    for (TrackGroup& g : sequence.trackGroups) {
        std::erase_if(g.members, [&](TrackId id) {
            const auto position = sequence.trackPosition(id);
            return !position || position->kind != g.kind;
        });
    }
    tidyGroups(sequence);
    return sequence.trackGroups != before;
}

std::optional<std::string> trackGroupsProblem(const Sequence& sequence) {
    if (sequence.trackGroups.size() > kMaxTrackGroups) {
        return "more than " + std::to_string(kMaxTrackGroups) + " track groups";
    }
    std::unordered_set<TrackGroupId> ids;
    std::unordered_set<TrackId> used;
    for (const TrackGroup& g : sequence.trackGroups) {
        const std::string what = "group " + std::to_string(g.id.value);
        if (!g.id.isValid() || !ids.insert(g.id).second) {
            return what + " has no id or shares it";
        }
        if (const auto problem = nameProblem(g.name)) {
            return what + ": " + *problem;
        }
        if (g.colorIndex < -1 || g.colorIndex > 15) {
            return what + " has a label colour outside 0..15";
        }
        if (g.members.empty()) {
            return what + " has no tracks";
        }
        std::size_t lastIndex = 0;
        bool first = true;
        for (const TrackId id : g.members) {
            const auto position = sequence.trackPosition(id);
            if (!position || position->kind != g.kind) {
                return what + " names a track that is not a " + (g.kind == TrackKind::Video ? "video" : "audio") + " track of the sequence";
            }
            if (!used.insert(id).second) {
                return what + " shares a track with another group or lists it twice";
            }
            if (!first && position->index <= lastIndex) {
                return what + " lists its tracks out of stack order";
            }
            lastIndex = position->index;
            first = false;
        }
    }
    return std::nullopt;
}

} // namespace wabcut::edit
