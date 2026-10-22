#pragma once

// MediaBridge: the media layer's background services behind the timeline and the library.
//
// MediaPeakSource (PeakSource): waveform envelopes from media::WaveformPeaksService (decoded on worker threads,
// cached on disk under <cacheDir>/peaks). Media whose locator does not resolve to a file (the demo project) keep
// the PlaceholderPeakSource envelope. peaks() never blocks: the first call for a stream queues the job and returns
// false; when it finishes, peaksReady() fires on the GUI thread and the timeline rebuilds its waveforms. Long streams
// (a 2-hour VOD decodes for tens of seconds) are drawn while they decode: the service's partial results are the
// stream's start, exact as far as they go; a range past them stays blank (peaks() returns false) and
// buildingUntil() says how far they reach. Partial results fire peaksReady() at most every 250 ms for all streams
// together; a finished stream fires it at once. Views rebuild only the waveforms of the media that changed since they
// last looked (mediaChangedSince(), p02 audio); the timeline shades what is still building. Up to three
// streams decode at once, so the audio tracks of one recording build together.
//
// ThumbnailImageProvider ("image://wabcut-thumbnails/<percent-encoded UTF-8 path>"): library thumbnails from
// media::ThumbnailService (one frame from the middle of the file, scaled to the requested height, cached on disk
// under <cacheDir>/thumbnails). The URL carries a file path, never an id. 0.2 Media Browser hover scrub: ".../s<n>"
// asks for a strip - n frames evenly spaced through the file, side by side in one image (stripUrlFor) - so moving the
// pointer over a thumbnail only shows another part of it.
//
// Threading: both services run their jobs on their own worker threads. MediaPeakSource joins its workers before it
// is destroyed; a thumbnail response holds only a weak link to its QQuickImageResponse, so a job that finishes after
// the image request was dropped does nothing.

#include "PeakSource.h"

#include <wabcut/media/WaveformPeaks.h>

#include <QObject>
#include <QQuickAsyncImageProvider>
#include <QString>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wabcut::media {
class ThumbnailService;
} // namespace wabcut::media

namespace wabcut::app {

/// Fills `out` from a peak pyramid for the source range [sourceStart, sourceEnd); false when it has no data there.
bool fillFromWaveformPeaks(const media::WaveformPeaks& peaks, Ticks sourceStart, Ticks sourceEnd, std::span<float> out);

class MediaPeakSource final : public QObject, public PeakSource {
    Q_OBJECT

public:
    /// A media file the source may decode: its path and the container index of each audio stream
    /// (StreamRef::index -> MediaInfo::audio[i].index).
    struct MediaFile {
        std::string path;
        std::vector<int> audioStreams;
    };

    explicit MediaPeakSource(std::filesystem::path cacheDir, QObject* parent = nullptr);
    ~MediaPeakSource() override;

    /// Replaces the known files (GUI thread). Media not listed use the placeholder envelope.
    void setMediaFiles(std::unordered_map<std::uint64_t, MediaFile> files);

    bool peaks(MediaId media, StreamRef stream, Ticks sourceStart, Ticks sourceEnd,
               std::span<float> out) const override;

    /// While the peaks of `stream` are still being decoded: the source time the decoded part reaches (zero until the
    /// first partial result). nullopt once they are complete or failed, before they are first asked for, and for
    /// media without a file. Any thread.
    std::optional<Ticks> buildingUntil(MediaId media, StreamRef stream) const override;

    /// p02 audio: the media whose peaks (partial or complete) arrived, or whose file changed, after `serial`; nullopt
    /// when `serial` is older than the source's history. Any thread.
    std::optional<std::vector<std::uint64_t>> mediaChangedSince(std::uint64_t serial, std::uint64_t& now) const override;

    /// True when every requested stream has finished (tests).
    bool idle() const;

signals:
    /// New peaks are available, complete or partial (GUI thread).
    void peaksReady();

private:
    enum class State { Pending, Ready, Failed };
    struct Entry {
        State state = State::Pending;
        /// Ready: the peaks. Pending: the latest partial result, or null.
        std::shared_ptr<const media::WaveformPeaks> peaks;
        /// Tells this request's callbacks from those of an earlier request for the same key (the file changed).
        std::uint64_t ticket = 0;
        /// Cancelled when the file changes; dropped once the job is done (it holds a copy of the result).
        media::Job<media::WaveformPeaks> job;
    };
    using Key = std::pair<std::uint64_t, int>; // media id, StreamRef::index

    media::WaveformPeaksService& service() const;

    std::filesystem::path m_cacheDir;
    mutable std::mutex m_mutex;
    std::unordered_map<std::uint64_t, MediaFile> m_files;
    mutable std::map<Key, Entry> m_entries;
    mutable std::uint64_t m_nextTicket = 0;
    std::chrono::steady_clock::time_point m_lastPartialNotify{}; // partial results' peaksReady(), coalesced
    /// p02 audio: a serial per change of a media's peaks (mediaChangedSince), under m_mutex.
    std::uint64_t m_serial = 1;
    std::unordered_map<std::uint64_t, std::uint64_t> m_changedAt; // media id -> serial of its last change
    void markChanged(std::uint64_t media); // m_mutex held
    PlaceholderPeakSource m_placeholder;
    // Created on the first real request; destroyed first (joining its workers) so no callback outlives `this`.
    mutable std::unique_ptr<media::WaveformPeaksService> m_service;
};

class ThumbnailImageProvider final : public QQuickAsyncImageProvider {
public:
    static constexpr const char* kProviderId = "wabcut-thumbnails";

    explicit ThumbnailImageProvider(std::filesystem::path cacheDir);
    ~ThumbnailImageProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    /// "image://wabcut-thumbnails/…" for a UTF-8 file path.
    static QString urlFor(const std::string& utf8Path);
    /// 0.2: the strip of `count` (2 .. 32) frames of the file, side by side (hover scrub).
    static QString stripUrlFor(const std::string& utf8Path, int count);

private:
    media::ThumbnailService& service();

    std::filesystem::path m_cacheDir;
    std::mutex m_mutex;
    std::unique_ptr<media::ThumbnailService> m_service; // created on the first request
};

} // namespace wabcut::app
