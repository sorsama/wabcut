#include <wabcut/ai/audio/Plans.h>

#include <algorithm>
#include <cstddef>

namespace wabcut::ai::audio {

std::vector<Segment> planChunks(std::int64_t frames, const ChunkOptions& options) {
    std::vector<Segment> out;
    if (frames <= 0) {
        return out;
    }
    const std::int64_t body = std::max<std::int64_t>(options.body, 1);
    const std::int64_t fade = std::clamp<std::int64_t>(options.crossfade, 0, body);
    for (std::int64_t begin = 0; begin < frames; begin += body) {
        const std::int64_t end = std::min(frames, begin + body);
        const bool last = end == frames;
        Segment s;
        s.outBegin = begin;
        s.fadeIn = begin == 0 ? 0 : fade;
        s.fadeOut = last ? 0 : std::min(fade, frames - end);
        s.outEnd = end + s.fadeOut;
        s.readBegin = std::max<std::int64_t>(0, begin - std::max<std::int64_t>(options.preroll, 0));
        s.readEnd = std::min(frames, s.outEnd + std::max<std::int64_t>(options.postroll, 0));
        out.push_back(s);
    }
    // A short final chunk (shorter than the crossfade) shortens the previous chunk's fade-out to fit.
    for (std::size_t i = 1; i < out.size(); ++i) {
        const std::int64_t fit = std::min(out[i - 1].fadeOut, out[i].outEnd - out[i].outBegin);
        out[i - 1].fadeOut = fit;
        out[i - 1].outEnd = out[i].outBegin + fit;
        out[i].fadeIn = fit;
    }
    return out;
}

std::vector<Segment> planSegments(std::int64_t frames, std::int64_t segment, std::int64_t overlap) {
    std::vector<Segment> out;
    if (frames <= 0 || segment <= 0) {
        return out;
    }
    overlap = std::clamp<std::int64_t>(overlap, 0, segment / 2);
    const std::int64_t stride = segment - overlap;
    for (std::int64_t begin = 0;; begin += stride) {
        Segment s;
        s.readBegin = s.outBegin = begin;
        s.readEnd = s.outEnd = std::min(frames, begin + segment);
        s.fadeIn = begin == 0 ? 0 : overlap;
        const bool last = s.readEnd >= frames;
        s.fadeOut = last ? 0 : overlap;
        out.push_back(s);
        if (last) {
            break; // a segment that is not the last ends before `frames`, so the next one has more than the overlap
        }
    }
    return out;
}

std::span<const float> OverlapAssembler::push(const Segment& segment, std::span<const float> output) {
    const auto ch = static_cast<std::size_t>(m_channels);
    const auto at = [&](std::int64_t frame) { return output.data() + static_cast<std::size_t>(frame - segment.readBegin) * ch; };
    const std::int64_t keepEnd = segment.outEnd - segment.fadeOut; // final after this push
    m_out.assign(static_cast<std::size_t>(std::max<std::int64_t>(keepEnd - segment.outBegin, 0)) * ch, 0.0f);
    for (std::int64_t f = segment.outBegin; f < keepEnd; ++f) {
        const float* src = at(f);
        float* dst = m_out.data() + static_cast<std::size_t>(f - segment.outBegin) * ch;
        const std::int64_t i = f - segment.outBegin;
        if (i < segment.fadeIn) {
            const float w = static_cast<float>((static_cast<double>(i) + 0.5) / static_cast<double>(segment.fadeIn));
            const float* tail = m_tail.data() + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                dst[c] = tail[c] + src[c] * w;
            }
        } else {
            std::copy_n(src, ch, dst);
        }
    }
    // The fade-out frames wait (weighted) for the next segment's fade-in.
    m_tail.assign(static_cast<std::size_t>(segment.fadeOut) * ch, 0.0f);
    for (std::int64_t i = 0; i < segment.fadeOut; ++i) {
        const float w = 1.0f - static_cast<float>((static_cast<double>(i) + 0.5) / static_cast<double>(segment.fadeOut));
        const float* src = at(keepEnd + i);
        float* dst = m_tail.data() + static_cast<std::size_t>(i) * ch;
        for (std::size_t c = 0; c < ch; ++c) {
            dst[c] = src[c] * w;
        }
    }
    m_finished = keepEnd;
    return m_out;
}

} // namespace wabcut::ai::audio
