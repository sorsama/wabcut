#pragma once

// ConfidenceBuckets: the per-frame confidence strip of a tracking job, kept at a bounded size. A 10-minute 60 fps clip is
// 36,000 frames; a strip that grows with every frame is converted to a QML list again on every progress report, which is
// quadratic work on the GUI thread. This keeps at most `capacity` buckets: when it is full, neighbouring buckets are merged
// (each keeps the LOWEST value of its frames, so a lost frame, -1, is never averaged away) and later frames fill buckets
// twice as wide. Pure C++ (no Qt): the bridge turns it into a list when it announces progress, a few times a second.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace wabcut::app {

class ConfidenceBuckets {
public:
    explicit ConfidenceBuckets(std::size_t capacity = 480) : m_capacity(std::max<std::size_t>(capacity, 2) & ~std::size_t{1}) {}

    void clear() {
        m_values.clear();
        m_perBucket = 1;
        m_inLast = 0;
        m_frames = 0;
    }

    /// One more frame's confidence (0..1, -1 = lost).
    void add(double value) {
        if (m_inLast == 0) {
            m_values.push_back(value);
        } else {
            m_values.back() = std::min(m_values.back(), value);
        }
        ++m_frames;
        if (++m_inLast >= m_perBucket) {
            m_inLast = 0;
        }
        if (m_values.size() >= m_capacity && m_inLast == 0) {
            merge();
        }
    }

    const std::vector<double>& values() const { return m_values; }
    /// Frames added since clear().
    std::size_t frames() const { return m_frames; }
    /// How many frames one bucket stands for now (the last one may hold fewer).
    std::size_t framesPerBucket() const { return m_perBucket; }

private:
    void merge() {
        std::size_t out = 0;
        for (std::size_t i = 0; i < m_values.size(); i += 2) {
            m_values[out++] = i + 1 < m_values.size() ? std::min(m_values[i], m_values[i + 1]) : m_values[i];
        }
        m_values.resize(out);
        m_perBucket *= 2;
    }

    std::size_t m_capacity;
    std::vector<double> m_values;
    std::size_t m_perBucket = 1;
    std::size_t m_inLast = 0;
    std::size_t m_frames = 0;
};

} // namespace wabcut::app
