#pragma once

// A.9: channel conversions of the audio engine (docs/ai.md "Audio channels and 5.1 (A.9)"). Pure functions.
//
//   * Formats (wabcut/model/AudioChannels.h): mono, stereo, 5.1 in the order L, R, C, LFE, Ls, Rs, 7.1 (0.7, card 048) in
//     the order L, R, C, LFE, Lrs, Rrs, Ls, Rs (a WAVE / FFmpeg 7.1 file's), adaptive (one to one). formatMatrix() converts
//     between them:
//       mono -> stereo: both sides at -3 dB (the constant-power centre, what FFmpeg's resampler does for a mono file);
//       stereo -> mono: the mean of the two;
//       5.1 -> stereo: ITU-R BS.775: L' = L + 0.7071 C + 0.7071 Ls, R' = R + 0.7071 C + 0.7071 Rs (LFE dropped);
//       5.1 -> mono: the mean of that stereo;
//       mono -> 5.1: the centre; stereo -> 5.1: L and R; 5.1 -> 5.1, stereo -> stereo: as they are;
//       7.1 -> 5.1: Ls' = Ls + 0.7071 Lrs, Rs' = Rs + 0.7071 Rrs (the side pair as it is, the rear pair folded in at -3 dB),
//       L, R, C and LFE as they are; 7.1 -> stereo and mono: that, then the 5.1 conversions (ITU-R BS.775);
//       5.1 -> 7.1: L, R, C, LFE as they are, 5.1's Ls / Rs to 7.1's side pair Ls / Rs, the rear pair silent (upmix51To71()
//       can copy them into it too); mono and stereo -> 7.1: as to 5.1, the rest silent;
//       adaptive (any count) to or from anything: channel k to channel k, extra channels dropped, missing ones silent.
//   * The 5.1 panner (Premiere's track 5.1 panner on a 5.1 master): a two-dimensional vector-based panner (VBAP) over
//     the five full-range speakers at -30 (L), +30 (R), 0 (C), -110 (Ls) and +110 (Rs) degrees; on a 7.1 master over the seven
//     at -30 (L), +30 (R), 0 (C), -90 (Ls), +90 (Rs), -150 (Lrs) and +150 (Rrs) degrees. The puck (x: -1 left ..
//     +1 right, y: -1 back .. +1 front) gives the direction (atan2(x, y)) and, by its distance from the centre, how
//     focused the sound is: at the rim one speaker pair (or one speaker), at the centre all five equally; the power
//     always sums to 1. A stereo input is placed at the puck's direction -30 / +30 degrees, so the default puck (front
//     centre) plays a stereo track on L and R and a mono track on C exactly. The LFE send adds the inputs' mean to the
//     LFE channel.

#include <wabcut/model/AudioChannels.h>

#include <array>
#include <vector>

namespace wabcut::playback {

/// A small channel matrix: out[r] = sum over c of at(r, c) * in[c].
struct ChannelMatrix {
    int rows = 0;
    int cols = 0;
    std::vector<float> values; ///< row-major, rows * cols

    ChannelMatrix() = default;
    ChannelMatrix(int r, int c) : rows(r), cols(c), values(static_cast<std::size_t>(r) * static_cast<std::size_t>(c), 0.0f) {}

    float& at(int r, int c) { return values[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c)]; }
    float at(int r, int c) const {
        return values[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c)];
    }
    bool isIdentity() const;
    /// this * other (this->cols must equal other.rows).
    ChannelMatrix operator*(const ChannelMatrix& other) const;
    /// Applies the matrix to `frames` interleaved frames: `in` has cols channels, `out` rows (they must not overlap).
    void apply(const float* in, float* out, std::size_t frames) const;
};

inline constexpr float kMinus3dB = 0.70710678f;

/// Converts `fromChannels` channels of format `from` to `toChannels` of format `to` (counts matter for adaptive).
ChannelMatrix formatMatrix(ChannelFormat from, int fromChannels, ChannelFormat to, int toChannels);

/// Converts a 5.1 mix to 7.1 with the 5.1 surrounds also in the rear pair (each at -3 dB in the side and the rear pair)
/// instead of leaving the rear pair silent (what formatMatrix() does).
ChannelMatrix upmix51To71(bool copyToRear);

/// The 5.1 panner's gains for one input at azimuth `degrees` (0 = front, positive = right) with `focus` in [0, 1] (1:
/// a point source between the two nearest speakers, 0: all five equally), in 5.1 order (LFE 0). Power sums to 1.
std::array<float, 6> surroundGains(double degrees, double focus);
/// The same on a 5.1 (`outputs` 6) or 7.1 (8) master, in that layout's order (LFE 0): the seven full-range speakers of 7.1
/// are at -150 (Lrs), -90 (Ls), -30 (L), 0 (C), +30 (R), +90 (Rs) and +150 (Rrs) degrees. Power sums to 1.
std::array<float, 8> surroundGainsFor(int outputs, double degrees, double focus);

/// The 5.1 panner's matrix (6 x inputs, inputs 1 or 2) for a puck at (x, y) and an LFE send `lfeGain` (linear).
ChannelMatrix surroundPanMatrix(int inputs, double x, double y, double lfeGain);
/// The same gains without allocating: `out` receives 6 rows of `inputs` (1 or 2) values, row-major.
void surroundPanGains(int inputs, double x, double y, double lfeGain, float* out);
/// On a 5.1 or 7.1 master: `outputs` (6 or 8) rows of `inputs` values.
void surroundPanGains(int outputs, int inputs, double x, double y, double lfeGain, float* out);

} // namespace wabcut::playback
