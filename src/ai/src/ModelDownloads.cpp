#include <wabcut/ai/ModelDownloads.h>

#include <algorithm>
#include <cstdio>

namespace wabcut::ai {

namespace {

using Clock = std::chrono::steady_clock;

std::optional<std::uint64_t> filesystemFree(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::path p = folder;
    while (!p.empty() && !std::filesystem::exists(p, ec)) {
        const std::filesystem::path parent = p.parent_path();
        if (parent == p)
            break;
        p = parent;
    }
    if (p.empty())
        return std::nullopt;
    const auto info = std::filesystem::space(p, ec);
    if (ec)
        return std::nullopt;
    return info.available;
}

std::string gb(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

} // namespace

std::string_view toString(DownloadStatus status) {
    switch (status) {
    case DownloadStatus::Queued: return "queued";
    case DownloadStatus::Running: return "running";
    case DownloadStatus::Paused: return "paused";
    case DownloadStatus::Done: return "done";
    case DownloadStatus::Failed: return "failed";
    case DownloadStatus::Cancelled: return "cancelled";
    }
    return "queued";
}

struct DownloadQueue::Entry {
    DownloadRequest request;
    DownloadState state;
    std::stop_source stop;
    bool pauseRequested = false;
    bool cancelRequested = false;
    bool active = false; ///< the worker is processing it
    // Speed: an exponential average over half-second samples.
    Clock::time_point sampleTime{};
    std::uint64_t sampleBytes = 0;
};

DownloadQueue::DownloadQueue(IDownloadTransport& transport, Options options)
    : m_transport(transport), m_options(std::move(options)) {
    if (!m_options.freeSpace)
        m_options.freeSpace = filesystemFree;
    m_worker = std::jthread([this](std::stop_token stop) { run(stop); });
}

DownloadQueue::~DownloadQueue() {
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries) {
            if (e->state.status == DownloadStatus::Running) {
                e->pauseRequested = true; // the partial file stays for the next session
                e->stop.request_stop();
            }
        }
    }
    m_worker.request_stop();
    m_changed.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

bool DownloadQueue::enqueue(DownloadRequest request) {
    if (request.id.empty() || request.files.empty())
        return false;
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if ((*it)->request.id != request.id)
                continue;
            const DownloadStatus s = (*it)->state.status;
            if (s == DownloadStatus::Queued || s == DownloadStatus::Running || s == DownloadStatus::Paused)
                return false;
            m_entries.erase(it); // a finished entry of the same id makes room for the new request
            break;
        }
        auto entry = std::make_shared<Entry>();
        entry->state.id = request.id;
        entry->state.name = request.name;
        entry->state.fileCount = static_cast<int>(request.files.size());
        for (const ModelInfo& f : request.files)
            entry->state.total += f.size;
        entry->request = std::move(request);
        m_entries.push_back(std::move(entry));
    }
    m_changed.notify_all();
    notify(true);
    return true;
}

bool DownloadQueue::pause(std::string_view id) {
    {
        std::lock_guard lock(m_mutex);
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& e) { return e->request.id == id; });
        if (it == m_entries.end())
            return false;
        Entry& e = **it;
        if (e.state.status == DownloadStatus::Running) {
            e.pauseRequested = true;
            e.stop.request_stop();
        } else if (e.state.status != DownloadStatus::Queued) {
            return false;
        }
        e.state.status = DownloadStatus::Paused;
        e.state.bytesPerSecond = 0.0;
        e.state.eta.reset();
    }
    m_changed.notify_all();
    notify(true);
    return true;
}

bool DownloadQueue::resume(std::string_view id) {
    {
        std::lock_guard lock(m_mutex);
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& e) { return e->request.id == id; });
        if (it == m_entries.end())
            return false;
        Entry& e = **it;
        if (e.state.status != DownloadStatus::Paused && e.state.status != DownloadStatus::Failed)
            return false;
        // While the worker is still stopping the paused transfer it sees Queued and leaves the entry for the next run.
        e.state.status = DownloadStatus::Queued;
        e.state.error.clear();
    }
    m_changed.notify_all();
    notify(true);
    return true;
}

