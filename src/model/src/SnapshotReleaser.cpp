#include <wabcut/model/SnapshotReleaser.h>

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace wabcut {

SnapshotReleaser::SnapshotReleaser(EditSession& session) : m_session(session) {
    m_listener = m_session.addListener([this](const ChangeEvent&) { poke(); });
    m_thread = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
    poke(); // what the session queued before we existed
}

SnapshotReleaser::~SnapshotReleaser() {
    m_session.removeListener(m_listener);
    m_thread.request_stop();
    m_wake.notify_all();
    m_thread.join();
    m_released += m_session.releasePending(); // anything queued after the thread's last pass
}

void SnapshotReleaser::poke() {
    {
        const std::lock_guard lock(m_mutex);
        ++m_requested;
    }
    m_wake.notify_one();
}

void SnapshotReleaser::flush() {
    std::unique_lock lock(m_mutex);
    const std::uint64_t wanted = ++m_requested;
    m_wake.notify_one();
    m_idle.wait(lock, [&] { return m_served >= wanted || !m_thread.joinable(); });
}

std::size_t SnapshotReleaser::released() const {
    const std::lock_guard lock(m_mutex);
    return m_released;
}

void SnapshotReleaser::releaseAndTrim() {
    {
        const std::lock_guard lock(m_mutex);
        m_trimAfter = ++m_requested;
    }
    m_wake.notify_one();
}

std::size_t SnapshotReleaser::trims() const {
    const std::lock_guard lock(m_mutex);
    return m_trims;
}

void SnapshotReleaser::trimFreedMemory() {
#if defined(_WIN32)
    HEAP_OPTIMIZE_RESOURCES_INFORMATION info{};
    info.Version = HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION;
    info.Flags = 0;
    (void)HeapSetInformation(nullptr, HeapOptimizeResources, &info, sizeof info); // every heap's LFH caches
    (void)_heapmin(); // the CRT heap's (the process heap's) free ranges decommitted
#elif defined(__APPLE__)
    (void)malloc_zone_pressure_relief(nullptr, 0); // every zone, as much as it can
#elif defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
}

void SnapshotReleaser::run(std::stop_token stop) {
    std::unique_lock lock(m_mutex);
    while (true) {
        m_wake.wait(lock, stop, [&] { return m_requested != m_served; });
        if (stop.stop_requested()) {
            break;
        }
        const std::uint64_t serving = m_requested;
        lock.unlock();
        const std::size_t freed = m_session.releasePending(); // thread-safe; frees outside the session's lock
        lock.lock();
        if (m_trimAfter != 0 && serving >= m_trimAfter) {
            m_trimAfter = 0;
            lock.unlock();
            trimFreedMemory();
            lock.lock();
            ++m_trims;
        }
        m_released += freed;
        m_served = serving;
        m_idle.notify_all();
    }
    m_served = m_requested;
    m_idle.notify_all();
}

} // namespace wabcut
