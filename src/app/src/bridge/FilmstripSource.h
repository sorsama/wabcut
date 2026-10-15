#pragma once

// Video filmstrips for the timeline (M5i): thumbnails of a media item's video, packed into one atlas image per media
// item, for TimelineView to upload as a scene-graph texture.
//
//  * FilmstripSource is the seam (like PeakSource): lookup() is called from TimelineView::updatePaintNode on the
//    render thread, never blocks and never decodes. A miss starts an asynchronous request and answers Pending (the
//    view draws placeholder tiles); media without a file (the demo project, titles) is Unavailable (nothing drawn).
//  * MediaFilmstripSource extracts the frames with media::ThumbnailService (disk-cached, on its worker thread),
//    preferring a media item's proxy when it has one, and builds the atlas on that thread too. Decoded atlases are
//    kept in memory up to a byte budget, least recently used first out; an evicted atlas is requested again (from the
//    disk cache) when it is next needed. stripsReady() is emitted (queued, on the owner's thread) when atlases arrive.
//  * Nested and multicam clips (I.9) get the nested sequence's pictures (a multicam clip: its angle), rendered through
//    the export path on a worker thread of their own (NestedPictures.h), in the same budget. An edit anywhere the
//    picture depends on (a stamp of the nested subtree, its media and the redactions) renders the strip again; the old
//    strip shows until the new one arrives.

#include <wabcut/model/Clip.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <QImage>
#include <QObject>
#include <QString>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wabcut::media {
class ThumbnailService;
}

namespace wabcut::app {

/// One media item's thumbnails, tiled left to right, top to bottom in `atlas` (`columns` per row).
struct FilmstripAtlas {
    QImage atlas;
    int columns = 1;
    int tileWidth = 0;
    int tileHeight = 0;
    std::vector<Ticks> times; ///< source time of each tile, ascending
    /// Distinguishes atlases of the same media (a re-request after eviction or a new file).
    std::uint64_t serial = 0;

    int count() const { return static_cast<int>(times.size()); }
    /// The tile nearest to `sourceTime` (0 when there is none).
    int tileAt(Ticks sourceTime) const;
    /// Where tile `index` is in the atlas, in pixels.
    QRect tileRect(int index) const;
    std::size_t bytes() const { return static_cast<std::size_t>(atlas.sizeInBytes()); }
};

class FilmstripSource {
public:
    enum class State { Unavailable, Pending, Ready };
    struct Lookup {
        State state = State::Unavailable;
        std::shared_ptr<const FilmstripAtlas> atlas;
        /// Which strip it is (its texture's key): the media id, or a key of a nested clip's picture.
        std::uint64_t key = 0;
    };
    virtual ~FilmstripSource() = default;
    /// Thread-safe, non-blocking (render thread).
    virtual Lookup lookup(MediaId media) const = 0;
    /// The strip of a nested or multicam clip (I.9): its nested sequence's pictures in nested time (Clip::sourceTimeAt).
    /// Thread-safe, non-blocking.
    virtual Lookup lookupNested(const Clip& clip) const {
        (void)clip;
        return {};
    }
};

/// No files: every media item is Unavailable.
class NoFilmstripSource final : public FilmstripSource {
public:
    Lookup lookup(MediaId) const override { return {}; }
};

class MediaFilmstripSource : public QObject, public FilmstripSource {
    Q_OBJECT
public:
    struct MediaFile {
        std::string path;    ///< UTF-8; the proxy when the media item has one
        double seconds = 0;  ///< the media's length (how many thumbnails to take)
        /// The media's source redactions (redacted::keyOf): a change re-extracts its strip, redacted anew.
        std::uint64_t redactionKey = 0;
    };
    /// Frames of one request: images (any size) and their source times, or nullopt when extraction failed.
    struct Frames {
        std::vector<QImage> images;
        std::vector<Ticks> times;
    };
    using Done = std::function<void(std::optional<Frames>)>;
    /// Starts extracting `count` frames of `height` px from `path`; `done` may run on any thread, later.
    using Requester = std::function<void(const std::string& path, int count, int height, Done done)>;

    /// `cacheDir`: where media::ThumbnailService keeps its disk cache.
    explicit MediaFilmstripSource(std::filesystem::path cacheDir, QObject* parent = nullptr);
    /// Tests: frames come from `requester` instead of ThumbnailService.
    MediaFilmstripSource(Requester requester, QObject* parent = nullptr);
    ~MediaFilmstripSource() override;

    Lookup lookup(MediaId media) const override;
    Lookup lookupNested(const Clip& clip) const override;

