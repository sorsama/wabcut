#include "ClipAccessModel.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/model/Effects.h>
#include <wabcut/model/Time.h>

#include <algorithm>

namespace wabcut::app {

ClipAccessModel::ClipAccessModel(ProjectController* controller)
    : QAbstractListModel(controller), m_controller(controller) {
    connect(controller, &ProjectController::mediaChanged, this, [this] {
        // Provenance lives on media, so a metadata-only edit does not update the track rows. Only a change of which
        // media is generated changes a spoken name (review P2-46: not every Source Monitor mark).
        std::unordered_set<std::uint64_t> generated;
        for (const MediaPtr& media : m_controller->snapshot()->media) {
            if (media->generated.has_value())
                generated.insert(media->id.value);
        }
        if (generated == m_generated)
            return;
        m_generated = std::move(generated);
        if (!m_rows.empty())
            emit dataChanged(index(0), index(rowCount() - 1), {AccessibleNameRole});
    });
}

int ClipAccessModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QHash<int, QByteArray> ClipAccessModel::roleNames() const {
    return {
        {ClipIdRole, "clipId"},     {NameRole, "name"},         {TrackNameRole, "trackName"},
        {StartRole, "startText"},   {EndRole, "endText"},       {SelectedRole, "selected"},
        {LockedRole, "locked"},     {EnabledRole, "clipEnabled"}, {AccessibleNameRole, "accessibleName"},
        {DurationRole, "durationText"}, {EffectsRole, "effectsText"}, {TrackKindRole, "trackKind"},
    };
}

const Clip& ClipAccessModel::clipOf(const Row& row) const {
    return m_tracks[static_cast<std::size_t>(row.track)].track->clips[row.index];
}

QVariant ClipAccessModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
        return {};
    const Row& row = m_rows[static_cast<std::size_t>(index.row())];
    const TrackRows& track = m_tracks[static_cast<std::size_t>(row.track)];
    const Clip& clip = clipOf(row);
    const auto lastFrame = [&] {
        // The end is exclusive; name the clip's last frame (as the clip editor does).
        const SequencePtr seq = m_controller->currentSequence();
        const Rate rate = seq ? seq->frameRate : kRate30;
        return std::max(clip.start(), clip.end() - framesToTicks(1, rate));
    };
    const auto effects = [&] {
        QStringList names;
        for (const EffectInstance& e : clip.content().effects) {
            const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
            names << (d ? QString::fromUtf8(d->displayName.data(), static_cast<qsizetype>(d->displayName.size()))
                        : QString::fromStdString(e.effect));
        }
        return names.join(QStringLiteral(", "));
    };
    switch (role) {
    case ClipIdRole:
        return clipKey(clip.id);
    case NameRole:
        return QString::fromStdString(clip.name());
    case TrackNameRole:
        return track.label;
    case StartRole:
        return m_controller->formatTimecode(clip.start());
    case EndRole:
        return m_controller->formatTimecode(lastFrame());
    case SelectedRole:
        return m_selected.contains(clip.id.value);
    case LockedRole:
        return track.track->locked;
    case EnabledRole:
        return clip.enabled;
    case DurationRole:
        return m_controller->formatTimecode(clip.duration());
    case EffectsRole:
        return effects();
    case TrackKindRole:
        return track.track->kind == TrackKind::Audio ? QStringLiteral("audio") : QStringLiteral("video");
    case AccessibleNameRole: {
        //: Spoken clip: name, track, first to last frame, length, e.g. "Interview A, V1, 00:00:00:00 to
        //: 00:00:23:23, 00:00:24:00 long"
        QString text = tr("%1, %2, %3 to %4, %5 long")
                           .arg(QString::fromStdString(clip.name()), track.label,
                                m_controller->formatTimecode(clip.start()), m_controller->formatTimecode(lastFrame()),
                                m_controller->formatTimecode(clip.duration()));
        if (const QString fx = effects(); !fx.isEmpty())
            text += tr(", effects: %1").arg(fx);
        if (const MediaItem* media = m_controller->snapshot()->findMedia(clip.media); media && media->generated)
            text += tr(", AI-generated");
        if (m_selected.contains(clip.id.value))
            text += tr(", selected");
        if (!clip.enabled)
            text += tr(", disabled");
        if (track.track->locked)
            text += tr(", locked");
        return text;
    }
    default:
        return {};
    }
}

void ClipAccessModel::reset(const Sequence* sequence) {
    const int before = static_cast<int>(m_rows.size());
    beginResetModel();
    m_tracks.clear();
    m_rows.clear();
    if (sequence) {
        const auto add = [&](const TrackPtr& t, const QString& label) {
            TrackRows tr{t, label, static_cast<int>(m_rows.size())};
            const int trackIndex = static_cast<int>(m_tracks.size());
            m_tracks.push_back(std::move(tr));
            for (std::size_t i = 0; i < t->clips.size(); ++i)
                m_rows.push_back({trackIndex, i});
        };
        for (std::size_t i = sequence->videoTracks.size(); i-- > 0;)
            add(sequence->videoTracks[i], QStringLiteral("V%1").arg(i + 1));
        for (std::size_t i = 0; i < sequence->audioTracks.size(); ++i)
            add(sequence->audioTracks[i], QStringLiteral("A%1").arg(i + 1));
    }
    rebuildIndex();
    endResetModel();
    if (before != static_cast<int>(m_rows.size()))
        emit countChanged();
    updateSelection();
}

