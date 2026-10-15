#include <wabcut/model/ClipList.h>

#include <algorithm>
#include <cassert>

namespace wabcut {

ClipList::ClipList(std::initializer_list<Clip> clips) {
    for (const Clip& c : clips) {
        push_back(c);
    }
}

ClipList::ClipList(std::vector<Clip> clips) {
    for (Clip& c : clips) {
        push_back(std::move(c));
    }
}

const Clip& ClipList::operator[](std::size_t index) const {
    const auto [chunk, offset] = locate(index);
    return (*m_chunks[chunk])[offset];
}

std::vector<Clip> ClipList::toVector() const { return {begin(), end()}; }

ClipList::const_iterator ClipList::iteratorAt(std::size_t index) const {
    if (index >= m_size) {
        return end();
    }
    const auto [chunk, offset] = locate(index);
    return {this, chunk, offset};
}

std::pair<std::size_t, std::size_t> ClipList::locate(std::size_t index) const {
    assert(index < m_size);
    const auto it = std::upper_bound(m_starts.begin(), m_starts.end(), index);
    const auto chunk = static_cast<std::size_t>(it - m_starts.begin()) - 1;
    return {chunk, index - m_starts[chunk]};
}

std::size_t ClipList::lowerBoundStart(Ticks t) const {
    const auto it = std::partition_point(m_chunks.begin(), m_chunks.end(),
                                         [&](const std::shared_ptr<Chunk>& c) { return c->back().start() < t; });
    if (it == m_chunks.end()) {
        return m_size;
    }
    const auto chunk = static_cast<std::size_t>(it - m_chunks.begin());
    const Chunk& c = **it;
    const auto in = std::partition_point(c.begin(), c.end(), [&](const Clip& clip) { return clip.start() < t; });
    return m_starts[chunk] + static_cast<std::size_t>(in - c.begin());
}

std::size_t ClipList::upperBoundStart(Ticks t) const {
    const auto it = std::partition_point(m_chunks.begin(), m_chunks.end(),
                                         [&](const std::shared_ptr<Chunk>& c) { return c->back().start() <= t; });
    if (it == m_chunks.end()) {
        return m_size;
    }
    const auto chunk = static_cast<std::size_t>(it - m_chunks.begin());
    const Chunk& c = **it;
    const auto in = std::partition_point(c.begin(), c.end(), [&](const Clip& clip) { return clip.start() <= t; });
    return m_starts[chunk] + static_cast<std::size_t>(in - c.begin());
}

ClipList::Chunk& ClipList::own(std::size_t chunk) {
    std::shared_ptr<Chunk>& c = m_chunks[chunk];
    // use_count() == 1 means no other list (and so no published snapshot) can see this chunk: no other
    // owner exists to copy it from, so the count cannot grow behind our back.
    if (c.use_count() != 1) {
        auto copy = std::make_shared<Chunk>();
        copy->reserve(c->size() + 4);
        copy->assign(c->begin(), c->end());
        c = std::move(copy);
    }
    return *c;
}

void ClipList::reindex(std::size_t fromChunk) {
    m_starts.resize(m_chunks.size());
    std::size_t start = 0;
    if (fromChunk > 0 && fromChunk <= m_chunks.size()) {
        start = m_starts[fromChunk - 1] + m_chunks[fromChunk - 1]->size();
    } else {
        fromChunk = 0;
    }
    for (std::size_t c = fromChunk; c < m_chunks.size(); ++c) {
        m_starts[c] = start;
        start += m_chunks[c]->size();
    }
}

void ClipList::splitIfLarge(std::size_t chunk) {
    if (m_chunks[chunk]->size() <= kChunkMax) {
        return;
    }
    Chunk& c = own(chunk);
    const auto half = static_cast<std::ptrdiff_t>(c.size() / 2);
    auto upper = std::make_shared<Chunk>(std::make_move_iterator(c.begin() + half), std::make_move_iterator(c.end()));
    c.erase(c.begin() + half, c.end());
    m_chunks.insert(m_chunks.begin() + static_cast<std::ptrdiff_t>(chunk + 1), std::move(upper));
}

void ClipList::compact() {
    // Drop empty chunks and merge undersized ones into their successor when both fit in one chunk.
    std::erase_if(m_chunks, [](const std::shared_ptr<Chunk>& c) { return c->empty(); });
    for (std::size_t c = 0; c + 1 < m_chunks.size();) {
        const std::size_t a = m_chunks[c]->size();
        const std::size_t b = m_chunks[c + 1]->size();
        if ((a < kChunkMin || b < kChunkMin) && a + b <= kChunkMax) {
            Chunk& merged = own(c);
            const Chunk& next = *m_chunks[c + 1];
            merged.insert(merged.end(), next.begin(), next.end());
            m_chunks.erase(m_chunks.begin() + static_cast<std::ptrdiff_t>(c + 1));
        } else {
            ++c;
        }
    }
    reindex(0);
}

Clip& ClipList::mutableAt(std::size_t index) {
    const auto [chunk, offset] = locate(index);
    return own(chunk)[offset];
}

void ClipList::push_back(Clip clip) {
    if (m_chunks.empty() || m_chunks.back()->size() >= kChunkTarget) {
        auto chunk = std::make_shared<Chunk>();
        chunk->reserve(kChunkTarget);
        m_chunks.push_back(std::move(chunk));
        m_starts.push_back(m_size);
    }
    own(m_chunks.size() - 1).push_back(std::move(clip));
    ++m_size;
}

void ClipList::insert(std::size_t index, Clip clip) {
    assert(index <= m_size);
    if (index == m_size) {
        if (m_chunks.empty() || m_chunks.back()->size() >= kChunkMax) {
            push_back(std::move(clip));
            return;
        }
        own(m_chunks.size() - 1).push_back(std::move(clip));
        ++m_size;
        return;
    }
    const auto [chunk, offset] = locate(index);
    Chunk& c = own(chunk);
    c.insert(c.begin() + static_cast<std::ptrdiff_t>(offset), std::move(clip));
    ++m_size;
    splitIfLarge(chunk);
    reindex(chunk);
}

void ClipList::erase(std::size_t first, std::size_t last) {
    assert(first <= last && last <= m_size);
    if (first == last) {
        return;
    }
    auto [chunk, offset] = locate(first);
    std::size_t remaining = last - first;
    while (remaining > 0) {
        const std::size_t available = m_chunks[chunk]->size() - offset;
        const std::size_t n = std::min(available, remaining);
        if (offset == 0 && n == m_chunks[chunk]->size()) {
            m_chunks[chunk] = std::make_shared<Chunk>(); // whole chunk goes; compact() drops it
        } else {
            Chunk& c = own(chunk);
            c.erase(c.begin() + static_cast<std::ptrdiff_t>(offset),
                    c.begin() + static_cast<std::ptrdiff_t>(offset + n));
        }
        remaining -= n;
        ++chunk;
        offset = 0;
    }
    m_size -= last - first;
    compact();
}

std::vector<Clip> ClipList::extract(std::span<const std::size_t> ascendingIndices) {
    std::vector<Clip> out;
    out.reserve(ascendingIndices.size());
    std::size_t k = 0;
    while (k < ascendingIndices.size()) {
        // m_starts still describes the original layout: chunks are compacted in place and reindexed at the end.
        const auto [chunk, firstOffset] = locate(ascendingIndices[k]);
        const std::size_t chunkBegin = m_starts[chunk];
        Chunk& c = own(chunk);
        std::size_t write = firstOffset;
        for (std::size_t read = firstOffset; read < c.size(); ++read) {
            if (k < ascendingIndices.size() && ascendingIndices[k] == chunkBegin + read) {
                out.push_back(std::move(c[read]));
                ++k;
            } else {
                if (write != read) {
                    c[write] = std::move(c[read]);
                }
                ++write;
            }
        }
        c.erase(c.begin() + static_cast<std::ptrdiff_t>(write), c.end());
    }
    m_size -= out.size();
    compact();
    return out;
}

void ClipList::clear() {
    m_chunks.clear();
    m_starts.clear();
    m_size = 0;
}

std::size_t ClipList::approxBytes() const {
    constexpr std::size_t kChunkOverhead = sizeof(Chunk) + 32; // vector header + shared_ptr control block
    return sizeof(ClipList) + m_chunks.size() * (kChunkOverhead + sizeof(std::shared_ptr<Chunk>) + sizeof(std::size_t)) +
           m_size * sizeof(Clip);
}

bool ClipList::operator==(const ClipList& other) const {
    return m_size == other.m_size && std::equal(begin(), end(), other.begin());
}

} // namespace wabcut
