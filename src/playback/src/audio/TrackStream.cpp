#include "TrackStream.h"

#include <wabcut/audio/TimeMath.h>

#include <algorithm>

namespace wabcut::playback::detail {

using audio::timemath::mulDivSaturate;

TrackStream::TrackStream(std::size_t capacityFrames, int channels, std::size_t maxChunks, int fadeFrames)
    : m_frames(capacityFrames, channels), m_chunks(maxChunks), m_fadeFrames(std::max(fadeFrames, 0)),
      m_lastOut(std::make_unique<float[]>(static_cast<std::size_t>(std::max(channels, 1)))),
      m_tailFrom(std::make_unique<float[]>(static_cast<std::size_t>(std::max(channels, 1)))) {}

// ---- Producer ----------------------------------------------------------------------------------------------

std::size_t TrackStream::writableFrames() const noexcept {
    return m_chunks.freeSlots() == 0 ? 0 : m_frames.writableFrames();
}

bool TrackStream::write(std::uint64_t epoch, Ticks timeline, std::span<const float> interleaved) noexcept {
    const std::size_t frames = interleaved.size() / static_cast<std::size_t>(m_frames.channels());
    if (frames == 0) {
        return true;
    }
    if (frames > writableFrames() || frames > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    // Frames first, then the header that makes them visible: the consumer acquires the header (SpscQueue) and
    // only then reads the frames, which the ring's own release/acquire pair has published before.
    m_frames.write(interleaved.first(frames * static_cast<std::size_t>(m_frames.channels())));
    return m_chunks.push({epoch, timeline.value, static_cast<std::uint32_t>(frames), 0});
}

void TrackStream::flush(std::uint64_t epoch, std::int64_t cutover) noexcept { m_marker.store({epoch, cutover}); }

// ---- Consumer ----------------------------------------------------------------------------------------------

void TrackStream::dropRest(Chunk& chunk) noexcept {
    m_frames.discard(chunk.frames - chunk.consumed);
    m_chunks.pop();
}

void TrackStream::shape(float* run, std::int64_t frames, bool data, bool discontinuous) noexcept {
    if (frames <= 0) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_frames.channels());
    if (m_fadeFrames > 0) {
        const bool startsAudio = data && (!m_prevData || discontinuous);
        const bool cutsAudio = m_prevData && (!data || discontinuous);
        if (cutsAudio) {
            std::copy_n(m_lastOut.get(), ch, m_tailFrom.get());
            m_tailPos = 0;
        }
        if (startsAudio) {
            m_inPos = 0;
        } else if (!data) {
            m_inPos = -1;
        }
        const float denominator = static_cast<float>(m_fadeFrames + 1);
        for (std::int64_t i = 0; i < frames && (m_inPos >= 0 || m_tailPos >= 0); ++i) {
            float* const frame = run + static_cast<std::size_t>(i) * ch;
            if (m_inPos >= 0) {
                const float g = static_cast<float>(m_inPos + 1) / denominator;
                for (std::size_t c = 0; c < ch; ++c) {
                    frame[c] *= g;
                }
                if (++m_inPos >= m_fadeFrames) {
                    m_inPos = -1;
                }
            }
            if (m_tailPos >= 0) {
                const float g = 1.0f - static_cast<float>(m_tailPos + 1) / denominator;
                for (std::size_t c = 0; c < ch; ++c) {
                    frame[c] += m_tailFrom[c] * g;
                }
                if (++m_tailPos >= m_fadeFrames) {
                    m_tailPos = -1;
                }
            }
        }
    }
    m_prevData = data;
    std::copy_n(run + static_cast<std::size_t>(frames - 1) * ch, ch, m_lastOut.get());
}

bool TrackStream::read(Ticks start, std::span<float> out, std::int64_t tpf, std::int64_t maxWaitFrames,
                       StreamCounters& counters) noexcept {
    const auto ch = static_cast<std::size_t>(m_frames.channels());
    const auto n = static_cast<std::int64_t>(out.size() / ch);
    std::fill(out.begin(), out.end(), 0.0f);

    Marker marker;
    if (m_marker.tryLoad(marker) && marker.epoch > m_epoch) {
        m_epoch = marker.epoch;
        m_cutover = marker.cutover;
        m_switching = true;
        if (marker.cutover == kImmediate) {
            // A seek or resync: the transport itself jumped (the engine de-clicks that edge), so the new audio
            // starts fresh instead of carrying a tail of whatever this track played before.
            m_prevData = false;
            m_tailPos = -1;
        }
    }

    bool reachable = true;
    std::int64_t pos = 0;
    while (pos < n) {
        const std::int64_t tt = start.value + pos * tpf;
        Chunk* chunk = m_chunks.frontMutable();
        if (chunk == nullptr) {
            counters.underrunFrames += static_cast<std::uint64_t>(n - pos);
            shape(out.data() + static_cast<std::size_t>(pos) * ch, n - pos, false, false);
            break;
        }
        if (chunk->epoch > m_epoch) {
            // Its marker raced with this read (we will see it next block); the data is newer: adopt it.
            m_epoch = chunk->epoch;
            m_cutover = kImmediate;
            m_switching = true;
        }
        std::int64_t limit = n - pos;
        if (chunk->epoch < m_epoch) {
            // Data of an older epoch: plays on only until an edit's cutover time, otherwise it is dropped.
            if (!m_switching || m_cutover == kImmediate || tt >= m_cutover) {
                dropRest(*chunk);
                m_discontinuity = true;
                continue;
            }
            limit = std::min(limit, mulDivSaturate(m_cutover - tt, 1, tpf, Rounding::Ceil));
        } else if (m_switching) {
            m_switching = false; // reached the current epoch's data
            m_discontinuity = true;
        }

        const std::int64_t chunkTime = chunk->timeline + static_cast<std::int64_t>(chunk->consumed) * tpf;
        const auto remaining = static_cast<std::int64_t>(chunk->frames - chunk->consumed);
        if (chunkTime == tt) {
            const std::int64_t k = std::min({limit, remaining});
            float* const dst = out.data() + static_cast<std::size_t>(pos) * ch;
            m_frames.read(std::span<float>(dst, static_cast<std::size_t>(k) * ch));
            shape(dst, k, true, m_discontinuity);
            m_discontinuity = false;
            chunk->consumed += static_cast<std::uint32_t>(k);
            if (chunk->consumed == chunk->frames) {
                m_chunks.pop();
            }
            pos += k;
        } else if (chunkTime < tt) {
            // Behind the transport (late data, or the transport jumped forward): drop what is stale.
            const std::int64_t d = std::min(remaining, mulDivSaturate(tt - chunkTime, 1, tpf, Rounding::Ceil));
            m_frames.discard(static_cast<std::size_t>(d));
            counters.skippedFrames += static_cast<std::uint64_t>(d);
            chunk->consumed += static_cast<std::uint32_t>(d);
            if (chunk->consumed == chunk->frames) {
                m_chunks.pop();
            }
            m_discontinuity = true;
        } else {
            // The data starts later (anchored ahead of the transport): silence until then, unless it is out of
            // reach (the transport jumped backwards): then the feeder must resync.
            const std::int64_t gap = mulDivSaturate(chunkTime - tt, 1, tpf, Rounding::Ceil);
            const std::int64_t g = std::min(limit, gap);
            if (gap > maxWaitFrames) {
                reachable = false;
                shape(out.data() + static_cast<std::size_t>(pos) * ch, n - pos, false, false);
                counters.waitFrames += static_cast<std::uint64_t>(n - pos);
                break;
            }
            shape(out.data() + static_cast<std::size_t>(pos) * ch, g, false, false);
            counters.waitFrames += static_cast<std::uint64_t>(g);
            pos += g;
        }
    }
    return reachable;
}

} // namespace wabcut::playback::detail