void ClipAccessModel::updateTracks(const Sequence& sequence, std::span<const TrackId> tracks) {
    const int before = static_cast<int>(m_rows.size());
    for (TrackId id : tracks) {
        const auto t = std::ranges::find_if(m_tracks, [&](const TrackRows& r) { return r.track->id == id; });
        const Track* next = sequence.findTrack(id);
        if (t == m_tracks.end() || !next) {
            reset(&sequence); // tracks added or removed: start over
            return;
        }
        // The shared node for the new revision of this track.
        TrackPtr nextPtr;
        for (const auto& list : {std::cref(sequence.videoTracks), std::cref(sequence.audioTracks)}) {
            for (const TrackPtr& p : list.get()) {
                if (p->id == id)
                    nextPtr = p;
            }
        }
        const int trackIndex = static_cast<int>(t - m_tracks.begin());
        unindexTrack(trackIndex, *t->track);
        const int first = t->first;
        const int oldCount = static_cast<int>(t->track->clips.size());
        const int newCount = static_cast<int>(nextPtr->clips.size());
        if (oldCount == newCount) {
            t->track = nextPtr;
            indexTrack(trackIndex, *nextPtr);
            if (newCount > 0)
                emit dataChanged(index(first), index(first + newCount - 1));
            continue;
        }
        if (oldCount > 0) {
            beginRemoveRows({}, first, first + oldCount - 1);
            m_rows.erase(m_rows.begin() + first, m_rows.begin() + first + oldCount);
            for (auto it = t + 1; it != m_tracks.end(); ++it)
                it->first -= oldCount;
            t->track = nextPtr;
            endRemoveRows();
        }
        t->track = nextPtr;
        if (newCount > 0) {
            beginInsertRows({}, first, first + newCount - 1);
            std::vector<Row> rows;
            rows.reserve(static_cast<std::size_t>(newCount));
            for (int i = 0; i < newCount; ++i)
                rows.push_back({trackIndex, static_cast<std::size_t>(i)});
            m_rows.insert(m_rows.begin() + first, rows.begin(), rows.end());
            for (auto it = m_tracks.begin() + trackIndex + 1; it != m_tracks.end(); ++it)
                it->first += newCount;
            endInsertRows();
        }
        indexTrack(trackIndex, *nextPtr);
    }
    if (before != static_cast<int>(m_rows.size()))
        emit countChanged();
    updateSelection();
}

void ClipAccessModel::rebuildIndex() {
    m_rowById.clear();
    m_rowById.reserve(m_rows.size());
    for (std::size_t t = 0; t < m_tracks.size(); ++t)
        indexTrack(static_cast<int>(t), *m_tracks[t].track);
}

void ClipAccessModel::indexTrack(int trackIndex, const Track& track) {
    for (std::size_t i = 0; i < track.clips.size(); ++i)
        m_rowById[track.clips[i].id.value] = Row{trackIndex, i};
}

void ClipAccessModel::unindexTrack(int trackIndex, const Track& track) {
    for (const Clip& c : track.clips) {
        const auto it = m_rowById.find(c.id.value);
        if (it != m_rowById.end() && it->second.track == trackIndex)
            m_rowById.erase(it);
    }
}

void ClipAccessModel::updateSelection() {
    std::unordered_set<std::uint64_t> next;
    for (ClipId id : m_controller->selection())
        next.insert(id.value);
    // Rows whose flag changed.
    std::vector<int> changed;
    for (std::uint64_t id : m_selected) {
        if (!next.contains(id))
            changed.push_back(rowOf(ClipId{id}));
    }
    for (std::uint64_t id : next) {
        if (!m_selected.contains(id))
            changed.push_back(rowOf(ClipId{id}));
    }
    m_selected = std::move(next);
    for (int row : changed) {
        if (row >= 0)
            emit dataChanged(index(row), index(row), {SelectedRole, AccessibleNameRole});
    }
    const int current = rowOf(m_controller->primaryClip());
    if (current != m_currentRow) {
        m_currentRow = current;
        emit currentRowChanged();
    }
}

int ClipAccessModel::rowOf(ClipId clip) const {
    const auto it = m_rowById.find(clip.value);
    if (it == m_rowById.end())
        return -1;
    return m_tracks[static_cast<std::size_t>(it->second.track)].first + static_cast<int>(it->second.index);
}

ClipId ClipAccessModel::clipAt(int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size()))
        return {};
    return clipOf(m_rows[static_cast<std::size_t>(row)]).id;
}

QString ClipAccessModel::clipIdAt(int row) const {
    const ClipId id = clipAt(row);
    return id ? clipKey(id) : QString();
}

} // namespace wabcut::app
