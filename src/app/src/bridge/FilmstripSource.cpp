#include "FilmstripSource.h"

#include "NestedPictures.h"
#include "RedactedPictures.h"

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/media/ThumbnailService.h>
#include <wabcut/model/Resolve.h>

#include <QMetaObject>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

// ---- Atlas ------------------------------------------------------------------------------------------------------

int FilmstripAtlas::tileAt(Ticks sourceTime) const {
    if (times.empty())
        return 0;
    const auto it = std::lower_bound(times.begin(), times.end(), sourceTime);
    if (it == times.begin())
        return 0;
    if (it == times.end())
        return count() - 1;
    const auto before = std::prev(it);
    return static_cast<int>((sourceTime - *before) <= (*it - sourceTime) ? before - times.begin() : it - times.begin());
}

QRect FilmstripAtlas::tileRect(int index) const {
    const int column = columns > 0 ? index % columns : 0;
    const int row = columns > 0 ? index / columns : 0;
    return {column * tileWidth, row * tileHeight, tileWidth, tileHeight};
}

std::shared_ptr<FilmstripAtlas> buildFilmstripAtlas(const std::vector<QImage>& frames, std::vector<Ticks> times,
                                                    int height) {
    auto atlas = std::make_shared<FilmstripAtlas>();
    const std::size_t n = std::min(frames.size(), times.size());
    if (n == 0 || height <= 0)
        return atlas;
    const QImage& first = frames.front();
    const double aspect = first.height() > 0 ? static_cast<double>(first.width()) / first.height() : 16.0 / 9.0;
    atlas->tileHeight = height;
    atlas->tileWidth = std::max(1, static_cast<int>(std::lround(height * std::clamp(aspect, 0.2, 5.0))));
    atlas->columns = static_cast<int>(std::min<std::size_t>(16, n));
    const int rows = static_cast<int>((n + atlas->columns - 1) / atlas->columns);
    atlas->atlas = QImage(atlas->columns * atlas->tileWidth, rows * atlas->tileHeight, QImage::Format_RGBA8888_Premultiplied);
    atlas->atlas.fill(Qt::black);
    QPainter painter(&atlas->atlas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (std::size_t i = 0; i < n; ++i)
        painter.drawImage(atlas->tileRect(static_cast<int>(i)), frames[i]);
    painter.end();
    times.resize(n);
    atlas->times = std::move(times);
    return atlas;
}

// ---- MediaFilmstripSource ----------------------------------------------------------------------------------------

namespace {

MediaFilmstripSource::Requester serviceRequester(std::unique_ptr<media::ThumbnailService>& service,
                                                 std::mutex& mutex, const std::filesystem::path& cacheDir) {
    return [&service, &mutex, cacheDir](const std::string& path, int count, int height,
                                        MediaFilmstripSource::Done done) {
        media::ThumbnailService* svc = nullptr;
        {
            std::lock_guard lock(mutex);
            if (!service)
                service = std::make_unique<media::ThumbnailService>(cacheDir, 1);
            svc = service.get();
        }
        media::ThumbnailOptions options;
        options.count = count;
        options.height = height;
        svc->request(path, options, [done = std::move(done), path](const std::expected<media::ThumbnailStrip, Error>& result) {
            if (!result) {
                done(std::nullopt);
                return;
            }
            MediaFilmstripSource::Frames frames;
            for (const media::Thumbnail& t : result->thumbnails) {
                if (t.width <= 0 || t.height <= 0 || t.rgba.size() < static_cast<std::size_t>(t.width) * t.height * 4)
                    continue;
                QImage image = QImage(t.rgba.data(), t.width, t.height, t.width * 4, QImage::Format_RGBA8888).copy();
                redacted::applyToFile(image, path, t.time); // source redactions (Codex wave-4 P2-9)
                frames.images.push_back(std::move(image));
                frames.times.push_back(t.time);
            }
            done(std::move(frames));
        });
    };
}

} // namespace

MediaFilmstripSource::MediaFilmstripSource(std::filesystem::path cacheDir, QObject* parent)
    : QObject(parent), m_cacheDir(std::move(cacheDir)), m_guard(std::make_shared<Guard>()) {
    m_guard->self = this;
    m_requester = serviceRequester(m_service, m_mutex, m_cacheDir);
}

MediaFilmstripSource::MediaFilmstripSource(Requester requester, QObject* parent)
    : QObject(parent), m_requester(std::move(requester)), m_guard(std::make_shared<Guard>()) {
    m_guard->self = this;
}

MediaFilmstripSource::~MediaFilmstripSource() {
    if (m_nestedThread.joinable()) { // a nested strip being rendered stops after its current frame
        m_nestedThread.request_stop();
        m_nestedWake.notify_all();
        m_nestedThread.join();
    }
    {
        std::lock_guard lock(m_guard->mutex);
        m_guard->self = nullptr;
    }
    m_service.reset(); // cancels and joins the worker: no callback runs after this
}

int MediaFilmstripSource::thumbnailCount(double seconds) {
    return static_cast<int>(std::clamp(std::ceil(seconds / 2.0), 8.0, 240.0));
}

FilmstripSource::Lookup MediaFilmstripSource::lookup(MediaId media) const {
    MediaFile file;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(m_mutex);
        const auto f = m_files.find(media.value);
        if (f == m_files.end())
            return {};
        if (const auto e = m_entries.find(media.value); e != m_entries.end()) {
            if (e->second.state == State::Ready)
                touch(media.value);
            return {e->second.state, e->second.atlas, media.value};
        }
        file = f->second;
        if (const auto g = m_generations.find(media.value); g != m_generations.end())
            generation = g->second;
        m_entries[media.value] = Entry{State::Pending, nullptr, generation};
    }
    request(media, file, generation); // outside the lock: a synchronous requester calls finish() at once
    std::lock_guard lock(m_mutex);
    const Entry& e = m_entries[media.value];
    return {e.state, e.atlas, media.value};
}

