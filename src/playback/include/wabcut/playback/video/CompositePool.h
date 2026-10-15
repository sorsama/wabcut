#pragma once

// CompositePool: preallocated PreparedComposites for the scheduler (no allocation in steady state).
//
// The pool keeps one reference to every composite it owns. A composite is free again once nobody else
// references it (use_count() == 1): the mailbox, the monitor and the frame importer only ever drop
// references, so the final release of a composite's decoded frames happens here, on the scheduler thread,
// when the composite is reused (never on the render thread).

#include <wabcut/render/PreparedComposite.h>

#include <memory>
#include <vector>

namespace wabcut::playback {

class CompositePool {
public:
    /// `size` composites, each with room for `layerCapacity` layers before its vector grows.
    explicit CompositePool(int size, int layerCapacity = 16);
    CompositePool(const CompositePool&) = delete;
    CompositePool& operator=(const CompositePool&) = delete;

    /// A reset composite nobody else references, or null when every one is in use. Single thread (the
    /// scheduler).
    std::shared_ptr<render::PreparedComposite> acquire();

    int size() const { return static_cast<int>(m_items.size()); }
    /// Composites currently referenced outside the pool (approximate when other threads are releasing).
    int inUse() const;

private:
    std::vector<std::shared_ptr<render::PreparedComposite>> m_items;
    std::size_t m_next = 0;
};

} // namespace wabcut::playback
