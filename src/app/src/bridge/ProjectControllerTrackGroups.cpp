// ProjectController, 0.7 (card 124): track groups - a named set of video or audio tracks with a header row in the timeline
// that folds its tracks into it when collapsed, and mutes, solos, locks, targets and routes them together. The model side is
// TrackGroups.h; every method here is one undo step through applyEditFn, like the track header menu's.

#include "ProjectController.h"

#include "BridgeIds.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TrackGroups.h>

#include <algorithm>

namespace wabcut::app {

namespace {

QString trackLabelText(TrackKind kind, std::size_t index) {
    return QString(kind == TrackKind::Video ? QLatin1Char('V') : QLatin1Char('A')) + QString::number(index + 1);
}

/// 0: none of the members has the flag, 1: some do, 2: all do.
int triState(const Sequence& seq, const TrackGroup& group, bool (*has)(const Track&)) {
    int count = 0;
    for (const TrackId id : group.members) {
        const Track* t = seq.findTrack(id);
        if (t && has(*t))
            ++count;
    }
    return count == 0 ? 0 : count == static_cast<int>(group.members.size()) ? 2 : 1;
}

QVariantMap groupMap(const Sequence& seq, const TrackGroup& g) {
    QStringList tracks;
    QStringList shortNames;
    for (const TrackId id : g.members) {
        tracks << trackKey(id);
        if (const auto position = seq.trackPosition(id))
            shortNames << trackLabelText(position->kind, position->index);
    }
    // Where an audio group's tracks go: the Mix, one submix, or a mix of destinations.
    QString output = QStringLiteral("mix");
    QString submixName;
    if (g.kind == TrackKind::Audio && !g.members.empty()) {
        std::optional<TrackId> first;
        bool same = true;
        for (const TrackId id : g.members) {
            const Track* t = seq.findTrack(id);
            const TrackId out = t ? t->output : TrackId{};
            if (!first)
                first = out;
            else if (*first != out)
                same = false;
        }
        if (!same)
            output = QStringLiteral("mixed");
        else if (first && first->isValid()) {
            output = QStringLiteral("submix");
            if (const Track* submix = seq.findSubmix(*first))
                submixName = QString::fromStdString(submix->name);
        }
    }
    return {{QStringLiteral("id"), trackGroupKey(g.id)},
            {QStringLiteral("name"), QString::fromStdString(g.name)},
            {QStringLiteral("kind"), g.kind == TrackKind::Video ? QStringLiteral("video") : QStringLiteral("audio")},
            {QStringLiteral("colorIndex"), g.colorIndex},
            {QStringLiteral("collapsed"), g.collapsed},
            {QStringLiteral("count"), static_cast<int>(g.members.size())},
            {QStringLiteral("tracks"), tracks},
            {QStringLiteral("shortNames"), shortNames},
            {QStringLiteral("muted"), triState(seq, g, [](const Track& t) { return t.muted; })},
            {QStringLiteral("solo"), triState(seq, g, [](const Track& t) { return t.solo; })},
            {QStringLiteral("locked"), triState(seq, g, [](const Track& t) { return t.locked; })},
            {QStringLiteral("targeted"), triState(seq, g, [](const Track& t) { return t.targeted; })},
            {QStringLiteral("enabled"), triState(seq, g, [](const Track& t) { return t.enabled; })},
            {QStringLiteral("syncLocked"), triState(seq, g, [](const Track& t) { return t.syncLocked; })},
            {QStringLiteral("canRoute"), g.kind == TrackKind::Audio},
            {QStringLiteral("output"), output},
            {QStringLiteral("submixName"), submixName}};
}

const TrackGroup* findGroup(const Sequence* seq, const QString& key) {
    const auto id = parseTrackGroupKey(key);
    return seq && id ? seq->findTrackGroup(*id) : nullptr;
}

/// The ids behind track keys, or nullopt when one is not a track key.
std::optional<std::vector<TrackId>> trackIdsOf(const QStringList& keys) {
    std::vector<TrackId> ids;
    for (const QString& key : keys) {
        const auto id = parseTrackKey(key);
        if (!id)
            return std::nullopt;
        ids.push_back(*id);
    }
    return ids;
}

/// The TrackStateChange for a group switch, or nullopt for an unknown flag.
std::optional<edit::TrackStateChange> flagChange(const QString& flag, bool value) {
    edit::TrackStateChange change;
    if (flag == QLatin1String("muted"))
        change.muted = value;
    else if (flag == QLatin1String("solo"))
        change.solo = value;
    else if (flag == QLatin1String("locked"))
        change.locked = value;
    else if (flag == QLatin1String("targeted"))
        change.targeted = value;
    else if (flag == QLatin1String("enabled"))
        change.enabled = value;
    else if (flag == QLatin1String("syncLocked"))
        change.syncLocked = value;
    else
        return std::nullopt;
    return change;
}

bool trackHas(const Track& t, const QString& flag) {
    return flag == QLatin1String("muted")      ? t.muted
           : flag == QLatin1String("solo")     ? t.solo
           : flag == QLatin1String("locked")   ? t.locked
           : flag == QLatin1String("enabled")  ? t.enabled
           : flag == QLatin1String("syncLocked") ? t.syncLocked
                                                 : t.targeted;
}

} // namespace

QVariantList ProjectController::trackGroups() const {
    QVariantList out;
    if (const SequencePtr seq = currentSequence()) {
        for (const TrackGroup& g : seq->trackGroups)
            out << groupMap(*seq, g);
    }
    return out;
}

QVariantMap ProjectController::trackGroupInfo(const QString& groupId) const {
    const SequencePtr seq = currentSequence();
    const TrackGroup* group = findGroup(seq.get(), groupId);
    return group ? groupMap(*seq, *group) : QVariantMap();
}

QString ProjectController::createTrackGroup(const QStringList& trackIds, const QString& name) {
    const auto ids = trackIdsOf(trackIds);
    if (!ids || ids->empty()) {
        setError(tr("Choose the tracks to group"));
        return {};
    }
    const SequenceId seq = m_sequence;
    TrackGroupId created;
    const bool ok = applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Group Tracks"),
                                [&](const ProjectState& state) -> edit::EditOutcome {
                                    auto result = edit::createTrackGroup(state, seq, *ids, name.trimmed().toStdString());
                                    if (result)
                                        created = result->createdTrackGroup;
                                    return result;
                                });
    return ok ? trackGroupKey(created) : QString();
}

