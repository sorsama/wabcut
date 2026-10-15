#include "Background.h"

#include "FfmpegUtil.h"

#include <atomic>
#include <format>
#include <fstream>
#include <system_error>

namespace wabcut::media::detail {

WorkerPool::WorkerPool(int threads) {
    const int n = std::max(1, threads);
    m_threads.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        m_threads.emplace_back([this] { run(); });
    }
}

WorkerPool::~WorkerPool() {
    m_stop.request_stop();
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (std::thread& t : m_threads) {
        t.join();
    }
}

void WorkerPool::submit(std::function<void()> task) {
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(task));
    }
    m_wake.notify_one();
}

void WorkerPool::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                return; // stopping and drained
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes, std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (const std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string hex64(std::uint64_t v) {
    return std::format("{:016x}", v);
}

bool ByteReader::verifyChecksum(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < sizeof(std::uint64_t)) {
        return false;
    }
    const std::size_t body = bytes.size() - sizeof(std::uint64_t);
    std::uint64_t stored = 0;
    std::memcpy(&stored, bytes.data() + body, sizeof stored);
    return stored == fnv1a(bytes.first(body));
}

bool writeFileAtomic(const std::filesystem::path& file, std::span<const std::uint8_t> bytes) {
    static std::atomic<std::uint64_t> counter{0};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto unique =
        std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ (counter.fetch_add(1) << 20);
    std::filesystem::path temp = file;
    temp += std::format(".{:x}.tmp", unique);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        // Windows refuses to replace a file another process has open; drop ours rather than fail the job.
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> readWholeFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace wabcut::media::detail