    /// The media files that have thumbnails (from the project's snapshot). Atlases of files that changed are dropped.
    void setMediaFiles(std::unordered_map<std::uint64_t, MediaFile> files);
    /// The project nested strips are rendered from (any revision change; GUI thread) and its file (relative media
    /// paths). Strips whose pictures changed are rendered again.
    void setProject(ProjectSnapshot snapshot, std::filesystem::path projectFile);
    /// Renders `count` pictures of `shown` (a sequence of `state`) `height` px high on the worker; tests replace the
    /// export renderer with it.
    using NestedRenderer = std::function<std::optional<Frames>(const ProjectState& state, const Sequence& shown, int count,
                                                               int height, const std::stop_token& stop)>;
    void setNestedRenderer(NestedRenderer renderer);
    /// Decoded atlases kept in memory (default 128 MiB).
    void setBudget(std::size_t bytes);
    std::size_t budget() const;
    std::size_t bytesInMemory() const;
    /// Media with an atlas in memory, most recently used first (tests).
    std::vector<MediaId> residentMedia() const;
    /// The file thumbnails of `media` come from ("" without one; tests: proxies are preferred).
    std::string fileOf(MediaId media) const;

    /// Thumbnails per media item: one every 2 s, 8 to 240.
    static int thumbnailCount(double seconds);
    static constexpr int kThumbnailHeight = 72;

signals:
    /// Atlases arrived (queued to this object's thread).
    void stripsReady();

private:
    struct Entry {
        FilmstripSource::State state = FilmstripSource::State::Pending;
        std::shared_ptr<const FilmstripAtlas> atlas;
        std::uint64_t generation = 0; ///< the file's generation when requested (nested: the picture stamp shown)
        std::uint64_t pendingStamp = 0; ///< nested: the stamp being rendered (0: none)
    };
    /// A nested strip: which picture it is.
    struct NestedInfo {
        SequenceId sequence;
        std::vector<std::size_t> tracks; ///< the video tracks read (a multicam clip: its angle's)
    };
    struct NestedJob {
        std::uint64_t key = 0;
        std::uint64_t stamp = 0;
        ProjectSnapshot snapshot;
        std::filesystem::path projectFile;
        std::shared_ptr<const Sequence> shown;
        int count = 0;
    };
    void request(MediaId media, const MediaFile& file, std::uint64_t generation) const;
    void finish(MediaId media, std::uint64_t generation, std::optional<Frames> frames);
    void touch(std::uint64_t media) const; // m_mutex held
    void evictOverBudget();                 // m_mutex held
    std::uint64_t nestedStamp(const NestedInfo& info) const;          // m_mutex held; 0 = gone
    void enqueueNested(std::uint64_t key, const NestedInfo& info, std::uint64_t stamp) const; // m_mutex held
    void nestedWorker(std::stop_token stop);
    void finishNested(std::uint64_t key, std::uint64_t stamp, std::optional<Frames> frames);

    Requester m_requester;
    std::filesystem::path m_cacheDir;
    mutable std::unique_ptr<media::ThumbnailService> m_service; // created on the first request
    mutable std::mutex m_mutex;
    std::unordered_map<std::uint64_t, MediaFile> m_files;
    std::unordered_map<std::uint64_t, std::uint64_t> m_generations; // media -> file generation
    std::uint64_t m_nextGeneration = 1;
    mutable std::unordered_map<std::uint64_t, Entry> m_entries;
    mutable std::list<std::uint64_t> m_lru; // most recent first (Ready entries only)
    std::size_t m_budget = 128u << 20;
    std::size_t m_bytes = 0;
    std::uint64_t m_serial = 0;
    /// Shared with in-flight callbacks: they do nothing once the source is gone (its destructor clears `self`).
    struct Guard {
        std::mutex mutex;
        MediaFilmstripSource* self = nullptr;
    };
    std::shared_ptr<Guard> m_guard;

    // Nested strips (I.9), all under m_mutex.
    ProjectSnapshot m_project;
    std::filesystem::path m_projectFile;
    mutable std::unordered_map<std::uint64_t, NestedInfo> m_nested;
    mutable std::unordered_map<std::uint64_t, std::uint64_t> m_stampMemo; ///< sequence -> picture stamp, this revision
    mutable std::deque<NestedJob> m_nestedJobs;
    mutable std::condition_variable_any m_nestedWake;
    NestedRenderer m_nestedRenderer;
    /// Started on the first nested strip; declared last: stopped and joined before anything above goes.
    mutable std::jthread m_nestedThread;
};

/// Packs frames into an atlas (a grid at most 16 tiles wide; every tile scaled to `height`, keeping the first frame's
/// aspect). Thread-safe (QImage only).
std::shared_ptr<FilmstripAtlas> buildFilmstripAtlas(const std::vector<QImage>& frames, std::vector<Ticks> times,
                                                    int height);

} // namespace wabcut::app
