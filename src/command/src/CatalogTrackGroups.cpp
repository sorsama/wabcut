// Built-in track group commands (0.7, card 124; model: TrackGroups.h): a named set of video or audio tracks that collapses
// into one header row and mutes, solos, locks, targets and routes its tracks together.

#include "Catalog.h"

#include <wabcut/model/TrackGroups.h>

#include <string>
#include <utility>
#include <vector>

namespace wabcut::command::detail {

namespace {

Json groupParam() {
    return schema::integer("The track group's id (sequence.summary lists them under trackGroups).", 1);
}

/// The group named by "trackGroup" in the context's sequence.
const TrackGroup& groupOf(ExecutionContext& c) {
    const auto id = static_cast<std::uint64_t>(c.integer("trackGroup"));
    if (const TrackGroup* group = c.sequence().findTrackGroup(TrackGroupId{id})) {
        return *group;
    }
    fail(ErrorCode::NotFound, "no track group " + std::to_string(id) + " in this sequence", Json{{"path", "/trackGroup"}});
}

} // namespace

void registerTrackGroupCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"trackGroup.create", 1, "Group Tracks",
                "Groups video tracks or audio tracks (0.7): a named set with a header row in the timeline that folds its "
                "tracks into it when collapsed and mutes, solos, locks, targets and routes them together. The tracks "
                "must be of one kind and in no group yet; they are gathered at the place of the first one. Returns the "
                "group's id. A group is saved with the project.",
                Capability::EditSequence,
                withSequence({{"tracks", tracksParam("The tracks to group (V1, A2, ...).")},
                              {"name", schema::string("The group's name (default \"Group 1\", \"Group 2\", ...).")},
                              {"colorIndex", schema::integer("The group's label colour, 0-15, or -1 for none (default).", -1, 15)}},
                             {"tracks"}),
                Json{{"tracks", Json::array({"A1", "A2"})}, {"name", "Dialogue"}}, {},
                [](ExecutionContext& c) {
                    const std::vector<TrackId> tracks = c.tracks("tracks");
                    const auto color = static_cast<std::int32_t>(c.has("colorIndex") ? c.integer("colorIndex") : -1);
                    edit::EditResult r = c.take(edit::createTrackGroup(c.state(), c.sequenceId(), tracks, c.string("name"), color));
                    const TrackGroupId id = r.createdTrackGroup;
                    CommandOutput out = edited(std::move(r), Json{{"trackGroup", id.value}});
                    out.createdIds.push_back(id.value);
                    return out;
                }});
    addBuiltin(registry,
               {"trackGroup.set", 1, "Track Group Settings",
                "Renames a track group, sets its label colour, or collapses / expands it (a collapsed group shows only its "
                "header row; its tracks still play and export).",
                Capability::EditSequence,
                withSequence({{"trackGroup", groupParam()},
                              {"name", schema::string("The new name.")},
                              {"colorIndex", schema::integer("The label colour, 0-15, or -1 for none.", -1, 15)},
                              {"collapsed", schema::boolean("Fold the tracks into the group's header row (true) or show them (false).")}},
                             {"trackGroup"}),
                Json{{"trackGroup", 12}, {"collapsed", true}}, {},
                [](ExecutionContext& c) {
                    const TrackGroup& group = groupOf(c);
                    edit::TrackGroupChange change;
                    if (c.has("name")) {
                        change.name = c.string("name");
                    }
                    if (c.has("colorIndex")) {
                        change.colorIndex = static_cast<std::int32_t>(c.integer("colorIndex"));
                    }
                    if (c.has("collapsed")) {
                        change.collapsed = c.boolean("collapsed", false);
                    }
                    return edited(c.take(edit::setTrackGroup(c.state(), c.sequenceId(), group.id, change)));
                }});
    addBuiltin(registry,
               {"trackGroup.ungroup", 1, "Ungroup Tracks",
                "Dissolves a track group; its tracks stay where they are, with their clips and settings.",
                Capability::EditSequence, withSequence({{"trackGroup", groupParam()}}, {"trackGroup"}),
                Json{{"trackGroup", 12}}, {},
                [](ExecutionContext& c) {
                    const TrackGroup& group = groupOf(c);
                    return edited(c.take(edit::ungroupTracks(c.state(), c.sequenceId(), group.id)));
                }});
    addBuiltin(registry,
               {"trackGroup.addTracks", 1, "Add Tracks to Group",
                "Adds tracks (of the group's kind) to a track group; each moves next to the group's last track and leaves "
                "the group it was in.",
                Capability::EditSequence,
                withSequence({{"trackGroup", groupParam()}, {"tracks", tracksParam("The tracks to add.")}}, {"trackGroup", "tracks"}),
                Json{{"trackGroup", 12}, {"tracks", Json::array({"A3"})}}, {},
                [](ExecutionContext& c) {
                    const TrackGroup& group = groupOf(c);
                    const std::vector<TrackId> tracks = c.tracks("tracks");
                    return edited(c.take(edit::addTracksToTrackGroup(c.state(), c.sequenceId(), group.id, tracks)));
                }});
    addBuiltin(registry,
               {"trackGroup.removeTracks", 1, "Remove Tracks from Group",
                "Takes tracks out of their track group (tracks in no group are ignored); a track from the middle of its "
                "group moves to the group's end so the group stays whole. A group left without tracks is dissolved.",
                Capability::EditSequence, withSequence({{"tracks", tracksParam("The tracks to take out.")}}, {"tracks"}),
                Json{{"tracks", Json::array({"A3"})}}, {},
                [](ExecutionContext& c) {
                    const std::vector<TrackId> tracks = c.tracks("tracks");
                    return edited(c.take(edit::removeTracksFromTrackGroups(c.state(), c.sequenceId(), tracks)));
                }});
    addBuiltin(registry,
               {"trackGroup.setTracksState", 1, "Set Track Group State",
                "Changes the header toggles of every track in a group in one step: lock, target, sync lock, mute, solo, "
                "output (the eye), label colour.",
                Capability::EditSequence,
                withSequence({{"trackGroup", groupParam()},
                              {"locked", schema::boolean("Track lock.")},
                              {"targeted", schema::boolean("Track targeting.")},
                              {"syncLocked", schema::boolean("Sync lock.")},
                              {"muted", schema::boolean("Mute (audio).")},
                              {"solo", schema::boolean("Solo.")},
                              {"enabled", schema::boolean("Track output on.")},
                              {"colorIndex", schema::integer("The tracks' label colour, 0-15, or -1 for none.", -1, 15)}},
                             {"trackGroup"}),
                Json{{"trackGroup", 12}, {"muted", true}}, {},
                [](ExecutionContext& c) {
                    const TrackGroup& group = groupOf(c);
                    edit::TrackStateChange change;
                    if (c.has("colorIndex")) {
                        change.colorIndex = static_cast<std::int32_t>(c.integer("colorIndex"));
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
                    return edited(c.take(edit::setTrackGroupTracksState(c.state(), c.sequenceId(), group.id, change)));
                }});
    addBuiltin(registry,
               {"trackGroup.route", 1, "Route Track Group",
                "Sets the output of every track in an audio group: \"mix\" (the master), \"newSubmix\" (adds a submix "
                "track named after the group and routes the group to it, so one fader and one effect stack work on the "
                "whole group) or an existing submix track by id.",
                Capability::EditSequence,
                withSequence({{"trackGroup", groupParam()},
                              {"output", schema::enumeration({"mix", "newSubmix"}, "Where the group goes.")},
                              {"submix", schema::integer("Or the id of an existing submix track.", 1)}},
                             {"trackGroup"}),
                Json{{"trackGroup", 12}, {"output", "newSubmix"}}, {},
                [](ExecutionContext& c) {
                    const TrackGroup& group = groupOf(c);
                    if (c.has("submix")) {
                        const TrackId submix{static_cast<std::uint64_t>(c.integer("submix"))};
                        if (!c.sequence().findSubmix(submix)) {
                            fail(ErrorCode::NotFound, "no submix track " + std::to_string(submix.value) + " in this sequence",
                                 Json{{"path", "/submix"}});
                        }
                        return edited(c.take(edit::routeTrackGroupToSubmix(c.state(), c.sequenceId(), group.id, submix)));
                    }
                    const std::string output = c.string("output", "mix");
                    if (output == "newSubmix") {
                        edit::EditResult r = c.take(edit::routeTrackGroupToNewSubmix(c.state(), c.sequenceId(), group.id));
                        const Json data{{"submix", r.createdTracks.empty() ? 0 : r.createdTracks.front().value}};
                        return edited(std::move(r), data);
                    }
                    return edited(c.take(edit::routeTrackGroupToSubmix(c.state(), c.sequenceId(), group.id, TrackId{})));
                }});
}

} // namespace wabcut::command::detail