bool DownloadQueue::cancel(std::string_view id) {
    std::shared_ptr<Entry> removed;
    bool running = false;
    {
        std::lock_guard lock(m_mutex);
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& e) { return e->request.id == id; });
        if (it == m_entries.end())
            return false;
        removed = *it;
        m_entries.erase(it);
        if (removed->active) {
            removed->cancelRequested = true;
            removed->stop.request_stop();
            running = true; // the worker deletes what it wrote once the transfer stopped
        }
        removed->state.status = DownloadStatus::Cancelled;
    }
    if (!running) {
        // Not on the worker: delete the partial files an earlier, paused run left.
        ModelStore store(removed->request.root);
        for (const ModelInfo& f : removed->request.files) {
            if (!store.isInstalled(f) && store.partialBytes(f) > 0)
                (void)store.remove(f);
        }
    }
    m_changed.notify_all();
    notify(true);
    return true;
}

void DownloadQueue::clearFinished() {
    {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_entries, [](const auto& e) {
            return e->state.status == DownloadStatus::Done || e->state.status == DownloadStatus::Failed ||
                   e->state.status == DownloadStatus::Cancelled;
        });
    }
    notify(true);
}

std::vector<DownloadState> DownloadQueue::snapshot() const {
    std::lock_guard lock(m_mutex);
    std::vector<DownloadState> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries)
        out.push_back(e->state);
    return out;
}

std::optional<DownloadState> DownloadQueue::state(std::string_view id) const {
    std::lock_guard lock(m_mutex);
    for (const auto& e : m_entries) {
        if (e->request.id == id)
            return e->state;
    }
    return std::nullopt;
}

bool DownloadQueue::busy() const {
    std::lock_guard lock(m_mutex);
    return std::any_of(m_entries.begin(), m_entries.end(), [](const auto& e) {
        return e->state.status == DownloadStatus::Queued || e->state.status == DownloadStatus::Running;
    });
}

bool DownloadQueue::waitIdle(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, timeout, [&] {
        return std::none_of(m_entries.begin(), m_entries.end(), [](const auto& e) {
            return e->state.status == DownloadStatus::Queued || e->state.status == DownloadStatus::Running;
        });
    });
}

void DownloadQueue::notify(bool force) {
    if (!m_options.onChanged)
        return;
    {
        std::lock_guard lock(m_mutex);
        const auto now = Clock::now();
        if (!force && now - m_lastNotify < m_options.notifyInterval)
            return;
        m_lastNotify = now;
    }
    m_options.onChanged();
}

void DownloadQueue::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::shared_ptr<Entry> next;
        {
            std::unique_lock lock(m_mutex);
            m_changed.wait(lock, stop, [&] {
                return std::any_of(m_entries.begin(), m_entries.end(),
                                   [](const auto& e) { return e->state.status == DownloadStatus::Queued; });
            });
            if (stop.stop_requested())
                return;
            for (const auto& e : m_entries) {
                if (e->state.status == DownloadStatus::Queued) {
                    next = e;
                    break;
                }
            }
            if (!next)
                continue;
            next->state.status = DownloadStatus::Running;
            next->state.error.clear();
            next->stop = std::stop_source();
            next->pauseRequested = false;
            next->cancelRequested = false;
            next->active = true;
            next->sampleTime = Clock::now();
            next->sampleBytes = 0;
        }
        notify(true);
        process(next);
        {
            std::lock_guard lock(m_mutex);
            next->active = false;
        }
        m_changed.notify_all();
        notify(true);
    }
}

