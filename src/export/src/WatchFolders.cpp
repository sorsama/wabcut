#include <wabcut/export/WatchFolders.h>

#include <wabcut/media/Probe.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <format>
#include <system_error>

namespace wabcut::exporter {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::filesystem::path outputFolderOf(const WatchFolder& w) {
    return w.output.empty() ? w.folder / "Output" : w.output;
}

/// "<folder>/<stem><ext>", numbered "<stem>_1<ext>", ... when taken.
std::filesystem::path freeName(const std::filesystem::path& folder, const std::filesystem::path& stem, std::string_view ext) {
    std::error_code ec;
    std::filesystem::path candidate = folder / stem;
    candidate += std::string(ext);
    for (int n = 1; std::filesystem::exists(candidate, ec) && n < 10000; ++n) {
        candidate = folder / stem;
        candidate += std::format("_{}", n);
        candidate += std::string(ext);
    }
    return candidate;
}

} // namespace

bool isWatchedMediaFile(const std::filesystem::path& file) {
    static constexpr std::array<std::string_view, 24> kExtensions = {
        ".mp4", ".mov", ".mkv", ".m4v", ".mxf", ".avi", ".webm", ".mts", ".m2ts", ".ts", ".wav", ".mp3",
        ".m4a", ".aif", ".aiff", ".flac", ".ogg", ".opus", ".wmv", ".mpg", ".mpeg", ".3gp", ".flv", ".dv"};
    const std::string ext = lower(pathToUtf8(file.extension()));
    return std::find(kExtensions.begin(), kExtensions.end(), ext) != kExtensions.end();
}

std::expected<ProjectState, Error> projectForMedia(const std::filesystem::path& file) {
    const std::string path = pathToUtf8(file);
    auto info = media::probe(path);
    if (!info) {
        return std::unexpected(info.error());
    }
    if (!info->hasVideo() && !info->hasAudio()) {
        return std::unexpected(Error{ErrorCode::Unsupported, path + " has no video or audio"});
    }
    ProjectState state;
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = pathToUtf8(file.filename());
    item.locator.path = path;
    item.info = *info;
    const MediaId media = item.id;
    state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));

    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = pathToUtf8(file.stem());
    Ticks duration = info->duration;
    if (info->hasVideo()) {
        const VideoStreamInfo& v = info->video.front();
        const bool sideways = v.rotationDegrees == 90 || v.rotationDegrees == 270;
        seq.width = std::max(16, (sideways ? v.height : v.width) & ~1);
        seq.height = std::max(16, (sideways ? v.width : v.height) & ~1);
        if (v.frameRate.isValid() && v.frameRate.toDouble() <= 240.0) {
            seq.frameRate = v.frameRate;
        }
        if (v.duration.value > 0) {
            duration = v.duration;
        }
    }
    if (duration.value <= 0) {
        return std::unexpected(Error{ErrorCode::Unsupported, path + " has no duration"});
    }
    seq.duration = duration;
    const auto track = [&](TrackKind kind, int stream, Ticks length) {
        Clip clip;
        clip.id = state.ids.allocate<ClipId>();
        clip.media = media;
        clip.stream = {kind == TrackKind::Video ? StreamKind::Video : StreamKind::Audio, stream};
        clip.timelineRange = {Ticks{}, length};
        Track t;
        t.id = state.ids.allocate<TrackId>();
        t.kind = kind;
        t.clips.push_back(std::move(clip));
        return std::make_shared<const Track>(std::move(t));
    };
    if (info->hasVideo()) {
        seq.videoTracks.push_back(track(TrackKind::Video, 0, duration));
    }
    for (std::size_t a = 0; a < info->audio.size() && a < 16; ++a) {
        const Ticks length = info->audio[a].duration.value > 0 ? std::min(info->audio[a].duration, duration) : duration;
        seq.audioTracks.push_back(track(TrackKind::Audio, static_cast<int>(a), length));
    }
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

struct WatchFolderService::Core {
    struct Seen {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type time{};
        int stableScans = 0;
        bool taken = false; ///< queued (or done / failed): not taken again unless it changes
    };
    struct Folder {
        WatchFolder folder;
        std::map<std::filesystem::path, Seen> seen;
        int queued = 0;
        int done = 0;
        int failed = 0;
        std::string lastError;
    };
    std::mutex mutex;
    std::map<std::uint64_t, Folder> folders;
    std::uint64_t nextId = 1;

    /// A watch-folder export ended: count it, move its source into "Source" when it succeeded.
    void finished(std::uint64_t id, const std::filesystem::path& source, bool succeeded, const std::string& error) {
        std::lock_guard lock(mutex);
        const auto it = folders.find(id);
        if (it == folders.end()) {
            return;
        }
        Folder& f = it->second;
        f.queued = std::max(0, f.queued - 1);
        if (!succeeded) {
            ++f.failed;
            f.lastError = std::format("{}: {}", pathToUtf8(source.filename()), error);
            return;
        }
        ++f.done;
        if (f.folder.moveSources) {
            std::error_code ec;
            const std::filesystem::path sourceFolder = f.folder.folder / "Source";
            std::filesystem::create_directories(sourceFolder, ec);
            const std::filesystem::path to = freeName(sourceFolder, source.stem(), pathToUtf8(source.extension()));
            std::filesystem::rename(source, to, ec);
            if (ec) {
                f.lastError = std::format("{} was encoded but could not move into Source: {}", pathToUtf8(source.filename()),
                                          ec.message());
            } else {
                f.seen.erase(source);
            }
        }
    }
};