// ---- Nested strips (I.9) -------------------------------------------------------------------------------------------

namespace {

/// Nested strips share the media strips' tables: their keys have the top bit set (media ids never do).
constexpr std::uint64_t kNestedKeyBit = std::uint64_t{1} << 63;

std::uint64_t mixKey(std::uint64_t key, std::uint64_t value) {
    key ^= value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
    return key * 0xFF51AFD7ED558CCDull;
}

std::uint64_t nestedKeyOf(SequenceId sequence, const std::vector<std::size_t>& tracks) {
    std::uint64_t key = mixKey(0x4E57'0000ull, sequence.value);
    for (const std::size_t t : tracks)
        key = mixKey(key, t + 1);
    return key | kNestedKeyBit;
}

} // namespace

std::uint64_t MediaFilmstripSource::nestedStamp(const NestedInfo& info) const {
    if (!m_project)
        return 0;
    auto [memo, inserted] = m_stampMemo.try_emplace(info.sequence.value, 0);
    if (inserted)
        memo->second = sequencePictureStamp(*m_project, info.sequence);
    if (memo->second == 0)
        return 0;
    std::uint64_t stamp = memo->second;
    for (const std::size_t t : info.tracks)
        stamp = mixKey(stamp, t + 1);
    return stamp == 0 ? 1 : stamp;
}

void MediaFilmstripSource::enqueueNested(std::uint64_t key, const NestedInfo& info, std::uint64_t stamp) const {
    const Sequence* nested = m_project ? m_project->findSequence(info.sequence) : nullptr;
    if (!nested)
        return;
    auto shown = std::make_shared<Sequence>(*nested);
    for (std::size_t i = 0; i < shown->videoTracks.size(); ++i) {
        if (!shown->videoTracks[i] || std::ranges::find(info.tracks, i) != info.tracks.end())
            continue;
        auto off = std::make_shared<Track>(*shown->videoTracks[i]);
        off->enabled = false; // a multicam clip shows one angle
        shown->videoTracks[i] = std::move(off);
    }
    NestedJob job{key, stamp, m_project, m_projectFile, std::move(shown), thumbnailCount(nested->duration.toSeconds())};
    // One job per strip: a newer revision replaces a queued one.
    const auto queued = std::ranges::find_if(m_nestedJobs, [&](const NestedJob& j) { return j.key == key; });
    if (queued != m_nestedJobs.end())
        *queued = std::move(job);
    else
        m_nestedJobs.push_back(std::move(job));
    if (!m_nestedThread.joinable())
        m_nestedThread = std::jthread([self = const_cast<MediaFilmstripSource*>(this)](std::stop_token stop) {
            self->nestedWorker(std::move(stop));
        });
    m_nestedWake.notify_one();
}

FilmstripSource::Lookup MediaFilmstripSource::lookupNested(const Clip& clip) const {
    if (!clip.isNested())
        return {};
    std::lock_guard lock(m_mutex);
    const Sequence* nested = m_project ? m_project->findSequence(clip.sequence) : nullptr;
    if (!nested || nested->duration.value <= 0 || nested->width <= 0 || nested->height <= 0)
        return {};
    NestedInfo info{clip.sequence, nestedTracksRead(*nested, clip)};
    const std::uint64_t key = nestedKeyOf(info.sequence, info.tracks);
    const std::uint64_t stamp = nestedStamp(info);
    if (stamp == 0)
        return {};
    auto [entry, inserted] = m_entries.try_emplace(key, Entry{State::Pending, nullptr, stamp, 0});
    Entry& e = entry->second;
    if (inserted || (e.generation != stamp && e.pendingStamp != stamp)) {
        // New, or what it shows changed: (re)render; the strip shown so far stays until the new one arrives.
        e.pendingStamp = stamp;
        m_nested[key] = info;
        enqueueNested(key, info, stamp);
    }
    if (e.state == State::Ready)
        touch(key);
    return {e.state, e.atlas, key};
}

void MediaFilmstripSource::setProject(ProjectSnapshot snapshot, std::filesystem::path projectFile) {
    std::lock_guard lock(m_mutex);
    m_project = std::move(snapshot);
    m_projectFile = std::move(projectFile);
    m_stampMemo.clear();
    // Strips already made whose pictures changed (an edit inside a nested sequence leaves the clip as it was, so the
    // timeline would not ask again): render them again now.
    for (auto it = m_nested.begin(); it != m_nested.end();) {
        const auto e = m_entries.find(it->first);
        const std::uint64_t stamp = nestedStamp(it->second);
        if (e == m_entries.end() || stamp == 0) {
            if (e != m_entries.end()) {
                if (e->second.atlas)
                    m_bytes -= std::min(m_bytes, e->second.atlas->bytes());
                m_lru.remove(it->first);
                m_entries.erase(e);
            }
            it = m_nested.erase(it); // evicted or gone: the next lookup starts it again
            continue;
        }
        if (e->second.generation != stamp && e->second.pendingStamp != stamp) {
            e->second.pendingStamp = stamp;
            enqueueNested(it->first, it->second, stamp);
        }
        ++it;
    }
}

void MediaFilmstripSource::setNestedRenderer(NestedRenderer renderer) {
    std::lock_guard lock(m_mutex);
    m_nestedRenderer = std::move(renderer);
}

void MediaFilmstripSource::nestedWorker(std::stop_token stop) {
    for (;;) {
        NestedJob job;
        NestedRenderer renderer;
        {
            std::unique_lock lock(m_mutex);
            if (!m_nestedWake.wait(lock, stop, [&] { return !m_nestedJobs.empty(); }))
                return;
            job = std::move(m_nestedJobs.front());
            m_nestedJobs.pop_front();
            renderer = m_nestedRenderer;
        }
        std::optional<Frames> frames;
        if (job.snapshot && job.shown) {
            const ProjectState checked = editor::withMediaChecked(*job.snapshot, job.projectFile);
            if (renderer) {
                frames = renderer(checked, *job.shown, job.count, kThumbnailHeight, stop);
            } else if (auto pictures = renderSequencePictures(checked, *job.shown, job.count, kThumbnailHeight, stop)) {
                frames = Frames{std::move(pictures->images), std::move(pictures->times)};
            }
        }
        if (stop.stop_requested())
            return;
        finishNested(job.key, job.stamp, std::move(frames));
    }
}

void MediaFilmstripSource::finishNested(std::uint64_t key, std::uint64_t stamp, std::optional<Frames> frames) {
    std::shared_ptr<FilmstripAtlas> atlas; // built outside the lock, on the worker
    if (frames && !frames->images.empty())
        atlas = buildFilmstripAtlas(frames->images, std::move(frames->times), kThumbnailHeight);
    {
        std::lock_guard lock(m_mutex);
        const auto e = m_entries.find(key);
        if (e == m_entries.end())
            return; // evicted or gone meanwhile
        Entry& entry = e->second;
        if (entry.pendingStamp == stamp)
            entry.pendingStamp = 0;
        if (atlas && atlas->count() > 0) {
            atlas->serial = ++m_serial;
            if (entry.atlas)
                m_bytes -= std::min(m_bytes, entry.atlas->bytes());
            else
                m_lru.push_front(key);
            entry.atlas = atlas;
            entry.state = State::Ready;
            entry.generation = stamp;
            m_bytes += atlas->bytes();
            touch(key);
            evictOverBudget();
        } else if (!entry.atlas) {
            entry.state = State::Unavailable; // e.g. offline media inside: no strip (never one without its redactions)
            entry.generation = stamp;
        }
    }
    QMetaObject::invokeMethod(this, &MediaFilmstripSource::stripsReady, Qt::QueuedConnection);
}

void MediaFilmstripSource::request(MediaId media, const MediaFile& file, std::uint64_t generation) const {
    std::weak_ptr<Guard> guard = m_guard;
    m_requester(file.path, thumbnailCount(file.seconds), kThumbnailHeight,
                [guard, media, generation](std::optional<Frames> frames) {
                    const auto g = guard.lock();
                    if (!g)
                        return;
                    std::lock_guard lock(g->mutex);
                    if (g->self)
                        g->self->finish(media, generation, std::move(frames));
                });
}

void MediaFilmstripSource::finish(MediaId media, std::uint64_t generation, std::optional<Frames> frames) {
    // Build the atlas outside the lock (QPainter on this worker thread, never the GUI thread).
    std::shared_ptr<FilmstripAtlas> atlas;
    if (frames && !frames->images.empty())
        atlas = buildFilmstripAtlas(frames->images, std::move(frames->times), kThumbnailHeight);
    {
        std::lock_guard lock(m_mutex);
        const auto e = m_entries.find(media.value);
        if (e == m_entries.end() || e->second.generation != generation || e->second.state != State::Pending)
            return; // the file changed or the media is gone
        if (!atlas || atlas->count() == 0) {
            e->second.state = State::Unavailable;
        } else {
            atlas->serial = ++m_serial;
            e->second.state = State::Ready;
            e->second.atlas = atlas;
            m_bytes += atlas->bytes();
            m_lru.push_front(media.value);
            evictOverBudget();
        }
    }
    QMetaObject::invokeMethod(this, &MediaFilmstripSource::stripsReady, Qt::QueuedConnection);
}

void MediaFilmstripSource::touch(std::uint64_t media) const {
    const auto it = std::find(m_lru.begin(), m_lru.end(), media);
    if (it != m_lru.begin() && it != m_lru.end())
        m_lru.splice(m_lru.begin(), m_lru, it);
}

void MediaFilmstripSource::evictOverBudget() {
    // Keep at least the most recent atlas, whatever its size.
    while (m_bytes > m_budget && m_lru.size() > 1) {
        const std::uint64_t victim = m_lru.back();
        m_lru.pop_back();
        if (const auto e = m_entries.find(victim); e != m_entries.end()) {
            if (e->second.atlas)
                m_bytes -= std::min(m_bytes, e->second.atlas->bytes());
            m_entries.erase(e); // requested again (from the disk cache) when next needed
        }
    }
}

void MediaFilmstripSource::setMediaFiles(std::unordered_map<std::uint64_t, MediaFile> files) {
    std::lock_guard lock(m_mutex);
    for (const auto& [id, file] : files) {
        const auto old = m_files.find(id);
        if (old == m_files.end() || old->second.path != file.path || old->second.redactionKey != file.redactionKey)
            m_generations[id] = m_nextGeneration++;
    }
    m_files = std::move(files);
    // Drop atlases of media that are gone or whose file changed (nested strips follow setProject()).
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if ((it->first & kNestedKeyBit) != 0) {
            ++it;
            continue;
        }
        const bool stale = !m_files.contains(it->first) || it->second.generation != m_generations[it->first];
        if (!stale) {
            ++it;
            continue;
        }
        if (it->second.atlas)
            m_bytes -= std::min(m_bytes, it->second.atlas->bytes());
        m_lru.remove(it->first);
        it = m_entries.erase(it);
    }
}

void MediaFilmstripSource::setBudget(std::size_t bytes) {
    std::lock_guard lock(m_mutex);
    m_budget = bytes;
    evictOverBudget();
}

std::size_t MediaFilmstripSource::budget() const {
    std::lock_guard lock(m_mutex);
    return m_budget;
}

std::size_t MediaFilmstripSource::bytesInMemory() const {
    std::lock_guard lock(m_mutex);
    return m_bytes;
}

std::string MediaFilmstripSource::fileOf(MediaId media) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_files.find(media.value);
    return it == m_files.end() ? std::string() : it->second.path;
}

std::vector<MediaId> MediaFilmstripSource::residentMedia() const {
    std::lock_guard lock(m_mutex);
    std::vector<MediaId> out;
    for (const std::uint64_t id : m_lru)
        out.push_back(MediaId{id});
    return out;
}

} // namespace wabcut::app
