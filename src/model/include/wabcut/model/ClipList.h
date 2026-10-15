#pragma once

// ClipList: the clips of a track, stored as a persistent vector of shared chunks.
//
// Clips live in chunks of up to kChunkMax clips. Copying a ClipList copies only the chunk pointers, and
// a change copies just the chunks it touches (copy-on-write), so an edit that changes one clip of a
// 10,000-clip track copies one chunk of about 64 clips and shares every other chunk with the previous
// revision. Reading is like a vector: size(), operator[] (O(log chunks)), and iteration (O(1) a step).
//
// A ClipList inside a published (const) Track is immutable. The mutating members are for building a
// new revision; they never change a chunk another list still references.

#include <wabcut/model/Clip.h>

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace wabcut {

class ClipList {
    using Chunk = std::vector<Clip>;

public:
    /// Clips per chunk when a list is built by push_back.
    static constexpr std::size_t kChunkTarget = 64;
    /// A chunk that grows beyond this is split in two.
    static constexpr std::size_t kChunkMax = 128;
    /// A chunk that shrinks below this is merged into a neighbour when they fit together.
    static constexpr std::size_t kChunkMin = 16;

    class const_iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = Clip;
        using difference_type = std::ptrdiff_t;
        using pointer = const Clip*;
        using reference = const Clip&;

        const_iterator() = default;
        reference operator*() const { return (*m_list->m_chunks[m_chunk])[m_offset]; }
        pointer operator->() const { return &**this; }
        const_iterator& operator++() {
            if (++m_offset == m_list->m_chunks[m_chunk]->size()) {
                ++m_chunk;
                m_offset = 0;
            }
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator copy = *this;
            ++*this;
            return copy;
        }
        const_iterator& operator--() {
            if (m_offset == 0) {
                --m_chunk;
                m_offset = m_list->m_chunks[m_chunk]->size() - 1;
            } else {
                --m_offset;
            }
            return *this;
        }
        const_iterator operator--(int) {
            const_iterator copy = *this;
            --*this;
            return copy;
        }
        bool operator==(const const_iterator& o) const { return m_chunk == o.m_chunk && m_offset == o.m_offset; }

    private:
        friend class ClipList;
        const_iterator(const ClipList* list, std::size_t chunk, std::size_t offset)
            : m_list(list), m_chunk(chunk), m_offset(offset) {}

        const ClipList* m_list = nullptr;
        std::size_t m_chunk = 0;
        std::size_t m_offset = 0;
    };
    using iterator = const_iterator;

    ClipList() = default;
    ClipList(std::initializer_list<Clip> clips);
    explicit ClipList(std::vector<Clip> clips);

    std::size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    const Clip& operator[](std::size_t index) const;
    const Clip& front() const { return m_chunks.front()->front(); }
    const Clip& back() const { return m_chunks.back()->back(); }
    const_iterator begin() const { return {this, 0, 0}; }
    const_iterator end() const { return {this, m_chunks.size(), 0}; }
    /// Iterator to the clip at `index` (end() when index == size()).
    const_iterator iteratorAt(std::size_t index) const;
    std::vector<Clip> toVector() const;

    /// Index of the first clip whose start is >= t (clips are sorted by start).
    std::size_t lowerBoundStart(Ticks t) const;
    /// Index of the first clip whose start is > t.
    std::size_t upperBoundStart(Ticks t) const;

    // --- Building a new revision ----------------------------------------------------------------------

    /// The clip at `index`, writable. Copies its chunk first if another list shares it.
    Clip& mutableAt(std::size_t index);
    /// Calls fn(Clip&) for every clip in [first, last), copying the chunks involved.
    template <class Fn>
    void mutateRange(std::size_t first, std::size_t last, Fn&& fn) {
        if (first >= last) {
            return;
        }
        auto [chunk, offset] = locate(first);
        std::size_t remaining = last - first;
        while (remaining > 0) {
            Chunk& c = own(chunk);
            for (; offset < c.size() && remaining > 0; ++offset, --remaining) {
                fn(c[offset]);
            }
            ++chunk;
            offset = 0;
        }
    }
    void push_back(Clip clip);
    void insert(std::size_t index, Clip clip);
    void erase(std::size_t first, std::size_t last);
    /// Removes the clips at the given ascending indices in one pass and returns them in that order.
    std::vector<Clip> extract(std::span<const std::size_t> ascendingIndices);
    void clear();

    // --- Introspection (tests, memory accounting, change detection) --------------------------------------

    std::size_t chunkCount() const { return m_chunks.size(); }
    /// Identity of chunk `c`: equal for two lists exactly when they share that chunk.
    const void* chunkIdentity(std::size_t c) const { return m_chunks[c].get(); }
    std::size_t chunkSize(std::size_t c) const { return m_chunks[c]->size(); }
    /// Index of the first clip of chunk `c`.
    std::size_t chunkStart(std::size_t c) const { return m_starts[c]; }
    /// Approximate heap bytes held by the list (chunks plus clips; clip content blocks not included).
    std::size_t approxBytes() const;

    /// Element-wise equality.
    bool operator==(const ClipList& other) const;

private:
    std::pair<std::size_t, std::size_t> locate(std::size_t index) const;
    Chunk& own(std::size_t chunk);
    void reindex(std::size_t fromChunk);
    void splitIfLarge(std::size_t chunk);
    void compact();

    std::vector<std::shared_ptr<Chunk>> m_chunks;
    std::vector<std::size_t> m_starts; // index of each chunk's first clip
    std::size_t m_size = 0;
};

} // namespace wabcut