void DownloadQueue::process(const std::shared_ptr<Entry>& entry) {
    const DownloadRequest& request = entry->request;
    ModelStore store(request.root);
    std::uint64_t total = 0;
    std::uint64_t have = 0;
    std::vector<bool> installedBefore;
    for (const ModelInfo& f : request.files) {
        total += f.size;
        const bool installed = store.isInstalled(f);
        installedBefore.push_back(installed);
        have += installed ? f.size : std::min(store.partialBytes(f), f.size);
    }
    {
        std::lock_guard lock(m_mutex);
        entry->state.total = total;
        entry->state.received = have;
        entry->sampleBytes = have;
    }
    const std::uint64_t needed = total - std::min(have, total);
    if (needed > 0 && m_options.freeSpace) {
        if (const auto free = m_options.freeSpace(request.root); free && *free < needed + m_options.spareBytes) {
            std::lock_guard lock(m_mutex);
            entry->state.status = DownloadStatus::Failed;
            entry->state.error = "Not enough free space on the drive: " + gb(needed + m_options.spareBytes) + " needed, " +
                                 gb(*free) + " free";
        }
    }
    auto finish = [&](DownloadStatus status, std::string error) {
        DownloadState copy;
        {
            std::lock_guard lock(m_mutex);
            if (entry->cancelRequested) {
                entry->state.status = DownloadStatus::Cancelled;
            } else if (entry->pauseRequested && entry->state.status == DownloadStatus::Queued && status != DownloadStatus::Done) {
                // resumed while the paused transfer was stopping: the next run picks it up
            } else if (entry->pauseRequested && status != DownloadStatus::Done) {
                entry->state.status = DownloadStatus::Paused;
            } else {
                entry->state.status = status;
                entry->state.error = std::move(error);
            }
            entry->state.bytesPerSecond = 0.0;
            entry->state.eta.reset();
            copy = entry->state;
        }
        if (copy.status == DownloadStatus::Cancelled) {
            // Delete what this request wrote: partial files, and weights it completed (runtimes stay: they are shared).
            for (std::size_t k = 0; k < request.files.size(); ++k) {
                const ModelInfo& f = request.files[k];
                if (installedBefore[k] || (store.isInstalled(f) && f.kind == ModelKind::Runtime))
                    continue;
                (void)store.remove(f);
            }
        }
        if ((copy.status == DownloadStatus::Done || copy.status == DownloadStatus::Failed) && m_options.onFinished)
            m_options.onFinished(copy);
    };
    {
        std::unique_lock lock(m_mutex);
        if (entry->state.status == DownloadStatus::Failed) {
            std::string error = entry->state.error;
            lock.unlock();
            finish(DownloadStatus::Failed, std::move(error));
            return;
        }
    }

    std::uint64_t done = 0;
    for (std::size_t k = 0; k < request.files.size(); ++k) {
        const ModelInfo& f = request.files[k];
        if (store.isInstalled(f)) {
            done += f.size;
            continue;
        }
        if (entry->stop.stop_requested()) {
            finish(DownloadStatus::Paused, {});
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            entry->state.fileIndex = static_cast<int>(k) + 1;
        }
        auto result = store.ensure(
            f, &m_transport,
            [&](const DownloadProgress& p) {
                {
                    std::lock_guard lock(m_mutex);
                    DownloadState& s = entry->state;
                    s.received = done + p.received;
                    const auto now = Clock::now();
                    const double dt = std::chrono::duration<double>(now - entry->sampleTime).count();
                    if (dt >= 0.5) {
                        const double instant = static_cast<double>(s.received - std::min(s.received, entry->sampleBytes)) / dt;
                        s.bytesPerSecond = s.bytesPerSecond <= 0.0 ? instant : 0.7 * s.bytesPerSecond + 0.3 * instant;
                        entry->sampleTime = now;
                        entry->sampleBytes = s.received;
                        if (s.bytesPerSecond > 1.0)
                            s.eta = std::chrono::seconds(static_cast<std::int64_t>(
                                static_cast<double>(s.total - std::min(s.total, s.received)) / s.bytesPerSecond));
                    }
                }
                notify(false);
            },
            entry->stop.get_token());
        if (!result) {
            finish(DownloadStatus::Failed, result.error().message);
            return;
        }
        done += f.size;
        {
            std::lock_guard lock(m_mutex);
            entry->state.received = done;
        }
    }
    finish(DownloadStatus::Done, {});
}

} // namespace wabcut::ai
