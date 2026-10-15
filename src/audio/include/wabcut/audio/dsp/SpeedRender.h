#pragma once

// Audible speed changes (A.10): the audio of a clip whose source position does not advance at 100 % (a constant speed,
// a speed ramp, a reverse segment of a time remap) rendered along that position, sample-accurately, in preview and
// export alike. Two renderers share one interface:
//
//   Varispeed     Maintain Audio Pitch off: the source resampled at the position of every output frame (band-limited
//                 interpolation: a Kaiser-windowed sinc whose cutoff follows the local speed, so speeding up does not
//                 alias). Pitch moves with the speed, like tape. At 1x on whole-frame positions it copies samples, so a
//                 100 % reverse is the time-reversed source exactly.
//   PitchStretch  Maintain Audio Pitch on: WSOLA (waveform-similarity overlap-add, Verhelst and Roelands 1993) driven by
//                 positions instead of a speed: synthesis hop k on a fixed grid of output frames (k * hop) takes a
//                 Hann-windowed source segment centred on the position of its centre frame, shifted within a tolerance
//                 to the offset that best continues the previous segment. Reverse segments read the source backwards.
//
// Determinism (preview == export): what either renderer outputs for a frame depends only on the positions, the source
// and the frame number, never on where rendering started or how it was cut into blocks. Varispeed is stateless. The
// stretch's only state is the previous segment's offset, so on a fixed grid of resync hops (every `resyncSeconds` of
// output) and at a piece's first hop the offset starts again from 0; a render that does not continue the previous one
// replays from the last resync hop before it (at most resyncSeconds plus one hop of extra work).
//
// A piece is the output range [begin, end) where a renderer applies. Its extent reaches a little further on both sides:
// there its output fades in and out (its coverage, 0..1, rises before `begin` and falls after `end`), and the caller
// fades the neighbouring audio (100 % reads, or silence) by 1 - coverage, so joins are click-free crossfades.
//
// Speeds outside [kMinAudibleSpeed, kMaxAudibleSpeed] are silent (audibleSpeedGain fades over the edges), both modes.
//
// Threading: one thread at a time; not real-time (buffers grow on first use). Frame numbers are absolute (a fixed grid,
// e.g. timeline frames), positions are source frames at the render rate (fractional).

#include <cstdint>
#include <memory>
#include <vector>

namespace wabcut::audio::dsp {

/// The slowest and fastest speeds (input frames per output frame, either direction) that sound.
inline constexpr double kMinAudibleSpeed = 0.1;
inline constexpr double kMaxAudibleSpeed = 20.0;

/// Gain for a speed: 1 inside the audible range, 0 below 0.08x and above 24x, linear between.
double audibleSpeedGain(double speed) noexcept;

/// Source frames [first, first + count) at the render rate, interleaved at the renderer's channel count, into `dst`
/// (count * channels floats). Frames the source does not have (before its start, past its end) read as silence.
class SpeedSource {
public:
    virtual ~SpeedSource() = default;
    virtual void read(std::int64_t first, std::int64_t count, float* dst) = 0;
};

/// The source position (source frames, fractional) that output frame `first + i` plays, for i in [0, count).
class SpeedPositions {
public:
    virtual ~SpeedPositions() = default;
    virtual void fill(std::int64_t first, int count, double* out) const = 0;
};

enum class SpeedMode : std::uint8_t { Varispeed, MaintainPitch };

struct PitchStretchOptions {
    double windowMs = 30.0;     ///< N; the synthesis hop is N / 2 (periodic Hann at 50 % overlap adds up to 1)
    double toleranceMs = 7.5;   ///< the similarity search range, +- this
    double resyncSeconds = 1.0; ///< the offset restarts from 0 every this much output (bounds a seek's replay)
};

class SpeedRenderer {
public:
    virtual ~SpeedRenderer() = default;

    /// The piece rendered from now on: output frames [begin, end) (begin < end). A different piece forgets the state.
    virtual void setPiece(std::int64_t begin, std::int64_t end) = 0;
    /// Where its output can be non-zero: [extentBegin, extentEnd) contains [begin, end).
    virtual std::int64_t extentBegin() const noexcept = 0;
    virtual std::int64_t extentEnd() const noexcept = 0;
    /// Its own fade at frame `frame`: 1 inside the piece, rising before and falling after it, 0 outside the extent.
    virtual double coverage(std::int64_t frame) const noexcept = 0;
    /// Output frames [first, first + count) into `out` (interleaved, zeroed first), already faded by coverage().
    virtual void render(std::int64_t first, int count, const SpeedPositions& positions, SpeedSource& source,
                        float* out) = 0;
    /// Forgets the state (the source cache too): the next render starts again (same output).
    virtual void reset() noexcept = 0;
    /// Output hops computed since construction (the stretch's work, for tests and benchmarks; 0 for varispeed).
    virtual std::int64_t hopsComputed() const noexcept { return 0; }
};

std::unique_ptr<SpeedRenderer> makeSpeedRenderer(SpeedMode mode, int channels, double sampleRate,
                                                 const PitchStretchOptions& options = {});

/// The shortest gap (frames) between two pieces whose extents stay apart; a caller folds closer pieces (and the 100 %
/// audio between them) into one.
std::int64_t minimumPieceGap(SpeedMode mode, double sampleRate, const PitchStretchOptions& options = {});

} // namespace wabcut::audio::dsp
