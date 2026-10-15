#include "MediaBridge.h"

#include "RedactedPictures.h"

#include <wabcut/media/ThumbnailService.h>
#include <wabcut/media/WaveformPeaks.h>

#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace wabcut::app {

// ---- Waveform peaks ---------------------------------------------------------------------------------------------

namespace {

/// Partial results of a long stream: one snapshot per stream this often, and one peaksReady() for all of them.
constexpr std::chrono::milliseconds kPartialInterval{250};

/// Streams decoded at once: the three audio tracks of an OBS / ShadowPlay recording build together (each pass reads
/// the whole file; passes that run together share the OS file cache instead of reading it three times), bounded so
/// peaks never take more than a few cores while editing.
int peakWorkerThreads() {
    const int cpus = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return std::clamp(cpus / 2, 1, 3);
}

/// The source time a (partial) result reaches.
Ticks reachOf(const media::WaveformPeaks& peaks) {
    return peaks.sampleRate.isValid() ? samplesToTicks(peaks.sampleCount, peaks.sampleRate) : Ticks{};
}

} // namespace

bool fillFromWaveformPeaks(const media::WaveformPeaks& w, Ticks sourceStart, Ticks sourceEnd, std::span<float> out) {
    if (w.channels <= 0 || w.levels.empty() || out.empty() || sourceEnd <= sourceStart)
        return false;
    const double rate = w.sampleRate.toDouble();
    const double s0 = sourceStart.toSeconds() * rate;
    const double s1 = sourceEnd.toSeconds() * rate;
    const double perColumn = (s1 - s0) / static_cast<double>(out.size());
    const media::PeakLevel* level = w.levelFor(perColumn);
    if (!level || level->samplesPerBin <= 0 || level->binCount <= 0)
        return false;
    const double binWidth = level->samplesPerBin;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double c0 = s0 + perColumn * static_cast<double>(i);
        const auto b0 = std::clamp(static_cast<std::int64_t>(std::floor(c0 / binWidth)), std::int64_t{0}, level->binCount);
        const auto b1 = std::clamp(std::max(b0 + 1, static_cast<std::int64_t>(std::ceil((c0 + perColumn) / binWidth))),
                                   std::int64_t{0}, level->binCount);
        int peak = 0;
        for (std::int64_t b = b0; b < b1; ++b) {
            for (int c = 0; c < w.channels; ++c) {
                peak = std::max({peak, std::abs(static_cast<int>(level->minAt(b, c, w.channels))),
                                 std::abs(static_cast<int>(level->maxAt(b, c, w.channels)))});
            }
        }
        out[i] = std::min(1.0f, static_cast<float>(peak) / 32767.0f);
    }
    return true;
}

MediaPeakSource::MediaPeakSource(std::filesystem::path cacheDir, QObject* parent)
    : QObject(parent), m_cacheDir(std::move(cacheDir)) {}

MediaPeakSource::~MediaPeakSource() {
    // Cancel and join the workers while the entries their callbacks write to still exist.
    std::unique_ptr<media::WaveformPeaksService> service;
    {
        std::lock_guard lock(m_mutex);
        service = std::move(m_service);
    }
    service.reset();
}

void MediaPeakSource::setMediaFiles(std::unordered_map<std::uint64_t, MediaFile> files) {
    std::lock_guard lock(m_mutex);
    // Forget results for media whose file changed or disappeared (and stop decoding them); keep the rest.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        const auto before = m_files.find(it->first.first);
        const auto after = files.find(it->first.first);
        const bool same = before != m_files.end() && after != files.end() && before->second.path == after->second.path;
        if (same) {
            ++it;
        } else {
            it->second.job.cancel();
            it = m_entries.erase(it);
        }
    }
    m_files = std::move(files);
}

media::WaveformPeaksService& MediaPeakSource::service() const {
    if (!m_service)
        m_service = std::make_unique<media::WaveformPeaksService>(m_cacheDir, peakWorkerThreads());
    return *m_service;
}