WatchFolderService::WatchFolderService(ExportQueue& queue, Options options)
    : m_queue(queue), m_options(std::move(options)), m_core(std::make_shared<Core>()) {
    if (m_options.interval.count() > 0) {
        m_thread = std::jthread([this](std::stop_token stop) {
            std::mutex sleepMutex;
            std::condition_variable_any wake;
            while (!stop.stop_requested()) {
                std::unique_lock lock(sleepMutex);
                if (wake.wait_for(lock, stop, m_options.interval, [] { return false; })) {
                    break;
                }
                if (stop.stop_requested()) {
                    break;
                }
                lock.unlock();
                scanNow();
            }
        });
    }
}

WatchFolderService::~WatchFolderService() {
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
}

std::expected<std::uint64_t, Error> WatchFolderService::add(WatchFolder folder) {
    if (folder.folder.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Choose a folder to watch"});
    }
    if (auto ok = validate(folder.settings); !ok) {
        return std::unexpected(ok.error());
    }
    std::error_code ec;
    std::filesystem::create_directories(folder.folder, ec);
    std::filesystem::create_directories(outputFolderOf(folder), ec);
    if (!std::filesystem::is_directory(folder.folder, ec) || !std::filesystem::is_directory(outputFolderOf(folder), ec)) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot create the watch folder " + pathToUtf8(folder.folder)});
    }
    std::lock_guard lock(m_core->mutex);
    for (const auto& [id, f] : m_core->folders) {
        if (std::filesystem::equivalent(f.folder.folder, folder.folder, ec)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, pathToUtf8(folder.folder) + " is watched already"});
        }
    }
    const std::uint64_t id = m_core->nextId++;
    m_core->folders[id].folder = std::move(folder);
    return id;
}

bool WatchFolderService::remove(std::uint64_t id) {
    std::lock_guard lock(m_core->mutex);
    return m_core->folders.erase(id) > 0;
}

std::vector<WatchFolderStatus> WatchFolderService::folders() const {
    std::lock_guard lock(m_core->mutex);
    std::vector<WatchFolderStatus> out;
    for (const auto& [id, f] : m_core->folders) {
        out.push_back(WatchFolderStatus{id, f.folder, f.queued, f.done, f.failed, f.lastError});
    }
    return out;
}

std::vector<ExportJobId> WatchFolderService::scanNow() {
    struct Ready {
        std::uint64_t folder;
        WatchFolder watch;
        std::filesystem::path file;
    };
    std::vector<Ready> ready;
    {
        std::lock_guard lock(m_core->mutex);
        for (auto& [id, f] : m_core->folders) {
            std::error_code ec;
            std::map<std::filesystem::path, Core::Seen> now;
            for (std::filesystem::directory_iterator it(f.folder.folder, ec), end; !ec && it != end; it.increment(ec)) {
                const std::filesystem::path& file = it->path();
                const std::string name = pathToUtf8(file.filename());
                if (!it->is_regular_file(ec) || name.starts_with(".") || name.find(".wabcut-partial") != std::string::npos ||
                    !isWatchedMediaFile(file)) {
                    continue;
                }
                Core::Seen seen;
                seen.size = it->file_size(ec);
                seen.time = it->last_write_time(ec);
                if (const auto old = f.seen.find(file); old != f.seen.end()) {
                    if (old->second.size == seen.size && old->second.time == seen.time) {
                        seen.stableScans = old->second.stableScans + 1;
                        seen.taken = old->second.taken;
                    }
                }
                if (!seen.taken && seen.stableScans >= m_options.settleScans) {
                    seen.taken = true;
                    ready.push_back(Ready{id, f.folder, file});
                }
                now.emplace(file, seen);
            }
            f.seen = std::move(now);
        }
    }

    std::vector<ExportJobId> queued;
    for (Ready& r : ready) {
        auto project = projectForMedia(r.file);
        if (!project) {
            {
                std::lock_guard lock(m_core->mutex);
                if (auto it = m_core->folders.find(r.folder); it != m_core->folders.end()) {
                    ++it->second.queued; // finished() counts it down
                }
            }
            m_core->finished(r.folder, r.file, false, project.error().message);
            continue;
        }
        ExportRequest request;
        request.sequence = project->sequences.front()->id;
        request.snapshot = std::make_shared<const ProjectState>(std::move(*project));
        request.settings = r.watch.settings;
        request.settings.presetId = r.watch.presetId;
        request.output = freeName(outputFolderOf(r.watch), r.file.stem(), outputExtension(r.watch.settings));
        request.videoSource = m_options.videoSource;
        std::weak_ptr<Core> core = m_core;
        const std::uint64_t folder = r.folder;
        const std::filesystem::path source = r.file;
        request.onProgress = [core, folder, source](const ExportProgress& p) {
            if (p.phase != ExportPhase::Done && p.phase != ExportPhase::Failed && p.phase != ExportPhase::Cancelled) {
                return;
            }
            if (auto c = core.lock()) {
                c->finished(folder, source, p.phase == ExportPhase::Done,
                            p.phase == ExportPhase::Cancelled ? "cancelled" : "the export failed (see the queue)");
            }
        };
        {
            std::lock_guard lock(m_core->mutex);
            if (auto it = m_core->folders.find(r.folder); it != m_core->folders.end()) {
                ++it->second.queued;
            }
        }
        queued.push_back(m_queue.enqueue(std::move(request)));
    }
    return queued;
}

} // namespace wabcut::exporter
