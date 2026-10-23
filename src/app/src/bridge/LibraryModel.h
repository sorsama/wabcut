#pragma once

// LibraryModel (M5k): the rows of the Library (Premiere's Project panel) for its grid and list views.
//
// One QAbstractTableModel serves both: the grid reads column 0's roles, the list (a TableView) one cell per visible
// column. Rows are bins, sequences and media items (subclips included) of the current bin, or of the whole project
// while Find filters are active. A row keeps only pointers into the snapshot it was built from (held here), and cell
// text is formatted on demand in data(), so 10,000 items cost one pass of pointer copies to list and nothing per
// scroll beyond the cells in view.
//
// Sorting is by any column within groups: bins first, then sequences, then media (folders first, like Explorer and
// Finder; sequences next to them, where an editor looks for them); stable; a column's sort key is computed once per sort. Principle 9: rows are keyed "b12" / "s3" / "m7"; cells are display text (timecode, "1920 ×
// 1080", "48 kHz stereo"), never ticks or ids.

#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectState.h>

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wabcut::app {

/// Library keys for QML (principle 9): "b4" = bin 4; media "m7" and sequences "s3" as BridgeIds.h writes them.
inline QString binKey(BinId id) { return id.isValid() ? QStringLiteral("b%1").arg(id.value) : QString(); }
inline std::optional<BinId> parseBinKey(const QString& text) {
    if (text.size() < 2 || text.front() != QLatin1Char('b'))
        return std::nullopt;
    bool ok = false;
    const quint64 value = QStringView(text).mid(1).toULongLong(&ok);
    return ok && value != 0 ? std::optional<BinId>(BinId{value}) : std::nullopt;
}
inline std::optional<MediaId> parseMediaKey(const QString& text) {
    if (text.size() < 2 || text.front() != QLatin1Char('m'))
        return std::nullopt;
    bool ok = false;
    const quint64 value = QStringView(text).mid(1).toULongLong(&ok);
    return ok && value != 0 ? std::optional<MediaId>(MediaId{value}) : std::nullopt;
}

/// The list view's columns (Premiere's Project panel metadata).
enum class LibraryColumn : std::uint8_t {
    Name,
    Label,
    MediaType,
    Duration,
    FrameRate,
    Resolution,
    Codec,
    AudioChannels,
    MediaIn,
    MediaOut,
    Proxy,
    Analysis,
    Generated,
    Date,
    Path,
    Comment,
    Description,
    // 0.2 (P.3): Premiere's clip metadata columns (hidden until chosen, like the other optional columns).
    Scene,
    Shot,
    Take,
    LogNote,
    Good,
    Client,
    TapeName,
    // Premiere's Status and the camera fields (0.2, P.3).
    Status,
    CameraAngle,
    CameraLabel,
    CameraModel,
    ShotDate,
    ShotLocation,
};
inline constexpr std::size_t kLibraryColumnCount = 30;

struct LibraryColumnInfo {
    LibraryColumn column;
    const char* id;    ///< stable key ("name", "fps", ...): saved in the settings and used by QML
    int defaultWidth;  ///< logical pixels
    bool defaultVisible;
    bool editable;     ///< inline editing in the list (name, comment, description, clip metadata; Good is a check box)
};
const std::array<LibraryColumnInfo, kLibraryColumnCount>& libraryColumns();
std::optional<LibraryColumn> libraryColumnFromId(const QString& id);
/// The translated column title.
QString libraryColumnTitle(LibraryColumn column);

/// Per media item state that does not live in the model (refreshed asynchronously).
struct LibraryMediaStatus {
    QString proxyState;     ///< MediaReadiness: "", "queued", "generating", "paused", "ready", "stale", "failed"
    double proxyProgress = 0.0;
    bool transcribed = false;
    bool indexed = false;   ///< in the smart-search index (AI-8)
    bool shots = false;     ///< shot detection cached (C.5)
};

/// What the Find bar asks for. Empty fields do not filter.
struct LibraryFilter {
    QString text;           ///< name, comment, description or file path contains it (case-insensitive)
    /// "", "video", "audio", "still", "title", "sequence", "subclip", "bin"
    QString type;
    int label = -1;         ///< label colour index, -1 = any
    bool transcribed = false;
    bool offline = false;
    bool proxy = false;     ///< has a proxy (any state but none)
    bool generated = false; ///< AI-generated (AI-10 provenance)
    bool unused = false;    ///< not used in any sequence
    bool duplicates = false;
    /// File date range, "YYYY-MM-DD", inclusive; empty = open.
    QString from;
    QString to;
    /// The panel's Audio tab: only bins and audio-only media (not a Find filter: active() ignores it).
    bool audioOnly = false;
    /// 0.2 (P.3, Search Bins): where `text` is looked for - "" (name, comment, description, clip metadata, tape name,
    /// path) or one of kBinSearchFields; and only media marked Good.
    QString field;
    bool good = false;
    /// 0.2 (P.3): a search bin's query as a filter (its text, field and filters).
    static LibraryFilter fromSearch(const BinSearch& search);
    /// The filter as a search bin's query (Create Search Bin from the Find bar).
    BinSearch toSearch() const;

    bool active() const;
    bool operator==(const LibraryFilter&) const = default;
};

class LibraryModel : public QAbstractTableModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by Library.items")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1, ///< "b12" | "s3" | "m7"
        KindRole,                   ///< "bin" | "sequence" | "video" | "audio" | "still" | "title" | "subclip"
        NameRole,
        ColumnIdRole,               ///< the cell's column ("name", "fps", ...)
        EditableRole,               ///< the cell can be edited inline
        LabelRole,                  ///< label colour index
        ThumbnailRole,              ///< image URL or ""
        DurationRole,               ///< duration text ("" for bins)
        MetaRole,                   ///< the grid card's second line
        OfflineRole,
        ProxyStateRole,
        ProxyProgressRole,
        GeneratedRole,
        SubclipRole,                ///< { inText, outText, restricted, master } or an empty map
        UnusedRole,
        DuplicateRole,              ///< shares its file content with another item
        SelectedRole,
        FilmstripRole,              ///< hover scrub can show its filmstrip (video with a file)
        BinPathRole,                ///< where it lives ("Footage › Day 1"), shown while filtering
        CommentRole,
        CountRole,                  ///< bins: items inside (direct children); search bins: the items they find
        DerivedRole,                ///< M5l: the source a stem was separated from (its name), or ""
        SearchBinRole,              ///< 0.2 (P.3): a search bin
        GoodRole,                   ///< 0.2 (P.3): media marked Good (the Good column's check box)
        /// 0.2 UX (stream T): the item's Source Monitor marks for the card's In/Out band, or an empty map (none set, a
        /// still or an item without a known length): { in, out (fractions 0..1 of the card's range: the media, or a
        /// subclip's range), inText, outText (the last marked frame), durationText (the marked length) }. A Library
        /// drag onto the timeline edits in that range (ProjectFile.dropMedia).
        MarksRole,
    };

    explicit LibraryModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    struct Row {
        enum class Kind : std::uint8_t { Bin, Sequence, Media };
        Kind kind = Kind::Media;
        std::uint64_t id = 0;
        const Bin* bin = nullptr;
        const Sequence* sequence = nullptr;
        const MediaItem* media = nullptr;
    };

    /// Everything a refresh needs besides the snapshot.
    struct Context {
        std::filesystem::path projectFile;
        /// Media with a file on disk (not offline) and its resolved path, for thumbnails and hover scrub.
        std::function<QString(const MediaItem&)> thumbnail;
        /// A sequence's thumbnail (its rendered middle frame, I.9; "" for none).
        std::function<QString(const ProjectState&, const Sequence&)> sequenceThumbnail;
        const std::unordered_map<std::uint64_t, LibraryMediaStatus>* status = nullptr;
        const std::unordered_set<std::uint64_t>* used = nullptr;       ///< media used by some sequence
        const std::unordered_set<std::uint64_t>* duplicates = nullptr; ///< media sharing content with another
    };

    /// Lists `bin`'s contents (or, with an active `filter`, every matching item of the project) from `snapshot`.
    /// A search bin (0.2, P.3) lists what its query finds in the whole project, narrowed by an active `filter`.
    /// Keeps delegates (dataChanged) when the rows are the same items in the same order.
    void refresh(ProjectSnapshot snapshot, BinId bin, const LibraryFilter& filter, const Context& context);
    /// How many items of the last snapshot `search` finds (a search bin's count).
    int searchCount(const BinSearch& search) const;
    /// Row text of one cell (tests, the bench, data()).
    QString cellText(int row, LibraryColumn column) const;
    /// Whether `row` passes `filter` (and its Audio scope) with the last refresh's context (status, used, duplicates).
    bool passes(const Row& row, const LibraryFilter& filter) const { return inScope(row, filter) && matches(row, filter); }

    const std::vector<Row>& rows() const { return m_rows; }
    /// The row of a key, or -1; the key of a row, or "" (QML: keyboard navigation).
    Q_INVOKABLE int rowOf(const QString& key) const;
    Q_INVOKABLE QString keyOf(int row) const;
    const ProjectSnapshot& snapshot() const { return m_snapshot; }

    /// Visible columns in display order.
    const std::vector<LibraryColumn>& columns() const { return m_columns; }
    void setColumns(std::vector<LibraryColumn> columns);
    LibraryColumn sortColumn() const { return m_sortColumn; }
    bool sortAscending() const { return m_ascending; }
    void setSort(LibraryColumn column, bool ascending);

    /// Selection highlight (SelectedRole); the bridge owns the selection.
    void setSelection(const QSet<QString>& keys);
    /// Re-reads status-dependent cells (proxy, analysis) of every row.
    void statusChanged();

    static QString keyFor(const Row& row);
    static QString kindOf(const Row& row);

signals:
    void countChanged();

private:
    void sortRows(std::vector<Row>& rows) const;
    QVariant sortKey(const Row& row, LibraryColumn column) const;
    QString binPath(BinId bin) const;
    bool matches(const Row& row, const LibraryFilter& filter) const;
    static bool inScope(const Row& row, const LibraryFilter& filter);
    LibraryColumn columnAt(int section) const;

    ProjectSnapshot m_snapshot;
    std::vector<Row> m_rows;
    QHash<QString, int> m_index;
    std::vector<LibraryColumn> m_columns;
    LibraryColumn m_sortColumn = LibraryColumn::Name;
    bool m_ascending = true;
    bool m_filtering = false;
    Context m_context;
    QSet<QString> m_selection;
};

/// Premiere's label name of colour `index` (0..15: Violet .. Yellow), translated.
QString libraryLabelName(int index);

/// "1920 × 1080", "29.97 fps", "48 kHz stereo"... the formatters the list and the tests share.
QString libraryFrameRateText(const MediaItem& media);
QString libraryResolutionText(const MediaItem& media);
QString libraryAudioText(const MediaItem& media);

} // namespace wabcut::app
