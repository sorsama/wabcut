#pragma once

// How a long take is cut into helper requests and put back together (A.4). Enhance Speech sends chunks with a
// pre-roll (DeepFilterNet's recurrent state and normalisations settle before the chunk's own samples) and a post-roll
// (its lookahead), and consecutive chunks overlap by a short crossfade; stem separation sends the model's fixed
// segments with a quarter overlap (Demucs's apply_model). Either way each request is independent (a restarted helper
// gives the same answer) and memory stays bounded by one request. Pure functions and a streaming assembler, so the
// seams are tested without a model.

#include <cstdint>
#include <span>
#include <vector>

namespace wabcut::ai::audio {

/// One request: the input frames it reads and the output frames it contributes. Its first `fadeIn` output frames
/// fade in over the previous segment's last `fadeOut` frames (linear, the two weights sum to 1).
struct Segment {
    std::int64_t readBegin = 0;
    std::int64_t readEnd = 0;
    std::int64_t outBegin = 0;
    std::int64_t outEnd = 0;
    std::int64_t fadeIn = 0;
    std::int64_t fadeOut = 0;

    bool operator==(const Segment&) const = default;
};

struct ChunkOptions {
    std::int64_t body = 48000 * 30;   ///< frames each chunk owns
    std::int64_t preroll = 48000 * 4; ///< read before the chunk, output discarded
    std::int64_t postroll = 4800;     ///< read after the chunk and its crossfade, output discarded
    std::int64_t crossfade = 2400;    ///< 50 ms at 48 kHz
};

/// Enhance Speech chunks over `frames` frames (none for 0). The chunks' output spans tile [0, frames).
std::vector<Segment> planChunks(std::int64_t frames, const ChunkOptions& options = {});

/// Stem separation segments of `segment` frames over `frames` frames, overlapping by `overlap` (a quarter of the
/// segment in Demucs); the last one may be shorter (the helper pads it).
std::vector<Segment> planSegments(std::int64_t frames, std::int64_t segment, std::int64_t overlap);

/// Puts segments' outputs back together in order, crossfading their overlaps, and hands out each stretch of output
/// once it is final. `channels` interleaved channels.
class OverlapAssembler {
public:
    explicit OverlapAssembler(int channels) : m_channels(channels) {}

    /// The output of `segment` (its read span, (readEnd - readBegin) x channels). Returns the frames that are now
    /// final (from finishedFrames() before the call on), valid until the next push.
    std::span<const float> push(const Segment& segment, std::span<const float> output);
    /// Output frames handed out so far.
    std::int64_t finishedFrames() const { return m_finished; }

private:
    int m_channels;
    std::int64_t m_finished = 0;
    std::vector<float> m_tail; ///< the previous segment's fade-out frames, weighted
    std::vector<float> m_out;
};

} // namespace wabcut::ai::audio