QString ProjectController::groupTargetedTracks(const QString& kind) {
    const SequencePtr current = currentSequence();
    if (!current) {
        return {};
    }
    if (!kind.isEmpty() && kind != QLatin1String("video") && kind != QLatin1String("audio")) {
        setError(tr("Group the targeted tracks of video or audio"));
        return {};
    }
    // The targeted tracks of each kind that are in no group (at least two: a group of one track is made from its menu).
    std::vector<std::vector<TrackId>> sets;
    for (const TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        if (!kind.isEmpty() && (kind == QLatin1String("video")) != (k == TrackKind::Video))
            continue;
        std::vector<TrackId> candidates;
        for (const TrackPtr& t : current->tracks(k)) {
            if (t->targeted && !current->trackGroupOf(t->id))
                candidates.push_back(t->id);
        }
        if (candidates.size() >= 2 || (!kind.isEmpty() && !candidates.empty()))
            sets.push_back(std::move(candidates));
    }
    if (sets.empty()) {
        setError(tr("Target two or more tracks that are in no group first (click their name chips; Shift-click targets every track)"));
        return {};
    }
    const SequenceId seq = m_sequence;
    TrackGroupId first;
    const bool ok = applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Group Tracks"),
                                [&](const ProjectState& state) -> edit::EditOutcome {
                                    ProjectState next = state;
                                    for (const std::vector<TrackId>& ids : sets) {
                                        auto step = edit::createTrackGroup(next, seq, ids);
                                        if (!step)
                                            return std::unexpected(step.error());
                                        if (!first.isValid())
                                            first = step->createdTrackGroup;
                                        next = std::move(step->state);
                                    }
                                    edit::EditResult result;
                                    result.changes = diff(state, next);
                                    result.state = std::move(next);
                                    return result;
                                });
    return ok ? trackGroupKey(first) : QString();
}

bool ProjectController::ungroupTracks(const QString& groupId) {
    const auto id = parseTrackGroupKey(groupId);
    if (!id) {
        setError(tr("That is not a track group"));
        return false;
    }
    const SequenceId seq = m_sequence;
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Ungroup Tracks"),
                       [&](const ProjectState& state) { return edit::ungroupTracks(state, seq, *id); });
}

bool ProjectController::renameTrackGroup(const QString& groupId, const QString& name) {
    const auto id = parseTrackGroupKey(groupId);
    if (!id) {
        setError(tr("That is not a track group"));
        return false;
    }
    const SequencePtr current = currentSequence();
    const TrackGroup* group = current ? current->findTrackGroup(*id) : nullptr;
    if (group && QString::fromStdString(group->name) == name.trimmed())
        return true; // already that name: no empty undo step
    edit::TrackGroupChange change;
    change.name = name.trimmed().toStdString();
    const SequenceId seq = m_sequence;
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Rename Group"),
                       [&](const ProjectState& state) { return edit::setTrackGroup(state, seq, *id, change); });
}

bool ProjectController::setTrackGroupColor(const QString& groupId, int colorIndex) {
    const auto id = parseTrackGroupKey(groupId);
    if (!id) {
        setError(tr("That is not a track group"));
        return false;
    }
    if (colorIndex < -1 || colorIndex > 15) {
        setError(tr("A group's label colour is one of the 16 labels"));
        return false;
    }
    const SequencePtr current = currentSequence();
    const TrackGroup* group = current ? current->findTrackGroup(*id) : nullptr;
    if (group && group->colorIndex == colorIndex)
        return true;
    edit::TrackGroupChange change;
    change.colorIndex = colorIndex;
    const SequenceId seq = m_sequence;
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Group Color"),
                       [&](const ProjectState& state) { return edit::setTrackGroup(state, seq, *id, change); });
}

