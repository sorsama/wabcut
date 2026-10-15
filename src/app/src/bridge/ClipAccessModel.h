#pragma once

// ClipAccessModel: the timeline's clips as a flat list for assistive technology and keyboard-only use
// (ProjectController.clipList).
//
// The timeline itself is one scene-graph item with no per-clip QML items, so screen readers see nothing inside it.
// This model is its semantic twin: one row per clip, tracks in display order (Vn..V1, A1..An), clips by start,
// with the clip's name, track, start/end timecode, duration, effects, selection, lock and enable state and a spoken
// name. A hidden ListView in the timeline panel presents it (TimelinePanel.qml); the keyboard moves through it and
// selects clips. The Timeline list (M5j, TimelineListView.qml) shows it as a visible, track-grouped list.
//
// Sync: ProjectController calls reset() for structural changes and updateTracks() with the tracks of each
// ChangeSet, which replaces only those tracks' rows; selection changes update the selected flags and currentRow.
// Strings are formatted when a row is read, so a 10,000-clip sequence costs one small struct per clip.

#include <wabcut/model/Sequence.h>

#include <QAbstractListModel>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wabcut::app {

class ProjectController;

class ClipAccessModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by ProjectController.clipList")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
    /// Row of the primary selected clip, or -1.
    Q_PROPERTY(int currentRow READ currentRow NOTIFY currentRowChanged FINAL)

public:
    enum Role {
        ClipIdRole = Qt::UserRole + 1, // opaque "c…"
        NameRole,
        TrackNameRole,                 // "V1"
        StartRole,                     // timecode
        EndRole,                       // timecode of the last frame
        SelectedRole,
        LockedRole,
        EnabledRole,
        AccessibleNameRole,            // "Interview A, V1, 00:00:00:00 to 00:00:23:23, 00:00:24:00 long, selected"
        DurationRole,                  // timecode of the clip's length (M5j)
        EffectsRole,                   // the applied effects' names, comma-separated ("" for none)
        TrackKindRole,                 // "video" or "audio"
    };

    explicit ClipAccessModel(ProjectController* controller);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int currentRow() const { return m_currentRow; }

    /// Rebuilds every row from `sequence` (nullptr = empty).
    void reset(const Sequence* sequence);
    /// Replaces the rows of `tracks` (a ChangeSet's changed tracks) from `sequence`.
    void updateTracks(const Sequence& sequence, std::span<const TrackId> tracks);
    /// Re-reads the selection (selected flags, currentRow).
    void updateSelection();

    int rowOf(ClipId clip) const;
    ClipId clipAt(int row) const;
    /// The opaque key of the clip at `row` ("" out of range), for QML.
    Q_INVOKABLE QString clipIdAt(int row) const;

signals:
    void countChanged();
    void currentRowChanged();

private:
    struct TrackRows {
        TrackPtr track;
        QString label; // "V1"
        int first = 0; // first row
    };
    struct Row {
        int track = 0;          // index into m_tracks
        std::size_t index = 0;  // into the track's clips
    };
    const Clip& clipOf(const Row& row) const;
    void rebuildIndex();
    /// M5n (P.1): the index keeps each clip's track and position, its row being the track's first row plus the
    /// position, so an edit re-indexes only the tracks it changed (it used to rehash every clip).
    void indexTrack(int trackIndex, const Track& track);
    /// Drops the track's clips from the index (those still indexed on this track: a clip that moved to a track
    /// indexed before keeps its new entry).
    void unindexTrack(int trackIndex, const Track& track);

    ProjectController* m_controller;
    std::vector<TrackRows> m_tracks;
    std::vector<Row> m_rows;
    std::unordered_map<std::uint64_t, Row> m_rowById; ///< clip id -> track and position
    std::unordered_set<std::uint64_t> m_selected;
    /// Generated (AI) media as of the last spoken names: only a change of it re-reads them (review P2-46).
    std::unordered_set<std::uint64_t> m_generated;
    int m_currentRow = -1;
};

} // namespace wabcut::app