bool MediaPeakSource::peaks(MediaId media, StreamRef stream, Ticks sourceStart, Ticks sourceEnd,
                            std::span<float> out) const {
    std::shared_ptr<const media::WaveformPeaks> available;
    bool partial = false;
    std::string path;
    media::WaveformOptions options;
    std::uint64_t ticket = 0;
    {
        std::lock_guard lock(m_mutex);
        const auto file = m_files.find(media.value);
        if (file == m_files.end() || stream.kind != StreamKind::Audio)
            return m_placeholder.peaks(media, stream, sourceStart, sourceEnd, out);
        const Key key{media.value, stream.index};
        if (const auto it = m_entries.find(key); it != m_entries.end()) {
            if (it->second.state == State::Failed || !it->second.peaks)
                return false;
            available = it->second.peaks;
            partial = it->second.state == State::Pending;
        } else {
            // First request for this stream: queue the job below (outside the lock: the service may call back at
            // once, e.g. for a cache hit).
            Entry entry;
            entry.ticket = ticket = ++m_nextTicket;
            m_entries.emplace(key, std::move(entry));
            path = file->second.path;
            const auto& streams = file->second.audioStreams;
            options.stream = stream.index >= 0 && static_cast<std::size_t>(stream.index) < streams.size()
                                 ? streams[static_cast<std::size_t>(stream.index)]
                                 : -1;
        }
    }
    if (available) {
        // A partial result is the start of the stream: a range wholly past it stays blank until it gets there (the
        // part of a range past it is drawn flat until the next snapshot).
        if (partial && sourceStart >= reachOf(*available))
            return false;
        return fillFromWaveformPeaks(*available, sourceStart, sourceEnd, out);
    }

    const Key key{media.value, stream.index};
    auto* self = const_cast<MediaPeakSource*>(this); // the callbacks update the cache and notify the GUI
    media::WaveformPartials partials;
    partials.interval = kPartialInterval;
    partials.onPartial = [self, key, ticket](std::shared_ptr<const media::WaveformPeaks> snapshot) {
        bool notify = false;
        {
            std::lock_guard lock(self->m_mutex);
            const auto it = self->m_entries.find(key);
            if (it == self->m_entries.end() || it->second.ticket != ticket || it->second.state != State::Pending)
                return; // the file changed meanwhile
            // The previous snapshot (megabytes for a long stream) is released below, outside the lock.
            std::swap(it->second.peaks, snapshot);
            // Every peaksReady() rebuilds all the timeline's waveforms: partial results of every stream share one
            // notification per interval (the next snapshot or the final result brings the rest).
            const auto now = std::chrono::steady_clock::now();
            if (now - self->m_lastPartialNotify >= kPartialInterval) {
                self->m_lastPartialNotify = now;
                notify = true;
            }
        }
        snapshot.reset();
        if (notify)
            QMetaObject::invokeMethod(self, &MediaPeakSource::peaksReady, Qt::QueuedConnection);
    };
    media::WaveformPeaksService* svc = nullptr;
    {
        std::lock_guard lock(m_mutex);
        svc = &service();
    }
    media::Job<media::WaveformPeaks> job = svc->request(
        path, options,
        [self, key, ticket](const std::expected<media::WaveformPeaks, Error>& result) {
            // Copied (and the replaced snapshot released) outside the lock the render and GUI threads take.
            std::shared_ptr<const media::WaveformPeaks> ready =
                result ? std::make_shared<const media::WaveformPeaks>(*result) : nullptr;
            media::Job<media::WaveformPeaks> finished;
            {
                std::lock_guard lock(self->m_mutex);
                const auto it = self->m_entries.find(key);
                if (it == self->m_entries.end() || it->second.ticket != ticket)
                    return; // the file changed meanwhile
                it->second.state = result ? State::Ready : State::Failed;
                std::swap(it->second.peaks, ready);
                std::swap(it->second.job, finished); // the job holds its own copy of the result: not kept
            }
            ready.reset();
            QMetaObject::invokeMethod(self, &MediaPeakSource::peaksReady, Qt::QueuedConnection);
        },
        std::move(partials));
    {
        std::lock_guard lock(m_mutex);
        // Kept for cancelling while it runs (a job that already finished is not kept: it holds its result).
        if (const auto it = m_entries.find(key);
            it != m_entries.end() && it->second.ticket == ticket && it->second.state == State::Pending)
            it->second.job = std::move(job);
    }
    return false;
}

