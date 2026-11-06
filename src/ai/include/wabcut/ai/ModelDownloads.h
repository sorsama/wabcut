#pragma once

// The model library's download manager (r07, docs/ai.md "Local model library"): a queue of model downloads that runs
// one at a time on its own thread, through ModelStore::ensure() (HTTP Range resume, size and SHA-256 verification) and
// an injected IDownloadTransport (the AI network stack in the app: proxies, TLS, the redacted request log, offline
// mode and the project's no-cloud lock; a fake in tests).
//
// A request is a list of ModelStore entries (a GGUF's shards, its vision projector, the llama-server runtime): files
// already installed are skipped. Before a request starts, the free space of the store's drive must hold what is left
// to download plus a spare. Pause stops the transfer and keeps the partial file; resume continues it with a range
// request; cancel stops it and deletes the partial files. Progress carries bytes, speed and the time left.

#include <wabcut/ai/ModelStore.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wabcut::ai {

struct DownloadRequest {
    std::string id;               ///< what the library calls it (a library model id, a built-in model id)
    std::string name;             ///< for people
    std::filesystem::path root;   ///< the model store
    std::vector<ModelInfo> files; ///< in order
};

enum class DownloadStatus { Queued, Running, Paused, Done, Failed, Cancelled };
std::string_view toString(DownloadStatus status);

struct DownloadState {
    std::string id;
    std::string name;
    DownloadStatus status = DownloadStatus::Queued;
    std::uint64_t received = 0;   ///< bytes on disk, partial files included
    std::uint64_t total = 0;
    double bytesPerSecond = 0.0;  ///< smoothed; 0 while not running
    std::optional<std::chrono::seconds> eta;
    std::string error;            ///< Failed: why, for people
    int fileIndex = 0;            ///< 1-based file being downloaded (0 before the first)
    int fileCount = 0;
};

class DownloadQueue {
public:
    struct Options {
        /// Free bytes on the drive of a folder; nullopt = unknown (the check is skipped). Default: filesystem::space.
        std::function<std::optional<std::uint64_t>(const std::filesystem::path&)> freeSpace;
        /// Kept free beyond the download.
        std::uint64_t spareBytes = std::uint64_t{512} << 20;
        /// After any change, from the worker thread (at most every `notifyInterval` while bytes arrive) or the caller's
        /// thread; never under the queue's lock.
        std::function<void()> onChanged;
        std::chrono::milliseconds notifyInterval{150};
        /// When a request ended as Done or Failed (not paused or cancelled), from the worker thread.
        std::function<void(const DownloadState&)> onFinished;
    };

    DownloadQueue(IDownloadTransport& transport, Options options);
    /// Stops the running download (its partial file stays for next time) and joins the worker.
    ~DownloadQueue();
    DownloadQueue(const DownloadQueue&) = delete;
    DownloadQueue& operator=(const DownloadQueue&) = delete;

    /// Queues a request. False when one with this id is queued, running or paused, or it has no files.
    bool enqueue(DownloadRequest request);
    /// Queued or running -> paused (the partial file stays). False when there is nothing to pause.
    bool pause(std::string_view id);
    /// Paused or failed -> queued again.
    bool resume(std::string_view id);
    /// Stops the request, deletes its partial files and drops it from the list. False when unknown.
    bool cancel(std::string_view id);
    /// Drops entries that are done, failed or cancelled.
    void clearFinished();

    std::vector<DownloadState> snapshot() const;
    std::optional<DownloadState> state(std::string_view id) const;
    /// Something is queued or running.
    bool busy() const;
    /// Blocks until nothing is queued or running, or the timeout passes (tests). True when idle.
    bool waitIdle(std::chrono::milliseconds timeout) const;

private:
    struct Entry;
    void run(std::stop_token stop);
    void process(const std::shared_ptr<Entry>& entry);
    void notify(bool force);

    IDownloadTransport& m_transport;
    Options m_options;
    mutable std::mutex m_mutex;
    mutable std::condition_variable_any m_changed;
    std::list<std::shared_ptr<Entry>> m_entries;
    std::chrono::steady_clock::time_point m_lastNotify{};
    std::jthread m_worker; // last: starts after everything above exists
};

} // namespace wabcut::ai
