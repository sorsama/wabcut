#pragma once

// Private infrastructure for the background services: a small worker pool, Job submission and an on-disk
// cache format toolkit (stable keys, little-endian serialisation, atomic writes).

#include <wabcut/media/Job.h>
#include <wabcut/model/Error.h>

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace wabcut::media::detail {

static_assert(std::endian::native == std::endian::little,
              "cache files are written in native little-endian order");

/// Fixed set of worker threads draining a FIFO of tasks. The destructor requests stop on stopToken(), lets
/// the remaining tasks run (they observe the stop and finish quickly) and joins the threads.
class WorkerPool {
public:
    explicit WorkerPool(int threads);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void submit(std::function<void()> task);
    std::stop_token stopToken() const { return m_stop.get_token(); }

private:
    void run();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_queue;
    bool m_stopping = false;
    std::stop_source m_stop;
    std::vector<std::thread> m_threads;
};

/// Queues `work(std::stop_token, std::function<void(float)> progress) -> std::expected<T, Error>` on `pool`.
/// The job is cancelled by Job::cancel() or when the pool shuts down.
template <typename T, typename Work>
Job<T> submitJob(WorkerPool& pool, std::function<void(const std::expected<T, Error>&)> onDone, Work work) {
    auto state = std::make_shared<JobState<T>>();
    state->onDone = std::move(onDone);
    pool.submit([state, work = std::move(work), poolStop = pool.stopToken()]() mutable {
        const std::stop_callback forward(poolStop, [state] { state->stop.request_stop(); });
        const std::stop_token token = state->stop.get_token();
        if (token.stop_requested()) {
            state->complete(std::unexpected(Error{ErrorCode::Cancelled, "Cancelled"}));
            return;
        }
        const std::function<void(float)> progress = [state](float p) {
            state->progress.store(std::clamp(p, 0.0f, 0.999f), std::memory_order_relaxed);
        };
        state->complete(work(token, progress));
    });
    return Job<T>(std::move(state));
}

// ---- Disk cache
// ------------------------------------------------------------------------------------------------

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes, std::uint64_t seed = 0xcbf29ce484222325ULL);
std::string hex64(std::uint64_t v);

class ByteWriter {
public:
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void pod(const T& v) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        m_bytes.insert(m_bytes.end(), p, p + sizeof(T));
    }
    void raw(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        m_bytes.insert(m_bytes.end(), p, p + size);
    }
    void string(std::string_view s) {
        pod(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    /// Appends an FNV-1a checksum of everything written so far.
    void checksum() { pod(fnv1a(m_bytes)); }
    const std::vector<std::uint8_t>& bytes() const { return m_bytes; }

private:
    std::vector<std::uint8_t> m_bytes;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    bool pod(T& v) {
        return raw(&v, sizeof(T));
    }
    bool raw(void* out, std::size_t size) {
        if (size > m_bytes.size() - m_pos) {
            return false;
        }
        std::memcpy(out, m_bytes.data() + m_pos, size);
        m_pos += size;
        return true;
    }
    bool string(std::string& s, std::size_t maxSize = 1 << 16) {
        std::uint32_t n = 0;
        if (!pod(n) || n > maxSize || n > m_bytes.size() - m_pos) {
            return false;
        }
        s.assign(reinterpret_cast<const char*>(m_bytes.data() + m_pos), n);
        m_pos += n;
        return true;
    }
    std::size_t remaining() const { return m_bytes.size() - m_pos; }
    /// Verifies the trailing checksum written by ByteWriter::checksum() over all preceding bytes.
    static bool verifyChecksum(std::span<const std::uint8_t> bytes);

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t m_pos = 0;
};

/// Writes `bytes` to a unique temporary file next to `file`, then renames it into place.
bool writeFileAtomic(const std::filesystem::path& file, std::span<const std::uint8_t> bytes);
std::optional<std::vector<std::uint8_t>> readWholeFile(const std::filesystem::path& file);

} // namespace wabcut::media::detail