std::optional<Ticks> MediaPeakSource::buildingUntil(MediaId media, StreamRef stream) const {
    std::lock_guard lock(m_mutex);
    if (stream.kind != StreamKind::Audio || !m_files.contains(media.value))
        return std::nullopt;
    const auto it = m_entries.find(Key{media.value, stream.index});
    if (it == m_entries.end() || it->second.state != State::Pending)
        return std::nullopt;
    return it->second.peaks ? reachOf(*it->second.peaks) : Ticks{};
}

bool MediaPeakSource::idle() const {
    std::lock_guard lock(m_mutex);
    return std::ranges::none_of(m_entries, [](const auto& e) { return e.second.state == State::Pending; });
}

// ---- Thumbnails ---------------------------------------------------------------------------------------------------

namespace {

class ThumbnailResponse final : public QQuickImageResponse {
public:
    ThumbnailResponse(media::ThumbnailService& service, const std::string& path, int height)
        : m_link(std::make_shared<Link>()) {
        m_link->target = this;
        media::ThumbnailOptions options;
        options.count = 1; // the frame in the middle of the file
        options.height = std::clamp(height, 8, 2160);
        m_job = service.request(path, options, [link = m_link, path](const std::expected<media::ThumbnailStrip, Error>& result) {
            QImage image;
            QString error;
            if (result && !result->thumbnails.empty()) {
                const media::Thumbnail& t = result->thumbnails.front();
                image = QImage(t.rgba.data(), t.width, t.height, t.width * 4, QImage::Format_RGBA8888).copy();
                redacted::applyToFile(image, path, t.time); // source redactions (Codex wave-4 P2-9)
            } else {
                error = result ? QStringLiteral("no frame") : QString::fromStdString(result.error().message);
            }
            std::lock_guard lock(link->mutex);
            if (ThumbnailResponse* target = link->target) {
                QMetaObject::invokeMethod(
                    target,
                    [target, image = std::move(image), error = std::move(error)] {
                        target->m_image = image;
                        target->m_error = error;
                        emit target->finished();
                    },
                    Qt::QueuedConnection);
            }
        });
    }

    ~ThumbnailResponse() override {
        std::lock_guard lock(m_link->mutex);
        m_link->target = nullptr;
        m_job.cancel();
    }

    QQuickTextureFactory* textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }
    void cancel() override { m_job.cancel(); }

private:
    struct Link {
        std::mutex mutex;
        ThumbnailResponse* target = nullptr;
    };
    std::shared_ptr<Link> m_link;
    media::Job<media::ThumbnailStrip> m_job;
    QImage m_image;
    QString m_error;
};

} // namespace

ThumbnailImageProvider::ThumbnailImageProvider(std::filesystem::path cacheDir) : m_cacheDir(std::move(cacheDir)) {}

ThumbnailImageProvider::~ThumbnailImageProvider() = default;

media::ThumbnailService& ThumbnailImageProvider::service() {
    std::lock_guard lock(m_mutex);
    if (!m_service)
        m_service = std::make_unique<media::ThumbnailService>(m_cacheDir, 1);
    return *m_service;
}

QQuickImageResponse* ThumbnailImageProvider::requestImageResponse(const QString& id, const QSize& requestedSize) {
    // "<percent-encoded path>[/r<redaction key>]": the path has its '/' encoded, so a raw '/' starts the cache-busting
    // key urlFor() adds while the file has redactions.
    const qsizetype slash = id.lastIndexOf(QLatin1Char('/'));
    const QString encoded = slash >= 0 ? id.left(slash) : id;
    const std::string path = QUrl::fromPercentEncoding(encoded.toUtf8()).toStdString();
    const int height = requestedSize.height() > 0 ? requestedSize.height() : 180;
    return new ThumbnailResponse(service(), path, height);
}

QString ThumbnailImageProvider::urlFor(const std::string& utf8Path) {
    QString url = QStringLiteral("image://%1/%2")
                      .arg(QLatin1String(kProviderId),
                           QString::fromLatin1(QUrl::toPercentEncoding(QString::fromStdString(utf8Path))));
    // A file with source redactions gets their key in the URL: a changed redaction is a new picture (Codex wave-4 P2-9).
    if (const std::uint64_t key = redacted::keyOfFile(utf8Path)) {
        url += QStringLiteral("/r") + QString::number(key, 16);
    }
    return url;
}

} // namespace wabcut::app