bool ProjectController::setTrackGroupCollapsed(const QString& groupId, bool collapsed) {
    const auto id = parseTrackGroupKey(groupId);
    if (!id) {
        setError(tr("That is not a track group"));
        return false;
    }
    const SequencePtr current = currentSequence();
    const TrackGroup* group = current ? current->findTrackGroup(*id) : nullptr;
    if (group && group->collapsed == collapsed)
        return true;
    edit::TrackGroupChange change;
    change.collapsed = collapsed;
    const SequenceId seq = m_sequence;
    return applyEditFn(collapsed ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Collapse Group")
                                 : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Expand Group"),
                       [&](const ProjectState& state) { return edit::setTrackGroup(state, seq, *id, change); });
}

bool ProjectController::setAllTrackGroupsCollapsed(bool collapsed) {
    const SequencePtr current = currentSequence();
    if (!current)
        return false;
    std::vector<TrackGroupId> ids;
    for (const TrackGroup& g : current->trackGroups) {
        if (g.collapsed != collapsed)
            ids.push_back(g.id);
    }
    if (ids.empty())
        return false;
    edit::TrackGroupChange change;
    change.collapsed = collapsed;
    const SequenceId seq = m_sequence;
    return applyEditFn(collapsed ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Collapse All Groups")
                                 : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Expand All Groups"),
                       [&](const ProjectState& state) -> edit::EditOutcome {
                           ProjectState next = state;
                           for (const TrackGroupId id : ids) {
                               auto step = edit::setTrackGroup(next, seq, id, change);
                               if (!step)
                                   return std::unexpected(step.error());
                               next = std::move(step->state);
                           }
                           edit::EditResult result;
                           result.changes = diff(state, next);
                           result.state = std::move(next);
                           return result;
                       });
}

bool ProjectController::addTracksToTrackGroup(const QString& groupId, const QStringList& trackIds) {
    const auto id = parseTrackGroupKey(groupId);
    const auto ids = trackIdsOf(trackIds);
    if (!id || !ids) {
        setError(tr("Choose a track group and its tracks"));
        return false;
    }
    const SequenceId seq = m_sequence;
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add to Group"),
                       [&](const ProjectState& state) { return edit::addTracksToTrackGroup(state, seq, *id, *ids); });
}

bool ProjectController::removeTracksFromTrackGroups(const QStringList& trackIds) {
    const auto ids = trackIdsOf(trackIds);
    if (!ids) {
        setError(tr("Choose the tracks to take out of their group"));
        return false;
    }
    const SequenceId seq = m_sequence;
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Remove from Group"),
                       [&](const ProjectState& state) { return edit::removeTracksFromTrackGroups(state, seq, *ids); });
}

bool ProjectController::setTrackGroupFlag(const QString& groupId, const QString& flag, bool value) {
    const auto id = parseTrackGroupKey(groupId);
    const auto change = flagChange(flag, value);
    const SequencePtr current = currentSequence();
    const TrackGroup* group = current && id ? current->findTrackGroup(*id) : nullptr;
    if (!group || !change) {
        setError(tr("That is not a track group switch"));
        return false;
    }
    const bool already = std::ranges::all_of(group->members, [&](TrackId t) {
        const Track* track = current->findTrack(t);
        return !track || trackHas(*track, flag) == value;
    });
    if (already)
        return true; // every track has it: no empty undo step
    const char* label =
        flag == QLatin1String("muted")
            ? (value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Mute Group")
                     : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Unmute Group"))
        : flag == QLatin1String("solo")
            ? (value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Solo Group")
                     : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Unsolo Group"))
        : flag == QLatin1String("locked")
            ? (value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Lock Group")
                     : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Unlock Group"))
        : flag == QLatin1String("targeted")
            ? (value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Target Group")
                     : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Untarget Group"))
        : flag == QLatin1String("enabled")
            ? (value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Show Group Output")
                     : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Hide Group Output"))
            : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Toggle Sync Lock of Group");
    const SequenceId seq = m_sequence;
    const TrackGroupId groupKey = *id;
    const edit::TrackStateChange each = *change;
    return applyEditFn(label, [&](const ProjectState& state) { return edit::setTrackGroupTracksState(state, seq, groupKey, each); });
}

bool ProjectController::routeTrackGroup(const QString& groupId, const QString& output) {
    const auto id = parseTrackGroupKey(groupId);
    if (!id || (output != QLatin1String("mix") && output != QLatin1String("newSubmix"))) {
        setError(tr("Route a track group to the Mix or to a new submix"));
        return false;
    }
    const SequenceId seq = m_sequence;
    const bool toMix = output == QLatin1String("mix");
    return applyEditFn(toMix ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Route Group to Mix")
                             : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Route Group to New Submix"),
                       [&](const ProjectState& state) {
                           return toMix ? edit::routeTrackGroupToSubmix(state, seq, *id, TrackId{})
                                        : edit::routeTrackGroupToNewSubmix(state, seq, *id);
                       });
}

} // namespace wabcut::app
