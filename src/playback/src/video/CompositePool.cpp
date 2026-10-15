#include <wabcut/playback/video/CompositePool.h>

#include <algorithm>
#include <atomic>

namespace wabcut::playback {

CompositePool::CompositePool(int size, int layerCapacity) {
    m_items.reserve(static_cast<std::size_t>(std::max(size, 1)));
    for (int i = 0; i < std::max(size, 1); ++i) {
        auto c = std::make_shared<render::PreparedComposite>();
        c->layers.reserve(static_cast<std::size_t>(std::max(layerCapacity, 1)));
        m_items.push_back(std::move(c));
    }
}

std::shared_ptr<render::PreparedComposite> CompositePool::acquire() {
    // Round-robin from the last hand-out, so recently published composites (still likely referenced) are
    // checked last.
    for (std::size_t n = 0; n < m_items.size(); ++n) {
        auto& item = m_items[(m_next + n) % m_items.size()];
        if (item.use_count() == 1) {
            // Every other reference is gone; make their (const) reads happen-before our writes.
            std::atomic_thread_fence(std::memory_order_acquire);
            m_next = (m_next + n + 1) % m_items.size();
            item->reset(); // releases the previous frames here, on the scheduler thread
            return item;
        }
    }
    return nullptr;
}

int CompositePool::inUse() const {
    return static_cast<int>(
        std::count_if(m_items.begin(), m_items.end(), [](const auto& item) { return item.use_count() > 1; }));
}

} // namespace wabcut::playback
